# Experimental nxdk-audio backend

The `apu` backend moves PCM/Xbox ADPCM decoding, resampling, and stereo voice
mixing to the Xbox APU through [nxdk-audio](https://github.com/Ryzee119/nxdk-audio).
It retains Halo's DirectSound ABI, packet callbacks, and synchronous flush
behavior. The default remains `software` while this prototype is evaluated.

This milestone uses the driver's 192 2D voice slots. It does not yet implement
Halo's listener/source positioning, distance/cone attenuation, Doppler, deferred
3D updates, HRTF selection, surround mix bins, or I3DL2/DSP reverb. Halo's
`DownloadEffectsImage` data is not loaded into the upstream passthrough DSP.
Bink movie audio also remains unsupported. This is not DirectSound feature parity.

## Build

Keep the upstream checkout alongside the Halo checkouts:

```bash
git clone https://github.com/Ryzee119/nxdk-audio /mnt/g/dev/nxdk-audio
git -C /mnt/g/dev/nxdk-audio checkout fc2deca2cc1e434805ac03ca7c2f500b3b028f36
cargo install dsp56300-asm --version 1.0.0 --locked --root "$PWD/build-audio-tools"
./build.sh apu
```

The checkout must be at the pinned revision with the consumed files unmodified.
`NXDK_AUDIO_DIR` and `NXDK_AUDIO_ASSEMBLER` are CMake cache paths if the checkout
or assembler lives elsewhere. `tools/prepare_nxaudio.py` copies the driver into
the build directory, applies `crt/nxaudio.patch`, and assembles/embeds its DSP
program. It leaves the upstream checkout unchanged and does not download tools
during CMake builds. Clang 18 is supported without `#embed`.

Direct CMake selection:

```bash
cmake -S . -B build-apu -DCMAKE_TOOLCHAIN_FILE=toolchains/llvm.cmake \
  -DNXDK_DIR=/mnt/g/dev/nxdk -DHALO_ROOT=/mnt/g/dev/halo \
  -DHALO_STANDALONE_SEED_XBE=/mnt/g/dev/halo/halo-patched/cachebeta.xbe \
  -DHALO_STANDALONE_AUDIO=ON -DHALO_STANDALONE_AUDIO_BACKEND=apu
cmake --build build-apu -j8
```

`./build.sh apu --deploy` uploads `standalone_apu.xbe` by default. The `seed`
variant continues to upload `standalone_seed.xbe`, allowing comparison and fallback.
Both variants use seeded legacy data and remain bring-up builds.

## Ownership and completion

Halo retains a four-packet ring; two slots feed hardware at a time. Each packet
uses a reusable contiguous staging allocation so an ADPCM block cannot cross
discontiguous physical pages. Allocations occur on the submitting thread.
The driver DPC only wakes a worker. The worker retires hardware ownership and
queues waiting packets under the audio lock; game callbacks run from
`DirectSoundDoWork` after releasing that lock.

Retiring old owners and assigning free hardware slots occur at DPC level so
a completion arriving before the next Process call cannot lose its callback.
Flush waits for the driver's asynchronous stop/idle handshake before releasing
DMA ownership, destroying the physical voice, and dispatching packet aborts.
The next submission recreates the voice. A stop timeout reports failure and
retains the object/data instead of releasing memory the APU might still read.
An aborted hardware packet currently reports zero completed bytes; partial
hardware cursor accounting is not implemented.

If the idle DPC finishes after the synchronous stop timeout, the worker
finishes the flush once the driver reports `NX_STOPPED`. It retires both
hardware-owned and still-waiting packets, clears the active bit, and leaves
their abort callbacks for `DirectSoundDoWork`. A stopping stream does not
advertise readiness for more packets. Without this recovery, campaign revert
can wait indefinitely in `sound_stop_all` even though its hardware voices
have stopped. The linked regression reproduces a delayed idle DPC with four
packets and two hardware slots and checks safe retirement and channel reuse.

Local upstream patches clear completed streaming SSL descriptors before
unlocking their buffers (otherwise persistent voices can replay stale data),
keep static buffer pages pinned until replacement or destruction,
cap streaming ADPCM SSL entries to the hardware sample-count
limit, reject truncated SSL tables, return playback offsets in bytes, embed
the DSP text on Clang 18, and avoid a `fabsf` dependency that pulls a duplicate
Halo/nxdk CRT `fabs` symbol into the link. Source license headers are preserved:
upstream code is MIT, and the HRTF table carries CC BY 4.0 attribution in
`audio_hrtf.h` (HRTF is not enabled in this milestone).

The local patch also protects voice-register PIO transactions at DPC level.
The completion DPC selects a voice to retire its SSL descriptor, so it must not
interrupt another transaction between `SET_CURRENT_VOICE` and its final write.
Otherwise gain, pitch, format, or buffer settings can reach a different sound.
This includes voice initialization, setters, and static-buffer submission;
floating-point gain/pitch calculations remain outside the protected section.
The linked regression injects the actual completion DPC during thirteen driver
operations, checks that their writes retain the intended voice, and verifies
that deferred completion runs after IRQL is restored, including failed static
submission. It reproduces a redirected gain write against the previous build.
This establishes the race and its correction; sustained environmental sounds
still need an audible xemu retest to confirm the reported distortion is resolved.

The standalone shim generator also recognizes absolute archive/object symbols.
Without this, its fallback replaces nxdk's `__tls_array = 4` with a function
stub and crashes the first CRT thread before the game starts. APU linked tests
check that the TLS symbol retains its required value.

## Validation and next steps

```bash
python3 tools/test_audio_apu.py build-apu
python3 tools/test_audio.py build-seed
python3 tools/audio_status.py build-apu/halo.map --host 127.0.0.1
```

The linked x86 tests simulate driver events and kernel boundaries. They check
four-packet/two-slot ownership, coalesced completions, completion before Process,
thread-only game callbacks, pause/gain/pitch, ADPCM staging, stop timeout safety,
32 channel reuse cycles using Halo's actual callback/cache-reference functions,
static buffer play/loop/seek/stop/replay, and descriptor retirement/static-page
pinning with the actual compiled driver DPC. The retirement assertion fails
against the pre-fix driver. They do not emulate APU playback or establish audio fidelity.
The software regression checks remain separate.

On 2026-09-30 the integrated title hit a transport initialization assertion
before reaching audio. Networking was being changed independently, so the APU
runtime check used a temporary source copy with the repository's previous
networking shim; no networking worktree files were changed. The resulting
`build-apu-isolated/default.xbe` was deployed as `standalone_apu_isolated.xbe`.
After the TLS fix, xemu ran gameplay and reported 2,492 hardware buffers queued,
2,406 completed, and zero driver errors, stop timeouts, or rejected packets in
the first snapshot. This establishes startup and streaming progress in that
isolated configuration, not success of the integrated networking build or
reference audio fidelity. The normal `apu` variant uses the current worktree's
networking implementation.

The user confirmed audible output but reported repeating fragments in dialogue,
UI sounds, and weapon effects. Inspection found that completed upstream SSL
descriptors remained active in hardware after their software pointers were
cleared. The local patch now invalidates each completed descriptor before
unlocking its pages. After deployment of that correction, the user confirmed
that audio sounded good. This confirms the reported repetition was resolved
in the tested xemu session; real hardware and reference-level comparisons
remain pending.

A later attempt to validate the combined networking/APU title compiled and
passed all seven APU regression groups, but the user reported a boot failure.
The captured Halo log included a null read in `XFindClose` during network-game
creation, followed by exception handling/rendering failures. The known audio
test title was restored. Promotion of APU audio to the default is deferred
until the integrated title passes its boot check; the networking worktree
files remain owned by the separate networking effort. The software fallback
was rebuilt with the current networking sources and passed all eleven audio
regression groups.

Use the map matching the deployed build. APU telemetry includes queued/completed
buffers, worker polls, stop timeouts, and driver errors. Software PCM peak/mixing
counters are inactive with the APU backend. Read ordinary title RAM only;
XBDM byte reads of AC97 registers previously crashed xemu.

Before changing the default, compare rapid-fire effects, music, sustained streams,
pause/resume, and loud multi-voice scenes on xemu and real hardware. Measure
levels against the reference title: upstream voice attenuation and AC97 volume
settings differ from the software mixer, so copied volume values alone do not
establish parity. Then implement listener/source/deferred 3D behavior, HRTF and
Doppler, followed by the effects-image/mix-bin/I3DL2 path. Each milestone needs
reference comparisons; successful packet tests are only lifecycle evidence.
