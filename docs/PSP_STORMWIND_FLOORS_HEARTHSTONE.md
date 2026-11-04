# Stormwind floor seams and hearthstone landing retry

Session analyzed: user attachment c6ec20b6-643a-4226-b5d5-cd581532645e.
The runtime reports build 118; the old launcher reused that number, so it cannot
identify the source revision. The run exits cleanly. The world stamp b70ab5f7
matches the locally inspected C:/Kiln-output asset set.

## Evidence

The hearthstone item (6948, spell 8690) starts at 93.033 s with a 10-second cast,
finishes at 103.060 s, and receives teleport ACK at 103.061 s. The client applies
the 649-yard move at 103.072 s. The movement freeze begins immediately afterward:
collision_wait reports terrain=1, doodads=0 continuously through 143.251 s.
The destination's tile job reports t=1 d=0 at 118.062 s. By 129.304 s the queue,
job and pending reads are all empty, yet the missing initial doodads never retry.
This confirms a post-arrival streaming/movement hold, rather than a failed cast.

Stormwind has eight logged floor_leave events between 403.311 and 553.579 s,
with drops to much lower geometry. Six representative query locations were
checked against the matching local world pack. Exact zero-width rays reproduced
missing upper floors. Offsetting the point by 3-10 cm recovered nearby support.
The production replacement query found the following support heights:

| Feet Z in log | New support Z |
|---:|---:|
| 96.530 | 96.769 |
| 87.770 | 87.746 |
| 94.190 | 94.200 |
| 96.580 | 96.825 |
| 94.270 | 94.277 |
| 94.580 | 94.586 |

The raw client geometry used by the local probe stays under outputs/checks and
is excluded from source/review archives. These asset queries verify geometry
support, not real-hardware gameplay outcomes.

## Changes

- Big-WMO floors now use a 12-cm support radius around triangle edges. The normal
  center ray remains the fast path. Queries include neighboring collision-grid
  cells so chunk seams can be bridged. Only walkable triangles below the probe
  qualify. Real ledges beyond the small radius still allow falling; geometry
  above the player is not used to snap upward through floors.
- The stationary retry scan now revisits missing initial doodads as well as
  failed terrain, once per second when the queue/job are idle. A failed inn or
  building install can retry after memory becomes available without requiring
  the already-blocked player to move first.
- Outside a big WMO, terrain near a teleport landing no longer releases the
  landing hold before initial building geometry is ready. Existing big-WMO
  readiness checks and the timeout remain; the normal collision loading barrier
  continues to protect movement if a required load is still pending.

The log does not prove an independent frozen-frame/GE fault. The reported fixed
building view coincides with a confirmed movement hold; visual behavior after
that hold is fixed still needs testing on the PSP/Vita.

Validation: full host UI/asset regression suite; new stationary-retry and landing
hold cases; synthetic seam/ramp/real-ledge/upward-floor cases; the six matching
Stormwind asset probes; SDK-header host syntax checks for streaming, landing and
BigWmo collision code. Hardware retest remains required.

Rebuild EBOOT.PBP only. No world, collision or audio pack rebake is required.
