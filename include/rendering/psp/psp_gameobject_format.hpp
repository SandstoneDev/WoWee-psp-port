// psp_gameobject_format.hpp - game object facts and the go_lock / go_snd / go_barber / goanim record layouts.
//
// Clean-room implementation of specs/world/gameobject_rules.md (v2). Header only, <cstdint> only, no platform
// headers, no STL containers. Used by the EBOOT (world session, world screen) and by the PC tools.
#pragma once

#include <cstdint>

namespace wowee {
namespace rendering {
namespace psp {

// ---------------------------------------------------------------------------------------------------------------
// 2.1 Object types (server game object type numbers)
// ---------------------------------------------------------------------------------------------------------------
enum GoType : uint8_t {
    kGoDoor = 0,
    kGoButton = 1,
    kGoQuestGiver = 2,
    kGoChest = 3,
    kGoBinder = 4,
    kGoGeneric = 5,
    kGoTrap = 6,
    kGoChair = 7,
    kGoSpellFocus = 8,
    kGoText = 9,
    kGoGoober = 10,
    kGoTransport = 11,
    kGoAreaDamage = 12,
    kGoCamera = 13,
    kGoMapObject = 14,
    kGoMoTransport = 15,
    kGoDuelFlag = 16,
    kGoFishingNode = 17,
    kGoRitual = 18,
    kGoMailbox = 19,
    kGoAuctionHouse = 20,
    kGoGuardPost = 21,
    kGoSpellCaster = 22,
    kGoMeetingStone = 23,
    kGoFlagStand = 24,
    kGoFishingHole = 25,
    kGoFlagDrop = 26,
    kGoMiniGame = 27,
    kGoLotteryKiosk = 28,
    kGoCapturePoint = 29,
    kGoAuraGenerator = 30,
    kGoDungeonDifficulty = 31,
    kGoBarberChair = 32,
    kGoDestructible = 33,
    kGoGuildBank = 34,
    kGoTrapDoor = 35,
};

constexpr uint8_t kGoTypeCount = 36;

// ---------------------------------------------------------------------------------------------------------------
// 2.2 Properties (values are this implementation's choice: distinct, non-zero, fit uint8_t)
// ---------------------------------------------------------------------------------------------------------------
enum GoProp : uint8_t {
    kPropStartOpen = 1,
    kPropAutoClose = 2,
    kPropLock = 3,
    kPropSpell = 4,
    kPropChairSlots = 5,
    kPropChairHeight = 6,
    kPropPageId = 7,
    kPropLanguage = 8,
    kPropPageMaterial = 9,
    kPropHighlight = 10,
    kPropFloatingTooltip = 11,
    kPropQuestId = 12,
    kPropCustomAnim = 13,
    kPropMinLevel = 14,
    kPropMaxLevel = 15,
    kPropNotInCombat = 16,
    kPropAllowMounted = 17,
    kPropCreatorOnly = 18,
};

// ---------------------------------------------------------------------------------------------------------------
// 2.3 Constants
// ---------------------------------------------------------------------------------------------------------------
constexpr uint32_t kGoFlagInUse = 0x1;
constexpr uint32_t kGoFlagLocked = 0x2;
constexpr uint32_t kGoFlagInteractCond = 0x4;
constexpr uint32_t kGoFlagNotSelectable = 0x10;
constexpr uint32_t kGoDynActivate = 0x1;
constexpr uint32_t kGoDynNoInteract = 0x4;
constexpr uint32_t kGoDynSparkle = 0x8;
constexpr uint8_t kGoStateActive = 0;
constexpr uint8_t kGoStateReady = 1;
constexpr uint8_t kGoStateActiveAlt = 2;
constexpr uint32_t kGoAnimDisplayBase = 0xF00000;

enum GoAnimState : uint8_t {
    kGoAnimSpawn = 0,
    kGoAnimClosed = 1,
    kGoAnimOpen = 2,
    kGoAnimOpened = 3,
    kGoAnimClose = 4,
    kGoAnimDestroy = 5,
    kGoAnimDestroyed = 6,
    kGoAnimRebuild = 7,
    kGoAnimCustom0 = 8,  // Custom1..3 = 9..11
    kGoAnimDespawn = 12,
    kGoAnimStateCount = 13,
};

enum GoClickRule : uint8_t { kClickNever = 0, kClickGeneric = 1, kClickChair = 2, kClickFishing = 3 };

struct GoAnimPick {
    uint16_t anim = 0;
    float speed = 1.0f;
};

// ---------------------------------------------------------------------------------------------------------------
// 3.1 data[] index of a property
// ---------------------------------------------------------------------------------------------------------------
inline int goDataIndex(uint8_t type, uint8_t prop) {
    switch (type) {
    case kGoDoor:
    case kGoButton:
        switch (prop) {
        case kPropStartOpen: return 0;
        case kPropLock: return 1;
        case kPropAutoClose: return 2;
        default: return -1;
        }
    case kGoQuestGiver:
        switch (prop) {
        case kPropLock: return 0;
        case kPropPageMaterial: return 2;
        case kPropCustomAnim: return 4;
        case kPropAllowMounted: return 8;
        default: return -1;
        }
    case kGoChest:
        switch (prop) {
        case kPropLock: return 0;
        case kPropQuestId: return 8;
        case kPropNotInCombat: return 12;
        case kPropFloatingTooltip: return 16;
        default: return -1;
        }
    case kGoGeneric:
        switch (prop) {
        case kPropFloatingTooltip: return 0;
        case kPropHighlight: return 1;
        case kPropQuestId: return 5;
        default: return -1;
        }
    case kGoTrap:
        switch (prop) {
        case kPropLock: return 0;
        case kPropSpell: return 3;
        case kPropAutoClose: return 6;
        default: return -1;
        }
    case kGoChair:
        switch (prop) {
        case kPropChairSlots: return 0;
        case kPropChairHeight: return 1;
        case kPropCreatorOnly: return 2;
        default: return -1;
        }
    case kGoSpellFocus:
        switch (prop) {
        case kPropQuestId: return 4;
        case kPropFloatingTooltip: return 6;
        default: return -1;
        }
    case kGoText:
        switch (prop) {
        case kPropPageId: return 0;
        case kPropLanguage: return 1;
        case kPropPageMaterial: return 2;
        case kPropAllowMounted: return 3;
        default: return -1;
        }
    case kGoGoober:
        switch (prop) {
        case kPropLock: return 0;
        case kPropQuestId: return 1;
        case kPropAutoClose: return 3;
        case kPropCustomAnim: return 4;
        case kPropPageId: return 7;
        case kPropLanguage: return 8;
        case kPropPageMaterial: return 9;
        case kPropSpell: return 10;
        case kPropAllowMounted: return 17;
        case kPropFloatingTooltip: return 18;
        default: return -1;
        }
    case kGoTransport:
        switch (prop) {
        case kPropStartOpen: return 1;
        case kPropAutoClose: return 2;
        default: return -1;
        }
    case kGoAreaDamage:
        switch (prop) {
        case kPropLock: return 0;
        case kPropAutoClose: return 5;
        default: return -1;
        }
    case kGoCamera:
    case kGoFlagStand:
    case kGoFlagDrop:
        return prop == kPropLock ? 0 : -1;
    case kGoRitual:
        return prop == kPropSpell ? 1 : -1;
    case kGoSpellCaster:
        switch (prop) {
        case kPropSpell: return 0;
        case kPropAllowMounted: return 3;
        default: return -1;
        }
    case kGoMeetingStone:
        switch (prop) {
        case kPropMinLevel: return 0;
        case kPropMaxLevel: return 1;
        default: return -1;
        }
    case kGoFishingHole:
        return prop == kPropLock ? 4 : -1;
    case kGoCapturePoint:
        switch (prop) {
        case kPropSpell: return 1;
        case kPropHighlight: return 19;
        default: return -1;
        }
    case kGoAuraGenerator:
        return prop == kPropStartOpen ? 0 : -1;
    case kGoBarberChair:
        return prop == kPropChairHeight ? 0 : -1;
    case kGoTrapDoor:
        switch (prop) {
        case kPropStartOpen: return 1;
        case kPropAutoClose: return 2;
        default: return -1;
        }
    default:
        // Binder, MapObject, MoTransport, DuelFlag, FishingNode, Mailbox, AuctionHouse, GuardPost, MiniGame,
        // LotteryKiosk, DungeonDifficulty, Destructible, GuildBank: no properties; >= 36: out of range.
        return -1;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// 3.2 interaction reach (yards)
// ---------------------------------------------------------------------------------------------------------------
inline float goUseDistance(uint8_t type) {
    switch (type) {
    case kGoQuestGiver:
    case kGoText:
    case kGoFlagStand:
    case kGoFlagDrop:
    case kGoMiniGame:
        return 50.0f / 9.0f;  // 5.5555553f, the single-precision value nearest 50/9
    case kGoChair:
    case kGoBarberChair:
        return 3.0f;
    case kGoFishingNode:
        return 100.0f;
    case kGoAreaDamage:
        return 0.0f;
    default:
        return 5.0f;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// 3.3 "locked" UI error id
// ---------------------------------------------------------------------------------------------------------------
inline uint16_t goLockedError(uint8_t type) {
    switch (type) {
    case kGoDoor: return 0xEB;
    case kGoButton: return 0xEC;
    default: return 0xEA;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// 3.4 eligibility rule class
// ---------------------------------------------------------------------------------------------------------------
inline GoClickRule goClickRule(uint8_t type) {
    switch (type) {
    case kGoDoor:
    case kGoButton:
    case kGoQuestGiver:
    case kGoChest:
    case kGoBinder:
    case kGoTrap:
    case kGoText:
    case kGoGoober:
    case kGoAreaDamage:
    case kGoCamera:
    case kGoRitual:
    case kGoMailbox:
    case kGoSpellCaster:
    case kGoMeetingStone:
    case kGoFlagStand:
    case kGoFlagDrop:
    case kGoMiniGame:
    case kGoGuildBank:
        return kClickGeneric;
    case kGoChair:
    case kGoBarberChair:
        return kClickChair;
    case kGoFishingNode:
        return kClickFishing;
    default:
        return kClickNever;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// 3.5 does a Lock.dbc case with this Action apply to the object's state
// ---------------------------------------------------------------------------------------------------------------
inline bool goLockActionApplies(uint32_t action, uint8_t state, bool lockedFlag) {
    const uint8_t s = state > 2 ? 3 : state;  // states other than 0, 1, 2 behave like state 3
    switch (action) {
    case 0: return s == 1 && !lockedFlag;
    case 1: return s == 1 && lockedFlag;
    case 2: return s == 0;
    case 3: return s == 1;
    case 4: return s == 2;
    default: return s != 2;  // action >= 5
    }
}

// ---------------------------------------------------------------------------------------------------------------
// 3.6 animation id of an animation state
// ---------------------------------------------------------------------------------------------------------------
inline uint16_t goAnimIdOf(uint8_t animState) {
    switch (animState) {
    case kGoAnimSpawn: return 145;
    case kGoAnimClosed: return 147;
    case kGoAnimOpen: return 148;
    case kGoAnimOpened: return 149;
    case kGoAnimClose: return 146;
    case kGoAnimDestroy: return 150;
    case kGoAnimDestroyed: return 151;
    case kGoAnimRebuild: return 152;
    case kGoAnimCustom0: return 153;
    case kGoAnimCustom0 + 1: return 154;
    case kGoAnimCustom0 + 2: return 155;
    case kGoAnimCustom0 + 3: return 156;
    case kGoAnimDespawn: return 157;
    default: return 0;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// 3.7 animation state entered on an object state change
// ---------------------------------------------------------------------------------------------------------------
inline uint8_t goAnimStateOnChange(uint8_t oldState, uint8_t newState, bool synced) {
    switch (newState) {
    case kGoStateActive:
        return (oldState == kGoStateReady || synced) ? kGoAnimOpen : kGoAnimOpened;
    case kGoStateReady:
        if (synced) return kGoAnimClose;
        if (oldState == kGoStateActive) return kGoAnimClose;
        if (oldState == kGoStateActiveAlt) return kGoAnimRebuild;
        return kGoAnimClosed;
    case kGoStateActiveAlt:
        return (oldState == kGoStateReady || synced) ? kGoAnimDestroy : kGoAnimDestroyed;
    default:
        return kGoAnimClosed;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// 3.8 state after a one-shot ends, and looping states
// ---------------------------------------------------------------------------------------------------------------
inline uint8_t goAnimStateAfter(uint8_t animState) {
    switch (animState) {
    case kGoAnimSpawn: return 0xFF;
    case kGoAnimClosed: return kGoAnimClosed;
    case kGoAnimOpen: return kGoAnimOpened;
    case kGoAnimOpened: return kGoAnimOpened;
    case kGoAnimClose: return kGoAnimClosed;
    case kGoAnimDestroy: return kGoAnimDestroyed;
    case kGoAnimDestroyed: return kGoAnimDestroyed;
    case kGoAnimRebuild: return kGoAnimClosed;
    case kGoAnimCustom0:
    case kGoAnimCustom0 + 1:
    case kGoAnimCustom0 + 2:
    case kGoAnimCustom0 + 3:
        return 0xFF;
    case kGoAnimDespawn: return kGoAnimDespawn;
    default: return animState;  // >= 13: the same value
    }
}

inline bool goAnimLoops(uint8_t animState) {
    return animState == kGoAnimClosed || animState == kGoAnimOpened || animState == kGoAnimDestroyed;
}

// ---------------------------------------------------------------------------------------------------------------
// 3.9 substitute animation when the model lacks the state's animation
// ---------------------------------------------------------------------------------------------------------------
template <typename Has>
inline GoAnimPick goPickAnim(uint8_t animState, Has has) {
    constexpr uint16_t kStand = 0, kClose = 146, kClosed = 147, kOpen = 148, kOpened = 149, kDestroy = 150,
                       kDestroyed = 151;
    GoAnimPick r;
    const uint16_t a = goAnimIdOf(animState);
    if (has(a)) {
        r.anim = a;
        r.speed = 1.0f;
        return r;
    }
    uint16_t c = a;
    float speed = 1.0f;
    switch (a) {
    case kClose:
        c = has(kOpen) ? kClose : kClosed;
        break;
    case kClosed:
        if (has(kClose)) {
            c = kClosed;
        } else if (has(kOpen)) {
            c = kOpen;
            speed = 0.0f;
        } else {
            c = kStand;
        }
        break;
    case kOpen:
        if (has(kClose)) c = kOpen;
        else c = has(kDestroy) ? kDestroy : kOpened;
        break;
    case kOpened:
        if (has(kOpen)) {
            c = kOpened;
        } else if (has(kClose)) {
            c = kClose;
            speed = 0.0f;
        } else {
            c = kDestroyed;
        }
        break;
    default:
        c = a;
        break;
    }
    r.anim = has(c) ? c : kStand;
    r.speed = speed;
    return r;
}

// ---------------------------------------------------------------------------------------------------------------
// 2.4 Record layouts (little-endian, natural alignment)
// ---------------------------------------------------------------------------------------------------------------
struct GoLockHeader {
    char magic[4];  // "WGLK"
    uint16_t version;  // 1
    uint16_t lockTypes;
    uint16_t locks;
    uint16_t cases;
    uint16_t spells;
    uint16_t reserved;
    uint32_t poolSize;
};

struct GoLockTypeRec {
    uint16_t id;  // LockType.dbc ID
    uint16_t reserved;
    uint32_t name;  // pool offsets
    uint32_t resource;
    uint32_t verb;
    uint32_t cursor;
};

struct GoLockRec {
    uint16_t id;  // Lock.dbc ID
    uint8_t count;  // cases with Type != 0
    uint8_t reserved;
    uint16_t first;  // index of its first case
    uint16_t reserved1;
};

struct GoLockCase {
    uint8_t type;  // 1 item, 2 lock type, 3 spell
    uint8_t slot;  // 0..7 in the row
    uint16_t skill;
    uint32_t index;
    uint32_t action;
};

struct GoLockSpellRec {
    uint32_t spell;  // spell with the open-lock effect (33)
    uint8_t effect;  // its effect slot
    uint8_t lockType;  // that effect's MiscValue
    uint16_t skillLine;  // 0 = none
    int32_t basePoints;
    int32_t dieSides;
};

struct GoSoundHeader {
    char magic[4];  // "WGSN"
    uint32_t count;
};

struct GoSoundRec {
    uint32_t display;  // GameObjectDisplayInfo ID
    uint16_t sound[10];  // 0 stand, 1 open, 2 loop, 3 close, 4 destroy, 5 opened, 6..9 custom 0..3
};

struct GoAnimHeader {
    char magic[4];  // "WGOA"
    uint32_t count;  // followed by uint32 GameObjectDisplayInfo ids[count], ascending
};

struct GoBarberHeader {
    char magic[4];  // "WGBS"
    uint16_t styles;
    uint16_t levels;  // gtBarberShopCostBase rows
    uint16_t races;
    uint16_t reserved;
    uint32_t poolSize;
};

struct GoBarberStyle {
    uint16_t id;  // BarberShopStyle ID
    uint8_t type;  // 0 hair style, 2 facial hair, 3 skin
    uint8_t race;
    uint8_t sex;
    uint8_t data;  // style index
    uint16_t reserved;
    float costModifier;
    uint32_t name;  // pool
};

struct GoBarberRace {
    uint32_t hairStyle;  // pool offsets of the selector labels
    uint32_t hairColor;
    uint32_t facial[2];  // by sex
    uint8_t hairColors[2];  // number of hair colours by sex
    uint16_t reserved;
};

static_assert(sizeof(GoLockHeader) == 20, "GoLockHeader size");
static_assert(sizeof(GoLockTypeRec) == 20, "GoLockTypeRec size");
static_assert(sizeof(GoLockRec) == 8, "GoLockRec size");
static_assert(sizeof(GoLockCase) == 12, "GoLockCase size");
static_assert(sizeof(GoLockSpellRec) == 16, "GoLockSpellRec size");
static_assert(sizeof(GoSoundHeader) == 8, "GoSoundHeader size");
static_assert(sizeof(GoSoundRec) == 24, "GoSoundRec size");
static_assert(sizeof(GoAnimHeader) == 8, "GoAnimHeader size");
static_assert(sizeof(GoBarberHeader) == 16, "GoBarberHeader size");
static_assert(sizeof(GoBarberStyle) == 16, "GoBarberStyle size");
static_assert(sizeof(GoBarberRace) == 20, "GoBarberRace size");

}  // namespace psp
}  // namespace rendering
}  // namespace wowee
