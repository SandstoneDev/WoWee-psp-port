// Spells for the PSP world session: upstream SpellHandler (src/game/
// spell_handler.cpp) without GameHandler, as the combat code mirrors CombatHandler.
// Upstream's packet parsers/builders are used unchanged (InitialSpellsParser,
// CastSpellPacket, CastFailedParser, SpellStartParser, SpellGoParser,
// AuraUpdateParser, SpellDamageLogParser, SpellHealLogParser,
// CancelCastPacket, CancelAuraPacket, SetActionButtonPacket); the inline
// handlers are copied with their 3.3.5a branch:
//   castSpell / cancelCast / cancelAura, updateTimers, handleInitialSpells,
//   handleCastFailed, handleSpellStart, handleSpellGo, handleSpellCooldown,
//   handleCooldownEvent, handleClearCooldown, handleModifyCooldown,
//   handleAuraUpdate, handleLearned/Removed/SupercededSpell, handleSpellFailure,
//   handleSpellFailedOther, handleSpellDelayed, handleChannelStart/Update,
//   handlePeriodicAuraLog, GameHandler's SMSG_ACTION_BUTTONS and
//   setActionBarSlot, CombatHandler::handleSpellDamageLog / handleSpellHealLog.
// PSP differences:
//   - spell data from data/spells.pak (SpellDb) instead of the DBC caches;
//   - no sounds / spell visuals / crafting / gathering / restoration / talents
//     yet; the animation callback is a queue the world screen drains;
//   - Attack (6603) is the world screen's toggleAutoAttack (it needs the
//     player position); item-targeted spells are refused (no item cursor);
//   - auto self-cast treats any other living player as friendly and every
//     creature as not (no faction table on the PSP yet - cloud batch 3 B);
//   - no client-side Charge / melee range pre-checks or facing packets: the
//     world screen turns the character to the target, the server decides;
//   - one aura list per guid (upstream keeps the target's twice, mirrored).
#include "platform/psp/psp_world_session.hpp"

#include "game/spell_classification.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_strings.hpp"
#include "platform/psp/psp_spell_db.hpp"
#include "platform/psp/psp_spellfx_events.hpp"  // spell visual events (off unless spellfx=1|2)

#include <pspkernel.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace wowee::platform::psp {

namespace {
constexpr uint32_t kSpellAttack = 6603;       // upstream SPELL_ID_ATTACK
constexpr uint32_t kSpellHearthstone = 8690;  // upstream SPELL_ID_HEARTHSTONE
constexpr uint8_t kSpellFailedNotReady = 67;
constexpr uint8_t kSpellFailedMoving = 51;
constexpr size_t kMaxQueue = 48;

uint64_t nowUsSpells() { return sceKernelGetSystemTimeWide(); }

// upstream mergeCooldownSeconds
float mergeCooldownSeconds(float current, float incoming) {
    constexpr float kEpsilon = 0.05f;
    if (incoming <= 0.0f) return 0.0f;
    if (current <= 0.0f) return incoming;
    if (incoming > current + kEpsilon) return current;
    return incoming;
}

// upstream isRoutineCastRejection (logged quieter; the line still shows)
bool isRoutineCastRejection(uint8_t result) {
    switch (result) {
        case 45: case 51: case 57: case 67: case 85: case 97: case 105: case 128: case 132: case 134:
            return true;
        default:
            return false;
    }
}

// upstream combatTextTypeFromSpellMissInfo
uint8_t missText(uint8_t missInfo) {
    using CT = game::CombatTextEntry;
    namespace MI = game::SpellMissInfo;
    switch (missInfo) {
        case MI::MISS: return CT::MISS;
        case MI::DODGE: return CT::DODGE;
        case MI::PARRY: return CT::PARRY;
        case MI::BLOCK: return CT::BLOCK;
        case MI::EVADE: return CT::EVADE;
        case MI::IMMUNE: return CT::IMMUNE;
        case MI::DEFLECT: return CT::DEFLECT;
        case MI::ABSORB: return CT::ABSORB;
        case MI::RESIST: return CT::RESIST;
        case MI::IMMUNE2:
        case MI::IMMUNE3: return CT::IMMUNE;
        case MI::REFLECT: return CT::REFLECT;
        default: return CT::MISS;
    }
}

// upstream spellclass::isRangedWeaponAutoAttack (Auto Shot / Shoot / Throw)
bool rangedAuto(uint32_t id) { return game::spellclass::isRangedWeaponAutoAttack(id); }

}  // namespace

uint64_t WorldSession::auraNowMs() { return nowUsSpells() / 1000u; }

const SpellInfo* WorldSession::spellInfo(uint32_t spellId) const {
    return spellDb_ ? spellDb_->get(spellId) : nullptr;
}

void WorldSession::pushCastAnim(uint64_t guid, bool start, bool isChannel, game::SpellCastType type,
                                uint32_t spellId) {
    if (castAnims_.size() >= kMaxQueue) castAnims_.pop_front();
    CastAnim a;
    a.guid = guid;
    a.start = start;
    a.isChannel = isChannel;
    a.castType = type;
    a.spellId = spellId;
    castAnims_.push_back(a);
}

void WorldSession::pushSpellText(uint8_t type, int32_t amount, uint32_t spellId, bool outgoing, uint64_t src,
                                 uint64_t dst) {
    if (spellTexts_.size() >= kMaxQueue) spellTexts_.pop_front();
    SpellText t;
    t.type = type;
    t.amount = amount;
    t.spellId = spellId;
    t.outgoing = outgoing;
    t.src = src;
    t.dst = dst;
    spellTexts_.push_back(t);
}

void WorldSession::spellError(const std::string& text) {
    if (text.empty()) return;
    if (spellErrors_.size() >= 8) spellErrors_.pop_front();
    spellErrors_.push_back(text);
}

bool WorldSession::takeCastAnim(CastAnim& out) {
    if (castAnims_.empty()) return false;
    out = castAnims_.front();
    castAnims_.pop_front();
    return true;
}

bool WorldSession::takeSpellText(SpellText& out) {
    if (spellTexts_.empty()) return false;
    out = spellTexts_.front();
    spellTexts_.pop_front();
    return true;
}

bool WorldSession::takeSpellError(std::string& out) {
    if (spellErrors_.empty()) return false;
    out = std::move(spellErrors_.front());
    spellErrors_.pop_front();
    return true;
}

uint8_t WorldSession::takeCastEnd(uint32_t& spellId) {
    const uint8_t e = castEnd_;
    spellId = castEndSpell_;
    castEnd_ = 0;
    return e;
}

float WorldSession::spellCooldown(uint32_t spellId) const {
    auto it = spellCooldowns_.find(spellId);
    return it != spellCooldowns_.end() ? it->second : 0.0f;
}

float WorldSession::spellCooldownTotal(uint32_t spellId) const {
    auto it = spellCooldownTotals_.find(spellId);
    return it != spellCooldownTotals_.end() ? it->second : spellCooldown(spellId);
}

float WorldSession::gcdRemaining() const {
    if (gcdTotal_ <= 0.0f) return 0.0f;
    const float elapsed = static_cast<float>(nowUsSpells() - gcdStartedUs_) / 1e6f;
    const float rem = gcdTotal_ - elapsed;
    return rem > 0.0f ? rem : 0.0f;
}

const std::vector<game::AuraSlot>* WorldSession::unitAuras(uint64_t guid) const {
    if (guid == playerGuid_) return &playerAuras_;
    auto it = unitAuras_.find(guid);
    return it != unitAuras_.end() ? &it->second : nullptr;
}

const game::UnitCastState* WorldSession::unitCast(uint64_t guid) const {
    auto it = unitCastStates_.find(guid);
    return it != unitCastStates_.end() && it->second.casting ? &it->second : nullptr;
}

// --- Casting ----------------------------------------------------------------------

void WorldSession::castSpell(uint32_t spellId, uint64_t targetGuid, bool moving) {
    if (state_ != State::InWorld || !socket_ || spellId == 0) return;
    if (spellId == kSpellAttack) return;  // PSP: the world screen's toggleAutoAttack
    // upstream castSpell - profession spells (Cooking, First Aid, Alchemy, ...) open the
    // crafting window client-side instead of being sent as casts, matching the real client.
    if (const uint32_t craftSkillLine = tradeSkillOpenerLine(spellId)) {
        PSP_LOGI(tag::Net, "craft_opener spell=%u line=%u", static_cast<unsigned>(spellId),
                 static_cast<unsigned>(craftSkillLine));
        openCraftingWindow(craftSkillLine);
        return;
    }
    const SpellInfo* info = spellInfo(spellId);
    // mounts - upstream castSpell: casting while mounted dismounts
    // first; the mount being ridden (its aura) only dismounts. (No flight on
    // the PSP: upstream's "can't do that while flying" branch never applies.)
    if (isMounted()) {
        bool ridingThisMount = spellId == mountAuraSpellId_;
        for (const auto& a : playerAuras_)
            if (!a.isEmpty() && a.spellId == spellId) ridingThisMount = true;
        dismount();
        if (ridingThisMount) {
            PSP_LOGI(tag::Net, "mount_dismount_by_spell id=%u", static_cast<unsigned>(spellId));
            return;
        }
    }
    requestedSpellId_ = spellId;  // the instant-cast pose is for our own requests
    requestedAtMs_ = logUptimeMs();

    if (casting_) {
        // Spell queue: within 400 ms of the end of a (non-channel) cast.
        if (!castIsChannel_ && castTimeRemaining_ > 0.0f && castTimeRemaining_ <= 0.4f) {
            queuedSpellId_ = spellId;
            queuedSpellTarget_ = targetGuid != 0 ? targetGuid : targetGuid_;
            PSP_LOGI(tag::Net, "spell_queue id=%u left_ms=%d", static_cast<unsigned>(spellId),
                     static_cast<int>(castTimeRemaining_ * 1000.0f));
        }
        return;
    }
    // A spell with a cast time is refused while moving (SPELL_FAILED_MOVING).
    if (moving && info && info->rec.castMs > 0) {
        const char* t = spellDb_ ? spellDb_->castFailText(kSpellFailedMoving) : nullptr;
        spellError(t ? t : cs(CS::SPELL_FAILED_MOVING));
        PSP_LOGI(tag::Net, "cast_refused id=%u moving cast_ms=%u", static_cast<unsigned>(spellId),
                 static_cast<unsigned>(info->rec.castMs));
        return;
    }
    uint64_t target = targetGuid != 0 ? targetGuid : targetGuid_;
    const bool selfCast = spellId == kSpellHearthstone || (info && game::spellclass::isSelfCastRange(info->rec.maxRange));
    if (selfCast) target = 0;
    // Spells aimed at an item (Disenchant, enchants): no item cursor yet.
    if (info && (info->rec.targets & 0x10u) != 0) {
        spellError(ps(PS::SPELL_ITEM_TARGET_UNSUPPORTED));
        PSP_LOGI(tag::Net, "cast_refused id=%u item_target", static_cast<unsigned>(spellId));
        return;
    }
    const uint32_t implicitA = info ? info->rec.implicitTargetA : 0;
    // Auto self-cast (upstream isAutoSelfCast, on by default).
    if (!selfCast && implicitA != 0 && game::spellclass::requiresFriendlyTarget(implicitA)) {
        bool haveFriendlyTarget = false;
        if (target != 0 && target != playerGuid_) {
            auto e = entities_.getEntity(target);
            haveFriendlyTarget = e && e->getType() == game::ObjectType::PLAYER;  // PSP: no faction table yet
        }
        if (!haveFriendlyTarget) target = playerGuid_;
    }
    if (target == 0 && !selfCast && game::spellclass::requiresHostileTarget(implicitA)) {
        spellError(cs(CS::ERR_GENERIC_NO_TARGET));
        PSP_LOGI(tag::Net, "cast_refused id=%u no_target", static_cast<unsigned>(spellId));
        return;
    }
    socket_->send(game::CastSpellPacket::build(spellId, target, ++castCount_));
    PSP_LOGI(tag::Net, "cast_spell id=%u target=%08X count=%u", static_cast<unsigned>(spellId),
             static_cast<unsigned>(target & 0xFFFFFFFFu), static_cast<unsigned>(castCount_));
    // Optimistic GCD.
    if (gcdRemaining() <= 0.0f) {
        gcdTotal_ = 1.5f;
        gcdStartedUs_ = nowUsSpells();
    }
}

void WorldSession::clearOwnCast() {
    cancelCraftQueue();  // upstream drops the craft queue on every failed / cancelled cast
    casting_ = false;
    castIsChannel_ = false;
    currentCastSpellId_ = 0;
    castTimeRemaining_ = 0.0f;
    queuedSpellId_ = 0;
    queuedSpellTarget_ = 0;
}

void WorldSession::cancelCast() {
    if (!casting_) return;
    if (state_ == State::InWorld && socket_ && currentCastSpellId_ != 0)
        socket_->send(game::CancelCastPacket::build(currentCastSpellId_));
    castEnd_ = 2;
    castEndSpell_ = currentCastSpellId_;
    clearOwnCast();
    pushCastAnim(playerGuid_, false, false, game::SpellCastType::OMNI, 0);  // PSP: the FSM leaves the cast pose
    spellFxCancel(playerGuid_);  // upstream cancelAllPrecastVisuals
}

void WorldSession::cancelAura(uint32_t spellId) {
    if (state_ != State::InWorld || !socket_) return;
    socket_->send(game::CancelAuraPacket::build(spellId));
}

void WorldSession::setActionBarSlot(int slot, game::ActionBarSlot::Type type, uint32_t id) {
    if (slot < 0 || slot >= kActionBarSlots) return;
    actionBar_[slot].type = id ? type : game::ActionBarSlot::EMPTY;
    actionBar_[slot].id = id;
    actionBar_[slot].cooldownRemaining = 0.0f;
    actionBar_[slot].cooldownTotal = 0.0f;
    if (type == game::ActionBarSlot::SPELL && id) {
        const float cd = spellCooldown(id);
        if (cd > 0.0f) {
            actionBar_[slot].cooldownRemaining = cd;
            actionBar_[slot].cooldownTotal = spellCooldownTotal(id);
        }
    }
    if (state_ == State::InWorld && socket_)
        socket_->send(game::SetActionButtonPacket::build(static_cast<uint8_t>(slot), static_cast<uint8_t>(type), id,
                                                         false));
    PSP_LOGI(tag::Net, "action_button slot=%d type=%u id=%u", slot, static_cast<unsigned>(type),
             static_cast<unsigned>(id));
}

void WorldSession::seedCooldownFromSpellInfo(uint32_t spellId) {
    if (spellId == 0) return;
    auto existing = spellCooldowns_.find(spellId);
    if (existing != spellCooldowns_.end() && existing->second > 0.5f) return;
    const SpellInfo* info = spellInfo(spellId);
    if (!info) return;
    const uint32_t cooldownMs = std::max(info->rec.recoveryMs, info->rec.categoryRecoveryMs);
    if (cooldownMs <= 1500) return;  // GCD-sized
    const float seconds = cooldownMs / 1000.0f;
    spellCooldowns_[spellId] = seconds;
    spellCooldownTotals_[spellId] = seconds;
    for (auto& slot : actionBar_) {
        if (slot.type != game::ActionBarSlot::SPELL || slot.id != spellId) continue;
        slot.cooldownRemaining = seconds;
        slot.cooldownTotal = seconds;
    }
}

// upstream SpellHandler::updateTimers + GameHandler's action bar tick.
void WorldSession::updateSpells(float dt) {
    if (casting_ && castTimeRemaining_ > 0.0f) {
        castTimeRemaining_ -= dt;
        if (castTimeRemaining_ < 0.0f) castTimeRemaining_ = 0.0f;
    }
    for (auto it = spellCooldowns_.begin(); it != spellCooldowns_.end();) {
        it->second -= dt;
        if (it->second <= 0.0f) {
            spellCooldownTotals_.erase(it->first);
            it = spellCooldowns_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& slot : actionBar_)
        if (slot.cooldownRemaining > 0.0f) slot.cooldownRemaining = std::max(0.0f, slot.cooldownRemaining - dt);
    for (auto it = unitCastStates_.begin(); it != unitCastStates_.end();) {
        if (it->second.casting && it->second.timeRemaining > 0.0f) {
            it->second.timeRemaining -= dt;
            if (it->second.timeRemaining <= 0.0f) {
                it = unitCastStates_.erase(it);
                continue;
            }
        }
        ++it;
    }
}

// --- Packets ------------------------------------------------------------------------

void WorldSession::handleInitialSpells(network::Packet& packet) {
    game::InitialSpellsData data;
    if (!game::InitialSpellsParser::parse(packet, data)) return;
    knownSpells_ = {data.spellIds.begin(), data.spellIds.end()};
    knownSpells_.insert(kSpellAttack);
    knownSpells_.insert(kSpellHearthstone);
    for (const auto& cd : data.cooldowns) {
        const uint32_t effectiveMs = std::max(cd.cooldownMs, cd.categoryCooldownMs);
        if (effectiveMs > 0) {
            spellCooldowns_[cd.spellId] = effectiveMs / 1000.0f;
            spellCooldownTotals_[cd.spellId] = effectiveMs / 1000.0f;
        }
    }
    // Upstream defaults, then its saved bar (PSP: SMSG_ACTION_BUTTONS, which
    // follows, is the saved bar).
    actionBar_[0].type = game::ActionBarSlot::SPELL;
    actionBar_[0].id = kSpellAttack;
    actionBar_[11].type = game::ActionBarSlot::SPELL;
    actionBar_[11].id = kSpellHearthstone;
    for (auto& slot : actionBar_) {
        if (slot.type != game::ActionBarSlot::SPELL || slot.id == 0) continue;
        auto it = spellCooldowns_.find(slot.id);
        if (it != spellCooldowns_.end() && it->second > 0.0f) slot.cooldownTotal = slot.cooldownRemaining = it->second;
    }
    ++knownSpellsVersion_;
    if (spellDb_) spellDb_->prefetch(std::vector<uint32_t>(knownSpells_.begin(), knownSpells_.end()));
    PSP_LOGI(tag::Net, "initial_spells n=%u cooldowns=%u", static_cast<unsigned>(knownSpells_.size()),
             static_cast<unsigned>(data.cooldowns.size()));
}

// GameHandler's SMSG_ACTION_BUTTONS, 3.3.5a: u8 mode + 144 x u32 (type << 24 | id).
void WorldSession::handleActionButtons(network::Packet& packet) {
    size_t rem = packet.getRemainingSize();
    if (rem < 1) return;
    packet.readUInt8();
    --rem;
    int filled = 0;
    for (int i = 0; i < kActionBarSlots; ++i) {
        if (rem < 4) break;
        const uint32_t packed = packet.readUInt32();
        rem -= 4;
        if (packed == 0) continue;  // upstream: zeros do not clear (keeps Attack / Hearthstone)
        const uint8_t type = static_cast<uint8_t>((packed >> 24) & 0xFF);
        const uint32_t id = packed & 0x00FFFFFFu;
        if (id == 0) continue;
        game::ActionBarSlot slot;
        switch (type) {
            case 0x00: slot.type = game::ActionBarSlot::SPELL; break;
            case 0x80: slot.type = game::ActionBarSlot::ITEM; break;
            case 0x40: slot.type = game::ActionBarSlot::MACRO; break;
            default: continue;
        }
        slot.id = id;
        if (slot.type == game::ActionBarSlot::SPELL) {
            auto cd = spellCooldowns_.find(id);
            if (cd != spellCooldowns_.end() && cd->second > 0.0f) slot.cooldownRemaining = slot.cooldownTotal = cd->second;
        }
        actionBar_[i] = slot;
        ++filled;
    }
    if (spellDb_) {
        std::vector<uint32_t> ids;
        for (const auto& s : actionBar_)
            if (s.type == game::ActionBarSlot::SPELL && s.id) ids.push_back(s.id);
        spellDb_->prefetch(ids);
    }
    char list[160];
    int n = 0;
    list[0] = '\0';
    for (int i = 0; i < kActionBarSlots && n < static_cast<int>(sizeof(list)) - 16; ++i)
        if (actionBar_[i].id)
            n += std::snprintf(list + n, sizeof(list) - n, " %d:%u", i, static_cast<unsigned>(actionBar_[i].id));
    PSP_LOGI(tag::Net, "action_buttons filled=%d slots=%s", filled, list);
}

void WorldSession::handleCastFailed(network::Packet& packet) {
    game::CastFailedData data;
    if (!game::CastFailedParser::parse(packet, data)) return;
    castEnd_ = 2;
    castEndSpell_ = data.spellId;
    const bool hadCast = casting_;
    clearOwnCast();
    if (data.result == kSpellFailedNotReady) seedCooldownFromSpellInfo(data.spellId);
    const char* t = spellDb_ ? spellDb_->castFailText(data.result) : nullptr;
    if (!t) t = game::getSpellCastResultString(data.result);
    std::string code;
    if (!t) {
        code = psf(PS::SPELL_ERROR_CODE, static_cast<unsigned>(data.result));
        t = code.c_str();
    }
    spellError(t);
    if (++spellLogs_ <= 200)
        PSP_LOGI(tag::Net, "cast_failed id=%u result=%u routine=%d", static_cast<unsigned>(data.spellId),
                 static_cast<unsigned>(data.result), isRoutineCastRejection(data.result) ? 1 : 0);
    // PSP: upstream's spellCastFailedCallback ends the cast pose.
    if (hadCast) pushCastAnim(playerGuid_, false, false, game::SpellCastType::OMNI, data.spellId);
    spellFxCancel(playerGuid_);  // upstream cancelAllPrecastVisuals
}

void WorldSession::handleSpellStart(network::Packet& packet) {
    game::SpellStartData data;
    if (!game::SpellStartParser::parse(packet, data)) return;
    game::SpellCastType castType = game::SpellCastType::DIRECTED;
    if (data.targetGuid == 0) castType = game::SpellCastType::AREA;
    else if (data.targetGuid == data.casterUnit) castType = game::SpellCastType::OMNI;
    const bool ranged = rangedAuto(data.spellId);
    if (data.casterUnit != playerGuid_ && data.castTime > 0 && !ranged) {
        auto& s = unitCastStates_[data.casterUnit];
        s.casting = true;
        s.isChannel = false;
        s.spellId = data.spellId;
        s.timeTotal = data.castTime / 1000.0f;
        s.timeRemaining = s.timeTotal;
        s.castType = castType;
        pushCastAnim(data.casterUnit, true, false, castType, data.spellId);
    }
    if (data.casterUnit == playerGuid_ && data.castTime > 0 && !ranged) {
        casting_ = true;
        castIsChannel_ = false;
        currentCastSpellId_ = data.spellId;
        castTimeTotal_ = data.castTime / 1000.0f;
        castTimeRemaining_ = castTimeTotal_;
        pushCastAnim(playerGuid_, true, false, castType, data.spellId);
        if (++spellLogs_ <= 200)
            PSP_LOGI(tag::Net, "spell_start id=%u cast_ms=%u type=%u", static_cast<unsigned>(data.spellId),
                     static_cast<unsigned>(data.castTime), static_cast<unsigned>(castType));
    }
    spellFxStart(data.casterUnit, data.spellId, data.castTime);  // upstream triggerCastVisual
}

void WorldSession::handleSpellGo(network::Packet& packet) {
    game::SpellGoData data;
    if (!game::SpellGoParser::parse(packet, data)) return;
    const bool ranged = rangedAuto(data.spellId);
    auto goType = [&]() {
        if (data.targetGuid != 0 && data.targetGuid != data.casterUnit) return game::SpellCastType::DIRECTED;
        if (data.targetGuid == 0 && data.hitCount > 1) return game::SpellCastType::AREA;
        return game::SpellCastType::OMNI;
    };
    if (data.casterUnit == playerGuid_) {
        if (ranged) ++ownRangedShots_;  // the ranged weapon in hand while shooting
        seedCooldownFromSpellInfo(data.spellId);
        const SpellInfo* info = spellInfo(data.spellId);
        const bool meleeAbility = !ranged && info && game::spellclass::isMeleeRange(info->rec.maxRange) &&
                                  currentCastSpellId_ != data.spellId;
        const bool wasInTimedCast = casting_ && data.spellId == currentCastSpellId_;
        if (meleeAbility) {
            CastAnim a;  // upstream meleeSwingCallback: the world screen swings
            a.guid = playerGuid_;
            a.start = true;
            a.spellId = data.spellId;
            a.melee = true;
            castAnims_.push_back(a);
        } else if (!wasInTimedCast && !ranged && data.spellId == requestedSpellId_ &&
                   logUptimeMs() - requestedAtMs_ < 3000u) {
            pushCastAnim(playerGuid_, true, false, goType(), data.spellId);  // instant: one-shot pose
        }
        if (++spellLogs_ <= 200)
            PSP_LOGI(tag::Net, "spell_go id=%u timed=%d melee=%d hits=%u misses=%u", static_cast<unsigned>(data.spellId),
                     wasInTimedCast ? 1 : 0, meleeAbility ? 1 : 0, static_cast<unsigned>(data.hitCount),
                     static_cast<unsigned>(data.missCount));
        castEnd_ = 1;
        castEndSpell_ = data.spellId;
        casting_ = false;
        castIsChannel_ = false;
        currentCastSpellId_ = 0;
        castTimeRemaining_ = 0.0f;
        castTimeTotal_ = 0.0f;
        if (!ranged && !meleeAbility && (wasInTimedCast || data.spellId == requestedSpellId_))
            pushCastAnim(playerGuid_, false, false, game::SpellCastType::OMNI, data.spellId);
        if (craftQueueSpellGo(data.spellId)) {
            // upstream - the craft queue re-casts first (the spell queue waits)
        } else if (queuedSpellId_ != 0) {
            const uint32_t next = queuedSpellId_;
            const uint64_t nextTarget = queuedSpellTarget_;
            queuedSpellId_ = 0;
            queuedSpellTarget_ = 0;
            castSpell(next, nextTarget, false);
        }
    } else {
        const bool tracked = unitCastStates_.count(data.casterUnit) != 0;
        if (!tracked && !ranged) pushCastAnim(data.casterUnit, true, false, goType(), data.spellId);
        if (!ranged) pushCastAnim(data.casterUnit, false, false, game::SpellCastType::OMNI, data.spellId);
    }
    unitCastStates_.erase(data.casterUnit);
    if (!data.missTargets.empty()) {
        const uint64_t caster = data.casterUnit != 0 ? data.casterUnit : data.casterGuid;
        const bool mine = caster == playerGuid_;
        for (const auto& m : data.missTargets) {
            if (!mine && m.targetGuid != playerGuid_) continue;
            pushSpellText(missText(m.missType), 0, data.spellId, mine, caster, m.targetGuid);
        }
    }
    spellFxGo(data.casterUnit, data.spellId, data.hitTargets);  // cast kit + triggerImpactVisual
}

void WorldSession::handleSpellCooldown(network::Packet& packet) {
    if (!packet.hasRemaining(9)) return;
    packet.readUInt64();  // guid
    packet.readUInt8();   // flags
    while (packet.getRemainingSize() >= 8) {
        const uint32_t spellId = packet.readUInt32();
        const uint32_t cooldownMs = packet.readUInt32();
        const float seconds = cooldownMs / 1000.0f;
        if (spellId == 0 && cooldownMs > 0 && cooldownMs <= 2000) {  // the GCD marker
            gcdTotal_ = seconds;
            gcdStartedUs_ = nowUsSpells();
            continue;
        }
        auto it = spellCooldowns_.find(spellId);
        if (it == spellCooldowns_.end()) {
            spellCooldowns_[spellId] = seconds;
            spellCooldownTotals_[spellId] = seconds;
        } else {
            it->second = mergeCooldownSeconds(it->second, seconds);
            float& total = spellCooldownTotals_[spellId];
            total = std::max(total, it->second);
        }
        for (auto& slot : actionBar_) {
            if (slot.type != game::ActionBarSlot::SPELL || slot.id != spellId) continue;
            const float prev = slot.cooldownRemaining;
            const float merged = mergeCooldownSeconds(slot.cooldownRemaining, seconds);
            slot.cooldownRemaining = merged;
            slot.cooldownTotal = (slot.cooldownTotal <= 0.0f || prev <= 0.0f) ? seconds : std::max(slot.cooldownTotal, merged);
        }
    }
}

void WorldSession::handleAuraUpdate(network::Packet& packet, bool isAll) {
    game::AuraUpdateData data;
    if (!game::AuraUpdateParser::parse(packet, data, isAll)) return;
    if (data.guid == 0) return;
    std::vector<game::AuraSlot>& list = data.guid == playerGuid_ ? playerAuras_ : unitAuras_[data.guid];
    if (isAll) list.clear();
    const uint64_t now = auraNowMs();
    size_t maxSlot = 0;
    for (const auto& u : data.updates) maxSlot = std::max<size_t>(maxSlot, u.first);
    if (!data.updates.empty() && list.size() <= maxSlot) list.resize(maxSlot + 1);
    for (auto [slot, aura] : data.updates) {
        if (aura.durationMs >= 0) aura.receivedAtMs = now;
        list[slot] = aura;
    }
    if (data.guid == playerGuid_ && spellDb_) {
        std::vector<uint32_t> ids;
        for (const auto& a : list)
            if (a.spellId) ids.push_back(a.spellId);
        for (uint32_t id : ids) spellDb_->get(id);
    }
}

void WorldSession::handleSpellFailure(network::Packet& packet) {
    const uint64_t guid = packet.readPackedGuid();
    if (packet.hasRemaining(6)) {
        packet.readUInt8();  // castCount
        const uint32_t spellId = packet.readUInt32();
        const uint8_t reason = packet.readUInt8();
        if (guid == playerGuid_ && reason != 0) {
            const char* t = spellDb_ ? spellDb_->castFailText(reason) : nullptr;
            if (!t) t = game::getSpellCastResultString(reason);
            if (t) {
                const std::string& name = spellDb_ ? spellDb_->name(spellId) : std::string();
                const std::string full = name.empty() ? std::string(t) : name + ": " + t;
                spellError(full);
                addSystemChatMessage(full);
            }
        }
    }
    if (guid == playerGuid_ || guid == 0) {
        if (casting_) {
            castEnd_ = 2;
            castEndSpell_ = currentCastSpellId_;
        }
        clearOwnCast();
        pushCastAnim(playerGuid_, false, false, game::SpellCastType::OMNI, 0);
        spellFxCancel(playerGuid_);  // upstream cancelAllPrecastVisuals
    } else {
        unitCastStates_.erase(guid);
        pushCastAnim(guid, false, false, game::SpellCastType::OMNI, 0);
    }
}

void WorldSession::handleChannelStart(network::Packet& packet) {
    const uint64_t caster = packet.readPackedGuid();
    if (!packet.hasRemaining(8)) return;
    const uint32_t spellId = packet.readUInt32();
    const uint32_t totalMs = packet.readUInt32();
    if (totalMs == 0 || caster == 0) return;
    if (caster == playerGuid_) {
        casting_ = true;
        castIsChannel_ = true;
        currentCastSpellId_ = spellId;
        castTimeTotal_ = totalMs / 1000.0f;
        castTimeRemaining_ = castTimeTotal_;
    } else {
        auto& s = unitCastStates_[caster];
        s.casting = true;
        s.isChannel = true;
        s.spellId = spellId;
        s.timeTotal = totalMs / 1000.0f;
        s.timeRemaining = s.timeTotal;
    }
    const game::SpellCastType type = (caster == playerGuid_ && targetGuid_ != 0) ? game::SpellCastType::DIRECTED
                                                                                 : game::SpellCastType::OMNI;
    pushCastAnim(caster, true, true, type, spellId);
}

void WorldSession::handleChannelUpdate(network::Packet& packet) {
    const uint64_t caster = packet.readPackedGuid();
    if (!packet.hasRemaining(4)) return;
    const uint32_t remainMs = packet.readUInt32();
    if (caster == playerGuid_) {
        castTimeRemaining_ = remainMs / 1000.0f;
        if (remainMs == 0) {
            castEnd_ = 1;
            castEndSpell_ = currentCastSpellId_;
            casting_ = false;
            castIsChannel_ = false;
            currentCastSpellId_ = 0;
        }
    } else if (caster != 0) {
        auto it = unitCastStates_.find(caster);
        if (it != unitCastStates_.end()) {
            it->second.timeRemaining = remainMs / 1000.0f;
            if (remainMs == 0) unitCastStates_.erase(it);
        }
    }
    if (remainMs == 0) pushCastAnim(caster, false, true, game::SpellCastType::OMNI, 0);
}

bool WorldSession::handleSpellPacket(game::LogicalOpcode op, network::Packet& packet) {
    using game::LogicalOpcode;
    switch (op) {
        case LogicalOpcode::SMSG_INITIAL_SPELLS: handleInitialSpells(packet); return true;
        case LogicalOpcode::SMSG_ACTION_BUTTONS: handleActionButtons(packet); return true;
        case LogicalOpcode::SMSG_CAST_FAILED: handleCastFailed(packet); return true;
        case LogicalOpcode::SMSG_SPELL_START: handleSpellStart(packet); return true;
        case LogicalOpcode::SMSG_SPELL_GO: handleSpellGo(packet); return true;
        case LogicalOpcode::SMSG_SPELL_COOLDOWN: handleSpellCooldown(packet); return true;
        case LogicalOpcode::SMSG_COOLDOWN_EVENT: {
            if (!packet.hasRemaining(4)) return true;
            const uint32_t spellId = packet.readUInt32();
            spellCooldowns_.erase(spellId);
            spellCooldownTotals_.erase(spellId);
            for (auto& slot : actionBar_)
                if (slot.type == game::ActionBarSlot::SPELL && slot.id == spellId) slot.cooldownRemaining = 0.0f;
            return true;
        }
        case LogicalOpcode::SMSG_CLEAR_COOLDOWN: {
            if (!packet.hasRemaining(4)) return true;
            const uint32_t spellId = packet.readUInt32();
            spellCooldowns_.erase(spellId);
            spellCooldownTotals_.erase(spellId);
            for (auto& slot : actionBar_)
                if (slot.type == game::ActionBarSlot::SPELL && slot.id == spellId) slot.cooldownRemaining = 0.0f;
            return true;
        }
        case LogicalOpcode::SMSG_MODIFY_COOLDOWN: {
            if (!packet.hasRemaining(16)) return true;
            const uint32_t spellId = packet.readUInt32();
            packet.readUInt64();
            const float diff = static_cast<int32_t>(packet.readUInt32()) / 1000.0f;
            auto it = spellCooldowns_.find(spellId);
            if (it != spellCooldowns_.end()) {
                it->second = std::max(0.0f, it->second + diff);
                for (auto& slot : actionBar_)
                    if (slot.type == game::ActionBarSlot::SPELL && slot.id == spellId)
                        slot.cooldownRemaining = std::max(0.0f, slot.cooldownRemaining + diff);
            }
            return true;
        }
        case LogicalOpcode::SMSG_AURA_UPDATE: handleAuraUpdate(packet, false); return true;
        case LogicalOpcode::SMSG_AURA_UPDATE_ALL: handleAuraUpdate(packet, true); return true;
        case LogicalOpcode::SMSG_LEARNED_SPELL: {
            if (!packet.hasRemaining(4)) return true;
            const uint32_t spellId = packet.readUInt32();
            const bool known = knownSpells_.count(spellId) != 0;
            knownSpells_.insert(spellId);
            ++knownSpellsVersion_;
            // talents - upstream returns before the line for a talent rank spell.
            if (!known && !isTalentRankSpell(spellId)) {  // ERR_LEARN_SPELL_S
                const std::string& name = spellDb_ ? spellDb_->name(spellId) : std::string();
                addSystemChatMessage(name.empty() ? std::string(ps(PS::SPELL_LEARNED_NEW))
                                                  : csf(CS::ERR_LEARN_SPELL_S, name));
            }
            return true;
        }
        case LogicalOpcode::SMSG_REMOVED_SPELL: {
            if (!packet.hasRemaining(4)) return true;
            knownSpells_.erase(packet.readUInt32());
            ++knownSpellsVersion_;
            return true;
        }
        case LogicalOpcode::SMSG_SUPERCEDED_SPELL: {
            if (!packet.hasRemaining(8)) return true;
            const uint32_t oldId = packet.readUInt32();
            const uint32_t newId = packet.readUInt32();
            knownSpells_.erase(oldId);
            knownSpells_.insert(newId);
            ++knownSpellsVersion_;
            for (auto& slot : actionBar_) {
                if (slot.type == game::ActionBarSlot::SPELL && slot.id == oldId) {
                    slot.id = newId;
                    slot.cooldownRemaining = slot.cooldownTotal = 0.0f;
                }
            }
            return true;
        }
        case LogicalOpcode::SMSG_SPELL_FAILURE: handleSpellFailure(packet); return true;
        case LogicalOpcode::SMSG_SPELL_FAILED_OTHER: {
            const uint64_t guid = packet.readPackedGuid();
            if (guid != 0 && guid != playerGuid_) unitCastStates_.erase(guid);
            packet.skipAll();
            return true;
        }
        case LogicalOpcode::SMSG_SPELL_DELAYED: {
            if (!packet.hasRemaining(1)) return true;
            const uint64_t caster = packet.readPackedGuid();
            if (!packet.hasRemaining(4)) return true;
            const uint32_t delayMs = packet.readUInt32();
            if (delayMs == 0) return true;
            const float d = delayMs / 1000.0f;
            if (caster == playerGuid_) {
                if (casting_) {
                    castTimeRemaining_ += d;
                    castTimeTotal_ += d;
                }
            } else {
                auto it = unitCastStates_.find(caster);
                if (it != unitCastStates_.end() && it->second.casting) {
                    it->second.timeRemaining += d;
                    it->second.timeTotal += d;
                }
            }
            return true;
        }
        case LogicalOpcode::MSG_CHANNEL_START: handleChannelStart(packet); return true;
        case LogicalOpcode::MSG_CHANNEL_UPDATE: handleChannelUpdate(packet); return true;
        case LogicalOpcode::SMSG_SPELLNONMELEEDAMAGELOG: {
            game::SpellDamageLogData d;
            if (!game::SpellDamageLogParser::parse(packet, d)) return true;
            const bool mine = d.attackerGuid == playerGuid_;
            const bool atMe = d.targetGuid == playerGuid_;
            if (!mine && !atMe) return true;
            if (atMe && d.attackerGuid != 0) {
                hostileAttackers_.insert(d.attackerGuid);
                autoTargetAttacker(d.attackerGuid);
            }
            using CT = game::CombatTextEntry;
            if (d.damage > 0)
                pushSpellText(d.isCrit ? CT::CRIT_DAMAGE : CT::SPELL_DAMAGE, static_cast<int32_t>(d.damage), d.spellId, mine,
                              d.attackerGuid, d.targetGuid);
            if (d.absorbed > 0)
                pushSpellText(CT::ABSORB, static_cast<int32_t>(d.absorbed), d.spellId, mine, d.attackerGuid, d.targetGuid);
            if (d.resisted > 0)
                pushSpellText(CT::RESIST, static_cast<int32_t>(d.resisted), d.spellId, mine, d.attackerGuid, d.targetGuid);
            return true;
        }
        case LogicalOpcode::SMSG_SPELLHEALLOG: {
            game::SpellHealLogData d;
            if (!game::SpellHealLogParser::parse(packet, d)) return true;
            const bool mine = d.casterGuid == playerGuid_;
            if (!mine && d.targetGuid != playerGuid_) return true;
            using CT = game::CombatTextEntry;
            pushSpellText(d.isCrit ? CT::CRIT_HEAL : CT::HEAL, static_cast<int32_t>(d.heal), d.spellId, mine, d.casterGuid,
                          d.targetGuid);
            if (d.absorbed > 0)
                pushSpellText(CT::ABSORB, static_cast<int32_t>(d.absorbed), d.spellId, mine, d.casterGuid, d.targetGuid);
            return true;
        }
        case LogicalOpcode::SMSG_PERIODICAURALOG: {
            if (!packet.hasFullPackedGuid()) return true;
            const uint64_t victim = packet.readPackedGuid();
            if (!packet.hasFullPackedGuid()) return true;
            const uint64_t caster = packet.readPackedGuid();
            if (!packet.hasRemaining(8)) return true;
            const uint32_t spellId = packet.readUInt32();
            const uint32_t count = packet.readUInt32();
            const bool mine = caster == playerGuid_;
            if ((!mine && victim != playerGuid_) || count > 64) {
                packet.skipAll();
                return true;
            }
            using CT = game::CombatTextEntry;
            for (uint32_t i = 0; i < count && packet.hasRemaining(4); ++i) {
                const uint32_t auraType = packet.readUInt32();
                if (auraType == 3 || auraType == 89) {  // 3.3.5a: dmg, overkill, school, absorbed, resisted, crit
                    if (!packet.hasRemaining(21)) break;
                    const uint32_t dmg = packet.readUInt32();
                    packet.readUInt32();
                    packet.readUInt32();
                    const uint32_t abs = packet.readUInt32();
                    const uint32_t res = packet.readUInt32();
                    const bool crit = packet.readUInt8() != 0;
                    if (dmg > 0)
                        pushSpellText(crit ? CT::CRIT_DAMAGE : CT::PERIODIC_DAMAGE, static_cast<int32_t>(dmg), spellId, mine,
                                      caster, victim);
                    if (abs > 0) pushSpellText(CT::ABSORB, static_cast<int32_t>(abs), spellId, mine, caster, victim);
                    if (res > 0) pushSpellText(CT::RESIST, static_cast<int32_t>(res), spellId, mine, caster, victim);
                } else if (auraType == 8 || auraType == 20) {  // 3.3.5a: heal, overheal, absorbed, crit
                    if (!packet.hasRemaining(13)) break;
                    const uint32_t heal = packet.readUInt32();
                    packet.readUInt32();
                    const uint32_t abs = packet.readUInt32();
                    const bool crit = packet.readUInt8() != 0;
                    pushSpellText(crit ? CT::CRIT_HEAL : CT::PERIODIC_HEAL, static_cast<int32_t>(heal), spellId, mine, caster,
                                  victim);
                    if (abs > 0) pushSpellText(CT::ABSORB, static_cast<int32_t>(abs), spellId, mine, caster, victim);
                } else if (auraType == 21 || auraType == 24) {  // power: type, amount
                    if (!packet.hasRemaining(8)) break;
                    packet.readUInt32();
                    const uint32_t amount = packet.readUInt32();
                    if (amount > 0) pushSpellText(CT::ENERGIZE, static_cast<int32_t>(amount), spellId, mine, caster, victim);
                } else {
                    packet.skipAll();
                    break;
                }
            }
            return true;
        }
        // uint64 guid + uint32 visual id each (upstream handlePlaySpellVisual /
        // the SMSG_PLAY_SPELL_IMPACT handler); consumed only when the effects are on.
        case LogicalOpcode::SMSG_PLAY_SPELL_VISUAL:
        case LogicalOpcode::SMSG_PLAY_SPELL_IMPACT: {
            if (!packet.hasRemaining(12)) return true;
            const uint64_t guid = packet.readUInt64();
            const uint32_t visualId = packet.readUInt32();
            spellFxVisual(op == LogicalOpcode::SMSG_PLAY_SPELL_IMPACT, guid, visualId);
            return true;
        }
        default:
            return false;
    }
}

} // namespace wowee::platform::psp
