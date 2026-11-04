// The own player's geoset and body-texture rules -
// upstream's GameScreen::updateCharacterGeosets and updateCharacterTextures
// (src/ui/game_screen_hud.cpp), their text cut out of that file by CMake when
// the converter is configured (tools/psp/converter/CMakeLists.txt writes
// equip_matrix_local.inc into the build folder; a change upstream is picked up
// by the next build). The UI file they live in (ImGui, the whole game screen)
// cannot be compiled alone. Two lines differ in each: the signature
// (MatrixApp in place of core::Application, appearanceComposer_ a parameter)
// and "auto& app = core::Application::getInstance();".
#include "upstream_local.hpp"

#include "core/geoset_rules.hpp"
#include "core/helm_visual.hpp"
#include "core/logger.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/item_textures.hpp"
#include "rendering/character_renderer.hpp"

#include <algorithm>
#include <memory>

namespace wowee {
namespace ui {

#include "equip_matrix_local.inc"

}  // namespace ui
}  // namespace wowee
