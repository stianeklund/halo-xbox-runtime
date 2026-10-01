# Standalone nxdk build (experimental)

Goal: build a runnable original-Xbox `default.xbe` from the reconstructed
source with [nxdk](https://github.com/XboxDev/nxdk). The build must not need the
Microsoft XDK or the original `cachebeta.xbe`. Users supply only their own Halo
game data (`maps/`).

The patched-XBE build in the halo repository remains the reference and
decompilation target. This repository builds against a halo checkout
(`HALO_ROOT`: its `src/`, `kb.json` and `tools/`) and never changes reference
codegen. The only source-level
hook is `HALO_STANDALONE`, which the reference build does not define.

**Status: experimental.** A stereo nxdk audio backend was added on 2026-09-30;
linked-code tests pass and audible playback was confirmed on xemu after fixing
the transport's dependency on S/PDIF completion. Real Xbox hardware remains
unverified.
The previous bring-up work (2026-09-27) moved to the reference build: compiling the
patch against nxdk headers instead of `third_party/xbox`. The reference build
already needs no XDK at compile time: clang, in-repo headers, and import libs
made by `lld-link /lib` from `.def` files. Only `ENABLE_XDK_HYBRID` (off by
default) uses the XDK. The seeded image boots into `game_initialize` (see
[Boot results](#boot-results-seeded-image-xemu)).

## Build

```bash
export NXDK_DIR=/path/to/nxdk          # built checkout (make -C $NXDK_DIR/samples/hello once)
export HALO_ROOT=/path/to/halo         # halo checkout or worktree; default ../halo
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$PWD/toolchains/llvm.cmake
cmake --build build -j
# -> build/default.xbe, halo.map, generated/link_report.json
cmake --build build --target standalone_inventory   # -> deps_inventory.json
```

## Audio

`HALO_STANDALONE_AUDIO` defaults to `ON`. `compat/audio/dsound_nxdk.c` implements
the Xbox DirectSound entry-point ABI and stream vtables used by Halo.
`HALO_STANDALONE_AUDIO_BACKEND` defaults to `apu`. This experimental
backend shares this adapter and uses nxdk-audio hardware voices.
Select `software` explicitly for the software output worker. Existing CMake
caches retain their selected backend; pass `-DHALO_STANDALONE_AUDIO_BACKEND=apu`
to switch them. `./build.sh` selects APU unless the backend environment variable
requests software. See [audio-apu.md](audio-apu.md) for its build steps,
local driver patches, validation, and remaining feature gaps.
`compat/audio/mixer.c` decodes mono/stereo 8/16-bit PCM and Xbox IMA ADPCM
(format `0x69`, 36 bytes per channel, 64 samples per block), resamples to
48 kHz, applies volume/pitch, mixes voices, and clips to signed 16-bit stereo.
Static buffers support playback, stop, seek, and looping.

`crt/nxdk_audio.c` uses nxdk's XAudio HAL, which is already linked through
`libnxdk_hal`; SDL is not required. Three contiguous 1024-frame buffers feed
AC97. The interrupt's DPC signals a semaphore; the worker uses a bounded wait
and tracks analog DMA's current descriptor with supported 32-bit MMIO reads.
This avoids waiting indefinitely for S/PDIF completion, which xemu currently
does not supply, and counts multiple completions when DPC signals coalesce.
The worker does the mixing independently of game frames. Game callbacks execute
only from `DirectSoundDoWork`, outside the mixer lock. A stream retains each
packet until that callback is dispatched. Flush and final release invoke
`DirectSoundDoWork` before returning to drain aborted packets: Halo can reuse
an idle channel immediately, and delayed aborts would decrement the new sound's
pending count. Completions indicate that data was mixed into the output queue;
they do not indicate the precise time samples reached the speakers.

The backend also supplies the missing game-side `dsound_channel_queue_packet`
and creates channel streams with the proper format and completion callback.
Channel offsets and packet splitting were checked against build 2276 at
`0x1c9670` and `0x1cb210`. The reference Halo checkout is not modified.

This is a stereo playback implementation. Spatial positioning/attenuation,
HRTF, Doppler, DSP reverb, and surround mix bins remain
unsupported (Bink movie sound streams through `compat/bink/sound.c`); their existing entry-point stubs are visible in `link_report.json`.
Audible playback was confirmed on xemu on 2026-09-30. A channel-reuse completion
defect caused rapid plasma-pistol fire to drop effects; the user confirmed that
the fix resolves that gameplay issue. Subtle level/mixing differences remain
reported. A brief live mixer sample showed no clipping, but does not establish
level accuracy or exclude clipping during louder gameplay. Real Xbox
playback and real-time performance with many voices still need testing.
Existing game bring-up blockers can prevent sounds from being
submitted even when the audio backend is working.

To select the silent backend:

```bash
cmake -S . -B build-seed -DHALO_STANDALONE_AUDIO=OFF
cmake --build build-seed -j
```

To run the linked 32-bit checks with Python `pefile` and `unicorn` installed:

```bash
cmake -S . -B build-seed -DHALO_STANDALONE_AUDIO=ON
cmake --build build-seed -j
python3 tools/test_audio.py build-seed
python3 tools/audio_status.py build-seed/halo.map --host 127.0.0.1
```

The tests simulate allocation, locking, and nxdk output while executing the
compiled adapter and mixer. They check decoding, stereo mixing/clipping,
resampling, volume/pitch, packet capacity, callbacks/events, pause/flush,
reference counting, buffer loops, the game's channel queue path, repeated
channel stop/reuse with the actual game callbacks and cache references, and stack
cleanup for the actual `__stdcall`/`thiscall` methods. Transport checks execute
the linked worker with simulated analog progress and no S/PDIF callbacks,
including underrun recovery and descriptor wrap. They do not emulate AC97.

`audio_status.py` reads ordinary RAM counters for packet submission/completion,
rendered buffers, nonzero frames, peak sample magnitude, and the worker's saved
DMA register snapshots. Use the map matching the deployed build. Do not read
AC97 MMIO with XBDM `getmem`: its bytewise reads can trigger xemu's
`nabm_readb` assertion. The driver itself uses supported 32-bit accesses.

`gen_legacy_image.py` needs `capstone` when given `--xbe` (always the case for
the seeded variant) and exits if it is missing. Point CMake at an interpreter
that has it, e.g. `-DPython3_EXECUTABLE=/usr/bin/python3`. Without it the
generator used to fall back to a per-call import that re-scanned `sys.path` on
`/mnt/g` and appeared to hang at `legacy.s`.

Bring-up variant (NOT standalone). This variant seeds the original initialized
`.rdata`/`.data` from a user-supplied XBE, so boot testing can run past data that
has not been recovered yet:

```bash
cmake -S . -B build-seed -DCMAKE_TOOLCHAIN_FILE=$PWD/toolchains/llvm.cmake \
      -DHALO_STANDALONE_SEED_XBE=$HALO_ROOT/halo-patched/cachebeta.xbe
```

## Boot-test loop (xemu / devkit over XBDM)

```bash
export HALO_WINDOWS_REEXEC=1           # bridged xemu is reachable from WSL only
python3 $HALO_ROOT/tools/xbox/xbdm_rdcp.py --host 10.0.0.21 --timeout 60 \
    --sendfile build-seed/default.xbe 'E:\GAMES\halo-patched\standalone_seed.xbe'
python3 tools/xbdm_boot_watch.py --host 10.0.0.21 \
    --title 'E:\GAMES\halo-patched\standalone_seed.xbe' --seconds 40
python3 tools/missing_dump.py build-seed/halo.map --host 10.0.0.21
python3 tools/symbolize.py build-seed/halo.map --host 10.0.0.21 --thread 48
python3 tools/crash_report.py build-seed/halo.map --host 10.0.0.21
```

`crash_report.py` decodes the first-exception snapshot (see Debugging notes).

Upload the image under its own name next to the real game files. That keeps
`D:\maps` available and leaves the deployed `default.xbe` untouched.
`missing_dump.py` lists, in first-hit order, every stubbed symbol the run
reached. That list is the burn-down queue.

## How the image is put together

| Piece | Where | What |
|---|---|---|
| Game code | `src/` (via `src/CMakeLists.txt`) | Same sources and clang flags as the reference build. Adds `-DHDATA=extern -DHALO_STANDALONE=1`. |
| Legacy layout | `generated/legacy.s` (`tools/gen_legacy_image.py`) | First `.text` input, so it occupies VA `0x11000..0x642000`: the original image range. Our code starts at `0x642000`. |
| Trampolines | legacy layout, original code range | A `jmp` at every kb.json function start. Implemented functions go to our C. Implemented `@<reg>` functions go through the reference build's reverse thunk (`generate_reverse_thunk` in halo's `tools/build/patch.py`, relocated), which adapts the register ABI. Kernel-forwarder routines go to the kernel import. An unimplemented original that is a lone `jmp rel32` follows it to the target's trampoline; one whose body is just `ret` / `ret N` becomes that instruction. Unimplemented XAPI `__stdcall` routines with a typed kb.json decl go to nxdk's winapi through a callee-pops bridge, and single-function LIBCMT objects go to the pdclib routine of the same name. Everything else goes to a logging stub; for a `__stdcall` decl the stub pops the declared argument bytes. Raw code addresses and function-pointer tables in `.data` therefore keep working. |
| Legacy data | legacy layout, `.rdata`/`.data` range | Zero-filled. kb.json data symbols resolve to their original VAs, so named and raw-address accesses agree. With `HALO_STANDALONE_SEED_XBE`, the initialized bytes come from that XBE (dev only). |
| Stubs | `generated/stubs.s` | One per unresolved function. Each logs its name once (`halo_standalone_missing`) and returns 0. It pops what the original's `ret N` pops (`original_pops`: a capstone walk of the original body, counted as `stub_pops_from_ret`), else the callee-pops size taken from the symbol decoration. A stub that pops the wrong amount skews ESP and the caller later returns to 0. |
| Unknown-function stubs | `generated/legacy.s` | Code addresses stored in `.rdata`/`.data` that are not kb.json function starts (`unknown_data_referenced_functions`). The previous kb function never reaches them, and a padding byte or `ret` precedes them. Without an entry, a call slides into the next trampoline and runs the wrong function. Each gets a `__halo_legacy_<va>` logging stub (`unknown_function_stub` in `link_report.json`). The 368 Ghidra-confirmed ones are now in kb.json `<common>` as `FUN_<addr>`, so they become ordinary missing stubs. |
| Bridges | `generated/stubs.s` | `__stdcall` XDK names go to nxdk's cdecl winapi. kb.json `void X(void)` placeholders are never bridged: they say nothing about the real arguments (RaiseException, PulseEvent), so those get typed entries in `compat/xapi/thread.c`. LIBCMT `crt_X` names go to pdclib `X`. Original `jmp [kernel thunk]` / `mov eax,[thunk]` routines go to kernel imports. |
| Compat layers | `compat/<library>/` | Hand-written replacements for XDK library routines, transcribed from the binary where they are thin wrappers. One folder per library: `audio` (DirectSound and the mixer), `bink`, `crt`, `d3d8`, `xapi`, `xnet` (see below). |
| Entry | `standalone_entry` (`compat/xapi/init.c`) | Does what `XapiInitProcess` does for Halo's init flags (mounts the utility drive `Z:` using the XAPI cache-partition database), then enters nxdk's `WinMainCRTStartup`. |
| nxdk | `libwinapi`, `libxboxkrnl`, `libxboxrt`, `libpdclib`, `libnxdk{,_hal,_automount_d}` | CRT startup (`WinMainCRTStartup` → Halo `main`), kernel imports, D: automount. |
| Post-link | `tools/xbe_make_writable.py` | cxbe marks `.text` read-only. The legacy data lives there, so the step sets the writable flag. It also clears cxbe's 64MB-limit init flag to match the original's flags (0x1), because the debug build needs devkit memory. |

Compat files:

| File | Replaces |
|---|---|
| `compat/xapi/init.c` | `XapiInitProcess` utility-drive mount: `XapiSelectCachePartition`, `XMountUtilityDrive`, `XapiValidateDiskPartition`. Formatting goes through nxdk `nxFormatVolume`. |
| `compat/xapi/memory.c` | `XPhysicalAlloc`, `physical_memory_protect` |
| `compat/xapi/thread.c` | `RaiseException`, `PulseEvent` (kb.json only has placeholders; callee-pops ABI from the original `ret N`) |
| `compat/xapi/fileio.c` | `ReadFileEx`/`WriteFileEx` (NtReadFile/NtWriteFile with the kernel `NtUserIoApcDispatcher` APC; used by the cache-file reader and the DVD→Z: copy thread) |
| `compat/xapi/xvutil.c` | `GetFileAttributesA` (kb `file_get_full_attributes`), `GlobalMemoryStatus`, `Sleep`, `SetFileAttributesA`, `RemoveDirectoryA`, `XQueryValue`, the `LocalAlloc` twin at 0x1d0bb9 and `XapiSetLastNTError`. A 0-returning stub is wrong for most of these: GetFileAttributesA returning 0 means "file exists". |
| `compat/xapi/heap.c` | `LocalAlloc`/`LocalFree`/`LocalReAlloc`/`LocalSize` (kb `FUN_001d0c48`/`c16`/`c65`/`c02`) on the pdclib heap |
| `compat/xapi/launch.c` | `XGetLaunchInfo` |
| `compat/xapi/input.c`, `crt/nxdk_usb.c`, `crt/nxdk_keyboard.c` | XAPI controller input on nxdk's USB/XID host driver. `XInputSetState` is asynchronous like the original (0x24c427): it returns `ERROR_IO_PENDING`, and the completion writes `dwStatus` and signals `hEvent`. nxdk allocates transfer descriptors without masking its interrupt, so USB completions never resubmit. They signal a worker thread that re-arms reads and finishes output requests under the USB lock that every thread-side caller holds. A resubmitting completion can share a TD with a rumble write, and xemu then aborts in `usb_packet_copy`. |
| `compat/xapi/keyboard.c` | The debug keyboard queue: `XInputDebugInitKeyboardQueue` (0x252d73) and `XInputDebugGetKeystroke` (0x252ad9). An empty queue returns `ERROR_HANDLE_EOF` (0x26) as the original does; a 0 stub means "key available" and `input_update_keyboard_devices` spins forever. |
| `compat/xapi/savegame.c` | XAPILIB saved-game and directory-enumeration routines. |
| `compat/xapi/nickname.c` | Nicknames (0x1d26f3 set, 0x1d29eb find first) in `NICKNAME.XBN`. |
| `compat/xapi/signature.c` | Content signatures (`XCalculateSignature*`). |
| `compat/d3d8/device.c` | Device creation, the device fields that game and lifted D3D code read (`D3D_g_pDevice` +0x1c/+0x3f0/+0x410/+0x2150), textures/cube/volume/vertex/index/palette resources in Xbox header layout with contiguous memory, locks, surface descriptions, caps (the original table), transforms, the D3DX projection/transform helpers, `Clear`, and a `Present` that returns once the vblank has latched the flip (the device has one back buffer). |
| `compat/d3d8/state.c` | Render-state and texture-state setters, transcribed from the XDK with the same pushes and mirror stores. |
| `compat/d3d8/draw.c` | D3D8 stage 2 draw path on pbkit, transcribed from the XDK: lazy state flush (0x1f1b50), vertex shaders and FVF (Create/Load/Select/Set/SetConstant), `SetPixelShader`/`SetPixelShaderProgram`, `SetTexture`/`SetPalette`/`SetTextureStageState`, `SetStreamSource`/`SetIndices`, `DrawVertices`/`DrawIndexedVertices`, immediate mode (`Begin`/`SetVertexData*`/`End`), and the init-time default state (0x1f3000). XDK const tables are C arrays with their source VA. Fixed-function color/alpha operations it does not transcribe, lighting (0x1f1640) and the inverse model-view transform (0x1f2650) call `halo_standalone_missing`. Resource `Data` holds physical addresses (DMA object 3 spans RAM). Debug knob `-DHALO_STANDALONE_TRACE_TEXTURE_STAGES=ON` logs the pushed `SET_SHADER_STAGE_PROGRAM` and each stage's texture, pushed format and control0 before each draw (first 64 draws, plus every draw that would trip xemu's `get_sampler_type` `dim == 2` assert, tagged `DIM!`), to DbgPrint and COM1. |
| `compat/d3d8/internal.h` | What the three D3D8 files share: the device layout and state mirrors, and the checked push-buffer API. |
| `compat/crt/string.c`, `compat/crt/stdio.c`, `compat/crt/wprintf.c` | LIBCMT internals kb.json only knows by address: `__ascii_stricmp` (0x1e3a50), the `crt_*` stdio/string routines, and the wide printf family. |
| `compat/xnet/xnet.c`, `compat/xnet/security.c`, `crt/nxdk_xnet.c` | The Xbox Winsock ABI on lwIP and the XNet secure transport; see [standalone-networking.md](standalone-networking.md). |
| `compat/audio/dsound_nxdk.c`, `compat/audio/mixer.c`, `crt/nxdk_audio.c` or `crt/nxdk_audio_apu.c` | DirectSound stream/buffer adapter, PCM/Xbox ADPCM mixer, and the AC97 or APU transport (see Audio and [audio-apu.md](audio-apu.md)). |
| `compat/audio/dsound_silent.c` | Optional silent backend (`HALO_STANDALONE_AUDIO=OFF`). |
| `compat/bink/` | RAD Bink playback, file IO and codecs; see [bink.md](bink.md). |
| `crt/msvc_print.c` | nxdk's `_PDCLIB_print` rebuilt so a NULL `%s` prints `(null)` like MSVC LIBCMT. pdclib calls `strlen(NULL)` and faults; Halo relies on the MSVC behaviour. Built with nxdk's flags (`crt/CMakeLists.txt`) and linked ahead of `libpdclib`. |

Source adaptations (all under `HALO_STANDALONE`, none in reference codegen):
- `main` skips `update_loaded_module_section_attributes()`. That debug routine
  makes the code section read-only, which would also cover the legacy `.data`.
- `main`'s `__except` filter first calls `standalone_report_exception`
  (`standalone_runtime.c`), which copies the first EXCEPTION_RECORD, CONTEXT and
  256 stack words into globals before the unwind reuses the stack.
- `physical_memory_map.c` keeps the build-2276 cache sizes (tag and texture
  0x1600000, sound 0x400000), and `xbox_texture_cache.c` no longer takes the
  retail 8MB texture cache for `HALO_STANDALONE`. `bink_playback_start` steals
  a 16MB pool from the texture cache for hi-res movies (intro.bik); with 8MB
  it trips `xbox_texture_cache.c:319` (`remaining_page_count>0`).

## Dependency inventory (2026-09-29, halo main 551a28c23, 217 objects)

`tools/deps_inventory.py` measures three layers
(`cmake --build build-seed --target standalone_inventory`). It reads the
link's own object list (`generated/objects.txt`), so objects left behind by an
earlier `HALO_ROOT` in the same build directory are not counted.

**Layer 1: link-level symbols** that the objects leave undefined once
`halo.xbe.lib` and the generated reg-arg thunks are removed. The provider
columns come from `generated/link_report.json`, which classifies a few symbols
differently from the inventory, so rows do not always add up exactly.

| Category | Undefined | Provided now | Still stubbed | Replacement plan |
|---|---:|---|---:|---|
| halo_code | 160 | 9 empty bodies (the original is a lone `ret`) | 151 | Lift with `/lift` (the closure below lists 395 in total) |
| halo_data | 172 | 172 → legacy VAs | 0 | Recover initializers for the initialized range (see layer 2) |
| kernel | 43 | nxdk libxboxkrnl | 0 | Done |
| xapi | 40 | 32 stdcall→cdecl winapi bridges, 10 direct nxdk, 1 kernel variable, plus `compat/xapi/` | 6 | Save games (no U: drive yet), remaining launch/signature helpers |
| crt | 89 | 24 pdclib aliases, 16 direct pdclib/xboxrt | 48 | Mostly unnamed LIBCMT internals (`FUN_001d98xx` = `exit` family, `__open`/`__read`/...). Identify, then alias or compat. |
| d3d8 | 8 | 1 data symbol, plus `compat/d3d8/` on pbkit | 7 | Push-buffer control (`KickPushBuffer`, `SetPushBufferSize`, `InsertCallback`, `IsBusy`, `BlockOnTime`, `PersistDisplay`). Inside the compat layer, fixed-function combiners, lighting and texture transforms still call `halo_standalone_missing`. |
| dsound | 30 | `compat/audio/dsound_nxdk.c` / nxdk AC97 | 30 | Stereo playback is implemented; spatial/DSP entry points remain stubbed. Counts in this historical inventory predate the audio backend. |
| xnet | 28 | 1 direct nxdk, 1 bridge, plus `compat/xnet/xnet.c` | 27 | nxdk lwIP sockets, or stub out system link at first |
| bink | 63 | 13 data symbols, plus `compat/bink/` | 0 | Done: open Bink 1 decoder and RAD runtime glue; see [bink.md](bink.md). |
| unknown | 37 | nxdk winapi (`CopyFileA`, `Sleep`, ...), `WinMainCRTStartup` | 0 | Done |

**Layer 2: fixed image addresses** compiled into our code (`*(T *)0x2c84c0`,
`(void *)0x5a90e0`, ...). There are 5,977 distinct addresses at 15,658 sites. The
legacy layout keeps all of them working, so none blocks linking. They do decide
what data must be reconstructed:

| Kind | Addresses | Meaning |
|---|---:|---|
| halo_data_bss | 2,898 | Zero-initialized. Correct as-is in the legacy layout. |
| halo_data_init | 1,294 | Needs recovered initial contents (strings, tables, float constants) |
| maybe_constant | 1,072 | In-range immediate with no kb symbol. Probably plain integers; review. |
| halo_code | 341 | Code addresses used as values. Served by the trampolines. |
| d3d8 / xapi / crt / dsound / bink / unknown | 313 / 37 / 10 / 2 / 1 / 9 | Library internals reached by raw address; handled with each library. Most d3d8 sites are in `compat/d3d8/draw.c` (350 sites), which uses XDK const tables and the render-state mirror at their original VAs. |

Initialized data that the unseeded image currently lacks: `.rdata` 0x75430 bytes
and `.data` 0x69a3c bytes (plus DOLBY 0x6d84 and BINKDATA 0x4250). Seeded runs
count it as `seeded_bytes_from_cachebeta` (958,012). That counter must reach 0.

Measured 2026-09-27 against the compiled standalone objects:
- The C code references 1,761 legacy initialized-data addresses: 898 `.rdata`, 852 `.data`, 13 `BINKDATA`. About 220 of them are strings.
- A conservative closure extends each reached object to the next reached address and follows every in-seed pointer to a fixpoint. It keeps 642,867 of the seed's ~642,900 nonzero bytes (6,481 objects).
- In practice the reconstructed code reaches all of the seeded data; the rest of the 958 KB is zeros. Pointer tables such as the hs function table and the tag-group tables link almost everything together.
- Sparse seeding therefore saves nothing. The seed goes away only as the data is recovered into source.
- Code pointers inside the data point at original VAs. The legacy trampolines keep those valid, so the recovered initializers can keep the same values as symbols.

**Layer 3: closure.** Walking the original binary's direct calls from the
layer-1/2 roots, plus unported functions stored in `.data` tables, yields
**395 of 595 unported Halo functions** (~216 KiB of x86; 2026-09-26: 454 of
546, ~335 KiB). They are reached through data tables (149), link symbols (131),
direct calls (106) and fixed addresses (9). The largest groups are `<common>`
(67), `ai_debug` (20), `vehicles` (19), `ui_widget` (18) and
`ui_widget_game_data_input_functions` (15). The same walk reaches 42 D3D8,
19 CRT, 16 XAPI and 1 DirectSound leaves, and 1,789 `.data`/`.rdata` addresses.

## Boot results (seeded image, xemu)

The image boots through `standalone_entry` (Z: mounted) and nxdk CRT start-up
into Halo `main`: `rasterizer_preinitialize` (D3D device created through the
compat layer) → `physical_memory_allocate` → `shell_initialize` →
`tag_files_open` (named `tag_files_close` in kb.json) → `real_math_initialize`
→ `random_math_initialize` → the cache-file reader (cache files copied to Z:) →
`main_loop` → `game_initialize`.

Last run (2026-09-27, later): map d20 runs with no assert and no exception.
Frames are presented at about 30 fps: over 6 s the swap count at 0x1fe670 rose
by 179, against 363 vblanks. That run followed a series of halt fixes, each
found by reading Halo's own error text (see the loop below). They lifted about
90 dormant bodies (`ported:false`): biped physics, collision sweep, camera
effect matrix, BSP-switch callbacks, `render_scene`, lens flares, AI look /
perception / props (`prop_add` was the prop-chain root cause), sound and
weapons.

Nothing visible is drawn yet: textures, shaders, streams and draw calls are
still unimplemented (see burn-down item 2). 112 distinct missing stubs are hit.
Most are XDK D3D/DirectSound entry points. The rest are deeper AI (combat,
perception refresh, pursuit, encounters), collision (`collision_bsp_test_pill`)
and `inverse_kinematics_adjust_matrices`.

Halt loop: a plain blue screen is Halo's `halt_and_catch_fire`, with the guard
byte at 0x46e392 == 1. Read the assert text with `getmem addr=0x5aa8e8
length=0x600`, then symbolize the addresses in it against `halo.map`.

Every XBDM call to a bridged guest (upload, getmem, and `tools/*`)
needs `HALO_WINDOWS_REEXEC=1`. Upload first and wait for the `=> E:\...`
confirmation, then boot.

Build switch: `-DHALO_STANDALONE_GPU_STATE=OFF` keeps the D3D state mirror and
device-field stores, but drops their push-buffer methods, for bisecting GPU
faults. One run with it ON took xemu down, deeper in the render path than
current runs reach; that is not yet reproduced.

Earlier blockers, now fixed:
- `ai_debug_initialize` asserted because its 10MB of debug arrays did not fit.
  cxbe set `XINIT_LIMIT_DEVKIT_MEMORY`, and the original's init flags are only
  0x1; `xbe_make_writable.py` now clears it.
- `sound_initialize` → `sound_dispose_from_old_map` asserted in
  `data_make_valid`: `dsound_initialize` and `dsound_initialize_channel` are
  now lifted, and `dsound_silent.c` returns a stream object from
  `IDirectSound_CreateSoundStream`, which the channel code asserts on.
- `'levels\ui\ui' does not appear to be a cache file`: the LIBCMT
  `__ascii_stricmp` stub returned 0, so every cache slot name "matched". It is
  now in `compat/crt/string.c`. `crt_sprintf` was stubbed too; the bridge now
  maps kb.json `crt_X` names in multi-function LIBCMT objects to pdclib `X`.
  The MSVC-vs-C99 differences of the aliased `snprintf`/`vsnprintf` (MSVC
  `_snprintf` returns -1 and skips the terminator on truncation) and of `rand`
  (a different generator) are not emulated yet.
- The cache copy thread asserted in `FUN_001ba930` (the write-completion wait
  at tags.c 0x5d6/0x5da/0x5db). The Z: cache slot picker `FUN_001bd210` and the
  profile writers `FUN_001c31f0` / `FUN_001c19e0` are now lifted. After that,
  `compat/xapi/fileio.c` still passed the import library's jmp thunk as the
  APC routine, because the vendored header declares `NtUserIoApcDispatcher`
  without dllimport. On xemu the kernel then ran the completion as a kernel APC
  at arbitrary points: the alertable `WaitForSingleObjectEx` timed out instead
  of returning `WAIT_IO_COMPLETION`. Like the original's `push [0x25318c]`, the
  compat now passes the export address from `__imp__NtUserIoApcDispatcher@12`.
- The subsystem pools (device groups, glow, light volumes, lightnings, unit
  globals) were never allocated, because their `*_initialize` /
  `*_initialize_for_new_map` / `*_dispose_from_old_map` routines were missing
  stubs. They are lifted now.
- `d3d_find_flipcount` (d3d_intimacy.cpp #53) reads XDK device fields at fixed
  addresses: interrupts +0x23ac, vblank callback +0x24ec, flip count +0x2518
  of the static device at 0x1fbb10. `compat/d3d8/device.c` now places the compat
  device there, stores the callback, and counts swaps in `Present`. A
  time-critical thread stands in for the XDK vblank DPC: it completes the
  flip and calls `main_vertical_blank_interrupt_handler` (frame timing and
  `input_tick`) on every vblank.
- The LIBCMT bridge also maps decorated `_X` names in multi-function objects
  (`_isalpha`, `_fread`, and the other `is*`/`wcs*` routines) to pdclib `X`.
- `player_control.c` #177: `get_local_player_input_blob` was a missing stub,
  so the input blob kept its 0xfa fill and the debug unit-switch bits fired.
  It is lifted now.
- `observer.c` #265 `!updated_for_frame` and #233 `Invalid camera command`:
  `observer_set_camera` and the camera update functions it depends on were
  missing stubs. They are lifted now.
- `main_rasterizer_throttle` crashed in `strlen`, because its
  `"%6I64d(targ%6I64d %s%2d)"` uses MSVC's `I64` length modifier, which pdclib
  does not parse. `crt/msvc_print.c` rewrites `I64`/`I32`/`I` before pdclib
  sees them.
- pdclib parses `%f`/`%e`/`%g` but formats nothing and never consumes the
  double, which shifts every later argument by eight bytes. `crt/msvc_print.c`
  now formats them like the MSVC CRT (three-digit exponent, `1.#INF`,
  `1.#QNAN`).
- `game_engine_player_get_change_color` got a NULL pointer from the missing
  stub `FUN_001c0ee0`, which is now lifted as `player_profile_get_rgb_color`.
- A CRT-only edit did not relink the XBE: with the Makefile generator, a
  custom command's DEPENDS on another target's objects does not order the
  build. `CMakeLists.txt` now adds a target-level dependency.
- `telnet_console_initialize` asserted `transport_initialized` because the
  XNet stubs never reported a link; `compat/xnet/xnet.c` now covers this.

Link report for that image: implemented 5500, jmp_thunk 51, kernel_forwarder
152, library_bridge 47, missing_stub 2478, reverse_thunk 913,
stub_pops_from_ret 2402, unknown_function_stub 388.

Fixes that got it this far, beyond lifting the functions on the
`missing_dump.py` list: callee-pops from the original `ret N`, stubs for
data-referenced functions missing from kb.json, the MSVC `(null)` printf, and
the `XInputDebugGetKeystroke` EOF result.

Dormant lifts: the standalone build compiles every C body in `src/`,
including ~140 functions that kb.json still marks `ported:false`. The
reference build keeps running the original bytes for those, so only the
standalone image executes them. They are unverified, and some are wrong:
`FUN_001a25e0` (bipeds.c) read the collision-BSP surface and plane blocks off
the wrong base and used an uninitialized index; both are fixed now. When the
standalone image asserts in C that the reference build never runs, check the
function's `ported` flag first. To list them, join kb.json's non-ported
entries against the `*.c.obj` symbols in `halo.map`.

Debugging notes:

* XBDM's notify channel reattaches after the title reboot and misses early
  `debugstr` / exception events, and XBDM execute breakpoints and `stopon` are
  inert under xemu. `missing_dump.py` reads the stub-hit table from memory
  instead.
* After a caught exception, the unwind and the halt path overwrite the
  EXCEPTION_RECORD / CONTEXT on the stack, so stack scans find later
  exceptions, not the first. `crash_report.py` reads the snapshot that
  `standalone_report_exception` took instead. It prints the code, address and
  registers (Xbox CONTEXT: registers start at +0x208), plus every stack word
  that points into code. Original code omits frame pointers, so there is no
  EBP chain to walk.
* `symbolize.py --thread N --scan WORDS` lists the code pointers on a live
  thread's stack, for the same reason. A thread spinning in
  `halt_and_catch_fire` still has the failing call chain above it.
* Inspect a running thread by suspending and resuming it alone. A global XBDM
  halt followed by `getcontext` killed the title once.
* xemu's QMP monitor is at 127.0.0.1:4446 (from WSL, the 10.0.0.24 guest). HMP
  `info registers` and `x /Nwx <addr>` over QMP read CPU state without XBDM.
* xemu's gdbstub (`xemu_attach_gdb`, 127.0.0.1:1234) supports hardware
  breakpoints (`hbreak`). Start gdb with `-nx`; the repository `.gdbinit`
  attaches on its own. Do not reboot the title while gdb holds the CPU: that
  killed the xemu instance.

## Physical memory budget (2026-09-28)

Titles get contiguous memory (`XPhysicalAlloc`, `MmAllocateContiguousMemoryEx`)
only from the low 64MB, even on a 128MB console with `XINIT_LIMIT_DEVKIT_MEMORY`
clear. Virtual allocations may use the upper 64MB; contiguous ones do not. The
walkmem layouts for the two builds:

| Contiguous allocation | 2276 / patched | standalone seed |
|---|---|---|
| game state + tag cache (fixed, `physical_memory_allocate`) | 25.2MB | 25.2MB |
| D3D (framebuffers, push buffer, device) | 5.75MB | ~13.4MB |
| sound cache | 4MB | 4MB (fails today) |
| texture cache | 22MB (0x1600000) | 22MB (0x1600000) |

The texture cache must be 0x1600000, the size in both 2276 and PAL 2342.
`bink_playback_start` steals a 16MB pool from it for hi-res movies
(`intro.bik`, flags 0xe6). An 8MB cache leaves 382 stealable pages against the
1025 requested and halts on `xbox_texture_cache.c:319 remaining_page_count>0`
on the way back to the main menu.

The standalone's extra ~7.6MB of D3D memory has two sources:

* pbkit's `pb_init` always allocates its own triple-buffered framebuffers
  (tile 0) and a Z-compressed depth buffer (tile 1). That is 4 × 640×480×32bpp,
  about 4.7MB. `compat/d3d8/device.c` never uses them: it renders into its own
  front, back and depth surfaces.
* The push buffer is `PUSH_BUFFER_BYTES` (4MB). Halo asks the XDK for 768KB,
  but pbkit never wraps, so one frame must fit.

### Future exploration: pbkit without its own surfaces (option C)

Add an nxdk pbkit option, for example `pb_set_surface_allocation(0)` called
before `pb_init`. With it, `pb_init` skips the FB/DS `MmAllocateContiguousMemoryEx`
calls and the tile 0/1 assignments, and the caller supplies its targets. The
D3D compat layer already binds its own targets through DMA object 3 with
physical offsets, the way the XDK does, so pbkit's buffers are dead weight.
This is the cleanest way to recover the 4.7MB. It changes the external nxdk
checkout (upstream `XboxDev/nxdk`), so it is a patch to carry or upstream.
Things to check:

* `pb_kill` and `pb_reset` paths that assume `pb_FBAddr[]` or
  `pb_DepthStencilAddr` are non-zero.
* `pb_back_buffer()` and `pb_show_*` callers.
* Whether the D3D compat depth surface should get a Z-compression tile of its
  own, as the XDK gives its depth buffer.

The push-buffer size (4MB against the XDK's 768KB) is a separate follow-up. It
needs push-buffer wrap support in pbkit, or a measured per-frame high-water mark.

## Burn-down order

1. Boot path: lift the Halo functions on the `missing_dump.py` list as they
   appear (`FUN_001087b0` chain, `cache_file_read_header_into_slot`,
   `simple_cache_copy_thread`, ...) via `/lift`, and add XAPI compat routines
   (save-game enumeration, per-title T:/U: drives).
2. D3D8 stage 2 on pbkit: the resource layer exists. `compat/d3d8/device.c` sets
   the video mode, scans out the front buffer (`Present` swaps the bits of
   the back/front buffer headers and points PCRTC at the new front), keeps the
   render target and depth buffer from `SetRenderTarget`, and clears through
   the GPU. pbkit is initialized in `CreateDevice`. Every vblank wait goes
   through `pb_wait_for_vbl`, because pbkit and `XVideoWaitForVBlank` would
   each connect GPU IRQ 3. Targets are bound through pbkit's all-RAM DMA
   object 3 with physical surface offsets, as in the XDK, and `NV097_CLEAR_SURFACE`
   takes the Xbox `D3DCLEAR` bits unchanged. `Present` waits for the GPU to go
   idle, then calls `pb_reset`: pbkit never wraps its push buffer on its own.
   Verified on xemu with the CPU clear (the game's clear color lands in both
   buffers); the CPU path remains the fallback when `pb_init` fails.
   XDK device layout (0x1fbb10): +0 push-buffer put pointer, +4 threshold,
   +8 deferred-state dirty flags (per-state masks at 0x1f9d50). The
   `D3D__RenderState` mirror at 0x1fb698 is read back by the game, so each
   setter must keep storing to it. Halo's own code reaches into the XDK device only through
   `d3d_intimacy` (the vblank and flip fields above); Bink is the other
   direct user. Next is feeding the
   render-state (`D3DDevice_SetRenderState_Simple(method, value)` is already
   an NV2A method/value pair), shader, texture-stage and draw calls into a pbkit
   push buffer, then `Present` flipping the back buffer. The game writes the
   D3D render-state mirror (`0x1fb6xx`) itself; that stays in the legacy
   layout.
3. DirectSound: stereo software mixing is implemented and audible on xemu.
   Verify real Xbox playback and performance, then add spatial audio and DSP effects.
4. Data recovery: move the `halo_data_init` ranges into source-level
   definitions, generated into the legacy layout, until
   `seeded_bytes_from_cachebeta` is 0.
5. XNet: stub first, replace later. (Bink is done, see [bink.md](bink.md).)
6. Once nothing is stubbed or seeded, `build/default.xbe` is the
   standalone deliverable.
