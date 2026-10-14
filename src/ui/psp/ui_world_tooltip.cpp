// GameTooltip - items, ShoppingTooltip1/2, spells.
//
// The line order of the item and the spell tooltips is kept as it is, pending observation; the colours and the
// other rules are the port's: FrameXML constants.lua's colours (gold 1/.82/0, white, red 1/.125/.125, grey .5),
// upstream's green 0/1/0 (lua_engine.cpp item builder) and quality colours (ui_colors.hpp getQualityColor); the
// INVTYPE_* and ITEM_MOD_* keys by AzerothCore's InventoryType / ItemModType enums; showItemLevel /
// alwaysCompareItems are CVars, default "0". Upstream's formatSpellDescription is the base of the description
// formatter below. Strings: GlobalStrings keys (strings.pak).
//
// Item (GameTooltip:SetBagItem / SetInventoryItem / SetMerchantItem), in this order:
//   [ShoppingTooltip: CURRENTLY_EQUIPPED grey]; the name in its quality colour (header font); ITEM_HEROIC green;
//   ITEM_CONJURED; the binding (ITEM_SOULBOUND / ITEM_BIND_*); uniqueness (ITEM_UNIQUE*, ITEM_LIMIT_CATEGORY);
//   ITEM_STARTS_QUEST; a bag: CONTAINER_SLOTS; the slot left (INVTYPE_*) / the subclass right (red when the class
//   cannot use it: upstream canUseItemType - weapons by SMSG_SET_PROFICIENCY, shields); DAMAGE_TEMPLATE / SPEED;
//   DPS_TEMPLATE; ARMOR_TEMPLATE; SHIELD_BLOCK_TEMPLATE; the stats Strength, Agility, Stamina, Intellect, Spirit
//   white (upstream's order), health and mana green; resistances; enchantments (green); sockets (icon +
//   EMPTY_SOCKET_* grey, a gem's enchantment) and ITEM_SOCKET_BONUS (green when every gem matches);
//   DURABILITY_TEMPLATE; ITEM_RACES_ALLOWED / ITEM_CLASSES_ALLOWED (red when not yours); ITEM_MIN_LEVEL (red when
//   higher); [ITEM_LEVEL - CVar showItemLevel]; the required skill / reputation (red when short); the ratings
//   ITEM_MOD_* green (upstream: no prefix); the item spells ITEM_SPELL_TRIGGER_ONEQUIP / _ONUSE / _ONPROC + the
//   description, green (+ ITEM_COOLDOWN_TOTAL_*, charges); the item set (ITEM_SET_NAME gold, the pieces light yellow /
//   grey, ITEM_SET_BONUS green / ITEM_SET_BONUS_GRAY grey); ITEM_COOLDOWN_TIME; the description in quotes, gold;
//   SELL_PRICE with coins when the item sells (upstream: always shown).
// Spell (GameTooltip:SetSpell / SetAction / SetTrainerService): the name white, the rank right grey; the cost
// (MANA_COST ...) and the range right (SPELL_RANGE, MELEE_RANGE); the cast time (SPELL_CAST_TIME_INSTANT,
// SPELL_CAST_TIME_SEC, SPELL_CAST_CHANNELED) and the cooldown right (SPELL_RECAST_TIME_SEC); SPELL_TOTEMS;
// SPELL_EQUIPPED_ITEM / SPELL_REQUIRED_FORM (red when not met); ITEM_MIN_LEVEL; SPELL_REAGENTS (a missing one red);
// ITEM_COOLDOWN_TIME; SPELL_USE_ALL_*; the description gold.
//
// PSP (all marked here): no mouse - "hover" is the interface cursor (SELECT), Shift = hold L (
// a proposed control row); the tooltip's place is the FrameXML anchor (ANCHOR_RIGHT / _LEFT / the default
// bottom right over the action bar) clamped to 480 x 272; a tooltip taller than the screen gets its body in
// the 8 px font; no random suffix names, no ITEM_CREATED_BY (ITEM_FIELD_CREATOR is not kept), no
// openable / readable / socketable hints (their wording is the mouse's), no lock / glyph / feral attack power
// lines, no spell modifiers (talents) in costs and cast times. wowee.cfg: showitemlevel=1 (CVar showItemLevel),
// comparealways=1 (CVar alwaysCompareItems), spelltooltip=0 (no spell tooltips: the earlier book).
#include "ui/psp/ui_world_screen.hpp"

#include "game/inventory.hpp"
#include "game/update_field_table.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"  // GlobalStrings keys
#include "ui/psp/ui_character_screen.hpp"  // raceName / className (strings.pak)

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
using UF = game::UF;

namespace {

// The tooltip colours, as 0xAABBGGRR (FrameXML constants.lua; the green is upstream's).
constexpr uint32_t kTipGold = 0xFF00D1FFu;        // 1, .82, 0
constexpr uint32_t kTipWhite = 0xFFFFFFFFu;       // 1, 1, 1
constexpr uint32_t kTipRed = 0xFF2020FFu;         // 1, .125, .125
constexpr uint32_t kTipGray = 0xFF808080u;        // .5, .5, .5
constexpr uint32_t kTipGreen = 0xFF00FF00u;       // 0, 1, 0
constexpr uint32_t kTipSetPiece = 0xFF97FFFFu;    // 1, 1, .59: an item set piece you wear
constexpr uint32_t kTipBackground = 0xBB1B0D0Du;  // UI-Tooltip-Background (142,139,142,187) x (.09, .09, .19)
// ITEM_QUALITY_COLORS: poor .. heirloom (upstream ui_colors.hpp getQualityColor).
constexpr uint32_t kQuality[8] = {0xFF9D9D9Du, 0xFFFFFFFFu, 0xFF00FF1Eu, 0xFFDD7000u,
                                  0xFFEE35A3u, 0xFF0080FFu, 0xFF80CCE6u, 0xFF80CCE6u};
const char* const kRedCode = "|cffff2020";  // RED_FONT_COLOR_CODE (a missing reagent)

// PSP layout (FrameXML units x ~0.6): text 6 px in from the border, 1 px between lines, 12 px between a
// double line's halves; the wrapped lines of a GameTooltip at 200 px (ShoppingTooltip 150).
constexpr float kPad = 6.0f, kLineGap = 1.0f, kSplitGap = 12.0f, kSocketW = 12.0f;
constexpr float kWrapW = 200.0f, kWrapShopW = 150.0f;

// INVTYPE_* by inventory type (AzerothCore ItemTemplate.h InventoryType; "" for 0 and past the table).
const char* invTypeName(uint32_t type) {
    switch (type) {
        case 1: return CLIENT_STR(INVTYPE_HEAD);
        case 2: return CLIENT_STR(INVTYPE_NECK);
        case 3: return CLIENT_STR(INVTYPE_SHOULDER);
        case 4: return CLIENT_STR(INVTYPE_BODY);
        case 5: return CLIENT_STR(INVTYPE_CHEST);
        case 6: return CLIENT_STR(INVTYPE_WAIST);
        case 7: return CLIENT_STR(INVTYPE_LEGS);
        case 8: return CLIENT_STR(INVTYPE_FEET);
        case 9: return CLIENT_STR(INVTYPE_WRIST);
        case 10: return CLIENT_STR(INVTYPE_HAND);
        case 11: return CLIENT_STR(INVTYPE_FINGER);
        case 12: return CLIENT_STR(INVTYPE_TRINKET);
        case 13: return CLIENT_STR(INVTYPE_WEAPON);
        case 14: return CLIENT_STR(INVTYPE_SHIELD);
        case 15: return CLIENT_STR(INVTYPE_RANGED);
        case 16: return CLIENT_STR(INVTYPE_CLOAK);
        case 17: return CLIENT_STR(INVTYPE_2HWEAPON);
        case 18: return CLIENT_STR(INVTYPE_BAG);
        case 19: return CLIENT_STR(INVTYPE_TABARD);
        case 20: return CLIENT_STR(INVTYPE_ROBE);
        case 21: return CLIENT_STR(INVTYPE_WEAPONMAINHAND);
        case 22: return CLIENT_STR(INVTYPE_WEAPONOFFHAND);
        case 23: return CLIENT_STR(INVTYPE_HOLDABLE);
        case 24: return CLIENT_STR(INVTYPE_AMMO);
        case 25: return CLIENT_STR(INVTYPE_THROWN);
        case 26: return CLIENT_STR(INVTYPE_RANGEDRIGHT);
        case 27: return CLIENT_STR(INVTYPE_QUIVER);
        case 28: return CLIENT_STR(INVTYPE_RELIC);
        default: return "";
    }
}

// ITEM_MOD_* by stat type (GlobalStrings). The white ones carry "%c%d".
const char* itemModFormat(uint32_t type) {
    switch (type) {
        case 0: return CLIENT_STR(ITEM_MOD_MANA);
        case 1: return CLIENT_STR(ITEM_MOD_HEALTH);
        case 3: return CLIENT_STR(ITEM_MOD_AGILITY);
        case 4: return CLIENT_STR(ITEM_MOD_STRENGTH);
        case 5: return CLIENT_STR(ITEM_MOD_INTELLECT);
        case 6: return CLIENT_STR(ITEM_MOD_SPIRIT);
        case 7: return CLIENT_STR(ITEM_MOD_STAMINA);
        case 12: return CLIENT_STR(ITEM_MOD_DEFENSE_SKILL_RATING);
        case 13: return CLIENT_STR(ITEM_MOD_DODGE_RATING);
        case 14: return CLIENT_STR(ITEM_MOD_PARRY_RATING);
        case 15: return CLIENT_STR(ITEM_MOD_BLOCK_RATING);
        case 16: return CLIENT_STR(ITEM_MOD_HIT_MELEE_RATING);
        case 17: return CLIENT_STR(ITEM_MOD_HIT_RANGED_RATING);
        case 18: return CLIENT_STR(ITEM_MOD_HIT_SPELL_RATING);
        case 19: return CLIENT_STR(ITEM_MOD_CRIT_MELEE_RATING);
        case 20: return CLIENT_STR(ITEM_MOD_CRIT_RANGED_RATING);
        case 21: return CLIENT_STR(ITEM_MOD_CRIT_SPELL_RATING);
        case 22: return CLIENT_STR(ITEM_MOD_HIT_TAKEN_MELEE_RATING);
        case 23: return CLIENT_STR(ITEM_MOD_HIT_TAKEN_RANGED_RATING);
        case 24: return CLIENT_STR(ITEM_MOD_HIT_TAKEN_SPELL_RATING);
        case 25: return CLIENT_STR(ITEM_MOD_CRIT_TAKEN_MELEE_RATING);
        case 26: return CLIENT_STR(ITEM_MOD_CRIT_TAKEN_RANGED_RATING);
        case 27: return CLIENT_STR(ITEM_MOD_CRIT_TAKEN_SPELL_RATING);
        case 28: return CLIENT_STR(ITEM_MOD_HASTE_MELEE_RATING);
        case 29: return CLIENT_STR(ITEM_MOD_HASTE_RANGED_RATING);
        case 30: return CLIENT_STR(ITEM_MOD_HASTE_SPELL_RATING);
        case 31: return CLIENT_STR(ITEM_MOD_HIT_RATING);
        case 32: return CLIENT_STR(ITEM_MOD_CRIT_RATING);
        case 33: return CLIENT_STR(ITEM_MOD_HIT_TAKEN_RATING);
        case 34: return CLIENT_STR(ITEM_MOD_CRIT_TAKEN_RATING);
        case 35: return CLIENT_STR(ITEM_MOD_RESILIENCE_RATING);
        case 36: return CLIENT_STR(ITEM_MOD_HASTE_RATING);
        case 37: return CLIENT_STR(ITEM_MOD_EXPERTISE_RATING);
        case 38: return CLIENT_STR(ITEM_MOD_ATTACK_POWER);
        case 39: return CLIENT_STR(ITEM_MOD_RANGED_ATTACK_POWER);
        case 41: return CLIENT_STR(ITEM_MOD_SPELL_HEALING_DONE);
        case 42: return CLIENT_STR(ITEM_MOD_SPELL_DAMAGE_DONE);
        case 43: return CLIENT_STR(ITEM_MOD_MANA_REGENERATION);
        case 44: return CLIENT_STR(ITEM_MOD_ARMOR_PENETRATION_RATING);
        case 45: return CLIENT_STR(ITEM_MOD_SPELL_POWER);
        case 46: return CLIENT_STR(ITEM_MOD_HEALTH_REGEN);
        case 47: return CLIENT_STR(ITEM_MOD_SPELL_PENETRATION);
        case 48: return CLIENT_STR(ITEM_MOD_BLOCK_VALUE);
        default: return nullptr;
    }
}
bool whiteStat(uint32_t type) { return type <= 7 && type != 2; }

// ITEM_RESIST_SINGLE's school: SPELL_SCHOOL1..6_CAP (holy .. arcane), declined by the form's "|3-7(%s)" (ruRU).
const char* resistSchool(int i) {
    switch (i) {
        case 0: return CLIENT_STR(SPELL_SCHOOL1_CAP);
        case 1: return CLIENT_STR(SPELL_SCHOOL2_CAP);
        case 2: return CLIENT_STR(SPELL_SCHOOL3_CAP);
        case 3: return CLIENT_STR(SPELL_SCHOOL4_CAP);
        case 4: return CLIENT_STR(SPELL_SCHOOL5_CAP);
        default: return CLIENT_STR(SPELL_SCHOOL6_CAP);
    }
}

std::string fmtStr(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string fmtStr(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

// A GlobalStrings form with "|4a:b:c;" (for n) and printf forms: the plural first, then the arguments.
template <typename... A>
std::string pluralFmt(const char* pattern, long n, const A&... a) {
    return pp::text::textf(pp::text::plural(pattern, n).c_str(), a...);
}

// A whole number as %d, else one decimal ("%.1f"): the description and expression values.
std::string fmtNum(double v) {
    const double r = std::round(v);
    if (std::fabs(v - r) < 0.05) return fmtStr("%ld", static_cast<long>(r));
    return fmtStr("%.1f", v);
}

// SpellDuration ms -> INT_SPELL_DURATION_SEC / _MIN / _HOURS / _DAYS, until cancelled ->
// SPELL_DURATION_UNTIL_CANCELLED. PSP: the largest unit that divides it (own rule); a
// fraction of a second goes into the SEC form's %d as text ("1.5").
std::string durationText(int32_t ms) {
    if (ms < 0) return CLIENT_STR(SPELL_DURATION_UNTIL_CANCELLED);
    const double s = ms / 1000.0;
    const long whole = static_cast<long>(s);
    if (std::fabs(s - static_cast<double>(whole)) > 0.001) return CLIENT_FMT(INT_SPELL_DURATION_SEC, fmtStr("%.1f", s));
    if (whole >= 86400 && whole % 86400 == 0) return pluralFmt(CLIENT_STR(INT_SPELL_DURATION_DAYS), whole / 86400, whole / 86400);
    if (whole >= 3600 && whole % 3600 == 0) return pluralFmt(CLIENT_STR(INT_SPELL_DURATION_HOURS), whole / 3600, whole / 3600);
    if (whole >= 60 && whole % 60 == 0) return CLIENT_FMT(INT_SPELL_DURATION_MIN, whole / 60);
    return CLIENT_FMT(INT_SPELL_DURATION_SEC, whole);
}

// "|cAARRGGBB" -> 0xAABBGGRR (alpha opaque).
uint32_t markupColor(const std::string& s, size_t at) {
    uint32_t rgb = 0;
    for (size_t k = at + 4; k < at + 10 && k < s.size(); ++k) {
        const char c = s[k];
        const uint32_t v = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                         : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 0;
        rgb = (rgb << 4) | v;
    }
    return 0xFF000000u | ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF);
}
bool isColorCode(const std::string& s, size_t i) { return s[i] == '|' && i + 9 < s.size() && s[i + 1] == 'c'; }

// A NUL-ended string inside a record blob from byte `at` (the end of the blob ends it too); `next` = after it.
std::string blobString(const std::vector<uint8_t>& b, size_t at, size_t* next = nullptr) {
    if (at >= b.size()) {
        if (next) *next = b.size();
        return std::string();
    }
    const auto begin = b.begin() + static_cast<std::ptrdiff_t>(at);
    const auto end = std::find(begin, b.end(), static_cast<uint8_t>(0));
    if (next) *next = static_cast<size_t>(end - b.begin()) + (end == b.end() ? 0u : 1u);
    return std::string(begin, end);
}
bool isResetCode(const std::string& s, size_t i) { return s[i] == '|' && i + 1 < s.size() && s[i + 1] == 'r'; }

// The width of a line with |c..|r markup.
float markupWidth(const Font& f, const std::string& s) {
    if (s.find('|') == std::string::npos) return static_cast<float>(f.measure(s.c_str()));
    std::string plain;
    for (size_t i = 0; i < s.size(); ++i) {
        if (isColorCode(s, i)) { i += 9; continue; }
        if (isResetCode(s, i)) { ++i; continue; }
        plain += s[i];
    }
    return static_cast<float>(f.measure(plain.c_str()));
}

// Draws a line with |c..|r runs (base colour outside them).
void drawMarkup(UiRenderer& ui, const Font& f, float x, float y, const std::string& s, uint32_t base) {
    uint32_t col = base;
    std::string seg;
    auto flush = [&]() {
        if (seg.empty()) return;
        ui.text(f, x, y, seg.c_str(), col);
        x += static_cast<float>(f.measure(seg.c_str()));
        seg.clear();
    };
    for (size_t i = 0; i < s.size(); ++i) {
        if (isColorCode(s, i)) {
            flush();
            col = markupColor(s, i);
            i += 9;
        } else if (isResetCode(s, i)) {
            flush();
            col = base;
            ++i;
        } else {
            seg += s[i];
        }
    }
    flush();
}

// Word wrap to maxW; a |c run open across the break is opened again on the next line. '\n' breaks.
std::vector<std::string> wrapMarkup(const Font& f, const std::string& text, float maxW) {
    std::vector<std::string> out;
    std::string color;  // the |c code open here ("" none)
    size_t p = 0;
    while (true) {
        const size_t nl = text.find('\n', p);
        std::string para = text.substr(p, nl == std::string::npos ? std::string::npos : nl - p);
        para.erase(std::remove(para.begin(), para.end(), '\r'), para.end());
        std::string line = color;
        bool empty = true;
        size_t q = 0;
        while (q <= para.size()) {
            size_t sp = para.find(' ', q);
            if (sp == std::string::npos) sp = para.size();
            const std::string word = para.substr(q, sp - q);
            const std::string trial = empty ? line + word : line + " " + word;
            if (!empty && markupWidth(f, trial) > maxW) {
                out.push_back(line);
                line = color + word;
            } else {
                line = trial;
            }
            empty = false;
            for (size_t i = 0; i < word.size(); ++i) {
                if (isColorCode(word, i)) color = word.substr(i, 10);
                else if (isResetCode(word, i)) color.clear();
            }
            q = sp + 1;
        }
        out.push_back(line);
        if (nl == std::string::npos) break;
        p = nl + 1;
    }
    return out;
}

float fieldFloat(uint32_t bits) {
    float v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

// ---------------------------------------------------------------------------------------------------------
// The spell description: upstream GameHandler::formatSpellDescription's walk ($s $o $m $M $d $z, $l / $g,
// $<var>, ${...}, $/N;) with the description syntax's other tokens (wowdev.wiki Spell.dbc) and the dice: $s = "min - max"
// (INT_SPELL_POINTS_SPREAD_TEMPLATE) when the effect rolls; $t $a $A $h $n $u $x $q $b $e $f $i $r $v,
// $?s / $?a conditions in the text, $*N;, $@spelldesc, Russian three-form plurals, and a precedence-correct
// expression evaluator with the player's stats ($AP $RAP $SP.. $MWS $mw $MW $PL $SPI $HND) and the functions
// $floor $ceil $max $min $cond $gt $gte $lt $lte $eq. PSP: upstream's evaluator (spell_description_eval.cpp)
// reads left to right and knows only $m/$M/$s/$o - "$m1+$AP*0.03" came out wrong or not at all.
// No level scaling of the base points (as upstream).
struct DescEnv {
    pp::SpellDb* db = nullptr;
    pp::WorldSession* session = nullptr;
    bool female = false;
    std::string homeZone;
    std::unordered_map<std::string, double> stats;  // lower-case names
};

class DescFmt {
public:
    DescFmt(DescEnv& env, uint32_t self) : env_(env), self_(self) {
        if (const pp::SpellTip* t = tip(self)) decls_ = t->var;
    }

    std::string text(const std::string& raw, int depth = 0) {
        std::string out;
        out.reserve(raw.size());
        const size_t n = raw.size();
        for (size_t i = 0; i < n;) {
            const char c = raw[i];
            if (c == '\r') { ++i; continue; }
            if (c != '$' || i + 1 >= n) { out += c; ++i; continue; }
            const char next = raw[i + 1];
            // $l / $L / $I plural, $g / $G gender: "$lone:few:many;" (PSP: "$|" too - a ruRU typo of "$l").
            const bool pluralI = next == 'I' && i + 2 < n && !std::isdigit(static_cast<unsigned char>(raw[i + 2])) &&
                                 raw.find(';', i) != std::string::npos && raw.find(':', i) < raw.find(';', i);
            if (next == 'l' || next == 'L' || next == 'g' || next == 'G' || next == '|' || pluralI) {
                const size_t semi = raw.find(';', i + 2);
                if (semi != std::string::npos) {
                    const std::string body = raw.substr(i + 2, semi - i - 2);
                    if (next == 'g' || next == 'G') {
                        const size_t colon = body.find(':');
                        out += colon == std::string::npos ? body
                                                          : (env_.female ? body.substr(colon + 1) : body.substr(0, colon));
                    } else {
                        out += pp::text::pluralForm(body, last_);
                    }
                    i = semi + 1;
                    continue;
                }
            }
            if (next == '<') {  // $<var> of SpellDescriptionVariables
                const size_t close = raw.find('>', i + 2);
                if (close != std::string::npos) {
                    double v = 0;
                    const bool ok = declaration(raw.substr(i + 2, close - i - 2), v, 0);
                    if (ok) emitRound(out, v);
                    i = close + 1;
                    if (!ok && i < n && raw[i] == '%') ++i;  // upstream: no bare '%' for a dropped value
                    continue;
                }
            }
            if (next == '{') {  // ${expression}
                size_t k = i + 2;
                double v = 0;
                if (expression(raw, k, v, 0) && k < n && raw[k] == '}') {
                    emitRound(out, v);
                    i = k + 1;
                    continue;
                }
                const size_t close = matching(raw, i + 1, '{', '}');
                i = close == std::string::npos ? n : close + 1;
                if (i < n && raw[i] == '%') ++i;
                continue;
            }
            if (next == '?') {  // $?s<id>[then]?a<id>[then][else]
                size_t k = i + 1;
                std::string body;
                if (condition(raw, k, body)) {
                    if (depth < 6) out += text(body, depth + 1);
                    i = k;
                    continue;
                }
                i += 2;
                continue;
            }
            if (next == '/' || next == '*') {  // $/10;s2  $*2;s1
                size_t k = i + 2;
                const char* start = raw.c_str() + k;
                char* endp = nullptr;
                const double factor = std::strtod(start, &endp);
                k += static_cast<size_t>(endp - start);
                if (k < n && raw[k] == ';') {
                    ++k;
                    if (k < n && raw[k] == '$') ++k;
                    double lo = 0, hi = 0;
                    if (token(raw, k, lo, hi) && factor != 0.0) {
                        emit(out, next == '/' ? lo / factor : lo * factor);
                        i = k;
                        continue;
                    }
                }
                const size_t semi = raw.find(';', i + 2);
                i = semi == std::string::npos ? i + 2 : semi + 1;
                while (i < n && std::isalnum(static_cast<unsigned char>(raw[i]))) ++i;
                continue;
            }
            if (next == '@') {  // $@spelldesc<id>
                static const char kDesc[] = "@spelldesc";
                if (raw.compare(i + 1, sizeof(kDesc) - 1, kDesc) == 0) {
                    size_t k = i + sizeof(kDesc);
                    uint32_t id = 0;
                    while (k < n && std::isdigit(static_cast<unsigned char>(raw[k]))) id = id * 10 + (raw[k++] - '0');
                    if (const pp::SpellTip* t = tip(id))
                        if (depth < 6) out += text(t->desc, depth + 1);
                    i = k;
                    continue;
                }
            }
            // $[spellId]code[index]
            size_t k = i + 1;
            uint32_t ref = 0;
            bool hasRef = false;
            while (k < n && std::isdigit(static_cast<unsigned char>(raw[k]))) {
                ref = ref * 10 + static_cast<uint32_t>(raw[k] - '0');
                hasRef = true;
                ++k;
            }
            if (k >= n || !std::isalpha(static_cast<unsigned char>(raw[k]))) {
                out += '$';
                ++i;
                continue;
            }
            const char code = raw[k++];
            int idx = 0;
            if (k < n && raw[k] >= '1' && raw[k] <= '3') idx = raw[k++] - '1';
            const uint32_t sid = hasRef ? ref : self_;
            bool done = false;
            if (code == 'z') {
                if (!env_.homeZone.empty()) {
                    out += env_.homeZone;
                    done = true;
                }
            } else if (code == 'd' || code == 'D') {
                if (const pp::SpellTip* t = tip(sid)) {
                    out += durationText(t->dur);
                    done = true;
                }
            } else {
                double lo = 0, hi = 0;
                if (value(sid, code, idx, lo, hi)) {
                    if (std::fabs(hi - lo) >= 0.5) {
                        out += fmtStr("%ld - %ld", std::lround(lo), std::lround(hi));  // INT_SPELL_POINTS_SPREAD_TEMPLATE
                        last_ = std::lround(hi);
                    } else {
                        emit(out, lo);
                    }
                    done = true;
                }
            }
            i = k;
            if (!done && i < n && raw[i] == '%') ++i;  // upstream: drop the '%' of an unresolved value
        }
        // upstream: collapse the spaces stripped tokens left
        std::string clean;
        clean.reserve(out.size());
        bool prevSpace = false;
        for (char ch : out) {
            const bool sp = ch == ' ';
            if (sp && prevSpace) continue;
            if (!clean.empty() && prevSpace && (ch == '.' || ch == ',' || ch == ';' || ch == '!' || ch == '?' || ch == ')'))
                clean.pop_back();
            clean += ch;
            prevSpace = sp;
        }
        return clean;
    }

private:
    DescEnv& env_;
    uint32_t self_;
    std::string decls_;
    long last_ = -1;
    int declNest_ = 0;
    std::unordered_map<uint32_t, pp::SpellTip> tips_;  // copies: the SpellDb cache may let a record go

    const pp::SpellTip* tip(uint32_t id) {
        auto it = tips_.find(id);
        if (it == tips_.end()) {
            pp::SpellTip copy;
            if (const pp::SpellTip* t = env_.db->tip(id)) copy = *t;
            it = tips_.emplace(id, std::move(copy)).first;
        }
        return it->second.id ? &it->second : nullptr;
    }

    void emit(std::string& out, double v) {
        out += fmtNum(v);
        last_ = std::lround(v);
    }
    // ${...} and $<name>: a whole number, as upstream (std::lround).
    void emitRound(std::string& out, double v) {
        last_ = std::lround(v);
        out += fmtStr("%ld", last_);
    }

    static size_t matching(const std::string& s, size_t open, char a, char b) {
        int depth = 0;
        for (size_t i = open; i < s.size(); ++i) {
            if (s[i] == a) ++depth;
            else if (s[i] == b && --depth == 0) return i;
        }
        return std::string::npos;
    }

    // A numeric token of spell sid (code, effect index): lo..hi, equal unless the effect rolls dice.
    bool value(uint32_t sid, char code, int idx, double& lo, double& hi) {
        const pp::SpellTip* t = tip(sid);
        if (!t || idx < 0 || idx > 2) return false;
        const pp::SpellTipEffect& e = t->eff[idx];
        // SpellEffectInfo::CalcValue: BasePoints + 1..DieSides (DieSides 0: BasePoints itself).
        auto points = [&](double& a, double& b) {
            a = static_cast<double>(e.basePoints) + (e.dieSides > 0 ? 1.0 : 0.0);
            b = static_cast<double>(e.basePoints) + static_cast<double>(e.dieSides > 0 ? e.dieSides : 0);
            a = std::fabs(a);
            b = std::fabs(b);
            if (a > b) std::swap(a, b);
        };
        switch (code) {
            case 's': case 'S': points(lo, hi); return true;
            case 'm': points(lo, hi); hi = lo; return true;
            case 'M': points(lo, hi); lo = hi; return true;
            case 'o': case 'O': {
                points(lo, hi);
                const double ticks = e.amplitude > 0 && t->dur > 0 ? std::floor(static_cast<double>(t->dur) / e.amplitude) : 1.0;
                lo *= ticks;
                hi *= ticks;
                return true;
            }
            case 't': case 'T': lo = hi = e.amplitude / 1000.0; return true;
            case 'a': lo = hi = e.radius; return true;
            case 'A': lo = hi = e.radiusMax > 0.0f ? e.radiusMax : e.radius; return true;
            case 'h': case 'H': lo = hi = t->procChance; return true;
            case 'n': case 'N': lo = hi = t->procCharges; return true;
            case 'u': case 'U': lo = hi = t->stack; return true;
            case 'x': case 'X': lo = hi = e.chainTarget; return true;
            case 'q': case 'Q': lo = hi = e.miscValue; return true;
            case 'b': case 'B': lo = hi = e.pointsPerCombo; return true;
            case 'e': case 'E': lo = hi = e.multipleValue; return true;
            case 'f': case 'F': lo = hi = e.damageMultiplier; return true;
            case 'i': case 'I': lo = hi = t->maxTargets; return true;
            case 'v': case 'V': lo = hi = t->maxTargetLevel; return true;
            case 'r': case 'R': {
                const pp::SpellInfo* si = env_.db->get(sid);
                lo = hi = t->maxRangeHostile > 0.0f ? t->maxRangeHostile : si ? si->rec.maxRange : 0.0f;
                return true;
            }
            case 'd': case 'D': lo = hi = t->dur > 0 ? t->dur / 1000.0 : 0.0; return true;  // in expressions
            default: return false;
        }
    }

    // [digits]code[index] at s[k] (the '$' already passed).
    bool token(const std::string& s, size_t& k, double& lo, double& hi) {
        uint32_t ref = 0;
        bool hasRef = false;
        while (k < s.size() && std::isdigit(static_cast<unsigned char>(s[k]))) {
            ref = ref * 10 + static_cast<uint32_t>(s[k++] - '0');
            hasRef = true;
        }
        if (k >= s.size() || !std::isalpha(static_cast<unsigned char>(s[k]))) return false;
        const char code = s[k++];
        int idx = 0;
        if (k < s.size() && s[k] >= '1' && s[k] <= '3') idx = s[k++] - '1';
        return value(hasRef ? ref : self_, code, idx, lo, hi);
    }

    // A row may chain its names ($arctic5 -> $arctic4 -> ...): up to 16 deep, the expression nesting to 64.
    bool declaration(const std::string& name, double& out, int depth) {
        if (depth > 64 || declNest_ >= 16 || name.empty()) return false;
        const std::string needle = "$" + name + "=";
        const size_t at = decls_.find(needle);
        if (at == std::string::npos) return false;
        const size_t from = at + needle.size();
        const size_t end = decls_.find_first_of("\r\n", from);
        const std::string rhs = decls_.substr(from, end == std::string::npos ? std::string::npos : end - from);
        size_t k = 0;
        ++declNest_;
        const bool ok = expression(rhs, k, out, depth + 1) && k >= rhs.size();
        --declNest_;
        return ok;
    }

    bool known(char kind, uint32_t id) {
        if (kind == 's' || kind == 'S') return env_.session->hasKnownSpell(id);
        if (kind == 'a' || kind == 'A') {
            for (const game::AuraSlot& a : env_.session->playerAuras())
                if (!a.isEmpty() && a.spellId == id) return true;
        }
        return false;
    }

    // "$?s1[a]?a2[b][c]" at s[k] = '$'..: picks the first true branch (or the last bracket).
    bool condition(const std::string& s, size_t& k, std::string& body) {
        bool chosen = false;
        std::string pick;
        while (k < s.size() && s[k] == '?') {
            ++k;
            bool neg = false;
            if (k < s.size() && s[k] == '!') { neg = true; ++k; }
            if (k >= s.size()) return false;
            const char kind = s[k++];
            uint32_t id = 0;
            while (k < s.size() && std::isdigit(static_cast<unsigned char>(s[k]))) id = id * 10 + (s[k++] - '0');
            if (k >= s.size() || s[k] != '[') return false;
            const size_t close = matching(s, k, '[', ']');
            if (close == std::string::npos) return false;
            if (!chosen && (known(kind, id) != neg)) {
                chosen = true;
                pick = s.substr(k + 1, close - k - 1);
            }
            k = close + 1;
        }
        if (k < s.size() && s[k] == '[') {  // the else
            const size_t close = matching(s, k, '[', ']');
            if (close == std::string::npos) return false;
            if (!chosen) pick = s.substr(k + 1, close - k - 1);
            k = close + 1;
        }
        body = pick;
        return true;
    }

    // expression := term (+|- term)*; term := factor (*|/ factor)*; factor := number | (expr) | -factor | $...
    bool expression(const std::string& s, size_t& k, double& out, int depth) {
        if (depth > 64 || !term(s, k, out, depth)) return false;
        while (true) {
            while (k < s.size() && s[k] == ' ') ++k;
            if (k >= s.size() || (s[k] != '+' && s[k] != '-')) return true;
            const char op = s[k++];
            double r = 0;
            if (!term(s, k, r, depth)) return false;
            out = op == '+' ? out + r : out - r;
        }
    }
    bool term(const std::string& s, size_t& k, double& out, int depth) {
        if (!factor(s, k, out, depth)) return false;
        while (true) {
            while (k < s.size() && s[k] == ' ') ++k;
            if (k >= s.size() || (s[k] != '*' && s[k] != '/')) return true;
            const char op = s[k++];
            double r = 0;
            if (!factor(s, k, r, depth)) return false;
            if (op == '/') {
                if (std::fabs(r) < 1e-9) return false;
                out /= r;
            } else {
                out *= r;
            }
        }
    }
    bool factor(const std::string& s, size_t& k, double& out, int depth) {
        while (k < s.size() && s[k] == ' ') ++k;
        if (k >= s.size()) return false;
        const char c = s[k];
        if (c == '(') {
            ++k;
            if (!expression(s, k, out, depth + 1) || k >= s.size() || s[k] != ')') return false;
            ++k;
            return true;
        }
        if (c == '-' || c == '+') {
            ++k;
            if (!factor(s, k, out, depth + 1)) return false;
            if (c == '-') out = -out;
            return true;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
            const char* start = s.c_str() + k;
            char* endp = nullptr;
            out = std::strtod(start, &endp);
            if (endp == start) return false;
            k += static_cast<size_t>(endp - start);
            return true;
        }
        if (c != '$' || k + 1 >= s.size()) return false;
        ++k;
        if (s[k] == '{') {
            ++k;
            if (!expression(s, k, out, depth + 1) || k >= s.size() || s[k] != '}') return false;
            ++k;
            return true;
        }
        if (s[k] == '<') {
            const size_t close = s.find('>', k);
            if (close == std::string::npos) return false;
            const std::string name = s.substr(k + 1, close - k - 1);
            k = close + 1;
            return declaration(name, out, depth + 1);
        }
        if (s[k] == '?') {
            std::string body;
            if (!condition(s, k, body)) return false;
            size_t b = 0;
            return expression(body, b, out, depth + 1) && b >= body.size();
        }
        // A name of two letters or more: a player stat or a function; else a one-letter token.
        size_t e = k;
        while (e < s.size() && std::isalpha(static_cast<unsigned char>(s[e]))) ++e;
        if (e - k >= 2) {
            const std::string orig = s.substr(k, e - k);
            std::string name = orig;
            for (char& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (orig == "MW" || orig == "MWB") name = "mw_hi";  // the high end of the weapon damage
            else if (orig == "RWB") name = "rwb_hi";
            if (e < s.size() && s[e] == '(') {  // $floor(x) $cond(c,a,b) ...
                k = e + 1;
                double a[3] = {};
                int na = 0;
                while (na < 3) {
                    if (!expression(s, k, a[na], depth + 1)) return false;
                    ++na;
                    while (k < s.size() && s[k] == ' ') ++k;
                    if (k < s.size() && s[k] == ',') { ++k; continue; }
                    break;
                }
                if (k >= s.size() || s[k] != ')') return false;
                ++k;
                if (name == "floor" && na == 1) out = std::floor(a[0]);
                else if (name == "ceil" && na == 1) out = std::ceil(a[0]);
                else if (name == "max" && na == 2) out = std::max(a[0], a[1]);
                else if (name == "min" && na == 2) out = std::min(a[0], a[1]);
                else if (name == "cond" && na == 3) out = a[0] != 0.0 ? a[1] : a[2];
                else if (name == "gt" && na == 2) out = a[0] > a[1];
                else if (name == "gte" && na == 2) out = a[0] >= a[1];
                else if (name == "lt" && na == 2) out = a[0] < a[1];
                else if (name == "lte" && na == 2) out = a[0] <= a[1];
                else if (name == "eq" && na == 2) out = a[0] == a[1];
                else return false;
                return true;
            }
            auto it = env_.stats.find(name);
            if (it == env_.stats.end()) return false;
            out = it->second;
            k = e;
            return true;
        }
        double lo = 0, hi = 0;
        if (!token(s, k, lo, hi)) return false;
        out = lo;
        return true;
    }
};

}  // namespace

// ---------------------------------------------------------------------------------------------------------

struct WorldScreen::GameTip {
    enum : uint8_t { kHeader, kBody, kSmall };  // GameTooltipHeaderText / GameTooltipText / ..Small
    struct Line {
        std::string left, right;
        uint32_t colL = kTipWhite, colR = kTipWhite;
        uint8_t font = kBody;
        bool wrap = false;
        int8_t socket = -1;  // the socket icon before the text: 0 meta, 1 red, 2 yellow, 3 blue, 4 none
        bool money = false;  // SetTooltipMoney: coins after the text
        uint32_t copper = 0;
        bool dressUp = false;  // PSP: "L + [Triangle]" before the text - the key of the dressing room
    };
    std::vector<Line> lines, shown;  // built / wrapped
    float w = 0.0f, h = 0.0f;
    uint64_t key = ~0ull;
    uint32_t builtMs = 0;
    bool retry = false;     // an item answer was asked for: build again shortly
    bool shopping = false;  // ShoppingTooltipTemplate fonts
    Line& add(const std::string& left, uint32_t col, uint8_t font, bool wrap = false) {
        lines.emplace_back();
        Line& l = lines.back();
        l.left = left;
        l.colL = col;
        l.font = font;
        l.wrap = wrap;
        return l;
    }
    Line& add2(const std::string& left, uint32_t colL, const std::string& right, uint32_t colR, uint8_t font) {
        Line& l = add(left, colL, font);
        l.right = right;
        l.colR = colR;
        return l;
    }
};

struct WorldScreen::TipState {
    pp::Texture border, socket[5];
    bool artTried = false;
    UiArtRetry artRetry;  // a failed load is tried again at the next window open
    uint32_t artBytes = 0;
    GameTip item, shop[2], spell;
    bool cfgRead = false, showItemLevel = false, alwaysCompare = false, spellTips = true;
    uint32_t zoneId = ~0u;
    std::string zoneName;
    uint32_t logs = 0;
    void releaseArt() {
        for (pp::Texture* t : {&border, &socket[0], &socket[1], &socket[2], &socket[3], &socket[4]})
            if (t->pixels) pp::textureRelease(*t);
        artTried = false;
        artBytes = 0;
    }
    ~TipState() { releaseArt(); }
};

void WorldScreen::destroyTipState(TipState* p) { delete p; }

WorldScreen::TipState& WorldScreen::tipState() {
    if (!tipState_) tipState_ = std::unique_ptr<TipState, void (*)(TipState*)>(new TipState(), &destroyTipState);
    TipState& ts = *tipState_;
    if (!ts.cfgRead) {
        ts.cfgRead = true;
        ts.showItemLevel = pp::configGetInt("showitemlevel", 0) != 0;  // CVar showItemLevel (default 0)
        ts.alwaysCompare = pp::configGetInt("comparealways", 0) != 0;  // CVar alwaysCompareItems (default 0)
        ts.spellTips = pp::configGetInt("spelltooltip", 1) != 0;
        PSP_LOGI(pp::tag::Gfx, "tooltip_cfg itemlevel=%d comparealways=%d spelltips=%d stip=%d",
                 ts.showItemLevel ? 1 : 0, ts.alwaysCompare ? 1 : 0, ts.spellTips ? 1 : 0, spellDb_.hasTips() ? 1 : 0);
    }
    if (ts.artTried && ts.artRetry.due()) ts.artTried = false;  // failed - again at the next window open
    if (!ts.artTried) {  // ui.pak "gt_border" + "gt_sock_*" (~3 KB) until the interface mode is left
        ts.artTried = true;
        UiArtPak pak("tooltip");  // ui_art_fail when the pak / a texture finds no memory
        int ok = 0;
        if (pak.isOpen()) {
            static const char* kNames[6] = {"gt_border", "gt_sock_meta", "gt_sock_red", "gt_sock_yellow",
                                            "gt_sock_blue", "gt_sock_none"};
            pp::Texture* tex[6] = {&ts.border, &ts.socket[0], &ts.socket[1], &ts.socket[2], &ts.socket[3], &ts.socket[4]};
            for (int i = 0; i < 6; ++i) {
                const bool had = tex[i]->pixels != nullptr;
                uint32_t bytes = 0;
                if (pak.load(kNames[i], *tex[i], true, &bytes)) {
                    if (!had) ts.artBytes += bytes;
                    ++ok;
                }
            }
        }
        if (pak.failed()) ts.artRetry.fail();
        pak.close();
        PSP_LOGI(pp::tag::Gfx, "tooltip_art ok=%d of 6 kb=%u retry=%d", ok,
                 static_cast<unsigned>((ts.artBytes + 1023u) / 1024u), pak.failed() ? 1 : 0);
    }
    return ts;
}

// ---------------------------------------------------------------------------------------------------------

std::string WorldScreen::spellDescription(uint32_t spellId) {
    const pp::SpellTip* t = spellDb_.tip(spellId);
    if (!t || t->desc.empty()) return std::string();
    const std::string raw = t->desc;  // the formatter's reads may let the cache record go
    DescEnv env;
    env.db = &spellDb_;
    env.session = &session_;
    if (const game::Character* me = session_.activeCharacter()) env.female = me->gender == game::Gender::FEMALE;
    // $z: the bind point's zone (SMSG_BINDPOINTUPDATE), its name from text.pak "zones" (read once per zone).
    TipState& ts = tipState();
    const uint32_t zone = session_.homeBindZone();
    if (zone != 0 && zone != ts.zoneId && raw.find("$z") != std::string::npos) {
        ts.zoneId = zone;
        ts.zoneName.clear();
        char path[256];
        pp::fsPath(path, sizeof(path), "data/text.pak");
        pp::Pak pak;
        if (pak.open(path)) {
            if (const pp::PakEntry* e = pak.find("zones")) {
                if (auto* b = static_cast<const char*>(pak.load(e))) {
                    const char *p = b + 4, *end = b + e->size;
                    while (p + 4 < end && ts.zoneName.empty()) {
                        int32_t key = 0;
                        std::memcpy(&key, p, 4);
                        p += 4;
                        const char* s = p;
                        while (p < end && *p) ++p;
                        if (key == static_cast<int32_t>(zone)) ts.zoneName.assign(s, p);
                        ++p;
                    }
                    pp::pakFree(const_cast<char*>(b));
                }
            }
            pak.close();
        }
    }
    env.homeZone = zone != 0 && zone == ts.zoneId ? ts.zoneName : std::string();
    // The player's stats the expressions name (as the character sheet reads them).
    auto F = [&](UF f, int add = 0) {
        const uint16_t i = game::fieldIndex(f);
        return i == 0xFFFF ? 0u : session_.selfField(static_cast<uint16_t>(i + add));
    };
    auto& st = env.stats;
    const double level = F(UF::UNIT_FIELD_LEVEL);
    st["pl"] = level;
    st["ap"] = static_cast<int32_t>(F(UF::UNIT_FIELD_ATTACK_POWER)) + static_cast<int32_t>(F(UF::UNIT_FIELD_ATTACK_POWER, 1));
    st["rap"] = static_cast<int32_t>(F(UF::UNIT_FIELD_RANGED_ATTACK_POWER)) +
                static_cast<int32_t>(F(UF::UNIT_FIELD_RANGED_ATTACK_POWER, 1));
    static const char* kSchool[7] = {"", "sph", "spfi", "spn", "spfr", "sps", "spa"};
    double sp = 0;
    for (int s = 1; s < 7; ++s) {
        const double v = static_cast<int32_t>(F(UF::PLAYER_FIELD_MOD_DAMAGE_DONE_POS, s));
        st[kSchool[s]] = v;
        sp = std::max(sp, v);
    }
    st["sp"] = sp;
    st["sph"] = std::max(st["sph"], static_cast<double>(static_cast<int32_t>(F(UF::PLAYER_FIELD_MOD_HEALING_DONE_POS))));
    const uint16_t br = game::fieldIndex(UF::UNIT_FIELD_BOUNDINGRADIUS);
    st["mws"] = br == 0xFFFF ? 0.0 : session_.selfField(static_cast<uint16_t>(br - 3)) / 1000.0;  // BASEATTACKTIME
    // The weapon damage: $mw / $mwb the low end, $MW / $MWB the high one (the main hand), $rwb / $RWB ranged.
    st["mw"] = fieldFloat(F(UF::UNIT_FIELD_MINDAMAGE));
    st["mwb"] = st["mw"];
    st["mw_hi"] = fieldFloat(F(UF::UNIT_FIELD_MAXDAMAGE));
    st["spi"] = static_cast<int32_t>(F(UF::UNIT_FIELD_STAT0, 4));
    const uint16_t rapIdx = game::fieldIndex(UF::UNIT_FIELD_RANGED_ATTACK_POWER);
    st["rwb"] = rapIdx == 0xFFFF ? 0.0 : fieldFloat(session_.selfField(static_cast<uint16_t>(rapIdx + 3)));
    st["rwb_hi"] = rapIdx == 0xFFFF ? 0.0 : fieldFloat(session_.selfField(static_cast<uint16_t>(rapIdx + 4)));
    const auto& mh = session_.getInventory().getEquipSlot(game::EquipSlot::MAIN_HAND);
    st["hnd"] = !mh.empty() && mh.item.inventoryType == 17 ? 2.0 : 1.0;  // INVTYPE_2HWEAPON
    DescFmt fmt(env, spellId);
    return fmt.text(raw);
}

// ---------------------------------------------------------------------------------------------------------
// GameTooltip:SetSpell / SetAction / SetTrainerService.
void WorldScreen::buildSpellTip(GameTip& t, uint32_t spellId) {
    t.lines.clear();
    t.retry = false;
    t.shopping = false;
    const pp::SpellInfo* siPtr = spellDb_.get(spellId);
    if (!siPtr) return;
    const pp::SpellInfo si = *siPtr;  // later reads may evict nothing here, but keep it local
    pp::SpellTip tip;
    const bool haveTip = [&]() {
        if (const pp::SpellTip* p = spellDb_.tip(spellId)) {
            tip = *p;
            return true;
        }
        return false;
    }();
    constexpr uint8_t kH = GameTip::kHeader, kB = GameTip::kBody;
    t.add2(si.name, kTipWhite, si.rank, kTipGray, kH);  // name, the rank right in grey

    // The cost (SpellInfo::CalcPowerCost as the action bar's usability) and the range.
    const uint32_t attr = si.rec.attributes, attrEx = si.rec.attributesEx;
    const uint8_t pt = si.rec.powerType;  // 255 = health
    const UseCtx& c = useCtx_;
    std::string cost;
    if (!(attrEx & 0x2u)) {  // SPELL_ATTR1_DRAIN_ALL_POWER: SPELL_USE_ALL_* below instead
        float v = static_cast<float>(si.rec.manaCost);
        if (si.manaPct) {
            const float pct = static_cast<float>(si.manaPct) / 100.0f;
            if (pt == 255) v += static_cast<float>(static_cast<int32_t>(static_cast<float>(c.baseHealth) * pct));
            else if (pt == 0) v += static_cast<float>(static_cast<int32_t>(static_cast<float>(c.baseMana) * pct));
            else if (pt >= 1 && pt <= 4) v += static_cast<float>(static_cast<int32_t>(static_cast<float>(c.maxPower[pt]) * pct));
        }
        int school = 0;
        for (int k = 0; k < 7; ++k)
            if (si.rec.schoolMask & (1u << k)) {
                school = k;
                break;
            }
        v += static_cast<float>(c.costMod[school]);
        const int32_t total = static_cast<int32_t>(v * (1.0f + c.costMul[school]));
        const int32_t perSec = haveTip ? static_cast<int32_t>(tip.manaPerSecond) : 0;
        const int tot = static_cast<int>(total), ps = static_cast<int>(perSec);
        if (pt == 5 && haveTip) {  // runes: RUNE_COST_BLOOD / _UNHOLY / _FROST
            const char* const kRune[3] = {CLIENT_STR(RUNE_COST_BLOOD), CLIENT_STR(RUNE_COST_UNHOLY),
                                          CLIENT_STR(RUNE_COST_FROST)};
            for (int r = 0; r < 3; ++r) {
                if (!tip.runes[r]) continue;
                if (!cost.empty()) cost += ' ';
                cost += pluralFmt(kRune[r], tip.runes[r], tip.runes[r]);
            }
        } else if (total > 0) {
            switch (pt) {
                // MANA_COST(_PER_TIME), RAGE_ / FOCUS_ / ENERGY_ / RUNIC_POWER_ / HEALTH_COST; rage and runic
                // power are kept x 10.
                case 0: cost = perSec ? CLIENT_FMT(MANA_COST_PER_TIME, tot, ps) : CLIENT_FMT(MANA_COST, tot); break;
                case 1: cost = perSec ? CLIENT_FMT(RAGE_COST_PER_TIME, tot / 10, ps / 10) : CLIENT_FMT(RAGE_COST, tot / 10); break;
                case 2: cost = perSec ? CLIENT_FMT(FOCUS_COST_PER_TIME, tot, ps) : CLIENT_FMT(FOCUS_COST, tot); break;
                case 3: cost = perSec ? CLIENT_FMT(ENERGY_COST_PER_TIME, tot, ps) : CLIENT_FMT(ENERGY_COST, tot); break;
                case 6: cost = pluralFmt(CLIENT_STR(RUNIC_POWER_COST), tot / 10, tot / 10); break;
                case 255: cost = perSec ? CLIENT_FMT(HEALTH_COST_PER_TIME, tot, ps) : CLIENT_FMT(HEALTH_COST, tot); break;
                default: break;
            }
        }
    }
    std::string range, range2;
    if (!(attr & 0x404u) && !(haveTip && (tip.attrEx3 & 0x40000000u))) {  // ON_NEXT_SWING, DONT_DISPLAY_RANGE
        // "%d-%d" with a minimum range, else "%d"; "" none; SPELL_RANGE_UNLIMITED from 50000 yards.
        auto rangeText = [](float mn, float mx) -> std::string {
            if (mx <= 0.0f) return std::string();
            if (mx >= 50000.0f) return CLIENT_STR(SPELL_RANGE_UNLIMITED);
            return mn > 0.0f ? fmtStr("%d-%d", static_cast<int>(mn), static_cast<int>(mx)) : fmtStr("%d", static_cast<int>(mx));
        };
        auto unlimited = [](const std::string& s) { return s == CLIENT_STR(SPELL_RANGE_UNLIMITED); };
        if (haveTip && (tip.rangeFlags & 1u)) {
            range = CLIENT_STR(MELEE_RANGE);
        } else {
            const float hMin = haveTip ? tip.minRangeHostile : si.rec.minRange;
            const float hMax = haveTip ? tip.maxRangeHostile : si.rec.maxRange;
            const std::string hostile = rangeText(hMin, hMax), friendly = rangeText(si.rec.minRange, si.rec.maxRange);
            if (!hostile.empty() && !friendly.empty() && hostile != friendly && !unlimited(hostile) &&
                !unlimited(friendly)) {
                range = CLIENT_FMT(SPELL_RANGE_DUAL, CLIENT_STR(ENEMY), hostile);
                range2 = CLIENT_FMT(SPELL_RANGE_DUAL, CLIENT_STR(FRIENDLY), friendly);
            } else if (!hostile.empty()) {
                range = unlimited(hostile) ? hostile : CLIENT_FMT(SPELL_RANGE, hostile);
            }
        }
    }
    if (cost.empty()) std::swap(cost, range);
    if (!cost.empty() || !range.empty()) t.add2(cost, kTipWhite, range, kTipWhite, kB);
    if (!range2.empty()) t.add2("", kTipWhite, range2, kTipWhite, kB);

    // The cast time and the cooldown (not for a passive or a trade skill).
    const uint8_t eff0 = haveTip ? tip.eff[0].effect : si.rec.effect[0];
    if (!(attr & 0x40u) && eff0 != 47 && eff0 != 78) {
        std::string cast;
        const uint32_t ms = si.rec.castMs;
        if (ms > 0) cast = ms >= 60000 ? CLIENT_FMT(SPELL_CAST_TIME_MIN, ms / 60000.0)
                                       : CLIENT_FMT(SPELL_CAST_TIME_SEC, ms / 1000.0);
        else if (attrEx & 0x44u) cast = CLIENT_STR(SPELL_CAST_CHANNELED);
        else if (attr & 0x404u) cast = CLIENT_STR(SPELL_ON_NEXT_SWING);
        else if (attr & 0x2u) cast = CLIENT_STR(SPELL_ON_NEXT_RANGED);
        else cast = CLIENT_STR(SPELL_CAST_TIME_INSTANT);
        std::string cd;
        const uint32_t cdMs = std::max(si.rec.recoveryMs, si.rec.categoryRecoveryMs);
        if (cdMs > 0 && !(haveTip && (tip.attrEx6 & 1u)))
            cd = cdMs >= 60000 ? CLIENT_FMT(SPELL_RECAST_TIME_MIN, cdMs / 60000.0)
                               : CLIENT_FMT(SPELL_RECAST_TIME_SEC, cdMs / 1000.0);
        t.add2(cast, kTipWhite, cd, kTipWhite, kB);
    }

    if (haveTip) {
        // SPELL_TOTEMS: the tool items and totem categories.
        std::string tools;
        for (int k = 0; k < 2; ++k) {
            if (tip.totem[k]) {
                if (const auto* qi = session_.getItemInfo(tip.totem[k])) {
                    tools += (tools.empty() ? "" : ", ") + qi->name;
                } else {
                    session_.ensureItemInfo(tip.totem[k]);
                    t.retry = true;
                }
            }
            if (tip.totemCat[k]) {
                const std::string n = spellDb_.tipName(pp::kTipTotem, tip.totemCat[k]);
                if (!n.empty()) tools += (tools.empty() ? "" : ", ") + n;
            }
        }
        if (!tools.empty()) t.add(CLIENT_STR(SPELL_TOTEMS) + tools, kTipWhite, kB, true);
        // SPELL_EQUIPPED_ITEM: ItemSubClassMask's name for the whole mask, else the subclasses' names.
        if (tip.eqClass == 2 || tip.eqClass == 4) {
            std::string names;
            std::vector<uint8_t> b;
            for (uint32_t row = 0; row < 8 && names.empty(); ++row)
                if (spellDb_.tipBlob(pp::kTipSubClassMask, static_cast<uint32_t>(tip.eqClass) << 24 | row, b) &&
                    b.size() > 4) {
                    uint32_t m = 0;
                    std::memcpy(&m, b.data(), 4);
                    if (m == tip.eqSubMask) names.assign(reinterpret_cast<const char*>(b.data() + 4), b.size() - 4);
                }
            if (names.empty()) {
                for (uint32_t sub = 0; sub < 32; ++sub) {
                    if (!(tip.eqSubMask & (1u << sub))) continue;
                    if (!spellDb_.tipBlob(pp::kTipSubClass, static_cast<uint32_t>(tip.eqClass) << 8 | sub, b) || b.size() < 4)
                        continue;
                    size_t after = 0;
                    const std::string disp = blobString(b, 4, &after), verbose = blobString(b, after);
                    names += (names.empty() ? "" : ", ") + (verbose.empty() ? disp : verbose);
                }
            }
            // Met: a worn weapon / shield of a subclass in the mask.
            bool met = false;
            static const game::EquipSlot kSlots[3] = {game::EquipSlot::MAIN_HAND, game::EquipSlot::OFF_HAND,
                                                      game::EquipSlot::RANGED};
            for (game::EquipSlot es : kSlots) {
                const auto& slot = session_.getInventory().getEquipSlot(es);
                if (slot.empty()) continue;
                if (const auto* qi = session_.getItemInfo(slot.item.itemId))
                    met |= static_cast<int32_t>(qi->itemClass) == tip.eqClass && (tip.eqSubMask & (1u << qi->subClass));
            }
            if (!names.empty()) t.add(CLIENT_FMT(SPELL_EQUIPPED_ITEM, names), met ? kTipWhite : kTipRed, kB, true);
        }
    }
    // SPELL_REQUIRED_FORM: the forms of Stances, white when we are in one.
    if (si.stances) {
        bool shapeshift = false;
        for (uint16_t a : si.rec.aura) shapeshift |= a == 36;  // SPELL_AURA_MOD_SHAPESHIFT: the form spell itself
        if (!shapeshift) {
            std::string names;
            for (uint32_t f = 1; f <= 32; ++f) {
                if (!(si.stances & (1u << (f - 1)))) continue;
                const std::string n = spellDb_.tipName(pp::kTipForm, f);
                if (!n.empty()) names += (names.empty() ? "" : ", ") + n;
            }
            const bool met = c.form > 0 && c.form <= 32 && (si.stances & (1u << (c.form - 1)));
            if (!names.empty()) t.add(CLIENT_FMT(SPELL_REQUIRED_FORM, names), met ? kTipWhite : kTipRed, kB, true);
        }
    }
    const uint32_t level = session_.selfField(game::fieldIndex(UF::UNIT_FIELD_LEVEL));
    if (haveTip && tip.spellLevel > level && level > 0)
        t.add(CLIENT_FMT(ITEM_MIN_LEVEL, static_cast<unsigned>(tip.spellLevel)), kTipRed, kB);
    // SPELL_REAGENTS (it ends in a line break): each "name" / "name (count)", red when the bags hold fewer.
    if (!si.reagents.empty()) {
        std::string list;
        for (uint32_t r : si.reagents) {
            const uint32_t item = r & 0x00FFFFFFu, need = r >> 24;
            const auto* qi = session_.getItemInfo(item);
            if (!qi) {
                session_.ensureItemInfo(item);
                t.retry = true;
                continue;
            }
            std::string part = need > 1 ? fmtStr("%s (%u)", qi->name.c_str(), static_cast<unsigned>(need)) : qi->name;
            if (reagentCount(item) < need) part = std::string(kRedCode) + part + "|r";
            list += (list.empty() ? "" : ", ") + part;
        }
        if (!list.empty()) t.add(CLIENT_STR(SPELL_REAGENTS) + list, kTipWhite, kB, true);
    }
    // ITEM_COOLDOWN_TIME: the cooldown still running.
    const float left = session_.spellCooldown(spellId);
    if (left > 0.0f) {
        const int s = static_cast<int>(std::ceil(left));
        t.add(s >= 60 ? CLIENT_FMT(ITEM_COOLDOWN_TIME_MIN, (s + 59) / 60) : CLIENT_FMT(ITEM_COOLDOWN_TIME_SEC, s),
              kTipWhite, kB);
    }
    if (attrEx & 0x2u) {  // SPELL_USE_ALL_*
        const char* all = pt == 0 ? CLIENT_STR(SPELL_USE_ALL_MANA) : pt == 1 ? CLIENT_STR(SPELL_USE_ALL_RAGE)
                        : pt == 2 ? CLIENT_STR(SPELL_USE_ALL_FOCUS) : pt == 3 ? CLIENT_STR(SPELL_USE_ALL_ENERGY)
                        : pt == 255 ? CLIENT_STR(SPELL_USE_ALL_HEALTH) : nullptr;
        if (all) t.add(all, kTipWhite, kB);
    }
    const std::string desc = spellDescription(spellId);
    if (!desc.empty()) t.add(desc, kTipGold, kB, true);
}

// ---------------------------------------------------------------------------------------------------------
// GameTooltip:SetBagItem / SetInventoryItem / SetMerchantItem / SetHyperlinkCompareItem.
void WorldScreen::buildItemTip(GameTip& t, const game::ItemDef& item, bool shopping, bool atMerchant) {
    t.lines.clear();
    t.retry = false;
    t.shopping = shopping;
    const uint8_t body = shopping ? GameTip::kSmall : GameTip::kBody;
    if (shopping) t.add(CLIENT_STR(CURRENTLY_EQUIPPED), kTipGray, GameTip::kSmall);
    const auto q = static_cast<size_t>(std::min<int>(static_cast<int>(item.quality), 7));
    t.add(item.name, kQuality[q], shopping ? GameTip::kBody : GameTip::kHeader);
    const auto* qi = session_.getItemInfo(item.itemId);
    if (!qi) {
        session_.ensureItemInfo(item.itemId);
        t.retry = true;
        t.add(CLIENT_STR(RETRIEVING_ITEM_INFO), kTipRed, body);
        return;
    }
    const auto* x = session_.itemTipExtra(item.itemId);
    const uint32_t flags = qi->itemFlags;
    const uint32_t level = session_.selfField(game::fieldIndex(UF::UNIT_FIELD_LEVEL));
    const game::Character* me = session_.activeCharacter();

    if (flags & 0x8u) t.add(CLIENT_STR(ITEM_HEROIC), kTipGreen, body);
    if (flags & 0x2u) t.add(CLIENT_STR(ITEM_CONJURED), kTipWhite, body);
    const bool account = (flags & 0x8000000u) != 0;                 // ITEM_FLAG_BIND_TO_ACCOUNT
    const char* bind = nullptr;
    if (item.soulbound) bind = qi->bindType == 4 ? CLIENT_STR(ITEM_BIND_QUEST)
                             : account ? CLIENT_STR(ITEM_ACCOUNTBOUND)
                                       : CLIENT_STR(ITEM_SOULBOUND);
    else if (account) bind = CLIENT_STR(ITEM_BIND_TO_ACCOUNT);
    else if (qi->bindType == 1) bind = CLIENT_STR(ITEM_BIND_ON_PICKUP);
    else if (qi->bindType == 2) bind = CLIENT_STR(ITEM_BIND_ON_EQUIP);
    else if (qi->bindType == 3) bind = CLIENT_STR(ITEM_BIND_ON_USE);
    else if (qi->bindType == 4) bind = CLIENT_STR(ITEM_BIND_QUEST);
    if (bind) t.add(bind, kTipWhite, body);
    if (flags & 0x80000u) t.add(CLIENT_STR(ITEM_UNIQUE_EQUIPPABLE), kTipWhite, body);
    else if (qi->maxCount == 1) t.add(CLIENT_STR(ITEM_UNIQUE), kTipWhite, body);
    else if (qi->maxCount > 1) t.add(CLIENT_FMT(ITEM_UNIQUE_MULTIPLE, static_cast<int>(qi->maxCount)), kTipWhite, body);
    if (x && x->limitCategory) {
        std::vector<uint8_t> b;
        if (spellDb_.tipBlob(pp::kTipLimit, x->limitCategory, b) && b.size() > 8) {
            uint32_t count = 0, lflags = 0;
            std::memcpy(&count, b.data(), 4);
            std::memcpy(&lflags, b.data() + 4, 4);
            const std::string name(reinterpret_cast<const char*>(b.data() + 8), b.size() - 8);
            t.add(pp::text::textf((lflags & 1u) ? CLIENT_STR(ITEM_LIMIT_CATEGORY_MULTIPLE) : CLIENT_STR(ITEM_LIMIT_CATEGORY),
                                  name, static_cast<unsigned>(count)),
                  kTipWhite, body);
        }
    }
    if (item.startQuestId) t.add(CLIENT_STR(ITEM_STARTS_QUEST), kTipWhite, body);

    // The slot / subclass line, or a bag's CONTAINER_SLOTS.
    auto subclass = [&](uint32_t cls, uint32_t sub, uint16_t& dflags) -> std::string {
        std::vector<uint8_t> b;
        dflags = 0;
        if (!spellDb_.tipBlob(pp::kTipSubClass, (cls & 0xFF) << 8 | (sub & 0xFF), b) || b.size() < 4) return {};
        std::memcpy(&dflags, b.data(), 2);
        return blobString(b, 4);
    };
    uint16_t dflags = 0;
    if ((qi->itemClass == 1 || qi->itemClass == 11) && qi->containerSlots > 0) {
        const std::string sub = subclass(qi->itemClass, qi->subClass, dflags);
        t.add(pluralFmt(CLIENT_STR(CONTAINER_SLOTS), static_cast<long>(qi->containerSlots),
                        static_cast<unsigned>(qi->containerSlots), sub.empty() ? CLIENT_STR(INVTYPE_BAG) : sub.c_str()),
              kTipWhite, body);
    } else if (item.inventoryType > 0 && item.inventoryType < 29) {
        const std::string right = subclass(qi->itemClass, qi->subClass, dflags);
        // Upstream canUseItemType: a weapon the weapon proficiency lacks, a shield the armor proficiency lacks.
        const uint32_t prof = qi->itemClass == 2 ? session_.weaponProficiency()
                            : (qi->itemClass == 4 && qi->subClass == 6) ? session_.armorProficiency() : 0u;
        const bool cannot = prof != 0 && qi->subClass < 32 && !(prof & (1u << qi->subClass));
        const std::string left = invTypeName(item.inventoryType);
        if (!left.empty()) t.add2(left, kTipWhite, right, cannot ? kTipRed : kTipWhite, body);
        else if (!right.empty()) t.add(right, cannot ? kTipRed : kTipWhite, body);
    }
    // DAMAGE_TEMPLATE + SPEED, DPS_TEMPLATE.
    if (item.damageMax > 0.0f && item.delayMs > 0) {
        const double speed = item.delayMs / 1000.0;
        t.add2(CLIENT_FMT(DAMAGE_TEMPLATE, std::lround(item.damageMin), std::lround(item.damageMax)), kTipWhite,
               CLIENT_STR(SPEED) + fmtStr(" %.2f", speed), kTipWhite, body);
        t.add(CLIENT_FMT(DPS_TEMPLATE, (item.damageMin + item.damageMax) * 0.5 / speed), kTipWhite, body);
    }
    if (item.armor > 0)
        t.add(CLIENT_FMT(ARMOR_TEMPLATE, static_cast<int>(item.armor)), kTipWhite, body);  // upstream: white
    if (x && x->block) t.add(CLIENT_FMT(SHIELD_BLOCK_TEMPLATE, static_cast<unsigned>(x->block)), kTipWhite, body);
    // The stats: the five primaries white, health and mana green (upstream: the extra stats are green).
    auto statValue = [&](uint32_t type) -> int32_t {
        switch (type) {
            case 3: return item.agility;
            case 4: return item.strength;
            case 5: return item.intellect;
            case 6: return item.spirit;
            case 7: return item.stamina;
            default:
                for (const auto& es : item.extraStats)
                    if (es.statType == type) return es.statValue;
                return 0;
        }
    };
    for (uint32_t type : {4u, 3u, 7u, 5u, 6u, 1u, 0u}) {
        const int32_t v = statValue(type);
        if (v == 0) continue;
        t.add(pp::text::textf(itemModFormat(type), v < 0 ? '-' : '+', std::abs(v)), type <= 1 ? kTipGreen : kTipWhite, body);
    }
    // ITEM_RESIST_ALL / ITEM_RESIST_SINGLE.
    const int32_t res[6] = {qi->holyRes, qi->fireRes, qi->natureRes, qi->frostRes, qi->shadowRes, qi->arcaneRes};
    if (res[0] > 0 && std::all_of(res, res + 6, [&](int32_t r) { return r == res[0]; })) {
        t.add(CLIENT_FMT(ITEM_RESIST_ALL, '+', static_cast<int>(res[0])), kTipWhite, body);
    } else {
        for (int i = 0; i < 6; ++i)
            if (res[i] != 0)
                t.add(questText(CLIENT_FMT(ITEM_RESIST_SINGLE, res[i] < 0 ? '-' : '+',
                                           std::abs(static_cast<int>(res[i])), resistSchool(i))),
                      kTipWhite, body);
    }
    // The instance's enchantments (green), sockets and their bonus.
    uint32_t perm = 0, temp = 0;
    std::array<uint32_t, 3> gems{};
    session_.itemEnchantments(item.guid, perm, temp, gems);
    for (uint32_t en : {perm, temp}) {
        if (!en) continue;
        const std::string n = spellDb_.tipName(pp::kTipEnchant, en);
        if (!n.empty()) t.add(n, kTipGreen, body, true);
    }
    bool anySocket = false, allMatch = true;
    for (int i = 0; i < 3; ++i) {
        const uint32_t color = qi->socketColor[i];
        if (!color) continue;
        anySocket = true;
        const int8_t icon = (color & 1u) ? 0 : (color & 2u) && !(color & 12u) ? 1 : (color & 4u) && !(color & 10u) ? 2
                          : (color & 8u) && !(color & 6u) ? 3 : 4;
        if (gems[static_cast<size_t>(i)]) {
            const std::string n = spellDb_.tipName(pp::kTipEnchant, gems[static_cast<size_t>(i)]);
            std::vector<uint8_t> g;
            uint32_t gemColor = 0;
            if (spellDb_.tipBlob(pp::kTipGem, gems[static_cast<size_t>(i)], g) && g.size() >= 8) std::memcpy(&gemColor, g.data() + 4, 4);
            allMatch &= (gemColor & color) != 0;
            t.add(n.empty() ? std::string("?") : n, kTipWhite, body).socket = icon;
        } else {
            allMatch = false;
            const char* const kEmpty[5] = {CLIENT_STR(EMPTY_SOCKET_META), CLIENT_STR(EMPTY_SOCKET_RED),
                                           CLIENT_STR(EMPTY_SOCKET_YELLOW), CLIENT_STR(EMPTY_SOCKET_BLUE),
                                           CLIENT_STR(EMPTY_SOCKET_NO_COLOR)};
            t.add(kEmpty[static_cast<size_t>(icon)], kTipGray, body).socket = icon;
        }
    }
    if (anySocket && qi->socketBonus) {
        const std::string n = spellDb_.tipName(pp::kTipEnchant, qi->socketBonus);
        if (!n.empty()) t.add(CLIENT_FMT(ITEM_SOCKET_BONUS, n), allMatch ? kTipGreen : kTipGray, body, true);
    }
    // DURABILITY_TEMPLATE (a template: its MaxDurability, full).
    const uint32_t maxDur = item.maxDurability ? item.maxDurability : x ? x->maxDurability : 0u;
    const uint32_t curDur = item.maxDurability ? item.curDurability : maxDur;
    if (maxDur) t.add(CLIENT_FMT(DURABILITY_TEMPLATE, static_cast<unsigned>(curDur), static_cast<unsigned>(maxDur)),
                      curDur == 0 ? kTipRed : kTipWhite, body);
    // ITEM_RACES_ALLOWED / ITEM_CLASSES_ALLOWED (when they restrict), red when we are not one of them.
    constexpr uint32_t kPlayableRaces = 0x6FFu, kPlayableClasses = 0x5FFu;
    if (qi->allowableRace && (qi->allowableRace & kPlayableRaces) != kPlayableRaces && (qi->allowableRace & kPlayableRaces)) {
        std::string names;
        for (int r = 1; r <= 11; ++r)
            if (qi->allowableRace & (1u << (r - 1)) & kPlayableRaces)
                names += (names.empty() ? "" : ", ") + std::string(raceName(static_cast<game::Race>(r)));
        const bool mine = me && (qi->allowableRace & (1u << (static_cast<int>(me->race) - 1)));
        t.add(CLIENT_FMT(ITEM_RACES_ALLOWED, names), mine ? kTipWhite : kTipRed, body, true);
    }
    if (qi->allowableClass && (qi->allowableClass & kPlayableClasses) != kPlayableClasses &&
        (qi->allowableClass & kPlayableClasses)) {
        std::string names;
        for (int cl = 1; cl <= 11; ++cl)
            if (qi->allowableClass & (1u << (cl - 1)) & kPlayableClasses)
                names += (names.empty() ? "" : ", ") + std::string(className(static_cast<game::Class>(cl), false));
        const bool mine = me && (qi->allowableClass & (1u << (static_cast<int>(me->characterClass) - 1)));
        t.add(CLIENT_FMT(ITEM_CLASSES_ALLOWED, names), mine ? kTipWhite : kTipRed, body, true);
    }
    if (item.requiredLevel > 1)
        t.add(CLIENT_FMT(ITEM_MIN_LEVEL, static_cast<unsigned>(item.requiredLevel)),
              level >= item.requiredLevel ? kTipWhite : kTipRed, body);
    if (tipState().showItemLevel && item.itemLevel > 0 &&
        (qi->itemClass == 2 || qi->itemClass == 4 || qi->itemClass == 5 || qi->itemClass == 6))
        t.add(CLIENT_FMT(ITEM_LEVEL, static_cast<unsigned>(item.itemLevel)), kTipWhite, body);
    // ITEM_MIN_SKILL: the skill line's name (spells.pak "skills"), red when ours is lower.
    if (qi->requiredSkill && qi->requiredSkillRank) {
        const pp::SpellDb::SkillLineInfo* sl = spellDb_.skillLine(qi->requiredSkill);
        uint32_t have = 0;
        for (const auto& s : session_.playerSkills())
            if (s.skillId == qi->requiredSkill) have = s.value;
        t.add(CLIENT_FMT(ITEM_MIN_SKILL, sl ? sl->name.c_str() : "?", static_cast<unsigned>(qi->requiredSkillRank)),
              have >= qi->requiredSkillRank ? kTipWhite : kTipRed, body);
    }
    // ITEM_REQ_REPUTATION (faction, standing; ruRU declines the faction "|3-7(%1$s)"): text.pak faction_text (read
    // for this line).
    if (qi->requiredReputationFaction && qi->requiredReputationRank) {
        std::string faction;
        uint32_t repList = 0xFFFFFFFFu;
        char path[256];
        pp::fsPath(path, sizeof(path), "data/text.pak");
        pp::Pak pak;
        if (pak.open(path)) {
            if (const pp::PakEntry* e = pak.find("faction_text")) {
                if (auto* b = static_cast<const char*>(pak.load(e))) {
                    const char *p = b + 4, *end = b + e->size;
                    while (p + 12 <= end && faction.empty()) {
                        uint32_t id = 0, rl = 0;
                        std::memcpy(&id, p, 4);
                        std::memcpy(&rl, p + 4, 4);
                        p += 12;
                        const char* name = p;
                        while (p < end && *p) ++p;
                        const char* nameEnd = p;
                        if (p < end) ++p;
                        while (p < end && *p) ++p;  // the description
                        if (p < end) ++p;
                        if (id == qi->requiredReputationFaction) {
                            faction.assign(name, nameEnd);
                            repList = rl;
                        }
                    }
                    pp::pakFree(const_cast<char*>(b));
                }
            }
            pak.close();
        }
        const char* const kRank[8] = {CLIENT_STR(FACTION_STANDING_LABEL1), CLIENT_STR(FACTION_STANDING_LABEL2),
                                      CLIENT_STR(FACTION_STANDING_LABEL3), CLIENT_STR(FACTION_STANDING_LABEL4),
                                      CLIENT_STR(FACTION_STANDING_LABEL5), CLIENT_STR(FACTION_STANDING_LABEL6),
                                      CLIENT_STR(FACTION_STANDING_LABEL7), CLIENT_STR(FACTION_STANDING_LABEL8)};
        static const int32_t kFloor[8] = {-42000, -6000, -3000, 0, 3000, 9000, 21000, 42000};
        const uint32_t rank = std::min<uint32_t>(qi->requiredReputationRank, 7);
        uint8_t rflags = 0;
        int32_t standing = -42000;
        const bool known = repList != 0xFFFFFFFFu && session_.factionStandingAt(repList, rflags, standing);
        const std::string line = questText(CLIENT_FMT(ITEM_REQ_REPUTATION, faction.empty() ? "?" : faction.c_str(),
                                                      kRank[rank]));
        t.add(line, known && standing >= kFloor[rank] ? kTipWhite : kTipRed, body, true);
    }
    // The ratings: ITEM_MOD_* in the item's own order, green, without a prefix (upstream).
    for (const auto& es : item.extraStats) {
        if (whiteStat(es.statType) || es.statValue == 0) continue;
        const char* f = itemModFormat(es.statType);
        if (!f) continue;
        t.add(pp::text::textf(f, es.statValue), kTipGreen, body, true);
    }
    // The item spells: ITEM_SPELL_TRIGGER_ONEQUIP / ONPROC / ONUSE + the description (green).
    for (int k = 0; k < 5; ++k) {
        const auto& sp = qi->spells[static_cast<size_t>(k)];
        if (sp.spellId == 0) continue;
        const char* trigger = sp.spellTrigger == 1 ? CLIENT_STR(ITEM_SPELL_TRIGGER_ONEQUIP)
                            : sp.spellTrigger == 2 ? CLIENT_STR(ITEM_SPELL_TRIGGER_ONPROC)
                            : (sp.spellTrigger == 0 || sp.spellTrigger == 5 || sp.spellTrigger == 6)
                                ? CLIENT_STR(ITEM_SPELL_TRIGGER_ONUSE)
                                : nullptr;
        if (!trigger) continue;
        std::string text = spellDescription(sp.spellId);
        if (text.empty()) text = spellDb_.name(sp.spellId);  // upstream: the name without a description
        if (text.empty()) continue;
        std::string line = std::string(trigger) + " " + text;
        const bool use = sp.spellTrigger != 1 && sp.spellTrigger != 2;
        const int32_t cdMs = x ? std::max(x->spellCooldown[k], x->spellCategoryCooldown[k]) : 0;
        if (use && cdMs > 0) {  // ITEM_COOLDOWN_TOTAL_*
            const int s = cdMs / 1000;
            line += " ";
            line += s >= 86400 ? pluralFmt(CLIENT_STR(ITEM_COOLDOWN_TOTAL_DAYS), s / 86400, s / 86400)
                  : s >= 3600 ? pluralFmt(CLIENT_STR(ITEM_COOLDOWN_TOTAL_HOURS), s / 3600, s / 3600)
                  : s >= 60 ? CLIENT_FMT(ITEM_COOLDOWN_TOTAL_MIN, s / 60)
                            : CLIENT_FMT(ITEM_COOLDOWN_TOTAL_SEC, s);
        }
        t.add(line, kTipGreen, body, true);
        const int charges = x ? std::abs(static_cast<int>(x->spellCharges[k])) : 0;
        if (charges > 1)
            t.add(pluralFmt(CLIENT_STR(ITEM_SPELL_CHARGES), charges, charges), kTipWhite, body);
    }
    // The item set: ITEM_SET_NAME gold, the pieces (worn: light yellow), the bonuses ITEM_SET_BONUS(_GRAY).
    if (qi->itemSetId) {
        std::vector<uint8_t> b;
        if (spellDb_.tipBlob(pp::kTipItemSet, qi->itemSetId, b) && b.size() >= 35 * 4) {
            uint32_t v[35];
            std::memcpy(v, b.data(), sizeof(v));
            const std::string setName(reinterpret_cast<const char*>(b.data() + 140), b.size() - 140);
            const auto& inv = session_.getInventory();
            auto worn = [&](uint32_t id) {
                for (int s = 0; s < game::Inventory::NUM_EQUIP_SLOTS; ++s) {
                    const auto& es = inv.getEquipSlot(static_cast<game::EquipSlot>(s));
                    if (!es.empty() && es.item.itemId == id) return true;
                }
                return false;
            };
            int total = 0, equipped = 0;
            for (int i = 0; i < 17; ++i)
                if (v[i]) {
                    ++total;
                    equipped += worn(v[i]) ? 1 : 0;
                }
            t.add(CLIENT_FMT(ITEM_SET_NAME, setName, equipped, total), kTipGold, body);
            for (int i = 0; i < 17; ++i) {
                if (!v[i]) continue;
                const auto* pi = session_.getItemInfo(v[i]);
                if (!pi) {
                    session_.ensureItemInfo(v[i]);
                    t.retry = true;
                    continue;
                }
                t.add("  " + pi->name, worn(v[i]) ? kTipSetPiece : kTipGray, body);
            }
            for (int i = 0; i < 8; ++i) {
                const uint32_t spell = v[17 + i], need = v[25 + i];
                if (!spell || !need) continue;
                std::string d = spellDescription(spell);
                if (d.empty()) d = spellDb_.name(spell);
                if (static_cast<uint32_t>(equipped) >= need)
                    t.add(CLIENT_FMT(ITEM_SET_BONUS, d), kTipGreen, body, true);
                else
                    t.add(pluralFmt(CLIENT_STR(ITEM_SET_BONUS_GRAY), static_cast<long>(need), static_cast<unsigned>(need), d),
                          kTipGray, body, true);
            }
        }
    }
    // ITEM_COOLDOWN_TIME / COOLDOWN_ON_LEAVE_COMBAT after the set, before the description.
    if (const std::string cd = itemCooldownText(item.itemId); !cd.empty()) t.add(cd, kTipWhite, body);
    if (!item.description.empty()) t.add("\"" + item.description + "\"", kTipGold, body, true);
    // SELL_PRICE whenever the item sells (upstream shows it everywhere): the whole stack's price.
    (void)atMerchant;
    if (qi->sellPrice > 0) {
        GameTip::Line& l = t.add(std::string(CLIENT_STR(SELL_PRICE)) + ":", kTipWhite, body);
        l.money = true;
        l.copper = qi->sellPrice * std::max<uint32_t>(1u, item.stackCount);
    }
    // PSP: the dressing room (DRESSUP_FRAME_INSTRUCTIONS) is L + Triangle, said here, after the other lines, on
    // what the dressing room can wear.
    if (!shopping && dressableItem(item.itemId)) t.add(CLIENT_STR(DRESSUP_FRAME), kTipGray, body).dressUp = true;
}

// ---------------------------------------------------------------------------------------------------------

void WorldScreen::layoutTip(GameTip& t) {
    auto font = [&](uint8_t f) -> const Font& {
        return kit_.font(f == GameTip::kHeader ? kFontNormal : f == GameTip::kBody ? kFontSmall : kFontTiny);
    };
    auto moneyWidth = [&](uint32_t copper) {
        const Font& fs = kit_.font(kFontSmall);
        const uint32_t parts[3] = {copper / 10000u, (copper / 100u) % 100u, copper % 100u};
        float w = 0.0f;
        for (int p = 0; p < 3; ++p) {
            if (parts[p] == 0 && !(p == 2 && parts[0] == 0 && parts[1] == 0)) continue;
            w += static_cast<float>(fs.measure(fmtStr("%u", static_cast<unsigned>(parts[p])).c_str())) + 13.0f;
        }
        return w;
    };
    for (int pass = 0; pass < 2; ++pass) {
        const float wrapW = t.shopping ? kWrapShopW : kWrapW;
        t.shown.clear();
        for (const GameTip::Line& l : t.lines) {
            if (l.wrap || l.left.find('\n') != std::string::npos) {
                for (const std::string& s : wrapMarkup(font(l.font), l.left, wrapW)) {
                    GameTip::Line w = l;
                    w.left = s;
                    w.wrap = false;
                    t.shown.push_back(std::move(w));
                }
            } else {
                t.shown.push_back(l);
            }
        }
        float w = 0.0f, h = 0.0f;
        for (const GameTip::Line& l : t.shown) {
            const Font& f = font(l.font);
            float lw = markupWidth(f, l.left) + (l.socket >= 0 ? kSocketW : 0.0f);
            if (l.money) lw += 4.0f + moneyWidth(l.copper);
            if (l.dressUp) lw += dressUpKeys(nullptr, 0.0f, 0.0f, static_cast<float>(f.lineHeight())) + 4.0f;
            if (!l.right.empty()) lw += kSplitGap + markupWidth(f, l.right);
            w = std::max(w, lw);
            h += static_cast<float>(f.lineHeight()) + kLineGap;
        }
        t.w = std::ceil(w + 2.0f * kPad);
        t.h = std::ceil(h - kLineGap + 2.0f * kPad);
        // PSP: a GameTooltip taller than the screen - its body in the 8 px font (own rule: nothing
        // scrolls).
        if (pass == 0 && t.h > pp::kScreenH - 4 && !t.shopping) {
            for (GameTip::Line& l : t.lines)
                if (l.font == GameTip::kBody) l.font = GameTip::kSmall;
            continue;
        }
        break;
    }
}

void WorldScreen::drawTip(UiRenderer& ui, const GameTip& t, float x, float y) {
    TipState& ts = tipState();
    const Rect r{x, y, t.w, t.h};
    ui.fill(Rect{r.x + 2, r.y + 2, r.w - 4, r.h - 4}, kTipBackground);
    if (ts.border.pixels) ui.nineSlice(ts.border, r);
    else if (kit_.has(kTexTipBorder)) ui.nineSlice(kit_.tex(kTexTipBorder), r);
    float ly = y + kPad;
    for (const GameTip::Line& l : t.shown) {
        const Font& f = kit_.font(l.font == GameTip::kHeader ? kFontNormal : l.font == GameTip::kBody ? kFontSmall : kFontTiny);
        const float lh = static_cast<float>(f.lineHeight());
        float lx = x + kPad;
        if (l.socket >= 0 && l.socket < 5) {
            const pp::Texture& s = ts.socket[static_cast<size_t>(l.socket)];
            if (s.pixels) ui.image(s, Rect{lx, ly + (lh - 10.0f) * 0.5f, 10, 10});
            lx += kSocketW;
        }
        if (l.dressUp) lx += dressUpKeys(&ui, lx, ly, lh) + 4.0f;  // "L + [Triangle]"
        drawMarkup(ui, f, lx, ly, l.left, l.colL);
        if (l.money) drawMoneyFrame(ui, lx + markupWidth(f, l.left) + 4.0f, ly, l.copper, false);
        if (!l.right.empty()) drawMarkup(ui, f, x + t.w - kPad - markupWidth(f, l.right), ly, l.right, l.colR);
        ly += lh + kLineGap;
    }
}

namespace {
// GameTooltip:SetOwner anchors (clampedToScreen): ANCHOR_RIGHT = its BOTTOMLEFT at the owner's TOPRIGHT,
// ANCHOR_LEFT = its BOTTOMRIGHT at the owner's TOPLEFT; GameTooltip_SetDefaultAnchor = BOTTOMRIGHT at the
// screen's bottom right over the bars (PSP: x 476, y 230 - over MainMenuBar's gryphons).
enum TipAnchor { kAnchorRight, kAnchorLeft, kAnchorDefault };
void anchorTip(TipAnchor a, const Rect& owner, float w, float h, float& x, float& y) {
    switch (a) {
        case kAnchorRight: x = owner.x + owner.w; y = owner.y - h; break;
        case kAnchorLeft: x = owner.x - w; y = owner.y - h; break;
        default: x = 476.0f - w; y = 230.0f - h; break;
    }
    x = std::clamp(x, 2.0f, std::max(2.0f, pp::kScreenW - 2.0f - w));
    y = std::clamp(y, 2.0f, std::max(2.0f, pp::kScreenH - 2.0f - h));
}
}  // namespace

// The entry (drawWindows, after every window): an item of ours (bags / paperdoll: invTipId_) or a
// merchant item (tipProtoItem_). The GameTooltip above + ShoppingTooltip1/2.
void WorldScreen::drawItemTooltip(UiRenderer& ui, const game::ItemDef& item, const Rect& anchor) {
    PROF_SCOPE("tooltip");
    TipState& ts = tipState();
    const bool ours = invTipId_ != 0;
    int bc = 0, bs = 0;
    const bool inBag = ours && bagSlotId(invTipId_, bc, bs);
    const bool atMerchant = ours && windowOpen(kWinVendor) && merch_.tab == 1;
    const uint32_t nowMs = pp::logUptimeMs();
    const uint32_t level = session_.selfField(game::fieldIndex(UF::UNIT_FIELD_LEVEL));
    const uint64_t key = (static_cast<uint64_t>(item.itemId) << 32) ^ item.guid ^
                         (static_cast<uint64_t>(session_.inventoryVersion()) << 8) ^ (atMerchant ? 1u : 0u) ^
                         (static_cast<uint64_t>(level) << 48) ^ (static_cast<uint64_t>(item.stackCount) << 40) ^
                         (static_cast<uint64_t>(itemCooldownTipKey(item.itemId)) << 12) ^
                         (ours ? 0ull : ((static_cast<uint64_t>(aucTipBid_) * 2654435761ull) ^
                                         (static_cast<uint64_t>(aucTipBuyout_) << 20)));
    GameTip& t = ts.item;
    if (key != t.key || (t.retry && nowMs - t.builtMs > 500u)) {
        t.key = key;
        t.builtMs = nowMs;
        buildItemTip(t, item, false, atMerchant);
        // AuctionFrameItem_OnEnter - a stack's per-unit bid ("|n" + AUCTION_TOOLTIP_BID_PREFIX) and
        // buyout (AUCTION_TOOLTIP_BUYOUT_PREFIX) with their coins (SetTooltipMoney).
        if (!ours && aucTipBid_ > 0) {
            t.add(" ", kTipWhite, GameTip::kBody);
            GameTip::Line& l = t.add(CLIENT_STR(AUCTION_TOOLTIP_BID_PREFIX), kTipWhite, GameTip::kBody);
            l.money = true;
            l.copper = aucTipBid_;
        }
        if (!ours && aucTipBuyout_ > 0) {
            GameTip::Line& l = t.add(CLIENT_STR(AUCTION_TOOLTIP_BUYOUT_PREFIX), kTipWhite, GameTip::kBody);
            l.money = true;
            l.copper = aucTipBuyout_;
        }
        layoutTip(t);
        if (++ts.logs <= 20)
            PSP_LOGI(pp::tag::Gfx, "tooltip_item entry=%u lines=%u w=%.0f h=%.0f reads=%u", static_cast<unsigned>(item.itemId),
                     static_cast<unsigned>(t.shown.size()), static_cast<double>(t.w), static_cast<double>(t.h),
                     static_cast<unsigned>(spellDb_.tipReads()));
    }
    // ContainerFrameItemButton_OnEnter: ANCHOR_LEFT right of the screen's middle, else ANCHOR_RIGHT; the paperdoll
    // and the merchant: ANCHOR_RIGHT.
    const TipAnchor a = inBag && anchor.x + anchor.w >= pp::kScreenW * 0.5f ? kAnchorLeft : kAnchorRight;
    float x = 0, y = 0;
    anchorTip(a, anchor, t.w, t.h, x, y);

    // GameTooltip_ShowCompareItem: Shift (PSP: L held) on an item not worn, always for a merchant's item
    // (MerchantItemButton_OnEnter) or with CVar alwaysCompareItems; the worn items of the slots it would take.
    bool worn = false;
    if (ours && !inBag) worn = true;  // a paperdoll slot
    const bool compare = item.inventoryType > 0 && !worn &&
                         (tipCompareHeld_ || (!ours && windowOpen(kWinVendor) && merch_.tab == 1) || ts.alwaysCompare ||
                          (!ours && aucTipRow_));  // AuctionFrameItem_OnEnter compares always
    int nShop = 0;
    if (compare) {
        for (game::EquipSlot es : game::comparableEquipSlots(item.inventoryType)) {
            if (nShop >= 2) break;
            const auto& slot = session_.getInventory().getEquipSlot(es);
            if (slot.empty()) continue;
            GameTip& s = ts.shop[nShop];
            const uint64_t sk = (static_cast<uint64_t>(slot.item.itemId) << 32) ^ slot.item.guid ^
                                (static_cast<uint64_t>(session_.inventoryVersion()) << 8) ^ 0x5u ^
                                (static_cast<uint64_t>(level) << 48);
            if (sk != s.key || (s.retry && nowMs - s.builtMs > 500u)) {
                s.key = sk;
                s.builtMs = nowMs;
                buildItemTip(s, slot.item, true, false);
                layoutTip(s);
            }
            ++nShop;
        }
    }
    if (nShop > 0) {
        // The side with more room; the main tooltip slides over when the comparison does not fit (the
        // anchor type is not ANCHOR_PRESERVE).
        float total = 0;
        for (int i = 0; i < nShop; ++i) total += ts.shop[i].w;
        const bool left = pp::kScreenW - (x + t.w) < x;
        if (left && total > x) x = std::min(total, pp::kScreenW - t.w - 2.0f);
        else if (!left && x + t.w + total > pp::kScreenW) x = std::max(2.0f, pp::kScreenW - t.w - total);
        float sx = left ? x : x + t.w;
        for (int i = 0; i < nShop; ++i) {
            const GameTip& s = ts.shop[i];
            if (left) sx -= s.w;
            float sy = std::clamp(y + 6.0f, 2.0f, std::max(2.0f, pp::kScreenH - 2.0f - s.h));  // TOPLEFT 0,-10
            drawTip(ui, s, std::max(0.0f, sx), sy);
            if (!left) sx += s.w;
        }
    }
    drawTip(ui, t, x, y);
}

// Every frame from drawActionBar (before the windows): the bar sets the button under the cursor again; out of
// the interface mode the tooltip art goes (it comes back with the next tooltip).
void WorldScreen::tipFrame(bool interfaceMode) {
    tipActionSlot_ = 0;
    if (!interfaceMode && tipState_ && tipState_->artTried) tipState_->releaseArt();
}

// The spell tooltips, after every window: the book's / the trainer icon's (tipSpellId_, ANCHOR_RIGHT of
// tipSpellRect_) and the action button's (ActionButton_SetTooltip: UberTooltips = 1, the default anchor).
void WorldScreen::drawSpellTooltips(UiRenderer& ui) {
    const uint32_t slot = tipActionSlot_;
    tipActionSlot_ = 0;
    TipState* tsp = tipState_.get();
    if (!uiMode_ || settings_.isOpen()) {
        if (tsp && tsp->artTried) tsp->releaseArt();  // PSP: the art goes with the interface mode
        return;
    }
    TipState& ts = tipState();
    if (!ts.spellTips) return;
    uint32_t spellId = 0;
    TipAnchor a = kAnchorRight;
    Rect owner = tipSpellRect_;
    if (tipSpellId_ != 0) {
        spellId = tipSpellId_;
    } else if (slot != 0) {
        const auto& bar = session_.actionBar();
        const size_t idx = static_cast<size_t>(mainBarBase()) + slot - 1;
        if (idx < bar.size()) {
            const game::ActionBarSlot& s = bar[idx];
            if (s.type == game::ActionBarSlot::SPELL) {
                spellId = s.id;
                a = kAnchorDefault;
            } else if (s.type == game::ActionBarSlot::ITEM && s.id) {  // SetAction on an item: its tooltip
                const game::ItemDef def = session_.protoItemDef(s.id, 1);
                GameTip& t = ts.item;
                const uint32_t nowMs = pp::logUptimeMs();
                const uint64_t key = (static_cast<uint64_t>(s.id) << 32) ^ 0xA5u ^ (static_cast<uint64_t>(session_.inventoryVersion()) << 8) ^
                                     (static_cast<uint64_t>(itemCooldownTipKey(s.id)) << 40);
                if (key != t.key || (t.retry && nowMs - t.builtMs > 500u)) {
                    t.key = key;
                    t.builtMs = nowMs;
                    buildItemTip(t, def, false, false);
                    layoutTip(t);
                }
                float x = 0, y = 0;
                anchorTip(kAnchorDefault, owner, t.w, t.h, x, y);
                drawTip(ui, t, x, y);
                return;
            }
        }
    }
    if (spellId == 0) return;
    PROF_SCOPE("tooltip");
    GameTip& t = ts.spell;
    const uint32_t nowMs = pp::logUptimeMs();
    const uint32_t cd = static_cast<uint32_t>(std::ceil(session_.spellCooldown(spellId)));
    const uint64_t key = (static_cast<uint64_t>(spellId) << 32) ^ (static_cast<uint64_t>(cd) << 12) ^
                         (static_cast<uint64_t>(session_.inventoryVersion()) << 40) ^ useCtx_.form ^
                         (static_cast<uint64_t>(session_.knownSpellsVersion()) << 20);
    if (key != t.key || (t.retry && nowMs - t.builtMs > 500u)) {
        t.key = key;
        t.builtMs = nowMs;
        buildSpellTip(t, spellId);
        layoutTip(t);
        if (++ts.logs <= 20)
            PSP_LOGI(pp::tag::Gfx, "tooltip_spell id=%u lines=%u w=%.0f h=%.0f reads=%u", static_cast<unsigned>(spellId),
                     static_cast<unsigned>(t.shown.size()), static_cast<double>(t.w), static_cast<double>(t.h),
                     static_cast<unsigned>(spellDb_.tipReads()));
    }
    if (t.lines.empty()) return;
    float x = 0, y = 0;
    anchorTip(a, owner, t.w, t.h, x, y);
    drawTip(ui, t, x, y);
}

// ClassTrainerSkillDescription = GetTrainerServiceDescription (GameFontHighlightSmall, white) under the cost.
// PSP: no scroll frame - the lines that fit, in the 8 px font; the description is formatted once per service.
void WorldScreen::drawTrainerDescription(UiRenderer& ui, uint32_t spellId, const Rect& box) {
    if (spellId == 0 || box.h <= 0.0f) return;
    static uint32_t lastId = 0;
    static std::vector<std::string> lines;
    static float lastW = 0.0f;
    const Font& f = kit_.font(kFontTiny);
    if (spellId != lastId || box.w != lastW) {
        lastId = spellId;
        lastW = box.w;
        lines = wrapMarkup(f, spellDescription(spellId), box.w);
    }
    const float lh = static_cast<float>(f.lineHeight());
    float y = box.y;
    for (const std::string& l : lines) {
        if (y + lh > box.y + box.h) break;
        drawMarkup(ui, f, box.x, y, l, kTipWhite);
        y += lh;
    }
}

}  // namespace wowee::ui::psp
