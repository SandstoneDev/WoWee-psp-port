#include "ui/psp/ui_character_screen.hpp"

#include "game/world_packets.hpp"
#include "platform/psp/psp_config.hpp"  // autoworlddelay
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_osk.hpp"
#include "ui/psp/ui_login_screen.hpp"  // FrameStats

#include <pspctrl.h>
#include <pspthreadman.h>

#include <algorithm>
#include <cstdio>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
namespace text = wowee::platform::psp::text;
using game::Class;
using game::Race;

namespace {

enum : uint32_t {
    kIdCreate = 1,
    kIdExit = 2,
    // 10..16: the earlier text buttons of the creation screen; CharacterCreate uses 40..77
    // (ui_character_create.cpp).
    kIdDelWord = 20,
    kIdDelOk = 21,
    kIdDelCancel = 22,
    // CharacterSelect's buttons.
    kIdEnterWorld = 30,
    kIdDelete = 31,
    kIdBack = 32,
    kIdChangeRealm = 33,
    kIdRotLeft = 34,
    kIdRotRight = 35,
    kIdChar0 = 100,
};

// CharacterSelect on 480 x 272 (CharacterSelect.xml at 1024 x 768, squeezed: the right
// column is CharacterSelectCharacterFrame, the left the model and ENTER_WORLD).
constexpr Rect kPanel{300, 4, 176, 238};       // CharacterSelectCharacterFrame (TOPRIGHT -5,-15)
constexpr float kListY = 44.0f, kListBottom = 214.0f;  // the character buttons
constexpr float kRowH = 38.0f;                 // name + info + location (256 x 70 buttons, 57 apart); 38 - at 57 the location line sat on the highlight border
constexpr float kModelCx = 150.0f, kModelCy = 132.0f, kModelH = 132.0f;

const Race kRaces[] = {Race::HUMAN, Race::DWARF, Race::NIGHT_ELF, Race::GNOME, Race::DRAENEI,
                       Race::ORC,   Race::UNDEAD, Race::TAUREN,   Race::TROLL, Race::BLOOD_ELF};

// 3.3.5a race/class combinations, Death Knight left out (the server wants a
// level-55 character on the account first).
bool allowed(Race r, Class c) {
    switch (r) {
        case Race::HUMAN: return c == Class::WARRIOR || c == Class::PALADIN || c == Class::ROGUE ||
                                 c == Class::PRIEST || c == Class::MAGE || c == Class::WARLOCK;
        case Race::ORC: return c == Class::WARRIOR || c == Class::HUNTER || c == Class::ROGUE ||
                               c == Class::SHAMAN || c == Class::WARLOCK;
        case Race::DWARF: return c == Class::WARRIOR || c == Class::PALADIN || c == Class::HUNTER ||
                                 c == Class::ROGUE || c == Class::PRIEST;
        case Race::NIGHT_ELF: return c == Class::WARRIOR || c == Class::HUNTER || c == Class::ROGUE ||
                                     c == Class::PRIEST || c == Class::DRUID;
        case Race::UNDEAD: return c == Class::WARRIOR || c == Class::ROGUE || c == Class::PRIEST ||
                                  c == Class::MAGE || c == Class::WARLOCK;
        case Race::TAUREN: return c == Class::WARRIOR || c == Class::HUNTER || c == Class::SHAMAN ||
                                  c == Class::DRUID;
        case Race::GNOME: return c == Class::WARRIOR || c == Class::ROGUE || c == Class::MAGE ||
                                 c == Class::WARLOCK;
        case Race::TROLL: return c == Class::WARRIOR || c == Class::HUNTER || c == Class::ROGUE ||
                                 c == Class::PRIEST || c == Class::SHAMAN || c == Class::MAGE;
        case Race::BLOOD_ELF: return c == Class::PALADIN || c == Class::HUNTER || c == Class::ROGUE ||
                                     c == Class::PRIEST || c == Class::MAGE || c == Class::WARLOCK;
        case Race::DRAENEI: return c == Class::WARRIOR || c == Class::PALADIN || c == Class::HUNTER ||
                                   c == Class::PRIEST || c == Class::SHAMAN || c == Class::MAGE;
        default: return false;
    }
}

}  // namespace

bool raceClassAllowed(Race r, Class c) { return allowed(r, c); }

namespace {

const Class kClasses[] = {Class::WARRIOR, Class::PALADIN, Class::HUNTER,  Class::ROGUE,   Class::PRIEST,
                          Class::SHAMAN,  Class::MAGE,    Class::WARLOCK, Class::DRUID};

// SMSG_CHAR_CREATE ResponseCodes (game::CharCreateResult) -> GlueStrings.lua (strings.pak, the running language).
const char* createResult(int r) {
    switch (r) {
        case 0x2F: return GLUE_STR(CHAR_CREATE_SUCCESS);
        case 0x30: return GLUE_STR(CHAR_CREATE_ERROR);
        case 0x31: return GLUE_STR(CHAR_CREATE_FAILED);
        case 0x32: return GLUE_STR(CHAR_CREATE_NAME_IN_USE);
        case 0x33: return GLUE_STR(CHAR_CREATE_DISABLED);
        case 0x34: return GLUE_STR(CHAR_CREATE_PVP_TEAMS_VIOLATION);
        case 0x35: return GLUE_STR(CHAR_CREATE_SERVER_LIMIT);
        case 0x36: return GLUE_STR(CHAR_CREATE_ACCOUNT_LIMIT);
        case 0x37: return GLUE_STR(CHAR_CREATE_SERVER_QUEUE);
        case 0x38: return GLUE_STR(CHAR_CREATE_ONLY_EXISTING);
        case 0x39: return GLUE_STR(CHAR_CREATE_EXPANSION);
        case 0x3A: return GLUE_STR(CHAR_CREATE_EXPANSION_CLASS);
        case 0x3B: return GLUE_STR(CHAR_CREATE_LEVEL_REQUIREMENT);
        case 0x3C: return GLUE_STR(CHAR_CREATE_UNIQUE_CLASS_LIMIT);
        case 0x3E: return GLUE_STR(CHAR_FACTION_CHANGE_RACECLASS_RESTRICTED);
        // CHAR_NAME_* (name validation)
        case 0x58: return GLUE_STR(CHAR_NAME_FAILURE);
        case 0x59: return GLUE_STR(CHAR_NAME_NO_NAME);
        case 0x5A: return GLUE_STR(CHAR_NAME_TOO_SHORT);
        case 0x5B: return GLUE_STR(CHAR_NAME_TOO_LONG);
        case 0x5C: return GLUE_STR(CHAR_NAME_INVALID_CHARACTER);
        case 0x5D: return GLUE_STR(CHAR_NAME_MIXED_LANGUAGES);
        case 0x5E: return GLUE_STR(CHAR_NAME_PROFANE);
        case 0x5F: return GLUE_STR(CHAR_NAME_RESERVED);
        case 0x60: return GLUE_STR(CHAR_NAME_INVALID_APOSTROPHE);
        case 0x61: return GLUE_STR(CHAR_NAME_MULTIPLE_APOSTROPHES);
        case 0x62: return GLUE_STR(CHAR_NAME_THREE_CONSECUTIVE);
        case 0x63: return GLUE_STR(CHAR_NAME_INVALID_SPACE);
        case 0x65: return GLUE_STR(CHAR_NAME_RUSSIAN_CONSECUTIVE_SILENT_CHARACTERS);
        case 0x66: return GLUE_STR(CHAR_NAME_RUSSIAN_SILENT_CHARACTER_AT_BEGINNING_OR_END);
        case 0x67: return GLUE_STR(CHAR_NAME_DECLENSION_DOESNT_MATCH_BASE_NAME);
        default: return nullptr;
    }
}

// SMSG_CHAR_DELETE ResponseCodes (AzerothCore SharedDefines.h 0x46..0x4B) -> the client's own words (GlueStrings.lua).
const char* deleteResult(int r) {
    switch (r) {
        case 0x46: return GLUE_STR(CHAR_DELETE_IN_PROGRESS);
        case 0x47: return GLUE_STR(CHAR_DELETE_SUCCESS);
        case 0x48: return GLUE_STR(CHAR_DELETE_FAILED);
        case 0x49: return GLUE_STR(CHAR_DELETE_FAILED_LOCKED_FOR_TRANSFER);
        case 0x4A: return GLUE_STR(CHAR_DELETE_FAILED_GUILD_LEADER);
        case 0x4B: return GLUE_STR(CHAR_DELETE_FAILED_ARENA_CAPTAIN);
        default: return nullptr;
    }
}

// strupper for the confirm word (CharacterSelect.xml, CharacterDeleteEditBox
// OnTextChanged: strupper(text) == strupper(DELETE_CONFIRM_STRING)); the ruRU
// word is Cyrillic, so the Cyrillic lower case is folded as well. Spaces around are dropped.
std::string upperWord(const std::string& s) {
    const size_t b = s.find_first_not_of(' '), e = s.find_last_not_of(' ');
    std::string out;
    if (b == std::string::npos) return out;
    for (size_t i = b; i <= e; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const unsigned char d = i + 1 <= e ? static_cast<unsigned char>(s[i + 1]) : 0;
        if (c >= 'a' && c <= 'z') {
            out += static_cast<char>(c - 32);
        } else if (c == 0xD0 && d >= 0xB0 && d <= 0xBF) {  // U+0430..U+043F
            out += '\xD0';
            out += static_cast<char>(d - 0x20);
            ++i;
        } else if (c == 0xD1 && d >= 0x80 && d <= 0x8F) {  // U+0440..U+044F
            out += '\xD0';
            out += static_cast<char>(d + 0x20);
            ++i;
        } else if (c == 0xD1 && d == 0x91) {  // U+0451
            out += "\xD0\x81";
            ++i;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

bool isFemale(const game::Character& c) {
    return c.gender == game::Gender::FEMALE || (c.gender == game::Gender::NONBINARY && c.useFemaleModel);
}

constexpr uint32_t kCharFlagGhost = 0x00002000u;  // CHARACTER_FLAG_GHOST (SMSG_CHAR_ENUM)

} // namespace

const char* raceName(Race race, bool female) {
    using pp::CS;
    // ChrRaces.dbc Name_lang / Name_female_lang of the running language (strings.pak DBC_RACE_<id>[_FEMALE]).
    static const CS kMale[] = {CS::DBC_RACE_1, CS::DBC_RACE_2, CS::DBC_RACE_3, CS::DBC_RACE_4,  CS::DBC_RACE_5, CS::DBC_RACE_6,
                               CS::DBC_RACE_7, CS::DBC_RACE_8, CS::DBC_RACE_9, CS::DBC_RACE_10, CS::DBC_RACE_11};
    static const CS kFemale[] = {CS::DBC_RACE_1_FEMALE, CS::DBC_RACE_2_FEMALE,  CS::DBC_RACE_3_FEMALE,
                                 CS::DBC_RACE_4_FEMALE, CS::DBC_RACE_5_FEMALE,  CS::DBC_RACE_6_FEMALE,
                                 CS::DBC_RACE_7_FEMALE, CS::DBC_RACE_8_FEMALE,  CS::DBC_RACE_9_FEMALE,
                                 CS::DBC_RACE_10_FEMALE, CS::DBC_RACE_11_FEMALE};
    const int i = static_cast<int>(race) - 1;
    if (i < 0 || i >= 11) return "?";
    return pp::cs(female ? kFemale[i] : kMale[i]);
}

const char* mapName(uint32_t mapId) {
    switch (mapId) {  // Map.dbc MapName_lang; any other map: "Dungeon" (the list's fallback, as before)
        case 0: return CLIENT_STR(DBC_MAP_0);
        case 1: return CLIENT_STR(DBC_MAP_1);
        case 530: return CLIENT_STR(DBC_MAP_530);
        case 571: return CLIENT_STR(DBC_MAP_571);
        case 609: return CLIENT_STR(DBC_MAP_609);
        default: return CLIENT_STR(LFG_TYPE_DUNGEON);
    }
}

CharacterScreen::CharacterScreen(const UiKit& kit, pp::WorldSession& session, std::string realmName,
                                 bool autoCreate, bool autoOsk)
    : kit_(kit), session_(session), realmName_(std::move(realmName)), autoCreate_(autoCreate), autoOsk_(autoOsk) {
    nameSeed_ = static_cast<uint32_t>(sceKernelGetSystemTimeLow());
    rerollName();
}

void CharacterScreen::releaseForWorld() {
    model_.release();
    scene_.release();
    art_.release();
    dialog_.hide();
}

void CharacterScreen::rerollName() {
    // Latin syllables: AzerothCore's default name rules accept letters only,
    // one capital first, 2-12 characters.
    static const char* const kFirst[] = {"Ka", "Tor", "Mel", "Ar", "Val", "Dor", "Ser", "Eli", "Bra", "Gar",
                                         "Lin", "Mor", "Tha", "Rin", "Vel", "Zor"};
    static const char* const kMid[] = {"", "a", "e", "i", "o", "ra", "ri", "an", "el", "or"};
    static const char* const kLast[] = {"n", "th", "dor", "mir", "las", "ric", "wen", "gar", "nis", "rak"};
    auto next = [this]() {
        nameSeed_ = nameSeed_ * 1664525u + 1013904223u;
        return nameSeed_ >> 16;
    };
    name_ = std::string(kFirst[next() % 16]) + kMid[next() % 10] + kLast[next() % 10];
}

void CharacterScreen::nextRace() {
    int idx = 0;
    for (int i = 0; i < 10; ++i)
        if (kRaces[i] == race_) idx = i;
    race_ = kRaces[(idx + 1) % 10];
    if (!allowed(race_, class_)) nextClass();
}

void CharacterScreen::nextClass() {
    int idx = 0;
    for (int i = 0; i < 9; ++i)
        if (kClasses[i] == class_) idx = i;
    for (int step = 1; step <= 9; ++step) {
        Class c = kClasses[(idx + step) % 9];
        if (allowed(race_, c)) {
            class_ = c;
            return;
        }
    }
}

int CharacterScreen::visibleRows() const { return static_cast<int>((kListBottom - kListY) / kRowH); }

// CharacterSelect_SelectCharacter: the highlight locks on it, its model is shown.
void CharacterScreen::selectCharacter(int index) {
    const auto& chars = session_.characters();
    if (index < 0 || index >= static_cast<int>(chars.size())) {
        selected_ = -1;
        model_.release();
        return;
    }
    if (index != selected_) PSP_LOGI(pp::tag::Loop, "char_select index=%d name=%s", index, chars[index].name.c_str());
    selected_ = index;
    model_.show(chars[index]);
}

// The D-pad on the list (CharacterSelect_OnKeyDown UP / DOWN walk the characters): the focus moves a
// row and the list scrolls to keep it on screen.
void CharacterScreen::focusCharacter(int index) {
    const int rows = visibleRows();
    if (index < scroll_) scroll_ = index;
    if (index >= scroll_ + rows) scroll_ = index - rows + 1;
    focus_.setFocus(kIdChar0 + static_cast<uint32_t>(index));
}

// CharacterSelect_EnterWorld.
void CharacterScreen::enterWorld() {
    const auto& chars = session_.characters();
    if (selected_ < 0 || selected_ >= static_cast<int>(chars.size()) || entering_ ||
        session_.state() != pp::WorldSession::State::Ready)
        return;
    const auto& c = chars[selected_];
    PSP_LOGI(pp::tag::Net, "char_selected name=%s guid=%08X%08X map=%u", c.name.c_str(),
             static_cast<unsigned>(c.guid >> 32), static_cast<unsigned>(c.guid), static_cast<unsigned>(c.mapId));
    selectedName_ = c.name;
    entering_ = true;
    enteringMap_ = c.mapId;
    session_.enterWorld(c.guid);
}

void CharacterScreen::update(const pp::PadState& padIn, float dtSec) {
    nowMs_ = pp::logUptimeMs();
    dt_ = dtSec;
    session_.update();
    model_.update(dtSec);
    if (entering_) return;  // the loading screen is up (LoginScreen); nothing here acts any more

    if (autoOsk_ && session_.state() == pp::WorldSession::State::Ready) {
        autoOsk_ = false;
        enterCreate();
        PSP_LOGI(pp::tag::Loop, "autoosk=1");
        pp::oskBegin(GLUE_STR(CHARACTER_NAME), name_, 12);
    }
    // The system keyboard owns the pad while it is open.
    if (pp::oskActive()) return;
    bool accepted = false;
    std::string typed;
    if (pp::oskFinished(accepted, typed)) {
        waitRelease_ = true;
        if (oskForDelete_) {
            oskForDelete_ = false;
            if (accepted) deleteWord_ = typed;
            if (deleteConfirmed()) focus_.setFocus(kIdDelOk);
        } else if (accepted && !typed.empty()) {
            name_ = typed;
            PSP_LOGI(pp::tag::Loop, "name_typed len=%u", static_cast<unsigned>(name_.size()));
        }
    }
    if (waitRelease_) {
        if (padIn.held) return;
        waitRelease_ = false;
    }

    const auto& chars = session_.characters();
    const bool ready = session_.state() == pp::WorldSession::State::Ready;
    pp::PadState pad = padIn;
    // The list walks with UP / DOWN (and scrolls); off its ends the focus ring takes over.
    if (mode_ == Mode::List && !dialog_.shown()) {
        const uint32_t f = focus_.focused();
        if (f >= kIdChar0 && f < kIdChar0 + chars.size()) {
            const int i = static_cast<int>(f - kIdChar0);
            if ((pad.pressed & PSP_CTRL_DOWN) && i + 1 < static_cast<int>(chars.size())) {
                focusCharacter(i + 1);
                pad.pressed &= ~static_cast<uint32_t>(PSP_CTRL_DOWN);
            } else if ((pad.pressed & PSP_CTRL_UP) && i > 0) {
                focusCharacter(i - 1);
                pad.pressed &= ~static_cast<uint32_t>(PSP_CTRL_UP);
            }
        }
        // L / R turn the model (CharacterSelectRotateLeft / Right held).
        if (pad.held & PSP_CTRL_LTRIGGER) model_.rotate(-2.0f * dtSec);
        if (pad.held & PSP_CTRL_RTRIGGER) model_.rotate(2.0f * dtSec);
    }
    focus_.update(pad, dtSec);
    if (mode_ == Mode::List && !dialog_.shown()) {  // the rotate buttons turn while Cross is held on them
        if (focus_.isPressed(kIdRotLeft)) model_.rotate(-2.0f * dtSec);
        if (focus_.isPressed(kIdRotRight)) model_.rotate(2.0f * dtSec);
    }

    // Status dialogs of the original ("CANCEL" type) while the server works: connecting and the list
    // (Cancel = StatusDialogClick: disconnect), creating, deleting (Cancel hides it).
    const auto st = session_.state();
    const char* wantStatus = nullptr;
    if (st == pp::WorldSession::State::Connecting || st == pp::WorldSession::State::WaitChallenge ||
        st == pp::WorldSession::State::WaitAuthResponse)
        wantStatus = GLUE_STR(GAME_SERVER_LOGIN);
    else if (st == pp::WorldSession::State::WaitCharList)
        wantStatus = GLUE_STR(CHAR_LIST_RETRIEVING);
    if (wantStatus && mode_ == Mode::List && (status_ == Status::List || !dialog_.shown())) {
        if (status_ != Status::List || dialog_.text() != wantStatus) dialog_.show(wantStatus, GLUE_STR(CANCEL));
        status_ = Status::List;
    } else if (status_ == Status::List && !wantStatus) {
        status_ = Status::None;
        dialog_.hide();
    } else if ((status_ == Status::Create || status_ == Status::Delete) && ready) {
        status_ = Status::None;
        dialog_.hide();
    }

    if (session_.charListFresh()) {
        session_.clearCharListFresh();
        listFocused_ = false;
        // UpdateCharacterList: keep the selection (index), the last one after a create, else the first.
        const int n = static_cast<int>(chars.size());
        int pick = selectLast_ ? n - 1 : (selected_ >= 0 && selected_ < n ? selected_ : 0);
        selectLast_ = false;
        selected_ = -1;
        scroll_ = 0;
        selectCharacter(n > 0 ? pick : -1);  // the model reloads only for another character
        const int rows = visibleRows();
        if (pick >= rows) scroll_ = pick - rows + 1;
    }
    // wowee.cfg autoworlddelay=<ms> (the autopilot's autotest.txt sets it) keeps the list on the
    // screen that long before the automatic world entry - the character select screen's texts can be read.
    const uint32_t autoDelayMs = static_cast<uint32_t>(std::max(0, pp::configGetInt("autoworlddelay", 0)));
    if (autoCreate_ && ready && lastCreateSeen_ < 0 && !chars.empty() && autoDelayMs > 0 &&
        (autoReadyMs_ == 0 || pp::logUptimeMs() - autoReadyMs_ < autoDelayMs)) {
        if (autoReadyMs_ == 0) autoReadyMs_ = pp::logUptimeMs() | 1u;
    } else if (autoCreate_ && ready && lastCreateSeen_ < 0) {
        autoCreate_ = false;
        if (!chars.empty()) {  // smoke test: straight into the world
            PSP_LOGI(pp::tag::Net, "autoworld=1 name=%s delay_ms=%u", chars.front().name.c_str(),
                     static_cast<unsigned>(autoDelayMs));
            selectCharacter(0);
            enterWorld();
            return;
        }
        PSP_LOGI(pp::tag::Net, "autocreate=1");
        game::CharCreateData d;
        d.name = name_;
        d.race = race_;
        d.characterClass = class_;
        d.gender = gender_;
        session_.createCharacter(d);
    }
    if (mode_ == Mode::List && !listFocused_ && !dialog_.shown() && ready) {
        // Land on the selected character, or on CREATE_NEW_CHARACTER when there are none.
        if (selected_ >= 0) focusCharacter(selected_);
        else focus_.setFocus(kIdCreate);
        listFocused_ = true;
    }
    if (session_.lastCreateResult() != lastCreateSeen_) {
        lastCreateSeen_ = session_.lastCreateResult();
        if (lastCreateSeen_ >= 0) {
            const char* known = createResult(lastCreateSeen_);
            const std::string msg = known ? std::string(known)  // CHAR_CREATE_ERROR + the code the client has no text for
                                          : text::textf("%s (0x%02X)", GLUE_STR(CHAR_CREATE_ERROR),
                                                        static_cast<unsigned>(lastCreateSeen_));
            PSP_LOGI(pp::tag::Net, "char_create_result code=0x%02X", static_cast<unsigned>(lastCreateSeen_));
            status_ = Status::None;
            if (lastCreateSeen_ == 0x2F) {  // CharacterCreate -> CharacterSelect, the new one selected
                // The "creating" status dialog goes too: status_ is None from here, so nothing else hides it (a
                // server that sends the result before the list - a community one - left it up over the new list).
                dialog_.hide();
                mode_ = Mode::List;
                listFocused_ = false;
                selectLast_ = true;
                model_.release();  // the preview; the list loads the new character's model
                ccArt_.release();
            } else {
                dialog_.show(msg, GLUE_STR(OKAY));
            }
        }
    }

    // The server's answer to a delete: a dialog, as GlueDialog shows it.
    if (session_.deleteSeq() != deleteSeqSeen_) {
        deleteSeqSeen_ = session_.deleteSeq();
        const char* known = deleteResult(session_.lastDeleteResult());
        const std::string msg = known ? std::string(known)
                                      : text::textf("%s (0x%02X)", GLUE_STR(CHAR_DELETE_FAILED),
                                                    static_cast<unsigned>(session_.lastDeleteResult() & 0xFF));
        status_ = Status::None;
        dialog_.show(msg, GLUE_STR(OKAY));
    }

    uint32_t act = focus_.activated();
    // A dialog is modal: only its buttons act (the focus may still name a list button under it).
    if (dialog_.shown()) {
        const uint32_t b = dialog_.pressed(act);
        if (b) {
            PSP_LOGI(pp::tag::Loop, "ui_activate id=%u (dialog)", static_cast<unsigned>(b));
            if (status_ == Status::List) {  // StatusDialogClick while the list loads: disconnect
                wantsBack_ = true;
            }
            status_ = Status::None;
            dialog_.hide();
            listFocused_ = false;
        }
        return;
    }
    // START = Escape: CharacterSelect_Exit / CharacterCreate BACK / the delete dialog's Cancel.
    if ((pad.pressed & PSP_CTRL_START)) {
        if (mode_ == Mode::List) {
            PSP_LOGI(pp::tag::Loop, "char_select_exit (START)");
            wantsBack_ = true;
        } else if (mode_ == Mode::Create) {
            leaveCreate();  // CharacterCreate_OnKeyDown ESCAPE -> CharacterCreate_Back
        } else {
            mode_ = Mode::List;
            listFocused_ = false;
        }
        return;
    }
    if (mode_ != Mode::List && act >= kIdChar0) act = 0;
    if (mode_ == Mode::Create && updateCreate(act, pad, dtSec)) return;  // CharacterCreate
    if (!act) return;
    PSP_LOGI(pp::tag::Loop, "ui_activate id=%u", static_cast<unsigned>(act));
    switch (act) {
        case kIdExit: wantsExit_ = true; break;
        case kIdBack:
            PSP_LOGI(pp::tag::Loop, "char_select_exit");
            wantsBack_ = true;
            break;
        case kIdChangeRealm:
            PSP_LOGI(pp::tag::Loop, "char_select_change_realm");
            wantsRealmList_ = true;
            break;
        case kIdEnterWorld: enterWorld(); break;
        case kIdDelete:
            // CharacterSelect_Delete: only with a character selected; the dialog wants the word typed.
            if (ready && selected_ >= 0 && selected_ < static_cast<int>(chars.size())) openDelete(chars[selected_]);
            break;
        case kIdDelWord:
            oskForDelete_ = true;
            if (!pp::oskBegin(GLUE_STR(DELETE_CHARACTER), deleteWord_, 16)) {
                oskForDelete_ = false;
                dialog_.show(OWN_STR(PSP_KEYBOARD_FAILED), GLUE_STR(OKAY));  // PSP: the system keyboard
            }
            break;
        case kIdDelOk:
            if (deleteConfirmed() && deleteGuid_) {
                PSP_LOGI(pp::tag::Net, "char_delete guid=%08X%08X", static_cast<unsigned>(deleteGuid_ >> 32),
                         static_cast<unsigned>(deleteGuid_));
                session_.deleteCharacter(deleteGuid_);
                deleteGuid_ = 0;
                deleteWord_.clear();
                mode_ = Mode::List;
                listFocused_ = false;
                status_ = Status::Delete;
                dialog_.show(GLUE_STR(CHAR_DELETE_IN_PROGRESS), GLUE_STR(CANCEL));
            }
            break;
        case kIdDelCancel:
            deleteGuid_ = 0;
            deleteWord_.clear();
            mode_ = Mode::List;
            listFocused_ = false;
            break;
        case kIdCreate:
            if (ready && chars.size() < 10) enterCreate();  // CharacterCreate_OnShow
            break;
        default:
            if (act >= kIdChar0 && act < kIdChar0 + chars.size()) {
                const int i = static_cast<int>(act - kIdChar0);
                if (i == selected_) enterWorld();  // CharacterSelectButton_OnDoubleClick
                else selectCharacter(i);           // CharacterSelectButton_OnClick
            }
            break;
    }
}

void CharacterScreen::openDelete(const game::Character& c) {
    PSP_LOGI(pp::tag::Loop, "char_delete_dialog name=%s", c.name.c_str());
    deleteGuid_ = c.guid;
    // CONFIRM_CHAR_DELETE (name, level, class): its first line is the title, the second (|cffffffff white) the
    // character - two FontStrings on the PSP (the renderer draws no colour codes).
    const std::string t = text::uiPlain(CLIENT_FMT(GLUE_CONFIRM_CHAR_DELETE, c.name, static_cast<unsigned>(c.level),
                                                   className(c.characterClass, isFemale(c))));
    const size_t nl = t.find('\n');
    deleteTitle_ = t.substr(0, nl);
    deleteLine_ = nl == std::string::npos ? std::string() : t.substr(nl + 1);
    deleteWord_.clear();
    mode_ = Mode::Delete;
    focus_.setFocus(kIdDelWord);
}

// DELETE_CONFIRM_STRING of the running language ("DELETE"), both sides folded.
bool CharacterScreen::deleteConfirmed() const {
    return upperWord(deleteWord_) == upperWord(GLUE_STR(DELETE_CONFIRM_STRING));
}

void CharacterScreen::drawDelete(UiRenderer& ui, FocusRing& focus) {
    // CharacterDeleteDialog: 512 x 256 units, centred; the alert icon on the left.
    const Rect dlg{80, 52, 320, 168};
    ui.fill(Rect{0, 0, static_cast<float>(pp::kScreenW), static_cast<float>(pp::kScreenH)}, 0x60000000u);
    kit_.panel(ui, dlg);
    if (art_.alert.pixels) ui.image(art_.alert, Rect{dlg.x + 14, dlg.y + 42, 30, 30});
    const float cx = dlg.x + dlg.w * 0.5f + 16.0f;
    const Font& fn = kit_.font(kFontNormal);
    ui.text(fn, cx, dlg.y + 16, deleteTitle_.c_str(), kGlueGold, Align::Center);
    ui.text(fn, cx, dlg.y + 34, deleteLine_.c_str(), kColWhite, Align::Center);
    glueWrapped(ui, kit_.font(kFontSmall), cx, dlg.y + 58, 230, GLUE_STR(CONFIRM_CHAR_DELETE_INSTRUCTIONS), kGlueGold, 13);
    // CharacterDeleteEditBox (opens the system keyboard).
    const Rect box{cx - 80, dlg.y + 84, 160, 24};
    const bool f = focus.addStop(kIdDelWord, box);
    if (kit_.has(kTexTipBg)) ui.tile(kit_.tex(kTexTipBg), Rect{box.x + 3, box.y + 3, box.w - 6, box.h - 6}, 0xD8171717u);
    else ui.fill(box, 0xD8171717u);
    if (kit_.has(kTexTipBorder)) ui.nineSlice(kit_.tex(kTexTipBorder), box, 1.0f, f ? kColWhite : 0xFFCCCCCCu);
    ui.text(fn, box.x + box.w * 0.5f, box.y + (box.h - fn.lineHeight()) * 0.5f, deleteWord_.c_str(), kColWhite,
            Align::Center);
    glueButton(ui, kit_, focus, kIdDelOk, Rect{cx - 112, dlg.y + dlg.h - 36, 106, 22}, GLUE_STR(OKAY), deleteConfirmed());
    glueButtonsBlue(nullptr);  // CharacterDeleteButton2 has no DeathKnightSwap
    glueButton(ui, kit_, focus, kIdDelCancel, Rect{cx + 6, dlg.y + dlg.h - 36, 106, 22}, GLUE_STR(CANCEL));
}

void CharacterScreen::drawList(UiRenderer& ui, FocusRing& focus) {
    // `focus`: focus_, or a throw-away ring when a dialog is over the list (a button under a
    // dialog must not take the focus nor act).
    const auto& chars = session_.characters();
    const bool ready = session_.state() == pp::WorldSession::State::Ready;
    const Font& fn = kit_.font(kFontNormal);
    const Font& fs = kit_.font(kFontSmall);
    const Font& ft = kit_.font(kFontTiny);

    // PSP: no glueLogo - the background art (bg_login) carries the logo already.

    // CharacterSelectCharacterFrame: Glue-Tooltip backdrop in DEFAULT_TOOLTIP_COLOR (border 0.8, back 0.09 at 0.85).
    if (kit_.has(kTexTipBg)) {
        ui.tile(kit_.tex(kTexTipBg), Rect{kPanel.x + 4, kPanel.y + 4, kPanel.w - 8, kPanel.h - 8}, 0xD8171717u);
    } else {
        ui.fill(kPanel, 0xD8171717u);
    }
    if (kit_.has(kTexTipBorder)) ui.nineSlice(kit_.tex(kTexTipBorder), kPanel, 1.0f, 0xFFCCCCCCu);
    const float pcx = kPanel.x + kPanel.w * 0.5f;
    // CharSelectRealmName (GlueFontDisableLarge), CharSelectChangeRealmButton (width = text + 50).
    ui.textMarquee(fn, Rect{kPanel.x + 8, kPanel.y + 5, kPanel.w - 16, 16}, realmName_.c_str(), kGlueDisable);
    const float crw = static_cast<float>(fs.measure(GLUE_STR(CHANGE_REALM))) + 30.0f;
    glueButton(ui, kit_, focus, kIdChangeRealm, Rect{pcx - crw * 0.5f, kPanel.y + 21, crw, 18}, GLUE_STR(CHANGE_REALM), true,
               kFontSmall);

    // CharSelectCharacterButton1..10: name (GlueFontNormal), CHARACTER_SELECT_INFO (GlueFontHighlightSmall),
    // the zone (GlueFontDisableSmall); the highlight (Glue-CharacterSelect-Highlight, ADD) on the
    // selected one, half as bright on the focused one.
    const int n = static_cast<int>(chars.size());
    const int rows = visibleRows();
    if (scroll_ > std::max(0, n - rows)) scroll_ = std::max(0, n - rows);
    for (int k = 0; k < rows && scroll_ + k < n; ++k) {
        const int i = scroll_ + k;
        const auto& c = chars[i];
        const Rect r{kPanel.x + 8, kListY + k * kRowH, kPanel.w - 24, kRowH - 1};
        const bool focused = focus.addStop(kIdChar0 + static_cast<uint32_t>(i), r);
        const bool sel = i == selected_;
        if ((sel || focused) && art_.csHl.pixels)
            ui.image(art_.csHl, Rect{r.x - 4, r.y - 1, r.w + 8, r.h + 2}, sel ? 0xFFFFFFFFu : 0x80FFFFFFu, Blend::Add);
        else if (sel || focused)
            ui.fill(r, sel ? 0x40209CFFu : 0x20209CFFu);
        const float tx = r.x + 6;
        ui.textShadow(fs, tx, r.y + 1, c.name.c_str(), sel || focused ? kColWhite : kGlueGold);
        // CHARACTER_SELECT_INFO[_GHOST] (level, class): ruRU reorders them ("%2$s %1$d-...").
        const std::string info =
            pp::csf((c.flags & kCharFlagGhost) ? pp::CS::GLUE_CHARACTER_SELECT_INFO_GHOST : pp::CS::GLUE_CHARACTER_SELECT_INFO,
                    static_cast<unsigned>(c.level), className(c.characterClass, isFemale(c)));
        ui.text(ft, tx, r.y + 13, info.c_str(), kColWhite);
        const char* zone = art_.zoneName(c.zoneId);
        // PSP: GlueFontDisableSmall grey with a shadow and lighter - plain 0.5 grey vanished on the
        // gold highlight of the PSP screen.
        ui.textShadow(ft, tx, r.y + 23, zone, 0xFFB4B4B4u);
    }
    // PSP: the list scrolls - the arrows of the ScrollFrame template, not focusable (the D-pad scrolls).
    if (n > rows) {
        const float ax = kPanel.x + kPanel.w - 18;
        const pp::Texture& up = scroll_ > 0 ? art_.scrUp : art_.scrUpDis;
        const pp::Texture& dn = scroll_ + rows < n ? art_.scrDn : art_.scrDnDis;
        if (up.pixels) ui.image(up, Rect{ax, kListY, 14, 14});
        if (dn.pixels) ui.image(dn, Rect{ax, kListBottom - 14, 14, 14});
    }
    // CharSelectCreateCharacterButton (BOTTOM 0,15, GlueButtonSmallTemplate): fewer than 10 characters.
    if (ready && n < 10) {
        const float cw = static_cast<float>(fs.measure(GLUE_STR(CREATE_NEW_CHARACTER))) + 40.0f;
        glueButton(ui, kit_, focus, kIdCreate, Rect{pcx - cw * 0.5f, kPanel.y + kPanel.h - 24, cw, 19},
                   GLUE_STR(CREATE_NEW_CHARACTER), true, kFontSmall);
    }
    // CharacterSelectDeleteButton + CharacterSelectBackButton (BOTTOMRIGHT -30,25).
    glueButton(ui, kit_, focus, kIdDelete, Rect{kPanel.x, 248, 112, 21}, GLUE_STR(DELETE_CHARACTER), ready && n > 0,
               kFontSmall);
    glueButton(ui, kit_, focus, kIdBack, Rect{kPanel.x + 116, 248, kPanel.w - 116, 21}, GLUE_STR(BACK), true, kFontSmall);

    // CharSelectCharacterName (GlueFontNormalHuge, BOTTOM 0,100), CharSelectEnterWorldButton (BOTTOM 0,30, 200 x 60),
    // CharacterSelectRotateLeft / Right under it (PSP: beside it).
    // 212 (was 202) - in the scene the feet stand near y 225, the name at their level
    // as CharSelectCharacterName (BOTTOM 0,100) is in the original.
    if (selected_ >= 0 && selected_ < n)
        ui.textShadow(kit_.font(kFontTitle), kModelCx, 212, chars[selected_].name.c_str(), kGlueGold, Align::Center);
    glueButton(ui, kit_, focus, kIdEnterWorld, Rect{kModelCx - 72, 234, 144, 28}, GLUE_STR(ENTER_WORLD),
               ready && selected_ >= 0);
    if (model_.loaded()) {
        const Rect rl{kModelCx - 102, 237, 22, 22}, rr{kModelCx + 80, 237, 22, 22};
        const bool fl = focus.addStop(kIdRotLeft, rl), fr = focus.addStop(kIdRotRight, rr);
        if (art_.rotL.pixels) ui.image(art_.rotL, rl, fl ? 0xFFFFFFFFu : 0xFFC0C0C0u);
        if (art_.rotR.pixels) ui.image(art_.rotR, rr, fr ? 0xFFFFFFFFu : 0xFFC0C0C0u);
    }
}

void CharacterScreen::draw(UiRenderer& ui, const FrameStats& /*stats*/) {
    ui.beginFrame();
    // SetBackgroundModel - the selected character's scene (GetSelectBackgroundModel), on
    // the creation screen the scene of the race being made (GetCreateBackgroundModel); the login art without
    // glue.pak's pictures. Loaded when it changes, one at a time, freed with the glue screens (releaseForWorld).
    const auto& chars = session_.characters();
    const bool haveSel = selected_ >= 0 && selected_ < static_cast<int>(chars.size());
    if (mode_ == Mode::Create) scene_.show(glueSceneFor(race_, class_));
    else if (haveSel) scene_.show(glueSceneFor(chars[selected_].race, chars[selected_].characterClass));
    else scene_.show("characterselect");  // PSP: no character - UI_CharacterSelect
    const float sceneShift = mode_ == Mode::Create ? ccCenterShift() : 0.0f;  // the model in the middle
    if (!scene_.draw(ui, sceneShift)) kit_.background(ui, mode_ != Mode::List);
    if (pp::oskActive()) {
        // The system keyboard draws over this frame: give it a plain, dimmed
        // backdrop (as Sony's and steam-psp's samples do), not our dialog.
        ui.fill(Rect{0, 0, static_cast<float>(pp::kScreenW), static_cast<float>(pp::kScreenH)}, 0x80000000u);
        ui.endFrame();
        return;
    }
    art_.load();
    if (mode_ == Mode::Create) ccPreview();  // the look being made
    if ((mode_ == Mode::List || mode_ == Mode::Create) && model_.loaded()) {
        ui.endFrame();
        if (const GlueScene* sc = scene_.scene()) model_.drawInScene(*sc, sceneShift);
        else model_.draw(mode_ == Mode::Create ? 240.0f : kModelCx, kModelCy, kModelH);
        ui.beginFrame();
    }
    inert_.setFocus(~0u);  // nothing in it is ever focused
    inert_.update(pp::PadState{}, 0.0f);
    FocusRing& top = dialog_.shown() ? inert_ : focus_;  // the topmost window takes the focus
    // CharacterSelect_DeathKnightSwap - blue buttons while a death knight is selected.
    glueButtonsBlue(mode_ != Mode::Create && haveSel && chars[selected_].characterClass == game::Class::DEATH_KNIGHT
                        ? &art_
                        : nullptr);
    if (mode_ == Mode::List || mode_ == Mode::Delete) drawList(ui, mode_ == Mode::List ? top : inert_);
    if (mode_ == Mode::Delete) drawDelete(ui, top);
    else if (mode_ == Mode::Create) drawCreate(ui, top);
    glueButtonsBlue(nullptr);
    dialog_.draw(ui, kit_, focus_, &art_);
    kit_.cursor(ui, focus_);
    ui.endFrame();
}

} // namespace wowee::ui::psp
