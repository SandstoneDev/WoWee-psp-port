/* sdbg.c -- SandstoneDebug, the user-mode half. The feature overview and the
 * main limitation are in sdbg.h; here there is only the mechanics and the
 * reasoning that concerns the implementation itself.
 *
 * WHERE THIS CODE COMES FROM. It was not written from scratch: every part was
 * taken out of the host game, where it passed hardware testing. The build
 * numbers in the comments are the numbers of runs on a PSP-2000 that drove the
 * decisions; they are kept on purpose, because without them any of these
 * oddities looks like superstition.
 */
#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspiofilemgr.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "sdbg.h"

/* ---- WHERE WE WRITE OUR LINES. The library does not keep a log of its own:
   the game already has one, with its own format, flushing and mirror in pspsh.
   Two callbacks, not one: a "direct" line must reach the card IMMEDIATELY
   (it is written about a death), a normal one goes through the buffer. Not
   set -- we stay silent. */
static sdbg_log_fn s_log, s_logDirect;

void sdbg_set_log(sdbg_log_fn normal, sdbg_log_fn direct)
{
    s_log = normal;
    s_logDirect = direct;
}

/* ONE PRINT DOOR FOR ALL PARTS OF THE LIBRARY (sdbg_psplink.c calls it too).
   We expand the format here and hand the game a finished line: the log is the
   game's own, and taking its function with a va_list would demand more of it
   than necessary. The stack buffer is small on purpose -- these lines are
   short by construction. */
void sdbg_log_line(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    if (!s_log) return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    s_log("%s", buf);
}

#define SDBG_LOG(...)  do { if (s_log) s_log(__VA_ARGS__); } while (0)
#define SDBG_DIR(...)  do { if (s_logDirect) s_logDirect(__VA_ARGS__); \
                            else if (s_log) s_log(__VA_ARGS__); } while (0)

/* ================= RING OF THE LAST LOG LINES =================
 *
 * WHY A SEPARATE RING WHEN THERE IS ALREADY A LOG. The log buffer is emptied
 * by a flush: lines older than the last flush are gone from memory, they are
 * already in the file. The catcher needs the opposite -- the last lines
 * REGARDLESS of flushing, because death comes between flushes. On hardware 455
 * the log file did not survive the death of the card driver at all, while the
 * ring does survive: it is read by the catcher's kernel thread, which does
 * not need the card.
 *
 * FLAT AND WITHOUT A LOCK. It is written by the same thread as the log; it is
 * read when the game has already stopped. A race is possible on the last
 * line -- cheaper than a lock in exception context (kernel calls are
 * forbidden there).
 *
 * THE POSITION IS MONOTONIC, NOT WRAPPED: the reader needs to know whether
 * the ring has wrapped at least once or not yet, otherwise it will print the
 * zeros from the tail. */
#define SDBG_RING_SZ 2048
static char s_ring[SDBG_RING_SZ];
static volatile unsigned s_ringPos;

void sdbg_log_append(const char *line, int len)
{
    int i;
    unsigned pos = s_ringPos;
    if (!line || len <= 0) return;
    for (i = 0; i < len; i++)
        s_ring[(pos + (unsigned)i) & (SDBG_RING_SZ - 1)] = line[i];
    s_ringPos = pos + (unsigned)len;
}

/* ================= VRAM MARKERS =================
 *
 * WHY. Hardware 423..428: the kernel died together with the card driver, and
 * not a single line after the latest milestone reached the file. Squares in
 * VRAM live as long as the screen lives, and a frozen screen is exactly what
 * the tester sees: number of squares = last milestone passed.
 *
 * COLOURS IN THE PSP'S OWN 5650 LAYOUT, NOT PC RGB. GE Users Manual p43:
 * bits 0..4 -- RED, 5..10 green, 11..15 blue. Before this the numbers were
 * written as RGB565, and "blue" glowed red, and the catcher's bar blue. */
#define SDBG_C5650(r, g, b) ((unsigned)(((b) << 11) | ((g) << 5) | (r)))
#define SDBG_VRAM  0x44000000u    /* uncached */
#define SDBG_BUF2  0x44000u
#define SDBG_STRIDE 512

static int s_markOn = 1;

void sdbg_mark_enable(int on) { s_markOn = on ? 1 : 0; }

static void mark_row(int n, int y0, unsigned color)
{
    volatile unsigned short *fb0 = (volatile unsigned short *)SDBG_VRAM;
    volatile unsigned short *fb1 = (volatile unsigned short *)(SDBG_VRAM + SDBG_BUF2);
    int k, x, y;
    if (!s_markOn) return;
    if (n > 24) n = 24;
    for (k = 0; k < n; k++)
        for (y = y0; y < y0 + 12; y++)
            for (x = 0; x < 12; x++) {
                int i = y * SDBG_STRIDE + 4 + k * 16 + x;
                fb0[i] = (unsigned short)color;
                fb1[i] = (unsigned short)color;
            }
}

/* Four rows -- four levels of bisection nesting: a milestone, a milestone
   inside it, a step inside that one and a step inside a function. The colours
   differ so that the tester can name them in words and not mix up the rows. */
static const struct { int y0; unsigned color; } s_rows[4] = {
    { 14, SDBG_C5650(31, 63,  0) },   /* yellow */
    { 28, SDBG_C5650( 0, 63, 31) },   /* cyan */
    { 42, SDBG_C5650(31,  0, 31) },   /* magenta */
    { 56, SDBG_C5650(31, 63, 31) },   /* white */
};

void sdbg_mark(int row, int n)
{
    if (row < 0 || row > 3) return;
    mark_row(n, s_rows[row].y0, s_rows[row].color);
}

void sdbg_mark_clear(int row)
{
    if (row < 0 || row > 3) return;
    mark_row(24, s_rows[row].y0, 0);
}

/* ================= THREADS: CANARY AND STATE =================
 *
 * CANARY. The kernel fills the stack with 0xFF on creation (without
 * PSP_THREAD_ATTR_NO_FILLSTACK), and sceKernelGetThreadStackFreeSize returns
 * the untouched tail -- that is, the MAXIMUM depth over the thread's
 * lifetime, not the current one. So a poll from the frame loop also sees the
 * depth reached inside map loading.
 *
 * STATE. The canary only answers "is the stack sinking". It does not see two
 * other ways to die: the kernel itself kills a thread for an overflow
 * (PSP_THREAD_KILLED, and the thread disappears silently), and a thread can
 * get stuck on a wait forever -- waitType/waitId tell about that. */
#define SDBG_THREADS 8
static struct {
    const char *name;
    int th, minKb;
    int lastSt, lastWt, lastWid, gone, said;
} s_th[SDBG_THREADS];
static int s_thN;
static int s_thTrace;   /* detailed printing of wait changes -- on the game's request */

void sdbg_watch_thread(const char *name, int thid)
{
    if (s_thN >= SDBG_THREADS || !name || thid < 0) return;
    s_th[s_thN].name = name;
    s_th[s_thN].th = thid;
    s_th[s_thN].minKb = 0x7FFFFFFF;
    s_th[s_thN].lastSt = -1;
    s_th[s_thN].lastWt = -1;
    s_th[s_thN].lastWid = -1;
    s_th[s_thN].gone = 0;
    s_th[s_thN].said = 0;
    s_thN++;
}

void sdbg_stack_report(const char *tag)
{
    char ln[192];
    int i, n = 0, changed = 0, alarm = 0;
    for (i = 0; i < s_thN; i++) {
        int freeB = sceKernelGetThreadStackFreeSize(s_th[i].th);
        int kb = (freeB > 0) ? (freeB >> 10) : 0;
        if (kb < s_th[i].minKb) { s_th[i].minKb = kb; changed = 1; }
        if (s_th[i].minKb <= 2) alarm = 1;
        n += snprintf(ln + n, sizeof(ln) - (unsigned)n, "%s%s %d",
                      i ? " " : "", s_th[i].name, s_th[i].minKb);
        if (n >= (int)sizeof(ln)) break;
    }
    if (alarm)      SDBG_DIR("[sdbg] stack min KB: %s -- SINKING%s%s", ln,
                             tag ? " | " : "", tag ? tag : "");
    else if (tag)   SDBG_LOG("[sdbg] stack min KB: %s | %s", ln, tag);
    else if (changed) SDBG_LOG("[sdbg] stack min KB: %s", ln);
}

void sdbg_thread_report(void)
{
    int i;
    for (i = 0; i < s_thN; i++) {
        SceKernelThreadInfo ti;
        int st, wt, wid;
        memset(&ti, 0, sizeof(ti));
        ti.size = sizeof(ti);
        if (sceKernelReferThreadStatus(s_th[i].th, &ti) < 0) {
            if (!s_th[i].gone) {
                s_th[i].gone = 1;
                SDBG_DIR("[sdbg] thread %s GONE: kernel lost its TCB | FAIL",
                         s_th[i].name);
            }
            continue;
        }
        st = ti.status; wt = ti.waitType; wid = ti.waitId;
        /* 32 -- PSP_THREAD_KILLED: "thread manager has killed the thread
           (stack overflow)" in the words of pspthreadman.h itself. As a direct
           line: after this the game is formally alive, and a buffered line
           might not survive until the flush. */
        if ((st & 32) && !s_th[i].said) {
            s_th[i].said = 1;
            SDBG_DIR("[sdbg] thread %s KILLED BY KERNEL (stack overflow), "
                     "stack %d B, free was %d KB | FAIL",
                     s_th[i].name, (int)ti.stackSize, s_th[i].minKb);
            continue;
        }
        if (st != s_th[i].lastSt || wt != s_th[i].lastWt || wid != s_th[i].lastWid) {
            s_th[i].lastSt = st; s_th[i].lastWt = wt; s_th[i].lastWid = wid;
            if (s_thTrace)
                SDBG_LOG("[sdbg] thread %s: state 0x%X, waits type %d object 0x%X, prio %d",
                         s_th[i].name, (unsigned)st, wt, (unsigned)wid, ti.currentPriority);
        }
    }
}

/* ================= HARDWARE COUNTERS =================
 *
 * The console itself counts, the catcher's kernel thread samples (registers
 * 0xBC400000 cannot be read from user code). Here there is only the shared
 * block and the conversion of raw numbers into a line a human can read. */
/* 21 and 'SPE2' -- change of 15.09.2026. The full analysis (why there are now
   21 words and why the magic changed together with the number) is in the
   banner of the same block in prx/sdbg_prx.c. The short version here: the
   numbers must match the kernel half, otherwise the catcher writes past our
   array. */
#define SDBG_PERF_N     SDBG_PERF_REGS
#define SDBG_PERF_MAGIC 0x53504534u

static struct {
    volatile unsigned magic, on, seq, us, step, n, readfn;
    volatile unsigned reg[SDBG_PERF_N];
} s_perf;
static unsigned s_perfSeen;

/* ---- THE COUNTERS EXIST ONLY ON HARDWARE, AND THAT HAS TO BE ASKED, NOT
 * ASSUMED.
 *
 * WHAT HAPPENED. A test build in PPSSPP died on the very first tick of the kernel
 * thread: "Invalid Memory Access, Read Word at bc400004, cur thread
 * psrc_crash_watch". On a real console the counter registers live at
 * 0xBC400000, and the emulator does not map this region at all -- neither
 * for reads nor for writes. The catcher's thread went to zero r[1] and killed
 * the game.
 *
 * WHOSE DEFECT IT IS. Ours, not the config's. The key perf=1 is a legitimate
 * request, and the answer to it must be "they are not here", not a crash. A
 * debug subsystem that crashes the game is worse than an absent one: it takes
 * away exactly the build on which another defect is being hunted.
 *
 * HOW WE TELL. PPSSPP has its own pseudo-device "kemulator:"; a real console
 * has no such driver and sceIoDevctl answers with a refusal. We ask once at
 * startup. The check is cheap and, most importantly, SAFE -- it does not touch
 * the disputed address, it asks the party that knows about itself.
 *
 * WHY WE SWITCH IT OFF HERE AND NOT IN THE KERNEL. In the emulator we simply
 * DO NOT HAND the catcher the block (argument 12 goes out as zero), and the
 * kernel thread itself reads nothing: its very first line is "no block --
 * exit". This way the address 0xBC400000 is never touched, and changing the
 * kernel code -- the most dangerous thing we have -- is not needed at all. */
static int perf_hw_present(void)
{
    static int s_known;      /* 0 -- not asked yet, 1 -- hardware, -1 -- emulator */
    int out = 0;
    if (s_known) return s_known > 0;
    /* 0x00000003 -- "is there a screen" for PPSSPP. What exactly it answers
       does not matter to us: what matters is that the device responded. */
    if (sceIoDevctl("kemulator:", 0x00000003, NULL, 0, &out, sizeof(out)) >= 0)
        s_known = -1;
    else
        s_known = 1;
    return s_known > 0;
}

void sdbg_perf_enable(int on)
{
    /* The PRX sets the magic once it has accepted the block. Without it there
       is nothing to ask for: the catcher is not up or is too old, and silently
       "switching on" would be a lie. */
    if (s_perf.magic != SDBG_PERF_MAGIC) {
        /* Two DIFFERENT causes, and they must not be confused: in the emulator
           there are no counters and cannot be, while on hardware it means the
           catcher did not come up or is old -- and there is something to fix
           there. */
        if (on)
            sdbg_log_line(perf_hw_present()
                          ? "[sdbg] perf: catcher did not take the block -- no counters"
                          : "[sdbg] perf: emulator, no hardware counters at 0xBC400000");
        return;
    }
    s_perf.on = on ? 1u : 0u;
}

int sdbg_perf_read(sdbg_perf *out)
{
    unsigned seq;
    if (!out || s_perf.magic != SDBG_PERF_MAGIC) return 0;
    seq = s_perf.seq;
    if (seq == s_perfSeen) return 0;
    s_perfSeen = seq;
    out->seq = seq;
    out->us  = s_perf.us;
    /* Register order comes from pspdebug.h; we copy field by field so that
       the layout is visible to the eye and does not silently drift apart. */
    out->enable         = s_perf.reg[0];
    out->systemck       = s_perf.reg[1];
    out->cpuck          = s_perf.reg[2];
    out->internal       = s_perf.reg[3];
    out->memory         = s_perf.reg[4];
    out->copz           = s_perf.reg[5];
    out->vfpu           = s_perf.reg[6];
    out->sleep          = s_perf.reg[7];
    out->bus_access     = s_perf.reg[8];
    out->uncached_load  = s_perf.reg[9];
    out->uncached_store = s_perf.reg[10];
    out->cached_load    = s_perf.reg[11];
    out->cached_store   = s_perf.reg[12];
    out->i_miss         = s_perf.reg[13];
    out->d_miss         = s_perf.reg[14];
    out->d_writeback    = s_perf.reg[15];
    out->cop0_inst      = s_perf.reg[16];
    out->fpu_inst       = s_perf.reg[17];
    out->vfpu_inst      = s_perf.reg[18];
    return 1;
}

/* Raw words -- without labels and without interpretation. It keeps its own
   "seen" next to s_perfSeen ON PURPOSE: the calibration and the [perf] line
   visit the block separately, and one shared counter would mean that whoever
   came first ate the window. */
static unsigned s_perfRawSeen;

int sdbg_perf_ready(void)
{
    return s_perf.magic == SDBG_PERF_MAGIC && s_perf.on;
}

int sdbg_perf_count(void)
{
    if (s_perf.magic != SDBG_PERF_MAGIC) return 0;
    return (int)s_perf.n;
}

unsigned sdbg_perf_step(void) { return s_perf.step; }

unsigned sdbg_perf_readfn(void)
{
    if (s_perf.magic != SDBG_PERF_MAGIC) return 0u;
    return s_perf.readfn;
}

int sdbg_perf_raw(unsigned *out, unsigned *seq, unsigned *us)
{
    unsigned s, i;
    if (!out || s_perf.magic != SDBG_PERF_MAGIC) return 0;
    s = s_perf.seq;
    if (s == s_perfRawSeen) return 0;
    s_perfRawSeen = s;
    for (i = 0; i < SDBG_PERF_N; i++) out[i] = s_perf.reg[i];
    if (seq) *seq = s;
    if (us)  *us  = s_perf.us;
    return 1;
}

void sdbg_perf_log(void)
{
    sdbg_perf p;
    unsigned ck, busy;
    if (!sdbg_perf_read(&p)) return;
    ck = p.cpuck ? p.cpuck : 1u;
    /* BUSY SHARE, NOT BARE CYCLES: sleep is the cycles when the processor
       was idle. Work share = (cpuck - sleep) / cpuck. */
    busy = (p.cpuck > p.sleep) ? ((p.cpuck - p.sleep) * 100u / ck) : 0u;
    /* MISSES PER THOUSAND CYCLES, not absolute: the absolute number depends
       on the window length and compares with nothing, while "so many per
       thousand" compares across builds and across maps. */
    /* EVERYTHING PER THOUSAND CYCLES, INCLUDING INSTRUCTIONS. The first
       hardware run (467) showed why: "fpu 1841893" by itself says nothing,
       while "fpu 79/kck" reads at once as "eight percent of cycles issue a
       floating-point instruction". Bare numbers have nothing to be compared
       with -- they grow with the window length. */
    sdbg_log_line("[perf] %u us %u kck: busy %u%% | miss i %u d %u wb %u bus %u "
                  "| inst fpu %u vfpu %u cop0 %u | mem ld %u st %u, uncached ld %u st %u "
                  "(all per 1000 cycles)",
                  p.us, ck / 1000u, busy,
                  p.i_miss * 1000u / ck, p.d_miss * 1000u / ck,
                  p.d_writeback * 1000u / ck, p.bus_access * 1000u / ck,
                  p.fpu_inst * 1000u / ck, p.vfpu_inst * 1000u / ck,
                  p.cop0_inst * 1000u / ck,
                  p.cached_load * 1000u / ck, p.cached_store * 1000u / ck,
                  p.uncached_load * 1000u / ck, p.uncached_store * 1000u / ck);
}

/* ================= CATCHER: LOADING, PROTOCOL, REMOVAL =================
 *
 * WHY A KERNEL PRX AND NOT A FUNCTION HERE. Registering an exception handler
 * is an ExceptionManagerForKernel export, available only to the kernel.
 * Module imports are resolved by the LOADER, before the first instruction, so
 * a single reference to this library from a user module prevents the game
 * from starting at all (8002013C, "library not found").
 *
 * WHY REMOVAL BY A WORD AND NOT BY A CALL. sceKernelStopModule returns 0, but
 * the kernel does NOT RUN module_stop for a game PRX (hardware 433..435), and
 * the PRX's exports are not visible to the game (the same 8002013C). One road
 * remained: a shared word in the game's memory. The game writes a request,
 * the catcher's kernel thread answers. The numbers differ under the mask --
 * on 436 the request matched the answer, and the game "saw" the answer
 * instantly. */
#define SDBG_ASK   0x5E7A5C11u
#define SDBG_DONE  0xD15A2A00u

static SceUID s_mod = -1;
static volatile unsigned s_disarm;
static char s_reportPath[96];

int sdbg_init(const sdbg_cfg *cfg)
{
    SceKernelLMOption lm;
    SceKernelModuleInfo mi;
    SceKernelThreadInfo ti;
    unsigned base = 0, buf[14];   /* buf[13] -- capacity of the counter block */
    int status = 0, rc, armed = 0;

    if (!cfg || !cfg->enable || !cfg->prx_path) return 0;

    memset(&lm, 0, sizeof(lm));
    lm.size = sizeof(lm);
    lm.mpidtext = PSP_MEMORY_PARTITION_KERNEL;
    lm.mpiddata = PSP_MEMORY_PARTITION_KERNEL;
    /* KERNEL PARTITION -- EXPLICITLY. A bare sceKernelLoadModule(path, 0, NULL)
       put the module into user memory, and the kernel did not accept a single
       registration (ARMED 0 of 12). */
    s_mod = sceKernelLoadModule(cfg->prx_path, 0, &lm);
    if (s_mod < 0) {
        SDBG_LOG("[sdbg] catcher NOT loaded: 0x%08X (%s)",
                 (unsigned)s_mod, cfg->prx_path);
        s_mod = -1;
        return 0;
    }

    /* Our own base and text size: the PRX has no way to know them, and
       without them the report cannot be matched against the symbol map. */
    memset(&mi, 0, sizeof(mi));
    mi.size = sizeof(mi);
    {
        SceUID self = sceKernelGetModuleIdByAddress((const void *)&sdbg_init);
        if (self >= 0 && sceKernelQueryModuleInfo(self, &mi) >= 0)
            base = (unsigned)mi.text_addr;
    }

    if (cfg->report_path) {
        int i;
        for (i = 0; i < (int)sizeof(s_reportPath) - 1 && cfg->report_path[i]; i++)
            s_reportPath[i] = cfg->report_path[i];
        s_reportPath[i] = 0;
    }

    s_disarm = 0;
    buf[0] = base;
    buf[1] = 0;
    buf[2] = (unsigned)cfg->self_test;
    buf[3] = (unsigned)&s_disarm;
    buf[4] = (unsigned)cfg->build;
    buf[5] = (unsigned)mi.text_size;
    buf[6] = (unsigned)s_ring;
    buf[7] = SDBG_RING_SZ;
    buf[8] = (unsigned)&s_ringPos;
    /* Bounds of OUR OWN stack: the firmware does not give us the context of
       the crashed thread (analysis in sdbg.h), but this thread's stack is
       intact at the moment of death, and it holds the whole call chain. */
    buf[9] = 0; buf[10] = 0;
    memset(&ti, 0, sizeof(ti));
    ti.size = sizeof(ti);
    if (sceKernelReferThreadStatus(sceKernelGetThreadId(), &ti) >= 0
        && ti.stack && ti.stackSize > 0) {
        buf[9]  = (unsigned)ti.stack;
        buf[10] = (unsigned)ti.stack + (unsigned)ti.stackSize;
    }
    buf[11] = s_reportPath[0] ? (unsigned)s_reportPath : 0;
    /* The counter block is handed ONLY to hardware: in the emulator there are
       no registers, and the kernel thread would kill the game on the very
       first tick (analysis at perf_hw_present). Zero here means "there will be
       no counters", and the catcher understands that. */
    buf[12] = perf_hw_present() ? (unsigned)&s_perf : 0u;
    /* THE CAPACITY OF OUR ARRAY GOES TO THE CATCHER, SO IT DOES NOT WRITE PAST
       IT. The catcher and the library go to the card separately (the prx sits
       next to the EBOOT), and a new catcher with an old library would, without
       this word, put the twenty-first word PAST the array. We pass our own
       number, not a shared one: whoever writes past someone else's array is
       the one who did not ask for its length. */
    /* HOW MANY REGISTERS TO REQUEST -- THE GAME DECIDES, NOT THE LIBRARY.
       Twenty is the hardware-verified set (0x00…0x4C). The twenty-first
       (0x50) on the model 8 board killed the catcher's kernel thread in build
       573 -- silently, with no report -- so it is taken ONLY on explicit
       request and in a separate run. The full analysis is in the block's
       banner in prx/sdbg_prx.c. */
    buf[13] = (cfg->perf_regs >= 4 && cfg->perf_regs <= SDBG_PERF_N)
              ? (unsigned)cfg->perf_regs : 20u;

    rc = sceKernelStartModule(s_mod, sizeof(buf), buf, &status, NULL);
    /* The kernel does not return the number of accepted handlers into our
       buffer (it gives module_start a COPY of the arguments) -- the PRX itself
       writes it to ARMED.TXT next to the report. Here we make do with the
       start code. */
    armed = (rc >= 0) ? 1 : 0;
    SDBG_LOG("[sdbg] catcher %s: start 0x%08X, text 0x%08X+0x%X, build %d",
             armed ? "armed" : "FAILED", (unsigned)rc, base,
             (unsigned)mi.text_size, cfg->build);
    return armed;
}

int sdbg_shutdown(void)
{
    int waited = 0, n = -1, status = 0;
    if (s_mod < 0) return 0;
    s_disarm = SDBG_ASK;
    while ((s_disarm & 0xFFFFFF00u) != SDBG_DONE && waited < 1000) {
        sceKernelDelayThread(20000);
        waited += 20;
    }
    if ((s_disarm & 0xFFFFFF00u) == SDBG_DONE) n = (int)(s_disarm & 0xFFu);
    /* ONE HUNDRED MILLISECONDS BEFORE UNLOADING. The catcher's kernel thread
       deletes itself right after answering, but unloading the module with a
       thread still sleeping inside it means death after ~50 ms, and the death
       site drifts with added code (hardware 455/456). */
    sceKernelDelayThread(100000);
    sceKernelStopModule(s_mod, 0, NULL, &status, NULL);
    sceKernelUnloadModule(s_mod);
    SDBG_LOG("[sdbg] catcher released %d handlers in %d ms | %s",
             n, waited, n > 0 ? "PASS" : "FAIL");
    s_mod = -1;
    return n;
}
