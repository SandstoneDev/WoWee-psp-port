// The hub of the raid / battleground frames (RaidBgUi, its data, the per-frame update, the
// window / action / tooltip dispatch) and the world states on the screen, as the original ruRU 3.3.5a client shows
// them (WorldStateFrame.xml / .lua):
//   - WorldStateAlwaysUpFrame (TOP of UIParent -5,-15): per WorldStateUI record of the map (and its zone / area) whose
//     state is above 0 (the state variable's value, 1 without one; GetWorldStateUIInfo), an AlwaysUpFrame (45 x 24,
//     the first at TOP -23,-20, the next under it): the icon (42 at LEFT -6), the text (GameFontNormalSmall at the
//     icon's RIGHT -12,+10) with "%NNNNw" = world state NNNN, "%NNNNk" = the time left to the realm time in it
//     (CMSG_WORLD_STATE_UI_TIMER_UPDATE), "|4one:few:many;"; the dynamic icon (32 at RIGHT +40,+9) for state 2
//     (flashing - UIFrameFlash 0.5 / 0.5) and 3; their tooltips. Type 1 (world PvP) only in the record's zone (PSP:
//     displayWorldPVPObjectives "2" without IsSubZonePVPPOI - the zone stands for the subzone); type 2 is the score
//     frame's columns; records of a phase are not shown (PSP: no phases). PSP: at the unit frames' 0.6, the first line
//     under the unit frames (y 64; the original's 35 units from the top is the target frame here);
//   - WorldStateCaptureBar (ExtendedUI CAPTUREPOINT, 173 x 26 under MinimapCluster's BOTTOMRIGHT, the next under it):
//     CaptureBar_Update(value, neutralPercent) - the Alliance / Horde / neutral bars, the lines, the indicator at
//     25 + 124 (1 - value / 100), its arrow toward the last move, the faction icon's highlight - at the minimap
//     cluster's scale (76 / 140).
// Data: ui.pak raw "bg_data" (BattlemasterList, the battlegrounds' Map.dbc, ChrClasses, ChrRaces - read once) and
// "ws_ui" (WorldStateUI.dbc - the current map's records, read again on another map), "ws_icons".
#include "ui/psp/ui_world_raid_bg.hpp"

#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_prof.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

constexpr float kS = 0.6f;               // the always-up lines (the unit frames' scale)
constexpr float kMm = 76.0f / 140.0f;    // the minimap cluster's scale (build_ui_pak.py MM_K)
constexpr float kAlwaysUpTop = 64.0f;    // PSP: under the unit frames (TargetFrame's bottom is at 62)
constexpr float kClusterBottom = 104.0f; // MinimapCluster (192 units at the cluster's scale) BOTTOMRIGHT

std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> out;
    size_t a = 0;
    for (;;) {
        const size_t b = line.find('\t', a);
        out.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return out;
}

std::string unescape(const std::string& s) {  // build_ui_pak.py esc: a newline as \n
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n') {
            out.push_back('\n');
            ++i;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

uint32_t num(const std::string& s) { return static_cast<uint32_t>(std::strtoul(s.c_str(), nullptr, 10)); }
int16_t cell(const std::string& s) { return static_cast<int16_t>(std::strtol(s.c_str(), nullptr, 10)); }

}  // namespace

// --- RaidBgUi ------------------------------------------------------------------------------------------------

WorldScreen::RaidBgUi::~RaidBgUi() {
    raidBgFree({&rfFrame, &rfGroup, &rfBtn, &rfBtnHl, &rfTab, &rfClasses, &rfPets, &rfMtBtn, &rfMaBtn});
    raidBgFree({&leader, &master, &assist, &mt, &ma, &hpFrame, &rc[0], &rc[1], &rc[2], &rtIcons, &rcFrame});
    raidBgFree({&bfFrame, &bfHl, &bfCur[0], &bfCur[1], &bfArena, &bfConfirm});
    raidBgFree({&mmIcon[0], &mmIcon[1], &mmIcon[2], &capBar, &wsIcons, &score, &scoreBg, &scoreHl, &classes});
}

void WorldScreen::destroyRaidBgUi(RaidBgUi* p) { delete p; }

WorldScreen::RaidBgUi& WorldScreen::raidBg() {
    if (!rbg_) rbg_ = std::unique_ptr<RaidBgUi, void (*)(RaidBgUi*)>(new RaidBgUi, &WorldScreen::destroyRaidBgUi);
    return *rbg_;
}

// bg_data once; ws_ui for the world states' map (again when it changes).
void WorldScreen::loadRaidBgData() {
    RaidBgUi& r = raidBg();
    const uint32_t map = session_.worldStateMap();
    if (r.bgDataTried && r.wsMap == map) return;
    char path[256];
    pp::fsPath(path, sizeof(path), "data/ui.pak");
    pp::Pak pak;
    if (!pak.open(path)) {
        r.bgDataTried = true;
        r.wsMap = map;
        return;
    }
    auto text = [&](const char* name) {
        std::string out;
        const pp::PakEntry* e = pak.find(name);
        if (void* b = e ? pak.load(e) : nullptr) {
            out.assign(static_cast<const char*>(b), e->size);
            pp::pakFree(b);
        }
        return out;
    };
    auto lines = [](const std::string& all, auto&& fn) {
        size_t a = 0;
        while (a < all.size()) {
            size_t b = all.find('\n', a);
            if (b == std::string::npos) b = all.size();
            if (b > a) fn(all.substr(a, b - a));
            a = b + 1;
        }
    };
    if (!r.bgDataTried) {
        r.bgDataTried = true;
        lines(text("bg_data"), [&](const std::string& l) {
            const std::vector<std::string> f = splitTabs(l);
            if (f.empty()) return;
            if (f[0] == "B" && f.size() >= 8) {
                RaidBgUi::Bg b;
                b.id = num(f[1]);
                b.type = num(f[2]);
                b.maxGroup = num(f[3]);
                b.minLevel = num(f[4]);
                b.maxLevel = num(f[5]);
                size_t a = 0;
                while (a < f[6].size()) {
                    size_t c = f[6].find(',', a);
                    if (c == std::string::npos) c = f[6].size();
                    if (c > a) b.maps.push_back(num(f[6].substr(a, c - a)));
                    a = c + 1;
                }
                b.name = f[7];
                if (f.size() >= 9) b.holidayWs = num(f[8]);
                r.bgs.push_back(std::move(b));
            } else if (f[0] == "M" && f.size() >= 6) {
                RaidBgUi::Map m;
                m.id = num(f[1]);
                m.area = num(f[2]);
                m.name = f[3];
                m.desc[0] = unescape(f[4]);
                m.desc[1] = unescape(f[5]);
                r.maps.push_back(std::move(m));
            } else if (f[0] == "C" && f.size() >= 4) {
                const uint32_t id = num(f[1]);
                if (id < 12) {
                    r.className[id][0] = f[2];
                    r.className[id][1] = f[3];
                }
            } else if (f[0] == "R" && f.size() >= 5) {
                const uint32_t id = num(f[1]);
                if (id < 12) {
                    r.raceName[id][0] = f[2];
                    r.raceName[id][1] = f[3];
                    r.raceFaction[id] = static_cast<int8_t>(num(f[4]));
                }
            }
        });
    }
    if (r.wsMap != map) {
        r.wsMap = map;
        r.ws.clear();
        const std::string mapText = std::to_string(map);
        lines(text("ws_ui"), [&](const std::string& l) {
            const std::vector<std::string> f = splitTabs(l);
            if (f.size() < 19 || f[1] != mapText) return;
            RaidBgUi::Ws w;
            w.id = num(f[0]);
            w.map = num(f[1]);
            w.area = num(f[2]);
            w.phase = num(f[3]);
            w.type = num(f[4]);
            w.var = num(f[5]);
            w.icon = cell(f[6]);
            w.dynIcon = cell(f[7]);
            w.dynFlash = cell(f[8]);
            w.rowIcon[0] = cell(f[9]);
            w.rowIcon[1] = cell(f[10]);
            w.hasIcon = f[11] == "1";
            w.capture = f[12] == "1";
            w.ev[0] = num(f[13]);
            w.ev[1] = num(f[14]);
            w.ev[2] = num(f[15]);
            w.text = unescape(f[16]);
            w.tip = unescape(f[17]);
            w.dynTip = unescape(f[18]);
            r.ws.push_back(std::move(w));
        });
    }
    pak.close();
    if (++r.logs <= 20)
        PSP_LOGI(pp::tag::Gfx, "raidbg_data bgs=%u maps=%u ws=%u map=%u", static_cast<unsigned>(r.bgs.size()),
                 static_cast<unsigned>(r.maps.size()), static_cast<unsigned>(r.ws.size()), static_cast<unsigned>(map));
}

// --- The hub (drawWindows / updateWindows) ---------------------------------------------------------------------

void WorldScreen::updateRaidBg() {
    session_.raidTick();  // the initiator's ready check end, the icons' end
    session_.bgTick();    // the battleground chat filter, the shutdown lines
    // Nothing to do (and nothing made) out of a group, a queue, a battle and with no frame of ours open.
    bool queue = false;
    for (int i = 0; i < pp::WorldSession::kBgQueueSlots; ++i) queue = queue || session_.bgQueue(i).status != 0;
    const bool any = rbg_ || session_.isInGroup() || session_.anyRaidMark() || queue ||
                     session_.battlefieldListSeq() != 0 || session_.readyCheckSeq() != 0 ||
                     session_.worldStatesVersion() != 0;
    if (!any) return;
    RaidBgUi& r = raidBg();
    updateRaid();
    updateBattleground();
    // PSP: the cursor comes up (SELECT) with one of our popups on top - the focus on its first button (as PARTY_INVITE).
    if (uiMode_ && !r.lastUi && !windows_.empty()) {
        const uint8_t top = windows_.back();
        if (top == kWinReadyCheck) winFocus_.setFocus(kIdRcYes);
        else if (top == kWinBgConfirm) winFocus_.setFocus(kIdBcEnter0);
        else if (top == kWinBgConfirm2) winFocus_.setFocus(kIdBcEnter0 + 4u);
    }
    r.lastUi = uiMode_;
    // The world state art (the icons atlas, the capture bar, the minimap icon): while a queue or a world state line
    // could be up; the map's records follow the world states' map.
    const bool wsArt = queue || r.wsVisible;
    if (session_.worldStatesVersion() != 0 && r.wsMap != session_.worldStateMap()) loadRaidBgData();
    if (!wsArt && r.wsTried) {
        raidBgFree({&r.mmIcon[0], &r.mmIcon[1], &r.mmIcon[2], &r.capBar, &r.wsIcons});
        if (r.wsBytes) PSP_LOGI(pp::tag::Gfx, "worldstate_art_free kb=%u", static_cast<unsigned>(r.wsBytes / 1024u));
        r.wsTried = false;
        r.wsBytes = 0;
    }
}

bool WorldScreen::drawRaidBgWindow(UiRenderer& ui, uint8_t w) {
    switch (w) {
        case kWinRaid: return drawRaidFrame(ui);
        case kWinReadyCheck: return drawReadyCheckFrame(ui);
        case kWinPullouts: drawRaidPullouts(ui); return true;
        case kWinBattlefield: return drawBattlefieldFrame(ui);
        case kWinBgConfirm: return drawBattlefieldConfirm(ui, 0);
        case kWinBgConfirm2: return drawBattlefieldConfirm(ui, 1);
        case kWinScore: return drawScoreFrame(ui);
        case kWinDropDown: {
            if (!rbg_) return true;
            if (rbg_->dropKind == 2) return drawPulloutMenu(ui);
            return drawBattlefieldMenu(ui);
        }
        default: return false;
    }
}

bool WorldScreen::raidBgAction(uint32_t act) {
    if (act < 0x7780 || act > 0x78FF) return false;
    if (raidFrameAction(act, false)) return true;
    return battlegroundAction(act, false) || true;  // our range: nothing else takes these ids
}

bool WorldScreen::raidBgAltAction(uint32_t alt) {
    if (alt < 0x7780 || alt > 0x78FF) return false;
    if (raidFrameAction(alt, true)) return true;
    return battlegroundAction(alt, true) || true;
}

// --- GameTooltip of our widgets (after the windows) -------------------------------------------------------------

void WorldScreen::drawRaidBgOverlay(UiRenderer& ui) {
    if (!rbg_ || !uiMode_ || settings_.isOpen()) return;
    RaidBgUi& r = *rbg_;
    const uint32_t f = winFocus_.focused();
    if (f == 0 || (f != r.tipId && f != r.bfTipId)) return;
    // The lines: the title (white, GameFontNormal), the others (GameFontNormalSmall) wrapped at 220 px.
    struct L {
        std::string text;
        uint32_t color;
        bool title;
    };
    std::vector<L> lines;
    const Font& fn = kit_.font(kFontNormal);
    const Font& fs = kit_.font(kFontSmall);
    constexpr float kMaxW = 220.0f;
    auto add = [&](const std::string& t, uint32_t color, bool title) {
        if (t.empty()) return;
        for (const std::string& l : wrapQuestText(title ? fn : fs, t, kMaxW)) lines.push_back(L{l, color, title});
    };
    if (f == r.bfTipId) {  // GameTooltip_SetDefaultAnchor: the title white, the newbie tip gold
        add(r.tipTitle, kColWhite, true);
        add(r.tipLine, kColGold, false);
    } else {
        add(r.tipTitle, kColWhite, true);
        add(r.tipLine, r.tipLineColor ? r.tipLineColor : kColWhite, false);
        add(r.tipLine2, r.tipLine2Color ? r.tipLine2Color : kColGold, false);
    }
    if (lines.empty()) return;
    float w = 0.0f, h = 10.0f;
    for (const L& l : lines) {
        w = std::max(w, static_cast<float>((l.title ? fn : fs).measure(l.text.c_str())));
        h += static_cast<float>((l.title ? fn : fs).lineHeight());
    }
    w += 16.0f;
    // Beside the widget (right, else left), kept on the screen.
    Rect t{r.tipRect.x + r.tipRect.w + 4.0f, r.tipRect.y, w, h};
    if (t.x + t.w > static_cast<float>(pp::kScreenW) - 2.0f) t.x = r.tipRect.x - 4.0f - w;
    t.x = std::clamp(t.x, 2.0f, static_cast<float>(pp::kScreenW) - w - 2.0f);
    t.y = std::clamp(t.y, 2.0f, static_cast<float>(pp::kScreenH) - h - 2.0f);
    if (kit_.has(kTexTipBg)) ui.tile(kit_.tex(kTexTipBg), Rect{t.x + 3, t.y + 3, t.w - 6, t.h - 6}, 0xE0FFFFFFu);
    else ui.fill(t, 0xE0100000u);
    if (kit_.has(kTexTipBorder)) ui.nineSlice(kit_.tex(kTexTipBorder), t);
    float y = t.y + 5.0f;
    for (const L& l : lines) {
        const Font& font = l.title ? fn : fs;
        ui.text(font, t.x + 8.0f, y, l.text.c_str(), l.color);
        y += static_cast<float>(font.lineHeight());
    }
}

// --- HUD (drawUnitFrames' "frames" pass) ----------------------------------------------------------------------

void WorldScreen::drawRaidBgHud(UiRenderer& ui) {
    if (!rbg_) return;
    PROF_SCOPE("hud.raidbg");
    drawRaidMarks(ui);
    drawWorldStates(ui);
}

void WorldScreen::drawWorldStates(UiRenderer& ui) {
    RaidBgUi& r = *rbg_;
    r.wsVisible = false;
    const uint32_t map = session_.worldStateMap();
    if (session_.worldStatesVersion() == 0 || r.wsMap != map || r.ws.empty()) return;
    const uint32_t zone = session_.worldStateZoneId(), area = session_.worldStateArea();
    const bool inPvp = session_.activeBgSlot() >= 0;
    // GetWorldStateUIInfo's records: the map's (its zone's / area's; no phases on the PSP), state above 0; type 1
    // (world PvP) in its zone only, type 2 is the score frame's.
    auto shownRecord = [&](const RaidBgUi::Ws& w, uint32_t& state) {
        if (w.map != map || w.type == 2 || w.phase != 0) return false;
        if (w.area != 0 && w.area != zone && w.area != area) return false;
        if (w.type == 1 && !inPvp && w.area == 0) return false;  // displayWorldPVPObjectives "2": PSP - in its zone
        state = w.var == 0 ? 1u : session_.worldState(w.var);
        return state != 0;
    };
    {
        bool any = false;
        uint32_t st = 0;
        for (const RaidBgUi::Ws& w : r.ws) any = any || shownRecord(w, st);
        if (!any) return;
    }
    r.wsVisible = true;
    if (!r.wsTried) {
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
        PSP_LOGI(pp::tag::Gfx, "worldstate_art icons=%d bar=%d kb=%u", r.wsIcons.pixels ? 1 : 0, r.capBar.pixels ? 1 : 0,
                 static_cast<unsigned>(r.wsBytes / 1024u));
    }
    const bool focusable = uiMode_ && !settings_.isOpen();
    const Font& fs = kit_.font(kFontSmall);
    const uint32_t now = pp::logUptimeMs();
    const float iconCell = r.wsIcons.pixels ? 32.0f * static_cast<float>(r.wsIcons.uw) / 256.0f : 0.0f;
    auto icon = [&](int16_t c, const Rect& dst, uint32_t col, Blend b) {
        if (c < 0 || !r.wsIcons.pixels) return;
        ui.image(r.wsIcons, dst,
                 SrcRect{static_cast<float>(c % 8) * iconCell, static_cast<float>(c / 8) * iconCell, iconCell, iconCell}, col, b);
    };
    // The realm's time now (the "%k" timers): SMSG_WORLD_STATE_UI_TIMER_UPDATE + the time since it came.
    auto serverNow = [&](bool& ok) -> int64_t {
        uint32_t t = 0, at = 0;
        ok = session_.worldStateServerTime(t, at);
        return ok ? static_cast<int64_t>(t) + static_cast<int64_t>((now - at) / 1000u) : 0;
    };
    // The text of a record: %NNNNw the world state's value, %NNNNk the time left to it, then the plurals.
    auto expand = [&](const std::string& s) {
        std::string out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%' && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '9') {
                size_t j = i + 1;
                uint32_t key = 0;
                while (j < s.size() && s[j] >= '0' && s[j] <= '9') key = key * 10 + static_cast<uint32_t>(s[j++] - '0');
                if (j < s.size() && (s[j] == 'w' || s[j] == 'k')) {
                    const uint32_t v = session_.worldState(key);
                    if (s[j] == 'w') {
                        out += std::to_string(v);
                    } else {
                        // PSP: minutes:seconds left (the realm time asked for every 10 s).
                        session_.requestWorldStateTimer();
                        bool ok = false;
                        const int64_t left = std::max<int64_t>(0, static_cast<int64_t>(v) - serverNow(ok));
                        char b[24];
                        std::snprintf(b, sizeof(b), "%d:%02d", static_cast<int>(left / 60), static_cast<int>(left % 60));
                        out += ok ? b : "";
                    }
                    i = j;
                    continue;
                }
            }
            out.push_back(s[i]);
        }
        return pp::text::plural(out);
    };
    float y = kAlwaysUpTop;
    int shown = 0, bars = 0;
    for (const RaidBgUi::Ws& w : r.ws) {
        uint32_t state = 0;
        if (!shownRecord(w, state)) continue;
        if (w.capture) {  // WorldStateCaptureBar: CaptureBar_Update(id, value, neutralPercent)
            if (bars >= 4 || !r.capBar.pixels) continue;
            const float value = static_cast<float>(std::min<uint32_t>(session_.worldState(w.ev[0]), 100u));
            const float neutral = static_cast<float>(std::min<uint32_t>(session_.worldState(w.ev[1]), 100u));
            const float t = static_cast<float>(r.capBar.uw) / 256.0f;  // texels of the 256 x 64 original
            const Rect b{static_cast<float>(pp::kScreenW) - 173.0f * kMm, kClusterBottom + static_cast<float>(bars) * 26.0f * kMm,
                         173.0f * kMm, 26.0f * kMm};
            const float cy = b.y + b.h * 0.5f;
            auto spr = [&](const Rect& dst, float u, float v, float uw, float vh, uint32_t col = 0xFFFFFFFFu,
                           Blend bl = Blend::Alpha) { ui.image(r.capBar, dst, SrcRect{u * t, v * t, uw * t, vh * t}, col, bl); };
            const float position = 25.0f + 124.0f * (1.0f - value / 100.0f);
            float& old = r.capOld[bars];
            int& dir = r.capDir[bars];
            if (old < 0.0f) old = position;
            if (position < old) dir = -1;
            else if (position > old) dir = 1;
            old = position;
            spr(Rect{b.x + 26.0f * kMm, cy - 4.5f * kMm, 61.0f * kMm, 9.0f * kMm}, 210, 0, 46, 9);           // LeftBar
            spr(Rect{b.x + b.w - 26.0f * kMm - 60.0f * kMm, cy - 4.5f * kMm, 60.0f * kMm, 9.0f * kMm}, 210, 11, 46, 9);  // RightBar
            const float mw = neutral == 0.0f ? 1.0f : neutral / 100.0f * 121.0f;
            const Rect mid{b.x + b.w * 0.5f - mw * kMm * 0.5f, cy - 4.5f * kMm, mw * kMm, 9.0f * kMm};
            spr(mid, 210, 22, 46, 9);                                                                          // MiddleBar
            spr(b, 0, 0, 173, 26);                                                                             // the frame
            spr(Rect{mid.x + mid.w - 1.0f * kMm, cy - 4.0f * kMm, 3.0f * kMm, 8.0f * kMm}, 191, 0, 3, 8);       // RightLine
            if (neutral != 0.0f) spr(Rect{mid.x + 1.0f * kMm - 3.0f * kMm, cy - 4.0f * kMm, 3.0f * kMm, 8.0f * kMm}, 191, 0, 3, 8);
            if (value > 50.0f + neutral * 0.5f)
                spr(Rect{b.x - 1.0f * kMm, cy - 14.0f * kMm, 27.0f * kMm, 28.0f * kMm}, 0, 28, 27, 28, 0xFFFFFFFFu, Blend::Add);
            else if (value < 50.0f - neutral * 0.5f)
                spr(Rect{b.x + b.w + 1.0f * kMm - 27.0f * kMm, cy - 14.0f * kMm, 27.0f * kMm, 28.0f * kMm}, 0, 28, 27, 28,
                    0xFFFFFFFFu, Blend::Add);
            const Rect ind{b.x + position * kMm - 2.5f * kMm, cy - 9.0f * kMm, 5.0f * kMm, 18.0f * kMm};
            if (dir < 0) spr(Rect{ind.x + 1.0f * kMm - 8.0f * kMm, cy - 7.5f * kMm, 8.0f * kMm, 15.0f * kMm}, 186, 9, 9, 15);
            if (dir > 0) spr(Rect{ind.x + ind.w - 1.0f * kMm, cy - 7.5f * kMm, 8.0f * kMm, 15.0f * kMm}, 195, 9, -9, 15);
            spr(ind, 199, 0, 5, 18);
            ++bars;
            continue;
        }
        // An AlwaysUpFrame.
        if (shown >= 8) continue;
        const float cx = static_cast<float>(pp::kScreenW) * 0.5f - 28.0f * kS;
        const Rect fr{cx - 22.5f * kS, y, 45.0f * kS, 24.0f * kS};
        const float my = fr.y + fr.h * 0.5f;
        const Rect ir{fr.x - 6.0f * kS, my - 21.0f * kS, 42.0f * kS, 42.0f * kS};
        icon(w.icon, ir, 0xFFFFFFFFu, Blend::Alpha);
        const std::string text = expand(w.text);
        const float tx = ir.x + ir.w - 12.0f * kS;
        const float ty = my - 10.0f * kS - static_cast<float>(fs.lineHeight()) * 0.5f;
        ui.textShadow(fs, tx, ty, text.c_str(), kColGold);
        if ((state == 2 || state == 3) && w.dynIcon >= 0) {  // $parentDynamicIconButton (RIGHT +40,+9)
            const Rect dr{fr.x + fr.w + 40.0f * kS - 32.0f * kS, my - 9.0f * kS - 16.0f * kS, 32.0f * kS, 32.0f * kS};
            icon(w.dynIcon, dr, 0xFFFFFFFFu, Blend::Alpha);
            if (state == 2 && w.dynFlash >= 0) {  // UIFrameFlash(frameFlash, 0.5, 0.5, -1)
                const float ph = static_cast<float>(now % 1000u) / 500.0f;
                const float a = ph < 1.0f ? ph : 2.0f - ph;
                icon(w.dynFlash, dr, (static_cast<uint32_t>(a * 255.0f) << 24) | 0x00FFFFFFu, Blend::Add);
            }
            const uint32_t did = kIdWsDyn0 + static_cast<uint32_t>(shown);
            if (focusable && !w.dynTip.empty() && winFocus_.addStop(did, dr)) {
                r.tipId = did;
                r.tipRect = dr;
                r.tipTitle = w.dynTip;
                r.tipLine.clear();
                r.tipLine2.clear();
            }
        }
        // frame.tooltip (only with an icon).
        const uint32_t lid = kIdWsLine0 + static_cast<uint32_t>(shown);
        const Rect hit{ir.x, fr.y, std::max(fr.w + 6.0f * kS, ir.w - 12.0f * kS + static_cast<float>(fs.measure(text.c_str()))), fr.h};
        if (focusable && w.icon >= 0 && !w.tip.empty() && winFocus_.addStop(lid, hit)) {
            r.tipId = lid;
            r.tipRect = hit;
            r.tipTitle = w.tip;
            r.tipLine.clear();
            r.tipLine2.clear();
        }
        y += 24.0f * kS;
        ++shown;
    }
    for (int k = bars; k < 4; ++k) {  // a bar gone: its last position is forgotten
        r.capOld[k] = -1.0f;
        r.capDir[k] = 0;
    }
}

}  // namespace wowee::ui::psp
