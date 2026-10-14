// InspectFrame of the original ruRU 3.3.5a client -
// the client's inspect add-on (its .xml / .lua, InspectPaperDollFrame.xml / .lua,
// InspectPVPFrame.xml / .lua, InspectTalentFrame.xml / .lua), over the session's inspect half
// (psp_world_session_inspect.cpp: NotifyInspect, SMSG_INSPECT_TALENT, RequestInspectHonorData, ClearInspectPlayer).
//
// Opened by the unit menu's INSPECT (ui_world_party.cpp) as InspectUnit -> InspectFrame_Show:
// CanInspect(unit, true) (another player, not hostile, within 28 yd - the port's refusal lines: ERR_OUT_OF_RANGE for
// a valid player too far, ERR_INVALID_INSPECT_TARGET for everything else, in the red line), NotifyInspect, tab 1,
// igCharacterInfoOpen. InspectFrame_OnEvent / OnUpdate: on the target's frame a new target is inspected when
// CanInspect allows it, else the frame hides; a unit out of sight (UnitIsVisible) hides it; OnHide:
// ClearInspectPlayer, igCharacterInfoClose.
//
// The frame (384 x 512 units, TOPLEFT 0,-104 as CharacterFrame) at the paperdoll's 0.6 x 0.55 (PX / PY):
//   - InspectPaperDollFrame's art: UI-Character-CharacterTab-L1 / R1 / BottomLeft / BottomRight (ui.pak insp_frame);
//   - InspectFramePortrait (60 x 60 at 7,-6): the race / sex portrait, round (as the paperdoll's: SetPortraitTexture);
//   - InspectNameText (GameFontNormal, white: its Color) at CENTER 6,232, InspectLevelText under it (PLAYER_LEVEL:
//     the level, UnitRace, UnitClass of his gender; -1 = "??");
//   - InspectModelFrame (233 x 300 at 65,-78): his CharacterModel of the world (remotes_, the same item models) in
//     its own VRAM target after the 3D pass, rotation 0.61 to start; InspectModelRotateLeft / RightButton (35 x 35 at
//     its TOPLEFT): a click turns it 0.03 rad (Model_RotateLeft / Right), while held it turns at
//     ROTATIONS_PER_SECOND (InspectModelFrame_OnUpdate), igInventoryRotateCharacter;
//   - the 19 InspectPaperDollItemSlotButtons in the paperdoll's columns: the icon of the visible item
//     (GetInventoryItemTexture: PLAYER_VISIBLE_ITEM_n -> Item.dbc -> ItemDisplayInfo.InventoryIcon), else the
//     slot's empty art (GetInventorySlotInfo; the ranged slot of a paladin / shaman / druid / death knight: Relic);
//     the tooltip GameTooltip:SetInventoryItem(unit, slot) - the item tooltip with the inspect's enchantments, gems
//     and sockets (WorldSession::kInspectItemGuid), L = Shift (the comparison); an empty slot: its name (HEADSLOT...,
//     RELICSLOT);
//   - InspectFrameCloseButton (CENTER at TOPRIGHT -44,-25), InspectFrameTab1..3 CHARACTER / PVP / TALENTS
//     (CENTER at BOTTOMLEFT 60,61, the next LEFT at the previous RIGHT -16; tooltips CHARACTER_INFO /
//     PLAYER_V_PLAYER / TALENTS; the talents tab disabled under level 10, InspectFrame_UpdateTalentTab).
// PSP (marked): the model shares the world's CharacterModel (no second copy); the slots' tooltips draw after the
// windows; the art loads on open and is freed on close (log inspect_art kb= / inspect_art_free kb=).
#include "ui/psp/ui_world_screen.hpp"

#include "core/coordinates.hpp"
#include "game/update_field_table.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"
#include "platform/psp/psp_texture.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "ui/psp/ui_world_sounds.hpp"

#include <pspgu.h>
#include <pspgum.h>
#include <pspthreadman.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

// Widget ids: 0x6F00-0x6FFF.
enum : uint32_t {
    kIdClose = 0x6F00,
    kIdRotLeft,
    kIdRotRight,
    kIdTab1,  // .. kIdTab1 + 2
    kIdSlot0 = 0x6F10,  // + EquipSlot 0..18
    kIdTalUp = 0x6F30,  // InspectTalentFrameScrollFrame's arrows
    kIdTalDown,
    kIdTalTab0 = 0x6F40,  // + 0..2 InspectTalentFrameTab1..3
    kIdTal0 = 0x6F80,     // + 0..63 the talent buttons (index in the tree, tier / column order)
};

constexpr float kSX = 0.6f, kSY = 0.55f;  // tools/psp/build_ui_pak.py PD_SX / PD_SY (insp_frame is cut like pd_frame)
constexpr float kOx = 4.0f, kOy = 2.0f;   // TOPLEFT 0,-104 (as CharacterFrame) -> the left, at the top
constexpr float kFrameW = 211.0f, kFrameH = 265.0f;
float PX(float x) { return kOx + x * kSX; }
float PY(float y) { return kOy + y * kSY; }
constexpr float kSlot = 21.0f;  // ItemButton 37 units, square (as the paperdoll)

constexpr int kLeftCol[8] = {0, 1, 2, 14, 4, 3, 18, 8};    // InspectHeadSlot .. InspectWristSlot (EquipSlot)
constexpr int kRightCol[8] = {9, 5, 6, 7, 10, 11, 12, 13};  // InspectHandsSlot .. InspectTrinket1Slot
constexpr int kWeapons[3] = {15, 16, 17};                   // InspectMainHandSlot, SecondaryHand, Ranged
// GetInventorySlotInfo's textures (ui.pak pd_slot_*, build_ui_pak.py PD_SLOTS) by EquipSlot: the back slot shows
// UI-PaperDoll-Slot-Chest, as the original.
constexpr const char* kSlotArt[19] = {"head",   "neck",    "shoulder", "shirt",   "chest",   "waist",   "legs",
                                      "feet",   "wrists",  "hands",    "finger",  "rfinger", "trinket", "trinket",
                                      "chest",  "mainhand", "secondaryhand", "ranged", "tabard"};
// InspectPaperDollItemSlotButton_OnEnter's empty-slot text: _G[strupper(strsub(name, 8))] (GlobalStrings).
using platform::psp::CS;
constexpr CS kSlotName[19] = {CS::HEADSLOT,      CS::NECKSLOT,     CS::SHOULDERSLOT,      CS::SHIRTSLOT,
                              CS::CHESTSLOT,     CS::WAISTSLOT,    CS::LEGSSLOT,          CS::FEETSLOT,
                              CS::WRISTSLOT,     CS::HANDSSLOT,    CS::FINGER0SLOT,       CS::FINGER1SLOT,
                              CS::TRINKET0SLOT,  CS::TRINKET1SLOT, CS::BACKSLOT,          CS::MAINHANDSLOT,
                              CS::SECONDARYHANDSLOT, CS::RANGEDSLOT, CS::TABARDSLOT};
constexpr CS kRelicSlot = CS::RELICSLOT;
constexpr CS kTabText[3] = {CS::CHARACTER, CS::PVP, CS::TALENTS};
constexpr CS kTabTip[3] = {CS::CHARACTER_INFO, CS::PLAYER_V_PLAYER, CS::TALENTS};
constexpr uint16_t kVisibleItemBase = 283;  // PLAYER_VISIBLE_ITEM_1_ENTRYID (3.3.5a), stride 2
constexpr float kInspectRange = 28.0f;      // AzerothCore INSPECT_DISTANCE; upstream CheckInteractDistance 1
constexpr float kRotationsPerSecond = 0.5f; // UIParent.lua ROTATIONS_PER_SECOND
constexpr uint32_t kColDisabled = 0xFF808080u;  // GameFontDisableSmall

// UnitHasRelicSlot: paladin, shaman, druid, death knight (the classes whose ranged slot is a relic).
bool hasRelicSlot(uint8_t cls) { return cls == 2 || cls == 7 || cls == 11 || cls == 6; }

// ---- InspectTalentFrame: TalentFrameBase.lua's branch drawing (as ui_world_trainer.cpp ports it for
// PlayerTalentFrame; a copy here - additive, the trainer's helpers are file-local) ----
struct BranchNode {  // TALENT_BRANCH_ARRAY node (TalentFrame_Load)
    int id = 0;  // talent index + 1, 0 = none
    int up = 0, down = 0, left = 0, right = 0, leftArrow = 0, rightArrow = 0, topArrow = 0;
};
constexpr int kTiers = 15, kCols = 4;  // MAX_NUM_TALENT_TIERS, NUM_TALENT_COLUMNS
using BranchGrid = BranchNode[kTiers][kCols];

// TalentFrame_DrawLines, tiers / columns from 0.
void drawLines(BranchGrid& g, int bt, int bc, int tier, int column, bool met) {
    const int rm = met ? 1 : -1;
    if (bt < 0 || bt >= kTiers || tier < 0 || tier >= kTiers || bc < 0 || bc >= kCols || column < 0 || column >= kCols)
        return;
    if (bc == column) {
        if (bt - tier > 1)
            for (int i = tier + 1; i <= bt - 1; ++i)
                if (g[i][bc].id) return;
        for (int i = tier; i <= bt - 1; ++i) {
            g[i][bc].down = rm;
            if (i + 1 <= bt - 1) g[i + 1][bc].up = rm;
        }
        g[bt][bc].topArrow = rm;
        return;
    }
    if (bt == tier) {
        const int left = std::min(bc, column), right = std::max(bc, column);
        if (right - left > 1)
            for (int i = left + 1; i <= right - 1; ++i)
                if (g[tier][i].id) return;
        for (int i = left; i <= right - 1; ++i) {
            g[tier][i].right = rm;
            g[tier][i + 1].left = rm;
        }
        if (bc < column) g[bt][bc].rightArrow = rm;
        else g[bt][bc].leftArrow = rm;
        return;
    }
    int left = std::min(bc, column), right = std::max(bc, column);
    if (left == column) ++left;
    else --right;
    bool blocked = false;
    for (int i = left; i <= right; ++i)
        if (g[tier][i].id) blocked = true;
    left = std::min(bc, column);
    right = std::max(bc, column);
    if (!blocked) {
        g[tier][bc].down = rm;
        g[bt][bc].up = rm;
        for (int i = tier; i <= bt - 1; ++i) {
            g[i][bc].down = rm;
            g[i + 1][bc].up = rm;
        }
        for (int i = left; i <= right - 1; ++i) {
            g[tier][i].right = rm;
            g[tier][i + 1].left = rm;
        }
        g[bt][bc].topArrow = rm;
        return;
    }
    if (left == bc) ++left;
    else --right;
    for (int i = left; i <= right; ++i)
        if (g[bt][i].id) return;
    for (int i = tier; i <= bt - 1; ++i) {
        g[i][column].up = rm;
        g[i + 1][column].down = rm;
    }
    if (bc < column) g[bt][bc].rightArrow = rm;
    else g[bt][bc].leftArrow = rm;
}

// TALENT_BRANCH_TEXTURECOORDS / TALENT_ARROW_TEXTURECOORDS: {left, right, top, bottom} for [1] / [-1].
struct TC {
    float l, r, t, b;
};
enum Branch { kUp, kDown, kLeft, kRight, kTopRight, kTopLeft, kBottomRight, kBottomLeft, kTDown, kTUp };
const TC kBranchTC[10][2] = {
    {{0.12890625f, 0.25390625f, 0, 0.484375f}, {0.12890625f, 0.25390625f, 0.515625f, 1.0f}},
    {{0, 0.125f, 0, 0.484375f}, {0, 0.125f, 0.515625f, 1.0f}},
    {{0.2578125f, 0.3828125f, 0, 0.5f}, {0.2578125f, 0.3828125f, 0.5f, 1.0f}},
    {{0.2578125f, 0.3828125f, 0, 0.5f}, {0.2578125f, 0.3828125f, 0.5f, 1.0f}},
    {{0.515625f, 0.640625f, 0, 0.5f}, {0.515625f, 0.640625f, 0.5f, 1.0f}},
    {{0.640625f, 0.515625f, 0, 0.5f}, {0.640625f, 0.515625f, 0.5f, 1.0f}},
    {{0.38671875f, 0.51171875f, 0, 0.5f}, {0.38671875f, 0.51171875f, 0.5f, 1.0f}},
    {{0.51171875f, 0.38671875f, 0, 0.5f}, {0.51171875f, 0.38671875f, 0.5f, 1.0f}},
    {{0.64453125f, 0.76953125f, 0, 0.5f}, {0.64453125f, 0.76953125f, 0.5f, 1.0f}},
    {{0.7734375f, 0.8984375f, 0, 0.5f}, {0.7734375f, 0.8984375f, 0.5f, 1.0f}},
};
enum Arrow { kArrowTop, kArrowRight, kArrowLeft };
const TC kArrowTC[3][2] = {
    {{0, 0.5f, 0, 0.5f}, {0, 0.5f, 0.5f, 1.0f}},
    {{1.0f, 0.5f, 0, 0.5f}, {1.0f, 0.5f, 0.5f, 1.0f}},
    {{0.5f, 1.0f, 0, 0.5f}, {0.5f, 1.0f, 0.5f, 1.0f}},
};
struct Piece {  // one branch / arrow texture: TOPLEFT at (x, y) of the scroll child, 32 x 32 units
    TC tc;
    float x, y;
    bool arrow;
};

// An ARGB colour of an arena team (background / emblem / border) as the tint 0xAABBGGRR, opaque.
uint32_t teamTint(uint32_t argb, uint32_t alpha) {
    return (alpha << 24) | ((argb & 0xFFu) << 16) | (argb & 0xFF00u) | ((argb >> 16) & 0xFFu);
}

// A GameTooltip of plain lines (SetOwner ANCHOR_RIGHT: BOTTOMLEFT at the owner's TOPRIGHT, clamped to the screen):
// the first in the header font, the rest in the small one; the kit's tooltip backdrop.
void drawTipLines(UiRenderer& ui, const UiKit& kit, const Rect& a, const std::vector<std::pair<std::string, uint32_t>>& lines) {
    if (lines.empty()) return;
    const Font& fh = kit.font(kFontNormal);
    const Font& fb = kit.font(kFontSmall);
    float w = 0.0f, h = 10.0f;
    for (size_t i = 0; i < lines.size(); ++i) {
        const Font& f = i == 0 ? fh : fb;
        w = std::max(w, static_cast<float>(f.measure(lines[i].first.c_str())));
        h += static_cast<float>(f.lineHeight()) + (i == 0 ? 1.0f : 0.0f);
    }
    w += 12.0f;
    float x = a.x + a.w, y = a.y - h;
    x = std::clamp(x, 2.0f, std::max(2.0f, static_cast<float>(pp::kScreenW) - 2.0f - w));
    y = std::clamp(y, 2.0f, std::max(2.0f, static_cast<float>(pp::kScreenH) - 2.0f - h));
    const Rect r{x, y, w, h};
    ui.fill(Rect{r.x + 2, r.y + 2, r.w - 4, r.h - 4}, 0xBB1B0D0Du);  // UI-Tooltip-Background x (.09, .09, .19)
    if (kit.has(kTexTipBorder)) ui.nineSlice(kit.tex(kTexTipBorder), r);
    float ly = y + 5.0f;
    for (size_t i = 0; i < lines.size(); ++i) {
        const Font& f = i == 0 ? fh : fb;
        ui.text(f, x + 6.0f, ly, lines[i].first.c_str(), lines[i].second);
        ly += static_cast<float>(f.lineHeight()) + (i == 0 ? 1.0f : 0.0f);
    }
}

}  // namespace

struct WorldScreen::InspectUi {
    pp::Texture frame, rotLeft, rotRight, relic;
    pp::Texture slot[19];
    bool artTried = false;
    uint32_t artBytes = 0;
    uint64_t guid = 0;         // InspectFrame.unit's player
    bool fromTarget = true;    // the unit token is "target" (else a party / raid frame's)
    bool shown = false;        // OnShow ran (OnHide is owed)
    uint8_t tab = 1;           // PanelTemplates_GetSelectedTab(InspectFrame)
    float rotation = 0.61f;    // InspectModelFrame.rotation
    int tipSlot = -1;          // the slot under the cursor this frame (its GameTooltip after the windows)
    // The slots' entries and display ids, looked up when an entry changes - ItemBank
    // keeps 4 Item.dbc blocks, a geared player's 19 items thrashed them (a card read per slot per frame).
    uint32_t slotEntry[19] = {}, slotDisplay[19] = {};
    Rect tipRect{};
    int tipTab = -1;           // a tab under the cursor
    uint32_t lastMs = 0;
    uint32_t logs = 0;
    uint8_t cls = 0;           // UnitClass(unit): its talent tree
    // InspectPVPFrame (tab 2): its art while it shows; a known team's border / emblem by row.
    pp::Texture pvpFrame, pvpTop, pvpBottom, pvpLeft, pvpRight, pvpBar, pvpLine, pvpArena, pvpFaction[2], pvpBanner[3];
    pp::Texture rowBorder[3], rowEmblem[3];
    int rowBorderKey[3] = {-1, -1, -1}, rowEmblemKey[3] = {-1, -1, -1};
    bool pvpTried = false;
    uint32_t pvpBytes = 0;
    // InspectTalentFrame (tab 3): its art while it shows; the selected tree (-1 = pick, each show), the scroll.
    pp::Texture talFrame, talSlot, talBranches, talArrows, talRank, talTabOn, talTabOff, talBg[3];
    bool talTried = false;
    uint32_t talBytes = 0;
    int talTab = -1;
    float talScroll = 0.0f;
    uint32_t talFocus = 0, tipTalent = 0;
    void loadArt();
    void releaseArt();
    void loadPvpArt();
    void loadTeamArt(int row, int size, int border, int emblem);
    void releasePvpArt();
    void loadTalArt(const std::vector<std::string>& backgrounds);
    void releaseTalArt();
    ~InspectUi() {
        releaseArt();
        releasePvpArt();
        releaseTalArt();
    }
};

void WorldScreen::destroyInspectUi(InspectUi* p) { delete p; }

// The inspected slot under the cursor - L + Triangle tries it on (its tooltip says so).
uint32_t WorldScreen::inspectHoverItem() const {
    if (!insp_ || insp_->tab != 1 || insp_->tipSlot < 0 || insp_->tipSlot >= 19 || !windowOpen(kWinInspect)) return 0;
    return insp_->slotEntry[insp_->tipSlot];
}

WorldScreen::InspectUi& WorldScreen::inspectUi() {
    if (!insp_) insp_ = std::unique_ptr<InspectUi, void (*)(InspectUi*)>(new InspectUi, &WorldScreen::destroyInspectUi);
    return *insp_;
}

// The frame's art: on open, freed on close (inspect_art kb= / inspect_art_free kb=).
void WorldScreen::InspectUi::loadArt() {
    if (artTried) return;
    artTried = true;
    char path[256];
    pp::fsPath(path, sizeof(path), "data/ui.pak");
    pp::Pak pak;
    if (!pak.open(path)) return;
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    int ok = 0, want = 0;
    auto load = [&](const char* name, pp::Texture& t) {
        ++want;
        const pp::PakEntry* e = pak.find(name);
        void* blob = e ? pak.load(e) : nullptr;
        if (blob && !pp::textureFromBlob(blob, e->size, t, true)) {
            pp::pakFree(blob);
            return;
        }
        if (t.pixels) {
            ++ok;
            artBytes += pp::textureBytes(t);
        }
    };
    load("insp_frame", frame);
    load("pd_rot_left", rotLeft);
    load("pd_rot_right", rotRight);
    load("pd_slot_relic", relic);
    char name[48];
    for (int s = 0; s < 19; ++s) {
        std::snprintf(name, sizeof(name), "pd_slot_%s", kSlotArt[s]);
        load(name, slot[s]);
    }
    pak.close();
    PSP_LOGI(pp::tag::Gfx, "inspect_art ok=%d/%d kb=%u ms=%u", ok, want, static_cast<unsigned>(artBytes / 1024u),
             static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u));
}

void WorldScreen::InspectUi::releaseArt() {
    if (!artTried) return;
    for (pp::Texture* t : {&frame, &rotLeft, &rotRight, &relic})
        if (t->pixels) pp::textureRelease(*t);
    for (pp::Texture& t : slot)
        if (t.pixels) pp::textureRelease(t);
    PSP_LOGI(pp::tag::Gfx, "inspect_art_free kb=%u", static_cast<unsigned>(artBytes / 1024u));
    artTried = false;
    artBytes = 0;
}

// CanInspect(unit, showError): another player (upstream lua_system_api.cpp CanInspect: players only) the player
// cannot attack (AzerothCore HandleInspectOpcode answers nothing for a valid attack target; PSP: "cannot attack" =
// not of a hostile faction, WorldSession::isHostileUnit, as the unit menu's canCoop), within 28 yd (AzerothCore
// INSPECT_DISTANCE). The port's own refusal lines: ERR_OUT_OF_RANGE for a valid player who is too far (walking
// closer helps), ERR_INVALID_INSPECT_TARGET for any other refusal.
bool WorldScreen::canInspect(uint64_t guid, bool showError) {
    auto err = [&](const char* s) {
        if (showError) session_.addUiError(s);
        return false;
    };
    auto e = guid ? session_.entities().getEntity(guid) : nullptr;
    if (!e || e->getType() != game::ObjectType::PLAYER || guid == session_.playerGuid() || session_.isHostileUnit(guid))
        return err(CLIENT_STR(ERR_INVALID_INSPECT_TARGET));
    const glm::vec3 at = core::coords::canonicalToServer(glm::vec3(e->getX(), e->getY(), e->getZ()));
    if (glm::length(at - glm::vec3(playerX_, playerY_, playerZ_)) > kInspectRange)
        return err(CLIENT_STR(ERR_OUT_OF_RANGE));
    return true;
}

// UnitPopup INSPECT's state (UnitPopup_OnUpdate): disabled while the player is dead or a ghost, or the unit is out
// of CheckInteractDistance(unit, 1) - 28 yd.
bool WorldScreen::inspectMenuEnabled(uint64_t guid) const {
    if (session_.life() != pp::WorldSession::Life::Alive) return false;
    auto e = guid ? session_.entities().getEntity(guid) : nullptr;
    if (!e) return false;
    const glm::vec3 at = core::coords::canonicalToServer(glm::vec3(e->getX(), e->getY(), e->getZ()));
    return glm::length(at - glm::vec3(playerX_, playerY_, playerZ_)) <= kInspectRange;
}

// InspectUnit -> InspectFrame_Show(unit): HideUIPanel, then CanInspect(unit, true) -> NotifyInspect, the unit,
// InspectSwitchTabs(1), ShowUIPanel, InspectFrame_UpdateTalentTab.
void WorldScreen::openInspect(uint64_t guid, bool fromTarget) {
    InspectUi& I = inspectUi();
    if (windowOpen(kWinInspect)) closeWindow(kWinInspect);  // HideUIPanel(InspectFrame)
    if (I.shown) {  // its OnHide
        I.shown = false;
        I.guid = 0;
        session_.clearInspectPlayer();
        playSoundByName("igCharacterInfoClose");
    }
    if (!canInspect(guid, true)) return;
    session_.notifyInspect(guid);
    I.guid = guid;
    I.fromTarget = fromTarget;
    I.tab = 1;  // InspectSwitchTabs(1); the model's rotation stays (InspectModelFrame_OnLoad set it once)
    openWindow(kWinInspect);
    if (!I.shown) {
        I.shown = true;
        playSoundByName("igCharacterInfoOpen");  // InspectFrame_OnShow
    }
    I.loadArt();
    I.lastMs = pp::logUptimeMs();
    if (++I.logs <= 40)
        PSP_LOGI(pp::tag::Ctrl, "inspect_open guid=%llx target=%d", static_cast<unsigned long long>(guid),
                 fromTarget ? 1 : 0);
}

// Every frame (drawWindows): InspectFrame_OnHide when the window went, InspectFrame_OnEvent (PLAYER_TARGET_CHANGED
// on a target's frame) and InspectFrame_OnUpdate (not UnitIsVisible: hidden).
void WorldScreen::updateInspect() {
    if (chat_.takeInspect()) openInspect(session_.target(), true);  // SLASH_INSPECT: InspectUnit("target")
    if (!insp_) return;
    InspectUi& I = *insp_;
    const bool open = windowOpen(kWinInspect);
    if (open && I.guid != 0) {
        if (I.fromTarget && session_.target() != I.guid) {
            const uint64_t t = session_.target();
            if (canInspect(t, false)) {  // InspectFrame_UnitChanged
                session_.notifyInspect(t);
                I.guid = t;
                // InspectFrame_UpdateTalentTab: under level 10 the talents tab goes to tab 1. PSP: the talent art
                // read again (the new player's class: its trees' backgrounds).
                auto ne = session_.entities().getEntity(t);
                const int lvl = ne ? static_cast<int>(ne->getField(game::fieldIndex(game::UF::UNIT_FIELD_LEVEL))) : 0;
                if (I.tab == 3 && lvl > 0 && lvl < 10) I.tab = 1;
                I.releaseTalArt();
                inspectTabShown(I.tab);  // InspectPVPFrame_OnShow when the PvP tab shows (the new player's honour)
                if (++I.logs <= 40) PSP_LOGI(pp::tag::Ctrl, "inspect_unit_changed guid=%llx", static_cast<unsigned long long>(t));
            } else {
                closeWindow(kWinInspect);
            }
        } else if (!session_.entities().getEntity(I.guid)) {
            closeWindow(kWinInspect);  // InspectFrame_OnUpdate: not UnitIsVisible
        }
    }
    if (I.shown && !windowOpen(kWinInspect)) {  // InspectFrame_OnHide
        I.shown = false;
        I.guid = 0;
        session_.clearInspectPlayer();
        playSoundByName("igCharacterInfoClose");
        I.releaseArt();
        I.releasePvpArt();
        I.releaseTalArt();
        I.talTab = -1;  // InspectFrame_OnHide: PanelTemplates_SetTab(InspectTalentFrame, nil)
        inspModelOk_ = false;
    }
    if (!open && !I.shown) {
        I.releaseArt();
        I.releasePvpArt();
        I.releaseTalArt();
    }
    // InspectModelFrame_OnUpdate: a rotate button held turns the model.
    const uint32_t now = pp::logUptimeMs();
    const float dt = std::min(0.1f, static_cast<float>(now - I.lastMs) / 1000.0f);
    I.lastMs = now;
    if (open && uiMode_) {
        const float step = dt * 2.0f * 3.14159265f * kRotationsPerSecond;
        if (winFocus_.isPressed(kIdRotLeft)) I.rotation -= step;
        if (winFocus_.isPressed(kIdRotRight)) I.rotation += step;
    }
}

// After the 3D pass, with the character window's model (the portrait's corner of eDRAM): the inspected player's
// model of the world, framed whole, turned to the viewer by the frame's rotation.
void WorldScreen::renderInspectModel(const rendering::psp::TerrainCamera& cam) {
    inspModelOk_ = false;
    if (!inspModelVram_ || !insp_ || !windowOpen(kWinInspect) || insp_->tab != 1 || !insp_->frame.pixels) return;
    auto it = remotes_.find(insp_->guid);
    if (it == remotes_.end() || !it->second.model || !it->second.hasCache) return;
    RemotePlayer& r = it->second;
    rendering::psp::CharacterModel& model = *r.model;
    const float h = std::max(0.8f, model.height());
    const glm::vec3 feet = r.mountReady ? r.pos + glm::vec3(0.0f, 0.0f, r.riderHeight) : r.pos;
    const float yaw = r.yaw + insp_->rotation - 0.61f;
    const glm::vec3 front(std::cos(r.yaw), std::sin(r.yaw), 0.0f);
    const glm::vec3 centre = feet + glm::vec3(0.0f, 0.0f, h * 0.5f);
    // InspectModelFrame is 233 x 300 units: the model framed a little taller than the character window's.
    const float dist = h * 0.5f / std::tan(20.0f * 3.14159265f / 180.0f) * 1.12f;
    const glm::vec3 eye = centre + front * dist;
    sceGuDrawBufferList(GU_PSM_5650, reinterpret_cast<void*>(static_cast<uintptr_t>(inspModelVram_)), 128);
    sceGuOffset(2048 - 64, 2048 - 64);
    sceGuViewport(2048, 2048, 128, 128);
    sceGuScissor(0, 0, 128, 128);
    sceGuClearColor(0xFF0D0D0Du);  // UI-Character-CharacterTab-L1's background
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
    model.draw(mc, feet, yaw, 0xFF0D0D0Du, 1.0f);
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
    inspModelOk_ = true;
}

bool WorldScreen::drawInspectWindow(UiRenderer& ui, uint8_t w) {
    if (w != kWinInspect) return false;
    InspectUi& I = inspectUi();
    I.tipSlot = -1;
    I.tipTab = -1;
    I.loadArt();
    PROF_SCOPE("inspect");
    loadNpcArt(false);  // the close button
    const bool focusable = uiMode_ && !settings_.isOpen() && !windowOpen(kWinBindConfirm);
    const Font& fs = kit_.font(kFontSmall);
    const Font& fn = kit_.font(kFontNormal);
    auto e = I.guid ? session_.entities().getEntity(I.guid) : nullptr;
    const uint32_t b0 = e ? e->getField(game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0)) : 0u;
    const uint8_t race = static_cast<uint8_t>(b0 & 0xFF), cls = static_cast<uint8_t>((b0 >> 8) & 0xFF);
    const bool female = ((b0 >> 16) & 0xFF) == 1;
    const int level = e ? static_cast<int>(e->getField(game::fieldIndex(game::UF::UNIT_FIELD_LEVEL))) : 0;

    // InspectFramePortrait (60 x 60 at 7,-6) under the frame.
    {
        const Rect pr{PX(7), PY(6), 60 * kSX, 60 * kSY};
        if (race > 0 && race < 12 && ufArt_.portrait[race][female ? 1 : 0].pixels)
            ui.imageCircle(ufArt_.portrait[race][female ? 1 : 0], pr);
        else
            ui.fillCircle(Rect{pr.x + 2, pr.y + 2, pr.w - 4, pr.h - 4}, 0xFF101010u);
    }
    I.cls = cls;
    // The shown subframe's art (InspectPaperDollFrame / InspectPVPFrame / InspectTalentFrame), the others' freed.
    if (I.tab != 2) I.releasePvpArt();
    if (I.tab != 3) {
        I.releaseTalArt();
        I.talTab = -1;  // InspectFrame_OnHide / the talents' OnHide: a tree picked again next time
    }
    if (I.tab == 2) I.loadPvpArt();
    if (I.tab == 3 && !I.talTried && session_.loadInspectTalents(cls)) {  // the trees' backgrounds with the frame
        std::vector<std::string> bgs;
        for (const auto& t : session_.inspectTalentTabs()) bgs.push_back(t.backgroundFile);
        I.loadTalArt(bgs);
    }
    const pp::Texture& frameArt = I.tab == 1 ? I.frame : I.tab == 2 ? I.pvpFrame : I.talFrame;
    const Rect frameRect = I.tab == 3 ? Rect{kOx, kOy, 356 * kSX, 465 * kSY} : Rect{kOx, kOy, kFrameW, kFrameH};
    if (frameArt.pixels) {
        ui.image(frameArt, frameRect);
    } else if (I.tab != 3) {  // ui.pak without the art (not rebaked): the kit's panel
        kit_.panel(ui, frameRect);
    }
    // InspectNameText (GameFontNormal, its Color white) at CENTER 6,232 (= 198,24) and InspectLevelText under it.
    const std::string* name = I.guid ? session_.nameOf(I.guid) : nullptr;
    ui.textShadow(fn, PX(198), PY(24) - static_cast<float>(fn.lineHeight()) * 0.5f, name ? name->c_str() : "",
                  kColWhite, Align::Center);
    if (I.tab == 1 && e) {
        // InspectPaperDollFrame_SetLevel: PLAYER_LEVEL with UnitRace / UnitClass (his gender), "??" for -1.
        char lv[16];
        if (level < 0) std::snprintf(lv, sizeof(lv), "??");
        else std::snprintf(lv, sizeof(lv), "%d", level);
        const std::string line = unitLevelText(race, cls, female, lv);
        ui.textShadow(fs, PX(198), PY(24) + 6 * kSY + 2, line.c_str(), kColGold, Align::Center);
    }
    // InspectFrameCloseButton (CENTER at TOPRIGHT -44,-25).
    {
        const Rect cr{PX(340) - 10, PY(25) - 10, 20, 20};
        const bool f = focusable && winFocus_.addStop(kIdClose, cr);
        const pp::Texture& t = f && winFocus_.isPressed(kIdClose) && npcArt_.closeDown.pixels ? npcArt_.closeDown
                                                                                              : npcArt_.close;
        if (t.pixels) ui.image(t, cr);
        if (f) ui.fill(cr, 0x40FFFFFFu);
    }
    if (I.tab == 1) {
        // InspectModelFrame (TOPLEFT 65,-78, 233 x 300): the square that fits the frame's model area, the full height.
        const Rect model{PX(65), PY(78), 233 * kSX, 300 * kSY};
        if (inspModelOk_ && inspModelTex_.pixels) {
            const float side = std::min(PX(298) - model.x, model.h);
            ui.image(inspModelTex_, Rect{model.x + (PX(298) - model.x - side) * 0.5f, model.y + (model.h - side) * 0.5f,
                                         side, side});
        }
        // InspectModelRotateLeftButton / RightButton (35 x 35 at the model's TOPLEFT).
        const Rect rl{model.x, model.y, 35 * kSY, 35 * kSY}, rr{model.x + 35 * kSY, model.y, 35 * kSY, 35 * kSY};
        if (I.rotLeft.pixels) ui.image(I.rotLeft, rl);
        if (I.rotRight.pixels) ui.image(I.rotRight, rr);
        if (focusable && winFocus_.addStop(kIdRotLeft, rl)) ui.fill(rl, 0x40FFFFFFu);
        if (focusable && winFocus_.addStop(kIdRotRight, rr)) ui.fill(rr, 0x40FFFFFFu);
        // The slots: GetInventoryItemTexture(unit, id) - the visible item's icon, else the empty slot's art.
        auto slot = [&](int s, float ux, float uy) {
            const Rect r{PX(ux), PY(uy), kSlot, kSlot};
            const uint32_t id = kIdSlot0 + static_cast<uint32_t>(s);
            const uint32_t entry = e ? e->getField(static_cast<uint16_t>(kVisibleItemBase + s * 2)) : 0u;
            uint32_t displayId = 0;
            if (entry && I.slotEntry[s] == entry && I.slotDisplay[s]) {
                displayId = I.slotDisplay[s];
            } else if (entry) {
                uint8_t invType = 0;
                if (!items_.itemDisplay(entry, displayId, invType)) {
                    if (const auto* info = session_.getItemInfo(entry); info && info->valid) displayId = info->displayInfoId;
                    else session_.ensureItemInfo(entry);
                }
            }
            I.slotEntry[s] = entry;
            I.slotDisplay[s] = displayId;
            const bool f = focusable && winFocus_.addStop(id, r);
            if (entry) {
                drawItemButton(ui, r, displayId, 1, -1, 0xFFFFFFFFu);
            } else {
                const bool relic = s == 17 && hasRelicSlot(cls) && I.relic.pixels;
                const pp::Texture& art = relic ? I.relic : I.slot[s];
                if (art.pixels) ui.image(art, r);
                if (spellArt_.slot.pixels) {
                    const float c = r.w * 64.0f / 37.0f;
                    ui.image(spellArt_.slot, Rect{r.x + r.w * 0.5f - c * 0.5f, r.y + r.h * 0.5f - c * 0.5f, c, c});
                }
            }
            if (f) {
                ui.fill(r, 0x50FFFFFFu);
                I.tipSlot = s;
                I.tipRect = Rect{r.x + ui.offsetX(), r.y + ui.offsetY(), r.w, r.h};  // screen: drawn after the windows
            }
        };
        for (int i = 0; i < 8; ++i) {
            slot(kLeftCol[i], 21, 74 + static_cast<float>(i) * 41);   // InspectHeadSlot TOPLEFT 21,-74, -4 apart
            slot(kRightCol[i], 305, 74 + static_cast<float>(i) * 41);  // InspectHandsSlot TOPLEFT 305,-74
        }
        for (int i = 0; i < 3; ++i) slot(kWeapons[i], 122 + static_cast<float>(i) * 42, 385);  // MainHand BOTTOMLEFT 122,127
    }

    // InspectFrameTab1..3: CENTER at BOTTOMLEFT 60,61; the next one LEFT at the previous RIGHT -16. The talents tab
    // is disabled under level 10 (InspectFrame_UpdateTalentTab).
    {
        const float th = 32 * kSY, cap = 20.0f * kSX;
        float x = 0.0f;
        for (int k = 0; k < 3; ++k) {
            const float w = std::max(60.0f, static_cast<float>(fs.measure(pp::cs(kTabText[k]))) + 2.0f * cap + 4.0f);
            if (k == 0) x = PX(60) - w * 0.5f;
            const float ty = PY(512 - 61) - th * 0.5f;
            const bool disabled = k == 2 && level > 0 && level < 10;
            if (disabled) {
                const pp::Texture& t = tabArt_[1];
                if (t.pixels) {
                    const float tw = static_cast<float>(t.uw), tht = static_cast<float>(t.uh), ct = tw * 20.0f / 128.0f;
                    ui.image(t, Rect{x, ty, cap, th}, SrcRect{0, 0, ct, tht});
                    ui.image(t, Rect{x + cap, ty, w - 2 * cap, th}, SrcRect{ct, 0, tw - 2 * ct, tht});
                    ui.image(t, Rect{x + w - cap, ty, cap, th}, SrcRect{tw - ct, 0, ct, tht});
                }
                ui.textShadow(fs, x + w * 0.5f, ty + (th - static_cast<float>(fs.lineHeight())) * 0.5f - 1.0f,
                              pp::cs(kTabText[k]), kColDisabled, Align::Center);
            } else {
                drawFrameTab(ui, kIdTab1 + static_cast<uint32_t>(k), x, ty, th, pp::cs(kTabText[k]), I.tab == k + 1, focusable);
            }
            if (focusable && winFocus_.focused() == kIdTab1 + static_cast<uint32_t>(k)) {
                I.tipTab = k;
                I.tipRect = Rect{x + ui.offsetX(), ty + ui.offsetY(), w, th};
            }
            x += w - 16.0f * kSX;
        }
    }
    if (I.tab != 1) drawInspectTab(ui, I.tab, focusable);
    return true;
}

// Cross on the frame's widgets.
bool WorldScreen::inspectAction(uint32_t act) {
    if (act < 0x6F00 || act > 0x6FFF || !insp_) return false;
    InspectUi& I = *insp_;
    if (act == kIdClose) {
        closeWindow(kWinInspect);
    } else if (act == kIdRotLeft || act == kIdRotRight) {
        // InspectModelRotate*Button_OnClick: 0.03 rad a click; held, InspectModelFrame_OnUpdate turns it on
        // (updateInspect) - PSP: Cross held on the button.
        I.rotation += act == kIdRotLeft ? -0.03f : 0.03f;
        playSoundByName("igInventoryRotateCharacter");
    } else if (act >= kIdTalTab0 && act < kIdTalTab0 + 3) {  // InspectTalentFrameTab_OnClick
        I.talTab = static_cast<int>(act - kIdTalTab0);
        I.talScroll = 0.0f;
        playSoundByName("igCharacterInfoTab");
    } else if (act == kIdTalUp || act == kIdTalDown) {  // the scroll arrows: half a page (InspectTalentFrameDownArrow)
        I.talScroll += act == kIdTalUp ? -63.0f : 63.0f;
        playSoundByName("UChatScrollButton");
    } else if (act >= kIdTab1 && act < kIdTab1 + 3) {
        const uint8_t tab = static_cast<uint8_t>(act - kIdTab1 + 1);
        auto e = session_.entities().getEntity(I.guid);
        const int level = e ? static_cast<int>(e->getField(game::fieldIndex(game::UF::UNIT_FIELD_LEVEL))) : 0;
        if (tab == 3 && level > 0 && level < 10) return true;  // PanelTemplates_DisableTab
        if (tab != I.tab) {
            playSoundByName("igCharacterInfoTab");  // InspectFrameTab_OnClick
            I.tab = tab;
            inspectTabShown(tab);
        }
    }
    return true;
}

// After all the windows: the slot's GameTooltip (SetInventoryItem, else the slot's name) or a tab's.
void WorldScreen::drawInspectOverlay(UiRenderer& ui) {
    if (!insp_ || !windowOpen(kWinInspect)) return;
    InspectUi& I = *insp_;
    if (I.tipTab >= 0) {
        drawInspectTip(ui, I.tipRect, pp::cs(kTabTip[I.tipTab]), kColWhite);
        return;
    }
    if (I.tab == 3 && I.tipTalent) {  // GameTooltip:SetTalent(tab, id, inspect)
        const game::TalentEntry* t = session_.inspectTalentEntry(I.tipTalent);
        if (!t) return;
        const auto& ranks = session_.inspectResult().talentRanks;
        auto rankOf = [&](uint32_t id) -> int {
            auto it = ranks.find(id);
            return it != ranks.end() ? it->second : 0;
        };
        const int rank = std::min(rankOf(t->talentId), 5);  // rankSpells[5] (the wire's u8 unchecked)
        const uint32_t spell = t->rankSpells[rank > 0 ? rank - 1 : 0];
        const std::string& name = spellDb_.name(spell ? spell : t->rankSpells[0]);
        std::vector<std::pair<std::string, uint32_t>> lines;
        lines.push_back({name.empty() ? std::string(CLIENT_STR(UNKNOWN)) : name, kColWhite});
        lines.push_back({CLIENT_FMT(TOOLTIP_TALENT_RANK, rank, static_cast<unsigned>(t->maxRank)), kColWhite});
        // TOOLTIP_TALENT_TIER_POINTS / TOOLTIP_TALENT_PREREQ (red) for what the inspected tree does not meet.
        int tabSpent = 0;
        std::string tabName;
        for (const auto& tb : session_.inspectTalentTabs())
            if (tb.tabId == t->tabId) tabName = tb.name;
        for (const auto& [tid, rk] : ranks)
            if (const game::TalentEntry* e = session_.inspectTalentEntry(tid); e && e->tabId == t->tabId) tabSpent += rk;
        if (static_cast<int>(t->row) * 5 > tabSpent) {
            const int need = static_cast<int>(t->row) * 5;
            lines.push_back({pp::text::plural(CLIENT_FMT(TOOLTIP_TALENT_TIER_POINTS, need, tabName)), 0xFF1A1AFFu});
        }
        for (int p = 0; p < 3; ++p) {
            if (t->prereqTalent[p] == 0 || rankOf(t->prereqTalent[p]) >= t->prereqRank[p] + 1) continue;
            const game::TalentEntry* pre = session_.inspectTalentEntry(t->prereqTalent[p]);
            const int need = t->prereqRank[p] + 1;
            const std::string& pn = pre ? spellDb_.name(pre->rankSpells[0]) : std::string();
            lines.push_back({pp::text::plural(CLIENT_FMT(TOOLTIP_TALENT_PREREQ, need, pn)), 0xFF1A1AFFu});
        }
        drawTipLines(ui, kit_, I.tipRect, lines);
        return;
    }
    if (I.tipSlot < 0) return;
    auto e = session_.entities().getEntity(I.guid);
    const uint32_t entry = e ? e->getField(static_cast<uint16_t>(kVisibleItemBase + I.tipSlot * 2)) : 0u;
    if (!entry) {
        const uint8_t cls = e ? static_cast<uint8_t>((e->getField(game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0)) >> 8) & 0xFF)
                              : 0;
        const bool relic = I.tipSlot == 17 && hasRelicSlot(cls);
        drawInspectTip(ui, I.tipRect, pp::cs(relic ? kRelicSlot : kSlotName[I.tipSlot]), kColWhite);
        return;
    }
    const auto* info = session_.getItemInfo(entry);
    if (!info || !info->valid) {
        session_.ensureItemInfo(entry);
        drawInspectTip(ui, I.tipRect, CLIENT_STR(RETRIEVING_ITEM_INFO), 0xFF2020FFu);  // red
        return;
    }
    game::ItemDef def = session_.protoItemDef(entry, 1);
    // PSP: the inspected slot's enchantments, gems and sockets through WorldSession::itemEnchantments.
    // + the inspected player and the inspect's version (bits 8..47) - the tooltip cache's key changes
    // with them (another player's same item, the enchantments that came later).
    const uint64_t tag = (I.guid ^ (static_cast<uint64_t>(session_.inspectResult().version) << 24)) & 0xFFFFFFFFFFull;
    def.guid = pp::WorldSession::kInspectItemGuid | (tag << 8) | static_cast<uint64_t>(I.tipSlot);
    drawItemTooltip(ui, def, I.tipRect);
}


// A one-line GameTooltip (SetOwner ANCHOR_RIGHT + SetText): the empty slot's name, a tab's name. The header font,
// the tooltip backdrop of the kit.
void WorldScreen::drawInspectTip(UiRenderer& ui, const Rect& a, const char* text, uint32_t color) {
    drawTipLines(ui, kit_, a, {{text, color}});
}

// The subframe's OnShow: InspectPVPFrame_OnShow asks for the honour data once (RequestInspectHonorData).
void WorldScreen::inspectTabShown(uint8_t tab) {
    if (tab == 2) {
        const auto& r = session_.inspectResult();
        if (!r.honorKnown) session_.requestInspectHonorData();
    }
}



// ---- InspectPVPFrame (tab 2) ----------------------------------------------------------------------------------
// InspectPVPFrame.xml / .lua at the frame's 0.6 x 0.55: UI-Character-General quarters + UI-Character-PVP (insp_pvp,
// baked), InspectPVPFrameHonor (PVP_LABEL_HONOR + the player's faction icon, InspectPVPFrame_SetFaction - the points are
// hidden), InspectPVPFrameArena (PVP_LABEL_ARENA + PVP-ArenaPoints-Icon), InspectPVPHonor (kills / honour today, yesterday,
// lifetime: GetInspectHonorData = MSG_INSPECT_HONOR_STATS, asked once by InspectPVPFrame_OnShow), InspectPVPTeam1..3
// (InspectPVPTeam_Update: the teams of sizes 2, 3, 5 that GetInspectArenaTeamData knows first, then the rest unused -
// alpha 0.4, the standard at 0.1, PVP_TEAMSIZE; a known team: its banner / border / emblem in its colours, name,
// ARENA_TEAM_RATING and the rating, ARENA_THIS_SEASON: games, wins - losses, the player's rating). Art while the tab shows.
void WorldScreen::InspectUi::loadPvpArt() {
    if (pvpTried) return;
    pvpTried = true;
    char path[256];
    pp::fsPath(path, sizeof(path), "data/ui.pak");
    pp::Pak pak;
    if (!pak.open(path)) return;
    auto load = [&](const char* name, pp::Texture& t) {
        const pp::PakEntry* e = pak.find(name);
        void* blob = e ? pak.load(e) : nullptr;
        if (blob && !pp::textureFromBlob(blob, e->size, t, true)) pp::pakFree(blob);
        if (t.pixels) pvpBytes += pp::textureBytes(t);
    };
    load("insp_pvp", pvpFrame);
    load("insp_pvp_top", pvpTop);
    load("insp_pvp_bottom", pvpBottom);
    load("insp_pvp_left", pvpLeft);
    load("insp_pvp_right", pvpRight);
    load("insp_pvp_bar", pvpBar);
    load("insp_pvp_line", pvpLine);
    load("insp_pvp_arena", pvpArena);
    load("insp_pvp_alliance", pvpFaction[0]);
    load("insp_pvp_horde", pvpFaction[1]);
    static const int kSizes[3] = {2, 3, 5};
    char name[32];
    for (int k = 0; k < 3; ++k) {
        std::snprintf(name, sizeof(name), "insp_banner_%d", kSizes[k]);
        load(name, pvpBanner[k]);
    }
    pak.close();
    PSP_LOGI(pp::tag::Gfx, "inspect_pvp_art frame=%d kb=%u", pvpFrame.pixels ? 1 : 0, static_cast<unsigned>(pvpBytes / 1024u));
}

// A shown team's border / emblem (by its style), read when its row first needs it.
void WorldScreen::InspectUi::loadTeamArt(int row, int size, int border, int emblem) {
    if (rowBorderKey[row] == size * 100 + border && rowEmblemKey[row] == emblem) return;
    for (pp::Texture* t : {&rowBorder[row], &rowEmblem[row]})
        if (t->pixels) {
            pvpBytes -= std::min(pvpBytes, static_cast<uint32_t>(pp::textureBytes(*t)));
            pp::textureRelease(*t);
        }
    rowBorderKey[row] = size * 100 + border;
    rowEmblemKey[row] = emblem;
    char path[256];
    pp::fsPath(path, sizeof(path), "data/ui.pak");
    pp::Pak pak;
    if (!pak.open(path)) return;
    auto load = [&](const char* name, pp::Texture& t) {
        const pp::PakEntry* e = pak.find(name);
        void* blob = e ? pak.load(e) : nullptr;
        if (blob && !pp::textureFromBlob(blob, e->size, t, true)) pp::pakFree(blob);
        if (t.pixels) pvpBytes += pp::textureBytes(t);
    };
    char name[32];
    if (border > 0) {
        std::snprintf(name, sizeof(name), "insp_border_%d_%d", size, border);
        load(name, rowBorder[row]);
    }
    if (emblem > 0) {
        std::snprintf(name, sizeof(name), "insp_emblem_%d", emblem);
        load(name, rowEmblem[row]);
    }
    pak.close();
}

void WorldScreen::InspectUi::releasePvpArt() {
    if (!pvpTried) return;
    for (pp::Texture* t : {&pvpFrame, &pvpTop, &pvpBottom, &pvpLeft, &pvpRight, &pvpBar, &pvpLine, &pvpArena,
                           &pvpFaction[0], &pvpFaction[1], &pvpBanner[0], &pvpBanner[1], &pvpBanner[2], &rowBorder[0],
                           &rowBorder[1], &rowBorder[2], &rowEmblem[0], &rowEmblem[1], &rowEmblem[2]})
        if (t->pixels) pp::textureRelease(*t);
    for (int k = 0; k < 3; ++k) rowBorderKey[k] = rowEmblemKey[k] = -1;
    PSP_LOGI(pp::tag::Gfx, "inspect_pvp_art_free kb=%u", static_cast<unsigned>(pvpBytes / 1024u));
    pvpTried = false;
    pvpBytes = 0;
}

// ---- InspectTalentFrame (tab 3) -----------------------------------------------------------------------------
// InspectTalentFrame.xml / .lua + TalentFrameBase.lua: the same art as PlayerTalentFrame (tal_frame: the General
// quarters, the points bar, the scroll bar; tb_<background> of each tree; the buttons' slot / rank border, branches,
// arrows) and the TabButtonTemplate tabs at the top (HelpFrameTab-Active / -Inactive, insp_htab_*) named by the
// trees; the tree of the inspected player's class with his ranks (SMSG_INSPECT_TALENT, the active group).
void WorldScreen::InspectUi::loadTalArt(const std::vector<std::string>& backgrounds) {
    if (talTried) return;
    talTried = true;
    char path[256];
    pp::fsPath(path, sizeof(path), "data/ui.pak");
    pp::Pak pak;
    if (!pak.open(path)) return;
    auto load = [&](const char* name, pp::Texture& t) {
        const pp::PakEntry* e = pak.find(name);
        void* blob = e ? pak.load(e) : nullptr;
        if (blob && !pp::textureFromBlob(blob, e->size, t, true)) pp::pakFree(blob);
        if (t.pixels) talBytes += pp::textureBytes(t);
    };
    load("tal_frame", talFrame);
    load("tal_slot", talSlot);
    load("tal_branches", talBranches);
    load("tal_arrows", talArrows);
    load("tal_rank", talRank);
    load("insp_htab_active", talTabOn);
    load("insp_htab_inactive", talTabOff);
    for (size_t k = 0; k < backgrounds.size() && k < 3; ++k) {
        std::string n = "tb_" + backgrounds[k];
        for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        load(n.c_str(), talBg[k]);
    }
    pak.close();
    PSP_LOGI(pp::tag::Gfx, "inspect_talent_art frame=%d kb=%u", talFrame.pixels ? 1 : 0,
             static_cast<unsigned>(talBytes / 1024u));
}

void WorldScreen::InspectUi::releaseTalArt() {
    if (!talTried) return;
    for (pp::Texture* t : {&talFrame, &talSlot, &talBranches, &talArrows, &talRank, &talTabOn, &talTabOff, &talBg[0],
                           &talBg[1], &talBg[2]})
        if (t->pixels) pp::textureRelease(*t);
    PSP_LOGI(pp::tag::Gfx, "inspect_talent_art_free kb=%u", static_cast<unsigned>(talBytes / 1024u));
    talTried = false;
    talBytes = 0;
}

void WorldScreen::drawInspectTab(UiRenderer& ui, uint8_t tab, bool focusable) {
    InspectUi& I = inspectUi();
    const Font& fs = kit_.font(kFontSmall);
    const Font& fn = kit_.font(kFontNormal);
    const float lh = static_cast<float>(fs.lineHeight());
    const auto& r = session_.inspectResult();
    char buf[160];
    if (tab == 2) {
        // InspectPVPFrameHonor: PVP_LABEL_HONOR (GameFontHighlightSmall) + the faction icon (UnitFactionGroup("player")).
        {
            const char* label = CLIENT_STR(PVP_LABEL_HONOR);
            const float x = PX(132), cy = PY(48);
            ui.textShadow(fs, x, cy - lh * 0.5f, label, kColWhite);
            const game::Character* me = session_.activeCharacter();
            const int race = me ? static_cast<int>(me->race) : 0;
            const bool horde = race == 2 || race == 5 || race == 6 || race == 8 || race == 10;
            const pp::Texture& ic = I.pvpFaction[horde ? 1 : 0];
            const float ix = x + static_cast<float>(fs.measure(label)) + (15 + 4) * kSX;
            if (ic.pixels) ui.image(ic, Rect{ix, PY(54) - 14 * kSY, 28 * kSX, 28 * kSY});
        }
        // InspectPVPFrameArena: PVP_LABEL_ARENA + PVP-ArenaPoints-Icon.
        {
            const char* label = CLIENT_STR(PVP_LABEL_ARENA);
            const float x = PX(134), cy = PY(141);
            ui.textShadow(fs, x, cy - lh * 0.5f, label, kColWhite);
            const float ix = x + static_cast<float>(fs.measure(label)) + (15 + 5) * kSX;
            if (I.pvpArena.pixels) ui.image(I.pvpArena, Rect{ix, cy - 7.5f * kSY, 17 * kSX, 15 * kSY});
        }
        // InspectPVPHonor (GetInspectHonorData: todayHK, todayHonor, yesterdayHK, yesterdayHonor, lifetimeHK).
        {
            const float kx = PX(54), ky = PY(88);
            const char* kills = CLIENT_STR(KILLS);
            const float kw = static_cast<float>(fs.measure(kills));
            ui.textShadow(fs, kx, ky, kills, kColDisabled);
            ui.textShadow(fs, kx + kw, ky + lh + 10 * kSY, CLIENT_STR(HONOR), kColDisabled, Align::Right);
            if (I.pvpLine.pixels)  // InspectPVPFrameLine1 (alpha 0.3)
                ui.image(I.pvpLine, Rect{kx + kw - 10 * kSX, ky - 4 * kSY - 5 * kSY, 250 * kSX, 5 * kSY}, 0x4DFFFFFFu);
            const char* labels[3] = {CLIENT_STR(HONOR_TODAY), CLIENT_STR(HONOR_YESTERDAY), CLIENT_STR(HONOR_LIFETIME)};
            char v[3][2][16];
            std::snprintf(v[0][0], sizeof(v[0][0]), "%u", static_cast<unsigned>(r.todayKills));
            std::snprintf(v[0][1], sizeof(v[0][1]), "%u", static_cast<unsigned>(r.todayHonor));
            std::snprintf(v[1][0], sizeof(v[1][0]), "%u", static_cast<unsigned>(r.yesterdayKills));
            std::snprintf(v[1][1], sizeof(v[1][1]), "%u", static_cast<unsigned>(r.yesterdayHonor));
            std::snprintf(v[2][0], sizeof(v[2][0]), "%u", static_cast<unsigned>(r.lifetimeKills));
            std::snprintf(v[2][1], sizeof(v[2][1]), "-");
            float lx = kx + kw + 30 * kSX;
            const float ly = ky - 10 * kSY - lh;
            for (int c = 0; c < 3; ++c) {
                const float w = static_cast<float>(fs.measure(labels[c]));
                const float cx = lx + w * 0.5f;
                ui.textShadow(fs, lx, ly, labels[c], kColDisabled);
                ui.textShadow(fs, cx, ly + lh + 10 * kSY, v[c][0], kColWhite, Align::Center);
                ui.textShadow(fs, cx, ly + 2 * lh + 20 * kSY, v[c][1], c == 2 ? kColDisabled : kColWhite, Align::Center);
                lx += w + 30 * kSX;
            }
        }
        // InspectPVPTeam1..3 (InspectPVPTeam_Update).
        {
            static const int kSizes[3] = {2, 3, 5};
            int index[3] = {-1, -1, -1};  // ARENA_TEAMS[k].index: the slot whose team has that size
            for (int k = 0; k < 3; ++k)
                for (int s = 0; s < 3; ++s)
                    if (r.arena[s].teamId && r.arena[s].infoKnown && static_cast<int>(r.arena[s].type) == kSizes[k])
                        index[k] = s;
            int order[3], n = 0;
            for (int k = 0; k < 3; ++k)
                if (index[k] >= 0) order[n++] = k;
            for (int k = 0; k < 3; ++k)
                if (index[k] < 0) order[n++] = k;
            static const float kRowY[3] = {207, 292, 377};  // LEFT of the background, +85 / 0 / -85
            for (int row = 0; row < 3; ++row) {
                const int k = order[row];
                const bool used = index[k] >= 0;
                const float cy = kRowY[row];
                const auto* team = used ? &r.arena[index[k]] : nullptr;
                // PVPTeamStandardTemplate: Bar, Banner (its colour), Border, Emblem; alpha 0.1 when unused.
                const uint32_t stdA = used ? 0xFFu : 0x1Au;
                if (I.pvpBar.pixels) ui.image(I.pvpBar, Rect{PX(22), PY(cy - 38), 50 * kSX, 13 * kSY}, stdA << 24 | 0xFFFFFFu);
                const Rect banner{PX(52 - 22.5f), PY(cy - 36), 45 * kSX, 90 * kSY};
                const pp::Texture& bt = I.pvpBanner[k];
                if (bt.pixels) ui.image(bt, banner, used ? teamTint(team->background, 0xFF) : (stdA << 24 | 0xFFFFFFu));
                if (used) {
                    const int border = static_cast<int>(team->border), emblem = static_cast<int>(team->emblem);
                    I.loadTeamArt(row, kSizes[k], border, emblem);
                    if (I.rowBorder[row].pixels) ui.image(I.rowBorder[row], banner, teamTint(team->borderColor, 0xFF));
                    if (I.rowEmblem[row].pixels)
                        ui.image(I.rowEmblem[row], Rect{PX(47 - 12), PY(cy - 8 - 12), 24 * kSX, 24 * kSY},
                                 teamTint(team->emblemColor, 0xFF));
                }
                // PVPTeamBorderTemplate (alpha 0.4 when unused): Top, Left, the black Background, Right, Bottom.
                const uint32_t a = used ? 0xFFu : 0x66u;
                const uint32_t tint = a << 24 | 0xFFFFFFu;
                ui.fill(Rect{PX(72), PY(cy - 31), 265 * kSX, 65 * kSY}, a << 24);
                if (I.pvpTop.pixels) ui.image(I.pvpTop, Rect{PX(63), PY(cy - 38), 279 * kSX, 11 * kSY}, tint);
                if (I.pvpLeft.pixels) ui.image(I.pvpLeft, Rect{PX(63), PY(cy - 27), 10 * kSX, 55 * kSY}, tint);
                if (I.pvpRight.pixels) ui.image(I.pvpRight, Rect{PX(332), PY(cy - 27), 10 * kSX, 55 * kSY}, tint);
                if (I.pvpBottom.pixels) ui.image(I.pvpBottom, Rect{PX(63), PY(cy + 27), 279 * kSX, 17 * kSY}, tint);
                if (!used) {  // $parentTeamType: PVP_TEAMSIZE (GameFontDisableLarge)
                    const std::string size = CLIENT_FMT(PVP_TEAMSIZE, kSizes[k], kSizes[k]);
                    ui.textShadow(fn, PX(63 + 150 - 8), PY(cy) - static_cast<float>(fn.lineHeight()) * 0.5f, size.c_str(),
                                  kColDisabled, Align::Center);
                    continue;
                }
                // PVPTeamDataTemplate.
                const float nx = PX(78), ny = PY(cy - 24);
                ui.pushClip(Rect{nx, ny - 1, 150 * kSX - 2 * kSX, lh + 2});
                ui.textShadow(fs, nx, ny, team->name.c_str(), kColGold);
                ui.popClip();
                ui.textShadow(fs, PX(226 + 75), ny, CLIENT_STR(ARENA_TEAM_RATING), kColDisabled, Align::Right);
                std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(team->rating));
                ui.textShadow(fs, PX(303), ny, buf, kColGold);
                if (I.pvpLine.pixels) ui.image(I.pvpLine, Rect{PX(80), PY(cy - 7), 250 * kSX, 2 * kSY});
                ui.textShadow(fs, nx, PY(cy + 26) - lh, CLIENT_STR(ARENA_THIS_SEASON), kColWhite);
                const float labelY = PY(cy - 2), valueY = PY(cy + 16);
                ui.textShadow(fs, PX(136 + 27.5f), labelY, CLIENT_STR(GAMES), kColDisabled, Align::Center);
                std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(team->games));
                ui.textShadow(fs, PX(136 + 27.5f), valueY, buf, kColWhite, Align::Center);
                ui.textShadow(fs, PX(191 + 37.5f), labelY, CLIENT_STR(WIN_LOSS), kColDisabled, Align::Center);
                const float sep = PX(191 + 37.5f - 2);
                ui.textShadow(fs, sep, valueY, " - ", kColWhite, Align::Center);
                const float sw = static_cast<float>(fs.measure(" - ")) * 0.5f;
                std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(team->wins));
                ui.textShadow(fs, sep - sw, valueY, buf, kColWhite, Align::Right);
                std::snprintf(buf, sizeof(buf), "%u",
                              static_cast<unsigned>(team->games >= team->wins ? team->games - team->wins : 0u));
                ui.textShadow(fs, sep + sw, valueY, buf, kColWhite);
                ui.textShadow(fs, PX(266 + 33.5f), labelY, CLIENT_STR(RATING), kColDisabled, Align::Center);
                std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(team->personalRating));
                ui.textShadow(fs, PX(266 + 33.5f), valueY, buf, kColWhite, Align::Center);
            }
        }
        return;
    }

    // tab 3: InspectTalentFrame.
    if (!session_.loadInspectTalents(I.cls)) {
        // The load is synchronous - with the class known, a failure is data the pak lacks.
        ui.textShadow(fs, PX(30), PY(90), I.cls ? OWN_STR(NO_TALENT_DATA) : OWN_STR(INSPECT_TALENTS_LOADING),
                      kColRed);
        return;
    }
    const auto& tabs = session_.inspectTalentTabs();
    if (!I.talTried) {
        std::vector<std::string> bgs;
        for (const auto& t : tabs) bgs.push_back(t.backgroundFile);
        I.loadTalArt(bgs);
    }
    auto spentIn = [&](uint32_t tabId) {
        int n = 0;
        for (const auto& [tid, rank] : r.talentRanks)
            if (const game::TalentEntry* e = session_.inspectTalentEntry(tid); e && e->tabId == tabId) n += rank;
        return n;
    };
    // InspectTalentFrame_Update: no tab selected (each show) - the tree with the most points, else the first.
    if (I.talTab < 0 || I.talTab >= static_cast<int>(std::min<size_t>(tabs.size(), 3))) {
        int best = 0, bestPts = 0;
        for (size_t k = 0; k < tabs.size() && k < 3; ++k)
            if (const int p = spentIn(tabs[k].tabId); p > bestPts) {
                bestPts = p;
                best = static_cast<int>(k);
            }
        I.talTab = best;
        I.talScroll = 0.0f;
    }
    const game::TalentTabEntry& tabE = tabs[static_cast<size_t>(I.talTab)];
    const int tabSpent = spentIn(tabE.tabId);
    std::vector<const game::TalentEntry*> tal;
    for (const auto& t : session_.inspectTalents())
        if (t.tabId == tabE.tabId) tal.push_back(&t);
    std::sort(tal.begin(), tal.end(), [](const auto* a, const auto* b) {
        return a->row != b->row ? a->row < b->row : a->column < b->column;
    });
    if (tal.size() > 64) tal.resize(64);
    int tiers = 0;
    for (const auto* t : tal) tiers = std::max(tiers, t->row + 1);
    auto rankOf = [&](uint32_t id) -> uint8_t {
        auto it = r.talentRanks.find(id);
        return it != r.talentRanks.end() ? it->second : 0;
    };
    // InspectTalentFrameScrollFrame (296 x 332 at 23,77): the tree's background under the scrolling child.
    const Rect pane{PX(23), PY(77), 296 * kSX, 332 * kSY};
    const float contentH = 20.0f + 63.0f * static_cast<float>(std::max(0, tiers - 1)) + 37.0f + 20.0f;
    const float maxScroll = std::max(0.0f, contentH - 332.0f);
    const uint32_t focused = winFocus_.focused();
    if (focusable && focused >= kIdTal0 && focused < kIdTal0 + tal.size() && focused != I.talFocus) {
        const auto* t = tal[focused - kIdTal0];
        const float y = 20.0f + 63.0f * static_cast<float>(t->row) - I.talScroll;
        if (y + 37.0f + 63.0f > 332.0f && I.talScroll < maxScroll) I.talScroll += 63.0f;
        else if (y - 63.0f < 0.0f && I.talScroll > 0.0f) I.talScroll -= 63.0f;
    }
    I.talFocus = focused;
    I.talScroll = std::clamp(I.talScroll, 0.0f, maxScroll);
    const float scroll = I.talScroll;
    auto childX = [&](float x) { return PX(23 + x); };
    auto childY = [&](float y) { return PY(77 + y - scroll); };
    if (I.talTab < 3 && I.talBg[static_cast<size_t>(I.talTab)].pixels) {
        const pp::Texture& bg = I.talBg[static_cast<size_t>(I.talTab)];
        ui.image(bg, pane, SrcRect{0, 0, bg.uw * 300.0f / 320.0f, bg.uh * 331.0f / 384.0f});
    } else {
        ui.fill(pane, 0xFF101010u);
    }
    // TalentFrame_Update with inspect: a talent without points is desaturated (forceDesaturated), the branches
    // lit where requirements are met.
    BranchGrid grid;
    struct State {
        uint8_t rank;
        bool lit;
    };
    std::vector<State> st(tal.size());
    for (size_t i = 0; i < tal.size(); ++i) {
        const game::TalentEntry& t = *tal[i];
        if (t.row >= kTiers || t.column >= kCols) continue;
        grid[t.row][t.column].id = static_cast<int>(i) + 1;
        const uint8_t rank = rankOf(t.talentId);
        const bool forceDesaturated = rank == 0;  // "(unspentPoints <= 0 or inspect) and rank == 0"
        const bool tierOpen = static_cast<int>(t.row) * 5 <= tabSpent;  // PLAYER_TALENTS_PER_TIER
        bool met = tierOpen && !forceDesaturated;
        for (int p = 0; p < 3; ++p) {
            if (t.prereqTalent[p] == 0) continue;
            const game::TalentEntry* pre = session_.inspectTalentEntry(t.prereqTalent[p]);
            if (!pre) continue;
            const bool learnable = rankOf(t.prereqTalent[p]) >= t.prereqRank[p] + 1;
            if (forceDesaturated || !learnable) met = false;
            drawLines(grid, t.row, t.column, pre->row, pre->column, met);
        }
        st[i] = State{rank, met};
    }
    std::vector<Piece> pieces;
    auto branch = [&](Branch b, int v, float x, float y) {
        if (v != 0) pieces.push_back(Piece{kBranchTC[b][v > 0 ? 0 : 1], x, y, false});
    };
    auto arrow = [&](Arrow a, int v, float x, float y) {
        if (v != 0) pieces.push_back(Piece{kArrowTC[a][v > 0 ? 0 : 1], x, y, true});
    };
    bool ignoreUp = false;
    for (int i = 0; i < kTiers; ++i) {
        for (int j = 0; j < kCols; ++j) {
            const BranchNode& node = grid[i][j];
            const float xo = static_cast<float>(j) * 63.0f + 35.0f + 2.0f;
            const float yo = static_cast<float>(i) * 63.0f + 20.0f + 2.0f;
            if (node.id) {
                if (node.up) {
                    if (!ignoreUp) branch(kUp, node.up, xo, yo - 32.0f);
                    else ignoreUp = false;
                }
                if (node.down) branch(kDown, node.down, xo, yo + 32.0f - 1.0f);
                if (node.left) branch(kLeft, node.left, xo - 32.0f, yo);
                if (node.right) {
                    const BranchNode* temp = j + 1 < kCols ? &grid[i][j + 1] : nullptr;
                    if (temp && temp->left && temp->down < 0) branch(kRight, temp->down, xo + 32.0f, yo);
                    else branch(kRight, node.right, xo + 32.0f + 1.0f, yo);
                }
                arrow(kArrowRight, node.rightArrow, xo + 16.0f + 5.0f, yo);
                arrow(kArrowLeft, node.leftArrow, xo - 16.0f - 5.0f, yo);
                arrow(kArrowTop, node.topArrow, xo, yo - 16.0f - 5.0f);
            } else if (node.up && node.left && node.right) {
                branch(kTUp, node.up, xo, yo);
            } else if (node.down && node.left && node.right) {
                branch(kTDown, node.down, xo, yo);
            } else if (node.left && node.down) {
                branch(kTopRight, node.left, xo, yo);
                branch(kDown, node.down, xo, yo + 32.0f);
            } else if (node.left && node.up) {
                branch(kBottomRight, node.left, xo, yo);
            } else if (node.left && node.right) {
                branch(kRight, node.right, xo + 32.0f, yo);
                branch(kLeft, node.left, xo + 1.0f, yo);
            } else if (node.right && node.down) {
                branch(kTopLeft, node.right, xo, yo);
                branch(kDown, node.down, xo, yo + 32.0f);
            } else if (node.right && node.up) {
                branch(kBottomLeft, node.right, xo, yo);
            } else if (node.up && node.down) {
                branch(kUp, node.up, xo, yo);
                branch(kDown, node.down, xo, yo + 32.0f);
                ignoreUp = true;
            }
        }
    }
    ui.pushClip(pane);
    auto drawPiece = [&](const Piece& p) {
        const pp::Texture& t = p.arrow ? I.talArrows : I.talBranches;
        const Rect pr{childX(p.x), childY(p.y), 32 * kSX, 32 * kSY};
        if (t.pixels)
            ui.image(t, pr, SrcRect{p.tc.l * t.uw, p.tc.t * t.uh, (p.tc.r - p.tc.l) * t.uw, (p.tc.b - p.tc.t) * t.uh});
    };
    for (const Piece& p : pieces)
        if (!p.arrow) drawPiece(p);
    const Font& ft = kit_.font(kFontTiny);
    const float side = 37.0f * kSY;
    I.tipTalent = 0;
    for (size_t i = 0; i < tal.size(); ++i) {
        const game::TalentEntry& t = *tal[i];
        const float bx = childX(35.0f + 63.0f * static_cast<float>(t.column)) + (37 * kSX - side) * 0.5f;
        const float by = childY(20.0f + 63.0f * static_cast<float>(t.row));
        const Rect b{bx, by, side, side};
        if (by + side < pane.y || by > pane.y + pane.h) continue;
        const bool inside = by >= pane.y - 1 && by + side <= pane.y + pane.h + 1;
        const bool f = focusable && inside && winFocus_.addStop(kIdTal0 + static_cast<uint32_t>(i), b);
        const State& s = st[i];
        const uint32_t slotCol = !s.lit ? 0xFF808080u : s.rank < t.maxRank ? 0xFF1AFF1Au : 0xFF00D1FFu;
        if (I.talSlot.pixels) {
            const float cw = 64 * kSX * side / (37 * kSX), ch = 64 * kSY;
            ui.image(I.talSlot, Rect{b.x + side * 0.5f - cw * 0.5f, b.y + side * 0.5f - ch * 0.5f, cw, ch}, slotCol);
        }
        if (const pp::Texture* tex = spellDb_.icon(t.rankSpells[0], spellFrame_)) {
            ui.image(*tex, b, s.lit ? 0xFFFFFFFFu : 0xFFA6A6A6u);  // SetItemButtonDesaturated(1, .65, .65, .65)
            if (!s.lit) ui.fill(b, 0x60595959u);
        } else {
            ui.fill(b, 0xFF404040u);
        }
        if (f && spellArt_.hilite.pixels) ui.image(spellArt_.hilite, b, 0xFFFFFFFFu, Blend::Add);
        if (s.lit || s.rank > 0) {  // $parentRankBorder + $parentRank
            const Rect rb{b.x + b.w - 32 * kSX * 0.5f, b.y + b.h - 32 * kSY * 0.5f, 32 * kSX, 32 * kSY};
            if (I.talRank.pixels) ui.image(I.talRank, rb, s.lit ? 0xFFFFFFFFu : 0xFF808080u);
            else ui.fill(Rect{rb.x + 3, rb.y + 4, rb.w - 6, rb.h - 8}, 0xC0000000u);
            std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(s.rank));
            const uint32_t rc = !s.lit ? 0xFF808080u : s.rank < t.maxRank ? 0xFF20FF20u : kColGold;
            ui.textShadow(ft, rb.x + rb.w * 0.5f, rb.y + (rb.h - static_cast<float>(ft.lineHeight())) * 0.5f, buf, rc,
                          Align::Center);
        }
        if (f) {
            I.tipTalent = t.talentId;
            I.tipRect = Rect{b.x + b.w + ui.offsetX(), b.y + ui.offsetY(), b.w, b.h};
        }
    }
    for (const Piece& p : pieces)
        if (p.arrow) drawPiece(p);
    ui.popClip();
    // The scroll bar's arrows (UIPanelScrollFrameTemplate; the bar art is in tal_frame).
    if (maxScroll > 0.0f) {
        const bool canUp = scroll > 0.5f, canDn = scroll < maxScroll - 0.5f;
        const Rect up{PX(326), PY(80), 11, 11}, dn{PX(326), PY(406) - 11, 11, 11};
        const pp::Texture& tu = canUp ? npcArt_.scrUp : npcArt_.scrUpDis;
        const pp::Texture& td = canDn ? npcArt_.scrDown : npcArt_.scrDownDis;
        if (tu.pixels) ui.image(tu, up);
        if (td.pixels) ui.image(td, dn);
        if (focusable && canUp && winFocus_.addStop(kIdTalUp, up)) ui.fill(up, 0x40FFFFFFu);
        if (focusable && canDn && winFocus_.addStop(kIdTalDown, dn)) ui.fill(dn, 0x40FFFFFFu);
    }
    // InspectTalentFramePointsBar: MASTERY_POINTS_SPENT left (the tree's name and its points), the unspent points
    // right (TALENT_POINTS: GetUnspentTalentPoints(inspect)).
    {
        const float cy = PY(418), xl = PX(28), xr = PX(336);
        std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(r.unspentTalents));
        const std::string un = buf;
        const std::string points = std::string(CLIENT_STR(TALENT_POINTS)) + ": ";
        const std::string left = CLIENT_FMT(MASTERY_POINTS_SPENT, tabE.name, "");  // the points drawn after it
        std::snprintf(buf, sizeof(buf), "%d", tabSpent);
        const std::string sp = buf;
        const Font* pf = &fs;
        if (static_cast<float>(fs.measure((left + sp).c_str()) + fs.measure((points + un).c_str())) + 8 >
            xr - xl)
            pf = &ft;  // PSP: the tiny font when the two do not fit
        const float y = cy - static_cast<float>(pf->lineHeight()) * 0.5f;
        const float unW = static_cast<float>(pf->measure(un.c_str()));
        ui.textShadow(*pf, xr, y, un.c_str(), kColWhite, Align::Right);
        ui.textShadow(*pf, xr - unW, y, points.c_str(), kColGold, Align::Right);
        const float rightStart = xr - unW - static_cast<float>(pf->measure(points.c_str())) - 6;
        ui.pushClip(Rect{xl, y - 2, std::max(0.0f, rightStart - xl), static_cast<float>(pf->lineHeight()) + 4});
        ui.textShadow(*pf, xl, y, left.c_str(), kColGold);
        ui.textShadow(*pf, xl + static_cast<float>(pf->measure(left.c_str())), y, sp.c_str(), kColWhite);
        ui.popClip();
    }
    // InspectTalentFrameTab1..3 (TabButtonTemplate, TOPLEFT 70,-40, the next at the previous RIGHT): the trees' names,
    // PanelTemplates_TabResize(tab, -10); HelpFrameTab-Active for the selected one (raised), -Inactive the others.
    {
        const float th = 32 * kSY, cap = 16 * kSX;
        float x = PX(70);
        for (size_t k = 0; k < tabs.size() && k < 3; ++k) {
            const bool sel = static_cast<int>(k) == I.talTab;
            // PanelTemplates_TabResize(tab, -10): the text + the two 16-unit ends - 10 units.
            const float w = std::max(2.0f * cap, static_cast<float>(fs.measure(tabs[k].name.c_str())) + 2 * cap - 10 * kSX);
            const float ty = PY(40) + (sel ? 3 * kSY : 0.0f);  // the selected one's (disabled) art 3 units lower
            const pp::Texture& t = sel ? I.talTabOn : I.talTabOff;
            if (t.pixels) {
                const float tw = static_cast<float>(t.uw), tth = static_cast<float>(t.uh), ct = tw * 0.25f;
                ui.image(t, Rect{x, ty, cap, th}, SrcRect{0, 0, ct, tth});
                ui.image(t, Rect{x + cap, ty, w - 2 * cap, th}, SrcRect{ct, 0, tw - 2 * ct, tth});
                ui.image(t, Rect{x + w - cap, ty, cap, th}, SrcRect{tw - ct, 0, ct, tth});
            }
            const Rect hit{x + 2, ty + 4, w - 4, th - 8};
            const bool f = focusable && !sel && winFocus_.addStop(kIdTalTab0 + static_cast<uint32_t>(k), hit);
            ui.textShadow(fs, x + w * 0.5f, ty + (th - lh) * 0.5f, tabs[k].name.c_str(), sel ? kColWhite : kColGold,
                          Align::Center);
            if (f) ui.fill(hit, 0x30FFFFFFu);
            x += w;
        }
    }
}

}  // namespace wowee::ui::psp
