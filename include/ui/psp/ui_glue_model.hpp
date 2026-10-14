// The selected character's model on CharacterSelect (audit #2; CharacterSelect is a
// ModelFFX: SetBackgroundModel + the character standing in it, CharacterSelectRotateLeft/Right).
//
// The world is not loaded on the glue screens, so the heap is free: the model is the world's
// CharacterModel (data/characters.pak, items.pak for what is worn), drawn straight into the frame
// after the background. Loaded when a character is selected, released when another one is, when the
// world starts loading and when the screen goes (PSP: never two models at once). The heap use is
// logged (`charsel_model ... model_kb=`, `mem at=charsel_model`). The race
// backgrounds of the original (Interface\Glues\Models\UI_<Race>) are pictures rendered through the
// scene's camera (ui_glue_scene.hpp); drawInScene() stands the model in them through the same camera.
#pragma once

#include "game/character.hpp"
#include "rendering/psp/psp_character.hpp"
#include "rendering/psp/psp_equipment.hpp"
#include "ui/psp/ui_glue_scene.hpp"

#include <cstdint>
#include <memory>

namespace wowee::ui::psp {

class GlueCharacterModel {
public:
    GlueCharacterModel() = default;
    GlueCharacterModel(const GlueCharacterModel&) = delete;
    GlueCharacterModel& operator=(const GlueCharacterModel&) = delete;
    ~GlueCharacterModel() { release(); }

    // Loads the model of `c` (once per character; cheap when it is already shown).
    // dressup: again when the same character comes back in other items / look (a new
    // SMSG_CHAR_ENUM: UpdateCharacterList dresses the model anew); CHARACTER_FLAG_HIDE_HELM / _HIDE_CLOAK of the
    // list hide the helm / cloak as the original's select screen does.
    void show(const game::Character& c);
    void release();
    bool loaded() const { return model_.loaded(); }
    uint64_t guid() const { return guid_; }
    void rotate(float radians) { yaw_ += radians; }
    void update(float dtSec);
    // Into the current frame (between UiRenderer frames): the model `heightPx` tall, its middle at
    // screen (cx, cy). Depth is cleared first; the GE state is left as the UI expects it.
    void draw(float cx, float cy, float heightPx);
    // On its scene's picture - at the scene's stand mark, facing its camera (+ the
    // rotate buttons), through that camera (shifted like the picture), in the scene's character light and
    // fog (SetLighting: AddCharacterLight with RaceLights, CharModelFogInfo).
    // extraShiftX: screen px the picture was moved further (GlueBackground::draw offsetX).
    void drawInScene(const GlueScene& s, float extraShiftX = 0.0f);

private:
    rendering::psp::CharacterModel model_;
    std::unique_ptr<rendering::psp::ItemBank> items_;  // items.pak open only while a model is shown
    uint64_t guid_ = 0;
    uint64_t lookKey_ = 0;  // dressup: what show() dressed (equipment, flags, appearance)
    bool tried_ = false;
    float yaw_ = 0.0f;
};

} // namespace wowee::ui::psp
