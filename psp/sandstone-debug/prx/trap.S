/* trap.S -- OUR OWN EXCEPTION TRAMPOLINE, NOT THE ONE FROM libpspdebug. DETAILS -- main.c.
 *
 * HOW IT DIFFERS FROM THE SDK (exception_asm.S, examined with psp-objdump 02.09.2026):
 * the SDK one saves the registers into a block, puts the C entry address into EPC and
 * does eret WITHOUT TOUCHING Status. For a USER thread that is a return into kernel code
 * in user mode -- a nested exception, a firmware panic, the console powering off.
 * That is how six hardware runs in a row died, and the check crash crashtest=1 too.
 * Here, before eret, the mode is forced to kernel (KSU=0), EXL/ERL are cleared and IE
 * is enabled: the C entry runs as ordinary kernel code with live interrupts -- the
 * scheduler, display and memory card keep working, the SYSCON watchdog gets fed, the
 * console does not go dark.
 *
 * BLOCK LAYOUT -- PspDebugRegBlock from pspdebug.h, byte for byte (r[i] at
 * offset 24 + 4*i, status 152, lo 156, hi 160, badvaddr 164, cause 168,
 * epc 172): the C side reads it through the same fields as before.
 *
 * v0/v1 ARE ALREADY CLOBBERED AT ENTRY: the kernel stores the original v0/v1 in the
 * COP0 control registers $4/$5 before passing control to the handler (this is exactly
 * how the SDK retrieves them too -- cfc0 $4/$5). at is saved FIRST, before any la. */

    .set noreorder
    .set noat

    .section .bss
    .align 4
    .global psrc_trap_regs
psrc_trap_regs:
    .space 0x400
    .align 4
psrc_trap_stack:
    .space 0x4000      /* 16 KB: for sceIoOpen/Write on the kernel path, audit R6 */
psrc_trap_stack_top:

    .section .text
    .align 4

/* ---- SIXTEEN ENTRIES OF SIXTEEN BYTES EACH. THE KERNEL FORMAT, NOT OUR TASTE.
 *
 * LOG 410 SHOWED THE PRICE OF NOT KNOWING: all thirteen registrations returned
 * 0x80020034 = SCE_KERNEL_ERROR_EXPHANDLER_USED, "handler already in use".
 * The answer is in the SDK objdump listing (psp-objdump libpspdebug): the entry point
 * _pspDebugExceptionHandler starts with TWO ZERO WORDS, and the first
 * instruction sits at +8. That is not alignment: ExceptionManager links the
 * handlers into a LIST and writes the next pointer straight into the first word of
 * the registered structure, and it reads a nonzero value there as
 * "this entry is already in the list". Our trampoline used to start with `lui` -- the
 * kernel saw garbage in next and refused, all twelve times.
 *
 * THAT IS WHY THERE ARE SIXTEEN ENTRIES, NOT ONE: a structure can sit in only
 * ONE list, and a second registration of the same address would get the same
 * refusal. Slots 1..12 are for exception codes 4..15, slots 13..15 are
 * for TLB misses (codes 1..3), slot 0 is free: the "default" handler
 * is no longer installed (see psrcCrashInstall). The stride is exactly
 * 16 bytes (two kernel words + jump + delay slot), and the C side relies on it:
 * TRAP_SLOT(i) in main.c. */
    .global psrc_trap_slots
psrc_trap_slots:
    .set  slot_idx, 0
    .rept 16
    .word 0            /* next -- WRITTEN BY THE KERNEL, must be zero before registration */
    .word 0            /* second kernel bookkeeping word, zero in the SDK too */
    j     psrc_trap_entry
    li    $k0, slot_idx   /* delay slot: SLOT NUMBER in k0 -- the trampoline uses it
                             to find the next word for passing on down the chain
                             (uOFW exceptions.c: the kernel writes next+8 into the first word) */
    .set  slot_idx, slot_idx + 1
    .endr

    .align 2
    .global psrc_trap_entry
    .ent psrc_trap_entry
psrc_trap_entry:
    /* v0 -- block base; the original v0 is in $4, v1 is in $5 (COP0 control). */
    lui   $v0, %hi(psrc_trap_regs)
    addiu $v0, $v0, %lo(psrc_trap_regs)
    sw    $zero, 24($v0)
    sw    $at,   28($v0)
    cfc0  $at, $4
    sw    $at,   32($v0)
    cfc0  $at, $5
    sw    $at,   36($v0)
    sw    $a0,   40($v0)
    sw    $a1,   44($v0)
    sw    $a2,   48($v0)
    sw    $a3,   52($v0)
    sw    $t0,   56($v0)
    sw    $t1,   60($v0)
    sw    $t2,   64($v0)
    sw    $t3,   68($v0)
    sw    $t4,   72($v0)
    sw    $t5,   76($v0)
    sw    $t6,   80($v0)
    sw    $t7,   84($v0)
    sw    $s0,   88($v0)
    sw    $s1,   92($v0)
    sw    $s2,   96($v0)
    sw    $s3,  100($v0)
    sw    $s4,  104($v0)
    sw    $s5,  108($v0)
    sw    $s6,  112($v0)
    sw    $s7,  116($v0)
    sw    $t8,  120($v0)
    sw    $t9,  124($v0)
    sw    $k0,  128($v0)
    sw    $k1,  132($v0)
    sw    $gp,  136($v0)
    sw    $sp,  140($v0)
    sw    $fp,  144($v0)
    sw    $ra,  148($v0)
    mfc0  $v1, $12
    sw    $v1,  152($v0)
    mflo  $v1
    sw    $v1,  156($v0)
    mfhi  $v1
    sw    $v1,  160($v0)
    mfc0  $v1, $8
    sw    $v1,  164($v0)
    mfc0  $v1, $13
    sw    $v1,  168($v0)
    mfc0  $v1, $14
    sw    $v1,  172($v0)

    /* ---- FILTER FOR CODES 9 (break) AND 13 (trap) -- FIX FOR THE "SILENCE" (audit 07.09).
       psp-gcc places `break 0x1c00` after every integer div
       (code 7 = division by zero; 6 = overflow): the game ELF has a hundred of them,
       fourteen in b3Solve. The catcher did not take code 9 at all, and int/0 went
       to the kernel default handler -- an endless loop with EXL=1, the SYSCON
       watchdog powers the console off, and by construction no stripe appears. But
       regular firmware traps (kuKernelCall, KDebug) also live on code 9, so OUR break
       is only one that sits in the game text (0x08800000..0x0A000000) and
       carries code 6/7; trap (13) -- any one in the game text. Everything else goes
       on down the chain: restore the registers and jump via next from the
       first word of OUR slot (k0 brought the slot number). The registers are already
       saved, so t0..t7 can be used freely here. */
    /* v31: SLOT 0 -- THE DEFAULT HANDLER, there is nobody further down: anything that
       got here was taken by nobody (this is how an FPU exception arrives, hardware
       run 453). The slot number is in k0 (128). */
    lw    $t1, 128($v0)
    beqz  $t1, 3f
    nop
    lw    $t0, 168($v0)          /* cause */
    srl   $t1, $t0, 2
    andi  $t1, $t1, 31           /* ExcCode */
    /* WOWEE: CODE 11 WITH CE = 1/2 -- LAZY FPU/VFPU ENABLING BY THE
       FIRMWARE, NOT A FAULT. A new thread gets CU1 (CU2) not at creation but
       on its first FPU (VFPU) instruction: a "coprocessor unusable" exception,
       the firmware sets the bit and saves the context. We sit at priority 0
       ahead of it and caught this as a crash: hardware run 82, cause 9000002C (CE=1),
       epc -- the first swc1 of the sfxFeeder sound thread; crash window during loading and on
       entering the world (streamer and minimap threads), minimap grey while its thread is stopped.
       CE is taken from Cause[29:28]; 0 (COP0 from user mode) -- ours. */
    li    $t2, 11
    bne   $t1, $t2, 5f
    srl   $t3, $t0, 28
    andi  $t3, $t3, 3            /* CE */
    bnez  $t3, 9f                /* COP1/COP2 -- to the firmware down the chain */
    nop
    b     3f                     /* COP0 -- to the report */
    nop
5:  li    $t2, 9
    beq   $t1, $t2, 1f
    li    $t2, 13
    bne   $t1, $t2, 3f           /* neither 9 nor 13 -- always ours */
    nop
1:  lw    $t3, 172($v0)          /* epc */
    bgez  $t0, 2f                /* BD bit (31) clear -- pc = epc */
    nop
    addiu $t3, $t3, 4            /* in a delay slot -- the instruction itself is at epc+4 */
2:  lui   $t4, 0x0880
    sltu  $t5, $t3, $t4          /* pc < 0x08800000 -- NOT game text: to the REPORT */
    bnez  $t5, 3f
    lui   $t4, 0x0A00
    sltu  $t5, $t3, $t4          /* pc >= 0x0A000000 -- to the report too (hardware run 441:
                                    firmware break at 0x88045E58 -- it is the firmware
                                    that is dying, passing it on = looping forever, 440) */
    beqz  $t5, 3f
    li    $t7, 13
    beq   $t1, $t7, 3f           /* trap in the game text -- ours */
    nop
    lw    $t6, 0($t3)            /* the break instruction itself */
    srl   $t6, $t6, 6
    lui   $t7, 0x000F
    ori   $t7, $t7, 0xFFFF
    and   $t6, $t6, $t7          /* break code: bits 25..6, 20 bits */
    /* GCC WRITES `break 7` INTO THE UPPER TEN BITS (bits 25..16): in the 20-bit
       field that is 7 << 10 = 0x1c00, overflow is 6 << 10 = 0x1800. Revision
       v23 compared the field with 7 and handed our own division to the firmware --
       the firmware issued its own break, which came back to us as "foreign", and the
       console froze in a loop (hardware run 440). The bare 7/6 are there in case of
       a different compiler. */
    li    $t7, 0x1c00
    beq   $t6, $t7, 3f           /* division by zero -- ours */
    li    $t7, 0x1800
    beq   $t6, $t7, 3f           /* overflow -- ours */
    li    $t7, 7
    beq   $t6, $t7, 3f
    li    $t7, 6
    beq   $t6, $t7, 3f
    nop

    /* ---- ON DOWN THE CHAIN: a foreign break/trap. Everything as it was at entry:
       registers from the block, hi/lo, v0/v1 are still in COP0 $4/$5 (we did
       not touch them), Status/Cause/EPC untouched. The address of the next handler
       is word 0 of our slot (the kernel writes next+8 there). */
9:  lw    $k0, 128($v0)          /* slot number (saved at entry) */
    sll   $k0, $k0, 4
    lui   $v1, %hi(psrc_trap_slots)
    addiu $v1, $v1, %lo(psrc_trap_slots)
    addu  $v1, $v1, $k0
    lw    $v1, 0($v1)            /* next */
    lw    $t0, 156($v0)
    mtlo  $t0
    lw    $t0, 160($v0)
    mthi  $t0
    lw    $at,   28($v0)
    lw    $a0,   40($v0)
    lw    $a1,   44($v0)
    lw    $a2,   48($v0)
    lw    $a3,   52($v0)
    lw    $t0,   56($v0)
    lw    $t1,   60($v0)
    lw    $t2,   64($v0)
    lw    $t3,   68($v0)
    lw    $t4,   72($v0)
    lw    $t5,   76($v0)
    lw    $t6,   80($v0)
    lw    $t7,   84($v0)
    lw    $s0,   88($v0)
    lw    $s1,   92($v0)
    lw    $s2,   96($v0)
    lw    $s3,  100($v0)
    lw    $s4,  104($v0)
    lw    $s5,  108($v0)
    lw    $s6,  112($v0)
    lw    $s7,  116($v0)
    lw    $t8,  120($v0)
    lw    $t9,  124($v0)
    lw    $k1,  132($v0)
    lw    $gp,  136($v0)
    lw    $sp,  140($v0)
    lw    $fp,  144($v0)
    lw    $ra,  148($v0)
    jr    $v1
    nop

3:  /* Status: KSU=0 (kernel), ERL=0, EXL=0, IE=1. Everything else -- as it was. */
    mfc0  $t0, $12
    li    $t1, ~0x1F
    and   $t0, $t0, $t1
    /* WE DO NOT ENABLE IE (v28): in C we are not a thread but a firmware exception context
       -- the very first interrupt let the scheduler switch away mid-drawing (hardware runs
       443/444: the report broke off at a random line). psrc_trap_c draws without
       interrupts and enables IE itself once the report is already on screen. */
    mtc0  $t0, $12

    /* EPC -> C entry; our own stack; a0 = block; k1 = 0 -- otherwise kernel functions
       (sceIoOpen and others) reject our own kernel addresses as "user" addresses
       based on the k1 of the crashed thread (audit A.3, psplink exception_asm.S:
       move k1,zero) -- hence the CRASH.TXT that almost never appeared. */
    lui   $t0, %hi(psrc_trap_c)
    addiu $t0, $t0, %lo(psrc_trap_c)
    mtc0  $t0, $14
    lui   $sp, %hi(psrc_trap_stack_top)
    addiu $sp, $sp, %lo(psrc_trap_stack_top)
    addiu $sp, $sp, -16
    move  $a0, $v0
    move  $k1, $zero
    nop
    nop
    eret
    nop
    nop
    .end psrc_trap_entry

    .set at
    .set reorder
