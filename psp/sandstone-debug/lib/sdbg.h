/* sdbg.h -- SandstoneDebug, portable PSP diagnostics.
 *
 * WHAT THIS IS. A hard-crash catcher, a ring of the last log lines, a thread
 * monitor and VRAM markers -- everything that, over a week of hunting on
 * hardware, proved itself necessary in the host game's PSP port and that any
 * other project on this console needs just the same.
 *
 * WHAT IS NOT HERE AND NEVER WILL BE. Not a single line that has not survived
 * hardware: every feature here first worked in the host game and was verified
 * by a run on a PSP-2000. The list of checks is in docs/sandstone-debug-plan.md.
 *
 * THE MAIN LIMITATION YOU MUST KNOW BEFORE USING IT. Priority exception
 * handlers do NOT receive a user thread's fault: the firmware intercepts it
 * first and hands us its own break. Therefore the report names a firmware
 * address, and the game's crash site is recovered FROM THE GAME'S OWN
 * STACK (sdbg_init determines its bounds by itself). The analysis is in
 * docs/why-not-the-firmware-context.md.
 */
#ifndef SDBG_H
#define SDBG_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- CONFIGURATION. Everything that distinguishes one project from another
   lives here and only here: the library itself does not read any settings file. */
typedef struct sdbg_cfg {
    const char *prx_path;     /* "ms0:/PSP/GAME/MYGAME/sdbg.prx" */
    const char *report_path;  /* where to write the report; ARMED.TXT goes next to it */
    int         build;        /* build number -- goes into the report */
    int         self_test;    /* 0 normally; 2/3 -- catcher self-tests */
    int         enable;       /* 0 -- do not load the catcher at all */
    /* HOW MANY WORDS OF THE COUNTER BLOCK TO SAMPLE. 0 (default) -- twenty,
       the hardware-verified set 0x00…0x4C. The twenty-first word (0x50) on
       the model 8 board KILLED the catcher's kernel thread in a test run:
       silently, with no crash report, and after that the instrument only
       looked alive -- the block magic was set, but there was not a single
       snapshot.
       So the extension is an EXPLICIT request by the game and a separate run,
       not the default. Twenty is ENOUGH to compare the two block layouts: both
       put all five counters of interest within 0..19. */
    int         perf_regs;
} sdbg_cfg;

/* Bring up the catcher. Returns the number of handlers the kernel accepted
   (0 -- the instrument is not armed, and that is NOT a reason to refuse to
   start). It finds out by itself the base and size of its own text, the
   bounds of its own stack and the address of the log ring. */
int  sdbg_init(const sdbg_cfg *cfg);

/* Take the catcher down BEFORE exiting. Mandatory: otherwise the kernel
   unloads the module while its handlers stay in the firmware lists as
   pointers into freed code (hardware 432..457 -- death on exit). Returns the
   number of handlers removed. */
int  sdbg_shutdown(void);

/* ---- RING OF THE LAST LINES. The log on the card is flushed rarely, and
   death comes between flushes: on hardware 455 the log file was lost
   entirely. The ring lives in the game's memory and is read by the catcher's
   KERNEL thread, which does not need the card at that moment. Call it on
   every completed log line. */
void sdbg_log_append(const char *line, int len);

/* ---- VRAM MARKERS. 12x12 squares in the top lines of the screen, up to
   twenty per row, four rows. The only instrument that works when the card
   driver dies and the log does not get through: the frozen screen counts the
   milestones passed. row 0..3, n -- how many squares. */
void sdbg_mark(int row, int n);
void sdbg_mark_clear(int row);
void sdbg_mark_enable(int on);   /* 0 in release -- there must be no squares on screen */

/* ---- THREADS. A stack canary (the kernel fills the stack with 0xFF on
   creation, we ask for the untouched tail) plus state: a thread killed by the
   kernel and the wait object. Register each of your threads right after it
   starts. */
void sdbg_watch_thread(const char *name, int thid);
void sdbg_stack_report(const char *tag);   /* stack minimums; tag != 0 -- always print */
void sdbg_thread_report(void);             /* killed by the kernel / changed its wait */

/* Where the library writes its lines. Not set -- it stays silent (except for
   the markers and the catcher report, which do not need the log). */
typedef void (*sdbg_log_fn)(const char *fmt, ...);
void sdbg_set_log(sdbg_log_fn normal, sdbg_log_fn direct);

/* Print a single line from inside the library -- goes to the game's log. */
void sdbg_log_line(const char *fmt, ...);

/* ---- PSP HARDWARE COUNTERS. They answer the question "WHY is it slow",
   unlike the zone timeline, which answers "what happened and when".
   Cache misses, writebacks, VFPU and FPU cycles, bus load -- all of this is
   sampled by the console itself, and only from the kernel (registers
   0xBC400000), that is, through sdbg.prx. A snapshot accumulates over a window
   of about 50 ms; the seq field grows with each new one, us is the actual
   length of the window.

   THE FIELD ORDER COMES FROM THE SDK'S pspdebug.h, AND IT HAS BEEN VERIFIED ON
   HARDWARE, NOT TAKEN ON FAITH. September 15, 2026, PSP-3000 (model 8 board),
   a calibration run (the host game's perfcal.c) ran seven workloads with
   known-in-advance behaviour and confirmed the layout counter by counter:

     r13 i_miss      -- grew ONLY on an I-cache flush (109 641 vs 1 820);
     r14 d_miss      -- grew on read AND on write EQUALLY (133 552 and
                        103 396 -- the same number of lines in both passes);
     r15 d_writeback -- grew ONLY on write (102 722 vs 1 218 on
                        read: a clean line does not travel back);
     r17 fpu_inst    -- grew only on floating-point arithmetic (5 358 030);
     r18 vfpu_inst   -- grew only on vector arithmetic (1 796 188).

   The competing PSPTEK `#prof` layout (it has one counter more --
   `CNTSTALL` at 0x0C, and everything from `internal` on is shifted) failed ALL
   FIVE labels on the same numbers. The question is closed.

   ⚠ WHAT IS STILL NOT CLOSED: absolute normalisation. `cpuck` still does not
   match 333 MHz, and `systemck` stays constant across all workloads.
   The numbers are fit for COMPARING windows with each other, not as a
   fraction of a cycle.

   Whoever needs the raw words without labels (the calibration does, otherwise
   its conclusion would be circular reasoning) -- see sdbg_perf_raw below. */
typedef struct sdbg_perf {
    unsigned seq, us;
    unsigned enable, systemck, cpuck, internal, memory, copz, vfpu, sleep;
    unsigned bus_access, uncached_load, uncached_store, cached_load, cached_store;
    unsigned i_miss, d_miss, d_writeback, cop0_inst, fpu_inst, vfpu_inst;
} sdbg_perf;

/* ---- RAW BLOCK: 21 WORDS WITHOUT A SINGLE LABEL.
   Needed by exactly one reader -- the calibration, and for a single reason:
   it finds out WHAT LIES at each offset, so it has no right to rely on the
   labels of the struct above. Were it to read `p.d_miss`, any conclusion it
   drew would be circular reasoning: "D-cache misses went up, therefore these
   are D-cache misses".
   The twenty-first word (0x50) is DELIBERATELY not put into the struct: it
   has no name in any document we trust, and inventing a label before the
   calibration would mean creating a third version of the layout.
   Returns: 1 -- the snapshot is fresh, 0 -- no new window yet or no catcher.
   out must hold SDBG_PERF_REGS words. */
#define SDBG_PERF_REGS 21
int  sdbg_perf_raw(unsigned *out, unsigned *seq, unsigned *us);

/* WHETHER THE COUNTERS EXIST AT ALL IS A SEPARATE QUESTION, AND NOT A LUXURY.
   sdbg_perf_raw returns zero for TWO different reasons: "no new window yet"
   (the usual case, the window is 50 ms) and "there are no counters and there
   will be none" (an emulator, or the catcher did not come up). Whoever waits
   for a window in a loop has to tell these two zeros apart, otherwise it will
   wait forever.
   The cost of getting this wrong has already been counted: the first version
   of the calibration told them apart with a watchdog of a million polls, so on
   an emulator loading would have stalled for about eight minutes -- and it
   would have looked like the game hanging, not like "there are no counters
   here".
   1 -- the catcher accepted the block and is sampling; 0 -- nothing to ask. */
int  sdbg_perf_ready(void);

/* How many words the catcher ACTUALLY fills. Not a constant: the game decides
   the number (sdbg_cfg.perf_regs), and the catcher confirms it by writing it
   into the block. A reader of the raw words must have it: an unfilled word
   stays zero, and zero is a legitimate counter value. Without knowing the
   bound, "nobody sampled it" and "it is zero all the time" cannot be told
   apart. 0 -- no catcher. */
int  sdbg_perf_count(void);

/* TRACE OF THE KERNEL THREAD'S LAST ACTION ON THE REGISTERS.
   The firmware removes a crashed kernel thread WITHOUT a report -- the catcher
   does not see its own death. From the game's side it looks like "no
   snapshots", and the cause cannot be distinguished: the thread did not
   start, the thread died, the counters are off.
   This number answers: 0x100+i -- was zeroing regs[i], 0x200+i -- was reading
   regs[i], 0x300 -- the whole window was sampled. Stuck at 0x114 -- the
   twentieth register killed it.
   Added after a test run where exactly this happened. */
unsigned sdbg_perf_step(void);

/* ---- ADDRESS OF THE KERNEL READER: SYNCHRONOUS SAMPLING FOR THOSE WHO FIND
   50 MS TOO COARSE.
   Background sampling gives a 50 ms window -- a whole frame fits into it with
   all of its phases mixed together, and from such a window you cannot ask
   "what did culling get stuck on". The answer is the difference of two
   snapshots taken AT THE BOUNDARIES of a phase, and those can only be taken
   from the kernel.
   What is returned here is the ADDRESS of the kernel function
   `int f(unsigned *dst, int n)` that the catcher published about itself.
   Calling it is up to the caller: the bridge into the kernel (kuKernelCall or
   another) depends on the firmware, and the library does not know about the
   firmware and must not. A sample call is perfzone.c in the host game.
   The registers are NOT RESET by this: they are free-running, and the work
   between two snapshots is their difference. They do not overflow within a
   frame.
   0 -- no door: the catcher did not come up or it is older than this change. */
unsigned sdbg_perf_readfn(void);

/* Turn counting on or off. While it is off, the console spends nothing. */
void sdbg_perf_enable(int on);
/* Fetch a fresh snapshot. 0 -- nothing new (or the catcher is not up). */
int  sdbg_perf_read(sdbg_perf *out);
/* A ready line for the log: busy share of cycles, misses per thousand cycles, bus. */
void sdbg_perf_log(void);

/* ---- PSPLINK INSIDE THE GAME. When you need a real debugger over USB: loads
   psplink.prx from the game itself (a technique taken from a sibling PSP
   port), because psplink cannot launch an EBOOT with LARGE_MEMORY. The only
   thing that shows the full context of the crashed thread. 0 -- did not come
   up. */
int  sdbg_psplink_boot(const char *prx_dir);
int  sdbg_psplink_active(void);

#ifdef __cplusplus
}
#endif
#endif /* SDBG_H */
