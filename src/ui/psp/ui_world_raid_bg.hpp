// The state and art of the raid / battleground frames (ui_world_raid.cpp,
// ui_world_battleground.cpp, ui_world_worldstate.cpp) - WorldScreen::RaidBgUi, made on first use
// (WorldScreen::raidBg). Each frame's art is loaded from ui.pak while it is shown and freed after (the pak is
// opened and closed at once); the data (bg_data, ws_ui) is small text kept once read.
// Internal to those three files.
#pragma once

#include "ui/psp/ui_world_screen.hpp"

#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_texture.hpp"

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace wowee::ui::psp {

// Widget ids 0x7780-0x78FF.
enum : uint32_t {
    // FriendsFrame (Raid tab)
    kIdRfClose = 0x7780,
    kIdRfConvert,       // RaidFrameConvertToRaidButton
    kIdRfInfo,          // RaidFrameRaidInfoButton
    kIdRfReadyCheck,    // RaidFrameReadyCheckButton
    kIdRfBrowser,       // RaidFrameRaidBrowserButton (in a raid)
    kIdRfBrowser2,      // RaidFrameNotInRaidRaidBrowserButton
    kIdRfMenu,          // GameMenuFrame SOCIAL_BUTTON (ToggleFriendsFrame; the FriendsFrame tab takes no focus)
    kIdRfLabel0 = 0x7788,  // RaidGroup1..8Label (a pullout of the group)
    kIdRfClass0 = 0x7790,  // RaidClassButton1..13
    kIdRfSlot0 = 0x77A0,   // RaidGroupNSlotM: (group - 1) * 5 + slot - 1 .. 0x77C7
    kIdRfMember0 = 0x77D0, // RaidGroupButton1..40 .. 0x77F7
    kIdRcYes = 0x77F8,     // ReadyCheckFrameYesButton
    kIdRcNo,
    // RaidPullout frames
    kIdPoTitle0 = 0x7800,   // a pullout's frame (its menu) .. 0x7807
    kIdPoButton0 = 0x7808,  // its buttons .. 0x783F
    kIdPoMenu0 = 0x7840,    // RaidPulloutDropDown rows .. 0x784F
    // BattlefieldFrame
    kIdBfClose = 0x7860,
    kIdBfJoin,
    kIdBfGroupJoin,
    kIdBfCancel,
    kIdBfUp,
    kIdBfDown,
    kIdBfZone0 = 0x7868,    // BattlefieldZone1..5 .. 0x786C
    // CONFIRM_BATTLEFIELD_ENTRY (slot * 4)
    kIdBcEnter0 = 0x7870,
    kIdBcLeave0,
    kIdBcClose0,
    // MiniMapBattlefieldFrame + MiniMapBattlefieldDropDown
    kIdMmBattlefield = 0x7880,
    kIdMmBgMenu0 = 0x7888,  // .. 0x788F
    // WorldStateScoreFrame
    kIdScClose = 0x7890,
    kIdScLeave,
    kIdScTab0,              // WorldStateScoreFrameTab1..3 .. 0x7894
    kIdScUp = 0x7895,
    kIdScDown,
    kIdScColumn0 = 0x78A0,  // the column headers (sort) .. 0x78AF
    kIdScRow0 = 0x78B0,     // WorldStateScoreButton1..20 .. 0x78C3
    // WorldStateAlwaysUpFrame
    kIdWsLine0 = 0x78E0,    // AlwaysUpFrameN (its tooltip) .. 0x78EF
    kIdWsDyn0 = 0x78F0,     // AlwaysUpFrameNDynamicIconButton .. 0x78FF
};

// The UnitPopup menus the raid code opens (ui_world_party.cpp MenuKind: SELF 1, PARTY 2, PLAYER 3 before them).
enum : uint8_t { kUnitMenuRaid = 4, kUnitMenuRaidPlayer = 5, kUnitMenuTarget = 6 };

// Loads one ui.pak texture (the blob stays the texture's); adds its size to `bytes`.
inline bool raidBgLoad(platform::psp::Pak& pak, const char* name, platform::psp::Texture& t, uint32_t& bytes) {
    const platform::psp::PakEntry* e = pak.find(name);
    void* blob = e ? pak.load(e) : nullptr;
    if (!blob) return false;
    if (!platform::psp::textureFromBlob(blob, e->size, t, true)) {
        platform::psp::pakFree(blob);
        return false;
    }
    t.name = "raidbg";
    bytes += e->size;
    return true;
}

inline void raidBgFree(std::initializer_list<platform::psp::Texture*> list) {
    for (platform::psp::Texture* t : list)
        if (t->pixels) platform::psp::textureRelease(*t);
}

struct WorldScreen::RaidBgUi {
    // --- Art (ui.pak; all optional) ---
    platform::psp::Texture rfFrame, rfGroup, rfBtn, rfBtnHl, rfTab, rfClasses, rfPets, rfMtBtn, rfMaBtn;
    bool rfTried = false;
    uint32_t rfBytes = 0;
    platform::psp::Texture leader, master, assist, mt, ma, hpFrame, rc[3], rtIcons;  // roles, bar frame, ready, marks
    bool iconsTried = false;
    uint32_t iconsBytes = 0;
    platform::psp::Texture rcFrame;
    bool rcTried = false;
    platform::psp::Texture bfFrame, bfHl, bfCur[2], bfArena, bfConfirm;
    bool bfTried = false;
    uint32_t bfBytes = 0;
    platform::psp::Texture mmIcon[3], capBar, wsIcons;  // minimap icon (Alliance, Horde, arena), capture bar, icons
    bool wsTried = false;
    uint32_t wsBytes = 0;
    platform::psp::Texture score, scoreBg, scoreHl, classes;
    bool scTried = false;
    uint32_t scBytes = 0;
    ~RaidBgUi();

    // --- Data (ui.pak raw bg_data / ws_ui) ---
    bool dataTried = false;
    struct Bg {
        uint32_t id = 0, type = 0, maxGroup = 0, minLevel = 0, maxLevel = 0, holidayWs = 0;
        std::vector<uint32_t> maps;
        std::string name;
    };
    std::vector<Bg> bgs;
    struct Map {
        uint32_t id = 0, area = 0;
        std::string name, desc[2];  // MapDescription0 (Alliance) / 1 (Horde)
    };
    std::vector<Map> maps;
    std::string className[12][2], raceName[12][2];  // [id][0 male, 1 female] (ChrClasses / ChrRaces ruRU)
    int8_t raceFaction[12] = {};                       // ChrRaces: 0 Alliance, 1 Horde
    struct Ws {
        uint32_t id = 0, map = 0, area = 0, phase = 0, type = 0, var = 0;
        int16_t icon = -1, dynIcon = -1, dynFlash = -1, rowIcon[2] = {-1, -1};
        bool hasIcon = false, capture = false;
        uint32_t ev[3] = {};
        std::string text, tip, dynTip;
    };
    std::vector<Ws> ws;         // this map's records (ws_ui is read again for another map)
    uint32_t wsMap = ~0u;
    bool bgDataTried = false;
    const Bg* bg(uint32_t id) const {
        for (const Bg& b : bgs)
            if (b.id == id) return &b;
        return nullptr;
    }
    const Map* map(uint32_t id) const {
        for (const Map& m : maps)
            if (m.id == id) return &m;
        return nullptr;
    }

    // --- RaidFrame ---
    int moving = -1;            // MOVING_RAID_MEMBER: the roster index picked up (PSP: Cross picks, Cross drops)
    uint64_t movingGuid = 0;
    std::vector<uint64_t> rows; // RaidGroupButton1..40: the roster's guid per button the last frame drew
    struct Slot {
        uint64_t guid = 0;      // the member in RaidGroupNSlotM, 0 = empty
    };
    Slot slots[8][5];
    uint32_t tipId = 0;         // the focused widget with a tooltip (drawn after the windows)
    Rect tipRect{};
    std::string tipTitle, tipLine, tipLine2;
    uint32_t tipLineColor = 0, tipLine2Color = 0;
    // RaidPullout frames (RAID_PULLOUT_POSITIONS: by group number, class token, MAINTANK / MAINASSIST / PETS, or a
    // player's name - this session only).
    struct Pullout {
        uint8_t kind = 0;       // 1 group, 2 class, 3 main tank, 4 main assist, 5 pets, 6 one player
        uint32_t id = 0;        // the group (1..8) or the class id
        std::string name;       // kind 6: the player
        bool showTarget = false, showTargetTarget = false, showBuffs = false, showBg = true;
        float x = 0.0f, y = 0.0f;  // top-left (px)
        std::vector<uint64_t> members;  // the last frame's buttons
    };
    std::vector<Pullout> pullouts;
    Rect poRect[8]{};           // the pullout frames on the screen (last frame; the dropdown's anchor)
    int pulloutMenu = -1;       // the pullout whose RaidPulloutDropDown is open
    std::vector<uint8_t> pulloutMenuRows;
    uint8_t dropKind = 0;       // kWinDropDown: 1 MiniMapBattlefieldDropDown, 2 RaidPulloutDropDown
    uint32_t dropFocusMs = 0;   // UIDROPDOWNMENU_SHOW_TIME
    // ReadyCheckFrame
    uint32_t rcSeq = 0, rcFinishSeq = 0;
    bool rcShown = false;       // ReadyCheckListenerFrame up (not answered)
    uint64_t rcInitiator = 0;
    std::string rcName;
    uint32_t rcVersion = 0;

    // --- BattlefieldFrame ---
    uint32_t listSeq = 0, statusSeq = 0;
    int selected = 0;           // GetSelectedBattlefield: 0 first available, n the n-th instance
    int offset = 0;             // FauxScrollFrame offset
    bool bfShown = false;
    uint32_t bfTipId = 0;
    // CONFIRM_BATTLEFIELD_ENTRY (multiple = 1: one per queue slot)
    bool confirmShown[2] = {};
    uint32_t confirmSeq[2] = {};
    // MiniMapBattlefieldFrame
    bool mmShown = false;
    uint32_t shineMs = 0;
    uint32_t prevQueued[2] = {};  // PREVIOUS_BATTLEFIELD_QUEUES (the queues' bg type, for the queue sound)
    uint32_t prevStatus[2] = {};      // the queues' last status and type said in the chat (the status lines)
    uint32_t prevStatusType[2] = {};
    std::vector<uint8_t> menuRows;  // MiniMapBattlefieldDropDown: (slot << 4) | action, 0xFF = title / spacer
    std::vector<std::string> menuText;
    // WorldStateScoreFrame
    int scoreTab = 0;           // 0 all, 1 Alliance, 2 Horde (SetBattlefieldScoreFaction)
    int scoreOffset = 0;
    int sortKey = -1;           // SortBattlefieldScoreData: the column, -1 the server's order
    bool sortDesc = false;
    uint32_t scoreAskMs = 0, scoreVersion = 0;
    bool winnerOpened = false;  // shown once for the winner (GetBattlefieldWinner)
    // WorldStateAlwaysUpFrame / WorldStateCaptureBar
    bool lastUi = false;        // the interface mode last frame (the cursor coming up over our popups)
    bool wsVisible = false;     // a line or a bar was up last frame (the art stays while it is)
    float capOld[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
    int capDir[4] = {};
    uint32_t logs = 0;
};

}  // namespace wowee::ui::psp
