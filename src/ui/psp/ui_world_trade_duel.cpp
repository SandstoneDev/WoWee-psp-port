// TradeFrame (the public TradeFrame.xml / .lua) and the StaticPopups the trade, the duels and the server's requests
// show (StaticPopup.lua: TRADE, DUEL_REQUESTED, DUEL_OUTOFBOUNDS, CONFIRM_SUMMON, QUEST_ACCEPT, QUEST_ACCEPT_LOG_FULL,
// TRADE_POTENTIAL_BIND_ENCHANT; UIParent.lua shows / hides them on the events) over the session's half
// (psp_world_session_trade_duel.cpp).
//
// TradeFrame (384 x 512 units, drawn x 0..360 y 0..462): the composite "trade_frame" of tools/psp/build_ui_pak.py
// (UI-TradeFrame quarters, the 7 + 7 slots - UI-EmptySlot, UI-Quickslot2, UI-QuestItemNameFrame -, the
// UI-TradeFrame-EnchantIcon of the 7th, the MoneyInputFrame boxes with their coins) at 0.6 x 0.55 = 216 x 254 px
// (one 256 x 256 T8, 65 KB) + "trade_hl" (UI-TradeFrame-Highlight, 4444): both only while the trade is open.
// Dynamic: the two portraits (the race / sex portraits of the unit frames, round), the names (GameFontNormal), the
// items (ItemButtonTemplate: icon, count; the name GameFontNormalSmall; the other side's not usable ones red), the 7th
// slot's TRADEFRAME_ENCHANT_SLOT_LABEL and its text (the enchantment green /
// TRADEFRAME_NOT_MODIFIED_TEXT white), our gold in the money boxes, theirs in a SmallMoneyFrame, the green
// highlights of an accepted side (TradeFrame_SetAcceptState), TRADE (off while we accepted or the bind popup
// is up), CANCEL (CancelTradeAccept when we accepted, else HideUIPanel -> CloseTrade), the close button.
// Controls (PSP, docs/PSP_CONTROLS.md): Cross on our slot = ClickTradeButton (the bag item held with Square / Cross
// in the bags goes there; nothing held: the item comes off the table onto the "cursor"), Circle on it =
// ClickTradeButton(i, "RightButton") (off the table); Circle on a bag item while trading = UseContainerItem (the
// first free slot; ui_world_inventory.cpp); Cross on a money box = the PSP keyboard (digits) -> SetTradeMoney;
// the focused slot shows the item's GameTooltip (theirs: the item's; the empty 7th: ENCHANT_SLOT's newbie tip).
// Window 0x22 (TradeFrame), popups 0x23..0x27 and 0x2C (TRADE); widgets 0x8700..0x87FF.
#include "ui/psp/ui_world_screen.hpp"

#include "core/coordinates.hpp"
#include "game/inventory_slots.hpp"
#include "game/update_field_table.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_osk.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_spell_db.hpp"
#include "platform/psp/psp_strings.hpp"
#include "platform/psp/psp_texture.hpp"
#include "ui/psp/ui_world_sounds.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <glm/glm.hpp>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
namespace td = wowee::platform::psp::tradeduel;

namespace {

enum : uint32_t {
    kIdTdMine0 = 0x8700,    // ..0x8706: our slots
    kIdTdTheirs0 = 0x8710,  // ..0x8716: theirs
    kIdTdGold = 0x8720,
    kIdTdSilver,
    kIdTdCopper,
    kIdTdTrade = 0x8730,
    kIdTdCancel,
    kIdTdClose,
    kIdTdDuelAccept = 0x8740,
    kIdTdDuelDecline,
    kIdTdSummonAccept = 0x8750,
    kIdTdSummonCancel,
    kIdTdQuestYes = 0x8760,
    kIdTdQuestNo,
    kIdTdBindOk = 0x8770,
    kIdTdBindCancel,
    kIdTdRequestYes = 0x8780,
    kIdTdRequestNo,
};

constexpr uint32_t kTradeRequestMs = 60000u;  // StaticPopup TRADE timeout = 60 (public StaticPopup.lua)

constexpr int kOskOwner = 0x5444;  // "TD": the money boxes' keyboard (psp_osk oskBeginFor)

// TradeFrame units -> px (tools/psp/build_ui_pak.py TRADE_*): 0.6 x 0.55, TOPLEFT 0,-104 -> the left, at the top.
constexpr float kSX = 0.6f, kSY = 0.55f;
constexpr float kOx = 4.0f, kOy = 2.0f;
constexpr float kFrameW = 216.0f, kFrameH = 254.0f;
float PX(float x) { return kOx + x * kSX; }
float PY(float y) { return kOy + y * kSY; }
constexpr float kIcon = 21.0f;  // ItemButton 37 units, square on the PSP (as the merchant's)

// TradePlayerItem1 TOPLEFT 26,-104 / TradeRecipientItem1 195,-104; each 7 under the last (37 high), the 7th 28.
float itemX(int side) { return side ? 195.0f : 26.0f; }
float itemY(int slot) { return 104.0f + static_cast<float>(std::min(slot, 5)) * 44.0f + (slot == 6 ? 65.0f : 0.0f); }

constexpr uint32_t kColGreenCode = 0xFF20FF20u;  // GREEN_FONT_COLOR_CODE
constexpr uint32_t kColItemRed = 0xFF0000E6u;    // SetItemButtonTextureVertexColor(0.9, 0, 0)

}  // namespace

struct WorldScreen::TradeDuelUi {
    pp::Texture frame, hl;
    bool artTried = false;
    uint32_t artBytes = 0;
    bool shown = false;                 // TradeFrame up (TRADE_SHOW .. TRADE_CLOSED / our close)
    uint32_t tradeSeq = 0, bindSeq = 0;
    int oskField = 0;                   // the money box the keyboard is for
    bool bindShown = false;
    // DUEL_REQUESTED (timeout 60), DUEL_OUTOFBOUNDS (timeout 10)
    bool duelShown = false, duelAnswered = false, boundsShown = false;
    uint32_t duelSeq = 0, boundsSeq = 0, finishedSeq = 0, duelAtMs = 0, boundsAtMs = 0;
    // CONFIRM_SUMMON
    bool summonShown = false;
    uint32_t summonSeq = 0, areaZone = 0;
    std::string areaName;
    // QUEST_ACCEPT / QUEST_ACCEPT_LOG_FULL
    bool questShown = false;
    uint32_t questSeq = 0;
    // TRADE (somebody asks us to trade)
    bool requestShown = false;
    uint32_t requestSeq = 0, requestAtMs = 0;
    bool lastUi = false;
    // the focused trade slot's tooltip (drawn after the windows)
    int tipSide = -1, tipSlot = -1;
    Rect tipRect{};
    uint32_t logs = 0;
    void loadArt();
    void releaseArt() {
        for (pp::Texture* t : {&frame, &hl})
            if (t->pixels) pp::textureRelease(*t);
        if (artTried) PSP_LOGI(pp::tag::Gfx, "trade_art_free kb=%u", static_cast<unsigned>(artBytes / 1024u));
        artTried = false;
        artBytes = 0;
    }
    ~TradeDuelUi() { releaseArt(); }
};

void WorldScreen::destroyTradeDuelUi(TradeDuelUi* p) { delete p; }

WorldScreen::TradeDuelUi& WorldScreen::tradeDuelUi() {
    if (!tradeDuel_)
        tradeDuel_ = std::unique_ptr<TradeDuelUi, void (*)(TradeDuelUi*)>(new TradeDuelUi, &WorldScreen::destroyTradeDuelUi);
    return *tradeDuel_;
}

void WorldScreen::TradeDuelUi::loadArt() {
    artTried = true;
    char path[256];
    pp::fsPath(path, sizeof(path), "data/ui.pak");
    pp::Pak pak;
    int ok = 0;
    if (pak.open(path)) {  // opened, read, closed at once (the card's file descriptors)
        auto load = [&](const char* name, pp::Texture& t) {
            const pp::PakEntry* e = pak.find(name);
            void* blob = e ? pak.load(e) : nullptr;
            if (!blob) return;
            if (!pp::textureFromBlob(blob, e->size, t, true)) {
                pp::pakFree(blob);
                return;
            }
            t.name = "trade";
            artBytes += e->size;
            ++ok;
        };
        load("trade_frame", frame);
        load("trade_hl", hl);
        pak.close();
    }
    PSP_LOGI(pp::tag::Gfx, "trade_art ok=%d/2 kb=%u", ok, static_cast<unsigned>(artBytes / 1024u));
}

// --- Per frame (drawWindows) ----------------------------------------------------------------------------------------

namespace {

// text.pak "zones" (AreaTable ruRU names): GetSummonConfirmAreaName for one zone - the pak opened, read, closed.
std::string zoneNameFromTextPak(uint32_t zoneId) {
    char path[256];
    pp::fsPath(path, sizeof(path), "data/text.pak");
    pp::Pak pak;
    std::string out;
    if (!pak.open(path)) return out;
    if (const pp::PakEntry* e = pak.find("zones")) {
        if (auto* b = static_cast<const char*>(pak.load(e))) {
            const char *p = b + 4, *end = b + e->size;
            while (p + 4 < end && out.empty()) {
                int32_t k = 0;
                std::memcpy(&k, p, 4);
                p += 4;
                const char* s = p;
                while (p < end && *p) ++p;
                if (k == static_cast<int32_t>(zoneId)) out.assign(s, p);
                ++p;
            }
            pp::pakFree(const_cast<char*>(b));
        }
    }
    pak.close();
    return out;
}

}  // namespace

void WorldScreen::updateTradeDuel() {
    TradeDuelUi& T = tradeDuelUi();
    const uint32_t now = pp::logUptimeMs();
    session_.tradeDuelTick();
    tradeDuelOskTick();
    for (std::string info; session_.takeTradeDuelInfo(info);) uiInfo(info);  // the duel countdown
    const bool alive = session_.life() == pp::WorldSession::Life::Alive;

    // TradeFrame: TRADE_SHOW -> ShowUIPanel (igCharacterInfoOpen), TRADE_CLOSED -> HideUIPanel; our own hide
    // (the close button, Circle on the title, CANCEL) -> TradeFrame_OnHide -> CloseTrade.
    if (session_.tradeOpen() && (!T.shown || session_.tradeSeq() != T.tradeSeq)) {
        if (!T.shown) {
            T.shown = true;
            openWindow(kWinTrade);
            if (uiMode_) winFocus_.setFocus(kIdTdMine0);
            playSoundByName("igCharacterInfoOpen");
            if (!T.artTried) T.loadArt();
            PSP_LOGI(pp::tag::Ctrl, "trade_frame_show peer=%llx", static_cast<unsigned long long>(session_.tradePeer()));
        }
        T.tradeSeq = session_.tradeSeq();
    } else if (T.shown && (!session_.tradeOpen() || !windowOpen(kWinTrade))) {
        const bool ours = session_.tradeOpen();  // still open on the session: the player hid the frame
        T.shown = false;
        T.tradeSeq = session_.tradeSeq();
        if (windowOpen(kWinTrade)) closeWindow(kWinTrade);
        if (ours) session_.tradeClose();
        if (windowOpen(kWinTradeBind)) closeWindow(kWinTradeBind);  // StaticPopup_Hide TRADE_POTENTIAL_BIND_ENCHANT
        T.bindShown = false;
        playSoundByName("igCharacterInfoClose");
        T.releaseArt();
        PSP_LOGI(pp::tag::Ctrl, "trade_frame_hide ours=%d", ours ? 1 : 0);
    }
    // TRADE_POTENTIAL_BIND_ENCHANT(canBecomeBound): StaticPopup_Show / _Hide (noCancelOnReuse).
    if (session_.tradeBindSeq() != T.bindSeq) {
        T.bindSeq = session_.tradeBindSeq();
        if (session_.tradeBindAsk() && T.shown) {
            T.bindShown = true;
            openWindow(kWinTradeBind);
            winFocus_.setFocus(kIdTdBindOk);
        } else if (T.bindShown) {
            T.bindShown = false;
            if (windowOpen(kWinTradeBind)) closeWindow(kWinTradeBind);
        }
    }
    if (T.bindShown && !windowOpen(kWinTradeBind)) {  // START (hideOnEscape): OnCancel = ClickTradeButton(7, true)
        T.bindShown = false;
        session_.tradeClearItem(td::kTradableSlots);
    }

    // TRADE_REQUEST -> StaticPopup_Show("TRADE", arg1): YES = BeginTrade, NO / START (hideOnEscape) / timeout 60 =
    // CancelTrade; not whileDead (StaticPopup_Show calls OnCancel at once). The session hides it when the request ends.
    if (session_.tradeAskSeq() != T.requestSeq) {
        T.requestSeq = session_.tradeAskSeq();
        if (session_.tradeAsked() && !T.requestShown) {
            if (!alive) {
                session_.tradeAnswerRequest(false);
            } else {
                T.requestShown = true;
                T.requestAtMs = now;
                openWindow(kWinTradeRequest);
                winFocus_.setFocus(kIdTdRequestYes);
                if (!playSoundByName("igPlayerInvite")) playUiSound(UiSound::QuestActivate);
                PSP_LOGI(pp::tag::Ctrl, "trade_request_popup");
            }
        }
    }
    if (T.requestShown && (!session_.tradeAsked() || now - T.requestAtMs >= kTradeRequestMs ||
                           !windowOpen(kWinTradeRequest))) {
        T.requestShown = false;
        if (windowOpen(kWinTradeRequest)) closeWindow(kWinTradeRequest);
        if (session_.tradeAsked()) session_.tradeAnswerRequest(false);  // timeout / START: OnCancel
    }

    // DUEL_REQUESTED -> StaticPopup_Show("DUEL_REQUESTED", arg1) (sound igPlayerInvite; not whileDead: OnCancel at
    // once); DUEL_FINISHED hides it; timeout 60 / START (hideOnEscape) -> OnCancel = CancelDuel.
    if (session_.duelRequestSeq() != T.duelSeq) {
        T.duelSeq = session_.duelRequestSeq();
        if (session_.duelRequested()) {
            if (!alive) {
                session_.cancelDuel();
            } else {
                T.duelShown = true;
                T.duelAnswered = false;
                T.duelAtMs = now;
                openWindow(kWinDuelRequest);
                winFocus_.setFocus(kIdTdDuelAccept);
                if (!playSoundByName("igPlayerInvite")) playUiSound(UiSound::QuestActivate);
                PSP_LOGI(pp::tag::Ctrl, "duel_popup");
            }
        }
    }
    if (session_.duelFinishedSeq() != T.finishedSeq) {  // DUEL_FINISHED: StaticPopup_Hide (no OnCancel)
        T.finishedSeq = session_.duelFinishedSeq();
        if (T.duelShown) {
            T.duelShown = false;
            T.duelAnswered = true;
            if (windowOpen(kWinDuelRequest)) closeWindow(kWinDuelRequest);
        }
        if (T.boundsShown) {
            T.boundsShown = false;
            if (windowOpen(kWinDuelBounds)) closeWindow(kWinDuelBounds);
        }
    }
    if (T.duelShown && (now - T.duelAtMs >= 60000u || !windowOpen(kWinDuelRequest))) {
        T.duelShown = false;
        if (windowOpen(kWinDuelRequest)) closeWindow(kWinDuelRequest);
        if (!T.duelAnswered) {
            T.duelAnswered = true;
            session_.cancelDuel();
        }
        PSP_LOGI(pp::tag::Ctrl, "duel_popup_hide");
    }
    // DUEL_OUTOFBOUNDS -> StaticPopup_Show (timeout 10, no buttons, not hideOnEscape), DUEL_INBOUNDS -> _Hide.
    if (session_.duelBoundsSeq() != T.boundsSeq) {
        T.boundsSeq = session_.duelBoundsSeq();
        if (session_.duelOutOfBounds()) {
            T.boundsShown = true;
            T.boundsAtMs = now;
            openWindow(kWinDuelBounds);
        }
    }
    if (T.boundsShown) {
        if (!session_.duelOutOfBounds() || now - T.boundsAtMs >= 10000u) {
            T.boundsShown = false;
            if (windowOpen(kWinDuelBounds)) closeWindow(kWinDuelBounds);
        } else if (!windowOpen(kWinDuelBounds)) {
            openWindow(kWinDuelBounds);  // PSP: START / Circle cannot close it (the dialog has no buttons)
        }
    }

    // CONFIRM_SUMMON / CANCEL_SUMMON (not whileDead; hideOnEscape -> OnCancel = CancelSummon; the time-out: the
    // session's tick sends CancelSummon).
    if (session_.summonSeq() != T.summonSeq) {
        T.summonSeq = session_.summonSeq();
        if (session_.summonPending()) {
            if (!alive) {
                session_.cancelSummon();
            } else {
                if (T.areaZone != session_.summonZone() || T.areaName.empty()) {
                    T.areaZone = session_.summonZone();
                    T.areaName = zoneNameFromTextPak(T.areaZone);
                }
                T.summonShown = true;
                openWindow(kWinSummon);
                winFocus_.setFocus(kIdTdSummonAccept);
                PSP_LOGI(pp::tag::Ctrl, "summon_popup zone=%u named=%d", static_cast<unsigned>(T.areaZone),
                         T.areaName.empty() ? 0 : 1);
            }
        } else if (T.summonShown) {
            T.summonShown = false;
            if (windowOpen(kWinSummon)) closeWindow(kWinSummon);
        }
    }
    if (T.summonShown && (!session_.summonPending() || !windowOpen(kWinSummon))) {
        T.summonShown = false;
        if (windowOpen(kWinSummon)) closeWindow(kWinSummon);
        if (session_.summonPending()) session_.cancelSummon();  // START: OnCancel
    }

    // QUEST_ACCEPT_CONFIRM -> QUEST_ACCEPT (or _LOG_FULL at MAX_QUESTS) at once, as upstream fires it; the starter's
    // name shows once the name query is answered.
    const auto& qc = session_.questConfirm();
    if (qc.seq != T.questSeq) {
        T.questSeq = qc.seq;
        if (qc.pending && alive) {
            T.questShown = true;
            openWindow(kWinQuestAccept);
            winFocus_.setFocus(kIdTdQuestYes);
            PSP_LOGI(pp::tag::Ctrl, "quest_accept_popup full=%d", session_.questLogFull() ? 1 : 0);
        }
    }
    if (T.questShown && (!qc.pending || !windowOpen(kWinQuestAccept))) {
        T.questShown = false;
        if (windowOpen(kWinQuestAccept)) closeWindow(kWinQuestAccept);
        session_.dismissQuestConfirm();  // NO / START: no OnCancel
    }

    // PSP: the cursor comes up (SELECT) with one of these on top - the focus on its first button.
    if (uiMode_ && !T.lastUi && !windows_.empty()) {
        switch (windows_.back()) {
            case kWinDuelRequest: winFocus_.setFocus(kIdTdDuelAccept); break;
            case kWinSummon: winFocus_.setFocus(kIdTdSummonAccept); break;
            case kWinQuestAccept: winFocus_.setFocus(kIdTdQuestYes); break;
            case kWinTradeBind: winFocus_.setFocus(kIdTdBindOk); break;
            case kWinTradeRequest: winFocus_.setFocus(kIdTdRequestYes); break;
            case kWinTrade: winFocus_.setFocus(kIdTdMine0); break;
            default: break;
        }
    }
    T.lastUi = uiMode_;
}

bool WorldScreen::tradeDuelKeepsPopupArt() const {
    return windowOpen(kWinDuelRequest) || windowOpen(kWinDuelBounds) || windowOpen(kWinSummon) ||
           windowOpen(kWinQuestAccept) || windowOpen(kWinTradeBind) || windowOpen(kWinTradeRequest);
}

// The PSP keyboard's digits for the money box it was opened for: MoneyInputFrame -> TradeFrame_UpdateMoney: more
// than we have (GetMoney) -> back to GetPlayerTradeMoney, else SetTradeMoney.
void WorldScreen::tradeDuelOskTick() {
    if (!tradeDuel_) return;
    TradeDuelUi& T = *tradeDuel_;
    bool accepted = false;
    std::string text;
    if (!pp::oskFinishedFor(kOskOwner, accepted, text)) return;
    const int field = T.oskField;
    T.oskField = 0;
    PSP_LOGI(pp::tag::Ctrl, "trade_osk field=%X ok=%d len=%u", static_cast<unsigned>(field), accepted ? 1 : 0,
             static_cast<unsigned>(text.size()));
    if (!accepted || !session_.tradeOpen()) return;
    uint32_t v = 0;
    const uint32_t max = field == static_cast<int>(kIdTdGold) ? 999999u : 99u;
    for (char c : text) {
        if (c < '0' || c > '9') continue;
        v = std::min(max, v * 10u + static_cast<uint32_t>(c - '0'));
    }
    const uint32_t cur = session_.tradeGold(0);
    uint32_t g = cur / 10000u, s = (cur / 100u) % 100u, c = cur % 100u;
    if (field == static_cast<int>(kIdTdGold)) g = v;
    else if (field == static_cast<int>(kIdTdSilver)) s = v;
    else c = v;
    const uint64_t copper = static_cast<uint64_t>(g) * 10000u + s * 100u + c;
    if (copper > session_.getMoneyCopper() || !session_.tradeSetMoney(static_cast<uint32_t>(copper)))
        PSP_LOGI(pp::tag::Ctrl, "trade_money_refused copper=%llu", static_cast<unsigned long long>(copper));
}

// UnitPopup TRADE (dist 2) / DUEL (dist 3): UnitPopup_OnUpdate - not dead / a ghost (we or the unit), within
// CheckInteractDistance 2 / 3 = 11.11 / 9.9 yd, 3D (upstream lua_unit_api.cpp lua_CheckInteractDistance). Bit 0:
// TRADE enabled, bit 1: DUEL.
uint8_t WorldScreen::tradeDuelMenuMask(uint64_t guid) const {
    if (session_.life() != pp::WorldSession::Life::Alive) return 0;
    UnitView u;
    if (!unitView(guid, u) || u.dead) return 0;
    auto e = session_.entities().getEntity(guid);
    if (!e) return 0;
    const glm::vec3 at = core::coords::canonicalToServer(glm::vec3(e->getX(), e->getY(), e->getZ()));
    const glm::vec3 d = at - glm::vec3(playerX_, playerY_, playerZ_);
    const float d2 = glm::dot(d, d);
    uint8_t m = 0;
    if (d2 <= 11.11f * 11.11f) m |= 1;
    if (d2 <= 9.9f * 9.9f) m |= 2;
    return m;
}

// UseContainerItem while trading (PSP: own rule, there is no drag - after the merchant's branch): the bag item to
// the table.
bool WorldScreen::tradeTakesBagItem(uint32_t invId) {
    if (!session_.tradeOpen()) return false;
    uint8_t bag = 0, slot = 0;
    if (!inventoryIdToWire(invId, bag, slot)) return false;
    session_.tradeAddItem(bag, slot);
    return true;
}

// --- Drawing --------------------------------------------------------------------------------------------------------

bool WorldScreen::drawTradeDuelWindow(UiRenderer& ui, uint8_t w) {
    if ((w < kWinTrade || w > kWinTradeBind) && w != kWinTradeRequest) return false;
    TradeDuelUi& T = tradeDuelUi();
    const bool focusable = uiMode_ && !settings_.isOpen() && !windowOpen(kWinBindConfirm);
    char buf[512];
    switch (w) {
        case kWinTrade: break;
        case kWinTradeRequest: {  // TRADE: TRADE_WITH_QUESTION with the asking player's name; YES / NO
            const std::string text = CLIENT_FMT(TRADE_WITH_QUESTION, session_.tradeAskerName());
            StaticPopup sp;
            sp.text = text.c_str();
            sp.button1 = CLIENT_STR(YES);
            sp.button1Id = kIdTdRequestYes;
            sp.button2 = CLIENT_STR(NO);
            sp.button2Id = kIdTdRequestNo;
            sp.focusable = focusable;
            drawStaticPopup(ui, sp);
            return true;
        }
        case kWinDuelRequest: {  // DUEL_REQUESTED: ACCEPT / DECLINE (the name as it is known now)
            const std::string text = CLIENT_FMT(DUEL_REQUESTED, session_.duelChallengerName());
            StaticPopup sp;
            sp.text = text.c_str();
            sp.button1 = CLIENT_STR(ACCEPT);
            sp.button1Id = kIdTdDuelAccept;
            sp.button2 = CLIENT_STR(DECLINE);
            sp.button2Id = kIdTdDuelDecline;
            sp.focusable = focusable;
            drawStaticPopup(ui, sp);
            return true;
        }
        case kWinDuelBounds: {  // DUEL_OUTOFBOUNDS_TIMER (timeleft < 60: SECONDS)
            const uint32_t left = 10000u - std::min<uint32_t>(10000u, pp::logUptimeMs() - T.boundsAtMs);
            const std::string text = pp::text::plural(
                CLIENT_FMT(DUEL_OUTOFBOUNDS_TIMER, static_cast<int>((left + 999u) / 1000u), CLIENT_STR(SECONDS)));
            StaticPopup sp;
            sp.text = text.c_str();
            sp.focusable = focusable;
            drawStaticPopup(ui, sp);
            return true;
        }
        case kWinSummon: {  // CONFIRM_SUMMON: the summoner, the area, the time left; ACCEPT (off in combat) / CANCEL
            const int left = session_.summonTimeLeftSec();
            const bool minutes = left >= 60;
            const std::string text = pp::text::plural(
                CLIENT_FMT(CONFIRM_SUMMON, session_.summonerName(), T.areaName, minutes ? (left + 59) / 60 : left,
                           minutes ? CLIENT_STR(MINUTES) : CLIENT_STR(SECONDS)));
            StaticPopup sp;
            sp.text = text.c_str();
            sp.button1 = CLIENT_STR(ACCEPT);
            sp.button1Id = kIdTdSummonAccept;
            sp.button1Enabled = session_.summonCanAccept();
            sp.button2 = CLIENT_STR(CANCEL);
            sp.button2Id = kIdTdSummonCancel;
            sp.focusable = focusable;
            drawStaticPopup(ui, sp);
            return true;
        }
        case kWinQuestAccept: {  // QUEST_ACCEPT / QUEST_ACCEPT_LOG_FULL (button1 off while the log is full)
            const auto& qc = session_.questConfirm();
            const bool full = session_.questLogFull();
            const std::string name = session_.questConfirmName();
            const std::string text =
                full ? CLIENT_FMT(QUEST_ACCEPT_LOG_FULL, name, qc.title) : CLIENT_FMT(QUEST_ACCEPT, name, qc.title);
            StaticPopup sp;
            sp.text = text.c_str();
            sp.button1 = CLIENT_STR(YES);
            sp.button1Id = kIdTdQuestYes;
            sp.button1Enabled = !full;
            sp.button2 = CLIENT_STR(NO);
            sp.button2Id = kIdTdQuestNo;
            sp.focusable = focusable;
            drawStaticPopup(ui, sp);
            return true;
        }
        case kWinTradeBind: {  // TRADE_POTENTIAL_BIND_ENCHANT: OKAY / CANCEL (showAlert: PSP - no alert icon)
            StaticPopup sp;
            sp.text = CLIENT_STR(TRADE_POTENTIAL_BIND_ENCHANT);
            sp.button1 = CLIENT_STR(OKAY);
            sp.button1Id = kIdTdBindOk;
            sp.button2 = CLIENT_STR(CANCEL);
            sp.button2Id = kIdTdBindCancel;
            sp.focusable = focusable;
            drawStaticPopup(ui, sp);
            return true;
        }
        default: return false;
    }

    // --- TradeFrame ---
    PROF_SCOPE("trade.frame");
    if (!T.artTried) T.loadArt();
    loadNpcArt(false);  // UIPanelButton, the close button, the coins
    const Font& fs = kit_.font(kFontSmall);
    const Font& fn = kit_.font(kFontNormal);
    const float lh = static_cast<float>(fs.lineHeight());
    const bool top = focusable;
    T.tipSide = T.tipSlot = -1;
    const uint64_t peer = session_.tradePeer();

    // Portraits (60 at 7,-6 / 183,-6) under the frame: SetPortraitTexture - the race / sex portraits, round.
    auto portrait = [&](uint64_t guid, float ux) {
        const Rect pr{PX(ux), PY(6), 60 * kSX, 60 * kSY};
        UnitView u;
        if (unitView(guid, u) && u.race < 12 && u.sex < 2 && ufArt_.portrait[u.race][u.sex].pixels)
            ui.imageCircle(ufArt_.portrait[u.race][u.sex], pr);
        else
            ui.fillCircle(Rect{pr.x + 2, pr.y + 2, pr.w - 4, pr.h - 4}, 0xFF101010u);
    };
    portrait(session_.playerGuid(), 7);
    portrait(peer, 183);
    if (T.frame.pixels) ui.image(T.frame, Rect{kOx, kOy, kFrameW, kFrameH});
    else kit_.panel(ui, Rect{kOx, kOy, kFrameW, kFrameH});

    // TradeFramePlayerNameText (75,-17, 100 wide) / TradeFrameRecipientNameText (245,-17, 80): GameFontNormal.
    auto nameText = [&](const std::string& name, float ux, float uw) {
        const Rect nr{PX(ux), PY(17) - 3.0f, uw * kSX, 16};
        if (static_cast<float>(fn.measure(name.c_str())) <= nr.w) ui.textShadow(fn, nr.x, nr.y, name.c_str(), kColGold);
        else if (static_cast<float>(fs.measure(name.c_str())) <= nr.w) ui.textShadow(fs, nr.x, nr.y + 2, name.c_str(), kColGold);
        else ui.textMarquee(fs, Rect{nr.x, nr.y + 2, nr.w, lh}, name.c_str(), kColGold);
    };
    nameText(name_, 75, 100);
    {
        UnitView u;
        nameText(unitView(peer, u) ? u.name : std::string(), 245, 80);
    }
    // TradeFrameCloseButton (UIPanelCloseButton, TOPRIGHT -25,-8).
    {
        const Rect cr{PX(327) + 1, PY(8) + 1, 18, 18};
        const bool f = top && winFocus_.addStop(kIdTdClose, cr);
        const bool p = f && winFocus_.isPressed(kIdTdClose);
        const pp::Texture& t = p && npcArt_.closeDown.pixels ? npcArt_.closeDown : npcArt_.close;
        if (t.pixels) ui.image(t, cr);
        if (f && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, cr, 0xFFFFFFFFu, Blend::Add);
    }

    // TradeHighlightPlayer / -Recipient (19 / 189,-100, 161 x 266) + -Enchant (4 under, 161 x 61): an accepted side.
    auto highlight = [&](float ux, float uy, float uh) {
        const Rect r{PX(ux), PY(uy), 161 * kSX, uh * kSY};
        if (!T.hl.pixels) {
            ui.fill(r, 0x3000FF00u);
            return;
        }
        const float tw = T.hl.uw, th = T.hl.uh, cap = th / 16.0f;  // top / bottom 16 of 256 (TexCoords 0.0625)
        const float capPx = std::floor(16.0f * kSY);
        ui.image(T.hl, Rect{r.x, r.y, r.w, capPx}, SrcRect{0, 0, tw, cap});
        ui.image(T.hl, Rect{r.x, r.y + capPx, r.w, r.h - 2 * capPx}, SrcRect{0, cap, tw, th - 2 * cap});
        ui.image(T.hl, Rect{r.x, r.y + r.h - capPx, r.w, capPx}, SrcRect{0, th - cap, tw, cap});
    };
    for (int side = 0; side < 2; ++side) {
        if (!session_.tradeAccepted(side)) continue;
        const float ux = side ? 189.0f : 19.0f;
        highlight(ux, 100, 266);
        highlight(ux, 370, 61);
    }

    // The items (TradeFrame_UpdatePlayerItem / _UpdateTargetItem).
    const uint32_t playerLevel = session_.selfField(game::fieldIndex(game::UF::UNIT_FIELD_LEVEL));
    for (int side = 0; side < 2; ++side) {
        for (int i = 0; i < td::kTradeSlots; ++i) {
            const float ux = itemX(side), uy = itemY(i);
            const uint32_t id = (side ? kIdTdTheirs0 : kIdTdMine0) + static_cast<uint32_t>(i);
            const Rect icon{PX(ux), PY(uy), kIcon, kIcon};
            const Rect hit{icon.x - 1, icon.y - 1, 153 * kSX, 40 * kSY};
            std::string name;
            uint32_t display = 0, count = 0;
            bool usable = true, has = false;
            if (side == 0) {
                game::ItemDef def;
                if (session_.tradeMineItem(i, def)) {
                    has = true;
                    name = def.name;
                    display = def.displayInfoId;
                    count = def.stackCount;
                }
            } else {
                const td::TradeSlotRecord& r = session_.tradeSlot(1, i);
                if (r.entry != 0) {
                    has = true;
                    display = r.displayId;
                    count = r.count;
                    const auto* info = session_.getItemInfo(r.entry);
                    if (info && info->valid) {
                        name = info->name;
                        if (display == 0) display = info->displayInfoId;
                        usable = info->requiredLevel <= playerLevel;  // PSP: the level rule of the merchant
                    } else {
                        session_.ensureItemInfo(r.entry);
                    }
                }
            }
            // Our slots are stops always (a held item goes there); theirs only with an item (the tooltip).
            const bool f = top && (side == 0 || has) && winFocus_.addStop(id, hit);
            if (has) drawItemButton(ui, icon, display, count, -1, usable ? 0xFFFFFFFFu : kColItemRed);
            if (side == 1 && has && !usable) ui.fill(Rect{PX(ux + 31), PY(uy - 2), 100 * kSX, 40 * kSY}, 0x300000E6u);
            if (f) {
                ui.fill(icon, 0x50FFFFFFu);  // ButtonHilight-Square
                T.tipSide = side;
                T.tipSlot = i;
                T.tipRect = Rect{icon.x + ui.offsetX(), icon.y + ui.offsetY(), icon.w, icon.h};
            }
            // $parentName (LEFT of the name frame +15 = x+46, 90 wide, GameFontNormalSmall); the 7th: the enchantment
            // (green) / TRADEFRAME_NOT_MODIFIED_TEXT (white), nothing without an item.
            const Rect nr{PX(ux + 46), PY(uy + 18.5f) - lh * 0.5f, 90 * kSX, lh};
            std::string text;
            uint32_t col = kColGold;
            if (i == td::kTradableSlots) {
                if (has) {
                    const uint32_t spell = session_.tradeSpell(side ? 0 : 1);  // the other side's spell on this item
                    if (spell != 0) {
                        text = spellDb_.name(spell);
                        col = kColGreenCode;
                    }
                    if (text.empty()) {
                        text = CLIENT_STR(TRADEFRAME_NOT_MODIFIED_TEXT);
                        col = kColWhite;
                    }
                }
            } else {
                text = name;
            }
            if (!text.empty()) {
                const std::vector<std::string> lines = wrapQuestText(fs, text, nr.w);
                if (f && lines.size() > 2) {
                    ui.textMarquee(fs, nr, text.c_str(), col);
                } else if (lines.size() >= 2) {
                    ui.pushClip(Rect{nr.x, nr.y - lh * 0.5f - 1, nr.w, lh * 2 + 2});
                    ui.textShadow(fs, nr.x, nr.y - lh * 0.5f, lines[0].c_str(), col);
                    ui.textShadow(fs, nr.x, nr.y + lh * 0.5f, lines[1].c_str(), col);
                    ui.popClip();
                } else {
                    ui.textShadow(fs, nr.x, nr.y, lines.empty() ? text.c_str() : lines[0].c_str(), col);
                }
            }
        }
    }
    // TradeFramePlayerEnchantText (26,-374) / Recipient (+170): TRADEFRAME_ENCHANT_SLOT_LABEL, GameFontHighlightSmall.
    // PSP: 3 px higher (the 10 px font is taller than the 12-unit one and would touch the 7th slot).
    ui.textShadow(fs, PX(26), PY(374) - 3, CLIENT_STR(TRADEFRAME_ENCHANT_SLOT_LABEL), kColWhite);
    ui.textShadow(fs, PX(196), PY(374) - 3, CLIENT_STR(TRADEFRAME_ENCHANT_SLOT_LABEL), kColWhite);

    // TradePlayerInputMoneyFrame (compact, 26,-73): gold 56 wide, silver +22 (30), copper +11 (30); the boxes and the
    // coins are baked. GetPlayerTradeMoney in them.
    {
        const uint32_t cur = session_.tradeGold(0);
        const uint32_t parts[3] = {cur / 10000u, (cur / 100u) % 100u, cur % 100u};
        const float xs[3] = {26.0f, 26.0f + 56.0f + 22.0f, 26.0f + 56.0f + 22.0f + 30.0f + 11.0f};
        const float ws[3] = {56.0f, 20.0f, 20.0f};
        for (int k = 0; k < 3; ++k) {
            const Rect r{PX(xs[k] - 5), PY(73), (ws[k] + 5) * kSX, 20 * kSY};
            const uint32_t id = kIdTdGold + static_cast<uint32_t>(k);
            const bool f = top && !session_.tradeAccepted(0) && winFocus_.addStop(id, r);
            std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(parts[k]));
            ui.textShadow(fs, PX(xs[k]), r.y + (r.h - lh) * 0.5f, buf, kColWhite);
            if (f) ui.fill(r, 0x30FFFFFFu);
        }
    }
    // TradeRecipientMoneyFrame (SmallMoneyFrame TOPRIGHT -40,-78): GetTargetTradeMoney.
    drawMoneyFrame(ui, PX(344), PY(78), session_.tradeGold(1), true);

    // TradeFrameTradeButton (85 x 22, BOTTOMRIGHT -113,55) / TradeFrameCancelButton (77 x 22, 3 right of it).
    // PSP: 14 px high on the buttons' centre line (the 0.55 height, 12 px, does not hold the 10 px text - as the
    // DressUpFrame's).
    const bool tradeEnabled = !session_.tradeAccepted(0) && !T.bindShown;
    const float by = std::floor(PY(446)) - 7.0f;
    drawPanelButton(ui, kIdTdTrade, Rect{PX(186), by, 85 * kSX, 14}, CLIENT_STR(TRADE), tradeEnabled, top);
    drawPanelButton(ui, kIdTdCancel, Rect{PX(274), by, 77 * kSX, 14}, CLIENT_STR(CANCEL), true, top);
    return true;
}

// GameTooltip:SetTradePlayerItem / SetTradeTargetItem (ANCHOR_RIGHT), the empty 7th: ENCHANT_SLOT's newbie tip.
void WorldScreen::drawTradeDuelOverlay(UiRenderer& ui) {
    if (!tradeDuel_ || !windowOpen(kWinTrade)) return;
    TradeDuelUi& T = *tradeDuel_;
    if (T.tipSide < 0 || T.tipSlot < 0) return;
    if (T.tipSide == 0) {
        game::ItemDef def;
        if (session_.tradeMineItem(T.tipSlot, def)) {
            drawItemTooltip(ui, def, T.tipRect);
        } else if (T.tipSlot == td::kTradableSlots) {
            kit_.tooltip(ui, T.tipRect, CLIENT_STR(ENCHANT_SLOT), CLIENT_STR(NEWBIE_TOOLTIP_ENCHANTSLOT));
        }
        return;
    }
    const td::TradeSlotRecord& r = session_.tradeSlot(1, T.tipSlot);
    if (r.entry == 0) return;
    const auto* info = session_.getItemInfo(r.entry);
    if (info && info->valid) drawItemTooltip(ui, session_.protoItemDef(r.entry, r.count), T.tipRect);
}

// --- Clicks ---------------------------------------------------------------------------------------------------------

bool WorldScreen::tradeDuelAction(uint32_t act) {
    if (act < 0x8700 || act > 0x87FF) return false;
    TradeDuelUi& T = tradeDuelUi();
    switch (act) {
        case kIdTdRequestYes:  // OnAccept: BeginTrade
        case kIdTdRequestNo:   // OnCancel: CancelTrade
            T.requestShown = false;
            closeWindow(kWinTradeRequest);
            session_.tradeAnswerRequest(act == kIdTdRequestYes);
            return true;
        case kIdTdDuelAccept:  // OnAccept: AcceptDuel
        case kIdTdDuelDecline:  // OnCancel: CancelDuel
            T.duelAnswered = true;
            T.duelShown = false;
            if (act == kIdTdDuelAccept) session_.acceptDuel();
            else session_.cancelDuel();
            closeWindow(kWinDuelRequest);
            return true;
        case kIdTdSummonAccept:  // OnAccept: ConfirmSummon
            session_.confirmSummon();
            T.summonShown = false;
            closeWindow(kWinSummon);
            return true;
        case kIdTdSummonCancel:  // OnCancel: CancelSummon
            session_.cancelSummon();
            T.summonShown = false;
            closeWindow(kWinSummon);
            return true;
        case kIdTdQuestYes:  // OnAccept: ConfirmAcceptQuest
            if (!session_.questLogFull()) session_.confirmAcceptQuest();
            T.questShown = false;
            closeWindow(kWinQuestAccept);
            session_.dismissQuestConfirm();
            return true;
        case kIdTdQuestNo:
            T.questShown = false;
            closeWindow(kWinQuestAccept);
            session_.dismissQuestConfirm();
            return true;
        case kIdTdBindOk:
            T.bindShown = false;
            closeWindow(kWinTradeBind);
            return true;
        case kIdTdBindCancel:  // OnCancel: ClickTradeButton(TRADE_ENCHANT_SLOT, true)
            T.bindShown = false;
            closeWindow(kWinTradeBind);
            session_.tradeClearItem(td::kTradableSlots);
            return true;
        case kIdTdClose: closeWindow(kWinTrade); return true;  // HideUIPanel -> OnHide -> CloseTrade (updateTradeDuel)
        case kIdTdTrade:  // AcceptTrade
            if (!session_.tradeAccepted(0) && !T.bindShown) session_.tradeAccept();
            return true;
        case kIdTdCancel:  // TradeFrameCancelButton_OnClick
            if (session_.tradeAccepted(0)) session_.tradeCancelAccept();
            else closeWindow(kWinTrade);
            return true;
        case kIdTdGold:
        case kIdTdSilver:
        case kIdTdCopper: {
            if (pp::oskActive() || session_.tradeAccepted(0)) return true;
            const uint32_t cur = session_.tradeGold(0);
            const uint32_t v = act == kIdTdGold ? cur / 10000u : act == kIdTdSilver ? (cur / 100u) % 100u : cur % 100u;
            // PSP: the keyboard's title - MONEY and the coin's *_AMOUNT_SYMBOL (the boxes have no label)
            const std::string coin = std::string(CLIENT_STR(MONEY)) + " (" +
                                     (act == kIdTdGold     ? CLIENT_STR(GOLD_AMOUNT_SYMBOL)
                                      : act == kIdTdSilver ? CLIENT_STR(SILVER_AMOUNT_SYMBOL)
                                                           : CLIENT_STR(COPPER_AMOUNT_SYMBOL)) +
                                     ")";
            T.oskField = static_cast<int>(act);
            if (!pp::oskBeginFor(kOskOwner, coin, v ? std::to_string(v) : std::string(), act == kIdTdGold ? 6 : 2,
                                 pp::OskCharset::Digits)) {
                T.oskField = 0;
                uiError(OWN_STR(PSP_KEYBOARD_FAILED));
            }
            return true;
        }
        default: break;
    }
    if (act >= kIdTdMine0 && act < kIdTdMine0 + td::kTradeSlots) {  // ClickTradeButton(i)
        const int slot = static_cast<int>(act - kIdTdMine0);
        if (invHeld_.active) {  // the held bag item goes there
            const InvHeld held = invHeld_;
            invHeld_ = InvHeld{};
            session_.tradeSetItem(slot, held.bag, held.slot);
        } else {  // off the table onto the cursor
            game::ItemDef def;
            uint8_t bag = 0, wslot = 0;
            if (session_.tradeMineItem(slot, def, &bag, &wslot)) {
                session_.tradeClearItem(slot);
                invHeld_ = InvHeld{true, bag, wslot};
            }
        }
        return true;
    }
    return act >= kIdTdTheirs0 && act < kIdTdTheirs0 + td::kTradeSlots;  // ClickTargetTradeButton: spells on the 7th
}

bool WorldScreen::tradeDuelAltAction(uint32_t alt) {
    if (alt >= kIdTdMine0 && alt < kIdTdMine0 + td::kTradeSlots) {  // ClickTradeButton(i, "RightButton")
        session_.tradeClearItem(static_cast<int>(alt - kIdTdMine0));
        return true;
    }
    return alt >= 0x8700 && alt <= 0x87FF;
}

}  // namespace wowee::ui::psp
