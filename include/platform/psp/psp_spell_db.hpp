// Spell data for the PSP: data/spells.pak (tools/psp/build_spells_pak.py) =
// what upstream's SpellHandler reads from Spell.dbc and its satellites at run
// time (loadSpellNameCache: name, rank, school, recovery, range, duration,
// implicit targets; getSpellData: cast time, ranges, power; the icon path via
// SpellIcon.dbc) plus the client's SPELL_FAILED_* texts.
//
// PSP: 49.8k spells do not stay in RAM. Only the header lives in memory
// (bucket directory + icon slot table, ~9 KB); a bucket of 256 spell ids
// (~17 KB) is read when one of its spells is first asked for and the
// records asked for are kept. Icons (32x32 T8, 2 KB) load on demand into a
// small cache. The pak is opened only while reading and closed after 3 s
// without reads (the card's open-file limit).
#pragma once

#include "platform/psp/psp_talent_format.hpp"  // Talents
#include "platform/psp/psp_texture.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee::platform::psp {

// "shdr" entry.
struct SpellPakHeader {
    char magic[4];  // "WSPL"
    uint32_t version, bucketShift, bucketCount, iconCount, iconStride, iconMaxId, recSize;
    // + bucketCount x {offset in "sdata", size, count}, (iconMaxId + 1) x u16 slot
};
static_assert(sizeof(SpellPakHeader) == 32, "spells.pak header");

// One Spell.dbc row, as baked (80 bytes).
struct SpellRec {
    uint32_t id, nameOff, rankOff, castMs, recoveryMs, categoryRecoveryMs;
    int32_t durationMs;  // SpellDuration base, -1 = until cancelled, 0 = none
    float minRange, maxRange;
    uint32_t attributes, attributesEx, targets, implicitTargetA, schoolMask, manaCost, visual;
    uint16_t iconSlot;  // 0xFFFF = none
    uint8_t powerType, dispel;
    uint16_t skillLine;  // SkillLineAbility (0 = none)
    uint8_t effect[3], pad;
    uint16_t aura[3];
};
static_assert(sizeof(SpellRec) == 80, "spells.pak record");

// Action bar: a "suse" record (24 bytes) - what ActionButton_UpdateUsable needs
// beyond SpellRec, only for the spells that have any of it (tools/psp/build_spells_pak.py build_use).
struct SpellUseRec {
    uint32_t id, stances, stancesNot;  // Spell.dbc Stances / StancesNot: 1 << (form - 1)
    uint8_t manaPct;                   // ManaCostPercentage
    uint8_t auraState[4];              // CasterAuraState, TargetAuraState, CasterAuraStateNot, TargetAuraStateNot
    uint8_t flags;                     // kUseNotNeedShapeshift
    uint8_t reagentCount, pad;
    uint16_t firstReagent, pad2;       // into the bucket's reagents (u32 item | count << 24)
};
static_assert(sizeof(SpellUseRec) == 24, "spells.pak suse record");

struct SpellInfo {
    SpellRec rec{};
    std::string name, rank;
    bool passive() const { return (rec.attributes & 0x40u) != 0; }         // SPELL_ATTR0_PASSIVE
    bool hiddenClientSide() const { return (rec.attributes & 0x80u) != 0; } // SPELL_ATTR0_HIDDEN_CLIENTSIDE
    // action bar: from "suse" (all zero / empty with an older spells.pak or a spell without a record).
    static constexpr uint8_t kUseNotNeedShapeshift = 1;  // SPELL_ATTR2_NOT_NEED_SHAPESHIFT
    uint32_t stances = 0, stancesNot = 0;
    uint8_t manaPct = 0, useFlags = 0;
    uint8_t auraState[4] = {};
    std::vector<uint32_t> reagents;  // item | count << 24
};

class Pak;  // Talents (openExtras)

// Tooltips: the GameTooltip data of spells.pak ("stip" and the name tables of
// tools/psp/build_spells_pak.py build_tooltips), read record by record on demand.
struct SpellTipEffect {
    uint8_t effect = 0;  // 0 = no effect in this slot
    uint16_t aura = 0;
    int32_t basePoints = 0, dieSides = 0, miscValue = 0, miscValueB = 0;
    float realPointsPerLevel = 0.0f, pointsPerCombo = 0.0f, radius = 0.0f, radiusMax = 0.0f;
    float multipleValue = 0.0f, damageMultiplier = 1.0f, bonusMultiplier = 0.0f;
    uint32_t amplitude = 0, chainTarget = 0, triggerSpell = 0;
};
struct SpellTip {
    uint32_t id = 0;
    int32_t dur = 0, durPerLevel = 0, durMax = 0;  // SpellDuration (ms, -1 = until cancelled)
    uint32_t procChance = 0, procCharges = 0, maxTargets = 0, stack = 0, maxTargetLevel = 0;
    uint32_t maxLevel = 0, baseLevel = 0, spellLevel = 0, attrEx2 = 0, attrEx3 = 0, attrEx6 = 0;
    uint32_t manaPerSecond = 0, manaPerLevel = 0;
    uint8_t runes[3] = {};  // SpellRuneCost: blood, unholy, frost
    int32_t eqClass = -1;   // EquippedItemClass (-1 none)
    uint32_t eqSubMask = 0, eqInvMask = 0, totem[2] = {}, totemCat[2] = {}, spellFocus = 0;
    float minRangeHostile = 0.0f, maxRangeHostile = 0.0f;
    uint32_t rangeFlags = 0;  // SpellRange.Flags (1 = melee)
    SpellTipEffect eff[3];
    std::string desc, var;  // Description (client locale), SpellDescriptionVariables text
};
// The name / blob tables beside "stip" (WBBT, one record per id).
enum SpellTipTable : uint8_t {
    kTipSpell, kTipForm, kTipTotem, kTipSubClass, kTipSubClassMask, kTipEnchant, kTipGem, kTipItemSet, kTipLimit,
    kTipTableCount
};

class SpellDb {
public:
    ~SpellDb();
    bool open();  // reads the header of data/spells.pak (then closes it)
    bool loaded() const { return !dir_.empty(); }

    // nullptr for an unknown id (or without the pak). Reads the spell's bucket
    // on the first ask; the pointer stays valid for the session.
    const SpellInfo* get(uint32_t spellId);
    // Reads the buckets of many spells in one go (SMSG_INITIAL_SPELLS).
    void prefetch(const std::vector<uint32_t>& ids);
    const std::string& name(uint32_t spellId);

    // The spell's icon, or nullptr (no pak / no icon). Loaded on demand;
    // `frame` = the caller's frame counter: textures used in the last two
    // frames are never evicted (the GE may still read them).
    const Texture* icon(uint32_t spellId, uint32_t frame);

    // SMSG_CAST_FAILED / SMSG_SPELL_FAILURE result -> text (the client's
    // SPELL_FAILED_* line, else upstream's English), nullptr for none.
    const char* castFailText(uint8_t result) const;

    // SpellShapeshiftForm.BonusActionBar of a form (UNIT_FIELD_BYTES_1 byte 3):
    // FrameXML's GetBonusBarOffset (warrior stances, druid forms, stealth).
    uint8_t bonusBar(uint8_t form) const { return form < sizeof(forms_) ? forms_[form] : 0; }
    // action bar: SpellShapeshiftForm.Flags (SHAPESHIFT_FLAG_STANCE 0x1, ..._CAN_ONLY_CAST_SHAPESHIFT_SPELLS
    // 0x400) - SpellInfo::CheckShapeshift; hasFormFlags() false with an older spells.pak.
    uint16_t formFlags(uint8_t form) const { return form < 64 ? formFlags_[form] : 0; }
    bool hasFormFlags() const { return formFlagsLoaded_; }
    bool hasUseData() const { return useOff_ != 0; }  // "suse" present (ActionButton_UpdateUsable data)

    void tick(uint32_t nowMs);  // closes the pak after 3 s without reads
    uint32_t bucketsRead() const { return bucketsRead_; }
    uint32_t iconsLoaded() const { return iconsLoaded_; }
    uint32_t spellsCached() const { return static_cast<uint32_t>(cache_.size()); }

private:
    bool ensureOpen();
    bool readRaw(uint32_t offset, void* dst, uint32_t size);
    // Hardware OOM: only the spells asked for are kept (was every
    // spell of the bucket: 3810 records ~0.8 MB at login); the last bucket read
    // stays as raw bytes so a run of asks into one bucket reads it once.
    void loadBucket(uint32_t bucket, const uint32_t* ids, size_t n);
    bool readBucket(uint32_t bucket);
    struct Bucket {
        uint32_t offset = 0, size = 0, count = 0;
    };
    std::string path_;
    int fd_ = -1;
    uint32_t lastReadMs_ = 0;
    uint32_t sdataOff_ = 0, iconsOff_ = 0;
    SpellPakHeader hdr_{};
    std::vector<Bucket> dir_;
    std::vector<uint8_t> lastBucketBuf_;
    uint32_t lastBucket_ = ~0u;
    std::unordered_set<uint32_t> missing_;  // asked for, not in the pak
    std::vector<uint16_t> iconSlots_;
    std::unordered_map<uint32_t, SpellInfo> cache_;
    uint8_t forms_[64] = {};
    // action bar: "sforms" flags and "suse" (its bucket directory stays on the card: 8 bytes read per
    // bucket load, no RAM).
    uint16_t formFlags_[64] = {};
    bool formFlagsLoaded_ = false;
    uint32_t useOff_ = 0, useBuckets_ = 0, useShift_ = 0;
    void loadUse(uint32_t bucket, const uint32_t* ids, size_t n);
    std::vector<char> failText_;         // "cfail": offsets + strings
    struct Icon {
        Texture tex;
        uint32_t lastFrame = 0;
        bool failed = false;
    };
    std::unordered_map<uint16_t, Icon> icons_;
    uint32_t bucketsRead_ = 0, iconsLoaded_ = 0;
    static constexpr size_t kMaxIcons = 64;

    // Tooltips: GameTooltip data. tip() parses the spell's "stip" record into a small cache
    // (12 spells, oldest out: the pointer stays valid until 12 other spells were asked for); tipBlob() reads
    // one record of a table (false: none / an older spells.pak). Nothing is kept beyond the cache.
public:
    bool hasTips() const { return tipTables_[kTipSpell].offset != 0; }
    const SpellTip* tip(uint32_t spellId);
    bool tipBlob(SpellTipTable table, uint32_t id, std::vector<uint8_t>& out);
    std::string tipName(SpellTipTable table, uint32_t id);  // a name table's string ("" none)
    uint32_t tipReads() const { return tipReads_; }

private:
    struct TipTable {
        uint32_t offset = 0, size = 0, shift = 0, buckets = 0;
    };
    TipTable tipTables_[kTipTableCount];
    std::vector<std::unique_ptr<SpellTip>> tipCache_;  // most recent last (the objects never move)
    uint32_t tipReads_ = 0;
    void openTips(Pak& pak);

    // Talents + skill lines (psp_spell_db_talents.cpp, entries
    // "skills" / "talents" of a rebaked spells.pak; an older pak has neither).
public:
    struct SkillLineInfo {  // what upstream loadSkillLineDbc keeps
        uint32_t id = 0, iconId = 0;  // iconId: SpellIcon.dbc id
        uint16_t category = 0;        // 7 class, 9 secondary, 11 profession
        std::string name;             // the client's locale
    };
    bool hasSkillLines() const { return !skills_.empty(); }
    const SkillLineInfo* skillLine(uint32_t id) const;
    uint32_t generalTabIconId() const { return generalIconId_; }  // INV_Misc_Book_09
    bool hasTalents() const { return talentsSize_ != 0; }
    // The raw "talents" entry (TalentPakHeader, tabs, talents, strings): read
    // through the short-lived descriptor; the caller keeps what it needs.
    bool readTalents(std::vector<uint8_t>& out);
    // An icon by SpellIcon.dbc id (talent tabs, skill lines), same cache.
    const Texture* iconById(uint32_t spellIconId, uint32_t frame);

private:
    void openExtras(Pak& pak);
    const Texture* iconBySlot(uint16_t slot, uint32_t frame);
    std::vector<SkillLineInfo> skills_;  // by id
    uint32_t generalIconId_ = 0, talentsOff_ = 0, talentsSize_ = 0;
    // Talents + skill lines (end)
};

} // namespace wowee::platform::psp
