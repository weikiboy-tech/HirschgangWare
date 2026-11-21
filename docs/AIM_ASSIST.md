# Aim Assist — direct tracking and guided setup

Optional native aim assistance on branch `feature/aim-assist`. Default: disabled,
unbound, dry run ON. Uses the existing remote-key snapshot, raw camera, player
snapshots and MAKCU serial owner. No additional DMA connection, game-memory
writes, WebRadar API or dependency.

## Findings from the reported live session

On 2026-11-21, `[assist]` recorded target 2 at 11:08:31–34 (15–78 px from the
centre), blocked by zero counts/radian calibration. Other samples had one player
rejected by the combined area/range filter or absent name. Therefore neither
“DMA supplies no players” nor “only a cosmetic issue” describes the incident.

The first implementation lacked a usable calibration workflow. Its production
adapter discarded candidates before the tested engine could report why, while
its synthetic engine tests supplied already-approved candidates. The former
`Record current lens` button stored FOV, not mouse sensitivity. The one-way
`Use direct tracking` action disappeared after use. These controls are replaced.

An earlier display bug erased an acquired target on blocked movement paths;
that fix alone was insufficient. The tests now exercise the production snapshot
adapter and ImGui panel as well as the mathematical engine.

## Current data and mode

The live reader provides human world roots, real bone positions/timestamps,
stable per-session IDs, player names/friend membership and the raw camera/lens.
There is no verified muzzle origin, ammo speed, drag/gravity model or inherited
launch velocity. Production therefore explicitly uses **Direct tracking**.
The legacy `assist_prediction` INI value is preserved for compatibility but
cannot block this explicit mode. Prediction is shown as unavailable; its pure
constant-gravity solver remains covered by offline tests only.

A screen/frustum check is not a line-of-sight test. Nothing here proves absence
of an obstruction or predicts a hit. The mouse response is measured, never
inferred from an ammunition name, FOV ratio or guessed sensitivity.

## UI and first test

Open **AIM ASSIST** below **TRIGGERBOT**. Four separate summaries show Input, Target,
Calibration and Output. Selection/preview runs even with the hold key released.

1. Enable the feature and bind the Main-PC hold key. Escape cancels binding.
   Leave **Dry run** ON. Existing trigger/Clutch key overlaps are displayed.
2. Put a named non-friend human inside the search circle, in front of the camera
   and within configured range. Expect a stable target ID, range and pixel error.
3. **Gefuehrt einrichten → Messung starten... → Jetzt messen** explicitly starts
   the attended setup. Stand still, look approximately level, release all Main-PC
   keys/buttons and do not move the physical mouse during the short measurement.
4. The program measures both axes, stores the mapping only after complete success
   and leaves dry run ON. No manual counts/radian entry or verification checkbox
   is required. Failed/cancelled attempts preserve the previous saved calibration.
5. Inspect dry-run proposals first. Only then switch dry run off yourself to test
   following. The same original trigger conditions continue to govern clicks;
   setup/following never issue click commands of their own. Choose a distinct
   hold key if the existing click feature should not be activated by this test.

Setup and Advanced controls have explicit open/close buttons. Closing setup while
measuring requests cancellation. The setup can also be stopped with its Cancel
button or Escape on either PC; it can be reopened afterward. Reset clears only
calibration, selects dry run and requires fresh guided setup. A standalone
“record lens” action and self-asserted “output verified” switch are removed.

A device-query channel occupied by legacy key polling cannot safely attribute
bare scalar responses. It is blocked with an explanation rather than changing
existing polling/bindings. MAKCU must already be connected on the existing serial
connection; UDP mode is not supported for live aim setup.

### Installed-device finding, 2026-11-21

The attended UI test stopped before its first pulse. A read-only control experiment
on the same live serial owner then found no usable response to `km.moving()`, a
positive `>>> km.MAKCU` response to `km.version()` at 12:06:17, and a positive
`>>> km.move(0,0)` echo at 12:07:32. This demonstrates a working command channel;
it does not establish `move_now` or pending-status support. The earlier hard
requirement for that query was incompatible with this observed interface.

The adapter now first probes pending status, then uses the version query as a
positive channel control if pending status times out/is rejected. Only that
positive response enables a **legacy camera-feedback trial**, using `km.move`.
Identity/echo alone never verifies actual movement: the first one-count trial
must produce a fresh, measured camera response or setup stops with no further
pulses. The original click/input logic is not replaced.

## Shared snapshot policy

`prepare()` consumes the actual entity field layout from `DzSnap::E`. It uses
one shared `inspect()`/projection policy, also used by the engine, and bounded
storage (128 candidates; nearest identified candidates and a valid lock retained).
Source rejection counts include all human entities even when candidate storage
fills. No raw pointers or names are substituted for stable session IDs.

Rejection states are distinct: missing stable ID, friend, missing bones, stale
root, stale bones, invalid selected bone, offscreen, outside range, outside circle.
A geometrically valid unnamed player is shown as **identity pending**, with no
motion until a current name establishes friend membership. Current friends are
excluded immediately; an old name is never cached onto a new/reused ID.

Defaults remain radius 120 px and range 300 m; a target at 388 m is outside that
range. The optional Advanced **Show search circle** uses the same viewport centre
and radius in both GPU and GDI drawing. It is suppressed with other extras during
Clutch Mode. Selected points use existing head/spine indexes, never invented bones.

Camera data must be younger than 35 ms, roots/bones younger than 75 ms. The
original display projection has its own stricter gate; rejecting one render
frame no longer cancels setup or resets otherwise fresh control data. No commands
are issued on stale camera samples; a source loss of 250 ms, viewport loss or
world change cancels setup. Invalid
snapshots, target death/removal, session changes, jumps and release reset motion.
A valid identity/error remains displayed if calibration or output blocks motion.
One `[assist]` line per second while the hold key is down records ID, range,
error, each rejection count, calibration and dry-run state.

## Guided calibration and device ownership

`aim_calibration.h` contains a deterministic state machine with an injected clock,
camera and device feedback; it has no I/O. The existing UI/render owner performs
proposed commands after the original click update. No thread, queue, second serial
reader, click masks, silent move-click commands or physical-input locks are added.

After the explicit confirmation, fresh camera/session/input and serial state are
required. Firmware with pending-status support must return idle before a pulse;
legacy mode instead requires the positive channel control and observed camera
completion. Sixteen single-axis pulses are used: +1, -1, +4, -4, +8, -8,
+16, -16 counts per axis. The actual budget is 58 absolute counts per axis,
below the 128-count cap; at most one pulse awaits completion. No response causes
an abort, not success or increasing blind retries.

The state machine requires 150 ms stable camera before a pulse, device-idle
confirmation within 50 ms afterward on supported firmware, a newer camera sample, at least 100 ms after
the pulse and 60 ms of stable response. It rejects unmeasurable response, more
than 10% inconsistent sensitivity, cross-axis response over 10% (5 µrad noise
floor), camera displacement over 5 cm, angular departure over 3 degrees, zoom
change, capture/held keys/buttons, stale data and device/session changes. Each
response has a 600 ms deadline and the full run a 20 s deadline. Near-vertical
view and leaning are rejected at entry. Legacy trials require an actual camera
response within 200 ms; a measured settling interval above 100 ms rejects setup.
The measured last-change interval plus an 8 ms observation margin, minimum 16 ms,
sets the minimum live command spacing. A held ADS button is incompatible with
this initial stationary calibration workflow; normal view is the first target.

Measured signed X/Y counts per radian and the current lens are committed together
only on Complete. Runtime readiness is scoped to the current connection. After
restart or reconnect, a previously measured profile can be restored at matching
zoom once the binding has been released and the device answers a fresh read-only
check. The pending-status path requires an idle reply. The legacy path requires
the expected MAKCU identity reply after checking for pending-status support.
Neither reply creates a calibration. Reusing the saved mapping assumes the same
hardware and in-game sensitivity. Remeasure after changing either. Zoom mismatch
pauses output.

`aim_motion_protocol.h` parses only the current pending query's documented 0/1,
explicit tagged response, or the expected identity control. It ignores prompts
and unrelated echoes. Matched motion replies are consumed so a scalar `1` cannot
be mistaken for a legacy keyboard press. Outside a pending motion query, the
original input parser remains unchanged. There is never more than one motion query
outstanding. Live movement is skipped while a query/movement or click is pending.
No application movement backlog is replayed. A sent HID report cannot be unsent;
observed camera/device timing in this setup is not proof for every firmware/load.

Legacy following permits one command per measured settling interval, at most
16 counts per axis, with current camera/root/bone data and the physical hold key
still valid. The strict isolated-response check remains in calibration only.
Normal gameplay mixes physical mouse input, sway, lean and recoil with injected
motion, so equality to a single command's expected camera delta is not a valid
gameplay acknowledgement. In particular, a one-count command must not destroy a
valid calibration merely because the combined view changed differently.

`OutputCycle` implements measured pacing without storing unsent commands. It
requires a fresh post-send camera sample and the measured interval before the next
command. A 250 ms update gap, invalid camera, session or lens change pauses output
without erasing the connection's established calibration verification. No new
commands are emitted while paused. After the actual Main-PC hold key is released,
two fresh samples at least 16 ms apart, the calibrated lens, valid input and a
positive read-only serial control are required to re-arm. Holding the key or an
initial-down suppression state cannot trigger this recovery. ERR/write/control
timeouts are actual transport failures and still invalidate device verification.

This pacing is **not a hardware-queue-empty guarantee**; the installed interface
does not expose that state. An already transmitted report cannot be recalled.
Physical mouse input is never locked. The controller does not advance its output clock
while the actuator is busy. A confirmed actuator cycle with continuously fresh
data can integrate up to 100 ms; an ordinary unconfirmed gap still resets above
50 ms, and a gap above 100 ms never catches up. Legacy commands are capped at the
16 counts/axis actually exercised by calibration, dropping unsent integer excess
instead of keeping a command backlog. Fractional counts are preserved.

Sources: [official MAKCU API](https://makcu.com/en/api/) documents version-dependent
`move_now` and `moving` behavior. No firmware/settings update is performed.
The optional [ballistic-solver](https://github.com/ujinf74/ballistic-solver) was
reviewed (MIT; Z-up RK4/quadratic-drag assumptions) and not copied: neither its
license nor solver proves compatibility with DayZ weapon physics (project Y-up).

## Persistence and files

Existing `assist_*` and unrelated INI values are retained; `assist_show_area` is
added (default 0), and `assist_response_ms` stores measured command spacing.
Enabling dry run never changes original click settings. Normal
ESP, friend colors, Clutch, radar and WebRadar controls are not repurposed.

Core: `src/aim_assist.h`, `src/aim_calibration.h`, `src/aim_motion_protocol.h`.
Integration: `src/esp.cpp`, `src/esp_launcher.h/.cpp`, `src/radar.h/.cpp`,
`src/trigger.h/.cpp`, `HirschgangWare.vcxproj`.
Tests: `tests/aim_assist.cpp`, `tests/aim_assist_ui.cpp`, their build scripts,
`tests/aim_assist_preservation.mjs` and extended `tests/dayz_camera.cpp`.
`tests/aim_protocol_input.cpp` and its build script exercise the production text
receiver with no device handle, including pending replies versus legacy keys.
No WebRadar assets or wire format change.

## Validation and attended acceptance

```bat
tests\build_aim_assist_test.bat
tests\build_aim_assist_ui_test.bat
tests\build_aim_protocol_input_test.bat
node tests/aim_assist_preservation.mjs
tests\build_dayz_camera_test.bat
build.bat x64
node tests/webmap.mjs
```

The unit suite includes synthetic physics/controller cases, 60/125/240 Hz
closed-loop following, source-shaped snapshot sequences, unnamed-to-friend
transitions, range/screen/circle boundaries, invalid/stale bones, capacity and
session cleanup, signed/reversed calibration, no/delayed response, nonlinearity,
cancellation and disconnect. The protocol suite verifies query attribution,
echo rejection, timeouts, unsolicited replies and reconnect invalidation.
The headless ImGui suite executes the production panel with synthetic UI events;
it never opens a device/DMA session or produces physical mouse input.

The preservation check strips only the additive motion adapter/state and exact
receive/reset hooks, then compares the remaining trigger source to baseline
9ced35a (SHA-256 ff4ff22fdbc36cdfb5c807fcd9a8235c7ba22c228a281ce38d69894e9c042a1b).
The loopback suite checks the unchanged click state machine and distinct motion
packets using localhost UDP only. Restricted networking can make this test time
out; it passes when executed outside that restriction.

Hardware acceptance remains mandatory: confirm acquisition while holding and
releasing E, correct friend/range rejection, successful attended guided setup,
then consciously enable live output. With a stationary target initially inside
the circle, expect <=3 px error within 2 s without sustained oscillation; releasing
E must stop further commands immediately. Repeat with moving targets and manual
camera input; test cancellation, zoom change and reconnect. Record measured error,
response time and device commands rather than declaring success from appearance.
No successful build/unit/loopback/UI test is claimed as a live DayZ movement test.

### Results for this revision — 2026-11-21

PASS: source-shaped snapshot/engine/controller/calibration/protocol/INI unit suite;
production ImGui panel headless interaction suite; exact trigger preservation
check; native full MSVC x64 build (separate test output); localhost UDP click and
motion suite (outside restricted networking); webmap.mjs; Clutch rebuilt against
the current trigger source; unchanged roster and DMA-liveness executables.

The Clutch test now waits for actual elapsed GetTickCount64 ticks instead of
assuming Sleep(9) advances the production sample clock. This changes test timing,
not input behavior. The UDP test gained packet ordinals for failures; its sandbox
timeout was resolved by running the same binary outside the network restriction.
The two existing trigger warnings (C4310 and inet_addr deprecation) remain.
Idle query responses are cached for 30 ms while no movement is submitted; normal
following services queries only while active, rather than polling an absent target.

Attended hardware results after the installed-device compatibility correction:

- The operator confirmed a death/respawn screen during the first one-count trial;
  it had no camera response and correctly sent no further pulse. The test resumed
  only after the operator confirmed a living character and released input.
- Short projection misses repeatedly cancelled setup after partial progress.
  The control/setup lifecycle is now independent of display-only misses while
  retaining the 35 ms command-data gate and 250 ms sustained-loss cancellation.
- At 12:35:25, all 16 physical pulse responses passed consistency checks. Saved
  X = 1097.137177 and Y = 1097.140260 counts/radian, lens 1.186824 / 0.667588,
  conservative observed response spacing 72.745 ms. Dry run stayed ON.
- At 12:37, Main-PC E produced stable target 15 at 232–255 m with calibrated
  dry-run movement computation. The normal UI was inspected and operated live.
- The measured >50 ms cycle exposed another controller issue: ordinary stall
  rejection also reset legitimate feedback waits. An added 80 ms actuator
  simulation now converges below 3 px within 2 s using <=16-count commands,
  without converting unconfirmed stalls into queued catch-up.

Not yet performed: physical player-following/convergence, manual-input coexistence
or physical click-latency measurements on the final feedback-paced build. Real
calibration movements were generated only in the attended, explicitly confirmed
setup. Synthetic/loopback/headless suites never opened a physical device. Do not
describe successful calibration or dry-run selection as a complete following test.

### Correction after the first live following trial

At 12:58:16 the adapter sent `km.move(1,0)`. Target 58 was valid at about 91 m;
the old exact-camera-delta checker timed out and called the generic hard-fault
function, clearing runtime verification. Later valid target 57 at about 78 m
remained permanently blocked despite the saved calibration still being valid.
This was an output-state design defect, not evidence that the successful
16-step calibration had been spoiled by the operator.

The in-game exact-delta requirement is replaced with the measured bounded pacing
described above. Transient source/view interruptions preserve the measured mapping
and require key release plus source/channel checks; real I/O failures cannot use
this recovery. Logs distinguish `verified`, `paused` and `fault`. The UI now
distinguishes stale camera data, changed zoom, missing calibration, unverified
connection and the actual device error. A fixed-height status area keeps changing
messages from moving setup controls between mouse-down and mouse-up.

Added deterministic tests reproduce mixed manual/recoil camera deltas after a
one-count command, enforce the measured 81.386 ms spacing, prevent re-arm while
the physical key is held/capture active/input missing/lens changed, preserve
calibration through transient pauses, and prove a real transport fault cannot be
softened into a recoverable pause. The production ImGui panel test also checks
that changing diagnostic text does not move its setup button.

### Calibration timing

The next live build stopped two trials after 14 and 9 validated steps. The first
reported a late camera response. The second reported stale camera data. Neither
trial replaced the saved mapping. The camera snapshot for movement and calibration
is now copied after rendering and the trigger update. Both the control gate and
calibration still require data younger than 35 ms. Calibration uses the camera
sample timestamp for the measured response and keeps the observation clock for
the stillness check. Short gaps send no commands and restart the quiet period
between pulses. Persistent loss, a changed view, and responses outside the bound
still stop calibration. Regression tests exercise all three timing paths.

### Smoothing and motion lead

Smoothing sets the velocity transition time. Strength sets the tracking gain.
The controller integrates displacement over the transition and limits speed and
acceleration, with braking near the aim point and hysteresis around the deadzone.
The measured legacy output interval and command limit still apply. In the
80 ms simulation, smoothing at 10, 70 and 300 all starts below the 16-count cap
and reaches the same steady output for a large fixed error.

Direct tracking uses the latest world velocity from the position scanner. This
history remains available when bones briefly go stale or the activation key is
released. The horizon includes pose age and half the measured response time,
capped at 65 ms. The engine reduces the lead to stay within 12 screen pixels
and the acquisition area. It uses the observed position if the velocity is
missing, stale or too slow, or if a bounded projection cannot be found.
This is latency compensation. The ballistic provider is unchanged.
In the sidebar below Triggerbot, use **Movement prediction** to switch it on or
off. **Prediction** scales the estimate from 25% to 150% before applying the
existing limits. Existing profiles default to on at 100%, matching the previous
behavior. The status reports the actual lead used on the current sample.
**Show search circle** sits below the acquisition radius and controls drawing only.
With Trigger Debug enabled, `[assist-move]` logs submitted counts, timestamp,
observed pixel error and lead for a hardware comparison.
