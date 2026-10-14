// Expansion windows: data/wotlk.pak (tools/psp/build_wotlk_pak.py) - the DBC part of the 3.3.5a
// windows the client keeps in its DBC caches: LFGDungeons / LFGDungeonGroup (the Dungeon Finder), Achievement /
// Achievement_Category / Achievement_Criteria (AchievementFrame), Holidays + HolidayNames / HolidayDescriptions
// and Map (CalendarFrame), plus the windows' art.
//
// PSP (docs/PSP_PORT_ARCHITECTURE.md: ~4 free file descriptors on the hardware): the pak is never kept open -
// each load() opens it, reads its entry and closes it (as CraftDb). The data lives while a window needs it.
#pragma once

#include "platform/psp/psp_texture.hpp"

#include <cstdint>

namespace wowee::platform::psp {

class Pak;

// --- "lfg" ---------------------------------------------------------------------------------------------------------
struct LfgPakHeader {  // 32 B
    char magic[4];     // "WLFG"
    uint16_t version, dungeonCount, groupCount, pad;
    uint32_t dungeonOff, groupOff, strOff, strSize;
    uint32_t coinIcon;  // SpellIcon.dbc id of INV_Misc_Coin_02 (the misc reward)
};
static_assert(sizeof(LfgPakHeader) == 32, "wotlk.pak lfg header");

// LFGDungeons.dbc (upstream game::LfgDungeon), sorted by id.
struct LfgDungeonRec {  // 32 B
    uint32_t id, name, texture, desc;  // string offsets; texture = TextureFilename (a dungeon's name in capitals)
    uint16_t mapId, orderIndex;
    uint8_t minLevel, maxLevel, recLevel, minRecLevel, maxRecLevel;
    uint8_t typeId;  // 1 dungeon, 2 raid, 4 zone, 5 heroic, 6 random
    int8_t faction;  // -1 both, 0 Horde, 1 Alliance
    uint8_t expansion, groupId, difficulty, flags;
    uint8_t encounters;  // DungeonEncounter.dbc rows of mapId / difficulty (GetLFGProposal's totalEncounters)
};
static_assert(sizeof(LfgDungeonRec) == 32, "wotlk.pak lfg dungeon");

// LFGDungeonGroup.dbc, by Order_index: the Dungeon Finder's headers ("Classic Dungeons"...).
struct LfgGroupRec {  // 12 B
    uint32_t id, name;
    uint16_t order;
    uint8_t typeId, pad;
};
static_assert(sizeof(LfgGroupRec) == 12, "wotlk.pak lfg group");

class LfgDb {
public:
    LfgDb() = default;
    LfgDb(const LfgDb&) = delete;
    LfgDb& operator=(const LfgDb&) = delete;
    ~LfgDb() { release(); }

    bool load();  // "lfg" of data/wotlk.pak (opened and closed here); false: no pak / a broken entry
    void release();
    void forget() {  // released, and the next user loads it again
        release();
        tried_ = false;
    }
    bool loaded() const { return hdr_ != nullptr; }
    bool tried() const { return tried_; }
    uint32_t bytes() const { return size_; }

    uint32_t dungeonCount() const { return hdr_ ? hdr_->dungeonCount : 0; }
    const LfgDungeonRec& dungeonAt(uint32_t i) const { return dungeons_[i]; }
    const LfgDungeonRec* dungeon(uint32_t id) const;  // binary search
    uint32_t groupCount() const { return hdr_ ? hdr_->groupCount : 0; }
    const LfgGroupRec& groupAt(uint32_t i) const { return groups_[i]; }
    const LfgGroupRec* group(uint32_t id) const;
    const char* str(uint32_t off) const { return off < strSize_ ? strings_ + off : ""; }
    const char* name(uint32_t dungeonId) const;  // "" when unknown
    uint32_t coinIcon() const { return hdr_ ? hdr_->coinIcon : 0; }

private:
    void* blob_ = nullptr;
    uint32_t size_ = 0;
    const LfgPakHeader* hdr_ = nullptr;
    const LfgDungeonRec* dungeons_ = nullptr;
    const LfgGroupRec* groups_ = nullptr;
    const char* strings_ = nullptr;
    uint32_t strSize_ = 0;
    bool tried_ = false;
};

// --- "ach" + "ac_<category>" -----------------------------------------------------------------------------------------
struct AchPakHeader {  // 32 B
    char magic[4];     // "WACH"
    uint16_t version, categoryCount, achievementCount, pad;
    uint32_t catOff, achOff, strOff, strSize, pad2;
};
static_assert(sizeof(AchPakHeader) == 32, "wotlk.pak ach header");

// Achievement_Category.dbc in the client's order (ui_order; the statistic tree after the achievement one).
struct AchCatRec {  // 12 B
    uint16_t id;
    int16_t parent;  // -1: top level
    uint16_t order;
    uint8_t flags, pad;  // flags 1: a statistic category
    uint32_t name;
};
static_assert(sizeof(AchCatRec) == 12, "wotlk.pak ach category");

// Achievement.dbc, sorted by id.
struct AchIdxRec {  // 16 B
    uint16_t id, category, prev, next;  // prev = Supercedes; next = the one that supercedes this (0 = none)
    uint16_t icon, flags, order;        // icon: SpellIcon.dbc id; flags: Achievement.dbc Flags (0x1 statistic)
    uint8_t points;
    int8_t faction;  // -1 both, 0 Horde, 1 Alliance
};
static_assert(sizeof(AchIdxRec) == 16, "wotlk.pak ach index");

class AchIndex {
public:
    AchIndex() = default;
    AchIndex(const AchIndex&) = delete;
    AchIndex& operator=(const AchIndex&) = delete;
    ~AchIndex() { release(); }

    bool load();
    void release();
    bool loaded() const { return hdr_ != nullptr; }
    bool tried() const { return tried_; }
    uint32_t bytes() const { return size_; }

    uint32_t categoryCount() const { return hdr_ ? hdr_->categoryCount : 0; }
    const AchCatRec& categoryAt(uint32_t i) const { return cats_[i]; }
    const AchCatRec* category(uint32_t id) const;
    uint32_t count() const { return hdr_ ? hdr_->achievementCount : 0; }
    const AchIdxRec& at(uint32_t i) const { return recs_[i]; }
    const AchIdxRec* find(uint32_t id) const;  // binary search
    const char* str(uint32_t off) const { return off < strSize_ ? strings_ + off : ""; }

private:
    void* blob_ = nullptr;
    uint32_t size_ = 0;
    const AchPakHeader* hdr_ = nullptr;
    const AchCatRec* cats_ = nullptr;
    const AchIdxRec* recs_ = nullptr;
    const char* strings_ = nullptr;
    uint32_t strSize_ = 0;
    bool tried_ = false;
};

struct AchCatHeader {  // 16 B
    char magic[4];     // "WACC"
    uint16_t version, count, critCount, pad;
    uint32_t strOff;
};
static_assert(sizeof(AchCatHeader) == 16, "wotlk.pak ac_ header");

struct AchRec {  // 20 B, in the client's list order (ui_order, then id)
    uint32_t id, name, desc, reward;  // string offsets (reward: "Reward: ..." or "")
    uint16_t critFirst, critCount;
};
static_assert(sizeof(AchRec) == 20, "wotlk.pak ac_ achievement");

struct AchCritRec {  // 24 B, by Achievement_Criteria's show order
    uint32_t id, desc, type, asset, quantity;
    uint16_t flags, order;  // flags 0x1: ACHIEVEMENT_CRITERIA_PROGRESS_BAR
};
static_assert(sizeof(AchCritRec) == 24, "wotlk.pak ac_ criteria");

// One category's achievements with their names, descriptions, rewards and criteria ("ac_<id>").
class AchCategory {
public:
    AchCategory() = default;
    AchCategory(const AchCategory&) = delete;
    AchCategory& operator=(const AchCategory&) = delete;
    ~AchCategory() { release(); }

    bool load(uint32_t categoryId);
    void release();
    bool loaded() const { return hdr_ != nullptr; }
    uint32_t id() const { return id_; }
    uint32_t bytes() const { return size_; }

    uint32_t count() const { return hdr_ ? hdr_->count : 0; }
    const AchRec& at(uint32_t i) const { return recs_[i]; }
    const AchRec* find(uint32_t achievementId) const;  // linear (a category holds at most a few hundred)
    const AchCritRec* criteria(const AchRec& r) const { return crits_ + r.critFirst; }
    const char* str(uint32_t off) const { return off < strSize_ ? strings_ + off : ""; }

private:
    void* blob_ = nullptr;
    uint32_t size_ = 0, id_ = 0;
    const AchCatHeader* hdr_ = nullptr;
    const AchRec* recs_ = nullptr;
    const AchCritRec* crits_ = nullptr;
    const char* strings_ = nullptr;
    uint32_t strSize_ = 0;
};

// --- "hol" / "maps" --------------------------------------------------------------------------------------------------
struct HolRec {  // 16 B, by id
    uint32_t id, name, desc, texture;
};
struct MapRec {  // 8 B, by id
    uint16_t id, type;  // Map.dbc InstanceType: 1 party, 2 raid
    uint32_t name;
};
static_assert(sizeof(HolRec) == 16 && sizeof(MapRec) == 8, "wotlk.pak hol / maps");

// Holidays.dbc names / descriptions and Map.dbc instance names - the calendar's (a few KB each).
class CalendarDb {
public:
    CalendarDb() = default;
    CalendarDb(const CalendarDb&) = delete;
    CalendarDb& operator=(const CalendarDb&) = delete;
    ~CalendarDb() { release(); }

    bool load();
    void release();
    bool loaded() const { return hol_ != nullptr; }
    uint32_t bytes() const { return holSize_ + mapSize_; }
    const HolRec* holiday(uint32_t id) const;
    const char* holidayStr(uint32_t off) const;
    const char* mapName(uint32_t mapId) const;  // "" when unknown
    uint16_t mapType(uint32_t mapId) const;     // 1 party, 2 raid, 0 unknown

private:
    void* hol_ = nullptr;
    void* map_ = nullptr;
    uint32_t holSize_ = 0, mapSize_ = 0;
};

// --- Plumbing --------------------------------------------------------------------------------------------------------
// Opens data/wotlk.pak (the caller closes it soon - never kept open across frames). False: no pak.
bool wotlkPakOpen(Pak& pak);
// One raw entry of an open pak (pakFree() it); nullptr when missing.
void* wotlkPakRaw(Pak& pak, const char* name, uint32_t& size);
// One texture of an open pak into `out` (released first); false when missing. Adds its size to *bytes.
bool wotlkPakTexture(Pak& pak, const char* name, Texture& out, uint32_t* bytes = nullptr);

}  // namespace wowee::platform::psp
