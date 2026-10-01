# halo-xbox-runtime

Xbox XDK compatibility layers and runtime support for Halo decompilation
projects. The current backend targets original Xbox through
[nxdk](https://github.com/XboxDev/nxdk).

This repository contains XDK compatibility layers
(`compat/`, one folder per library, with the entry point in `compat/xapi/init.c`), CRT fixups
(`crt/`), bring-up runtime support (`standalone_runtime.c`),
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
HRTF filtering and DSP reverb remain unsupported.
Bink movies (intro, attract, credits) play through an open decoder in
[`compat/bink/`](docs/bink.md); see that page for licensing.
Audible playback was confirmed on xemu;
real Xbox hardware remains unverified.
Configure with `-DHALO_STANDALONE_AUDIO=OFF` to use the silent backend.
The APU backend remains experimental; spatial audio and DSP feature parity are pending.
Use `HALO_STANDALONE_AUDIO_BACKEND=software ./build.sh` or
`-DHALO_STANDALONE_AUDIO_BACKEND=software` for the software stereo mixer.

See [docs/standalone-build.md](docs/standalone-build.md) for the design, the
seeded bring-up variant, the boot-test loop and the status.

## License

Unless a file states otherwise, original contributions to halo-xbox-runtime are
licensed under the **GNU General Public License, version 3 only**
(`GPL-3.0-only`). See [LICENSE](LICENSE) for the full terms.

The FFmpeg-derived Bink decoder files retain their **LGPL-2.1-or-later**
license and copyright notices; see [docs/bink.md](docs/bink.md#license) and
[the LGPL text](LICENSES/LGPL-2.1-or-later.txt). Other third-party code and
dependencies retain their respective licenses.

This license grant covers the project's original contributions. It does not
grant rights to external Halo game code, assets, proprietary SDK code, or
data copied from an original XBE by the seeded build.
