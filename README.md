# halo-nxdk

Standalone original-Xbox build of the reconstructed Halo CE source, linked
against [nxdk](https://github.com/XboxDev/nxdk) instead of the Microsoft XDK.

This repository contains only the nxdk-side pieces: XDK compatibility layers
(`compat/`), CRT fixups (`crt/`), the runtime entry (`standalone_runtime.c`),
and the build and debug tools (`tools/`). The game code, `kb.json` and the
reference build tooling come from a halo checkout, which you point at with
`HALO_ROOT`.

```bash
export NXDK_DIR=/path/to/nxdk      # built nxdk checkout
export HALO_ROOT=/path/to/halo     # halo checkout; default ../halo-clean-main
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$PWD/toolchains/llvm.cmake
cmake --build build -j             # -> build/default.xbe
```

Or use `./build.sh [seed|standalone|nogpu] [--fresh] [--inventory] [--deploy]`,
which sets the variables above to local defaults (see `./build.sh --help`).

See [docs/standalone-build.md](docs/standalone-build.md) for the design, the
seeded bring-up variant, the boot-test loop and the status.
