# PSP exit cleanup

The main-menu Quit action, in-world Exit Game, completed exit logout, Exit Now,
and the HOME callback all leave the same main loop. Previously the loop tore
down networking, shared UI textures, and GU without destroying its LoginScreen
or calling audioShutdown. systemExit never returns, so screen destructors were
never reached: their streaming/minimap workers, sockets and PCM banks remained
live during firmware exit. The attached session reached `NET: stopped` at quit;
it does not identify a single failing firmware call.

The screen now has an explicit lifetime scope. Exit stops and joins the audio
workers first, clears mixer references to PCM, drains queued output, then destroys
the screen and its owned loaders/resources while networking and graphics remain
available. Shared UI art and GU shut down afterward. An exit requested from
update also skips the final draw. Audio joins now check their result; the normal
path deletes stopped threads, and a timed-out worker uses the termination fallback.
Output drain has a 500 ms limit so a stuck audio driver cannot loop forever there.

Flushed `shutdown stage=audio/screens/ui/display/complete` log entries identify
which stage a remaining hardware failure reaches. Existing rested-area handling,
server logout approval, twenty-second countdown, Cancel and Exit Now are retained.

`tests/test_psp_audio_shutdown.py` compiles the production audioShutdown routine
with simulated PSP kernel/audio APIs. It verifies worker joins before freeing
PCM or codec resources, timeout fallback, channel drain, bounded driver stalls,
and idempotent/disabled cleanup. It passed on the host using MSVC. The compiler
memory barrier is replaced with a host atomic signal fence for this test only.
A PSP build and PSP/Vita exit test are still required; WSL cannot run here.

Rebuild tools/psp/build_psp.sh --no-bump and replace only psp/bin/EBOOT.PBP.
No asset rebuild is needed. On hardware test main-menu Quit, rested-area Exit
Game, outdoor countdown completion, Exit Now, and cancellation followed by a
later exit. On Vita also confirm Adrenaline returns without requiring a restart.

## Vita black screen after exit

The report was clarified as exiting to a black screen on Vita, after which other
applications cannot launch until reboot. The earlier log confirms that the
SandstoneDebug kernel PRX was loaded. Its user-side unload path does not require
a successful disarm acknowledgement before unloading its code. That is a
separate potential failure from worker cleanup; the report does not establish
which one caused this incident.

Kernel crash catching is now opt-in through `crashcatcher=1` in wowee.cfg; the
default is off, including when sdbg.prx remains next to EBOOT.PBP. Config is loaded
before this decision. Normal session logging and the game's profiler continue,
but PRX-provided hardware counters and kernel crash reports are unavailable
while the catcher is disabled. No PRX replacement or asset rebuild is required.
Reboot a currently affected Vita before testing the rebuilt executable. A real
Vita test is still required to confirm resolution.
