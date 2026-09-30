#!/usr/bin/env python3
"""Decode Halo's Bink movies with the compat codec and compare against ffmpeg.

Video must match bit-exactly (yuv420p); audio within 1 LSB. The movies are
not in the repo: HALO_BINK_MEDIA names a directory of .bik files (default
build-bink/media). HALO_BINK_FRAMES limits frames per movie (default 300,
0 for all). Skips when the media or ffmpeg is missing.
"""
from array import array
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
media = Path(os.environ.get("HALO_BINK_MEDIA", root / "build-bink/media"))
max_frames = int(os.environ.get("HALO_BINK_FRAMES", "300"))
movies = sorted(media.glob("*.bik")) if media.is_dir() else []
if not movies or not shutil.which("ffmpeg"):
    print(f"bink: SKIP (no .bik files in {media} or no ffmpeg)")
    sys.exit(0)


def ffmpeg(movie, args, output):
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", str(movie), *args, str(output)],
                   check=True)


with tempfile.TemporaryDirectory(prefix="halo-bink-") as directory:
    directory = Path(directory)
    binary = directory / "test-bink"
    subprocess.run([
        *shlex.split(os.environ.get("CC", "cc")), "-std=c99", "-O2", "-Wall", "-Wextra",
        "-Werror", "-DHALO_BINK_HOST_TEST", "-fsanitize=address,undefined", "-g",
        *(str(root / "compat/bink" / name) for name in ("video.c", "audio.c", "blit.c")),
        str(root / "tools/test_bink.c"),
        "-o", str(binary),
    ], check=True)
    failed = False
    for movie in movies:
        video, audio = directory / "video.yuv", directory / "audio.s16"
        reference_video, reference_audio = directory / "ref.yuv", directory / "ref.s16"
        limit = ["-frames:v", str(max_frames)] if max_frames else []
        subprocess.run([str(binary), str(movie), str(video), str(audio), "/dev/null",
                        str(max_frames or 0xffffffff)], check=True, timeout=600)
        ffmpeg(movie, [*limit, "-an", "-f", "rawvideo", "-pix_fmt", "yuv420p"], reference_video)
        ffmpeg(movie, ["-vn", "-f", "s16le"], reference_audio)

        decoded, reference = video.read_bytes(), reference_video.read_bytes()
        video_ok = decoded == reference
        samples, reference_samples = array("h"), array("h")
        samples.frombytes(audio.read_bytes())
        reference_samples.frombytes(reference_audio.read_bytes())
        # RAD clips each chunk's final block, so only the common prefix of a
        # partial decode is comparable.
        count = min(len(samples), len(reference_samples))
        worst = max((abs(a - b) for a, b in zip(samples[:count], reference_samples[:count])),
                    default=0)
        audio_ok = worst <= 1 and (count > 0 or not reference_samples)
        print(f"{movie.name}: video {len(decoded)} bytes {'match' if video_ok else 'DIFFER'}, "
              f"audio {count} samples max diff {worst}")
        failed |= not (video_ok and audio_ok)
    if failed:
        sys.exit("bink: FAIL")
print("bink video bit-exact and audio within 1 LSB against ffmpeg: PASS")
