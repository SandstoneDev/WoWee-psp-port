// PSP (docs/PSP_PORT_ARCHITECTURE.md): the on-device autopilot's screen part (platform/psp/psp_autotest.hpp)
// - a band at the top centre in the client's yellow UIErrorsFrame font: "Test 3/14: <place> - look: ...", a small
// step counter, the START+SELECT pause line; at the end the summary panel (PASS / FAIL per block) until Cross.
// Drawn by main_psp.cpp after the screen (login, character select, loading screen, world) - nothing without a run.
#pragma once

#include "ui/psp/ui_kit.hpp"
#include "ui/psp/ui_renderer.hpp"

namespace wowee::ui::psp {

void drawAutotestOverlay(UiRenderer& ui, const UiKit& kit);

}  // namespace wowee::ui::psp
