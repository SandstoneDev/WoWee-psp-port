// CharacterCreate of the original (include/ui/psp/ui_character_create.hpp).
//
// GlueXML CharacterCreate.xml / .lua on 480 x 272. The original (1024 x 768) has the configuration frame
// on the left (faction banners, 2 x 5 race icons Alliance | Horde, the gender, 2 x 5 class icons, five
// customizations, RANDOMIZE), the race and class descriptions on the right, the name, ACCEPT
// and BACK at the bottom and the model in the middle on the race's scene. PSP layout: the left
// column keeps the icons (26 px, the banners are not drawn), the right column the description of the
// race or the class (whichever icon was chosen last), the customizations, RANDOMIZE, the name
// and the buttons; the model stands in the middle (the scene picture moved right by kCenterShift).
// Logic as the original: SetCharacterRace / SetCharacterClass / SetCharacterGender, unavailable classes
// desaturated (IsRaceClassValid; the death knight wants a level-55 character - the PSP never offers it,
// as before), CycleCharCustomization wraps, RandomizeCharCustomization, CreateCharacter with the look.
#include "ui/psp/ui_character_create.hpp"

#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_osk.hpp"
#include "platform/psp/psp_pak.hpp"
#include "ui/psp/ui_character_screen.hpp"

#include <pspctrl.h>
#include <pspthreadman.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
using game::Class;
using game::Race;

// --- CharCreateArt ---------------------------------------------------------------------------

namespace {

void loadCcTex(pp::Pak& pak, const char* name, pp::Texture& t) {
    if (t.pixels) return;
    const pp::PakEntry* e = pak.find(name);
    void* blob = e ? pak.load(e) : nullptr;
    if (blob && !pp::textureFromBlob(blob, e->size, t, true)) pp::pakFree(blob);
}

void* loadRaw(pp::Pak& pak, const char* name, uint32_t& size) {
    size = 0;
    const pp::PakEntry* e = pak.find(name);
    void* b = e ? pak.load(e) : nullptr;
    if (b) size = e->size;
    return b;
}

pp::Texture* ccTextures(CharCreateArt& a, int i) {
    pp::Texture* all[] = {&a.races,  &a.racesRound, &a.classes,  &a.classesGrey, &a.gender,     &a.hl,       &a.shadow,
                          &a.arrowL, &a.arrowLDown, &a.arrowLHl, &a.arrowR,      &a.arrowRDown, &a.arrowRHl};
    return i < static_cast<int>(sizeof(all) / sizeof(all[0])) ? all[i] : nullptr;
}

}  // namespace

void CharCreateArt::load() {
    if (tried) return;
    tried = true;
    char path[256];
    pp::fsPath(path, sizeof(path), "data/glue.pak");
    pp::Pak pak;
    if (!pak.open(path)) {
        PSP_LOGW(pp::tag::Gfx, "char_create_art glue_pak missing");
        return;
    }
    loadCcTex(pak, "cc_races", races);
    loadCcTex(pak, "cc_racesround", racesRound);
    loadCcTex(pak, "cc_classes", classes);
    loadCcTex(pak, "cc_classes_grey", classesGrey);
    loadCcTex(pak, "cc_gender", gender);
    loadCcTex(pak, "cc_hl", hl);
    loadCcTex(pak, "cc_shadow", shadow);
    loadCcTex(pak, "cc_arrow_l", arrowL);
    loadCcTex(pak, "cc_arrow_l_d", arrowLDown);
    loadCcTex(pak, "cc_arrow_l_hl", arrowLHl);
    loadCcTex(pak, "cc_arrow_r", arrowR);
    loadCcTex(pak, "cc_arrow_r_d", arrowRDown);
    loadCcTex(pak, "cc_arrow_r_hl", arrowRHl);
    custom = loadRaw(pak, "cc_custom", customSize);
    outfits = loadRaw(pak, "cc_outfits", outfitsSize);
    text = loadRaw(pak, "cc_text", textSize);
    pak.close();
    uint32_t bytes = customSize + outfitsSize + textSize;
    for (int i = 0; pp::Texture* t = ccTextures(*this, i); ++i)
        if (t->pixels) bytes += pp::textureBytes(*t);
    PSP_LOGI(pp::tag::Gfx, "char_create_art kb=%u icons=%d text=%u", static_cast<unsigned>(bytes / 1024u),
             races.pixels && classes.pixels ? 1 : 0, static_cast<unsigned>(textSize));
}

void CharCreateArt::release() {
    for (int i = 0; pp::Texture* t = ccTextures(*this, i); ++i)
        if (t->pixels) pp::textureRelease(*t);
    for (void** b : {&custom, &outfits, &text}) {
        if (*b) pp::pakFree(*b);
        *b = nullptr;
    }
    customSize = outfitsSize = textSize = 0;
    tried = false;
}

const char* CharCreateArt::str(const char* key) const {
    if (!text || textSize < 4) return "";
    const auto* b = static_cast<const uint8_t*>(text);
    uint32_t n = 0;
    std::memcpy(&n, b, 4);
    if (4u + 8u * n > textSize) return "";
    uint32_t lo = 0, hi = n;  // sorted by key
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        uint32_t ko = 0, vo = 0;
        std::memcpy(&ko, b + 4 + 8 * mid, 4);
        std::memcpy(&vo, b + 8 + 8 * mid, 4);
        if (ko >= textSize || vo >= textSize) return "";
        const int c = std::strcmp(reinterpret_cast<const char*>(b + ko), key);
        if (c == 0) return reinterpret_cast<const char*>(b + vo);
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return "";
}

const char* CharCreateArt::flavor(const char* key, bool female) const {
    if (female) {
        char k[96];  // the keys are < 64 bytes
        std::snprintf(k, sizeof(k), "%s_FEMALE", key);
        const char* f = str(k);
        if (*f) return f;
    }
    return str(key);
}

CharCreateArt::Ranges CharCreateArt::ranges(Race race, bool female) const {
    Ranges r;
    if (!custom || customSize < 4) return r;
    const auto* b = static_cast<const uint8_t*>(custom);
    uint32_t n = 0;
    std::memcpy(&n, b, 4);
    for (uint32_t i = 0; i < n && 4u + (i + 1) * 32u <= customSize; ++i) {
        const uint8_t* row = b + 4 + i * 32;
        if (row[0] != static_cast<uint8_t>(race) || row[1] != (female ? 1 : 0)) continue;
        r.skins = row[2];
        r.faces = row[3];
        r.hairStyles = row[4];
        r.hairColors = row[5];
        r.facials = row[6];
        std::memcpy(r.hairKey, row + 8, 11);
        r.hairKey[11] = '\0';
        std::memcpy(r.facialKey, row + 20, 11);
        r.facialKey[11] = '\0';
        break;
    }
    return r;
}

void CharCreateArt::outfit(game::Character& c) const {
    c.equipment.assign(23, game::EquipmentItem{0, 0, 0});
    if (!outfits || outfitsSize < 4) return;
    const auto* b = static_cast<const uint8_t*>(outfits);
    uint32_t n = 0;
    std::memcpy(&n, b, 4);
    constexpr uint32_t kRow = 4 + 24 * 4 + 24;
    const uint8_t sex = c.gender == game::Gender::FEMALE ? 1 : 0;
    for (uint32_t i = 0; i < n && 4u + (i + 1) * kRow <= outfitsSize; ++i) {
        const uint8_t* row = b + 4 + i * kRow;
        if (row[0] != static_cast<uint8_t>(c.race) || row[1] != static_cast<uint8_t>(c.characterClass) || row[2] != sex)
            continue;
        bool mainHand = false;
        for (int k = 0; k < 24; ++k) {
            uint32_t display = 0;
            std::memcpy(&display, row + 4 + k * 4, 4);
            const uint8_t inv = row[4 + 96 + k];
            if (!display || !inv) continue;
            // InventoryType -> EquipmentSlot (the server equips the outfit the same way).
            int slot = -1;
            switch (inv) {
                case 1: slot = 0; break;            // head
                case 2: slot = 1; break;            // neck
                case 3: slot = 2; break;            // shoulders
                case 4: slot = 3; break;            // shirt
                case 5: case 20: slot = 4; break;   // chest, robe
                case 6: slot = 5; break;            // waist
                case 7: slot = 6; break;            // legs
                case 8: slot = 7; break;            // feet
                case 9: slot = 8; break;            // wrists
                case 10: slot = 9; break;           // hands
                case 11: slot = 10; break;          // finger
                case 12: slot = 12; break;          // trinket
                case 16: slot = 14; break;          // cloak
                case 13: slot = mainHand ? 16 : 15; mainHand = true; break;  // one-hand: main, then off hand
                case 17: case 21: slot = 15; mainHand = true; break;         // two-hand, main hand
                case 14: case 22: case 23: slot = 16; break;                 // shield, off hand, held
                case 15: case 25: case 26: case 28: slot = 17; break;        // ranged, thrown, wand, relic
                case 19: slot = 18; break;          // tabard
                default: break;                     // bags, ammo, quivers
            }
            if (slot >= 0 && c.equipment[slot].displayModel == 0) c.equipment[slot] = game::EquipmentItem{display, inv, 0};
        }
        return;
    }
}

// --- CharacterScreen: the creation screen ---------------------------------------------------

namespace {

enum : uint32_t {
    kIdCcRace0 = 40,    // 40..49 CharacterCreateRaceButton1..10
    kIdCcClass0 = 50,   // 50..59 CharacterCreateClassButton1..10
    kIdCcMale = 60,
    kIdCcFemale = 61,
    kIdCcLeft0 = 62,    // 62..66 CharacterCustomizationButtonFrame1..5 LeftButton
    kIdCcRight0 = 67,   // 67..71 RightButton
    kIdCcRandom = 72,   // CharCreateRandomizeButton
    kIdCcRotL = 73,     // CharacterCreateRotateLeft / Right
    kIdCcRotR = 74,
    kIdCcName = 75,     // CharacterCreateNameEdit
    kIdCcOkay = 76,     // CharCreateOkayButton
    kIdCcBack = 77,     // CharCreateBackButton
};

// GetAvailableRaces / GetAvailableClasses order of 3.3.5a (Alliance column, then Horde; ChrClasses ids).
const Race kCcRaces[10] = {Race::HUMAN, Race::DWARF,  Race::NIGHT_ELF, Race::GNOME, Race::DRAENEI,
                           Race::ORC,   Race::UNDEAD, Race::TAUREN,    Race::TROLL, Race::BLOOD_ELF};
const Class kCcClasses[10] = {Class::WARRIOR,      Class::PALADIN, Class::HUNTER, Class::ROGUE,   Class::PRIEST,
                              Class::DEATH_KNIGHT, Class::SHAMAN,  Class::MAGE,   Class::WARLOCK, Class::DRUID};

// ChrRaces / ChrClasses file strings (RACE_INFO_<x>, CLASS_<x>, CLASS_INFO_<x>N, <x>_DISABLED).
const char* raceFile(Race r) {
    switch (r) {
        case Race::HUMAN: return "HUMAN";
        case Race::ORC: return "ORC";
        case Race::DWARF: return "DWARF";
        case Race::NIGHT_ELF: return "NIGHTELF";
        case Race::UNDEAD: return "SCOURGE";
        case Race::TAUREN: return "TAUREN";
        case Race::GNOME: return "GNOME";
        case Race::TROLL: return "TROLL";
        case Race::BLOOD_ELF: return "BLOODELF";
        case Race::DRAENEI: return "DRAENEI";
        default: return "HUMAN";
    }
}

const char* classFile(Class c) {
    switch (c) {
        case Class::WARRIOR: return "WARRIOR";
        case Class::PALADIN: return "PALADIN";
        case Class::HUNTER: return "HUNTER";
        case Class::ROGUE: return "ROGUE";
        case Class::PRIEST: return "PRIEST";
        case Class::DEATH_KNIGHT: return "DEATHKNIGHT";
        case Class::SHAMAN: return "SHAMAN";
        case Class::MAGE: return "MAGE";
        case Class::WARLOCK: return "WARLOCK";
        case Class::DRUID: return "DRUID";
    }
    return "WARRIOR";
}

// RACE_ICON_TCOORDS (8 x 4 cells of the atlas; females two rows down).
SrcRect raceCell(const pp::Texture& t, Race r, bool female) {
    int col = 0, row = 0;
    switch (r) {
        case Race::HUMAN: col = 0; row = 0; break;
        case Race::DWARF: col = 1; row = 0; break;
        case Race::GNOME: col = 2; row = 0; break;
        case Race::NIGHT_ELF: col = 3; row = 0; break;
        case Race::DRAENEI: col = 4; row = 0; break;
        case Race::TAUREN: col = 0; row = 1; break;
        case Race::UNDEAD: col = 1; row = 1; break;
        case Race::TROLL: col = 2; row = 1; break;
        case Race::ORC: col = 3; row = 1; break;
        case Race::BLOOD_ELF: col = 4; row = 1; break;
        default: break;
    }
    if (female) row += 2;
    const float cw = t.uw / 8.0f, ch = t.uh / 4.0f;
    return SrcRect{col * cw, row * ch, cw, ch};
}

// CLASS_ICON_TCOORDS (4 x 4 cells).
SrcRect classCell(const pp::Texture& t, Class c) {
    int col = 0, row = 0;
    switch (c) {
        case Class::WARRIOR: col = 0; row = 0; break;
        case Class::MAGE: col = 1; row = 0; break;
        case Class::ROGUE: col = 2; row = 0; break;
        case Class::DRUID: col = 3; row = 0; break;
        case Class::HUNTER: col = 0; row = 1; break;
        case Class::SHAMAN: col = 1; row = 1; break;
        case Class::PRIEST: col = 2; row = 1; break;
        case Class::WARLOCK: col = 3; row = 1; break;
        case Class::PALADIN: col = 0; row = 2; break;
        case Class::DEATH_KNIGHT: col = 1; row = 2; break;
    }
    const float cw = t.uw / 4.0f, ch = t.uh / 4.0f;
    return SrcRect{col * cw, row * ch, cw, ch};
}

bool alliance(Race r) {
    return r == Race::HUMAN || r == Race::DWARF || r == Race::NIGHT_ELF || r == Race::GNOME || r == Race::DRAENEI;
}

// Left-aligned lines broken at spaces and '\n' to fit maxW; draws lines skip .. skip + maxLines - 1 from y
// (maxLines 0: only counts). Returns the lines the text takes.
int wrapLeft(UiRenderer& ui, const Font& f, float x, float y, float maxW, const char* text, uint32_t col, float lineH,
             int skip, int maxLines) {
    std::string line, word;
    int lines = 0;
    auto emit = [&]() {
        if (lines >= skip && lines < skip + maxLines) ui.text(f, x, y + (lines - skip) * lineH, line.c_str(), col);
        ++lines;
        line.clear();
    };
    for (const char* p = text;; ++p) {
        const char ch = *p;
        if (ch != ' ' && ch != '\n' && ch != '\0') {
            word += ch;
            continue;
        }
        if (!word.empty()) {
            std::string cand = line.empty() ? word : line + " " + word;
            if (!line.empty() && f.measure(cand.c_str()) > maxW) {
                emit();
                cand = word;
            }
            line = cand;
            word.clear();
        }
        if (ch == '\n') {
            if (!line.empty()) emit();
            else if (lines > 0) ++lines;  // a blank line between paragraphs
        }
        if (ch == '\0') break;
    }
    if (!line.empty()) emit();
    return lines;
}

// PSP layout.
constexpr Rect kCfg{4, 4, 146, 264};      // CharacterCreateConfigurationFrame
constexpr Rect kInfo{330, 4, 146, 264};   // the description, customizations, name, buttons
constexpr float kIcon = 26.0f;            // CharacterCreateIconButtonTemplate 38 x 38
constexpr float kCenterShift = 90.0f;     // the scene picture / model moved right: the model in the middle
constexpr float kModelX = 240.0f;

}  // namespace

void CharacterScreen::enterCreate() {
    mode_ = Mode::Create;
    model_.release();  // the list's model goes; the preview takes its place
    ccArt_.load();
    // CharacterCreate_OnShow: ResetCharCustomize (a random look), the race / class / gender kept.
    if (!raceClassAllowed(race_, class_)) nextClass();
    ccRandomize();
    ccRaceInfo_ = true;
    ccFacing_ = -15.0f * 3.14159265f / 180.0f;  // SetCharacterCreateFacing(-15)
    focus_.setFocus(kIdCcRace0);
    PSP_LOGI(pp::tag::Loop, "char_create_open race=%u class=%u", static_cast<unsigned>(race_),
             static_cast<unsigned>(class_));
}

void CharacterScreen::leaveCreate() {
    mode_ = Mode::List;
    listFocused_ = false;
    model_.release();
    ccArt_.release();
    const auto& chars = session_.characters();
    if (selected_ >= 0 && selected_ < static_cast<int>(chars.size())) model_.show(chars[selected_]);
}

void CharacterScreen::ccClamp() {
    const CharCreateArt::Ranges r = ccArt_.ranges(race_, gender_ == game::Gender::FEMALE);
    const uint8_t max[5] = {r.skins, r.faces, r.hairStyles, r.hairColors, r.facials};
    for (int i = 0; i < 5; ++i)
        if (look_[i] >= std::max<uint8_t>(1, max[i])) look_[i] = 0;
}

// RandomizeCharCustomization.
void CharacterScreen::ccRandomize() {
    const CharCreateArt::Ranges r = ccArt_.ranges(race_, gender_ == game::Gender::FEMALE);
    const uint8_t max[5] = {r.skins, r.faces, r.hairStyles, r.hairColors, r.facials};
    for (int i = 0; i < 5; ++i) {
        nameSeed_ = nameSeed_ * 1664525u + 1013904223u;
        look_[i] = static_cast<uint8_t>((nameSeed_ >> 16) % std::max<uint8_t>(1, max[i]));
    }
}

// The model of the look being made (SetCharCustomizeFrame): reloaded when something changed.
void CharacterScreen::ccPreview() {
    const uint64_t key = 0xCC00000000000000ull | (static_cast<uint64_t>(race_) << 48) |
                         (static_cast<uint64_t>(class_) << 40) |
                         (static_cast<uint64_t>(gender_ == game::Gender::FEMALE ? 1 : 0) << 39) |
                         (static_cast<uint64_t>(look_[0]) << 32) | (static_cast<uint64_t>(look_[1]) << 24) |
                         (static_cast<uint64_t>(look_[2]) << 16) | (static_cast<uint64_t>(look_[3]) << 8) | look_[4];
    if (model_.guid() == key) return;
    game::Character c{};
    c.guid = key;
    c.name = name_;
    c.race = race_;
    c.characterClass = class_;
    c.gender = gender_;
    c.level = 1;
    c.appearanceBytes = static_cast<uint32_t>(look_[0]) | (static_cast<uint32_t>(look_[1]) << 8) |
                        (static_cast<uint32_t>(look_[2]) << 16) | (static_cast<uint32_t>(look_[3]) << 24);
    c.facialFeatures = look_[4];
    ccArt_.outfit(c);
    model_.show(c);
    model_.rotate(ccFacing_);
}

bool CharacterScreen::updateCreate(uint32_t act, const pp::PadState& pad, float dtSec) {
    // L / R turn the model as CharacterCreateRotateLeft / Right held (CHARACTER_FACING_INCREMENT).
    if (pad.held & PSP_CTRL_LTRIGGER) model_.rotate(-2.0f * dtSec);
    if (pad.held & PSP_CTRL_RTRIGGER) model_.rotate(2.0f * dtSec);
    if (focus_.isPressed(kIdCcRotL)) model_.rotate(-2.0f * dtSec);
    if (focus_.isPressed(kIdCcRotR)) model_.rotate(2.0f * dtSec);
    if (!act) return false;
    const bool busy = session_.state() == pp::WorldSession::State::WaitCharCreate;
    if (act >= kIdCcRace0 && act < kIdCcRace0 + 10) {  // CharacterRace_OnClick -> SetCharacterRace
        if (!busy) {
            race_ = kCcRaces[act - kIdCcRace0];
            if (!raceClassAllowed(race_, class_)) nextClass();
            ccClamp();
            ccRaceInfo_ = true;
            PSP_LOGI(pp::tag::Loop, "char_create_race race=%u", static_cast<unsigned>(race_));
        }
        return true;
    }
    if (act >= kIdCcClass0 && act < kIdCcClass0 + 10) {  // CharacterClass_OnClick (enabled buttons only)
        const Class c = kCcClasses[act - kIdCcClass0];
        if (!busy && raceClassAllowed(race_, c)) {
            class_ = c;
            ccRaceInfo_ = false;
        }
        return true;
    }
    if (act == kIdCcMale || act == kIdCcFemale) {  // SetCharacterGender
        if (!busy) {
            gender_ = act == kIdCcMale ? game::Gender::MALE : game::Gender::FEMALE;
            ccClamp();
        }
        return true;
    }
    if (act >= kIdCcLeft0 && act < kIdCcRight0 + 5) {  // CycleCharCustomization(i, -1 / 1)
        const bool right = act >= kIdCcRight0;
        const int i = static_cast<int>(act - (right ? kIdCcRight0 : kIdCcLeft0));
        const CharCreateArt::Ranges r = ccArt_.ranges(race_, gender_ == game::Gender::FEMALE);
        const int max[5] = {r.skins, r.faces, r.hairStyles, r.hairColors, r.facials};
        const int n = std::max(1, max[i]);
        if (!busy) look_[i] = static_cast<uint8_t>((look_[i] + (right ? 1 : n - 1)) % n);
        return true;
    }
    switch (act) {
        case kIdCcRandom:
            if (!busy) ccRandomize();
            return true;
        case kIdCcName:
            // Character names: 2-12 letters. Latin or Cyrillic both pass AzerothCore's default check
            // (StrictPlayerNames = 0); the server has the last word.
            if (!pp::oskBegin(GLUE_STR(CHARACTER_NAME), name_, 12)) dialog_.show(OWN_STR(PSP_KEYBOARD_FAILED), GLUE_STR(OKAY));
            return true;
        case kIdCcBack: leaveCreate(); return true;  // CharacterCreate_Back
        case kIdCcOkay: {                              // CharacterCreate_Okay -> CreateCharacter
            if (busy) return true;
            game::CharCreateData d;
            d.name = name_;
            d.race = race_;
            d.characterClass = class_;
            d.gender = gender_;
            d.skin = look_[0];
            d.face = look_[1];
            d.hairStyle = look_[2];
            d.hairColor = look_[3];
            d.facialHair = look_[4];
            PSP_LOGI(pp::tag::Net, "char_create race=%u class=%u sex=%u look=%u,%u,%u,%u,%u",
                     static_cast<unsigned>(race_), static_cast<unsigned>(class_),
                     static_cast<unsigned>(gender_ == game::Gender::FEMALE), look_[0], look_[1], look_[2], look_[3],
                     look_[4]);
            session_.createCharacter(d);
            status_ = Status::Create;
            dialog_.show(GLUE_STR(CHAR_CREATE_IN_PROGRESS), GLUE_STR(CANCEL));
            return true;
        }
        default: break;
    }
    return false;
}

void CharacterScreen::drawCreate(UiRenderer& ui, FocusRing& focus) {
    ccArt_.load();
    const bool female = gender_ == game::Gender::FEMALE;
    const bool busy = session_.state() == pp::WorldSession::State::WaitCharCreate;
    const Font& ft = kit_.font(kFontTiny);
    const Font& fs = kit_.font(kFontSmall);
    const Font& fn = kit_.font(kFontNormal);
    // FACTION_BACKDROP_COLOR_TABLE: border and background by the race's faction.
    const bool ally = alliance(race_);
    const uint32_t backCol = ally ? 0xE0301717u : 0xE00D0D30u;    // (0.09,0.09,0.19) / (0.19,0.05,0.05)
    const uint32_t edgeCol = ally ? 0xFF808080u : 0xFF333380u;    // (0.5,0.5,0.5) / (0.5,0.2,0.2)
    auto panel = [&](const Rect& r) {
        if (kit_.has(kTexTipBg)) ui.tile(kit_.tex(kTexTipBg), Rect{r.x + 4, r.y + 4, r.w - 8, r.h - 8}, backCol);
        else ui.fill(r, backCol);
        if (kit_.has(kTexTipBorder)) ui.nineSlice(kit_.tex(kTexTipBorder), r, 1.0f, edgeCol);
    };
    // CharacterCreateIconButtonTemplate: shadow, icon, bevel edge, checked (ADD), highlight on the focus (ADD).
    auto iconButton = [&](uint32_t id, const Rect& r, const pp::Texture& tex, const SrcRect& src, bool checked,
                          bool enabled) {
        const bool f = focus.addStop(id, r);
        if (ccArt_.shadow.pixels) {
            const float s = r.w * 58.0f / 38.0f;
            ui.image(ccArt_.shadow, Rect{r.x + (r.w - s) * 0.5f, r.y + (r.h - s) * 0.5f, s, s}, 0xC0FFFFFFu);
        }
        if (tex.pixels) ui.image(tex, r, src, enabled ? 0xFFFFFFFFu : 0xFFB0B0B0u);
        else ui.fill(r, 0xFF303030u);
        const pp::Texture& hl = ccArt_.hl;
        if (hl.pixels) {
            const float hw = hl.uw * 0.5f, hh = hl.uh * 0.5f;
            ui.image(hl, r, SrcRect{0, hh, hw, hh});                                         // BevelEdge
            if (checked) ui.image(hl, r, SrcRect{hw, 0, hw, hh}, 0xFFFFFFFFu, Blend::Add);   // CheckedTexture
            if (f) ui.image(hl, r, SrcRect{0, 0, hw, hh}, 0xFFFFFFFFu, Blend::Add);          // HighlightTexture
        } else if (checked || f) {
            ui.fill(Rect{r.x, r.y + r.h - 2, r.w, 2}, checked ? 0xFF00C7FFu : 0xFFFFFFFFu);
        }
        return f;
    };

    // --- CharacterCreateConfigurationFrame ---
    panel(kCfg);
    const float colA = kCfg.x + 40, colH = kCfg.x + 106;  // Race buttons 1-5 TOP -50, 6-10 TOP +50
    ui.textShadow(fs, colA, kCfg.y + 6, ccArt_.str("ALLIANCE"), kGlueGold, Align::Center);
    ui.textShadow(fs, colH, kCfg.y + 6, ccArt_.str("HORDE"), kGlueGold, Align::Center);
    for (int i = 0; i < 10; ++i) {
        const Race r = kCcRaces[i];
        const float cx = i < 5 ? colA : colH;
        const Rect b{cx - kIcon * 0.5f, kCfg.y + 20 + (i % 5) * 29.0f, kIcon, kIcon};
        if (iconButton(kIdCcRace0 + i, b, ccArt_.races, raceCell(ccArt_.races, r, female), r == race_, true))
            ccRaceInfo_ = true;
    }
    // CharacterCreateGender (the chosen gender's name) over the two gender buttons.
    const float gy = kCfg.y + 166;
    const float pcx = kCfg.x + kCfg.w * 0.5f;
    ui.textShadow(fs, pcx, gy, female ? ccArt_.str("FEMALE") : ccArt_.str("MALE"), kColWhite, Align::Center);
    const pp::Texture& gt = ccArt_.gender;
    const float gw = gt.uw * 0.5f;
    iconButton(kIdCcMale, Rect{pcx - 4 - kIcon, gy + 12, kIcon, kIcon}, gt, SrcRect{0, 0, gw, static_cast<float>(gt.uh)},
               !female, true);
    iconButton(kIdCcFemale, Rect{pcx + 4, gy + 12, kIcon, kIcon}, gt, SrcRect{gw, 0, gw, static_cast<float>(gt.uh)},
               female, true);
    // CharacterCreateClassButton1..10: two rows of five; unavailable ones desaturated (SetButtonDesaturated).
    for (int i = 0; i < 10; ++i) {
        const Class c = kCcClasses[i];
        const bool ok = raceClassAllowed(race_, c);
        const Rect b{kCfg.x + 7 + (i % 5) * 27.0f, kCfg.y + 207 + (i / 5) * 28.0f, kIcon, kIcon};
        const pp::Texture& t = ok || !ccArt_.classesGrey.pixels ? ccArt_.classes : ccArt_.classesGrey;
        if (iconButton(kIdCcClass0 + i, b, t, classCell(t, c), c == class_, ok)) {
            ccRaceInfo_ = false;
            if (!ok) {  // the button's tooltip: <CLASS>_DISABLED
                char key[48];
                std::snprintf(key, sizeof(key), "%s_DISABLED", classFile(c));
                const char* tip = ccArt_.str(key);
                if (*tip) kit_.tooltip(ui, b, className(c, female), std::strchr(tip, '\n') ? std::strchr(tip, '\n') + 1 : "");
            }
        }
    }

    // --- The description (CharacterCreateCharacterRace / Class): the last chosen of the two ---
    panel(kInfo);
    const float ix = kInfo.x + 8, iw = kInfo.w - 16;
    char key[64];
    if (ccRaceInfo_) {
        if (ccArt_.racesRound.pixels)
            ui.image(ccArt_.racesRound, Rect{ix, kInfo.y + 6, 22, 22}, raceCell(ccArt_.racesRound, race_, female));
        std::snprintf(key, sizeof(key), "RACE_NAME_%u%s", static_cast<unsigned>(race_), female ? "_FEMALE" : "");
        const char* nm = ccArt_.str(key);
        ui.textShadow(fn, ix + 26, kInfo.y + 9, *nm ? nm : raceName(race_), kGlueGold);
    } else {
        if (ccArt_.classes.pixels) ui.image(ccArt_.classes, Rect{ix, kInfo.y + 6, 22, 22}, classCell(ccArt_.classes, class_));
        ui.textShadow(fn, ix + 26, kInfo.y + 9, className(class_, female), kGlueGold);
    }
    {
        // ABILITY_INFO_<RACE>1.. / CLASS_INFO_<CLASS>0.. (GlueFontNormalSmall, gold), then the flavour text
        // (GlueFontCharacterCreate, white). PSP: the box holds 10 lines and has no scroll bar - a longer text
        // scrolls by itself (3 s still, a line every 1.2 s, 3 s at the end, again).
        std::string abilities;
        for (int k = ccRaceInfo_ ? 1 : 0; k < 12; ++k) {
            std::snprintf(key, sizeof(key), ccRaceInfo_ ? "ABILITY_INFO_%s%d" : "CLASS_INFO_%s%d",
                          ccRaceInfo_ ? raceFile(race_) : classFile(class_), k);
            const char* t = ccArt_.str(key);
            if (!*t) break;
            if (!abilities.empty()) abilities += '\n';
            abilities += t;
        }
        constexpr float kLineH = 9.0f;
        constexpr int kLines = 10;
        const float ty = kInfo.y + 32;
        std::snprintf(key, sizeof(key), ccRaceInfo_ ? "RACE_INFO_%s" : "CLASS_%s",
                      ccRaceInfo_ ? raceFile(race_) : classFile(class_));
        const char* flav = ccArt_.flavor(key, female);
        const int na = wrapLeft(ui, ft, ix, ty, iw, abilities.c_str(), kGlueGold, kLineH, 0, 0);
        const int total = na + 1 + wrapLeft(ui, ft, ix, ty, iw, flav, kColWhite, kLineH, 0, 0);
        const uint32_t infoKey = (ccRaceInfo_ ? 0x10000u : 0u) | (static_cast<uint32_t>(race_) << 8) |
                                 (static_cast<uint32_t>(class_) << 1) | (female ? 1u : 0u);
        if (infoKey != ccInfoKey_) {
            ccInfoKey_ = infoKey;
            ccInfoT0_ = nowMs_;
        }
        int skip = 0;
        if (total > kLines) {
            const int steps = total - kLines;
            const uint32_t cycle = 3000u + static_cast<uint32_t>(steps) * 1200u + 3000u;
            const uint32_t t = (nowMs_ - ccInfoT0_) % cycle;
            skip = t < 3000u ? 0 : std::min(steps, static_cast<int>((t - 3000u) / 1200u) + 1);
        }
        wrapLeft(ui, ft, ix, ty, iw, abilities.c_str(), kGlueGold, kLineH, skip, kLines);
        const int fs0 = na + 1 - skip;  // the flavour text's first line on the box (may be above it)
        wrapLeft(ui, ft, ix, ty + std::max(0, fs0) * kLineH, iw, flav, kColWhite, kLineH, std::max(0, -fs0),
                 kLines - std::max(0, fs0));
    }

    // --- CharacterCustomizationButtonFrame1..5: CharacterCreate-LabelFrame text between the arrows ---
    const CharCreateArt::Ranges rng = ccArt_.ranges(race_, female);
    char hs[40], hc[40], fh[40];
    std::snprintf(hs, sizeof(hs), "HAIR_%s_STYLE", rng.hairKey);
    std::snprintf(hc, sizeof(hc), "HAIR_%s_COLOR", rng.hairKey);
    std::snprintf(fh, sizeof(fh), "FACIAL_HAIR_%s", rng.facialKey);
    const char* labels[5] = {ccArt_.str("CHAR_CUSTOMIZATION1_DESC"), ccArt_.str("CHAR_CUSTOMIZATION2_DESC"),
                             ccArt_.str(hs), ccArt_.str(hc), ccArt_.str(fh)};
    float y = kInfo.y + 128;
    for (int i = 0; i < 5; ++i, y += 17) {
        if (i == 4 && std::strcmp(rng.facialKey, "NONE") == 0) continue;  // FACIAL_HAIR_NONE: the row is hidden
        const Rect l{kInfo.x + 6, y, 16, 16}, r{kInfo.x + kInfo.w - 22, y, 16, 16};
        const bool fl = !busy && focus.addStop(kIdCcLeft0 + i, l);
        const bool fr = !busy && focus.addStop(kIdCcRight0 + i, r);
        const pp::Texture& al = focus.isPressed(kIdCcLeft0 + i) && ccArt_.arrowLDown.pixels ? ccArt_.arrowLDown : ccArt_.arrowL;
        const pp::Texture& ar = focus.isPressed(kIdCcRight0 + i) && ccArt_.arrowRDown.pixels ? ccArt_.arrowRDown : ccArt_.arrowR;
        if (al.pixels) ui.image(al, l);
        if (ar.pixels) ui.image(ar, r);
        if (fl && ccArt_.arrowLHl.pixels) ui.image(ccArt_.arrowLHl, l, 0xFFFFFFFFu, Blend::Add);
        if (fr && ccArt_.arrowRHl.pixels) ui.image(ccArt_.arrowRHl, r, 0xFFFFFFFFu, Blend::Add);
        ui.textShadow(fs, kInfo.x + kInfo.w * 0.5f, y + 3, *labels[i] ? labels[i] : "?", kColWhite, Align::Center);
    }
    // CharCreateRandomizeButton, CharacterCreateNameEdit (NAME), CharCreateOkayButton, CharCreateBackButton.
    const float bx = kInfo.x + 8, bw = kInfo.w - 16;
    glueButton(ui, kit_, focus, kIdCcRandom, Rect{bx, kInfo.y + 214, bw, 17}, ccArt_.str("RANDOMIZE"), !busy, kFontSmall);
    {
        const Rect box{bx, kInfo.y + 233, bw, 15};
        const bool f = !busy && focus.addStop(kIdCcName, box);
        ui.fill(box, 0xD8171717u);
        if (kit_.has(kTexTipBorder)) ui.nineSlice(kit_.tex(kTexTipBorder), box, 1.0f, f ? kColWhite : edgeCol);
        char line[64];
        std::snprintf(line, sizeof(line), "%s: %s", ccArt_.str("NAME"), name_.c_str());
        ui.text(fs, box.x + box.w * 0.5f, box.y + 2, line, kColWhite, Align::Center);
    }
    const float hw = (bw - 4) * 0.5f;
    glueButton(ui, kit_, focus, kIdCcOkay, Rect{bx, kInfo.y + 250, hw, 16}, ccArt_.str("CHARACTER_CREATE_ACCEPT"), !busy,
               kFontSmall);
    glueButton(ui, kit_, focus, kIdCcBack, Rect{bx + hw + 4, kInfo.y + 250, hw, 16}, ccArt_.str("BACK"), !busy,
               kFontSmall);

    // CharacterCreateRotateLeft / Right (BOTTOM -16 / +16) under the model.
    if (model_.loaded()) {
        const Rect rl{kModelX - 30, 244, 22, 22}, rr{kModelX + 8, 244, 22, 22};
        const bool fl = focus.addStop(kIdCcRotL, rl), fr = focus.addStop(kIdCcRotR, rr);
        if (art_.rotL.pixels) ui.image(art_.rotL, rl, fl ? 0xFFFFFFFFu : 0xFFC0C0C0u);
        if (art_.rotR.pixels) ui.image(art_.rotR, rr, fr ? 0xFFFFFFFFu : 0xFFC0C0C0u);
    }
}

float CharacterScreen::ccCenterShift() { return kCenterShift; }

} // namespace wowee::ui::psp
