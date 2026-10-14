// The quest tracker of the original - WatchFrame.xml / .lua
// ("the list of active quests on the screen as in the original, and
// it can be collapsed").
//
//   - under the minimap cluster at the right (WatchFrame TOPRIGHT of
//     MinimapCluster's BOTTOMRIGHT), OBJECTIVES_TRACKER_LABEL " (N)" (
//     GameFontNormal) and the collapse / expand button
//     (UI-Panel-QuestHideButton: TexCoords 0..0.5 x 0.5..1 open, 0..0.5 x
//     0..0.5 collapsed);
//   - WatchFrame_DisplayTrackedQuests: per watched quest its title (0.75,
//     0.61, 0), then its unfinished objectives as "- 3/10 Name"
//     (WatchFrame_ReverseQuestObjective), white; a complete quest shows its
//     completion text instead; 10 units between quests (WATCHFRAME_QUEST_OFFSET);
//     what does not fit above the action bar is left out, as the original
//     stops at its height;
//   - every quest is watched unless taken off with TRACK_QUEST in the quest
//     log (AUTO_QUEST_WATCH: accepted quests are watched; PSP: the ones in the
//     log from before too).
// Collapse: the button (in the interface, SELECT) or wowee.cfg watchcollapsed. The window column is only
// "Book" right of the action bar, under these lines.
#include "ui/psp/ui_world_screen.hpp"

#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_strings.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_texture.hpp"
#include "ui/psp/ui_world_sounds.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <cstdio>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

constexpr uint32_t kIdCollapse = 0x6E00;
constexpr float kRight = 476.0f, kTop = 126.0f, kBottom = 238.0f;  // under the minimap, above the action bar
constexpr float kWidth = 150.0f;                                    // WATCHFRAME_EXPANDEDWIDTH 204 at ~0.72
constexpr uint32_t kTitleCol = 0xFF009CBFu;                         // quest title (0.75, 0.61, 0)

// WatchFrame_ReverseQuestObjective: "text: have/need" -> "have/need text" (the client's
// QUEST_MONSTERS_KILLED / QUEST_OBJECTS_FOUND formatted in its language).
std::string reverseObjective(const std::string& s) {
    const size_t colon = s.rfind(':');
    if (colon == std::string::npos) return s;
    size_t a = colon + 1;
    while (a < s.size() && s[a] == ' ') ++a;
    return s.substr(a) + " " + s.substr(0, colon);
}

}  // namespace

bool WorldScreen::questWatched(uint32_t questId) const {
    return std::find(watchOff_.begin(), watchOff_.end(), questId) == watchOff_.end();
}

// (An untracked quest came back tracked after a restart:) the quests taken
// off the tracker are kept per character in wowee.cfg (watchoff_<name>=id,id,...) - the client keeps its watch
// list between sessions. PSP: per device (as the client's WTF folder is per PC).
static std::string watchKey(const std::string& name) {
    std::string k = "watchoff_";
    for (char c : name) k += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    return k;
}

void WorldScreen::setQuestWatched(uint32_t questId, bool on) {
    watchRebuildMs_ = 0;  // the tracker at once
    auto it = std::find(watchOff_.begin(), watchOff_.end(), questId);
    if (on && it != watchOff_.end()) watchOff_.erase(it);
    if (!on && it == watchOff_.end()) watchOff_.push_back(questId);
    std::string v;
    for (uint32_t id : watchOff_) {
        if (!v.empty()) v += ',';
        v += std::to_string(id);
    }
    pp::configSet(watchKey(name_).c_str(), v);
    pp::configSave();
}

void WorldScreen::drawWatchFrame(UiRenderer& ui) {
    if (!watchInit_) {
        watchInit_ = true;
        watchCollapsed_ = pp::configGetInt("watchcollapsed", 0) != 0;
        const std::string off = pp::configGet(watchKey(name_).c_str(), "");  // kept between sessions
        for (size_t p = 0; p < off.size();) {
            const size_t c = off.find(',', p);
            const uint32_t id = static_cast<uint32_t>(std::strtoul(off.substr(p, c - p).c_str(), nullptr, 10));
            if (id) watchOff_.push_back(id);
            if (c == std::string::npos) break;
            p = c + 1;
        }
        char path[256];
        pp::fsPath(path, sizeof(path), "data/ui.pak");
        pp::Pak pak;
        if (pak.open(path)) {
            const pp::PakEntry* e = pak.find("watch_hide");
            void* blob = e ? pak.load(e) : nullptr;
            if (blob && !pp::textureFromBlob(blob, e->size, watchBtn_, true)) pp::pakFree(blob);
            pak.close();
        }
    }
    if (settings_.isOpen()) return;
    const auto& log = session_.getQuestLog();
    int watched = 0;
    for (const auto& q : log)
        if (questWatched(q.questId)) ++watched;
    if (watched == 0) return;  // WatchFrameHeader:Hide() with nothing to track
    PROF_SCOPE("hud.watch");
    const Font& fs = kit_.font(kFontSmall);
    const Font& fn = kit_.font(kFontNormal);
    const float lh = static_cast<float>(fs.lineHeight());
    const float x0 = kRight - kWidth;

    // Header: OBJECTIVES_TRACKER_LABEL .. " (" .. n .. ")" and the collapse / expand button at its right.
    char title[64];
    std::snprintf(title, sizeof(title), "%s (%d)", CLIENT_STR(OBJECTIVES_TRACKER_LABEL), watched);
    ui.textShadow(fn, x0, kTop, title, kColGold);
    const Rect br{kRight - 14, kTop + 1, 14, 14};
    if (watchBtn_.pixels) {
        const float hw = watchBtn_.uw * 0.5f, hh = watchBtn_.uh * 0.5f;
        ui.image(watchBtn_, br, SrcRect{0, watchCollapsed_ ? 0.0f : hh, hw, hh});
    } else {
        ui.textShadow(fs, br.x + 4, br.y, watchCollapsed_ ? "+" : "-", kColGold);
    }
    if (uiMode_) {
        // Under a window: no focus (the windows declare their occluders the same way).
        Rect occl[16];
        int n = 0;
        for (uint8_t w : windows_) {
            auto it = winRect_.find(w);
            if (it != winRect_.end() && n < 16) occl[n++] = it->second;
        }
        winFocus_.setOccluders(occl, n);
        if (winFocus_.addStop(kIdCollapse, br)) ui.fill(br, 0x40FFFFFFu);
        winFocus_.setOccluders(nullptr, 0);
    }
    if (watchCollapsed_) return;

    // The lines (hud.watch took 0.95 ms a frame on hardware): built - tokens, objective lines, word
    // wrap, the fit check - twice a second into watchRows_; every frame only draws them.
    const uint32_t now = pp::logUptimeMs();
    if (now >= watchRebuildMs_) {
        watchRebuildMs_ = now + 500u;
        watchRows_.clear();
        float y = kTop + static_cast<float>(fn.lineHeight()) + 2.0f;
        const float textW = kWidth - 8.0f;  // after the dash (DASH_WIDTH)
        for (const auto& q : log) {
            if (!questWatched(q.questId)) continue;
            std::vector<std::pair<std::string, bool>> lines;  // text, header
            lines.emplace_back(questText(q.title), true);
            if (q.complete) {
                lines.emplace_back(questText(q.completionText.empty() ? std::string(CLIENT_STR(QUEST_COMPLETE)) : q.completionText),
                                   false);  // GetQuestLogCompletionText (QUEST_COMPLETE without one)
            } else {
                for (const auto& ko : q.killObjectives) {
                    if (ko.npcOrGoId == 0 || ko.required == 0) continue;
                    const uint32_t entry = static_cast<uint32_t>(ko.npcOrGoId > 0 ? ko.npcOrGoId : -ko.npcOrGoId);
                    auto kc = q.killCounts.find(entry);
                    const uint32_t have = kc != q.killCounts.end() ? kc->second.first : 0;
                    if (have >= ko.required) continue;  // finished objectives are not shown
                    std::string name;
                    if (ko.npcOrGoId > 0)
                        if (const auto* ci = session_.creatureInfo(entry)) name = ci->name;
                    // WatchFrame_ReverseQuestObjective of QUEST_MONSTERS_KILLED / QUEST_OBJECTS_FOUND.
                    if (ko.npcOrGoId > 0)
                        lines.emplace_back(reverseObjective(CLIENT_FMT(QUEST_MONSTERS_KILLED,
                                                                       name.empty() ? CLIENT_STR(CREATURE) : name.c_str(),
                                                                       have, ko.required)),
                                           false);
                    else
                        lines.emplace_back(reverseObjective(CLIENT_FMT(QUEST_OBJECTS_FOUND, CLIENT_STR(HELPFRAME_ITEM_TITLE),
                                                                       have, ko.required)),
                                           false);
                }
                for (const auto& io : q.itemObjectives) {
                    if (io.itemId == 0 || io.required == 0) continue;
                    auto ic = q.itemCounts.find(io.itemId);
                    const uint32_t have = ic != q.itemCounts.end() ? ic->second : 0;
                    if (have >= io.required) continue;
                    const auto* info = session_.getItemInfo(io.itemId);
                    if (!info || !info->valid) session_.ensureItemInfo(io.itemId);
                    char b[160];
                    std::snprintf(b, sizeof(b), "%u/%u %s", static_cast<unsigned>(have),
                                  static_cast<unsigned>(io.required), info && info->valid ? info->name.c_str() : CLIENT_STR(HELPFRAME_ITEM_TITLE));
                    lines.emplace_back(b, false);
                }
                if (lines.size() == 1 && !q.objectives.empty())  // no countable objectives: the objectives text
                    lines.emplace_back(questText(q.objectives), false);
            }
            // Wrap and measure; a quest that does not fit is left out (and the rest with it).
            std::vector<WatchRow> rows;
            for (const auto& l : lines) {
                const auto parts = wrapQuestText(fs, l.first, l.second ? kWidth : textW);
                for (size_t k = 0; k < parts.size() && k < (l.second ? 2u : 3u); ++k)
                    rows.push_back(WatchRow{parts[k], l.second, !l.second && k == 0, 0.0f});  // the dash: first line
            }
            const float h = lh * static_cast<float>(rows.size());
            if (y + h > kBottom) break;
            for (WatchRow& r : rows) {
                r.y = y;
                y += lh;
                watchRows_.push_back(std::move(r));
            }
            y += 10.0f * 0.72f;  // WATCHFRAME_QUEST_OFFSET
        }
    }
    const float textX = x0 + 8.0f;
    for (const WatchRow& r : watchRows_) {
        if (r.header) {
            ui.textShadow(fs, x0, r.y, r.text.c_str(), kTitleCol);
        } else {
            if (r.dash) ui.textShadow(fs, x0 + 1, r.y, "-", kColWhite);  // QUEST_DASH
            ui.textShadow(fs, textX, r.y, r.text.c_str(), kColWhite);
        }
    }
}

bool WorldScreen::watchAction(uint32_t act) {
    if (act != kIdCollapse) return false;
    watchCollapsed_ = !watchCollapsed_;  // WatchFrame_CollapseExpandButton_OnClick
    watchRebuildMs_ = 0;
    pp::configSet("watchcollapsed", watchCollapsed_ ? "1" : "0");
    pp::configSave();
    playUiSound(UiSound::ButtonClick);  // igMiniMapOpen / Close
    return true;
}

}  // namespace wowee::ui::psp
