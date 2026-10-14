/* crashprx/main.c -- A KERNEL MODULE THAT CATCHES A HARD CRASH.
 *
 * WHY IT EXISTS AT ALL. On hardware a hard exception takes the console down
 * silently: session.log stops mid-word, the PSP powers off, and NOT A SINGLE
 * byte about the cause is left. In one week three crashes in a row went that
 * way, all in the same scenario (dragging a car with the physics gun), and
 * there was nothing to analyse them with: memory was healthy at the time --
 * two megabytes free, zero allocation failures -- so this is not running out
 * of memory but a bad address, and that address has to be SEEN.
 *
 * WHY THIS IS A SEPARATE PRX AND NOT TEN LINES IN THE ENGINE. Because they
 * CANNOT be put in the engine, and that was learned the expensive way:
 * pspDebugInstallErrorHandler calls sceKernelRegisterDefaultExceptionHandler
 * -- an export of ExceptionManagerForKernel, available to the kernel only.
 * A module's imports are resolved by the LOADER, BEFORE the first instruction
 * runs, so a single reference to this library from a user module is enough
 * for the game not to start at all: the PSP answers "the game could not be
 * started (8002013C)" -- "library not found". pspSdkSetK1(0) fundamentally
 * cannot help here: it lifts pointer checks INSIDE an already privileged
 * call, while the failure happens at LOAD time. The full analysis is in the
 * banner of install_exception_handler in src/main.c.
 *
 * THE PATTERN IS TAKEN FROM A SIBLING PORT, NOT INVENTED: a working kernel
 * module from a sibling PSP port, with the same scaffolding
 * (BUILD_PRX, USE_KERNEL_LIBC/LIBS, psp-build-exports --build-stubs).
 * The game-side loading idiom comes from there too --
 * pspSdkLoadStartModule(path, PSP_MEMORY_PARTITION_KERNEL). Reinventing it
 * would mean spending a day on something that already works next door.
 */
#include <pspkernel.h>
#include <pspctrl.h>   /* HOME on the crash screen -- see the display loop */
#include <pspdebug.h>
#include <pspexception.h>   /* sceKernelRegisterDefaultExceptionHandler -- our own trampoline, trap.S */
#include <pspiofilemgr.h>

PSP_MODULE_INFO("sdbg", PSP_MODULE_KERNEL, 1, 0);

/* WHERE WE WRITE IS DECIDED BY THE GAME, NOT BY US (moved out into sandstone-debug).
   The path is a KERNEL one (ms0:/), not relative: the handler has no current
   directory -- it does not run in the game's context. It used to be hard-coded
   as the string ms0:/PSP/GAME/<GAME>/CRASH.TXT, and exactly that prevented taking
   the catcher into another project. Now it arrives as a start argument; if the
   game did not send one, a fallback remains -- a report in the wrong place is
   better than no report. */
#define CRASH_PATH_MAX 96
static char g_crashPath[CRASH_PATH_MAX] = "ms0:/PSP/GAME/SDBG.TXT";
#define CRASH_PATH g_crashPath

/* THE GAME'S TEXT BASE, SO THAT AN ADDRESS CAN BE FOUND IN THE SYMBOL MAP.
 *
 * WITHOUT IT THE WHOLE TOOL IS USELESS, and this is the main idea of the file.
 * A user module is loaded at an ADDRESS chosen by the system, while the game's .map (we
 * already produce it, -Wl,-Map at link time) describes addresses RELATIVE to the
 * start of text. Had we printed a bare epc, we would get a number that matches
 * nothing in the map, and "crashed at address 0x08A1B3C4" would remain as mute
 * as a dark screen. The difference epc - base is looked up in the map with a
 * plain grep.
 *
 * The game sets it during install (psrcCrashInstall): it knows its own
 * location via sceKernelQueryModuleInfo, and we do not. */
static unsigned g_textBase;

/* ---- TWO STUBS THAT CUT OFF THE DEBUG SCREEN ----
 *
 * WHAT THEY FIX. libpspdebug is built so that exception.o pulls in
 * scr_printf.o -- because it has ITS OWN fallback handler, and that one
 * prints registers to the screen. The screen needs sceGeEdramGetAddr and
 * vsnprintf, and the kernel build (USE_KERNEL_LIBC) has neither: linking
 * fails on four unresolved references.
 *
 * WHY STUBS AND NOT MORE LIBRARIES. Because we do not need the screen AT ALL:
 * our own handler writes a file, and printing anything on a dying console is
 * pointless -- the tester will see a flash and a power-off. By dragging the
 * libraries in, we would pull the whole formatted-output machinery into the
 * kernel module for code that never runs.
 *
 * HOW IT WORKS. The linker takes from an archive only the objects that
 * resolve UNresolved references. By defining these two names ourselves, we
 * resolve them earlier -- and scr_printf.o is not pulled in at all, along with
 * its dependencies. It is a well-known trick; it is written down here because
 * it looks like dead code and is the first thing that begs to be deleted.
 *
 * IF YOU DELETE THEM -- exactly that link error comes back:
 *   undefined reference to `sceGeEdramGetAddr'
 *   undefined reference to `vsnprintf' */
void pspDebugScreenInit(void) { }
void pspDebugScreenPrintf(const char *fmt, ...) { (void)fmt; }

/* Flag that the handler is installed. Exposed via psrcCrashArmed(), and this
   whole export exists for its sake: "the tool did not install" and
   "installed, but silent" look IDENTICAL on screen, yet they must be told
   apart -- this tree has already paid for that more than once. */
static int g_armed;
/* Registration return codes: [0] -- default, [4..15] -- priority ones by
   exception number. They go into ARMED.TXT: log 409 showed "accepted 0 of
   12", and only the code itself can tell "NID not resolved" (0x8002013A) from
   "priority taken" or "number out of range". */
static unsigned g_rc[16];
static unsigned g_prevDefault = 0xFFFFFFFFu;   /* v31: previous default handler (ARMED.TXT: DEF) */
static volatile unsigned g_fwBreaks;  /* WoWee: firmware asserts parked silently (psrc_trap_c) */
static unsigned g_build;                      /* v33: game build number -- into CRASH.TXT, otherwise the report cannot be matched to the game's .map */
static unsigned g_textSize = 0x300000u;       /* v33: game text size; before v33 it was hard-coded to 3 MB */
/* v33: ring of the last game log lines (prof.c, crash_tail_append) --
   read by a KERNEL thread when writing CRASH.TXT, not from the exception context. */
static const char *g_tailBuf;
static unsigned g_tailSz;
static const volatile unsigned *g_tailPos;
/* v36: stack bounds of the game's main thread -- the game sends them during install,
   the catcher uses them to extract the call chain. Analysis -- at GAME STACK in crash_watch_thread. */
static unsigned g_stackLo, g_stackHi;
void *sceKernelGetActiveDefaultExceptionHandler(void);

/* ---- HAND-ROLLED NUMBER OUTPUT, NO sprintf AND NO LIBRARY BUFFERS ----
 *
 * WHY NOT sprintf. The handler runs AFTER something has already broken: the
 * stack may have drifted, the heap may be corrupted, and libc internally uses
 * both. A function that works flawlessly in normal life has every right here
 * to bring us down a SECOND time -- and then the tool eats exactly the report
 * it was written for. An eight-digit hexadecimal number is written in four
 * lines and touches nothing else. */
/* ARMED.TXT NEXT TO THE REPORT. The report path is set by the game; the arming
   report goes into the same directory, replacing the last path segment. We
   have no sprintf of our own and must not have one (analysis -- at hex8 below),
   hence the copy and the manual tail replacement. */
static char g_armedPath[CRASH_PATH_MAX + 8];
static const char *armed_path(void)
{
    int i, cut = 0;
    for (i = 0; i < CRASH_PATH_MAX && g_crashPath[i]; i++)
        if (g_crashPath[i] == '/') cut = i + 1;
    for (i = 0; i < cut; i++) g_armedPath[i] = g_crashPath[i];
    g_armedPath[cut + 0] = 'A'; g_armedPath[cut + 1] = 'R';
    g_armedPath[cut + 2] = 'M'; g_armedPath[cut + 3] = 'E';
    g_armedPath[cut + 4] = 'D'; g_armedPath[cut + 5] = '.';
    g_armedPath[cut + 6] = 'T'; g_armedPath[cut + 7] = 'X';
    g_armedPath[cut + 8] = 'T'; g_armedPath[cut + 9] = 0;
    return g_armedPath;
}

static char *hex8(char *p, unsigned v)
{
    static const char D[] = "0123456789ABCDEF";
    int i;
    for (i = 28; i >= 0; i -= 4) *p++ = D[(v >> i) & 0xF];
    return p;
}

static char *put(char *p, const char *s)
{
    while (*s) *p++ = *s++;
    return p;
}

/* ---- THE HANDLER ITSELF ----
 *
 * WHAT WE PRINT AND WHY EXACTLY THIS:
 *   cause    -- WHICH exception (bits 2..6: 4/5 -- read/write at a bad
 *               address, 6/7 -- instruction fetch, 10 -- unknown instruction,
 *               12 -- overflow). Without it the rest cannot be read;
 *   epc      -- WHERE it crashed. This is what it was all started for;
 *   epc-base -- the same in the game's .map addresses, i.e. in function names;
 *   ra       -- WHO called. epc alone is not enough: one can crash inside
 *               memcpy, and what matters is not memcpy but its caller;
 *   badvaddr -- WHICH address was accessed. Zero here is strong evidence:
 *               a null pointer dereference;
 *   sp, gp   -- a check that the stack is intact. A drifted sp means a stack
 *               overflow, and that is a DIFFERENT disease, treated elsewhere.
 *
 * THE FILE IS WRITTEN WITH ONE sceIoWrite AND CLOSED. No appending piece by
 * piece: every extra call is one more chance to die before finishing. The
 * buffer is static; there may be no room on the stack at this moment.
 *
 * AFTER WRITING -- sceKernelExitGame. Execution cannot continue after an
 * exception, and a hung console forces pulling the power, which hurts the
 * memory card. */
static char g_buf[1536];   /* v26: memory window; v31: exc line; v33: call chain, fcsr, build */
/* Report ready to be written (v28): set by psrc_trap_c -- the exception
   context cannot write a file -- and written by the psrc_crash_watch thread,
   which has a real TCB and its own stack. */
static volatile unsigned g_reportLen;
static volatile int g_reportReady, g_reportWritten;

extern void psrc_trap_entry(void);   /* trap.S -- common trampoline entry */
extern char psrc_trap_slots[];       /* trap.S -- 16 entries of 16 bytes */

/* ---- SCREEN FIRST, FILE LATER -- AND THIS IS A TURNAROUND OF THE TOOL, NOT A TIDY-UP.
 *
 * WHAT TURNED OUT. Five hardware hangs in a row (builds 395..403, throwing a
 * car with the physics gun) -- all "totally mute": the screen froze, not a
 * single line in the log, no CRASH.TXT. For three days they were hunted in
 * the I/O layer (graceful close, a gate on card access, disabling the log
 * entirely) -- all wide of the mark. Meanwhile the emulator log held the
 * answer: 201 bad memory accesses in sessions of this game, float reads and
 * writes at NULL+4 and NULL+0xC on the main thread -- and PPSSPP swallowed
 * all of them silently (IgnoreBadMemAccess). On hardware the same access is
 * an exception, and it DID ARRIVE HERE. And here the very first thing was
 * sceIoOpen: a file call FROM EXCEPTION CONTEXT, where interrupts are off and
 * no thread runs. If the card is busy (an audio grain, a model LOD -- exactly
 * what goes on during throws), the open waits for a completion that never
 * comes. That is the "muteness": not death without an exception, but an
 * exception strangled by its own catcher. If the card is free -- file and
 * ExitGame: that very "it just closed".
 *
 * HENCE THE SCREEN NOW. The screen needs nothing alive: VRAM is memory,
 * sceDisplaySetFrameBuf is a write to display registers, both work with
 * interrupts off. Not a single library: our own 5x7 font, our own output in
 * 5650 (layout from gu.c: stride 512, 16 bits, buffer 0 at address 0x44000000).
 * The file comes AFTER, if we are lucky. And NO ExitGame: after the screen --
 * an eternal spin, so the console shows the report until the owner photographs
 * it. It will power off soon enough without our help. */
#define VRAM_BASE   0x44000000u
#define VRAM_STRIDE 512
#define SCR_W       480
#define GLYPH_W     5
#define GLYPH_H     7
#define CELL_W      12        /* 5*2 + 2 gap at scale 2 */
#define CELL_H      16        /* 7*2 + 2 */

/* A font only for what we print: digits, A-F and the label letters. A glyph
   row is 5 bits, MSB on the left. An unknown character is a space. */
static const unsigned char *glyph_of(char c)
{
    static const unsigned char G[][GLYPH_H] = {
        /* 0 */ {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
        /* 1 */ {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
        /* 2 */ {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
        /* 3 */ {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
        /* 4 */ {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
        /* 5 */ {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
        /* 6 */ {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},
        /* 7 */ {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
        /* 8 */ {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
        /* 9 */ {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
        /* A */ {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},
        /* B */ {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
        /* C */ {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
        /* D */ {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
        /* E */ {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
        /* F */ {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
        /* G */ {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},
        /* H */ {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
        /* P */ {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
        /* R */ {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
        /* S */ {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},
        /* T */ {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
        /* U */ {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
        /* V */ {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
        /* X */ {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
        /* M */ {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},
        /* N */ {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
        /* O */ {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
        /* W */ {0x11,0x11,0x11,0x15,0x15,0x1B,0x11},
        /* / */ {0x01,0x02,0x02,0x04,0x08,0x08,0x10},
        /* : */ {0x00,0x04,0x00,0x00,0x00,0x04,0x00},
        /* - */ {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},
        /*   */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    };
    static const char order[] = "0123456789ABCDEFGHPRSTUVXMNOW/:- ";
    int i;
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    for (i = 0; order[i]; i++) if (order[i] == c) return G[i];
    return G[sizeof(order) - 2];   /* space */
}

/* ---- INTO BOTH BUFFERS, WITHOUT IMPORTING THE DISPLAY DRIVER. Revisions v2/v3
   called sceDisplaySetFrameBuf (sceDisplay_driver), and both killed the
   console ALREADY AT module LOAD: logs 404 and 405 stop right after [trace],
   before the line "tool ARMED" -- that is, before any exception. The only PRX
   that loaded on this firmware imported just ExceptionManager and IoFileMgr.
   So we do not touch the display at all: we draw into BOTH ping-pong buffers
   (gu.c: 0 and 0x44000), whichever one is being shown.
   VRAM_BUF2 is the offset of the second buffer in the gu.c layout. */
#define VRAM_BUF2   0x44000u

static void scr_glyph(unsigned base, int cx, int cy, char c, unsigned short col)
{
    const unsigned char *g = glyph_of(c);
    volatile unsigned short *vram = (volatile unsigned short *)base;
    int r, k, sx, sy;
    for (r = 0; r < GLYPH_H; r++)
        for (k = 0; k < GLYPH_W; k++)
            if (g[r] & (0x10 >> k))
                for (sy = 0; sy < 2; sy++)
                    for (sx = 0; sx < 2; sx++) {
                        int x = cx * CELL_W + k * 2 + sx;
                        int y = cy * CELL_H + r * 2 + sy;
                        if (x < SCR_W) vram[y * VRAM_STRIDE + x] = col;
                    }
}

/* The report from g_buf goes to the screen: lines split on '\n', the header in
   green, the rest in white. First a black band under the text: white is not
   readable on top of a frozen frame. */
/* clear -- whether to fill the background. ONLY THE FIRST TIME (v27): redrawing
   every 100 ms with a fill produced a black window of tens of milliseconds,
   and the owner's photo (443) caught the report cut off at "STATUS 6000".
   Glyphs are redrawn every round to survive the GE; the background -- once. */
static void scr_report(const char *s, const char *end, int clear)
{
    unsigned bases[2];
    int b;
    bases[0] = VRAM_BASE;
    bases[1] = VRAM_BASE + VRAM_BUF2;
    for (b = 0; b < 2; b++) {
        volatile unsigned short *vram = (volatile unsigned short *)bases[b];
        const char *q;
        int x, y, cx = 0, cy = 0;
        unsigned short col = 0x07E0;   /* green in 5650 */
        if (clear)
            for (y = 0; y < CELL_H * 17; y++)   /* 17 rows of 16 px = the whole 272 screen */
                for (x = 0; x < SCR_W; x++) vram[y * VRAM_STRIDE + x] = 0;
        for (q = s; q < end; q++) {
            if (*q == '\n') { cx = 0; cy++; col = 0xFFFF; continue; }
            if (cx * CELL_W + CELL_W <= SCR_W) scr_glyph(bases[b], cx, cy, *q, col);
            cx++;
        }
    }
}

/* ---- C ENTRY AFTER THE TRAMPOLINE (trap.S): KERNEL MODE, INTERRUPTS ALIVE.
 *
 * WHY NOT pspDebugInstallErrorHandler. Its trampoline (exception_asm.S)
 * does eret into the C entry without touching Status: the user thread
 * returns into kernel code in user mode -- a nested exception, a firmware
 * panic, power-off. Verified with crashtest=1 on hardware: a write to zero
 * during load kills the console with no screen. Our own trampoline fixes
 * Status before eret, and we arrive here as ordinary kernel code on our own
 * stack -- from here we can draw, write a file, and WAIT via
 * sceKernelDelayThread: the SYSCON watchdog gets fed, and the console stays
 * up with the report for as long as needed. Not static: the trampoline needs
 * the name. */
void psrc_trap_c(PspDebugRegBlock *regs)
{
    char *p = g_buf;
    SceUID fd;

    /* ---- WOWEE: A FIRMWARE ASSERT -- SILENTLY. A break/trap in
       firmware text (0x88000000..0x88400000; on the PSP-3000 it is 0x88045E58,
       `bnez a3; sw; break 0`) arrives in a foreign context during load and on
       entering the world. One build drew it and parked it -- the window
       flickered, the game lived on. The next one passed it down the chain to the
       firmware -- the console died at startup (on hardware: log cut off
       after psm=5551), exactly as analysis 441 in trap.S warned. Now: the same
       parking as before, only with no screen and no report -- the firmware
       thread stays here, interrupts are enabled, the game goes on. */
    {
        unsigned code = (regs->cause >> 2) & 31u;
        unsigned pc = regs->epc + ((regs->cause & 0x80000000u) ? 4u : 0u);
        if ((code == 9u || code == 13u) && pc >= 0x88000000u && pc < 0x88400000u) {
            g_fwBreaks++;
            __asm__ __volatile__(
                "mfc0 $t0, $12\n\t"
                "ori  $t0, $t0, 1\n\t"
                "mtc0 $t0, $12\n\t"
                "nop\n\tnop\n\t" ::: "t0", "memory");
            for (;;) {
                volatile unsigned spin;
                for (spin = 0; spin < 4000000u; spin++) { }
            }
        }
    }

    /* ---- A RED BAND FIRST THING: "I AM HERE". Run 411 on throws did not
       flicker AT ALL, and from that it was impossible to tell "the handler
       was not called" from "it was called but died on its very first line".
       Four lines across the screen in both buffers -- two thousand words,
       fractions of a millisecond, and they answer that question once and
       for all. */
    {
        volatile unsigned short *a = (volatile unsigned short *)VRAM_BASE;
        volatile unsigned short *b = (volatile unsigned short *)(VRAM_BASE + VRAM_BUF2);
        int i, n = VRAM_STRIDE * 4;
        for (i = 0; i < n; i++) { a[i] = 0x001F; b[i] = 0x001F; }   /* 5650 on PSP: bits 0..4 are RED (GE UM p43); 0xF800 was blue, and the owner saw "blue on top" */
    }

    p = put(p, "WOWEE CRASH\n");  /* WoWee: was the host game's own banner */
    p = put(p, "cause    "); p = hex8(p, regs->cause);    *p++ = '\n';
    /* v31: THE EXCEPTION NUMBER AND NAME IN WORDS, AND WHICH ENTRY IT CAME THROUGH.
       Slot 0 is the default handler (DEF): that is how an FPU exception (15)
       arrives when the kernel FPU handler did not finish emulating it
       (hardware run 453). */
    {
        static const char *names[16] = {
            "INT", "TLBMOD", "TLBL", "TLBS", "ADEL", "ADES", "IBE", "DBE",
            "SYS", "BP", "RI", "CPU", "OV", "TR", "?", "FPE" };
        unsigned code = (regs->cause >> 2) & 31u;
        p = put(p, "exc      "); p = hex8(p, code); *p++ = ' ';
        p = put(p, names[code]);
        p = put(p, regs->r[26] == 0 ? " VIA DEF" : " VIA PRIO");
        *p++ = '\n';
    }
    p = put(p, "epc      "); p = hex8(p, regs->epc);      *p++ = '\n';
    p = put(p, "textbase "); p = hex8(p, g_textBase);     *p++ = '\n';
    /* THE ADDRESS OF THE CATCHER ITSELF -- TO TELL "CRASHED IN OUR CODE" FROM
       "CRASHED IN THE FIRMWARE". The very first catch (410) came with epc
       0x88045E58 -- a kernel address, and without this line it was impossible
       to say whose it was: our kernel module lives there too, at 0x88xxxxxx.
       The difference between epc and this number is looked up in
       sdbg.elf (the kernel module) the same way epc-base is looked up in the game's .map. */
    p = put(p, "modbase  "); p = hex8(p, (unsigned)psrc_trap_entry); *p++ = '\n';
    p = put(p, "epc-base "); p = hex8(p, regs->epc - g_textBase); *p++ = '\n';
    p = put(p, "ra       "); p = hex8(p, regs->r[31]);    *p++ = '\n';
    p = put(p, "ra-base  "); p = hex8(p, regs->r[31] - g_textBase); *p++ = '\n';
    p = put(p, "badvaddr "); p = hex8(p, regs->badvaddr); *p++ = '\n';
    p = put(p, "sp       "); p = hex8(p, regs->r[29]);    *p++ = '\n';
    p = put(p, "gp       "); p = hex8(p, regs->r[28]);    *p++ = '\n';
    p = put(p, "status   "); p = hex8(p, regs->status);   *p++ = '\n';

    /* CALL ARGUMENTS -- a0..a3, registers 4..7. Often they are exactly what
       names the culprit: a pointer that turned out to be zero, or an index
       that ran past a table. They cost four lines and save a round of
       guesswork. */
    p = put(p, "a0 "); p = hex8(p, regs->r[4]);
    p = put(p, " a1 "); p = hex8(p, regs->r[5]);
    p = put(p, " a2 "); p = hex8(p, regs->r[6]);
    p = put(p, " a3 "); p = hex8(p, regs->r[7]); *p++ = '\n';

    /* ---- MEMORY WINDOW: GAME ADDRESSES AROUND A2 AND SP (hardware 441/442).
       The firmware's break handler sits ahead of us at any priority: our
       break (division by zero) is taken by it first, it saves the crashed
       thread's registers in its own storage and issues ITS OWN break at
       0x88045E58 -- that is the one we catch, with a2 = 0x88220A4C right next
       to the kernel sp, i.e., by all signs, a pointer to its context block.
       The location in the game is recovered from it: we walk words from a2
       and from sp (only inside kernel RAM 0x88000000..0x88400000 or the
       game's partition), and everything that falls into the game text
       [textBase, textBase + 3 MB) is printed as offsets -- they are looked up
       in the game's .map like epc-base. The first such word from a2 is almost
       certainly the game's EPC, the next one is RA. Eight of them, two lines. */
    {
        unsigned srcs[2];
        int si, found = 0;
        srcs[0] = regs->r[6];
        srcs[1] = regs->r[29];
        /* INSTRUCTION WORDS AT EPC: which firmware break this is (code in bits
           25..6) and what precedes it -- they identify the function in a
           kernel dump. */
        {
            unsigned e = regs->epc & ~3u;
            p = put(p, "EPC W:");
            if ((e >= 0x88000000u && e < 0x883FF000u) || (e >= 0x08800000u && e < 0x09FFF000u)) {
                int k;
                for (k = -2; k <= 0; k++) { *p++ = ' '; p = hex8(p, *(volatile unsigned *)(e + (unsigned)(k * 4))); }
            } else p = put(p, " ?");
            *p++ = '\n';
        }
        /* THE FIRST WORDS AT A2 -- to see what it points to. */
        {
            unsigned a = regs->r[6] & ~3u;
            p = put(p, "A2 W:");
            if (a >= 0x88000000u && a < 0x883FF000u) {
                int k;
                for (k = 0; k < 3; k++) { *p++ = ' '; p = hex8(p, *(volatile unsigned *)(a + (unsigned)k * 4u)); }
            } else p = put(p, " ?");
            *p++ = '\n';
        }
        /* A WINDOW OF +-256 WORDS AROUND A2 AND SP (v29: 96 forward found nothing). */
        p = put(p, "GAME:");
        for (si = 0; si < 2 && found < 4; si++) {
            unsigned c = srcs[si] & ~3u;
            int i;
            if (!(c >= 0x88001000u && c < 0x883FE000u)) continue;
            for (i = -256; i < 256 && found < 4; i++) {
                unsigned w = *(volatile unsigned *)(c + (unsigned)(i * 4));
                if (w >= g_textBase && w < g_textBase + 0x300000u) {
                    *p++ = ' ';
                    *p++ = (char)(si ? 'S' : 'A');
                    p = hex8(p, w - g_textBase);
                    found++;
                }
            }
        }
        if (!found) p = put(p, " NONE");
        *p++ = '\n';
    }

    /* ---- THE CALL CHAIN. THE TECHNIQUE IS TAKEN FROM THE SDK, NOT INVENTED.
       pspsdk/src/debug/stacktrace.c: a word on the stack is treated as a
       return address if it points into the module text AND the instruction
       TWO words before it is a `jal` (top six bits 0x0C000000). Then the
       target of that `jal` is the called function, and the word itself is
       the call site. False positives are possible (stale addresses may lie
       around on the stack), but this is an order of magnitude more precise
       than the GAME window above: there any address in text qualified, here
       only one backed by a real call.
       WE PRINT PAIRS func<-call, both as the game's .map offsets: the first is
       looked up in the map as a function name, the second tells which line
       it was called from.
       WE READ ONLY WHAT IS VERIFIED. We are in exception context: a second
       exception here would eat the whole report. So every address is checked
       before reading for alignment and for falling into known regions --
       the game/kernel stack for the scan, the game text for the instruction
       before the return address. */
    {
        unsigned sp = regs->r[29] & ~3u;
        unsigned top = sp + 2048u;
        unsigned a;
        int n = 0;
        p = put(p, "CHAIN:");
        /* The game stack lives in user memory, the kernel one at 0x88xxxxxx.
           Anything else is a drifted sp, and it must not be scanned. */
        if (sp >= 0x08800000u && sp < 0x0C000000u) {
            if (top > 0x0C000000u) top = 0x0C000000u;
        } else if (sp >= 0x88001000u && sp < 0x883FE000u) {
            if (top > 0x883FE000u) top = 0x883FE000u;
        } else {
            top = sp;   /* drifted sp -- the loop will not run even once */
        }
        for (a = sp; a < top && n < 6; a += 4) {
            unsigned w = *(volatile unsigned *)a;
            unsigned ins;
            if (w < g_textBase + 8u || w >= g_textBase + g_textSize) continue;
            if (w & 3u) continue;
            ins = *(volatile unsigned *)(w - 8u);
            if ((ins & 0xFC000000u) != 0x0C000000u) continue;   /* not a jal */
            *p++ = ' ';
            p = hex8(p, ((ins & 0x03FFFFFFu) << 2) - g_textBase);   /* who was called */
            *p++ = '<';
            p = hex8(p, (w - 8u) - g_textBase);                    /* where it was called from */
            n++;
        }
        if (!n) p = put(p, " NONE");
        *p++ = '\n';
    }

    /* ---- FPU STATE -- ONLY IF IT WAS ENABLED AT ALL.
       Hardware 453: the death on entering the map was an FPU exception on a
       comparison with +inf, and the only thing that named it was psplink.
       FCSR (control register 31) holds the cause bits: division by zero,
       overflow, invalid operation -- with them inf is told apart from NaN and
       from overflow without a single guess.
       CU1 IS ALWAYS CHECKED: `cfc1` with the coprocessor disabled raises an
       exception itself (code 11), i.e. a diagnostic line would kill the
       report exactly where it is needed. Status bit 29 is the crashed
       thread's CU1. */
    if (regs->status & (1u << 29)) {
        unsigned fcsr;
        asm volatile("cfc1 %0, $31" : "=r"(fcsr));
        p = put(p, "fcsr     "); p = hex8(p, fcsr); *p++ = '\n';
    }
    /* Build number -- so the report matches that exact game .map (v33). */
    p = put(p, "build    "); p = hex8(p, g_build);
    p = put(p, " textsz "); p = hex8(p, g_textSize); *p++ = '\n';
    /* ---- WHERE THE REPORT WENT -- ON THE SCREEN, NOT ONLY IN THE FILE.
       A crash screen without the file name forces you to search for it
       blindly among a dozen CRASH.*.TXT files on the card, and to search
       LATER, when the console has already been rebooted and the time of the
       crash forgotten. The line costs next to nothing, and it removes a whole
       step of analysis. The path is the very one we are writing to right now
       (g_crashPath), not a guess about it. */
    p = put(p, "SAVED TO "); p = put(p, g_crashPath[0] ? g_crashPath : CRASH_PATH);
    *p++ = '\n';


    /* ---- THE REPORT IS HELD, NOT DRAWN ONCE. A FIX FROM A LIVE CATCH.
       The first real crash on throws (03.09) the owner described like this:
       "something black started to appear at the top, but never fully did".
       The black is our fill under the text, i.e. the handler fired and was
       drawing. It did not hold because our interrupts are alive (the
       trampoline fixes Status), the GE still held the last frame's list and
       drew the game ON TOP of the report. Who overdraws whom is a race, and
       it has to be won not by speed but by persistence: we draw in a loop,
       every hundred milliseconds, forever. The file -- on the fifth round,
       when the picture has settled: sceIoOpen may hang at that moment (card
       busy), and if it hangs it must do so AFTER the report became visible,
       not instead of it.
       Not ExitGame: it blanks the screen together with the report. Sleeping
       via the kernel yields the quantum to others -- the SYSCON watchdog gets
       fed, the console does not power itself off, and the report stays until
       someone turns it off by hand. */
    {
        int i, wrote = 0;
        /* ---- NOT A SINGLE KERNEL CALL FROM THIS CONTEXT (v28, audit C.2/C.3).
           Hardware 443/444: the report was cut off on the very first drawing
           pass, in a different place every time. We are NOT a thread here: we
           catch the firmware handler's internal break, i.e. we sit in its
           exception context; the trampoline used to enable interrupts -- the
           first one took the scheduler away, and it never came back to us.
           Hence: the trampoline leaves IE=0, the report is drawn entirely
           without interrupts (only VRAM writes), and only then IE=1 -- so the
           kernel lives, the SYSCON watchdog gets fed and our kernel thread
           psrc_crash_watch writes the file (g_report* is its signal). After
           that -- glyph redraws without a single syscall: waiting is an empty
           loop, not DelayThread. */
        (void)fd; (void)wrote;
        scr_report(g_buf, p, 1);
        g_reportLen = (unsigned)(p - g_buf);
        g_reportReady = 1;
        __asm__ __volatile__(
            "mfc0 $t0, $12\n\t"
            "ori  $t0, $t0, 1\n\t"
            "mtc0 $t0, $12\n\t"
            "nop\n\tnop\n\t" ::: "t0", "memory");
        /* ---- HOW THE SCREEN SURVIVES OTHER THREADS, SINCE WE HAVE NOTHING TO STOP THEM WITH.
           The handler holds ONLY the crashed thread. If some other thread
           crashed -- and on hardware run 492 it was a firmware thread spawned
           for the system keyboard -- the game's main thread kept drawing on
           top: the owner saw the crash window "flicker and vanish".
           sceKernelSuspendAllUserThreads suggested itself, but this SDK has no
           declaration for it, and pulling in an import for it is not allowed:
           it was exactly new imports that made the module die twice already
           at LOAD (logs 404 and 405, analysis at scr_report). The cost of a
           mistake here is not a flickering screen but having no catcher at all.
           So we use what we already have: redrawing more often than the other
           frame. Twenty milliseconds against a 50 ms game frame -- our text is
           on top in two of every three frames, i.e. readable. The background
           is refreshed rarely (every half second): filling it every round
           means a black window of tens of milliseconds and a cut-off report
           in the photo, which we already paid for in 443. */
        /* ---- EXIT TO XMB ON HOME. The owner's request, and a correct one.
         *
         * WHAT IT WAS. The crash screen hangs forever, and the only way out is
         * holding the power button. Yet the console is ALIVE: the report has
         * already been written to the card, the kernel works, there is no
         * reason for it to go dark. Making a person yank the power where a
         * button press would do is bad form, and a hard power-off on a memory
         * card sometimes costs the file system.
         *
         * WHY IT IS SAFE EXACTLY HERE. By this moment the report is WRITTEN
         * and closed (g_reportWritten above), so there is nothing to lose. The
         * pad is read with the kernel sceCtrlPeekBufferPositive -- it does not
         * wait or block, unlike Read; nothing may wait in an exception handler.
         *
         * WHY HOME AND NOT ANY BUTTON. The crash screen is read by eye, and
         * read for a long time -- registers get copied down, addresses
         * compared. Leaving on an accidental press would take away from the
         * owner exactly the numbers the screen is drawn for. HOME means
         * nothing else on this screen: there is no system menu inside an
         * exception handler anyway. */
        for (i = 0; ; i++) {
            volatile unsigned spin;
            SceCtrlData pad;
            if (sceCtrlPeekBufferPositive(&pad, 1) > 0 &&
                (pad.Buttons & PSP_CTRL_HOME)) {
                /* We do not write a line into the report: the file is closed,
                   and reopening it for one line is I/O in an exception
                   handler, i.e. a new risk exactly where we have already
                   crashed once. */
                sceKernelExitGame();
            }
            /* MORE OFTEN THAN BEFORE. A hundred milliseconds is five frames of
               a game drawing on top; if stopping the threads failed, nothing
               of the report is left on screen within that time. Twenty
               milliseconds beat even a 20 Hz frame. */
            for (spin = 0; spin < 4000000u; spin++) { }   /* ~20 ms at 333 MHz */
            /* Every 25th round -- with the background: other frames litter the
               area around the letters within half a second, while a solid
               fill every time causes flicker. */
            scr_report(g_buf, p, (i % 25) == 24);
        }
    }
}

/* ---- INSTALL, CALLED BY THE GAME ----
 *
 * THE TEXT BASE IS PASSED BY THE GAME TOO, as an argument. We cannot ask for
 * it from here: the kernel module does not know which of the loaded modules
 * is the game, while the game knows its own location for free
 * (sceKernelQueryModuleInfo on itself). */
/* Entry #i: two zero words for the kernel list, then a jump into the common
   trampoline. The stride is set in trap.S, the analysis is there too. */
#define TRAP_SLOT(i) ((void *)(((unsigned)psrc_trap_slots + (unsigned)(i) * 16u) \
                               | 0x80000000u))

int psrcCrashInstall(unsigned textBase)
{
    int exno, got = 0;

    g_textBase = textBase;
    /* Our own trampoline instead of pspDebugInstallErrorHandler -- analysis
       at psrc_trap_c. The top bit is the cached kernel alias: exactly how
       the SDK registers its own entry (exception.o: or a0, 0x80000000).

       ---- NOT ONLY "DEFAULT", BUT A PRIORITY HANDLER FOR EVERY NUMBER ----
       Four revisions of the catcher (file, screen, own trampoline, no
       imports) and three crashtest=1 test crashes -- control never once
       arrived here: the console dies on the write to zero, the screen is
       empty. The kernel calls the "default" handler only for numbers that
       have no handler OF THEIR OWN, and the firmware has its own for address
       errors (4/5) and others -- it is the firmware that kills the console.
       That is why psplink registers PRIORITY handlers by name: priority 1
       puts us in front of the firmware's own. Numbers are MIPS ExcCode: 4/5
       read/write address, 6/7 bus error, 8 syscall (not ours -- skipped),
       9 break, 10 unknown instruction, 11 coprocessor unusable, 12 overflow,
       13 trap, 15 FPE. One success is enough: g_armed = "armed at least
       somewhere", and a refusal on an individual number is no reason to
       stay silent. */
    /* OUR OWN ENTRY FOR EACH NUMBER -- otherwise a second registration of the
       same address returns EXPHANDLER_USED (analysis in trap.S). Slot 0 goes
       to "default", slots 1..12 to numbers 4..15. */
    /* ---- NUMBERS 1..3 (TLB) ARE OURS TOO, AND THIS IS A FIX FROM AN EMPTY CATCH.
       Run 411 on throws: the log cut off at 33.19 s, the screen did NOT
       flicker at all -- so the handler was not called, i.e. the exception
       was not one of the nine subscribed. On MIPS dereferencing a wild or
       null pointer gives not an address error but a TLB MISS: 2 -- on read,
       3 -- on write, 1 -- write to a protected page. Those were missing.
       THE PRIORITY HERE IS LOW (from 2), unlike for real faults: the TLB is
       a working memory mechanism, and jumping ahead of the firmware on it
       would repeat the story of the break trap that hung the load. */
    for (exno = 1; exno <= 3; exno++) {
        int rc, prio;
        rc = -1;
        for (prio = 2; prio <= 3 && rc < 0; prio++)
            rc = sceKernelRegisterPriorityExceptionHandler(exno, prio,
                                                           TRAP_SLOT(exno + 12));
        g_rc[exno] = (unsigned)rc;
        if (rc >= 0) got++;
    }
    for (exno = 4; exno <= 15; exno++) {
        int rc, prio;
        /* WE DO NOT TOUCH THE KERNEL'S REGULAR TRAPS -- A FIX FROM THE VERY FIRST CATCH.
           Run 410 with the full list caught cause 0x10000024: ExcCode 9,
           breakpoint, EPC 0x88045E58 -- a KERNEL address with a user gp.
           That is not our write to zero (it would have given code 5 with epc
           in the game): the firmware intercepted it with its own handler and
           issued a break inside it, and we were sitting on nine as well.
           8 (syscall), 9 (break) and 13 (trap) are working mechanisms of the
           kernel and CFW (kuKernelCall lives through them), and hooking them
           means breaking the console in places where nothing is broken. What
           remains are the REAL faults: 4/5 address, 6/7 bus, 10 unknown
           instruction, 11 coprocessor, 12 overflow, 14/15 -- those are what
           we hunt for. */
        /* Since v23 -- 9 (break) AND 13 (trap) ARE OURS TOO: the filter in the
           trampoline (trap.S) passes other firmware traps on down the chain,
           while a break with code 7/6 in the game text (division by zero after
           div -- psp-gcc puts one after every division, fourteen in b3Solve)
           becomes a report. Without this int/0 went into a kernel loop and
           killed the console. 8 (syscall) -- no: every kernel call lives
           through it. */
        if (exno == 8) { g_rc[exno] = 0xFFFFFFFFu; continue; }
        /* 9 AND 13 -- AT THE HEAD (priority 0), LIKE THE REST: only as the
           first handler do we see the GAME's EPC. At the tail (v24, hardware
           441) only the firmware's internal break 0x88045E58 got through --
           a report, but no location. Other breaks (kubridge, KDebug) are
           passed on down the chain by the trampoline filter; ours (code 7/6
           in the game text) and any in kernel text go into the report. Loop
           440 was caused not by the chain but by the break code (7 << 10),
           analysis in trap.S. */
        /* Priority 0 FIRST: the firmware has its own handlers for address
           errors, and "before it" is the only place where we see the game's
           epc rather than its own break. If taken -- try 1..3. */
        rc = -1;
        for (prio = 0; prio <= 3 && rc < 0; prio++)
            rc = sceKernelRegisterPriorityExceptionHandler(exno, prio,
                                                           TRAP_SLOT(exno - 3));
        g_rc[exno] = (unsigned)rc;
        if (rc >= 0) got++;
    }
    /* ---- THE "DEFAULT" HANDLER IS NOT INSTALLED, AND THIS IS THE MAIN FIX.
       It is called for numbers NOBODY subscribed to -- that is, exactly for
       the firmware's working traps. kuKernelCall from kubridge lives through
       break (code 9), and the game calls it during load ([home], step 3/4).
       Two runs in a row the catcher printed the same thing: cause
       0x10000024 (break), epc 0x88045E58 -- an address IN THE FIRMWARE,
       below our modbase -- and the console froze. The crashes during load
       came from the tool itself: without it the game loads, with it it does
       not. We catch only what is subscribed by name above; other traps go
       their own way. */
    /* ---- v32: THE "DEFAULT" HANDLER IS AGAIN NOT INSTALLED (hardware 458).
       v31 installed it for the sake of FPE and THE LOAD DIED: the game armed
       15 handlers (ARMED 0000000F) and went dark right after -- before
       gu_init, before the paks, before crashtest. The answer is the same as
       in the old banner, which we removed in vain: default is called for
       what NOBODY took, and during load the firmware lives on it (its own
       default 0x88021574 -- DEF prev in ARMED.TXT). By taking it without
       passing down the chain, we stole the firmware's regular path, and its
       very first exception during load went into our slot 0 -- no band (the
       context is not ours), the console goes dark. psplink can do this
       because the game runs UNDER it and it owns the exceptions; for us the
       owner is the firmware, and default cannot be taken from it without a
       chain. FPE (code 15) is caught by the priority handler on 15 (it is
       installed anyway, RC15=0), and the REAL fix for FPE is to remove inf
       from Box3D, which was done in 454 (a finite threshold, sentinels
       +-FLT_MAX): infinity is no longer born in the game, there is nothing
       to catch. A chain to g_prevDefault is left for the future as a
       separate careful task. */
    /* The previous default is only READ (an operation with no side effects):
       its address is needed by that future chain task, and it also answers
       the question "was there one at all" -- the DEF prev line in ARMED.TXT. */
    g_prevDefault = (unsigned)sceKernelGetActiveDefaultExceptionHandler();
    g_rc[0] = 0xFFFFFFFFu;   /* default is NOT registered -- visible in ARMED.TXT (RC00) */
    g_armed = got;
    return g_armed;
}

int psrcCrashArmed(void)
{
    return g_armed;
}

/* ---- TWO SELF-TESTS -- TO SPLIT THE SILENCE INTO LAYERS.
 *
 * Five revisions of the catcher and four crashtest=1 crashes -- not a single
 * line on screen, ever. The silence is one, but there are three layers it
 * can live in: the reporter itself (VRAM, font), the trampoline (trap.S:
 * saving, Status, eret), and the kernel registration (does a user exception
 * reach us at all). These two functions separate the layers from each other
 * with ONE run each, and the game calls them via the crashtest key (main.c):
 *   2 -- psrcCrashSelfTest: the reporter directly, with no exception and no
 *        trampoline. Green screen -- the reporter is healthy; none -- it is
 *        the broken one.
 *   3 -- psrcCrashKernelFault: a write to zero FROM THE KERNEL (we are a
 *        kernel module). Green -- the trampoline and registration work at
 *        least for kernel exceptions, and all that remains is "user ones do
 *        not reach us"; none -- the trampoline or the registration is at fault.
 * Neither returns: the reporter sits in an eternal spin by construction. */
static PspDebugRegBlock g_fake;

void psrcCrashSelfTest(void)
{
    int i;
    for (i = 0; i < 32; i++) g_fake.r[i] = 0xA0000000u + (unsigned)i;
    g_fake.cause    = 0x5E1F7E57u;
    g_fake.epc      = g_textBase + 0x1234u;
    g_fake.badvaddr = 0x0BADBADDu;
    g_fake.status   = 0x0000FF11u;
    psrc_trap_c(&g_fake);
}

void psrcCrashKernelFault(void)
{
    volatile int *p = (volatile int *)0;
    *p = 0xDEAD;
}

/* module_start DOES NOT INSTALL THE HANDLER, and this is deliberate: until the
   game calls us we do not know the text base, and a handler without it would
   print an address that cannot be found in the map. Between module load and
   the game's first line there is a window of a fraction of a second -- not
   worth losing the meaning of the report for. */
/* ---- INSTALL -- IN module_start, VIA THE START ARGUMENT, WITH NO CALLS FROM THE GAME.
 *
 * WHAT TURNED OUT (log 408): "handlers accepted -2147352262" =
 * 0x8002013A = "library not found" -- the code returned by an UNRESOLVED
 * import stub. The user module (the game) imports the psrcCrash library,
 * and that lives in a kernel PRX loaded LATER than the game: the loader left
 * the stubs empty, and not one call to psrcCrashInstall in the whole history
 * of the tool ever reached this file. The game printed "ARMED" based on the
 * truthiness of a negative error code. Hence "found nothing twice" in 381,
 * and six silent hangs: there never was a handler.
 *
 * SO NOW EVERYTHING IS HERE. The game calls sceKernelStartModule with a
 * buffer of three words: [0] the game's text base (for epc-base), [1] where
 * we write the number of accepted handlers, [2] the crashtest self-test mode
 * (2 -- the reporter directly, 3 -- a fault from the kernel, otherwise
 * nothing). The buffer lives in the game's memory, and the kernel module
 * writes to it freely. The return is ALWAYS 0: a non-zero code would mean to
 * the kernel "the module does not stay resident", and the handlers would
 * point into unloaded memory. */
/* ---- DISARM VIA A WORD IN GAME MEMORY -- OUR OWN THREAD IN THE KERNEL (hardware 433..435).
 * Three ways of asking the kernel to remove our handlers did not work:
 * sceKernelStopModule returns 0, but module_stop does not run;
 * sctrlHENFindFunction does not find our export; importing from the PRX is
 * forbidden to the game by the loader. So there is no point asking the
 * kernel: the catcher waits for the command itself. The game passes the
 * ADDRESS of its word as the fourth start word (a[3]); a sleeping kernel
 * thread reads it every 50 ms (the kernel can access all of user memory),
 * and on seeing CRASH_DISARM_ASK it removes the handlers (crash_release_all,
 * report in ARMED.TXT) and writes CRASH_DISARM_DONE(n) into the same word.
 * The game waits up to half a second for the answer and only then stops,
 * unloads and exits. The thread sleeps, holds nothing and dies after the
 * very first answer. */
#define CRASH_DISARM_ASK  0x5E7A5C11u      /* does NOT match DONE under mask 0xFFFFFF00 (436: it did, the game did not wait) */
#define CRASH_DISARM_DONE 0xD15A2A00u      /* + number removed in the low byte */
static volatile unsigned *g_disarmWord;
static unsigned g_watchRc = 0xFFFFFFFFu, g_watchStartRc = 0xFFFFFFFFu;   /* in ARMED.TXT as the WATCH line */
static int crash_release_all(void);        /* defined below, near module_stop */

/* ================= PSP HARDWARE COUNTERS =================
 *
 * WHAT THIS IS AND HOW IT DIFFERS FROM THE TRACER. psptrace answers the
 * question "what happened and when": frame zones on a timeline. These
 * counters answer another one -- "WHY is it slow": instruction and data
 * cache misses, cache write-backs, VFPU and FPU cycles, bus load. The tracer
 * will show that world rendering ate the frame; the counters will say it ate
 * it on cache misses, not on arithmetic. Neither replaces the other.
 *
 * WHY HERE AND NOT IN THE LIBRARY. 0xBC400000 are hardware registers
 * accessible only to the kernel: the SDK sample (samples/debug/profiler) is
 * declared PSP_MODULE_INFO(..., 0x1000, ...), i.e. as a kernel module. From
 * user code this read does not go through.
 *
 * HOW WE SAMPLE. The catcher's kernel thread already wakes up every 50 ms --
 * it also takes the window: read twenty registers into a shared block, zero
 * them, bump the snapshot number. The game reads the block whenever it
 * wants; each snapshot is exactly the last 50 ms. Zeroing is mandatory:
 * cpuck at 333 MHz overflows 32 bits in thirteen seconds, and without a
 * reset the numbers become lies after half a minute of running.
 *
 * ZEROING -- AFTER READING, NOT BEFORE. Between the read and the reset a
 * handful of cycles is lost; between a reset and the read the whole window
 * would be lost. */
/* TWENTY-ONE WORDS, NOT TWENTY -- CHANGE OF 15.09.2026, AND HERE IS WHY.
 * The block layout is described in TWO documents, and they differ by exactly
 * one register: the SDK's `pspdebug.h` declares enable + 19 counters (20
 * words), while PSPTEK `#prof` describes 21 words, 0x00…0x50 -- its extra one
 * is `CNTSTALL` ("total stalls") at 0x0C. While we read 20, the last word
 * (0x50) was not sampled AT ALL: if PSPTEK is right, we were losing
 * `CNTLOCALBUS`, and all labels from the fourth onward were shifted by one
 * register, i.e. "D-cache misses" would actually be I-cache misses.
 *
 * The dispute is settled not by reading documents but by calibration
 * (perfcal.c in the host game): loads with known behaviour, and the counter
 * that moves under them is the one we are looking for. But there is nothing
 * to calibrate while the last word is not read -- hence 21.
 *
 * READING 0x50 ON HARDWARE IS SAFE: both documents describe the block at
 * least up to 0x4C, PSPTEK up to 0x50, and these are registers of one device.
 * On the emulator we never get here at all: the library hands out a null
 * block pointer when it has recognised PPSSPP (analysis at perf_hw_present
 * in sdbg.c).
 *
 * THE MAGIC CHANGED TOGETHER WITH THE COUNT, AND THIS IS NOT COSMETIC. There
 * are TWO artifacts on the card, and they ship separately: the EBOOT carries
 * the library, sdbg.prx the kernel half. An old catcher with a new library
 * would fill 20 words out of 21, and the twenty-first would stay zero -- i.e.
 * the tool would SILENTLY report "counter 0x50 is dead" instead of "nobody
 * sampled it". The new magic makes such a pair honestly non-working: the
 * library does not recognise the block and says so out loud. The reverse
 * pair (new catcher, old library) is guarded by the capacity from the
 * argument -- see g_perfN below. */
/* ⚠ TWENTY-ONE WORDS COST A LIVE RUN -- READ THIS BEFORE CHANGING THE NUMBER.
 * A test run on a PSP-3000 (board model 8): the catcher took the block (the magic
 * is set, sdbg_perf_ready returned one), but `seq` did NOT grow EVEN ONCE in
 * 46 seconds. There was no crash report and the game lived until exit --
 * i.e. the kernel thread died SILENTLY on its very first tick, and the
 * firmware simply removed it alone. The only thing that changed in it
 * compared to the previous, working build was the access to word 0x50.
 *
 * SO THE DEFAULT WENT BACK TO TWENTY, and the twenty-first is requested
 * ONLY on the game's explicit request (key perfreg21). Twenty is enough for
 * the task this was all started for: both compared layouts put all five
 * wanted counters within 0..19 (SDK 13,14,15,17,18; PSPTEK 14,15,16,18,19).
 * So 0x50 is curiosity, not necessity, and paying for it with a dead tool
 * is not acceptable.
 *
 * WHAT 0x50 REALLY MEANS, WE DO NOT KNOW. PSPTEK calls it CNTLOCALBUS; our
 * board answers an access to it with the death of the thread. This is an
 * argument AGAINST the PSPTEK map, but not proof: the register might be
 * read-only, might not exist on later boards, might require a different
 * access order. The `step` field below will find out -- on explicit request
 * and in a separate run.
 *
 * THE MAGIC CHANGED A THIRD TIME, because the block layout is different again
 * (`step` was added). The rule is the same: the artifacts ship to the card
 * separately, and a mixed pair must be honestly non-working, not silently
 * wrong. */
#define SDBG_PERF_BASE  0xBC400000u
#define SDBG_PERF_N     21            /* ceiling: 0x00…0x50 inclusive */
#define SDBG_PERF_SAFE  20            /* verified on hardware: 0x00…0x4C */
#define SDBG_PERF_MAGIC 0x53504534u   /* 'SPE4' -- recognised by the library */

typedef struct {
    volatile unsigned magic;   /* SDBG_PERF_MAGIC once the block is accepted */
    volatile unsigned on;      /* 1 -- sample; set by the game */
    volatile unsigned seq;     /* snapshot number; grows every window */
    volatile unsigned us;      /* window length in microseconds */
    /* A TRACE OF THE LAST ACTION ON THE REGISTERS -- THE ONLY WAY TO LEARN
       WHERE THE KERNEL THREAD DIED. The firmware removes such a thread with
       no report, and from the game's side "died zeroing register 0x50" and
       "died somewhere else entirely" look the same -- like nothing. Written
       BEFORE each access: 0x100+i -- about to zero regs[i], 0x200+i -- about
       to read regs[i], 0x300 -- the whole window is taken. If the number
       freezes at 0x114 -- the twentieth register is to blame, without a
       single guess. */
    volatile unsigned step;
    volatile unsigned n;       /* how many words the catcher actually fills */
    /* ADDRESS OF THE KERNEL READER -- FOR THOSE WHO FIND A 50 MS WINDOW TOO COARSE.
       The catcher thread samples the counters twenty times a second; a frame
       phase lasts one to fifteen milliseconds, i.e. one window contains the
       whole frame and all its phases mixed together. To ask "what did this
       phase get stuck on", a synchronous sample AT THE PHASE BOUNDARY is
       needed -- i.e. a call into the kernel right from the frame.
       WHY A POINTER AND NOT AN EXPORT. An export would have to be looked up
       by NID via sctrlHENFindFunction, i.e. we would add a third way to reach
       this module and a third place where it would one day drift apart from
       the rest. The module already publishes itself into this block -- let it
       publish the address too. The game calls it via kuKernelCall (the same
       bridge as in home.c), and it needs to know neither a name nor a NID.
       Zero here is a legitimate answer "there is no door": the catcher is
       old or did not come up. */
    volatile unsigned readfn;
    volatile unsigned reg[SDBG_PERF_N];
} sdbg_perf_block;


static sdbg_perf_block *g_perf;
static int g_perfOn;
static unsigned g_perfLast;
/* HOW MANY WORDS THE LIBRARY TAKES -- ITS NUMBER, NOT OURS. We write into ITS
   memory, and only it has the right to decide how much room there is.
   Twenty is the default for a library that does not know this field yet
   (fewer arguments arrived): its block is exactly that size. Without this
   field a new catcher with an old library would write the twenty-first word
   PAST the array -- corrupting four bytes of someone else's memory every
   50 ms, which would be hunted for anywhere but the catcher.
   The default is SDBG_PERF_SAFE, not the ceiling: see the block banner, 0x50
   kills this thread on our board. */
static unsigned g_perfN = SDBG_PERF_SAFE;
/* Previous register snapshot -- the window is computed as a difference, not
   by zeroing. Analysis -- in the "TWO OWNERS" banner in perf_tick. */
static unsigned g_prev[SDBG_PERF_N];

/* READER FOR SYNCHRONOUS SAMPLING. Called FROM THE game's FRAME via
   kuKernelCall, i.e. on the hot path -- so there is nothing here but the
   read loop. No log, no zeroing: zeroing between phases is NOT ALLOWED (the
   counters are free-running, and the difference of two snapshots is exactly
   the work between them), and an overflow within a frame does not happen:
   cpuck at 333 MHz overflows 32 bits in thirteen seconds.
   ⚠ n IS CLAMPED BY THE SAME NUMBER as the background sampling: word 0x50 on
   our board has already killed the kernel thread once, and repeating that on
   the frame's hot path would kill the game. Analysis -- in the block banner
   above. */
static int perf_read_k(unsigned *dst, int n)
{
    volatile unsigned *r = (volatile unsigned *)SDBG_PERF_BASE;
    int i;
    if (!dst || n <= 0) return 0;
    if ((unsigned)n > g_perfN) n = (int)g_perfN;
    for (i = 0; i < n; i++) dst[i] = r[i];
    return n;
}

static void perf_tick(void)
{
    volatile unsigned *r = (volatile unsigned *)SDBG_PERF_BASE;
    unsigned now, i;
    if (!g_perf) return;
    if (!g_perf->on) {
        if (g_perfOn) { r[0] = 0; g_perfOn = 0; }   /* clear PROFEN, do NOT touch the registers */
        return;
    }
    if (!g_perfOn) {
        /* ENABLE AND REMEMBER, DO NOT ZERO. Analysis -- in the block banner
           ("TWO OWNERS"). Order: PROFEN first, then the snapshot -- if writing
           PROFEN itself resets the counters, the snapshot will see it, while
           the reverse order would make the first window garbage. */
        r[0] = 1;
        for (i = 0; i < g_perfN; i++) g_prev[i] = r[i];
        g_perf->step = 0x100u;          /* "counting armed" -- the game waits for this */
        g_perfOn = 1;
        g_perfLast = (unsigned)sceKernelGetSystemTimeLow();
        return;
    }
    now = (unsigned)sceKernelGetSystemTimeLow();
    /* ============ TWO OWNERS OF THE SAME REGISTERS ============
     *
     * ZEROING USED TO BE HERE, AND IT POISONED THE OTHER READER. This thread
     * took the window and zeroed the counters; while there was one reader,
     * everything added up. With the arrival of per-frame-phase sampling
     * (perfzone.c) there were TWO readers, and they count differently: that
     * one takes the difference of its two snapshots at the phase boundaries.
     * Zeroing between them gives now < snap, the unsigned difference turns
     * into four billion, and garbage lands in the phase's accumulator -- the
     * SAME in all counters at once.
     *
     * ON SCREEN IT LOOKED CONVINCING, AND THAT IS THE WORST PART OF SUCH A BUG:
     * run 578 printed neat `stallmem=997 imiss=993 dmiss=1986` -- i.e. a ratio
     * of exactly one for everything, which does not exist in nature (a cache
     * miss on every cycle). The numbers were plausible in form and
     * meaningless in substance.
     *
     * SO WE NO LONGER TOUCH THE REGISTERS AT ALL, except for the single PROFEN
     * write on enable. We compute the window OURSELVES -- as a difference from
     * our previous snapshot; into the block goes exactly what went there
     * before, so the [perf] line and the calibration do not notice the change.
     * There is no overflow within 50 ms (cpuck at 333 MHz lasts thirteen
     * seconds), and the unsigned difference is correct across wrap-around too.
     *
     * RULE FOR THE FUTURE: a destructive operation on a shared resource may
     * be performed only by its SOLE owner. Once a second reader appears,
     * there is no owner any more, and the zeroing has to go. */
    for (i = 0; i < g_perfN; i++) {
        unsigned cur;
        g_perf->step = 0x200u + i;
        cur = r[i];
        g_perf->reg[i] = cur - g_prev[i];
        g_prev[i] = cur;
    }
    g_perf->step = 0x300u;
    g_perf->us = now - g_perfLast;
    g_perfLast = now;
    g_perf->seq++;
}

static int crash_watch_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    for (;;) {
        sceKernelDelayThread(50000);
        perf_tick();
        if (g_reportReady && !g_reportWritten) {
            SceUID fd;
            g_reportWritten = 1;
            fd = sceIoOpen(CRASH_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
            if (fd >= 0) {
                sceIoWrite(fd, g_buf, (SceSize)g_reportLen);
                /* ---- THE GAME LOG TAIL (v33). HERE AND NOT IN THE REPORT: we
                   are a real thread with a real stack, and psrc_trap_c is not.
                   The ring is read by a monotonic counter: if it is less than
                   the size, the ring has not wrapped yet and reading starts
                   from zero; otherwise -- from (pos & (sz-1)) around the ring,
                   i.e. from the oldest surviving line to the freshest. The
                   first line is almost always cut in the middle -- that is
                   fine and cheaper than searching for a newline. */
                if (g_tailBuf && g_tailPos && g_tailSz) {
                    unsigned pos = *g_tailPos;
                    unsigned n = (pos < g_tailSz) ? pos : g_tailSz;
                    unsigned start = (pos < g_tailSz) ? 0u : (pos & (g_tailSz - 1u));
                    unsigned first = g_tailSz - start;
                    sceIoWrite(fd, "\n---- LOG TAIL ----\n", 20);
                    if (first > n) first = n;
                    sceIoWrite(fd, g_tailBuf + start, (SceSize)first);
                    if (n > first) sceIoWrite(fd, g_tailBuf, (SceSize)(n - first));
                }
                sceIoClose(fd);
                /* Flush the card cache: the death on exit (455) showed that a
                   closed file may not survive the death of the kernel. */
                sceIoSync("ms0:", 0);
            }
            /* ---- RAW MEMORY AROUND THE FIRMWARE CONTEXT (v34).
             *
             * WHY. Hardware 461, crashtest=1: the game writes to zero, and the
             * report names the firmware break 0x88045E58 -- its own signal
             * "a user thread crashed". By this point the firmware has already
             * saved the game's real context (its EPC, ra, stack) IN ITS OWN
             * storage, and the +-256 word window around a2/sp did not find it:
             * GAME NONE. Guessing at which offset it lies costs more than
             * taking a memory dump ONCE and looking at it: the dump is
             * analysed on the PC (tools/crashmem.py), and the offset found is
             * then baked into the report as one line.
             *
             * WHAT WE TAKE. Three 4 KB windows, page-aligned: around a1 and
             * a2 (the arguments of the firmware break -- by all signs,
             * pointers to its structures) and around the kernel sp. A
             * six-word header says what lies where, otherwise the dump is
             * unreadable.
             *
             * WHY IT IS SAFE. We are an ordinary kernel thread, not an
             * exception context; every address is checked to lie in kernel
             * RAM before any read. The file is written AFTER CRASH.TXT: if
             * the card no longer responds, the dump is lost, not the report. */
            /* ---- THE GAME STACK -- THE ONLY PLACE WHERE THE TRAIL IS INTACT (v36).
             *
             * WHAT THREE RUNS SHOWED. 461: our priority handlers receive not
             * the game's fault but the firmware's OWN break (0x88045E58) --
             * it intercepts the user thread's crash first. 462: three 4 KB
             * windows around a1/a2/kernel sp -- not a single game code
             * address (two words in the whole dump, and those below the text
             * base). 463: a search across ALL kernel memory gave eighteen
             * candidates, and not one made sense -- the symbolicator produced
             * `module_info+0x2963` and the like. The conclusion is firm: the
             * game's context cannot be reached this way.
             *
             * BUT THE GAME STACK IS INTACT. The thread is frozen inside the
             * handler, nobody touched its frames, and they hold the return
             * addresses of the whole call chain -- from main to the crash
             * site. The game knows its stack bounds
             * (sceKernelReferThreadStatus on itself) and passes them during
             * install; the catcher walks them with the SDK rule: a word
             * qualifies if it points into the game text AND the instruction
             * two words before it is a `jal`.
             *
             * WE PRINT BOTTOM UP, i.e. from the deepest frames to the
             * earliest: on MIPS the stack grows down, and the crash site lies
             * at the LOWER bound. Below the current sp there is leftover
             * garbage from past calls -- some lines will be false, and that
             * is more honest than trying to guess sp: the names in
             * symbolicate.py will immediately show which chain makes sense.
             *
             * WE READ SAFELY: we are an ordinary kernel thread, the bounds
             * came from the game and were checked against the user range. */
            if (g_stackLo && g_stackHi > g_stackLo) {
                /* APPENDED TO THE REPORT ITSELF, NOT TO A SEPARATE FILE (v37):
                   hardware 464 proved the chain is real (at the bottom
                   main <- _start, above it prof_logf from main -- exactly the
                   line the game wrote before dying). Keeping it separate
                   would mean stitching two files by hand every time; one
                   artifact -- one road to the answer. */
                SceUID f3 = sceIoOpen(CRASH_PATH, PSP_O_WRONLY | PSP_O_APPEND, 0777);
                if (f3 >= 0) {
                    char *q = g_buf;   /* the report is already on the card -- the buffer is free */
                    unsigned a;
                    int shown = 0;
                    q = put(q, "\n---- GAME STACK CHAIN ----\nGAME STACK ");
                    q = hex8(q, g_stackLo); *q++ = ' ';
                    q = hex8(q, g_stackHi);
                    q = put(q, " text "); q = hex8(q, g_textBase); *q++ = ' ';
                    q = hex8(q, g_textBase + g_textSize); *q++ = 0x0A;
                    for (a = g_stackLo; a < g_stackHi && shown < 40; a += 4) {
                        unsigned w = *(volatile unsigned *)a;
                        unsigned ins;
                        if (w < g_textBase + 8u || w >= g_textBase + g_textSize) continue;
                        if (w & 3u) continue;
                        ins = *(volatile unsigned *)(w - 8u);
                        if ((ins & 0xFC000000u) != 0x0C000000u) continue;   /* not a jal */
                        q = hex8(q, a); *q++ = ' ';
                        q = hex8(q, ((ins & 0x03FFFFFFu) << 2) - g_textBase);
                        *q++ = '<';
                        q = hex8(q, (w - 8u) - g_textBase);
                        *q++ = 0x0A;
                        shown++;
                    }
                    if (!shown) q = put(q, "NONE\n");
                    sceIoWrite(f3, g_buf, (SceSize)(q - g_buf));
                    sceIoClose(f3);
                    sceIoSync("ms0:", 0);
                }
            }
        }
        if (g_disarmWord && *g_disarmWord == CRASH_DISARM_ASK) {
            int n = crash_release_all();
            /* THE THREAD LEAVES BY ITSELF, AND THIS IS THE EXIT FIX (hardware
               455/456, construct: 3 squares and death in ExitGame, then 1
               square and death in prof_shutdown -- the spot drifts with added
               IO, so it is a time fuse of ~50 ms). On getting the answer the
               game calls sceKernelUnloadModule; the kernel does not run
               module_stop for us (433), and this thread kept sleeping in
               sceKernelDelayThread INSIDE the unloaded module -- 50 ms later
               it woke up in freed memory. The answer is written LAST, and
               after it the thread deletes itself; the game waits 100 ms
               before unloading. */
            *g_disarmWord = CRASH_DISARM_DONE | ((unsigned)n & 0xFFu);
            g_disarmWord = 0;
            sceKernelExitDeleteThread(0);
        }
    }
    return 0;
}

int module_start(SceSize args, void *argp)
{
    unsigned *a = (unsigned *)argp;
    unsigned mode = 0;

    if (args >= 12 && a) {
        g_textBase = a[0];
        mode = a[2];
        /* v33: build number and text size -- optional words, an old game
           does not send them. Default size 3 MB (as it was hard-coded). */
        if (args >= 20) g_build = a[4];
        if (args >= 24 && a[5] >= 0x10000u && a[5] <= 0x800000u) g_textSize = a[5];
        /* v33: ring of the last game log lines -- appended to CRASH.TXT by
           the kernel thread. The size must be a power of two (the ring is
           addressed by a mask) and sane; otherwise we do not take it. */
        if (args >= 36 && a[6] && a[8] && a[7] >= 256u && a[7] <= 65536u
            && (a[7] & (a[7] - 1u)) == 0u) {
            g_tailBuf = (const char *)a[6];
            g_tailSz  = a[7];
            g_tailPos = (const volatile unsigned *)a[8];
        }
        /* Counter block: the game allocates it on its side and asks for
           sampling by setting the on field. We check the magic so that a
           random word does not become a pointer we will write through. */
        if (args >= 52 && a[12]) {
            sdbg_perf_block *pb = (sdbg_perf_block *)a[12];
            /* THE CAPACITY OF SOMEONE ELSE'S ARRAY -- FROM THE NEXT WORD, AND
               ONLY IF IT ARRIVED. A library older than this change does not
               send the argument (args == 52), and then its block is exactly
               twenty words -- we write twenty. Upper clamp at SDBG_PERF_N:
               more registers than the hardware has cannot be handed to
               anyone, however many are asked for. */
            if (args >= 56 && a[13] >= 4u && a[13] <= SDBG_PERF_N)
                g_perfN = a[13];
            pb->step   = 0;
            pb->n      = g_perfN;   /* how many words to expect -- we say it ourselves */
            pb->readfn = (unsigned)&perf_read_k;
            pb->magic = SDBG_PERF_MAGIC;
            g_perf = pb;
        }
        /* Report path (moved out): the string lives in the game's memory,
           and the game may die before we get to write the file -- so we
           copy it to ourselves. */
        if (args >= 48 && a[11]) {
            const char *src = (const char *)a[11];
            int i;
            for (i = 0; i < CRASH_PATH_MAX - 1 && src[i]; i++) g_crashPath[i] = src[i];
            g_crashPath[i] = 0;
        }
        /* v36: stack bounds of the game's main thread. We check the range
           and the sanity of the size -- reads are later done within them. */
        if (args >= 44 && a[9] && a[10] > a[9]
            && a[9] >= 0x08800000u && a[10] <= 0x0C000000u
            && (a[10] - a[9]) <= 0x80000u) {
            g_stackLo = a[9];
            g_stackHi = a[10];
        }
        a[1] = (unsigned)psrcCrashInstall(g_textBase);
        if (args >= 16 && a[3]) {
            SceUID th;
            g_disarmWord = (volatile unsigned *)a[3];
            /* PRIORITY 0x12, NOT 0x30, AND THIS WAS PAID FOR WITH TWO LIVE
               RUNS. The PSP scheduler is strictly priority-based: a thread
               with a higher number gets NOT A SINGLE CYCLE while even one
               with a lower number is ready. At 0x30 (48) this thread sat
               below both the game's main thread (32) and all its service
               threads (12..18), i.e. it ran only in the moments when the
               game had nothing to do.
               OUTWARDLY THIS SHOWED UP AS A TOOL THAT LIES SILENTLY. The
               [perf] line comes out every 50 ms -- but only if the thread
               managed to wake up, and in a heavy frame it did not. So the
               counters systematically missed the MOST LOADED frames --
               exactly the ones they are read for. And the calibration
               (perfcal.c), which loads the CPU on purpose and does not block
               at all, did not get a single window in three seconds: the step
               field stayed zero, while the same thread calmly took a window
               at second 21, when the game reached the menu.
               THE COST OF RAISING IT IS NIL: the thread sleeps 50 ms and
               makes a couple dozen register accesses. It also writes the
               crash report faster, and that is the only reason it exists
               at all.
               0x12 (18) is the same league as the game's prof_flusher; above
               it there is only the game's watchdog at 12. We set no new
               precedent. */
            th = sceKernelCreateThread("wowee_crash_watch", crash_watch_thread,
                                       0x12, 0x1000, 0, NULL);
            g_watchRc = (unsigned)th;
            if (th >= 0) g_watchStartRc = (unsigned)sceKernelStartThread(th, 0, NULL);
        }
    } else {
        psrcCrashInstall(0);
    }
    /* THE NUMBER ACCEPTED -- INTO A FILE, BEFORE ANY SELF-TEST. The game
       prints a[1] only after sceKernelStartModule returns, and the mode 2/3
       self-tests do not return -- in log 409 the number never appeared. The
       file is written from here, by an ordinary kernel thread, the same way
       as CRASH.TXT. The first line is how many of the twelve registrations
       the kernel accepted; zero here explains any silence without further
       guessing. */
    {
        char *p = g_buf;
        SceUID fd;
        p = put(p, "ARMED ");   p = hex8(p, (unsigned)g_armed);
        p = put(p, " OF 0C\nTEXTBASE "); p = hex8(p, g_textBase);
        p = put(p, "\nMODE "); p = hex8(p, mode); *p++ = '\n';
        /* The disarm-word watcher thread: id (or failure code) and start rc. */
        p = put(p, "DEF prev "); p = hex8(p, g_prevDefault); p = put(p, " rc "); p = hex8(p, g_rc[0]); *p++ = '\n';
        p = put(p, "WATCH "); p = hex8(p, g_watchRc); *p++ = ' '; p = hex8(p, g_watchStartRc);
        p = put(p, " AT "); p = hex8(p, (unsigned)g_disarmWord); *p++ = '\n';
        /* Return codes: RC0 -- default, RC4..RC15 -- priority ones. */
        {
            int i;
            for (i = 0; i < 16; i++) {
                if (i != 0 && i < 4) continue;
                p = put(p, "RC"); *p++ = (char)('0' + i / 10); *p++ = (char)('0' + i % 10);
                *p++ = ' '; p = hex8(p, g_rc[i]); *p++ = '\n';
            }
        }
        /* ARMED.TXT goes NEXT TO THE REPORT: the report path is set by the
           game, and scattering the catcher's two files across different
           directories would mean searching for them separately. The name is
           obtained by replacing the tail. */
        fd = sceIoOpen(armed_path(),
                       PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (fd >= 0) {
            sceIoWrite(fd, g_buf, (SceSize)(p - g_buf));
            sceIoClose(fd);
        }
    }
    if (mode == 2)      psrcCrashSelfTest();
    else if (mode == 3) psrcCrashKernelFault();
    return 0;
}

/* Handler release -- not declared in the SDK, our own stub (exmrel.S). */
int sceKernelReleaseExceptionHandler(int exno, void *func);
int sceKernelReleaseDefaultExceptionHandler(void *func);

/* ---- module_stop REMOVES ALL HANDLERS, AND THIS IS THE EXIT FIX (hardware 432).
 * Without the catcher, "Exit" from the pause menu returned to the XMB; with
 * the catcher -- a red band and the console went dark inside
 * sceKernelExitGame, three exit marks on screen. On exit the kernel unloads
 * the module, but its twelve entries stay in the ExceptionManager lists as
 * pointers to freed code: the very first exception on the exit path jumps
 * into our dead trampoline. The empty module_stop was that hole.
 * We remove exactly what we installed: the same numbers, the same slot
 * addresses. The number removed is the return value: the game reads it as
 * the status of sceKernelStopModule and writes it to the log BEFORE the log
 * is closed. A release failure is not written to ARMED.TXT: the file may be
 * busy at this moment, and the kernel will unload the module right after
 * anyway. */
/* ---- DISARM ON THE GAME'S CALL VIA THE BRIDGE (hardware 434).
 * sceKernelStopModule returned 0, but module_stop DID NOT RUN: ARMED.TXT was
 * left over from start, the handlers were still in the lists, the module was
 * unloaded, and the exit jumped into freed memory. The game cannot use an
 * export either (importing from a PRX loaded after it breaks its own load --
 * 8002013C). What remains is the home.c road:
 * sctrlHENFindFunction("psrccrash", "psrcCrash", NID) gives the address of
 * this function, and kuKernelCall runs it in the kernel. Below is the same
 * body as module_stop; that one is kept in case the kernel does call it
 * after all. Returns the number removed; the report goes to ARMED.TXT. */
static int crash_release_all(void)
{
    int exno, released = 0;
    /* We remove exactly what was installed: g_rc < 0 -- there was no
       registration (8 and failures). THERE IS NO FILE HERE ANY MORE (audit
       07.09, C.8): on the exit path the card may be busy, and the number
       removed reaches the game anyway in the answer word (DONE | n). Exit
       must not depend on the card. */
    for (exno = 1; exno <= 3; exno++) {
        if ((int)g_rc[exno] < 0) continue;
        if (sceKernelReleaseExceptionHandler(exno, TRAP_SLOT(exno + 12)) >= 0) released++;
    }
    for (exno = 4; exno <= 15; exno++) {
        if ((int)g_rc[exno] < 0) continue;
        if (sceKernelReleaseExceptionHandler(exno, TRAP_SLOT(exno - 3)) >= 0) released++;
    }
    /* The default handler is not installed since v32 (analysis -- in
       psrcCrashInstall), but the release stays here: if it ever comes back
       with a chain, forgetting it would leave the firmware a pointer into
       unloaded code. */
    if ((int)g_rc[0] >= 0) {
        if (sceKernelReleaseDefaultExceptionHandler(TRAP_SLOT(0)) >= 0) released++;
    }
    g_armed = 0;
    return released;
}

int psrcCrashUninstall(void)
{
    return crash_release_all();
}

int module_stop(SceSize args, void *argp)
{
    (void)args; (void)argp;
    return g_armed ? crash_release_all() : 0;
}
