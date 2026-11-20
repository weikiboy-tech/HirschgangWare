# Clutch Mode

In the native radar sidebar open **ESP OVERLAY → CLUTCH MODE**. Enable the
feature, click **Hotkey**, release held keys, and press one keyboard key on the
gaming PC. Local keys and mouse buttons cannot become a Clutch binding. Escape
on either keyboard cancels capture and retains the previous binding; Clear
removes it. End, F8, Windows keys and modifier-only bindings are rejected.
Toggle is the default; Hold is selectable. Status is BINDING, NO INPUT, READY
or ACTIVE. A shared trigger key produces a warning and still serves both
functions. Starting a trigger capture cancels Clutch capture; starting Clutch
capture ends trigger capture. This does not disable normal trigger operation.

Clutch is a runtime override of `drawScene`, used by both GPU and GDI. Only
kind-1 human skeletons reach drawing, in their existing enemy/friend colors.
The existing real bone links and head rendering are reused. Real Skeletons Only
also excludes players without projected pelvis and neck joints, instead of
drawing the existing box-derived stick fallback. A compact distance in metres
is always drawn below each player skeleton, even if normal Distance is off.
The configured crosshair also remains visible, respecting its existing enable
switch, shape, size and color. Both modes use the same crosshair drawing code.
Box, name, held-item,
health, infected/animal/vehicle, loot, minimap, FPS, region debugging
and bone-ID drawing are bypassed. No Clutch status is drawn in the overlay.
The native map and WebRadar are unchanged. Normal ESP preferences are never
rewritten by activation; leaving Clutch immediately restores their effect.

## Input and lifetime

`trigger::setRemoteInputNeeded` requests the same cached DMA keyboard stream
already used by trigger bindings. `remoteSample` exposes that read-only sample.
The existing 8 ms input throttle and worker's 200 ms stale-sample rejection
remain unchanged. Clutch works without an enabled trigger or trigger binding.
No extra bitmap resolver, input thread, mouse output, or game writes are added.
The Clutch state machine has no Win32 calls, allocations, sleeps or timers.
Only the UI/render thread accesses it. Human bone collection is prepared while
the feature is enabled and bound, even with normal Skeleton disabled, via an
atomic request to the existing scanner. Clutch alone does not request infected
bones, text or loot. Existing radar/WebRadar data consumers remain active.

Missing input, changed keyboard-session PID, capture, stopping and application
startup clear activation. After initialization or reconnection the bound key
must be observed released before it can activate. Toggle only responds to
rising edges. Bind capture requires all remote keyboard keys released first;
ambiguous multi-key samples require another complete release. Invalid samples
clear the capture arming as well. Escape never replaces the previous binding.

The existing INI reader/writer saves only these settings (not runtime state):

```ini
clutch_enabled=0
clutch_hotkey=0
clutch_activation=toggle
clutch_real_bones_only=0
```

An absent feature configuration defaults disabled and unbound. Invalid/reserved
saved keys become unbound. Unknown activation values become Toggle. Other INI
keys, normal ESP settings, trigger bindings and saved WebRadar profiles remain.

## Files and checks

- `src/clutch.h`: state machine, config codecs and renderer policy.
- `src/esp_launcher.{h,cpp}`: settings and existing INI integration.
- `src/radar.{h,cpp}`: compact sidebar controls, key labels and Escape handling.
- `src/trigger.{h,cpp}`: shared-input subscription and read-only snapshot.
- `src/esp.cpp`: runtime integration, bone demand, rendering override, stop reset.
- `HirschgangWare.vcxproj`: header registration; no new library/dependency.
- `tests/clutch.cpp`, `tests/build_clutch_test.bat`: deterministic state/policy
  tests, actual shared trigger input integration, simultaneous trigger binds,
  capture arbitration, and Windows INI round-trip preserving an unrelated key.

Run `tests\build_clutch_test.bat`, `tests\build_dayz_camera_test.bat`,
`node tests/webmap.mjs`, and `build.bat x64` using the existing MSVC toolchain.
The camera suite also exercises projected body regions and click dispatch
against an isolated local test receiver, not the gaming PC's real mouse box.
Pure policy tests verify normal/Clutch decisions; GPU and GDI share drawScene
by construction. Those tests do not prove real rendered pixel output or real
DMA/fuser behavior.

## Live acceptance still required

1. Start from the launcher. Disable the trigger and clear its bindings. In the
   Clutch panel bind a main-PC key, then verify local keyboard/mouse activity
   cannot activate Clutch. Cancel another capture with Escape on each PC.
2. Enable ordinary text, boxes, FPS, crosshair, minimap and loot; disable normal
   Skeleton. Press the remote key once: only human skeletons and their distances
   and the enabled crosshair should remain. Disable normal Distance too: Clutch
   distances must stay visible. Check the crosshair's shape, color, size and
   enable switch in both modes.
   Hold it: Toggle must not flicker. Release and press: every original option
   should reappear unchanged. Friends keep their colors.
3. Test Hold, Real Skeletons Only, missing-bone fallback, an identical trigger
   key and both trigger bindings. Inspect that trigger behavior remains intact.
4. Disconnect while active and while holding the key. Clutch must leave active
   state. Reconnect while still holding: no activation until release + press.
5. Restart: configuration retained, active state OFF. Repeat in GPU and forced
   GDI mode and compare actual overlay/fuser output and frame timings.

No hardware success, identical GPU/GDI pixels, latency bound or absence of FPS
regression is claimed until those live checks have been performed.
