// the pet, its action bar, its talents and the stable. Upstream WoWee's SpellHandler / CombatHandler pet half
// (handlePetSpells, sendPetAction, togglePetSpellAutocast, renamePet, abandonPet, handleListStabledPets, the stable
// senders, the pet Lua API) is the base; the packet layouts and enums are AzerothCore's (Player.cpp
// PetSpellInitialize, PetHandler.cpp, NPCHandler.cpp, CharmInfo.h, PetDefines.h). Where upstream has nothing:
//   - SMSG_PET_SPELLS is read with its cooldown block (upstream drops it): the bar shows each entry's cooldown, or
//     its category cooldown for a spell with a category - the one AzerothCore fills;
//   - a bar slot is a spell when its type is ACT_PASSIVE / ACT_DISABLED / ACT_ENABLED (CharmInfo.h
//     IsActionBarForSpell: type & 0x3F == 1) or a vehicle slot 8..17 (Player.cpp VehicleSpellInitialize);
//   - the reaction / command clicked shows at once: AzerothCore never sends SMSG_PET_MODE after CMSG_PET_ACTION
//     (its HandlePetActionHelper only sets the state); the attack indicator follows SMSG_ATTACKSTART / _ATTACKSTOP;
//   - rearranging the bar (the port's rule): picking an action up only puts it on the cursor; dropping it on another
//     slot swaps the two slots, sent as one CMSG_PET_SET_ACTION with both pairs (AzerothCore HandlePetSetAction
//     accepts a command / reaction only moved that way);
//   - the pet name: CMSG_PET_NAME_QUERY carries UNIT_FIELD_PETNUMBER, the answer comes back keyed by it (AzerothCore
//     SendPetNameQuery); asked when SMSG_PET_SPELLS names a pet (upstream) and after a rename (the port's rule);
//   - the stable: the active pet (flag 1) is record 0, the stabled ones (flag 2) follow; a drop with a pet on the
//     cursor sends the AzerothCore request that does it - the active pet to an empty slot CMSG_STABLE_PET, to an
//     occupied one CMSG_STABLE_SWAP_PET, a stabled pet to the active place CMSG_STABLE_SWAP_PET while a pet is out
//     else CMSG_UNSTABLE_PET; every SMSG_STABLE_RESULT and every change of the pet asks the list again;
//   - the lines: the GlobalStrings key of each is the one named like AzerothCore's enum value (PetActionFeedback,
//     PetTameResult, StableResultCode), a cast failure the SPELL_FAILED_* text; where a line shows is the port's
//     GameLine rule (refusals red, news in the chat), and where upstream shows it in one place (the action feedback,
//     SMSG_PET_BROKEN, a learned spell: the chat) that place.
// PSP differences (PSP: below): no pet spell book (PetSpellBook) and no targeting cursor - a pet spell always goes as
// CMSG_PET_ACTION with the player's target; the rename sends no declined names (AzerothCore takes the name without
// them).
#include "platform/psp/psp_world_session.hpp"

#include "core/coordinates.hpp"
#include "game/protocol_constants.hpp"
#include "game/update_field_table.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_pet_parse.hpp"
#include "platform/psp/psp_spell_db.hpp"
#include "platform/psp/psp_strings.hpp"
#include "platform/psp/psp_talent_format.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace wowee::platform::psp {

namespace {

// UpdateFields.h 3.3.5a (OBJECT_END 6 + the unit offsets) - not in upstream's field table.
constexpr uint16_t kFieldPower5 = 29;           // UNIT_FIELD_POWER5 = happiness (POWER_HAPPINESS 4)
constexpr uint16_t kFieldMaxPower5 = 37;        // UNIT_FIELD_MAXPOWER5
constexpr uint16_t kFieldPetNumber = 75;        // UNIT_FIELD_PETNUMBER
constexpr uint16_t kFieldBytes2 = 122;          // UNIT_FIELD_BYTES_2 (byte 2: UNIT_CAN_BE_RENAMED 1, _ABANDONED 2)
constexpr uint8_t kClassHunter = 3;

// PETTAME_* by the reason (AzerothCore PetTameResult PET_TAME_*: the same names, 1..13), through ERR_TAME_FAILED
// "%s.".
const char* tameText(uint8_t reason) {
    switch (reason) {
        case 1: return cs(CS::PETTAME_INVALIDCREATURE);
        case 2: return cs(CS::PETTAME_TOOMANY);
        case 3: return cs(CS::PETTAME_CREATUREALREADYOWNED);
        case 4: return cs(CS::PETTAME_NOTTAMEABLE);
        case 5: return cs(CS::PETTAME_ANOTHERSUMMONACTIVE);
        case 6: return cs(CS::PETTAME_UNITSCANTTAME);
        case 7: return cs(CS::PETTAME_NOPETAVAILABLE);
        case 8: return cs(CS::PETTAME_INTERNALERROR);
        case 9: return cs(CS::PETTAME_TOOHIGHLEVEL);
        case 10: return cs(CS::PETTAME_DEAD);
        case 11: return cs(CS::PETTAME_NOTDEAD);
        case 12: return cs(CS::PETTAME_CANTCONTROLEXOTIC);
        default: return cs(CS::PETTAME_UNKNOWNERROR);
    }
}


uint64_t fieldGuid(const game::Entity& e, uint16_t lo) {
    return static_cast<uint64_t>(e.getField(lo)) | (static_cast<uint64_t>(e.getField(static_cast<uint16_t>(lo + 1))) << 32);
}

}  // namespace

// --- State ---------------------------------------------------------------------------------------------------------

std::shared_ptr<game::Entity> WorldSession::petUnit() const {
    const uint64_t g = petGuid();
    if (g == 0) return nullptr;
    auto e = entities_.getEntity(g);
    return e && e->getType() == game::ObjectType::UNIT ? e : nullptr;
}

// PetHasActionBar: the player has a pet (upstream lua_unit_api.cpp PetHasActionBar = hasPet).
bool WorldSession::petHasActionBar() const { return petGuid() != 0; }

// A hunter's pet (the port's rule): the player is a hunter and has a pet.
bool WorldSession::petIsHunterPet() const { return petGuid() != 0 && getPlayerClass() == kClassHunter; }

// HasPetUI: has a pet (upstream lua_action_api.cpp HasPetUI); the second value (petstable.lua: isHunterPet) =
// petIsHunterPet.
bool WorldSession::hasPetUI(bool* hunterPet) const {
    const bool has = petGuid() != 0;
    if (hunterPet) *hunterPet = has && petIsHunterPet();
    return has;
}

bool WorldSession::petCanBeRenamed() const {
    auto pet = petUnit();
    return pet && fieldGuid(*pet, game::fieldIndex(game::UF::UNIT_FIELD_SUMMONEDBY_LO)) == playerGuid_ &&
           ((pet->getField(kFieldBytes2) >> 16) & 0x01);
}

bool WorldSession::petCanBeAbandoned() const {
    auto pet = petUnit();
    return pet && fieldGuid(*pet, game::fieldIndex(game::UF::UNIT_FIELD_SUMMONEDBY_LO)) == playerGuid_ &&
           ((pet->getField(kFieldBytes2) >> 16) & 0x02);
}

// PetCanBeDismissed: has a pet (upstream lua_unit_api.cpp).
bool WorldSession::petCanBeDismissed() const { return petGuid() != 0; }

// GetPetHappiness: a pet with a happiness power (UNIT_FIELD_MAXPOWER5 != 0: upstream lua_action_api.cpp
// GetPetHappiness answers nil otherwise); the level by PetPersonality 1's thresholds (pet_text).
bool WorldSession::petHappiness(int& level, int& damagePercent) const {
    auto pet = petUnit();
    if (!pet || pet->getField(kFieldMaxPower5) == 0) return false;
    const PetTextData& t = const_cast<WorldSession*>(this)->petText();
    level = t.happinessLevel(pet->getField(kFieldPower5));
    damagePercent = t.damagePercent(level);
    return true;
}

// GetPetExperience: the pet's PETEXPERIENCE / PETNEXTLEVELEXP (upstream lua_unit_api.cpp), 0, 0 without a pet.
bool WorldSession::petExperience(uint32_t& cur, uint32_t& next) const {
    cur = next = 0;
    auto pet = petUnit();
    if (!pet) return false;
    cur = pet->getField(game::fieldIndex(game::UF::UNIT_FIELD_PETEXPERIENCE));
    next = pet->getField(game::fieldIndex(game::UF::UNIT_FIELD_PETNEXTLEVELEXP));
    return true;
}

// GetPetActionSlotUsable: the slot holds an action (upstream: not empty) - a command, a reaction or a spell id.
bool WorldSession::petSlotUsable(int slot) const {
    const uint32_t s = petActionSlot(slot);
    const uint8_t t = petact::type(s);
    return petGuid() != 0 && (petact::id(s) != 0 || t == petact::kTypeCommand || t == petact::kTypeReaction);
}

float WorldSession::petSpellCooldown(uint32_t spellId, float* total) const {
    auto it = petCooldowns_.find(spellId);
    if (total) *total = 0.0f;
    if (it == petCooldowns_.end()) return 0.0f;
    const float elapsed = static_cast<float>(logUptimeMs() - it->second.first) / 1000.0f;
    const float rem = it->second.second - elapsed;
    if (rem <= 0.0f) return 0.0f;
    if (total) *total = it->second.second;
    return rem;
}

void WorldSession::petCooldown(uint32_t spellId, uint32_t ms) {
    if (spellId == 0) return;
    if (ms == 0) {
        petCooldowns_.erase(spellId);
        return;
    }
    // first = logUptimeMs at the start, second = the length (s).
    petCooldowns_[spellId] = {logUptimeMs(), static_cast<float>(ms) / 1000.0f};
}

// --- Senders -------------------------------------------------------------------------------------------------------

void WorldSession::castPetAction(int slot, uint64_t target) {
    const uint32_t s = petActionSlot(slot);
    if (s == 0 || petGuid() == 0) return;
    petAction(s, target, target != 0);
}

// CastPetAction's core: CMSG_PET_ACTION (pet guid, packed action, target guid; upstream sendPetAction), the
// server checks the target. A reaction / a stay / follow command shows at once in the mode (react in the low byte,
// command in the next: SMSG_PET_MODE's layout), as AzerothCore sends no SMSG_PET_MODE back.
void WorldSession::petAction(uint32_t packed, uint64_t target, bool targetGiven) {
    if (petGuid() == 0 || state_ != State::InWorld || !socket_) return;
    if (!targetGiven && target == 0) target = targetGuid_;  // the player's target
    const uint8_t type = petact::type(packed);
    const uint32_t id = petact::id(packed);
    // PSP: every pet spell as CMSG_PET_ACTION (no ground-targeting cursor for CMSG_PET_CAST_SPELL).
    if (type == petact::kTypeReaction) {
        petMode_ = (petMode_ & 0xFFFFFF00u) | (id & 0xFFu);
    } else if (type == petact::kTypeCommand && id < petact::kAttack) {
        petMode_ = (petMode_ & 0x080000FFu) | (id << 8);
    }
    ++petVersion_;  // PET_BAR_UPDATE
    socket_->send(petpkt::buildPetAction(game::wireOpcode(game::LogicalOpcode::CMSG_PET_ACTION), petGuid(), packed, target));
    if (++petLogs_ <= 200)
        PSP_LOGI(tag::Net, "pet_action data=%08X target=%08X", static_cast<unsigned>(packed),
                 static_cast<unsigned>(target & 0xFFFFFFFFu));
}

void WorldSession::petStopAttack() {
    if (!petAttacking_ || state_ != State::InWorld || !socket_) return;
    network::Packet pkt(game::wireOpcode(game::LogicalOpcode::CMSG_PET_STOP_ATTACK));
    pkt.writeUInt64(petGuid());
    socket_->send(pkt);
    petAttacking_ = false;
    ++petVersion_;
}

// CMSG_PET_SET_ACTION: the pet's guid, then (slot, data) pairs - the other slot of a swap first.
void WorldSession::petSendSetAction(int other, int slot) {
    if (state_ != State::InWorld || !socket_ || petGuid() == 0) return;
    const bool two = other >= 0 && other < kPetSlots;
    socket_->send(petpkt::buildPetSetAction(game::wireOpcode(game::LogicalOpcode::CMSG_PET_SET_ACTION), petGuid(),
                                            two ? other : -1, two ? petBar_[other] : 0, slot, petBar_[slot]));
    if (++petLogs_ <= 200)
        PSP_LOGI(tag::Net, "pet_set_action slot=%d data=%08X other=%d", slot, static_cast<unsigned>(petBar_[slot]), other);
}

// TogglePetAutocast: CMSG_PET_SPELL_AUTOCAST (pet guid, spell, u8 on) for an autocastable slot (bit 31), its bit 30
// flipped at once with the spell list - upstream togglePetSpellAutocast; AzerothCore HandlePetSpellAutocastOpcode.
void WorldSession::togglePetAutocast(int slot) {
    if (slot < 0 || slot >= kPetSlots || petGuid() == 0 || state_ != State::InWorld || !socket_) return;
    uint32_t& s = petBar_[slot];
    if (!(s & petact::kAutocastable) || petact::id(s) == 0) return;
    s ^= petact::kAutocastOn;
    for (uint32_t& sp : petSpellList_)
        if ((sp & 0x3FFFFFFFu) == (s & 0x3FFFFFFFu)) sp = s;
    const uint16_t wire = game::wireOpcode(game::LogicalOpcode::CMSG_PET_SPELL_AUTOCAST);
    if (wire != 0xFFFF) {
        network::Packet pkt(wire);
        pkt.writeUInt64(petGuid());
        pkt.writeUInt32(petact::id(s));
        pkt.writeUInt8((s & petact::kAutocastOn) ? 1 : 0);
        socket_->send(pkt);
    }
    ++petVersion_;
    if (++petLogs_ <= 200)
        PSP_LOGI(tag::Net, "pet_autocast spell=%u on=%d", static_cast<unsigned>(petact::id(s)),
                 (s & petact::kAutocastOn) ? 1 : 0);
}

// PickupPetAction: the slot's action goes on the cursor; the bar stays as it is until the drop.
uint32_t WorldSession::petPickup(int slot) {
    if (slot < 0 || slot >= kPetSlots || petGuid() == 0) return 0;
    const uint32_t s = petBar_[slot];
    const uint8_t t = petact::type(s);
    if (petact::id(s) == 0 && t != petact::kTypeCommand && t != petact::kTypeReaction) return 0;  // an empty slot
    petPickFrom_ = slot;
    return s;
}

// The drop on a pet button: the picked-up slot and this one swap (one CMSG_PET_SET_ACTION with both pairs); a drop
// back on its own slot puts it back. Returns what stays on the cursor (`data`: refused, 0: done).
uint32_t WorldSession::petPlace(int slot, uint32_t data) {
    if (slot < 0 || slot >= kPetSlots || petGuid() == 0) return data;
    const int from = petPickFrom_;
    if (from < 0 || from >= kPetSlots || petBar_[from] != data) return data;
    petPickFrom_ = -1;
    if (from == slot) return 0;
    petBar_[from] = petBar_[slot];
    petBar_[slot] = data;
    petSendSetAction(from, slot);
    ++petVersion_;
    return 0;
}

// PetDismiss: CMSG_PET_ACTION with COMMAND_ABANDON (0x07000003) and no target (upstream dismissPet),
// PET_DISMISS_START.
void WorldSession::petDismiss() {
    if (!petCanBeDismissed() || state_ != State::InWorld || !socket_) return;
    socket_->send(petpkt::buildPetAction(game::wireOpcode(game::LogicalOpcode::CMSG_PET_ACTION), petGuid(),
                                         petact::pack(petact::kTypeCommand, petact::kDismiss), 0));
    petEvents_.push_back(PetEvent{PetEvent::DismissStart, 10000});
    PSP_LOGI(tag::Net, "pet_dismiss");
}

// PetAbandon: CMSG_PET_ABANDON (the pet's guid) - upstream abandonPet.
void WorldSession::petAbandon() {
    if (petGuid() == 0 || state_ != State::InWorld || !socket_) return;
    network::Packet pkt(game::wireOpcode(game::LogicalOpcode::CMSG_PET_ABANDON));
    pkt.writeUInt64(petGuid());
    socket_->send(pkt);
    PSP_LOGI(tag::Net, "pet_abandon");
}

// PetRename: CMSG_PET_RENAME = guid, name, u8 declined (0 here, see the file comment) - upstream renamePet; the
// server answers a bad name with SMSG_PET_NAME_INVALID. The name is asked again after it (the port's rule: the
// server takes the two in order, the answer carries the new name). The menu offers it only for PetCanBeRenamed.
void WorldSession::petRename(const std::string& name) {
    if (petGuid() == 0 || name.empty() || state_ != State::InWorld || !socket_) return;
    socket_->send(petpkt::buildPetRename(game::wireOpcode(game::LogicalOpcode::CMSG_PET_RENAME), petGuid(), name));  // PSP: no declined names
    petNameWanted_ = true;
    PSP_LOGI(tag::Net, "pet_rename len=%u", static_cast<unsigned>(name.size()));
}

bool WorldSession::takePetEvent(PetEvent& out) {
    if (petEvents_.empty()) return false;
    out = petEvents_.front();
    petEvents_.pop_front();
    return true;
}

void WorldSession::petLine(const std::string& text, GameLine kind) { gameLine(kind, text); }

// CMSG_PET_NAME_QUERY: the pet number (UNIT_FIELD_PETNUMBER) and the guid; sent once a wanted name's pet is in sight
// with its number (wanted: SMSG_PET_SPELLS named a pet, a rename went out).
void WorldSession::petQueryName() {
    if (!petNameWanted_) return;
    auto pet = petUnit();
    if (!pet || !socket_ || state_ != State::InWorld) return;
    const uint32_t number = pet->getField(kFieldPetNumber);
    if (number == 0) return;
    petNameWanted_ = false;
    network::Packet pkt(game::wireOpcode(game::LogicalOpcode::CMSG_PET_NAME_QUERY));
    pkt.writeUInt32(number);
    pkt.writeUInt64(petGuid());
    socket_->send(pkt);
}

// --- Talents -------------------------------------------------------------------------------------------------------

// The pet's talent tab: the TalentTab whose PetTalentMask has the bit of the family's PetTalentType, its
// talents (spells.pak "talents", as loadInspectTalents reads them).
bool WorldSession::loadPetTalents() {
    uint32_t family = petFamily_;
    if (family == 0)
        if (auto pet = petUnit()) family = creatureFamilyOf(pet->getField(game::fieldIndex(game::UF::OBJECT_FIELD_ENTRY)));
    const PetFamilyRec* f = family ? petText().family(family) : nullptr;
    const int32_t type = f && petIsHunterPet() ? f->talentType : -1;
    if (type == petTalentType_) return petTalentTab_.tabId != 0;
    petTalentType_ = type;
    petTalentTab_ = game::TalentTabEntry{};
    petTalentList_.clear();
    std::vector<uint8_t> raw;
    if (type < 0 || type > 31 || !spellDb_ || !spellDb_->loaded() || !spellDb_->hasTalents() || !spellDb_->readTalents(raw) ||
        raw.size() < sizeof(TalentPakHeader))
        return false;
    TalentPakHeader h;
    std::memcpy(&h, raw.data(), sizeof(h));
    const size_t tabBytes = static_cast<size_t>(h.tabCount) * sizeof(TalentTabRec);
    const size_t talBytes = static_cast<size_t>(h.talentCount) * sizeof(TalentRec);
    if (std::memcmp(h.magic, "WTAL", 4) != 0 || h.version != 1 || h.tabRecSize != sizeof(TalentTabRec) ||
        h.talentRecSize != sizeof(TalentRec) || raw.size() < sizeof(h) + tabBytes + talBytes + 1)
        return false;
    const uint8_t* tabsP = raw.data() + sizeof(h);
    const uint8_t* talsP = tabsP + tabBytes;
    const char* strings = reinterpret_cast<const char*>(talsP + talBytes);
    const size_t strBytes = raw.size() - 1 - sizeof(h) - tabBytes - talBytes;
    auto str = [&](uint32_t off) { return off < strBytes ? std::string(strings + off) : std::string(); };
    for (uint32_t i = 0; i < h.tabCount && petTalentTab_.tabId == 0; ++i) {
        TalentTabRec r;
        std::memcpy(&r, tabsP + i * sizeof(TalentTabRec), sizeof(r));
        if (r.id == 0 || !(r.petMask & (1u << type))) continue;
        petTalentTab_.tabId = r.id;
        petTalentTab_.name = str(r.nameOff);
        petTalentTab_.classMask = r.classMask;
        petTalentTab_.orderIndex = r.orderIndex;
        petTalentTab_.backgroundFile = str(r.bgOff);
    }
    if (petTalentTab_.tabId == 0) return false;
    for (uint32_t i = 0; i < h.talentCount; ++i) {
        TalentRec r;
        std::memcpy(&r, talsP + i * sizeof(TalentRec), sizeof(r));
        if (r.id == 0 || r.tabId != petTalentTab_.tabId) continue;
        game::TalentEntry e;
        e.talentId = r.id;
        e.tabId = r.tabId;
        e.row = r.row;
        e.column = r.column;
        for (int k = 0; k < 5; ++k) e.rankSpells[k] = r.rankSpell[k];
        for (int p = 0; p < 3; ++p) {
            e.prereqTalent[p] = r.prereqTalent[p];
            e.prereqRank[p] = r.prereqRank[p];
        }
        for (int k = 0; k < 5; ++k)
            if (e.rankSpells[k] != 0) e.maxRank = static_cast<uint8_t>(k + 1);
        petTalentList_.push_back(e);
    }
    std::vector<uint32_t> ids;
    for (const auto& t : petTalentList_) ids.push_back(t.rankSpells[0]);
    if (!ids.empty()) spellDb_->prefetch(ids);
    PSP_LOGI(tag::Net, "pet_talents_load family=%u type=%d tab=%u talents=%u", static_cast<unsigned>(family), static_cast<int>(type),
             static_cast<unsigned>(petTalentTab_.tabId), static_cast<unsigned>(petTalentList_.size()));
    return true;
}

const game::TalentEntry* WorldSession::petTalentEntry(uint32_t talentId) const {
    for (const auto& t : petTalentList_)
        if (t.talentId == talentId) return &t;
    return nullptr;
}

uint8_t WorldSession::petTalentRank(uint32_t talentId) const {
    auto it = petTalents_.find(talentId);
    return it == petTalents_.end() ? 0 : it->second;
}

// LearnTalent(tab, talent, true): CMSG_PET_LEARN_TALENT = the pet's guid, the talent, the requested rank
// counted from zero (the current rank's index + 1).
void WorldSession::learnPetTalent(uint32_t talentId) {
    if (petGuid() == 0 || state_ != State::InWorld || !socket_ || !petTalentEntry(talentId)) return;
    socket_->send(petpkt::buildPetLearnTalent(game::wireOpcode(game::LogicalOpcode::CMSG_PET_LEARN_TALENT), petGuid(), talentId,
                                              petTalentRank(talentId)));
    PSP_LOGI(tag::Net, "pet_talent_learn id=%u rank=%u", static_cast<unsigned>(talentId),
             static_cast<unsigned>(petTalentRank(talentId)));
}

const PetTextData& WorldSession::petText() {
    if (petTextTried_) return petText_;
    petTextTried_ = true;
    char path[256];
    fsPath(path, sizeof(path), "data/text.pak");
    Pak pak;
    if (pak.open(path)) {
        const PakEntry* e = pak.find("pet_text");
        void* blob = e ? pak.load(e) : nullptr;
        if (blob) {
            petText_.parse(static_cast<const uint8_t*>(blob), e->size);
            pakFree(blob);
        }
        pak.close();
    }
    PSP_LOGI(tag::Fs, "pet_text ok=%d families=%u foods=%u slots=%u", petText_.loaded ? 1 : 0,
             static_cast<unsigned>(petText_.families.size()), static_cast<unsigned>(petText_.foods.size()),
             static_cast<unsigned>(petText_.slotCost.size()));
    return petText_;
}

uint32_t WorldSession::creatureFamilyOf(uint32_t entry) {
    if (entry == 0) return 0;
    if (const auto* info = creatureInfo(entry)) return info->family;
    queryCreature(entry, 0);
    return 0;
}

// --- Stable --------------------------------------------------------------------------------------------------------

void WorldSession::stableRequestList() {
    if (stableMaster_ == 0 || state_ != State::InWorld || !socket_) return;
    network::Packet pkt(game::wireOpcode(game::LogicalOpcode::MSG_LIST_STABLED_PETS));
    pkt.writeUInt64(stableMaster_);
    socket_->send(pkt);
}

int WorldSession::stableSelected() const {
    if (stableSelNumber_ == -1) return -1;
    if (stableSelNumber_ != 0)
        for (size_t i = 0; i < stabled_.size(); ++i)
            if (stabled_[i].petNumber == static_cast<uint32_t>(stableSelNumber_)) return static_cast<int>(i);
    return -2;
}

void WorldSession::stableSelect(int index) {
    if (index == -1) stableSelNumber_ = -1;
    else if (index >= 0 && index < static_cast<int>(stabled_.size())) stableSelNumber_ = stabled_[index].petNumber;
    else stableSelNumber_ = 0;
    ++stableVersion_;
}

// ClickStablePet with the PSP's cursor: *cursor = -2 none, -1 the active pet, k the k-th stabled pet. A drop sends
// the AzerothCore request that does that move (NPCHandler.cpp HandleStablePet / HandleUnstablePet /
// HandleStableSwapPet).
bool WorldSession::stableClick(int index, int* cursor) {
    const int held = cursor ? *cursor : -2;
    if (held == -2) {  // no stable pet on the cursor: select
        stableSelect(index);
        return true;
    }
    if (cursor) *cursor = -2;  // ClearCursor in every other branch
    if (held == index || stableMaster_ == 0 || state_ != State::InWorld || !socket_) return false;
    auto send = [&](game::LogicalOpcode op, bool withNumber, uint32_t number) {
        socket_->send(petpkt::buildStable(game::wireOpcode(op), stableMaster_, withNumber, number));
        PSP_LOGI(tag::Net, "stable_send op=%u pet=%u", static_cast<unsigned>(game::wireOpcode(op)),
                 static_cast<unsigned>(number));
    };
    if (held == -1 && index >= 0) {  // the active pet dropped on a stable slot
        if (index < static_cast<int>(stabled_.size()) && stabled_[index].petNumber != 0)
            send(game::LogicalOpcode::CMSG_STABLE_SWAP_PET, true, stabled_[index].petNumber);
        else if (static_cast<uint32_t>(index) < stableSlots_)
            send(game::LogicalOpcode::CMSG_STABLE_PET, false, 0);
    } else if (held >= 0 && index == -1 && held < static_cast<int>(stabled_.size()) && stabled_[held].petNumber != 0) {
        // a stabled pet dropped on the active slot: swap with the pet out, else take it out
        send(petGuid() != 0 ? game::LogicalOpcode::CMSG_STABLE_SWAP_PET : game::LogicalOpcode::CMSG_UNSTABLE_PET, true,
             stabled_[held].petNumber);
    }
    return false;
}

uint32_t WorldSession::stableNextSlotCost() { return petText().stableSlotCost(stableSlots_ + 1); }

// BuyStableSlot's checks: below MAX_PET_STABLES (4), a price for the next slot, money for it (AzerothCore
// NPCHandler.cpp HandleBuyStableSlot).
bool WorldSession::stableCanBuySlot() {
    const uint32_t cost = stableNextSlotCost();
    return stableMaster_ != 0 && stableSlots_ != 4 && cost != 0 && cost <= getMoneyCopper();
}

void WorldSession::stableBuySlot() {
    if (!stableCanBuySlot() || state_ != State::InWorld || !socket_) return;
    socket_->send(petpkt::buildStable(game::wireOpcode(game::LogicalOpcode::CMSG_BUY_STABLE_SLOT), stableMaster_, false, 0));
    PSP_LOGI(tag::Net, "stable_buy slots=%u cost=%u", static_cast<unsigned>(stableSlots_),
             static_cast<unsigned>(stableNextSlotCost()));
}

void WorldSession::stableClose() {
    if (stableMaster_ == 0) return;
    stableMaster_ = 0;
    petEvents_.push_back(PetEvent{PetEvent::StableClosed, 0});
    ++stableVersion_;
    PSP_LOGI(tag::Net, "stable_close");
}

// MSG_LIST_STABLED_PETS: the stable master, u8 pets, u8 slots, per pet u32 number, u32 entry, u32 level,
// the name, u8 flag - the one without flag 2 is the active pet (record 0), the rest in list order.
void WorldSession::handleListStabledPets(network::Packet& packet) {
    petpkt::StableList list;
    if (!petpkt::parseStableList(packet, list)) return;
    stableMaster_ = list.master;
    stableSlots_ = list.slots;
    stableSelNumber_ = 0;  // none: PetStable_Update picks the first
    auto conv = [&](const petpkt::StableRec& r) {
        StablePet p;
        p.petNumber = r.petNumber;
        p.entry = r.entry;
        p.level = r.level;
        p.name = r.name;
        p.flag = r.flag;
        creatureFamilyOf(r.entry);  // the family name / icon / talent tree of GetStablePetInfo
        return p;
    };
    stableCur_ = list.current.petNumber ? conv(list.current) : StablePet{};
    stabled_.clear();
    for (const auto& r : list.stabled) stabled_.push_back(conv(r));
    petEvents_.push_back(PetEvent{PetEvent::StableShow, 0});
    ++stableVersion_;
    PSP_LOGI(tag::Net, "stable_list master=%08X pets=%u slots=%u cur=%u", static_cast<unsigned>(stableMaster_ & 0xFFFFFFFFu),
             static_cast<unsigned>(stabled_.size() + (stableCur_.petNumber ? 1 : 0)), static_cast<unsigned>(stableSlots_),
             static_cast<unsigned>(stableCur_.petNumber));
}

void WorldSession::petsUpdate(float dtSec, const glm::vec3& selfPos) {
    (void)dtSec;
    petQueryName();
    if (stableMaster_ != 0) {
        if (auto npc = entities_.getEntity(stableMaster_)) {
            const glm::vec3 p = core::coords::canonicalToServer(glm::vec3(npc->getX(), npc->getY(), npc->getZ()));
            const float dx = selfPos.x - p.x, dy = selfPos.y - p.y;
            if (std::sqrt(dx * dx + dy * dy) > game::NPC_INTERACT_MAX_DISTANCE) {
                PSP_LOGI(tag::Net, "stable_far dist=%.1f", std::sqrt(dx * dx + dy * dy));
                stableClose();
            }
        }
    }
    // The pet's unit gone: its attack is over too (PET_ATTACK_STOP).
    if (petAttacking_ && !petUnit()) {
        petAttacking_ = false;
        ++petVersion_;
    }
}

// --- Packets -------------------------------------------------------------------------------------------------------

// a new pet (or none): not attacking until SMSG_ATTACKSTART says so, its name wanted, UNIT_PET; an open stable asks
// its list again (the active pet changed).
void WorldSession::petSetGuid(uint64_t guid) {
    const bool changed = guid != petGuid();
    if (guid == 0) petGuids_.clear();
    else if (petGuids_.empty()) petGuids_.push_back(guid);
    else petGuids_[0] = guid;
    if (!changed) return;
    petAttacking_ = false;
    petName_.clear();
    petNameWanted_ = guid != 0;
    petPickFrom_ = -1;
    petTalentType_ = -2;
    if (stableMaster_ != 0) stableRequestList();
    ++petVersion_;
}

// SMSG_PET_SPELLS.
void WorldSession::handlePetSpells(network::Packet& packet) {
    std::fill(std::begin(petBar_), std::end(petBar_), 0u);
    petSpellList_.clear();
    petCooldowns_.clear();
    petpkt::PetSpells d;
    if (!petpkt::parsePetSpells(packet, d) || d.guid == 0) {
        petFamily_ = 0;
        petMode_ = 0;
        petSetGuid(0);
        PSP_LOGI(tag::Net, "pet_spells none");
        return;
    }
    petFamily_ = d.family;
    petDurationMs_ = d.duration;
    petMode_ = d.mode;
    std::copy(std::begin(d.bar), std::end(d.bar), std::begin(petBar_));
    petSpellList_ = d.spells;
    // AzerothCore PetSpellInitialize fills one of the two: the category cooldown for a spell with a category.
    for (const auto& c : d.cooldowns) petCooldown(c.spell, c.category ? c.categoryCooldown : c.cooldown);
    petSetGuid(d.guid);
    petNameWanted_ = true;  // upstream handlePetSpells: requestPetName
    ++petVersion_;
    std::vector<uint32_t> ids;
    for (uint32_t s : petBar_)
        if (petact::isSpell(s) && petact::id(s)) ids.push_back(petact::id(s));
    if (spellDb_ && !ids.empty()) spellDb_->prefetch(ids);
    PSP_LOGI(tag::Net, "pet_spells guid=%08X family=%u mode=%08X spells=%u cds=%u", static_cast<unsigned>(d.guid & 0xFFFFFFFFu),
             static_cast<unsigned>(petFamily_), static_cast<unsigned>(petMode_), static_cast<unsigned>(d.spells.size()),
             static_cast<unsigned>(d.cooldowns.size()));
}

bool WorldSession::handlePetPacket(game::LogicalOpcode op, network::Packet& packet) {
    using game::LogicalOpcode;
    switch (op) {
        case LogicalOpcode::SMSG_PET_SPELLS: handlePetSpells(packet); return true;
        case LogicalOpcode::SMSG_PET_MODE: {  // guid, u32 mode - for our pet
            if (!packet.hasRemaining(12)) return true;
            const uint64_t g = packet.readUInt64();
            const uint32_t mode = packet.readUInt32();
            if (g != 0 && g == petGuid()) {
                petMode_ = mode;
                ++petVersion_;
            }
            return true;
        }
        case LogicalOpcode::SMSG_PET_GUIDS: {  // u32 count, guids
            if (!packet.hasRemaining(4)) return true;
            const uint32_t n = std::min<uint32_t>(packet.readUInt32(), 16);
            std::vector<uint64_t> list;
            for (uint32_t i = 0; i < n && packet.hasRemaining(8); ++i) list.push_back(packet.readUInt64());
            const uint64_t first = list.empty() ? 0 : list[0];
            if (first != petGuid()) petSetGuid(first);
            petGuids_ = std::move(list);
            ++petVersion_;
            return true;
        }
        case LogicalOpcode::SMSG_PET_ACTION_FEEDBACK: {  // AzerothCore PetActionFeedback 1..3
            if (!packet.hasRemaining(1)) return true;
            const uint8_t msg = packet.readUInt8();
            // upstream CombatHandler::handlePetActionFeedback shows these in the chat
            if (msg == 1) petLine(cs(CS::ERR_PET_SPELL_DEAD), GameLine::News);              // FEEDBACK_PET_DEAD
            else if (msg == 2) petLine(cs(CS::ERR_NO_ATTACK_TARGET), GameLine::News);       // FEEDBACK_NOTHING_TO_ATT
            else if (msg == 3) petLine(cs(CS::ERR_INVALID_ATTACK_TARGET), GameLine::News);  // FEEDBACK_CANT_ATT_TARGET
            packet.skipAll();
            return true;
        }
        case LogicalOpcode::SMSG_PET_BROKEN: petLine(cs(CS::ERR_PET_BROKEN), GameLine::News); return true;  // upstream: chat
        case LogicalOpcode::SMSG_PET_RENAMEABLE:  // PET_RENAMEABLE -> StaticPopup RENAME_PET
            petEvents_.push_back(PetEvent{PetEvent::Renameable, 0});
            return true;
        case LogicalOpcode::SMSG_PET_LEARNED_SPELL:
        case LogicalOpcode::SMSG_PET_UNLEARNED_SPELL: {
            if (!packet.hasRemaining(4)) return true;
            const uint32_t spell = packet.readUInt32();
            const SpellInfo* info = spellInfo(spell);
            if (!info) return true;
            const std::string name = info->rank.empty() ? info->name : info->name + " (" + info->rank + ")";
            if (op == LogicalOpcode::SMSG_PET_UNLEARNED_SPELL) petLine(csf(CS::ERR_PET_SPELL_UNLEARNED_S, name), GameLine::News);
            else petLine(csf(CS::ERR_PET_LEARN_SPELL_S, name), GameLine::News);  // upstream: a chat line for a learned spell
            return true;
        }
        case LogicalOpcode::SMSG_PET_CAST_FAILED: {  // every result: its SPELL_FAILED_* text (upstream handlePetCastFailed)
            if (!packet.hasRemaining(6)) return true;
            packet.readUInt8();  // cast count
            const uint32_t spell = packet.readUInt32();
            const uint8_t result = packet.readUInt8();
            packet.skipAll();
            const char* t = spellDb_ ? spellDb_->castFailText(result) : nullptr;
            if (!t) t = game::getSpellCastResultString(result);
            if (t && *t) petLine(t, GameLine::Refused);
            if (++petLogs_ <= 200)
                PSP_LOGI(tag::Net, "pet_cast_failed spell=%u result=%u", static_cast<unsigned>(spell), static_cast<unsigned>(result));
            return true;
        }
        case LogicalOpcode::SMSG_PET_TAME_FAILURE: {  // ERR_TAME_FAILED "%s."
            const uint8_t reason = packet.hasRemaining(1) ? packet.readUInt8() : 0;
            petLine(csf(CS::ERR_TAME_FAILED, tameText(reason)), GameLine::Refused);
            return true;
        }
        case LogicalOpcode::SMSG_PET_NAME_QUERY_RESPONSE: {  // u32 pet number, name, u32 timestamp, declined names
            if (!packet.hasRemaining(4)) return true;
            const uint32_t number = packet.readUInt32();
            const std::string name = packet.readString();
            packet.skipAll();
            auto pet = petUnit();
            if (pet && number != 0 && pet->getField(kFieldPetNumber) == number && !name.empty()) {
                petName_ = name;
                ++petVersion_;  // UNIT_NAME_UPDATE("pet")
                if (auto u = std::dynamic_pointer_cast<game::Unit>(pet)) u->setName(name);
                PSP_LOGI(tag::Net, "pet_name number=%u", static_cast<unsigned>(number));
            }
            return true;
        }
        case LogicalOpcode::SMSG_PET_NAME_INVALID: {  // u32 error, name, u8 declined [5 names]
            if (!packet.hasRemaining(4)) return true;
            const uint32_t err = packet.readUInt32();
            packet.readString();
            const uint8_t declined = packet.hasRemaining(1) ? packet.readUInt8() : 0;
            packet.skipAll();
            // PSP: one generic text for every name error (the port's own string).
            petLine(psf(PS::PET_NAME_INVALID, static_cast<unsigned>(err)), GameLine::Refused);
            if (declined) petEvents_.push_back(PetEvent{PetEvent::ForceDeclension, err});
            return true;
        }
        case LogicalOpcode::MSG_LIST_STABLED_PETS: handleListStabledPets(packet); return true;
        case LogicalOpcode::SMSG_STABLE_RESULT: {  // AzerothCore StableResultCode; every result asks the list again
            const uint8_t r = packet.hasRemaining(1) ? packet.readUInt8() : 0;
            PSP_LOGI(tag::Net, "stable_result r=%u", static_cast<unsigned>(r));
            if (r == 1) petLine(cs(CS::ERR_NOT_ENOUGH_MONEY), GameLine::Refused);                   // STABLE_ERR_MONEY
            else if (r == 12) petLine(csf(CS::ERR_TAME_FAILED, tameText(12)), GameLine::Refused); // STABLE_ERR_EXOTIC
            stableRequestList();
            ++stableVersion_;
            return true;
        }
        // Peeked: the pet's cooldowns and attack state, its talents - the rest goes on to its own handler.
        // read in place and the read position put back (petpkt::peek*) - no copy of the whole
        // packet for every SPELL_COOLDOWN / COOLDOWN_EVENT / ATTACKSTART / ATTACKSTOP of a busy place; no pet - no
        // read at all.
        case LogicalOpcode::SMSG_SPELL_COOLDOWN: {  // guid, u8 flags, {u32 spell, u32 ms}: our pet's
            uint64_t g = 0;
            if (petGuid() == 0 || !packet.hasRemaining(9) || !petpkt::peekGuid(packet, g) || g != petGuid()) return false;
            packet.readUInt64();
            packet.readUInt8();
            while (packet.hasRemaining(8)) {
                const uint32_t spell = packet.readUInt32();
                petCooldown(spell, packet.readUInt32());  // 0 ms: no timer shown
            }
            ++petVersion_;  // PET_BAR_UPDATE_COOLDOWN
            return true;
        }
        case LogicalOpcode::SMSG_COOLDOWN_EVENT: {  // u32 spell, guid - our pet's
            uint32_t spell = 0;
            uint64_t g = 0;
            if (petGuid() == 0 || !petpkt::peekSpellGuid(packet, spell, g) || g != petGuid()) return false;
            packet.skipAll();
            petCooldowns_.erase(spell);
            ++petVersion_;
            return true;
        }
        case LogicalOpcode::SMSG_CLEAR_COOLDOWN: {
            // u32 spell, guid (Pet::RemoveSpellCooldown sends the pet's): the pet bar's copy goes here, the packet
            // goes on to the player's cooldowns (upstream handleClearCooldown).
            uint32_t spell = 0;
            uint64_t g = 0;
            if (petGuid() == 0 || !petpkt::peekSpellGuid(packet, spell, g) || g != petGuid()) return false;
            if (petCooldowns_.erase(spell)) ++petVersion_;
            return false;
        }
        case LogicalOpcode::SMSG_ATTACKSTART: {  // PET_ATTACK_START: our pet attacks
            uint64_t g = 0;
            if (petGuid() != 0 && packet.hasRemaining(16) && petpkt::peekGuid(packet, g) && g == petGuid()) {
                petAttacking_ = true;
                ++petVersion_;
            }
            return false;
        }
        case LogicalOpcode::SMSG_ATTACKSTOP: {  // PET_ATTACK_STOP
            uint64_t g = 0;
            if (petGuid() != 0 && petpkt::peekPackedGuid(packet, g) && g == petGuid()) {
                petAttacking_ = false;
                ++petVersion_;
            }
            return false;
        }
        case LogicalOpcode::SMSG_TALENTS_INFO: {  // u8 type 1 = the pet's
            uint8_t type = 0;  // peeked in place - only the pet's (type 1) is read on
            if (!petpkt::peekU8(packet, type) || type != 1) return false;
            network::Packet& p = packet;
            p.readUInt8();
            if (!p.hasRemaining(5)) return true;
            petTalentPoints_ = p.readUInt32();
            const uint8_t n = p.readUInt8();
            petTalents_.clear();
            for (uint8_t i = 0; i < n && p.hasRemaining(5); ++i) {
                const uint32_t id = p.readUInt32();
                const uint8_t rank = p.readUInt8();
                petTalents_[id] = static_cast<uint8_t>(rank + 1u);  // the wire counts from zero
            }
            ++petTalentVersion_;
            PSP_LOGI(tag::Net, "pet_talents points=%u learned=%u", static_cast<unsigned>(petTalentPoints_),
                     static_cast<unsigned>(petTalents_.size()));
            return true;
        }
        case LogicalOpcode::SMSG_TALENTS_INVOLUNTARILY_RESET: {  // u8 1 = the pet's: TALENTS_INVOLUNTARILY_RESET_PET
            uint8_t type = 0;  // peeked in place, as SMSG_TALENTS_INFO
            if (!petpkt::peekU8(packet, type) || type != 1) return false;
            packet.skipAll();
            petEvents_.push_back(PetEvent{PetEvent::TalentsReset, 1});
            return true;
        }
        default: return false;
    }
}

}  // namespace wowee::platform::psp
