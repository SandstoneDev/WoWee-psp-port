// The quest log of the original - QuestLogFrame.xml / .lua and
// QuestInfo.lua's log template (the quest log like the original).
//
//   - the frame composed by tools/psp/build_ui_pak.py (qlog_frame: the book
//     icon, UI-QuestLogDualPane-Left/-Right, the QuestLogCount box) at 0.57 of
//     its frame units (389 x 254 px), centred; positions through FR();
//   - the list (QuestLogScrollFrame TOPLEFT 19,-75, 305 x 335): the quests
//     grouped under their zone (ZoneOrSort: AreaTable / QuestSort ruRU names,
//     text.pak "zones"), the zones by name, then the sorts, then MISCELLANEOUS, as
//     upstream's questRows; headers with UI-PlusButton / UI-MinusButton (Cross
//     collapses), the titles in GetQuestDifficultyColor, "(COMPLETE)" /
//     "(FAILED)" tags, the selection in UI-QuestLogTitleHighlight;
//   - QUESTS_COLON + "N/25" (QUEST_LOG_COUNT_TEMPLATE) in its box;
//   - the details on the parchment (QuestLogDetailScrollFrame TOPRIGHT -32,-77,
//     298 x 333) in QUEST_TEMPLATE_LOG order: title, objectives text, the
//     leaderboard (QUEST_MONSTERS_KILLED / QUEST_ITEMS_NEEDED /
//     QUEST_OBJECTS_FOUND), QUEST_DESCRIPTION + the description, QUEST_REWARDS with the
//     items' icons and the money;
//   - ABANDON_QUEST_ABBREV -> ABANDON_QUEST_CONFIRM YES / NO (the StaticPopup
//     window kWinQuestAbandon, ui_world_popup.cpp), SHARE_QUEST_ABBREV and
//     TRACK_QUEST_ABBREV (no group / no watch frame on the PSP yet: disabled),
//     CLOSE, the close button.
// PSP: Cross on the selected quest again pages its details (the stick would
// have to leave the list for the scroll arrows otherwise).
#include "ui/psp/ui_world_screen.hpp"

#include "game/update_field_table.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"
#include "platform/psp/psp_texture.hpp"
#include "ui/psp/ui_world_sounds.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

// Widget ids: 0x6B00-0x6BFF.
enum : uint32_t {
    kIdRow0 = 0x6B00,  // 0x6B00..0x6B3F: the visible list rows
    kIdDetailUp = 0x6B40,
    kIdDetailDown,
    kIdAbandon,
    kIdShare,
    kIdTrack,
    kIdCloseBtn,
    kIdCloseX,
    kIdYes,
    kIdNo,
};

constexpr float kS = 0.57f;                       // tools/psp/build_ui_pak.py QLOG_S
constexpr float kFrameW = 389.0f, kFrameH = 254.0f;
constexpr float kOx = (480.0f - kFrameW) * 0.5f, kOy = 4.0f;
Rect FR(float x, float y, float w, float h) { return Rect{kOx + x * kS, kOy + y * kS, w * kS, h * kS}; }
constexpr float kRowH = 14.0f;                    // QuestLogTitleButton 16 units; the PSP font is 13 px
constexpr uint32_t kParch = 0xFF0F1F2Eu;          // Parchment text (0.18, 0.12, 0.06)
constexpr uint32_t kBlack = 0xFF000000u;

uint32_t rgb(float r, float g, float b) {
    return 0xFF000000u | (static_cast<uint32_t>(b * 255.0f) << 16) | (static_cast<uint32_t>(g * 255.0f) << 8) |
           static_cast<uint32_t>(r * 255.0f);
}

// GetQuestDifficultyColor (QuestDifficultyColors) with the client's grey level
// (Trinity::XP::GetGrayLevel, the server's own rule).
uint32_t difficultyColor(int32_t questLevel, uint32_t playerLevel) {
    const int pl = static_cast<int>(playerLevel);
    const int ql = questLevel > 0 ? questLevel : pl;  // -1 = scales with the player
    const int diff = ql - pl;
    const int gray = pl <= 5 ? 0 : pl <= 39 ? pl - 5 - pl / 10 : pl <= 59 ? pl - 1 - pl / 5 : pl - 9;
    if (diff >= 5) return rgb(1.0f, 0.1f, 0.1f);    // impossible
    if (diff >= 3) return rgb(1.0f, 0.5f, 0.25f);   // verydifficult
    if (diff >= -2) return rgb(1.0f, 0.82f, 0.0f);  // difficult
    if (ql > gray) return rgb(0.25f, 0.75f, 0.25f); // standard
    return rgb(0.5f, 0.5f, 0.5f);                   // trivial
}

uint32_t itemQualityColor(uint32_t q) {
    switch (q) {
        case 0: return 0xFF9E9E9Eu;
        case 2: return 0xFF00FF1Eu;
        case 3: return 0xFFDD7000u;
        case 4: return 0xFFEE35A3u;
        case 5: return 0xFF0080FFu;
        default: return kParch;
    }
}

struct Row {
    bool header = false;
    int32_t key = 0;      // ZoneOrSort of a header
    int quest = -1;       // quest log index of a quest row
};

}  // namespace

WorldScreen::QuestLogUi::~QuestLogUi() { release(); }

void WorldScreen::QuestLogUi::release() {
    for (pp::Texture* t : {&frame, &empty, &plus, &minus, &titleHl, &check})
        if (t->pixels) pp::textureRelease(*t);
    tried = false;
    zoneNames.clear();
    zonesLoaded = false;
}

void WorldScreen::loadQuestLogArt() {
    if (qlog_.tried && qlog_.artRetry.due()) qlog_.tried = false;  // failed - again at the next open
    if (qlog_.tried) return;
    qlog_.tried = true;
    UiArtPak art("questlog");  // ui_art_fail when the pak / a texture finds no memory
    art.load("qlog_frame", qlog_.frame);
    art.load("ql_plus", qlog_.plus);
    art.load("ql_minus", qlog_.minus);
    art.load("ql_titlehl", qlog_.titleHl);
    art.load("ql_check", qlog_.check);  // the watched mark
    if (session_.getQuestLog().empty()) art.load("qlog_empty", qlog_.empty);
    if (art.failed()) qlog_.artRetry.fail();
    art.close();
    PSP_LOGI(pp::tag::Gfx, "questlog_art frame=%d retry=%d", qlog_.frame.pixels ? 1 : 0, art.failed() ? 1 : 0);
}

// The zone / sort names of the quests in the log, from text.pak "zones" (the
// whole table is read and only the keys in the log are kept).
const std::string& WorldScreen::questZoneName(int32_t key) {
    static const std::string kMisc = CLIENT_STR(MISCELLANEOUS);
    if (key == 0) return kMisc;
    auto it = qlog_.zoneNames.find(key);
    if (it != qlog_.zoneNames.end()) return it->second;
    if (!qlog_.zonesLoaded) {
        qlog_.zonesLoaded = true;  // once per opening of the log
        std::vector<int32_t> want;
        for (const auto& q : session_.getQuestLog()) want.push_back(q.zoneOrSort);
        char path[256];
        pp::fsPath(path, sizeof(path), "data/text.pak");
        pp::Pak pak;
        if (pak.open(path)) {
            if (const pp::PakEntry* e = pak.find("zones")) {
                if (auto* b = static_cast<const char*>(pak.load(e))) {
                    const char *p = b + 4, *end = b + e->size;
                    while (p + 4 < end) {
                        int32_t k = 0;
                        std::memcpy(&k, p, 4);
                        p += 4;
                        const char* s = p;
                        while (p < end && *p) ++p;
                        if (std::find(want.begin(), want.end(), k) != want.end()) qlog_.zoneNames[k] = std::string(s, p);
                        ++p;
                    }
                    pp::pakFree(const_cast<char*>(b));
                }
            }
            pak.close();
        }
        PSP_LOGI(pp::tag::Gfx, "questlog_zones keys=%u named=%u", static_cast<unsigned>(want.size()),
                 static_cast<unsigned>(qlog_.zoneNames.size()));
        it = qlog_.zoneNames.find(key);
        if (it != qlog_.zoneNames.end()) return it->second;
    }
    return kMisc;
}

bool WorldScreen::drawQuestLogWindow(UiRenderer& ui, uint8_t w) {
    if (w != kWinQuestLog) return false;
    loadQuestLogArt();
    if (!qlog_.frame.pixels) return false;  // no art baked: the plain window of ui_world_npc.cpp
    PROF_SCOPE("npc.questlog");
    loadNpcArt(false);
    const bool top = uiMode_ && !settings_.isOpen() && !windowOpen(kWinBindConfirm);  // every window
    const bool live = top && !windowOpen(kWinQuestAbandon);  // the abandon popup takes the focus
    const Font& fs = kit_.font(kFontSmall);
    const Font& fn = kit_.font(kFontNormal);
    const float lh = static_cast<float>(fs.lineHeight());
    const auto& log = session_.getQuestLog();
    const uint32_t playerLevel = session_.selfField(game::fieldIndex(game::UF::UNIT_FIELD_LEVEL));
    const uint32_t focused = winFocus_.focused();
    auto pressSeen = [&](uint32_t id) {
        if (winFocus_.isPressed(id)) npc_.guard = false;
    };

    ui.image(qlog_.frame, Rect{kOx, kOy, kFrameW, kFrameH});
    ui.textShadow(fn, kOx + kFrameW * 0.5f, FR(0, 15, 0, 0).y, CLIENT_STR(QUEST_LOG), kColGold, Align::Center);
    {
        // QuestLogCount: QUEST_LOG_COUNT_TEMPLATE (QUESTS_COLON |cffffffff%d/%d|r) right-aligned in its box (TOPLEFT 80,-41, 120 x 20).
        const Rect box = FR(80, 41, 120, 20);
        char n[16];
        std::snprintf(n, sizeof(n), "%u/25", static_cast<unsigned>(log.size()));
        const float nw = static_cast<float>(fs.measure(n));
        const float ty = box.y + (box.h - lh) * 0.5f;
        ui.textShadow(fs, box.x + box.w - 5, ty, n, kColWhite, Align::Right);
        ui.textShadow(fs, box.x + box.w - 5 - nw - 3, ty, CLIENT_STR(QUESTS_COLON), kColGold, Align::Right);
    }
    {  // QuestLogFrameCloseButton (TOPRIGHT 2,-8)
        const Rect cr{kOx + kFrameW - 18 * 1.0f - 2, FR(0, 10, 0, 0).y, 18, 18};
        const bool f = live && winFocus_.addStop(kIdCloseX, cr);
        if (f) pressSeen(kIdCloseX);
        const bool p = f && winFocus_.isPressed(kIdCloseX);
        const pp::Texture& t = p && npcArt_.closeDown.pixels ? npcArt_.closeDown : npcArt_.close;
        if (t.pixels) ui.image(t, cr);
        if (f && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, cr, 0xFFFFFFFFu, Blend::Add);
    }

    // --- The list ------------------------------------------------------------------------
    std::vector<Row> rows;
    {
        // upstream questRows: zones by name, then the sorts, then MISCELLANEOUS (0).
        std::vector<int32_t> keys;
        for (const auto& q : log)
            if (std::find(keys.begin(), keys.end(), q.zoneOrSort) == keys.end()) keys.push_back(q.zoneOrSort);
        std::stable_sort(keys.begin(), keys.end(), [&](int32_t a, int32_t b) {
            const int ga = a > 0 ? 0 : a < 0 ? 1 : 2, gb = b > 0 ? 0 : b < 0 ? 1 : 2;
            if (ga != gb) return ga < gb;
            return questZoneName(a) < questZoneName(b);
        });
        for (int32_t k : keys) {
            rows.push_back(Row{true, k, -1});
            if (std::find(qlog_.collapsed.begin(), qlog_.collapsed.end(), k) != qlog_.collapsed.end()) continue;
            for (size_t i = 0; i < log.size(); ++i)
                if (log[i].zoneOrSort == k) rows.push_back(Row{false, k, static_cast<int>(i)});
        }
    }
    // QuestLog_SetFirstValidSelection.
    if (qlog_.sel >= static_cast<int>(log.size())) qlog_.sel = -1;
    if (qlog_.sel < 0)
        for (const Row& r : rows)
            if (!r.header) {
                qlog_.sel = r.quest;
                qlog_.detailScroll = 0;
                break;
            }
    const Rect list = FR(19, 75, 305, 335);
    const int visible = static_cast<int>(list.h / kRowH);
    qlog_.listScroll = std::clamp(qlog_.listScroll, 0, std::max(0, static_cast<int>(rows.size()) - visible));
    // The list follows the focus (the stops are the visible rows only): the focus on the
    // first / last visible row scrolls one row while there is more, the focus stays on its row.
    if (live && focused >= kIdRow0 && focused < kIdRow0 + static_cast<uint32_t>(visible)) {
        const int vi = static_cast<int>(focused - kIdRow0);
        if (vi == visible - 1 && qlog_.listScroll + visible < static_cast<int>(rows.size())) {
            ++qlog_.listScroll;
            winFocus_.setFocus(focused - 1);
        } else if (vi == 0 && qlog_.listScroll > 0) {
            --qlog_.listScroll;
            winFocus_.setFocus(focused + 1);
        }
    }
    if (log.empty()) {
        const Rect er = FR(19, 73, 302, 356);
        if (qlog_.empty.pixels) ui.image(qlog_.empty, er);
        ui.textShadow(fn, er.x + er.w * 0.5f - 6 * kS, er.y + er.h * 0.5f - 16 * kS - lh * 0.5f, CLIENT_STR(QUESTLOG_NO_QUESTS_TEXT),
                      kColWhite, Align::Center);
    }
    ui.pushClip(list);
    for (int v = 0; v < visible; ++v) {
        const int i = qlog_.listScroll + v;
        if (i >= static_cast<int>(rows.size())) break;
        const Row& r = rows[static_cast<size_t>(i)];
        const Rect rr{list.x, list.y + static_cast<float>(v) * kRowH, list.w - 2, kRowH};
        const uint32_t id = kIdRow0 + static_cast<uint32_t>(v);
        const bool f = live && winFocus_.addStop(id, rr);
        if (f) pressSeen(id);
        const float ty = rr.y + (kRowH - lh) * 0.5f;
        if (r.header) {
            const bool col = std::find(qlog_.collapsed.begin(), qlog_.collapsed.end(), r.key) != qlog_.collapsed.end();
            const pp::Texture& t = col ? qlog_.plus : qlog_.minus;
            if (t.pixels) ui.image(t, Rect{rr.x + 3, rr.y + (kRowH - 10) * 0.5f, 10, 10});
            ui.textShadow(fs, rr.x + 16, ty, questZoneName(r.key).c_str(), f ? kColWhite : rgb(0.7f, 0.7f, 0.7f));
            if (f && qlog_.titleHl.pixels) ui.image(qlog_.titleHl, rr, 0x80B3B3B3u, Blend::Add);
            continue;
        }
        const auto& q = log[static_cast<size_t>(r.quest)];
        const uint32_t col = difficultyColor(q.level, playerLevel);
        const bool selected = r.quest == qlog_.sel;
        if ((selected || f) && qlog_.titleHl.pixels)  // _QuestLog_HighlightQuest: the highlight in the title's colour
            ui.image(qlog_.titleHl, rr, (col & 0x00FFFFFFu) | (selected ? 0xFF000000u : 0x80000000u), Blend::Add);
        const char* tagWord = q.failed ? CLIENT_STR(FAILED) : q.complete ? CLIENT_STR(COMPLETE) : nullptr;
        const std::string tagText = tagWord ? std::string("(") + tagWord + ")" : std::string();
        const char* tag = tagWord ? tagText.c_str() : nullptr;
        const float tagW = tag ? static_cast<float>(fs.measure(tag)) + 4 : 0.0f;
        if (tag) ui.textShadow(fs, rr.x + rr.w - 2, ty, tag, selected ? kColWhite : col, Align::Right);
        const Rect tr{rr.x + 12, rr.y, rr.w - 14 - tagW, kRowH};
        const std::string title = questText(q.title);
        // $parentCheck (UI-CheckBox-Check): the quest is on the tracker.
        if (questWatched(q.questId) && qlog_.check.pixels) {
            const float cw = static_cast<float>(fs.measure(title.c_str()));
            const float cx = std::min(tr.x + cw + 1, tr.x + tr.w - 10);
            ui.image(qlog_.check, Rect{cx, rr.y + (kRowH - 10) * 0.5f, 10, 10});
        }
        if (f && static_cast<float>(fs.measure(title.c_str())) > tr.w) {
            ui.textMarquee(fs, Rect{tr.x, ty, tr.w, lh}, title.c_str(), selected ? kColWhite : col);
        } else {
            ui.pushClip(tr);
            ui.textShadow(fs, tr.x, ty, title.c_str(), selected ? kColWhite : col);
            ui.popClip();
        }
    }
    ui.popClip();

    // --- The details on the parchment (QUEST_TEMPLATE_LOG) -----------------------------
    const Rect view = FR(682 - 32 - 298, 77, 298, 333);
    if (qlog_.sel >= 0 && qlog_.sel < static_cast<int>(log.size())) {
        const auto& q = log[static_cast<size_t>(qlog_.sel)];
        const float tx = view.x + 6, tw = view.w - 12;
        struct Line {
            enum Kind : uint8_t { Text, Head, Item, Money, Gap } kind;
            std::string s;
            uint32_t col = 0, value = 0, display = 0;
            float h = 0;
        };
        std::vector<Line> lines;
        auto addText = [&](const std::string& text, uint32_t col, bool head) {
            const Font& f = head ? fn : fs;
            for (const std::string& l : wrapQuestText(f, text, tw)) {
                Line ln{head ? Line::Head : Line::Text, l, col};
                ln.h = static_cast<float>(f.lineHeight());
                lines.push_back(std::move(ln));
            }
        };
        auto gap = [&](float h) {
            Line ln{Line::Gap, std::string(), 0};
            ln.h = h;
            lines.push_back(std::move(ln));
        };
        addText(questText(q.title), kBlack, true);  // QuestInfo_ShowTitle
        gap(4);
        if (!q.objectives.empty()) addText(questText(q.objectives), kParch, false);  // ShowObjectivesText
        // QuestInfo_ShowObjectives: GetQuestLogLeaderBoard lines, finished ones lighter.
        bool anyObj = false;
        for (const auto& ko : q.killObjectives) {
            if (ko.npcOrGoId == 0 || ko.required == 0) continue;
            const uint32_t entry = static_cast<uint32_t>(ko.npcOrGoId > 0 ? ko.npcOrGoId : -ko.npcOrGoId);
            auto kc = q.killCounts.find(entry);
            const uint32_t have = kc != q.killCounts.end() ? kc->second.first : 0;
            std::string name;
            if (ko.npcOrGoId > 0)
                if (const auto* ci = session_.creatureInfo(entry)) name = ci->name;
            const std::string b =
                ko.npcOrGoId > 0
                    ? CLIENT_FMT(QUEST_MONSTERS_KILLED, name.empty() ? std::string(CLIENT_STR(CREATURE)) : name,
                                 static_cast<unsigned>(have), static_cast<unsigned>(ko.required))
                    : CLIENT_FMT(QUEST_OBJECTS_FOUND, OWN_STR(QUEST_ITEM_PLACEHOLDER), static_cast<unsigned>(have),
                                 static_cast<unsigned>(ko.required));
            if (!anyObj) gap(6);
            anyObj = true;
            addText(b, have >= ko.required ? 0xFF333333u : kBlack, false);
        }
        for (const auto& io : q.itemObjectives) {
            if (io.itemId == 0 || io.required == 0) continue;
            auto ic = q.itemCounts.find(io.itemId);
            const uint32_t have = ic != q.itemCounts.end() ? ic->second : 0;
            const auto* info = session_.getItemInfo(io.itemId);
            if (!info || !info->valid) session_.ensureItemInfo(io.itemId);
            const std::string b =
                CLIENT_FMT(QUEST_ITEMS_NEEDED, info && info->valid ? info->name.c_str() : OWN_STR(QUEST_ITEM_PLACEHOLDER),
                           static_cast<unsigned>(have), static_cast<unsigned>(io.required));
            if (!anyObj) gap(6);
            anyObj = true;
            addText(b, have >= io.required ? 0xFF333333u : kBlack, false);
        }
        if (q.failed) addText(CLIENT_STR(QUEST_FAILED), 0xFF1A1AE6u, false);
        if (!q.description.empty()) {
            gap(8);
            addText(CLIENT_STR(QUEST_DESCRIPTION), kBlack, true);
            gap(2);
            addText(questText(q.description), kParch, false);
        }
        // QuestInfo_ShowRewards.
        std::vector<const game::QuestRewardItem*> fixed, choices;
        for (const auto& it : q.rewardItems)
            if (it.itemId) fixed.push_back(&it);
        for (const auto& it : q.rewardChoiceItems)
            if (it.itemId) choices.push_back(&it);
        const uint32_t money = q.rewardMoney > 0 ? static_cast<uint32_t>(q.rewardMoney) : 0u;
        auto addItems = [&](const std::vector<const game::QuestRewardItem*>& items) {
            for (size_t i = 0; i < items.size(); ++i) {
                const auto* info = session_.getItemInfo(items[i]->itemId);
                if (!info || !info->valid) session_.ensureItemInfo(items[i]->itemId);
                Line ln{Line::Item, info && info->valid ? info->name : std::string(),
                        itemQualityColor(info && info->valid ? info->quality : 1)};
                ln.value = items[i]->count;
                ln.display = items[i]->displayInfoId ? items[i]->displayInfoId : info && info->valid ? info->displayInfoId : 0;
                ln.h = (i % 2 == 1 || i + 1 == items.size()) ? 28.0f : 0.0f;
                lines.push_back(std::move(ln));
            }
        };
        if (money || !fixed.empty() || !choices.empty() || q.rewardSpellId) {
            gap(8);
            addText(CLIENT_STR(QUEST_REWARDS), kBlack, true);
            if (!choices.empty()) {
                addText(CLIENT_STR(REWARD_CHOICES), kParch, false);
                gap(2);
                addItems(choices);
            }
            if (money || !fixed.empty()) {
                gap(4);
                addText(choices.empty() ? CLIENT_STR(REWARD_ITEMS_ONLY) : CLIENT_STR(REWARD_ITEMS), kParch, false);
                if (money) {
                    Line ln{Line::Money, std::string(), 0};
                    ln.value = money;
                    ln.h = lh + 2;
                    lines.push_back(std::move(ln));
                }
                gap(2);
                addItems(fixed);
            }
        }
        float total = 4;
        std::vector<float> ys(lines.size());
        for (size_t i = 0; i < lines.size(); ++i) {
            ys[i] = total;
            total += lines[i].h;
        }
        const float maxScroll = std::max(0.0f, total - view.h + 4);
        qlog_.detailMax = static_cast<int>(maxScroll);
        const float scroll = std::clamp(static_cast<float>(qlog_.detailScroll), 0.0f, maxScroll);
        qlog_.detailScroll = static_cast<int>(scroll);
        ui.pushClip(view);
        for (size_t i = 0; i < lines.size(); ++i) {
            const Line& ln = lines[i];
            const float y = view.y + ys[i] - scroll;
            const float rowH = ln.kind == Line::Item ? 28.0f : ln.h;
            if (y + rowH < view.y || y > view.y + view.h) continue;
            switch (ln.kind) {
                case Line::Text: ui.text(fs, tx, y, ln.s.c_str(), ln.col); break;
                case Line::Head: ui.text(fn, tx, y, ln.s.c_str(), ln.col); break;
                case Line::Item: {
                    size_t k = 0;
                    for (size_t j = i; j > 0 && lines[j - 1].kind == Line::Item; --j) ++k;
                    const float cx = tx + ((k % 2) == 1 ? tw * 0.5f : 0.0f);
                    drawItemButton(ui, Rect{cx, y + 2, 22, 22}, ln.display, ln.value, -1, 0xFFFFFFFFu);
                    const float nx = cx + 25, nw = tw * 0.5f - 27;
                    const auto nl = wrapQuestText(fs, ln.s, nw);
                    ui.pushClip(Rect{nx, y, nw, 26});
                    const float ny = y + (26 - lh * static_cast<float>(std::min<size_t>(2, nl.size()))) * 0.5f;
                    for (size_t n = 0; n < nl.size() && n < 2; ++n)
                        ui.text(fs, nx, ny + lh * static_cast<float>(n), nl[n].c_str(), ln.col);
                    ui.popClip();
                    break;
                }
                case Line::Money: drawMoneyFrame(ui, tx, y + 1, ln.value, false); break;
                case Line::Gap: break;
            }
        }
        ui.popClip();
        if (maxScroll > 0) {  // the scroll bar's arrows (right of the parchment)
            const Rect up{view.x + view.w + 3, view.y, 14, 14}, dn{view.x + view.w + 3, view.y + view.h - 14, 14, 14};
            const bool canUp = scroll > 0.5f, canDn = scroll < maxScroll - 0.5f;
            const pp::Texture& tu = canUp ? npcArt_.scrUp : npcArt_.scrUpDis;
            const pp::Texture& td = canDn ? npcArt_.scrDown : npcArt_.scrDownDis;
            if (tu.pixels) ui.image(tu, up);
            if (td.pixels) ui.image(td, dn);
            if (live && canUp && winFocus_.addStop(kIdDetailUp, up)) ui.fill(up, 0x40FFFFFFu);
            if (live && canDn && winFocus_.addStop(kIdDetailDown, dn)) ui.fill(dn, 0x40FFFFFFu);
        }
    }

    // --- QuestLogControlPanel (BOTTOMLEFT 18,11) and CLOSE (BOTTOMRIGHT -7,14) ------
    const bool haveSel = qlog_.sel >= 0 && qlog_.sel < static_cast<int>(log.size());
    // PSP: the words are wider in the PSP font than the 110 / 1 / 100-unit buttons - each button is
    // its label + 16 px, in the original order from the left (the right of the row is free).
    {
        const Font& bf = kit_.font(kFontSmall);
        const Rect row = FR(18, 413, 0, 21);
        float bx = row.x;
        auto btn = [&](uint32_t id, const char* label, bool enabled) {
            const float bw = static_cast<float>(bf.measure(label)) + 16.0f;
            drawPanelButton(ui, id, Rect{bx, row.y, bw, row.h}, label, enabled, live);
            bx += bw;
        };
        btn(kIdAbandon, CLIENT_STR(ABANDON_QUEST_ABBREV), haveSel);
        // QuestLogControlPanel_UpdateState - GetQuestLogPushable and a group (party / raid).
        btn(kIdShare, CLIENT_STR(SHARE_QUEST_ABBREV),
            haveSel && session_.isInGroup() && session_.questPushable(log[static_cast<size_t>(qlog_.sel)].questId));
        btn(kIdTrack, CLIENT_STR(TRACK_QUEST_ABBREV), haveSel);  // WatchFrame
    }
    drawPanelButton(ui, kIdCloseBtn, FR(682 - 7 - 80, 447 - 14 - 22, 80, 22), CLIENT_STR(CLOSE), true, live);

    // The rows as the action sees them (visible index -> row).
    qlog_.visibleRows.clear();
    for (int v = 0; v < visible && qlog_.listScroll + v < static_cast<int>(rows.size()); ++v) {
        const Row& r = rows[static_cast<size_t>(qlog_.listScroll + v)];
        qlog_.visibleRows.push_back(r.header ? (static_cast<int64_t>(r.key) << 8) | 1 : static_cast<int64_t>(r.quest) << 8);
    }
    return true;
}

// StaticPopupDialogs["ABANDON_QUEST"] - ABANDON_QUEST_CONFIRM, YES / NO - as its own window over
// the quest log (was drawn inside it on the generic panel): START closes it first (hideOnEscape = NO), the
// log stays. It goes away with the quest log or the selection.
bool WorldScreen::drawQuestAbandon(UiRenderer& ui, uint8_t w) {
    if (w != kWinQuestAbandon) return false;
    const auto& log = session_.getQuestLog();
    if (!windowOpen(kWinQuestLog) || qlog_.sel < 0 || qlog_.sel >= static_cast<int>(log.size())) {
        closeWindow(kWinQuestAbandon);
        return true;
    }
    const std::string b = CLIENT_FMT(ABANDON_QUEST_CONFIRM, questText(log[static_cast<size_t>(qlog_.sel)].title));
    StaticPopup p;
    p.text = b.c_str();
    p.button1 = CLIENT_STR(YES);
    p.button1Id = kIdYes;
    p.button2 = CLIENT_STR(NO);
    p.button2Id = kIdNo;
    p.focusable = uiMode_ && !settings_.isOpen() && !windowOpen(kWinBindConfirm);
    drawStaticPopup(ui, p);
    return true;
}

bool WorldScreen::questLogWindowAction(uint32_t act) {
    if (act < 0x6B00 || act > 0x6BFF) return false;
    const auto& log = session_.getQuestLog();
    const bool haveSel = qlog_.sel >= 0 && qlog_.sel < static_cast<int>(log.size());
    switch (act) {
        case kIdCloseX:
        case kIdCloseBtn: closeWindow(kWinQuestLog); return true;
        case kIdDetailUp: qlog_.detailScroll = std::max(0, qlog_.detailScroll - 40); return true;
        case kIdTrack:  // QuestLogFrameTrackButton_OnClick - AddQuestWatch / RemoveQuestWatch
            if (haveSel) {
                const uint32_t id = log[static_cast<size_t>(qlog_.sel)].questId;
                setQuestWatched(id, !questWatched(id));
            }
            return true;
        case kIdDetailDown: qlog_.detailScroll += 40; return true;
        case kIdShare:  // QuestLogPushQuest(); PlaySound("igQuestLogOpen")
            if (haveSel) {
                session_.pushQuestToParty(log[static_cast<size_t>(qlog_.sel)].questId);
                playSoundByName("igQuestLogOpen");
            }
            return true;
        case kIdAbandon:
            if (haveSel) {
                openWindow(kWinQuestAbandon);  // QuestLogFrameAbandonButton: SetAbandonQuest + StaticPopup_Show
                winFocus_.setFocus(kIdNo);
            }
            return true;
        case kIdYes:
            if (windowOpen(kWinQuestAbandon) && haveSel) {
                PSP_LOGI(pp::tag::Net, "questlog_abandon quest=%u", static_cast<unsigned>(log[static_cast<size_t>(qlog_.sel)].questId));
                session_.abandonQuest(log[static_cast<size_t>(qlog_.sel)].questId);
                qlog_.sel = -1;
            }
            closeWindow(kWinQuestAbandon);
            winFocus_.setFocus(kIdRow0);
            return true;
        case kIdNo:
            closeWindow(kWinQuestAbandon);
            winFocus_.setFocus(kIdAbandon);
            return true;
        default: break;
    }
    if (act >= kIdRow0 && act < kIdRow0 + 64) {
        const size_t v = act - kIdRow0;
        if (v >= qlog_.visibleRows.size()) return true;
        const int64_t r = qlog_.visibleRows[v];
        if (r & 1) {  // a header: collapse / expand (QuestLogTitleButton_OnClick)
            const auto key = static_cast<int32_t>(r >> 8);
            auto it = std::find(qlog_.collapsed.begin(), qlog_.collapsed.end(), key);
            if (it == qlog_.collapsed.end()) qlog_.collapsed.push_back(key);
            else qlog_.collapsed.erase(it);
        } else {
            const int quest = static_cast<int>(r >> 8);
            if (quest == qlog_.sel) {  // PSP: Cross again pages the details, back to the top at the end
                qlog_.detailScroll = qlog_.detailScroll >= qlog_.detailMax ? 0 : qlog_.detailScroll + 120;
            } else {
                qlog_.sel = quest;  // QuestLog_SetSelection
                qlog_.detailScroll = 0;
            }
        }
        playUiSound(UiSound::ButtonClick);
    }
    return true;
}

}  // namespace wowee::ui::psp
