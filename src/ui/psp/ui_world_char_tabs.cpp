// The character window's Reputation and Skills tabs of the original ("every window
// 1:1 on the original art") - ReputationFrame.xml / .lua and SkillFrame.xml / .lua of the ruRU 3.3.5a client,
// drawn in CharacterFrame instead of PaperDollFrame (CharacterFrameTab3 / Tab4, ui_world_paperdoll.cpp).
//
// ReputationFrame:
//   - the frame (ui.pak rep_frame: the UI-Character-General quarters) at the paperdoll's 0.6 x 0.55, the
//     portrait, the name, the close button and the tabs of CharacterFrame; FACTION / STANDING;
//   - GetFactionInfo's rows = upstream GameHandler::getReputationList + getReputationRows: the factions the
//     server flags VISIBLE and not HIDDEN (SMSG_INITIALIZE_FACTIONS / SMSG_SET_FACTION_VISIBLE) under every
//     faction they descend from (Faction.dbc ParentFactionID - text.pak faction_text), each group where its first
//     member is in the server's list, depth first; ExpandFactionHeader / CollapseFactionHeader;
//   - NUM_FACTIONS_DISPLAYED (15) ReputationBars 23 units apart (FauxScrollFrame: its bar and arrows only for a
//     longer list - the rows then end at -70 instead of -46), ReputationFrame_SetRowType's four row types
//     (indent, name width, the bar ends of UI-Character-ReputationBar, the row background), the
//     ExpandOrCollapseButton (UI-PlusButton / UI-MinusButton), the tree lines (ReputationBar_DrawVerticalLine /
//     _DrawHorizontalLine);
//   - the bar: barMin / barMax / barValue of GetFactionInfo (reputation_standing.hpp) normalised, the colour
//     FACTION_BAR_COLORS[standingID], FACTION_STANDING_LABEL<id> (_FEMALE by UnitSex) on it - on the focus (OnEnter)
//     " value / max" instead; AtWarHighlight1/2 for a faction at war, Highlight1/2 on the focused row and on the
//     selected one while its details are shown;
//   - ReputationDetailFrame (✕ on a faction with a standing - ReputationBar_OnClick; again: hide): the name, the
//     description, AT_WAR (FactionToggleAtWar - disabled for a header or a PEACE_FORCED faction),
//     MOVE_TO_INACTIVE (SetFactionInactive / SetFactionActive - disabled for a header),
//     SHOW_FACTION_ON_MAINSCREEN (SetWatchedFactionIndex), their GameTooltips, the close button.
// SkillFrame:
//   - the frame (ui.pak skill_frame: the General top, SkillFrame-BotLeft / -BotRight, the horizontal bar, the
//     ALL tab);
//   - GetSkillLineInfo's rows = upstream's skillRows (lua_quest_api.cpp): the skills of PLAYER_SKILL_INFO_*
//     (WorldSession::playerSkills = SpellHandler::extractSkillFields) by name under their SkillLineCategory
//     heading in its SortIndex order, category 12 (not displayed) left out (text.pak skill_text);
//   - SKILLS_TO_DISPLAY (12) rows 18 units apart: SkillTypeLabel (+/−, the heading) or SkillRankFrame
//     (SkillFrame_SetStatusBar: the bar's colours, the name, "rank/max" or "rank (+mod)/max", a proficiency
//     (max 1) grey and full, the border and its highlight - locked on the selected skill);
//     SkillFrameCollapseAllButton ALL; CLOSE; the list's scroll bar;
//   - the details (SkillDetailFrame_SetStatusBar): the bar again, the description (SKILL_DESCRIPTION), the
//     unlearn button of a skill that can be abandoned -> StaticPopup UNLEARN_SKILL (UNLEARN / CANCEL, 60 s) ->
//     AbandonSkill (CMSG_UNLEARN_SKILL).
// PSP:
//   - the text on the bars (the standing, the skill name and rank) in the 8 px font: the 10 px one does not fit
//     the 101-unit bar (FACTION_STANDING_LABEL8) or the 15-unit skill bar;
//   - ReputationDetailFrame at the dialogs' 0.8 (kDialogScale): it is a dialog box, and the description did
//     not fit it at 0.6 x 0.55; the 8 px font when the 10 px one still does not fit;
//   - the selection is a faction / skill id (the original keeps a row index, which a collapse moves to
//     another row);
//   - a faction right under a main header is not a child (ReputationFrame.lua's row type 0: indent 44, the
//     13-unit line); upstream's getReputationRows makes every row under the top one a child;
//   - the inactive factions are listed last under FACTION_INACTIVE, as the client lists them;
//     upstream leaves them in their groups;
//   - ALL collapses / expands every heading (CollapseSkillHeader(0)); upstream ignores the index 0;
//   - a skill's rank is its value, the modifier its temporary + permanent bonus (the client's meaning);
//     upstream's GetSkillLineInfo gives the temporary bonus as numTempPoints, which SkillFrame.lua adds again;
//   - the unlearn button for a primary profession (SkillLine category 11) - what AzerothCore's
//     HandleUnlearnSkillOpcode accepts (the client's own rule, SkillRaceClassInfo, is not baked);
//   - ✕ on the selected skill again pages its description (as the quest log);
//   - the tree lines solid, half transparent, 1 px (the 2-unit dotted texture is 1.2 px here);
//   - the collapsed headers kept in wowee.cfg per character (repcollapsed_<name> / skillcollapsed_<name>) as
//     upstream's character config keeps collapsed_factions / collapsed_skill_categories;
//   - no newbie tooltips on FACTION / STANDING, no alert icon on the popup, no ReputationWatchBar (the main
//     menu bar's) - SHOW_FACTION_ON_MAINSCREEN only tells the server.
// Memory: the frame of the shown tab (T8 132 KB), its small art (~20-90 KB) and its text table (faction_text
// 36 KB / skill_text 13 KB) only while that tab is shown; the paperdoll's art is released meanwhile. Log:
// chartab_art, chartab_text, chartab_rows, chartab_release.
#include "ui/psp/ui_world_screen.hpp"

#include "game/character.hpp"
#include "game/reputation_standing.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"
#include "platform/psp/psp_texture.hpp"
#include "ui/psp/ui_world_sounds.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

// Widget ids 0x6D40-0x6DFF (ui_world_paperdoll.cpp paperDollAction passes them on).
enum : uint32_t {
    kIdTabClose = 0x6D40,
    kIdRepUp,
    kIdRepDown,
    kIdRepDetailClose,
    kIdRepAtWar,
    kIdRepInactive,
    kIdRepWatch,
    kIdSkillUp,
    kIdSkillDown,
    kIdSkillAll,
    kIdSkillClose,
    kIdSkillUnlearn,
    kIdSkillDescUp,
    kIdSkillDescDown,
    kIdUnlearnYes,
    kIdUnlearnNo,
    kIdRepRow0 = 0x6D60,    // 15: the rows (ReputationBar_OnClick)
    kIdRepBtn0 = 0x6D70,    // 15: a header's ExpandOrCollapseButton
    kIdSkillRow0 = 0x6D80,  // 12: SkillTypeLabel / SkillRankFrame's border
};

constexpr float kSX = 0.6f, kSY = 0.55f;  // ui_world_paperdoll.cpp (tools/psp/build_ui_pak.py PD_SX / PD_SY)
constexpr float kOx = 4.0f, kOy = 2.0f;
constexpr float kFrameW = 211.0f, kFrameH = 265.0f;
float PX(float x) { return kOx + x * kSX; }
float PY(float y) { return kOy + y * kSY; }
constexpr float kDS = 0.8f;  // ReputationDetailFrame: WorldScreen::kDialogScale (build_ui_pak.py REPD_S)

constexpr int kRepRows = 15;           // NUM_FACTIONS_DISPLAYED
constexpr float kRepPitch = 23.0f;     // REPUTATIONFRAME_ROWSPACING (20-unit rows, 3 apart)
constexpr int kSkillRows = 12;         // SKILLS_TO_DISPLAY
constexpr float kSkillPitch = 18.0f;   // 15-unit bars, 3 apart
constexpr uint32_t kInactiveHeader = 0x7FFFFFFFu;  // the FACTION_INACTIVE group's key (no faction id)
constexpr uint32_t kNotDisplayed = 12;             // SkillLineCategory "not displayed"
constexpr uint32_t kCategoryProfession = 11;       // SKILL_CATEGORY_PROFESSION

// upstream GameHandler::FACTION_FLAG_* (game_handler.hpp).
constexpr uint8_t kFlagVisible = 0x01, kFlagAtWar = 0x02, kFlagHidden = 0x04, kFlagPeaceForced = 0x10,
                  kFlagInactive = 0x20;

constexpr uint32_t kRed = 0xFF1A1AFFu;       // RED_FONT_COLOR (1, 0.1, 0.1)
constexpr uint32_t kGray = 0xFF808080u;      // GRAY_FONT_COLOR (0.5)
constexpr uint32_t kGreenCode = 0xFF20FF20u; // GREEN_FONT_COLOR_CODE |cff20ff20
constexpr uint32_t kRedCode = 0xFF2020FFu;   // RED_FONT_COLOR_CODE |cffff2020
constexpr uint32_t kLine = 0x80356F7Bu;      // UI-Character-ReputationLines (rgb 7b6f35, dotted -> half alpha)

uint32_t rgba(float r, float g, float b, float a) {
    auto c = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return (c(a) << 24) | (c(b) << 16) | (c(g) << 8) | c(r);
}

// FACTION_BAR_COLORS (ReputationFrame.lua).
uint32_t factionBarColor(int standingId) {
    switch (standingId) {
        case 1:
        case 2: return rgba(0.8f, 0.3f, 0.22f, 1.0f);
        case 3: return rgba(0.75f, 0.27f, 0.0f, 1.0f);
        case 4: return rgba(0.9f, 0.7f, 0.0f, 1.0f);
        default: return rgba(0.0f, 0.6f, 0.1f, 1.0f);
    }
}

// FACTION_STANDING_LABEL1..8 and _FEMALE (GlobalStrings.lua; GetText(.., UnitSex("player"))).
const char* standingLabel(int id, bool female) {
    using pp::CS;
    static constexpr CS kMale[8] = {CS::FACTION_STANDING_LABEL1, CS::FACTION_STANDING_LABEL2,
                                    CS::FACTION_STANDING_LABEL3, CS::FACTION_STANDING_LABEL4,
                                    CS::FACTION_STANDING_LABEL5, CS::FACTION_STANDING_LABEL6,
                                    CS::FACTION_STANDING_LABEL7, CS::FACTION_STANDING_LABEL8};
    static constexpr CS kFemale[8] = {CS::FACTION_STANDING_LABEL1_FEMALE, CS::FACTION_STANDING_LABEL2_FEMALE,
                                      CS::FACTION_STANDING_LABEL3_FEMALE, CS::FACTION_STANDING_LABEL4_FEMALE,
                                      CS::FACTION_STANDING_LABEL5_FEMALE, CS::FACTION_STANDING_LABEL6_FEMALE,
                                      CS::FACTION_STANDING_LABEL7_FEMALE, CS::FACTION_STANDING_LABEL8_FEMALE};
    const int i = std::clamp(id, 1, 8) - 1;
    return pp::cs(female ? kFemale[i] : kMale[i]);
}

void loadTextures(std::initializer_list<std::pair<const char*, pp::Texture*>> list) {
    char path[256];
    pp::fsPath(path, sizeof(path), "data/ui.pak");
    pp::Pak pak;
    if (!pak.open(path)) return;
    for (const auto& [name, tex] : list) {
        if (tex->pixels) continue;
        const pp::PakEntry* e = pak.find(name);
        void* blob = e ? pak.load(e) : nullptr;
        if (blob && !pp::textureFromBlob(blob, e->size, *tex, true)) pp::pakFree(blob);
    }
    pak.close();
}

// A text.pak entry, whole (closed at once: the card's open-file limit).
void* loadTextEntry(const char* entry, uint32_t& size) {
    size = 0;
    char path[256];
    pp::fsPath(path, sizeof(path), "data/text.pak");
    pp::Pak pak;
    if (!pak.open(path)) return nullptr;
    const pp::PakEntry* e = pak.find(entry);
    void* blob = e ? pak.load(e) : nullptr;
    if (blob) size = e->size;
    pak.close();
    return blob;
}

const char* readCString(const char*& p, const char* end) {
    const char* s = p;
    while (p < end && *p) ++p;
    if (p >= end) return nullptr;  // not terminated: a broken entry
    ++p;
    return s;
}

std::string cfgKey(const char* prefix, const std::string& name) {
    std::string k = prefix;
    for (char c : name) k += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    return k;
}

}  // namespace

// --------------------------------------------------------------------------------------------------------------

struct WorldScreen::CharTabs {
    // Art (ui.pak): the shown tab's only.
    pp::Texture repFrame, repScrollTex, repDetailTex, repBar, repBarHl;
    pp::Texture skillFrame, skillScrollTex, skillDScrollTex, skillBorder, skillBorderHl, unlearn, unlearnDn, unlearnHl;
    pp::Texture bar, plus, minus, plusHl, cbUp, cbHl, cbSword, cbCheck, cbCheckDis;
    uint8_t artTab = 0;  // the tab whose art is loaded (kCharTabRep / kCharTabSkills), 0 none

    // Texts (text.pak), the shown tab's only.
    struct FacText {
        uint32_t id = 0, repListId = 0, parent = 0;
        const char* name = "";
        const char* desc = "";
    };
    struct CatText {
        uint32_t id = 0, sort = 0;
        const char* name = "";
    };
    struct SkillText {
        uint32_t id = 0, category = 0;
        const char* name = "";
        const char* desc = "";
    };
    void* facBlob = nullptr;
    uint32_t facBlobSize = 0;
    std::vector<FacText> facs;        // by id
    std::vector<int16_t> facByRep;    // ReputationListID -> facs index (-1)
    void* skillBlob = nullptr;
    uint32_t skillBlobSize = 0;
    std::vector<CatText> cats;        // by id
    std::vector<SkillText> skillTexts;  // by id
    bool facTried = false, skillTried = false;

    // Collapsed headers (upstream collapsedFactionIds_ / collapsedSkillCategories_), per character.
    std::vector<uint32_t> repCollapsed, skillCollapsed;
    std::string collapsedFor;

    // ReputationFrame.
    struct RepRow {
        uint32_t factionId = 0, repListId = 0;
        bool isHeader = false, isChild = false, hasRep = false;
        uint8_t flags = 0;
        int32_t standing = 0;
    };
    std::vector<RepRow> repRows;   // GetFactionInfo's rows
    std::vector<RepRow> repAll;    // every visible faction with a standing (the details' source)
    uint32_t repVersion = ~0u;
    bool repDirty = true;
    int repScroll = 0;
    uint32_t repSel = 0;           // GetSelectedFaction (a faction id here)
    bool repDetail = false;        // ReputationDetailFrame shown
    int repVisible[kRepRows] = {}; // per drawn row: the repRows index (-1)

    // SkillFrame.
    struct SkillRow {
        uint32_t skillId = 0, category = 0;
        bool isHeader = false;
    };
    std::vector<SkillRow> skillRows;
    uint32_t skillVersion = ~0u;
    bool skillDirty = true;
    int skillScroll = 0;
    uint32_t skillSel = 0;         // GetSelectedSkill (a skill id here)
    float descScroll = 0.0f, descMax = 0.0f;
    int skillVisible[kSkillRows] = {};
    uint32_t unlearnSkill = 0, unlearnSinceMs = 0;  // StaticPopup UNLEARN_SKILL's data / when shown

    // GameTooltip (drawn after all windows).
    const char* tip = nullptr;
    Rect tipAnchor{};
    uint32_t logs = 0;

    ~CharTabs() {
        releaseArt();
        releaseTexts();
    }
    uint32_t artBytes() const {
        uint32_t n = 0;
        for (const pp::Texture* t : {&repFrame, &repScrollTex, &repDetailTex, &repBar, &repBarHl, &skillFrame, &skillScrollTex,
                                     &skillDScrollTex, &skillBorder, &skillBorderHl, &unlearn, &unlearnDn, &unlearnHl, &bar,
                                     &plus, &minus, &plusHl, &cbUp, &cbHl, &cbSword, &cbCheck, &cbCheckDis})
            if (t->pixels) n += pp::textureBytes(*t);
        return n;
    }
    void releaseArt() {
        for (pp::Texture* t : {&repFrame, &repScrollTex, &repDetailTex, &repBar, &repBarHl, &skillFrame, &skillScrollTex,
                               &skillDScrollTex, &skillBorder, &skillBorderHl, &unlearn, &unlearnDn, &unlearnHl, &bar, &plus,
                               &minus, &plusHl, &cbUp, &cbHl, &cbSword, &cbCheck, &cbCheckDis})
            if (t->pixels) pp::textureRelease(*t);
        artTab = 0;
    }
    void releaseFactions() {
        if (facBlob) pp::pakFree(facBlob);
        facBlob = nullptr;
        facBlobSize = 0;
        facs.clear();
        facs.shrink_to_fit();
        facByRep.clear();
        facByRep.shrink_to_fit();
        repRows.clear();
        repRows.shrink_to_fit();
        repAll.clear();
        repAll.shrink_to_fit();
        facTried = false;
        repDirty = true;
    }
    void releaseSkills() {
        if (skillBlob) pp::pakFree(skillBlob);
        skillBlob = nullptr;
        skillBlobSize = 0;
        cats.clear();
        cats.shrink_to_fit();
        skillTexts.clear();
        skillTexts.shrink_to_fit();
        skillRows.clear();
        skillRows.shrink_to_fit();
        skillTried = false;
        skillDirty = true;
    }
    void releaseTexts() {
        releaseFactions();
        releaseSkills();
    }

    // The art of `tab`, the other tab's released (one frame in memory).
    void loadArt(uint8_t tab) {
        if (artTab == tab) return;
        releaseArt();
        artTab = tab;
        if (tab == kCharTabRep) {
            loadTextures({{"rep_frame", &repFrame}, {"rep_scroll", &repScrollTex}, {"rep_detail", &repDetailTex},
                          {"rep_bar", &repBar}, {"rep_bar_hl", &repBarHl}, {"skill_bar", &bar}, {"ql_plus", &plus},
                          {"ql_minus", &minus}, {"ct_plushl", &plusHl}, {"cb_up", &cbUp}, {"cb_hl", &cbHl},
                          {"cb_sword", &cbSword}, {"ql_check", &cbCheck}, {"cb_check_dis", &cbCheckDis}});
        } else {
            loadTextures({{"skill_frame", &skillFrame}, {"skill_scroll", &skillScrollTex}, {"skill_dscroll", &skillDScrollTex},
                          {"skill_bar", &bar}, {"skill_border", &skillBorder}, {"skill_border_hl", &skillBorderHl},
                          {"unlearn_btn", &unlearn}, {"unlearn_btn_dn", &unlearnDn}, {"unlearn_btn_hl", &unlearnHl},
                          {"ql_plus", &plus}, {"ql_minus", &minus}, {"ct_plushl", &plusHl}});
        }
        PSP_LOGI(pp::tag::Gfx, "chartab_art tab=%u frame=%d kb=%u", static_cast<unsigned>(tab),
                 (tab == kCharTabRep ? repFrame.pixels : skillFrame.pixels) ? 1 : 0,
                 static_cast<unsigned>(artBytes() / 1024u));
    }

    // text.pak faction_text: the whole table while the tab is shown (upstream loadFactionNameCache).
    void loadFactions() {
        if (facTried) return;
        facTried = true;
        facBlob = loadTextEntry("faction_text", facBlobSize);
        const char* p = static_cast<const char*>(facBlob);
        const char* end = p ? p + facBlobSize : nullptr;
        if (p && facBlobSize >= 4) {
            uint32_t n = 0;
            std::memcpy(&n, p, 4);
            p += 4;
            facs.reserve(std::min<uint32_t>(n, facBlobSize / 14u));
            for (uint32_t i = 0; i < n && p + 12 <= end; ++i) {
                FacText f;
                std::memcpy(&f.id, p, 4);
                std::memcpy(&f.repListId, p + 4, 4);
                std::memcpy(&f.parent, p + 8, 4);
                p += 12;
                const char* name = readCString(p, end);
                const char* desc = name ? readCString(p, end) : nullptr;
                if (!desc) break;
                f.name = name;
                f.desc = desc;
                facs.push_back(f);
            }
            std::sort(facs.begin(), facs.end(), [](const FacText& a, const FacText& b) { return a.id < b.id; });
            for (size_t i = 0; i < facs.size(); ++i) {
                const uint32_t rl = facs[i].repListId;
                if (rl == 0xFFFFFFFFu || rl > 4096u) continue;
                if (facByRep.size() <= rl) facByRep.resize(rl + 1, -1);
                facByRep[rl] = static_cast<int16_t>(i);
            }
        }
        PSP_LOGI(pp::tag::Gfx, "chartab_text kind=factions n=%u kb=%u", static_cast<unsigned>(facs.size()),
                 static_cast<unsigned>((facBlobSize + facs.capacity() * sizeof(FacText)) / 1024u));
    }
    const FacText* faction(uint32_t id) const {
        auto it = std::lower_bound(facs.begin(), facs.end(), id, [](const FacText& f, uint32_t v) { return f.id < v; });
        return it != facs.end() && it->id == id ? &*it : nullptr;
    }
    uint32_t parentOf(uint32_t id) const {  // upstream getFactionParentId
        const FacText* f = faction(id);
        return f && f->parent != 0 && f->parent != id ? f->parent : 0u;
    }
    const char* factionName(uint32_t id) const {
        if (id == kInactiveHeader) return CLIENT_STR(FACTION_INACTIVE);
        const FacText* f = faction(id);
        return f ? f->name : "";
    }

    // text.pak skill_text (upstream loadSkillLineDbc).
    void loadSkills() {
        if (skillTried) return;
        skillTried = true;
        skillBlob = loadTextEntry("skill_text", skillBlobSize);
        const char* p = static_cast<const char*>(skillBlob);
        const char* end = p ? p + skillBlobSize : nullptr;
        if (p && skillBlobSize >= 4) {
            uint32_t n = 0;
            std::memcpy(&n, p, 4);
            p += 4;
            bool ok = true;
            for (uint32_t i = 0; i < n && ok; ++i) {
                if (p + 8 > end) break;
                CatText c;
                std::memcpy(&c.id, p, 4);
                std::memcpy(&c.sort, p + 4, 4);
                p += 8;
                const char* name = readCString(p, end);
                ok = name != nullptr;
                if (ok) {
                    c.name = name;
                    cats.push_back(c);
                }
            }
            if (ok && p + 4 <= end) {
                std::memcpy(&n, p, 4);
                p += 4;
                skillTexts.reserve(std::min<uint32_t>(n, skillBlobSize / 10u));
                for (uint32_t i = 0; i < n && p + 8 <= end; ++i) {
                    SkillText s;
                    std::memcpy(&s.id, p, 4);
                    std::memcpy(&s.category, p + 4, 4);
                    p += 8;
                    const char* name = readCString(p, end);
                    const char* desc = name ? readCString(p, end) : nullptr;
                    if (!desc) break;
                    s.name = name;
                    s.desc = desc;
                    skillTexts.push_back(s);
                }
            }
            std::sort(cats.begin(), cats.end(), [](const CatText& a, const CatText& b) { return a.id < b.id; });
            std::sort(skillTexts.begin(), skillTexts.end(),
                      [](const SkillText& a, const SkillText& b) { return a.id < b.id; });
        }
        PSP_LOGI(pp::tag::Gfx, "chartab_text kind=skills cats=%u lines=%u kb=%u", static_cast<unsigned>(cats.size()),
                 static_cast<unsigned>(skillTexts.size()), static_cast<unsigned>(skillBlobSize / 1024u));
    }
    const SkillText* skillText(uint32_t id) const {
        auto it = std::lower_bound(skillTexts.begin(), skillTexts.end(), id,
                                   [](const SkillText& s, uint32_t v) { return s.id < v; });
        return it != skillTexts.end() && it->id == id ? &*it : nullptr;
    }
    const CatText* category(uint32_t id) const {
        for (const CatText& c : cats)
            if (c.id == id) return &c;
        return nullptr;
    }

    // The collapsed headers of this character (wowee.cfg; upstream's character config).
    void loadCollapsed(const std::string& name) {
        if (collapsedFor == name) return;
        collapsedFor = name;
        auto parse = [](const std::string& v, std::vector<uint32_t>& out) {
            out.clear();
            const char* s = v.c_str();
            while (*s) {
                char* e = nullptr;
                const unsigned long id = std::strtoul(s, &e, 10);
                if (e == s) break;
                if (id != 0) out.push_back(static_cast<uint32_t>(id));
                s = *e == ',' ? e + 1 : e;
            }
        };
        parse(pp::configGet(cfgKey("repcollapsed_", name).c_str(), ""), repCollapsed);
        parse(pp::configGet(cfgKey("skillcollapsed_", name).c_str(), ""), skillCollapsed);
    }
    void saveCollapsed(bool rep) {
        const std::vector<uint32_t>& ids = rep ? repCollapsed : skillCollapsed;
        std::string v;
        for (uint32_t id : ids) {
            if (!v.empty()) v += ',';
            v += std::to_string(id);
        }
        pp::configSet(cfgKey(rep ? "repcollapsed_" : "skillcollapsed_", collapsedFor).c_str(), v);
        pp::configSave();
    }
    static bool has(const std::vector<uint32_t>& v, uint32_t id) { return std::find(v.begin(), v.end(), id) != v.end(); }
    static void toggle(std::vector<uint32_t>& v, uint32_t id, bool collapsed) {
        auto it = std::find(v.begin(), v.end(), id);
        if (collapsed && it == v.end()) v.push_back(id);
        if (!collapsed && it != v.end()) v.erase(it);
    }

    // upstream GameHandler::getReputationList + getReputationRows (game_handler.cpp), see the top for the PSP
    // differences (the row type 0, the inactive group).
    void buildRepRows(pp::WorldSession& s) {
        repRows.clear();
        repAll.clear();
        struct Entry {
            uint32_t factionId, repListId;
            uint8_t flags;
            int32_t standing;
        };
        std::vector<Entry> flat, inactive;
        const uint32_t n = s.factionStandingCount();
        for (uint32_t rep = 0; rep < n; ++rep) {
            uint8_t flags = 0;
            int32_t standing = 0;
            if (!s.factionStandingAt(rep, flags, standing)) continue;
            // Hidden factions are never shown, and one the player has not met yet is not visible either.
            if ((flags & kFlagHidden) != 0 || (flags & kFlagVisible) == 0) continue;
            const int fi = rep < facByRep.size() ? facByRep[rep] : -1;
            if (fi < 0) continue;
            const Entry e{facs[static_cast<size_t>(fi)].id, rep, flags, standing};
            ((flags & kFlagInactive) != 0 ? inactive : flat).push_back(e);
            repAll.push_back(RepRow{e.factionId, rep, false, false, true, flags, standing});
        }
        // Everything drawn: the visible factions, plus every faction one of them descends from.
        std::unordered_map<uint32_t, const Entry*> byId;
        std::unordered_set<uint32_t> displayed;
        for (const Entry& e : flat) {
            byId[e.factionId] = &e;
            displayed.insert(e.factionId);
        }
        for (const Entry& e : flat) {
            uint32_t p = parentOf(e.factionId);
            for (int depth = 0; p != 0 && depth < 8; ++depth) {
                displayed.insert(p);
                p = parentOf(p);
            }
        }
        // Where each group sits: at its first member's place in the server's list.
        constexpr size_t kNoRank = static_cast<size_t>(-1);
        std::unordered_map<uint32_t, size_t> rank;
        for (uint32_t id : displayed) rank[id] = kNoRank;
        for (size_t i = 0; i < flat.size(); ++i) {
            uint32_t id = flat[i].factionId;
            for (int depth = 0; id != 0 && depth < 9; ++depth) {
                auto it = rank.find(id);
                if (it != rank.end() && i < it->second) it->second = i;
                id = parentOf(id);
            }
        }
        // The tree, with a parent outside the drawn set counting as no parent.
        std::unordered_map<uint32_t, std::vector<uint32_t>> children;
        for (uint32_t id : displayed) {
            const uint32_t parent = parentOf(id);
            children[displayed.count(parent) ? parent : 0u].push_back(id);
        }
        const auto byRank = [&](uint32_t a, uint32_t b) {
            if (rank[a] != rank[b]) return rank[a] < rank[b];
            return std::strcmp(factionName(a), factionName(b)) < 0;
        };
        for (auto& [parent, kids] : children) {
            (void)parent;
            std::sort(kids.begin(), kids.end(), byRank);
        }
        // Depth first, so a heading is immediately followed by what is under it.
        std::vector<std::pair<uint32_t, int>> stack;
        const std::vector<uint32_t>& top = children[0];
        for (auto it = top.rbegin(); it != top.rend(); ++it) stack.emplace_back(*it, 0);
        while (!stack.empty()) {
            const auto [factionId, depth] = stack.back();
            stack.pop_back();
            auto kidsIt = children.find(factionId);
            const bool isHeader = kidsIt != children.end() && !kidsIt->second.empty();
            RepRow row;
            row.factionId = factionId;
            row.isHeader = isHeader;
            // PSP: as ReputationFrame.lua's row types - a heading under a heading is a child, and a faction is a
            // child under a child heading; one right under a main heading is row type 0 (upstream: depth > 0).
            row.isChild = isHeader ? depth > 0 : depth > 1;
            auto entry = byId.find(factionId);
            if (entry != byId.end()) {
                row.repListId = entry->second->repListId;
                row.flags = entry->second->flags;
                row.standing = entry->second->standing;
                row.hasRep = true;
            }
            repRows.push_back(row);
            // A closed group keeps its heading and loses everything under it.
            if (!isHeader || has(repCollapsed, factionId)) continue;
            for (auto it = kidsIt->second.rbegin(); it != kidsIt->second.rend(); ++it)
                stack.emplace_back(*it, depth + 1);
        }
        // PSP: the client's inactive group (FACTION_INACTIVE) last, in the server's order.
        if (!inactive.empty()) {
            RepRow h;
            h.factionId = kInactiveHeader;
            h.isHeader = true;
            repRows.push_back(h);
            if (!has(repCollapsed, kInactiveHeader))
                for (const Entry& e : inactive)
                    repRows.push_back(RepRow{e.factionId, e.repListId, false, false, true, e.flags, e.standing});
        }
        if (++logs <= 30)
            PSP_LOGI(pp::tag::Gfx, "chartab_rows kind=rep rows=%u visible=%u inactive=%u", static_cast<unsigned>(repRows.size()),
                     static_cast<unsigned>(flat.size()), static_cast<unsigned>(inactive.size()));
    }
    const RepRow* repFaction(uint32_t factionId) const {
        for (const RepRow& r : repRows)
            if (r.factionId == factionId && r.hasRep) return &r;
        for (const RepRow& r : repAll)
            if (r.factionId == factionId) return &r;
        return nullptr;
    }

    // upstream skillOrder + skillRows (lua_quest_api.cpp).
    void buildSkillRows(pp::WorldSession& s) {
        skillRows.clear();
        const auto& skills = s.playerSkills();
        std::vector<uint32_t> ids;
        ids.reserve(skills.size());
        for (const auto& sk : skills) ids.push_back(sk.skillId);
        auto name = [&](uint32_t id) {
            const SkillText* t = skillText(id);
            return t ? t->name : "";
        };
        // By name, with the id breaking ties.
        std::sort(ids.begin(), ids.end(), [&](uint32_t a, uint32_t b) {
            const int c = std::strcmp(name(a), name(b));
            return c != 0 ? c < 0 : a < b;
        });
        std::map<std::pair<uint32_t, uint32_t>, std::vector<uint32_t>> grouped;
        for (uint32_t id : ids) {
            const SkillText* t = skillText(id);
            const uint32_t cat = t ? t->category : 0u;
            if (cat == kNotDisplayed) continue;
            const CatText* c = category(cat);
            grouped[{c ? c->sort : 0xFFFFFFFFu, cat}].push_back(id);
        }
        for (const auto& [key, list] : grouped) {
            const uint32_t cat = key.second;
            const CatText* c = category(cat);
            // A category with no name: its skills are still listed, just without a heading over them.
            if (cat != 0 && c && c->name[0]) {
                skillRows.push_back(SkillRow{0, cat, true});
                if (has(skillCollapsed, cat)) continue;
            }
            for (uint32_t id : list) skillRows.push_back(SkillRow{id, cat, false});
        }
        if (++logs <= 30)
            PSP_LOGI(pp::tag::Gfx, "chartab_rows kind=skills rows=%u skills=%u", static_cast<unsigned>(skillRows.size()),
                     static_cast<unsigned>(skills.size()));
    }
};

void WorldScreen::destroyCharTabs(CharTabs* p) { delete p; }

WorldScreen::CharTabs& WorldScreen::charTabs() {
    if (!charTabs_) charTabs_ = std::unique_ptr<CharTabs, void (*)(CharTabs*)>(new CharTabs(), &destroyCharTabs);
    return *charTabs_;
}

// CharacterFrameTab_OnClick -> ToggleCharacter -> CharacterFrame_ShowSubFrame (+ igCharacterInfoTab).
void WorldScreen::selectCharTab(uint8_t tab) {
    if (tab == charTab_) return;  // the selected tab is disabled (PanelTemplates_SetTab)
    charTab_ = tab;
    if (!playSoundByName("igCharacterInfoTab")) playUiSound(UiSound::ButtonClick);
    if ((tab == kCharTabPaperDoll || tab == kCharTabPet) && charTabs_) {  // the pet tab has neither
        const uint32_t kb = (charTabs_->artBytes() + charTabs_->facBlobSize + charTabs_->skillBlobSize) / 1024u;
        charTabs_->releaseArt();
        charTabs_->releaseTexts();
        PSP_LOGI(pp::tag::Gfx, "chartab_release kb=%u (tab 1)", static_cast<unsigned>(kb));
    } else if (charTabs_) {
        // The other tab's text goes (its art is swapped by loadArt).
        if (tab == kCharTabRep) charTabs_->releaseSkills();
        else charTabs_->releaseFactions();
    }
    if (windowOpen(kWinUnlearnSkill)) closeWindow(kWinUnlearnSkill);
    PSP_LOGI(pp::tag::Ctrl, "chartab tab=%u", static_cast<unsigned>(tab));
}

void WorldScreen::charTabsClosed() {
    if (charTab_ != kCharTabPaperDoll) charTab_ = kCharTabPaperDoll;  // ToggleCharacter("PaperDollFrame") next time
    if (!charTabs_ || (charTabs_->artTab == 0 && !charTabs_->facBlob && !charTabs_->skillBlob)) return;
    const uint32_t kb = (charTabs_->artBytes() + charTabs_->facBlobSize + charTabs_->skillBlobSize) / 1024u;
    charTabs_->releaseArt();
    charTabs_->releaseTexts();
    PSP_LOGI(pp::tag::Gfx, "chartab_release kb=%u (closed)", static_cast<unsigned>(kb));
}

// ---------------------------------------------------------------------------------------------------------------
// The tab, inside CharacterFrame (ui_world_inventory.cpp drawInventoryWindow -> drawPaperDoll -> here).

bool WorldScreen::drawCharTab(UiRenderer& ui, bool focusable) {
    CharTabs& t = charTabs();
    t.loadArt(charTab_);
    const pp::Texture& frame = charTab_ == kCharTabRep ? t.repFrame : t.skillFrame;
    if (!frame.pixels) {  // an old ui.pak: back to PaperDollFrame
        charTab_ = kCharTabPaperDoll;
        t.releaseArt();
        combatError_ = OWN_STR(CHARTAB_NO_ART);
        combatErrorUntilMs_ = pp::logUptimeMs() + 3000u;
        return false;
    }
    PROF_SCOPE("inventory.chartab");
    loadNpcArt(false);  // the close button, the scroll arrows, the panel buttons
    t.loadCollapsed(name_);
    t.tip = nullptr;
    const bool live = focusable && !windowOpen(kWinUnlearnSkill);  // the popup takes the focus
    const Font& fs = kit_.font(kFontSmall);
    const Font& fn = kit_.font(kFontNormal);
    const Font& ft = kit_.font(kFontTiny);
    const float lh = static_cast<float>(fs.lineHeight()), th = static_cast<float>(ft.lineHeight());
    const game::Character* me = session_.activeCharacter();
    const bool female = me && me->gender == game::Gender::FEMALE;
    const uint32_t focused = winFocus_.focused();

    // CharacterFramePortrait (under the frame), the frame, CharacterNameText, CharacterFrameCloseButton.
    {
        const Rect pr{PX(7), PY(6), 60 * kSX, 60 * kSY};
        const int race = me ? static_cast<int>(me->race) : 0, sex = female ? 1 : 0;
        if (race > 0 && race < 12 && ufArt_.portrait[race][sex].pixels) ui.imageCircle(ufArt_.portrait[race][sex], pr);
        else ui.fillCircle(Rect{pr.x + 2, pr.y + 2, pr.w - 4, pr.h - 4}, 0xFF101010u);
    }
    ui.image(frame, Rect{kOx, kOy, kFrameW, kFrameH});
    ui.textShadow(fn, PX(198), PY(24) - static_cast<float>(fn.lineHeight()) * 0.5f, name_.c_str(), kColGold,
                  Align::Center);
    {
        const Rect cr{PX(340) - 10, PY(25) - 10, 20, 20};
        const bool f = live && winFocus_.addStop(kIdTabClose, cr);
        const pp::Texture& tx = f && winFocus_.isPressed(kIdTabClose) && npcArt_.closeDown.pixels ? npcArt_.closeDown
                                                                                                  : npcArt_.close;
        if (tx.pixels) ui.image(tx, cr);
        if (f) ui.fill(cr, 0x40FFFFFFu);
    }

    // A StatusBar's fill: the texture cut at the value, tinted.
    auto statusBar = [&](const Rect& r, float frac, uint32_t color) {
        frac = std::clamp(frac, 0.0f, 1.0f);
        if (frac <= 0.0f || !t.bar.pixels) return;
        ui.image(t.bar, Rect{r.x, r.y, r.w * frac, r.h},
                 SrcRect{0, 0, static_cast<float>(t.bar.uw) * frac, static_cast<float>(t.bar.uh)}, color);
    };
    // FrameXML TexCoords of a texture as a content rectangle.
    auto tc = [](const pp::Texture& tx, float l, float r, float tp, float b) {
        return SrcRect{l * tx.uw, tp * tx.uh, (r - l) * tx.uw, (b - tp) * tx.uh};
    };
    // A scroll bar's arrows (UIPanelScrollBarTemplate; the bar's art is baked).
    auto arrows = [&](uint32_t upId, uint32_t downId, const Rect& up, const Rect& dn, bool canUp, bool canDn) {
        const pp::Texture& tu = canUp ? npcArt_.scrUp : npcArt_.scrUpDis;
        const pp::Texture& td = canDn ? npcArt_.scrDown : npcArt_.scrDownDis;
        if (tu.pixels) ui.image(tu, up);
        if (td.pixels) ui.image(td, dn);
        if (live && canUp && winFocus_.addStop(upId, up)) ui.fill(up, 0x40FFFFFFu);
        if (live && canDn && winFocus_.addStop(downId, dn)) ui.fill(dn, 0x40FFFFFFu);
    };
    // The list follows the focus (as the quest log): the focus on the first / last drawn row scrolls one row
    // while there is more, the focus staying on its row.
    auto follow = [&](int& scroll, int rows, int total, uint32_t id0, uint32_t id1) {
        for (uint32_t base : {id0, id1}) {
            if (base == 0 || focused < base || focused >= base + static_cast<uint32_t>(rows)) continue;
            const int vi = static_cast<int>(focused - base);
            if (vi == rows - 1 && scroll + rows < total) {
                ++scroll;
                winFocus_.setFocus(focused - 1);
            } else if (vi == 0 && scroll > 0) {
                --scroll;
                winFocus_.setFocus(focused + 1);
            }
        }
    };
    // A text in a box: clipped, scrolling on the focus when longer (as the other framed windows).
    auto boxText = [&](const Font& f, const Rect& r, const char* s, uint32_t col, bool focusedRow) {
        if (focusedRow && static_cast<float>(f.measure(s)) > r.w) {
            ui.textMarquee(f, r, s, col);
            return;
        }
        ui.pushClip(Rect{r.x, r.y - 1, r.w, r.h + 2});
        ui.textShadow(f, r.x, r.y, s, col);
        ui.popClip();
    };
    // A GameTooltip of a focused control, drawn after all windows (drawCharTabsOverlay).
    auto setTip = [&](const char* text, const Rect& r) {
        t.tip = text;
        t.tipAnchor = Rect{r.x + ui.offsetX(), r.y + ui.offsetY(), r.w, r.h};
    };

    if (charTab_ == kCharTabRep) {
        // ================================ ReputationFrame ================================
        t.loadFactions();
        if (t.repVersion != session_.factionVersion() || t.repDirty) {  // UPDATE_FACTION / a header toggled
            t.repVersion = session_.factionVersion();
            t.repDirty = false;
            t.buildRepRows(session_);
        }
        ui.textShadow(fn, PX(70), PY(57), CLIENT_STR(FACTION), kColWhite);     // GameFontHighlight
        ui.textShadow(fn, PX(215), PY(59), CLIENT_STR(STANDING), kColWhite);
        const int total = static_cast<int>(t.repRows.size());
        const bool scrollShown = total > kRepRows;  // FauxScrollFrame_Update
        t.repScroll = std::clamp(t.repScroll, 0, std::max(0, total - kRepRows));
        if (live) follow(t.repScroll, kRepRows, total, kIdRepRow0, kIdRepBtn0);
        const float rowRight = 384.0f - (scrollShown ? 70.0f : 46.0f);  // ReputationListScrollFrame OnShow / OnHide
        if (t.repDetail && !t.repFaction(t.repSel)) t.repDetail = false;

        // The tree lines (ReputationFrame_Update's walk): the vertical ones are the two top textures and each
        // header's BottomLine, a row's LeftLine joins it to the line on its left.
        struct VLine {
            float x = 0, top = 0, fudge = 0;
            int rows = 0;
            bool shown = false;
        };
        VLine vl[2 + kRepRows];
        vl[0] = VLine{35, 76, 5, 0, false};  // ReputationFrameTopTreeTexture (TOP at TOPLEFT 35,-76)
        vl[1] = VLine{53, 76, 5, 0, false};  // ReputationFrameTopTreeTexture2
        struct HLine {
            float x0 = 0, x1 = 0, y = 0;
            bool shown = false;
        };
        HLine hl[kRepRows];
        int prevBig = 0, prevBig2 = 1, prevRows = 0, prevRows2 = 0;
        auto drawVertical = [&](int idx, int rows) {  // ReputationBar_DrawVerticalLine
            vl[idx].rows = rows;
            vl[idx].shown = true;
        };
        static const float kLeft[4] = {44, 62, 20, 39};  // ReputationFrame_SetRowType: row types 0..3
        for (int v = 0; v < kRepRows; ++v) {
            t.repVisible[v] = -1;
            const int i = t.repScroll + v;
            if (i >= total) continue;
            t.repVisible[v] = i;
            const auto& r = t.repRows[static_cast<size_t>(i)];
            const int rt = (r.isChild ? 1 : 0) + (r.isHeader ? 2 : 0);
            const float rowL = kLeft[rt], cy = 93.0f + kRepPitch * static_cast<float>(v);
            vl[2 + v] = VLine{rowL + 3 + 6.5f + 5, cy, 0, 0, false};  // BottomLine: TOP at the button's CENTER +5
            if (r.isHeader && !r.isChild) {
                if (prevRows == 0) vl[prevBig].shown = false;
                prevBig = 2 + v;
                prevRows = 0;
            } else if (r.isHeader && r.isChild) {
                hl[v] = HLine{rowL + 3 + 3 - 11, rowL + 3 + 3, cy, true};  // 11 wide, RIGHT at the button's LEFT +3
                if (prevRows2 == 0) vl[prevBig2].shown = false;
                prevBig2 = 2 + v;
                prevRows2 = 0;
                drawVertical(prevBig, ++prevRows);
            } else if (r.isChild) {
                hl[v] = HLine{rowL + 3 - 11, rowL + 3, cy, true};  // RIGHT at the background's LEFT +3
                ++prevRows;
                drawVertical(prevBig2, ++prevRows2);
            } else {  // right under a main heading
                hl[v] = HLine{rowL + 3 - 13, rowL + 3, cy, true};
                drawVertical(prevBig, ++prevRows);
            }
        }
        for (int i = t.repScroll + kRepRows; i < total; ++i) {  // a line going off the panel to its next row
            const auto& r = t.repRows[static_cast<size_t>(i)];
            if (r.isHeader && !r.isChild) break;
            if ((r.isHeader && r.isChild) || !(r.isHeader || r.isChild)) {
                drawVertical(prevBig, prevRows + 1);
                break;
            }
            if (r.isChild) {
                drawVertical(prevBig2, prevRows2 + 1);
                break;
            }
        }
        for (const VLine& l : vl)
            if (l.shown && l.rows > 0)
                ui.fill(Rect{std::floor(PX(l.x - 1)), PY(l.top), 1.0f, (static_cast<float>(l.rows) * kRepPitch - l.fudge) * kSY},
                        kLine);
        for (const HLine& l : hl)
            if (l.shown) ui.fill(Rect{PX(l.x0), std::floor(PY(l.y)), (l.x1 - l.x0) * kSX, 1.0f}, kLine);

        // The rows (ReputationBarTemplate).
        for (int v = 0; v < kRepRows; ++v) {
            if (t.repVisible[v] < 0) continue;
            const auto& r = t.repRows[static_cast<size_t>(t.repVisible[v])];
            const int rt = (r.isChild ? 1 : 0) + (r.isHeader ? 2 : 0);
            const float rowL = kLeft[rt], cy = 93.0f + kRepPitch * static_cast<float>(v);
            const Rect row{PX(rowL), PY(cy - 10), (rowRight - rowL) * kSX, 20 * kSY};
            const bool showBar = r.hasRep || !r.isHeader;  // SetRowType: a header without a standing has no bar
            // Stops: a header has its ExpandOrCollapseButton (13 x 13 at LEFT 3, HitRectInsets 1,-4,-2,-2) and the
            // row; a header without a standing is one stop, the button (PSP: the row's click does nothing there).
            bool fRow = false, fBtn = false;
            const uint32_t rowId = kIdRepRow0 + static_cast<uint32_t>(v), btnId = kIdRepBtn0 + static_cast<uint32_t>(v);
            if (r.isHeader) {
                const float bx = r.hasRep ? rowL + 3 + 1 : rowL;
                const float bw = r.hasRep ? 13 - 1 + 4 : rowRight - rowL;
                fBtn = live && winFocus_.addStop(btnId, Rect{PX(bx), PY(cy - 8.5f), bw * kSX, 17 * kSY});
                if (r.hasRep) {
                    const float x0 = rowL + 3 + 13 + 4;
                    fRow = live && winFocus_.addStop(rowId, Rect{PX(x0), row.y, (rowRight - x0) * kSX, row.h});
                }
            } else {
                fRow = live && winFocus_.addStop(rowId, row);
            }
            // The bar: RIGHT of the row, 101 x 13 (a header 99), its ends of UI-Character-ReputationBar.
            const float barW = r.isHeader ? 99.0f : 101.0f, barL = rowRight - barW;
            const float endH = r.isHeader ? 15.0f : 21.0f, lw = r.isHeader ? 60.0f : 62.0f, rw = r.isHeader ? 39.0f : 42.0f;
            if (!r.isHeader && t.repBar.pixels)  // $parentBackground: the row's LEFT to the bar's left end
                ui.image(t.repBar, Rect{row.x, PY(cy - 10.5f), (barL - rowL) * kSX, 21 * kSY},
                         tc(t.repBar, 0.0f, 0.7578125f, 0.0f, 0.328125f));
            const auto& band = game::reputationStandingFor(r.standing);
            const int barMax = band.ceiling + 1 - band.floor;  // GetFactionInfo's barMax - barMin (normalised)
            const int barValue = r.standing - band.floor;
            if (showBar) {
                statusBar(Rect{PX(barL), PY(cy - 6.5f), barW * kSX, 13 * kSY},
                          barMax > 0 ? static_cast<float>(barValue) / static_cast<float>(barMax) : 0.0f,
                          factionBarColor(band.id));
                if (t.repBar.pixels) {
                    const Rect le{PX(barL), PY(cy - endH * 0.5f), lw * kSX, endH * kSY};
                    const Rect re{PX(barL + lw), PY(cy - endH * 0.5f), rw * kSX, endH * kSY};
                    if (r.isHeader) {
                        ui.image(t.repBar, le, tc(t.repBar, 0.765625f, 1.0f, 0.046875f, 0.28125f));
                        ui.image(t.repBar, re, tc(t.repBar, 0.0f, 0.15234375f, 0.390625f, 0.625f));
                    } else {
                        ui.image(t.repBar, le, tc(t.repBar, 0.7578125f, 1.0f, 0.0f, 0.328125f));
                        ui.image(t.repBar, re, tc(t.repBar, 0.0f, 0.1640625f, 0.34375f, 0.671875f));
                    }
                }
                // $parentFactionStanding at the bar's CENTER; OnEnter shows the tooltip " value / max" there.
                char num[40];
                const char* text = standingLabel(band.id, female);
                if (fRow) {
                    std::snprintf(num, sizeof(num), " %d / %d", barValue, barMax);
                    text = num;
                }
                ui.textShadow(ft, PX(barL + barW * 0.5f), PY(cy) - th * 0.5f, text, kColWhite, Align::Center);
                // AtWarHighlight1/2 (alpha 0.2, ADD): 3,-2 .. -1,-2 of the row, 17 high.
                if ((r.flags & kFlagAtWar) && r.hasRep && t.repBar.pixels) {
                    const float y0 = PY(cy - 10 + 2), h = 17 * kSY;
                    ui.image(t.repBar, Rect{PX(rowL + 3), y0, std::max(0.0f, rowRight - 1 - 103 - rowL - 3) * kSX, h},
                             tc(t.repBar, 0.25390625f, 1.0f, 0.375f, 0.640625f), 0x33FFFFFFu, Blend::Add);
                    ui.image(t.repBar, Rect{PX(rowRight - 1 - 103), y0, 103 * kSX, h},
                             tc(t.repBar, 0.0f, 0.40234375f, 0.734375f, 1.0f), 0x33FFFFFFu, Blend::Add);
                }
                // Highlight1/2 (ADD): the focused row (OnEnter) and the selected one while its details are shown.
                if ((fRow || (t.repDetail && t.repSel == r.factionId && r.hasRep)) && t.repBarHl.pixels) {
                    const float y0 = PY(cy - 14), h = 28 * kSY;
                    ui.image(t.repBarHl, Rect{PX(rowL - 2), y0, (rowRight - 10 - rowL + 2) * kSX, h},
                             tc(t.repBarHl, 0.0f, 1.0f, 0.0f, 0.4375f), 0xFFFFFFFFu, Blend::Add);
                    ui.image(t.repBarHl, Rect{PX(rowRight - 13), y0, 17 * kSX, h},
                             tc(t.repBarHl, 0.0f, 0.06640625f, 0.4375f, 0.875f), 0xFFFFFFFFu, Blend::Add);
                }
            }
            // The ExpandOrCollapseButton: UI-MinusButton-Up / UI-PlusButton-Up, UI-PlusButton-Hilight (ADD).
            float nameX = rowL + 10, nameW = rt == 0 ? 160.0f : 150.0f;
            if (r.isHeader) {
                const Rect ic{PX(rowL + 6), PY(cy) - 5.0f, 10, 10};
                const pp::Texture& tx = CharTabs::has(t.repCollapsed, r.factionId) ? t.plus : t.minus;
                if (tx.pixels) ui.image(tx, ic);
                if (fBtn && t.plusHl.pixels) ui.image(t.plusHl, ic, 0xFFFFFFFFu, Blend::Add);
                nameX = rowL + 3 + 13 + 10;
                nameW = rt == 2 ? 145.0f : 135.0f;
            }
            // $parentFactionName: GameFontHighlightSmall (white); a header GameFontNormalLeft (gold).
            const float nw = std::max(0.0f, std::min(nameW, (showBar ? barL : rowRight) - 2 - nameX)) * kSX;
            const uint32_t col = r.isHeader && !(fBtn || fRow) ? kColGold : kColWhite;
            boxText(fs, Rect{PX(nameX), PY(cy) - lh * 0.5f, nw, lh}, t.factionName(r.factionId), col, fRow || fBtn);
        }
        if (total == 0 && t.facs.empty())  // PSP: an old text.pak says so
            ui.textShadow(fs, PX(44), PY(83), OWN_STR(CHARTAB_NO_FACTION_TEXT), kGray);
        // ReputationListScrollFrame's bar (rep_scroll: x 316, y 71..434) and its arrows.
        if (scrollShown) {
            if (t.repScrollTex.pixels) ui.image(t.repScrollTex, Rect{PX(316), PY(71), 31 * kSX, (434 - 71) * kSY});
            arrows(kIdRepUp, kIdRepDown, Rect{PX(322), PY(77), 11, 11}, Rect{PX(322), PY(428) - 11, 11, 11},
                   t.repScroll > 0, t.repScroll + kRepRows < total);
        }

        // ReputationDetailFrame: TOPLEFT at the frame's TOPRIGHT -33,-28; PSP: at the dialogs' 0.8.
        if (const CharTabs::RepRow* sel = t.repDetail ? t.repFaction(t.repSel) : nullptr) {
            const float dx = std::floor(PX(384 - 33)), dy = std::floor(PY(28));
            auto D = [&](float x, float y, float w, float h) { return Rect{dx + x * kDS, dy + y * kDS, w * kDS, h * kDS}; };
            const Rect box = D(0, 0, 212, 203);
            if (t.repDetailTex.pixels) ui.image(t.repDetailTex, box);
            else kit_.panel(ui, box);
            {  // ReputationDetailCloseButton (UIPanelCloseButton, TOPRIGHT -3,-3)
                const Rect cr{box.x + box.w - 3 * kDS - 22, box.y + 3 * kDS + 2, 20, 20};
                const bool f = live && winFocus_.addStop(kIdRepDetailClose, cr);
                const pp::Texture& tx = f && winFocus_.isPressed(kIdRepDetailClose) && npcArt_.closeDown.pixels
                                            ? npcArt_.closeDown
                                            : npcArt_.close;
                if (tx.pixels) ui.image(tx, cr);
                if (f) ui.fill(cr, 0x40FFFFFFu);
            }
            // ReputationDetailFactionName (GameFontNormal, 160 wide at 20,-21) and the description under it
            // (ReputationDetailFont: FRIZQT 10, white; 170 wide) down to the divider (y 131).
            const CharTabs::FacText* fx = t.faction(sel->factionId);
            float y = box.y + 21 * kDS;
            for (const std::string& l : wrapQuestText(fn, fx ? fx->name : "", 160 * kDS)) {
                ui.textShadow(fn, box.x + 20 * kDS, y, l.c_str(), kColGold);
                y += static_cast<float>(fn.lineHeight());
            }
            y += 2 * kDS;
            const float descBottom = box.y + 131 * kDS;
            const std::string desc = fx ? fx->desc : "";
            std::vector<std::string> lines = wrapQuestText(fs, desc, 170 * kDS);
            const Font* df = &fs;
            if (y + static_cast<float>(lines.size()) * lh > descBottom) {  // PSP: the 8 px font when it does not fit
                lines = wrapQuestText(ft, desc, 170 * kDS);
                df = &ft;
            }
            ui.pushClip(Rect{box.x, y, box.w, std::max(0.0f, descBottom - y)});
            for (const std::string& l : lines) {
                ui.textShadow(*df, box.x + 20 * kDS, y, l.c_str(), kColWhite);
                y += static_cast<float>(df->lineHeight());
            }
            ui.popClip();
            // The check boxes (UI-CheckBox-Up, -Highlight; checked: SwordCheck / Check, disabled Check-Disabled).
            auto checkBox = [&](uint32_t id, const Rect& r, bool checked, bool enabled, bool sword, const char* label,
                                uint32_t labelCol, const char* tipText) {
                const bool f = live && enabled && winFocus_.addStop(id, Rect{r.x, r.y, r.w + 2, r.h});
                if (t.cbUp.pixels) ui.image(t.cbUp, r);
                if (checked) {
                    if (!enabled && t.cbCheckDis.pixels) ui.image(t.cbCheckDis, r);
                    else if (sword && t.cbSword.pixels)  // CheckedTexture 32 x 32 at TOPLEFT 3,-5
                        ui.image(t.cbSword, Rect{r.x + 3 * kDS, r.y + 5 * kDS, 32 * kDS, 32 * kDS});
                    else if (t.cbCheck.pixels) ui.image(t.cbCheck, r);
                }
                if (f && t.cbHl.pixels) ui.image(t.cbHl, r, 0xFFFFFFFFu, Blend::Add);
                if (f) setTip(tipText, r);
                const float tx = r.x + r.w - 2 * kDS;  // $parentText LEFT at the box's RIGHT
                ui.textShadow(fs, tx, r.y + (r.h - lh) * 0.5f, label, labelCol);
                return tx + static_cast<float>(fs.measure(label));
            };
            const bool atWar = (sel->flags & kFlagAtWar) != 0;
            const bool canToggle = (sel->flags & kFlagPeaceForced) == 0 && !sel->isHeader;
            const Rect warBox = D(14, 203 - 34 - 26, 26, 26);  // BOTTOMLEFT 14,34
            const float warEnd = checkBox(kIdRepAtWar, warBox, atWar, canToggle, true, CLIENT_STR(AT_WAR),
                                          canToggle ? kRed : kGray, CLIENT_STR(REPUTATION_AT_WAR_DESCRIPTION));
            checkBox(kIdRepInactive, Rect{warEnd + 3 * kDS, warBox.y, warBox.w, warBox.h},
                     (sel->flags & kFlagInactive) != 0, !sel->isHeader, false, CLIENT_STR(MOVE_TO_INACTIVE),
                     sel->isHeader ? kGray : kColGold, CLIENT_STR(REPUTATION_MOVE_TO_INACTIVE));
            checkBox(kIdRepWatch, Rect{warBox.x, warBox.y + warBox.h - 3 * kDS, warBox.w, warBox.h},
                     session_.watchedFactionIndex() == static_cast<int32_t>(sel->repListId), true, false,
                     CLIENT_STR(SHOW_FACTION_ON_MAINSCREEN), kColGold, CLIENT_STR(REPUTATION_SHOW_AS_XP));
        }
    } else {
        // ================================ SkillFrame ================================
        t.loadSkills();
        if (t.skillVersion != session_.skillsVersion() || t.skillDirty) {  // SKILL_LINES_CHANGED / a header toggled
            t.skillVersion = session_.skillsVersion();
            t.skillDirty = false;
            t.buildSkillRows(session_);
        }
        const auto& skills = session_.playerSkills();
        auto findSkill = [&](uint32_t id) -> const pp::WorldSession::PlayerSkill* {
            for (const auto& s : skills)
                if (s.skillId == id) return &s;
            return nullptr;
        };
        const int total = static_cast<int>(t.skillRows.size());
        t.skillScroll = std::clamp(t.skillScroll, 0, std::max(0, total - kSkillRows));
        if (live) follow(t.skillScroll, kSkillRows, total, kIdSkillRow0, 0);
        if (t.skillSel && !findSkill(t.skillSel)) t.skillSel = 0;  // unlearnt

        // A skill's bar as SkillFrame_SetStatusBar / SkillDetailFrame_SetStatusBar colour it, its name and rank.
        // PSP: rank = the value, the modifier = temporary + permanent bonus (see the top).
        auto skillBar = [&](const Rect& bar, const pp::WorldSession::PlayerSkill& s, const char* name, bool white) {
            const bool proficiency = s.maxValue == 1;
            ui.fill(bar, proficiency ? rgba(1.0f, 1.0f, 1.0f, 0.1f) : rgba(0.0f, 0.0f, 0.75f, 0.1f));  // $parentBackground
            if (proficiency) statusBar(bar, 1.0f, rgba(0.5f, 0.5f, 0.5f, 1.0f));
            else if (s.maxValue > 0)
                statusBar(bar, static_cast<float>(s.value) / static_cast<float>(s.maxValue), rgba(0.0f, 0.0f, 1.0f, 0.5f));
            const float ny = bar.y + (bar.h - th) * 0.5f - 1 * kSY;  // $parentSkillName LEFT 6,1 (GameFontNormalSmall)
            const float nx = bar.x + 6 * kSX;
            ui.textShadow(ft, nx, ny, name, white ? kColWhite : kColGold);
            if (proficiency || s.maxValue == 0) return;
            float rx = nx + static_cast<float>(ft.measure(name)) + 13 * kSX;  // $parentSkillRank LEFT at the name +13
            const int mod = static_cast<int16_t>(s.bonusTemp) + static_cast<int16_t>(s.bonusPerm);
            char b[48];
            auto seg = [&](const char* text, uint32_t col) {
                ui.textShadow(ft, rx, ny, text, col);
                rx += static_cast<float>(ft.measure(text));
            };
            if (mod == 0) {
                std::snprintf(b, sizeof(b), "%u/%u", static_cast<unsigned>(s.value), static_cast<unsigned>(s.maxValue));
                seg(b, kColWhite);
            } else {  // skillRank.." ("..color..skillModifier.."|r)/"..skillMaxRank
                std::snprintf(b, sizeof(b), "%u (", static_cast<unsigned>(s.value));
                seg(b, kColWhite);
                std::snprintf(b, sizeof(b), mod > 0 ? "+%d" : "%d", mod);
                seg(b, mod > 0 ? kGreenCode : kRedCode);
                std::snprintf(b, sizeof(b), ")/%u", static_cast<unsigned>(s.maxValue));
                seg(b, kColWhite);
            }
        };
        auto skillName = [&](uint32_t id) {
            const CharTabs::SkillText* st = t.skillText(id);
            return st && st->name[0] ? st->name : "?";
        };

        // SkillFrameCollapseAllButton ALL (LEFT at the expand tab's left +5, 40 x 22): minus while every heading
        // is open.
        {
            bool allOpen = true;
            for (const auto& r : t.skillRows)
                if (r.isHeader && CharTabs::has(t.skillCollapsed, r.category)) allOpen = false;
            const Rect b{PX(75), PY(51), 40 * kSX + 16, 22 * kSY};
            const bool f = live && winFocus_.addStop(kIdSkillAll, b);
            const Rect ic{PX(78), b.y + (b.h - 10) * 0.5f, 10, 10};
            if (const pp::Texture& tx = allOpen ? t.minus : t.plus; tx.pixels) ui.image(tx, ic);
            if (f && t.plusHl.pixels) ui.image(t.plusHl, ic, 0xFFFFFFFFu, Blend::Add);
            ui.textShadow(fs, PX(100), b.y + (b.h - lh) * 0.5f, CLIENT_STR(ALL), kColWhite);  // GameFontHighlightLeft
        }

        // The rows: SkillTypeLabel1..12 (LEFT 22,-86, 18 apart) / SkillRankFrame1..12 (TOPLEFT 38,-79).
        for (int v = 0; v < kSkillRows; ++v) {
            t.skillVisible[v] = -1;
            const int i = t.skillScroll + v;
            if (i >= total) continue;
            t.skillVisible[v] = i;
            const auto& r = t.skillRows[static_cast<size_t>(i)];
            const uint32_t id = kIdSkillRow0 + static_cast<uint32_t>(v);
            if (r.isHeader) {  // SkillLabelTemplate: 285 x 14, +/- at LEFT 3, the text at LEFT 25 (white)
                const float cy = 86.0f + kSkillPitch * static_cast<float>(v);
                const Rect lr{PX(22), PY(cy - 7), 285 * kSX, 14 * kSY};
                const bool f = live && winFocus_.addStop(id, lr);
                const Rect ic{PX(25), PY(cy) - 5.0f, 10, 10};
                if (const pp::Texture& tx = CharTabs::has(t.skillCollapsed, r.category) ? t.plus : t.minus; tx.pixels)
                    ui.image(tx, ic);
                if (f && t.plusHl.pixels) ui.image(t.plusHl, ic, 0xFFFFFFFFu, Blend::Add);
                const CharTabs::CatText* c = t.category(r.category);
                boxText(fs, Rect{PX(47), PY(cy) - lh * 0.5f, (307 - 47) * kSX, lh}, c ? c->name : "", kColWhite, f);
                continue;
            }
            const pp::WorldSession::PlayerSkill* s = findSkill(r.skillId);
            if (!s) continue;
            const Rect bar{PX(38), PY(79 + kSkillPitch * static_cast<float>(v)), 271 * kSX, 15 * kSY};
            // $parentBorder: a button 281 x 32 at LEFT -5 (HitRectInsets 0,0,7,7) - the highlight on the focus,
            // locked on the selected skill.
            const Rect border{PX(33), bar.y + bar.h * 0.5f - 16 * kSY, 281 * kSX, 32 * kSY};
            const bool f = live && winFocus_.addStop(id, Rect{border.x, border.y + 7 * kSY, border.w, 18 * kSY});
            skillBar(bar, *s, skillName(r.skillId), false);
            if (t.skillBorder.pixels) ui.image(t.skillBorder, border);
            if ((f || t.skillSel == r.skillId) && t.skillBorderHl.pixels)
                ui.image(t.skillBorderHl, border, 0xFFFFFFFFu, Blend::Add);
        }
        if (t.skillTexts.empty())  // PSP: an old text.pak says so (in the details' place)
            ui.textShadow(fs, PX(38), PY(345), OWN_STR(CHARTAB_NO_SKILL_TEXT), kGray);
        // SkillListScrollFrame's bar (skill_scroll: x 314, y 70..297) and its arrows.
        if (total > kSkillRows) {
            if (t.skillScrollTex.pixels) ui.image(t.skillScrollTex, Rect{PX(314), PY(70), 30 * kSX, (297 - 70) * kSY});
            arrows(kIdSkillUp, kIdSkillDown, Rect{PX(320), PY(76), 11, 11}, Rect{PX(320), PY(293) - 11, 11, 11},
                   t.skillScroll > 0, t.skillScroll + kSkillRows < total);
        }

        // The details (SkillDetailScrollFrame x 21..317, y 303..410): SkillDetailStatusBar (211 x 15, CENTER at
        // 171,-323), its border, the unlearn button, the description (SKILL_DESCRIPTION, GameFontHighlightSmall).
        t.descMax = 0.0f;
        if (const pp::WorldSession::PlayerSkill* s = t.skillSel ? findSkill(t.skillSel) : nullptr) {
            const Rect bar{PX(65.5f), PY(315.5f), 211 * kSX, 15 * kSY};
            skillBar(bar, *s, skillName(s->skillId), false);
            if (t.skillBorder.pixels)
                ui.image(t.skillBorder, Rect{PX(60.5f), bar.y + bar.h * 0.5f - 16 * kSY, 220 * kSX, 32 * kSY});
            const CharTabs::SkillText* st = t.skillText(s->skillId);
            // isAbandonable: PSP - a primary profession (see the top).
            if (st && st->category == kCategoryProfession) {
                const Rect ub{PX(278.5f), PY(324 - 16), 32 * kSX, 32 * kSY};  // LEFT at the border's RIGHT -2,-1
                const bool f = live && winFocus_.addStop(kIdSkillUnlearn, ub);
                const pp::Texture& tx = f && winFocus_.isPressed(kIdSkillUnlearn) && t.unlearnDn.pixels ? t.unlearnDn
                                                                                                         : t.unlearn;
                if (tx.pixels) ui.image(tx, ub);
                if (f && t.unlearnHl.pixels) ui.image(t.unlearnHl, ub, 0xFFFFFFFFu, Blend::Add);
                if (f) setTip(CLIENT_STR(UNLEARN_SKILL_TOOLTIP), ub);
            }
            // PSP: the 8 px font; the description scrolls (its bar and arrows only when it overflows).
            const Rect view{PX(38.5f), PY(340), 275 * kSX, (410 - 340) * kSY};
            const std::vector<std::string> lines = wrapQuestText(ft, st ? st->desc : "", view.w);
            const float total_h = static_cast<float>(lines.size()) * th;
            t.descMax = std::max(0.0f, total_h - view.h);
            t.descScroll = std::clamp(t.descScroll, 0.0f, t.descMax);
            ui.pushClip(view);
            float y = view.y - t.descScroll;
            for (const std::string& l : lines) {
                if (y + th >= view.y && y <= view.y + view.h) ui.textShadow(ft, view.x, y, l.c_str(), kColWhite);
                y += th;
            }
            ui.popClip();
            if (t.descMax > 0.0f) {
                if (t.skillDScrollTex.pixels)
                    ui.image(t.skillDScrollTex, Rect{PX(314), PY(298), 30 * kSX, (412 - 298) * kSY});
                arrows(kIdSkillDescUp, kIdSkillDescDown, Rect{PX(320), PY(304), 11, 11},
                       Rect{PX(320), PY(408) - 11, 11, 11}, t.descScroll > 0.5f, t.descScroll < t.descMax - 0.5f);
            }
        } else {
            t.descScroll = 0.0f;
        }
        // SkillFrameCancelButton CLOSE (CENTER at TOPLEFT 305,-422, 80 x 22).
        drawPanelButton(ui, kIdSkillClose, Rect{PX(265), PY(411), 80 * kSX, 22 * kSY}, CLIENT_STR(CLOSE), true, live);
    }
    drawCharacterFrameTabs(ui, live);
    return true;
}

// StaticPopupDialogs["UNLEARN_SKILL"]: UNLEARN_SKILL, UNLEARN / CANCEL, OnAccept AbandonSkill(data), timeout 60,
// hideOnEscape (START). Its own window over the character window, as the quest log's ABANDON_QUEST.
bool WorldScreen::drawUnlearnSkillPopup(UiRenderer& ui, uint8_t w) {
    if (w != kWinUnlearnSkill) return false;
    const CharTabs::SkillText* st = charTabs_ ? charTabs_->skillText(charTabs_->unlearnSkill) : nullptr;
    if (!windowOpen(kWinCharacter) || charTab_ != kCharTabSkills || !st ||
        pp::logUptimeMs() - charTabs_->unlearnSinceMs > 60000u) {
        closeWindow(kWinUnlearnSkill);
        return true;
    }
    const std::string b = CLIENT_FMT(UNLEARN_SKILL, st->name);
    StaticPopup p;
    p.text = b.c_str();
    p.button1 = CLIENT_STR(UNLEARN);
    p.button1Id = kIdUnlearnYes;
    p.button2 = CLIENT_STR(CANCEL);
    p.button2Id = kIdUnlearnNo;
    p.focusable = uiMode_ && !settings_.isOpen() && !windowOpen(kWinBindConfirm);
    drawStaticPopup(ui, p);
    return true;
}

// The GameTooltip of a focused check box / the unlearn button (SetText(.., wrap)): right of it (ANCHOR_RIGHT),
// on the left when it does not fit; after all windows.
void WorldScreen::drawCharTabsOverlay(UiRenderer& ui) {
    if (!charTabs_ || !charTabs_->tip) return;
    const char* text = charTabs_->tip;
    charTabs_->tip = nullptr;
    if (!windowOpen(kWinCharacter) || charTab_ == kCharTabPaperDoll || !uiMode_) return;
    const Font& fs = kit_.font(kFontSmall);
    const float lh = static_cast<float>(fs.lineHeight());
    const std::vector<std::string> lines = wrapQuestText(fs, text, 180.0f);
    float w = 0.0f;
    for (const std::string& l : lines) w = std::max(w, static_cast<float>(fs.measure(l.c_str())));
    const Rect& a = charTabs_->tipAnchor;
    Rect r{a.x + a.w + 4, a.y, w + 12.0f, lh * static_cast<float>(lines.size()) + 8.0f};
    if (r.x + r.w > pp::kScreenW - 2) r.x = a.x - 4 - r.w;
    if (r.x < 2) r.x = 2;
    if (r.y + r.h > pp::kScreenH - 2) r.y = pp::kScreenH - 2 - r.h;
    if (r.y < 2) r.y = 2;
    if (kit_.has(kTexTipBg)) ui.tile(kit_.tex(kTexTipBg), Rect{r.x + 3, r.y + 3, r.w - 6, r.h - 6}, 0xF0FFFFFFu);
    else ui.fill(r, 0xF0100000u);
    if (kit_.has(kTexTipBorder)) ui.nineSlice(kit_.tex(kTexTipBorder), r);
    float y = r.y + 4;
    for (const std::string& l : lines) {
        ui.text(fs, r.x + 6, y, l.c_str(), kColWhite);
        y += lh;
    }
}

bool WorldScreen::charTabAction(uint32_t act) {
    if (act < 0x6D40 || act > 0x6DFF) return false;
    CharTabs& t = charTabs();
    auto checkSound = [](bool on) {
        if (!playSoundByName(on ? "igMainMenuOptionCheckBoxOn" : "igMainMenuOptionCheckBoxOff"))
            playUiSound(UiSound::ButtonClick);
    };
    const CharTabs::RepRow* sel = t.repDetail ? t.repFaction(t.repSel) : nullptr;
    switch (act) {
        case kIdTabClose: closeWindow(kWinCharacter); return true;
        case kIdRepUp: t.repScroll = std::max(0, t.repScroll - 1); return true;
        case kIdRepDown: ++t.repScroll; return true;  // clamped when drawn
        case kIdRepDetailClose: t.repDetail = false; return true;
        case kIdRepAtWar:  // FactionToggleAtWar(GetSelectedFaction())
            if (sel && sel->hasRep) {
                const bool on = (sel->flags & kFlagAtWar) == 0;
                session_.setFactionAtWar(sel->repListId, on);
                checkSound(on);
            }
            return true;
        case kIdRepInactive:  // SetFactionInactive / SetFactionActive
            if (sel && sel->hasRep) {
                const bool on = (sel->flags & kFlagInactive) == 0;
                session_.setFactionInactive(sel->repListId, on);
                checkSound(on);
            }
            return true;
        case kIdRepWatch:  // SetWatchedFactionIndex(GetSelectedFaction()) / (0)
            if (sel && sel->hasRep) {
                const bool on = session_.watchedFactionIndex() != static_cast<int32_t>(sel->repListId);
                session_.setWatchedFaction(on ? static_cast<int32_t>(sel->repListId) : -1);
                checkSound(on);
            }
            return true;
        case kIdSkillUp: t.skillScroll = std::max(0, t.skillScroll - 1); return true;
        case kIdSkillDown: ++t.skillScroll; return true;
        case kIdSkillDescUp: t.descScroll = std::max(0.0f, t.descScroll - 22.0f); return true;
        case kIdSkillDescDown: t.descScroll += 22.0f; return true;
        case kIdSkillAll: {  // SkillFrameCollapseAllButton: CollapseSkillHeader(0) / ExpandSkillHeader(0)
            bool allOpen = true;
            for (const auto& r : t.skillRows)
                if (r.isHeader && CharTabs::has(t.skillCollapsed, r.category)) allOpen = false;
            if (allOpen) {
                for (const auto& r : t.skillRows)
                    if (r.isHeader) CharTabs::toggle(t.skillCollapsed, r.category, true);
            } else {
                t.skillCollapsed.clear();
            }
            t.skillDirty = true;
            t.saveCollapsed(false);
            return true;
        }
        case kIdSkillClose: closeWindow(kWinCharacter); return true;  // CancelSkillUps; HideUIPanel(CharacterFrame)
        case kIdSkillUnlearn:  // StaticPopup_Show("UNLEARN_SKILL", name), data = the skill
            if (t.skillSel) {
                t.unlearnSkill = t.skillSel;
                t.unlearnSinceMs = pp::logUptimeMs();
                openWindow(kWinUnlearnSkill);
                winFocus_.setFocus(kIdUnlearnNo);
            }
            return true;
        case kIdUnlearnYes:  // OnAccept: AbandonSkill
            if (windowOpen(kWinUnlearnSkill) && t.unlearnSkill) session_.unlearnSkill(t.unlearnSkill);
            closeWindow(kWinUnlearnSkill);
            t.unlearnSkill = 0;
            return true;
        case kIdUnlearnNo:
            closeWindow(kWinUnlearnSkill);
            t.unlearnSkill = 0;
            winFocus_.setFocus(kIdSkillUnlearn);
            return true;
        default: break;
    }
    if (act >= kIdRepRow0 && act < kIdRepRow0 + kRepRows) {  // ReputationBar_OnClick
        const int i = t.repVisible[act - kIdRepRow0];
        if (i < 0 || i >= static_cast<int>(t.repRows.size())) return true;
        const auto& r = t.repRows[static_cast<size_t>(i)];
        if (t.repDetail && t.repSel == r.factionId) {
            t.repDetail = false;
        } else if (r.hasRep) {
            t.repSel = r.factionId;
            t.repDetail = true;
        }
        return true;
    }
    if (act >= kIdRepBtn0 && act < kIdRepBtn0 + kRepRows) {  // ExpandFactionHeader / CollapseFactionHeader
        const int i = t.repVisible[act - kIdRepBtn0];
        if (i < 0 || i >= static_cast<int>(t.repRows.size())) return true;
        const auto& r = t.repRows[static_cast<size_t>(i)];
        if (r.isHeader) {
            CharTabs::toggle(t.repCollapsed, r.factionId, !CharTabs::has(t.repCollapsed, r.factionId));
            t.repDirty = true;
            t.saveCollapsed(true);
        }
        return true;
    }
    if (act >= kIdSkillRow0 && act < kIdSkillRow0 + kSkillRows) {
        const int i = t.skillVisible[act - kIdSkillRow0];
        if (i < 0 || i >= static_cast<int>(t.skillRows.size())) return true;
        const auto& r = t.skillRows[static_cast<size_t>(i)];
        if (r.isHeader) {  // SkillLabelTemplate OnClick: ExpandSkillHeader / CollapseSkillHeader
            CharTabs::toggle(t.skillCollapsed, r.category, !CharTabs::has(t.skillCollapsed, r.category));
            t.skillDirty = true;
            t.saveCollapsed(false);
        } else if (t.skillSel == r.skillId) {  // PSP: ✕ again pages the description, back to its top at the end
            t.descScroll = t.descScroll >= t.descMax ? 0.0f : t.descScroll + 33.0f;
        } else {  // SkillBar_OnClick: SetSelectedSkill
            t.skillSel = r.skillId;
            t.descScroll = 0.0f;
            playUiSound(UiSound::ButtonClick);
        }
        return true;
    }
    return true;
}

}  // namespace wowee::ui::psp
