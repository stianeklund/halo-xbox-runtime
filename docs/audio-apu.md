# Experimental nxdk-audio backend

The `apu` backend moves PCM/Xbox ADPCM decoding, resampling, and stereo voice
mixing to the Xbox APU through [nxdk-audio](https://github.com/Ryzee119/nxdk-audio).
It retains Halo's DirectSound ABI, packet callbacks, and synchronous flush
behavior. APU is the default backend; software remains an explicit fallback.

This milestone uses the driver's 192 2D voice slots. Both audible backends
approximate DirectSound 3D in `compat/audio/dsound_nxdk.c`: listener/source
positions, inverse distance rolloff clamped at max distance, cones, deferred
updates committed once per frame, the I3DL2 direct level with occlusion and
obstruction at their low-frequency ratio, front mix-bin volumes, and a stereo
pan with a head-shadow floor. It does not implement Doppler, HRTF filtering,
high-frequency occlusion filtering, surround mix bins, or I3DL2/DSP reverb. Halo's
`DownloadEffectsImage` data is not loaded into the upstream passthrough DSP.
Bink movie audio also remains unsupported. This is not DirectSound feature parity.

## Build

Keep the upstream checkout alongside the Halo checkouts:

```bash
git clone https://github.com/Ryzee119/nxdk-audio /mnt/g/dev/nxdk-audio
git -C /mnt/g/dev/nxdk-audio checkout fc2deca2cc1e434805ac03ca7c2f500b3b028f36
cargo install dsp56300-asm --version 1.0.0 --locked --root "$PWD/build-audio-tools"
./build.sh
```

The checkout must be at the pinned revision with the consumed files unmodified.
`NXDK_AUDIO_DIR` and `NXDK_AUDIO_ASSEMBLER` are CMake cache paths if the checkout
or assembler lives elsewhere. `tools/prepare_nxaudio.py` copies the driver into
the build directory, applies `crt/nxaudio.patch`, and assembles/embeds its DSP
program. It leaves the upstream checkout unchanged and does not download tools
during CMake builds. Clang 18 is supported without `#embed`.

Direct CMake selection:

```bash
cmake -S . -B build-seed -DCMAKE_TOOLCHAIN_FILE=toolchains/llvm.cmake \
  -DNXDK_DIR=/mnt/g/dev/nxdk -DHALO_ROOT=/mnt/g/dev/halo \
  -DHALO_STANDALONE_SEED_XBE=/mnt/g/dev/halo/halo-patched/cachebeta.xbe \
  -DHALO_STANDALONE_AUDIO=ON -DHALO_STANDALONE_AUDIO_BACKEND=apu
cmake --build build-seed -j8
```

`./build.sh --deploy` uploads `standalone_seed.xbe` by default. To build the
software fallback, use `HALO_STANDALONE_AUDIO_BACKEND=software ./build.sh`.
Both use seeded legacy data and remain bring-up builds.

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
limit, reject truncated SSL tables, return playback offsets in bytes, count
static-buffer end and current offsets in samples (64 per ADPCM block, as SSL
lengths and xemu's voice processor do), embed
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

Streaming submissions now preserve hardware A/B FIFO order across empty
intervals. The driver records the next SSL slot in an existing padding byte,
so the pinned voice structure's size and existing field offsets stay unchanged.
The adapter waits if that slot is still owned instead of aborting a packet or
filling the other free slot. `VOICE_LOCK` stays held through descriptor/notifier
publication and ownership assignment, preventing an empty-slot notification
from racing a refill. The linked PCM/ADPCM regression covers repeated odd-length
queue drains, a busy next slot, malformed descriptor rollback, and delayed
completion; the previous driver publishes descriptors after unlocking and
refills A when the APU is waiting for B. Audible dialogue validation remains
separate from these ordering and ownership checks.

The standalone shim generator also recognizes absolute archive/object symbols.
Without this, its fallback replaces nxdk's `__tls_array = 4` with a function
stub and crashes the first CRT thread before the game starts. APU linked tests
check that the TLS symbol retains its required value.

## Validation and next steps

```bash
python3 tools/test_audio_apu.py build-seed
python3 tools/test_audio.py build-software
python3 tools/audio_status.py build-seed/halo.map --host 127.0.0.1
```

The linked x86 tests simulate driver events and kernel boundaries. They check
four-packet/two-slot ownership, coalesced completions, completion before Process,
thread-only game callbacks, pause/gain/absolute XDK pitch, ADPCM staging and
sample-unit loop regions, stop timeout safety,
32 channel reuse cycles using Halo's actual callback/cache-reference functions,
static buffer play/loop/seek/stop/replay, and descriptor retirement/static-page
pinning with the actual compiled driver DPC. The retirement assertion fails
against the pre-fix driver. They do not emulate APU playback or establish audio fidelity.
The software regression checks remain separate.

The user traced audio stutter in xemu to its enabled DSP JIT. Disabling the
DSP JIT removed the stutter in that session. This observation does not establish
an audio-buffering defect in the title; real Xbox playback remains unverified.

Gain staging now follows cachebeta's DirectSound voice defaults. Its original
voice-settings constructor at `0x2054a8` sets 600 mB (6 dB) of headroom for
ordinary 2D voices and zero for 3D voices; `SetVolume` subtracts that headroom
before the hardware-volume calculation at `0x208000`. The adapter includes this
once in the per-voice gain, including initial creation. The local driver patch
removes upstream's implicit 6 dB floor from the gain converter, which previously
applied it twice (master plus channel). Software output clips the summed voice
samples without another global attenuation stage. Relative to the previous APU
build, this raises ordinary 2D output by 6 dB and 3D output by 12 dB at the same
requested volume. Explicit Halo mix-bin levels and spatial attenuation still
apply, including its roughly -6 dB per-side routing for mono 2D sounds.

A linked regression runs the actual driver gain setter and checks front-bin
PIO attenuation at unity, -6 dB requested volume, and mute for both 2D and 3D
voices. The previous linked build fails its default 2D check with 768 units
(12 dB), where the reference default is 384 units (6 dB). This establishes the
gain-staging correction; measured output parity with cachebeta still needs a
comparison of the same audio segment and capture settings. AC97 PCM output
remains at the upstream `0x0808` setting, and the passthrough DSP still lacks
Halo's effects image.

On 2026-09-30 the integrated title hit a transport initialization assertion
before reaching audio. Networking was being changed independently, so the APU
runtime check used a temporary source copy with the repository's previous
networking shim; no networking worktree files were changed. The resulting
`build-apu-isolated/default.xbe` was deployed as `standalone_apu_isolated.xbe`.
After the TLS fix, xemu ran gameplay and reported 2,492 hardware buffers queued,
2,406 completed, and zero driver errors, stop timeouts, or rejected packets in
the first snapshot. This establishes startup and streaming progress in that
isolated configuration, not success of the integrated networking build or
reference audio fidelity. The normal build uses the current worktree's
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
test title was restored, and default promotion was deferred at that point.
APU is now the requested default after subsequent integrated testing; audio-level
comparisons and feature parity remain pending. The software fallback
was rebuilt with the current networking sources and passed all eleven audio
regression groups.

Use the map matching the deployed build. APU telemetry includes queued/completed
buffers, worker polls, stop timeouts, and driver errors. Software PCM peak/mixing
counters are inactive with the APU backend. Read ordinary title RAM only;
XBDM byte reads of AC97 registers previously crashed xemu.

Continue comparing rapid-fire effects, music, sustained streams,
pause/resume, and loud multi-voice scenes on xemu and real hardware. Measure
levels against the reference title: upstream voice attenuation and AC97 volume
settings differ from the software mixer, so copied volume values alone do not
establish parity. Then compare the approximate 3D levels against the reference
title and add HRTF and Doppler, followed by the effects-image/I3DL2 reverb path. Each milestone needs
reference comparisons; successful packet tests are only lifecycle evidence.
