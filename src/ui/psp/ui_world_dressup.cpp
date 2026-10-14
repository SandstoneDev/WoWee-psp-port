// The dressing room of the original ruRU 3.3.5a - DressUpFrame.xml / DressUpFrame.lua
// - and the character window's model controls.
//
// DressUpFrame (UIPanelWindows: area "left", pushable 2 - the PSP places it beside the open panels):
//   - the frame: UI-Character-General-TopLeft / -TopRight and SkillFrame-BotLeft / -BotRight with
//     DressUpBackground-<race>1..4 (DressUpTexturePath: gnomes take the dwarves', trolls the orcs') at 22,-76 -
//     one composite per race (ui.pak "du_frame_<race>", tools/psp/build_ui_pak.py dressup_frames) cut and scaled
//     like the character window (0.6 x 0.55: 211 x 265 px), T8; DressUpFramePortrait (SetPortraitTexture "player":
//     the race portrait, round), DressUpFrameTitleText (DRESSUP_FRAME, GameFontHighlight),
//     DressUpFrameDescriptionText (DRESSUP_FRAME_INSTRUCTIONS - PSP: the PSP keys instead of "Ctrl + the left
//     mouse button"), DressUpFrameCloseButton, RESET (DressUpModel:Dress(), gsTitleOptionOK) and
//     CLOSE (HideParentPanel), the sounds igCharacterInfoOpen / igCharacterInfoClose (OnShow / OnHide);
//   - DressUpModel (316 x 331 at BOTTOM -11,105): the player as SetUnit("player") shows him (what player_ wears)
//     plus what TryOn put on, in its own Stand animation (with the idle variations), weapons in the hands;
//     DressUpModelRotateLeftButton / -RightButton: Model_RotateLeft / Right (0.03 a click) and Model_OnUpdate
//     (a button held turns it half a turn a second); Model_OnLoad's 0.61;
//   - DressUpItemLink(link): nothing unless IsDressableItem (upstream lua_IsDressableItem: a weapon or armour);
//     the frame not shown -> ShowUIPanel + SetUnit("player"); then TryOn(link).
// TryOn (upstream __WoweeTryOn: the item stands in for what is worn in its place): the item goes to the slot its
// InventoryType is equipped in. PSP (upstream compares the InventoryType alone: a robe over a chest, a two-hander
// over a one-hander stayed beside each other): by the slot, a two-hander empties the off hand, an off-hand item
// empties a two-handed main hand, a ranged weapon empties both hands so the model holds it (the PSP's own-player
// rules show a ranged weapon only with the main hand empty) - not checked against the client's DressUpModel.
// The model is a view of player_ (CharacterModel::loadView): the race model, its skeleton and textures are
// player_'s, only the pose, the body composite (64 KB) and the attachments are its own - ~90 KB instead of a
// second 0.7-1.5 MB "n_" model. It is made when the window opens and freed when it closes (dressUpTick), as the
// frame art (129 KB); log dressup_art / dressup_model / dressup_release, mem at=dressup_open.
//
// The PSP key: L + Triangle on an item under the cursor = the original's Ctrl+click (IsModifiedClick("DRESSUP"):
// HandleModifiedItemClick -> DressUpItemLink) - in the bags, the character window, at a merchant, in the loot
// window, on quest rewards and on an item link in the chat; the item's GameTooltip says so in its last line
// (PSP). L alone stays Shift (the comparison); Triangle alone on the loot window still takes everything.
//
// The character window (ui_world_paperdoll.cpp calls these): the rotate buttons as above (pdSpin_), and the
// check boxes SHOW_CLOAK / SHOW_HELM - InterfaceOptionsDisplayPanelShowCloak / -ShowHelm of the
// original (SHOW_CLOAK / SHOW_HELM, InterfaceOptionsCheckButtonTemplate: UI-CheckBox-Up / -Highlight / -Check,
// igMainMenuOptionCheckBoxOn / Off, the tooltip OPTION_TOOLTIP_SHOW_*); ShowCloak / ShowHelm -> the session
// (CMSG_SHOWING_CLOAK / _HELM, psp_world_session_dressup.cpp). PSP: the original keeps them in Interface ->
// Display, which the PSP has not (one settings dialog, VIDEOOPTIONS_MENU): they sit in the character window's model
// frame, top right (where the original's GearManagerToggleButton is), with the head / back slot art beside them;
// their names are the tooltip.
#include "ui/psp/ui_world_screen.hpp"

#include "game/inventory.hpp"
#include "platform/psp/psp_bigheap.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"
#include "platform/psp/psp_system.hpp"  // logMemory
#include "platform/psp/psp_texture.hpp"
#include "rendering/psp/psp_lighting_ge.hpp"
#include "ui/psp/ui_dressup_rules.hpp"

#include <pspctrl.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspthreadman.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <malloc.h>
#include <string>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
namespace rp = wowee::rendering::psp;

namespace {

// Widget ids: 0x6300..0x63FF.
enum : uint32_t {
    kIdShowCloak = 0x6300,  // the character window's check boxes
    kIdShowHelm,
    kIdDuClose = 0x6310,    // DressUpFrameCloseButton
    kIdDuCancel,            // DressUpFrameCancelButton (CLOSE)
    kIdDuReset,             // DressUpFrameResetButton (RESET)
    kIdDuRotLeft,           // DressUpModelRotateLeftButton
    kIdDuRotRight,
};
// 0x6380..0x639F: the quest frame's item buttons without an id of their own (ui_world_npc_frame.cpp);
// 0x63C0..0x63DF: the chat's item links (WorldChat::kLinkId0).
constexpr uint32_t kIdQuestItem0 = 0x6380, kIdQuestItemEnd = 0x63A0;

constexpr float kPi = 3.14159265f;
constexpr float kSX = 0.6f, kSY = 0.55f;  // the character window's scale (build_ui_pak.py PD_SX / PD_SY)
constexpr float kOx = 4.0f, kOy = 2.0f;   // the left panel area, as CharacterFrame
constexpr float kFrameW = 211.0f, kFrameH = 265.0f;
float PX(float x) { return kOx + x * kSX; }
float PY(float y) { return kOy + y * kSY; }

constexpr uint32_t kTipBack = 0xBB1B0D0Du;  // UI-Tooltip-Background x TOOLTIP_DEFAULT_BACKGROUND_COLOR

// DressUpTexturePath: UnitRace's file name; GNOME -> Dwarf, TROLL -> Orc (the lua's -- HACK), else Orc.
const char* dressUpRaceFile(int race) {
    switch (race) {
        case 1: return "human";
        case 2: return "orc";
        case 3: return "dwarf";
        case 4: return "nightelf";
        case 5: return "scourge";
        case 6: return "tauren";
        case 7: return "dwarf";
        case 8: return "orc";
        case 10: return "bloodelf";
        case 11: return "draenei";
        default: return "orc";
    }
}

void loadUiTexture(pp::Pak& pak, const char* name, pp::Texture& t, uint32_t& bytes) {
    const pp::PakEntry* e = pak.find(name);
    void* blob = e ? pak.load(e) : nullptr;
    if (blob && pp::textureFromBlob(blob, e->size, t, true)) bytes += e->size;
    else if (blob) pp::pakFree(blob);
}

// The model into `r` (window units) now, between the interface's quads: UiRenderer::pushClip flushes them and
// sets the GE's scissor to the rect; there the depth is cleared and the model drawn through a camera in front of
// it, the projection centred on the rect; then the interface's GE state (UiRenderer::beginFrame's) comes back.
// The model frame's own light (no tint of the hour, no fog bands). False when nothing was drawn.
bool drawModelInRect(UiRenderer& ui, rp::CharacterModel& m, const Rect& r, float yaw) {
    const float sc = ui.scale();
    const float sx = (r.x + ui.offsetX()) * sc, sy = (r.y + ui.offsetY()) * sc, sw = r.w * sc, sh = r.h * sc;
    if (!m.loaded() || sw < 4.0f || sh < 4.0f || sx >= pp::kScreenW || sy >= pp::kScreenH || sx + sw <= 0.0f ||
        sy + sh <= 0.0f)
        return false;
    if (pp::displayListFree() < pp::kListReserveNpc) {  // the own-player model is never stopped by draw() itself
        pp::displayListFull("dressup");
        return false;
    }
    ui.pushClip(r);
    const float h = std::max(0.8f, m.height());
    constexpr float kFovY = 30.0f, kNear = 0.1f, kFar = 60.0f;
    const float dist = h * 1.12f * 0.5f / std::tan(kFovY * 0.5f * kPi / 180.0f);  // the height + 6 % each end
    const glm::vec3 centre(0.0f, 0.0f, h * 0.5f);
    const glm::vec3 eye = centre + glm::vec3(dist, 0.0f, 0.0f);  // in front of the model (it faces +x at yaw 0)
    sceGuClearDepth(0);
    sceGuClear(GU_DEPTH_BUFFER_BIT);  // inside the scissor
    sceGuViewport(2048 - pp::kScreenW / 2 + static_cast<int>(std::lround(sx + sw * 0.5f)),
                  2048 - pp::kScreenH / 2 + static_cast<int>(std::lround(sy + sh * 0.5f)),
                  static_cast<int>(std::lround(sw)), static_cast<int>(std::lround(sh)));
    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadIdentity();
    sceGumPerspective(kFovY, sw / sh, kNear, kFar);
    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();
    ScePspFVector3 e = {0.0f, 0.0f, 0.0f};
    ScePspFVector3 t = {centre.x - eye.x, centre.y - eye.y, centre.z - eye.z};
    ScePspFVector3 up = {0.0f, 0.0f, 1.0f};
    sceGumLookAt(&e, &t, &up);
    sceGumUpdateMatrix();
    rp::TerrainCamera cam;
    cam.pos = eye;  // the model draws camera-relative
    cam.forward = glm::vec3(-1.0f, 0.0f, 0.0f);
    cam.fovyDeg = kFovY;
    cam.nearZ = kNear;
    cam.farZ = kFar;
    rp::FrameLight& fl = rp::frameLight();
    const rp::FrameLight saved = fl;
    fl.tint = rp::kTintOff;
    fl.fogBands = false;
    fl.twoX = false;
    m.draw(cam, glm::vec3(0.0f), yaw, 0xFF000000u, 1.0f);  // GE passes "char" / "char.attach"
    PROF_GE_PASS("ui.windows");  // what follows is the windows' again (ui_world_hud.cpp's pass)
    fl = saved;
    sceGuDisable(GU_FOG);
    sceGuViewport(2048, 2048, pp::kScreenW, pp::kScreenH);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuEnable(GU_ALPHA_TEST);
    sceGuAlphaFunc(GU_GREATER, 0, 0xFF);
    sceGuShadeModel(GU_FLAT);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    pp::textureResetBinding();
    ui.popClip();
    return true;
}

// The model's sheath state for what it wears - the ranged weapon with both hands empty
// (dressup::tryOn frees them for it) is held: SHEATH_STATE_RANGED; else the hands' items drawn.
uint8_t dressUpSheathState(const rp::Equipment& e) {
    return (e.displayId[17] && !e.displayId[15] && !e.displayId[16]) ? rp::kSheathStateRanged : rp::kSheathStateMelee;
}

}  // namespace

struct WorldScreen::DressUpUi {
    rp::CharacterModel model;  // DressUpModel: a view of player_ (CharacterModel::loadView)
    rp::Equipment worn;        // SetUnit("player") / Dress(): what player_ wears
    rp::Equipment shown;       // worn + what TryOn put on
    bool modelTried = false;   // the view was made (or failed) for this opening
    uint32_t modelMs = 0;      // its animation clock
    ModelSpin spin;            // DressUpModel's rotation (Model_OnLoad 0.61, kept while the UI lives)
    uint32_t pendingItem = 0, pendingMs = 0;  // an item whose data was asked for (TryOn when it comes)
    pp::Texture frame;         // ui.pak "du_frame_<race>"
    pp::Texture rotL, rotR;    // UI-RotationLeft / -Right-Button-Up (ui.pak pd_rot_*, the character window's)
    std::string frameName;
    bool frameTried = false;
    uint32_t frameBytes = 0;
    std::vector<std::string> desc;  // DRESSUP_FRAME_INSTRUCTIONS (PSP) wrapped
    // The character window's check boxes (InterfaceOptionsCheckButtonTemplate).
    pp::Texture cbUp, cbHl, cbCheck;
    bool cbTried = false;
    uint32_t tipId = 0;        // the check box under the cursor this frame (its GameTooltip after the windows)
    Rect tipRect{};
    uint32_t logs = 0;
    void releaseFrame() {
        for (pp::Texture* t : {&frame, &rotL, &rotR})
            if (t->pixels) pp::textureRelease(*t);
        frame = pp::Texture{};
        frameTried = false;
        frameName.clear();
        frameBytes = 0;
    }
    void releaseChecks() {
        for (pp::Texture* t : {&cbUp, &cbHl, &cbCheck})
            if (t->pixels) pp::textureRelease(*t);
        cbTried = false;
    }
    ~DressUpUi() {
        model.clear();
        releaseFrame();
        releaseChecks();
    }
};

void WorldScreen::destroyDressUpUi(DressUpUi* p) { delete p; }

WorldScreen::DressUpUi& WorldScreen::dressUpUi() {
    if (!du_) du_ = std::unique_ptr<DressUpUi, void (*)(DressUpUi*)>(new DressUpUi(), &destroyDressUpUi);
    return *du_;
}

// --- DressUpItemLink -----------------------------------------------------------------------------------------

int WorldScreen::dressUpResolve(uint32_t itemId, uint32_t& displayId, uint8_t& inventoryType, bool ask) {
    displayId = 0;
    inventoryType = 0;
    if (!itemId) return 0;
    if (const auto* qi = session_.getItemInfo(itemId); qi && qi->valid) {
        // upstream lua_IsDressableItem: the item class as the server sent it - 2 weapon, 4 armour.
        if (qi->itemClass != 2 && qi->itemClass != 4) return 0;
        displayId = qi->displayInfoId;
        inventoryType = static_cast<uint8_t>(qi->inventoryType);
        return displayId && dressup::slotOfInventoryType(inventoryType) >= 0 ? 1 : 0;
    }
    // PSP: the client's Item.dbc (items.pak "u_") answers before any query, as the original client does.
    if (items_.isOpen() && items_.itemDisplay(itemId, displayId, inventoryType))
        return displayId && dressup::slotOfInventoryType(inventoryType) >= 0 ? 1 : 0;
    if (ask) session_.ensureItemInfo(itemId);
    return -1;
}

bool WorldScreen::dressableItem(uint32_t itemId) {
    static const bool on = pp::configGetInt("dressup", 1) != 0;  // wowee.cfg dressup=0: no dressing room (A/B)
    if (!on || !player_.loaded()) return false;
    uint32_t d = 0;
    uint8_t t = 0;
    return dressUpResolve(itemId, d, t, false) > 0;
}

// "L + [Triangle]" in a line `h` high at x, y; the width it takes (ui null: the width only).
float WorldScreen::dressUpKeys(UiRenderer* ui, float x, float y, float h) {
    const Font& fs = kit_.font(kFontSmall);
    float w = kit_.glyphWidth(kPadL, h);
    if (ui) kit_.glyphFill(*ui, kPadL, Rect{x, y, w, h});
    const float pw = static_cast<float>(fs.measure("+"));
    if (ui) ui->textShadow(fs, x + w + 1.0f, y + (h - static_cast<float>(fs.lineHeight())) * 0.5f, "+", kColWhite);
    w += pw + 3.0f;
    if (ui) kit_.glyphFill(*ui, kPadTriangle, Rect{x + w, y, h, h});
    return w + h;
}

void WorldScreen::dressUpItemLink(uint32_t itemId) {
    if (!itemId || !player_.loaded() || pp::configGetInt("dressup", 1) == 0) return;
    uint32_t displayId = 0;
    uint8_t invType = 0;
    // IsDressableItem; an item not known yet (a server's own item, no answer so far) is asked for, the frame opens
    // and tries it on when the answer comes.
    const int r = dressUpResolve(itemId, displayId, invType, true);
    if (r == 0) return;
    DressUpUi& du = dressUpUi();
    if (!windowOpen(kWinDressUp)) {
        // ShowUIPanel(DressUpFrame); DressUpModel:SetUnit("player").
        du.worn = player_.equipment();
        du.shown = du.worn;
        du.modelTried = false;
        du.pendingItem = 0;
        openWindow(kWinDressUp);
        PSP_LOGI(pp::tag::Ctrl, "dressup_open item=%u", static_cast<unsigned>(itemId));
    }
    if (r < 0) {
        du.pendingItem = itemId;
        du.pendingMs = pp::logUptimeMs();
        return;
    }
    // TryOn: the item in the slot it is worn in, the hands' rules (ui_dressup_rules.hpp).
    const int slot = dressup::tryOn(du.shown.displayId, du.shown.invType, displayId, invType);
    if (slot < 0) return;
    if (du.model.loaded()) {
        du.model.setEquipment(charPak_, du.shown, items_.isOpen() ? &items_ : nullptr);
        du.model.setSheathState(dressUpSheathState(du.shown));
    }
    if (++du.logs <= 40)
        PSP_LOGI(pp::tag::Gfx, "dressup_tryon item=%u display=%u inv=%u slot=%d", static_cast<unsigned>(itemId),
                 static_cast<unsigned>(displayId), static_cast<unsigned>(invType), slot);
}

// L + Triangle = the original's Ctrl+click on the item under the cursor (last frame's: dressUpHover).
void WorldScreen::dressUpPad(pp::PadState& pad) {
    if (!(pad.pressed & PSP_CTRL_TRIANGLE) || !(pad.held & PSP_CTRL_LTRIGGER)) return;
    pad.pressed &= ~PSP_CTRL_TRIANGLE;  // never the loot window's "take all"
    pad.held &= ~PSP_CTRL_TRIANGLE;
    if (duHoverItem_) dressUpItemLink(duHoverItem_);
}

// drawWindows, every frame: what a closed DressUpFrame held goes (its model, its art); the check boxes' art goes
// with the character window.
void WorldScreen::dressUpTick() {
    if (!du_) return;
    DressUpUi& d = *du_;
    d.tipId = 0;
    if (!windowOpen(kWinDressUp) && (d.model.loaded() || d.frame.pixels || d.modelTried)) {
        const uint32_t kb = d.model.memoryKb() + d.frameBytes / 1024u;
        d.model.clear();
        d.releaseFrame();
        d.modelTried = false;
        d.pendingItem = 0;
        PSP_LOGI(pp::tag::Gfx, "dressup_release kb=%u", static_cast<unsigned>(kb));
    }
    if (!windowOpen(kWinCharacter) && d.cbTried) d.releaseChecks();
}

// drawWindows, after the windows: the item under the cursor (a bag / character window slot, a merchant's, loot or
// quest item - invTipId_ / tipProtoItem_ - or the chat's link) for L + Triangle; a chat link gets its GameTooltip.
void WorldScreen::dressUpHover() {
    if (!invTipId_ && !tipProtoItem_) {
        Rect r;
        if (const uint32_t link = chat_.hoverItem(&r)) {
            tipProtoItem_ = link;
            tipProtoCount_ = 1;
            tipProtoRect_ = r;
        }
    }
    // The tooltip of an item the client has not seen yet (a loot / quest / chat item): asked for, shown when it comes.
    if (tipProtoItem_ && !session_.getItemInfo(tipProtoItem_)) session_.ensureItemInfo(tipProtoItem_);
    duHoverItem_ = 0;
    if (invTipId_) {
        if (const game::ItemDef* it = inventoryItemAt(invTipId_)) duHoverItem_ = it->itemId;
    } else {
        duHoverItem_ = tipProtoItem_ ? tipProtoItem_ : inspectHoverItem();  // + an inspected slot
    }
}

// loadCharacter: player_ is about to be loaded again - the view lets go of its blob and textures; the frame makes
// a new one on the next draw.
void WorldScreen::dressUpBaseChanged() {
    if (!du_ || !du_->model.loaded()) return;
    du_->model.clear();
    du_->modelTried = false;
}

// --- The rotate buttons (UIParent.lua) ------------------------------------------------------------------------

void WorldScreen::modelSpinClick(ModelSpin& s, bool left) {
    s.rotation += left ? -0.03f : 0.03f;  // Model_RotateLeft / Model_RotateRight (rotationIncrement 0.03)
    playSoundByName("igInventoryRotateCharacter");
}

// Model_OnUpdate: a rotate button PUSHED turns the model elapsed x 2 pi x ROTATIONS_PER_SECOND (.5); the button's
// OnClick fires on LeftButtonDown too (RegisterForClicks "LeftButtonDown", "LeftButtonUp" - the up is the focus
// ring's activation).
void WorldScreen::modelSpinTick(ModelSpin& s, uint32_t leftId, uint32_t rightId) {
    const uint32_t now = pp::logUptimeMs();
    const float dt = s.lastMs ? std::min(0.1f, static_cast<float>(now - s.lastMs) / 1000.0f) : 0.0f;
    s.lastMs = now;
    const uint32_t pushed = winFocus_.isPressed(leftId) ? leftId : winFocus_.isPressed(rightId) ? rightId : 0u;
    if (pushed && pushed != s.pushed) modelSpinClick(s, pushed == leftId);
    s.pushed = pushed;
    if (pushed == leftId) s.rotation += dt * 2.0f * kPi * 0.5f;
    else if (pushed == rightId) s.rotation -= dt * 2.0f * kPi * 0.5f;
    if (s.rotation < 0.0f) s.rotation += 2.0f * kPi;
    if (s.rotation > 2.0f * kPi) s.rotation -= 2.0f * kPi;
}

// --- DressUpFrame ---------------------------------------------------------------------------------------------

bool WorldScreen::drawDressUpWindow(UiRenderer& ui, uint8_t w) {
    if (w != kWinDressUp) return false;
    PROF_SCOPE("dressup");
    DressUpUi& d = dressUpUi();
    const bool focusable = uiMode_ && !settings_.isOpen() && !windowOpen(kWinBindConfirm);
    const game::Character* me = session_.activeCharacter();
    const int race = me ? static_cast<int>(me->race) : 0;
    const int sex = me && (me->gender == game::Gender::FEMALE ||
                           (me->gender == game::Gender::NONBINARY && me->useFemaleModel)) ? 1 : 0;
    const uint32_t nowMs = pp::logUptimeMs();

    // The frame art of the race (SetDressUpBackground at OnLoad - the player's race).
    char name[32];
    std::snprintf(name, sizeof(name), "du_frame_%s", dressUpRaceFile(race));
    if (!d.frameTried || d.frameName != name) {
        d.releaseFrame();
        d.frameTried = true;
        d.frameName = name;
        char path[256];
        pp::fsPath(path, sizeof(path), "data/ui.pak");
        pp::Pak pak;
        if (pak.open(path)) {
            loadUiTexture(pak, name, d.frame, d.frameBytes);
            loadUiTexture(pak, "pd_rot_left", d.rotL, d.frameBytes);
            loadUiTexture(pak, "pd_rot_right", d.rotR, d.frameBytes);
            pak.close();
        }
        PSP_LOGI(pp::tag::Gfx, "dressup_art name=%s ok=%d kb=%u", name, d.frame.pixels ? 1 : 0,
                 static_cast<unsigned>((d.frameBytes + 1023u) / 1024u));
    }
    loadNpcArt(false);  // UIPanelCloseButton, UIPanelButtonTemplate

    // DressUpModel: the view of player_, made once the frame is up; a pending TryOn; its clock.
    if (!d.modelTried && player_.loaded() && charPak_.isOpen()) {
        d.modelTried = true;
        const uint32_t heap0 = pp::heapUsedBytes();  // both pools
        const uint64_t t0 = sceKernelGetSystemTimeWide();
        d.model.setLocalPlayer(true);         // the own player's attachment rules
        d.model.setSheathState(dressUpSheathState(d.shown));  // weapons in the hands (a ranged one too)
        const bool ok = d.model.loadView(player_, charPak_, d.shown, items_.isOpen() ? &items_ : nullptr);
        d.modelMs = nowMs;
        PSP_LOGI(pp::tag::Gfx, "dressup_model ok=%d model_kb=%u heap_delta_kb=%d ms=%u", ok ? 1 : 0,
                 static_cast<unsigned>(d.model.memoryKb()),
                 static_cast<int>((static_cast<int64_t>(pp::heapUsedBytes()) - heap0) / 1024),
                 static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u));
        pp::logMemory("dressup_open");
    }
    if (d.pendingItem) {
        uint32_t display = 0;
        uint8_t inv = 0;
        const int r = dressUpResolve(d.pendingItem, display, inv, false);
        if (r > 0) {
            const uint32_t item = d.pendingItem;
            d.pendingItem = 0;
            dressUpItemLink(item);
        } else if (r == 0 || nowMs - d.pendingMs > 5000u) {
            d.pendingItem = 0;
        }
    }
    if (d.model.loaded()) {
        d.model.update(std::min(0.1f, static_cast<float>(nowMs - d.modelMs) / 1000.0f));
        d.modelMs = nowMs;
    }

    // DressUpFramePortrait (60 x 60 at 7,-6) under the frame; SetPortraitTexture is round.
    {
        const Rect pr{PX(7), PY(6), 60 * kSX, 60 * kSY};
        if (race > 0 && race < 12 && ufArt_.portrait[race][sex].pixels) ui.imageCircle(ufArt_.portrait[race][sex], pr);
        else ui.fillCircle(Rect{pr.x + 2, pr.y + 2, pr.w - 4, pr.h - 4}, 0xFF101010u);
    }
    const Font& fn = kit_.font(kFontNormal);
    const Font& ft = kit_.font(kFontTiny);
    if (d.frame.pixels) {
        ui.image(d.frame, Rect{kOx, kOy, kFrameW, kFrameH});
    } else {
        kit_.panel(ui, Rect{kOx, kOy, kFrameW, kFrameH});
        ui.textShadow(ft, PX(192), PY(240), OWN_STR(DU_NO_ART), kColGrey, Align::Center);
    }
    // DressUpFrameTitleText (GameFontHighlight, TOP 0,-16, 300 x 14).
    ui.textShadow(fn, PX(192), PY(23) - static_cast<float>(fn.lineHeight()) * 0.5f, CLIENT_STR(DRESSUP_FRAME), kColWhite,
                  Align::Center);  // DRESSUP_FRAME
    // DressUpFrameDescriptionText (GameFontNormalSmall, 260 x 36 centred 10,-22 under the title). PSP: the PSP
    // keys instead of DRESSUP_FRAME_INSTRUCTIONS' Ctrl + left mouse button (its first half: own DU_INSTRUCTIONS).
    {
        const float maxW = 260 * kSX + 8.0f, lh = static_cast<float>(ft.lineHeight());
        if (d.desc.empty())
            d.desc = wrapQuestText(ft, OWN_STR(DU_INSTRUCTIONS), maxW);
        const float keysW = dressUpKeys(nullptr, 0.0f, 0.0f, lh);
        const bool ownLine = !d.desc.empty() && static_cast<float>(ft.measure(d.desc.back().c_str())) + 3.0f + keysW > maxW;
        const size_t rows = d.desc.size() + (ownLine ? 1u : 0u);
        float y = PY(52) - lh * static_cast<float>(rows) * 0.5f;
        for (size_t i = 0; i < d.desc.size(); ++i, y += lh) {
            const bool last = i + 1 == d.desc.size();
            const float tw = static_cast<float>(ft.measure(d.desc[i].c_str())) + (last && !ownLine ? 3.0f + keysW : 0.0f);
            const float x = PX(202) - tw * 0.5f;
            ui.textShadow(ft, x, y, d.desc[i].c_str(), kColGold);
            if (last && !ownLine) dressUpKeys(&ui, x + tw - keysW, y, lh);
        }
        if (ownLine) dressUpKeys(&ui, PX(202) - keysW * 0.5f, y, lh);
    }

    // DressUpModel (316 x 331 at BOTTOM -11,105): the model over the race background.
    const Rect mr{PX(23), PY(76), 316 * kSX, 331 * kSY};
    drawModelInRect(ui, d.model, mr, -d.spin.rotation);
    // DressUpModelRotateLeftButton (35 x 35 at 21,-75) / -RightButton beside it.
    {
        const Rect rl{PX(21), PY(75), 35 * kSY, 35 * kSY}, rr{PX(21) + 35 * kSY, PY(75), 35 * kSY, 35 * kSY};
        if (d.rotL.pixels) ui.image(d.rotL, rl);
        if (d.rotR.pixels) ui.image(d.rotR, rr);
        if (focusable && winFocus_.addStop(kIdDuRotLeft, rl)) ui.fill(rl, 0x40FFFFFFu);
        if (focusable && winFocus_.addStop(kIdDuRotRight, rr)) ui.fill(rr, 0x40FFFFFFu);
        modelSpinTick(d.spin, kIdDuRotLeft, kIdDuRotRight);
    }
    // DressUpFrameCancelButton CLOSE (80 x 22, CENTER at 305,-422) and DressUpFrameResetButton RESET at its
    // left. PSP: 52 x 14 px (the 0.6 x 0.55 button, 48 x 12, does not hold the 10 px text).
    {
        const float bw = 52.0f, bh = 14.0f, by = PY(422) - bh * 0.5f;
        const Rect cancel{PX(305) - bw * 0.5f, by, bw, bh}, reset{cancel.x - bw, by, bw, bh};
        drawPanelButton(ui, kIdDuReset, reset, CLIENT_STR(RESET), true, focusable);
        drawPanelButton(ui, kIdDuCancel, cancel, CLIENT_STR(CLOSE), true, focusable);
    }
    // DressUpFrameCloseButton (UIPanelCloseButton, CENTER at TOPRIGHT -46,-25).
    {
        const Rect cr{PX(384 - 46) - 10, PY(25) - 10, 20, 20};
        const bool f = focusable && winFocus_.addStop(kIdDuClose, cr);
        const pp::Texture& t = f && winFocus_.isPressed(kIdDuClose) && npcArt_.closeDown.pixels ? npcArt_.closeDown
                                                                                              : npcArt_.close;
        if (t.pixels) ui.image(t, cr);
        if (f) ui.fill(cr, 0x40FFFFFFu);
    }
    return true;
}

bool WorldScreen::dressUpAction(uint32_t act) {
    if (act < 0x6300 || act >= WorldChat::kLinkId0) return false;  // the chat's links are the chat's
    if (act >= kIdQuestItem0 && act < kIdQuestItemEnd) return true;  // a quest item button: the click does nothing
    switch (act) {
        case kIdShowCloak:
        case kIdShowHelm: {
            // InterfaceOptionsCheckButtonTemplate OnClick: the sound, then SetValue -> ShowCloak / ShowHelm.
            const bool cloak = act == kIdShowCloak;
            const bool on = !(cloak ? session_.isCloakVisible() : session_.isHelmVisible());
            playSoundByName(on ? "igMainMenuOptionCheckBoxOn" : "igMainMenuOptionCheckBoxOff");
            if (cloak) session_.showCloak(on);
            else session_.showHelm(on);
            return true;
        }
        case kIdDuClose:
        case kIdDuCancel: closeWindow(kWinDressUp); return true;  // HideParentPanel (its OnHide: the sound)
        case kIdDuReset: {  // DressUpModel:Dress() - the player as he is now
            DressUpUi& d = dressUpUi();
            d.worn = player_.equipment();
            d.shown = d.worn;
            d.pendingItem = 0;
            if (d.model.loaded()) {
                d.model.setEquipment(charPak_, d.shown, items_.isOpen() ? &items_ : nullptr);
                d.model.setSheathState(dressUpSheathState(d.shown));
            }
            playSoundByName("gsTitleOptionOK");
            PSP_LOGI(pp::tag::Gfx, "dressup_reset");
            return true;
        }
        case kIdDuRotLeft: modelSpinClick(dressUpUi().spin, true); return true;
        case kIdDuRotRight: modelSpinClick(dressUpUi().spin, false); return true;
        default: return true;
    }
}

// --- The character window's check boxes ----------------------------------------------------------------------

// SHOW_CLOAK (ShowCloak) and below it SHOW_HELM (ShowHelm) - the Display panel's order - in the
// model square's top right, each with the slot's empty art (UI-PaperDoll-Slot-Chest of the back slot, -Head)
// left of the box; the names and OPTION_TOOLTIP_SHOW_* in the GameTooltip (drawDressUpOverlay).
void WorldScreen::drawShowChecks(UiRenderer& ui, const Rect& sq, bool focusable) {
    DressUpUi& d = dressUpUi();
    if (!d.cbTried) {
        d.cbTried = true;
        char path[256];
        pp::fsPath(path, sizeof(path), "data/ui.pak");
        pp::Pak pak;
        uint32_t bytes = 0;
        if (pak.open(path)) {
            loadUiTexture(pak, "cb_up", d.cbUp, bytes);     // UI-CheckBox-Up
            loadUiTexture(pak, "cb_hl", d.cbHl, bytes);     // UI-CheckBox-Highlight
            loadUiTexture(pak, "ql_check", d.cbCheck, bytes);  // UI-CheckBox-Check
            pak.close();
        }
        PSP_LOGI(pp::tag::Gfx, "dressup_checks_art ok=%d kb=%u", d.cbUp.pixels && d.cbCheck.pixels ? 1 : 0,
                 static_cast<unsigned>((bytes + 1023u) / 1024u));
    }
    constexpr float kBox = 13.0f, kIcon = 11.0f;
    struct Check {
        uint32_t id;
        bool on;
        int slotArt;  // pdArt_.slot index
    };
    const Check checks[2] = {{kIdShowCloak, session_.isCloakVisible(), 14}, {kIdShowHelm, session_.isHelmVisible(), 0}};
    for (int i = 0; i < 2; ++i) {
        const Check& c = checks[i];
        const Rect box{sq.x + sq.w - kBox - 2.0f, sq.y + 2.0f + static_cast<float>(i) * (kBox + 2.0f), kBox, kBox};
        const Rect icon{box.x - kIcon - 1.0f, box.y + (kBox - kIcon) * 0.5f, kIcon, kIcon};
        if (pdArt_.slot[c.slotArt].pixels) ui.image(pdArt_.slot[c.slotArt], icon, 0xC0FFFFFFu);
        const bool f = focusable && winFocus_.addStop(c.id, Rect{icon.x, box.y, box.x + box.w - icon.x, kBox});
        if (d.cbUp.pixels) ui.image(d.cbUp, box);
        else ui.fill(box, 0xC0303030u);
        if (c.on) {
            if (d.cbCheck.pixels) ui.image(d.cbCheck, box);
            else ui.fill(Rect{box.x + 3, box.y + 3, box.w - 6, box.h - 6}, 0xFF00D1FFu);
        }
        if (f) {
            if (d.cbHl.pixels) ui.image(d.cbHl, box, 0xFFFFFFFFu, Blend::Add);
            else ui.fill(box, 0x40FFFFFFu);
            d.tipId = c.id;
            d.tipRect = Rect{box.x + ui.offsetX(), box.y + ui.offsetY(), box.w, box.h};
        }
    }
}

// After the windows: the focused check box's GameTooltip - ANCHOR_RIGHT (InterfaceOptionsCheckButtonTemplate
// OnEnter); PSP: its name (the original shows it beside the box) above OPTION_TOOLTIP_SHOW_*.
void WorldScreen::drawDressUpOverlay(UiRenderer& ui) {
    if (!du_ || !du_->tipId || !uiMode_ || settings_.isOpen()) return;
    const DressUpUi& d = *du_;
    const bool cloak = d.tipId == kIdShowCloak;
    const char* title = cloak ? CLIENT_STR(SHOW_CLOAK) : CLIENT_STR(SHOW_HELM);
    const char* text = cloak ? CLIENT_STR(OPTION_TOOLTIP_SHOW_CLOAK) : CLIENT_STR(OPTION_TOOLTIP_SHOW_HELM);
    const Font& fn = kit_.font(kFontNormal);
    const Font& fs = kit_.font(kFontSmall);
    const float pad = 6.0f;
    const float w = std::ceil(std::max(static_cast<float>(fn.measure(title)), static_cast<float>(fs.measure(text))) + 2 * pad);
    const float h = std::ceil(static_cast<float>(fn.lineHeight() + fs.lineHeight()) + 1.0f + 2 * pad);
    float x = d.tipRect.x + d.tipRect.w, y = d.tipRect.y - h;
    x = std::clamp(x, 2.0f, std::max(2.0f, static_cast<float>(pp::kScreenW) - 2.0f - w));
    y = std::clamp(y, 2.0f, std::max(2.0f, static_cast<float>(pp::kScreenH) - 2.0f - h));
    const Rect r{x, y, w, h};
    ui.fill(Rect{r.x + 2, r.y + 2, r.w - 4, r.h - 4}, kTipBack);
    if (kit_.has(kTexTipBorder)) ui.nineSlice(kit_.tex(kTexTipBorder), r);
    ui.text(fn, x + pad, y + pad, title, kColWhite);
    ui.text(fs, x + pad, y + pad + static_cast<float>(fn.lineHeight()) + 1.0f, text, kColGold);
}

}  // namespace wowee::ui::psp
