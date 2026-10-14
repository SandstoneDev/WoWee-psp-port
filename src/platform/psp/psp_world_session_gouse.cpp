// Using a game object: the port's rules, taken from upstream WoWee where it has them and from AzerothCore's
// public handlers otherwise. Upstream's packet builders and parsers are used as they are (GameObjectUsePacket,
// GameObjectQueryPacket / GameObjectQueryResponseParser, PageTextQueryPacket / PageTextQueryResponseParser,
// CastSpellPacket::buildGameObjectTarget, UseItemPacket's field order).
//
//   the checks         own rule, in this order: within reach (ERR_USE_TOO_FAR), alive (ERR_PLAYER_DEAD), a chest's
//                      notInCombat field while in combat (ERR_NOT_IN_COMBAT; AzerothCore GameObjectData.h
//                      chest.notInCombat), the LOCKED flag with nothing that opens it (the type's locked error);
//                      everything else is the server's to refuse
//   the lock           upstream planGameObjectOpen: a known OPEN_LOCK spell (effect 33, MiscValue = the lock type)
//                      is cast at the object first, else a key in the bags is used on it (its on-use spell, the object
//                      as target); a spell case only when the spell is known (upstream: "we never fabricate a cast").
//                      Skill against requirement as AzerothCore Spell::CanOpenLock: the player's skill of the lock
//                      type's profession (SkillByLockType) against the case's Skill, none needed for other types
//   nothing opens it   own rule: a known spell short on skill says ERR_USE_LOCKED_WITH_SPELL_KNOWN_SI, else the first
//                      case that applies now words the line (_WITH_ITEM_S / _WITH_SPELL_S / ERR_USE_CANT_OPEN); a lock
//                      with no case that applies in the object's state does not stop the use
//   the packets        upstream performGameObjectInteractionNow: CMSG_GAMEOBJ_USE for every plain use (text, mailbox
//                      and guild vault included), then the type's frame (ItemTextFrame / the mail frame /
//                      CMSG_GUILD_BANKER_ACTIVATE); CMSG_GAMEOBJ_REPORT_USE after a plain use except mailbox / vault
//   mounted            upstream dismounts before a use; the object's allowMounted field keeps the mount (AzerothCore
//                      GameObjectTemplate::IsUsableMounted: questgiver, text, goober, spellcaster)
//   meeting stone      AzerothCore GameObject::Use MEETINGSTONE: the selection a member of the group, both levels at
//                      least the stone's minLevel (ERR_MEETING_STONE_* lines), then the plain use
// The data: ui.pak "go_lock" (Lock.dbc, LockType.dbc, the OPEN_LOCK spells - tools/psp/build_ui_pak.py).
//
// PSP: the reach test is the world screen's (the model's bounding sphere - gameobjects.pak keeps the sphere);
// no click-to-move when too far: the red line.
#include "platform/psp/psp_world_session.hpp"

#include "game/inventory_slots.hpp"
#include "game/protocol_constants.hpp"
#include "game/update_field_table.hpp"
#include "game/world_packets.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_go_parse.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_strings.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_spell_db.hpp"
#include "rendering/psp/psp_gameobject_format.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace wowee::platform::psp {

using game::LogicalOpcode;
namespace rp = wowee::rendering::psp;

namespace {

unsigned lo32(uint64_t guid) { return static_cast<unsigned>(guid & 0xFFFFFFFFu); }

// 3.3.5a UNIT descriptor index the upstream table does not name (AzerothCore UpdateFields.h UNIT_FIELD_CHANNEL_OBJECT).
constexpr uint16_t kUnitChannelObject = 0x14;
constexpr uint32_t kUnitFlagInCombat = 0x80000;  // UNIT_FLAG_IN_COMBAT (AzerothCore UnitDefines.h)
constexpr uint32_t kSpellMeetingStoneSummon = 23598;  // what the server casts (AzerothCore GameObject::Use)
constexpr size_t kMaxQueue = 16;

// The profession a lock type's skill comes from: AzerothCore SharedDefines.h SkillByLockType (LOCKTYPE_PICKLOCK,
// _HERBALISM, _MINING, _FISHING, _INSCRIPTION); 0 = the type needs no skill.
uint32_t skillByLockType(uint32_t lockType) {
    switch (lockType) {
        case 1: return 633;   // SKILL_LOCKPICKING
        case 2: return 182;   // SKILL_HERBALISM
        case 3: return 186;   // SKILL_MINING
        case 19: return 356;  // SKILL_FISHING
        case 20: return 773;  // SKILL_INSCRIPTION
        default: return 0;
    }
}

// GlobalStrings keys of the lines this file raises - strings.pak.
constexpr CS kErrPlayerDead = CS::ERR_PLAYER_DEAD;
constexpr CS kErrNotInCombat = CS::ERR_NOT_IN_COMBAT;
constexpr CS kErrTooFar = CS::ERR_USE_TOO_FAR;
constexpr CS kErrUseLocked = CS::ERR_USE_LOCKED;
constexpr CS kErrDoorLocked = CS::ERR_DOOR_LOCKED;
constexpr CS kErrButtonLocked = CS::ERR_BUTTON_LOCKED;
constexpr CS kErrCantOpen = CS::ERR_USE_CANT_OPEN;
constexpr CS kErrLockedWithItem = CS::ERR_USE_LOCKED_WITH_ITEM_S;
constexpr CS kErrLockedWithSpell = CS::ERR_USE_LOCKED_WITH_SPELL_S;
constexpr CS kErrLockedWithSpellKnown = CS::ERR_USE_LOCKED_WITH_SPELL_KNOWN_SI;
constexpr CS kErrStoneNeedParty = CS::ERR_MEETING_STONE_NEED_PARTY;
constexpr CS kErrStoneLevel = CS::ERR_MEETING_STONE_INVALID_LEVEL;
constexpr CS kErrStoneNotInParty = CS::ERR_MEETING_STONE_TARGET_NOT_IN_PARTY;
constexpr CS kErrStoneTargetLevel = CS::ERR_MEETING_STONE_TARGET_INVALID_LEVEL;
constexpr CS kErrFishNotHooked = CS::ERR_FISH_NOT_HOOKED;
constexpr CS kErrFishEscaped = CS::ERR_FISH_ESCAPED;
constexpr CS kErrNotBarberSitting = CS::ERR_NOT_BARBER_SITTING;
constexpr CS kErrNotEnoughMoney = CS::ERR_NOT_ENOUGH_MONEY;
constexpr CS kSpellInProgress = CS::SPELL_FAILED_SPELL_IN_PROGRESS;

}  // namespace

// ---- The baked tables (ui.pak "go_lock") ----

struct WorldSession::GoLockData {
    std::vector<uint8_t> blob;
    const rp::GoLockHeader* h = nullptr;
    const rp::GoLockTypeRec* types = nullptr;
    const rp::GoLockRec* locks = nullptr;
    const rp::GoLockCase* cases = nullptr;
    const rp::GoLockSpellRec* spells = nullptr;
    const char* pool = nullptr;
    const char* str(uint32_t off) const { return off < h->poolSize ? pool + off : ""; }
    const rp::GoLockRec* lock(uint32_t id) const {
        const rp::GoLockRec* end = locks + h->locks;
        const rp::GoLockRec* it = std::lower_bound(locks, end, id, [](const rp::GoLockRec& r, uint32_t v) { return r.id < v; });
        return it != end && it->id == id ? it : nullptr;
    }
    const rp::GoLockTypeRec* type(uint32_t id) const {
        for (uint32_t i = 0; i < h->lockTypes; ++i)
            if (types[i].id == id) return &types[i];
        return nullptr;
    }
    const rp::GoLockSpellRec* spell(uint32_t id) const {
        const rp::GoLockSpellRec* end = spells + h->spells;
        const rp::GoLockSpellRec* it =
            std::lower_bound(spells, end, id, [](const rp::GoLockSpellRec& r, uint32_t v) { return r.spell < v; });
        return it != end && it->spell == id ? it : nullptr;
    }
};

bool WorldSession::loadGoLockData() {
    if (goLockData_) return true;
    if (goLockTried_) return false;
    goLockTried_ = true;
    char path[256];
    fsPath(path, sizeof(path), "data/ui.pak");
    Pak pak;
    if (!pak.open(path)) return false;
    const PakEntry* e = pak.find("go_lock");
    void* raw = e ? pak.load(e) : nullptr;
    if (!raw) {
        PSP_LOGW(tag::Net, "go_lock missing (old ui.pak): no locks - every object is used with CMSG_GAMEOBJ_USE");
        return false;
    }
    auto d = std::make_shared<GoLockData>();
    d->blob.assign(static_cast<const uint8_t*>(raw), static_cast<const uint8_t*>(raw) + e->size);
    pakFree(raw);
    const uint8_t* b = d->blob.data();
    const size_t size = d->blob.size();
    if (size < sizeof(rp::GoLockHeader) || std::memcmp(b, "WGLK", 4) != 0) return false;
    d->h = reinterpret_cast<const rp::GoLockHeader*>(b);
    size_t off = sizeof(rp::GoLockHeader);
    const size_t need = off + d->h->lockTypes * sizeof(rp::GoLockTypeRec) + d->h->locks * sizeof(rp::GoLockRec) +
                        d->h->cases * sizeof(rp::GoLockCase) + d->h->spells * sizeof(rp::GoLockSpellRec) +
                        d->h->poolSize;
    if (d->h->version != 1 || need > size) {
        PSP_LOGW(tag::Net, "go_lock bad (version=%u need=%u size=%u)", static_cast<unsigned>(d->h->version),
                 static_cast<unsigned>(need), static_cast<unsigned>(size));
        return false;
    }
    d->types = reinterpret_cast<const rp::GoLockTypeRec*>(b + off);
    off += d->h->lockTypes * sizeof(rp::GoLockTypeRec);
    d->locks = reinterpret_cast<const rp::GoLockRec*>(b + off);
    off += d->h->locks * sizeof(rp::GoLockRec);
    d->cases = reinterpret_cast<const rp::GoLockCase*>(b + off);
    off += d->h->cases * sizeof(rp::GoLockCase);
    d->spells = reinterpret_cast<const rp::GoLockSpellRec*>(b + off);
    off += d->h->spells * sizeof(rp::GoLockSpellRec);
    d->pool = reinterpret_cast<const char*>(b + off);
    goLockData_ = d;
    PSP_LOGI(tag::Net, "go_lock types=%u locks=%u cases=%u spells=%u kb=%u", static_cast<unsigned>(d->h->lockTypes),
             static_cast<unsigned>(d->h->locks), static_cast<unsigned>(d->h->cases),
             static_cast<unsigned>(d->h->spells), static_cast<unsigned>((size + 1023) / 1024));
    return true;
}

// ---- The object cache ----

const WorldSession::GoTemplate* WorldSession::goTemplate(uint32_t entry) {
    if (entry == 0) return nullptr;
    auto it = goTemplates_.find(entry);
    if (it != goTemplates_.end()) return &it->second;
    if (goQueried_.insert(entry).second && state_ == State::InWorld && socket_) {
        uint64_t guid = 0;  // an object of that entry is named (upstream queryGameObjectInfo(entry, guid))
        for (const auto& g : gameObjects_)
            if (g.entry == entry) {
                guid = g.guid;
                break;
            }
        socket_->send(game::GameObjectQueryPacket::build(entry, guid));
        if (++goQueryLogs_ <= 40) PSP_LOGI(tag::Net, "go_query entry=%u guid=%08X", static_cast<unsigned>(entry), lo32(guid));
    }
    return nullptr;
}

const WorldSession::GameObjectInfo* WorldSession::findGameObject(uint64_t guid) const {
    for (const auto& g : gameObjects_)
        if (g.guid == guid) return &g;
    return nullptr;
}

int WorldSession::goDataIndex(uint8_t type, uint8_t prop) { return rp::goDataIndex(type, prop); }

uint32_t WorldSession::goData(const GameObjectInfo& g, uint8_t prop) {
    const int i = rp::goDataIndex(g.type, prop);
    if (i < 0) return 0;
    const GoTemplate* t = goTemplate(g.entry);
    return t && t->known ? t->data[i] : 0u;
}

float WorldSession::goUseDistance(uint8_t type) { return rp::goUseDistance(type); }

// ---- The lock ----

bool WorldSession::goKnownLockSpell(uint32_t lockType, uint32_t& spell, int32_t& skill) {
    // A known spell with the OPEN_LOCK effect whose MiscValue is the lock type (upstream planGameObjectOpen). Its
    // value is the player's skill in the lock type's profession, as AzerothCore Spell::CanOpenLock reads it
    // (GetSkillValue of SkillByLockType, the temporary bonus included); -1 when the type needs no skill.
    spell = 0;
    skill = -1;
    if (!goLockData_) return false;
    for (uint32_t id : knownSpells_) {
        const rp::GoLockSpellRec* s = goLockData_->spell(id);
        if (!s || s->lockType != lockType) continue;
        spell = id;
        break;
    }
    if (spell == 0) return false;
    if (const uint32_t line = skillByLockType(lockType)) {
        skill = 0;
        for (const auto& ps : playerSkills())
            if (ps.skillId == line) skill = static_cast<int32_t>(ps.value) + ps.bonusTemp;
    }
    return true;
}

bool WorldSession::goFindItem(uint32_t itemId, uint8_t& bag, uint8_t& slot, uint64_t& guid) const {
    const game::Inventory& inv = *inventory_;
    for (int i = 0; i < inv.getBackpackSize(); ++i)
        if (inv.getBackpackSlot(i).item.itemId == itemId) {
            bag = 0xFF;
            slot = static_cast<uint8_t>(game::slots::backpackWireSlot(i));
            guid = getBackpackItemGuid(i);
            return true;
        }
    for (int b = 0; b < game::Inventory::NUM_BAG_SLOTS; ++b)
        for (int s = 0; s < inv.getBagSize(b); ++s)
            if (inv.getBagSlot(b, s).item.itemId == itemId) {
                bag = static_cast<uint8_t>(game::slots::wornBagContainer(b));
                slot = static_cast<uint8_t>(s);
                guid = getBagItemGuid(b, s);
                return true;
            }
    for (int i = 0; i < inv.getKeyringSize(); ++i)
        if (inv.getKeyringSlot(i).item.itemId == itemId) {
            bag = 0xFF;
            slot = static_cast<uint8_t>(game::slots::keyringWireSlot(i));
            guid = i < static_cast<int>(keyringSlotGuids_.size()) ? keyringSlotGuids_[static_cast<size_t>(i)] : 0;
            return true;
        }
    return false;
}

bool WorldSession::goLock(const GameObjectInfo& g, GoLockState& out) {
    out = GoLockState{};
    const uint32_t lockId = goData(g, rp::kPropLock);
    if (lockId == 0 || !loadGoLockData()) return false;
    const rp::GoLockRec* row = goLockData_->lock(lockId);
    if (!row) return false;
    const bool lockedFlag = (g.flags & rp::kGoFlagLocked) != 0;
    auto describe = [&](const rp::GoLockCase& c) {
        out.kind = c.type;
        out.index = c.index;
        out.required = static_cast<int32_t>(c.skill);  // AzerothCore CanOpenLock: Lock.dbc Skill as it is
        out.caseIndex = c.slot;
        out.typeName.clear();
        out.cursor.clear();
        if (c.type == 2)
            if (const rp::GoLockTypeRec* t = goLockData_->type(c.index)) {
                out.typeName = goLockData_->str(t->name);
                out.cursor = goLockData_->str(t->cursor);
            }
    };
    const rp::GoLockCase* first = nullptr;  // the first case (slot order) that applies in the object's state
    const rp::GoLockCase* short_ = nullptr;  // a lock type whose known spell lacks the skill
    uint32_t shortSpell = 0;
    int32_t shortSkill = 0;
    const rp::GoLockCase* key = nullptr;  // a key the bags hold
    uint8_t keyBag = 0, keySlot = 0;
    uint64_t keyGuid = 0;
    for (uint32_t k = 0; k < row->count; ++k) {
        const rp::GoLockCase& c = goLockData_->cases[row->first + k];
        if (c.type < 1 || c.type > 3) continue;
        out.hasCase = true;
        if (!rp::goLockActionApplies(c.action, g.state, lockedFlag)) continue;
        if (!first) first = &c;
        if (c.type == 2) {  // a lock type: a known OPEN_LOCK spell of that type, cast at once (upstream's first choice)
            uint32_t spell = 0;
            int32_t skill = -1;
            if (!goKnownLockSpell(c.index, spell, skill)) continue;
            if (skill < 0 || skill >= static_cast<int32_t>(c.skill)) {
                describe(c);
                out.spell = spell;
                out.skill = skill;
                out.openable = true;
                return true;
            }
            if (!short_) {
                short_ = &c;
                shortSpell = spell;
                shortSkill = skill;
            }
        } else if (c.type == 3) {  // a spell: cast only when the player knows it
            if (knownSpells_.count(c.index) == 0) continue;
            describe(c);
            out.spell = c.index;
            out.openable = true;
            return true;
        } else if (!key) {  // an item: the key in the bags (used when no known spell opens the lock)
            if (goFindItem(c.index, keyBag, keySlot, keyGuid)) key = &c;
        }
    }
    if (key) {  // the key's on-use OPEN_LOCK spell, the object as its target
        describe(*key);
        uint32_t useSpell = 0;
        if (const game::ItemQueryResponseData* it = getItemInfo(key->index)) {
            for (const auto& sp : it->spells)
                if (sp.spellTrigger == 0 && sp.spellId != 0 && goLockData_->spell(sp.spellId)) {
                    useSpell = sp.spellId;
                    break;
                }
        } else {
            ensureItemInfo(key->index);
        }
        out.spell = useSpell;
        out.item = key->index;
        out.itemGuid = keyGuid;
        out.itemBag = keyBag;
        out.itemSlot = keySlot;
        out.openable = true;
        return true;
    }
    if (!first) {  // no case applies in the object's state: the lock does not stop the use
        out.openable = true;
        return true;
    }
    // Nothing opens it: the case that words the refusal and the tooltip's lock line.
    out.applies = true;
    out.openable = false;
    if (short_) {
        describe(*short_);
        out.spell = shortSpell;  // known, the skill is short
        out.skill = shortSkill;
    } else {
        describe(*first);
    }
    return true;
}

// ---- Clickable ----

bool WorldSession::goClickable(const GameObjectInfo& g) {
    const rp::GoClickRule rule = rp::goClickRule(g.type);
    if (rule == rp::kClickNever) return false;
    if (rule == rp::kClickFishing) {  // only the own channel object (AzerothCore GameObject::Use FISHINGNODE)
        const uint64_t ch = (static_cast<uint64_t>(selfField(static_cast<uint16_t>(kUnitChannelObject + 1))) << 32) | selfField(kUnitChannelObject);
        if (ch != g.guid) return false;
    }
    // PSP: a trap is never used (no disarm spells on the PSP's bar).
    if (g.type == rp::kGoTrap) return false;
    // The flags that stop an interaction (AzerothCore SharedDefines.h GameObjectFlags / GameObjectDynamicLowFlags).
    if (g.dynamic & rp::kGoDynNoInteract) return false;
    if (g.flags & (rp::kGoFlagInUse | rp::kGoFlagNotSelectable)) return false;
    if ((g.flags & rp::kGoFlagInteractCond) && !(g.dynamic & rp::kGoDynActivate)) return false;
    // A chair "only for its creator" (AzerothCore GameObjectData.h chair.onlyCreatorUse).
    if (goData(g, rp::kPropCreatorOnly) != 0 && g.createdBy != playerGuid_) return false;
    if (rule == rp::kClickChair) {  // PSP: sitting on any chair (stand state 4..6), stand up first - the stick
        const uint8_t stand = static_cast<uint8_t>(selfField(game::fieldIndex(game::UF::UNIT_FIELD_BYTES_1)) & 0xFFu);
        if (stand >= 4 && stand <= 6) return false;
    }
    return true;
}

// ---- The use ----

void WorldSession::goError(const std::string& text) {
    if (text.empty()) return;
    if (goErrors_.size() >= kMaxQueue) goErrors_.pop_front();
    goErrors_.push_back(text);
}

bool WorldSession::takeGoError(std::string& out) {
    if (goErrors_.empty()) return false;
    out = std::move(goErrors_.front());
    goErrors_.pop_front();
    return true;
}

void WorldSession::goReportUse(uint64_t guid) {
    if (state_ != State::InWorld || !socket_) return;
    network::Packet p(game::wireOpcode(LogicalOpcode::CMSG_GAMEOBJ_REPORT_USE));
    p.writeUInt64(guid);
    socket_->send(p);
}

// A spell cast with the object as the target (TARGET_FLAG_GAMEOBJECT + its packed guid): upstream castSpell's guards
// (off the mount, not while casting, a cast time not on the move), the shared SpellCastTargets builder.
void WorldSession::goCastAtObject(uint32_t spellId, uint64_t goGuid, bool moving) {
    if (state_ != State::InWorld || !socket_ || spellId == 0) return;
    const SpellInfo* info = spellInfo(spellId);
    if (isMounted()) dismount();
    if (casting_) {
        spellError(cs(kSpellInProgress));
        return;
    }
    if (moving && info && info->rec.castMs > 0) {
        const char* t = spellDb_ ? spellDb_->castFailText(51) : nullptr;  // SPELL_FAILED_MOVING
        spellError(t ? t : CLIENT_STR(SPELL_FAILED_MOVING));
        return;
    }
    castSpellAt(spellId, SpellCastTargets::gameObject(goGuid));  // the shared SpellCastTargets builder
    PSP_LOGI(tag::Net, "go_cast spell=%u go=%08X count=%u", static_cast<unsigned>(spellId), lo32(goGuid),
             static_cast<unsigned>(castCount_));
}

// A key used on the object: CMSG_USE_ITEM with the key's on-use spell and the object as the target (upstream
// performGameObjectInteractionNow's key-item branch), through the shared item use (its cooldown).
void WorldSession::goUseKey(const GoLockState& lock, uint64_t goGuid) {
    if (state_ != State::InWorld || !socket_) return;
    if (lock.spell == 0) {  // the item's query is not in yet: the next press has it
        ensureItemInfo(lock.item);
        goError(cs(kErrCantOpen));
        return;
    }
    if (isMounted()) dismount();
    PendingItemUse use;
    use.active = true;
    use.bag = lock.itemBag;
    use.slot = lock.itemSlot;
    use.itemGuid = lock.itemGuid;
    use.entry = lock.item;
    use.spellId = lock.spell;
    sendUseItem(use, SpellCastTargets::gameObject(goGuid));
    PSP_LOGI(tag::Net, "go_key item=%u spell=%u bag=%u slot=%u go=%08X", static_cast<unsigned>(lock.item),
             static_cast<unsigned>(lock.spell), static_cast<unsigned>(lock.itemBag), static_cast<unsigned>(lock.itemSlot),
             lo32(goGuid));
}

bool WorldSession::useGameObject(uint64_t guid, bool inRange, bool moving) {
    if (state_ != State::InWorld || !socket_) return false;
    const GameObjectInfo* gp = findGameObject(guid);
    if (!gp) return false;
    const GameObjectInfo g = *gp;  // (the list may change below)
    const GoTemplate* t = goTemplate(g.entry);
    const uint32_t unitFlags = selfField(game::fieldIndex(game::UF::UNIT_FIELD_FLAGS));
    auto refuse = [&](const std::string& text, const char* why) {
        goError(text);
        if (++goUseLogs_ <= 80)
            PSP_LOGI(tag::Net, "go_use_refused guid=%08X entry=%u type=%u why=%s", lo32(guid), static_cast<unsigned>(g.entry),
                     static_cast<unsigned>(g.type), why);
        return false;
    };
    // --- The checks (own rule: reach, alive, notInCombat, the LOCKED flag; the rest is the server's) ---
    if (!inRange) return refuse(cs(kErrTooFar), "far");
    if (life() != Life::Alive) return refuse(cs(kErrPlayerDead), "dead");
    GoLockState lock;
    const bool hasLock = goLock(g, lock);
    if ((unitFlags & kUnitFlagInCombat) && goData(g, rp::kPropNotInCombat) != 0) return refuse(cs(kErrNotInCombat), "combat");
    if ((g.flags & rp::kGoFlagLocked) && hasLock && !lock.openable) {
        const uint16_t e = rp::goLockedError(g.type);
        return refuse(cs(e == 0xEB ? kErrDoorLocked : e == 0xEC ? kErrButtonLocked : kErrUseLocked), "locked");
    }
    ++goUses_;
    if (++goUseLogs_ <= 80)
        PSP_LOGI(tag::Net, "go_use guid=%08X entry=%u type=%u name=%s lock=%u kind=%u spell=%u item=%u open=%d",
                 lo32(guid), static_cast<unsigned>(g.entry), static_cast<unsigned>(g.type), t ? t->name.c_str() : "?",
                 static_cast<unsigned>(goData(g, rp::kPropLock)), static_cast<unsigned>(lock.kind),
                 static_cast<unsigned>(lock.spell), static_cast<unsigned>(lock.item), lock.openable ? 1 : 0);
    // The plain use (upstream): off the mount unless the object allows it (AzerothCore IsUsableMounted), then
    // CMSG_GAMEOBJ_USE; CMSG_GAMEOBJ_REPORT_USE after it except for a mailbox / a guild vault.
    auto plainUse = [&](bool report) {
        if (isMounted() && goData(g, rp::kPropAllowMounted) == 0) dismount();
        socket_->send(game::GameObjectUsePacket::build(guid));
        if (report) goReportUse(guid);
    };
    // The meeting stone: the server summons only a selected member of the group, both at least the stone's minLevel
    // (AzerothCore GameObject::Use MEETINGSTONE, which then casts Meeting Stone Summon) - said here first.
    auto meetingStoneOk = [&]() {
        if (!isInGroup()) return goError(cs(kErrStoneNeedParty)), false;
        if (targetGuid_ == 0 || targetGuid_ == playerGuid_ || !partyMember(targetGuid_))
            return goError(cs(kErrStoneNotInParty)), false;
        const uint32_t minL = goData(g, rp::kPropMinLevel);
        if (minL != 0 && selfField(game::fieldIndex(game::UF::UNIT_FIELD_LEVEL)) < minL)
            return goError(cs(kErrStoneLevel)), false;
        if (auto te = entities_.getEntity(targetGuid_)) {
            const uint32_t lv = te->getField(game::fieldIndex(game::UF::UNIT_FIELD_LEVEL));
            if (minL != 0 && lv != 0 && lv < minL) return goError(cs(kErrStoneTargetLevel)), false;
        }
        PSP_LOGI(tag::Net, "go_meeting_stone target=%08X (server: spell %u)", lo32(targetGuid_),
                 static_cast<unsigned>(kSpellMeetingStoneSummon));
        return true;
    };
    switch (g.type) {
        case rp::kGoText:  // the use, then ItemTextFrame from the object's own page (upstream showGameObjectPageText)
            plainUse(true);
            openItemText(guid, t ? t->name : std::string(), goData(g, rp::kPropPageId),
                         static_cast<uint8_t>(goData(g, rp::kPropPageMaterial)));
            break;
        case rp::kGoMailbox:  // the use, then the mail frame (upstream openMailbox)
            plainUse(false);
            openMailbox(guid);
            break;
        case rp::kGoGuildBank:  // the use, then CMSG_GUILD_BANKER_ACTIVATE (upstream openGuildBank)
            PSP_LOGI(tag::Net, "go_guild_bank guid=%08X", lo32(guid));
            plainUse(false);
            openGuildBank(guid);
            break;
        case rp::kGoMeetingStone:  // the checks, then the plain use
            if (!meetingStoneOk()) break;
            [[fallthrough]];
        default: {
            if (!lock.openable) {  // nothing opens it: the line for the case goLock chose
                if (lock.kind == 1) {
                    if (const game::ItemQueryResponseData* it = getItemInfo(lock.index)) {
                        if (!it->name.empty()) goError(csf(kErrLockedWithItem, it->name));
                    } else {
                        ensureItemInfo(lock.index);
                        goError(cs(kErrCantOpen));
                    }
                } else if (lock.kind == 2) {
                    const std::string name = lock.typeName.empty() ? "UNKNOWN" : lock.typeName;
                    if (lock.spell == 0) {
                        goError(csf(kErrLockedWithSpell, name));
                    } else {
                        goError(csf(kErrLockedWithSpellKnown, name, static_cast<int>(lock.required)));
                    }
                } else if (lock.kind == 3) {
                    const std::string name = spellDb_ ? spellDb_->name(lock.index) : std::string();
                    goError(csf(kErrLockedWithSpell, name.empty() ? "UNKNOWN" : name.c_str()));
                } else {
                    goError(cs(kErrCantOpen));
                }
                break;
            }
            if (lock.item != 0) {
                goUseKey(lock, guid);
            } else if (lock.spell != 0) {
                goCastAtObject(lock.spell, guid, moving);
            } else {
                plainUse(true);
            }
            break;
        }
    }
    return true;
}

// ---- ItemTextFrame (the Lua API ItemTextGet* / ItemTextNextPage / ItemTextPrevPage / CloseItemText) ----

void WorldSession::itemTextRequestPage(uint32_t pageId) {
    itemText_.ready = false;
    itemText_.pageId = pageId;
    auto it = pageCache_.find(pageId);
    if (it != pageCache_.end()) {  // the page cache
        itemText_.text = it->second.first;
        itemText_.hasNext = it->second.second != 0;
        itemText_.ready = true;
        itemTextWaitPage_ = 0;
        ++itemText_.version;  // ITEM_TEXT_READY
        return;
    }
    itemTextWaitPage_ = pageId;
    itemTextAskMs_ = logUptimeMs();
    if (state_ == State::InWorld && socket_) socket_->send(game::PageTextQueryPacket::build(pageId, itemText_.source));
    ++itemText_.version;
}

void WorldSession::openItemText(uint64_t source, const std::string& title, uint32_t pageId, uint8_t material) {
    if (itemText_.open) closeItemText();
    const uint32_t version = itemText_.version + 1;
    itemText_ = ItemTextState{};
    itemText_.version = version;
    itemText_.open = true;  // ITEM_TEXT_BEGIN
    itemText_.source = source;
    itemText_.title = title;
    itemText_.material = material;
    itemText_.page = 1;
    PSP_LOGI(tag::Net, "itemtext_open src=%08X page=%u material=%u", lo32(source), static_cast<unsigned>(pageId),
             static_cast<unsigned>(material));
    if (pageId == 0) {  // nothing to show: nothing is said
        itemText_.open = false;
        return;
    }
    itemTextRequestPage(pageId);
}

void WorldSession::itemTextNextPage() {
    if (!itemText_.open || !itemText_.ready || !itemText_.hasNext) return;
    auto it = pageCache_.find(itemText_.pageId);
    if (it == pageCache_.end() || it->second.second == 0) return;
    itemText_.history.push_back(itemText_.pageId);
    ++itemText_.page;
    itemTextRequestPage(it->second.second);
}

void WorldSession::itemTextPrevPage() {
    if (!itemText_.open || itemText_.history.empty()) return;
    const uint32_t prev = itemText_.history.back();
    itemText_.history.pop_back();
    if (itemText_.page > 1) --itemText_.page;
    itemTextRequestPage(prev);
}

void WorldSession::closeItemText() {
    if (!itemText_.open) return;
    itemText_.open = false;  // ITEM_TEXT_CLOSED
    itemText_.ready = false;
    itemTextWaitPage_ = 0;
    ++itemText_.version;
    PSP_LOGI(tag::Net, "itemtext_close");
}

// ---- Fishing, the barber shop, the stand state, the animations ----

uint64_t WorldSession::fishingBobber() const {
    const uint64_t ch = (static_cast<uint64_t>(selfField(static_cast<uint16_t>(kUnitChannelObject + 1))) << 32) | selfField(kUnitChannelObject);
    if (ch == 0) return 0;
    const GameObjectInfo* g = findGameObject(ch);
    return g && g->type == rp::kGoFishingNode ? ch : 0;
}

void WorldSession::alterAppearance(uint32_t hairStyle, uint32_t hairColor, uint32_t facialHair) {
    if (state_ != State::InWorld || !socket_) return;
    network::Packet p(game::wireOpcode(LogicalOpcode::CMSG_ALTER_APPEARANCE));
    writeAlterAppearance(p, hairStyle, hairColor, facialHair, 0);  // skin 0: CanAlterSkin - none in 3.3.5a
    socket_->send(p);
    PSP_LOGI(tag::Net, "barber_alter hair=%u color=%u facial=%u", static_cast<unsigned>(hairStyle),
             static_cast<unsigned>(hairColor), static_cast<unsigned>(facialHair));
}

void WorldSession::cancelBarberShop() {
    if (!barberOpen_) return;
    barberOpen_ = false;
    sendStandState(0);  // CancelBarberShop: the player stands up from the chair (BARBER_SHOP_CLOSE)
    PSP_LOGI(tag::Net, "barber_close");
}

bool WorldSession::takeBarberResult(uint32_t& result) {
    if (barberResults_.empty()) return false;
    result = barberResults_.front();
    barberResults_.pop_front();
    return true;
}

bool WorldSession::takeGoAnimEvent(GoAnimEvent& out) {
    if (goAnimEvents_.empty()) return false;
    out = goAnimEvents_.front();
    goAnimEvents_.pop_front();
    return true;
}

void WorldSession::gameObjectUseUpdate(float dtSec, const glm::vec3& selfPos) {
    (void)dtSec;
    // ItemTextFrame: a page that never came (a lost answer) is asked for again after 3 s; the frame closes past
    // the interaction distance from its object (PSP: as the bank / mail windows).
    if (itemText_.open && itemTextWaitPage_ != 0 && logUptimeMs() - itemTextAskMs_ > 3000u) itemTextRequestPage(itemTextWaitPage_);
    if (itemText_.open && itemText_.source != 0)
        if (const GameObjectInfo* g = findGameObject(itemText_.source)) {
            const float dx = selfPos.x - g->pos.x, dy = selfPos.y - g->pos.y;
            if (dx * dx + dy * dy > game::NPC_INTERACT_MAX_DISTANCE * game::NPC_INTERACT_MAX_DISTANCE) closeItemText();
        }
    // A new bobber: no bite yet.
    const uint64_t bobber = fishingBobber();
    if (bobber != bobberSeen_) {
        bobberSeen_ = bobber;
        bobberBiteMs_ = 0;
    }
    // The barber shop closes when the player is no longer on a chair (stood up, moved, teleported).
    if (barberOpen_) {
        const uint8_t stand = static_cast<uint8_t>(selfField(game::fieldIndex(game::UF::UNIT_FIELD_BYTES_1)) & 0xFFu);
        if (stand < 4 || stand > 6) {
            barberOpen_ = false;
            PSP_LOGI(tag::Net, "barber_close stand=%u", static_cast<unsigned>(stand));
        }
    }
}

// ---- Packets ----

bool WorldSession::handleGameObjectPacket(LogicalOpcode op, network::Packet& packet) {
    switch (op) {
        case LogicalOpcode::SMSG_GAMEOBJECT_QUERY_RESPONSE: {
            // Read from a copy and passed on (false): other readers of the answer (the transports' lifts) see it
            // too - nothing else in onPacket handles it.
            network::Packet q = packet;
            game::GameObjectQueryResponseData d;
            if (!game::GameObjectQueryResponseParser::parse(q, d)) return false;
            GoTemplate t;
            t.entry = d.entry;
            t.known = !d.name.empty() || d.hasData;
            t.type = d.type;
            t.displayId = d.displayId;
            t.name = d.name;
            std::memcpy(t.data, d.data, sizeof(t.data));
            readGoQueryTail(q, t.size, t.questItems);  // the 3.3.5a tail the parser leaves (psp_go_parse.hpp)
            goTemplates_[t.entry] = std::move(t);
            ++goTemplateVersion_;
            if (goTemplates_.size() > 600) {  // PSP: the cache keeps what is in sight
                for (auto it = goTemplates_.begin(); it != goTemplates_.end();) {
                    const uint32_t e = it->first;
                    const bool seen = std::any_of(gameObjects_.begin(), gameObjects_.end(),
                                                  [e](const GameObjectInfo& g) { return g.entry == e; });
                    if (!seen) {
                        goQueried_.erase(e);
                        it = goTemplates_.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            if (++goQueryLogs_ <= 60)
                PSP_LOGI(tag::Net, "go_template entry=%u type=%u display=%u data0=%u name=%s", static_cast<unsigned>(d.entry),
                         static_cast<unsigned>(d.type), static_cast<unsigned>(d.displayId),
                         static_cast<unsigned>(d.data[0]), d.name.c_str());
            return false;
        }
        case LogicalOpcode::SMSG_PAGE_TEXT_QUERY_RESPONSE: {
            game::PageTextQueryResponseData d;
            if (!game::PageTextQueryResponseParser::parse(packet, d)) return true;
            pageCache_[d.pageId] = {d.text, d.nextPageId};
            if (pageCache_.size() > 16) {  // PSP: a small cache (16 pages) - never the page that just came
                auto victim = pageCache_.begin();
                if (victim->first == d.pageId) ++victim;
                if (victim != pageCache_.end()) pageCache_.erase(victim);
            }
            PSP_LOGI(tag::Net, "page_text id=%u next=%u len=%u", static_cast<unsigned>(d.pageId),
                     static_cast<unsigned>(d.nextPageId), static_cast<unsigned>(d.text.size()));
            if (itemText_.open && itemTextWaitPage_ == d.pageId) itemTextRequestPage(d.pageId);  // ITEM_TEXT_READY
            return true;
        }
        case LogicalOpcode::SMSG_GAMEOBJECT_PAGETEXT: {  // a goober with a page (AzerothCore GameObject::Use)
            if (!packet.hasRemaining(8)) return true;
            const uint64_t guid = packet.readUInt64();
            if (const GameObjectInfo* g = findGameObject(guid)) {
                const GoTemplate* t = goTemplate(g->entry);
                openItemText(guid, t ? t->name : std::string(), goData(*g, rp::kPropPageId),
                             static_cast<uint8_t>(goData(*g, rp::kPropPageMaterial)));
            }
            return true;
        }
        case LogicalOpcode::SMSG_GAMEOBJECT_CUSTOM_ANIM: {
            if (!packet.hasRemaining(12)) return true;
            const uint64_t guid = packet.readUInt64();
            const uint32_t anim = packet.readUInt32();
            if (goAnimEvents_.size() >= kMaxQueue) goAnimEvents_.pop_front();
            goAnimEvents_.push_back(GoAnimEvent{guid, 1, anim});
            if (guid != 0 && guid == fishingBobber()) {  // the bite: the bobber's Custom0
                bobberBiteMs_ = logUptimeMs();
                PSP_LOGI(tag::Net, "fish_bite bobber=%08X", lo32(guid));
            }
            return true;
        }
        case LogicalOpcode::SMSG_GAMEOBJECT_DESPAWN_ANIM:
        case LogicalOpcode::SMSG_GAMEOBJECT_RESET_STATE: {
            if (!packet.hasRemaining(8)) return true;
            const uint64_t guid = packet.readUInt64();
            if (goAnimEvents_.size() >= kMaxQueue) goAnimEvents_.pop_front();
            goAnimEvents_.push_back(GoAnimEvent{guid, static_cast<uint8_t>(op == LogicalOpcode::SMSG_GAMEOBJECT_DESPAWN_ANIM ? 2 : 3), 0});
            return true;
        }
        case LogicalOpcode::SMSG_FISH_NOT_HOOKED: goError(cs(kErrFishNotHooked)); return true;
        case LogicalOpcode::SMSG_FISH_ESCAPED: goError(cs(kErrFishEscaped)); return true;
        case LogicalOpcode::SMSG_ENABLE_BARBER_SHOP:  // BARBER_SHOP_OPEN
            barberOpen_ = true;
            ++barberSeq_;
            PSP_LOGI(tag::Net, "barber_open seq=%u", static_cast<unsigned>(barberSeq_));
            return true;
        case LogicalOpcode::SMSG_BARBER_SHOP_RESULT: {
            if (!packet.hasRemaining(4)) return true;
            const uint32_t r = packet.readUInt32();
            if (barberResults_.size() >= kMaxQueue) barberResults_.pop_front();
            barberResults_.push_back(r);
            if (r == 2) goError(cs(kErrNotBarberSitting));
            else if (r == 1 || r == 3) goError(cs(kErrNotEnoughMoney));
            PSP_LOGI(tag::Net, "barber_result r=%u", static_cast<unsigned>(r));
            return true;
        }
        default: return false;
    }
}

}  // namespace wowee::platform::psp
