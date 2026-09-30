# Seeded standalone issue tracker

Track observed problems in the seeded nxdk build and fix them one at a time.
Missing Halo implementations, nxdk compatibility gaps, and defects in existing
lifts need different investigations. An observed symptom is not enough to choose
which category applies.

## Build and evidence

- Recorded on 2026-09-30 against the seeded build uploaded to
  `127.0.0.1`, `E:\GAMES\halo-patched\standalone_seed.xbe`.
- Halo source commit: `af6bbf894` in `/mnt/g/dev/halo`.
- `FUN_0016c5a0` was imported from `halo-clean-main`; the seeded build now
  links its model-rendering body instead of a stub. Its `ported: false`
  setting remains unchanged for normal original-XBE patch builds.
- Live code at `0x16c5a0`, `0x41180`, `0xdb950`, `0x16dee0`, and
  `0x16e160` matched the local `build-seed/default.xbe` at the time sampled.
- The missing-function recorder reported **61 distinct symbols reached**.
  This counts first hits, not calls or all missing functions in the project.
- Later XBDM reads received `ConnectionRefusedError`. Further live checks
  need the endpoint available again; the reason it became unavailable is unknown.
- The networking log below is a separate user report. Its exact executable
  and reproduction steps have not yet been verified against this build.

Use stable issue IDs below when recording fixes and new observations. Update an
issue to **fixed** only after its acceptance check passes on the running build.
See [the build guide](standalone-build.md) for build and deployment commands.

## Open issues

| ID | Priority | Symptom | Evidence status | Next step |
| --- | --- | --- | --- | --- |
| NET-001 | P0 | Server connection creation fails, followed by a networking halt | Original socket placeholder replaced with nxdk/lwIP; host adapter tests pass | Verify connection creation and failure cleanup on the emulator/device |
| AI-001 | P1 | Enemies stand still and never attack | Missing perception functions reached live; AI enabled | Implement and validate the perception frontier, then retest enemy reactions |
| HUD-001 | P2 | Radar sweep does not animate | Radar history advances; renderer-ready flag sampled as zero | Trace radar bitmap/texture readiness and the animated rendering pass |
| HUD-002 | P2 | Radar blips appear but look incorrect | User observation; cause unconfirmed | Check blip placement, size, fading, and compositing after HUD-001 |
| VERIFY-001 | P2 | Imported model renderer lacks complete behavioral verification | Strict lift gate fails on inherited FPU warnings | Resolve warnings with binary evidence and obtain meaningful rendering coverage |

P0 blocks the affected game flow, P1 blocks gameplay, and P2 affects rendering
or validation. These priorities can change after reproduction.

## NET-001 Server connection failure and halt

**Observed:** the user supplied this log sequence, with timestamps preserved:

```text
09.30.26 04:28:40  winsock error #2: <unknown error>
09.30.26 04:28:40  failed to create the server connection
09.30.26 04:28:40  notified all clients that we are going down
09.30.26 04:28:40  Printing stuff for Mat's edification
09.30.26 04:28:40  00835AD0 __halo_legacy_1d0589 + 0002 : stubs.obj
09.30.26 04:28:40  00835B23 __halo_legacy_1d0a5c + 000D : stubs.obj
09.30.26 04:28:40  EXCEPTION halt in c:\halo\SOURCE\networking\network_connection.c,#816: connection [rev=89ae958]
```

This is an excerpt of the supplied stack dump. Unknown frames and nearest-symbol
names need the map/executable corresponding to that run before interpretation.
The `rev` banner alone does not identify which seeded executable was running.

**Confirmed in source:** `network_connection_active` asserts that its
`connection` argument is nonzero at original source line `0x330` (816).
Location: `/mnt/g/dev/halo/src/halo/networking/network_connection.c:197`.

**Original confirmed cause in standalone source:** `compat/xnet/xnet.c` supplied
successful startup placeholders while `xnet_socket` always returned `-1`.
Other required Winsock calls used generated stubs. Halo's
`network_connection_new` already has an implementation in main and is marked
`ported: true`.

**Implementation now available:** the adapter uses a locally rebuilt nxdk/lwIP
stack with 64 sockets. Required TCP/UDP calls, nonblocking mode, Winsock
address/fd-set layouts and error translation, startup/cleanup, and opaque LAN
session registration have implementations. Sanitized host integration tests
pass; the seeded Xbox build links these bodies. See
[standalone-networking.md](standalone-networking.md) for scope and validation.
Stock encrypted XNet framing remains unsupported. Runtime hosting/joining and
the original reported halt have not yet been verified with this build.

**Leading cause in the old build:** missing standalone XNet/Winsock support
prevented socket creation. Verify the exact reported run before treating this
as the complete cause of its halt.

**Inferred:** failed connection creation may leave a null connection that is
subsequently passed to the active-connection predicate. The first transport
failure and the meaning of error `#2` remain unresolved. Do not assume it is a
standard Winsock error or that the legacy stubs shown in the stack caused it.

Next steps:

1. Record the exact executable, map, and menu actions that reproduce the halt.
2. Capture the missing-function list and transport return/error values for that
   run. Identify the first operation that fails.
3. Repair the missing transport implementation or incorrect caller behavior
   supported by that evidence, preserving cleanup on creation failure.

**Acceptance:** the same flow creates the required connection or handles a
creation failure without a halt. Retry, leave-game, and ordinary single-player
map loading still work.

## AI-001 Enemies remain idle

**Observed:** enemies stand still and never attack the player.

**Confirmed live:** AI globals were `01 01 01 00 ...`, and the AI pause/random
movement flags at `0x5abaa0` were both zero. `ai_update`, `actor_update`, and
`actor_perception_update` have implementations. The live recorder reached these
missing functions, whose standalone stubs return without performing their work:

| Function | Original VA | Role to investigate |
| --- | --- | --- |
| `actor_perception_refresh_test_object` | `0x342a0` | Process candidate objects into enemy/friend perception lists |
| `actor_visibility_at_point` | `0x314f0` | Compute visibility; the stub returns zero |
| `actor_audibility_at_point` | `0x31850` | Compute audibility; the stub returns zero |
| `actor_perception_refresh_danger_zone` | `0x32380` | Refresh danger awareness |
| `actor_action_determine_pursuit_availability` | `0x1cda0` | Determine available pursuit behavior |
| `actor_action_handle_danger_avoidance` | `0x20990` | Handle danger avoidance |

The existing `actor_perception_refresh` calls the object-test function while
iterating nearby objects. `prop_status_refresh` consumes visibility and hearing
results. Source: `/mnt/g/dev/halo/src/halo/ai/actor_perception.c`.

**Inferred:** missing object processing plus zero visibility/hearing results
can explain actors ignoring the player. These are confirmed missing dependencies,
but implementing them may expose additional AI or movement gaps.

Next steps:

1. Lift the object-test, visibility, and audibility functions from the original
   binary, checking prototypes, call arguments, and register ABI.
2. Test one function at a time, then the group, on the same encounter.
3. If enemies detect the player but still remain idle, follow decision,
   pursuit, pathfinding, and unit-control dependencies reached in that test.

**Acceptance:** enemies detect and engage the player, move when their encounter
requires it, and react to appropriate sound/damage stimuli without new halts.

## HUD-001 Radar sweep remains static

**Observed:** the sweep does not animate; some blips are visible.

**Confirmed live:** radar history state at `motion_sensor_globals + 0x15a0`
changed from tick 18 / history slot 5 to tick 51 / history slot 8 across two
samples. The feature byte at `0x3256db` was one, while the renderer-ready byte
at `0x47e007` was zero in both samples.

**Confirmed in source:** the radar update, history drawing, preparation, blip
drawing, and composition functions all have compiled bodies. In particular:

- `FUN_0016dee0` clears the ready byte and only sets it after the two blip
  bitmap texture lookups succeed.
- `_rasterizer_hud_motion_sensor_blip_draw` and `FUN_0016e2e0` gate their drawing
  on that ready byte; the latter implements the swept bitmap pass.

Source: `/mnt/g/dev/halo/src/halo/rasterizer/rasterizer.c` and
`/mnt/g/dev/halo/src/halo/interface/event_manager.c`.

**Inferred:** readiness or texture/render-target setup can prevent the animated
pass despite advancing game time. The zero flag does not yet establish which
texture lookup fails, whether preparation runs each frame, or whether a missing
compatibility operation is involved. A bitmap-record probe could not complete
after XBDM became unavailable.

**Acceptance:** the sweep animates continuously during gameplay and survives a
map reload without disrupting the rest of the HUD.

## HUD-002 Incorrect radar blips

**Observed:** the user reports visible but incorrect blips. The exact visual
defect has not yet been captured or isolated.

Next steps: capture the radar while turning and moving past a known unit; compare
position, facing rotation, size, color, fading, and clipping against the original
build. Check blip preparation and final composition together. Do not assume the
visible marks came from the intended blip pass while its ready flag is zero.

**Acceptance:** nearby units appear at the correct relative positions; turning
rotates those positions correctly, and history trails fade without stray marks.

## VERIFY-001 Model renderer verification

**Completed:** the missing `FUN_0016c5a0` body was imported, committed, built,
and uploaded. Normal build, seeded build, hazard/type/ABI checks, and commit
hooks passed. Its old missing-function entry was absent from the new live list.

**Still open:** VC71 reports 90.0% mnemonic match and 82.1% operand-normalized
match. The strict lift pipeline fails on five inherited FPU warnings. Equivalence
reached only 1.9% coverage, which is inconclusive. These percentages are not
raw-byte accuracy or proof of rendering equivalence.

Evidence: `/mnt/g/dev/halo/artifacts/lift_runs/20260930-120236/summary.json`
and `/mnt/g/dev/halo/artifacts/score_context/FUN_0016c5a0.json`.

**Acceptance:** resolve or substantiate the FPU differences against the original
binary and confirm model rendering through meaningful runtime tests.

## Additional missing functions to revisit

The same live run reached the following Halo stubs. These are follow-up leads,
not independently confirmed user-visible issues:

- `actor_look_decode_direction`, `path_state_estimated_distance`,
  `ai_consider_major_upgrade`, `firing_position_pre_evaluate`,
  `firing_position_store_evaluation_debug`, `ai_communication_actor_talk_weight`,
  `signed_angle_between_vectors2d`.
- `biped_limp_noodle_valid_joint_rotation`, `inverse_kinematics_adjust_matrices`.
- `local_player_aim_assist`, `player_aim_projectile`,
  `player_effect_update_camera_impulse`, `player_effect_update_camera_shake`,
  `scripted_player_effect_start`.
- Unnamed functions including `FUN_00012ad0`, `FUN_0005ef80`, `FUN_000639e0`,
  `FUN_00136bc0`, `FUN_00138f70`, `FUN_00149ce0`, `FUN_001621c0`,
  `FUN_001631d0`, `FUN_00170c90`, `FUN_0017cd00`, `FUN_001be7b0`, and an
  unknown function at `0x14670`.

Refresh the recorder for each reproduction rather than treating this list as
complete. The run also reached DirectSound stubs; track specific audio symptoms
as separate issues when reproduced.

## Fix record

For each change, record the issue ID, source commit, build variant, deployed
artifact, reproduction steps, checks run, and observed result. Distinguish
implementation/build success from runtime acceptance. Refresh the live
missing-function list after each accepted fix.
