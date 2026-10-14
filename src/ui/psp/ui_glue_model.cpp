// The character model on CharacterSelect (include/ui/psp/ui_glue_model.hpp).
#include "ui/psp/ui_glue_model.hpp"

#include "core/character_paths.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_bigheap.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_system.hpp"  // logMemory
#include "platform/psp/psp_texture.hpp"
#include "rendering/psp/psp_lighting_ge.hpp"

#include <pspgu.h>
#include <pspgum.h>
#include <pspthreadman.h>

#include <algorithm>
#include <cmath>
#include <malloc.h>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
namespace rp = wowee::rendering::psp;

namespace {
// SMSG_CHAR_ENUM's character flags (AzerothCore Player::BuildEnumData from PLAYER_FLAGS_HIDE_*).
constexpr uint32_t kCharFlagHideHelm = 0x00000400u;   // CHARACTER_FLAG_HIDE_HELM
constexpr uint32_t kCharFlagHideCloak = 0x00000800u;  // CHARACTER_FLAG_HIDE_CLOAK

// What the model shows of a character: its look, its 19 visible items, the helm / cloak flags (FNV-1a).
uint64_t lookKeyOf(const game::Character& c) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) {
        for (int b = 0; b < 8; ++b) {
            h ^= (v >> (b * 8)) & 0xFFu;
            h *= 1099511628211ull;
        }
    };
    mix(c.guid);
    mix(c.appearanceBytes);
    mix(c.facialFeatures);
    mix(c.flags & (kCharFlagHideHelm | kCharFlagHideCloak));
    for (size_t s = 0; s < 19 && s < c.equipment.size(); ++s)
        mix(static_cast<uint64_t>(c.equipment[s].displayModel) << 8 | c.equipment[s].inventoryType);
    return h;
}
}  // namespace

void GlueCharacterModel::release() {
    if (model_.loaded()) PSP_LOGI(pp::tag::Gfx, "charsel_model release");
    model_.clear();  // before the bank: the model keeps a pointer to it
    items_.reset();
    // the item models/textures this character held leave the shared cache now,
    // not when the world's bank first binds it (the world loads with that much more heap).
    rp::attachmentCacheTrim();
    guid_ = 0;
    tried_ = false;
}

void GlueCharacterModel::show(const game::Character& c) {
    const uint64_t key = lookKeyOf(c);  // the same character in other items is shown anew
    if (tried_ && guid_ == c.guid && lookKey_ == key) return;
    release();
    tried_ = true;
    guid_ = c.guid;
    lookKey_ = key;
    yaw_ = 0.0f;
    const uint32_t heap0 = pp::heapUsedBytes();  // both pools
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    // As WorldScreen::loadCharacter (look + what SMSG_CHAR_ENUM says is worn).
    rp::CharacterLook look;
    look.race = static_cast<uint8_t>(c.race);
    look.sex = (c.gender == game::Gender::FEMALE || (c.gender == game::Gender::NONBINARY && c.useFemaleModel)) ? 1 : 0;
    const auto a = core::unpackAppearanceBytes(c.appearanceBytes);
    look.skin = a.skinId;
    look.face = a.faceId;
    look.hairStyle = a.hairStyleId;
    look.hairColor = a.hairColorId;
    look.facial = c.facialFeatures;
    rp::Equipment equip;
    for (size_t s = 0; s < 19 && s < c.equipment.size(); ++s) {
        equip.displayId[s] = c.equipment[s].displayModel;
        equip.invType[s] = c.equipment[s].inventoryType;
    }
    // the Show Helm / Show Cloak options off - the select screen shows the character without them
    // (the head without the helm's model and hair rules, no cape geoset or texture). Batch 5 merge: through
    // the equipment code's flags (shownEquipment - the world model's one rule).
    equip.hideHelm = (c.flags & kCharFlagHideHelm) != 0;
    equip.hideCloak = (c.flags & kCharFlagHideCloak) != 0;
    if (c.flags & (kCharFlagHideHelm | kCharFlagHideCloak))
        PSP_LOGI(pp::tag::Gfx, "charsel_hide name=%s helm=%d cloak=%d", c.name.c_str(),
                 (c.flags & kCharFlagHideHelm) ? 1 : 0, (c.flags & kCharFlagHideCloak) ? 1 : 0);
    char path[256];
    items_ = std::make_unique<rp::ItemBank>();
    pp::fsPath(path, sizeof(path), "data/items.pak");
    if (!items_->open(path)) PSP_LOGW(pp::tag::Gfx, "charsel_model items_missing (no equipment drawn)");
    pp::fsPath(path, sizeof(path), "data/characters.pak");
    pp::Pak pak;
    if (!pak.open(path)) {
        PSP_LOGW(pp::tag::Gfx, "charsel_model characters_missing");
        items_.reset();
        return;
    }
    model_.setLocalPlayer(true);
    const bool ok = model_.load(pak, look, true, &equip, items_->isOpen() ? items_.get() : nullptr);
    pak.close();
    if (ok) {
        // PSP: weapons sheathed, as the character stands on the original's select screen.
        model_.setWeaponsSheathed(true);
        model_.playAnimation(0, true);  // Stand
    }
    PSP_LOGI(pp::tag::Gfx, "charsel_model name=%s ok=%d model_kb=%u heap_delta_kb=%d ms=%u", c.name.c_str(), ok ? 1 : 0,
             static_cast<unsigned>(model_.memoryKb()),
             static_cast<int>((static_cast<int64_t>(pp::heapUsedBytes()) - heap0) / 1024),
             static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u));
    pp::logMemory("charsel_model");
    if (!ok) {  // nothing to draw; tried_ / guid_ stay set so it is not retried every frame
        model_.clear();
        items_.reset();
    }
}

void GlueCharacterModel::update(float dtSec) {
    if (model_.loaded()) model_.update(dtSec);
}

void GlueCharacterModel::draw(float cx, float cy, float heightPx) {
    if (!model_.loaded()) return;
    const float h = std::max(0.8f, model_.height());
    constexpr float kFovY = 40.0f;
    const float tanHalf = std::tan(kFovY * 0.5f * 3.14159265f / 180.0f);
    // Projected height = h / (d tan(fov/2)) x (screen height / 2).
    const float dist = h * (pp::kScreenH * 0.5f) / (heightPx * tanHalf);
    const glm::vec3 feet(0.0f, 0.0f, 0.0f);
    const glm::vec3 centre(0.0f, 0.0f, h * 0.5f);
    const glm::vec3 eye = centre + glm::vec3(dist, 0.0f, 0.0f);  // in front of the model (facing +x at yaw 0)

    sceGuClearDepth(0);
    sceGuClear(GU_DEPTH_BUFFER_BIT);
    // The projection centre moved to (cx, cy): the model stands left of the character list.
    sceGuViewport(2048 - pp::kScreenW / 2 + static_cast<int>(cx), 2048 - pp::kScreenH / 2 + static_cast<int>(cy),
                  pp::kScreenW, pp::kScreenH);
    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadIdentity();
    sceGumPerspective(kFovY, static_cast<float>(pp::kScreenW) / pp::kScreenH, 0.1f, 60.0f);
    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();
    ScePspFVector3 e = {0.0f, 0.0f, 0.0f};
    ScePspFVector3 t = {centre.x - eye.x, centre.y - eye.y, centre.z - eye.z};
    ScePspFVector3 up = {0.0f, 0.0f, 1.0f};
    sceGumLookAt(&e, &t, &up);
    sceGumUpdateMatrix();
    rp::TerrainCamera cam;
    cam.pos = eye;
    cam.forward = glm::vec3(-1.0f, 0.0f, 0.0f);
    cam.fovyDeg = kFovY;
    cam.nearZ = 0.1f;
    cam.farZ = 60.0f;
    model_.draw(cam, feet, yaw_, 0xFF000000u, 1.0f);
    sceGuDisable(GU_FOG);
    sceGuViewport(2048, 2048, pp::kScreenW, pp::kScreenH);
    sceGuScissor(0, 0, pp::kScreenW, pp::kScreenH);
    sceGuTexFlush();
}

void GlueCharacterModel::drawInScene(const GlueScene& s, float extraShiftX) {
    if (!model_.loaded()) return;
    const glm::vec3 eye(s.eye[0], s.eye[1], s.eye[2]);
    const glm::vec3 target(s.target[0], s.target[1], s.target[2]);
    const glm::vec3 feet(s.stand[0], s.stand[1], s.stand[2]);
    glm::vec3 fwd = target - eye;
    fwd = glm::length(fwd) > 1e-4f ? glm::normalize(fwd) : glm::vec3(-1.0f, 0.0f, 0.0f);
    const float fovyDeg = s.fovy * 180.0f / 3.14159265f;
    // Only the character is drawn (depth cleared): a depth range around it, not the scene's 0.22..600.
    constexpr float kNear = 0.2f, kFar = 60.0f;

    sceGuClearDepth(0);
    sceGuClear(GU_DEPTH_BUFFER_BIT);
    // PSP: the picture was rendered with the projection centre moved by shiftX (the feet left of the list).
    sceGuViewport(2048 + static_cast<int>(std::lround(s.shiftX + extraShiftX)), 2048, pp::kScreenW, pp::kScreenH);
    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadIdentity();
    sceGumPerspective(fovyDeg, static_cast<float>(pp::kScreenW) / pp::kScreenH, kNear, kFar);
    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();
    ScePspFVector3 e = {0.0f, 0.0f, 0.0f};
    ScePspFVector3 t = {fwd.x, fwd.y, fwd.z};
    ScePspFVector3 up = {0.0f, 0.0f, 1.0f};
    sceGumLookAt(&e, &t, &up);
    sceGumUpdateMatrix();
    rp::TerrainCamera cam;
    cam.pos = eye;  // the model draws camera-relative
    cam.forward = fwd;
    cam.fovyDeg = fovyDeg;
    cam.nearZ = kNear;
    cam.farZ = kFar;
    // The scene's light on the character (one colour over its baked vertex light: PSP models carry no
    // normals) and its fog (CharModelFogInfo: near 0, far per race; none = ClearFog).
    rp::FrameLight& fl = rp::frameLight();
    const rp::FrameLight saved = fl;
    fl.tint = rp::kTintGeAmbient;
    fl.twoX = false;
    fl.ambientABGR = rp::abgrFrom(glm::vec3(s.tint[0], s.tint[1], s.tint[2]));
    fl.fogBands = s.fog[3] > 0.0f;
    fl.fogStart = 0.0f;
    fl.fogEnd = s.fog[3];
    fl.fogABGR = rp::abgrFrom(glm::vec3(s.fog[0], s.fog[1], s.fog[2]));
    model_.draw(cam, feet, s.yaw + yaw_, 0xFF000000u, 1.0f);
    rp::lightEnd();
    fl = saved;
    sceGuDisable(GU_FOG);
    sceGuViewport(2048, 2048, pp::kScreenW, pp::kScreenH);
    sceGuScissor(0, 0, pp::kScreenW, pp::kScreenH);
    sceGuTexFlush();
}

} // namespace wowee::ui::psp
