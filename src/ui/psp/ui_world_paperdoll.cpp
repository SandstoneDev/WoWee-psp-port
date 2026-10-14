// The character window of the original - CharacterFrame.xml and
// PaperDollFrame.xml / .lua (the windows exactly as the original).
//
//   - the frame composed by tools/psp/build_ui_pak.py (pd_frame: the
//     CharacterTab quarters, the two stat blocks, the resistance icons) at
//     0.6 x 0.55 of its units, positions through PX / PY;
//   - the portrait (the race portrait of the unit frames), the name
//     (GameFontNormal) and CharacterLevelText (PLAYER_LEVEL with the race and
//     the class, ruRU through the client's declension);
//   - the 19 PaperDollItemSlotButtons in the original's columns with their
//     empty-slot art (UI-PaperDoll-Slot-*), the items' icons;
//   - CharacterModelFrame: our model rendered into VRAM after the 3D pass
//     (like the NPC portraits), the rotate buttons;
//   - CharacterResistanceFrame: the five resistances;
//   - CharacterAttributesFrame: two stat blocks (PlayerStatFrameLeft/Right,
//     PLAYERSTAT_* as the dropdowns: Cross on a header cycles it), the left
//     one PLAYERSTAT_BASE_STATS, the right one by class (PaperDollFrame_OnEvent);
//   - the close button and the tabs (CHARACTER / REPUTATION_ABBR / SKILLS_ABBR; the Reputation and
//     Skills tabs are ReputationFrame / SkillFrame of ui_world_char_tabs.cpp).
// Actions on the slots stay the old ones (ui_world_inventory.cpp: Cross
// takes an item off, Square moves it).
// CharacterModelFrame as the original's PlayerModel - its own Stand pose (it stands
// while the character runs, sits, rides or dances), the model frame's light (not the hour's), the rotation of
// UIParent.lua (Model_OnLoad 0.61, the buttons held turn it half a turn a second, Model_OnUpdate), re-dressed the
// frame after the inventory changes (UNIT_MODEL_CHANGED -> SetUnit: the 250 ms poll of syncOwnEquipment runs
// every frame while the window is open); SHOW_CLOAK / SHOW_HELM (ui_world_dressup.cpp).
#include "ui/psp/ui_world_screen.hpp"

#include "game/update_field_table.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"
#include "platform/psp/psp_texture.hpp"
#include "rendering/psp/psp_lighting_ge.hpp"

#include <pspgu.h>
#include <pspgum.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

// Widget ids: 0x6D00-0x6DFF.
enum : uint32_t {
    kIdClose = 0x6D00,
    kIdRotLeft,
    kIdRotRight,
    kIdStatLeft,
    kIdStatRight,
    kIdTab1,
    kIdTab3,
    kIdTab4,
};
constexpr uint32_t kIdInvEquip0 = 0x6010;  // ui_world_inventory.cpp

constexpr float kSX = 0.6f, kSY = 0.55f;   // tools/psp/build_ui_pak.py PD_SX / PD_SY
constexpr float kOx = 4.0f, kOy = 2.0f;    // TOPLEFT 0,-104 -> the left, at the top
constexpr float kFrameW = 211.0f, kFrameH = 265.0f;
float PX(float x) { return kOx + x * kSX; }
float PY(float y) { return kOy + y * kSY; }
constexpr float kSlot = 21.0f;             // ItemButton 37 units, square

constexpr int kLeftCol[8] = {0, 1, 2, 14, 4, 3, 18, 8};    // Head .. Wrist (EquipSlot)
constexpr int kRightCol[8] = {9, 5, 6, 7, 10, 11, 12, 13};  // Hands .. Trinket1
constexpr int kWeapons[3] = {15, 16, 17};
// ui.pak pd_slot_* per EquipSlot (build_ui_pak.py PD_SLOTS order).
constexpr const char* kSlotArt[19] = {"head",   "neck",    "shoulder", "shirt",   "chest",   "waist",   "legs",
                                      "feet",   "wrists",  "hands",    "finger",  "rfinger", "trinket", "trinket",
                                      "chest",  "mainhand", "secondaryhand", "ranged", "tabard"};

using platform::psp::CS;
constexpr CS kCategory[5] = {CS::PLAYERSTAT_BASE_STATS, CS::PLAYERSTAT_MELEE_COMBAT, CS::PLAYERSTAT_RANGED_COMBAT,
                             CS::PLAYERSTAT_SPELL_COMBAT, CS::PLAYERSTAT_DEFENSES};

float fieldFloat(uint32_t v) {
    float f;
    std::memcpy(&f, &v, sizeof(f));
    return f;
}

}  // namespace

WorldScreen::PaperDollArt::~PaperDollArt() { release(); }

void WorldScreen::PaperDollArt::release() {
    for (pp::Texture* t : {&frame, &rotLeft, &rotRight})
        if (t->pixels) pp::textureRelease(*t);
    for (pp::Texture& t : slot)
        if (t.pixels) pp::textureRelease(t);
    tried = false;
}

void WorldScreen::loadPaperDollArt() {
    if (pdArt_.tried && pdArt_.artRetry.due()) pdArt_.tried = false;  // failed - again at the next open
    if (pdArt_.tried) return;
    pdArt_.tried = true;
    UiArtPak art("character");  // ui_art_fail when the pak / a texture finds no memory
    if (!art.isOpen()) {
        pdArt_.artRetry.fail();
        return;
    }
    auto load = [&](const char* name, pp::Texture& t) { art.load(name, t); };
    load("pd_frame", pdArt_.frame);
    load("pd_rot_left", pdArt_.rotLeft);
    load("pd_rot_right", pdArt_.rotRight);
    char name[48];
    for (int s = 0; s < 19; ++s) {
        std::snprintf(name, sizeof(name), "pd_slot_%s", kSlotArt[s]);
        load(name, pdArt_.slot[s]);
    }
    load("pd_slot_bag", pdArt_.slot[19]);
    load("pd_slot_ammo", pdArt_.slot[20]);
    if (!tabArtTried_) {
        tabArtTried_ = true;
        load("tab_active", tabArt_[0]);
        load("tab_inactive", tabArt_[1]);
    }
    if (art.failed()) {
        pdArt_.artRetry.fail();
        if (!tabArt_[0].pixels || !tabArt_[1].pixels) tabArtTried_ = false;
    }
    art.close();
    PSP_LOGI(pp::tag::Gfx, "paperdoll_art frame=%d slots=%d tabs=%d retry=%d", pdArt_.frame.pixels ? 1 : 0,
             pdArt_.slot[0].pixels ? 1 : 0, tabArt_[0].pixels ? 1 : 0, art.failed() ? 1 : 0);
}

float WorldScreen::drawFrameTab(UiRenderer& ui, uint32_t id, float x, float y, float h, const char* label,
                                bool selected, bool focusable) {
    const Font& fs = kit_.font(kFontSmall);
    const float cap = 20.0f * kSX;  // the template's 20-unit ends
    const float w = std::max(60.0f, static_cast<float>(fs.measure(label)) + 2.0f * cap + 4.0f);
    const float ty = y - (selected ? 5.0f * kSY : 0.0f);
    const pp::Texture& t = tabArt_[selected ? 0 : 1];
    if (t.pixels) {
        const float tw = static_cast<float>(t.uw), th = static_cast<float>(t.uh), ct = tw * 20.0f / 128.0f;
        ui.image(t, Rect{x, ty, cap, h}, SrcRect{0, 0, ct, th});
        ui.image(t, Rect{x + cap, ty, w - 2 * cap, h}, SrcRect{ct, 0, tw - 2 * ct, th});
        ui.image(t, Rect{x + w - cap, ty, cap, h}, SrcRect{tw - ct, 0, ct, th});
    }
    const Rect hit{x + 4, ty + 2, w - 8, h - 4};
    const bool f = focusable && winFocus_.addStop(id, hit);
    // GameFontNormalSmall gold, the selected one GameFontHighlightSmall (white); text a little above the middle.
    ui.textShadow(fs, x + w * 0.5f, ty + (h - static_cast<float>(fs.lineHeight())) * 0.5f - 1.0f, label,
                  selected ? kColWhite : kColGold, Align::Center);
    if (f) ui.fill(hit, 0x30FFFFFFu);
    return w;
}

// After the 3D pass: our model into its VRAM target, framed whole, turned to the viewer.
void WorldScreen::renderPaperDollModel(const rendering::psp::TerrainCamera& cam) {
    pdModelOk_ = false;
    if (!pdModelVram_ || !windowOpen(kWinCharacter) || !player_.loaded() || !pdArt_.frame.pixels) return;
    const float h = std::max(0.8f, player_.height());
    const glm::vec3 feet(playerX_, playerY_, shownZ_);
    // The camera in front of the character (its facing), the model turned by the rotate buttons.
    // the frame's rotation (Model_OnLoad 0.61; + = towards the viewer's left).
    const float yaw = facing_ - pdSpin_.rotation;
    const glm::vec3 front(std::cos(facing_), std::sin(facing_), 0.0f);
    const glm::vec3 centre = feet + glm::vec3(0.0f, 0.0f, h * 0.5f);
    const float dist = h * 0.5f / std::tan(20.0f * 3.14159265f / 180.0f) * 1.18f;
    const glm::vec3 eye = centre + front * dist;

    sceGuDrawBufferList(GU_PSM_5650, reinterpret_cast<void*>(static_cast<uintptr_t>(pdModelVram_)), 128);
    sceGuOffset(2048 - 64, 2048 - 64);
    sceGuViewport(2048, 2048, 128, 128);
    sceGuScissor(0, 0, 128, 128);
    sceGuClearColor(0xFF0D0D0Du);  // the model frame's background (UI-Character-CharacterTab-L1 average)
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadIdentity();
    sceGumPerspective(40.0f, 1.0f, 0.1f, 60.0f);
    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();
    rendering::psp::TerrainCamera mc = cam;
    mc.pos = eye;
    ScePspFVector3 e = {0.0f, 0.0f, 0.0f};
    ScePspFVector3 t = {centre.x - eye.x, centre.y - eye.y, centre.z - eye.z};
    ScePspFVector3 up = {0.0f, 0.0f, 1.0f};
    sceGumLookAt(&e, &t, &up);
    sceGumUpdateMatrix();
    // the model frame's own light (the PSP model's baked one, no tint of the hour, no fog bands)
    // and its own Stand pose (PlayerModel plays Stand; the world's animation is left as it is).
    rendering::psp::FrameLight& fl = rendering::psp::frameLight();
    const rendering::psp::FrameLight savedLight = fl;
    fl.tint = rendering::psp::kTintOff;
    fl.fogBands = false;
    fl.twoX = false;
    player_.drawStand(mc, feet, yaw, 0xFF0D0D0Du, static_cast<float>(pp::logUptimeMs() % 3600000u));
    fl = savedLight;
    sceGuDisable(GU_FOG);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    pp::textureResetBinding();
    sceGuDrawBufferList(GU_PSM_5551, pp::displayDrawBuffer(), 512);
    sceGuOffset(2048 - pp::kScreenW / 2, 2048 - pp::kScreenH / 2);
    sceGuViewport(2048, 2048, pp::kScreenW, pp::kScreenH);
    sceGuScissor(0, 0, pp::kScreenW, pp::kScreenH);
    sceGuTexFlush();
    sceGuTexSync();  // texture reads wait for this render target's drawing (GE Users Manual §13.8)
    pdModelOk_ = true;
}

bool WorldScreen::drawPaperDoll(UiRenderer& ui, bool focusable) {
    // ReputationFrame / SkillFrame instead of PaperDollFrame (CharacterFrame_ShowSubFrame);
    // the paperdoll's art goes while another tab is shown.
    if (charTab_ != kCharTabPaperDoll) {
        if (pdArt_.tried) pdArt_.release();
        if (charTab_ == kCharTabPet) return drawPetPaperDoll(ui, focusable);  // PetPaperDollFrame
        return drawCharTab(ui, focusable);
    }
    loadPaperDollArt();
    if (!pdArt_.frame.pixels) return false;
    PROF_SCOPE("inventory.paperdoll");
    // PaperDollFrame_OnEvent UNIT_MODEL_CHANGED -> SetUnit("player"): what is put on or taken off is
    // on the model the next frame (syncOwnEquipment's 250 ms poll runs every frame while the window is open).
    ownEquipCheckMs_ = 0;
    loadNpcArt(false);
    const Font& fs = kit_.font(kFontSmall);
    const Font& fn = kit_.font(kFontNormal);
    const float lh = static_cast<float>(fs.lineHeight());
    const auto& inv = session_.getInventory();
    auto F = [&](game::UF f, int add = 0) {
        return session_.selfField(static_cast<uint16_t>(game::fieldIndex(f) + add));
    };
    const game::Character* me = session_.activeCharacter();

    // CharacterFramePortrait (60 x 60 at 7,-6) under the frame.
    {
        const Rect pr{PX(7), PY(6), 60 * kSX, 60 * kSY};
        const int race = me ? static_cast<int>(me->race) : 0, sex = me && me->gender == game::Gender::FEMALE ? 1 : 0;
        // Round, as SetPortraitTexture.
        if (race > 0 && race < 12 && ufArt_.portrait[race][sex].pixels) ui.imageCircle(ufArt_.portrait[race][sex], pr);
        else ui.fillCircle(Rect{pr.x + 2, pr.y + 2, pr.w - 4, pr.h - 4}, 0xFF101010u);
    }
    ui.image(pdArt_.frame, Rect{kOx, kOy, kFrameW, kFrameH});
    // CharacterModelFrame (TOPLEFT 65,-78, 233 x 215): the model over the frame's background, square in the
    // middle of it (the target is cleared to that background's colour).
    const Rect model{PX(65), PY(78), 233 * kSX, 215 * kSY};
    if (pdModelOk_ && pdModelTex_.pixels) {
        // Left of CharacterResistanceFrame (x 265): the square that fits there, the model's full height.
        const float side = std::min(PX(263) - model.x, model.h);
        ui.image(pdModelTex_, Rect{model.x, model.y + (model.h - side) * 0.5f, side, side});
    }

    // CharacterNameText (CENTER 6,232 = 198,24) and CharacterLevelText under it (PLAYER_LEVEL).
    ui.textShadow(fn, PX(198), PY(24) - static_cast<float>(fn.lineHeight()) * 0.5f, name_.c_str(), kColGold,
                  Align::Center);
    {
        // PLAYER_LEVEL (level, race, class); $R / $C and the ruRU declension |3-6() by questText.
        const std::string b = CLIENT_FMT(PLAYER_LEVEL, static_cast<unsigned>(F(game::UF::UNIT_FIELD_LEVEL)), "$R", "$C");
        ui.textShadow(fs, PX(198), PY(24) + 6 * kSY + 2, questText(b).c_str(), kColGold, Align::Center);
    }
    // CharacterFrameCloseButton (CENTER at TOPRIGHT -44,-25).
    {
        const Rect cr{PX(340) - 10, PY(25) - 10, 20, 20};
        const bool f = focusable && winFocus_.addStop(kIdClose, cr);
        const pp::Texture& t = f && winFocus_.isPressed(kIdClose) && npcArt_.closeDown.pixels ? npcArt_.closeDown
                                                                                              : npcArt_.close;
        if (t.pixels) ui.image(t, cr);
        if (f) ui.fill(cr, 0x40FFFFFFu);
    }
    // The rotate buttons (35 x 35 at the model's TOPLEFT).
    {
        const Rect rl{model.x, model.y, 35 * kSY, 35 * kSY}, rr{model.x + 35 * kSY, model.y, 35 * kSY, 35 * kSY};
        if (pdArt_.rotLeft.pixels) ui.image(pdArt_.rotLeft, rl);
        if (pdArt_.rotRight.pixels) ui.image(pdArt_.rotRight, rr);
        if (focusable && winFocus_.addStop(kIdRotLeft, rl)) ui.fill(rl, 0x40FFFFFFu);
        if (focusable && winFocus_.addStop(kIdRotRight, rr)) ui.fill(rr, 0x40FFFFFFu);
        modelSpinTick(pdSpin_, kIdRotLeft, kIdRotRight);  // Model_OnUpdate (a button held turns it)
    }
    {  // SHOW_CLOAK / SHOW_HELM in the model's top right (ui_world_dressup.cpp)
        const float side = std::min(PX(263) - model.x, model.h);
        drawShowChecks(ui, Rect{model.x, model.y + (model.h - side) * 0.5f, side, side}, focusable);
    }

    // The slots.
    auto slot = [&](int s, float ux, float uy) {
        const Rect r{PX(ux), PY(uy), kSlot, kSlot};
        const uint32_t id = kIdInvEquip0 + static_cast<uint32_t>(s);
        const auto& is = inv.getEquipSlot(static_cast<game::EquipSlot>(s));
        const bool f = focusable && winFocus_.addStop(id, r);
        uint8_t hb = 0, hs = 0;
        const bool held = invHeld_.active && inventoryIdToWire(id, hb, hs) && hb == invHeld_.bag && hs == invHeld_.slot;
        if (!is.empty()) {
            drawItemButton(ui, r, is.item.displayInfoId, 1, -1, held ? 0xFF606060u : 0xFFFFFFFFu);
        } else {
            if (pdArt_.slot[s].pixels) ui.image(pdArt_.slot[s], r);
            if (spellArt_.slot.pixels) {
                const float c = r.w * 64.0f / 37.0f;
                ui.image(spellArt_.slot, Rect{r.x + r.w * 0.5f - c * 0.5f, r.y + r.h * 0.5f - c * 0.5f, c, c});
            }
        }
        if (f) {
            ui.fill(r, 0x50FFFFFFu);
            if (!is.empty()) {
                invTipId_ = id;
                invTipRect_ = Rect{r.x + ui.offsetX(), r.y + ui.offsetY(), r.w, r.h};  // screen: drawn after the windows
            }
        }
    };
    for (int i = 0; i < 8; ++i) {
        slot(kLeftCol[i], 21, 74 + static_cast<float>(i) * 41);   // CharacterHeadSlot TOPLEFT 21,-74, -4 apart
        slot(kRightCol[i], 305, 74 + static_cast<float>(i) * 41);  // CharacterHandsSlot TOPLEFT 305,-74
    }
    for (int i = 0; i < 3; ++i) slot(kWeapons[i], 122 + static_cast<float>(i) * 42, 385);  // MainHand BOTTOMLEFT 122,127

    // CharacterResistanceFrame: arcane, fire, nature, frost, shadow (MagicResFrame ids 6, 2, 3, 4, 5).
    {
        static const int kRes[5] = {6, 2, 3, 4, 5};
        for (int k = 0; k < 5; ++k) {
            char b[12];
            std::snprintf(b, sizeof(b), "%d", static_cast<int>(F(game::UF::UNIT_FIELD_RESISTANCES, kRes[k])));
            ui.textShadow(fs, PX(281), PY(77 + 29 * k + 26) - lh + 1, b, kColWhite, Align::Center);
        }
    }

    // CharacterAttributesFrame: the two stat blocks.
    if (pdStatRight_ < 0) {
        // The client's CVars playerStatLeftDropdown / playerStatRightDropdown (kept between sessions).
        pdStatLeft_ = std::clamp(pp::configGetInt("statleft", pdStatLeft_), 0, 4);
        pdStatRight_ = std::clamp(pp::configGetInt("statright", -1), -1, 4);
    }
    if (pdStatRight_ < 0) {
        const int cls = me ? static_cast<int>(me->characterClass) : 1;
        pdStatRight_ = (cls == 8 || cls == 5 || cls == 9 || cls == 11) ? 3 : cls == 3 ? 2 : 1;  // MAGE PRIEST WARLOCK DRUID / HUNTER
    }
    auto statBlock = [&](uint32_t headerId, int category, float ux) {
        // PlayerStatFrame*DropDown: the category over the block; Cross cycles it.
        const Rect hr{PX(ux + 4), PY(291 - 16), 107 * kSX, 14 * kSY + 4};
        const bool hf = focusable && winFocus_.addStop(headerId, hr);
        ui.pushClip(Rect{hr.x, hr.y, hr.w - 11, hr.h});
        ui.textShadow(fs, hr.x + 2, hr.y + (hr.h - lh) * 0.5f, pp::cs(kCategory[category]), hf ? kColWhite : kColGold);
        ui.popClip();
        if (npcArt_.scrDown.pixels) ui.image(npcArt_.scrDown, Rect{hr.x + hr.w - 10, hr.y + (hr.h - 9) * 0.5f, 9, 9});
        if (hf) ui.fill(hr, 0x30FFFFFFu);
        char lines[6][2][48] = {};
        auto set = [&](int i, const char* label, const char* fmt, double v1, double v2 = 0) {
            std::snprintf(lines[i][0], sizeof(lines[i][0]), "%s:", label);  // STAT_FORMAT
            std::snprintf(lines[i][1], sizeof(lines[i][1]), fmt, v1, v2);
        };
        const int ci = static_cast<int>(game::fieldIndex(game::UF::PLAYER_FIELD_COMBAT_RATING_1));
        auto rating = [&](int cr) { return static_cast<double>(session_.selfField(static_cast<uint16_t>(ci + cr))); };
        switch (category) {
            case 0: {
                const char* const kStat[5] = {CLIENT_STR(SPELL_STAT1_NAME), CLIENT_STR(SPELL_STAT2_NAME),
                                              CLIENT_STR(SPELL_STAT3_NAME), CLIENT_STR(SPELL_STAT4_NAME),
                                              CLIENT_STR(SPELL_STAT5_NAME)};
                for (int i = 0; i < 5; ++i)
                    set(i, kStat[i], "%.0f", static_cast<double>(static_cast<int32_t>(F(game::UF::UNIT_FIELD_STAT0, i))));
                set(5, CLIENT_STR(ARMOR), "%.0f", static_cast<double>(static_cast<int32_t>(F(game::UF::UNIT_FIELD_RESISTANCES))));
                break;
            }
            case 1: {
                const double lo = fieldFloat(F(game::UF::UNIT_FIELD_MINDAMAGE)), hi = fieldFloat(F(game::UF::UNIT_FIELD_MAXDAMAGE));
                set(0, CLIENT_STR(DAMAGE), "%.0f - %.0f", lo, hi);
                // UNIT_FIELD_BASEATTACKTIME = UNIT_FIELD_BOUNDINGRADIUS - 3 (UpdateFields.h 3.3.5a).
                const uint32_t ms = session_.selfField(static_cast<uint16_t>(game::fieldIndex(game::UF::UNIT_FIELD_BOUNDINGRADIUS) - 3));
                set(1, CLIENT_STR(SPEED), "%.2f", ms / 1000.0);
                const double ap = static_cast<int32_t>(F(game::UF::UNIT_FIELD_ATTACK_POWER)) +
                                  static_cast<int32_t>(F(game::UF::UNIT_FIELD_ATTACK_POWER, 1));
                set(2, CLIENT_STR(ATTACK_POWER_TOOLTIP), "%.0f", ap);
                set(3, CLIENT_STR(COMBAT_RATING_NAME6), "%.0f", rating(5));                                       // CR_HIT_MELEE
                set(4, CLIENT_STR(MELEE_CRIT_CHANCE), "%.2f%%", fieldFloat(F(game::UF::PLAYER_CRIT_PERCENTAGE)));     // MELEE_CRIT_CHANCE
                set(5, CLIENT_STR(STAT_EXPERTISE), "%.0f", static_cast<double>(F(game::UF::PLAYER_EXPERTISE)));
                break;
            }
            case 2: {
                const int rap = static_cast<int>(game::fieldIndex(game::UF::UNIT_FIELD_RANGED_ATTACK_POWER));
                const double lo = fieldFloat(session_.selfField(static_cast<uint16_t>(rap + 3)));
                const double hi = fieldFloat(session_.selfField(static_cast<uint16_t>(rap + 4)));
                set(0, CLIENT_STR(DAMAGE), "%.0f - %.0f", lo, hi);
                const uint32_t ms = session_.selfField(static_cast<uint16_t>(game::fieldIndex(game::UF::UNIT_FIELD_BOUNDINGRADIUS) - 1));
                set(1, CLIENT_STR(SPEED), "%.2f", ms / 1000.0);
                set(2, CLIENT_STR(ATTACK_POWER_TOOLTIP), "%.0f", static_cast<double>(static_cast<int32_t>(session_.selfField(static_cast<uint16_t>(rap))) +
                                                                  static_cast<int32_t>(session_.selfField(static_cast<uint16_t>(rap + 1)))));
                set(3, CLIENT_STR(COMBAT_RATING_NAME7), "%.0f", rating(6));                                      // CR_HIT_RANGED
                set(4, CLIENT_STR(RANGED_CRIT_CHANCE), "%.2f%%", fieldFloat(F(game::UF::PLAYER_RANGED_CRIT_PERCENTAGE)));
                break;
            }
            case 3: {
                uint32_t dmg = ~0u;
                float crit = 1e9f;
                for (int s = 1; s < 7; ++s) {
                    dmg = std::min(dmg, F(game::UF::PLAYER_FIELD_MOD_DAMAGE_DONE_POS, s));
                    crit = std::min(crit, fieldFloat(F(game::UF::PLAYER_SPELL_CRIT_PERCENTAGE1, s)));
                }
                set(0, CLIENT_STR(BONUS_DAMAGE), "%.0f", static_cast<double>(dmg));                              // BONUS_DAMAGE
                set(1, CLIENT_STR(BONUS_HEALING), "%.0f", static_cast<double>(F(game::UF::PLAYER_FIELD_MOD_HEALING_DONE_POS)));
                set(2, CLIENT_STR(COMBAT_RATING_NAME8), "%.0f", rating(7));                                       // CR_HIT_SPELL
                set(3, CLIENT_STR(SPELL_CRIT_CHANCE), "%.2f%%", crit);
                set(4, CLIENT_STR(SPELL_HASTE_ABBR), "%.0f", rating(19));                                            // SPELL_HASTE_ABBR
                set(5, CLIENT_STR(MANA_REGEN_ABBR), "%.0f", fieldFloat(F(game::UF::UNIT_FIELD_POWER_REGEN_FLAT_MODIFIER)) * 5.0);
                break;
            }
            default: {
                set(0, CLIENT_STR(ARMOR), "%.0f", static_cast<double>(static_cast<int32_t>(F(game::UF::UNIT_FIELD_RESISTANCES))));
                set(1, CLIENT_STR(DEFENSE), "%.0f", rating(1));
                set(2, CLIENT_STR(DODGE), "%.2f%%", fieldFloat(F(game::UF::PLAYER_DODGE_PERCENTAGE)));
                set(3, CLIENT_STR(PARRY), "%.2f%%", fieldFloat(F(game::UF::PLAYER_PARRY_PERCENTAGE)));
                set(4, CLIENT_STR(STAT_BLOCK), "%.2f%%", fieldFloat(F(game::UF::PLAYER_BLOCK_PERCENTAGE)));
                set(5, CLIENT_STR(STAT_RESILIENCE), "%.0f", rating(14));
                break;
            }
        }
        // StatFrameTemplate: 104 x 13, label GameFontNormalSmall (gold) left, value GameFontHighlightSmall right.
        // PSP: the 8 px font (13 units = 7 px rows); the label is cut before the value.
        const Font& ft = kit_.font(kFontTiny);
        const float th = static_cast<float>(ft.lineHeight());
        const float x0 = PX(ux + 6), x1 = PX(ux + 6 + 104);
        for (int i = 0; i < 6; ++i) {
            if (!lines[i][0][0]) continue;
            const float y = PY(291 + 3 + 13 * static_cast<float>(i)) + (13 * kSY - th) * 0.5f;
            const float vw = static_cast<float>(ft.measure(lines[i][1]));
            ui.pushClip(Rect{x0, y - 1, std::max(0.0f, x1 - x0 - vw - 2), th + 2});
            ui.text(ft, x0, y, lines[i][0], kColGold);
            ui.popClip();
            ui.text(ft, x1 - vw, y, lines[i][1], kColWhite);
        }
    };
    statBlock(kIdStatLeft, pdStatLeft_, 67);
    statBlock(kIdStatRight, pdStatRight_, 67 + 115);

    drawCharacterFrameTabs(ui, focusable);
    return true;
}

// CharacterFrameTab1 BOTTOMLEFT 11,46; no pet: Tab3 takes Tab2's place, Tab4 after it -15. the
// shown tab selected (PanelTemplates_SetTab), for all three subframes.
void WorldScreen::drawCharacterFrameTabs(UiRenderer& ui, bool focusable) {
    if (!tabArtTried_) {
        tabArtTried_ = true;
        char path[256];
        pp::fsPath(path, sizeof(path), "data/ui.pak");
        pp::Pak pak;
        if (pak.open(path)) {
            for (int i = 0; i < 2; ++i) {
                const pp::PakEntry* e = pak.find(i == 0 ? "tab_active" : "tab_inactive");
                void* blob = e ? pak.load(e) : nullptr;
                if (blob && !pp::textureFromBlob(blob, e->size, tabArt_[i], true)) pp::pakFree(blob);
            }
            pak.close();
        }
    }
    const float th = 32 * kSY, ty = PY(512 - 46) - th;
    float x = PX(11);
    x += drawFrameTab(ui, kIdTab1, x, ty, th, CLIENT_STR(CHARACTER), charTab_ == kCharTabPaperDoll, focusable) - 15 * kSX;  // CHARACTER
    if (petTabShown())  // CharacterFrameTab2 (PET) while HasPetUI (CharacterFrame_UpdateTabs)
        x += drawFrameTab(ui, kIdCharTabPet, x, ty, th, CLIENT_STR(PET), charTab_ == kCharTabPet, focusable) - 15 * kSX;
    x += drawFrameTab(ui, kIdTab3, x, ty, th, CLIENT_STR(REPUTATION_ABBR), charTab_ == kCharTabRep, focusable) - 15 * kSX;  // REPUTATION_ABBR
    drawFrameTab(ui, kIdTab4, x, ty, th, CLIENT_STR(SKILLS_ABBR), charTab_ == kCharTabSkills, focusable);                  // SKILLS_ABBR
}

bool WorldScreen::paperDollAction(uint32_t act) {
    if (act < 0x6D00 || act > 0x6DFF) return false;
    switch (act) {
        case kIdClose: closeWindow(kWinCharacter); break;
        case kIdRotLeft: modelSpinClick(pdSpin_, true); break;    // Model_RotateLeft (was -0.5 rad)
        case kIdRotRight: modelSpinClick(pdSpin_, false); break;  // Model_RotateRight
        case kIdStatLeft:
            pdStatLeft_ = (pdStatLeft_ + 1) % 5;
            pp::configSet("statleft", std::to_string(pdStatLeft_));  // SetCVar
            pp::configSave();
            break;
        case kIdStatRight:
            pdStatRight_ = (pdStatRight_ + 1) % 5;
            pp::configSet("statright", std::to_string(pdStatRight_));
            pp::configSave();
            break;
        // CharacterFrameTab_OnClick -> ToggleCharacter (the selected tab is disabled: nothing).
        case kIdTab1: selectCharTab(kCharTabPaperDoll); break;
        case kIdTab3: selectCharTab(kCharTabRep); break;
        case kIdTab4: selectCharTab(kCharTabSkills); break;
        default: charTabAction(act); break;  // the Reputation / Skills tabs' ids (0x6D40..)
    }
    return true;
}

}  // namespace wowee::ui::psp
