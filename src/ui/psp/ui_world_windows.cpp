// In-world windows and the cursor mode.
//
// SELECT switches between the game (stick = camera, buttons = movement) and
// the interface, like the Back button of upstream's GamepadControls
// (setPointerMode: "a mode, not a hold"). In the interface:
//   stick      free cursor (FocusRing: source-psp's nub cursor, dead zone,
//              acceleration), hovering focuses a widget;
//   D-pad      jumps focus between widgets (source-psp nav ring);
//   Cross      activates (upstream: South = click);
//   Circle     closes the top window, then leaves the mode (upstream: East =
//              Escape);
//   SELECT     back to the game (open windows stay on screen, inactive).
// The game gets no input while the cursor is up (the PSP has one stick and
// the face buttons are the click / back keys). A bar of buttons in the
// bottom right opens the windows; debug switches that used to live on
// button combos are in the "Debug" window.
#include "ui/psp/ui_world_screen.hpp"

#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_public_safe.hpp"  // PSP: automation / debug only on a dev server
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"
#include "platform/psp/psp_system.hpp"
#include "rendering/psp/psp_attachments.hpp"  // Debug toggle

#include <pspctrl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
namespace prof = wowee::platform::psp::prof;

namespace {

enum : uint32_t {
    kIdBarMenu = 0x5701,
    kIdBarDebug,
    kIdMenuGraphics = 0x5711,
    kIdMenuExit,
    kIdMenuClose,
    kIdDbgOverlay = 0x5720,
    kIdDbgCapture,
    kIdDbgModels,
    kIdDbgTerrain,
    kIdDbgCharAnim,
    kIdDbgNpcs,
    kIdDbgFill,
    kIdDbgBlend,
    kIdDbgClose,
};

// forest LOD (Debug window rows, not saved; wowee.cfg sets the start values)
enum : uint32_t { kIdDbgTreeLod = 0x57F0, kIdDbgLod2Dist, kIdDbgImpostors, kIdDbgImpDist };
constexpr int kForestRowsH = 52;  // two rows of buttons
// NPC animation (npcanim) and attached items (attachments) A/B
enum : uint32_t { kIdDbgNpcAnim = 0x57F8, kIdDbgAttach, kIdDbgHudLines, kIdDbgDrop };
constexpr int kBatch2RowsH = 26;
constexpr int kBatch3FSoundRowsH = 26;  // sounds (test sound, footsteps mode)
const char* impostorModeText(int m) {
    switch (m) {
        case 1: return OWN_STR(DBG_IMP_QUADS);
        case 2: return OWN_STR(DBG_IMP_SPRITES);
        case 3: return OWN_STR(LIGHT_COLOR);
        default: return OWN_STR(PORT_OFF_LC);
    }
}
// Next value of a cycled distance list (yards).
float nextStep(float cur, std::initializer_list<int> steps) {
    for (int s : steps)
        if (s > cur + 0.5f) return static_cast<float>(s);
    return static_cast<float>(*steps.begin());
}

const char* terrainModeName(int m) {
    switch (m) {
        case 1: return OWN_STR(DBG_TERRAIN_UNTEXTURED);
        case 2: return OWN_STR(DBG_TERRAIN_HIDDEN);
        case 3: return OWN_STR(DBG_TERRAIN_NO_DETAIL);
        default: return OWN_STR(DBG_TERRAIN_NORMAL);
    }
}

} // namespace

bool WorldScreen::windowOpen(uint8_t w) const {
    return std::find(windows_.begin(), windows_.end(), w) != windows_.end();
}

// upstream UiSoundManager open/close sounds of the bags,
// the character sheet and the quest log.
static void windowSound(uint8_t w, bool open) {
    switch (w) {
        case 0x50: playUiSound(open ? UiSound::CharacterSheetOpen : UiSound::CharacterSheetClose); break;  // kWinCharacter
        case 0x51: playUiSound(open ? UiSound::BagOpen : UiSound::BagClose); break;                        // kWinBags
        case 0x5D: playUiSound(open ? UiSound::QuestLogOpen : UiSound::QuestLogClose); break;              // kWinQuestLog
        // DressUpFrame OnShow / OnHide - igCharacterInfoOpen / igCharacterInfoClose
        case 0xFC: playUiSound(open ? UiSound::CharacterSheetOpen : UiSound::CharacterSheetClose); break;  // kWinDressUp
        default: break;
    }
}

bool WorldScreen::isPopupWindow(uint8_t w) const {
    switch (w) {
        case kWinMenu: case kWinDebug: case kWinRelease: case kWinRecoverCorpse: case kWinResurrect: case kWinXpLoss:
        case kWinBindConfirm: case kWinUnlearnSkill: case kWinTalentWipe: case kWinQuestAbandon: case kWinUnitPopup:
        case kWinPartyInvite: case kWinLootRollConfirm: case kWinBankBuy: case kWinMailPopup:
        case kWinMinimapTracking: case kWinTutorialMenu:
        case kWinBgConfirm: case kWinBgConfirm2: case kWinDropDown:
        case kWinWotlkDropDown: case kWinWotlkPopup:  // MiniMapLFGFrameDropDown, CALENDAR_ERROR
        case kWinInstanceBoot: case kWinInstanceLock:  // INSTANCE_BOOT / _LOCK
        case kWinDeleteItem:  // DELETE_ITEM / DELETE_GOOD_ITEM
        case kWinUseBind:  // StaticPopup USE_BIND
        case kWinAuctionPopup: case kWinAuctionDropDown: case kWinAuctionDisabled:
        case kWinGuildInvite: case kWinSocialConfirm: case kWinSocialDropDown:  // GUILD_INVITE, confirms
        case kWinDuelRequest: case kWinDuelBounds: case kWinSummon: case kWinQuestAccept: case kWinTradeBind:
        case kWinTradeRequest:  // StaticPopup TRADE
        case kWinPetRename: case kWinPetPopup:  // RENAME_PET, PETRENAMECONFIRM / ABANDON_PET / ...
        case kWinTotemFlyout:  // MultiCastFlyoutFrame
        case kWinGuildBankPopup: case kWinGlyphConfirm: case kWinChannelDaughter:
        case kWinChannelDropDown: case kWinChannelPopup: case kWinGuildBankTabPopup:
            return true;
        default:
            return false;
    }
}

// right of the rightmost open panel, else left of the leftmost - wholly on the screen, else the
// default place (overlapping, as before). The full-screen world map is no panel.
void WorldScreen::placeBeside(uint8_t w, const Rect& base) {
    float minX = 1e9f, maxX = -1e9f;
    bool any = false;
    for (uint8_t o : windows_) {
        if (o == w || isPopupWindow(o) || (o == kWinWorldMap && !mapMini())) continue;
        if (o == kWinPullouts || o == kWinReadyCheck) continue;  // HUD-like frames, not panels
        auto r = winRect_.find(o);
        if (r == winRect_.end() || r->second.w <= 0.0f) continue;
        minX = std::min(minX, r->second.x);
        maxX = std::max(maxX, r->second.x + r->second.w);
        any = true;
    }
    if (!any || base.w >= pp::kScreenW) return;
    float x = -1.0f;
    if (maxX + 2.0f + base.w <= pp::kScreenW) x = maxX + 2.0f;
    else if (minX - 2.0f - base.w >= 0.0f) x = minX - 2.0f - base.w;
    if (x < 0.0f) return;
    autoPos_[w] = WinPos{std::floor(x), base.y};
    PSP_LOGI(pp::tag::Ctrl, "window_beside id=%u x=%.0f", static_cast<unsigned>(w), static_cast<double>(x));
}

// Circle on the title band (top 22 px) of the topmost window under the cursor closes it.
bool WorldScreen::closeTitleUnderCursor() {
    const float cx = winFocus_.cursorX(), cy = winFocus_.cursorY();
    for (auto it = winDrawn_.rbegin(); it != winDrawn_.rend(); ++it) {
        auto r = winRect_.find(*it);
        if (r == winRect_.end() || !r->second.contains(cx, cy)) continue;
        if (cy >= r->second.y + 22.0f || deathPopupKeepsOnEscape(*it)) return false;  // the topmost one decides
        PSP_LOGI(pp::tag::Ctrl, "window_title_close id=%u", static_cast<unsigned>(*it));
        closeWindow(*it);
        return true;
    }
    return false;
}

void WorldScreen::openWindow(uint8_t w) {
    if (fxOpenWindow(w)) return;  // framexml - a FrameXML panel replaces this window (fxhide*)
    static_assert(kWinCharacter == 0x50 && kWinBags == 0x51 && kWinQuestLog == 0x5D, "windowSound ids");
    // A panel opened while other panels are open goes beside them (unless dragged somewhere before).
    if (!windowOpen(w) && !isPopupWindow(w) && !winPos_.count(w)) {
        bool others = false;
        for (uint8_t o : windows_) others = others || (!isPopupWindow(o) && o != kWinWorldMap);
        autoPos_.erase(w);
        if (others && std::find(placeNext_.begin(), placeNext_.end(), w) == placeNext_.end()) placeNext_.push_back(w);
    }
    if (!windowOpen(w)) windowSound(w, true);
    uiArtNoteWindowOpen();  // art that failed to load is tried again (UiArtRetry)
    windows_.erase(std::remove(windows_.begin(), windows_.end(), w), windows_.end());
    windows_.push_back(w);  // on top
    // PSP: the game menu opens with its first button focused (was: the focus stayed on the HUD,
    // the D-pad first had to find the menu).
    if (w == kWinMenu) winFocus_.setFocus(kIdMenuGraphics);
    PSP_LOGI(pp::tag::Ctrl, "window_open id=%u", static_cast<unsigned>(w));
}

void WorldScreen::closeWindow(uint8_t w) {
    if (windowOpen(w)) windowSound(w, false);
    windows_.erase(std::remove(windows_.begin(), windows_.end(), w), windows_.end());
    autoPos_.erase(w);  // the next open starts from its own place again
    placeNext_.erase(std::remove(placeNext_.begin(), placeNext_.end(), w), placeNext_.end());
}

void WorldScreen::setUiMode(bool on) {
    if (on == uiMode_) return;
    uiMode_ = on;
    // chat button - SELECT, Cross opens the chat.
    if (on) winFocus_.setFocus(windows_.empty() ? WorldChat::kBarButtonId : 0u);
    PSP_LOGI(pp::tag::Ctrl, "ui_mode on=%d windows=%u", on ? 1 : 0, static_cast<unsigned>(windows_.size()));
}

void WorldScreen::updateWindows(const pp::PadState& pad, float dtSec) {
    // Cross = the left mouse button, Circle = the right one (FocusRing activated /
    // activatedAlt), START = Esc: it closes the top window, with none open it opens the game menu
    // (was: Circle closed windows and left the interface). SELECT still leaves the interface.
    pp::PadState p = pad;
    p.held &= ~PSP_CTRL_START;
    p.pressed &= ~PSP_CTRL_START;
    p.released &= ~PSP_CTRL_START;
    if ((pad.pressed & PSP_CTRL_START)) {
        if (mapCloseDropDown()) {  // Esc closes the map's dropdown list first
            winFocus_.update(p, dtSec);
            return;
        }
        if (tradeSkillCloseDropDown()) {  // Esc closes the trade skill dropdown list first
            winFocus_.update(p, dtSec);
            return;
        }
        if (wotlkCloseDropDown()) {  // Esc closes the LFD / achievement / calendar lists first
            winFocus_.update(p, dtSec);
            return;
        }
        if (socialCloseDropDown()) {  // Esc closes the status / who / rank lists first
            winFocus_.update(p, dtSec);
            return;
        }
        if (ggbCloseDropDown()) {  // Esc closes ChannelListDropDown / the roster menu first
            winFocus_.update(p, dtSec);
            return;
        }
        if (petClearCursor()) {  // Esc drops a pet action / a stable pet off the cursor (ClearCursor)
            winFocus_.update(p, dtSec);
            return;
        }
        // A popup without hideOnEscape (DEATH, RECOVER_CORPSE) is passed by.
        // Two windows at once: START closes only the menu and popups (StaticPopup
        // EscapePressed); with panels open it opens the game menu over them - they stay (the panels close with
        // their close button or Circle on the title).
        uint8_t escTop = 0;
        for (auto it = windows_.rbegin(); it != windows_.rend() && !escTop; ++it)
            if (!deathPopupKeepsOnEscape(*it) && isPopupWindow(*it)) escTop = *it;
        if (itemUseEscape()) {
            // Esc while an item spell waits for its item - SpellStopTargeting
        } else if (ggbEscape()) {
            // Esc with a glyph waiting for its socket / a vault item on the cursor
        } else if (bookPick_ != 0) {
            bookPick_ = 0;  // Esc with a spell on the cursor drops it (ClearCursor)
            bookPickItem_ = false;
        } else if (escTop != 0) {
            // layers - the Debug window is drawn on top, so it closes first.
            if (windowOpen(kWinDebug)) closeWindow(kWinDebug);
            else closeWindow(escTop);  // through closeWindow (its sound)
        } else {
            openWindow(kWinMenu);
        }
    }
    tipCompareHeld_ = (pad.held & PSP_CTRL_LTRIGGER) != 0;  // L = Shift (IsModifiedClick("COMPAREITEMS"))
    dragWindows(p, pad);  // R + stick moves a window
    dressUpPad(p);  // L + Triangle = Ctrl+click (DressUpItemLink) - before the loot window's Triangle
    lootWindowPad(p);  // loot - Up / Triangle take all, before the focus sees Up
    partyWindowPad(p);  // a click outside the unit menu closes it (before the focus moves)
    raidBgWindowPad(p);  // Square = the drag (raid members, pullouts), clicks outside a dropdown
    socialWindowPad(p);  // a click outside the social dropdown list closes it
    petWindowPad(p);  // Square = the drag (pet buttons, stable slots)
    mapZonePad(p);  // the D-pad walks the world map's zones on the continent view
    winFocus_.update(p, dtSec);
    if (dragWin_) {
        // The window follows the cursor the stick just moved.
        // The saved top-left follows the cursor; windowPos() keeps the
        // whole window on the screen (was: 40 x 20 px of it).
        WinPos& wp = winPos_[dragWin_];
        wp.dx += winFocus_.cursorX() - dragX_;
        wp.dy += winFocus_.cursorY() - dragY_;
        dragX_ = winFocus_.cursorX();
        dragY_ = winFocus_.cursorY();
        auto it = winRect_.find(dragWin_);
        if (it != winRect_.end()) {
            const Rect& r = it->second;
            wp.dx = std::clamp(wp.dx, 0.0f, std::max(0.0f, pp::kScreenW - r.w));
            wp.dy = std::clamp(wp.dy, 0.0f, std::max(0.0f, pp::kScreenH - r.h));
        }
        return;  // no clicks while dragging
    }
    if (deleteItemPad(p)) return;  // the held item put down on the world -> DELETE_ITEM
    ggbWindowPad(p);        // Square on the vault's slots (before the bags' own)
    bankMailWindowPad(p);   // Square on the bank / letter slots (before the bags' own)
    auctionWindowPad(p);    // Square on the sell slot (its drag)
    inventoryWindowPad(p);  // inventory (Square = pick up / put down)
    spellWindowPad(p);      // Square on a book spell / an action button = pick up
    fxWindowPad(p);  // framexml - Square on a FrameXML frame = its right click
    // Circle with nothing under it (a window's title band) closes that window.
    if ((p.released & PSP_CTRL_CIRCLE) && !winFocus_.activatedAlt() && closeTitleUnderCursor()) return;
    // Circle = the right click on what the focus is on.
    if (const uint32_t alt = winFocus_.activatedAlt()) {
        if (partyAltAction(alt)) return;  // a unit frame - its menu (UnitPopup)
        if (petAltAction(alt)) return;  // TogglePetAutocast, PetFrame's menu
        if (classBarAltAction(alt)) return;  // DestroyTotem, a totem slot's flyout, eject
        if (raidBgAltAction(alt)) return;  // a raid member's menu, the pullouts, the battleground
        if (socialAltAction(alt)) return;  // a friend / who / guild row - the FRIEND menu
        if (ggbAltAction(alt)) return;  // glyph sockets, vault slots, a bag item into the vault
        if (bankMailAltAction(alt)) return;  // bank / mail slots; a bag item at the bank / to the letter
        if (auctionAltAction(alt)) return;  // a bag item into the sell slot (the auctions tab shown)
        if (tradeSkillAltAction(alt)) return;  // a recipe - select and CREATE
        if (tradeDuelAltAction(alt)) return;  // our trade slot - ClickTradeButton(i, "RightButton")
        if (wotlkAltAction(alt)) return;  // the LFG eye's menu, the LFD / achievement / calendar frames
        if (gameObjectAltAction(alt)) return;  // ItemTextFrame's text (a box back)
        if (inventoryAltAction(alt)) return;
        if (merchantWindowAction(alt)) return;  // a merchant item: buy (BuyMerchantItem, as the left click)
        if (lootWindowAction(alt)) return;      // a loot slot: take it
        if (trainerAltAction(alt)) return;      // a trainer service - select and learn it
        if (mapWindowAltAction(alt)) return;    // the map - zoom out (WorldMapButton RightButton)
        if (alt >= 0x5C80 && alt < 0x5C8C) spellWindowAction(alt);  // a book spell - CastSpell
        return;
    }
    const uint32_t act = winFocus_.activated();
    if (!act) return;
    if (partyWindowAction(act)) return;  // unit frames, the unit menu, PARTY_INVITE
    if (petWindowAction(act)) return;  // PetFrame, PetActionBar, PetStableFrame, the pet popups / tabs
    if (classBarWindowAction(act)) return;  // the class bars, VehicleMenuBar, the flyout
    if (inspectAction(act)) return;  // InspectFrame
    if (gameObjectWindowAction(act)) return;  // ItemTextFrame, BarberShopFrame
    if (tradeDuelAction(act)) return;  // TradeFrame, the duel / summon / quest / bind popups
    if (raidBgAction(act)) return;  // RaidFrame, ReadyCheckFrame, BattlefieldFrame, the score...
    if (socialAction(act)) return;  // FriendsFrame (friends, ignore, who, guild), GUILD_INVITE, confirms
    if (ggbAction(act)) return;  // GlyphFrame, GuildBankFrame, ChannelFrame, their popups
    if (bankMailAction(act)) return;  // BankFrame, MailFrame, OpenMailFrame, their popups
    if (auctionAction(act)) return;  // AuctionFrame, its popups and dropdown list
    if (tradeSkillAction(act)) return;  // TradeSkillFrame
    // LFDParentFrame, its popups, AchievementFrame, CalendarFrame (+ the minimap's calendar
    // button: ToggleCalendar, then minimapAction's flash off).
    if (wotlkWindowAction(act)) return;
    if (combatWindowAction(act)) return;  // combat
    if (spellWindowAction(act)) return;   // spells: book, action buttons, buffs
    if (lootWindowAction(act)) return;  // loot
    if (dressUpAction(act)) return;  // DressUpFrame, the character window's check boxes
    if (paperDollAction(act)) return;  // CharacterFrame
    if (watchAction(act)) return;  // WatchFrame collapse / expand
    if (inventoryWindowAction(act)) return;  // inventory
    if (lightingWindowAction(act)) return;  // lighting
    auto logSet = [&](const char* key, int value) {
        PSP_LOGI(pp::tag::Ctrl, "debug_set %s=%d", key, value);
    };
    if (chat_.activate(act)) return;  // chat ("Chat" on the bar, history scroll)
    if (merchantWindowAction(act)) return;  // MerchantFrame
    if (questLogWindowAction(act)) return;  // QuestLogFrame
    if (npcWindowAction(act)) return;  // npc (NPC windows, QUEST_LOG)
    if (soundDebugAction(act)) return;  // sounds
    if (trainerWindowAction(act)) return;  // trainer, talents, spell book tabs
    if (tutorialWindowAction(act)) return;  // tutorials (window, alert, Menu -> Tutorials)
    if (minimapAction(act)) return;  // MinimapCluster buttons (zoom sounds, tracking, calendar)
    if (mapWindowAction(act)) return;  // map (Menu -> Map, the map window, minimap zoom)
    if (taxiWindowAction(act)) return;  // TaxiFrame (a node: TakeTaxiNode, the close button)
    if (instancePopupAction(act)) return;  // StaticPopup INSTANCE_LOCK
    if (deleteItemAction(act)) return;  // StaticPopup DELETE_ITEM / DELETE_GOOD_ITEM
    if (gmWindowAction(act)) return;  // the GM window (gmtools=1)
    if (itemUseWindowAction(act)) return;  // StaticPopup USE_BIND
    switch (act) {
        case kIdBarMenu:
            if (windowOpen(kWinMenu)) closeWindow(kWinMenu);
            else openWindow(kWinMenu);
            break;
        case kIdBarDebug:
            if (windowOpen(kWinDebug)) closeWindow(kWinDebug);
            else openWindow(kWinDebug);
            break;
        case kIdMenuGraphics: settings_.open(liveSettings()); break;
        case kIdMenuExit:
            PSP_LOGI(pp::tag::Exit, "reason=menu_exit");
            pp::systemRequestExit();
            break;
        case kIdMenuClose: closeWindow(kWinMenu); break;
        case kIdDbgOverlay:
            prof::toggleOverlay();
            logSet("overlay", prof::overlayOn() ? 1 : 0);
            break;
        case kIdDbgCapture:
            prof::requestCapture(300);
            logSet("capture", 300);
            break;
        case kIdDbgModels:
            modelsOn_ = !modelsOn_;
            logSet("models", modelsOn_ ? 1 : 0);
            break;
        case kIdDbgTerrain:
            terrain_.debugMode = (terrain_.debugMode + 1) % 4;
            nextStatsMs_ = pp::logUptimeMs() + 3000;
            logSet("terrain_mode", terrain_.debugMode);
            break;
        case kIdDbgCharAnim:
            charAnimated_ = !charAnimated_;
            loadCharacter();
            logSet("char_anim", charAnimated_ ? 1 : 0);
            break;
        case kIdDbgNpcs:
            creaturesOn_ = !creaturesOn_;
            logSet("npcs", creaturesOn_ ? 1 : 0);
            break;
        case kIdDbgFill:
            fillTest_ = !fillTest_;
            logSet("fill_test", fillTest_ ? 1 : 0);
            break;
        case kIdDbgBlend: {
            int& b = rendering::psp::CharacterModel::blendPercent;
            b = b <= 0 ? 50 : b <= 50 ? 100 : b <= 100 ? 150 : 0;
            logSet("anim_blend", b);
            break;
        }
        case kIdDbgClose: closeWindow(kWinDebug); break;
        // forest LOD
        case kIdDbgTreeLod:
            models_.forest.treeLod = (models_.forest.treeLod + 1) % 3;
            nextStatsMs_ = pp::logUptimeMs() + 3000;
            logSet("treelod", models_.forest.treeLod);
            break;
        case kIdDbgLod2Dist:
            models_.forest.lod2Distance = nextStep(models_.forest.lod2Distance, {30, 50, 70, 90, 120});
            logSet("treelod2dist", static_cast<int>(models_.forest.lod2Distance));
            break;
        case kIdDbgImpostors:
            models_.forest.impostors = (models_.forest.impostors + 1) % 4;
            nextStatsMs_ = pp::logUptimeMs() + 3000;
            logSet("impostors", models_.forest.impostors);
            break;
        case kIdDbgImpDist:
            models_.forest.impostorDistance = nextStep(models_.forest.impostorDistance, {60, 90, 120, 160, 200});
            logSet("impostordist", static_cast<int>(models_.forest.impostorDistance));
            break;

        case kIdDbgNpcAnim:
            creatures_.setAnimate(!creatures_.animate());
            logSet("npcanim", creatures_.animate() ? 1 : 0);
            break;
        case kIdDbgHudLines:  // the debug numbers (build, fps, tiles...) under the frames
            hudLines_ = !hudLines_;
            logSet("hudlines", hudLines_ ? 1 : 0);
            break;
        case kIdDbgDrop:  // test of the disconnect path (as a server kick)
            if (pp::devToolsAllowed()) session_.debugDropConnection();  // PSP: a dev server's test only
            else PSP_LOGW(pp::tag::Net, "debug_drop_refused public=1");
            break;
        case kIdDbgAttach:
            rendering::psp::setAttachmentsEnabled(!rendering::psp::attachmentsEnabled());
            logSet("attachments", rendering::psp::attachmentsEnabled() ? 1 : 0);
            break;
        default: break;
    }
}

// Windows move. R held + stick: the window under the cursor
// (topmost first) follows the cursor; release R to drop it - the place is
// saved in wowee.cfg as winxy_<id>=x,y (the top-left on the screen;
// the build-84 offsets winpos_<id> are ignored). The world map (full screen) stays.
// Returns the offset for this frame's draw: the saved place (if any) against the
// window's default bounds of the last frame, clamped so the WHOLE window stays on
// the screen - every frame, not only while dragging.
WorldScreen::WinPos WorldScreen::windowPos(uint8_t w) {
    if (!winPosLoaded_) {
        winPosLoaded_ = true;
        for (int id = 1; id < 256; ++id) {
            char key[16];
            std::snprintf(key, sizeof(key), "winxy_%02x", id);
            const std::string v = pp::configGet(key, "");
            float x = 0, y = 0;
            if (!v.empty() && std::sscanf(v.c_str(), "%f,%f", &x, &y) == 2)
                winPos_[static_cast<uint8_t>(id)] = WinPos{x, y};
        }
    }
    WinPos off{};
    if (w != kWinWorldMap || mapMini()) {  // the mini world map is a window like the others
        auto base = winBase_.find(w);
        if (base != winBase_.end()) {
            const Rect& b = base->second;
            // A panel opened beside others gets its place once its size is known (one frame).
            if (auto pn = std::find(placeNext_.begin(), placeNext_.end(), w); pn != placeNext_.end()) {
                placeNext_.erase(pn);
                placeBeside(w, b);
            }
            const WinPos* saved = nullptr;
            if (auto s = winPos_.find(w); s != winPos_.end()) saved = &s->second;
            else if (auto a = autoPos_.find(w); a != autoPos_.end()) saved = &a->second;  // beside
            if (saved) {
                off.dx = saved->dx - b.x;
                off.dy = saved->dy - b.y;
            }
            const float sw = static_cast<float>(pp::kScreenW), sh = static_cast<float>(pp::kScreenH);
            off.dx = b.w >= sw ? -b.x : std::clamp(off.dx, -b.x, sw - b.x - b.w);
            off.dy = b.h >= sh ? -b.y : std::clamp(off.dy, -b.y, sh - b.y - b.h);
        }
    }
    return off;
}

void WorldScreen::dragWindows(pp::PadState& p, const pp::PadState& pad) {
    const bool rHeld = (pad.held & PSP_CTRL_RTRIGGER) != 0;
    if (rHeld && !dragWin_ && (pad.pressed & PSP_CTRL_RTRIGGER)) {
        const float cx = winFocus_.cursorX(), cy = winFocus_.cursorY();
        for (auto it = winDrawn_.rbegin(); it != winDrawn_.rend(); ++it) {
            auto r = winRect_.find(*it);
            if ((*it != kWinWorldMap || mapMini()) && r != winRect_.end() && r->second.contains(cx, cy)) {
                dragWin_ = *it;
                break;
            }
        }
        if (dragWin_) {
            const Rect& r = winRect_[dragWin_];
            winPos_[dragWin_] = WinPos{r.x, r.y};  // from where it is drawn now
            dragX_ = cx;
            dragY_ = cy;
            PSP_LOGI(pp::tag::Ctrl, "window_drag id=%u", static_cast<unsigned>(dragWin_));
        }
    }
    if (dragWin_ && !rHeld) {
        const WinPos wp = winPos_[dragWin_];
        char key[16], val[32];
        std::snprintf(key, sizeof(key), "winxy_%02x", static_cast<unsigned>(dragWin_));
        std::snprintf(val, sizeof(val), "%.0f,%.0f", wp.dx, wp.dy);
        pp::configSet(key, val);
        pp::configSave();
        PSP_LOGI(pp::tag::Ctrl, "window_drop id=%u pos=%s", static_cast<unsigned>(dragWin_), val);
        dragWin_ = 0;
    }
    if (dragWin_) {
        // The stick moves the cursor; the D-pad would jump it between widgets.
        const uint32_t dpad = PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT;
        p.held &= ~dpad;
        p.pressed &= ~dpad;
    }
}

void WorldScreen::drawWindows(UiRenderer& ui) {
    PROF_SCOPE("ui.windows");
    dressUpTick();  // a closed DressUpFrame / character window gives back its model and art
    beginPopups();  // StaticPopup stack, the dialog art
    updateParty();  // PARTY_INVITE / the unit menu follow the session, the party art (every frame)
    updateRaidBg();  // the ready check, the battleground queues / popups / lines, their art
    updateWotlk();  // the LFG / achievement / calendar events -> windows, popups, alerts
    updateInspect();  // InspectFrame follows its unit (OnEvent / OnUpdate / OnHide)
    updateItemUse();  // the realm's stand state, USE_BIND, the item targeting (every frame)
    updateSocial();  // GUILD_INVITE, the friend sound, the who / guild events, the frames' art
    updateGlyphsGuildBank();  // USE_GLYPH, the vault's events, the channels, their art
    updateTradeDuel();  // TradeFrame and the request popups follow the session (every frame)
    updatePets();  // PET_RENAMEABLE / PET_STABLE_SHOW... -> windows, the keyboard, the pet art
    devtoolsUpdate();  // the PC's devcmd.txt (devcmd=1)
    if (!uiMode_ && windows_.empty()) drawWotlkAlerts(ui);  // the alerts outside the interface
    if (!uiMode_ && windows_.empty()) return;
    std::string line;  // the Debug window's rows - own strings (ps_b7_world_hud.inc DBG_*)
    auto onOff = [](bool b) { return b ? OWN_STR(PORT_ON_LC) : OWN_STR(PORT_OFF_LC); };
    // layers - stack order, the Debug window last (on top: it
    // is read over everything). Debug on the right, Menu on the left:
    // they never cover each other; the chat box and its buttons stay clear.
    uint8_t order[32];
    size_t shown = 0;
    for (uint8_t w : windows_)
        if (w != kWinDebug && shown < 31) order[shown++] = w;
    if (windowOpen(kWinDebug)) order[shown++] = kWinDebug;
    winDrawn_.assign(order, order + shown);
    invTipId_ = 0;  // the windows set them, the tooltip is drawn after them all
    tipProtoItem_ = 0;
    tipSpellId_ = 0;  // the book's
    tipBookTab_ = 0;
    // Each window's stops under the windows drawn over it (their bounds of the last frame) are not
    // declared - every window takes the focus, a covered button does not.
    Rect occl[32];
    auto occludeAbove = [&](size_t from) {
        int n = 0;
        for (size_t k = from; k < shown && n < 32; ++k) {
            auto it = winRect_.find(order[k]);
            if (it != winRect_.end() && it->second.w > 0) occl[n++] = it->second;
        }
        winFocus_.setOccluders(occl, n);
    };
    occludeAbove(0);
    drawWotlkAlerts(ui);  // AchievementAlertFrame / DungeonCompletionAlertFrame (under the windows)
    if (!fullMapOpen()) drawWindowColumn(ui);  // under the windows (hidden with the HUD)
    // Each window at its own offset; its drawn bounds are what R grabs.
    struct WinScope {
        UiRenderer& ui;
        FocusRing& focus;
        std::unordered_map<uint8_t, Rect>& rects;
        std::unordered_map<uint8_t, Rect>& bases;
        uint8_t w;
        WinPos off;
        WinScope(UiRenderer& u, FocusRing& f, std::unordered_map<uint8_t, Rect>& r,
                 std::unordered_map<uint8_t, Rect>& b, uint8_t id, WinPos p)
            : ui(u), focus(f), rects(r), bases(b), w(id), off(p) {
            ui.setOffset(p.dx, p.dy);
            focus.setOffset(p.dx, p.dy);
            ui.boundsBegin();
        }
        ~WinScope() {
            const Rect r = ui.boundsEnd();
            rects[w] = r;
            // The default place (no offset), what windowPos() clamps.
            if (r.w > 0.0f && r.h > 0.0f) bases[w] = Rect{r.x - off.dx, r.y - off.dy, r.w, r.h};
            ui.setOffset(0.0f, 0.0f);
            focus.setOffset(0.0f, 0.0f);
        }
    };
    for (size_t wi = 0; wi < shown; ++wi) {
        const uint8_t w = order[wi];
        occludeAbove(wi + 1);
        // The window hides what was declared before it (HUD, chat, the windows under it) from the
        // cursor - its bounds of the last frame, in screen coordinates (no offset yet).
        if (auto br = winRect_.find(w); br != winRect_.end()) winFocus_.addBlocker(br->second);
        WinScope scope(ui, winFocus_, winRect_, winBase_, w, windowPos(w));
        if (drawCombatWindow(ui, w)) continue;  // combat
        if (drawSpellWindow(ui, w)) continue;   // spells: the spell book
        if (drawLootWindow(ui, w)) continue;  // loot
        if (drawNpcWindow(ui, w)) continue;  // npc
        if (drawInventoryWindow(ui, w)) continue;  // inventory
        if (drawDressUpWindow(ui, w)) continue;  // DressUpFrame
        if (drawLightingWindow(ui, w)) continue;  // lighting
        if (drawTrainerWindow(ui, w)) continue;  // trainer, talents
        if (drawTradeSkillWindow(ui, w)) continue;  // TradeSkillFrame
        if (drawTutorialWindow(ui, w)) continue;  // tutorials
        if (drawMapWindow(ui, w)) continue;  // map
        if (drawMinimapWindow(ui, w)) continue;  // MiniMapTrackingDropDown
        if (drawQuestAbandon(ui, w)) continue;  // StaticPopup ABANDON_QUEST
        if (drawUnlearnSkillPopup(ui, w)) continue;  // StaticPopup UNLEARN_SKILL
        if (drawPartyWindow(ui, w)) continue;  // UnitPopup menu, StaticPopup PARTY_INVITE
        if (drawTaxiWindow(ui, w)) continue;  // TaxiFrame
        if (drawBankMailWindow(ui, w)) continue;  // BankFrame, MailFrame, OpenMailFrame, popups
        if (drawAuctionWindow(ui, w)) continue;  // AuctionFrame, its popups and dropdown list
        if (drawInstancePopup(ui, w)) continue;  // StaticPopup INSTANCE_BOOT / INSTANCE_LOCK
        if (drawDeleteItemPopup(ui, w)) continue;  // StaticPopup DELETE_ITEM / DELETE_GOOD_ITEM
        if (drawGmWindow(ui, w)) continue;  // the GM window (gmtools=1)
        if (drawRaidBgWindow(ui, w)) continue;  // the raid / battleground frames and popups
        if (drawWotlkWindow(ui, w)) continue;  // LFD, its popups, achievements, calendar
        if (drawInspectWindow(ui, w)) continue;  // InspectFrame
        if (drawGameObjectWindow(ui, w)) continue;  // ItemTextFrame, BarberShopFrame
        if (drawItemUseWindow(ui, w)) continue;  // StaticPopup USE_BIND
        if (drawSocialWindow(ui, w)) continue;  // FriendsFrame, GUILD_INVITE, the confirms, the lists
        if (drawGgbWindow(ui, w)) continue;  // GuildBankFrame, the glyph / vault / channel popups
        if (drawTradeDuelWindow(ui, w)) continue;  // TradeFrame and the request popups
        if (drawPetWindow(ui, w)) continue;  // PetStableFrame, RENAME_PET, the pet popups
        if (drawClassBarWindow(ui, w)) continue;  // MultiCastFlyoutFrame
        if (w == kWinMenu) {
            drawGameMenu(ui);  // GameMenuFrame (was the 2 x 3 grid on the generic panel)
        } else if (w == kWinDebug) {
            // Debug switches (not saved). View distances, floor, prediction and AFK
            // live in the graphics settings (Menu -> Graphics, saved in wowee.cfg).
            // right side (B), 8 rows at a 23 px pitch so the window
            // ends above the window bar (y 228) with the sound row (F) added.
            constexpr float kDbgPitch = 23.0f;
            // left of the window-button column (right edge).
            const Rect r{pp::kScreenW - 312.0f - 82, 4, 312, 30 + 8 * kDbgPitch + 4};
            kit_.panel(ui, r);
            ui.textShadow(kit_.font(kFontNormal), r.x + r.w * 0.5f, r.y + 12, OWN_STR(DBG_TITLE), kColGold, Align::Center);
            drawLightingDebugButton(ui, r.x + r.w - 62, r.y + 4);  // lighting (the "Light" window)
            // Debug numbers on screen (off by default), title row between "Loot" and "Light".
            kit_.button(ui, winFocus_, kIdDbgHudLines, Rect{r.x + r.w - 124, r.y + 4, 58, 20}, hudLines_ ? OWN_STR(DBG_NUMBERS_PLUS) : OWN_STR(DBG_NUMBERS));
            // drops the world connection the way a server kick does (left of the title).
            kit_.button(ui, winFocus_, kIdDbgDrop, Rect{r.x + 68, r.y + 4, 58, 20}, OWN_STR(DBG_DROP));
            drawGmDebugButton(ui, r.x + 8, r.y + 4);  // "GM" (gmtools=1)
            const float c0 = r.x + 12, c1 = r.x + r.w * 0.5f + 2, bw = r.w * 0.5f - 14, bh = 21;
            float y = r.y + 30;
            line = OWN_FMT(DBG_PROFILER, onOff(prof::overlayOn()));
            kit_.button(ui, winFocus_, kIdDbgOverlay, Rect{c0, y, bw, bh}, line.c_str());
            kit_.button(ui, winFocus_, kIdDbgCapture, Rect{c1, y, bw, bh},
                        prof::capturing() ? OWN_STR(DBG_CAPTURING) : OWN_STR(DBG_CAPTURE), !prof::capturing());
            y += kDbgPitch;
            line = OWN_FMT(DBG_MODELS, onOff(modelsOn_));
            kit_.button(ui, winFocus_, kIdDbgModels, Rect{c0, y, bw, bh}, line.c_str());
            line = OWN_FMT(DBG_NPCS, onOff(creaturesOn_));
            kit_.button(ui, winFocus_, kIdDbgNpcs, Rect{c1, y, bw, bh}, line.c_str());
            y += kDbgPitch;
            line = OWN_FMT(DBG_TERRAIN, terrainModeName(terrain_.debugMode));
            kit_.button(ui, winFocus_, kIdDbgTerrain, Rect{c0, y, bw, bh}, line.c_str());
            line = OWN_FMT(DBG_CHARACTER, charAnimated_ ? OWN_STR(DBG_ANIMATED) : OWN_STR(DBG_STATIC));
            kit_.button(ui, winFocus_, kIdDbgCharAnim, Rect{c1, y, bw, bh}, line.c_str());
            y += kDbgPitch;
            line = OWN_FMT(DBG_FILL_TEST, onOff(fillTest_));
            kit_.button(ui, winFocus_, kIdDbgFill, Rect{c0, y, bw, bh}, line.c_str());
            line = OWN_FMT(DBG_BLEND, rendering::psp::CharacterModel::blendPercent);
            kit_.button(ui, winFocus_, kIdDbgBlend, Rect{c1, y, bw, bh}, line.c_str());
            // (no CLOSE row: Circle closes; the window has to clear the bar)
            // forest LOD
            const auto& fl = models_.forest;
            const auto& ms = models_.stats();
            y += kDbgPitch;
            line = OWN_FMT(DBG_TREE_LOD, fl.treeLod, static_cast<unsigned>(ms.lod2));
            kit_.button(ui, winFocus_, kIdDbgTreeLod, Rect{c0, y, bw, bh}, line.c_str());
            line = OWN_FMT(DBG_LOD2_FROM, static_cast<double>(fl.lod2Distance));
            kit_.button(ui, winFocus_, kIdDbgLod2Dist, Rect{c1, y, bw, bh}, line.c_str());
            y += kDbgPitch;
            line = OWN_FMT(DBG_IMPOSTORS, impostorModeText(fl.impostors), static_cast<unsigned>(ms.impostors));
            kit_.button(ui, winFocus_, kIdDbgImpostors, Rect{c0, y, bw, bh}, line.c_str());
            line = OWN_FMT(DBG_IMP_FROM, static_cast<double>(fl.impostorDistance));
            kit_.button(ui, winFocus_, kIdDbgImpDist, Rect{c1, y, bw, bh}, line.c_str());

            y += kDbgPitch;
            line = OWN_FMT(DBG_NPC_ANIM, onOff(creatures_.animate()));
            kit_.button(ui, winFocus_, kIdDbgNpcAnim, Rect{c0, y, bw, bh}, line.c_str());
            line = OWN_FMT(DBG_ITEMS, onOff(rendering::psp::attachmentsEnabled()));
            kit_.button(ui, winFocus_, kIdDbgAttach, Rect{c1, y, bw, bh}, line.c_str());
            drawLootDebugRow(ui, r);  // loot - "Loot" in the title row
            drawSoundDebugRow(ui, c0, c1, bw, bh, y + kDbgPitch);  // sounds
        }
    }
    winFocus_.setOccluders(nullptr, 0);
    dressUpHover();  // the item under the cursor (L + Triangle), a chat link's GameTooltip
    // The item tooltip over every window, not part of any window's bounds.
    if (invTipId_ != 0) {
        if (const game::ItemDef* item = inventoryItemAt(invTipId_)) drawItemTooltip(ui, *item, invTipRect_);
    } else if (tipProtoItem_ != 0) {
        const auto* info = session_.getItemInfo(tipProtoItem_);
        if (info && info->valid) drawItemTooltip(ui, session_.protoItemDef(tipProtoItem_, tipProtoCount_), tipProtoRect_);
    }
    drawPetOverlay(ui);  // the pet tooltips (a pet spell's through drawSpellTooltips), the cursor's icon
    drawClassBarOverlay(ui);  // the bars' tooltips (a spell's through drawSpellTooltips)
    drawSpellCursor(ui);  // the book's tooltip, the spell on the cursor
    drawSpellTooltips(ui);  // GameTooltip of a book / trainer / action bar spell
    drawItemUseOverlay(ui);  // the targeting cursor of an item spell
    drawTrainerOverlay(ui);  // the talent tooltip, the red line over the trainer / talent frames
    drawCharTabsOverlay(ui);  // the Reputation / Skills tabs' tooltip
    drawTradeSkillOverlay(ui);  // UIErrorsFrame over the frame, the check box's tooltip
    drawXpTooltip(ui);  // ExhaustionToolTipText (the XP bar / ExhaustionTick under the cursor)
    drawMinimapTooltip(ui);  // the minimap cluster's GameTooltip
    drawTaxiTooltip(ui);  // TaxiNodeOnButtonEnter's GameTooltip
    drawBankMailOverlay(ui);  // the bank / mail tooltips
    drawAuctionOverlay(ui);  // AuctionFrame's one-line tooltips
    drawRaidBgOverlay(ui);  // the raid / battleground / world state tooltips
    drawWotlkOverlay(ui);  // their GameTooltip, LFDSearchStatus
    drawInspectOverlay(ui);  // InspectFrame's GameTooltip (an inspected item, a slot, a tab)
    drawTradeDuelOverlay(ui);  // the trade items' GameTooltip
    drawDressUpOverlay(ui);  // the character window's check boxes' GameTooltip
    drawSocialOverlay(ui);  // FriendsFrame's GameTooltip (a friend, the tabs, the guild buttons)
    drawGgbOverlay(ui);  // the glyph tooltip, the cursor's glyph / vault item
    if (!uiMode_ || settings_.isOpen()) return;
    kit_.cursor(ui, winFocus_);
}

// The game menu of the original - GameMenuFrame.xml ("every window 1:1 on the original
// art"). UI-DialogBox frame with the header and MAIN_MENU, one column of GameMenuButtonTemplate
// buttons, centred (anchor CENTER). The original's buttons with a PSP equivalent keep their strings
// (VIDEOOPTIONS_MENU, EXIT_GAME, RETURN_TO_GAME); the PSP has no micro buttons / key bindings, so the
// panels they open are buttons here, named as the original names them (CHARACTER, TALENTS, QUEST_LOG,
// WORLD_MAP), plus the bags and the tutorials (PSP). Absent on the PSP: SOUNDOPTIONS_MENU / UIOPTIONS_MENU (one
// settings dialog: VIDEOOPTIONS_MENU), KEY_BINDINGS, MACROS, LOGOUT (no logout).
// Nine buttons of 21 units, 1 apart, RETURN_TO_GAME 16 under them: 262 units = 210 px at 0.8 - one column fits.
// + SOCIAL_BUTTON (FriendsFrame's Raid tab) before EXIT_GAME - 284 units = 227 px.
void WorldScreen::drawGameMenu(UiRenderer& ui) {
    constexpr float S = kDialogScale;
    auto U = [](float units) { return std::floor(units * S + 0.5f); };
    // above RETURN_TO_GAME: SOCIAL_BUTTON, achievements, the dungeon finder
    constexpr int kRows = 11;
    const float w = U(195), h = U(26.5f + kRows * 22.0f - 1.0f + 16.0f + 21.0f + 23.5f);
    const Rect r{std::floor((pp::kScreenW - w) * 0.5f), std::floor((pp::kScreenH - h) * 0.5f), w, h};
    drawDialogBox(ui, r);
    drawDialogHeader(ui, r, CLIENT_STR(MAIN_MENU));
    const bool focusable = uiMode_ && !settings_.isOpen() && !windowOpen(kWinBindConfirm);
    // GameMenuButtonOptions: CENTER at TOP 0,-37 (top 26.5); the next ones TOP to BOTTOM 0,-1.
    const float bw = U(144), bh = U(21), bx = r.x + std::floor((w - bw) * 0.5f);
    auto row = [&](int k) { return Rect{bx, r.y + U(26.5f + static_cast<float>(k) * 22.0f), bw, bh}; };
    drawGameMenuButton(ui, kIdMenuGraphics, row(0), CLIENT_STR(VIDEOOPTIONS_MENU), focusable);  // PSP: all settings
    drawInventoryMenuRow(ui, row(1), row(4), focusable);                         // CHARACTER / the bags (PSP)
    drawGameMenuButton(ui, kIdMenuTalents, row(2), CLIENT_STR(TALENTS), focusable);
    drawGameMenuButton(ui, kIdNpcQuestLog, row(3), CLIENT_STR(QUEST_LOG), focusable);
    drawGameMenuButton(ui, kIdMenuMap, row(5), CLIENT_STR(WORLD_MAP), focusable);
    drawTutorialMenuButton(ui, row(6), focusable);                               // the tutorials
    drawRaidMenuButton(ui, row(7), focusable);  // SOCIAL_BUTTON (ToggleFriendsFrame())
    drawWotlkMenuButtons(ui, row(8), row(9), focusable);  // ACHIEVEMENT_BUTTON, DUNGEONS_BUTTON
    drawGameMenuButton(ui, kIdMenuExit, row(kRows - 1), CLIENT_STR(EXIT_GAME), focusable);
    // GameMenuButtonContinue: TOP to GameMenuButtonQuit's BOTTOM 0,-16.
    const Rect back{bx, r.y + U(26.5f + (kRows - 1) * 22.0f + 21.0f + 16.0f), bw, bh};
    drawGameMenuButton(ui, kIdMenuClose, back, CLIENT_STR(RETURN_TO_GAME), focusable);
}

// The window buttons: a column at the right edge, under the minimap and above
// the action bar (a row covered the hints); so that they do not get in
// the way, drawn UNDER the windows; a button a window covers
// (its bounds of the last frame) takes no focus - not drawn, not clickable.
// Only "Book", right of the action bar (under the WatchFrame); "Chat" -> a click on
// the chat box, "Menu" -> START, "Debug" only with wowee.cfg debugbutton=1 (above "Book").
void WorldScreen::drawWindowColumn(UiRenderer& ui) {
    if (!uiMode_ || settings_.isOpen()) return;
    const float bw = 64, bh = 20, bx = pp::kScreenW - bw - 4, gap = 2;
    const float by = pp::kScreenH - bh - 4;
    auto covered = [&](const Rect& b) {
        for (uint8_t w : windows_) {
            auto it = winRect_.find(w);
            if (it == winRect_.end()) continue;
            const Rect& r = it->second;
            if (r.x < b.x + b.w && b.x < r.x + r.w && r.y < b.y + b.h && b.y < r.y + r.h) return true;
        }
        return false;
    };
    auto button = [&](uint32_t id, const Rect& r, const char* label) {
        kit_.button(ui, winFocus_, id, r, label, !covered(r));
    };
    button(0x5C01 /* spells: kIdBarBook */, Rect{bx, by, bw, bh}, OWN_STR(WIN_BOOK));
    static const bool debugButton = pp::configGetInt("debugbutton", 0) != 0;
    if (debugButton) button(kIdBarDebug, Rect{bx, by - bh - gap, bw, bh}, OWN_STR(DBG_TITLE));
}

} // namespace wowee::ui::psp
