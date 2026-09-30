# halo-nxdk

Standalone original-Xbox build of the reconstructed Halo CE source, linked
against [nxdk](https://github.com/XboxDev/nxdk) instead of the Microsoft XDK.

This repository contains only the nxdk-side pieces: XDK compatibility layers
(`compat/`), CRT fixups (`crt/`), the runtime entry (`standalone_runtime.c`),
and the build and debug tools (`tools/`). The game code, `kb.json` and the
reference build tooling come from a halo checkout, which you point at with
`HALO_ROOT`. Any halo checkout or worktree works.

```bash
export NXDK_DIR=/path/to/nxdk      # built nxdk checkout
export HALO_ROOT=/path/to/halo     # halo checkout or worktree; default ../halo
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$PWD/toolchains/llvm.cmake
cmake --build build -j             # -> build/default.xbe
```

Or use `./build.sh [seed|apu|standalone|nogpu] [--fresh] [--inventory] [--deploy]`,
which sets the variables above to local defaults (see `./build.sh --help`).

Audio is enabled by default: PCM and Xbox ADPCM feed a software stereo mixer
through nxdk's AC97 output. HRTF, spatial attenuation, DSP reverb, and Bink
movie audio remain unsupported. Audible playback was confirmed on xemu;
real Xbox hardware remains unverified.
Configure with `-DHALO_STANDALONE_AUDIO=OFF` to use the silent backend.
An optional [nxdk-audio APU prototype](docs/audio-apu.md) is available through
`./build.sh apu` or `-DHALO_STANDALONE_AUDIO_BACKEND=apu`. It uses hardware
voices for stereo playback; spatial audio and DSP feature parity are still pending.

See [docs/standalone-build.md](docs/standalone-build.md) for the design, the
seeded bring-up variant, the boot-test loop and the status.
