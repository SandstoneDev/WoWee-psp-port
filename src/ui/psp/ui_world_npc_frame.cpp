// The NPC windows of the original - GossipFrame.xml/.lua and
// QuestFrame.xml + QuestInfo.lua ("the dialog window must be exactly
// like the original").
//
// One frame for the four conversation windows, as in the client:
//   - UI-QuestGreeting (ui.pak npc_frame, 384 x 448 units drawn at 0.6 - the
//     PSP screen is 272 px high), the portrait circle (dark until NPC
//     portraits exist), the NPC name (GameFontHighlight, white) beside it,
//     the close button (UIPanelCloseButton at TOPRIGHT -42,-31);
//   - the scroll frame (TOPLEFT 23,-81, 300 x 334) on the parchment with the
//     text in QuestFont's parchment colour (MATERIAL_TEXT_COLOR_TABLE
//     Parchment 0.18/0.12/0.06), headings black (QuestTitleFont), the arrows
//     of its scroll bar in the frame's right column;
//   - gossip: the greeting, then the quests (available, then active) and the
//     options with their GossipFrame icons and UI-QuestTitleHighlight;
//   - quests: QuestInfo's templates - DETAIL (title, description,
//     QUEST_OBJECTIVES, objectives, rewards), PROGRESS (title, text,
//     TURN_IN_ITEMS, REQUIRED_MONEY), REWARD (title, text, rewards with
//     REWARD_CHOOSE); rewards as QuestInfo_ShowRewards: QUEST_REWARDS, the
//     choices, REWARD_ITEMS / REWARD_ITEMS_ONLY, money with the MoneyFrame
//     coins, EXPERIENCE_COLON, the items with their icons;
//   - UIPanelButtons in the frame's bottom slots: GOODBYE / ACCEPT -
//     DECLINE / CONTINUE - CANCEL / COMPLETE_QUEST - CANCEL.
// Actions stay in npcWindowAction (ids of ui_world_npc.cpp).
#include "ui/psp/ui_world_screen.hpp"

#include "game/item_text.hpp"
#include "game/text_tokens.hpp"
#include "game/update_field_table.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"
#include "platform/psp/psp_texture.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

// Ids shared with ui_world_npc.cpp (its enum; 0x6100-0x61FF).
constexpr uint32_t kIdTextUp = 0x6101, kIdTextDown = 0x6102, kIdGoodbye = 0x6105, kIdAccept = 0x6108,
                   kIdDecline = 0x6109, kIdComplete = 0x610A, kIdCancel = 0x610B, kIdTakeReward = 0x610C,
                   kIdRow0 = 0x6120, kIdChoice0 = 0x6130, kIdNpcClose = 0x6140;

constexpr float kS = 0.6f;                   // frame units -> px
constexpr float kFx = 4.0f, kFy = 2.0f;      // GossipFrame TOPLEFT 0,-104 -> the left, at the top (269 px tall)
constexpr uint32_t kParch = 0xFF0F1F2Eu;     // Parchment text (0.18, 0.12, 0.06)
constexpr uint32_t kBlack = 0xFF000000u;     // Parchment title text
constexpr uint32_t kBtnText = 0xFF00D1FFu;   // GameFontNormal (1, 0.82, 0)
constexpr uint32_t kBtnDis = 0xFF808080u;

Rect FR(float x, float y, float w, float h) { return Rect{kFx + x * kS, kFy + y * kS, w * kS, h * kS}; }

uint32_t questQuality(uint32_t q) {  // ITEM_QUALITY_COLORS as 0xAABBGGRR
    switch (q) {
        case 0: return 0xFF9E9E9Eu;
        case 2: return 0xFF00FF1Eu;
        case 3: return 0xFFDD7000u;
        case 4: return 0xFFEE35A3u;
        case 5: return 0xFF0080FFu;
        case 6:
        case 7: return 0xFF80CCE6u;
        default: return 0xFFFFFFFFu;
    }
}

std::vector<std::string> wrapText(const Font& f, const std::string& text, float maxW) {
    std::vector<std::string> out;
    std::string line, word;
    auto flushWord = [&]() {
        if (word.empty()) return;
        const std::string t = line.empty() ? word : line + " " + word;
        if (!line.empty() && static_cast<float>(f.measure(t.c_str())) > maxW) {
            out.push_back(line);
            line = word;
        } else {
            line = t;
        }
        word.clear();
    };
    for (size_t i = 0; i <= text.size(); ++i) {
        const char c = i < text.size() ? text[i] : '\n';
        if (c == '\r') continue;
        if (c == ' ' || c == '\n') {
            flushWord();
            if (c == '\n') {
                out.push_back(line);
                line.clear();
            }
        } else {
            word += c;
        }
    }
    while (!out.empty() && out.back().empty()) out.pop_back();
    return out;
}

// The ruRU client's text grammar for the server's quest texts
// (data/text.pak, tools/psp/build_text_pak.py): $C / $R as the Russian class /
// race name of the player's gender, then "|3-N(word)" declined through
// DeclinedWord (N 1..5 cases, 6 nominative lower case, 7/8 plural); a word or
// a case the table lacks stays as written.
struct RuText {
    bool tried = false;
    std::vector<std::array<std::string, 9>> decl;  // [0] nominative, [1..8] cases
    std::string cls[11][2], race[12][2];           // [id][0 male, 1 female]
};
RuText& ruText() {
    static RuText t;
    if (t.tried) return t;
    t.tried = true;
    char path[256];
    pp::fsPath(path, sizeof(path), "data/text.pak");
    pp::Pak pak;
    if (!pak.open(path)) return t;
    auto strs = [](const char*& p, const char* end) {
        std::string out;
        while (p < end && *p) out += *p++;
        if (p < end) ++p;
        return out;
    };
    if (const pp::PakEntry* e = pak.find("decl")) {
        if (auto* b = static_cast<const char*>(pak.load(e))) {
            uint32_t n = 0;
            std::memcpy(&n, b, 4);
            const char *p = b + 4, *end = b + e->size;
            for (uint32_t i = 0; i < n && p < end; ++i) {
                std::array<std::string, 9> w;
                for (auto& f : w) f = strs(p, end);
                t.decl.push_back(std::move(w));
            }
            pp::pakFree(const_cast<char*>(b));
        }
    }
    if (const pp::PakEntry* e = pak.find("names")) {
        if (auto* b = static_cast<const char*>(pak.load(e))) {
            const char *p = b, *end = b + e->size;
            uint32_t n = 0;
            std::memcpy(&n, p, 4);
            p += 4;
            for (uint32_t i = 0; i < n && i < 11; ++i)
                for (auto& f : t.cls[i]) f = strs(p, end);
            std::memcpy(&n, p, 4);
            p += 4;
            for (uint32_t i = 0; i < n && i < 12; ++i)
                for (auto& f : t.race[i]) f = strs(p, end);
            pp::pakFree(const_cast<char*>(b));
        }
    }
    pak.close();
    PSP_LOGI(pp::tag::Gfx, "text_pak words=%u", static_cast<unsigned>(t.decl.size()));
    return t;
}

// "|3-N(word)" -> the word in case N.
std::string declineMarkup(const std::string& s) {
    if (s.find("|3-") == std::string::npos) return s;
    const RuText& t = ruText();
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const size_t m = s.find("|3-", i);
        if (m == std::string::npos) break;
        out.append(s, i, m - i);
        size_t k = m + 3;
        int n = 0;
        while (k < s.size() && s[k] >= '0' && s[k] <= '9') n = n * 10 + (s[k++] - '0');
        if (k >= s.size() || s[k] != '(') {
            out.append(s, m, k - m);
            i = k;
            continue;
        }
        const size_t close = s.find(')', k);
        if (close == std::string::npos) {
            out.append(s, m, std::string::npos);
            i = s.size();
            break;
        }
        const std::string word = s.substr(k + 1, close - k - 1);
        std::string rep = word;
        if (n >= 1 && n <= 8)
            for (const auto& w : t.decl)
                if (w[0] == word && !w[static_cast<size_t>(n)].empty()) {
                    rep = w[static_cast<size_t>(n)];
                    break;
                }
        out += rep;
        i = close + 1;
    }
    if (i < s.size()) out.append(s, i, std::string::npos);
    return out;
}

}  // namespace

std::vector<std::string> WorldScreen::wrapQuestText(const Font& f, const std::string& text, float maxW) {
    return wrapText(f, text, maxW);
}

// The same text grammar for the quest log (ui_world_questlog.cpp).
std::string WorldScreen::questText(const std::string& s) {
    game::TextSubject subject;
    if (const game::Character* me = session_.activeCharacter()) {
        subject.name = me->name;
        const RuText& rt = ruText();
        const int fem = me->gender == game::Gender::FEMALE ? 1 : 0;
        const auto ci = static_cast<size_t>(me->characterClass), ri = static_cast<size_t>(me->race);
        subject.className = ci < 11 && !rt.cls[ci][0].empty()
                                ? (fem && !rt.cls[ci][1].empty() ? rt.cls[ci][1] : rt.cls[ci][0])
                                : game::getClassName(me->characterClass);
        subject.raceName = ri < 12 && !rt.race[ri][0].empty()
                               ? (fem && !rt.race[ri][1].empty() ? rt.race[ri][1] : rt.race[ri][0])
                               : game::getRaceName(me->race);
        subject.gender = me->gender;
    }
    return declineMarkup(game::resolveTextTokens(s, subject));
}

WorldScreen::NpcArt::~NpcArt() { release(true); }

void WorldScreen::NpcArt::release(bool all) {
    if (frame.pixels) pp::textureRelease(frame);
    frameTried = false;
    if (!all) return;
    for (pp::Texture* t : {&titleHl, &btnUp, &btnDown, &btnDis, &btnHl, &scrUp, &scrDown, &scrUpDis, &scrDownDis,
                           &money, &close, &closeDown})
        if (t->pixels) pp::textureRelease(*t);
    for (pp::Texture& t : icons)
        if (t.pixels) pp::textureRelease(t);
    smallTried = false;
}

void WorldScreen::loadNpcArt(bool frame) {
    // What failed (no pak, no memory: ui_art_fail) is loaded again at the next window
    // open - was "no pak, nothing to retry": the plain gossip / merchant buttons and coins for the session.
    if (npcArt_.artRetry.due()) {
        if (!npcArt_.frame.pixels) npcArt_.frameTried = false;
        npcArt_.smallTried = false;
    }
    if ((npcArt_.frameTried || !frame) && npcArt_.smallTried) return;
    UiArtPak art("npc");
    if (!art.isOpen()) {
        npcArt_.frameTried = npcArt_.smallTried = true;
        npcArt_.artRetry.fail();
        return;
    }
    auto load = [&](const char* name, pp::Texture& t) { art.load(name, t); };
    if (!npcArt_.frameTried && frame) {
        npcArt_.frameTried = true;
        load("npc_frame", npcArt_.frame);
    }
    if (!npcArt_.smallTried) {
        npcArt_.smallTried = true;
        static const char* const kIcons[kNpcIconCount] = {
            "npc_ic_gossip",   "npc_ic_vendor",  "npc_ic_taxi",   "npc_ic_trainer",      "npc_ic_healer",
            "npc_ic_binder",   "npc_ic_banker",  "npc_ic_petition", "npc_ic_tabard",     "npc_ic_battlemaster",
            "npc_ic_unlearn",  "npc_ic_available", "npc_ic_active", "npc_ic_incomplete", "npc_ic_daily",
            "npc_ic_dailyactive"};
        for (int i = 0; i < kNpcIconCount; ++i) load(kIcons[i], npcArt_.icons[i]);
        load("npc_titlehl", npcArt_.titleHl);
        load("pbtn_up", npcArt_.btnUp);
        load("pbtn_dn", npcArt_.btnDown);
        load("pbtn_dis", npcArt_.btnDis);
        load("pbtn_hl", npcArt_.btnHl);
        load("scr_up", npcArt_.scrUp);
        load("scr_dn", npcArt_.scrDown);
        load("scr_up_dis", npcArt_.scrUpDis);
        load("scr_dn_dis", npcArt_.scrDownDis);
        load("money_icons", npcArt_.money);
        load("loot_close", npcArt_.close);
        load("loot_close_dn", npcArt_.closeDown);
    }
    if (art.failed()) npcArt_.artRetry.fail();
    art.close();
    PSP_LOGI(pp::tag::Gfx, "npc_art frame=%d icons=%d buttons=%d retry=%d", npcArt_.frame.pixels ? 1 : 0,
             npcArt_.icons[0].pixels ? 1 : 0, npcArt_.btnUp.pixels ? 1 : 0, art.failed() ? 1 : 0);
}

// UIPanelButtonTemplate: the button art in three slices (ends 6 px), the
// highlight added over it, gold text (grey when disabled).
void WorldScreen::drawPanelButton(UiRenderer& ui, uint32_t id, const Rect& r, const char* label, bool enabled,
                                  bool top) {
    const bool focused = enabled && top && winFocus_.addStop(id, r);
    const bool pressed = focused && winFocus_.isPressed(id);
    if (focused && pressed) npc_.guard = false;  // a press seen on the window
    const pp::Texture& t = !enabled && npcArt_.btnDis.pixels ? npcArt_.btnDis : pressed && npcArt_.btnDown.pixels
                                                                                      ? npcArt_.btnDown
                                                                                      : npcArt_.btnUp;
    if (t.pixels) {
        const float cap = 6.0f, tw = t.uw, th = t.uh, capT = cap * tw / 64.0f;
        ui.image(t, Rect{r.x, r.y, cap, r.h}, SrcRect{0, 0, capT, th});
        ui.image(t, Rect{r.x + cap, r.y, r.w - 2 * cap, r.h}, SrcRect{capT, 0, tw - 2 * capT, th});
        ui.image(t, Rect{r.x + r.w - cap, r.y, cap, r.h}, SrcRect{tw - capT, 0, capT, th});
        if (focused && npcArt_.btnHl.pixels) ui.image(npcArt_.btnHl, r, 0xFFFFFFFFu, Blend::Add);
    } else {
        kit_.button(ui, winFocus_, id, r, label, enabled && top);
        return;
    }
    const Font& fs = kit_.font(kFontSmall);
    ui.textShadow(fs, r.x + r.w * 0.5f + (pressed ? 1.0f : 0.0f), r.y + (r.h - fs.lineHeight()) * 0.5f, label,
                  enabled ? kBtnText : kBtnDis, Align::Center);
}

bool WorldScreen::drawNpcFrame(UiRenderer& ui, uint8_t w) {
    if (w != kWinGossip && w != kWinQuestDetails && w != kWinQuestProgress && w != kWinQuestReward) return false;
    PROF_SCOPE("npc.frame");
    loadNpcArt();
    if (!npcArt_.frame.pixels) return false;  // no art baked: the plain windows of ui_world_npc.cpp
    const bool top = !windows_.empty() && windows_.back() == w;
    const Font& fs = kit_.font(kFontSmall);
    const Font& fn = kit_.font(kFontNormal);
    const float lh = static_cast<float>(fs.lineHeight() > 0 ? fs.lineHeight() : 12);
    const uint32_t frameNo = spellFrame_;

    // Text tokens ($N, $C, $R, $G...) as upstream resolveTextTokens.
    game::TextSubject subject;
    if (const game::Character* me = session_.activeCharacter()) {
        subject.name = me->name;
        // The ruRU names of the player's gender (text.pak), upstream's English without it.
        const RuText& rt = ruText();
        const int fem = me->gender == game::Gender::FEMALE ? 1 : 0;
        const auto ci = static_cast<size_t>(me->characterClass), ri = static_cast<size_t>(me->race);
        subject.className = ci < 11 && !rt.cls[ci][0].empty()
                                ? (fem && !rt.cls[ci][1].empty() ? rt.cls[ci][1] : rt.cls[ci][0])
                                : game::getClassName(me->characterClass);
        subject.raceName = ri < 12 && !rt.race[ri][0].empty()
                               ? (fem && !rt.race[ri][1].empty() ? rt.race[ri][1] : rt.race[ri][0])
                               : game::getRaceName(me->race);
        subject.gender = me->gender;
    }
    auto tokens = [&](const std::string& s) { return declineMarkup(game::resolveTextTokens(s, subject)); };
    auto itemName = [&](uint32_t id) -> std::string {
        const pp::NpcItemInfo* info = session_.npcItemInfo(id);
        return info && !info->name.empty() ? info->name : std::string();
    };

    // --- The frame -------------------------------------------------------------------
    uint64_t npcGuid = 0;
    if (w == kWinGossip) npcGuid = session_.getCurrentGossip().npcGuid;
    else if (w == kWinQuestDetails) npcGuid = session_.getQuestDetails().npcGuid;
    else if (w == kWinQuestProgress) npcGuid = session_.getQuestRequestItems().npcGuid;
    else npcGuid = session_.getQuestOfferReward().npcGuid;
    // The portrait circle (GossipFramePortrait 60 x 60 at 7,-6): the NPC's model, under the art.
    // Round (SetPortraitTexture is circular; the square stuck out of the ring).
    if (const pp::Texture* face = portraitOf(npcGuid)) ui.imageCircle(*face, FR(7, 6, 60, 60));
    else ui.fillCircle(FR(9, 8, 56, 56), 0xFF101010u);
    ui.image(npcArt_.frame, Rect{kFx, kFy, 384 * kS, 448 * kS});
    {
        UnitView u;
        const std::string name = unitView(npcGuid, u) && !u.name.empty() ? u.name : std::string();
        const Rect nr = FR(86, 20, 238, 20);  // right of the portrait circle
        // A long name: the small font, then a marquee (the PSP font is wider than GameFontHighlight).
        const Font& nf = static_cast<float>(fn.measure(name.c_str())) <= nr.w ? fn : fs;
        if (static_cast<float>(nf.measure(name.c_str())) <= nr.w) {
            ui.textShadow(nf, nr.x + nr.w * 0.5f, nr.y + (nr.h - nf.lineHeight()) * 0.5f, name.c_str(), 0xFFFFFFFFu,
                          Align::Center);
        } else {
            ui.textMarquee(fs, nr, name.c_str(), 0xFFFFFFFFu);
        }
    }
    {
        const Rect cr = Rect{kFx + (384 - 42) * kS - 10, kFy + 31 * kS - 10, 20, 20};
        const bool f = top && winFocus_.addStop(kIdNpcClose, cr);
        const bool p = f && winFocus_.isPressed(kIdNpcClose);
        if (p) npc_.guard = false;
        const pp::Texture& t = p && npcArt_.closeDown.pixels ? npcArt_.closeDown : npcArt_.close;
        if (t.pixels) ui.image(t, cr);
        if (f && npcArt_.btnHl.pixels) ui.image(npcArt_.titleHl, cr, 0xFFFFFFFFu, Blend::Add);
    }

    // --- The scroll frame's content ------------------------------------------------
    const Rect view = FR(23, 81, 300, 334);
    const float tx = view.x + 10 * kS, tw = 270 * kS;
    struct Line {
        enum Kind : uint8_t { Text, Head, Row, Item, Money, Gap } kind;
        std::string s;
        uint32_t col = 0;
        uint32_t id = 0;       // focus id (rows, choices)
        uint8_t icon = 0;      // row icon
        uint32_t value = 0;    // money / count
        uint32_t display = 0;  // item icon
        bool selected = false;
        float h = 0;
        uint32_t itemId = 0;  // the item (its GameTooltip, L + Triangle)
    };
    std::vector<Line> lines;
    lines.reserve(64);
    auto addText = [&](const std::string& text, uint32_t col, bool head) {
        const Font& f = head ? fn : fs;
        for (const std::string& l : wrapText(f, text, tw)) {
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
    auto addMoney = [&](uint32_t copper, const char* label) {
        if (label) addText(label, kBlack, false);
        Line ln{Line::Money, std::string(), 0};
        ln.value = copper;
        ln.h = lh + 2;
        lines.push_back(std::move(ln));
    };
    // Item buttons two to a line (QuestInfoItem 1..10: left, right).
    // every item button takes the cursor (QuestInfoItem OnEnter: GameTooltip:SetQuestItem); the
    // ones that are not a reward choice get the ids 0x6380.. (WorldScreen::dressUpAction: Cross does nothing).
    uint32_t dressUpItemId = 0x6380;
    auto addItems = [&](const std::vector<game::QuestRewardItem>& items, bool choose) {
        for (size_t i = 0; i < items.size(); ++i) {
            Line ln{Line::Item, itemName(items[i].itemId), 0};
            const pp::NpcItemInfo* info = session_.npcItemInfo(items[i].itemId);
            ln.col = questQuality(info ? info->quality : 1);
            ln.value = items[i].count;
            ln.display = items[i].displayInfoId;
            ln.itemId = items[i].itemId;
            ln.id = choose ? kIdChoice0 + static_cast<uint32_t>(i) : (dressUpItemId < 0x63A0 ? dressUpItemId++ : 0);
            ln.selected = choose && npc_.rewardChoice == static_cast<int>(i);
            ln.h = (i % 2 == 1 || i + 1 == items.size()) ? 28.0f : 0.0f;  // the line advances after the right cell
            lines.push_back(std::move(ln));
        }
    };
    // QuestInfo_ShowRewards.
    auto addRewards = [&](uint32_t money, uint32_t xp, const std::vector<game::QuestRewardItem>& fixed,
                          const std::vector<game::QuestRewardItem>& choices, bool chooseMode) {
        if (money == 0 && xp == 0 && fixed.empty() && choices.empty()) return;
        gap(8);
        addText(CLIENT_STR(QUEST_REWARDS), kBlack, true);
        if (!choices.empty()) {
            addText(chooseMode ? CLIENT_STR(REWARD_CHOOSE) : CLIENT_STR(REWARD_CHOICES), kParch, false);
            gap(2);
            addItems(choices, chooseMode);
        }
        if (money || xp || !fixed.empty()) {
            gap(4);
            addText(choices.empty() ? CLIENT_STR(REWARD_ITEMS_ONLY) : CLIENT_STR(REWARD_ITEMS), kParch, false);
            if (money) addMoney(money, nullptr);
            if (xp) addText(std::string(CLIENT_STR(EXPERIENCE_COLON)) + " " + std::to_string(xp), kParch, false);
            gap(2);
            addItems(fixed, false);
        }
    };

    if (w == kWinGossip) {
        const auto& g = session_.getCurrentGossip();
        std::string greet = session_.getQuestGreeting();
        if (greet.empty() && g.titleTextId) greet = session_.getNpcText(g.titleTextId);
        addText(tokens(greet), kParch, false);
        gap(10);
        // GossipFrameUpdate: available quests, then active ones, then the options.
        auto questRow = [&](size_t i) {
            const auto& q = g.quests[i];
            const bool reward = q.questIcon == 5 || q.questIcon == 6 || q.questIcon == 10;
            const bool incomplete = q.questIcon == 3 || q.questIcon == 4;
            Line ln{Line::Row, tokens(q.title), kBlack};
            ln.id = kIdRow0 + static_cast<uint32_t>(i);
            ln.icon = reward ? (q.isRepeatable ? kNpcIconDailyActive : kNpcIconActive)
                             : incomplete ? kNpcIconIncomplete
                                          : (q.isRepeatable ? kNpcIconDaily : kNpcIconAvailable);
            if (incomplete) ln.col = 0xFF404040u;
            ln.h = std::max(lh, 12.0f) + 3;
            lines.push_back(std::move(ln));
        };
        bool any = false;
        for (size_t i = 0; i < g.quests.size() && i < 16; ++i) {
            const uint32_t ic = g.quests[i].questIcon;
            if (!(ic == 5 || ic == 6 || ic == 10 || ic == 3 || ic == 4)) {
                questRow(i);
                any = true;
            }
        }
        if (any) gap(6);
        any = false;
        for (size_t i = 0; i < g.quests.size() && i < 16; ++i) {
            const uint32_t ic = g.quests[i].questIcon;
            if (ic == 5 || ic == 6 || ic == 10 || ic == 3 || ic == 4) {
                questRow(i);
                any = true;
            }
        }
        if (any) gap(6);
        for (size_t o = 0; o < g.options.size() && g.quests.size() + o < 16; ++o) {
            const auto& opt = g.options[o];
            Line ln{Line::Row, tokens(opt.text), kBlack};
            ln.id = kIdRow0 + static_cast<uint32_t>(g.quests.size() + o);
            ln.icon = opt.icon <= 9 ? opt.icon : static_cast<uint8_t>(kNpcIconGossip);  // the client's 10 option icons
            ln.h = std::max(lh, 12.0f) + 3;
            lines.push_back(std::move(ln));
        }
    } else if (w == kWinQuestDetails) {
        const auto& d = session_.getQuestDetails();
        addText(tokens(d.title), kBlack, true);  // QuestInfo_ShowTitle
        gap(4);
        addText(tokens(d.details), kParch, false);
        if (!d.objectives.empty()) {
            gap(8);
            addText(CLIENT_STR(QUEST_OBJECTIVES), kBlack, true);
            gap(2);
            addText(tokens(d.objectives), kParch, false);
        }
        addRewards(d.rewardMoney, d.rewardXp, d.rewardItems, d.rewardChoiceItems, false);
    } else if (w == kWinQuestProgress) {
        const auto& q = session_.getQuestRequestItems();
        addText(tokens(q.title), kBlack, true);
        gap(4);
        addText(tokens(q.completionText), kParch, false);
        if (!q.requiredItems.empty()) {
            gap(8);
            addText(CLIENT_STR(TURN_IN_ITEMS), kBlack, true);
            gap(2);
            addItems(q.requiredItems, false);
        }
        if (q.requiredMoney) {
            gap(6);
            addMoney(q.requiredMoney, CLIENT_STR(REQUIRED_MONEY));
        }
    } else {
        const auto& q = session_.getQuestOfferReward();
        addText(tokens(q.title), kBlack, true);
        gap(4);
        addText(tokens(q.rewardText), kParch, false);
        addRewards(q.rewardMoney, q.rewardXp, q.fixedRewards, q.choiceRewards, true);
    }

    // Layout: y of each line; keep the focused row / choice in view.
    float total = 10 * kS;
    std::vector<float> ys(lines.size());
    const uint32_t focusedId = winFocus_.focused();
    float focusY = -1, focusH = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        ys[i] = total;
        if (lines[i].id && lines[i].id == focusedId) {
            focusY = total;
            focusH = lines[i].kind == Line::Item ? 28.0f : lines[i].h;
        }
        total += lines[i].h;
    }
    const float maxScroll = std::max(0.0f, total - view.h + 4);
    float scroll = std::clamp(static_cast<float>(npc_.scroll), 0.0f, maxScroll);
    if (top && focusY >= 0) {
        if (focusY < scroll) scroll = focusY;
        else if (focusY + focusH > scroll + view.h) scroll = focusY + focusH - view.h;
    }
    npc_.scroll = static_cast<int>(scroll);

    ui.pushClip(view);
    for (size_t i = 0; i < lines.size(); ++i) {
        const Line& ln = lines[i];
        const float y = view.y + ys[i] - scroll;
        const float rowH = ln.kind == Line::Item ? 28.0f : ln.h;
        if (y + rowH < view.y || y > view.y + view.h) continue;
        const bool visible = y >= view.y - 1 && y + rowH <= view.y + view.h + 1;
        switch (ln.kind) {
            case Line::Text:
                ui.text(fs, tx, y, ln.s.c_str(), ln.col);
                break;
            case Line::Head:
                ui.text(fn, tx, y, ln.s.c_str(), ln.col);
                break;
            case Line::Row: {
                const Rect rr{tx - 3, y, tw + 6, ln.h - 1};
                const bool focused = top && visible && winFocus_.addStop(ln.id, rr);
                if (focused && winFocus_.isPressed(ln.id)) npc_.guard = false;
                if (focused && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, rr, 0xFFFFFFFFu, Blend::Add);
                const pp::Texture& ic = npcArt_.icons[ln.icon < kNpcIconCount ? ln.icon : 0];
                if (ic.pixels) ui.image(ic, Rect{tx, y + (ln.h - 12) * 0.5f, 12, 12});
                ui.pushClip(Rect{tx + 15, y, tw - 15, ln.h});
                ui.text(fs, tx + 15, y + (ln.h - lh) * 0.5f, ln.s.c_str(), ln.col);
                ui.popClip();
                break;
            }
            case Line::Item: {
                // Which cell: count the items of this run before it.
                size_t k = 0;
                for (size_t j = i; j > 0 && lines[j - 1].kind == Line::Item; --j) ++k;
                const bool right = (k % 2) == 1;
                const float cy = y;  // the left cell is 0 high: both cells start on one line
                const float cx = tx + (right ? tw * 0.5f : 0.0f);
                const Rect icon{cx, cy + 2, 22, 22};
                bool focused = false;
                if (ln.id) {
                    focused = top && visible && winFocus_.addStop(ln.id, Rect{cx, cy, tw * 0.5f - 2, 26});
                    if (focused && winFocus_.isPressed(ln.id)) npc_.guard = false;
                }
                if (focused && ln.itemId) {  // GameTooltip:SetQuestItem (drawn after the windows)
                    tipProtoItem_ = ln.itemId;
                    tipProtoCount_ = std::max<uint32_t>(1u, ln.value);
                    tipProtoRect_ = Rect{icon.x + ui.offsetX(), icon.y + ui.offsetY(), icon.w, icon.h};
                }
                if (ln.selected || focused)
                    ui.fill(Rect{cx - 1, cy, tw * 0.5f - 2, 26}, ln.selected ? 0x6000D1FFu : 0x40FFFFFFu);
                if (const pp::Texture* t = itemIcons().byDisplay(ln.display, frameNo)) ui.image(*t, icon);
                else ui.fill(icon, 0xFF202020u);
                if (spellArt_.slot.pixels) {
                    const float c = 38.0f;
                    ui.image(spellArt_.slot, Rect{icon.x + 11 - c * 0.5f, icon.y + 11 - c * 0.5f, c, c});
                }
                if (ln.value > 1) {
                    char cnt[12];
                    std::snprintf(cnt, sizeof(cnt), "%u", static_cast<unsigned>(ln.value));
                    ui.textShadow(fs, icon.x + icon.w - 1, icon.y + icon.h - fs.lineHeight(), cnt, 0xFFFFFFFFu,
                                  Align::Right);
                }
                const float nx = cx + 25, nw = tw * 0.5f - 27;
                const auto nl = wrapText(fs, ln.s, nw);
                ui.pushClip(Rect{nx, cy, nw, 26});
                const float ny = cy + (26 - lh * static_cast<float>(std::min<size_t>(2, nl.size()))) * 0.5f;
                for (size_t n = 0; n < nl.size() && n < 2; ++n)
                    ui.text(fs, nx, ny + lh * static_cast<float>(n), nl[n].c_str(), ln.col == 0xFFFFFFFFu ? kParch : ln.col);
                ui.popClip();
                break;
            }
            case Line::Money: {
                // MoneyFrame: gold / silver / copper, each the amount and its coin.
                const uint32_t g = ln.value / 10000u, s = (ln.value / 100u) % 100u, c = ln.value % 100u;
                float x = tx;
                const uint32_t parts[3] = {g, s, c};
                for (int p = 0; p < 3; ++p) {
                    if (parts[p] == 0 && !(p == 2 && g == 0 && s == 0)) continue;
                    char b[12];
                    std::snprintf(b, sizeof(b), "%u", static_cast<unsigned>(parts[p]));
                    ui.textShadow(fs, x, y + 1, b, 0xFFFFFFFFu);
                    x += static_cast<float>(fs.measure(b)) + 1;
                    if (npcArt_.money.pixels) {
                        const float u0 = npcArt_.money.uw * 0.25f * static_cast<float>(p);
                        ui.image(npcArt_.money, Rect{x, y + 1, 10, 10},
                                 SrcRect{u0, 0, npcArt_.money.uw * 0.25f, static_cast<float>(npcArt_.money.uh)});
                    }
                    x += 14;
                }
                break;
            }
            case Line::Gap:
                break;
        }
    }
    ui.popClip();

    // The scroll bar's arrows (UIPanelScrollFrameTemplate) in the frame's right column.
    if (maxScroll > 0) {
        const Rect up = FR(327, 82, 18, 18), dn = FR(327, 396, 18, 18);
        const bool canUp = scroll > 0.5f, canDn = scroll < maxScroll - 0.5f;
        const pp::Texture& tu = canUp ? npcArt_.scrUp : npcArt_.scrUpDis;
        const pp::Texture& td = canDn ? npcArt_.scrDown : npcArt_.scrDownDis;
        if (tu.pixels) ui.image(tu, up);
        if (td.pixels) ui.image(td, dn);
        if (top && canUp && winFocus_.addStop(kIdTextUp, up) && npcArt_.btnHl.pixels)
            ui.image(npcArt_.titleHl, up, 0xFFFFFFFFu, Blend::Add);
        if (top && canDn && winFocus_.addStop(kIdTextDown, dn) && npcArt_.btnHl.pixels)
            ui.image(npcArt_.titleHl, dn, 0xFFFFFFFFu, Blend::Add);
    }

    // --- The buttons in the frame's bottom slots -------------------------------------
    const float by = kFy + 415 * kS, bw = 64, bh = 16;
    const Rect left{kFx + 20 * kS, by, bw, bh}, right{kFx + 346 * kS - bw, by, bw, bh};
    if (w == kWinGossip) {
        drawPanelButton(ui, kIdGoodbye, right, CLIENT_STR(GOODBYE), true, top);
    } else if (w == kWinQuestDetails) {
        drawPanelButton(ui, kIdAccept, left, CLIENT_STR(ACCEPT), true, top);
        drawPanelButton(ui, kIdDecline, right, CLIENT_STR(DECLINE), true, top);
    } else if (w == kWinQuestProgress) {
        drawPanelButton(ui, kIdComplete, left, CLIENT_STR(CONTINUE), session_.getQuestRequestItems().isCompletable(),
                        top);
        drawPanelButton(ui, kIdCancel, right, CLIENT_STR(CANCEL), true, top);
    } else {
        drawPanelButton(ui, kIdTakeReward, left, CLIENT_STR(COMPLETE_QUEST), true, top);
        drawPanelButton(ui, kIdCancel, right, CLIENT_STR(CANCEL), true, top);
    }
    return true;
}


// another character's PLAYER_LEVEL line (InspectPaperDollFrame_SetLevel:
// PLAYER_LEVEL (ruRU: positional, the class declined by |3-6) with UnitRace / UnitClass of his gender) - the names and
// the declension of text.pak as questText takes them for the player; a druid (class 11, past the table's 11 ids) by
// its ChrClasses name (DBC_CLASS_11).
std::string WorldScreen::unitLevelText(uint8_t race, uint8_t cls, bool female, const char* level) {
    const RuText& rt = ruText();
    const int sex = female ? 1 : 0;
    std::string raceName = race < 12 ? (!rt.race[race][sex].empty() ? rt.race[race][sex] : rt.race[race][0]) : "";
    std::string className = cls < 11 ? (!rt.cls[cls][sex].empty() ? rt.cls[cls][sex] : rt.cls[cls][0]) : "";
    if (className.empty() && cls == 11) className = female ? CLIENT_STR(DBC_CLASS_11_FEMALE) : CLIENT_STR(DBC_CLASS_11);
    return declineMarkup(CLIENT_FMT(PLAYER_LEVEL, level, raceName, className));
}

// the ChrClasses / ChrRaces ruRU names of a gender (GetFriendInfo / GetWhoInfo / GetGuildRosterInfo) and
// the |3-N(word) declension for the social lines - through the same text.pak tables (a druid, class 11, by its name).
std::string WorldScreen::socialClassName(uint8_t cls, bool female) {
    const RuText& rt = ruText();
    const int sex = female ? 1 : 0;
    std::string name = cls < 11 ? (!rt.cls[cls][sex].empty() ? rt.cls[cls][sex] : rt.cls[cls][0]) : "";
    if (name.empty() && cls == 11) name = female ? CLIENT_STR(DBC_CLASS_11_FEMALE) : CLIENT_STR(DBC_CLASS_11);
    return name;
}

std::string WorldScreen::socialRaceName(uint8_t race, bool female) {
    const RuText& rt = ruText();
    const int sex = female ? 1 : 0;
    return race < 12 ? (!rt.race[race][sex].empty() ? rt.race[race][sex] : rt.race[race][0]) : "";
}

std::string WorldScreen::socialDecline(const std::string& text) { return declineMarkup(text); }

}  // namespace wowee::ui::psp
