// PSP (docs/PSP_PORT_ARCHITECTURE.md, docs/PSP_AUTOTEST.md): the on-device autopilot. autotest.txt next
// to the EBOOT (the script: psp_autotest_script.hpp) makes the client test itself on the PSP while the owner watches:
// test blocks one after another, a band at the top saying what to look at, a report next to the EBOOT, a summary
// panel at the end. Without the file nothing of this runs (one sceIoOpen at boot).
//
// main_psp.cpp: autotestBoot() right after configLoad() (its "set" lines are this run's wowee.cfg values),
// autotestTick() once a frame after inputUpdate(), drawAutotestOverlay() (ui_autotest_overlay.cpp) after the screen.
// The world screen runs the devcmd verbs (say / open / close / ui / target / pos): autotestServiceWorld() from
// WorldScreen::devtoolsUpdate() every frame - a step that needs a world waits until there is one.
// START + SELECT pauses / resumes (psp_input.hpp inputFilterStartSelect; the game has no START+SELECT exit since Build
// 121 - HOME / the menu's exit).
// When the script ends: autotest.txt -> autotest.done (the next start is a normal game).
#pragma once

#include <cstdint>
#include <string>

namespace wowee::platform::psp {

// After configLoad(): reads autotest.txt, applies its "set" lines (configOverride), arms the log listener.
// Returns true when the autopilot runs this session.
bool autotestBoot();
bool autotestActive();  // the run is on (also while its summary panel is up)
void autotestTick();    // main loop, every frame

// The world screen's executor for the devcmd verbs (WorldScreen::devCommand); true = done.
using AutotestWorldExec = bool (*)(void* ctx, const std::string& line);
void autotestServiceWorld(AutotestWorldExec exec, void* ctx);

// What the overlay draws (ui_autotest_overlay.cpp); nullptr when there is nothing to draw.
struct AutotestView {
    bool paused = false;
    bool finished = false;      // the summary panel
    bool pauseHint = false;     // "START+SELECT - pause" (the first seconds, and while paused)
    int block = 0, blocks = 0;  // 1-based current block, number of blocks
    int step = 0, steps = 0;    // 1-based current step, number of steps
    uint32_t elapsedMs = 0;     // since the first step (pauses included)
    const char* blockCaption = "";  // in the running language (ru for ruRU, else en)
    const char* lookCaption = "";
    const char* stepVerb = "";
    // The summary: per block id / caption / pass.
    int resultCount = 0;
    const char* resultId(int i) const;
    const char* resultCaption(int i) const;
    bool resultPass(int i) const;
    uint32_t resultMs(int i) const;
    int passCount = 0, failCount = 0;
};
const AutotestView* autotestView();
// PSP (public safe): non-null for 30 s after the autopilot refused to run (the server is not a dev server):
// the overlay shows the one-line notice AT_REFUSED_PUBLIC. Log: autotest_refused public=1 at=boot|run.
const char* autotestRefusedNotice();

}  // namespace wowee::platform::psp
