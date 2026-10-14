// Talents + skill lines in data/spells.pak (tools/psp/build_spells_pak.py).
//
// Two optional WPAK entries next to the spell buckets; an older spells.pak
// without them stays readable (SpellDb::hasTalents / hasSkillLines = false,
// the talent window then asks for a rebake):
//   "skills"  SkillPakHeader, SkillLineRec[count] sorted by id, strings
//             (SkillLine.dbc: what upstream loadSkillLineDbc keeps - name in
//             the client's locale, category, SpellIcon id)
//   "talents" TalentPakHeader, TalentTabRec[tabCount], TalentRec[talentCount],
//             strings (Talent.dbc / TalentTab.dbc: upstream loadTalentDbc)
// Plain structs, no PSP headers: the host test reads a synthetic pak with
// the same definitions (tools/psp/host_tests/talents_pak_test.cpp).
#pragma once

#include <cstdint>

namespace wowee::platform::psp {

struct SkillPakHeader {
    char magic[4];  // "WSKL"
    uint32_t version, count, recSize, generalIconId, reserved;
};
static_assert(sizeof(SkillPakHeader) == 24, "skills header");

struct SkillLineRec {
    uint32_t id, nameOff, iconId;  // iconId = SpellIcon.dbc id (0 = none)
    uint16_t category, pad;        // SkillLine.dbc Category (7 class, 9 secondary, 11 profession)
};
static_assert(sizeof(SkillLineRec) == 16, "skills record");

struct TalentPakHeader {
    char magic[4];  // "WTAL"
    uint32_t version, tabCount, talentCount, tabRecSize, talentRecSize;
};
static_assert(sizeof(TalentPakHeader) == 24, "talents header");

struct TalentTabRec {
    uint32_t id, nameOff, iconId, classMask, petMask, bgOff;  // bgOff: BackgroundFile ("WarriorArms")
    uint8_t orderIndex, pad[3];
};
static_assert(sizeof(TalentTabRec) == 28, "talent tab record");

struct TalentRec {
    uint32_t id, tabId;
    uint8_t row, column, maxRank, pad;
    uint32_t rankSpell[5];
    uint32_t prereqTalent[3];
    uint8_t prereqRank[3], pad2;  // DBC: counted from zero
};
static_assert(sizeof(TalentRec) == 48, "talent record");

// TalentFrame's rule for a talent button (the TalentUI addon's Lua
// TalentFrame_Update + upstream GetTalentInfo / GetTalentPrereqs): a tier
// opens after 5 points per tier spent in its tree; every prerequisite at
// its rank; a rank left; a point to spend. Pure, so the host test runs it.
struct TalentLearnCheck {
    bool tierOpen = false, prereqsMet = false, rankLeft = false, havePoint = false;
    bool learnable() const { return tierOpen && prereqsMet && rankLeft && havePoint; }
};
// Pets: perTier = PLAYER_TALENTS_PER_TIER 5, or PET_TALENTS_PER_TIER 3 for the pet's tree (TalentFrameBase.lua:
// (tier - 1) * (TalentFrame.pet and PET_TALENTS_PER_TIER or PLAYER_TALENTS_PER_TIER) <= tabPointsSpent).
inline TalentLearnCheck talentLearnCheck(uint8_t row, uint8_t rank, uint8_t maxRank, int tabPointsSpent, int unspent,
                                         const uint8_t (&prereqHave)[3], const uint8_t (&prereqNeed)[3], int perTier = 5) {
    TalentLearnCheck c;
    c.tierOpen = tabPointsSpent >= static_cast<int>(row) * perTier;
    c.prereqsMet = true;
    for (int p = 0; p < 3; ++p)
        if (prereqNeed[p] != 0 && prereqHave[p] < prereqNeed[p]) c.prereqsMet = false;
    c.rankLeft = rank < maxRank;
    c.havePoint = unspent > 0;
    return c;
}

}  // namespace wowee::platform::psp
