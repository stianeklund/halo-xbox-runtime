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

Audio is enabled by default: PCM and Xbox ADPCM use the
[nxdk-audio APU backend](docs/audio-apu.md) for hardware stereo playback.
Distance/cone attenuation, occlusion levels, and stereo panning are approximated.
HRTF filtering, DSP reverb, and Bink movie audio remain unsupported.
Audible playback was confirmed on xemu;
real Xbox hardware remains unverified.
Configure with `-DHALO_STANDALONE_AUDIO=OFF` to use the silent backend.
The APU backend remains experimental; spatial audio and DSP feature parity are pending.
Use `HALO_STANDALONE_AUDIO_BACKEND=software ./build.sh` or
`-DHALO_STANDALONE_AUDIO_BACKEND=software` for the software stereo mixer.

See [docs/standalone-build.md](docs/standalone-build.md) for the design, the
seeded bring-up variant, the boot-test loop and the status.
