# Bink movies

Halo plays its intro, attract and credits movies through RAD's Bink library.
The decompiled front end (`halo/src/halo/bink/bink.c`: BinkOpen, BinkDoFrame,
BinkNextFrame, BinkCopyToBuffer, ...) stays as it was in the original.
The standalone build replaces the RAD internals it calls. These are the
kb.json `bink` stubs, so `link_report.json` now shows only the 13
legacy data symbols for that category.

All files are in `compat/bink/`:

| File | Provides |
|------|----------|
| `video.c` | Bink 1 video decoder (`FUN_00238070`, bundle sizes `FUN_00236210`), revisions f–i, alpha and grayscale |
| `audio.c` | Bink RDFT audio decoder (`FUN_0023cd10` / `FUN_0023ec50` / `FUN_0023ce50`), no libm |
| `blit.c` | YUV→RGB blitters (`FUN_00244b40` and siblings), BT.601, masked and unmasked |
| `memory.c` | radmalloc/radfree, push/popmalloc, RADTimerRead, BinkSetError |
| `radcb.c` | RADCB callback threads (IO read-ahead and sound handlers) |
| `playback.c` | Sound ring servicing, BinkPause, BinkSetSoundOnOff, set_frame, frame timers, RADCB priority/dispatch callbacks |
| `file.c` | `BinkFileOpen`: page-based read-ahead ring over nxdk file IO |
| `sound.c` | `BinkOpenDirectSound`: BINKSND over `compat/audio/pcm_stream.h` |
| `tables.h`, `internal.h` | Codec tables; shared declarations and nxdk winapi aliases |

The runtime glue follows the cachebeta.xbe behavior as reconstructed in
halo-pal-2342's `libs/binkxbox`. One part differs: the original sound backend
wrote into a looping DirectSoundBuffer. Here it queues four ~120ms PCM
packets on a callback-less stream (`pcm_stream_*` in `dsound_nxdk.c`), and
the Bink sound thread retires those packets itself. That way, game
DirectSound callbacks never run off the game thread. When the game thread
holds every callback it wants, the RADCB thread blocks on their mutexes
(up to 2ms) instead of polling them in a loop the way RAD did. The silent backend
(`HALO_STANDALONE_AUDIO=OFF`) cannot open a stream, so movies play without
sound.

## Testing

`tools/test_bink.py` builds the codec natively and compares each movie in
`HALO_BINK_MEDIA` (default `build-bink/media`) against ffmpeg. Video must be
bit-exact (yuv420p) and audio within 1 LSB. `HALO_BINK_FRAMES` sets how many
frames to decode per movie (default 300, 0 means all). The movies are not in
the repo. Copy them from the title's `media` directory. The whole of
`intro.bik` and `attract3.bik` has matched.

## License

`video.c`, `audio.c` and `tables.h` are derived from FFmpeg's `libavcodec/bink.c`,
`binkaudio.c` and `binkdata.h`, which are LGPL-2.1-or-later. Each of them
keeps that notice. The full license is in
[`LICENSES/LGPL-2.1-or-later.txt`](../LICENSES/LGPL-2.1-or-later.txt).
The original contributions in the other compatibility files use the project's
AGPL-3.0-only license; see [`LICENSE`](../LICENSE).

The project license does not replace the decoder's LGPL license or upstream
copyright notices. When distributing a linked executable, include the applicable
license texts and corresponding source, including the decoder modifications and
build scripts. External Halo sources and data retain their existing rights;
the project license does not grant permission to redistribute them.
