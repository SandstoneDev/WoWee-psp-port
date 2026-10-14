// Pets: the pet packets as the servers write and read them - header-only over
// upstream's network::Packet, so tools/psp/host_tests/pets_test.cpp checks them against bodies written the way
// AzerothCore 3.3.5 writes them (Player::PetSpellInitialize, SendStablePet...). psp_world_session_pets.cpp uses them.
//   SMSG_PET_SPELLS: guid; 0 = no pet. Else u16 creature family, u32 duration (ms), u32 mode (react,
//     command, u16 flags), 10 x u32 bar slots, u8 n + n x u32 spells (packed as the slots), u8 m + m x {u32 spell,
//     u16 category, u32 cooldown, u32 category cooldown (bit 31 a flag)}.
//   MSG_LIST_STABLED_PETS: guid of the stable master, u8 pets, u8 slots, per pet u32 number, u32 entry,
//     u32 level, the name, u8 flag (1 the active pet, 2 in a stable slot).
//   CMSG_PET_ACTION: pet guid, u32 packed action, target guid.
//   CMSG_PET_SET_ACTION: pet guid, then one or two (u32 slot, u32 data) pairs - the slot that took
//     the old content first.
//   CMSG_PET_RENAME: pet guid, the name, u8 declined (+ 5 names when 1).
//   CMSG_PET_LEARN_TALENT: pet guid, u32 talent, u32 the requested rank from 0.
//   CMSG_STABLE_PET / CMSG_BUY_STABLE_SLOT: the master's guid; CMSG_UNSTABLE_PET / CMSG_STABLE_SWAP_PET: + u32 pet number.
#pragma once

#include "network/packet.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace wowee::platform::psp::petpkt {

struct PetCooldown {
    uint32_t spell = 0, category = 0, cooldown = 0, categoryCooldown = 0;
};
struct PetSpells {
    uint64_t guid = 0;
    uint16_t family = 0;
    uint32_t duration = 0, mode = 0;
    uint32_t bar[10] = {};
    std::vector<uint32_t> spells;
    std::vector<PetCooldown> cooldowns;
};
// false: too short (the pet is then gone, as for a packet without a guid - upstream handlePetSpells clears it).
inline bool parsePetSpells(network::Packet& p, PetSpells& out) {
    out = PetSpells{};
    if (!p.hasRemaining(8)) return false;
    out.guid = p.readUInt64();
    if (out.guid == 0) return true;
    if (!p.hasRemaining(2 + 4 + 4 + 10 * 4 + 1)) return false;
    out.family = p.readUInt16();
    out.duration = p.readUInt32();
    out.mode = p.readUInt32();
    for (uint32_t& s : out.bar) s = p.readUInt32();
    const uint8_t n = p.readUInt8();
    for (uint8_t i = 0; i < n && p.hasRemaining(4); ++i) out.spells.push_back(p.readUInt32());
    if (!p.hasRemaining(1)) return true;
    const uint8_t m = p.readUInt8();
    for (uint8_t i = 0; i < m && p.hasRemaining(14); ++i) {
        PetCooldown c;
        c.spell = p.readUInt32();
        c.category = p.readUInt16();
        c.cooldown = p.readUInt32();
        c.categoryCooldown = p.readUInt32() & 0x7FFFFFFFu;
        out.cooldowns.push_back(c);
    }
    return true;
}

struct StableRec {
    uint32_t petNumber = 0, entry = 0, level = 0;
    std::string name;
    uint8_t flag = 0;
};
struct StableList {
    uint64_t master = 0;
    uint8_t slots = 0;
    StableRec current;               // petNumber 0 = none
    std::vector<StableRec> stabled;  // in list order (GetStablePetInfo 1..)
};
inline bool parseStableList(network::Packet& p, StableList& out) {
    out = StableList{};
    if (!p.hasRemaining(10)) return false;
    out.master = p.readUInt64();
    const uint8_t count = p.readUInt8();
    out.slots = p.readUInt8();
    for (uint8_t i = 0; i < count; ++i) {
        if (!p.hasRemaining(12)) break;
        StableRec r;
        r.petNumber = p.readUInt32();
        r.entry = p.readUInt32();
        r.level = p.readUInt32();
        r.name = p.readString();
        if (!p.hasRemaining(1)) break;
        r.flag = p.readUInt8();
        if (!(r.flag & 2)) out.current = r;
        else out.stabled.push_back(std::move(r));
    }
    return true;
}

// The peeks of psp_world_session_pets.cpp - read in place and the read position put back, so a
// packet that is not the pet's goes on to its own handler untouched (no copy of the whole packet); false when the
// bytes are not all there (the position is not moved either).
//   SMSG_SPELL_COOLDOWN / SMSG_ATTACKSTART: a leading guid; SMSG_ATTACKSTOP: a leading packed guid;
//   SMSG_COOLDOWN_EVENT / SMSG_CLEAR_COOLDOWN: u32 spell, the unit's guid (AzerothCore Player::SendCooldownEvent,
//   Player::SendClearCooldown, Pet::RemoveSpellCooldown); SMSG_TALENTS_INFO / _INVOLUNTARILY_RESET: a leading u8.
inline bool peekU8(network::Packet& p, uint8_t& out) {
    if (!p.hasRemaining(1)) return false;
    const size_t pos = p.getReadPos();
    out = p.readUInt8();
    p.setReadPos(pos);
    return true;
}
inline bool peekGuid(network::Packet& p, uint64_t& out) {
    if (!p.hasRemaining(8)) return false;
    const size_t pos = p.getReadPos();
    out = p.readUInt64();
    p.setReadPos(pos);
    return true;
}
inline bool peekPackedGuid(network::Packet& p, uint64_t& out) {
    if (!p.hasFullPackedGuid()) return false;
    const size_t pos = p.getReadPos();
    out = p.readPackedGuid();
    p.setReadPos(pos);
    return true;
}
inline bool peekSpellGuid(network::Packet& p, uint32_t& spell, uint64_t& guid) {
    if (!p.hasRemaining(12)) return false;
    const size_t pos = p.getReadPos();
    spell = p.readUInt32();
    guid = p.readUInt64();
    p.setReadPos(pos);
    return true;
}

inline network::Packet buildPetAction(uint16_t opcode, uint64_t pet, uint32_t data, uint64_t target) {
    network::Packet p(opcode);
    p.writeUInt64(pet);
    p.writeUInt32(data);
    p.writeUInt64(target);
    return p;
}
// pairs: (slot, data), the one pushed aside first; one or two.
inline network::Packet buildPetSetAction(uint16_t opcode, uint64_t pet, int otherSlot, uint32_t otherData, int slot,
                                         uint32_t data) {
    network::Packet p(opcode);
    p.writeUInt64(pet);
    if (otherSlot >= 0) {
        p.writeUInt32(static_cast<uint32_t>(otherSlot));
        p.writeUInt32(otherData);
    }
    p.writeUInt32(static_cast<uint32_t>(slot));
    p.writeUInt32(data);
    return p;
}
inline network::Packet buildPetRename(uint16_t opcode, uint64_t pet, const std::string& name) {
    network::Packet p(opcode);
    p.writeUInt64(pet);
    p.writeString(name);
    p.writeUInt8(0);  // no declined names (see psp_world_session_pets.cpp)
    return p;
}
inline network::Packet buildPetLearnTalent(uint16_t opcode, uint64_t pet, uint32_t talent, uint32_t rankFromZero) {
    network::Packet p(opcode);
    p.writeUInt64(pet);
    p.writeUInt32(talent);
    p.writeUInt32(rankFromZero);
    return p;
}
inline network::Packet buildStable(uint16_t opcode, uint64_t master, bool withNumber, uint32_t petNumber) {
    network::Packet p(opcode);
    p.writeUInt64(master);
    if (withNumber) p.writeUInt32(petNumber);
    return p;
}

}  // namespace wowee::platform::psp::petpkt
