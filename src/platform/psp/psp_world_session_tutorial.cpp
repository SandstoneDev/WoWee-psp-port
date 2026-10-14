// tutorials - the tutorial flags and the engine's TUTORIAL_TRIGGER.
//
// Upstream WoWee only logs SMSG_TUTORIAL_FLAGS (GameHandler::handleTutorialFlags,
// game_handler_callbacks.cpp) and never fires TUTORIAL_TRIGGER; the rest is the
// 3.3.5a client's behaviour, rebuilt on the same packets:
//   SMSG_TUTORIAL_FLAGS   8 x u32 at login (the account's seen tutorials)
//   CMSG_TUTORIAL_FLAG    u32 id  - the server sets bit id % 32 of word id / 32
//   CMSG_TUTORIAL_CLEAR   every bit set   (Lua ClearTutorials: "don't show any")
//   CMSG_TUTORIAL_RESET   every bit clear (Lua ResetTutorials: "show them again")
// TUTORIAL_TRIGGER(id) is raised by the client engine at game points; the
// TutorialFrame (Lua in the client, src/ui/psp/ui_world_tutorial.cpp here)
// queues and shows it. PSP: the engine's points are polled here once every
// 250 ms from the session's own state (level, death, resting, target, combat,
// lootable corpses, quests, vendor, whispers, reputation, learned spells,
// durability) - the same moment within a quarter second, and no edits in the
// other files. The ids are listed with their points below (tutorialUpdate).
// Items (ITEM_PUSH / inventory full) come from one hook in pushInventoryEvent:
// those events are consumed by the inventory UI before a poll could see them.
#include "platform/psp/psp_world_session.hpp"

#include "game/update_field_table.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_prof.hpp"

#include <algorithm>
#include <cstring>

namespace wowee::platform::psp {

namespace {

constexpr size_t kMaxTriggers = 16;
constexpr uint32_t kPlayerFlagsResting = 0x00000020;  // PLAYER_FLAGS_RESTING
constexpr int32_t kInventoryFull = 50;                // EQUIP_ERR_INVENTORY_FULL (upstream "Inventory is full.")
constexpr uint32_t kWatchSettleMs = 12000;            // login packets settle before the watch starts

// First login: the TutorialFrame's own reset list (InterfaceOptionsPanels.xml,
// ResetTutorials button: TriggerTutorial 42, 2, 3, 1) - dropped when flagged.
constexpr uint32_t kLoginTutorials[] = {42, 2, 3, 1};

// Level-up points (the level reached, the tutorial): Ding on every level; the
// client's level-gated ones (talents at 10, dungeon finder at 15, riding at 20).
struct LevelTutorial {
    uint32_t level, id;
};
constexpr LevelTutorial kLevelTutorials[] = {{2, 14}, {5, 38}, {10, 13}, {15, 51}, {20, 53}};

int popcount32(uint32_t v) {
    int n = 0;
    for (; v; v &= v - 1) ++n;
    return n;
}

} // namespace

bool WorldSession::handleTutorialPacket(game::LogicalOpcode op, network::Packet& packet) {
    if (op != game::LogicalOpcode::SMSG_TUTORIAL_FLAGS) return false;
    // Upstream handleTutorialFlags: 32 bytes, else warn and ignore.
    if (!packet.hasRemaining(32)) {
        PSP_LOGW(tag::Net, "tutorial_flags short size=%u", static_cast<unsigned>(packet.getSize()));
        return true;
    }
    int set = 0;
    for (uint32_t& w : tutorialFlags_) {
        w = packet.readUInt32();
        set += popcount32(w);
    }
    const bool first = !tutorialFlagsReceived_;
    tutorialFlagsReceived_ = true;
    // The login's own packets (initial spells, the quest log resync - up to
    // 10 s, factions) must not read as game events: the watch starts later.
    if (first) tutorialWatch_.nextMs = logUptimeMs() + kWatchSettleMs;
    PSP_LOGI(tag::Net, "tutorial_flags set=%d of=%u show=%d", set, static_cast<unsigned>(kTutorialFlagWords * 32),
             showTutorials_ ? 1 : 0);
    if (first) {
        for (uint32_t id : kLoginTutorials) triggerTutorial(id);
        // Triggers raised before the flags came in (none normally).
        std::deque<uint32_t> waiting;
        waiting.swap(tutorialWaiting_);
        for (uint32_t id : waiting) triggerTutorial(id);
    }
    return true;
}

bool WorldSession::isTutorialFlagged(uint32_t id) const {
    if (id >= kTutorialFlagWords * 32) return false;
    return (tutorialFlags_[id / 32] >> (id % 32)) & 1u;
}

void WorldSession::flagTutorial(uint32_t id) {
    if (id >= kTutorialFlagWords * 32) return;
    const bool was = isTutorialFlagged(id);
    tutorialFlags_[id / 32] |= 1u << (id % 32);
    if (state_ != State::InWorld || !socket_) return;
    network::Packet p(game::wireOpcode(game::LogicalOpcode::CMSG_TUTORIAL_FLAG));
    p.writeUInt32(id);
    socket_->send(p);
    PSP_LOGI(tag::Net, "tutorial_flag id=%u was=%d", static_cast<unsigned>(id), was ? 1 : 0);
}

void WorldSession::clearTutorials() {
    tutorialFlags_.fill(0xFFFFFFFFu);
    tutorialTriggers_.clear();
    tutorialWaiting_.clear();
    if (state_ == State::InWorld && socket_) {
        network::Packet p(game::wireOpcode(game::LogicalOpcode::CMSG_TUTORIAL_CLEAR));
        socket_->send(p);
    }
    PSP_LOGI(tag::Net, "tutorial_clear");
}

void WorldSession::resetTutorials() {
    tutorialFlags_.fill(0u);
    tutorialTriggers_.clear();
    tutorialWaiting_.clear();
    if (state_ == State::InWorld && socket_) {
        network::Packet p(game::wireOpcode(game::LogicalOpcode::CMSG_TUTORIAL_RESET));
        socket_->send(p);
    }
    PSP_LOGI(tag::Net, "tutorial_reset");
}

bool WorldSession::canResetTutorials() const {
    for (uint32_t w : tutorialFlags_)
        if (w) return true;
    return false;
}

// PSP: the client keeps no history of the order tutorials were seen in (only
// the flag bits) - "next / previous completed" walk the flagged ids that have
// a text, in id order.
uint32_t WorldSession::getNextCompletedTutorial(uint32_t id, const std::vector<uint32_t>& known) const {
    for (uint32_t k : known)
        if (k > id && isTutorialFlagged(k)) return k;
    return 0;
}

uint32_t WorldSession::getPrevCompletedTutorial(uint32_t id, const std::vector<uint32_t>& known) const {
    uint32_t best = 0;
    for (uint32_t k : known)
        if (k < id && isTutorialFlagged(k)) best = k;
    return best;
}

void WorldSession::triggerTutorial(uint32_t id) {
    if (id == 0 || id >= kTutorialFlagWords * 32) return;
    if (!tutorialFlagsReceived_) {  // too early to know whether it was seen
        if (tutorialWaiting_.size() < kMaxTriggers &&
            std::find(tutorialWaiting_.begin(), tutorialWaiting_.end(), id) == tutorialWaiting_.end())
            tutorialWaiting_.push_back(id);
        return;
    }
    const bool flagged = isTutorialFlagged(id);
    const bool queued = std::find(tutorialTriggers_.begin(), tutorialTriggers_.end(), id) != tutorialTriggers_.end();
    const bool shown = showTutorials_ && !flagged && !queued;
    if (shown) {
        if (tutorialTriggers_.size() >= kMaxTriggers) tutorialTriggers_.pop_front();
        tutorialTriggers_.push_back(id);
    }
    if (++tutorialLogs_ <= 60)
        PSP_LOGI(tag::Net, "tutorial_trigger id=%u shown=%d flagged=%d", static_cast<unsigned>(id), shown ? 1 : 0,
                 flagged ? 1 : 0);
}

bool WorldSession::takeTutorialTrigger(uint32_t& id) {
    if (tutorialTriggers_.empty()) return false;
    id = tutorialTriggers_.front();
    tutorialTriggers_.pop_front();
    return true;
}

// The hook in pushInventoryEvent: ITEM_PUSH -> LootedItem (8), and the
// item's kind once its query answer is in (tutorialUpdate); the inventory-full
// error -> FullBags (58).
void WorldSession::tutorialInventoryEvent(const char* name, int32_t arg, uint32_t entry) {
    if (!tutorialFlagsReceived_ || !showTutorials_) return;
    if (std::strcmp(name, "ITEM_PUSH") == 0 && entry != 0) {
        triggerTutorial(8);
        auto& items = tutorialWatch_.items;
        if (items.size() < 8 && std::find(items.begin(), items.end(), entry) == items.end()) items.push_back(entry);
    } else if (std::strcmp(name, "UI_ERROR_MESSAGE") == 0 && arg == kInventoryFull) {
        triggerTutorial(58);
    }
}

void WorldSession::tutorialUpdate() {
    if (!tutorialFlagsReceived_ || !showTutorials_ || !selfKnown()) return;
    TutorialWatch& w = tutorialWatch_;
    const uint32_t now = logUptimeMs();
    if (now < w.nextMs) return;  // (the first poll waits for the login state to settle)
    w.nextMs = now + 250u;
    PROF_SCOPE("tutorial");

    const uint32_t level = selfField(game::fieldIndex(game::UF::UNIT_FIELD_LEVEL));
    const uint32_t pflags = selfField(game::fieldIndex(game::UF::PLAYER_FLAGS));
    const uint8_t life = static_cast<uint8_t>(this->life());
    const uint64_t tgt = target();
    const bool combat = inCombat();
    const bool lootable = !lootable_.empty();
    uint32_t complete = 0;
    for (const auto& q : questLog_) complete += q.complete ? 1u : 0u;
    int64_t repSum = 0;
    for (const auto& f : initialFactions_) repSum += f.standing;
    const uint32_t known = static_cast<uint32_t>(knownSpells_.size());
    const uint32_t quests = static_cast<uint32_t>(questLog_.size());
    const bool whisper = !lastWhisperSender_.empty();
    // Worn items below 20 % (the client's durability warning) / broken.
    bool damaged = false, broken = false;
    for (uint64_t g : equipSlotGuids_) {
        auto it = g ? onlineItems_.find(g) : onlineItems_.end();
        if (it == onlineItems_.end() || it->second.maxDurability == 0) continue;
        damaged |= it->second.curDurability < it->second.maxDurability / 5u;
        broken |= it->second.curDurability == 0;
    }

    if (!w.init) {  // what the character already has at login is not an event
        w.init = true;
        w.level = level;
        w.playerFlags = pflags;
        w.life = life;
        w.target = tgt;
        w.combat = combat;
        w.lootable = lootable;
        w.questsComplete = complete;
        w.questLog = quests;
        w.repSum = repSum;
        w.repCount = static_cast<uint32_t>(initialFactions_.size());
        w.knownSpells = known;
        w.vendor = vendorWindowOpen_;
        w.whisper = whisper;
        w.damaged = damaged;
        w.broken = broken;
        return;
    }
    if (level > w.level && w.level != 0) {
        triggerTutorial(55);  // Ding
        for (const auto& lt : kLevelTutorials)
            if (w.level < lt.level && level >= lt.level) triggerTutorial(lt.id);
    }
    if (life == static_cast<uint8_t>(Life::Dead) && w.life != life) triggerTutorial(25);          // Death
    if ((pflags & kPlayerFlagsResting) && !(w.playerFlags & kPlayerFlagsResting)) triggerTutorial(30);  // Resting
    if (tgt != 0 && tgt != w.target && tgt != playerGuid_) {
        auto e = entities_.getEntity(tgt);
        if (e && e->getType() == game::ObjectType::PLAYER) triggerTutorial(19);                   // Players
        else if (e && e->getType() == game::ObjectType::UNIT && isHostileUnit(tgt)) triggerTutorial(5);  // Combat1
    }
    if (combat && !w.combat) triggerTutorial(57);                                                 // EnemyHealth
    if (lootable && !w.lootable) triggerTutorial(7);                                              // LootPig
    if (quests > w.questLog && w.questLog == 0) triggerTutorial(21);                              // QuestLog
    if (complete > w.questsComplete) triggerTutorial(34);                                         // QuestComplete
    if (vendorWindowOpen_ && !w.vendor) triggerTutorial(20);                                      // Vendor
    if (whisper && !w.whisper) triggerTutorial(17);                                               // Reply
    if (repSum != w.repSum && w.repCount != 0 && w.repCount == initialFactions_.size()) triggerTutorial(16);  // Rep
    if (known > w.knownSpells && w.knownSpells != 0) triggerTutorial(15);                         // SpellBook
    if (damaged && !w.damaged) triggerTutorial(36);                                               // DamagedItems
    if (broken && !w.broken) triggerTutorial(37);                                                 // BrokenItems
    // Pushed items: equippable -> Equip (24), a bag -> Bags (10), usable -> OnUseItem (9).
    for (auto it = w.items.begin(); it != w.items.end();) {
        const game::ItemQueryResponseData* info = getItemInfo(*it);
        if (!info) {
            ++it;
            continue;
        }
        if (info->itemClass == 1) triggerTutorial(10);                     // ITEM_CLASS_CONTAINER
        else if (info->inventoryType != 0) triggerTutorial(24);
        else if (info->itemClass == 0 && info->spells[0].spellId != 0 && info->spells[0].spellTrigger == 0)
            triggerTutorial(9);                                            // consumable with a use spell
        it = w.items.erase(it);
    }
    w.level = level;
    w.playerFlags = pflags;
    w.life = life;
    w.target = tgt;
    w.combat = combat;
    w.lootable = lootable;
    w.questsComplete = complete;
    w.questLog = quests;
    w.repSum = repSum;
    w.repCount = static_cast<uint32_t>(initialFactions_.size());
    w.knownSpells = known;
    w.vendor = vendorWindowOpen_;
    w.whisper = whisper;
    w.damaged = damaged;
    w.broken = broken;
}

} // namespace wowee::platform::psp
