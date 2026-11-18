# Two independent trigger bindings

The trigger controls in both the sidebar and module window show Primary and
Secondary bindings. Secondary is optional and initially unbound. Each slot
captures a keyboard key or mouse button on the main PC (PCIe input) or second
PC (local input), remembering the source separately. Clear removes only that
slot. Existing primary INI keys retain their meaning; secondKey,
secondKeyHid and secondKeySource are additional settings.

Activation is Primary OR Secondary, never a two-key chord. Releasing one
while the other remains held does not deactivate the trigger or start a
second click stream. The existing target, friend exclusion, dwell and click
timing rules apply to the combined held state. Each slot displays HELD.

Escape is reserved for cancellation, including while capture is waiting for
other keys to be released. It leaves previous bindings intact; a previously
empty slot stays empty. Existing Escape values loaded from configuration are
cleared. After capture, a newly bound held key must be released before it can
activate the trigger. Cancellation also requires release of held old binds.
The cancellation consumes the corresponding window Escape event and repeats,
so the radar's normal Escape-to-close shortcut does not also close the window.

PCIe samples are still read when only Secondary uses the main PC. The existing
left-button synthetic-feedback guard is evaluated per binding so unrelated
keys remain readable. MAKCU binary keyboard events retain per-HID held states
instead of only the last keyboard event. Untagged legacy box text responses
remain primary-only; they cannot reliably identify two keyboard keys.

`tests/build_dayz_camera_test.bat` tests both slots and both sources with
simulated PCIe snapshots and a local input adapter, all five mouse buttons,
overlapping holds, wrong-PC isolation, reconnect baseline, capture/cancel,
INI round trips and Escape rejection. It also exercises the existing target,
friend-filter and actual click-dispatch code against an isolated UDP receiver.
It does not inject desktop input or verify physical two-PC/MAKCU hardware.

Live acceptance: keep the existing Primary; bind a different key/button to
Secondary. Check each alone and both held together, then release Primary
while continuing to hold Secondary. Repeat with bindings captured from each
PC's keyboard and mouse. Start capture and press Escape on each PC: neither
saved bind may change, and an empty slot must stay empty. Green friends must
continue to produce no target candidates.
