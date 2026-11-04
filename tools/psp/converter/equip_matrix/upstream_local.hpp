#pragma once
// What the own player's equipment rules run with. Upstream's
// GameScreen::updateCharacterGeosets / updateCharacterTextures (src/ui/
// game_screen_hud.cpp) ask core::Application for the renderer, the asset
// manager, the game handler and the composed skin; MatrixApp answers the same
// calls (seam side: built with equip_matrix/seam first on the include path).
#include "core/appearance_composer.hpp"
#include "game/game_handler.hpp"
#include "pipeline/asset_manager.hpp"
#include "rendering/renderer.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace wowee {
namespace ui {

struct MatrixApp {
    rendering::Renderer* renderer = nullptr;
    pipeline::AssetManager* assets = nullptr;
    game::GameHandler* gameHandler = nullptr;
    core::AppearanceComposer* composer = nullptr;
    rendering::Renderer* getRenderer() const { return renderer; }
    pipeline::AssetManager* getAssetManager() const { return assets; }
    game::GameHandler* getGameHandler() const { return gameHandler; }
    const std::string& getBodySkinPath() const { return composer->getBodySkinPath(); }
    const std::vector<std::string>& getUnderwearPaths() const { return composer->getUnderwearPaths(); }
    uint32_t getSkinTextureSlotIndex() const { return composer->getSkinTextureSlotIndex(); }
    uint32_t getCloakTextureSlotIndex() const { return composer->getCloakTextureSlotIndex(); }
};

// upstream GameScreen::updateCharacterGeosets / updateCharacterTextures, their text (upstream_local.cpp).
void matrix_updateCharacterGeosets(MatrixApp& matrixApp, core::AppearanceComposer* appearanceComposer,
                                   game::Inventory& inventory);
void matrix_updateCharacterTextures(MatrixApp& matrixApp, core::AppearanceComposer* appearanceComposer,
                                    game::Inventory& inventory);

}  // namespace ui
}  // namespace wowee
