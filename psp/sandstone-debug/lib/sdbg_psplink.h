/* psplink_embed.h -- PSPLink INSIDE THE GAME (the psplink=1 key in the game's
   config file).

   WHY. Deaths 423..431 on hardware are kernel ones: the memory card LED is
   lit, the console goes dark, nothing reaches the file, the catcher hangs on
   the busy card. The only channel that needs no card is USB: PSPLink keeps
   its own exception handler and prints the registers, thread and stack to
   pspsh on the PC at the moment of the exception, and usbhostfs wraps the
   game's stdout into the same console -- a log without the grace of a file
   descriptor.

   HOW (the pattern comes from a sibling PSP port, its "PSPLink embed" in
   main.c): psplink cannot ldstart our EBOOT (user PRX + LARGE_MEMORY +
   kubridge -> 0x80020148), so the game loads psplink.prx ITSELF through
   kuKernelLoadModule (a kernel load through ARK; pspSdkLoadStartModule with
   PARTITION_KERNEL from user mode hangs). psplink.prx takes its directory
   from argp and brings up usbhostfs.prx and psplink.ini from
   ms0:/PSP/GAME/psplink/ by itself. Then on the PC: run_usbhostfs.bat,
   run_pspsh.bat (from the psplinkusb sources, SETUP_WINDOWS.md).

   TWO GATES: a build with kubridge (PSRC_HAVE_KUBRIDGE, new SDK only) AND
   the psplink=1 key. A normal start does not touch the kernel loader. Only
   on hardware with ARK; in PPSSPP kuKernelLoadModule returns a refusal, and
   the log line says so.

   DOES NOT LIVE WITH THE CATCHER: both register priority handlers for the
   same exception numbers; with psplink=1 the game does not load the
   catcher. */
#ifndef PSRC_PSPLINK_EMBED_H
#define PSRC_PSPLINK_EMBED_H

/* Reads the key, loads and starts psplink.prx. Call after prof_init and the
   config, before the exception catcher. Writes a [psplink] line to the log
   whatever the outcome. */
int sdbg_psplink_boot(const char *prx_dir);

/* 1 -- psplink.prx was loaded and started in this session. */
int sdbg_psplink_active(void);

#endif
