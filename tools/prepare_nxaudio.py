#!/usr/bin/env python3
"""Build a patched copy of pinned nxdk-audio, leaving its checkout untouched."""
import argparse
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--assembler", type=Path, required=True)
    parser.add_argument("--patch", type=Path, required=True)
    args = parser.parse_args()
    revision = subprocess.check_output(["git", "-C", str(args.source), "rev-parse", "HEAD"], text=True).strip()
    if revision != args.revision:
        parser.error(f"nxdk-audio is at {revision}; expected {args.revision}")
    files = [*args.source.glob("*.h"), *args.source.glob("include/*.h")]
    files += [args.source / f for f in ("audio_core.c", "audio_buffer.c", "audio_voice.c", "passthrough.a56")]
    subprocess.run(["git", "-C", str(args.source), "diff", "--exit-code", "HEAD", "--",
                    *(str(f.relative_to(args.source)) for f in files)], check=True)
    version = subprocess.check_output([str(args.assembler), "--version"], text=True).strip()
    if version != "dsp56300-asm 1.0.0":
        parser.error(f"expected dsp56300-asm 1.0.0; got {version}")
    args.output.mkdir(parents=True, exist_ok=True)
    for source in files:
        target = args.output / source.relative_to(args.source)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    subprocess.run(["patch", "--batch", "--forward", "-p1", "-i", str(args.patch.resolve())],
                   cwd=args.output, check=True)
    program = args.output / "passthrough.out"
    subprocess.run([str(args.assembler), "-f", "lod", "-o", str(program),
                    str(args.output / "passthrough.a56")], check=True)
    # Clang 18 lacks #embed. Preserve upstream's text parser and embed the
    # assembler's LOD text as an ordinary C byte initializer instead.
    data = program.read_bytes()
    rows = [", ".join(str(value) for value in data[i:i + 24]) for i in range(0, len(data), 24)]
    (args.output / "passthrough.inc").write_text(",\n".join(rows) + "\n")


if __name__ == "__main__":
    main()
