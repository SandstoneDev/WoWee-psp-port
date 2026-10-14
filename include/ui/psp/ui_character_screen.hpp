// Milestone 4: character list and a minimal character creator, on top of
// the PSP WorldSession (upstream packet builders/parsers).
//
// The list is CharacterSelect.xml / .lua of the original (audit "Login, realms, characters"
// #1-#4, #8, #10): the expansion logo; on the right CharacterSelectCharacterFrame (glue tooltip
// backdrop) with the realm name, CHANGE_REALM (CHANGE_REALM) and up to 10 characters (name,
// CHARACTER_SELECT_INFO "Level N Class" [ghost], the zone) - PSP: 5 rows on screen, the list
// scrolls with the D-pad (the 1024 x 768 frame shows all 10); CREATE_NEW_CHARACTER under them;
// DELETE_CHARACTER and BACK bottom right; the selected character's name, its model with the
// rotate buttons, and ENTER_WORLD (ENTER_WORLD) on the left. Cross on a character selects
// it (CharacterSelectButton_OnClick), Cross on the selected one enters the world (OnDoubleClick),
// START = Escape (CharacterSelect_Exit: back to the login screen), L / R turn the model.
//
// Creation is controller-only: race, class and gender cycle on X, the name comes from a syllable
// generator or the system keyboard. The server validates the name; its answer is shown in a
// GlueDialog with the client's CHAR_CREATE_* / CHAR_NAME_* text.
//
// Deleting a character follows the original CharacterSelect: DELETE_CHARACTER
// opens CharacterDeleteDialog - the character named, "type DELETE_CONFIRM_STRING to confirm" (GlueStrings
// ruRU), OK only once the word matches (strupper compare, Cyrillic folded too); the server's answer
// (CHAR_DELETE_*) comes up as a dialog and the list is asked for again.
#pragma once

#include "game/character.hpp"
#include "platform/psp/psp_input.hpp"
#include "platform/psp/psp_world_session.hpp"
#include "ui/psp/ui_focus.hpp"
#include "ui/psp/ui_glue.hpp"
#include "ui/psp/ui_glue_model.hpp"
#include "ui/psp/ui_glue_scene.hpp"
#include "ui/psp/ui_character_create.hpp"
#include "ui/psp/ui_kit.hpp"
#include "ui/psp/ui_renderer.hpp"

#include <cstdint>
#include <string>

namespace wowee::ui::psp {

struct FrameStats;

class CharacterScreen {
public:
    // autoCreate: emulator smoke test - create one character if the list is empty.
    // realmName: CharSelectRealmName ("Name (PvP)").
    CharacterScreen(const UiKit& kit, platform::psp::WorldSession& session, std::string realmName,
                    bool autoCreate = false, bool autoOsk = false);

    void update(const platform::psp::PadState& pad, float dtSec);
    void draw(UiRenderer& ui, const FrameStats& stats);
    bool wantsExit() const { return wantsExit_; }
    // BACK / START (CharacterSelect_Exit) and CHANGE_REALM (CharacterSelect_ChangeRealm),
    // taken by the login screen.
    bool wantsBack() const { return wantsBack_; }
    bool wantsRealmList() const { return wantsRealmList_; }
    // ENTER_WORLD was pressed: CMSG_PLAYER_LOGIN sent; the map of the character
    // (SMSG_CHAR_ENUM) for the loading screen.
    bool enteringWorld() const { return entering_; }
    uint32_t enteringMapId() const { return enteringMap_; }
    // The model and the glue art go before the world loads (PSP heap).
    void releaseForWorld();
    const std::string& selectedName() const { return selectedName_; }

private:
    enum class Mode { List, Create, Delete };

    void drawList(UiRenderer& ui, FocusRing& focus);
    void drawCreate(UiRenderer& ui, FocusRing& focus);  // CharacterCreate (ui_character_create.cpp)
    // Character creation: CharacterCreate as the original (ui_character_create.cpp).
    void enterCreate();
    void leaveCreate();
    void ccClamp();
    void ccRandomize();
    void ccPreview();
    bool updateCreate(uint32_t act, const platform::psp::PadState& pad, float dtSec);
    static float ccCenterShift();
    void drawDelete(UiRenderer& ui, FocusRing& focus);
    void openDelete(const game::Character& c);
    bool deleteConfirmed() const;
    void rerollName();
    void nextRace();
    void nextClass();
    void selectCharacter(int index);
    void focusCharacter(int index);
    void enterWorld();
    int visibleRows() const;

    const UiKit& kit_;
    platform::psp::WorldSession& session_;
    std::string realmName_;
    FocusRing focus_;
    FocusRing inert_;  // The list drawn under a dialog declares its stops here
    Mode mode_ = Mode::List;
    bool wantsExit_ = false;
    bool wantsBack_ = false;
    bool wantsRealmList_ = false;
    bool listFocused_ = false;
    int lastCreateSeen_ = -1;
    bool waitRelease_ = false;  // swallow the X that closed the keyboard
    bool autoCreate_ = false;
    uint32_t autoReadyMs_ = 0;  // wowee.cfg autoworlddelay - when the list was first ready (autotest.txt)
    bool autoOsk_ = false;

    game::Race race_ = game::Race::HUMAN;
    game::Class class_ = game::Class::WARRIOR;
    game::Gender gender_ = game::Gender::MALE;
    std::string name_;
    uint32_t nameSeed_ = 0;
    std::string selectedName_;

    // CharacterSelect.selectedIndex (0-based here, -1 none), the first row shown, the model.
    int selected_ = -1;
    int scroll_ = 0;
    bool selectLast_ = false;  // SELECT_LAST_CHARACTER after a create
    bool entering_ = false;
    uint32_t enteringMap_ = 0;
    GlueArt art_;
    GlueCharacterModel model_;
    GlueBackground scene_;  // SetBackgroundModel - the race's scene behind the model
    CharCreateArt ccArt_;   // the creation screen's art and data (glue.pak cc_*), while it is open
    uint8_t look_[5] = {};  // skin, face, hair style, hair colour, facial hair (CharacterCustomization 1..5)
    bool ccRaceInfo_ = true;  // the description shows the race (else the class)
    float ccFacing_ = 0.0f;
    uint32_t ccInfoKey_ = 0, ccInfoT0_ = 0;  // the description shown and since when (its auto scroll)
    GlueDialog dialog_;
    enum class Status { None, List, Create, Delete } status_ = Status::None;  // a "CANCEL" status dialog

    // Character deletion (CharacterDeleteDialog).
    uint64_t deleteGuid_ = 0;
    std::string deleteTitle_;  // CONFIRM_CHAR_DELETE's first line
    std::string deleteLine_;   // its second: the character (name, level, class)
    std::string deleteWord_;  // what the player typed
    bool oskForDelete_ = false;
    uint32_t deleteSeqSeen_ = 0;

    uint32_t nowMs_ = 0;
    float dt_ = 0.0f;
};

// The client's names in the running language (strings.pak DBC_*; upstream's getRaceName/getClassName are English).
const char* raceName(game::Race race, bool female = false);
// The 3.3.5a race / class combinations the creation screen offers (the death knight left out).
bool raceClassAllowed(game::Race race, game::Class cls);
const char* mapName(uint32_t mapId);

} // namespace wowee::ui::psp
