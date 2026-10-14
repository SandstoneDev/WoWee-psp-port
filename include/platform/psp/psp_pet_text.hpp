// Pets: data/text.pak "pet_text" (tools/psp/build_text_pak.py pet_text) - the pet DBC tables (layouts on
// wowdev.wiki) the pet frame, the stable and the pet talents read (never the server's database):
//   - CreatureFamily.dbc: id, PetTalentType (-1 none, 0 Ferocity, 1 Tenacity, 2 Cunning - the TalentTab whose
//     PetTalentMask has the bit), PetFoodMask (bit n-1 = ItemPetFood n), IconFile (as its SpellIcon.dbc id: the
//     icons live in spells.pak), Name_lang ruRU - GetPetIcon / GetPetFoodTypes / GetPetTalentTree /
//     UnitCreatureFamily / GetStablePetInfo;
//   - ItemPetFood.dbc: id, Name_lang ruRU (PET_DIET_TEMPLATE);
//   - PetPersonality.dbc row 1: HappinessThreshold[3], HappinessDamage[3] - GetPetHappiness (the
//     level is the count of thresholds 2..3 the happiness power reaches);
//   - StableSlotPrices.dbc: id, Cost - GetNextStableSlotCost / BuyStableSlot (row numSlots + 1).
// Layout (little endian): "WPET", u32 version (1);
//   u32 familyCount, per family: u32 id, i32 talentType, u32 foodMask, u32 iconId, name (NUL-terminated UTF-8);
//   u32 foodCount, per food: u32 id, name (NUL-terminated);
//   u32 threshold[3], f32 damage[3];
//   u32 slotCount, per slot: u32 id, u32 cost (copper).
// Header-only and free of PSP headers: tools/psp/host_tests/pets_test.cpp parses a synthetic blob with it.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace wowee::platform::psp {

struct PetFamilyRec {
    uint32_t id = 0;
    int32_t talentType = -1;
    uint32_t foodMask = 0, iconId = 0;
    std::string name;
};

struct PetTextData {
    std::vector<PetFamilyRec> families;
    std::vector<std::pair<uint32_t, std::string>> foods;
    uint32_t threshold[3] = {0, 333000, 666000};  // AzerothCore Pet.h HAPPINESS_LEVEL_SIZE steps (until the DBC is read)
    float damage[3] = {0.75f, 1.0f, 1.25f};
    std::vector<std::pair<uint32_t, uint32_t>> slotCost;  // StableSlotPrices id -> cost
    bool loaded = false;

    const PetFamilyRec* family(uint32_t id) const {
        for (const PetFamilyRec& f : families)
            if (f.id == id) return &f;
        return nullptr;
    }
    // StableSlotPrices.Cost of row `id` (0 when there is none, as GetNextStableSlotCost).
    uint32_t stableSlotCost(uint32_t id) const {
        for (const auto& s : slotCost)
            if (s.first == id) return s.second;
        return 0;
    }
    // GetPetFoodTypes: the ItemPetFood rows whose bit (id - 1) the family's mask has, in DBC order.
    std::vector<std::string> foodNames(uint32_t foodMask) const {
        std::vector<std::string> out;
        for (const auto& f : foods)
            if (f.first >= 1 && f.first <= 32 && (foodMask & (1u << (f.first - 1u)))) out.push_back(f.second);
        return out;
    }
    // GetPetHappiness: 1 unhappy, 2 content, 3 happy (thresholds [1] and [2]) and the damage percent.
    int happinessLevel(uint32_t power) const {
        int level = 1;
        for (int i = 1; i < 3; ++i)
            if (power >= threshold[i]) level = i + 1;
            else break;
        return level;
    }
    int damagePercent(int level) const {
        const int i = level < 1 ? 0 : level > 3 ? 2 : level - 1;
        return static_cast<int>(damage[i] * 100.0f + 0.5f);
    }

    bool parse(const uint8_t* p, size_t n) {
        size_t o = 0;
        auto u32 = [&](uint32_t& v) {
            if (o + 4 > n) return false;
            std::memcpy(&v, p + o, 4);
            o += 4;
            return true;
        };
        auto str = [&](std::string& s) {
            const void* z = o < n ? std::memchr(p + o, 0, n - o) : nullptr;
            if (!z) return false;
            const size_t len = static_cast<const uint8_t*>(z) - (p + o);
            s.assign(reinterpret_cast<const char*>(p + o), len);
            o += len + 1;
            return true;
        };
        if (n < 8 || std::memcmp(p, "WPET", 4) != 0) return false;
        o = 4;
        uint32_t version = 0, count = 0;
        if (!u32(version) || version != 1 || !u32(count) || count > 4096) return false;
        families.clear();
        foods.clear();
        slotCost.clear();
        for (uint32_t i = 0; i < count; ++i) {
            PetFamilyRec f;
            uint32_t tt = 0;
            if (!u32(f.id) || !u32(tt) || !u32(f.foodMask) || !u32(f.iconId) || !str(f.name)) return false;
            f.talentType = static_cast<int32_t>(tt);
            families.push_back(std::move(f));
        }
        if (!u32(count) || count > 4096) return false;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t id = 0;
            std::string name;
            if (!u32(id) || !str(name)) return false;
            foods.emplace_back(id, std::move(name));
        }
        for (uint32_t& t : threshold)
            if (!u32(t)) return false;
        for (float& d : damage) {
            uint32_t bits = 0;
            if (!u32(bits)) return false;
            std::memcpy(&d, &bits, 4);
        }
        if (!u32(count) || count > 64) return false;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t id = 0, cost = 0;
            if (!u32(id) || !u32(cost)) return false;
            slotCost.emplace_back(id, cost);
        }
        loaded = true;
        return true;
    }
};

// The pet action bar's packed slot (the bar's 10 slots and CMSG_PET_ACTION / _SET_ACTION's data; AzerothCore
// CharmInfo.h MAKE_UNIT_ACTION_BUTTON): the spell or command in the low 24 bits, the type in the high byte -
// ACT_PASSIVE 0x01, ACT_DISABLED 0x81 (castable), ACT_ENABLED 0xC1 (castable + autocast on), ACT_REACTION 0x06
// (0 passive, 1 defensive, 2 aggressive), ACT_COMMAND 0x07 (0 stay, 1 follow, 2 attack, 3 abandon); a vehicle's
// spells come as types 8..17 (Player.cpp VehicleSpellInitialize: slot + 8). Pure, for the host test.
namespace petact {
constexpr uint32_t kAutocastable = 0x80000000u, kAutocastOn = 0x40000000u;
constexpr uint8_t kTypeSpell = 1, kTypeReaction = 6, kTypeCommand = 7;
constexpr uint32_t kStay = 0, kFollow = 1, kAttack = 2, kDismiss = 3;
inline uint8_t type(uint32_t slot) { return static_cast<uint8_t>((slot >> 24) & 0x3Fu); }
inline uint32_t id(uint32_t slot) { return slot & 0x00FFFFFFu; }
inline bool isSpell(uint32_t slot) {  // CharmInfo.h IsActionBarForSpell, or a vehicle slot
    const uint8_t t = type(slot);
    return t == kTypeSpell || (t >= 8 && t <= 17);
}
inline uint32_t pack(uint8_t t, uint32_t action) { return (static_cast<uint32_t>(t) << 24) | (action & 0x00FFFFFFu); }
}  // namespace petact

}  // namespace wowee::platform::psp
