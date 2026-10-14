// The battlegrounds on the world screen, as the original ruRU 3.3.5a client shows them.
//
// BattlefieldFrame (BattlefieldFrame.xml / .lua, 384 x 512 at the character frame's 0.6 x 0.55, ui.pak "bf_frame"):
// BATTLEFIELDS_SHOW (SMSG_BATTLEFIELD_LIST from a battlemaster) opens it with "first available" selected; the
// title is the battleground's name (BattlemasterList), BATTLEFIELD_NAME, BattlefieldZone1..5 (FIRST_AVAILABLE and
// "name instance" rows, the queue status icons: the faction's PVP-Currency while queued, UI-StateIcon's swords when
// ready), the description (Map.dbc MapDescription0 / 1 by the player's faction) or, for the random battleground /
// the holiday, the rewards (RANDOM_BATTLEGROUND(_EXPLANATION) / BATTLEGROUND_HOLIDAY(_EXPLANATION), LFD_REWARDS,
// WIN / LOSS with the honor / arena points of the list packet), BATTLEFIELD_GROUP_JOIN (the party leader in a group),
// BATTLEFIELD_JOIN (JoinBattlefield(GetSelectedBattlefield[, 1]) and the frame hides), CANCEL and the close
// button. PSP: the list scrolls by its arrows (FauxScrollFrame's buttons); the description is cut at the box.
// StaticPopup CONFIRM_BATTLEFIELD_ENTRY (one per queue, multiple = 1): ENTER_BATTLE / LEAVE_QUEUE (disabled for an
// arena), the close button hides it (closeButtonIsHide), START hides it without declining (noCancelOnEscape);
// PlaySound("PVPTHROUGHQUEUE"). The queue sound PVPENTERQUEUE and the minimap shine for a new queue.
// MiniMapBattlefieldFrame (Minimap.xml: 33 at the Minimap's BOTTOMLEFT 13,-13 - the cluster's scale): the faction's
// Battleground-* icon (an arena match: PVP-ArenaPoints-Icon), MiniMap-TrackingBorder, BattlegroundShine; its
// tooltip (BATTLEFIELD_IN_QUEUE / _QUEUE_CONFIRM / _IN_BATTLEFIELD, RIGHT_CLICK_MESSAGE); Cross in a battle =
// ToggleWorldStateScoreFrame, Circle = MiniMapBattlefieldDropDown (the queue's name, LEAVE_QUEUE / ENTER_BATTLE,
// LEAVE_BATTLEGROUND / LEAVE_ARENA). PSP: BATTLEFIELD_IN_BATTLEFIELD without its Shift-click line (no battlefield
// minimap).
// WorldStateScoreFrame (WorldStateFrame.xml / .lua, ui.pak "ws_score" in three slices, PSP: the x scale fits the
// width of the battleground's columns into the screen, 0.55 high): STAT_TEMPLATE, the columns (name, killing blows,
// deaths, honorable kills, damage, healing, the battleground's WorldStateUI type-2 columns, honor), the rows
// (WorldStateScoreTemplate: the faction band, the class icon, the name, the values, the column icons "x n"), the
// counts, TIME_ELAPSED, the tabs ALL / FACTION_ALLIANCE / FACTION_HORDE, the winner banner, LEAVE_BATTLEGROUND and
// TIME_TO_PORT; shown by itself for the winner. Sorting by a column header (SortBattlefieldScoreData), Circle on a
// player of the raid: ScorePlayerDropDown (PVP_REPORT_AFK). PSP: the scores are asked for once a second while
// it is shown (RequestBattlefieldScoreData every frame in the original, throttled by the client).
// The lines of SMSG_GROUP_JOINED_BATTLEGROUND as upstream SocialHandler says them, shown by the client-wide GameLine
// rule: a joined queue (ERR_GROUP_JOIN_BATTLEGROUND_S) in the chat, every refusal on the red error line. A queue's
// status change says one chat line as upstream handleBattlefieldStatus does (queued / ready / entered).
#include "ui/psp/ui_world_raid_bg.hpp"

#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

constexpr float kSX = 0.6f, kSY = 0.55f;  // build_ui_pak.py BF_SX / BF_SY
constexpr float kOx = 4.0f, kOy = 2.0f;   // BattlefieldFrame TOPLEFT 0,-104 (as the character frame)
constexpr float kFrameW = 210.0f, kFrameH = 240.0f;
float PX(float x) { return kOx + x * kSX; }
float PY(float y) { return kOy + y * kSY; }
Rect R(float x, float y, float w, float h) { return Rect{PX(x), PY(y), w * kSX, h * kSY}; }

// GlobalStrings (strings.pak). The score frame's tabs: ALL, FACTION_ALLIANCE, FACTION_HORDE.
constexpr pp::CS kTabs[3] = {pp::CS::ALL, pp::CS::FACTION_ALLIANCE, pp::CS::FACTION_HORDE};
constexpr pp::CS kVictory[2] = {pp::CS::VICTORY_TEXT0, pp::CS::VICTORY_TEXT1};
constexpr pp::CS kVictoryArena[2] = {pp::CS::VICTORY_TEXT_ARENA0, pp::CS::VICTORY_TEXT_ARENA1};

// SMSG_GROUP_JOINED_BATTLEGROUND: the results -15..-2 by AzerothCore's GroupJoinBattlegroundResult (the names are
// the GlobalStrings keys); "" for -9 (AzerothCore: "doesn't exist in client").
const char* joinResultText(int result) {
    static constexpr pp::CS k[14] = {
        pp::CS::ERR_IN_NON_RANDOM_BG,                     // -15
        pp::CS::ERR_IN_RANDOM_BG,                         // -14
        pp::CS::ERR_LFG_CANT_USE_BATTLEGROUND,            // -13
        pp::CS::ERR_BATTLEGROUND_JOIN_FAILED,             // -12
        pp::CS::ERR_BATTLEGROUND_JOIN_TIMED_OUT,          // -11
        pp::CS::ERR_BATTLEGROUND_JOIN_RANGE_INDEX,        // -10
        pp::CS::ERR_BATTLEGROUND_JOIN_FAILED,             // -9 (not used)
        pp::CS::ERR_BATTLEGROUND_NOT_IN_BATTLEGROUND,     // -8
        pp::CS::ERR_BATTLEGROUND_TEAM_LEFT_QUEUE,         // -7
        pp::CS::ERR_BATTLEDGROUND_QUEUED_FOR_RATED,       // -6
        pp::CS::ERR_BATTLEGROUND_CANNOT_QUEUE_FOR_RATED,  // -5
        pp::CS::ERR_BATTLEGROUND_TOO_MANY_QUEUES,         // -4
        pp::CS::ERR_ARENA_TEAM_PARTY_SIZE,                // -3
        pp::CS::ERR_GROUP_JOIN_BATTLEGROUND_DESERTERS,    // -2
    };
    if (result < -15 || result > -2 || result == -9) return "";
    return pp::cs(k[result + 15]);
}

// BATTLEFIELD_IN_BATTLEFIELD without its colour and (PSP) without the last line (Shift-click: the zone map).
std::string inBattlefieldText(const std::string& name) {
    std::string t = pp::text::uiPlain(CLIENT_FMT(BATTLEFIELD_IN_BATTLEFIELD, name));
    const size_t last = t.rfind('\n');
    if (last != std::string::npos && t.find('\n') != last) t.erase(last);
    return t;
}

constexpr uint32_t kColDisable = 0xFF808080u;
constexpr uint32_t kColBlack = 0xFF000000u;

// SecondsToTime (UIParent.lua): the largest two units, TIME_UNIT_DELIMITER " "; noSeconds drops the seconds
// (SecondsToTime(t, 1)).
std::string secondsToTime(long seconds, bool noSeconds = false) {
    if (seconds < 0) seconds = 0;
    std::string t;
    int count = 0;
    auto add = [&](const char* f, long v) {
        if (!t.empty()) t += ' ';
        t += pp::text::plural(pp::text::textf(f, static_cast<int>(v)));  // enUS "%d |4Min:Min;"
        ++count;
    };
    if (seconds >= 86400) {
        add(CLIENT_STR(DAYS_ABBR), seconds / 86400);
        seconds %= 86400;
    }
    if (count < 2 && seconds >= 3600) {
        add(CLIENT_STR(HOURS_ABBR), seconds / 3600);
        seconds %= 3600;
    }
    if (count < 2 && seconds >= 60) {
        add(CLIENT_STR(MINUTES_ABBR), seconds / 60);
        seconds %= 60;
    }
    if (count < 2 && seconds > 0 && !noSeconds) add(CLIENT_STR(SECONDS_ABBR), seconds);
    return t;
}

// The pattern is the client's text of the running language (strings.pak): textf, not snprintf.
std::string fmtS(const char* f, const std::string& a) { return pp::text::textf(f, a); }

bool allianceRace(uint8_t race) { return race == 1 || race == 3 || race == 4 || race == 7 || race == 11; }

}  // namespace

// --- Art -----------------------------------------------------------------------------------------------

namespace {

void loadBfArt(WorldScreen::RaidBgUi& r) {
    r.bfTried = true;
    char path[256];
    pp::fsPath(path, sizeof(path), "data/ui.pak");
    pp::Pak pak;
    int ok = 0;
    if (pak.open(path)) {
        const struct {
            const char* name;
            pp::Texture* t;
        } list[] = {{"bf_frame", &r.bfFrame}, {"bf_hl", &r.bfHl}, {"bf_cur_a", &r.bfCur[0]}, {"bf_cur_h", &r.bfCur[1]},
                    {"bf_arena", &r.bfArena}, {"bf_confirm", &r.bfConfirm}};
        for (const auto& x : list)
            if (raidBgLoad(pak, x.name, *x.t, r.bfBytes)) ++ok;
        pak.close();
    }
    PSP_LOGI(pp::tag::Gfx, "battlefield_art ok=%d/6 kb=%u", ok, static_cast<unsigned>(r.bfBytes / 1024u));
}

void freeBfArt(WorldScreen::RaidBgUi& r) {
    raidBgFree({&r.bfFrame, &r.bfHl, &r.bfCur[0], &r.bfCur[1], &r.bfArena, &r.bfConfirm});
    r.bfTried = false;
    r.bfBytes = 0;
}

void loadScoreArt(WorldScreen::RaidBgUi& r) {
    r.scTried = true;
    char path[256];
    pp::fsPath(path, sizeof(path), "data/ui.pak");
    pp::Pak pak;
    int ok = 0;
    if (pak.open(path)) {
        const struct {
            const char* name;
            pp::Texture* t;
        } list[] = {{"ws_score", &r.score}, {"ws_sc_bg", &r.scoreBg}, {"ws_sc_hl", &r.scoreHl}, {"ws_classes", &r.classes}};
        for (const auto& x : list)
            if (raidBgLoad(pak, x.name, *x.t, r.scBytes)) ++ok;
        pak.close();
    }
    PSP_LOGI(pp::tag::Gfx, "score_art ok=%d/4 kb=%u", ok, static_cast<unsigned>(r.scBytes / 1024u));
}

void freeScoreArt(WorldScreen::RaidBgUi& r) {
    raidBgFree({&r.score, &r.scoreBg, &r.scoreHl, &r.classes});
    r.scTried = false;
    r.scBytes = 0;
}

}  // namespace

// --- GetBattlefieldStatus' mapName ----------------------------------------------------------------------

std::string WorldScreen::battlefieldName(int slot) {
    RaidBgUi& r = raidBg();
    loadRaidBgData();
    const auto& q = session_.bgQueue(slot);
    if (q.status == 0) return std::string();
    if (q.arenaType != 0) {  // teamSize: ARENA_RATED_MATCH / ARENA_CASUAL PVP_TEAMSIZE
        return std::string(q.rated ? CLIENT_STR(ARENA_RATED_MATCH) : CLIENT_STR(ARENA_CASUAL)) + " " +
               CLIENT_FMT(PVP_TEAMSIZE, q.arenaType, q.arenaType);
    }
    const RaidBgUi::Bg* b = r.bg(q.bgTypeId);
    std::string name = b ? b->name : std::to_string(q.bgTypeId);
    if (q.instanceId != 0) name += " " + std::to_string(q.instanceId);
    return name;
}

// --- Per frame (updateRaidBg) ------------------------------------------------------------------------------

void WorldScreen::updateBattleground() {
    RaidBgUi& r = raidBg();
    const uint32_t now = pp::logUptimeMs();
    // BATTLEFIELDS_SHOW: ShowUIPanel(BattlefieldFrame), SetSelectedBattlefield(0) (not for an arena battlemaster:
    // the arena frames are not on the PSP).
    if (session_.battlefieldListSeq() != r.listSeq) {
        r.listSeq = session_.battlefieldListSeq();
        loadRaidBgData();
        const RaidBgUi::Bg* b = r.bg(session_.battlefieldList().bgTypeId);
        if (!b || b->type != 4) {
            r.selected = 0;
            r.offset = 0;
            if (!windowOpen(kWinBattlefield)) {
                openWindow(kWinBattlefield);
                playSoundByName("igCharacterInfoOpen");
            }
            if (uiMode_) winFocus_.setFocus(kIdBfJoin);
            PSP_LOGI(pp::tag::Ctrl, "battlefield_frame type=%u instances=%u",
                     static_cast<unsigned>(session_.battlefieldList().bgTypeId),
                     static_cast<unsigned>(session_.battlefieldList().instances.size()));
        }
    }
    if (windowOpen(kWinBattlefield) && !r.bfTried) loadBfArt(r);
    else if (!windowOpen(kWinBattlefield) && r.bfTried) freeBfArt(r);
    // UPDATE_BATTLEFIELD_STATUS -> BattlefieldFrame_UpdateStatus(false, slot).
    if (session_.bgStatusSeq() != r.statusSeq) {
        r.statusSeq = session_.bgStatusSeq();
        loadRaidBgData();
        const int changed = session_.bgStatusSlot();
        for (int i = 0; i < pp::WorldSession::kBgQueueSlots; ++i) {
            const auto& q = session_.bgQueue(i);
            const uint8_t w = i == 0 ? kWinBgConfirm : kWinBgConfirm2;
            if (q.status != 2 && (r.confirmShown[i] || windowOpen(w))) {  // StaticPopup_Hide("CONFIRM_BATTLEFIELD_ENTRY", i)
                if (windowOpen(w)) closeWindow(w);
                r.confirmShown[i] = false;
            }
            if (q.status == 1 && r.prevQueued[i] != q.bgTypeId + 1u) {  // a new queue: fade in, shine, the sound
                r.shineMs = now | 1u;
                playSoundByName("PVPENTERQUEUE");
            }
            r.prevQueued[i] = q.status == 1 ? q.bgTypeId + 1u : 0u;
            // upstream handleBattlefieldStatus: one chat line when a queue's status (or battleground) changes.
            if (q.status != r.prevStatus[i] || q.bgTypeId != r.prevStatusType[i]) {
                const std::string name = battlefieldName(i);
                if (q.status == 1) session_.addSystemChatMessage(OWN_FMT(BG_QUEUED_FOR, name));
                else if (q.status == 2) session_.addSystemChatMessage(OWN_FMT(BG_READY, name));
                else if (q.status == 3) session_.addSystemChatMessage(OWN_FMT(BG_ENTERED, name));
                r.prevStatus[i] = q.status;
                r.prevStatusType[i] = q.bgTypeId;
            }
            if (q.status == 2 && i == changed) {  // StaticPopup_Show("CONFIRM_BATTLEFIELD_ENTRY", mapName, nil, i)
                r.confirmShown[i] = true;
                openWindow(w);
                if (uiMode_) winFocus_.setFocus(kIdBcEnter0 + static_cast<uint32_t>(i) * 4u);
                playSoundByName("PVPTHROUGHQUEUE");
                PSP_LOGI(pp::tag::Ctrl, "battlefield_confirm slot=%d", i);
            }
        }
    }
    // A confirm popup the stack lost (START: hideOnEscape, the close button): hidden, the queue stays.
    for (int i = 0; i < 2; ++i)
        if (r.confirmShown[i] && !windowOpen(i == 0 ? kWinBgConfirm : kWinBgConfirm2)) r.confirmShown[i] = false;
    // SMSG_GROUP_JOINED_BATTLEGROUND (upstream SocialHandler): a joined queue -> the chat; a refusal -> the red
    // error line (GameLine).
    pp::WorldSession::BgEvent ev;
    while (session_.takeBgEvent(ev)) {
        loadRaidBgData();
        std::string refusal;
        if (ev.result >= -15 && ev.result <= -2) {
            const char* f = joinResultText(ev.result);
            if (!*f) continue;
            refusal = ev.result == -11 ? fmtS(f, ev.name) : std::string(f);
        } else if (const RaidBgUi::Bg* b = ev.result > 0 ? r.bg(static_cast<uint32_t>(ev.result)) : nullptr) {
            session_.addSystemChatMessage(fmtS(CLIENT_STR(ERR_GROUP_JOIN_BATTLEGROUND_S), b->name));
            continue;
        } else {
            refusal = CLIENT_STR(ERR_GROUP_JOIN_BATTLEGROUND_FAIL);
        }
        session_.gameLine(pp::GameLine::Refused, refusal);
    }
    // The minimap button: while any queue is up.
    bool any = false;
    for (int i = 0; i < pp::WorldSession::kBgQueueSlots; ++i) any = any || session_.bgQueue(i).status != 0;
    r.mmShown = any;
    if (!any && r.dropKind == 1 && windowOpen(kWinDropDown)) {
        closeWindow(kWinDropDown);
        r.dropKind = 0;
    }
    // WorldStateScoreFrame: shown by itself once there is a winner; hidden out of the battle (PLAYER_ENTERING_WORLD).
    const bool inBattle = session_.activeBgSlot() >= 0;
    const auto& sb = session_.bgScoreboard();
    if (inBattle && sb.hasWinner && !r.winnerOpened) {
        r.winnerOpened = true;
        if (!windowOpen(kWinScore)) openWindow(kWinScore);
    }
    if (!inBattle) {
        r.winnerOpened = false;
        if (windowOpen(kWinScore)) closeWindow(kWinScore);
    }
    if (windowOpen(kWinScore)) {
        if (!r.scTried) loadScoreArt(r);
        if (now - r.scoreAskMs >= 1000u) {  // RequestBattlefieldScoreData (OnUpdate)
            r.scoreAskMs = now;
            session_.requestPvpLog();
        }
    } else if (r.scTried) {
        freeScoreArt(r);
    }
    // The dropdown: closed by leaving the interface or UIDROPDOWNMENU_SHOW_TIME away.
    if (r.dropKind == 1 || r.dropKind == 3) {
        const uint32_t f = winFocus_.focused();
        if ((f >= kIdMmBgMenu0 && f < kIdMmBgMenu0 + 8) || f == kIdMmBattlefield) r.dropFocusMs = now;
        if (!windowOpen(kWinDropDown) || !uiMode_ || now - r.dropFocusMs > 2000u) {
            if (windowOpen(kWinDropDown)) closeWindow(kWinDropDown);
            r.dropKind = 0;
        }
    }
}

// --- BattlefieldFrame (window kWinBattlefield) --------------------------------------------------------------

bool WorldScreen::drawBattlefieldFrame(UiRenderer& ui) {
    PROF_SCOPE("bg.frame");
    RaidBgUi& r = raidBg();
    if (!r.bfTried) loadBfArt(r);
    loadRaidBgData();
    loadNpcArt(false);
    const bool focusable = uiMode_ && !settings_.isOpen() && !windowOpen(kWinBindConfirm);
    const Font& fs = kit_.font(kFontSmall);
    const Font& fn = kit_.font(kFontNormal);
    const Font& ft = kit_.font(kFontTiny);
    const auto& list = session_.battlefieldList();
    const RaidBgUi::Bg* bg = r.bg(list.bgTypeId);
    if (r.bfFrame.pixels) ui.image(r.bfFrame, Rect{kOx, kOy, kFrameW, kFrameH});
    else kit_.panel(ui, Rect{kOx, kOy, kFrameW, kFrameH});
    const std::string mapName = bg ? bg->name : std::to_string(list.bgTypeId);
    // BattlefieldFrameFrameLabel (TOP 0,-17): the battleground's name; BattlefieldFrameNameHeader (75,-55).
    ui.textShadow(fn, PX(192), PY(17), mapName.c_str(), kColGold, Align::Center);
    ui.textShadow(fs, PX(75), PY(55), CLIENT_STR(BATTLEFIELD_NAME), kColWhite);
    // BattlefieldFrameCloseButton (TOPRIGHT -29,-8: CENTER at 339,24).
    {
        const Rect cr{PX(339) - 10, PY(24) - 10, 20, 20};
        const bool f = focusable && winFocus_.addStop(kIdBfClose, cr);
        const pp::Texture& t = f && winFocus_.isPressed(kIdBfClose) && npcArt_.closeDown.pixels ? npcArt_.closeDown : npcArt_.close;
        if (t.pixels) ui.image(t, cr);
        if (f) ui.fill(cr, 0x40FFFFFFu);
    }
    // BattlefieldZone1..5 (293 x 16 from 23,-79): FIRST_AVAILABLE, then "name instance".
    const int total = 1 + static_cast<int>(list.instances.size());
    r.offset = std::clamp(r.offset, 0, std::max(0, total - 5));
    const uint8_t myRace = session_.activeCharacter() ? static_cast<uint8_t>(session_.activeCharacter()->race) : 1;
    const int faction = allianceRace(myRace) ? 0 : 1;
    r.bfTipId = 0;
    for (int k = 0; k < 5; ++k) {
        const int idx = r.offset + k;
        if (idx >= total) break;
        const Rect b = R(23, 79 + 16.0f * static_cast<float>(k), 293, 16);
        const uint32_t id = kIdBfZone0 + static_cast<uint32_t>(k);
        const bool f = focusable && winFocus_.addStop(id, b);
        const bool sel = idx == r.selected;
        if ((sel || f) && r.bfHl.pixels) ui.image(r.bfHl, b, 0xFF00D1FFu, Blend::Add);  // SetVertexColor(1, 0.82, 0)
        const uint32_t inst = idx == 0 ? 0 : list.instances[static_cast<size_t>(idx - 1)];
        const std::string text = idx == 0 ? std::string(CLIENT_STR(FIRST_AVAILABLE)) : mapName + " " + std::to_string(inst);
        ui.textShadow(fs, b.x + 20.0f * kSX, b.y + (b.h - static_cast<float>(fs.lineHeight())) * 0.5f + 1.0f, text.c_str(),
                      f ? kColWhite : kColGold);
        // The queue status icon (16 at LEFT, its texture 21): queued - the faction's currency; ready - the swords.
        for (int s = 0; s < pp::WorldSession::kBgQueueSlots; ++s) {
            const auto& q = session_.bgQueue(s);
            if (q.status == 0 || q.bgTypeId != list.bgTypeId) continue;
            const bool match = idx == 0 ? q.instanceId == 0 : q.instanceId == inst;
            if (!match || (idx == 0 && q.status != 1)) continue;
            const Rect ir{b.x + 8.0f * kSX - 10.5f * kSX, b.y + b.h * 0.5f - 10.5f * kSY, 21.0f * kSX, 21.0f * kSY};
            const pp::Texture& t = q.status == 1 ? r.bfCur[faction] : r.bfConfirm;
            if ((q.status == 1 || q.status == 2) && t.pixels) ui.image(t, ir);
        }
        if (f) {
            r.bfTipId = id;
            r.tipRect = Rect{b.x + ui.offsetX(), b.y + ui.offsetY(), b.w, b.h};  // screen (the overlay)
            r.tipTitle = text;
            r.tipLine =
                idx == 0 ? CLIENT_STR(NEWBIE_TOOLTIP_FIRST_AVAILABLE) : CLIENT_STR(NEWBIE_TOOLTIP_ENTER_BATTLEGROUND);
        }
    }
    // The list's scroll buttons (FauxScrollFrame, only for a longer list).
    if (total > 5) {
        const Rect up = R(23 + 293 + 2, 74, 16, 16), dn = R(23 + 293 + 2, 74 + 95 - 16, 16, 16);
        const bool fu = focusable && r.offset > 0 && winFocus_.addStop(kIdBfUp, up);
        const bool fd = focusable && r.offset + 5 < total && winFocus_.addStop(kIdBfDown, dn);
        const pp::Texture& tu = r.offset > 0 ? npcArt_.scrUp : npcArt_.scrUpDis;
        const pp::Texture& td = r.offset + 5 < total ? npcArt_.scrDown : npcArt_.scrDownDis;
        if (tu.pixels) ui.image(tu, up);
        if (td.pixels) ui.image(td, dn);
        if (fu) ui.fill(up, 0x40FFFFFFu);
        if (fd) ui.fill(dn, 0x40FFFFFFu);
    }
    // BattlefieldFrameInfoScrollFrame (298 x 232 at BOTTOMLEFT 19,100): the rewards of the random battleground /
    // the holiday, else the map's description (GameFontBlackMedium at 15,-15, 275 wide).
    const Rect info = R(19, 512 - 100 - 232, 298, 232);
    // GetBattlefieldInfo's isHoliday: the BattlemasterList row's HolidayWorldState is set (the call to arms).
    const bool holiday = bg && bg->holidayWs != 0 && session_.worldState(bg->holidayWs) != 0;
    ui.pushClip(info);
    if (list.isRandom || holiday) {
        float y = info.y + 15.0f * kSY;
        ui.textShadow(fn, info.x + 20.0f * kSX, y,
                      list.isRandom ? CLIENT_STR(RANDOM_BATTLEGROUND) : CLIENT_STR(BATTLEGROUND_HOLIDAY), kColGold);
        y += static_cast<float>(fn.lineHeight()) + 5.0f * kSY;
        const char* explanation =
            list.isRandom ? CLIENT_STR(RANDOM_BATTLEGROUND_EXPLANATION) : CLIENT_STR(BATTLEGROUND_HOLIDAY_EXPLANATION);
        for (const std::string& l : wrapQuestText(ft, explanation, 280.0f * kSX)) {
            ui.text(ft, info.x + 20.0f * kSX, y, l.c_str(), kColBlack);
            y += static_cast<float>(ft.lineHeight());
        }
        y += 10.0f * kSY;
        ui.textShadow(fn, info.x + 20.0f * kSX, y, CLIENT_STR(LFD_REWARDS), kColGold);
        y += static_cast<float>(fn.lineHeight()) + 15.0f * kSY;
        const uint32_t winHonor = list.isRandom ? list.winHonorRandom : list.winHonor;
        const uint32_t winArena = list.isRandom ? list.winArenaRandom : list.winArena;
        const uint32_t lossHonor = list.isRandom ? list.lossHonorRandom : list.lossHonor;
        auto reward = [&](float yy, const char* label, uint32_t honor, uint32_t arena, uint32_t bgc) {
            const Rect row{info.x + 1.0f * kSX, yy, 319.0f * kSX, 40.0f * kSY};
            ui.fill(row, bgc);
            const float cy = row.y + row.h * 0.5f;
            ui.textShadow(fs, row.x + (5.0f + 45.0f) * kSX, cy - static_cast<float>(fs.lineHeight()) * 0.5f, label, kColWhite,
                          Align::Center);
            float x = row.x + (5.0f + 90.0f + 5.0f) * kSX;
            if (honor != 0) {
                if (r.bfCur[faction].pixels) ui.image(r.bfCur[faction], Rect{x, cy - 16.0f * kSY, 32.0f * kSX, 32.0f * kSY});
                x += (32.0f + 3.0f) * kSX;
                ui.textShadow(fs, x, cy - static_cast<float>(fs.lineHeight()) * 0.5f, std::to_string(honor).c_str(), kColWhite);
                x += (65.0f + 5.0f) * kSX;
            }
            if (arena != 0) {
                if (r.bfArena.pixels) ui.image(r.bfArena, Rect{x, cy - 16.0f * kSY, 32.0f * kSX, 32.0f * kSY});
                x += (32.0f + 3.0f) * kSX;
                ui.textShadow(fs, x, cy - static_cast<float>(fs.lineHeight()) * 0.5f, std::to_string(arena).c_str(), kColWhite);
            }
        };
        reward(y, CLIENT_STR(WIN), winHonor, winArena, 0x33007000u);                 // (0, 0.439, 0, 0.2)
        reward(y + (40.0f + 2.0f) * kSY, CLIENT_STR(LOSS), lossHonor, 0, 0x3300008Fu);  // (0.5608, 0, 0, 0.2)
    } else if (bg) {
        const RaidBgUi::Map* m = !bg->maps.empty() ? r.map(bg->maps[0]) : nullptr;
        if (m) {
            float y = info.y + 15.0f * kSY;
            for (const std::string& l : wrapQuestText(ft, m->desc[faction], 275.0f * kSX)) {
                ui.text(ft, info.x + 15.0f * kSX, y, l.c_str(), kColBlack);
                y += static_cast<float>(ft.lineHeight());
            }
        }
    }
    ui.popClip();
    // BattlefieldFrameGroupJoinButton (CanJoinBattlefieldAsGroup: the party leader in a group), the join button,
    // the cancel button (CENTER at 305,-423).
    const bool inGroup = session_.isInGroup();
    const bool canGroup = inGroup && session_.isGroupLeader();
    const Rect cancel = R(305 - 41.5f, 423 - 11, 83, 22);
    const Rect join{cancel.x - 2.0f * kSX - 109.0f * kSX, cancel.y, 109.0f * kSX, cancel.h};
    const Rect group{join.x - 136.0f * kSX, cancel.y, 136.0f * kSX, cancel.h};
    drawPanelButton(ui, kIdBfGroupJoin, group, CLIENT_STR(BATTLEFIELD_GROUP_JOIN), canGroup, focusable);
    drawPanelButton(ui, kIdBfJoin, join, CLIENT_STR(BATTLEFIELD_JOIN), true, focusable);
    drawPanelButton(ui, kIdBfCancel, cancel, CLIENT_STR(CANCEL), true, focusable);
    if (focusable && winFocus_.focused() == kIdBfGroupJoin) {
        r.bfTipId = kIdBfGroupJoin;
        r.tipRect = Rect{group.x + ui.offsetX(), group.y + ui.offsetY(), group.w, group.h};
        r.tipTitle = CLIENT_STR(BATTLEFIELD_GROUP_JOIN);
        r.tipLine = CLIENT_STR(NEWBIE_TOOLTIP_BATTLEFIELD_GROUP_JOIN);
    }
    return true;
}

// --- CONFIRM_BATTLEFIELD_ENTRY (windows kWinBgConfirm / kWinBgConfirm2) -------------------------------------

bool WorldScreen::drawBattlefieldConfirm(UiRenderer& ui, int slot) {
    RaidBgUi& r = raidBg();
    if (!r.confirmShown[slot]) return true;
    const auto& q = session_.bgQueue(slot);
    const std::string text = fmtS(CLIENT_STR(CONFIRM_BATTLEFIELD_ENTRY), battlefieldName(slot));
    StaticPopup sp;
    sp.text = text.c_str();
    sp.button1 = CLIENT_STR(ENTER_BATTLE);
    sp.button1Id = kIdBcEnter0 + static_cast<uint32_t>(slot) * 4u;
    sp.button2 = CLIENT_STR(LEAVE_QUEUE);
    sp.button2Id = kIdBcLeave0 + static_cast<uint32_t>(slot) * 4u;
    sp.button2Enabled = q.arenaType == 0;  // OnShow: an arena's queue cannot be left from here
    sp.focusable = uiMode_ && !settings_.isOpen() && !windowOpen(kWinBindConfirm);
    const Rect f = drawStaticPopup(ui, sp);
    // closeButton / closeButtonIsHide: StaticPopupNCloseButton at the TOPRIGHT (-3,-3), 32 units.
    loadNpcArt(false);
    const float cs = std::floor(32.0f * kDialogScale);
    const Rect cr{f.x + f.w - cs - std::floor(3.0f * kDialogScale), f.y + std::floor(3.0f * kDialogScale), cs, cs};
    const uint32_t cid = kIdBcClose0 + static_cast<uint32_t>(slot) * 4u;
    const bool fc = sp.focusable && winFocus_.addStop(cid, cr);
    const pp::Texture& t = fc && winFocus_.isPressed(cid) && npcArt_.closeDown.pixels ? npcArt_.closeDown : npcArt_.close;
    if (t.pixels) ui.image(t, cr);
    if (fc) ui.fill(cr, 0x40FFFFFFu);
    return true;
}

// --- MiniMapBattlefieldFrame (drawn by drawMinimapCluster) ---------------------------------------------------

void WorldScreen::drawBattlefieldMinimapButton(UiRenderer& ui, const pp::Texture* cluster, bool focusable) {
    if (!rbg_ || !rbg_->mmShown) return;
    RaidBgUi& r = *rbg_;
    if (!r.wsTried) {  // the icons (with the world state art: they come and go together)
        r.wsTried = true;
        char path[256];
        pp::fsPath(path, sizeof(path), "data/ui.pak");
        pp::Pak pak;
        if (pak.open(path)) {
            raidBgLoad(pak, "bf_mm_a", r.mmIcon[0], r.wsBytes);
            raidBgLoad(pak, "bf_mm_h", r.mmIcon[1], r.wsBytes);
            raidBgLoad(pak, "bf_arena", r.mmIcon[2], r.wsBytes);
            raidBgLoad(pak, "ws_capbar", r.capBar, r.wsBytes);
            raidBgLoad(pak, "ws_icons", r.wsIcons, r.wsBytes);
            pak.close();
        }
    }
    // 33 units at the Minimap's BOTTOMLEFT +13,-13 (MM_K = 76 / 140): the tracking border's ring at the button,
    // the icon (32 at CENTER -1,0; a rated arena: PVP-ArenaPoints-Icon 19 at -1,2).
    constexpr Rect kRect{402, 77, 18, 18};
    constexpr SrcRect kSrcTrkBorder{106, 18, 18, 18}, kSrcShine{152, 18, 15, 15};  // ui_world_minimap.cpp's atlas
    const uint8_t myRace = session_.activeCharacter() ? static_cast<uint8_t>(session_.activeCharacter()->race) : 1;
    bool rated = false;
    for (int i = 0; i < pp::WorldSession::kBgQueueSlots; ++i) rated = rated || session_.bgQueue(i).rated;
    if (rated && r.mmIcon[2].pixels) ui.image(r.mmIcon[2], Rect{kRect.x + 3.5f, kRect.y + 3.0f, 10.0f, 10.0f});
    else if (const pp::Texture& t = r.mmIcon[allianceRace(myRace) ? 0 : 1]; t.pixels)
        ui.image(t, Rect{kRect.x - 0.5f, kRect.y + 0.5f, 17.0f, 17.0f});
    if (cluster) ui.image(*cluster, kRect, kSrcTrkBorder);
    if (r.shineMs) {  // BattlegroundShineFadeIn / Out: 0.5 s each way
        const uint32_t t = pp::logUptimeMs() - r.shineMs;
        if (t >= 1000u) {
            r.shineMs = 0;
        } else if (cluster) {
            const float a = t < 500u ? static_cast<float>(t) / 500.0f : static_cast<float>(1000u - t) / 500.0f;
            ui.image(*cluster, Rect{kRect.x + 3.0f, kRect.y + 1.0f, 15.0f, 15.0f}, kSrcShine,
                     (static_cast<uint32_t>(a * 255.0f) << 24) | 0x00FFFFFFu, Blend::Add);
        }
    }
    if (focusable && winFocus_.addStop(kIdMmBattlefield, kRect)) {
        // MiniMapBattlefieldFrame OnEnter / BattlefieldFrame_UpdateStatus(tooltipOnly): the queues' lines.
        std::string tip;
        bool rightClick = false;
        for (int i = 0; i < pp::WorldSession::kBgQueueSlots; ++i) {
            const auto& q = session_.bgQueue(i);
            if (q.status == 0) continue;
            const std::string name = battlefieldName(i);
            std::string line;
            if (q.status == 1) {
                const std::string wait = q.avgWaitMs == 0 ? CLIENT_STR(QUEUE_TIME_UNAVAILABLE)
                                         : q.avgWaitMs < 60000u ? CLIENT_STR(LESS_THAN_ONE_MINUTE)
                                                                : secondsToTime(q.avgWaitMs / 1000u, true);
                const long waited = q.queuedSinceMs ? static_cast<long>((pp::logUptimeMs() - q.queuedSinceMs) / 1000u) : 0;
                line = CLIENT_FMT(BATTLEFIELD_IN_QUEUE, name, wait, secondsToTime(waited));
                rightClick = true;
            } else if (q.status == 2) {
                const int32_t left = q.portExpireMs ? static_cast<int32_t>(q.portExpireMs - pp::logUptimeMs()) : 0;
                const std::string s = secondsToTime(left > 0 ? left / 1000 : 0);
                line = !s.empty() ? CLIENT_FMT(BATTLEFIELD_QUEUE_CONFIRM, name, s)
                                  : CLIENT_FMT(BATTLEFIELD_QUEUE_PENDING_REMOVAL, name);
                rightClick = true;
            } else if (q.status == 3) {
                line = q.arenaType != 0 ? name : inBattlefieldText(name);
            }
            if (!line.empty()) tip += (tip.empty() ? "" : "\n\n") + line;
        }
        if (!tip.empty() && rightClick) tip += "\n" + pp::text::uiPlain(CLIENT_STR(RIGHT_CLICK_MESSAGE));
        r.tipId = kIdMmBattlefield;
        r.tipRect = kRect;
        r.tipTitle = tip;
        r.tipLine.clear();
        r.tipLine2.clear();
    }
}

// MiniMapBattlefieldDropDown (dropKind 1) / ScorePlayerDropDown (dropKind 3) in kWinDropDown.
bool WorldScreen::drawBattlefieldMenu(UiRenderer& ui) {
    RaidBgUi& r = raidBg();
    const Font& fs = kit_.font(kFontSmall);
    const bool focusable = uiMode_ && !settings_.isOpen();
    // MiniMapBattlefieldDropDown_Initialize: per queue (a spacer between them) its name as the title, then LEAVE_QUEUE
    // (queued), ENTER_BATTLE + LEAVE_QUEUE (ready; no leave for an arena), LEAVE_BATTLEGROUND / LEAVE_ARENA (active).
    r.menuRows.clear();
    r.menuText.clear();
    if (r.dropKind == 1) {
        int shown = 0;
        for (int i = 0; i < pp::WorldSession::kBgQueueSlots; ++i) {
            const auto& q = session_.bgQueue(i);
            if (q.status == 0) continue;
            if (++shown > 1) {
                r.menuRows.push_back(0xFF);
                r.menuText.emplace_back();
            }
            const std::string name = battlefieldName(i);
            r.menuRows.push_back(0xFF);  // the title
            r.menuText.push_back(name);
            if (q.status == 1) {
                r.menuRows.push_back(static_cast<uint8_t>(i << 4 | 1));
                r.menuText.emplace_back(CLIENT_STR(LEAVE_QUEUE));
            } else if (q.status == 2) {
                r.menuRows.push_back(static_cast<uint8_t>(i << 4 | 2));
                r.menuText.emplace_back(CLIENT_STR(ENTER_BATTLE));
                if (q.arenaType == 0) {
                    r.menuRows.push_back(static_cast<uint8_t>(i << 4 | 1));
                    r.menuText.emplace_back(CLIENT_STR(LEAVE_QUEUE));
                }
            } else if (q.status == 3) {
                r.menuRows.push_back(static_cast<uint8_t>(i << 4 | 3));
                r.menuText.emplace_back(q.arenaType != 0 ? CLIENT_STR(LEAVE_ARENA) : CLIENT_STR(LEAVE_BATTLEGROUND));
            }
        }
    } else if (r.dropKind == 3) {  // ScorePlayerDropDown_Initialize: the name, PVP_REPORT_AFK, CANCEL
        r.menuRows.push_back(0xFF);
        r.menuText.push_back(r.tipTitle);
        r.menuRows.push_back(4);
        r.menuText.emplace_back(CLIENT_STR(PVP_REPORT_AFK));
        r.menuRows.push_back(5);
        r.menuText.emplace_back(CLIENT_STR(CANCEL));
    }
    if (r.menuRows.empty()) return true;
    const float rowH = static_cast<float>(std::max(12, fs.lineHeight()));
    const float border = std::round(15.0f * 0.6f);
    float maxW = 0.0f;
    for (const std::string& t : r.menuText) maxW = std::max(maxW, static_cast<float>(fs.measure(t.c_str())) + std::round(10.0f * 0.6f));
    const float w = maxW + std::round(25.0f * 0.6f), h = static_cast<float>(r.menuRows.size()) * rowH + 2.0f * border;
    // ToggleDropDownMenu(..., "MiniMapBattlefieldFrame", 0, -5): under the button.
    const Rect lr{std::clamp(r.dropKind == 1 ? 402.0f : r.tipRect.x, 2.0f, static_cast<float>(pp::kScreenW) - w - 2.0f),
                  std::clamp(r.dropKind == 1 ? 95.0f + 3.0f : r.tipRect.y + r.tipRect.h, 2.0f,
                             static_cast<float>(pp::kScreenH) - h - 2.0f),
                  w, h};
    if (kit_.has(kTexTipBg)) ui.tile(kit_.tex(kTexTipBg), Rect{lr.x + 3, lr.y + 3, lr.w - 6, lr.h - 6}, 0xE0FFFFFFu);
    else ui.fill(lr, 0xE0301717u);
    if (kit_.has(kTexTipBorder)) ui.nineSlice(kit_.tex(kTexTipBorder), lr);
    loadNpcArt(false);
    for (size_t i = 0; i < r.menuRows.size() && i < 8; ++i) {
        const Rect b{lr.x + std::round(15.0f * 0.6f), lr.y + border + static_cast<float>(i) * rowH, maxW, rowH};
        const bool title = r.menuRows[i] == 0xFF;
        const bool f = focusable && !title && winFocus_.addStop(kIdMmBgMenu0 + static_cast<uint32_t>(i), b);
        if (f && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, b, 0xFFFFFFFFu, Blend::Add);
        ui.textShadow(fs, b.x, b.y + std::floor((rowH - static_cast<float>(fs.lineHeight())) * 0.5f), r.menuText[i].c_str(),
                      title ? kColGold : kColWhite);
    }
    return true;
}

// --- WorldStateScoreFrame (window kWinScore) ----------------------------------------------------------------

namespace {

struct ScoreRow {
    const game::BgPlayerScore* p = nullptr;
    std::string name;
    int faction = -1;  // 0 Horde, 1 Alliance (the Lua's numbering), -1 unknown
    uint8_t cls = 0, race = 0, sex = 0;
};

}  // namespace

bool WorldScreen::drawScoreFrame(UiRenderer& ui) {
    PROF_SCOPE("bg.score");
    RaidBgUi& r = raidBg();
    if (!r.scTried) loadScoreArt(r);
    loadRaidBgData();
    loadNpcArt(false);
    if (!r.wsTried) {  // the column icons (ws_icons) come with the world state art
        r.wsTried = true;
        char path[256];
        pp::fsPath(path, sizeof(path), "data/ui.pak");
        pp::Pak pak;
        if (pak.open(path)) {
            raidBgLoad(pak, "bf_mm_a", r.mmIcon[0], r.wsBytes);
            raidBgLoad(pak, "bf_mm_h", r.mmIcon[1], r.wsBytes);
            raidBgLoad(pak, "bf_arena", r.mmIcon[2], r.wsBytes);
            raidBgLoad(pak, "ws_capbar", r.capBar, r.wsBytes);
            raidBgLoad(pak, "ws_icons", r.wsIcons, r.wsBytes);
            pak.close();
        }
    }
    if (!tabArtTried_) {
        tabArtTried_ = true;
        char path[256];
        pp::fsPath(path, sizeof(path), "data/ui.pak");
        pp::Pak pak;
        uint32_t bytes = 0;
        if (pak.open(path)) {
            raidBgLoad(pak, "tab_active", tabArt_[0], bytes);
            raidBgLoad(pak, "tab_inactive", tabArt_[1], bytes);
            pak.close();
        }
    }
    const bool focusable = uiMode_ && !settings_.isOpen() && !windowOpen(kWinBindConfirm);
    const Font& ft = kit_.font(kFontTiny);
    const Font& fn = kit_.font(kFontNormal);
    const auto& sb = session_.bgScoreboard();
    const bool isArena = sb.isArena;
    // GetNumBattlefieldStats / GetBattlefieldStatInfo: the battleground's WorldStateUI type-2 columns.
    std::vector<const RaidBgUi::Ws*> stats;
    for (const RaidBgUi::Ws& w : r.ws)
        if (w.type == 2 && w.map == session_.bgActiveMap() && stats.size() < 7) stats.push_back(&w);
    // WorldStateScoreFrame_Resize: 35 + name + class + 76 per column (6 base + 1 + the stats; arena 3).
    const int columns = (isArena ? 3 : 6) + 1 + static_cast<int>(stats.size());
    const float W = 35.0f + 175.0f + 16.0f + (isArena ? 43.0f : 0.0f) + static_cast<float>(columns) * 76.0f;
    // PSP: the drawn part (HitRectInsets 13 / 115) fits the screen's width; 0.5 at most, 0.55 high.
    const float sx = std::min(0.5f, 470.0f / (W - 128.0f)), sy = 0.55f;
    const float x0 = std::floor((static_cast<float>(pp::kScreenW) - (W - 128.0f) * sx) * 0.5f - 13.0f * sx);
    const float y0 = std::floor((static_cast<float>(pp::kScreenH) - 459.0f * sy) * 0.5f - 16.0f * sy);
    auto X = [&](float u) { return x0 + u * sx; };
    auto Y = [&](float u) { return y0 + u * sy; };
    // WorldStateScoreFrameTopBackground (BACKGROUND, TopLeft +5,-4, W-129 wide, its 256 texels repeated).
    if (r.scoreBg.pixels) {
        for (float u = 0.0f; u < W - 129.0f; u += 256.0f) {
            const float span = std::min(256.0f, W - 129.0f - u);
            ui.image(r.scoreBg, Rect{X(13.0f + u), Y(20.0f), span * sx, 64.0f * sy},
                     SrcRect{0, 0, static_cast<float>(r.scoreBg.uw) * span / 256.0f, static_cast<float>(r.scoreBg.uh)});
        }
    }
    // The frame: TopLeft at 8,-16 | the Top band stretched | TopRight (256), over Bot*; units 16..442 shown.
    if (r.score.pixels) {
        const float tu = static_cast<float>(r.score.uw) / 640.0f;  // texels per unit (build_ui_pak.py: 0.4 x 0.5)
        const float vh = 426.0f * static_cast<float>(r.score.uh) / 512.0f;
        ui.image(r.score, Rect{X(8), Y(16), 128.0f * sx, 426.0f * sy}, SrcRect{0, 0, 128.0f * tu, vh});
        ui.image(r.score, Rect{X(136), Y(16), (W - 136.0f - 256.0f) * sx, 426.0f * sy}, SrcRect{128.0f * tu, 0, 256.0f * tu, vh});
        ui.image(r.score, Rect{X(W - 256.0f), Y(16), 256.0f * sx, 426.0f * sy}, SrcRect{384.0f * tu, 0, 256.0f * tu, vh});
    } else {
        kit_.panel(ui, Rect{X(8), Y(16), (W - 8.0f - 30.0f) * sx, 426.0f * sy});
    }
    // WorldStateScoreFrameLabel (TOP -57,-21): STAT_TEMPLATE with the tab's name.
    const char* tabName = pp::cs(kTabs[std::clamp(r.scoreTab, 0, 2)]);
    ui.textShadow(fn, X(W * 0.5f - 57.0f), Y(21), fmtS(CLIENT_STR(STAT_TEMPLATE), tabName).c_str(), kColGold,
                  Align::Center);
    // WorldStateScoreFrameCloseButton (TOPRIGHT -111,-12).
    {
        const Rect cr{X(W - 127.0f) - 10.0f, Y(28.0f) - 10.0f, 20, 20};  // 32 at TOPRIGHT -111,-12: CENTER W-127,28
        const bool f = focusable && winFocus_.addStop(kIdScClose, cr);
        const pp::Texture& t = f && winFocus_.isPressed(kIdScClose) && npcArt_.closeDown.pixels ? npcArt_.closeDown : npcArt_.close;
        if (t.pixels) ui.image(t, cr);
        if (f) ui.fill(cr, 0x40FFFFFFu);
    }
    // The winner (WorldStateScoreWinnerFrame: TOPLEFT 17,-20 .. TOPRIGHT -141,-20, 16 high).
    if (sb.hasWinner) {
        const int w = sb.winner ? 1 : 0;
        const uint32_t band = isArena ? (w == 0 ? 0xFF1C9130u : 0xFF42B5D9u) : (w == 0 ? 0xFF2E1385u : 0xFF82421Cu);
        const uint32_t text = isArena ? (w == 0 ? 0xFF1AFF1Au : 0xFF00D1FFu) : (w == 0 ? 0xFF1A1AFFu : 0xFFF0AD00u);
        const Rect wb{X(17), Y(20), (W - 17.0f - 141.0f) * sx, 16.0f * sy};
        if (r.scoreHl.pixels) ui.image(r.scoreHl, wb, band);
        else ui.fill(wb, band & 0x80FFFFFFu);
        ui.textShadow(fn, wb.x + wb.w * 0.5f, wb.y + (wb.h - static_cast<float>(fn.lineHeight())) * 0.5f,
                      pp::cs(isArena ? kVictoryArena[w] : kVictory[w]), text, Align::Center);
    }
    // The columns (WorldStateScoreColumnTemplate, 75 x 30, GameFontHighlightSmall): name at 18,-48 (175), then KB
    // (+4), deaths / HK (-5 each), damage, healing, the stats (0 apart), honor (+5).
    struct Col {
        float x;
        const char* text;
        const char* tip;
        int key;  // the sort key: 0 name, 1 kb, 2 deaths, 3 hk, 4 damage, 5 healing, 6 honor, 10 + n stat n
    };
    std::vector<Col> cols;
    float cx = 18.0f + 175.0f + 4.0f;
    cols.push_back(Col{cx, CLIENT_STR(SCORE_KILLING_BLOWS), CLIENT_STR(KILLING_BLOW_TOOLTIP), 1});
    if (!isArena) {
        cx += 75.0f - 5.0f;
        cols.push_back(Col{cx, CLIENT_STR(DEATHS), CLIENT_STR(DEATHS_TOOLTIP), 2});
        cx += 75.0f - 5.0f;
        cols.push_back(Col{cx, CLIENT_STR(SCORE_HONORABLE_KILLS), CLIENT_STR(HONORABLE_KILLS_TOOLTIP), 3});
    }
    cx += 75.0f - 5.0f;
    cols.push_back(Col{cx, CLIENT_STR(SCORE_DAMAGE_DONE), CLIENT_STR(DAMAGE_DONE_TOOLTIP), 4});
    cx += 75.0f - 5.0f;
    cols.push_back(Col{cx, CLIENT_STR(SCORE_HEALING_DONE), CLIENT_STR(HEALING_DONE_TOOLTIP), 5});
    for (size_t k = 0; k < stats.size(); ++k) {
        cx += 75.0f;
        cols.push_back(Col{cx, stats[k]->text.c_str(), stats[k]->tip.c_str(), 10 + static_cast<int>(k)});
    }
    if (!isArena) {
        cx += 75.0f + 5.0f;
        cols.push_back(Col{cx, CLIENT_STR(SCORE_HONOR_GAINED), CLIENT_STR(HONOR_GAINED_TOOLTIP), 6});
    }
    uint32_t tipCol = 0;
    {  // NAME (its text LEFT +20)
        const Rect hr{X(18), Y(48), 175.0f * sx, 30.0f * sy};
        const bool f = focusable && winFocus_.addStop(kIdScColumn0, hr);
        ui.textShadow(ft, X(18 + 20), Y(48) + (hr.h - static_cast<float>(ft.lineHeight())) * 0.5f, CLIENT_STR(NAME),
                      f ? kColGold : kColWhite);
    }
    for (size_t k = 0; k < cols.size() && k < 15; ++k) {
        const Rect hr{X(cols[k].x), Y(48), 75.0f * sx, 30.0f * sy};
        const uint32_t id = kIdScColumn0 + 1u + static_cast<uint32_t>(k);
        const bool f = focusable && winFocus_.addStop(id, hr);
        std::vector<std::string> lines = wrapQuestText(ft, cols[k].text, 1e9f);
        float ty = hr.y + (hr.h - static_cast<float>(ft.lineHeight()) * static_cast<float>(lines.size())) * 0.5f;
        for (const std::string& l : lines) {
            ui.textShadow(ft, hr.x + hr.w * 0.5f, ty, l.c_str(), f ? kColGold : kColWhite, Align::Center);
            ty += static_cast<float>(ft.lineHeight());
        }
        if (f) {
            tipCol = id;
            r.tipRect = Rect{hr.x + ui.offsetX(), hr.y + ui.offsetY(), hr.w, hr.h};
            r.tipTitle = cols[k].tip;
        }
    }
    // GetBattlefieldScore: the rows (SetBattlefieldScoreFaction by the tab), SortBattlefieldScoreData.
    std::vector<ScoreRow> rows;
    for (const auto& ps : sb.players) {
        ScoreRow row;
        row.p = &ps;
        row.name = ps.name;
        if (row.name.empty())
            if (const std::string* n = session_.nameOf(ps.guid)) row.name = *n;
        session_.bgPlayerLook(ps.guid, row.race, row.sex, row.cls);
        if (ps.guid == session_.playerGuid() && session_.activeCharacter()) {
            row.race = static_cast<uint8_t>(session_.activeCharacter()->race);
            row.cls = static_cast<uint8_t>(session_.activeCharacter()->characterClass);
            if (row.name.empty()) row.name = name_;
        }
        if (ps.hasTeam) row.faction = ps.team;
        else if (row.race != 0) row.faction = allianceRace(row.race) ? 1 : 0;
        if (r.scoreTab == 1 && row.faction != 1) continue;
        if (r.scoreTab == 2 && row.faction != 0) continue;
        rows.push_back(std::move(row));
    }
    if (r.sortKey >= 0) {
        auto val = [&](const ScoreRow& a) -> int64_t {
            switch (r.sortKey) {
                case 1: return a.p->killingBlows;
                case 2: return a.p->deaths;
                case 3: return a.p->honorableKills;
                case 4: return a.p->damageDone;
                case 5: return a.p->healingDone;
                case 6: return a.p->bonusHonor;
                default: {
                    const size_t n = static_cast<size_t>(r.sortKey - 10);
                    return n < a.p->bgStats.size() ? a.p->bgStats[n].second : 0;
                }
            }
        };
        std::stable_sort(rows.begin(), rows.end(), [&](const ScoreRow& a, const ScoreRow& b) {
            if (r.sortKey == 0) return r.sortDesc ? a.name > b.name : a.name < b.name;
            return r.sortDesc ? val(a) > val(b) : val(a) < val(b);
        });
    }
    const int numScores = static_cast<int>(rows.size());
    r.scoreOffset = std::clamp(r.scoreOffset, 0, std::max(0, numScores - 20));
    const bool scroll = numScores > 20;
    const float rowW = W - (scroll ? 165.0f : 137.0f);
    float lastBottom = 78.0f;
    for (int i = 0; i < 20; ++i) {
        const int idx = r.scoreOffset + i;
        if (idx >= numScores) break;
        const ScoreRow& row = rows[static_cast<size_t>(idx)];
        const float ry = 78.0f + 16.0f * static_cast<float>(i);
        lastBottom = ry + 16.0f;
        const Rect rr{X(17), Y(ry), rowW * sx, 16.0f * sy};
        // FactionLeft / FactionRight (WorldStateFinalScore-Highlight, the right one mirrored to the row's end).
        if (row.faction >= 0) {
            const uint32_t band = isArena ? (row.faction == 0 ? 0xFF1C9130u : 0xFF42B5D9u)
                                          : (row.faction == 0 ? 0xFF2E1385u : 0xFF82421Cu);
            if (r.scoreHl.pixels) {
                const float lw = std::min(256.0f, rowW);
                ui.image(r.scoreHl, Rect{rr.x, rr.y, lw * sx, rr.h}, band);
                if (rowW > 256.0f)
                    ui.image(r.scoreHl, Rect{rr.x + lw * sx, rr.y, (rowW - 256.0f) * sx, rr.h},
                             SrcRect{static_cast<float>(r.scoreHl.uw), 0, -static_cast<float>(r.scoreHl.uw),
                                     static_cast<float>(r.scoreHl.uh)},
                             band);  // TexCoords 1..0: mirrored to the row's end
            } else {
                ui.fill(rr, band & 0x60FFFFFFu);
            }
        }
        const uint32_t id = kIdScRow0 + static_cast<uint32_t>(i);
        const bool f = focusable && winFocus_.addStop(id, rr);
        // The class icon (Icons-Classes, 16 at LEFT), the name (GameFontNormal at 20,1) in the faction's colour, the
        // player's own in gold (battlegrounds).
        int col = 0, crow = 0;
        auto classCell2 = [](uint8_t cls, int& c, int& rw) {
            switch (cls) {
                case 1: c = 0, rw = 0; return true;
                case 8: c = 1, rw = 0; return true;
                case 4: c = 2, rw = 0; return true;
                case 11: c = 3, rw = 0; return true;
                case 3: c = 0, rw = 1; return true;
                case 7: c = 1, rw = 1; return true;
                case 5: c = 2, rw = 1; return true;
                case 9: c = 3, rw = 1; return true;
                case 2: c = 0, rw = 2; return true;
                case 6: c = 1, rw = 2; return true;
                default: return false;
            }
        };
        if (r.classes.pixels && classCell2(row.cls, col, crow)) {
            const float c = static_cast<float>(r.classes.uw) * 0.25f;
            ui.image(r.classes, Rect{rr.x, rr.y, 16.0f * sx, 16.0f * sy},
                     SrcRect{static_cast<float>(col) * c, static_cast<float>(crow) * c, c, c});
        }
        uint32_t nameCol = row.faction == 0 ? (isArena ? 0xFF1AFF1Au : 0xFF1A1AFFu)
                           : row.faction == 1 ? (isArena ? 0xFF00D1FFu : 0xFFF0AD00u)
                                              : kColWhite;
        if (!isArena && row.p->guid == session_.playerGuid()) nameCol = 0xFF00D1FFu;
        const float ty = rr.y + (rr.h - static_cast<float>(ft.lineHeight())) * 0.5f;
        ui.pushClip(Rect{X(17 + 20), rr.y, 190.0f * sx, rr.h});
        ui.textShadow(ft, X(17 + 20), ty, row.name.c_str(), f ? kColWhite : nameCol);
        ui.popClip();
        // The values, centred under their columns (GameFontNormalSmall, gold).
        for (const Col& c : cols) {
            const float vx = X(c.x + 75.0f * 0.5f);
            std::string v;
            uint32_t icon = ~0u;
            switch (c.key) {
                case 1: v = std::to_string(row.p->killingBlows); break;
                case 2: v = std::to_string(row.p->deaths); break;
                case 3: v = std::to_string(row.p->honorableKills); break;
                case 4: v = std::to_string(row.p->damageDone); break;
                case 5: v = std::to_string(row.p->healingDone); break;
                case 6: v = std::to_string(row.p->bonusHonor); break;
                default: {
                    const size_t n = static_cast<size_t>(c.key - 10);
                    const uint32_t data = n < row.p->bgStats.size() ? row.p->bgStats[n].second : 0;
                    const RaidBgUi::Ws* w = n < stats.size() ? stats[n] : nullptr;
                    if (w && w->hasIcon) {  // "x n" with the column's icon..faction when above 0
                        if (data > 0) {
                            v = CLIENT_FMT(FLAG_COUNT_TEMPLATE, static_cast<int>(data));
                            const int fa = row.faction == 1 ? 1 : 0;
                            if (w->rowIcon[fa] >= 0) icon = static_cast<uint32_t>(w->rowIcon[fa]);
                        }
                    } else {
                        v = std::to_string(data);
                    }
                    break;
                }
            }
            ui.textShadow(ft, vx, ty, v.c_str(), kColGold, Align::Center);
            if (icon != ~0u && r.wsIcons.pixels) {  // $parentColumnNIcon: 16 at the text's LEFT (0,-1)
                const float tw = static_cast<float>(ft.measure(v.c_str()));
                const float cell = 32.0f * static_cast<float>(r.wsIcons.uw) / 256.0f;
                ui.image(r.wsIcons, Rect{vx - tw * 0.5f - 16.0f * sx, rr.y, 16.0f * sx, 16.0f * sy},
                         SrcRect{static_cast<float>(icon % 8) * cell, static_cast<float>(icon / 8) * cell, cell, cell});
            }
        }
        if (f) {  // the name's tooltip: race class (ruRU, the player's sex)
            r.tipRect = Rect{rr.x + ui.offsetX(), rr.y + ui.offsetY(), rr.w, rr.h};
            r.tipTitle = row.name;
            const int fem = row.sex == 1 ? 1 : 0;
            std::string race = row.race < 12 ? r.raceName[row.race][fem] : std::string();
            if (race.empty() && row.race < 12) race = r.raceName[row.race][0];
            std::string cls = row.cls < 12 ? r.className[row.cls][fem] : std::string();
            if (cls.empty() && row.cls < 12) cls = r.className[row.cls][0];
            r.tipLine = race + " " + cls;
            tipCol = id;
        }
    }
    // The scroll buttons (FauxScrollFrame, more than 20 rows).
    if (scroll) {
        const Rect up{X(W - 148.0f), Y(76), 16.0f * sx * 1.6f, 16.0f * sy * 1.6f};
        const Rect dn{X(W - 148.0f), Y(76.0f + 361.0f - 26.0f), up.w, up.h};
        const bool fu = focusable && r.scoreOffset > 0 && winFocus_.addStop(kIdScUp, up);
        const bool fd = focusable && r.scoreOffset + 20 < numScores && winFocus_.addStop(kIdScDown, dn);
        if (npcArt_.scrUp.pixels) ui.image(r.scoreOffset > 0 ? npcArt_.scrUp : npcArt_.scrUpDis, up);
        if (npcArt_.scrDown.pixels) ui.image(r.scoreOffset + 20 < numScores ? npcArt_.scrDown : npcArt_.scrDownDis, dn);
        if (fu) ui.fill(up, 0x40FFFFFFu);
        if (fd) ui.fill(dn, 0x40FFFFFFu);
    }
    // WorldStateScorePlayerCount (under the last row, LEFT +15), WorldStateScoreBattlegroundRunTime (RIGHT -20).
    if (!isArena) {
        int a = 0, h = 0;
        for (const auto& ps : sb.players) {
            uint8_t race = 0, sex = 0, cls = 0;
            session_.bgPlayerLook(ps.guid, race, sex, cls);
            if (ps.guid == session_.playerGuid() && session_.activeCharacter())
                race = static_cast<uint8_t>(session_.activeCharacter()->race);
            if (race == 0) continue;
            if (allianceRace(race)) ++a;
            else ++h;
        }
        const std::string ba = pp::text::plural(CLIENT_FMT(PLAYER_COUNT_ALLIANCE, a)), bh = pp::text::plural(CLIENT_FMT(PLAYER_COUNT_HORDE, h));
        std::string count = a > 0 && h > 0 ? ba + " / " + bh : a > 0 ? ba : h > 0 ? bh : "";
        if (!count.empty()) ui.textShadow(ft, X(17 + 15), Y(lastBottom + 6.0f), count.c_str(), kColGold);
    }
    if (const uint32_t start = session_.bgStartMs()) {
        const std::string t = std::string(CLIENT_STR(TIME_ELAPSED)) + " " +
                              secondsToTime(static_cast<long>((pp::logUptimeMs() - start) / 1000u), true);
        ui.textShadow(ft, X(17.0f + rowW - 20.0f), Y(lastBottom + 7.0f), t.c_str(), kColGold, Align::Right);
    }
    // The winner: LEAVE_BATTLEGROUND (BOTTOM -30,76, 153 x 21) and TIME_TO_PORT + the shutdown timer (BOTTOMLEFT 20,77).
    if (sb.hasWinner) {
        const Rect lb{X(W * 0.5f - 30.0f - 76.5f), Y(512.0f - 76.0f - 21.0f), 153.0f * sx, 21.0f * sy};
        drawPanelButton(ui, kIdScLeave, lb, isArena ? CLIENT_STR(LEAVE_ARENA) : CLIENT_STR(LEAVE_BATTLEGROUND), true,
                        focusable);
        std::string t = isArena ? CLIENT_STR(TIME_TO_PORT_ARENA) : CLIENT_STR(TIME_TO_PORT);
        if (const uint32_t exp = session_.bgExpireMs()) {
            const int32_t left = static_cast<int32_t>(exp - pp::logUptimeMs());
            t += " " + secondsToTime(left > 0 ? left / 1000 : 0);
        }
        ui.textShadow(ft, X(20), Y(512.0f - 77.0f) - static_cast<float>(ft.lineHeight()), t.c_str(), kColWhite);
    }
    // WorldStateScoreFrameTab1..3 (BOTTOMLEFT 10,69; -16 apart), not for an arena.
    if (!isArena) {
        const float th = 32.0f * sy;
        float tx = X(10);
        for (int k = 0; k < 3; ++k)
            tx += drawFrameTab(ui, kIdScTab0 + static_cast<uint32_t>(k), tx, Y(512.0f - 69.0f), th, pp::cs(kTabs[k]),
                               r.scoreTab == k, focusable) - 16.0f * sx;
    }
    if (tipCol) r.tipId = tipCol;  // the overlay draws it while that widget keeps the focus
    return true;
}

// --- Actions ----------------------------------------------------------------------------------------------------

bool WorldScreen::battlegroundAction(uint32_t act, bool alt) {
    RaidBgUi& r = raidBg();
    if (act == kIdBfClose || act == kIdBfCancel) {  // HideParentPanel -> OnHide: CloseBattlefield
        if (alt) return true;
        closeWindow(kWinBattlefield);
        playSoundByName("igCharacterInfoClose");
        return true;
    }
    if (act == kIdBfJoin || act == kIdBfGroupJoin) {  // BattlefieldFrameJoinButton_OnClick
        if (alt) return true;
        session_.joinBattlefield(r.selected, act == kIdBfGroupJoin);
        closeWindow(kWinBattlefield);
        playSoundByName("igCharacterInfoClose");
        return true;
    }
    if (act == kIdBfUp || act == kIdBfDown) {
        r.offset += act == kIdBfUp ? -1 : 1;
        return true;
    }
    if (act >= kIdBfZone0 && act < kIdBfZone0 + 5) {  // BattlefieldButton_OnClick: SetSelectedBattlefield
        r.selected = r.offset + static_cast<int>(act - kIdBfZone0);
        return true;
    }
    for (int s = 0; s < 2; ++s) {
        const uint32_t base = static_cast<uint32_t>(s) * 4u;
        const uint8_t w = s == 0 ? kWinBgConfirm : kWinBgConfirm2;
        if (act == kIdBcEnter0 + base) {  // OnAccept: AcceptBattlefieldPort(data, 1) (a refusal keeps the popup)
            if (alt) return true;
            const auto before = session_.bgQueue(s).status;
            session_.acceptBattlefieldPort(s, true);
            if (session_.bgQueue(s).status != before) {
                closeWindow(w);
                r.confirmShown[s] = false;
            }
            return true;
        }
        if (act == kIdBcLeave0 + base) {  // OnCancel: AcceptBattlefieldPort(data, 0)
            if (alt) return true;
            session_.acceptBattlefieldPort(s, false);
            closeWindow(w);
            r.confirmShown[s] = false;
            return true;
        }
        if (act == kIdBcClose0 + base) {  // closeButtonIsHide: hidden, the queue stays
            if (alt) return true;
            closeWindow(w);
            r.confirmShown[s] = false;
            return true;
        }
    }
    if (act == kIdMmBattlefield) {
        // OnClick: in a battle the left click toggles the score; the right click (or anything not in a battle the
        // left click does nothing) - the dropdown.
        bool active = false;
        for (int i = 0; i < pp::WorldSession::kBgQueueSlots; ++i) active = active || session_.bgQueue(i).status == 3;
        if (!alt && active) {
            if (windowOpen(kWinScore)) closeWindow(kWinScore);
            else if (session_.activeBgSlot() >= 0 && !session_.bgScoreboard().isArena) openWindow(kWinScore);
            return true;
        }
        if (!alt) return true;
        if (windowOpen(kWinDropDown) && r.dropKind == 1) {
            closeWindow(kWinDropDown);
            r.dropKind = 0;
            return true;
        }
        if (windowOpen(kWinDropDown)) closeWindow(kWinDropDown);
        r.dropKind = 1;
        r.dropFocusMs = pp::logUptimeMs();
        openWindow(kWinDropDown);
        winFocus_.setFocus(kIdMmBgMenu0 + 1);
        playSoundByName("igMainMenuOpen");
        return true;
    }
    if (act >= kIdMmBgMenu0 && act < kIdMmBgMenu0 + 8) {
        if (alt) return true;
        const size_t i = act - kIdMmBgMenu0;
        if (i >= r.menuRows.size() || r.menuRows[i] == 0xFF) return true;
        const uint8_t row = r.menuRows[i];
        if (r.dropKind == 1) {
            const int slot = row >> 4;
            switch (row & 0x0F) {
                case 1: session_.acceptBattlefieldPort(slot, false); break;  // LEAVE_QUEUE
                case 2: session_.acceptBattlefieldPort(slot, true); break;   // ENTER_BATTLE
                case 3: session_.leaveBattlefield(); break;                  // LEAVE_BATTLEGROUND / LEAVE_ARENA
                default: break;
            }
        } else if (r.dropKind == 3 && row == 4) {  // ScorePlayerDropDown_OnClick: ReportPlayerIsPVPAFK
            for (const auto& ps : session_.bgScoreboard().players)
                if (ps.guid != 0 && (ps.name == r.tipTitle || (session_.nameOf(ps.guid) && *session_.nameOf(ps.guid) == r.tipTitle)))
                    session_.reportPvpAfk(ps.guid);
        }
        playSoundByName("UChatScrollButton");
        closeWindow(kWinDropDown);
        r.dropKind = 0;
        return true;
    }
    if (act == kIdScClose) {
        if (!alt) closeWindow(kWinScore);
        return true;
    }
    if (act == kIdScLeave) {  // LeaveBattlefield
        if (!alt) session_.leaveBattlefield();
        return true;
    }
    if (act >= kIdScTab0 && act < kIdScTab0 + 3) {  // WorldStateScoreFrameTab_OnClick
        if (!alt) {
            r.scoreTab = static_cast<int>(act - kIdScTab0);
            r.scoreOffset = 0;
            playSoundByName("igCharacterInfoTab");
        }
        return true;
    }
    if (act == kIdScUp || act == kIdScDown) {
        r.scoreOffset += act == kIdScUp ? -1 : 1;
        return true;
    }
    if (act >= kIdScColumn0 && act < kIdScColumn0 + 16) {  // SortBattlefieldScoreData(sortType)
        if (alt) return true;
        const int k = static_cast<int>(act - kIdScColumn0);
        // The header order: NAME, then the columns as drawn (KB, deaths, HK, damage, healing, stats, honor).
        const bool isArena = session_.bgScoreboard().isArena;
        int key = 0;
        if (k > 0) {
            std::vector<int> keys = {1};
            if (!isArena) {
                keys.push_back(2);
                keys.push_back(3);
            }
            keys.push_back(4);
            keys.push_back(5);
            int stats = 0;
            for (const RaidBgUi::Ws& w : r.ws)
                if (w.type == 2 && w.map == session_.bgActiveMap() && stats < 7) keys.push_back(10 + stats++);
            if (!isArena) keys.push_back(6);
            key = static_cast<size_t>(k - 1) < keys.size() ? keys[static_cast<size_t>(k - 1)] : -1;
        }
        if (key == r.sortKey) r.sortDesc = !r.sortDesc;
        else {
            r.sortKey = key;
            r.sortDesc = key != 0;
        }
        playSoundByName("igMainMenuOptionCheckBoxOn");
        return true;
    }
    if (act >= kIdScRow0 && act < kIdScRow0 + 20) {
        // ScorePlayer_OnMouseUp: the right click on another player of the raid - ScorePlayerDropDown.
        if (!alt) return true;
        const std::string name = r.tipTitle;
        bool inRaid = false;
        for (const auto& m : session_.getPartyData().members) inRaid = inRaid || m.name == name;
        if (name.empty() || name == name_ || !inRaid) return true;
        if (windowOpen(kWinDropDown)) closeWindow(kWinDropDown);
        r.dropKind = 3;
        r.dropFocusMs = pp::logUptimeMs();
        openWindow(kWinDropDown);
        winFocus_.setFocus(kIdMmBgMenu0 + 1);
        return true;
    }
    return false;
}

}  // namespace wowee::ui::psp
