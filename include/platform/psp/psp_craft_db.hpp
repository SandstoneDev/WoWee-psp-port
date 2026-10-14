// Trade skills: data/craft.pak (tools/psp/build_craft_pak.py) - the recipe part of
// Spell.dbc / SkillLineAbility.dbc / Item.dbc / ItemSubClass.dbc that upstream's trade skill code reads
// from its DBC caches (SpellHandler::loadSpellNameCache: reagents, CREATE_ITEM's EffectItemType;
// loadSkillLineAbilityDbc: the skill line and TrivialSkillLineRankHigh / Low; the crafted item's subclass).
//
// PSP: one entry per profession ("cl_<SkillLine id>", 1..65 KB), read when its window opens (or when a
// profession spell is cast, to see whether the window has anything to show) and freed when it closes.
// The recipe names stay in spells.pak (SpellDb), the item names come from the server (item query).
#pragma once

#include <cstdint>

namespace wowee::platform::psp {

struct CraftLineHeader {  // 40 B
    char magic[4];        // "WCRL"
    uint16_t version, skillLine;
    uint16_t recipeCount, reagentCount, itemCount, groupCount, slotCount, pad;
    uint32_t strOff, strSize;  // from the entry start
    uint32_t altVerb;          // SkillLine.AlternateVerb (string offset, 0 = none)
    uint32_t pad2, pad3;
};
static_assert(sizeof(CraftLineHeader) == 40, "craft.pak line header");

struct CraftRecipeRec {  // 28 B, sorted by spellId
    uint32_t spellId, madeItem, descOff;
    uint16_t trivialHigh, trivialLow;  // SkillLineAbility.TrivialSkillLineRankHigh / Low
    uint16_t firstReagent, madeMin, madeMax;
    uint8_t reagentCount, group, invType, pad;  // group 0xFF = no created item (no heading)
    uint16_t pad2;
};
static_assert(sizeof(CraftRecipeRec) == 28, "craft.pak recipe");

struct CraftReagentRec {
    uint32_t item, count;
};
struct CraftItemRec {  // sorted by item
    uint32_t item, displayId;
};
struct CraftGroupRec {
    uint8_t itemClass, subClass;
    uint16_t pad;
    uint32_t name;
};
struct CraftSlotRec {
    uint8_t invType, pad[3];
    uint32_t name;
};
static_assert(sizeof(CraftReagentRec) == 8 && sizeof(CraftItemRec) == 8 && sizeof(CraftGroupRec) == 8 &&
                  sizeof(CraftSlotRec) == 8,
              "craft.pak records");

class CraftDb {
public:
    CraftDb() = default;
    CraftDb(const CraftDb&) = delete;
    CraftDb& operator=(const CraftDb&) = delete;
    ~CraftDb() { release(); }

    // Reads "cl_<skillLine>" of data/craft.pak (the pak is opened and closed here). False: no pak, no such
    // line (no recipes: Fishing) or a broken entry; noPak() tells the first apart.
    bool load(uint32_t skillLine);
    void release();
    void swap(CraftDb& o);  // PSP: a probe loaded beside the open line, then kept
    bool loaded() const { return hdr_ != nullptr; }
    bool noPak() const { return noPak_; }
    uint32_t skillLine() const { return hdr_ ? hdr_->skillLine : 0; }
    uint32_t bytes() const { return size_; }

    uint32_t recipeCount() const { return hdr_ ? hdr_->recipeCount : 0; }
    const CraftRecipeRec& recipeAt(uint32_t i) const { return recipes_[i]; }
    const CraftRecipeRec* recipe(uint32_t spellId) const;  // binary search
    const CraftReagentRec* reagents(const CraftRecipeRec& r) const { return reagents_ + r.firstReagent; }
    uint32_t displayOf(uint32_t itemId) const;  // ItemDisplayInfo id, 0 = unknown
    uint32_t groupCount() const { return hdr_ ? hdr_->groupCount : 0; }
    const CraftGroupRec& groupAt(uint32_t i) const { return groups_[i]; }
    uint32_t slotCount() const { return hdr_ ? hdr_->slotCount : 0; }
    const CraftSlotRec& slotAt(uint32_t i) const { return slots_[i]; }
    const char* str(uint32_t off) const { return off < strSize_ ? strings_ + off : ""; }
    const char* altVerb() const { return hdr_ ? str(hdr_->altVerb) : ""; }

private:
    void* blob_ = nullptr;
    uint32_t size_ = 0;
    const CraftLineHeader* hdr_ = nullptr;
    const CraftRecipeRec* recipes_ = nullptr;
    const CraftReagentRec* reagents_ = nullptr;
    const CraftItemRec* items_ = nullptr;
    const CraftGroupRec* groups_ = nullptr;
    const CraftSlotRec* slots_ = nullptr;
    const char* strings_ = nullptr;
    uint32_t strSize_ = 0;
    bool noPak_ = false;
};

}  // namespace wowee::platform::psp
