# Falling onto ramps

The supplied session shows a collision hold at 169.36 seconds near
(1806.76, 230.07, 60.45) in the Undercity entrance, with one of two required
chunks resident. Later falling animations are logged, but no exact floor-drop
position is recorded. The previous floor_drop diagnostic only covered movement
that remained grounded, so it could not identify a missed floor that started
a fall. These observations do not establish that every reported fall shares
one cause.

Code review found a reproducible landing error: airborne movement first advances
XY, then casts downward from the previous feet height plus 0.05 yards. When the
player descends while moving onto an uphill ramp, the ramp at the new XY can
already be above that ray origin. The query misses it and can select a lower
floor instead.

The landing probe now includes the maximum walkable rise across the horizontal
substep, capped at the existing 0.6-yard step height. Stationary falls retain
the original tolerance. Existing spatial substeps, collision loading barriers,
jumping, gravity and normal ledge falls remain in place. The change applies to
the shared terrain/model/WMO/transport floor query. Grounded departures with
a drop larger than 1.5 yards now log floor_leave, including XYZ and the selected
floor, to help diagnose remaining locations.

tests/test_psp_ramp_landing.py extracts the production airborne integration and
tests uphill landings in both directions at several world heights, low-FPS thin
deck landings, overhead decks, rising jumps, ledges and pending collision.
The bridge loading regression and 4,608 transformed model floor queries also
pass. Hardware confirmation remains required. Rebuild EBOOT only; no collision
pack rebake is required for this movement fix.

Fog received a separate small adjustment: the authored fade begins earlier by
8% of its existing fade width. Its end distance, color and classic projection
are unchanged; NPC/adaptive ranges do not control it. Background NPC loading
remains enabled.
