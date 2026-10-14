/* psplink_embed.c -- PSPLink inside the game. The contract and the reasoning are in the header. */
#include <pspkernel.h>
#include <string.h>
#include "sdbg_psplink.h"
#include "sdbg.h"




#ifndef PSRC_HAVE_KUBRIDGE
#define PSRC_HAVE_KUBRIDGE 0
#endif
#if PSRC_HAVE_KUBRIDGE
#include <kubridge.h>   /* kuKernelLoadModule -- a kernel load through ARK */
#endif

static int s_active;

int sdbg_psplink_active(void) { return s_active; }

/* A directory, not a full path: the name psplink.prx is the same for
   everyone, but it can live anywhere. Null -- take the usual ms0 path. */
int sdbg_psplink_boot(const char *prx_dir)
{
    /* THE PATH IS THE SAME AS FOR A PSPLINK INSTALL STARTED FROM THE XMB
       (SETUP_WINDOWS.md): psplink.prx finds usbhostfs.prx and psplink.ini
       next to itself through argp, so argp is the whole path string,
       including the terminating zero (as in the sibling port). */
    static char path[128];
    const char *dir = prx_dir ? prx_dir : "ms0:/PSP/GAME/psplink";
    { int i = 0, j;
      while (dir[i] && i < 100) { path[i] = dir[i]; i++; }
      if (i && path[i - 1] != 0x2F) path[i++] = 0x2F;
      { static const char nm[] = "psplink.prx";
        for (j = 0; nm[j]; j++) path[i++] = nm[j]; }
      path[i] = 0; }

#if PSRC_HAVE_KUBRIDGE
    {
        SceUID mod;
        int status = 0, rc;
        mod = kuKernelLoadModule(path, 0, 0);
        if (mod < 0) {
            sdbg_log_line("[psplink] NOT loaded: kuKernelLoadModule(%s) 0x%08X -- "
                      "no ARK, no file on the card, or this is an emulator",
                      path, (unsigned)mod);
            prof_commit_now();
            return 0;
        }
        rc = sceKernelStartModule(mod, sizeof(path), (void *)path, &status, NULL);
        if (rc < 0) {
            sdbg_log_line("[psplink] loaded (0x%08X), but NOT started: 0x%08X",
                      (unsigned)mod, (unsigned)rc);
            prof_commit_now();
            return 0;
        }
        s_active = 1;
        sdbg_log_line("[psplink] up inside the game: module 0x%08X, status %d -- "
                  "on the PC run_usbhostfs.bat, then run_pspsh.bat (modlist, "
                  "memdump); the exception catcher is NOT loaded this session",
                  (unsigned)mod, status);
        return 1;
    }
#else
    (void)prx_dir;
    sdbg_log_line("[psplink] build without kubridge (old SDK) -- "
              "no kernel loader, skipping");
#endif
    return 0;
}
