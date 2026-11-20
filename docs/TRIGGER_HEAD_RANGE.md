# Head/range investigation

The report is that torso activation works more often than head activation and
long-distance activation is unreliable. The inspected configuration had Head
enabled (`hitMask=1023`), a 354 m trigger limit, 30 ms continuous target dwell,
and radius 2. These settings are retained, not silently replaced.

The previous head drawing and head decision differed: skeleton rendering used
the projected head-to-neck separation as its circle radius, with a two-pixel
minimum. Trigger geometry uses a world radius of half the head/neck gap, capped
at 0.12 m, and rejects gaps outside 0.04–0.5 m. At distance its projected region
can be subpixel. Thus being inside the illustrative drawn circle alone does not
prove the reticle lies in the tested region. Show body regions draws the actual
polygons. Those polygons are anatomical approximations, not game collision data.

The existing 75 ms pose limit is retained. Invalid/old data never authorizes a
click, and camera/region/ID checks, friend exclusion, both trigger binds and
device dispatch remain unchanged. A continuous 30 ms dwell can be interrupted
on a small target; a missed shot after valid detection is a separate question
from failure to detect the target.

`trigger_geometry::headShape` centralizes the existing head validity/radius
calculation without enlarging it. The Live area now reports the closest
sampled human head's reason, distance, head/neck gap, pose age, projected radius
and reticle-center offset. The log records `[trigger-head]` at most once a
second while a trigger bind is held or Debug is on. It also reports the actual
target/bone/dwell and configured range. These measurements use existing snapshots
and do not add DMA reads. The probe describes the nearest sampled head, not
necessarily the selected torso target or a guaranteed visible/unoccluded enemy.

`tests/dayz_camera.cpp` adds head-only center/interior/outside checks at
50/150/300/354/600/1000 m and three lens scales, including subpixel regions and
the 354 m boundary. They pass with unchanged geometric thresholds. The existing
camera, body, friends, key-capture and isolated mouse-output tests also pass.
This does not prove the real head bones, timing or fuser alignment are correct.

## Shared head outline correction

The existing validated world head radius is now projected through one shared
`projectDisc` function for both head decision polygons and the skeleton head
outline. The outline no longer uses the full head/neck gap or a two-pixel
minimum. Its center and radii remain floating point until drawing. The GPU
draws this ellipse with subpixel antialiasing, restoring its previous antialias
mode afterwards; the GDI fallback rasterizes the same bounds to integer pixels.
GDI raster output cannot represent an arbitrarily small geometric region.
The head has one colored stroke rather than a three-pixel dark halo. Skeleton
links, friend colors, trigger geometry size, dwell and user configuration remain
unchanged. Clutch uses the same outline.

Tests compare displayed ellipse bounds to actual head polygon extrema over the
existing distance/lens matrix. A recorded-style 0.17 m head/neck gap at 100 m
and tanX 1.279907 projects to about 0.638 px radius instead of the former minimum
2 px circle. This is a display/decision consistency fix, not a proven cure for
every missed automatic click or a change to game hitboxes.

Live test: hold the configured bind on a stationary target's head first nearby,
then at about 150–300 m. Compare the actual region overlay with the game head
and retain the `[trigger-head]` lines. Repeat with the torso and with iron sights
versus the failing scope. A ready head with offset greater than the radius is
not equivalent to a confirmed head overlap (the full polygon test is decisive);
an old pose or rejected bone gap identifies an upstream data/geometry gate.
Only change geometric/timing behavior after these measurements establish which
gate actually fails. No completed real-hardware fix is claimed from these tests.
