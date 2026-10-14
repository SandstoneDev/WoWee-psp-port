// The pet's frames of the ruRU 3.3.5a client over the session's pet half (psp_world_session_pets.cpp):
//   - PetFrame (PetFrame.xml / .lua) at PlayerFrame TOPLEFT 80,-60 (the unit frames' 0.6): the round portrait (the pet's
//     own model, rendered once into a 64 x 64 VRAM target - SetPortraitTexture is a still in the client too), the name
//     (the name query's answer), UI-SmallTargetingFrame (-NoMana without power), health / mana / focus bars
//     (PowerBarColor), the numbers under the cursor (TextStatusBarText), PetAttackModeTexture (UI-Player-AttackStatus,
//     ADD, alpha 55..255 over 0.5 s - PET_ATTACK_START / _STOP), Debuff1..4, PetFrameHappiness (UI-PetHappiness by
//     GetPetHappiness, its tooltip PET_HAPPINESSn + PET_DAMAGE_PERCENTAGE); Circle = the unit menu (UnitPopup PET),
//     Cross = TargetUnit("pet");
//   - PetActionBarFrame (PetActionBarFrame.xml / .lua) at MainMenuBar BOTTOMLEFT 36,97 (the bar's 0.5): UI-PetBar, ten
//     PetActionButtons (UI-Quickslot2 / UI-Quickslot, the icon - a spell's or PET_*_TEXTURE of a command / reaction -
//     CheckButtonHilight while active (the attack at alpha 0.5 and the red flash while the pet attacks), the cooldown
//     sweep, UI-AutoCastableOverlay on an autocastable spell and the AutoCastShine sparkles while autocast is on, grey
//     when GetPetActionSlotUsable says no, the empty ones only while a pet action is on the cursor); Cross =
//     CastPetAction, Circle = TogglePetAutocast (the right click), Square = PickupPetAction / the drop; the tooltip:
//     a command's name (PET_ACTION_*, PET_MODE_*) or GameTooltip:SetPetAction (the spell's tooltip);
//   - PetStableFrame (PetStable.xml / .lua): the frame, the player's portrait, STABLES, the selected pet's line
//     (STABLE_PET_INFO_TEXT), the current pet slot and the stable slots (UI-EmptySlot, red past GetNumStableSlots),
//     CheckButtonHilight on the selected one, their tooltips (STABLE_PET_INFO_TOOLTIP_TEXT / EMPTY_STABLE_SLOT), the diet
//     icon (PET_DIET_TEMPLATE), STABLE_SLOT_TEXT, COSTS_LABEL + the cost, PURCHASE
//     (StaticPopup CONFIRM_BUY_STABLE_SLOT), the player's money, the close button; Cross = ClickStablePet, Square =
//     PickupStablePet and the drop (stable / unstable / swap);
//   - the StaticPopups RENAME_PET (the edit box = the PSP keyboard: it opens with the dialog, as editBox:SetFocus;
//     its Enter goes on to PETRENAMECONFIRM), PETRENAMECONFIRM, ABANDON_PET, CONFIRM_BUY_STABLE_SLOT,
//     TALENTS_INVOLUNTARILY_RESET_PET;
//   - PlayerTalentFrame's PlayerSpecTab1 / PlayerSpecTab3 ("petspec1": the pet's tree, LearnTalent(.., true)) and
//     CharacterFrameTab2 = PetPaperDollFrame (the name, UNIT_TYPE_LEVEL_TEMPLATE, the XP bar, the stats, the
//     resistances, the happiness / diet icon).
// PSP keys (a proposal): controls=2 - L + D-pad left / up / right / down = PetActionButton1..4 in both
// modes (the D-pad with L is free there); the rest with the cursor. UIParent_ManageFramePositions' "pet = 35": the chat
// box goes up with the bar (CastingBarFrame stays: it is above the bar on the PSP already).
#include "ui/psp/ui_world_screen.hpp"

#include "game/update_field_table.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_osk.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"
#include "platform/psp/psp_texture.hpp"
#include "ui/psp/ui_world_sounds.hpp"

#include <pspctrl.h>
#include <pspge.h>
#include <pspkernel.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
namespace pa = wowee::platform::psp::petact;

namespace {

// Widget ids: 0x8800..0x89FF.
enum : uint32_t {
    kIdPetFrame = 0x8800,
    kIdPetHappiness = 0x8801,
    kIdPetBar0 = 0x8810,        // .. 0x8819 PetActionButton1..10
    kIdStableClose = 0x8820,
    kIdStablePurchase = 0x8821,
    kIdStableSlot0 = 0x8822,    // the current pet; + 1..4 PetStableStabledPet1..4
    kIdStableInfo = 0x8827,     // PetStablePetInfo (the diet tooltip)
    kIdRenameAccept = 0x8830,
    kIdRenameCancel = 0x8831,
    kIdRenameEdit = 0x8832,
    kIdPopupYes = 0x8833,
    kIdPopupNo = 0x8834,
    kIdSpecTab1 = 0x8840,       // PlayerSpecTab1 ("spec1")
    kIdSpecTabPet = 0x8841,     // PlayerSpecTab3 ("petspec1")
    kIdPdClose = 0x8850,        // PetPaperDollCloseButton
    kIdPdInfo = 0x8851,         // PetPaperDollPetInfo
    kIdPdStat0 = 0x8852,        // PetStatFrame1..5, then attack power, damage, spell damage, armor (.. 0x885A)
    kIdPdRes0 = 0x8860,         // PetMagicResFrame1..5
    kIdPdXp = 0x8866,           // PetPaperDollFrameExpBar
};
constexpr int kOskOwner = 0x5045;  // "PE": the rename box's keyboard (psp_osk oskBeginFor)

// The unit frames' scale (ui_world_target.cpp kFrameScale) and PetFrame's place: PlayerFrame (2, 2) + 80,-60 units.
constexpr float kS = 0.6f;
constexpr float kPetX = 2.0f + 80.0f * kS, kPetY = 2.0f + 60.0f * kS;
// The framed windows' units (as ui_world_paperdoll.cpp / _trainer.cpp): 0.6 x 0.55 from (4, 2).
constexpr float kSX = 0.6f, kSY = 0.55f, kOx = 4.0f, kOy = 2.0f;
float PX(float x) { return kOx + x * kSX; }
float PY(float y) { return kOy + y * kSY; }
Rect FR(float x, float y, float w, float h) { return Rect{PX(x), PY(y), w * kSX, h * kSY}; }
// MainMenuBar's 0.5 (ui_world_mainmenubar.cpp): ActionButton1 at x 110 = the bar's left + 8 units.
constexpr float kBarS = 0.5f;
constexpr float kMmbLeft = 110.0f - 8.0f * kBarS, kMmbBottom = 272.0f;

constexpr uint32_t kColDisable = 0xFF808080u;
constexpr uint32_t kColGreenFont = 0xFF1AFF1Au;  // GREEN_FONT_COLOR (0.1, 1, 0.1)
constexpr uint32_t kColRedFont = 0xFF1A1AFFu;    // RED_FONT_COLOR (1, 0.1, 0.1)
constexpr uint32_t kTintUnusable = 0xFF666666u;  // SetDesaturation - PSP: the icon darkened (no desaturation in the GE)

// The pet frames' GlobalStrings (strings.pak): PET_HAPPINESS1..3, PET_ACTION_* / PET_MODE_* (GetPetActionInfo's tokens:
// PET_ACTION_%s over WAIT, FOLLOW, ATTACK, DISMISS; PET_MODE_%s over PASSIVE, DEFENSIVE, AGGRESSIVE).
const char* happinessText(int level) {
    static constexpr pp::CS k[3] = {pp::CS::PET_HAPPINESS1, pp::CS::PET_HAPPINESS2, pp::CS::PET_HAPPINESS3};
    return pp::cs(k[std::clamp(level, 1, 3) - 1]);
}
const char* commandName(uint32_t id) {
    static constexpr pp::CS k[4] = {pp::CS::PET_ACTION_WAIT, pp::CS::PET_ACTION_FOLLOW, pp::CS::PET_ACTION_ATTACK,
                                    pp::CS::PET_ACTION_DISMISS};
    return id < 4 ? pp::cs(k[id]) : "";
}
const char* reactName(uint32_t id) {
    static constexpr pp::CS k[3] = {pp::CS::PET_MODE_PASSIVE, pp::CS::PET_MODE_DEFENSIVE, pp::CS::PET_MODE_AGGRESSIVE};
    return id < 3 ? pp::cs(k[id]) : "";
}
// A client format of two lines ("...|n%N$s", STABLE_PET_INFO_TEXT / _TOOLTIP_TEXT) with its last part left empty: the
// first line (the PSP draws the talent tree on its own line).
std::string firstLine(const std::string& s) { return s.substr(0, s.find('\n')); }
// PET_*_TEXTURE (PetActionBarFrame.lua) as their SpellIcon.dbc ids (spells.pak keeps the icons by SpellIcon id):
// Spell_Nature_TimeStop 58, Ability_Tracking 179, Ability_GhoulFrenzy 108, Spell_Shadow_Teleport 87; Ability_Seal 29,
// Ability_Defend 28, Ability_Racial_BloodRage 86.
constexpr uint32_t kCommandIcon[4] = {58, 179, 108, 87};
constexpr uint32_t kReactIcon[3] = {29, 28, 86};

uint32_t powerColor(uint8_t type) {  // PowerBarColor (as ui_world_target.cpp)
    switch (type) {
        case 0: return 0xFFFF0000u;
        case 1: return 0xFF0000FFu;
        case 2: return 0xFF4080FFu;
        case 3: return 0xFF00FFFFu;
        case 4: return 0xFF00FFFFu;  // happiness (yellow; the pet frame shows its power type's bar)
        case 6: return 0xFFFFD100u;
        default: return 0xFFFF0000u;
    }
}

uint32_t debuffColor(uint8_t dispel) {  // DebuffTypeColor
    switch (dispel) {
        case 1: return 0xFFFF9933u;
        case 2: return 0xFFFF0099u;
        case 3: return 0xFF006699u;
        case 4: return 0xFF009900u;
        default: return 0xFF0000CCu;
    }
}

bool loadOne(pp::Pak& pak, const char* name, pp::Texture& t, uint32_t& bytes) {
    const pp::PakEntry* e = pak.find(name);
    void* blob = e ? pak.load(e) : nullptr;
    if (!blob) return false;
    if (!pp::textureFromBlob(blob, e->size, t, true)) {
        pp::pakFree(blob);
        return false;
    }
    t.name = "pet";
    bytes += e->size;
    return true;
}

}  // namespace

struct WorldScreen::PetUi {
    // PetFrame + PetActionBarFrame art (while there is a pet), the stable's (while it is open), the pet paper doll's.
    // UI-Quickslot2 / UI-Quickslot / ButtonHilight-Square / UI-QuickslotRed: the main bar's copies (spellArt_, always
    // loaded) - not a second set (~26 KB).
    pp::Texture frame, frameNoMana, happy, attack, bar, autocast, sparkle, checked;
    bool artTried = false;
    uint32_t artBytes = 0;
    pp::Texture stableFrame, emptySlot;
    bool stableArtTried = false;
    uint32_t stableArtBytes = 0;
    pp::Texture pdFrame;
    bool pdArtTried = false;
    uint32_t pdArtBytes = 0;
    pp::Texture talentBg;
    int32_t talentBgFor = -2;
    uint32_t talentBgBytes = 0;
    // The cursor: a pet action (PickupPetAction) and where it came from (PSP: put back when dropped nowhere).
    uint32_t cursor = 0;
    int cursorFrom = -1;
    int stableCursor = -2;  // PickupStablePet: -2 none, -1 the current pet, k the k-th stabled pet
    // Popups: one of RENAME_PET (kWinPetRename) and one of the confirmations (kWinPetPopup) at a time.
    enum PopupKind : uint8_t { kPopNone, kPopRenameConfirm, kPopAbandon, kPopBuySlot, kPopTalentsReset };
    uint8_t popup = kPopNone;
    std::string renameText, confirmName;
    bool oskOpen = false;
    // PlayerTalentFrame: the "petspec1" tab selected.
    bool talentPet = false;
    // Tooltips of this frame (drawPetOverlay).
    int tipButton = -1;         // a pet button under the cursor
    Rect tipRect{};
    std::vector<std::pair<std::string, uint32_t>> tipLines;  // a plain GameTooltip (first line large)
    Rect tipLinesAnchor{};
    uint32_t attackPhaseMs = 0;  // PetFrame_OnUpdate's counter
    uint32_t lastStableVersion = ~0u;
    bool stableShown = false;
    uint64_t portraitGuid = 0;
    uint32_t portraitDisplay = 0, portraitMs = 0;
    bool portraitOk = false;
    uint32_t logs = 0;
    uint32_t artLoads = 0;  // loadArt calls since the world came (log pet_art n=; one per pet / stable)

    void loadArt() {
        artTried = true;
        ++artLoads;
        char path[256];
        pp::fsPath(path, sizeof(path), "data/ui.pak");
        pp::Pak pak;
        int ok = 0;
        const uint64_t t0 = sceKernelGetSystemTimeWide();
        if (pak.open(path)) {
            const struct {
                const char* name;
                pp::Texture* tex;
            } list[] = {{"pet_frame", &frame}, {"pet_frame_nomana", &frameNoMana}, {"pet_happy", &happy},
                        {"pet_attack", &attack}, {"pet_bar", &bar}, {"pet_autocast", &autocast},
                        {"pet_sparkle", &sparkle}, {"sb_checked", &checked}};
            for (const auto& x : list)
                if (loadOne(pak, x.name, *x.tex, artBytes)) ++ok;
            pak.close();
        }
        PSP_LOGI(pp::tag::Gfx, "pet_art ok=%d/8 kb=%u ms=%u n=%u", ok, static_cast<unsigned>((artBytes + 1023u) / 1024u),
                 static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u), static_cast<unsigned>(artLoads));
    }
    void releaseArt() {
        for (pp::Texture* t : {&frame, &frameNoMana, &happy, &attack, &bar, &autocast, &sparkle, &checked})
            if (t->pixels) pp::textureRelease(*t);
        if (artTried) PSP_LOGI(pp::tag::Gfx, "pet_art_free kb=%u", static_cast<unsigned>((artBytes + 1023u) / 1024u));
        artTried = false;
        artBytes = 0;
    }
    void loadStableArt() {
        stableArtTried = true;
        char path[256];
        pp::fsPath(path, sizeof(path), "data/ui.pak");
        pp::Pak pak;
        int ok = 0;
        if (pak.open(path)) {
            ok += loadOne(pak, "pet_stable", stableFrame, stableArtBytes) ? 1 : 0;
            ok += loadOne(pak, "ct_slot", emptySlot, stableArtBytes) ? 1 : 0;  // UI-EmptySlot (the trainer's copy)
            pak.close();
        }
        PSP_LOGI(pp::tag::Gfx, "stable_art ok=%d/2 kb=%u", ok, static_cast<unsigned>((stableArtBytes + 1023u) / 1024u));
    }
    // The family icons not in SpellIcon.dbc (ui.pak pet_icon_<family>: Warp Stalker 32, Chimaera 38), on demand.
    pp::Texture famIcon[2];
    bool famTried[2] = {false, false};
    uint32_t famBytes = 0;
    const pp::Texture* familyIcon(uint32_t family) {
        const int k = family == 32 ? 0 : family == 38 ? 1 : -1;
        if (k < 0) return nullptr;
        if (!famTried[k]) {
            famTried[k] = true;
            char path[256];
            pp::fsPath(path, sizeof(path), "data/ui.pak");
            pp::Pak pak;
            if (pak.open(path)) {
                char name[24];
                std::snprintf(name, sizeof(name), "pet_icon_%u", static_cast<unsigned>(family));
                loadOne(pak, name, famIcon[k], famBytes);
                pak.close();
            }
        }
        return famIcon[k].pixels ? &famIcon[k] : nullptr;
    }
    void releaseStableArt() {
        for (int k = 0; k < 2; ++k) {
            if (famIcon[k].pixels) pp::textureRelease(famIcon[k]);
            famTried[k] = false;
        }
        famBytes = 0;
        for (pp::Texture* t : {&stableFrame, &emptySlot})
            if (t->pixels) pp::textureRelease(*t);
        if (stableArtTried)
            PSP_LOGI(pp::tag::Gfx, "stable_art_free kb=%u", static_cast<unsigned>((stableArtBytes + 1023u) / 1024u));
        stableArtTried = false;
        stableArtBytes = 0;
    }
    void loadPdArt() {
        pdArtTried = true;
        char path[256];
        pp::fsPath(path, sizeof(path), "data/ui.pak");
        pp::Pak pak;
        int ok = 0;
        if (pak.open(path)) {
            ok += loadOne(pak, "pet_pd_frame", pdFrame, pdArtBytes) ? 1 : 0;
            pak.close();
        }
        PSP_LOGI(pp::tag::Gfx, "petpd_art ok=%d/1 kb=%u", ok, static_cast<unsigned>((pdArtBytes + 1023u) / 1024u));
    }
    void releasePdArt() {
        for (pp::Texture* t : {&pdFrame})
            if (t->pixels) pp::textureRelease(*t);
        if (pdArtTried) PSP_LOGI(pp::tag::Gfx, "petpd_art_free kb=%u", static_cast<unsigned>((pdArtBytes + 1023u) / 1024u));
        pdArtTried = false;
        pdArtBytes = 0;
    }
    void releaseTalentBg() {
        if (talentBg.pixels) {
            pp::textureRelease(talentBg);
            PSP_LOGI(pp::tag::Gfx, "pet_talent_bg_free kb=%u", static_cast<unsigned>((talentBgBytes + 1023u) / 1024u));
        }
        talentBgFor = -2;
        talentBgBytes = 0;
    }
    ~PetUi() {
        releaseArt();
        releaseStableArt();
        releasePdArt();
        releaseTalentBg();
    }
};

void WorldScreen::destroyPetUi(PetUi* p) { delete p; }

WorldScreen::PetUi& WorldScreen::petUi() {
    if (!pet_) pet_ = std::unique_ptr<PetUi, void (*)(PetUi*)>(new PetUi, &WorldScreen::destroyPetUi);
    return *pet_;
}

// --- Per frame (drawWindows) ---------------------------------------------------------------------------------------

void WorldScreen::updatePets() {
    PetUi& p = petUi();
    session_.petsUpdate(0.0f, glm::vec3(playerX_, playerY_, playerZ_));  // the name query, the stable's reach
    const bool havePet = session_.petGuid() != 0;
    // The frame / bar art while there is a pet (or a pet action on the cursor); freed with it.
    // and while the stable shows (its slots use it - drawPetWindow loads it) - without a pet it was freed
    // here and loaded there every frame.
    const bool needArt = havePet || p.cursor != 0 || windowOpen(kWinPetStable);
    if (needArt && !p.artTried) p.loadArt();
    else if (!needArt && p.artTried) p.releaseArt();
    // The session's events (PET_RENAMEABLE, PET_STABLE_SHOW / _CLOSED, TALENTS_INVOLUNTARILY_RESET...).
    pp::WorldSession::PetEvent ev;
    while (session_.takePetEvent(ev)) {
        using E = pp::WorldSession::PetEvent;
        if (ev.kind == E::Renameable && havePet) {  // StaticPopup_Show("RENAME_PET")
            p.renameText.clear();
            openWindow(kWinPetRename);
            winFocus_.setFocus(kIdRenameEdit);
            if (!pp::oskActive() && pp::oskBeginFor(kOskOwner, CLIENT_STR(PET_RENAME_LABEL), "", 12, pp::OskCharset::Any)) p.oskOpen = true;
        } else if (ev.kind == E::StableShow) {  // PetStable_OnEvent: ShowUIPanel(self)
            if (!windowOpen(kWinPetStable)) {
                openWindow(kWinPetStable);
                playUiSound(UiSound::CharacterSheetOpen);  // igCharacterInfoOpen
            }
            p.stableShown = true;
            p.stableCursor = -2;
            // PetStable_Update: no selection - the active pet when there is one (GetPetIcon), else the first stabled.
            if (session_.stableSelected() == -2) {
                bool hunterPet = false;
                if (session_.hasPetUI(&hunterPet) && hunterPet) session_.stableSelect(-1);
                else if (session_.stableCurrent().petNumber != 0) session_.stableSelect(-1);
                else if (!session_.stabledPets().empty()) session_.stableSelect(0);
            }
        } else if (ev.kind == E::StableClosed) {  // HideUIPanel(self); StaticPopup_Hide("CONFIRM_BUY_STABLE_SLOT")
            if (windowOpen(kWinPetStable)) closeWindow(kWinPetStable);
            if (p.popup == PetUi::kPopBuySlot) {
                closeWindow(kWinPetPopup);
                p.popup = PetUi::kPopNone;
            }
        } else if (ev.kind == E::TalentsReset) {
            p.popup = PetUi::kPopTalentsReset;
            openWindow(kWinPetPopup);
            winFocus_.setFocus(kIdPopupYes);
        } else if (ev.kind == E::UiClose) {
            // PET_UI_CLOSE: the pet's paper doll / stable details of the old pet (PSP: the talent view goes back).
            p.talentPet = false;
        }
    }
    // The stable window closed by START / its close button / Circle on its title: OnHide -> ClosePetStables.
    if (p.stableShown && !windowOpen(kWinPetStable)) {
        p.stableShown = false;
        p.stableCursor = -2;
        session_.stableClose();
        playUiSound(UiSound::CharacterSheetClose);  // igCharacterInfoClose
    }
    if (windowOpen(kWinPetStable) && !p.stableArtTried) p.loadStableArt();
    else if (!windowOpen(kWinPetStable) && p.stableArtTried) p.releaseStableArt();
    // RENAME_PET's keyboard (the edit box): accepted = EditBoxOnEnterPressed (PETRENAMECONFIRM, the box hides).
    bool accepted = false;
    std::string text;
    if (pp::oskFinishedFor(kOskOwner, accepted, text)) {
        p.oskOpen = false;
        if (accepted && !text.empty()) {
            p.renameText = text;
            p.confirmName = text;
            if (windowOpen(kWinPetRename)) closeWindow(kWinPetRename);
            p.popup = PetUi::kPopRenameConfirm;
            openWindow(kWinPetPopup);
            winFocus_.setFocus(kIdPopupYes);
        } else if (!accepted && windowOpen(kWinPetRename)) {
            closeWindow(kWinPetRename);  // EditBoxOnEscapePressed hides the dialog
        }
        PSP_LOGI(pp::tag::Ctrl, "pet_rename_osk ok=%d len=%u", accepted ? 1 : 0, static_cast<unsigned>(text.size()));
    }
    // OnUpdate of RENAME_PET / PETRENAMECONFIRM / ABANDON_PET: hidden when the pet is gone.
    if (!havePet) {
        if (windowOpen(kWinPetRename)) closeWindow(kWinPetRename);
        if (p.popup == PetUi::kPopRenameConfirm || p.popup == PetUi::kPopAbandon) {
            closeWindow(kWinPetPopup);
            p.popup = PetUi::kPopNone;
        }
        if (p.cursor) {
            p.cursor = 0;
            p.cursorFrom = -1;
        }
    }
    if (p.popup != PetUi::kPopNone && !windowOpen(kWinPetPopup)) p.popup = PetUi::kPopNone;  // START (hideOnEscape)
    if (!uiMode_ && p.cursor) {  // PSP: the interface left with an action on the cursor - it goes back
        if (p.cursorFrom >= 0) session_.petPlace(p.cursorFrom, p.cursor);
        p.cursor = 0;
        p.cursorFrom = -1;
    }
    if (!uiMode_) p.stableCursor = -2;
    // The pet talents' tree background while the talent window shows the pet's tree.
    if (!windowOpen(kWinTalents) || !p.talentPet) {
        if (p.talentBgFor != -2) p.releaseTalentBg();
    }
    if (!windowOpen(kWinTalents)) p.talentPet = false;
    // CharacterFrame's pet tab: gone with the pet (CharacterFrame_UpdateTabs), its art only while it is shown.
    if (charTab_ == kCharTabPet && !petTabShown()) selectCharTab(kCharTabPaperDoll);
    if (p.pdArtTried && (!windowOpen(kWinCharacter) || charTab_ != kCharTabPet)) p.releasePdArt();
}

bool WorldScreen::isPetPopup(uint8_t w) const { return w == kWinPetRename || w == kWinPetPopup; }

bool WorldScreen::petKeepsPopupArt() const { return windowOpen(kWinPetRename) || windowOpen(kWinPetPopup); }

float WorldScreen::petPartyShift() const {
    // PSP: PetFrame takes y 38..72 under PlayerFrame; PartyMemberFrame1 (y 56) goes under it while it is shown.
    if (!unitFramesOn_ || !session_.petUnit()) return 0.0f;
    return 18.0f;
}

bool WorldScreen::petBarShown() const {
    return session_.petHasActionBar() && !fxHides(kFxHideBar) && !fullMapOpen();
}

float WorldScreen::petChatLift() const { return petBarShown() ? std::round(35.0f * kBarS) : 0.0f; }

// --- PetFrame ------------------------------------------------------------------------------------------------------

void WorldScreen::drawPetFrame(UiRenderer& ui) {
    auto pet = session_.petUnit();
    if (!pet || settings_.isOpen()) return;
    PROF_SCOPE("hud.pet");
    PetUi& p = petUi();
    if (!p.artTried) p.loadArt();
    const Font& small = kit_.font(kFontSmall);
    const Font& tiny = kit_.font(kFontTiny);
    const bool focusable = uiMode_ && !settings_.isOpen();
    const float x = kPetX, y = kPetY;
    auto R = [&](float lx, float ly, float lw, float lh) { return Rect{x + lx * kS, y + ly * kS, lw * kS, lh * kS}; };
    UnitView u;
    unitView(session_.petGuid(), u);
    // PetPortrait (37 x 37 at 7,-6): the pet's model still (renderPetPortrait), else a dark disc.
    const Rect port = R(7, 6, 37, 37);
    const uint32_t tint = u.dead ? 0xFF595959u : kColWhite;
    if (p.portraitOk && petPortraitTex_.pixels) {
        const float in = port.w * 0.08f;
        ui.imageCircle(petPortraitTex_, Rect{port.x + in, port.y + in, port.w - 2 * in, port.h - 2 * in}, tint);
    } else {
        const float in = port.w * 0.1f;
        ui.fillCircle(Rect{port.x + in, port.y + in, port.w - 2 * in, port.h - 2 * in}, 0xC0101010u);
    }
    auto bar = [&](const Rect& r, uint32_t cur, uint32_t max, uint32_t color) {
        if (max == 0) return;
        const float f = std::clamp(static_cast<float>(cur) / static_cast<float>(max), 0.0f, 1.0f);
        if (f <= 0.0f) return;
        const Rect filled{r.x, r.y, r.w * f, r.h};
        if (ufArt_.bar.pixels)
            ui.image(ufArt_.bar, filled, SrcRect{0, 0, ufArt_.bar.uw * f, static_cast<float>(ufArt_.bar.uh)}, color);
        else ui.fill(filled, color);
    };
    const Rect hpBar = R(47, 22, 69, 8), mpBar = R(47, 29, 69, 8);
    ui.fill(R(45, 21, 73, 17), 0x80000000u);
    bar(hpBar, u.dead ? 0 : u.hp, u.maxHp, 0xFF00FF00u);  // UnitFrameHealthBar_OnValueChanged: green
    bar(mpBar, u.power, u.maxPower, powerColor(u.powerType));
    // PetFrameTexture (0,-2): UI-SmallTargetingFrame, -NoMana when UnitPowerMax("pet") == 0.
    const pp::Texture& art = u.maxPower == 0 && p.frameNoMana.pixels ? p.frameNoMana : p.frame;
    if (art.pixels) ui.image(art, R(0, 2, 128, 64));
    // PetName (GameFontNormalSmall, BOTTOMLEFT 52,33: the text's bottom 20 units under the frame's top).
    const std::string& name = !session_.petName().empty() ? session_.petName() : u.name;
    ui.textMarquee(small, Rect{x + 52.0f * kS, std::floor(y + 20.0f * kS - static_cast<float>(small.lineHeight())),
                               66.0f * kS, static_cast<float>(small.lineHeight())},
                   name.c_str(), kColGold);
    // PetAttackModeTexture: while the pet attacks (PET_ATTACK_START), ADD, alpha 55..255 and back over 0.5 s.
    if (session_.petAttacking() && p.attack.pixels) {
        const float t = static_cast<float>(pp::logUptimeMs() % 1000u) / 1000.0f;
        const float a = t < 0.5f ? (255.0f - t * 400.0f) / 255.0f : (55.0f + (t - 0.5f) * 400.0f) / 255.0f;
        ui.image(p.attack, R(6, 9, 76, 64), 0x00FFFFFFu | (static_cast<uint32_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f) << 24),
                 Blend::Add);
    }
    // Debuff1..4 (PartyDebuffFrameTemplate, 15 units at 48,-42, 2 apart): RefreshDebuffs.
    if (const std::vector<game::AuraSlot>* auras = session_.unitAuras(session_.petGuid())) {
        int k = 0;
        for (const auto& au : *auras) {
            if (au.isEmpty() || !(au.flags & 0x80) || k >= 4) continue;
            const Rect r = R(48.0f + 17.0f * static_cast<float>(k), 42, 15, 15);
            if (const pp::Texture* ic = spellDb_.icon(au.spellId, spellFrame_)) ui.image(*ic, r);
            else ui.fill(r, 0xFF404040u);
            if (spellArt_.debuff.pixels) {
                const pp::SpellInfo* info = spellDb_.get(au.spellId);
                ui.image(spellArt_.debuff, R(47.0f + 17.0f * static_cast<float>(k), 41, 17, 17),
                         debuffColor(info ? info->rec.dispel : 0));
            }
            ++k;
        }
    }
    // PetFrameHappiness (24 x 23, LEFT at the frame's RIGHT -7,-4): a hunter's pet only.
    int level = 0, dmg = 0;
    const bool happy = session_.petHappiness(level, dmg) && level >= 1 && level <= 3;
    const Rect hr = R(128.0f - 7.0f, 26.5f + 4.0f - 11.5f, 24, 23);
    if (happy && p.happy.pixels) {
        // texcoords: 3 = (0, .1875), 2 = (.1875, .375), 1 = (.375, .5625); 0 .. .359375 (the baked atlas: 72 x 23).
        const float cell = static_cast<float>(p.happy.uw) / 3.0f;
        ui.image(p.happy, hr, SrcRect{cell * static_cast<float>(3 - level), 0, cell, static_cast<float>(p.happy.uh)});
    }
    const bool fHappy = happy && focusable && winFocus_.addStop(kIdPetHappiness, hr);
    // The button (HitRectInsets 7 / 66 / 6 / 7 = the portrait and the name; the bars click it too - as the party's).
    const bool f = focusable && winFocus_.addStop(kIdPetFrame, R(7, 6, 110, 40));
    if (f) {
        const float capH = std::round(static_cast<float>(tiny.pixelSize()) * 0.72f);
        auto numY = [&](const Rect& b) {
            return std::floor(b.y + (b.h - capH) * 0.5f + capH - static_cast<float>(tiny.ascent()) + 0.5f);
        };
        char line[48];
        if (u.dead) std::snprintf(line, sizeof(line), "%s", CLIENT_STR(DEAD));
        else std::snprintf(line, sizeof(line), "%u / %u", static_cast<unsigned>(u.hp), static_cast<unsigned>(u.maxHp));
        ui.textShadow(tiny, hpBar.x + hpBar.w * 0.5f, numY(hpBar), line, kColWhite, Align::Center);
        if (u.maxPower > 0 && !u.dead) {
            std::snprintf(line, sizeof(line), "%u / %u", static_cast<unsigned>(u.power), static_cast<unsigned>(u.maxPower));
            ui.textShadow(tiny, mpBar.x + mpBar.w * 0.5f, numY(mpBar), line, kColWhite, Align::Center);
        }
        // UnitFrame_OnEnter: GameTooltip:SetUnit - the name and TOOLTIP_UNIT_LEVEL (PSP: beside the frame).
        p.tipLines.clear();
        p.tipLines.push_back({name, kColWhite});
        if (u.level > 0) p.tipLines.push_back({CLIENT_FMT(TOOLTIP_UNIT_LEVEL, std::to_string(u.level)), kColWhite});
        p.tipLinesAnchor = Rect{x + 122.0f * kS, y + 44.0f, 1.0f, 1.0f};
    } else if (fHappy) {
        // PetFrameHappiness OnEnter: PET_HAPPINESSn, PET_DAMAGE_PERCENTAGE (white).
        const std::string buf = CLIENT_FMT(PET_DAMAGE_PERCENTAGE, dmg);
        p.tipLines.clear();
        p.tipLines.push_back({happinessText(level), kColWhite});
        p.tipLines.push_back({buf, kColWhite});
        p.tipLinesAnchor = Rect{hr.x + hr.w, hr.y, 1.0f, 1.0f};
    }
}

// The pet's model still for PetPortrait: drawn into its VRAM target when the pet / its look changes and every 2 s
// while the frame is up (CreatureRenderer::drawPortrait needs the unit drawn this frame - it usually is, by us).
void WorldScreen::renderPetPortrait(const rendering::psp::TerrainCamera& cam) {
    if (!petPortraitVram_ || !creaturesOn_) return;
    PetUi& p = petUi();
    auto pet = session_.petUnit();
    if (!pet) {
        p.portraitOk = false;
        p.portraitGuid = 0;
        return;
    }
    const uint32_t display = pet->getField(game::fieldIndex(game::UF::UNIT_FIELD_DISPLAYID));
    const uint32_t now = pp::logUptimeMs();
    const bool due = !p.portraitOk || pet->getGuid() != p.portraitGuid || display != p.portraitDisplay ||
                     now - p.portraitMs > 2000u;
    if (!due) return;
    if (creatures_.drawPortrait(pet->getGuid(), petPortraitVram_, cam)) {
        p.portraitOk = true;
        p.portraitGuid = pet->getGuid();
        p.portraitDisplay = display;
        p.portraitMs = now;
        if (++p.logs <= 20) PSP_LOGI(pp::tag::Gfx, "pet_portrait display=%u", static_cast<unsigned>(display));
    } else if (pet->getGuid() != p.portraitGuid) {
        p.portraitOk = false;
    }
}

// --- PetActionBarFrame ---------------------------------------------------------------------------------------------

namespace {

// PetActionButton n (1-based) at the bar's BOTTOMLEFT 36,2, each LEFT of the last RIGHT +8 (+7 for the 7th), 30 x 30.
Rect petButtonRect(int i, float shift = 0.0f) {  // + shift (PETACTIONBAR_XPOS)
    const float barTop = kMmbBottom - 97.0f * kBarS;
    const float left = kMmbLeft + 36.0f * kBarS + shift;
    float x = left + 36.0f * kBarS;
    for (int k = 0; k < i; ++k) x += (30.0f + (k == 5 ? 7.0f : 8.0f)) * kBarS;
    const float bottom = barTop + 43.0f * kBarS - 2.0f * kBarS;
    return Rect{std::floor(x), std::floor(bottom - 30.0f * kBarS), 30.0f * kBarS, 30.0f * kBarS};
}

// AutoCastShine_OnUpdate's four timers (speeds 2, 4, 6, 8 s; each sparkle runs the edge in `speed`, the whole
// square in 4 x speed) and the 16 sparkles (sizes 13 / 10 / 7 / 4 - $parent1..16 by four).
void shinePositions(float t, float side, float (&px)[16], float (&py)[16]) {
    static const float kSpeed[4] = {2.0f, 4.0f, 6.0f, 8.0f};
    for (int i = 0; i < 4; ++i) {
        const float sp = kSpeed[i];
        const float timer = std::fmod(t, sp * 4.0f);
        const int phase = static_cast<int>(timer / sp);
        const float base = (timer - sp * static_cast<float>(phase)) / sp * side;
        // corners: TL (0,0), TR (side,0), BR (side,side), BL (0,side); y down.
        auto at = [&](int corner, float& ox, float& oy) {
            switch (corner) {
                case 0: ox = base; oy = 0; break;                  // TOPLEFT + base, 0
                case 1: ox = side; oy = base; break;               // TOPRIGHT 0, -base
                case 2: ox = side - base; oy = side; break;        // BOTTOMRIGHT -base, 0
                default: ox = 0; oy = side - base; break;          // BOTTOMLEFT 0, base
            }
        };
        // sparkles[0+i] starts at TL, [4+i] at BR, [8+i] at TR, [12+i] at BL; each phase moves them a corner on.
        const int start[4] = {0, 2, 1, 3};
        for (int g = 0; g < 4; ++g) at((start[g] + phase) % 4, px[g * 4 + i], py[g * 4 + i]);
    }
}

}  // namespace

void WorldScreen::drawPetActionBar(UiRenderer& ui) {
    if (pet_) pet_->tipButton = -1;
    if (!petBarShown()) return;
    PROF_SCOPE("hud.petbar");
    PetUi& p = petUi();
    if (!p.artTried) p.loadArt();
    const bool clickable = uiMode_ && !settings_.isOpen();
    // SlidingActionBarTexture0 / 1 (UI-PetBar: 256 x 44 + 184 x 44 of the two rows, baked one above the other).
    // PetActionBar_UpdatePositionValues - right of the shapeshift / multicast bar, the leave button.
    const float petShift = classBarPetShift();
    const float barTop = kMmbBottom - 97.0f * kBarS, barLeft = kMmbLeft + 36.0f * kBarS + petShift;
    if (p.bar.pixels) {
        const float rowH = static_cast<float>(p.bar.uh) * 0.5f;
        ui.image(p.bar, Rect{barLeft, barTop, 256.0f * kBarS, 44.0f * kBarS},
                 SrcRect{0, 0, static_cast<float>(p.bar.uw), rowH});
        ui.image(p.bar, Rect{barLeft + 256.0f * kBarS, barTop, 184.0f * kBarS, 44.0f * kBarS},
                 SrcRect{0, rowH, static_cast<float>(p.bar.uw) * 0.71875f, rowH});
    }
    const uint32_t now = pp::logUptimeMs();
    const bool showGrid = p.cursor != 0;  // PET_BAR_SHOWGRID while a pet action is on the cursor
    p.tipButton = -1;
    for (int i = 0; i < pp::WorldSession::kPetSlots; ++i) {
        const uint32_t s = session_.petActionSlot(i);
        const Rect r = petButtonRect(i, petShift);
        const float fs = r.w * 54.0f / 30.0f;  // NormalTexture UI-Quickslot2 54 around the 30 icon (0,-1)
        const Rect frame{r.x + (r.w - fs) * 0.5f, r.y + (r.h - fs) * 0.5f + 0.5f, fs, fs};
        const uint8_t type = pa::type(s);
        const uint32_t id = pa::id(s);
        const bool spell = pa::isSpell(s);
        const bool hasAction = s != 0 && (!spell || id != 0);
        if (!hasAction && !showGrid) continue;  // the button hides (showgrid == 0)
        const bool focused = clickable && winFocus_.addStop(kIdPetBar0 + static_cast<uint32_t>(i), r);
        if (focused && hasAction) {
            p.tipButton = i;
            p.tipRect = Rect{r.x, r.y, r.w, r.h};
        }
        if (!hasAction) {
            if (spellArt_.empty.pixels) ui.image(spellArt_.empty, frame, 0xB0FFFFFFu);  // UI-Quickslot
            else ui.fill(r, 0x60000000u);
            if (focused && spellArt_.hilite.pixels) ui.image(spellArt_.hilite, r, 0xFFFFFFFFu, Blend::Add);
            continue;
        }
        // The icon: a spell's, or PET_*_TEXTURE of a command / reaction; grey when not usable.
        const pp::Texture* icon = nullptr;
        if (spell) icon = spellDb_.icon(id, spellFrame_);
        else if (type == pa::kTypeCommand && id < 4) icon = spellDb_.iconById(kCommandIcon[id], spellFrame_);
        else if (type == pa::kTypeReaction && id < 3) icon = spellDb_.iconById(kReactIcon[id], spellFrame_);
        const uint32_t iconTint = session_.petSlotUsable(i) ? 0xFFFFFFFFu : kTintUnusable;
        if (icon) ui.image(*icon, r, iconTint);
        else ui.fill(r, 0xFF303030u);
        if (spell) {
            float total = 0.0f;
            const float rem = session_.petSpellCooldown(id, &total);
            drawCooldown(ui, r, rem, total);
        }
        if (spellArt_.slot.pixels) ui.image(spellArt_.slot, frame);
        // isActive: the stance in effect, the command in effect, the attack while the pet attacks (CheckedTexture
        // CheckButtonHilight; the attack's at alpha 0.5 and its red Flash every ATTACK_BUTTON_FLASH_TIME 0.4 s).
        bool active = false;
        if (type == pa::kTypeReaction) active = id == session_.petReact();
        else if (type == pa::kTypeCommand && id == pa::kAttack) active = session_.petAttacking();
        else if (type == pa::kTypeCommand) active = (session_.petMode() >> 8) == id;
        const bool attack = type == pa::kTypeCommand && id == pa::kAttack;
        if (active && p.checked.pixels) ui.image(p.checked, r, attack ? 0x80FFFFFFu : 0xFFFFFFFFu, Blend::Add);
        if (active && attack && spellArt_.red.pixels && ((now / 400u) & 1u) == 0) ui.image(spellArt_.red, frame, 0xFFFFFFFFu, Blend::Add);
        // $parentAutoCastable (58 x 58 UI-AutoCastableOverlay) and the shine (28 x 28, sparkles 0.95, 0.95, 0.32 ADD).
        if (spell && (s & pa::kAutocastable) && p.autocast.pixels) {
            const float os = 58.0f * kBarS;
            ui.image(p.autocast, Rect{r.x + (r.w - os) * 0.5f, r.y + (r.h - os) * 0.5f, os, os});
        }
        if (spell && (s & pa::kAutocastOn) && p.sparkle.pixels) {
            const float side = 28.0f * kBarS;
            const float ox = r.x + (r.w - side) * 0.5f, oy = r.y + (r.h - side) * 0.5f;
            float sx[16], sy[16];
            shinePositions(static_cast<float>(now % 960000u) / 1000.0f, side, sx, sy);
            static const float kSize[4] = {13.0f, 10.0f, 7.0f, 4.0f};
            for (int k = 0; k < 16; ++k) {
                const float sz = kSize[k % 4] * kBarS;
                ui.image(p.sparkle, Rect{ox + sx[k] - sz * 0.5f, oy + sy[k] - sz * 0.5f, sz, sz}, 0xFF52F2F2u, Blend::Add);
            }
        }
        if (focused && spellArt_.hilite.pixels) ui.image(spellArt_.hilite, r, 0xFFFFFFFFu, Blend::Add);  // ButtonHilight-Square
        // PSP: the keys of buttons 1..4 (controls=2: L + D-pad) as the main bar's glyphs, outside the interface.
        if (i < 4 && controlsScheme_ >= 2 && !clickable) {
            constexpr PadGlyph kDir[4] = {kPadLeft, kPadUp, kPadRight, kPadDown};
            const Rect g{r.x + r.w - 7.0f, r.y - 4.0f, 9.0f, 9.0f};
            kit_.glyph(ui, kDir[i], g);
            kit_.glyph(ui, kPadL, Rect{g.x - 7.0f, g.y + 1.0f, 8.0f, 8.0f});
        }
    }
}

// controls=2: L + D-pad (left, up, right, down) = PetActionButton1..4 (PetActionButtonDown / Up -> CastPetAction).
void WorldScreen::petKeys(uint32_t live, bool lHeld, bool rHeld) {
    if (!lHeld || rHeld || !petBarShown()) return;
    constexpr uint32_t kDir[4] = {PSP_CTRL_LEFT, PSP_CTRL_UP, PSP_CTRL_RIGHT, PSP_CTRL_DOWN};
    for (int i = 0; i < 4; ++i) {
        if (!(live & kDir[i])) continue;
        session_.castPetAction(i);
        PSP_LOGI(pp::tag::Ctrl, "pet_key slot=%d data=%08X", i, static_cast<unsigned>(session_.petActionSlot(i)));
    }
}

// --- Clicks --------------------------------------------------------------------------------------------------------

namespace {
bool petBarId(uint32_t id, int& slot) {
    if (id < kIdPetBar0 || id >= kIdPetBar0 + 10) return false;
    slot = static_cast<int>(id - kIdPetBar0);
    return true;
}
bool stableSlotId(uint32_t id, int& index) {  // -1 the current pet, 0..3 the stable slots
    if (id < kIdStableSlot0 || id > kIdStableSlot0 + 4) return false;
    index = static_cast<int>(id - kIdStableSlot0) - 1;
    return true;
}
}  // namespace

bool WorldScreen::petWindowAction(uint32_t act) {
    if (act < 0x8800 || act > 0x89FF) return false;
    PetUi& p = petUi();
    // A drop on a pet button / a pick-up (PetActionButton_OnReceiveDrag / OnDragStart -> PickupPetAction).
    auto dropOrPick = [&](int slot) {
        if (p.cursor) {
            const uint32_t back = session_.petPlace(slot, p.cursor);
            if (back == p.cursor) return;  // refused: it stays on the cursor
            p.cursor = back;
            p.cursorFrom = -1;
        } else {
            p.cursor = session_.petPickup(slot);
            p.cursorFrom = p.cursor ? slot : -1;
        }
        PSP_LOGI(pp::tag::Ctrl, "pet_cursor slot=%d data=%08X", slot, static_cast<unsigned>(p.cursor));
    };
    int slot = 0;
    if (petBarId(act, slot)) {  // PetActionButton_OnClick (LeftButton): CastPetAction / the drop of the cursor
        if (p.cursor) dropOrPick(slot);
        else session_.castPetAction(slot);
        return true;
    }
    if (stableSlotId(act, slot)) {  // PetStableSlotTemplate OnClick / OnReceiveDrag: ClickStablePet
        session_.stableClick(slot, &p.stableCursor);
        return true;
    }
    switch (act) {
        case kIdPetFrame: session_.setTarget(session_.petGuid()); return true;  // SecureUnitButton: TargetUnit("pet")
        case kIdStableClose: closeWindow(kWinPetStable); return true;
        case kIdStablePurchase:  // StaticPopup_Show("CONFIRM_BUY_STABLE_SLOT")
            p.popup = PetUi::kPopBuySlot;
            openWindow(kWinPetPopup);
            winFocus_.setFocus(kIdPopupYes);
            return true;
        case kIdRenameEdit:  // the edit box: the keyboard
            if (!pp::oskActive() && pp::oskBeginFor(kOskOwner, CLIENT_STR(PET_RENAME_LABEL), p.renameText, 12, pp::OskCharset::Any))
                p.oskOpen = true;
            return true;
        case kIdRenameAccept:  // RENAME_PET OnAccept: PETRENAMECONFIRM with the text
            if (windowOpen(kWinPetRename)) closeWindow(kWinPetRename);
            if (!p.renameText.empty()) {
                p.confirmName = p.renameText;
                p.popup = PetUi::kPopRenameConfirm;
                openWindow(kWinPetPopup);
                winFocus_.setFocus(kIdPopupYes);
            }
            return true;
        case kIdRenameCancel: closeWindow(kWinPetRename); return true;
        case kIdPopupYes:
        case kIdPopupNo: {
            const uint8_t kind = p.popup;
            p.popup = PetUi::kPopNone;
            closeWindow(kWinPetPopup);
            if (act == kIdPopupYes) {
                if (kind == PetUi::kPopRenameConfirm) session_.petRename(p.confirmName);  // PetRename(data)
                else if (kind == PetUi::kPopAbandon) session_.petAbandon();             // PetAbandon()
                else if (kind == PetUi::kPopBuySlot) session_.stableBuySlot();          // BuyStableSlot()
            }
            return true;
        }
        case kIdSpecTab1:  // PlayerSpecTab_OnClick
        case kIdSpecTabPet: {
            const bool pet = act == kIdSpecTabPet;
            if (pet != p.talentPet) {
                p.talentPet = pet;
                trainer_.talTab = 0;
                trainer_.talScrollU = 0.0f;
                trainer_.talSel = 0;
                if (!playSoundByName("igCharacterInfoTab")) playUiSound(UiSound::ButtonClick);
                PSP_LOGI(pp::tag::Ctrl, "talent_spec pet=%d", pet ? 1 : 0);
            }
            return true;
        }
        case kIdPdClose: closeWindow(kWinCharacter); return true;  // PetPaperDollCloseButton: HideUIPanel
        case kIdCharTabPet: selectCharTab(kCharTabPet); return true;  // CharacterFrameTab_OnClick (tab 2)
        default: return true;  // the tooltip-only frames (happiness, diet, stats)
    }
}

// Circle = the right click: TogglePetAutocast on a pet button, the unit menu on PetFrame.
bool WorldScreen::petAltAction(uint32_t alt) {
    if (alt < 0x8800 || alt > 0x89FF) return false;
    int slot = 0;
    if (petBarId(alt, slot)) {
        session_.togglePetAutocast(slot);
        return true;
    }
    if (alt == kIdPetFrame) {  // ToggleDropDownMenu(1, nil, PetFrameDropDown, "PetFrame", 44, 8)
        openPetUnitMenu(kPetX + 44.0f * kS, kPetY + (53.0f - 8.0f) * kS, kIdPetFrame);
        return true;
    }
    return petWindowAction(alt);
}

// Square: the drag - a pet button (PickupPetAction / the drop) or a stable slot (PickupStablePet / the drop).
void WorldScreen::petWindowPad(pp::PadState& pad) {
    if (!(pad.pressed & PSP_CTRL_SQUARE) || !uiMode_) return;
    const uint32_t f = winFocus_.focused();
    PetUi& p = petUi();
    // A drop on a pet button / a pick-up (PetActionButton_OnReceiveDrag / OnDragStart -> PickupPetAction).
    auto dropOrPick = [&](int slot) {
        if (p.cursor) {
            const uint32_t back = session_.petPlace(slot, p.cursor);
            if (back == p.cursor) return;  // refused: it stays on the cursor
            p.cursor = back;
            p.cursorFrom = -1;
        } else {
            p.cursor = session_.petPickup(slot);
            p.cursorFrom = p.cursor ? slot : -1;
        }
        PSP_LOGI(pp::tag::Ctrl, "pet_cursor slot=%d data=%08X", slot, static_cast<unsigned>(p.cursor));
    };
    int slot = 0;
    if (petBarId(f, slot)) {
        dropOrPick(slot);
    } else if (stableSlotId(f, slot)) {
        if (p.stableCursor != -2) {
            session_.stableClick(slot, &p.stableCursor);
        } else {
            const bool has = slot == -1 ? session_.stableCurrent().petNumber != 0 || session_.petGuid() != 0
                                        : slot < static_cast<int>(session_.stabledPets().size());
            if (has) p.stableCursor = slot;  // PickupStablePet
        }
    } else {
        return;
    }
    pad.pressed &= ~PSP_CTRL_SQUARE;
}

bool WorldScreen::petClearCursor() {
    if (!pet_) return false;
    PetUi& p = *pet_;
    if (p.cursor) {
        // ClearCursor. PSP: no pet spell book to take it from again - the action goes back to its slot (or the first
        // empty spell slot).
        int to = p.cursorFrom;
        if (to < 0)
            for (int i = 0; i < pp::WorldSession::kPetSlots && to < 0; ++i) {
                const uint32_t s = session_.petActionSlot(i);
                if (pa::type(s) == pa::kTypeSpell && pa::id(s) == 0) to = i;
            }
        if (to >= 0) session_.petPlace(to, p.cursor);
        p.cursor = 0;
        p.cursorFrom = -1;
        return true;
    }
    if (p.stableCursor != -2) {
        p.stableCursor = -2;
        return true;
    }
    return false;
}

// --- The unit menu (UnitPopup PET) ---------------------------------------------------------------------------------

bool WorldScreen::petMenuShown(uint8_t which) const {
    switch (which) {
        case 0: return session_.petCanBeAbandoned();                                    // PET_PAPERDOLL
        case 1: return session_.petCanBeAbandoned() && session_.petCanBeRenamed();      // PET_RENAME
        case 2: return session_.petCanBeAbandoned();                                    // PET_ABANDON
        case 3: return !session_.petCanBeAbandoned() && session_.petCanBeDismissed();   // PET_DISMISS
        default: return false;
    }
}

void WorldScreen::petMenuAction(uint8_t which) {
    PetUi& p = petUi();
    switch (which) {
        case 0:  // PET_PAPERDOLL: ToggleCharacter("PetPaperDollFrame")
            if (!windowOpen(kWinCharacter)) openWindow(kWinCharacter);
            selectCharTab(kCharTabPet);
            break;
        case 1:  // PET_RENAME: StaticPopup_Show("RENAME_PET") - its edit box takes the focus (the keyboard)
            p.renameText.clear();
            openWindow(kWinPetRename);
            winFocus_.setFocus(kIdRenameEdit);
            if (!pp::oskActive() && pp::oskBeginFor(kOskOwner, CLIENT_STR(PET_RENAME_LABEL), "", 12, pp::OskCharset::Any)) p.oskOpen = true;
            break;
        case 2:  // PET_ABANDON: StaticPopup_Show("ABANDON_PET")
            p.popup = PetUi::kPopAbandon;
            openWindow(kWinPetPopup);
            winFocus_.setFocus(kIdPopupNo);
            break;
        case 3: session_.petDismiss(); break;  // PET_DISMISS: PetDismiss()
        default: break;
    }
}

// --- Windows -------------------------------------------------------------------------------------------------------

bool WorldScreen::drawPetWindow(UiRenderer& ui, uint8_t w) {
    if (w != kWinPetStable && w != kWinPetRename && w != kWinPetPopup) return false;
    PetUi& p = petUi();
    const bool top = !windows_.empty() && windows_.back() == w;
    const bool focusable = uiMode_ && !settings_.isOpen() && !windowOpen(kWinBindConfirm);
    const Font& fn = kit_.font(kFontNormal);
    const Font& fs = kit_.font(kFontSmall);
    if (w == kWinPetRename) {
        // StaticPopup RENAME_PET (hasEditBox, maxLetters 12): PET_RENAME_LABEL, the edit box, ACCEPT / CANCEL. The box
        // lies on two empty text lines under the label (StaticPopup_Resize: + editBox height + 8).
        const float lh = static_cast<float>(fn.lineHeight());
        StaticPopup sp;
        const std::string text = std::string(CLIENT_STR(PET_RENAME_LABEL)) + "\n \n ";
        sp.text = text.c_str();
        sp.button1 = CLIENT_STR(ACCEPT);
        sp.button1Id = kIdRenameAccept;
        sp.button1Enabled = !p.renameText.empty();
        sp.button2 = CLIENT_STR(CANCEL);
        sp.button2Id = kIdRenameCancel;
        sp.focusable = focusable && top;
        const Rect r = drawStaticPopup(ui, sp);
        const size_t labelLines = std::max<size_t>(1, wrapQuestText(fn, CLIENT_STR(PET_RENAME_LABEL), std::floor(290.0f * kDialogScale)).size());
        const float bw = std::floor(150.0f * kDialogScale);
        const Rect box{std::floor(r.x + (r.w - bw) * 0.5f), r.y + std::floor(16.0f * kDialogScale) + lh * static_cast<float>(labelLines) + 2.0f,
                       bw, lh * 2.0f - 4.0f};
        // PSP: the edit box as a dark field with a light edge (UI-ChatInputBorder is not baked); Cross opens the keyboard.
        ui.fill(box, 0xC0000000u);
        ui.fill(Rect{box.x, box.y, box.w, 1}, 0xFF808080u);
        ui.fill(Rect{box.x, box.y + box.h - 1, box.w, 1}, 0xFF808080u);
        ui.fill(Rect{box.x, box.y, 1, box.h}, 0xFF808080u);
        ui.fill(Rect{box.x + box.w - 1, box.y, 1, box.h}, 0xFF808080u);
        ui.textShadow(fn, box.x + 4, box.y + (box.h - lh) * 0.5f, p.renameText.c_str(), kColWhite);
        if (focusable && top && winFocus_.addStop(kIdRenameEdit, box)) ui.fill(box, 0x30FFFFFFu);
        return true;
    }
    if (w == kWinPetPopup) {
        StaticPopup sp;
        std::string text;
        sp.focusable = focusable && top;
        sp.button1Id = kIdPopupYes;
        sp.button2Id = kIdPopupNo;
        switch (p.popup) {
            case PetUi::kPopRenameConfirm:  // PETRENAMECONFIRM: YES / NO
                text = CLIENT_FMT(PET_RENAME_CONFIRMATION, p.confirmName);
                sp.button1 = CLIENT_STR(YES);
                sp.button2 = CLIENT_STR(NO);
                break;
            case PetUi::kPopAbandon:  // ABANDON_PET: OKAY / CANCEL
                text = CLIENT_STR(ABANDON_PET);
                sp.button1 = CLIENT_STR(OKAY);
                sp.button2 = CLIENT_STR(CANCEL);
                break;
            case PetUi::kPopBuySlot:  // CONFIRM_BUY_STABLE_SLOT: YES / NO, the money frame (GetNextStableSlotCost)
                text = CLIENT_STR(CONFIRM_BUY_STABLE_SLOT);
                sp.button1 = CLIENT_STR(YES);
                sp.button2 = CLIENT_STR(NO);
                sp.hasMoneyFrame = true;
                sp.money = session_.stableNextSlotCost();
                break;
            case PetUi::kPopTalentsReset:  // TALENTS_INVOLUNTARILY_RESET_PET: OKAY
                text = CLIENT_STR(TALENTS_INVOLUNTARILY_RESET_PET);
                sp.button1 = CLIENT_STR(OKAY);
                break;
            default: return true;
        }
        sp.text = text.c_str();
        drawStaticPopup(ui, sp);
        return true;
    }
    // PetStableFrame.
    PROF_SCOPE("stable");
    if (!p.stableArtTried) p.loadStableArt();
    loadNpcArt(false);  // the close button, the panel buttons, the coins
    if (!p.artTried) p.loadArt();  // the slot frames (UI-Quickslot2), CheckButtonHilight, ButtonHilight-Square
    // PetStableFramePortrait (60 x 60 at 7,-6, BACKGROUND): SetPortraitTexture(.., "player").
    {
        const Rect pr = FR(7, 6, 60, 60);
        const game::Character* me = session_.activeCharacter();
        const int race = me ? static_cast<int>(me->race) : 0, sex = me && me->gender == game::Gender::FEMALE ? 1 : 0;
        if (race > 0 && race < 12 && ufArt_.portrait[race][sex].pixels) ui.imageCircle(ufArt_.portrait[race][sex], pr);
        else ui.fillCircle(pr, 0xFF101010u);
    }
    if (p.stableFrame.pixels) ui.image(p.stableFrame, FR(0, 0, 350, 437));
    else kit_.panel(ui, FR(8, 8, 330, 420));
    const float lh = static_cast<float>(fn.lineHeight());
    ui.textShadow(fn, PX(192), PY(18), CLIENT_STR(STABLES), kColWhite, Align::Center);  // PetStableTitleLabel (GameFontHighlight)
    // The close button (UIPanelCloseButton TOPRIGHT -29,-8: its centre 339, 24).
    {
        const Rect cr{PX(339) - 9, PY(24) - 9, 18, 18};
        const bool f = focusable && winFocus_.addStop(kIdStableClose, cr);
        const pp::Texture& t = f && winFocus_.isPressed(kIdStableClose) && npcArt_.closeDown.pixels ? npcArt_.closeDown : npcArt_.close;
        if (t.pixels) ui.image(t, cr);
        if (f && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, cr, 0xFFFFFFFFu, Blend::Add);
    }
    // The selection and its line (PetStable_Update): STABLE_PET_INFO_TEXT (name, level, family |n talent tree).
    const int sel = session_.stableSelected();
    bool hunterPet = false;
    const bool petUI = session_.hasPetUI(&hunterPet);
    const bool noPets = session_.petGuid() != 0 && petUI && !hunterPet;  // PetStable_NoPetsAllowed (a warlock's pet)
    const pp::PetTextData& pt = session_.petText();
    struct Info {
        bool has = false;
        uint32_t icon = 0, level = 0, foodMask = 0, familyId = 0;
        std::string name, family, talent;
    };
    auto info = [&](int index) {  // GetStablePetInfo(index + 1); the current pet from its unit (as PetStable_Update)
        Info in;
        uint32_t entry = 0;
        if (index == -1) {
            auto unit = session_.petUnit();
            if (unit && petUI && hunterPet) {
                UnitView u;
                unitView(unit->getGuid(), u);
                in.name = !session_.petName().empty() ? session_.petName() : u.name;
                in.level = u.level;
                entry = unit->getField(game::fieldIndex(game::UF::OBJECT_FIELD_ENTRY));
            } else if (session_.stableCurrent().petNumber != 0) {
                const auto& c = session_.stableCurrent();
                in.name = c.name;
                in.level = c.level;
                entry = c.entry;
            } else {
                return in;
            }
        } else if (index >= 0 && index < static_cast<int>(session_.stabledPets().size())) {
            const auto& sp = session_.stabledPets()[index];
            in.name = sp.name;
            in.level = sp.level;
            entry = sp.entry;
        } else {
            return in;
        }
        uint32_t family = session_.creatureFamilyOf(entry);
        if (index == -1 && family == 0) family = session_.petFamily();
        if (const pp::PetFamilyRec* fr = family ? pt.family(family) : nullptr) {
            in.icon = fr->iconId;
            in.familyId = fr->id;
            in.family = fr->name;
            in.foodMask = fr->foodMask;
            if (fr->talentType >= 0)  // the TalentTab with the family's bit in PetTalentMask
                in.talent = fr->talentType == 0   ? OWN_STR(PET_TREE_FEROCITY)
                            : fr->talentType == 1 ? OWN_STR(PET_TREE_TENACITY)
                                                  : OWN_STR(PET_TREE_CUNNING);
        }
        in.has = true;
        return in;
    };
    const Info selInfo = noPets ? Info{} : info(sel);
    if (selInfo.has) {
        const std::string buf =
            firstLine(CLIENT_FMT(STABLE_PET_INFO_TEXT, selInfo.name, static_cast<unsigned>(selInfo.level), selInfo.family, ""));
        ui.textShadow(fn, PX(192), PY(18) + lh + 8 * kSY, buf.c_str(), kColGold, Align::Center);
        if (!selInfo.talent.empty())
            ui.textShadow(fn, PX(192), PY(18) + 2 * lh + 8 * kSY, selInfo.talent.c_str(), kColGold, Align::Center);
    }
    // PetStableModel (313 x 223 at 23,-76). PSP: no 3D model of a stabled pet (the creature renderer draws units in
    // sight only) - the frame's own backdrop shows; PetStablePetInfo (the diet) at the model's corner.
    Rect dietRect{};
    if (selInfo.has && !noPets) {
        dietRect = FR(23 + 7, 76 + 35, 24, 23);
        if (p.happy.pixels) {
            const float cell = static_cast<float>(p.happy.uw) / 3.0f;
            ui.image(p.happy, dietRect, SrcRect{0, 0, cell, static_cast<float>(p.happy.uh)});
        }
        if (focusable && winFocus_.addStop(kIdStableInfo, dietRect)) {
            // PET_DIET_TEMPLATE with BuildListString(GetPetFoodTypes / GetStablePetFoodTypes).
            std::string list;
            for (const std::string& f : pt.foodNames(selInfo.foodMask)) list += (list.empty() ? "" : ", ") + f;
            p.tipLines.clear();
            p.tipLines.push_back({CLIENT_FMT(PET_DIET_TEMPLATE, list), kColWhite});
            p.tipLinesAnchor = Rect{dietRect.x + dietRect.w + ui.offsetX(), dietRect.y + ui.offsetY(), 1, 1};
        }
    }
    // The slots: the current pet (BOTTOMLEFT 45,150) and PetStableStabledPet1..4 (53 to its right, then 15 apart).
    const uint32_t numSlots = session_.stableSlots();
    ui.textShadow(fs, PX(45 + 18.5f), PY(512 - 150 - 37 - 6) - static_cast<float>(fs.lineHeight()), CLIENT_STR(CURRENT_PET), kColGold,
                  Align::Center);
    ui.textShadow(fs, PX(187 + 18.5f + 24), PY(512 - 150 - 37 - 6) - static_cast<float>(fs.lineHeight()), CLIENT_STR(STABLED_PETS),
                  kColGold, Align::Center);
    for (int k = -1; k < 4; ++k) {
        const float sx = k < 0 ? 45.0f : 135.0f + 52.0f * static_cast<float>(k);
        const Rect r = FR(sx, 512 - 150 - 37, 37, 37);
        const bool enabled = k < 0 || static_cast<uint32_t>(k + 1) <= numSlots;
        // $parentBackground UI-EmptySlot 64 x 64 at -13,13 (red past GetNumStableSlots).
        if (p.emptySlot.pixels) ui.image(p.emptySlot, FR(sx - 13, 512 - 150 - 37 - 13, 64, 64), enabled ? 0xFFFFFFFFu : 0xFF1A1AFFu);
        const Info in = noPets ? Info{} : info(k);
        if (in.has)  // SetItemButtonTexture(button, icon): the family's IconFile
            if (const pp::Texture* ic = in.icon ? spellDb_.iconById(in.icon, spellFrame_) : p.familyIcon(in.familyId))
                ui.image(*ic, r);
        if (spellArt_.slot.pixels) {
            const float fsz = 64.0f * kSX;
            ui.image(spellArt_.slot, Rect{r.x + (r.w - fsz) * 0.5f, r.y + (r.h - 64.0f * kSY) * 0.5f - 0.5f, fsz, 64.0f * kSY});
        }
        if (in.has && k == sel && p.checked.pixels) ui.image(p.checked, r, 0xFFFFFFFFu, Blend::Add);  // SetChecked(1)
        const bool f = enabled && focusable && winFocus_.addStop(kIdStableSlot0 + static_cast<uint32_t>(k + 1), r);
        if (f && spellArt_.hilite.pixels) ui.image(spellArt_.hilite, r, 0xFFFFFFFFu, Blend::Add);
        if (f) {
            // OnEnter: the tooltip (name / STABLE_PET_INFO_TOOLTIP_TEXT (level, family |n talent tree), or EMPTY_STABLE_SLOT).
            p.tipLines.clear();
            if (in.has) {
                const std::string buf =
                    firstLine(CLIENT_FMT(STABLE_PET_INFO_TOOLTIP_TEXT, static_cast<unsigned>(in.level), in.family, ""));
                p.tipLines.push_back({in.name, kColWhite});
                p.tipLines.push_back({buf, kColWhite});
                if (!in.talent.empty()) p.tipLines.push_back({in.talent, kColWhite});
            } else {
                p.tipLines.push_back({CLIENT_STR(EMPTY_STABLE_SLOT), kColWhite});
            }
            p.tipLinesAnchor = Rect{r.x + r.w + ui.offsetX(), r.y + ui.offsetY(), 1, 1};
        }
    }
    // Purchase: hidden at NUM_PET_STABLE_SLOTS or away from the master; enabled when the money covers the cost.
    if (!noPets && numSlots < 4 && session_.stableMaster() != 0) {
        const uint32_t cost = session_.stableNextSlotCost();
        const bool afford = session_.getMoneyCopper() >= cost && cost != 0;
        const float sy = PY(512 - 132) - static_cast<float>(fs.lineHeight());
        ui.textShadow(fs, PX(182), sy, CLIENT_STR(STABLE_SLOT_TEXT), kColWhite, Align::Center);  // PetStableSlotText (GameFontHighlightSmall)
        const float cy = PY(512 - 110) - lh;
        ui.textShadow(fn, PX(74), cy, CLIENT_STR(COSTS_LABEL), kColGold);  // PetStableCostLabel
        const float mx = PX(74) + static_cast<float>(fn.measure(CLIENT_STR(COSTS_LABEL))) + 5 * kSX;
        if (afford) drawMoneyFrame(ui, mx, cy + 1, cost, false);
        else {  // SetMoneyFrameColor("red"): PSP - the amount in red text
            const std::string buf = std::to_string(cost / 10000u) + CLIENT_STR(GOLD_AMOUNT_SYMBOL) + " " +
                                    std::to_string((cost / 100u) % 100u) + CLIENT_STR(SILVER_AMOUNT_SYMBOL) + " " +
                                    std::to_string(cost % 100u) + CLIENT_STR(COPPER_AMOUNT_SYMBOL);
            ui.textShadow(fs, mx, cy + 1, buf.c_str(), kColRedFont);
        }
        drawPanelButton(ui, kIdStablePurchase, FR(194, 512 - 105 - 22, 80, 22), CLIENT_STR(PURCHASE), afford, focusable);
    }
    // PetStableMoneyFrame: the player's money, right-aligned at BOTTOMRIGHT -45,84.
    drawMoneyFrame(ui, PX(339), PY(512 - 84) - static_cast<float>(fs.lineHeight()), static_cast<uint32_t>(session_.getMoneyCopper()),
                   true);
    return true;
}

// After all the windows: the pet bar's GameTooltip (a command / reaction: its name, PSP its key; a spell:
// GameTooltip:SetPetAction = the spell tooltip), the frames' plain tooltips, a pet action / stable pet on the cursor.
void WorldScreen::drawPetOverlay(UiRenderer& ui) {
    if (!pet_) return;
    PetUi& p = *pet_;
    const bool ui_ = uiMode_ && !settings_.isOpen();
    if (ui_ && p.tipButton >= 0 && p.tipButton < pp::WorldSession::kPetSlots && !p.cursor) {
        const uint32_t s = session_.petActionSlot(p.tipButton);
        if (pa::isSpell(s) && pa::id(s)) {
            tipSpellId_ = pa::id(s);  // drawSpellTooltips (next) draws it
            tipSpellRect_ = p.tipRect;
        } else {
            const uint8_t t = pa::type(s);
            const uint32_t id = pa::id(s);
            std::string name = t == pa::kTypeCommand ? commandName(id) : t == pa::kTypeReaction ? reactName(id) : "";
            if (!name.empty() && p.tipButton < 4 && controlsScheme_ >= 2) {
                // PetActionButton_OnEnter (a token): the name, then NORMAL_FONT_COLOR " (binding)". PSP: the binding is
                // L + the D-pad arrow, as pad glyphs (the font has no arrows); the box as UiKit::tooltip's one line.
                static constexpr PadGlyph kArrow[4] = {kPadLeft, kPadUp, kPadRight, kPadDown};
                const Font& ft = kit_.font(kFontNormal);
                const float gh = static_cast<float>(ft.lineHeight());
                const float nameW = static_cast<float>(ft.measure(name.c_str()));
                const float openW = static_cast<float>(ft.measure(" ("));
                const float lW = kit_.glyphWidth(kPadL, gh);
                const float plusW = static_cast<float>(ft.measure(" + "));
                const float closeW = static_cast<float>(ft.measure(")"));
                Rect r{p.tipRect.x, p.tipRect.y - 2 - 44.0f, nameW + openW + lW + plusW + gh + closeW + 16.0f, 22.0f};
                if (r.x + r.w > pp::kScreenW - 4) r.x = pp::kScreenW - 4 - r.w;
                if (r.x < 4) r.x = 4;
                ui.boundsHold(true);
                if (kit_.has(kTexTipBg)) ui.tile(kit_.tex(kTexTipBg), Rect{r.x + 3, r.y + 3, r.w - 6, r.h - 6}, 0xE0FFFFFFu);
                else ui.fill(r, 0xE0100000u);
                if (kit_.has(kTexTipBorder)) ui.nineSlice(kit_.tex(kTexTipBorder), r);
                float x = r.x + 8.0f;
                const float y = r.y + 5.0f;
                ui.text(ft, x, y, name.c_str(), kColWhite);
                x += nameW;
                ui.text(ft, x, y, " (", kColGold);
                x += openW;
                if (!kit_.glyphFill(ui, kPadL, Rect{x, y, lW, gh})) ui.text(ft, x, y, "L", kColGold);
                x += lW;
                ui.text(ft, x, y, " + ", kColGold);
                x += plusW;
                kit_.glyphFill(ui, kArrow[p.tipButton], Rect{x, y, gh, gh});
                x += gh;
                ui.text(ft, x, y, ")", kColGold);
                ui.boundsHold(false);
            } else if (!name.empty()) {
                kit_.tooltip(ui, Rect{p.tipRect.x, p.tipRect.y - 2, p.tipRect.w, 1}, name.c_str(), "");
            }
        }
    }
    if (ui_ && !p.tipLines.empty()) {
        const Font& fs = kit_.font(kFontSmall);
        const Font& fn = kit_.font(kFontNormal);
        float w = 0, h = 8;
        for (size_t i = 0; i < p.tipLines.size(); ++i) {
            const Font& f = i == 0 ? fn : fs;
            w = std::max(w, static_cast<float>(f.measure(p.tipLines[i].first.c_str())));
            h += static_cast<float>(f.lineHeight());
        }
        Rect r{p.tipLinesAnchor.x + 2, p.tipLinesAnchor.y, w + 14, h};
        if (r.x + r.w > pp::kScreenW - 2) r.x = std::max(2.0f, p.tipLinesAnchor.x - r.w - 24);
        r.y = std::clamp(r.y, 2.0f, std::max(2.0f, pp::kScreenH - r.h - 2));
        if (kit_.has(kTexTipBg)) ui.tile(kit_.tex(kTexTipBg), Rect{r.x + 3, r.y + 3, r.w - 6, r.h - 6}, 0xF0FFFFFFu);
        else ui.fill(r, 0xF0100000u);
        if (kit_.has(kTexTipBorder)) ui.nineSlice(kit_.tex(kTexTipBorder), r);
        float y = r.y + 4;
        for (size_t i = 0; i < p.tipLines.size(); ++i) {
            const Font& f = i == 0 ? fn : fs;
            ui.text(f, r.x + 7, y, p.tipLines[i].first.c_str(), p.tipLines[i].second);
            y += static_cast<float>(f.lineHeight());
        }
    }
    p.tipLines.clear();
    // The cursor's icon (the original's cursor texture is the action's / the pet's icon).
    if (uiMode_ && (p.cursor || p.stableCursor != -2)) {
        const pp::Texture* icon = nullptr;
        if (p.cursor) {
            const uint8_t t = pa::type(p.cursor);
            const uint32_t id = pa::id(p.cursor);
            if (pa::isSpell(p.cursor)) icon = spellDb_.icon(id, spellFrame_);
            else if (t == pa::kTypeCommand && id < 4) icon = spellDb_.iconById(kCommandIcon[id], spellFrame_);
            else if (t == pa::kTypeReaction && id < 3) icon = spellDb_.iconById(kReactIcon[id], spellFrame_);
        } else {
            uint32_t entry = 0;
            if (p.stableCursor == -1) {
                if (auto u = session_.petUnit()) entry = u->getField(game::fieldIndex(game::UF::OBJECT_FIELD_ENTRY));
                else entry = session_.stableCurrent().entry;
            } else if (p.stableCursor >= 0 && p.stableCursor < static_cast<int>(session_.stabledPets().size())) {
                entry = session_.stabledPets()[p.stableCursor].entry;
            }
            const uint32_t fam = session_.creatureFamilyOf(entry);
            if (const pp::PetFamilyRec* fr = fam ? session_.petText().family(fam) : nullptr)
                icon = fr->iconId ? spellDb_.iconById(fr->iconId, spellFrame_) : p.familyIcon(fr->id);
        }
        const Rect r{winFocus_.cursorX() + 6.0f, winFocus_.cursorY() + 6.0f, 16.0f, 16.0f};
        if (icon) ui.image(*icon, r, 0xD0FFFFFFu);
        else ui.fill(r, 0xA0404040u);
    }
}

// --- PlayerTalentFrame: the pet's spec tab ----------------------------------------------------------------------------

bool WorldScreen::talentPetView() const {
    return pet_ && pet_->talentPet && session_.petTalentTab() != nullptr;
}

const pp::Texture* WorldScreen::petTalentBg() {
    PetUi& p = petUi();
    const game::TalentTabEntry* tab = session_.petTalentTab();
    if (!tab) return nullptr;
    const int32_t key = static_cast<int32_t>(tab->tabId);
    if (p.talentBgFor != key) {
        p.releaseTalentBg();
        p.talentBgFor = key;
        char path[256];
        pp::fsPath(path, sizeof(path), "data/ui.pak");
        pp::Pak pak;
        if (pak.open(path)) {
            std::string name = "tb_";
            for (char c : tab->backgroundFile) name += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            loadOne(pak, name.c_str(), p.talentBg, p.talentBgBytes);
            pak.close();
        }
        PSP_LOGI(pp::tag::Gfx, "pet_talent_bg tab=%u ok=%d kb=%u", static_cast<unsigned>(tab->tabId), p.talentBg.pixels ? 1 : 0,
                 static_cast<unsigned>((p.talentBgBytes + 1023u) / 1024u));
    }
    return p.talentBg.pixels ? &p.talentBg : nullptr;
}

// PlayerSpecTab1 ("spec1": the player's portrait) and PlayerSpecTab3 ("petspec1": the pet's, shown while the pet has
// a talent tree - GetNumTalentGroups(false, true)); the selected one checked (GOLD_INSIDE: CheckButtonHilight); their
// tooltips TALENT_SPEC_PRIMARY / TALENT_SPEC_PET_PRIMARY. Background SpellBook-SkillLineTab (64 x 64 at -3,11).
void WorldScreen::drawTalentSpecTabs(UiRenderer& ui, bool top, const Rect& specTab1, const Rect& petTab) {
    PetUi& p = petUi();
    const bool petTree = session_.loadPetTalents();
    if (!petTree) {
        p.talentPet = false;
        return;
    }
    if (!p.artTried) p.loadArt();
    static pp::Texture tabBg;  // SpellBook-SkillLineTab (ui.pak sb_tab), kept once loaded (2 KB)
    static bool tabTried = false;
    if (!tabTried) {
        tabTried = true;
        char path[256];
        pp::fsPath(path, sizeof(path), "data/ui.pak");
        pp::Pak pak;
        uint32_t bytes = 0;
        if (pak.open(path)) {
            loadOne(pak, "sb_tab", tabBg, bytes);
            pak.close();
        }
    }
    for (int k = 0; k < 2; ++k) {
        const Rect& r = k == 0 ? specTab1 : petTab;
        const bool selected = (k == 1) == p.talentPet;
        if (tabBg.pixels) {
            const float sx = r.w / 32.0f, sy = r.h / 32.0f;
            ui.image(tabBg, Rect{r.x - 3.0f * sx, r.y - 11.0f * sy, 64.0f * sx, 64.0f * sy});
        }
        // SetPortraitTexture(normalTexture, "player" / "pet").
        if (k == 0) {
            const game::Character* me = session_.activeCharacter();
            const int race = me ? static_cast<int>(me->race) : 0, sex = me && me->gender == game::Gender::FEMALE ? 1 : 0;
            if (race > 0 && race < 12 && ufArt_.portrait[race][sex].pixels) ui.image(ufArt_.portrait[race][sex], r);
            else ui.fill(r, 0xFF202020u);
        } else if (p.portraitOk && petPortraitTex_.pixels) {
            ui.image(petPortraitTex_, r);
        } else {
            ui.fill(r, 0xFF202020u);
        }
        if (selected && p.checked.pixels) ui.image(p.checked, r, 0xFFFFFFFFu, Blend::Add);
        const bool f = top && uiMode_ && winFocus_.addStop(k == 0 ? kIdSpecTab1 : kIdSpecTabPet, r);
        if (f) {
            if (spellArt_.hilite.pixels) ui.image(spellArt_.hilite, r, 0xFFFFFFFFu, Blend::Add);
            p.tipLines.clear();
            p.tipLines.push_back({k == 0 ? CLIENT_STR(TALENT_SPEC_PRIMARY) : CLIENT_STR(TALENT_SPEC_PET_PRIMARY), kColWhite});
            p.tipLinesAnchor = Rect{r.x + r.w + ui.offsetX(), r.y + ui.offsetY(), 1, 1};
        }
    }
}

// --- CharacterFrame's pet tab: PetPaperDollFrame ------------------------------------------------------------------

bool WorldScreen::petTabShown() const { return session_.hasPetUI(); }  // CharacterFrame_UpdateTabs: HasPetUI()

bool WorldScreen::drawPetPaperDoll(UiRenderer& ui, bool focusable) {
    PetUi& p = petUi();
    if (!p.pdArtTried) p.loadPdArt();
    if (!p.artTried) p.loadArt();
    loadNpcArt(false);
    const Font& fn = kit_.font(kFontNormal);
    const Font& fs = kit_.font(kFontSmall);
    const Font& ft = kit_.font(kFontTiny);
    // CharacterFramePortrait (the player's, as the other tabs) under the frame.
    {
        const Rect pr = FR(7, 6, 60, 60);
        const game::Character* me = session_.activeCharacter();
        const int race = me ? static_cast<int>(me->race) : 0, sex = me && me->gender == game::Gender::FEMALE ? 1 : 0;
        if (race > 0 && race < 12 && ufArt_.portrait[race][sex].pixels) ui.imageCircle(ufArt_.portrait[race][sex], pr);
    }
    if (p.pdFrame.pixels) ui.image(p.pdFrame, FR(0, 0, 352, 482));
    else kit_.panel(ui, FR(8, 8, 336, 466));
    // The close button (CharacterFrameCloseButton CENTER at TOPRIGHT -44,-25), PetPaperDollCloseButton CLOSE.
    {
        const Rect cr{PX(340) - 9, PY(25) - 9, 18, 18};
        const bool f = focusable && winFocus_.addStop(kIdPdClose, cr);
        const pp::Texture& t = f && winFocus_.isPressed(kIdPdClose) && npcArt_.closeDown.pixels ? npcArt_.closeDown : npcArt_.close;
        if (t.pixels) ui.image(t, cr);
    }
    drawCharacterFrameTabs(ui, focusable);
    auto pet = session_.petUnit();
    if (!pet) return true;  // PetPaperDollFrame_Update: not hasPetUI -> nothing
    UnitView u;
    unitView(pet->getGuid(), u);
    // PetNameText (GameFontHighlight, CENTER 6,232 -> 198, 24) instead of CharacterNameText.
    const std::string& name = !session_.petName().empty() ? session_.petName() : u.name;
    ui.textShadow(fn, PX(198), PY(24) - static_cast<float>(fn.lineHeight()) * 0.5f, name.c_str(), kColWhite, Align::Center);
    // PetLevelText (GameFontNormalSmall CENTER 0,-55 -> y 311): UNIT_TYPE_LEVEL_TEMPLATE (level, family).
    const uint32_t family = session_.petFamily() ? session_.petFamily()
                                                 : session_.creatureFamilyOf(pet->getField(game::fieldIndex(game::UF::OBJECT_FIELD_ENTRY)));
    const pp::PetFamilyRec* fr = family ? session_.petText().family(family) : nullptr;
    if (fr) {
        const std::string buf = CLIENT_FMT(UNIT_TYPE_LEVEL_TEMPLATE, static_cast<unsigned>(u.level), fr->name);
        ui.textShadow(fs, PX(192), PY(311) - static_cast<float>(fs.lineHeight()) * 0.5f, buf.c_str(), kColGold,
                      Align::Center);
    }
    // PetModelFrame (318 x 224 at 25,-78): PSP - the pet's portrait still in the middle of the model's place (the
    // creature renderer has one VRAM model target, the character window's own, and it shows the player).
    if (p.portraitOk && petPortraitTex_.pixels) {
        const float side = 110.0f * kSX;
        ui.imageCircle(petPortraitTex_, Rect{PX(25 + 159) - side * 0.5f, PY(78 + 112) - side * 0.5f * kSY / kSX, side, side * kSY / kSX});
    }
    // PetPaperDollPetInfo (the diet), when the pet can gain experience (HasPetUI's second value).
    bool hunterPet = false;
    session_.hasPetUI(&hunterPet);
    if (hunterPet) {
        const Rect dr = FR(23 + 7, 76 + 35, 24, 23);
        if (p.happy.pixels) {
            const float cell = static_cast<float>(p.happy.uw) / 3.0f;
            ui.image(p.happy, dr, SrcRect{0, 0, cell, static_cast<float>(p.happy.uh)});
        }
        if (focusable && winFocus_.addStop(kIdPdInfo, dr) && fr) {
            std::string list;
            for (const std::string& f : session_.petText().foodNames(fr->foodMask)) list += (list.empty() ? "" : ", ") + f;
            p.tipLines.clear();
            p.tipLines.push_back({CLIENT_FMT(PET_DIET_TEMPLATE, list), kColWhite});
            p.tipLinesAnchor = Rect{dr.x + dr.w + ui.offsetX(), dr.y + ui.offsetY(), 1, 1};
        }
    }
    // PetPaperDollFrameExpBar (319 x 11 at BOTTOMLEFT 23,103 -> y 398): GetPetExperience, (0.58, 0, 0.55), the
    // UI-MainMenuBar-Dwarf strips over it; the text "XP x / y" under the cursor (TextStatusBar).
    uint32_t cur = 0, next = 0;
    session_.petExperience(cur, next);
    {
        const Rect xb = FR(23, 398, 319, 11);
        ui.fill(xb, 0x80000000u);
        if (next > 0) {
            const float f = std::clamp(static_cast<float>(cur) / static_cast<float>(next), 0.0f, 1.0f);
            const Rect fill{xb.x, xb.y, xb.w * f, xb.h};
            if (ufArt_.bar.pixels) ui.image(ufArt_.bar, fill, SrcRect{0, 0, ufArt_.bar.uw * f, static_cast<float>(ufArt_.bar.uh)}, 0xFF8C0094u);
            else ui.fill(fill, 0xFF8C0094u);
        }
        const bool f = focusable && winFocus_.addStop(kIdPdXp, xb);
        if (f) {
            const std::string buf = std::string(CLIENT_STR(XP)) + " " + std::to_string(cur) + " / " + std::to_string(next);
            ui.textShadow(ft, xb.x + xb.w * 0.5f, xb.y + (xb.h - static_cast<float>(ft.lineHeight())) * 0.5f, buf.c_str(),
                          kColWhite, Align::Center);
        }
    }
    // PetAttributesFrame (67,-320): PetStatFrame1..5 left, attack power / damage / spell damage / armor right; the
    // label GameFontNormalSmall (STAT_FORMAT), the value GameFontHighlightSmall right (green with a bonus, red with a
    // malus). 13 units a row, 1 apart.
    const char* const kStatName[5] = {CLIENT_STR(SPELL_STAT1_NAME), CLIENT_STR(SPELL_STAT2_NAME), CLIENT_STR(SPELL_STAT3_NAME),
                                      CLIENT_STR(SPELL_STAT4_NAME), CLIENT_STR(SPELL_STAT5_NAME)};
    auto field = [&](uint16_t i) { return pet->getField(i); };
    auto row = [&](float x, int k, const char* label, const std::string& value, uint32_t col, uint32_t id) {
        const float y = 320.0f + 5.0f + 14.0f * static_cast<float>(k);
        const Rect r = FR(x, y, 104, 13);
        char l[48];
        std::snprintf(l, sizeof(l), "%s:", label);
        ui.textShadow(ft, r.x, r.y, l, kColGold);
        ui.textShadow(ft, r.x + r.w, r.y, value.c_str(), col, Align::Right);
        if (focusable && winFocus_.addStop(id, r)) ui.fill(r, 0x20FFFFFFu);
    };
    const uint16_t fStat0 = game::fieldIndex(game::UF::UNIT_FIELD_STAT0);
    for (int i = 0; i < 5; ++i) {
        // UnitStat: effective STATn, POSSTATn (89..93) / NEGSTATn (94..98) - 3.3.5a UpdateFields.h.
        const int32_t eff = static_cast<int32_t>(field(static_cast<uint16_t>(fStat0 + i)));
        const int32_t pos = static_cast<int32_t>(field(static_cast<uint16_t>(89 + i)));
        const int32_t neg = static_cast<int32_t>(field(static_cast<uint16_t>(94 + i)));
        const uint32_t col = neg < 0 ? kColRedFont : pos > 0 ? kColGreenFont : kColWhite;
        row(73, i, kStatName[i], std::to_string(eff), col, kIdPdStat0 + static_cast<uint32_t>(i));
    }
    // UnitAttackPower("pet"): AzerothCore Creature::UpdateAttackPowerAndDamage writes the base (ATTACK_POWER), the
    // mods as one signed int32 (ATTACK_POWER_MODS) and the multiplier - 1 (ATTACK_POWER_MULTIPLIER); the total is
    // (base + mods) x (1 + multiplier) (Unit::GetTotalAttackPowerValue). Own rule: positive mods are the bonus,
    // negative the malus (PaperDollFormatStat's green / red), the total rounded to the nearest.
    {
        const int32_t ap = static_cast<int32_t>(field(game::fieldIndex(game::UF::UNIT_FIELD_ATTACK_POWER)));
        const int32_t mods = static_cast<int32_t>(field(124));
        float mult = 0.0f;
        const uint32_t mb = field(125);
        std::memcpy(&mult, &mb, 4);
        if (!std::isfinite(mult)) mult = 0.0f;
        const int32_t total = static_cast<int32_t>(std::lround((1.0f + mult) * static_cast<float>(ap + mods)));
        row(67 + 122, 0, CLIENT_STR(ATTACK_POWER), std::to_string(std::max<int32_t>(0, total)),
            mods < 0 ? kColRedFont : mods > 0 ? kColGreenFont : kColWhite, kIdPdStat0 + 5);
    }
    {  // UnitDamage: MINDAMAGE / MAXDAMAGE (floats), "%d - %d"
        float mn = 0.0f, mx = 0.0f;
        const uint32_t a = field(game::fieldIndex(game::UF::UNIT_FIELD_MINDAMAGE)), b = field(game::fieldIndex(game::UF::UNIT_FIELD_MAXDAMAGE));
        std::memcpy(&mn, &a, 4);
        std::memcpy(&mx, &b, 4);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d - %d", std::max(1, static_cast<int>(std::floor(mn))), std::max(1, static_cast<int>(std::ceil(mx))));
        row(67 + 122, 1, CLIENT_STR(DAMAGE), buf, kColWhite, kIdPdStat0 + 6);
    }
    {  // GetPetSpellBonusDamage: the owner's PLAYER_PET_SPELL_POWER (UNIT_END + 0x499 = 1325; AzerothCore's pet scripts
       // write the pet's spell power there)
        const int32_t sp = static_cast<int32_t>(session_.selfField(1325));
        row(67 + 122, 2, CLIENT_STR(SPELL_BONUS), sp > 0 ? "+" + std::to_string(sp) : std::to_string(sp),
            sp > 0 ? kColGreenFont : sp < 0 ? kColRedFont : kColWhite, kIdPdStat0 + 7);
    }
    {  // UnitArmor: RESISTANCES[0] (+ the buff mods 106 / 113)
        const int32_t armor = static_cast<int32_t>(field(game::fieldIndex(game::UF::UNIT_FIELD_RESISTANCES)));
        const int32_t pos = static_cast<int32_t>(field(106)), neg = static_cast<int32_t>(field(113));
        row(67 + 122, 3, CLIENT_STR(ARMOR), std::to_string(armor), neg < 0 ? kColRedFont : pos > 0 ? kColGreenFont : kColWhite,
            kIdPdStat0 + 8);
    }
    // PetResistanceFrame (TOPRIGHT at 347,-77): PetMagicResFrame1..5 = arcane (6), fire, nature, frost, shadow; the value
    // at the icon's BOTTOM 0,3 (GameFontHighlightSmall; green > 0, red < 0). The icons are in pet_pd_frame.
    {
        static const int kRes[5] = {6, 2, 3, 4, 5};
        const char* const kResName[5] = {CLIENT_STR(RESISTANCE6_NAME), CLIENT_STR(RESISTANCE2_NAME), CLIENT_STR(RESISTANCE3_NAME),
                                         CLIENT_STR(RESISTANCE4_NAME), CLIENT_STR(RESISTANCE5_NAME)};
        const uint16_t fRes = game::fieldIndex(game::UF::UNIT_FIELD_RESISTANCES);
        for (int k = 0; k < 5; ++k) {
            const int32_t v = static_cast<int32_t>(field(static_cast<uint16_t>(fRes + kRes[k])));
            const Rect r = FR(315, 77 + 29.0f * static_cast<float>(k), 32, 29);
            ui.textShadow(ft, r.x + r.w * 0.5f, r.y + r.h - 3 * kSY - static_cast<float>(ft.lineHeight()), std::to_string(v).c_str(),
                          v > 0 ? kColGreenFont : v < 0 ? kColRedFont : kColWhite, Align::Center);
            if (focusable && winFocus_.addStop(kIdPdRes0 + static_cast<uint32_t>(k), r)) {
                p.tipLines.clear();
                p.tipLines.push_back({kResName[k], kColWhite});
                p.tipLinesAnchor = Rect{r.x + r.w + ui.offsetX(), r.y + ui.offsetY(), 1, 1};
            }
        }
    }
    drawPanelButton(ui, kIdPdClose, Rect{PX(305) - 40 * kSX, PY(422) - 11 * kSY, 80 * kSX, 22 * kSY}, CLIENT_STR(CLOSE), true,
                    focusable);
    return true;
}

}  // namespace wowee::ui::psp
