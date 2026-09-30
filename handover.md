# Audio handover

## Update — 2026-10-01

APU is now the default; software remains available explicitly. The adapter
implements approximate distance/cone attenuation, occlusion levels, deferred
updates, stereo panning, and the reference 2D voice headroom. HRTF filtering,
Doppler, DSP reverb, surround output, and Bink movie audio remain pending.

The user traced xemu's audio stutter to its enabled DSP JIT; disabling it
removed the stutter in that session. This is not evidence of a title buffering
defect. Real Xbox playback and reference audio comparisons remain unverified.

The current seeded APU and software builds passed 14 linked audio regression
groups each. Keyboard queue, CRT icon comparison, and follow-iterator checks
also passed. USB/XID debug keyboard input and a remote console mailbox are
implemented. The `right-stick` assertion was traced to a missing `__wcsnicmp`
stub and corrected in the standalone CRT compatibility layer.

The sections below retain the earlier handover as history. Their default-backend,
boot-blocker, and deployment guidance describes the older state.

## Historical handover — 2026-09-30

## Current state

An optional nxdk-audio APU backend implements Halo's DirectSound compatibility
layer with hardware PCM/Xbox ADPCM playback, stereo mixing, gain/pitch, packet
completion, flush, pause, and static-buffer play/loop/seek. The user confirmed
that dialogue, UI sounds, and weapon effects sound good after the repetition fix.
This is xemu validation; real hardware and reference audio comparisons remain pending.

The default remains `software`. The user agreed to promote APU audio after the
integrated build boots and passes validation; that condition is still pending.
3D positioning/attenuation, deferred updates, Doppler, HRTF, surround mix bins,
effects-image/I3DL2 reverb, and Bink audio remain unimplemented.

## Important files and fixes

- `compat/dsound_nxdk.c`, `compat/audio_mixer.*`, `compat/audio_backend.h`:
  DirectSound ABI, four-packet ring, game callbacks, software fallback.
- `crt/nxdk_audio_apu.c`: APU adapter; two hardware slots per voice, contiguous
  staging, worker polling, synchronous flush with stop-timeout ownership safety.
- `crt/nxaudio.patch`, `crt/nxaudio.cmake`, `tools/prepare_nxaudio.py`:
  build-local upstream patches. Completed streaming SSL descriptors must be
  cleared in hardware **before** unlocking buffers; otherwise persistent voices
  replay stale dialogue/UI/weapon fragments. Static buffers remain pinned until
  replacement/destruction; ADPCM descriptor limits and byte cursors are corrected.
- `tools/gen_legacy_image.py`: recognizes absolute symbols, preserving nxdk's
  `__tls_array = 4`. Generating a stub for it previously crashed the first CRT thread.
- `docs/audio-apu.md`: detailed implementation, validation history, and remaining work.

Upstream checkout: `/mnt/g/dev/nxdk-audio`, pinned to
`fc2deca2cc1e434805ac03ca7c2f500b3b028f36`. Keep it pristine; patches apply to a
build-local copy. DSP assembler: `build-audio-tools/bin/dsp56300-asm`, version 1.0.0.

## Boot blocker and build distinction

- `build-apu/default.xbe` / deployed `standalone_apu.xbe`: current networking + APU.
- `build-seed/default.xbe` / `standalone_seed.xbe`: current networking + software.
- `build-apu-isolated/default.xbe` / `standalone_apu_isolated.xbe`: diagnostic
  snapshot using the previous networking shim, sourced from `/tmp/halo-apu-isolated`.
  It is stale and does not represent the current integrated tree.

The isolated title previously ran with working audio, but now halts during b40
startup. Latest live inspection confirmed thread 48 stopped at
`0x00777BBF`, `_halt_and_catch_fire+0x1f`, using its matching map. Its last log
line was `obj_chasm1: this is not a valid hud_message name`; that warning alone
does not establish the halt's cause. The user suspects a missing implementation
in `/mnt/g/dev/halo` and is fixing it. Leave xemu untouched until they are ready.

Earlier integrated testing also recorded a null read in `XFindClose` during
network-game creation. Networking is being changed by another agent; recheck
the current tree rather than treating that older failure as current. Preserve
their edits. The xemu NV2A assertion during exception rendering may be secondary;
its cause has not been established.

## Resume

After the user's Halo fix, build the current integrated APU title:

```bash
./build.sh apu
python3 tools/test_audio_apu.py build-apu
python3 tools/test_audio.py build-software
```

Last completed checks: seven APU regression groups and eleven software groups
passed. These are linked x86 lifecycle tests, not hardware or fidelity proof;
rebuild the software artifact before claiming validation of newer sources.

Deploy to the authorized local xemu at `127.0.0.1:731`, including the matching
`cachebeta.map` alongside the XBE. Direct WSL XBDM uses `HALO_WINDOWS_REEXEC=1`.
Verify `xbeinfo running`, then read telemetry with
`python3 tools/audio_status.py build-apu/halo.map --host 127.0.0.1`.
Read ordinary RAM only: XBDM byte reads of AC97 MMIO previously crashed xemu.

Retest boot, dialogue, rapid-fire effects, music, pause/resume, and crowded scenes.
Then promote APU to the default if integrated validation succeeds. Continue
feature parity with listener/source 3D behavior, followed by DSP/effects work.
