// Profiler overlay (SELECT): frame split bar, GE passes, top zones, counters.
// Data: platform/psp/psp_prof.hpp summary (averaged over 60 frames).
#pragma once

#include "ui/psp/ui_kit.hpp"
#include "ui/psp/ui_renderer.hpp"

namespace wowee::ui::psp {

void drawProfOverlay(UiRenderer& ui, const UiKit& kit);

} // namespace wowee::ui::psp
