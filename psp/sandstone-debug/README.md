# SandstoneDebug

PSP diagnostics, taken out of another PSP port after a week of hunting crashes
on hardware. Not a single line that has not survived a real console.

## What it does

* **Hard-crash catcher** -- a kernel PRX. The report goes to the screen and to a
  file: cause, exception name, registers, a memory window, **the call chain
  from the game's stack**, FCSR on an FPU fault, the build number, **the log
  tail**.
* **Ring of the last log lines** -- survives both rare commits and the death of
  the memory card driver: it is read by a kernel thread that needs no card.
* **VRAM markers** -- four rows of coloured squares. The only instrument that
  still works when the log no longer gets through: a frozen screen counts the
  milestones that were passed.
* **Thread monitor** -- a stack canary plus the thread the kernel killed for an
  overflow (`PSP_THREAD_KILLED`), and the object a thread is blocked on.
* **PSPLink inside the game** -- for when a real debugger over USB is needed.
* **symbolicate.py** -- turns a report into function names using the symbol map.

## Integration

```c
#include "sdbg.h"

sdbg_cfg cfg = {0};
cfg.enable      = 1;
cfg.prx_path    = "ms0:/PSP/GAME/MYGAME/sdbg.prx";
cfg.report_path = "ms0:/PSP/GAME/MYGAME/CRASH.TXT";
cfg.build       = MY_BUILD_NUMBER;
sdbg_set_log(my_log, my_log_direct);   /* the log stays yours */
sdbg_init(&cfg);
...
sdbg_shutdown();                       /* REQUIRED before exit */
```

Build: `make` in `lib/` and in `prx/`; the project gets `-I.../lib -L.../lib -lsdbg`
and `sdbg.prx` next to the EBOOT.

## What not to expect

Priority exception handlers **do not receive** a fault of a user thread: the
firmware intercepts it first and hands out its own `break`. So the `epc` in the
report is a firmware address, and the place where the game crashed is taken
from the game's own stack. Checked three ways and closed by measurement: the
windows around the firmware's registers are empty, a search through all of
kernel memory finds only noise, and taking the default handler away from the
firmware is not possible -- boot dies. When the real context is needed, use
PSPLink.

## Example consumer

The library is meant to be built against nothing but `lib/`: a tiny test
application (not part of this source tree) that uses ONLY `lib/`, without a
single file from the port, is the readiness criterion of the library -- while
there is only one consumer, a leftover thread tying the library to it goes
unnoticed. Its buttons: circle -- write through a null pointer, cross --
division by zero, square -- VRAM markers, triangle -- report of stacks and
threads.
