// The character-select background of the original - a scene per race, as the original
// client shows each race on its own background.
//
// The client: CharacterSelect_SelectCharacter -> GetSelectBackgroundModel(id) ->
// SetBackgroundModel (GlueParent.lua) loads Interface\Glues\Models\UI_<name>\UI_<name>.m2 into
// the ModelFFX with camera 0, the race's fog and lights, and the character stands in it;
// CharacterCreate does the same with GetCreateBackgroundModel for the race being made.
// PSP: the scene is a picture rendered on the PC through that camera, with those lights and fog
// (tools/psp/converter/psp_glue_scene_conv -> glue.pak cs_<name>, 512 x 256 T8 = 129 KB, one at a
// time, only on the glue screens), and the character is drawn over it in 3D through the same
// camera (GlueScene) - the M2 scene itself is too heavy to draw live on the PSP. Without the entries
// (an old glue.pak) the screens keep the login art.
#pragma once

#include "game/character.hpp"
#include "platform/psp/psp_texture.hpp"
#include "ui/psp/ui_renderer.hpp"

#include <cstdint>

namespace wowee::ui::psp {

// glue.pak "cs_scenes" row (tools/psp/build_glue_pak.py CS_SCENE_FMT), the scene's server-space
// coordinates (Z up) as the M2 has them.
struct GlueScene {
    char name[16];
    float eye[3];
    float target[3];
    float fovy;      // radians (0.6 x the M2 camera's FoV)
    float nearZ, farZ;
    float stand[3];  // attachment 0: the character's feet
    float yaw;       // the character faces the camera
    float shiftX;    // PSP: screen px the picture was moved sideways (the feet at x 150, left of the list)
    float fog[4];    // CharModelFogInfo r, g, b, far (far 0 = ClearFog)
    float tint[3];   // RaceLights at the character's front: the PSP character's light colour
    float pad;
};
static_assert(sizeof(GlueScene) == 104, "glue.pak cs_scenes rows are 104 bytes");

// GetSelectBackgroundModel / GetCreateBackgroundModel: the class file name for a death knight
// ("DEATHKNIGHT", CharacterSelect_DeathKnightSwap), else the race's scene - gnomes stand in the
// dwarves' one and trolls in the orcs' (GlueAmbienceTracks: GlueScreenDwarfGnome / OrcTroll; no
// UI_Gnome / UI_Troll models in 3.3.5a). Lower case, as the pak names them.
const char* glueSceneFor(game::Race race, game::Class cls);

class GlueBackground {
public:
    GlueBackground() = default;
    GlueBackground(const GlueBackground&) = delete;
    GlueBackground& operator=(const GlueBackground&) = delete;
    ~GlueBackground() { release(); }

    // cs_<scene> from glue.pak; nothing to do when it is already shown, the old one is freed first.
    void show(const char* scene);
    void release();
    // The picture over the whole screen, moved right by offsetX (the creation screen: the model in the
    // middle; black where it leaves the screen bare); false when there is none (the caller draws its own).
    bool draw(UiRenderer& ui, float offsetX = 0.0f) const;
    // The camera of the shown picture, null without one.
    const GlueScene* scene() const { return have_ ? &scene_ : nullptr; }

private:
    platform::psp::Texture tex_;
    GlueScene scene_{};
    bool have_ = false;
    char name_[16] = {};
};

} // namespace wowee::ui::psp
