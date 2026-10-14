// World-server session for the PSP, milestone 4: connect to the realm,
// authenticate with the auth-server session key, list and create characters.
//
// Uses upstream's WorldSocket (RC4 header crypt, framing) and packet
// builders/parsers (world_packets.cpp) unchanged; replaces only the part of
// GameHandler that drives the handshake - GameHandler itself is not built for
// the PSP yet (it pulls in rendering, audio and the addon layer). The sequence
// mirrors GameHandler::handleAuthChallenge / sendAuthSession /
// handleAuthResponse / requestCharacterList / handleCharEnum.
#pragma once

#include "game/character.hpp"
#include "game/entity.hpp"
#include "game/expansion_profile.hpp"
#include "game/opcode_table.hpp"
#include "game/world_packets.hpp"
#include "network/world_socket.hpp"
#include "game/chat_filters.hpp"  // Chat
#include "game/handler_types.hpp"  // spells: SpellCastType, UnitCastState
#include "game/spell_defines.hpp"  // spells: AuraSlot, ActionBarSlot
#include "game/quest_giver_status.hpp"                 // NPC
#include "platform/psp/psp_world_session_npc.hpp"  // NPC
#include "rendering/psp/psp_dbc_format.hpp"  // Factions
#include "game/inventory.hpp"  // Inventory
#include "game/packed_time.hpp"  // minimap: the realm's date (GameTimeFrame)
#include "platform/psp/psp_craft_db.hpp"  // trade skills: data/craft.pak
#include "game/calendar_data.hpp"        // expansion windows: the calendar (upstream parser)
#include "platform/psp/psp_wotlk_db.hpp"  // expansion windows: data/wotlk.pak
#include "platform/psp/psp_inspect_parse.hpp"  // inspect: SMSG_INSPECT_TALENT
#include "platform/psp/psp_spell_targets.hpp"  // item use: SpellCastTargets (CMSG_USE_ITEM / CMSG_CAST_SPELL)
#include "platform/psp/psp_auction_proto.hpp"  // auction: the auction house's packets and rules
#include "platform/psp/psp_game_line.hpp"      // GameLine: red error line or system chat
#include "platform/psp/psp_social_parse.hpp"  // social: the friends / who / guild packets
#include "platform/psp/psp_trade_duel_parse.hpp"  // trade and duels: the trade / duel / request packets
#include "platform/psp/psp_pet_text.hpp"  // pets: text.pak "pet_text", the pet bar's slot packing
#include "platform/psp/psp_classbar_data.hpp"  // class bars: text.pak "class_bars" / "class_vehicles"
#include "platform/psp/psp_glyph_bank_parse.hpp"  // glyphs, the guild bank, chat channels

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee::platform::psp {

class SpellDb;
struct SpellInfo;

class WorldSession {
public:
    enum class State : uint8_t {
        Idle,
        Connecting,
        WaitChallenge,
        WaitAuthResponse,
        Ready,           // authenticated, nothing pending
        WaitCharList,
        WaitCharCreate,
        WaitCharDelete,  // CMSG_CHAR_DELETE sent, waiting for SMSG_CHAR_DELETE
        WaitWorld,       // CMSG_PLAYER_LOGIN sent, waiting for SMSG_LOGIN_VERIFY_WORLD
        InWorld,
        Failed,
    };

    // Loads the expansion registry + opcode table from <dataRoot>/expansions
    // (card: ms0:/PSP/GAME/WOWEEPSP/Data). Once per process.
    static bool initTables(const std::string& dataRoot, const std::string& expansionId);

    WorldSession();
    ~WorldSession();

    // Blocking TCP connect (up to ~5 s), then the handshake runs in update().
    bool begin(const std::string& host, uint16_t port, const std::string& account,
               const std::vector<uint8_t>& sessionKey, uint32_t realmId);
    void update();
    void close();
    // Debug window "Drop": closes the socket the way a server kick does
    // (the next update() fails the session: GLUE_DISCONNECTED).
    void debugDropConnection();
    // CMSG_KEEP_ALIVE while the world loads (the loading screen's steps, LoginScreen::loadingHook);
    // true when sent.
    bool keepAlive();

    void requestCharacterList();
    void createCharacter(const game::CharCreateData& data);
    // CMSG_CHAR_DELETE (upstream GameHandler::deleteCharacter); the answer
    // (SMSG_CHAR_DELETE, ResponseCodes 0x46..0x4B) bumps deleteSeq() and the
    // list is asked for again, as upstream does.
    void deleteCharacter(uint64_t characterGuid);
    // Milestone 5: log the character into the world.
    void enterWorld(uint64_t characterGuid);
    // In world: sends a MSG_MOVE_* for the player (upstream MovementPacket;
    // server coordinates, time = client ms as in the time-sync replies).
    void sendMovement(game::Opcode opcode, game::MovementInfo info);
    // CMSG_STANDSTATECHANGE (upstream SocialHandler::setStandState).
    void sendStandState(uint8_t state);
    // weapons: CMSG_SET_SHEATHED (u32 sheath state: 0 put away, 1 melee, 2 ranged) - the
    // original client tells the server when its weapons come out or go away, the others see it in UNIT_FIELD_BYTES_2
    // (upstream sends nothing: its Z only moves the own model's weapons).
    void sendSetSheathed(uint8_t state);
    // weapons: own ranged auto-attack shots (Auto Shot / Shoot / Throw SMSG_SPELL_GO) so far - the
    // ranged weapon in hand while shooting (upstream meleeSwingCallback -> showRangedWeapon).
    uint32_t ownRangedShots() const { return ownRangedShots_; }
    // CMSG_TEXT_EMOTE (upstream ChatHandler::sendTextEmote): EmotesText.dbc id;
    // the server answers everyone around with SMSG_EMOTE (+ the text).
    void sendTextEmote(uint32_t textEmoteId);
    const game::LoginVerifyWorldData& worldEntry() const { return worldEntry_; }

    State state() const { return state_; }
    const char* stateName() const;
    const std::string& failure() const { return failure_; }
    const std::vector<game::Character>& characters() const { return characters_; }
    bool charListFresh() const { return charListFresh_; }
    void clearCharListFresh() { charListFresh_ = false; }
    // Last SMSG_CHAR_CREATE result (CharCreateResult) or -1.
    int lastCreateResult() const { return lastCreateResult_; }
    // Last SMSG_CHAR_DELETE result and a counter of the answers (a repeated
    // code is still a new answer).
    int lastDeleteResult() const { return lastDeleteResult_; }
    uint32_t deleteSeq() const { return deleteSeq_; }

    // M10a: objects around the player (upstream UpdateObjectParser +
    // EntityManager; positions canonical, see core/coordinates.hpp).
    const game::EntityManager& entities() const { return entities_; }
    uint64_t playerGuid() const { return playerGuid_; }
    // Bumped whenever a unit appears, disappears or changes its look.
    uint32_t entityVersion() const { return entityVersion_; }
    // Upstream unitAnimHintCallback (MSG_MOVE_JUMP -> anim 38) and
    // unitMoveFlagsCallback (WALKING): taken by the world screen.
    bool takeJumpHint(uint64_t guid) { return jumpHints_.erase(guid) != 0; }
    bool isWalking(uint64_t guid) const { return walking_.count(guid) != 0; }
    // SMSG_INIT_WORLD_STATES zone id (upstream GameHandler::getWorldStateZoneId).
    uint32_t worldStateZoneId() const { return worldStateZone_; }
    // Our own player's update fields (the own object is not an entity here).
    // (the same store as the combat code's selfField - after the
    // batch 2 merge combatUpdateBlock took the own block first and the emote
    // state never arrived here: /dance did not play.)
    uint32_t ownField(uint16_t index) const { return selfField(index); }
    // SMSG_EMOTE (emote id, source guid), oldest first; the world screen maps
    // it through Emotes.dbc like upstream's emoteAnimCallback.
    bool takeEmote(uint64_t& guid, uint32_t& emoteId) {
        if (emotes_.empty()) return false;
        guid = emotes_.front().first;
        emoteId = emotes_.front().second;
        emotes_.erase(emotes_.begin());
        return true;
    }
    // PSP (Wi-Fi jitter): where another player is now, predicted from their
    // last movement packet the way the original client does - position + speed x
    // direction from the movement flags x time since the packet (sender clock,
    // mapped with the smallest seen latency), ballistic z while FALLING.
    // Server coordinates; false when there is nothing to predict from (on a
    // transport, no packet yet). `moving` = locomotion flags set.
    bool predictRemote(uint64_t guid, glm::vec3& pos, float& yaw, bool& moving, bool& falling) const;
    const std::string* nameOf(uint64_t guid) const;

    // M10c: equipment of another player, 19 visible slots in EquipSlot order
    // (upstream InventoryHandler::emitOtherPlayerEquipment): item entries from
    // PLAYER_VISIBLE_ITEM_n (field 283 + 2 n), resolved to ItemDisplayInfo ids
    // and inventory types by CMSG_ITEM_QUERY_SINGLE. False while entries exist
    // but none has been answered yet (so an unanswered query never strips a
    // model). `unresolved` (optional): entries still waiting for their query
    // (the PSP re-dresses once they are all in, see WorldScreen).
    bool equipmentOf(uint64_t guid, uint32_t (&displayIds)[19], uint8_t (&invTypes)[19],
                     int* unresolved = nullptr) const;
    // item displays: the same with the client's Item.dbc first - `dbc(user, entry, displayId,
    // invType)` answers from items.pak (rendering::psp::ItemBank::itemDisplay), as the original client dresses
    // other players from its own Item.dbc; the item query above only for the entries the DBC lacks (a server's
    // custom items). The queries are still sent as upstream does.
    using ItemDbcLookup = bool (*)(void* user, uint32_t entry, uint32_t& displayId, uint8_t& invType);
    bool equipmentOf(uint64_t guid, uint32_t (&displayIds)[19], uint8_t (&invTypes)[19], int* unresolved,
                     ItemDbcLookup dbc, void* user) const;
    // Advances remote movement interpolation (upstream Entity::updateMovement).
    void updateEntities(float dtSec);

private:
    static bool initTablesImpl(const std::string& dataRoot, const std::string& expansionId);
    void onPacket(const network::Packet& packet);
    // PSP: Warden (psp_warden.cpp over psp_warden_core.hpp) - every SMSG_WARDEN_DATA, in any state.
    struct WardenState;
    std::shared_ptr<WardenState> warden_;  // shared_ptr: the type stays incomplete outside psp_warden.cpp
    void wardenBegin();
    void wardenEnd();
    void handleWardenData(network::Packet& packet);
    void handleUpdateObject(network::Packet& packet);
    void applyUpdateBlock(const game::UpdateBlock& block);
    void handleOtherMovement(network::Packet& packet, bool stop, bool jump = false);
    void removeEntity(uint64_t guid);
    void fail(const std::string& reason);
    void setState(State s);

    std::unique_ptr<network::WorldSocket> socket_;
    State state_ = State::Idle;
    std::string failure_;
    std::string account_;
    std::vector<uint8_t> sessionKey_;
    uint32_t realmId_ = 1;
    uint32_t clientSeed_ = 0;
    std::vector<game::Character> characters_;
    bool charListFresh_ = false;
    int lastCreateResult_ = -1;
    int lastDeleteResult_ = -1;
    uint32_t deleteSeq_ = 0;
    uint64_t stateSinceUs_ = 0;
    game::LoginVerifyWorldData worldEntry_{};
    uint64_t playerGuid_ = 0;
    uint64_t lastPingUs_ = 0;
    uint32_t pingSeq_ = 0;
    uint32_t timeSyncs_ = 0;

    game::EntityManager entities_;
    uint32_t entityVersion_ = 0;
    std::unordered_map<uint64_t, std::string> names_;
    std::unordered_set<uint64_t> nameQueried_;
    std::unordered_map<uint64_t, uint32_t> moveTimeMs_;       // last MSG_MOVE time per mover
    std::unordered_map<uint64_t, float> moveIntervalMs_;      // smoothed packet interval
    std::unordered_set<uint64_t> jumpHints_;
    std::unordered_set<uint64_t> walking_;
    uint32_t worldStateZone_ = 0;
    std::vector<std::pair<uint64_t, uint32_t>> emotes_;
    struct RemoteMotion {
        game::MovementInfo info;
        int64_t clockOffsetMs = 0;  // local ms - sender ms, smallest seen (+ slow drift)
    };
    std::unordered_map<uint64_t, RemoteMotion> motion_;
    uint32_t updateBlocks_ = 0;
    struct ItemInfo {
        uint32_t displayId = 0;
        uint8_t invType = 0;
    };
    std::unordered_map<uint32_t, ItemInfo> itemInfo_;   // entry -> resolved (upstream itemInfoCache_)
    std::unordered_set<uint32_t> itemQueried_;
    void queryVisibleItems(const game::Entity& e);

    // Chat (psp_world_session_chat.cpp): upstream ChatHandler's
    // receive and send path without GameHandler, as M10a mirrors the
    // EntityController. The chat window (ui_world_chat.cpp) takes the lines.
public:
    // SMSG_TEXT_EMOTE with the sender's name resolved; the window writes the
    // line from the EmotesText table (upstream getEmoteTextByDbcId).
    struct TextEmoteHeard {
        uint64_t sender = 0;
        std::string senderName;
        uint32_t textEmoteId = 0;
        std::string targetName;
        uint8_t gender = 2;  // sender's UNIT_FIELD_BYTES_0 byte 2 (0 male, 1 female, 2 unknown)
    };
    // CMSG_MESSAGECHAT + the local echo (upstream ChatHandler::sendChatMessage).
    void sendChatMessage(game::ChatType type, const std::string& message, const std::string& target = "");
    // CMSG_TEXT_EMOTE with a target (upstream ChatHandler::sendTextEmote).
    void sendTextEmoteAt(uint32_t textEmoteId, uint64_t targetGuid);
    // A line made by the client itself (upstream addSystemChatMessage).
    void addSystemChatMessage(const std::string& text);
    // A finished line (our own TEXT_EMOTE lines come from the window).
    void addLocalChatMessage(const game::MessageChatData& msg);
    bool takeChatLine(game::MessageChatData& out);
    bool takeTextEmote(TextEmoteHeard& out);
    const std::string& lastWhisperSender() const { return lastWhisperSender_; }
    // Our character (the CHAR_ENUM row of playerGuid()), or nullptr.
    const game::Character* activeCharacter() const;
    // UI errors: refusals the original shows in UIErrorsFrame (upstream addUIError /
    // raiseUiError); the world screen takes them into its error stack (ui_world_errors.cpp).
    void addUiError(const std::string& text);
    bool takeUiError(std::string& out);
    // A game line by the port's rule (psp_game_line.hpp): Refused -> the red error line, News -> system chat.
    void gameLine(GameLine kind, const std::string& text) {
        if (kind == GameLine::Refused) addUiError(text);
        else addSystemChatMessage(text);
    }

private:
    bool handleChatPacket(game::LogicalOpcode op, network::Packet& packet);
    void handleMessageChat(network::Packet& packet);
    void deliverChatMessage(game::MessageChatData data, bool alreadyWaited);
    void handleTextEmote(network::Packet& packet);
    void chatUpdate();  // lines waiting for a name: released when it lands or after 1 s
    std::string knownChatName(uint64_t guid) const;
    struct ChatAwaitingName {
        game::MessageChatData data;
        uint64_t guid = 0;
        uint64_t deadlineUs = 0;
    };
    std::vector<ChatAwaitingName> chatAwaitingName_;
    std::deque<game::MessageChatData> chatLines_;
    std::deque<TextEmoteHeard> textEmotesHeard_;
    std::deque<game::RecentChatLine> recentChatLines_;
    std::string lastWhisperSender_;
    uint32_t chatRxLogs_ = 0;
    uint32_t chatTxLogs_ = 0;
    std::deque<std::string> uiErrors_;  // UI errors: addUiError -> the world screen
    // Chat (end)
    // Combat - target, melee, creature names, death
    // (psp_world_session_combat.cpp; upstream CombatHandler / EntityController).
public:
    struct MeleeHit {  // SMSG_ATTACKERSTATEUPDATE, upstream AttackerStateUpdateParser
        uint64_t attacker = 0, victim = 0;
        int32_t damage = 0;
        uint32_t hitInfo = 0, victimState = 0, blocked = 0, absorbed = 0, resisted = 0;
    };
    enum class AttackError : uint8_t { None, TooFar, NotInRange, BadFacing, NotStanding, CantAttack, OutOfRange };
    enum class Life : uint8_t { Alive, Dead, Ghost };
    void setTarget(uint64_t guid);  // CMSG_SET_SELECTION (0 = clear)
    uint64_t target() const { return targetGuid_; }
    // selfPos: the player's feet, server coordinates (upstream movementInfo).
    void startAutoAttack(uint64_t guid, const glm::vec3& selfPos);
    void stopAutoAttack();
    void updateAutoAttack(float dtSec, const glm::vec3& selfPos);
    bool autoAttackRequested() const { return autoAttackRequested_; }
    bool autoAttacking() const { return autoAttacking_; }
    uint64_t autoAttackTarget() const { return autoAttackTarget_; }
    bool inCombat() const { return autoAttacking_ || !hostileAttackers_.empty(); }
    bool isHostileAttacker(uint64_t guid) const { return hostileAttackers_.count(guid) != 0; }
    bool takeMeleeHit(MeleeHit& out);
    AttackError takeAttackError();
    uint32_t selfField(uint16_t index) const;
    bool selfKnown() const { return !selfFields_.empty(); }
    Life life() const;
    void releaseSpirit();  // CMSG_REPOP_REQUEST (+ MSG_CORPSE_QUERY)
    const game::CreatureQueryResponseData* creatureInfo(uint32_t entry) const;
    uint32_t creatureInfoVersion() const { return creatureInfoVersion_; }

    // Death -> ghost -> corpse run -> resurrection (psp_world_session_death.cpp):
    // upstream MovementHandler::handleTeleportAck, EntityController::markPlayerDead /
    // onCreateCorpse / the PLAYER_FLAGS ghost edges, CombatHandler::canReclaimCorpse /
    // reclaimCorpse / acceptResurrect / declineResurrect / activateSpiritHealer and the
    // GameHandler handlers of MSG_CORPSE_QUERY, SMSG_CORPSE_RECLAIM_DELAY,
    // SMSG_DEATH_RELEASE_LOC, SMSG_RESURRECT_REQUEST and SMSG_SPIRIT_HEALER_CONFIRM.
    // Positions: server coordinates (the world screen's playerX_/Y_/Z_).
    struct ResurrectOffer {
        bool pending = false;       // upstream resurrectRequestPending_
        uint64_t caster = 0;
        std::string name;           // the caster (SMSG_RESURRECT_REQUEST), empty for a spirit healer
        bool spiritHealer = false;  // SMSG_SPIRIT_HEALER_CONFIRM (the CONFIRM_XP_LOSS prompt)
        bool sickness = false;      // ResurrectHasSickness
        bool hasTimer = true;       // ResurrectHasTimer
        uint32_t seq = 0;           // bumped by every offer
    };
    // A MSG_MOVE_TELEPORT_ACK for us, already acknowledged to the server: the world screen
    // puts the player there and sends MSG_MOVE_STOP + HEARTBEAT (upstream does both in
    // handleTeleportAck; the PSP's position lives in the screen). False: none pending.
    bool takeTeleport(glm::vec3& pos, float& orientation);
    void markPlayerDead(const glm::vec3& pos);  // the corpse where we fell (upstream markPlayerDead)
    void queryCorpse();                         // MSG_CORPSE_QUERY
    bool corpseKnown() const { return corpseValid_ && corpseMapId_ == worldEntry_.mapId; }
    const glm::vec3& corpsePosition() const { return corpsePos_; }
    bool canReclaimCorpse(const glm::vec3& selfPos) const;
    float corpseReclaimDelaySec() const;        // GetCorpseRecoveryDelay
    void reclaimCorpse(const glm::vec3& selfPos);  // CMSG_RECLAIM_CORPSE (RetrieveCorpse)
    // SMSG_DEATH_RELEASE_LOC: where the spirit healer is (this map only).
    bool graveyardKnown() const { return graveyardValid_ && graveyardMapId_ == worldEntry_.mapId; }
    const glm::vec3& graveyardPosition() const { return graveyardPos_; }
    const ResurrectOffer& resurrectOffer() const { return resOffer_; }
    void acceptResurrect();   // CMSG_RESURRECT_RESPONSE 1 / CMSG_SPIRIT_HEALER_ACTIVATE (AcceptXPLoss)
    void declineResurrect();  // CMSG_RESURRECT_RESPONSE 0
    void forgetResurrectOffer() { resOffer_.pending = false; }

private:
    bool handleDeathPacket(game::LogicalOpcode op, network::Packet& packet);
    void deathOwnFields();                         // ghost flag edges of our PLAYER_FLAGS
    void deathUpdateBlock(const game::UpdateBlock& b);  // our CORPSE object (onCreateCorpse)
    bool teleportPending_ = false;
    glm::vec3 teleportPos_{0.0f};
    float teleportO_ = 0.0f;
    bool ghostSeen_ = false;
    bool corpseValid_ = false;
    uint32_t corpseMapId_ = 0;
    glm::vec3 corpsePos_{0.0f};
    uint64_t corpseGuid_ = 0;
    uint32_t corpseReclaimAtMs_ = 0;  // logUptimeMs when the reclaim delay ends (0 = none)
    bool graveyardValid_ = false;
    uint32_t graveyardMapId_ = 0;
    glm::vec3 graveyardPos_{0.0f};
    ResurrectOffer resOffer_;
    uint32_t deathLogs_ = 0;

private:
    bool handleCombatPacket(game::LogicalOpcode op, network::Packet& packet);
    bool combatUpdateBlock(const game::UpdateBlock& b);
    void queryCreature(uint32_t entry, uint64_t guid);
    void autoTargetAttacker(uint64_t guid);
    void raiseAttackError(AttackError e);
    uint64_t targetGuid_ = 0;
    bool autoAttackRequested_ = false, autoAttacking_ = false, autoAttackRetryPending_ = false;
    bool autoAttackOutOfRange_ = false;
    uint64_t autoAttackTarget_ = 0;
    float autoAttackOutOfRangeTime_ = 0.0f, autoAttackResendTimer_ = 0.0f, rangeWarnCooldown_ = 0.0f;
    std::unordered_set<uint64_t> hostileAttackers_;
    std::vector<MeleeHit> meleeHits_;
    AttackError attackError_ = AttackError::None;
    std::unordered_map<uint16_t, uint32_t> selfFields_;
    bool forcedDead_ = false;
    uint32_t lastRepopMs_ = 0;
    std::unordered_map<uint32_t, game::CreatureQueryResponseData> creatureInfo_;
    std::unordered_set<uint32_t> creatureQueried_;
    uint32_t creatureInfoVersion_ = 0;
    uint32_t combatLogs_ = 0;
    // Creature anim - NPC movement (psp_world_session_move.cpp).
public:
    // Upstream unitMoveFlagsCallback state of a unit: WALKING / SWIMMING /
    // FLYING from the spline mode opcodes and the create block (0 = none).
    uint32_t unitMoveFlags(uint64_t guid) const;
    // PSP: npc-walk - Walk (true) or Run for a moving unit: the gait of its
    // last SMSG_MONSTER_MOVE (psp_move_anim.hpp), else the WALKING move flag.
    bool unitWalkAnim(uint64_t guid) const;
    // PSP: npc-walk - FlyForward / FlyIdle: FLYING / CAN_FLY or a MoveSplineFlag::Flying spline (see the .cpp).
    bool unitFlyAnim(uint64_t guid) const;

private:
    void handleNpcMove(game::LogicalOpcode op, network::Packet& packet);
    void handleMonsterMove(network::Packet& packet);
    void handleCompressedMoves(network::Packet& packet);
    void setUnitMoveFlags(uint64_t guid, uint32_t flags, uint32_t mask);
    // PSP: npc-walk - the spline's ground speed and Walk/Run gait of a unit (both monster move opcodes).
    void noteSplineGait(const game::MonsterMoveData& d);
    std::unordered_map<uint64_t, uint32_t> unitMoveFlags_;
    std::unordered_map<uint64_t, uint8_t> splineGait_;  // PSP: npc-walk - gait of the unit's last spline
    uint32_t monsterMoves_ = 0, monsterMoveLogs_ = 0, monsterMoveFails_ = 0;

    // Spells (psp_world_session_spells.cpp): upstream SpellHandler's cast
    // state machine, cooldowns / GCD, auras, known spells and the action bar
    // (GameHandler::actionBar + SMSG_ACTION_BUTTONS) without GameHandler.
    // The world screen (ui_world_spells.cpp) draws it and plays the cast
    // animations (upstream spellCastAnimCallback -> takeCastAnim).
public:
    static constexpr int kActionBarSlots = 144;  // upstream GameHandler::ACTION_BAR_SLOTS (12 x 12)
    struct CastAnim {  // one upstream spellCastAnimCallback call
        uint64_t guid = 0;
        bool start = false, isChannel = false;
        game::SpellCastType castType = game::SpellCastType::OMNI;
        uint32_t spellId = 0;
        bool melee = false;  // upstream meleeSwingCallback (an instant melee ability)
    };
    struct SpellText {  // upstream addCombatText from the spell logs / SPELL_GO misses
        uint8_t type = 0;  // CombatTextEntry::Type
        int32_t amount = 0;
        uint32_t spellId = 0;
        bool outgoing = false;
        uint64_t src = 0, dst = 0;
    };
    void setSpellDb(SpellDb* db) { spellDb_ = db; }
    // upstream SpellHandler::castSpell (target 0 = the current target);
    // moving = the player's horizontal movement flags are set.
    void castSpell(uint32_t spellId, uint64_t targetGuid, bool moving);
    void cancelCast();
    void cancelAura(uint32_t spellId);
    // upstream GameHandler::setActionBarSlot (CMSG_SET_ACTION_BUTTON).
    void setActionBarSlot(int slot, game::ActionBarSlot::Type type, uint32_t id);
    const std::array<game::ActionBarSlot, kActionBarSlots>& actionBar() const { return actionBar_; }
    const std::unordered_set<uint32_t>& knownSpells() const { return knownSpells_; }
    uint32_t knownSpellsVersion() const { return knownSpellsVersion_; }
    bool isCasting() const { return casting_; }
    bool isChanneling() const { return casting_ && castIsChannel_; }
    uint32_t currentCastSpellId() const { return currentCastSpellId_; }
    float castProgress() const {
        return castTimeTotal_ > 0.0f ? (castTimeTotal_ - castTimeRemaining_) / castTimeTotal_ : 0.0f;
    }
    float castTimeRemaining() const { return castTimeRemaining_; }
    float castTimeTotal() const { return castTimeTotal_; }
    float spellCooldown(uint32_t spellId) const;
    float spellCooldownTotal(uint32_t spellId) const;
    float gcdRemaining() const;
    float gcdTotal() const { return gcdTotal_; }
    const std::vector<game::AuraSlot>& playerAuras() const { return playerAuras_; }
    const std::vector<game::AuraSlot>* unitAuras(uint64_t guid) const;
    const game::UnitCastState* unitCast(uint64_t guid) const;
    static uint64_t auraNowMs();  // the clock of AuraSlot::receivedAtMs
    bool takeCastAnim(CastAnim& out);
    bool takeSpellText(SpellText& out);
    // A cast refusal / failure for the red error line (upstream addUIError).
    bool takeSpellError(std::string& out);
    // Last cast event of the player for the cast bar: 1 = succeeded,
    // 2 = failed / interrupted (upstream UNIT_SPELLCAST_STOP / _FAILED /
    // _INTERRUPTED of "player"); taken once.
    uint8_t takeCastEnd(uint32_t& spellId);
    void updateSpells(float dtSec);

private:
    bool handleSpellPacket(game::LogicalOpcode op, network::Packet& packet);
    void handleInitialSpells(network::Packet& packet);
    void handleCastFailed(network::Packet& packet);
    void handleSpellStart(network::Packet& packet);
    void handleSpellGo(network::Packet& packet);
    void handleSpellCooldown(network::Packet& packet);
    void handleAuraUpdate(network::Packet& packet, bool isAll);
    void handleSpellFailure(network::Packet& packet);
    void handleChannelStart(network::Packet& packet);
    void handleChannelUpdate(network::Packet& packet);
    void handleActionButtons(network::Packet& packet);
    void seedCooldownFromSpellInfo(uint32_t spellId);
    void clearOwnCast();
    void pushCastAnim(uint64_t guid, bool start, bool isChannel, game::SpellCastType type, uint32_t spellId);
    void pushSpellText(uint8_t type, int32_t amount, uint32_t spellId, bool outgoing, uint64_t src, uint64_t dst);
    void spellError(const std::string& text);
    const SpellInfo* spellInfo(uint32_t spellId) const;
    SpellDb* spellDb_ = nullptr;
    std::unordered_set<uint32_t> knownSpells_;
    uint32_t knownSpellsVersion_ = 0;
    std::array<game::ActionBarSlot, kActionBarSlots> actionBar_{};
    bool casting_ = false, castIsChannel_ = false;
    uint32_t currentCastSpellId_ = 0;
    uint32_t ownRangedShots_ = 0;  // weapons
    float castTimeRemaining_ = 0.0f, castTimeTotal_ = 0.0f;
    uint8_t castCount_ = 0;
    uint32_t queuedSpellId_ = 0;
    // The last spell the player asked for (castSpell) and when -
    // an instant SMSG_SPELL_GO poses the character only for it (the server's
    // own casts on us, e.g. 836 LOGINEFFECT at world entry, raised the arm).
    uint32_t requestedSpellId_ = 0;
    uint32_t requestedAtMs_ = 0;
    uint64_t queuedSpellTarget_ = 0;
    std::unordered_map<uint32_t, float> spellCooldowns_, spellCooldownTotals_;
    float gcdTotal_ = 0.0f;
    uint64_t gcdStartedUs_ = 0;
    std::vector<game::AuraSlot> playerAuras_;
    std::unordered_map<uint64_t, std::vector<game::AuraSlot>> unitAuras_;
    std::unordered_map<uint64_t, game::UnitCastState> unitCastStates_;
    std::deque<CastAnim> castAnims_;
    std::deque<SpellText> spellTexts_;
    std::deque<std::string> spellErrors_;
    uint8_t castEnd_ = 0;
    uint32_t castEndSpell_ = 0;
    uint32_t spellLogs_ = 0;
    // Loot - loot and loot money (psp_world_session_loot.cpp):
    // upstream InventoryHandler's loot half with GameHandler's names, the
    // data model task A's Lua adapter reads (LootFrame: GetNumLootItems /
    // GetLootSlotInfo / LootSlot / CloseLoot).
public:
    struct LootItemInfo {  // from SMSG_ITEM_QUERY_SINGLE_RESPONSE (upstream ItemQueryResponseData subset)
        std::string name;
        uint8_t quality = 1;   // ItemQuality
        uint8_t bindType = 0;  // 1 = bind on pickup
    };
    void lootTarget(uint64_t guid);                              // CMSG_LOOT
    bool lootItem(uint8_t slotIndex, bool confirmed = false);    // CMSG_AUTOSTORE_LOOT_ITEM
    bool hasPendingLootConfirm() const { return pendingLootActive_; }  // LOOT_BIND_CONFIRM
    void confirmPendingLoot();
    void lootMoney();                                            // CMSG_LOOT_MONEY
    void closeLoot();                                            // CMSG_LOOT_RELEASE
    bool isLootWindowOpen() const { return lootWindowOpen_; }
    const game::LootResponseData& getCurrentLoot() const { return currentLoot_; }
    const std::vector<game::LootItem>& getLootItems() const { return currentLoot_.items; }
    uint32_t getLootMoney() const { return currentLoot_.goldLeft() ? currentLoot_.gold : 0u; }
    uint64_t getLootGuid() const { return currentLoot_.lootGuid; }
    void setAutoLoot(bool on) { autoLoot_ = on; }
    bool isAutoLoot() const { return autoLoot_; }
    // Corpse opened before and still holding items (upstream localLootState_,
    // what CombatHandler::tabTarget lets corpses cycle by).
    bool hasLocalLootItems(uint64_t guid) const;
    const LootItemInfo* lootItemInfo(uint32_t entry) const;
    // Bumped on every change of the loot (open, slot taken, money, close).
    uint32_t lootVersion() const { return lootVersion_; }
    // Dead units marked UNIT_DYNFLAG_LOOTABLE (for the sparkle), refreshed by updateLoot.
    const std::vector<uint64_t>& lootableCorpses() const { return lootable_; }
    // Timers (money announce fallback) + the lootable scan + upstream's local
    // despawn of an emptied corpse. `despawnLooted` = wowee.cfg lootdespawn.
    void updateLoot(float dtSec, bool despawnLooted);

private:
    bool handleLootPacket(game::LogicalOpcode op, network::Packet& packet);
    void handleLootResponse(network::Packet& packet);
    void handleLootReleaseResponse();
    void handleLootRemoved(network::Packet& packet);
    void announceLootMoney(uint64_t lootGuid, uint32_t amount);
    void clearLootMoney();
    void queryLootItem(uint32_t entry);
    struct LocalLootState {
        game::LootResponseData data;
        bool moneyTaken = false;
        bool itemAutoLootSent = false;
    };
    game::LootResponseData currentLoot_;
    bool lootWindowOpen_ = false, autoLoot_ = false, pendingLootActive_ = false;
    uint8_t pendingLootSlot_ = 0;
    std::unordered_map<uint64_t, LocalLootState> localLootState_;
    std::unordered_map<uint64_t, float> recentLootMoneyAnnounceCooldowns_;
    uint64_t pendingLootMoneyGuid_ = 0;
    uint32_t pendingLootMoneyAmount_ = 0;
    float pendingLootMoneyNotifyTimer_ = 0.0f;
    std::unordered_map<uint32_t, LootItemInfo> lootItemInfo_;
    std::vector<uint64_t> lootable_;
    float lootScanTimer_ = 0.0f;
    uint32_t lootVersion_ = 0, lootLogs_ = 0, lootableLogs_ = 0;
    // Loot (end)
    // NPC - gossip, vendor, quests (psp_world_session_npc.cpp):
    // upstream QuestHandler + the vendor half of InventoryHandler without
    // GameHandler. Names as GameHandler's, so the Lua-API adapter is thin.
public:
    using QuestLogEntry = NpcQuestLogEntry;
    void interactWithNpc(uint64_t guid);   // CMSG_GOSSIP_HELLO (upstream)
    void questgiverHello(uint64_t guid);   // PSP: CMSG_QUESTGIVER_HELLO (the original client, by NPC flags)
    bool isGossipWindowOpen() const { return gossipWindowOpen_; }
    const game::GossipMessageData& getCurrentGossip() const { return currentGossip_; }
    const std::vector<game::GossipOption>& getGossipOptions() const { return currentGossip_.options; }
    const std::string& getNpcText(uint32_t textId) const;
    const std::string& getQuestGreeting() const { return questGreeting_; }
    void selectGossipOption(uint32_t optionId, const std::string& code = "");
    void selectGossipQuest(uint32_t questId);
    void closeGossip();
    bool isQuestDetailsOpen() const { return questDetailsOpen_; }
    const game::QuestDetailsData& getQuestDetails() const { return currentQuestDetails_; }
    void acceptQuest();
    void declineQuest();
    bool isQuestRequestItemsOpen() const { return questRequestItemsOpen_; }
    const game::QuestRequestItemsData& getQuestRequestItems() const { return currentQuestRequestItems_; }
    void completeQuest();
    void closeQuestRequestItems();
    bool isQuestOfferRewardOpen() const { return questOfferRewardOpen_; }
    const game::QuestOfferRewardData& getQuestOfferReward() const { return currentQuestOfferReward_; }
    void chooseQuestReward(uint32_t rewardIndex);
    void closeQuestOfferReward();
    const std::vector<QuestLogEntry>& getQuestLog() const { return questLog_; }
    int questLogIndexOf(uint32_t questId) const;  // 1-based, 0 = not in the log
    void abandonQuest(uint32_t questId);
    bool requestQuestQuery(uint32_t questId, bool force = false);
    game::QuestGiverStatus getQuestGiverStatus(uint64_t guid) const;
    const std::unordered_map<uint64_t, game::QuestGiverStatus>& getNpcQuestStatuses() const { return npcQuestStatus_; }
    void openVendor(uint64_t npcGuid);
    void closeVendor();
    bool isVendorWindowOpen() const { return vendorWindowOpen_; }
    const game::ListInventoryData& getVendorItems() const { return currentVendorItems_; }
    void setVendorCanRepair(bool v) { currentVendorItems_.canRepair = v; }
    void buyItem(uint64_t vendorGuid, uint32_t itemId, uint32_t slot, uint32_t count);
    void sellItem(uint64_t vendorGuid, uint64_t itemGuid, uint32_t count);
    bool sellBackpackSlot(int index);  // PSP: the first-bag debug sale (task C owns the bags)
    void repairAll(uint64_t vendorGuid, bool useGuildBank = false);
    // MerchantFrame - the server's buyback ring (PLAYER_FIELD_VENDORBUYBACK_SLOT_1,
    // BUYBACK_PRICE_1, BUYBACK_TIMESTAMP_1; wire slots 74..85, as upstream's kBuybackWireSlotStart)
    // read straight from our player fields (upstream mirrors it beside them), the sale of a bag
    // item by its guid (the original's right click with the merchant open) and the buyback.
    struct BuybackRow {
        uint32_t wireSlot = 0;
        uint64_t guid = 0;
        uint32_t itemId = 0, count = 1, price = 0, time = 0;
    };
    std::vector<BuybackRow> buybackRows() const;  // in slot order (GetBuybackItemInfo 1..12)
    void buybackItem(uint32_t wireSlot);
    bool sellItemByGuid(uint64_t itemGuid);
    // An ItemDef from the item cache (no instance): tooltips of merchant / quest items.
    game::ItemDef protoItemDef(uint32_t itemId, uint32_t count = 1);
    const NpcItemInfo* npcItemInfo(uint32_t entry) const;  // PSP: until task C's getItemInfo
    void npcQueryItem(uint32_t entry);
    // Per frame (upstream GameHandler::update's quest bookkeeping + closeIfTooFar).
    void npcUpdate(float dtSec, const glm::vec3& selfPos);

private:
    bool handleNpcPacket(game::LogicalOpcode op, network::Packet& packet);
    void npcUpdateBlock(const game::UpdateBlock& b);
    void handleGossipMessage(network::Packet& packet);
    void handleQuestgiverQuestList(network::Packet& packet);
    void classifyGossipQuests(bool updateQuestLog);
    void handleNpcTextUpdate(network::Packet& packet);
    void handleQuestDetails(network::Packet& packet);
    void handleQuestRequestItems(network::Packet& packet);
    void handleQuestOfferReward(network::Packet& packet);
    void handleQuestQueryResponse(network::Packet& packet);
    void handleListInventory(network::Packet& packet);
    bool hasQuestInLog(uint32_t questId) const;
    int findQuestLogSlotIndexFromServer(uint32_t questId) const;
    void addQuestToLocalLogIfMissing(uint32_t questId, const std::string& title, const std::string& objectives);
    bool resyncQuestLogFromServerSlots(bool forceQueryMetadata);
    void applyQuestStateFromFields();
    void applyPackedKillCountsFromFields(QuestLogEntry& quest);
    void clearPendingQuestAccept(uint32_t questId);
    void triggerQuestAcceptResync(uint32_t questId, uint64_t npcGuid, const char* reason);
    void requeryNearbyQuestGiverStatus();
    void sendQuestGiverStatusQuery(uint64_t guid);
    void sendQuestGiverStatusQueries();
    bool gossipWindowOpen_ = false, questDetailsOpen_ = false, questRequestItemsOpen_ = false;
    bool questOfferRewardOpen_ = false, vendorWindowOpen_ = false;
    game::GossipMessageData currentGossip_;
    std::string questGreeting_;
    game::QuestDetailsData currentQuestDetails_;
    game::QuestRequestItemsData currentQuestRequestItems_;
    game::QuestOfferRewardData currentQuestOfferReward_;
    game::ListInventoryData currentVendorItems_;
    uint32_t pendingTurnInQuestId_ = 0;
    uint64_t pendingTurnInNpcGuid_ = 0;
    bool pendingTurnInRewardRequest_ = false;
    uint32_t pendingBuyItemId_ = 0, pendingBuyItemSlot_ = 0, pendingBuyCount_ = 0;
    std::unordered_map<uint32_t, float> pendingQuestAcceptTimeouts_;
    std::unordered_map<uint32_t, uint64_t> pendingQuestAcceptNpcGuids_;
    std::vector<QuestLogEntry> questLog_;
    std::unordered_set<uint32_t> pendingQuestQueryIds_;
    std::unordered_map<uint64_t, game::QuestGiverStatus> npcQuestStatus_;
    std::unordered_map<uint32_t, std::string> npcTextCache_;
    std::unordered_map<uint32_t, NpcItemInfo> npcItems_;
    std::unordered_set<uint32_t> npcItemQueried_;
    float questGiverRequeryCooldown_ = 0.0f;
    bool questGiverRequeryPending_ = false;
    bool questFieldsDirty_ = false, loginQuestResync_ = true;
    float loginQuestResyncTimeout_ = 10.0f;
    uint32_t npcLogs_ = 0, questStatusLogs_ = 0, questUpdateLogs_ = 0;
    // NPC (end)
    // Factions + NPC walk/run (psp_world_session_factions.cpp,
    // psp_world_session_move.cpp). Upstream GameHandler's faction maps
    // (faction_hostility.cpp, isHostileFaction, isFriendlyFaction,
    // factionAtWarLive, unitReactionToPlayer) and SocialHandler's
    // SMSG_INITIALIZE_FACTIONS / SET_FACTION_STANDING / SET_FACTION_ATWAR.
public:
    // data/dbc.pak "factions" -> RAM (pak opened once and closed), the maps
    // for our race. False without the pak (callers then treat everyone as a
    // candidate: "factions_missing").
    bool loadFactions();
    bool factionsLoaded() const { return !factionReactions_.templates.empty(); }
    bool isHostileFaction(uint32_t factionTemplateId) const;
    bool isFriendlyFaction(uint32_t factionTemplateId) const;
    int factionAtWarLive(uint32_t factionTemplateId) const;
    // Upstream Unit::isHostile (UNIT_FIELD_FACTIONTEMPLATE through
    // isHostileFaction) and unitReactionToPlayer (1 hated .. 8 exalted).
    bool isHostileUnit(uint64_t guid) const;
    int unitReactionToPlayer(uint64_t guid) const;
    // SMSG_MONSTER_MOVE: the spline's ground speed (path length / duration),
    // 0 when unknown - the creature renderer scales Walk/Run by it.
    float unitSplineSpeed(uint64_t guid) const;

private:
    bool handleFactionPacket(game::LogicalOpcode op, network::Packet& packet);
    uint32_t unitFactionTemplate(uint64_t guid) const;
    struct FactionStandingInit {  // upstream GameHandler::FactionStandingInit
        uint8_t flags = 0;
        int32_t standing = 0;     // PSP: earned only; the race/class base is added when read
    };
    std::vector<FactionStandingInit> initialFactions_;  // by ReputationListID
    std::vector<rendering::psp::FactionRow> factionRows_;  // Faction.dbc rows with a standing (base standings)
    rendering::psp::FactionReactions factionReactions_;    // our race's view of every template
    uint8_t factionRace_ = 0, factionClass_ = 0;
    std::unordered_map<uint64_t, float> unitSplineSpeed_;
    // Factions + NPC walk/run (end)
    // character tabs (psp_world_session_chartabs.cpp): what the character window's Reputation and Skills
    // tabs read and send, as upstream - GameHandler::getFactionStanding (earned + factionBaseReputation),
    // setFactionAtWar / setFactionInactive / setWatchedFactionId, SocialHandler::handleSetFactionVisible,
    // SpellHandler::extractSkillFields (PLAYER_SKILL_INFO_1_1..) and GameHandler::unlearnSkill.
public:
    // upstream game::PlayerSkill (game_handler.hpp, not includable on the PSP).
    struct PlayerSkill {
        uint32_t skillId = 0;
        uint16_t value = 0;      // base + permanent item bonuses
        uint16_t maxValue = 0;
        uint16_t bonusTemp = 0;  // temporary buff bonus (food, potions, etc.)
        uint16_t bonusPerm = 0;  // permanent spec/misc bonus (rarely non-zero)
    };
    // SMSG_INITIALIZE_FACTIONS rows (by ReputationListID): the flags and the whole standing (earned + the
    // race / class base of data/dbc.pak, as upstream's getFactionStanding). False past the end.
    uint32_t factionStandingCount() const { return static_cast<uint32_t>(initialFactions_.size()); }
    bool factionStandingAt(uint32_t repListId, uint8_t& flags, int32_t& standing) const;
    // Bumped by every faction packet and change (FrameXML's UPDATE_FACTION).
    uint32_t factionVersion() const { return factionVersion_; }
    void setFactionAtWar(uint32_t repListId, bool atWar);       // CMSG_SET_FACTION_ATWAR
    void setFactionInactive(uint32_t repListId, bool inactive); // CMSG_SET_FACTION_INACTIVE
    void setWatchedFaction(int32_t repListId);                  // CMSG_SET_WATCHED_FACTION (-1 = none)
    int32_t watchedFactionIndex() const;  // PLAYER_FIELD_WATCHED_FACTION_INDEX (-1 = none)
    // The skills of PLAYER_SKILL_INFO_*, re-read when an update touched them; skillsVersion() changes with them
    // (SKILL_LINES_CHANGED).
    const std::vector<PlayerSkill>& playerSkills();
    uint32_t skillsVersion() { (void)playerSkills(); return skillsVersion_; }
    void unlearnSkill(uint32_t skillId);  // CMSG_UNLEARN_SKILL

private:
    void extractSkillFields();
    std::vector<PlayerSkill> playerSkills_;
    bool skillsDirty_ = true;
    uint32_t skillsVersion_ = 0, factionVersion_ = 0;
    // character tabs (end)
    // dressup (psp_world_session_dressup.cpp): the helm / cloak switches - upstream
    // GameHandler::isHelmVisible / isCloakVisible / toggleHelm / toggleCloak and the client's ShowingHelm /
    // ShowHelm / ShowingCloak / ShowCloak (InterfaceOptionsDisplayPanelShowHelm / -ShowCloak). The realm keeps
    // them in our PLAYER_FLAGS (PLAYER_FLAGS_HIDE_HELM 0x400, _HIDE_CLOAK 0x800); CMSG_SHOWING_HELM / _CLOAK
    // toggles the flag whatever its byte says, so a packet goes only when the asked state differs (as upstream's
    // ShowHelm). What was asked for stands until the realm's PLAYER_FLAGS shows it (or 3 s - upstream keeps its
    // copy until the next flags update).
public:
    bool isHelmVisible() const;
    bool isCloakVisible() const;
    void showHelm(bool show);   // ShowHelm(value)
    void showCloak(bool show);  // ShowCloak(value)

private:
    mutable int8_t helmAsked_ = -1, cloakAsked_ = -1;  // -1: none; else the state sent last
    uint32_t helmAskedMs_ = 0, cloakAskedMs_ = 0;
    uint32_t dressupLogs_ = 0;
    // dressup (end)
    // Inventory (psp_world_session_inventory.cpp) - upstream
    // InventoryHandler + EntityController item tracking without GameHandler.
    // Names and semantics are GameHandler's (getInventory, getItemInfo,
    // getMoneyCopper, autoEquipItemBySlot, swapContainerItems...) so the Lua
    // API adapter (task A) stays thin; events are upstream's addon events.
public:
    struct InventoryEvent {
        const char* name = "";  // BAG_UPDATE, UNIT_INVENTORY_CHANGED, PLAYER_MONEY, ITEM_PUSH,
                                // GET_ITEM_INFO_RECEIVED, PLAYER_EQUIPMENT_CHANGED,
                                // UI_ERROR_MESSAGE, AUTOEQUIP_BIND_CONFIRM
        int32_t arg = 0;        // bag id / bag button id / item entry / error code / wire slot
        uint32_t entry = 0;     // ITEM_PUSH: item id
        uint32_t count = 0;     // ITEM_PUSH: count
    };
    struct PendingEquip {  // upstream InventoryHandler::PendingEquip (bind-on-equip prompt)
        bool active = false, fromBag = false;
        int bag = 0, slot = 0;
        uint8_t wireSlot = 0;
    };
    const game::Inventory& getInventory() const { return *inventory_; }
    const game::ItemQueryResponseData* getItemInfo(uint32_t entry) const;
    const std::unordered_map<uint32_t, game::ItemQueryResponseData>& getItemInfoCache() const {
        return itemInfoCache_;
    }
    void queryItemInfo(uint32_t entry, uint64_t guid);
    void ensureItemInfo(uint32_t entry) { queryItemInfo(entry, 0); }
    uint64_t getMoneyCopper() const;
    uint64_t getEquipSlotGuid(int slot) const;
    uint64_t getBackpackItemGuid(int index) const;
    uint64_t getBagItemGuid(int bagIndex, int slotIndex) const;
    uint32_t getItemIdByGuid(uint64_t guid) const;
    uint64_t resolveOnlineItemGuid(uint32_t itemId) const;
    // Own equipment from the inventory (upstream resolvePlayerEquipment);
    // `unresolved` = worn items whose query has not come back yet.
    bool resolvePlayerEquipment(uint32_t (&displayIds)[19], uint8_t (&invTypes)[19], int* unresolved = nullptr) const;
    // PSP (wowee.cfg ownequip=2): the same from our PLAYER_VISIBLE_ITEM fields.
    bool resolvePlayerVisibleEquipment(uint32_t (&displayIds)[19], uint8_t (&invTypes)[19],
                                       int* unresolved = nullptr) const;
    uint32_t inventoryVersion() const { return inventoryVersion_; }   // bumped per rebuild
    uint32_t equipmentVersion() const { return equipmentVersion_; }   // upstream onlineEquipDirty
    bool takeInventoryEvent(InventoryEvent& out);
    const std::string& lastInventoryError(uint32_t* atMs = nullptr) const;
    void autoEquipItemBySlot(int backpackIndex, bool confirmed = false);
    void autoEquipItemInBag(int bagIndex, int slotIndex, bool confirmed = false);
    bool equipWouldBindFromBackpack(int backpackIndex) const;
    bool equipWouldBindFromBag(int bagIndex, int slotIndex) const;
    const PendingEquip& pendingEquip() const { return pendingEquip_; }
    void equipPendingItem();
    void cancelPendingEquip() { pendingEquip_ = PendingEquip{}; }
    void equipItemToSlot(uint64_t itemGuid, uint8_t equipSlot);
    void unequipToBackpack(game::EquipSlot equipSlot);
    void swapContainerItems(uint8_t srcBag, uint8_t srcSlot, uint8_t dstBag, uint8_t dstSlot,
                            bool applyLocally = true);
    void swapBagSlots(int srcBagIndex, int dstBagIndex);
    void destroyItem(uint8_t bag, uint8_t slot, uint8_t count = 1);
    void splitItemTo(uint8_t srcBag, uint8_t srcSlot, uint8_t dstBag, uint8_t dstSlot, uint8_t count);
    void splitItem(uint8_t srcBag, uint8_t srcSlot, uint8_t count);
    bool readWireSlot(uint8_t container, uint8_t slot, game::ItemDef& out) const;

private:
    struct OnlineItemInfo {  // upstream GameHandler::OnlineItemInfo
        uint32_t entry = 0, stackCount = 1, curDurability = 0, maxDurability = 0;
        uint32_t permanentEnchantId = 0, temporaryEnchantId = 0;
        std::array<uint32_t, 3> socketEnchantIds{};
        uint32_t flags = 0;
        int32_t randomPropertyId = 0;
        uint32_t suffixFactor = 0;
    };
    struct ContainerInfo {  // upstream containerContents_ entry
        uint32_t numSlots = 0;
        std::array<uint64_t, 36> slotGuids{};
    };
    struct PendingItemPush {
        uint32_t itemId = 0, count = 0;
        int bagButtonId = 0;
    };
    bool handleInventoryPacket(game::LogicalOpcode op, network::Packet& packet);
    bool inventoryUpdateBlock(const game::UpdateBlock& b);
    void inventoryForget(uint64_t guid);
    void inventoryUpdate();
    void handleItemQueryResponse(network::Packet& packet);
    void handleItemPushResult(network::Packet& packet);
    void handleInventoryChangeFailure(network::Packet& packet);
    void trackItemOnCreate(const game::UpdateBlock& b);
    void updateItemOnValues(const game::UpdateBlock& b);
    void detectInventorySlotBases(const game::FlatFieldMap& fields);
    bool applyInventoryFields(const game::FlatFieldMap& fields);
    void extractContainerFields(uint64_t containerGuid, const game::FlatFieldMap& fields);
    game::ItemDef buildItemDef(uint64_t guid, const OnlineItemInfo& info);
    void rebuildOnlineInventory();
    void fireBagUpdates();
    void pushInventoryEvent(const char* name, int32_t arg = 0, uint32_t entry = 0, uint32_t count = 0);
    void raiseInventoryError(const std::string& msg, int32_t code);
    bool writeWireSlot(uint8_t container, uint8_t slot, const game::ItemDef& item);
    std::unique_ptr<game::Inventory> inventory_ = std::make_unique<game::Inventory>();
    std::unordered_map<uint32_t, game::ItemQueryResponseData> itemInfoCache_;
    std::unordered_map<uint64_t, OnlineItemInfo> onlineItems_;
    std::unordered_map<uint64_t, ContainerInfo> containerContents_;
    std::array<uint64_t, 23> equipSlotGuids_{};
    std::array<uint64_t, 16> backpackSlotGuids_{};
    std::array<uint64_t, 32> keyringSlotGuids_{};
    std::array<uint64_t, 28> bankSlotGuids_{};
    std::array<uint64_t, 7> bankBagSlotGuids_{};
    std::array<uint32_t, 19> lastEquipDisplayIds_{};
    std::array<uint64_t, 19> lastEquipEnchantIds_{};
    int invLastBags_ = -1, invLastItems_ = -1;
    uint64_t invLastMoney_ = ~0ull;
    int invSlotBase_ = -1, packSlotBase_ = -1;
    int effectiveBankSlots_ = 28, effectiveBankBagSlots_ = 7;
    std::vector<PendingItemPush> pendingItemPushNotifs_;
    std::deque<InventoryEvent> inventoryEvents_;
    PendingEquip pendingEquip_;
    std::string inventoryError_;
    uint32_t inventoryErrorMs_ = 0;
    uint64_t moneyCopper_ = 0;
    bool inventoryDirty_ = false;
    uint32_t inventoryVersion_ = 0, equipmentVersion_ = 0;
    uint32_t invLogs_ = 0, invSwapLogs_ = 0, invPushLogs_ = 0, invErrorLogs_ = 0;
    // Inventory (end)
    // Lighting - the realm clock (psp_world_session_time.cpp):
    // SMSG_LOGIN_SETTIMESPEED, SMSG_GAMETIME_SET/UPDATE, SMSG_GAMESPEED_SET,
    // unpacked as upstream GameHandler does (unpackWowPackedTime).
public:
    // Upstream GameHandler::getGameTime: hours since midnight as the server
    // last said (-1 = nothing yet); getTimeSpeed: its time scale.
    float getGameTime() const { return gameTime_; }
    float getTimeSpeed() const { return timeSpeed_; }
    // logUptimeMs() when getGameTime() arrived (the PSP advances it).
    uint32_t gameTimeReceivedMs() const { return gameTimeMs_; }

private:
    bool handleTimePacket(game::LogicalOpcode op, network::Packet& packet);
    float gameTime_ = -1.0f;
    float timeSpeed_ = 0.0166f;
    uint32_t gameTimeMs_ = 0;
    uint32_t timeLogs_ = 0;

    // batch3 merge F+C+D+E: UI sounds raised by session logic (the session
    // cannot call UI code): queued here, played by WorldScreen each frame
    // (ui_world_sounds.cpp drainSessionSounds -> playUiSound).
public:
    enum class SessionSound : uint8_t {
        LootCoinSmall, LootCoinLarge, LootItem, Error, QuestActivate, QuestComplete, QuestFailed, QuestUpdate
    };
    void queueSound(SessionSound s) {
        if (soundQueue_.size() < 16) soundQueue_.push_back(s);
    }
    bool takeSound(SessionSound& out) {
        if (soundQueue_.empty()) return false;
        out = soundQueue_.front();
        soundQueue_.erase(soundQueue_.begin());
        return true;
    }

private:
    std::vector<SessionSound> soundQueue_;

    // Trainers, talents, spell book tabs - upstream InventoryHandler's
    // trainer half, SpellHandler's talent half and getSpellBookTabs without
    // GameHandler (psp_world_session_trainer.cpp, psp_world_session_talents.cpp).
    // Names and semantics are GameHandler's, so the Lua-API adapter is thin.
public:
    struct TrainerTab {  // upstream GameHandler::TrainerTab
        std::string name;
        std::vector<const game::TrainerSpell*> spells;
    };
    bool isTrainerWindowOpen() const { return trainerWindowOpen_; }
    const game::TrainerListData& getTrainerSpells() const { return currentTrainerList_; }
    const std::vector<TrainerTab>& getTrainerTabs() const { return trainerTabs_; }
    void trainSpell(uint32_t spellId);
    void closeTrainer();
    bool hasKnownSpell(uint32_t spellId) const { return knownSpells_.count(spellId) != 0; }
    const std::string& getSkillLineName(uint32_t skillLineId) const;
    uint32_t trainerVersion() const { return trainerVersion_; }  // bumped per list / purchase (TRAINER_UPDATE)
    // Talents (upstream SpellHandler): ranks counted from 1, two specs.
    uint8_t getActiveTalentSpec() const { return activeTalentSpec_; }
    uint8_t getUnspentTalentPoints() const { return unspentTalentPoints_[activeTalentSpec_]; }
    uint8_t getUnspentTalentPoints(uint8_t spec) const { return spec < 2 ? unspentTalentPoints_[spec] : 0; }
    const std::unordered_map<uint32_t, uint8_t>& getLearnedTalents() const { return learnedTalents_[activeTalentSpec_]; }
    const std::unordered_map<uint32_t, uint8_t>& getLearnedTalents(uint8_t spec) const {
        return learnedTalents_[spec < 2 ? spec : 0];
    }
    uint8_t getTalentRank(uint32_t talentId) const;
    void learnTalent(uint32_t talentId, uint32_t requestedRank);
    const game::TalentEntry* getTalentEntry(uint32_t talentId) const;
    const game::TalentTabEntry* getTalentTabEntry(uint32_t tabId) const;
    const std::unordered_map<uint32_t, game::TalentEntry>& getAllTalents() const { return talentCache_; }
    const std::unordered_map<uint32_t, game::TalentTabEntry>& getAllTalentTabs() const { return talentTabCache_; }
    uint32_t talentTabIconId(uint32_t tabId) const;  // PSP: TalentTab.SpellIconID for the tab buttons
    // upstream loadTalentDbc; PSP: spells.pak "talents", the player's class
    // only. False without data (an old spells.pak): talentDataMissing().
    bool loadTalentDbc();
    bool talentDataMissing() const { return talentDbcLoaded_ && talentTabCache_.empty(); }
    bool talentsInitialized() const { return talentsInitialized_; }
    uint8_t getPlayerClass() const;
    uint32_t talentVersion() const { return talentVersion_; }  // PLAYER_TALENT_UPDATE
    bool showTalentWipeConfirmDialog() const { return talentWipePending_; }
    uint32_t getTalentWipeCost() const { return talentWipeCost_; }
    uint64_t getTalentWipeNpcGuid() const { return talentWipePending_ ? talentWipeNpcGuid_ : 0; }
    void confirmTalentWipe();
    void cancelTalentWipe() { talentWipePending_ = false; }
    // upstream SpellHandler::getSpellBookTabs. PSP: iconId = SpellIcon.dbc id
    // (SpellDb::iconById) instead of the icon path.
    struct SpellBookTab {
        std::string name;
        uint32_t iconId = 0;
        std::vector<uint32_t> spellIds;
    };
    const std::vector<SpellBookTab>& getSpellBookTabs();
    // Per frame: upstream closeIfTooFar for the trainer, the talent-learn watch.
    void trainerUpdate(float dtSec, const glm::vec3& selfPos);

private:
    bool handleTrainerPacket(game::LogicalOpcode op, network::Packet& packet);
    bool handleTalentPacket(game::LogicalOpcode op, network::Packet& packet);
    void handleTrainerList(network::Packet& packet);
    void categorizeTrainerSpells();
    void handleTalentsInfo(network::Packet& packet);
    void talentLearnedSpell(uint32_t spellId);
    bool isTalentRankSpell(uint32_t spellId);
    bool trainerWindowOpen_ = false;
    game::TrainerListData currentTrainerList_;
    std::vector<TrainerTab> trainerTabs_;
    uint32_t trainerVersion_ = 0;
    uint32_t pendingTrainerBuy_ = 0;
    std::unordered_map<uint32_t, game::TalentEntry> talentCache_;
    std::unordered_map<uint32_t, game::TalentTabEntry> talentTabCache_;
    std::unordered_map<uint32_t, uint32_t> talentTabIcons_;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint8_t>> talentRankSpells_;  // rank spell -> talent, rank (1..5)
    std::unordered_map<uint32_t, uint8_t> learnedTalents_[2];
    uint8_t unspentTalentPoints_[2] = {0, 0};
    uint8_t activeTalentSpec_ = 0;
    bool talentsInitialized_ = false, talentDbcLoaded_ = false;
    bool talentWipePending_ = false;
    uint64_t talentWipeNpcGuid_ = 0;
    uint32_t talentWipeCost_ = 0;
    uint32_t talentVersion_ = 0;
    uint32_t pendingTalentId_ = 0, pendingTalentRank_ = 0;
    float pendingTalentTimer_ = 0.0f;
    std::vector<SpellBookTab> spellBookTabs_;
    uint32_t spellBookTabsVersion_ = ~0u;
    // Trainers, talents, spell book tabs (end)
    // Tutorials (psp_world_session_tutorial.cpp) - the account's
    // tutorial flags (SMSG_TUTORIAL_FLAGS, CMSG_TUTORIAL_FLAG / CLEAR /
    // RESET) and the engine's TUTORIAL_TRIGGER events, fired at the game
    // points the original client fires them. Names follow the client's Lua API
    // (IsTutorialFlagged, FlagTutorial, ...) so a FrameXML adapter stays thin.
public:
    static constexpr uint32_t kTutorialFlagWords = 8;  // 256 ids
    bool tutorialFlagsReceived() const { return tutorialFlagsReceived_; }
    const std::array<uint32_t, kTutorialFlagWords>& getTutorialFlags() const { return tutorialFlags_; }
    bool isTutorialFlagged(uint32_t id) const;        // IsTutorialFlagged
    void flagTutorial(uint32_t id);                   // FlagTutorial -> CMSG_TUTORIAL_FLAG
    void clearTutorials();                            // ClearTutorials -> CMSG_TUTORIAL_CLEAR (all seen)
    void resetTutorials();                            // ResetTutorials -> CMSG_TUTORIAL_RESET (all unseen)
    bool canResetTutorials() const;                   // CanResetTutorials
    // GetNextCompleatedTutorial / GetPrevCompleatedTutorial: flagged ids after /
    // before `id` among `known` (the ids that have a text), 0 = none.
    uint32_t getNextCompletedTutorial(uint32_t id, const std::vector<uint32_t>& known) const;
    uint32_t getPrevCompletedTutorial(uint32_t id, const std::vector<uint32_t>& known) const;
    // The engine's TUTORIAL_TRIGGER (Lua TriggerTutorial): dropped while
    // showTutorials is off or the id is flagged; kept until the flags arrive.
    void triggerTutorial(uint32_t id);
    bool takeTutorialTrigger(uint32_t& id);           // TUTORIAL_TRIGGER events, oldest first
    void setShowTutorials(bool on) { showTutorials_ = on; }  // CVar showTutorials (wowee.cfg tutorials)
    bool showTutorials() const { return showTutorials_; }

private:
    bool handleTutorialPacket(game::LogicalOpcode op, network::Packet& packet);
    void tutorialUpdate();                            // polls the game points (psp_world_session_tutorial.cpp)
    void tutorialInventoryEvent(const char* name, int32_t arg, uint32_t entry);
    std::array<uint32_t, kTutorialFlagWords> tutorialFlags_{};
    bool tutorialFlagsReceived_ = false, showTutorials_ = true;
    std::deque<uint32_t> tutorialTriggers_, tutorialWaiting_;
    struct TutorialWatch {  // last seen state of the polled game points
        bool init = false;
        uint32_t level = 0, playerFlags = 0, knownSpells = 0, questLog = 0, questsComplete = 0, repCount = 0;
        int64_t repSum = 0;
        uint64_t target = 0;
        uint8_t life = 0;
        bool combat = false, lootable = false, vendor = false, whisper = false, damaged = false, broken = false;
        std::vector<uint32_t> items;  // pushed items waiting for their query (equip / bag / use)
        uint32_t nextMs = 0;
    } tutorialWatch_;
    uint32_t tutorialLogs_ = 0;
    // Map - our player's explored-area bits for the world map
    // (psp_world_session_map.cpp): upstream GameHandler::getPlayerExploredZoneMasks /
    // hasPlayerExploredZoneMasks, filled like SpellHandler::extractExploredZoneFields
    // from PLAYER_EXPLORED_ZONES_1.. of the own update fields.
public:
    static constexpr size_t PLAYER_EXPLORED_ZONES_COUNT = 128;
    const std::vector<uint32_t>& getPlayerExploredZoneMasks() const;
    bool hasPlayerExploredZoneMasks() const;

private:
    mutable std::vector<uint32_t> playerExploredZones_;
    mutable bool hasPlayerExploredZones_ = false;
    // Map (end)
    // Mounts (psp_world_session_mount.cpp) - upstream
    // EntityController::detectPlayerMountChange (own UNIT_FIELD_MOUNTDISPLAYID,
    // the 1.5 s dismount grace), MovementHandler::dismount
    // (CMSG_CANCEL_MOUNT_AURA), handleForceSpeedChange (SMSG_FORCE_*_CHANGE +
    // the ACK), the spline / MSG_MOVE_SET_*_SPEED handlers and the speeds of
    // the own create block (applyServerMovementSpeeds) without GameHandler.
public:
    // The display the own player rides (0 = on foot), as upstream's
    // currentMountDisplayId_: a stale non-zero field inside the dismount grace
    // is ignored.
    uint32_t ownMountDisplayId() const { return currentMountDisplayId_; }
    bool isMounted() const { return currentMountDisplayId_ != 0; }
    uint32_t mountAuraSpellId() const { return mountAuraSpellId_; }
    // upstream GameHandler::dismount: local state cleared at once, then
    // CMSG_CANCEL_MOUNT_AURA (the field follows from the server).
    void dismount();
    // Server movement speeds of the own player (upstream MovementHandler
    // serverRunSpeed_ ...; the original client's defaults until the server says otherwise).
    // class bars: while we move a vehicle / a possessed unit, its run speed (its block / its speed changes).
    float serverRunSpeed() const { return cbarMover_ != 0 && cbarMoverRun_ > 0.01f ? cbarMoverRun_ : serverRunSpeed_; }
    float serverRunBackSpeed() const { return serverRunBackSpeed_; }
    float serverWalkSpeed() const { return serverWalkSpeed_; }
    // Another unit's run speed (create block speeds, MSG_MOVE_SET_RUN_SPEED,
    // SMSG_SPLINE_SET_RUN_SPEED); 7 when unknown. PSP: predictRemote moves a
    // mounted player at it.
    float unitRunSpeed(uint64_t guid) const;
    // PSP: npc-walk - another unit's walk speed (create block, MSG_MOVE_SET_WALK_SPEED,
    // SMSG_SPLINE_SET_WALK_SPEED); 2.5 when unknown.
    float unitWalkSpeed(uint64_t guid) const;
    // Ages the dismount grace (upstream MovementHandler::update).
    void updateMount(float dtSec);

private:
    bool handleMountPacket(game::LogicalOpcode op, network::Packet& packet);
    void mountUpdateBlock(const game::UpdateBlock& b);
    void handleForceSpeedChange(network::Packet& packet, const char* name, game::Opcode ack, float* storage);
    void handleSplineSpeed(network::Packet& packet, game::LogicalOpcode op);
    void handleMoveSetSpeed(network::Packet& packet, game::LogicalOpcode op);
    uint32_t currentMountDisplayId_ = 0;
    uint32_t mountAuraSpellId_ = 0;
    float dismountGraceRemaining_ = 0.0f;
    float serverRunSpeed_ = 7.0f, serverRunBackSpeed_ = 4.5f, serverWalkSpeed_ = 2.5f;
    float serverSwimSpeed_ = 4.722f, serverSwimBackSpeed_ = 2.5f, serverFlightSpeed_ = 7.0f;
    float serverFlightBackSpeed_ = 4.5f, serverTurnRate_ = 3.14159f, serverPitchRate_ = 3.14159f;
    std::unordered_map<uint64_t, float> unitRunSpeed_;
    std::unordered_map<uint64_t, float> unitWalkSpeed_;  // PSP: npc-walk
    game::MovementInfo lastOwnMove_{};  // what sendMovement last sent: the ACK payload
    uint32_t mountLogs_ = 0;
    // Mounts (end)
    // minimap: the notices of the minimap cluster (psp_world_session_minimap.cpp) -
    // MiniMapMailFrame (upstream InventoryHandler hasNewMail / setHasNewMail over SMSG_RECEIVED_MAIL and
    // MSG_QUERY_NEXT_MAIL_TIME), GameTimeFrame (upstream calendarPendingInvites_ over
    // SMSG_CALENDAR_SEND_NUM_PENDING / _EVENT_INVITE_ALERT; CalendarGetDate = the realm's date).
public:
    bool hasNewMail() const { return hasNewMail_; }  // upstream GameHandler::hasNewMail (HasNewMail)
    // upstream InventoryHandler::setHasNewMail - for the mailbox too (opening it clears the flag, reading the
    // last unread letter clears it: the bank-mail windows call this).
    void setHasNewMail(bool value);
    // GetLatestThreeSenders: the player senders of the last MSG_QUERY_NEXT_MAIL_TIME answer (names as known).
    std::vector<std::string> latestMailSenders() const;
    void queryNextMailTime();  // MSG_QUERY_NEXT_MAIL_TIME (at most every 2 s)
    uint32_t getCalendarPendingInvites() const { return calendarPendingInvites_; }  // upstream GameHandler
    // Once per session, in world: MSG_QUERY_NEXT_MAIL_TIME + CMSG_CALENDAR_GET_NUM_PENDING (the minimap's
    // first frame).
    void requestMinimapNotices();
    // The realm's date (CalendarGetDate): its packed time and logUptimeMs() when it came; false before.
    bool realmDate(game::WowDate& date, uint32_t& receivedMs) const;

private:
    bool handleMinimapPacket(game::LogicalOpcode op, network::Packet& packet);
    bool hasNewMail_ = false, minimapNoticesAsked_ = false, realmDateKnown_ = false;
    uint32_t calendarPendingInvites_ = 0;
    std::vector<uint64_t> mailSenderGuids_;
    game::WowDate realmDate_{};
    uint32_t realmDateMs_ = 0, lastMailQueryMs_ = 0, minimapNoticeLogs_ = 0;
    // minimap (end)

    // tooltips (psp_world_session_tooltip.cpp): what the GameTooltip reads beyond the item
    // cache - the item template fields upstream's ItemQueryResponseParser skips (read from a copy of the same
    // answer, the parser unchanged), SMSG_SET_PROFICIENCY (GameHandler weaponProficiency_ / armorProficiency_:
    // the item subclass in red), SMSG_BINDPOINTUPDATE (the Hearthstone's "$z"), the item instances' enchantments.
public:
    struct ItemTipExtra {
        uint32_t lockId = 0, block = 0, maxDurability = 0, duration = 0, limitCategory = 0;
        int32_t randomProperty = 0, randomSuffix = 0;
        float armorDamageModifier = 0.0f;
        int32_t spellCharges[5] = {}, spellCooldown[5] = {}, spellCategoryCooldown[5] = {};
        int32_t spellCategory[5] = {};  // item use: the item spells' SpellCategory (the shared cooldowns)
        uint32_t totemCategory = 0;     // class bars: TotemCategory (GetTotemInfo's haveTotem)
    };
    const ItemTipExtra* itemTipExtra(uint32_t entry) const;
    uint32_t weaponProficiency() const { return weaponProficiency_; }  // 0 = not sent yet (upstream: all usable)
    uint32_t armorProficiency() const { return armorProficiency_; }
    uint32_t homeBindZone() const { return homeBindZone_; }  // AreaTable id, 0 = unknown
    // An item instance's enchantments (ITEM_FIELD_ENCHANTMENT_*): false = not one of ours.
    bool itemEnchantments(uint64_t guid, uint32_t& permanent, uint32_t& temporary,
                          std::array<uint32_t, 3>& sockets) const;

private:
    bool handleTooltipPacket(game::LogicalOpcode op, network::Packet& packet);
    std::unordered_map<uint32_t, ItemTipExtra> itemTipExtra_;
    uint32_t weaponProficiency_ = 0, armorProficiency_ = 0, homeBindZone_ = 0, homeBindMap_ = 0;
    uint32_t tipLogs_ = 0;
    // tooltips (end)
    // party (psp_world_session_party.cpp): upstream SocialHandler's group half without
    // GameHandler - SMSG_GROUP_INVITE / DECLINE / LIST / DESTROYED / CANCEL / UNINVITE / SET_LEADER,
    // SMSG_PARTY_COMMAND_RESULT, SMSG_PARTY_MEMBER_STATS(_FULL) and the senders (inviteToGroup,
    // acceptGroupInvite, declineGroupInvite, leaveGroup, uninvitePlayer, promoteToLeader, setLootMethod).
    // PSP: the lines are the original client's (ruRU GlobalStrings ERR_*, at the points the 3.3.5a client
    // prints them), not upstream's English ones.
public:
    const game::GroupListData& getPartyData() const { return partyData_; }
    bool isInGroup() const { return !partyData_.isEmpty(); }
    bool isGroupLeader() const { return partyData_.leaderGuid != 0 && partyData_.leaderGuid == playerGuid_; }
    const game::GroupMember* partyMember(uint64_t guid) const;
    // FrameXML "party1".."party4": the roster without the player, in roster order (upstream's unit ids).
    const game::GroupMember* partyUnit(int index) const;
    int partyUnitCount() const;
    // Bumped by every roster / leader / loot / member stats change (GROUP_ROSTER_UPDATE, UNIT_HEALTH...).
    uint32_t partyVersion() const { return partyVersion_; }
    // PARTY_INVITE_REQUEST: the invite on offer (upstream pendingGroupInvite / pendingInviterName); its
    // number goes up with each offer. PARTY_INVITE_CANCEL (SMSG_GROUP_CANCEL): taken once.
    bool hasPendingGroupInvite() const { return pendingGroupInvite_; }
    const std::string& pendingInviterName() const { return pendingInviterName_; }
    uint32_t groupInviteSeq() const { return groupInviteSeq_; }
    bool takeGroupInviteCancel() {
        const bool c = groupInviteCancel_;
        groupInviteCancel_ = false;
        return c;
    }
    // igPlayerInviteAccept (a member joined): bumped, the world screen plays it.
    uint32_t partyJoinSoundSeq() const { return partyJoinSoundSeq_; }
    void inviteToGroup(const std::string& playerName);  // CMSG_GROUP_INVITE (InviteUnit)
    void acceptGroupInvite();                           // CMSG_GROUP_ACCEPT (AcceptGroup)
    void declineGroupInvite();                          // CMSG_GROUP_DECLINE (DeclineGroup)
    void leaveGroup();                                  // CMSG_GROUP_DISBAND (LeaveParty)
    void uninvitePlayer(const std::string& playerName);  // CMSG_GROUP_UNINVITE (UninviteUnit)
    void promoteToLeader(uint64_t guid);                // CMSG_GROUP_SET_LEADER (PromoteToLeader)
    void setLootMethod(uint8_t method, uint64_t masterGuid, uint8_t threshold);  // CMSG_LOOT_METHOD
    // A member's race / gender / class from its name query (the portrait while out of range); false = unknown.
    bool partyMemberLook(uint64_t guid, uint8_t& race, uint8_t& gender, uint8_t& cls) const;
    // GROUP_UPDATE_FLAG_STATUS bits (AzerothCore GroupMemberOnlineStatus).
    static constexpr uint16_t kMemberOnline = 0x0001, kMemberDead = 0x0004, kMemberGhost = 0x0008;
    // Group loot rolls (upstream InventoryHandler's roll half, LootRollEntry): SMSG_LOOT_START_ROLL opens one
    // (START_LOOT_ROLL -> GroupLootFrame), the answer is CMSG_LOOT_ROLL (RollOnLoot: 0 pass, 1 need,
    // 2 greed, 3 disenchant), SMSG_LOOT_ROLL_WON / ALL_PASSED close it (CANCEL_LOOT_ROLL). PSP: the chat
    // lines are the original's LOOT_ROLL_*, upstream's English ones are not written.
    struct LootRoll {
        int rollId = 0;
        uint64_t objectGuid = 0;
        uint32_t slot = 0, itemId = 0, count = 1, countdownMs = 60000, startedMs = 0;
        uint8_t voteMask = 0xFF;  // RollMask: 0x02 need, 0x04 greed, 0x08 disenchant
    };
    const std::vector<LootRoll>& lootRolls() const { return lootRolls_; }
    void rollOnLoot(int rollId, uint8_t rollType);
    // PSP: a roll whose answer never came (left the group, a lost packet) goes 5 s after its countdown.
    void dropExpiredLootRolls();

private:
    bool handlePartyPacket(game::LogicalOpcode op, network::Packet& packet);
    void handleGroupInvite(network::Packet& packet);
    void handleGroupList(network::Packet& packet);
    void handlePartyCommandResult(network::Packet& packet);
    void handlePartyMemberStats(network::Packet& packet, bool isFull);
    void handleGroupSetLeader(network::Packet& packet);
    void clearPartyData();
    void partyLootLines(bool announce);  // the loot method / master / threshold lines (false: taken silently)
    void partyRefusal(const std::string& text);  // a refused party action: the red error line (GameLine)
    std::string partyMemberName(uint64_t guid) const;
    game::GroupListData partyData_;
    bool pendingGroupInvite_ = false, groupInviteCancel_ = false;
    std::string pendingInviterName_;
    uint32_t groupInviteSeq_ = 0, partyVersion_ = 0, partyJoinSoundSeq_ = 0;
    uint8_t shownLootMethod_ = 0xFF, shownLootThreshold_ = 0xFF;  // what the loot lines last said
    uint64_t shownLooterGuid_ = ~0ull;
    struct PartyLook {
        uint8_t race = 0, gender = 0, cls = 0;
    };
    std::unordered_map<uint64_t, PartyLook> partyLooks_;  // name query answers of group members
    std::unordered_set<uint64_t> partyNameQueried_;
    uint32_t partyLogs_ = 0, partyStatLogs_ = 0;
    void handleLootStartRoll(network::Packet& packet);
    void handleLootRollVote(network::Packet& packet);
    void handleLootRollWon(network::Packet& packet);
    void handleLootAllPassed(network::Packet& packet);
    void endLootRoll(uint64_t objectGuid, uint32_t slot);
    void lootRollLine(const std::string& text);  // a system chat line (upstream)
    std::string lootRollItemLink(uint32_t itemId);
    std::vector<LootRoll> lootRolls_;
    int nextLootRollId_ = 1;
    // party (end)
    // taxi: flight paths (psp_world_session_taxi.cpp) - upstream MovementHandler's taxi half
    // (handleShowTaxiNodes, buildTaxiCostMap, getTaxiRouteTo, activateTaxi, handleActivateTaxiReply,
    // startClientTaxiPath / beginTaxiFlightMotion / updateClientTaxi / finishClientTaxiFlight, closeTaxi) and the
    // GameHandler / EntityController pieces around it (SMSG_TAXINODE_STATUS, SMSG_NEW_TAXI_PATH, SMSG_DISMOUNT and
    // the mount field during a flight, UNIT_FLAG_TAXI_FLIGHT at login, the taxi heartbeat) without GameHandler. The
    // DBC tables come from data/taxi.pak (tools/psp/build_taxi_pak.py), for the map the player is on, only while
    // the window is open or a flight is on. Positions: server coordinates (the world screen's playerX_/Y_/Z_).
public:
    struct TaxiNode {  // upstream MovementHandler::TaxiNode, one continent
        uint32_t id = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f;  // server coordinates (TaxiNodes.dbc)
        uint32_t nameOff = 0;                // into the names of the loaded map (taxiNodeName)
        uint32_t mountCreature[2] = {0, 0};  // TaxiNodes MountCreatureID [Horde, Alliance] (creature entries)
    };
    struct TaxiEdge {  // upstream TaxiPathEdge + where its TaxiPathNode points are in taxi.pak
        uint16_t from = 0, to = 0;
        uint32_t pathId = 0, cost = 0, wpFirst = 0, wpCount = 0;
    };
    bool isTaxiWindowOpen() const { return taxiWindowOpen_; }
    void closeTaxi();                          // upstream closeTaxi (CloseTaxiMap)
    void activateTaxi(uint32_t destNodeId);    // upstream activateTaxi (TakeTaxiNode)
    bool isOnTaxiFlight() const { return onTaxiFlight_; }
    bool isTaxiMountActive() const { return taxiMountActive_; }
    bool isTaxiActivationPending() const { return taxiActivatePending_; }
    const game::ShowTaxiNodesData& getTaxiData() const { return currentTaxiData_; }
    uint32_t getTaxiCurrentNode() const { return currentTaxiData_.nearestNode; }
    uint64_t getTaxiNpcGuid() const { return taxiNpcGuid_; }
    // The nodes of the loaded map, ascending id (upstream taxiNodeOrder); empty when nothing is loaded.
    const std::vector<TaxiNode>& getTaxiNodes() const { return taxiNodes_; }
    const TaxiNode* taxiNode(uint32_t id) const;
    const char* taxiNodeName(const TaxiNode& n) const;  // TaxiNodes Name_lang (ruRU), "" when none
    bool isKnownTaxiNode(uint32_t nodeId) const;        // upstream: the last SMSG_SHOWTAXINODES mask
    uint32_t getTaxiCostTo(uint32_t destNodeId) const;
    bool hasTaxiRouteTo(uint32_t destNodeId) const;
    std::vector<uint32_t> getTaxiRouteTo(uint32_t destNodeId) const;
    bool taxiNpcHasRoutes(uint64_t guid) const;          // SMSG_TAXINODE_STATUS
    // WorldMapContinent TaxiMin / TaxiMax of the loaded map: the square of the world the TaxiMap picture shows.
    bool taxiMapBounds(float& minX, float& minY, float& maxX, float& maxY) const;
    uint32_t taxiMapId() const { return taxiMapId_; }
    // PSP: a flight master without UNIT_NPC_FLAG_GOSSIP is asked as the original client asks it (by NPC flags):
    // CMSG_TAXIQUERYAVAILABLENODES (the server answers SMSG_SHOWTAXINODES, or teaches the node).
    void taxiQueryAvailableNodes(uint64_t npcGuid);
    // Per frame from the world screen: the flight (upstream updateClientTaxi + GameHandler's taxi bookkeeping) and
    // the window's reach (closeIfTooFar). selfPos = the player's feet now.
    void updateTaxi(float dtSec, const glm::vec3& selfPos);
    // The flight moves the player this frame: where and which way (server coordinates / yaw). False: own movement.
    bool taxiDrivesPlayer(glm::vec3& pos, float& orientation) const;
    // A flight ended (finishClientTaxiFlight): where the player was put - the screen's landing clamp. Taken once.
    bool takeTaxiLanding(glm::vec3& pos);
    uint32_t taxiDataBytes() const;       // the loaded tables (the log)

private:
    bool handleTaxiPacket(game::LogicalOpcode op, network::Packet& packet);
    void taxiUpdateBlock(const game::UpdateBlock& b);               // own create: UNIT_FLAG_TAXI_FLIGHT at login
    bool taxiOwnMountField(uint32_t newDisplayId, const game::UpdateBlock& b);  // the mount field mid-flight
    void taxiOwnMonsterMove(const game::MonsterMoveData& d);         // PSP: our own server spline
    void handleShowTaxiNodes(network::Packet& packet);
    void handleActivateTaxiReply(network::Packet& packet);
    bool loadTaxiMap(uint32_t mapId);
    void releaseTaxiData();
    void buildTaxiCostMap();
    bool loadTaxiWaypoints(const std::vector<uint32_t>& pathNodes);
    void startClientTaxiPath(const std::vector<uint32_t>& pathNodes);
    void beginTaxiFlightMotion();
    void finishClientTaxiFlight(bool snapToFinalWaypoint);
    void deferServerTaxiCompletion() { if (taxiClientActive_) taxiServerCompletionPending_ = true; }
    bool isNearTaxiDestination(float maxDistance = 24.0f) const;
    void updateClientTaxi(float dtSec);
    void forceClearTaxi(const char* why);
    void taxiSendMove(game::Opcode opcode);  // MSG_MOVE_* at the flight's position, no movement flags
    bool serverStillTaxiing(const game::UpdateBlock* b) const;
    bool taxiWindowOpen_ = false;
    game::ShowTaxiNodesData currentTaxiData_;
    uint64_t taxiNpcGuid_ = 0;
    uint32_t knownTaxiMask_[game::TLK_TAXI_MASK_SIZE] = {};
    bool taxiMaskInitialized_ = false;
    std::unordered_map<uint64_t, bool> taxiNpcHasRoutes_;
    std::unordered_map<uint32_t, uint32_t> taxiCostMap_, taxiPrevMap_;
    std::vector<TaxiNode> taxiNodes_;
    std::vector<TaxiEdge> taxiEdges_;
    std::vector<char> taxiNames_;
    uint32_t taxiMapId_ = ~0u;
    float taxiBounds_[4] = {0, 0, 0, 0};
    uint32_t taxiWpOffset_ = 0, taxiWpCount_ = 0;  // "taxiwp_<map>" in taxi.pak
    bool onTaxiFlight_ = false, taxiMountActive_ = false, taxiActivatePending_ = false, taxiClientActive_ = false;
    bool taxiServerCompletionPending_ = false;
    float taxiActivateTimer_ = 0.0f, taxiLandingCooldown_ = 0.0f, taxiStartGrace_ = 0.0f;
    uint32_t taxiDestNodeId_ = 0;
    std::string taxiDestName_;
    std::vector<glm::vec3> taxiClientPath_;          // server coordinates
    std::vector<float> taxiClientSegmentArcLengths_;
    size_t taxiClientIndex_ = 0;
    float taxiClientSpeed_ = 32.0f;                  // upstream (AzerothCore PLAYER_FLIGHT_SPEED)
    float taxiClientSegmentProgress_ = 0.0f;
    glm::vec3 taxiPos_{0.0f};                        // the flight's position (upstream movementInfo)
    float taxiOrientation_ = 0.0f;
    game::Entity taxiServerSpline_;                  // PSP: our own SMSG_MONSTER_MOVE (no client path)
    bool taxiServerSplineActive_ = false;
    bool taxiLanded_ = false;
    glm::vec3 taxiLandPos_{0.0f};
    uint32_t taxiFlightStartMs_ = 0, taxiFrameLogMs_ = 0, taxiLogs_ = 0;
    // taxi (end)
    // bank and mail (psp_world_session_mail.cpp): the bank and the mailbox - upstream InventoryHandler's
    // bank half (openBank / closeBank / buyBankSlot / depositItem / withdrawItem, SMSG_SHOW_BANK,
    // SMSG_BUY_BANK_SLOT_RESULT) and mail half (openMailbox / closeMailbox / refreshMailList / sendMail, the
    // attachments, mailTakeMoney / mailTakeItem / mailDelete / mailReturnToSender / mailMarkAsRead,
    // SMSG_SHOW_MAILBOX, SMSG_MAIL_LIST_RESULT, SMSG_SEND_MAIL_RESULT), GameHandler::interactWithGameObject
    // for a mailbox (CMSG_GAMEOBJ_USE + openMailbox) and getMailSenderName / getMailDisplaySubject, without
    // GameHandler. Names and semantics are upstream's (the Lua adapter stays thin). The minimap's new-mail
    // envelope (SMSG_RECEIVED_MAIL, MSG_QUERY_NEXT_MAIL_TIME) is the minimap task's, not here.
public:
    static constexpr int MAIL_MAX_ATTACHMENTS = 12;  // upstream InventoryHandler::MAIL_MAX_ATTACHMENTS
    struct MailAttachSlot {                         // upstream InventoryHandler::MailAttachSlot
        uint64_t itemGuid = 0;
        game::ItemDef item;
        uint8_t srcBag = 0xFF;
        uint8_t srcSlot = 0;
        bool occupied() const { return itemGuid != 0; }
    };
    struct MailEvent {  // the addon events upstream fires (MailFrame / BankFrame listen to them)
        const char* name = "";  // BANKFRAME_OPENED, BANKFRAME_CLOSED, PLAYERBANKBAGSLOTS_CHANGED, MAIL_SHOW,
                                // MAIL_CLOSED, MAIL_INBOX_UPDATE, MAIL_SEND_SUCCESS, MAIL_FAILED, MAIL_SUCCESS,
                                // CLOSE_INBOX_ITEM, MAIL_SEND_INFO_UPDATE
        int64_t arg = 0;        // MAIL_FAILED: the server's error; CLOSE_INBOX_ITEM: the mail id
    };
    // Bank.
    bool isBankOpen() const { return bankOpen_; }
    uint64_t bankerGuid() const { return bankerGuid_; }
    void activateBanker(uint64_t npcGuid);  // CMSG_BANKER_ACTIVATE (PSP: a banker without gossip, by NPC flags)
    void closeBank();
    void buyBankSlot();                    // CMSG_BUY_BANK_SLOT (PurchaseSlot)
    // GetNumBankSlots: PLAYER_BYTES_2 byte 2 (upstream EntityController -> Inventory::setPurchasedBankBagSlots).
    int purchasedBankBagSlots() const;
    void depositItem(uint8_t srcBag, uint8_t srcSlot);   // CMSG_AUTOBANK_ITEM (UseContainerItem at the bank)
    void withdrawItem(uint8_t srcBag, uint8_t srcSlot);  // CMSG_AUTOSTORE_BANK_ITEM
    // Mail.
    void useMailbox(uint64_t goGuid);  // interactWithGameObject for a mailbox: CMSG_GAMEOBJ_USE + openMailbox
    void openMailbox(uint64_t guid);
    void closeMailbox();
    bool isMailboxOpen() const { return mailboxOpen_; }
    uint64_t mailboxGuid() const { return mailboxGuid_; }
    // PSP: the mailboxes in sight (GAMEOBJECT_TYPE_MAILBOX objects of SMSG_UPDATE_OBJECT, server coords) -
    // the nearest within maxDist of selfPos (false: none); the PSP has no mouse to click one with.
    bool nearestMailbox(const glm::vec3& selfPos, float maxDist, uint64_t& guid, float& dist) const;
    // The mailboxes in sight for the HUD's label (the PSP draws no game object models yet).
    size_t mailboxCount() const { return mailboxes_.size(); }
    glm::vec3 mailboxPos(size_t i) const { return mailboxes_[i].pos; }
    void refreshMailList();  // CMSG_GET_MAIL_LIST (CheckInbox)
    const std::vector<game::MailMessage>& getMailInbox() const { return mailInbox_; }
    uint32_t mailTotalCount() const { return mailTotalCount_; }  // GetInboxNumItems' second value
    uint32_t mailListAgeSec() const;                              // since the list arrived (daysLeft runs down)
    std::string getMailSenderName(const game::MailMessage& mail);   // queries what it does not know yet
    std::string getMailDisplaySubject(const game::MailMessage& mail);
    void sendMail(const std::string& recipient, const std::string& subject, const std::string& body,
                  uint64_t money, uint64_t cod);
    bool attachItemFromBackpack(int backpackIndex);
    bool attachItemFromBag(int bagIndex, int slotIndex);
    bool detachMailAttachment(int attachIndex);
    void clearMailAttachments();
    const std::array<MailAttachSlot, MAIL_MAX_ATTACHMENTS>& getMailAttachments() const { return mailAttachments_; }
    int getMailAttachmentCount() const;
    bool isItemAttachedToMail(uint8_t bag, uint8_t slot) const;  // the bag slot shows locked (wire numbering)
    void mailTakeMoney(uint32_t mailId);
    void mailTakeItem(uint32_t mailId, uint32_t itemGuidLow);
    void mailReturnToSender(uint32_t mailId);
    void mailDelete(uint32_t mailId);
    void mailMarkAsRead(uint32_t mailId);
    void takeInboxTextItem(uint32_t mailId);  // CMSG_MAIL_CREATE_TEXT_ITEM (upstream SocialHandler)
    uint32_t mailVersion() const { return mailVersion_; }  // bumped by every inbox / draft change
    bool takeMailEvent(MailEvent& out);
    // Per frame: PSP - the bank / mailbox close past NPC_INTERACT_MAX_DISTANCE, as the client's frames.
    void bankMailUpdate(float dtSec, const glm::vec3& selfPos);

private:
    bool handleBankMailPacket(game::LogicalOpcode op, network::Packet& packet);
    void bankMailUpdateBlock(const game::UpdateBlock& b);  // mailbox game objects
    void bankMailForget(uint64_t guid);
    // PSP (owner: "no mailbox model - check for more of this"): every game object in sight
    // (psp_world_session_gameobject.cpp) - upstream EntitySpawner::queueGameObjectSpawn's data.
public:
    struct GameObjectInfo {
        uint64_t guid = 0;
        uint32_t entry = 0, displayId = 0, dynamic = 0;
        uint8_t type = 0, state = 0;  // GAMEOBJECT_BYTES_1: byte 1 the type, byte 0 the state
        glm::vec3 pos{0.0f};          // server coords
        float orientation = 0.0f, scale = 1.0f;
        // game object use: the rest of the fields the client reads for a game object (3.3.5a descriptor indices).
        uint32_t flags = 0;           // GAMEOBJECT_FLAGS: 0x1 in use, 0x2 locked, 0x4 interact cond, 0x10 not selectable
        uint32_t faction = 0;         // GAMEOBJECT_FACTION (a FactionTemplate id, 0 = none)
        uint32_t level = 0;           // GAMEOBJECT_LEVEL (a lock's skill when its row says 0: level x 5)
        uint64_t createdBy = 0;       // GAMEOBJECT_FIELD_CREATED_BY (a fishing bobber's owner, a summoned object's)
        uint8_t artKit = 0, animProgress = 0;  // GAMEOBJECT_BYTES_1 bytes 2 and 3
        uint8_t prevState = 0xFF;     // the state before the last change (0xFF: none seen since the create)
        uint32_t stateSeq = 0;        // bumped by every change of the state (the model's transition)
        uint32_t seenMs = 0;          // logUptimeMs of the create block
    };
    const std::vector<GameObjectInfo>& gameObjects() const { return gameObjects_; }
    uint32_t gameObjectsVersion() const { return goVersion_; }  // grows on every change

private:
    void gameObjectUpdateBlock(const game::UpdateBlock& b);
    void gameObjectForget(uint64_t guid);
    std::vector<GameObjectInfo> gameObjects_;
    uint32_t goVersion_ = 0, goLogs_ = 0;
    void openBank(uint64_t guid);
    void handleMailListResult(network::Packet& packet);
    void handleSendMailResult(network::Packet& packet);
    void refuseSend(const std::string& reason, const char* logLine);
    void noteMailAttach();
    void pushMailEvent(const char* name, int64_t arg = 0);
    std::string inventoryErrorString(uint8_t code) const;  // psp_world_session_inventory.cpp's table (EQUIP_ERROR)
    bool bankOpen_ = false, mailboxOpen_ = false;
    uint64_t bankerGuid_ = 0, mailboxGuid_ = 0;
    std::vector<game::MailMessage> mailInbox_;
    uint32_t mailTotalCount_ = 0, mailListMs_ = 0, mailVersion_ = 0;
    std::array<MailAttachSlot, MAIL_MAX_ATTACHMENTS> mailAttachments_{};
    struct MailboxObject {
        uint64_t guid = 0;
        glm::vec3 pos{0.0f};  // server coords
    };
    std::vector<MailboxObject> mailboxes_;
    std::deque<MailEvent> mailEvents_;
    uint32_t mailLogs_ = 0;
    // bank and mail (end)
    // auction (psp_world_session_auction.cpp): the auction house - the auctioneer and his house (MSG_AUCTION_HELLO),
    // the three lists ("list", "bidder", "owner") with their totals, sort tables and query throttles, the sell slot,
    // the multi-stack sale, the outbid list, the notifications - the packets of psp_auction_proto.hpp and upstream's
    // builders (AuctionHelloPacket, AuctionListItemsPacket, AuctionPlaceBidPacket, AuctionRemoveItemPacket,
    // AuctionListOwnerItemsPacket, AuctionListBidderItemsPacket). Events for AuctionFrame (the auction UI addon) in a
    // queue as the bank / mail ones.
public:
    using AuctionItem = auction::Item;
    struct AuctionEvent {
        const char* name = "";  // AUCTION_HOUSE_SHOW / _CLOSED / _DISABLED, NEW_AUCTION_UPDATE, AUCTION_ITEM_LIST_UPDATE,
                                // AUCTION_OWNED_LIST_UPDATE, AUCTION_BIDDER_LIST_UPDATE, AUCTION_MULTISELL_START /
                                // _UPDATE / _FAILURE
        int32_t a = 0, b = 0;   // MULTISELL_START: stacks; MULTISELL_UPDATE: done, stacks
    };
    // An auctioneer right-clicked by his NPC flags (MSG_AUCTION_HELLO); the gossip
    // option is the server's (selectGossipOption).
    void auctionHello(uint64_t npcGuid);
    bool isAuctionHouseOpen() const { return auctioneerGuid_ != 0; }
    uint64_t auctioneerGuid() const { return auctioneerGuid_; }
    uint32_t auctionHouseId() const { return auctionHouseId_; }
    void closeAuctionHouse();  // CloseAuctionHouse: AUCTION_HOUSE_CLOSED
    bool takeAuctionEvent(AuctionEvent& out);
    // CanSendAuctionQuery(type) - its two values: a query of that list now / a mass query (getAll, once in 15 min).
    bool canSendAuctionQuery(int list) const;
    bool canSendAuctionMassQuery() const;
    // QueryAuctionItems with the ids already looked up (0xFFFFFFFF = any): the page of 50, the list's
    // sort table. Nothing goes while the list's throttle runs.
    void queryAuctionItems(const std::string& name, uint8_t minLevel, uint8_t maxLevel, uint32_t invType,
                           uint32_t itemClass, uint32_t subClass, int page, bool usable, uint32_t quality, bool getAll);
    void getOwnerAuctionItems();          // GetOwnerAuctionItems (sent when the throttle allows)
    void getBidderAuctionItems(int page); // GetBidderAuctionItems: + the outbid auctions
    const std::vector<AuctionItem>& auctionItems(int list) const { return auctionLists_[list]; }
    uint32_t auctionTotal(int list) const { return auctionTotals_[list]; }  // GetNumAuctionItems' second value
    uint32_t auctionVersion() const { return auctionVersion_; }             // grows with every change of the lists
    // SortAuctionClearSort / SortAuctionSetSort / SortAuctionApplySort, GetAuctionSort's table.
    void sortAuctionClearSort(int list) { auctionSorts_[list].clear(); }
    void sortAuctionSetSort(int list, uint8_t column, bool reverse) { auctionSorts_[list].set(column, reverse); }
    void sortAuctionApplySort(int list);
    const auction::SortTable& auctionSort(int list) const { return auctionSorts_[list]; }
    // PlaceAuctionBid: the port's checks (red lines), then CMSG_AUCTION_PLACE_BID.
    void placeAuctionBid(int list, int index, uint32_t bid);
    bool canCancelAuction(int index) const;  // CanCancelAuction: an owner entry not sold yet
    void cancelAuction(int index);           // CancelAuction: the 5 % of the bid must be there
    // The sell slot: ClickAuctionSellItemButton's half in the session - the bag item (wire bag / slot), drawn
    // locked in the bag; NEW_AUCTION_UPDATE on every change.
    auction::Line canAuctionItem(uint8_t bag, uint8_t slot) const;  // kLineNone or the refusal's line
    void setAuctionSellItem(uint8_t bag, uint8_t slot);
    void clearAuctionSellItem();
    bool auctionSellItem(uint8_t& bag, uint8_t& slot) const;
    bool auctionSellItemDef(game::ItemDef& out) const;  // the bag item in the slot (false: none / gone)
    uint32_t auctionSellTotalCount() const;  // GetAuctionSellItemInfo's totalCount (that item in the bags)
    bool auctionItemLocked(uint8_t bag, uint8_t slot) const;  // the sell slot's item
    // StartAuction: one stack, or several (AUCTION_MULTISELL_START, a CMSG_AUCTION_SELL_ITEM per
    // stack as the results come). runTime 1..3 (12 / 24 / 48 h).
    void startAuction(uint32_t minBid, uint32_t buyout, int runTime, uint32_t stackSize, uint32_t numStacks);
    void cancelSell();                       // CancelSell: the progress frame's button
    bool auctionMultisellActive() const { return auctionMulti_.active; }
    // GetAuctionItemInfo's names: the owner / bidder (a name query once; nullptr until it is back).
    const std::string* auctionNameOf(uint64_t guid);
    // A line of the auction code: the red error line or a system chat line (GameLine).
    void auctionGameError(auction::Line line, const std::string& itemName = std::string());
    // Per frame: the auctioneer's distance, the queries waiting for their throttle.
    void auctionUpdate(const glm::vec3& selfPos);

private:
    bool handleAuctionPacket(game::LogicalOpcode op, network::Packet& packet);
    void pushAuctionEvent(const char* name, int32_t a = 0, int32_t b = 0);
    void auctionListResult(int list, network::Packet& packet);
    void auctionCommandResult(network::Packet& packet);
    void auctionBidderNotification(network::Packet& packet);
    void auctionOwnerNotification(network::Packet& packet);
    void auctionRemovedNotification(network::Packet& packet);
    void auctionRemove(uint32_t auctionId);
    void auctionSortList(int list);
    void auctionAnnounce(int list);                 // the list's AUCTION_*_LIST_UPDATE
    void auctionSendWanted();                       // the queries asked for, once their throttle allows
    void auctionMultisellNext();
    void auctionMultisellStop(bool failureEvent);
    void auctionItemLine(auction::Line line, uint32_t itemEntry);  // a news line with the item's name
    struct AuctionQuery {                               // the last browse query (asked again after a bid)
        std::string name;
        uint8_t minLevel = 0, maxLevel = 0;
        uint32_t invType = 0, itemClass = 0, subClass = 0, quality = 0;
        int page = 0;
        bool usable = false;
    };
    uint64_t auctioneerGuid_ = 0;
    uint32_t auctionHouseId_ = 0;
    std::vector<AuctionItem> auctionLists_[auction::kListCount];
    uint32_t auctionTotals_[auction::kListCount] = {};  // list, bidder, owner
    auction::SortTable auctionSorts_[auction::kListCount];
    uint32_t auctionNextMs_[auction::kListCount] = {};  // the list's next query not before (0: free)
    bool auctionWant_[auction::kListCount] = {};        // a query asked for, waiting for the throttle
    AuctionQuery auctionLastQuery_;
    bool auctionHasQuery_ = false;
    bool auctionOut_[auction::kListCount] = {};         // a query sent and not answered yet
    std::vector<uint32_t> auctionOutbid_;               // the auctions the outbid notices named
    uint64_t auctionSellGuid_ = 0;
    uint8_t auctionSellBag_ = 0, auctionSellSlot_ = 0;
    struct AuctionMultisell {
        bool active = false;
        uint64_t auctioneer = 0;
        uint32_t minBid = 0, buyout = 0, minutes = 0, stackSize = 0, stacks = 0, left = 0;
        std::vector<auction::SellPiece> sources;
    };
    AuctionMultisell auctionMulti_;
    std::deque<AuctionEvent> auctionEvents_;
    uint32_t auctionVersion_ = 0, auctionLogs_ = 0;
    // auction (end)
    // trade skills (psp_world_session_tradeskill.cpp): the trade skill window's data and the craft queue,
    // as upstream - SpellHandler::tradeskillOpenerSkillLine / openCraftingWindow / closeCraftingWindow /
    // startCraftQueue / cancelCraftQueue (+ the re-cast in handleSpellGo, the drop in clearOwnCast) and
    // GameHandler::getCraftingRecipes / getRecipeDifficulty / countItemInBags. PSP: the recipes of the open
    // line come from data/craft.pak (CraftDb), loaded with the window and freed with it.
public:
    struct CraftRecipe {  // upstream GameHandler::CraftRecipe
        uint32_t spellId = 0;
        std::string name;
        int difficulty = 0;  // 0 optimal, 1 medium, 2 easy, 3 trivial
        int canMake = 0;     // how many the bags have reagents for
        const CraftRecipeRec* rec = nullptr;  // PSP: its craft.pak record (valid while the line is loaded)
    };
    // A profession spell's skill line when casting it opens the window (0 = cast it normally).
    uint32_t tradeSkillOpenerLine(uint32_t spellId);
    void openCraftingWindow(uint32_t skillLine);  // TRADE_SKILL_SHOW
    void closeCraftingWindow();                   // CloseTradeSkill (TRADE_SKILL_CLOSE)
    bool isCraftingWindowOpen() const { return craftingWindowOpen_; }
    uint32_t getCraftingSkillLine() const { return craftingSkillLine_; }
    uint32_t craftingShows() const { return craftShows_; }  // bumped by every TRADE_SKILL_SHOW
    const CraftDb& craftDb() const { return craftDb_; }
    std::vector<CraftRecipe> getCraftingRecipes();  // the known recipes of the open line, by name
    static int getRecipeDifficulty(const CraftRecipeRec& r, uint32_t skill);
    uint32_t countItemInBags(uint32_t itemId) const;
    bool craftSkill(uint32_t& rank, uint32_t& maxRank);  // GetTradeSkillLine's rank / maxRank
    void startCraftQueue(uint32_t spellId, int count, bool moving);  // DoTradeSkill
    void cancelCraftQueue();                                         // StopTradeSkillRepeat
    int getCraftQueueRemaining() const { return craftQueueRemaining_; }
    uint32_t getCraftQueueSpellId() const { return craftQueueSpellId_; }
    uint32_t craftQueueVersion() const { return craftQueueVersion_; }  // UPDATE_TRADESKILL_RECAST

private:
    bool craftQueueSpellGo(uint32_t spellId);  // handleSpellGo: the next craft of the queue
    CraftDb craftDb_;
    bool craftingWindowOpen_ = false;
    uint32_t craftingSkillLine_ = 0, craftShows_ = 0;
    uint32_t craftQueueSpellId_ = 0, craftQueueVersion_ = 0;
    int craftQueueRemaining_ = 0;
    // trade skills (end)
    // map transfer (psp_world_session_transfer.cpp): transfers between
    // maps, area triggers and the instance messages - GameHandler's SMSG_TRANSFER_PENDING / SMSG_TRANSFER_ABORTED,
    // MovementHandler::handleNewWorld (the clears, MSG_MOVE_WORLDPORT_ACK at once + STOP / HEARTBEAT + a ping),
    // MovementHandler::loadAreaTriggerDbc / checkAreaTriggers (CMSG_AREATRIGGER: instance portals, the tram, rest
    // areas, explore points) and SocialHandler's instance packets, without GameHandler. The LoginScreen owns the
    // world's life: on Ready it destroys the WorldScreen, commits the new entry and builds the next one.
public:
    enum class TransferStage : uint8_t {
        None,     // in the world
        Pending,  // SMSG_TRANSFER_PENDING: the loading screen of transferMapId() over the old world
        Ready,    // SMSG_NEW_WORLD to another map, acknowledged: rebuild the world, then commitTransfer()
    };
    TransferStage transferStage() const { return transferStage_; }
    uint32_t transferMapId() const { return transferMapId_; }
    // SMSG_TRANSFER_PENDING carried a transport (a ship / zeppelin): SMSG_NEW_WORLD's place is then transport-local.
    bool transferOnTransport() const { return transferTransportEntry_ != 0; }
    uint32_t transferPendingMs() const { return transferPendingMs_; }  // logUptimeMs of the pending packet
    // PSP: a transfer on a transport waits for our own create block on the new map (its world place; the PSP has no
    // transports to compose SMSG_NEW_WORLD's transport-local place with, as upstream does).
    bool transferAwaitingSelf() const { return transferAwaitSelf_; }
    // The old world is gone: the new map becomes worldEntry() (area triggers armed as after a login).
    void commitTransfer();
    // A pending transfer the server never finished (no SMSG_NEW_WORLD / SMSG_TRANSFER_ABORTED): forget it.
    void dropPendingTransfer(const char* why);
    uint32_t transfersDone() const { return transfersDone_; }
    // A map's data: Map.dbc Directory (lower case - the paks' <dir>), MapName_lang ruRU, InstanceType, the WDT's
    // global WMO (glue.pak "mapdirs"; without it the four continents by a built-in table). False: unknown map.
    struct MapInfo {
        uint32_t id = ~0u;
        uint8_t instanceType = 0, flags = 0, expansion = 0, maxPlayers = 0;
        std::string dir, name;
        bool found = false;
    };
    static bool mapInfo(uint32_t mapId, MapInfo& out);
    // Per frame from the world screen, after the movement (upstream GameHandler::update: every 0.25 s).
    // selfPos = the player's feet, server coordinates.
    void updateAreaTriggers(float dtSec, const glm::vec3& selfPos);
    // StaticPopup INSTANCE_BOOT (SMSG_RAID_GROUP_ONLY) and INSTANCE_LOCK (SMSG_INSTANCE_LOCK_WARNING_QUERY), as the
    // original's UIParent shows them: seconds left (<= 0: none).
    float instanceBootSecondsLeft() const;
    float instanceLockSecondsLeft() const;
    bool instanceLockPreviouslySaved() const { return instanceLockSaved_; }
    uint32_t instanceLockCompletedMask() const { return instanceLockMask_; }  // encounters already done
    void respondInstanceLock(bool accept);  // RespondInstanceLock -> CMSG_INSTANCE_LOCK_RESPONSE
    uint32_t instanceDifficulty() const { return instanceDifficulty_; }

private:
    bool handleTransferPacket(game::LogicalOpcode op, network::Packet& packet);
    void handleTransferPending(network::Packet& packet);
    void handleNewWorld(network::Packet& packet);
    void handleTransferAborted(network::Packet& packet);
    void transferResetWorld();  // upstream handleNewWorld's clears (entities, combat, casts, windows of the old map)
    void transferSendArrival(const glm::vec3& pos, float orientation);  // MSG_MOVE_STOP + HEARTBEAT at the new place
    void transferSelfBlock(const game::UpdateBlock& b);  // applyUpdateBlock: our create block while awaited
    bool transferAwaitSelf_ = false;
    void armAreaTriggers(uint32_t mapId);  // upstream: suppress first, 10 s cooldown (1.5 s on the Deeprun Tram)
    bool loadAreaTriggers(uint32_t mapId);
    std::string mapLabel(uint32_t mapId) const;  // MapName_lang ruRU, else "#<id>"
    TransferStage transferStage_ = TransferStage::None;
    uint32_t transferMapId_ = 0, transferTransportEntry_ = 0, transferTransportMap_ = 0;
    uint32_t transferPendingMs_ = 0, transfersDone_ = 0, transferLogs_ = 0;
    game::LoginVerifyWorldData transferEntry_{};
    // Area triggers (upstream GameHandler areaTriggers_ / activeAreaTriggers_ / areaTriggerCheckTimer_ /
    // areaTriggerCooldown_ / areaTriggerSuppressFirst_, MovementHandler lastAreaTriggerId_ and the post-transfer
    // return portal of the Deeprun Tram).
    struct AreaTrigger {
        uint32_t id = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f;  // server coordinates (DBC fields 2, 3, 4)
        float radius = 0.0f, boxLength = 0.0f, boxWidth = 0.0f, boxHeight = 0.0f, boxYaw = 0.0f;
    };
    std::vector<AreaTrigger> areaTriggers_;
    uint32_t areaTriggerMap_ = ~0u;  // the map areaTriggers_ were read for
    std::unordered_set<uint32_t> activeAreaTriggers_, nearMissLogged_;
    float areaTriggerCheckTimer_ = -5.0f, areaTriggerCooldown_ = 10.0f;
    bool areaTriggerSuppressFirst_ = true, areaTriggerArmed_ = false;
    uint32_t lastAreaTriggerId_ = 0, postTransferReturnTrigger_ = 0;
    bool postTransferReturnSawNear_ = false;
    bool pendingTriggerDestValid_ = false;
    uint32_t pendingTriggerDestMap_ = 0;
    glm::vec3 pendingTriggerDestPos_{0.0f};
    float pendingTriggerDestO_ = 0.0f;
    glm::vec3 lastTriggerCheckPos_{0.0f};
    uint32_t lastTriggerCheckMap_ = ~0u;
    bool lastTriggerCheckValid_ = false;
    uint32_t areaTriggerLogs_ = 0;
    // Instance popups.
    uint32_t instanceBootDeadlineMs_ = 0, instanceLockDeadlineMs_ = 0, instanceLockMask_ = 0;
    bool instanceLockSaved_ = false;
    uint32_t instanceDifficulty_ = 0;
    // map transfer (end)
    // particles: SMSG_WEATHER as upstream reads it (game_handler_packets.cpp: uint32 type,
    // float intensity, uint8 isAbrupt on 2.4.3 / 3.3.5a) - names as GameHandler's getWeatherType / Intensity.
    // What a 3.3.5a realm sends in the first field is its WeatherState (ui_world_weather.cpp decodes it).
public:
    uint32_t getWeatherType() const { return weatherType_; }
    float getWeatherIntensity() const { return weatherIntensity_; }
    bool weatherAbrupt() const { return weatherAbrupt_; }
    uint32_t weatherVersion() const { return weatherVersion_; }  // grows with every SMSG_WEATHER

private:
    uint32_t weatherType_ = 0, weatherVersion_ = 0, weatherLogs_ = 0;
    float weatherIntensity_ = 0.0f;
    bool weatherAbrupt_ = false;
    // particles (end)
    // cinematics (psp_world_session_movie.cpp): SMSG_TRIGGER_MOVIE (upstream acknowledges it at
    // once, playing nothing) -> the world screen's MovieFrame (PLAY_MOVIE), then GameMovieFinished; and
    // SMSG_TRIGGER_CINEMATIC (CINEMATIC_START, the flyby of a new character): the client says the camera began
    // (CMSG_NEXT_CINEMATIC_CAMERA, as upstream) and ended (CMSG_COMPLETE_CINEMATIC - StopCinematic / its end).
public:
    bool takeMovie(uint32_t& movieId);         // a movie the server started (Movie.dbc id), once
    void movieFinished();                      // GameMovieFinished: CMSG_COMPLETE_MOVIE
    bool takeCinematic(uint32_t& sequenceId);  // a CinematicSequences.dbc id, once
    void cinematicCameraStarted();             // CMSG_NEXT_CINEMATIC_CAMERA
    void cinematicFinished();                  // CMSG_COMPLETE_CINEMATIC

private:
    bool handleMoviePacket(game::LogicalOpcode op, network::Packet& packet);
    uint32_t pendingMovie_ = 0, pendingCinematic_ = 0;
    // cinematics (end)
    // raid and battlegrounds: the raid half of upstream SocialHandler beyond the party code (convertToRaid,
    // setRaidSubgroup, swapRaidSubgroup, setGroupAssistant, setPartyAssignment, the raid marks of
    // MSG_RAID_TARGET_UPDATE, the ready check) in psp_world_session_raid.cpp, and its battleground half
    // (handleBattlefieldStatus / List, joinBattlefield, acceptBattlefield, leaveBattlefield, handlePvpLogData,
    // SMSG_GROUP_JOINED_BATTLEGROUND...) with GameHandler's world states (SMSG_INIT_WORLD_STATES /
    // SMSG_UPDATE_WORLD_STATE) in psp_world_session_battleground.cpp. PSP: the lines are upstream's in the
    // running language, and where upstream reads an older layout the 3.3.5a one as AzerothCore writes it
    // (SMSG_BATTLEFIELD_LIST, the none /
    // in-progress tails of SMSG_BATTLEFIELD_STATUS, the 8 bytes of CMSG_BATTLEFIELD_PORT /
    // CMSG_LEAVE_BATTLEFIELD).
public:
    // --- Raid (GetNumRaidMembers / GetRaidRosterInfo / IsRaidLeader / IsRaidOfficer) ---
    bool isInRaid() const { return game::groupIsRaid(partyData_.groupType); }
    bool isInBattlegroundGroup() const { return (partyData_.groupType & 0x01) != 0; }  // GROUPTYPE_BG
    struct RaidRosterEntry {
        uint64_t guid = 0;
        std::string name;
        uint8_t subGroup = 0;  // 0..7 (FrameXML shows 1..8)
        uint8_t rank = 0;      // 2 leader, 1 assistant (MEMBER_FLAG_ASSISTANT), 0
        uint8_t flags = 0;     // MEMBER_FLAG_*: 0x02 main tank, 0x04 main assist
        bool online = true, master = false, self = false;
    };
    // GetRaidRosterInfo 1..n: the roster in the server's order, the player after the members (as the client's
    // GROUP_LIST handler appends him); empty when not in a raid.
    std::vector<RaidRosterEntry> raidRoster() const;
    uint8_t raidRank(uint64_t guid) const;  // 2 leader, 1 assistant, 0
    bool isRaidOfficer() const;              // IsRaidOfficer: the player leads or assists the raid
    void convertToRaid();                                            // ConvertToRaid
    void setRaidSubgroup(const std::string& name, uint8_t group);    // SetRaidSubgroup: group 1..8
    void swapRaidSubgroup(const std::string& a, const std::string& b);  // SwapRaidSubgroup
    void setGroupAssistant(uint64_t guid, bool apply);               // PromoteToAssistant / DemoteAssistant
    void setPartyAssignment(uint8_t assignment, uint64_t guid, bool apply);  // 0 MAINTANK, 1 MAINASSIST
    void reportPvpAfk(uint64_t guid);                                // ReportPlayerIsPVPAFK
    // Raid marks (upstream raidTargetGuids_): GetRaidTargetIndex - 1, kNoRaidMark when unmarked.
    static constexpr uint8_t kNoRaidMark = 0xFF;
    uint8_t raidMarkOf(uint64_t guid) const;
    uint64_t raidMarkGuid(uint8_t icon) const { return icon < 8 ? raidMarks_[icon] : 0; }
    bool anyRaidMark() const;
    // SetRaidTarget(unit, index): icon 0..7, kNoRaidMark clears the unit's mark (the local marks when not in a
    // group - as upstream setRaidMarkLocally).
    void setRaidMark(uint64_t guid, uint8_t icon);
    uint32_t raidMarksVersion() const { return raidMarksVersion_; }  // RAID_TARGET_UPDATE
    // Ready check (GetReadyCheckStatus): tracked only by the raid's leader / assistants and the party leader,
    // whom the server sends the answers (AzerothCore Group::BroadcastReadyCheck).
    enum class ReadyState : uint8_t { None, Waiting, Ready, NotReady, Afk };
    ReadyState readyCheckState(uint64_t guid) const;
    uint32_t readyCheckSeq() const { return readyCheckSeq_; }              // READY_CHECK
    uint32_t readyCheckFinishSeq() const { return readyCheckFinishSeq_; }  // READY_CHECK_FINISHED
    uint32_t readyCheckFinishedMs() const { return readyCheckFinishedMs_; }  // logUptimeMs of FINISHED (0 = none)
    uint32_t readyCheckVersion() const { return readyCheckVersion_; }      // READY_CHECK_CONFIRM & co.
    uint64_t readyCheckInitiator() const { return readyCheckInitiator_; }  // 0 = no check running
    const std::string& readyCheckInitiatorName() const { return readyCheckInitiatorName_; }
    uint32_t readyCheckTimeLeftMs() const;  // READY_CHECK's timeLeft
    void doReadyCheck();                    // DoReadyCheck
    void confirmReadyCheck(bool ready);     // ConfirmReadyCheck
    void raidTick();                        // per frame: the initiator's ProcessReadyCheckTimer, the icons' end
    // --- Battlegrounds (GetBattlefieldStatus / GetBattlefieldInfo / JoinBattlefield / AcceptBattlefieldPort) ---
    static constexpr int kBgQueueSlots = 2;  // MAX_BATTLEFIELD_QUEUES (the client drops slot 2+)
    struct BgQueue {
        uint32_t status = 0;  // 0 none, 1 queued, 2 confirm, 3 active, 4 waiting to leave
        uint8_t raw[8] = {};  // arena type, isArena, bg type, 0x1F90 as they came (PORT / LEAVE repeat them)
        uint32_t bgTypeId = 0, instanceId = 0, mapId = 0, avgWaitMs = 0;
        uint8_t arenaType = 0, minLevel = 0, maxLevel = 0;  // arenaType 2/3/5 = arena team size, 0 = battleground
        bool rated = false;
        uint32_t queuedSinceMs = 0;  // logUptimeMs when the queue began (GetBattlefieldTimeWaited)
        uint32_t portExpireMs = 0;   // status 2: logUptimeMs when the invitation lapses (0 = none)
        uint32_t seq = 0;            // bumped by every status of the slot
    };
    const BgQueue& bgQueue(int slot) const;
    uint32_t bgStatusSeq() const { return bgStatusSeq_; }  // UPDATE_BATTLEFIELD_STATUS
    int bgStatusSlot() const { return bgStatusSlot_; }     // its slot (BattlefieldFrame_UpdateStatus' mapIndex)
    int activeBgSlot() const { return activeBgSlot_; }     // the battlefield we are in (status 3), -1 none
    uint32_t bgActiveMap() const { return bgActiveMap_; }
    uint32_t bgExpireMs() const { return bgExpireMs_; }    // GetBattlefieldInstanceExpiration (0 = not closing)
    uint32_t bgStartMs() const { return bgStartMs_; }      // GetBattlefieldInstanceRunTime's start (0 = unknown)
    // The battlemaster's list (BATTLEFIELDS_SHOW).
    struct BgList {
        uint64_t battlemaster = 0;
        uint8_t fromWhere = 0, minLevel = 0, maxLevel = 0;
        uint32_t bgTypeId = 0;
        bool hasWin = false, isRandom = false, hasWinRandom = false;
        uint32_t winHonor = 0, winArena = 0, lossHonor = 0, winHonorRandom = 0, winArenaRandom = 0, lossHonorRandom = 0;
        std::vector<uint32_t> instances;
    };
    const BgList& battlefieldList() const { return bgList_; }
    uint32_t battlefieldListSeq() const { return bgListSeq_; }  // BATTLEFIELDS_SHOW
    void battlemasterHello(uint64_t guid);           // CMSG_BATTLEMASTER_HELLO (an NPC without gossip)
    void joinBattlefield(int index, bool asGroup);   // JoinBattlefield: 0 first available, n the list's n-th
    void acceptBattlefieldPort(int slot, bool accept);  // AcceptBattlefieldPort
    void leaveBattlefield();                         // LeaveBattlefield
    void requestPvpLog();                            // RequestBattlefieldScoreData (MSG_PVP_LOG_DATA)
    const game::BgScoreboardData& bgScoreboard() const { return bgScore_; }
    uint32_t bgScoreVersion() const { return bgScoreVersion_; }  // UPDATE_BATTLEFIELD_SCORE
    bool bgPlayerLook(uint64_t guid, uint8_t& race, uint8_t& gender, uint8_t& cls) const;
    // SMSG_GROUP_JOINED_BATTLEGROUND (result, or a BattlemasterList id) - the screen writes the line (the
    // BG names are ui.pak's); SMSG_BATTLEFIELD_PORT_DENIED and the ready check error are red lines here.
    struct BgEvent {
        int32_t result = 0;
        std::string name;  // ERR_BATTLEGROUND_JOIN_TIMED_OUT's player
    };
    bool takeBgEvent(BgEvent& out);
    // World states (upstream worldStates_): the value of a key (0 when unknown), the map / area of the last
    // SMSG_INIT_WORLD_STATES, a version bumped by every change; the realm time of the last
    // SMSG_WORLD_STATE_UI_TIMER_UPDATE (the "%Nk" timers of WorldStateUI) and logUptimeMs when it came.
    uint32_t worldState(uint32_t key, bool* found = nullptr) const;
    uint32_t worldStateMap() const { return wsMap_; }
    uint32_t worldStateArea() const { return wsArea_; }
    uint32_t worldStatesVersion() const { return wsVersion_; }
    bool worldStateServerTime(uint32_t& unixTime, uint32_t& receivedMs) const;
    void requestWorldStateTimer();  // CMSG_WORLD_STATE_UI_TIMER_UPDATE (at most every 10 s)
    void bgTick();  // per frame: the battleground chat filter (WorldStateAlwaysUpFrame), the shutdown lines

private:
    bool handleRaidBgPacket(game::LogicalOpcode op, network::Packet& packet);   // raid; then the battleground's
    bool handleBattlegroundPacket(game::LogicalOpcode op, network::Packet& packet);
    void raidRosterUpdate();  // handleGroupList: the raid's names for the ready check
    void raidSystemLine(const std::string& text, bool joined, const std::string& name);  // through the BG filter
    std::string raidName(uint64_t guid) const;
    void handleReadyCheck(network::Packet& packet);
    void handleReadyCheckConfirm(network::Packet& packet);
    void processReadyCheck();  // the initiator's summary + MSG_RAID_READY_CHECK_FINISHED
    void clearRaidMarks();
    struct RaidKnown {
        uint64_t guid = 0;
        std::string name;
    };
    std::vector<RaidKnown> raidKnown_;  // the raid's names (the ready check names those who left)
    std::array<uint64_t, 8> raidMarks_{};
    uint32_t raidMarksVersion_ = 0, raidLogs_ = 0;
    struct ReadyEntry {
        uint64_t guid = 0;
        ReadyState state = ReadyState::None;
    };
    std::vector<ReadyEntry> readyStates_;
    uint64_t readyCheckInitiator_ = 0;
    std::string readyCheckInitiatorName_;
    uint32_t readyCheckEndMs_ = 0;  // the ready check's timer (0 = none)
    uint32_t readyCheckFinishedMs_ = 0, readyCheckSeq_ = 0, readyCheckFinishSeq_ = 0, readyCheckVersion_ = 0;
    void handleBattlefieldStatus(network::Packet& packet);
    void handleBattlefieldList(network::Packet& packet);
    void handlePvpLogData(network::Packet& packet);
    void handleGroupJoinedBattleground(network::Packet& packet);
    void handleBattlegroundPlayer(network::Packet& packet, bool joined);
    void handleInitWorldStates(network::Packet packet);  // a copy: the zone goes on to onPacket's switch
    void bgFilterStart();
    void bgFilterStop();
    std::array<BgQueue, kBgQueueSlots> bgQueues_{};
    int activeBgSlot_ = -1, bgStatusSlot_ = -1;
    uint32_t bgActiveMap_ = 0, bgExpireMs_ = 0, bgStartMs_ = 0, bgStatusSeq_ = 0;
    uint8_t bgTeam_ = 0;
    BgList bgList_;
    uint32_t bgListSeq_ = 0;
    game::BgScoreboardData bgScore_;
    uint32_t bgScoreVersion_ = 0;
    std::unordered_map<uint64_t, PartyLook> bgLooks_;  // name query answers of the score's players
    std::unordered_set<uint64_t> bgNameQueried_;
    std::deque<BgEvent> bgEvents_;
    std::unordered_map<uint32_t, uint32_t> worldStates_;
    uint32_t wsMap_ = 0, wsArea_ = 0, wsVersion_ = 0, wsServerTime_ = 0, wsServerTimeMs_ = 0, wsTimerAskMs_ = 0;
    // WorldStateAlwaysUpFrame's chat filter (FILTERED_BG_CHAT_*): joins gathered for the first 60 s and said
    // every 5 s as one line, leaves dropped once the battle has a winner.
    bool bgFilterOn_ = false;
    uint32_t bgFilterStartMs_ = 0, bgFilterLastMs_ = 0;
    std::vector<std::string> bgAdded_, bgSubtracted_;
    // BattlefieldTimerFrame_OnUpdate: the shutdown / battle-over lines at 600 / 300 / 60 / 15 s.
    int bgTimerThreshold_ = 0;
    int64_t bgTimerPrevMod_ = 0;
    uint32_t bgTimerCheckMs_ = 0, bgLogs_ = 0;
    // raid and battlegrounds (end)
    // expansion windows: the Dungeon Finder (psp_world_session_lfg.cpp: upstream SocialHandler's LFG
    // half), the achievements (psp_world_session_achievements.cpp: GameHandler::handleAllAchievementData,
    // SMSG_CRITERIA_UPDATE, SpellHandler::handleAchievementEarned) and the calendar (psp_world_session_calendar.cpp:
    // upstream's calendar handlers and SocialHandler's calendar senders) without GameHandler; the DBC side from
    // data/wotlk.pak (psp_wotlk_db.hpp), read on demand. PSP: the chat / error lines are the original client's
    // (GlobalStrings ruRU, red or chat by the port's GameLine rule), not upstream's English.
public:
    // --- Dungeon Finder ---
    struct LfgEvent {  // the addon events upstream fires (LFGEventFrame / LFDFrame / AlertFrame listen to them)
        enum Kind : uint8_t {
            Update,             // LFG_UPDATE (the queue state / queued list)
            QueueStatus,        // LFG_QUEUE_STATUS_UPDATE
            ProposalShow,       // LFG_PROPOSAL_SHOW
            ProposalUpdate,     // LFG_PROPOSAL_UPDATE
            ProposalFailed,     // LFG_PROPOSAL_FAILED
            ProposalSucceeded,  // LFG_PROPOSAL_SUCCEEDED
            RoleCheckShow,      // LFG_ROLE_CHECK_SHOW
            RoleCheckHide,      // LFG_ROLE_CHECK_HIDE
            BootUpdate,         // LFG_BOOT_PROPOSAL_UPDATE
            RandomInfo,         // LFG_UPDATE_RANDOM_INFO + LFG_LOCK_INFO_RECEIVED (SMSG_LFG_PLAYER_INFO)
            CompletionReward,   // LFG_COMPLETION_REWARD
            OfferContinue,      // LFG_OFFER_CONTINUE (arg: the dungeon id)
            OpenFinder,         // SMSG_OPEN_LFG_DUNGEON_FINDER
        };
        Kind kind = Update;
        uint32_t arg = 0;
    };
    struct LfgQueueStats {  // SMSG_LFG_QUEUE_STATUS (GetLFGQueueStats)
        bool known = false;
        uint32_t dungeonId = 0;
        int32_t avgWait = -1, myWait = -1, tankWait = -1, healerWait = -1, dpsWait = -1;  // s, -1 = unknown
        uint8_t needTank = 0, needHealer = 0, needDps = 0;
        uint32_t queuedSec = 0;   // in the queue when the packet came
        uint32_t receivedMs = 0;  // logUptimeMs() then
    };
    struct LfgBoot {  // SMSG_LFG_BOOT_PROPOSAL_UPDATE (GetLFGBootProposal)
        bool inProgress = false, didVote = false, myVote = false;
        uint64_t victim = 0;
        uint32_t totalVotes = 0, bootVotes = 0, timeLeft = 0, votesNeeded = 0, receivedMs = 0;
        std::string reason;
    };
    struct LfgPartyLock {  // one member of SMSG_LFG_PARTY_INFO
        uint64_t guid = 0;
        std::unordered_map<uint32_t, uint32_t> locks;  // dungeon id -> LfgLockStatusType
    };
    bool takeLfgEvent(LfgEvent& out);
    game::LfgState lfgState() const { return lfgState_; }
    uint32_t lfgVersion() const { return lfgVersion_; }  // bumped by every LFG packet
    uint32_t lfgDungeonId() const { return lfgDungeonId_; }
    const std::vector<uint32_t>& lfgQueuedList() const { return lfgQueuedList_; }  // GetLFGQueuedList (ids)
    // GetLFGProposal: the proposal on offer (0 = none), the encounters done (a mask), the group.
    uint32_t lfgProposalId() const { return lfgProposalId_; }
    uint32_t lfgProposalEncounters() const { return lfgProposalEncounters_; }
    const std::vector<game::LfgProposalMember>& lfgProposalMembers() const { return lfgProposalMembers_; }
    bool lfgProposalAnswered() const;  // the own row has answered (hasResponded)
    const LfgQueueStats& lfgQueueStats() const { return lfgQueue_; }
    const std::vector<game::LfgRoleCheckDungeon>& lfgRoleCheckDungeons() const { return lfgRoleCheckDungeons_; }
    const LfgBoot& lfgBoot() const { return lfgBoot_; }
    std::string lfgBootTargetName() const;
    // SMSG_LFG_PLAYER_INFO: the random dungeons on offer with their rewards, and the locks (dungeon -> status);
    // SMSG_LFG_PARTY_INFO: the other members' locks.
    bool lfgPlayerInfoKnown() const { return lfgPlayerInfoKnown_; }
    const std::vector<game::LfgReward>& lfgRandomDungeons() const { return lfgRewards_; }
    const std::unordered_map<uint32_t, uint32_t>& lfgLocks() const { return lfgLocks_; }
    const std::vector<LfgPartyLock>& lfgPartyLocks() const { return lfgPartyLocks_; }
    const game::LfgCompletionReward& lfgCompletionReward() const { return lfgCompletionReward_; }
    // GetLFGRoles / SetLFGRoles (bits 1 leader, 2 tank, 4 healer, 8 damage). PSP: kept in wowee.cfg lfgroles (the
    // client keeps them between sessions).
    uint8_t lfgRoles();
    void lfgSetRoles(uint8_t roles);
    void lfgJoin(const std::vector<uint32_t>& dungeonIds);  // JoinLFG with lfgRoles()
    void lfgLeave();                                        // LeaveLFG
    void lfgAnswerProposal(bool accept);                    // AcceptProposal / RejectProposal
    void lfgCompleteRoleCheck(bool accept);                 // CompleteLFGRoleCheck (CMSG_LFG_SET_ROLES)
    void lfgSetBootVote(bool vote);                         // SetLFGBootVote
    void lfgTeleport(bool out);                             // LFGTeleport
    void lfgRequestLockInfo();                              // RequestLFDPlayerLockInfo / RequestLFDPartyLockInfo
    // The dungeon catalogue (data/wotlk.pak "lfg"), loaded on first use; the window frees it when nothing needs it.
    LfgDb& lfgDb();
    void lfgReleaseDb();
    std::string lfgDungeonName(uint32_t dungeonId);
    // --- Achievements ---
    struct CriteriaProgress {  // achievement_criteria.hpp's record, as kept
        uint64_t counter = 0;
        uint32_t flags = 0, date = 0, elapsed = 0;
    };
    const std::unordered_map<uint32_t, uint32_t>& earnedAchievements() const { return achEarned_; }  // id -> date
    const std::unordered_map<uint32_t, CriteriaProgress>& criteriaProgress() const { return achCriteria_; }
    bool achievementsKnown() const { return achKnown_; }        // SMSG_ALL_ACHIEVEMENT_DATA seen
    uint32_t achievementVersion() const { return achVersion_; }  // bumped by every change
    bool takeAchievementEarned(uint32_t& id);                  // ACHIEVEMENT_EARNED (own): the alert
    std::string achievementName(uint32_t id);                  // the chat link's name (wotlk.pak on demand)
    void queryInspectAchievements(uint64_t guid);              // CMSG_QUERY_INSPECT_ACHIEVEMENTS (comparison)
    const std::unordered_map<uint32_t, uint32_t>* inspectedAchievements(uint64_t guid) const;
    uint32_t inspectVersion() const { return achInspectVersion_; }
    // --- Calendar ---
    void calendarRequest();  // CMSG_CALENDAR_GET_CALENDAR + _GET_NUM_PENDING (OpenCalendar)
    bool calendarKnown() const { return calendarKnown_; }
    const game::CalendarData& calendarData() const { return calendarData_; }
    uint32_t calendarVersion() const { return calendarVersion_; }
    uint32_t calendarReceivedMs() const { return calendarReceivedMs_; }
    // An alert changed the list (an invite, a status, an event, a lockout): the window asks again
    // (CALENDAR_UPDATE_EVENT_LIST). Cleared by calendarRequest().
    bool calendarStale() const { return calendarStale_; }
    void calendarRequestEvent(uint64_t eventId);  // CMSG_CALENDAR_GET_EVENT (CalendarOpenEvent)
    bool calendarEventKnown() const { return calendarEventKnown_; }
    const game::CalendarEventDetail& calendarEvent() const { return calendarEvent_; }
    uint32_t calendarEventVersion() const { return calendarEventVersion_; }
    // CMSG_CALENDAR_EVENT_RSVP (CalendarContextInviteAvailable / ...Decline / ...Tentative; wire status: 1 accepted,
    // 2 declined, 8 tentative) and CMSG_CALENDAR_EVENT_REMOVE_INVITE of the own invite (CalendarContextInviteRemove).
    void calendarRsvp(uint64_t eventId, uint64_t inviteId, uint32_t status);
    void calendarRemoveOwnInvite(uint64_t eventId, uint64_t inviteId);
    void calendarSignUp(uint64_t eventId, bool tentative);  // CalendarEventSignUp: a guild event's sign-up
    bool takeCalendarError(std::string& out);  // CALENDAR_ERROR (SMSG_CALENDAR_COMMAND_RESULT)

private:
    bool handleLfgPacket(game::LogicalOpcode op, network::Packet& packet);
    bool handleAchievementPacket(game::LogicalOpcode op, network::Packet& packet);
    bool handleCalendarPacket(game::LogicalOpcode op, network::Packet& packet);
    void lfgPush(LfgEvent::Kind kind, uint32_t arg = 0);
    void lfgLine(const std::string& text, GameLine kind);  // a game line (GameLine)
    void lfgReadDungeonList(network::Packet& packet);
    void lfgApplyUpdate(uint8_t updateType, bool extraInfo, bool queued, bool inDungeon);
    game::LfgState lfgState_ = game::LfgState::None;
    uint32_t lfgVersion_ = 0, lfgDungeonId_ = 0, lfgProposalId_ = 0, lfgProposalEncounters_ = 0, lfgShownProposal_ = 0;
    std::vector<uint32_t> lfgQueuedList_;
    std::vector<game::LfgProposalMember> lfgProposalMembers_;
    LfgQueueStats lfgQueue_;
    std::vector<game::LfgRoleCheckDungeon> lfgRoleCheckDungeons_;
    std::vector<std::pair<uint64_t, uint32_t>> lfgRoleCheckRoles_;  // the members' roles of the last role check
    LfgBoot lfgBoot_;
    bool lfgPlayerInfoKnown_ = false, lfgRolesLoaded_ = false;
    uint8_t lfgRolesValue_ = 0;
    std::vector<game::LfgReward> lfgRewards_;
    std::unordered_map<uint32_t, uint32_t> lfgLocks_;
    std::vector<LfgPartyLock> lfgPartyLocks_;
    game::LfgCompletionReward lfgCompletionReward_;
    std::deque<LfgEvent> lfgEvents_;
    LfgDb lfgDb_;
    uint32_t lfgLogs_ = 0;
    std::unordered_map<uint32_t, uint32_t> achEarned_;
    std::unordered_map<uint32_t, CriteriaProgress> achCriteria_;
    std::unordered_map<uint64_t, std::unordered_map<uint32_t, uint32_t>> achInspected_;
    std::unordered_map<uint32_t, std::string> achNameCache_;
    std::deque<uint32_t> achEarnedQueue_;
    bool achKnown_ = false;
    uint32_t achVersion_ = 0, achInspectVersion_ = 0, achLogs_ = 0;
    game::CalendarData calendarData_;
    game::CalendarEventDetail calendarEvent_;
    bool calendarKnown_ = false, calendarEventKnown_ = false, calendarStale_ = false;
    uint32_t calendarVersion_ = 0, calendarEventVersion_ = 0, calendarReceivedMs_ = 0, calendarLogs_ = 0;
    std::deque<std::string> calendarErrors_;
    // expansion windows (end)
    // inspect: the inspect of another player (InspectFrame) -
    // upstream SocialHandler's inspect half (inspectUnit, handleInspectResults, requestInspectHonorData,
    // MSG_INSPECT_HONOR_STATS) without GameHandler, the packets read and written as the servers write and read them
    // (psp_world_session_inspect.cpp): NotifyInspect, SMSG_INSPECT_TALENT (talents, the
    // items), RequestInspectHonorData, MSG_INSPECT_HONOR_STATS, MSG_INSPECT_ARENA_TEAMS,
    // ClearInspectPlayer. Only answers for the player being inspected are kept (upstream's honour check).
public:
    using InspectSlot = InspectItemRecord;  // one record of BuildEnchantmentsInfoData (psp_inspect_parse.hpp)
    struct InspectArenaTeam {        // MSG_INSPECT_ARENA_TEAMS, a slot (+ SMSG_ARENA_TEAM_QUERY_RESPONSE)
        uint32_t teamId = 0, rating = 0, games = 0, wins = 0, played = 0, personalRating = 0;
        bool infoKnown = false;      // the query's answer
        std::string name;
        uint32_t type = 0, background = 0, emblem = 0, emblemColor = 0, border = 0, borderColor = 0;
    };
    struct InspectResult {
        uint64_t guid = 0;           // NotifyInspect's player (0 = none yet)
        bool talentsKnown = false;   // SMSG_INSPECT_TALENT for him came
        uint32_t unspentTalents = 0;
        uint8_t talentGroups = 0, activeGroup = 0;
        std::unordered_map<uint32_t, uint8_t> talentRanks;  // the active group: talent id -> rank (1..5)
        uint32_t talentPoints = 0;   // spent in the active group
        bool gearKnown = false;
        std::array<InspectSlot, 19> slots{};
        bool honorKnown = false;     // MSG_INSPECT_HONOR_STATS for him came
        uint8_t lifetimeRank = 0;
        uint16_t todayKills = 0, yesterdayKills = 0;
        uint32_t todayHonor = 0, yesterdayHonor = 0, lifetimeKills = 0;
        std::array<InspectArenaTeam, 3> arena{};
        uint32_t version = 0;        // bumped by every answer and every change of the player
    };
    // NotifyInspect: CMSG_INSPECT for `guid` (the UI checks CanInspect first); a new player drops what was known.
    void notifyInspect(uint64_t guid);
    void clearInspectPlayer();       // ClearInspectPlayer (InspectFrame_OnHide)
    void requestInspectHonorData();  // RequestInspectHonorData: MSG_INSPECT_HONOR_STATS + MSG_INSPECT_ARENA_TEAMS
    const InspectResult& inspectResult() const { return inspect_; }
    // GameTooltip:SetInventoryItem(InspectFrame.unit, slot): the item tooltip of an inspected slot takes this
    // pseudo item guid (+ the slot); itemEnchantments answers it from the inspect (else the visible item field).
    static constexpr uint64_t kInspectItemGuid = 0x7E57000000000000ull;
    bool inspectEnchantments(uint64_t guid, uint32_t& permanent, uint32_t& temporary,
                             std::array<uint32_t, 3>& sockets) const;
    // InspectTalentFrame (GetTalentTabInfo / GetTalentInfo with inspect = true): the talent tree of a class - spells.pak
    // "talents" filtered by it (the own one is loadTalentDbc's), read when the tab first shows, dropped with the
    // inspect. Tabs in OrderIndex order; false = no talent data (an old spells.pak).
    bool loadInspectTalents(uint8_t classId);
    const std::vector<game::TalentTabEntry>& inspectTalentTabs() const { return inspectTabs_; }
    const std::vector<game::TalentEntry>& inspectTalents() const { return inspectTalentList_; }
    const game::TalentEntry* inspectTalentEntry(uint32_t talentId) const;

private:
    bool handleInspectPacket(game::LogicalOpcode op, network::Packet& packet);
    InspectResult inspect_;
    uint32_t inspectLogs_ = 0;
    uint8_t inspectTalentClass_ = 0;
    std::vector<game::TalentTabEntry> inspectTabs_;
    std::vector<game::TalentEntry> inspectTalentList_;
    // inspect (end)
    // transports (psp_world_session_transport.cpp): what the
    // client keeps of its transports and their riders. The game objects of GAMEOBJECT_TYPE_TRANSPORT (11: lifts,
    // the tram) and _MO_TRANSPORT (15: ships, zeppelins) - the server sends them map-wide - with the fields the client
    // moves them by (the movement block's path time, GAMEOBJECT_LEVEL, _PARENTROTATION, the state); an
    // MO_TRANSPORT's template (CMSG_GAMEOBJECT_QUERY: data0 path, data1 speed, data2 acceleration); the units and
    // players on a transport (MOVEMENTFLAG_ONTRANSPORT in their create / movement blocks,
    // MSG_MOVE_* and SMSG_MONSTER_MOVE_TRANSPORT: the transport's guid and the place in its frame); and ours as the
    // world screen last said it (sent with every movement packet) and as the server puts it (our create block, a
    // transfer on a ship).
public:
    struct TransportObject {
        uint64_t guid = 0;
        uint32_t entry = 0, displayId = 0;
        uint8_t type = 0, state = 0;
        glm::vec3 pos{0.0f};            // the create block's place (server coords): a lift's rest, a ship's place then
        float orientation = 0.0f, scale = 1.0f;
        glm::vec4 parentRot{0.0f, 0.0f, 0.0f, 1.0f};  // GAMEOBJECT_PARENTROTATION (x, y, z, w)
        uint32_t level = 0;             // GAMEOBJECT_LEVEL: a ship's period, a lift's pause time
        uint32_t timeOffsetMs = 0;      // create block's path progress minus transportClockMs() on arrival
        bool hasTime = false;
        uint32_t stateSerial = 0;       // bumped on every state change (the lift's state machine)
        uint8_t prevState = 0;          // the state before the last change
        uint32_t stateChangeMs = 0;     // transportClockMs() at the last change
    };
    struct TransportTemplate {         // SMSG_GAMEOBJECT_QUERY_RESPONSE of a transport entry
        uint32_t type = 0, taxiPathId = 0, moveSpeed = 0, accelRate = 0, physics = 0;
        bool known = false;
    };
    struct TransportRider {             // a unit / player on a transport
        uint64_t transport = 0;
        glm::vec3 local{0.0f};          // where it stands, the transport's frame (server axes)
        glm::vec3 from{0.0f};           // where it stood: the drawn place eases from here to `local`
        float localO = 0.0f;
        uint32_t moveStartMs = 0, moveMs = 0;
        uint32_t moveFlags = 0;         // its movement flags (FORWARD...: the run animation)
        bool spline = false;            // SMSG_MONSTER_MOVE_TRANSPORT: moving for moveMs
    };
    static uint32_t transportClockMs();  // the client clock the path times are kept against
    const std::unordered_map<uint64_t, TransportObject>& transportObjects() const { return transportObjects_; }
    uint32_t transportObjectsVersion() const { return transportVersion_; }
    // The template of an MO_TRANSPORT entry; the first ask sends CMSG_GAMEOBJECT_QUERY (nullptr until answered).
    const TransportTemplate* transportTemplate(uint32_t entry, uint64_t guid);
    const std::unordered_map<uint64_t, TransportRider>& transportRiders() const { return transportRiders_; }
    // The world screen's riding state (0 = none), kept for the transfer on a ship (SMSG_NEW_WORLD's place is then
    // the place on the transport).
    void setOwnTransport(uint64_t guid, const glm::vec3& local, float localO);
    uint64_t ownTransport() const { return ownTransportGuid_; }
    // Where the server put us on a transport (our create block, or SMSG_NEW_WORLD while riding): once.
    bool takeOwnTransportPlace(uint64_t& guid, glm::vec3& local, float& localO);

private:
    bool handleTransportPacket(game::LogicalOpcode op, network::Packet& packet);
    void transportUpdateBlock(const game::UpdateBlock& b);
    void transportForget(uint64_t guid);
    void transportResetWorld();  // a new map: none of the old one's transports and riders
    // handleOtherMovement's ONTRANSPORT block (guid, offset, time, seat, time2) and the rider it describes.
    void transportReadMoveBlock(network::Packet& packet, game::MovementInfo& info);
    void transportOtherMove(uint64_t guid, const game::MovementInfo& info, uint32_t durationMs);
    void handleMonsterMoveTransport(network::Packet& packet);
    bool transportArrival(const glm::vec3& pos, float orientation);  // transferSendArrival on a ship: true = handled
    std::unordered_map<uint64_t, TransportObject> transportObjects_;
    std::unordered_map<uint32_t, TransportTemplate> transportTemplates_;
    std::unordered_set<uint32_t> transportQueried_;
    std::unordered_map<uint64_t, TransportRider> transportRiders_;
    uint32_t transportVersion_ = 0, transportLogs_ = 0, riderLogs_ = 0;
    uint64_t ownTransportGuid_ = 0;
    glm::vec3 ownTransportLocal_{0.0f};
    float ownTransportO_ = 0.0f;
    bool ownPlacePending_ = false;
    uint64_t ownPlaceGuid_ = 0;
    glm::vec3 ownPlaceLocal_{0.0f};
    float ownPlaceO_ = 0.0f;
    // transports (end)
    // flying (psp_world_session_flying.cpp): the server's movement-flag toggles of the own mover (CAN_FLY, WATER_WALK,
    // FEATHER_FALL, HOVER, gravity, CAN_TRANSITION_BETWEEN_SWIM_AND_FLY) - the flag set / cleared and ACKed as upstream's
    // handleForceMoveFlagChange (guid, counter, the movement block, AzerothCore's isApplied); the flight / swim speeds of
    // the spline opcodes; other players' climb packets, flight / swim speeds and their prediction.
public:
    uint32_t ownGrantedMoveFlags() const { return ownGranted_; }    // CAN_FLY | WATER_WALK | FEATHER_FALL | HOVER | LEVITATING
    uint32_t ownCreateMoveFlags() const { return ownCreateFlags_; } // our create block's flags (in the air / water)
    uint32_t moveToggleSeq() const { return moveToggleSeq_; }       // bumps on every toggle / create block / reset
    float serverSwimSpeed() const { return serverSwimSpeed_; }
    float serverSwimBackSpeed() const { return serverSwimBackSpeed_; }
    float serverFlightSpeed() const { return serverFlightSpeed_; }
    float serverFlightBackSpeed() const { return serverFlightBackSpeed_; }
    uint32_t remoteMoveFlags(uint64_t guid) const;  // another unit's last movement flags (0 = none known)
private:
    bool handleFlyingPacket(game::LogicalOpcode op, network::Packet& packet);
    void flyingUpdateBlock(const game::UpdateBlock& b);  // applyUpdateBlock: speeds of others, our create flags
    void flyingForget(uint64_t guid);                    // removeEntity
    void flyingResetWorld();                             // transferResetWorld
    void moveToggle(network::Packet& packet, int toggle, bool on, game::Opcode ack, const char* name);
    void flightSplineSpeed(network::Packet& packet, game::LogicalOpcode op);
    void peekFlightSpeed(network::Packet& packet, game::LogicalOpcode op);
    bool flightPredict(uint64_t guid, const game::MovementInfo& m, float dt, float moveYaw, glm::vec3& pos,
                       bool& moving) const;  // predictRemote: a flier / swimmer
    uint32_t ownGranted_ = 0, ownCreateFlags_ = 0, moveToggleSeq_ = 0, flyLogs_ = 0;
    std::unordered_map<uint64_t, float> unitFlightSpeed_, unitSwimSpeed_;
    // flying (end)
    // game object use (psp_world_session_gouse.cpp): using a game object
    // as the 3.3.5a client does it: a right click = the type's "can use" check -> its use (generally: the lock
    // over Lock.dbc - a known
    // SPELL_EFFECT_OPEN_LOCK spell cast at the object, a key item used on it, else CMSG_GAMEOBJ_USE) -> then
    // CMSG_GAMEOBJ_REPORT_USE; the clickable rule, the reach; the object cache
    // (SMSG_GAMEOBJECT_QUERY_RESPONSE), ItemTextFrame's pages of a text object (CMSG_PAGE_TEXT_QUERY), the
    // fishing bobber, the barber shop, the stand state a chair sets, the objects' custom / despawn animations. The
    // client's per-type property table says which data[] field is a type's lock, page, spell...
public:
    struct GoTemplate {  // upstream GameObjectQueryResponseData + the 3.3.5a tail (size, quest items)
        uint32_t entry = 0, type = 0, displayId = 0;
        std::string name;
        uint32_t data[24] = {};
        float size = 1.0f;
        uint32_t questItems[6] = {};
        bool known = false;  // false: the server does not know the entry (the high bit of the answer)
    };
    const GoTemplate* goTemplate(uint32_t entry);  // nullptr until the answer; CMSG_GAMEOBJECT_QUERY once per entry
    uint32_t goTemplateVersion() const { return goTemplateVersion_; }
    const GameObjectInfo* findGameObject(uint64_t guid) const;
    // The data[] index of a property (lock 4, page 15, material 17, spell 10, ...) for a type, -1 none.
    static int goDataIndex(uint8_t type, uint8_t prop);
    uint32_t goData(const GameObjectInfo& g, uint8_t prop);  // 0 when the type lacks it or the template is not in
    // The object's lock as the port resolves it (psp_world_session_gouse.cpp) and what its tooltip says.
    struct GoLockState {
        bool hasCase = false;   // a Lock.dbc case of a known kind
        bool openable = true;   // nothing stops it: no case applies now, a known spell with the skill, a key in the bags
        bool applies = false;   // not openable: a case applies in the object's state and nothing opens it
        uint8_t kind = 0;       // the case reported: 1 item (a key), 2 lock type (a skill), 3 spell
        uint8_t caseIndex = 0;
        uint32_t index = 0;     // the case's Index: the item / LockType / spell id
        uint32_t spell = 0;     // what is cast at the object (0: CMSG_GAMEOBJ_USE); not openable: a known spell short on skill
        uint32_t item = 0;      // a key used on it (CMSG_USE_ITEM)
        uint64_t itemGuid = 0;
        uint8_t itemBag = 0xFF, itemSlot = 0;
        int32_t skill = -1, required = 0;  // the player's skill (-1: the lock type needs none) against the case's Skill
        std::string typeName;   // LockType Name_lang: "Herbalism", "Mining", "Opening"...
        std::string cursor;     // LockType CursorName (GatherHerbs, Mine, PickLock)
    };
    bool goLock(const GameObjectInfo& g, GoLockState& out);  // false: no lock row (or no Lock data in ui.pak)
    // Clickable: the object takes the PSP's highlight.
    bool goClickable(const GameObjectInfo& g);
    // The use reach by type: 5 yd; text / questgiver / flags 5.56; chairs 3; a fishing node 100; area damage 0
    // (AzerothCore GameObject::GetInteractionDistance).
    static float goUseDistance(uint8_t type);
    // The use. `inRange` = the caller's reach test (the player within the model's bounds + the reach), the
    // PSP's world screen knows the bounds. False: refused - the ERR_* line went to the red line.
    bool useGameObject(uint64_t guid, bool inRange, bool moving);
    uint32_t goUses() const { return goUses_; }

    // ItemTextFrame (ITEM_TEXT_BEGIN / ITEM_TEXT_READY / ITEM_TEXT_CLOSED) - the pages of a text object or a goober's
    // page (SMSG_GAMEOBJECT_PAGETEXT); item use may open an item's through openItemText.
    struct ItemTextState {
        bool open = false;     // between ITEM_TEXT_BEGIN and ITEM_TEXT_CLOSED
        bool ready = false;    // ITEM_TEXT_READY: the page's text is in
        uint64_t source = 0;   // the object (or item)
        std::string title;     // ItemTextGetItem
        uint8_t material = 0;  // ItemTextGetMaterial: PageTextMaterial.dbc id (0 = Parchment)
        uint32_t page = 0;     // ItemTextGetPage, 1..
        uint32_t pageId = 0;   // PageText id shown
        std::string text;      // ItemTextGetText ($B ... resolved by upstream's normalizeWowTextTokens)
        bool hasNext = false;  // ItemTextHasNextPage
        std::vector<uint32_t> history;  // the pages before (ItemTextPrevPage)
        uint32_t version = 0;  // bumped by every event
    };
    const ItemTextState& itemText() const { return itemText_; }
    void openItemText(uint64_t source, const std::string& title, uint32_t pageId, uint8_t material);
    void itemTextNextPage();  // ItemTextNextPage
    void itemTextPrevPage();  // ItemTextPrevPage
    void closeItemText();     // CloseItemText (ItemTextFrame_OnHide)
    // Fishing: the own bobber (UNIT_FIELD_CHANNEL_OBJECT is a FISHINGNODE in sight), 0 = none; when it last bit
    // (SMSG_GAMEOBJECT_CUSTOM_ANIM on it: the client plays its Custom0), 0 = not since it was cast.
    uint64_t fishingBobber() const;
    uint32_t bobberBiteMs() const { return bobberBiteMs_; }
    // The barber shop: SMSG_ENABLE_BARBER_SHOP (BARBER_SHOP_OPEN) until the player stands up / CancelBarberShop.
    bool barberShopOpen() const { return barberOpen_; }
    uint32_t barberShopSeq() const { return barberSeq_; }  // bumped by every BARBER_SHOP_OPEN
    void alterAppearance(uint32_t hairStyle, uint32_t hairColor, uint32_t facialHair);  // CMSG_ALTER_APPEARANCE
    void cancelBarberShop();  // CancelBarberShop (BARBER_SHOP_CLOSE): stand up
    bool takeBarberResult(uint32_t& result);  // SMSG_BARBER_SHOP_RESULT, once (0 = BARBER_SHOP_SUCCESS)
    // SMSG_STANDSTATE_UPDATE (a chair's SIT_*_CHAIR, standing up): the item use's takeStandState (merged:
    // one queue, one handler).
    // Animations of objects the server asks for: 1 custom (SMSG_GAMEOBJECT_CUSTOM_ANIM, arg = the anim index),
    // 2 despawn (SMSG_GAMEOBJECT_DESPAWN_ANIM), 3 reset (SMSG_GAMEOBJECT_RESET_STATE). Oldest first.
    struct GoAnimEvent {
        uint64_t guid = 0;
        uint8_t kind = 0;
        uint32_t arg = 0;
    };
    bool takeGoAnimEvent(GoAnimEvent& out);
    // The refusals of OnRightClick / Use (ERR_USE_*, ERR_NOT_WHILE_*...) as GlobalStrings ruRU words them - with
    // "|3-N(word)" declension marks the world screen resolves (text.pak) before the red line; oldest first.
    bool takeGoError(std::string& out);
    // Per frame from the world screen: the item text's reach, the bobber, the barber chair.
    void gameObjectUseUpdate(float dtSec, const glm::vec3& selfPos);

private:
    bool handleGameObjectPacket(game::LogicalOpcode op, network::Packet& packet);
    bool loadGoLockData();                       // ui.pak "go_lock" (once)
    bool goKnownLockSpell(uint32_t lockType, uint32_t& spell, int32_t& skill);  // a known OPEN_LOCK spell + skill
    bool goFindItem(uint32_t itemId, uint8_t& bag, uint8_t& slot, uint64_t& guid) const;
    void goCastAtObject(uint32_t spellId, uint64_t goGuid, bool moving);  // stitch point (item use)
    void goUseKey(const GoLockState& lock, uint64_t goGuid);               // stitch point (item use)
    void goReportUse(uint64_t guid);             // CMSG_GAMEOBJ_REPORT_USE
    void itemTextRequestPage(uint32_t pageId);
    std::unordered_map<uint32_t, GoTemplate> goTemplates_;
    std::unordered_set<uint32_t> goQueried_;
    uint32_t goTemplateVersion_ = 0;
    struct GoLockData;  // the baked tables (psp_world_session_gouse.cpp)
    std::shared_ptr<GoLockData> goLockData_;
    bool goLockTried_ = false;
    ItemTextState itemText_;
    std::unordered_map<uint32_t, std::pair<std::string, uint32_t>> pageCache_;  // PageText id -> text, next page
    uint32_t itemTextWaitPage_ = 0, itemTextAskMs_ = 0;
    uint32_t bobberBiteMs_ = 0;
    uint64_t bobberSeen_ = 0;
    bool barberOpen_ = false;
    uint32_t barberSeq_ = 0;
    std::deque<uint32_t> barberResults_;
    std::deque<GoAnimEvent> goAnimEvents_;
    std::deque<std::string> goErrors_;
    void goError(const std::string& text);
    uint32_t goUses_ = 0, goUseLogs_ = 0, goQueryLogs_ = 0;
    // game object use (end)

    // item use (psp_world_session_items.cpp): UseContainerItem -> the item's use -> the spell cast (its targets) ->
    // CMSG_USE_ITEM in AzerothCore's field order (psp_spell_targets.hpp); CMSG_OPEN_ITEM (clams, lockboxes, gifts ->
    // the loot window); an item that starts a quest = CMSG_QUESTGIVER_QUERY_QUEST naming the item; item cooldowns
    // (an own SMSG_SPELL_GO of an item cast starts the template's cooldown / category cooldown, SMSG_ITEM_COOLDOWN =
    // the 30 s equip cooldown, SpellCategory.dbc flag 4 / SPELL_ATTR0_COOLDOWN_ON_EVENT hold it until
    // SMSG_COOLDOWN_EVENT); SMSG_STANDSTATE_UPDATE (the realm sits us down for food and drink, upstream
    // standStateCallback).
public:
    enum class ItemUse : uint8_t {
        None,      // nothing happens (no use spell, a charter, gift paper)
        Sent,      // CMSG_USE_ITEM
        Equip,     // UseContainerItem equipped it (CMSG_AUTOEQUIP_ITEM)
        Opened,    // CMSG_OPEN_ITEM
        Quest,     // CMSG_QUESTGIVER_QUERY_QUEST (the quest window follows)
        Confirm,   // USE_BIND_CONFIRM: the popup decides (confirmUseBind / cancelUseBind)
        PickItem,  // the spell waits for an item (SpellIsTargeting): useOnItem with the next item clicked
        Readable,  // a readable item (ItemTextFrame: not on the PSP yet)
        NoInfo,    // the item query is not answered yet (asked)
        Error,     // refused here: the red line (takeSpellError)
        Glyph,     // glyphs, guild bank, channels: USE_GLYPH - the cast waits for a socket of GlyphFrame (placeGlyph)
    };
    // UseContainerItem (bag 0 = the backpack, 1..4 = a worn bag; slot from 0) after the window branches the screen
    // owns: a quest is offered, a readable item read, an equippable item equipped, a lootable one opened, the rest
    // used (upstream lua_UseContainerItem).
    ItemUse useContainerItem(int bag, int slot, uint64_t unitTarget = 0);
    // The use of the item at a wire position (upstream dispatchUseItem; bag 0xFF: the worn slots 0..18 / the
    // backpack 23..38; 19..22 a worn bag). unitTarget: the unit the caller names (0 = the selection).
    ItemUse useItemAt(uint8_t wireBag, uint8_t wireSlot, uint64_t unitTarget = 0, bool confirmed = false);
    // UseAction on an item button (upstream useItemById): the first one in the backpack, the bags, the worn slots -
    // used. None: not carried.
    ItemUse useItemByEntry(uint32_t entry, uint64_t unitTarget = 0);
    // upstream getItemIdForSpell + useItemById: a SPELL button whose spell is an item's ON_USE spell (upstream's
    // default Hearthstone 8690 on button 12) is that item's use. False: no such item carried.
    bool useItemForSpell(uint32_t spellId);
    bool itemUseSpellCarried(uint32_t spellId) const;  // such an item is in the backpack / bags
    // CMSG_CAST_SPELL with explicit targets and the session's cast counter
    // (gameobject-use's gathering / opening casts: castSpellAt(spell, SpellCastTargets::gameObject(guid))).
    void castSpellAt(uint32_t spellId, const SpellCastTargets& targets);
    // StaticPopup USE_BIND: ConfirmBindOnUse / the cancel.
    bool hasPendingUseConfirm() const { return pendingUse_.active; }
    ItemUse confirmUseBind();
    void cancelUseBind() { pendingUse_ = PendingItemUse{}; }
    // SpellIsTargeting with an item spell (stones, oils, enchant scrolls): the next item clicked is the target.
    bool isItemTargeting() const { return itemTargeting_.active; }
    uint32_t itemTargetingSpell() const { return itemTargeting_.spellId; }
    uint32_t itemTargetingEntry() const { return itemTargeting_.entry; }
    ItemUse useOnItem(uint8_t wireBag, uint8_t wireSlot);  // SpellTargetItem
    void cancelItemTargeting() { itemTargeting_ = PendingItemUse{}; }  // SpellStopTargeting
    // GetItemCooldown: the item's use spell and the item's spell category.
    // onHold = the cooldown starts when combat ends (COOLDOWN_ON_LEAVE_COMBAT, GetActionCooldown enable 0).
    bool itemCooldown(uint32_t entry, float& remaining, float& total, bool& onHold) const;
    uint32_t itemUseSpell(uint32_t entry) const;  // the first ON_USE (trigger 0) spell, 0 = none / unknown
    uint32_t itemCount(uint32_t entry) const;     // GetActionCount: worn + backpack + bags
    bool itemEquipped(uint32_t entry) const;      // IsEquippedAction
    bool itemEquippable(uint32_t entry) const;    // InventoryType != 0, not a quest item (upstream)
    bool itemConsumable(uint32_t entry) const;    // IsConsumableAction: a consumable-class item (upstream)
    bool itemStackable(uint32_t entry) const;     // IsStackableAction: MaxCount > 1
    uint32_t itemUseVersion() const { return itemUseVersion_; }  // bumped by every item cooldown change
    // SMSG_STANDSTATE_UPDATE, oldest first (the world screen sits / stands without echoing it back).
    bool takeStandState(uint8_t& state);
    // USE_BIND_CONFIRM for the screen (the popup), taken once.
    bool takeUseBindPrompt() { const bool v = useBindPrompt_; useBindPrompt_ = false; return v; }

private:
    struct PendingItemUse {
        bool active = false;
        uint8_t bag = 0xFF, slot = 0;
        uint64_t itemGuid = 0, unitTarget = 0;
        uint32_t entry = 0, spellId = 0;
    };
    struct ItemCooldown {  // an item spell's cooldown
        uint32_t spellId = 0, entry = 0, category = 0;
        uint32_t startMs = 0, durationMs = 0, catStartMs = 0, catDurationMs = 0;
        bool onHold = false;
    };
    bool handleItemUsePacket(game::LogicalOpcode op, network::Packet& packet);
    bool itemAtWire(uint8_t bag, uint8_t slot, game::ItemDef& def, uint64_t& guid) const;
    ItemUse castItemSpell(const PendingItemUse& use, const game::ItemQueryResponseData& info);
    void sendUseItem(const PendingItemUse& use, const SpellCastTargets& targets);
    void startItemCooldown(uint32_t entry, uint32_t spellId);
    void addItemCooldown(const ItemCooldown& cd);
    int itemSpellIndex(const game::ItemQueryResponseData& info, uint32_t spellId) const;
    bool unitFitsTargetRequest(uint64_t guid, uint32_t mask) const;
    void itemUseError(const char* text);
    PendingItemUse pendingUse_, itemTargeting_, lastItemCast_;
    uint32_t lastItemCastMs_ = 0;
    std::vector<ItemCooldown> itemCooldowns_;
    std::deque<uint8_t> standStates_;
    bool useBindPrompt_ = false;
    uint32_t itemUseVersion_ = 0, itemUseLogs_ = 0;
    // item use (end)
    // social (psp_world_session_social.cpp): friends, ignore, who and the guild
    // (packets: psp_social_parse.hpp) - upstream SocialHandler's friend/who/guild half
    // without GameHandler. PSP: the lines are GlobalStrings keys (the running language's strings.pak), not upstream's
    // English ones; no voice (mutes), no account-wide friends.
public:
    // --- Friends / ignore (GetNumFriends / GetFriendInfo / GetNumIgnores / GetIgnoreName) ---
    struct FriendInfo {
        uint64_t guid = 0;
        std::string note;
        uint8_t status = 0;        // FriendStatus: != 0 online; 2 AFK, 4 DND
        uint32_t area = 0, level = 0, cls = 0;
        bool online() const { return status != 0; }
    };
    // The port's order: online first, then by name. Names: socialName(guid).
    const std::vector<FriendInfo>& friends() const { return friends_; }
    const std::vector<uint64_t>& ignores() const { return ignores_; }  // by name
    uint32_t friendsVersion() const { return friendsVersion_; }  // FRIENDLIST_UPDATE
    uint32_t ignoresVersion() const { return ignoresVersion_; }  // IGNORELIST_UPDATE
    // The name cache's name of a contact / guild member ("" while its CMSG_NAME_QUERY is out).
    std::string socialName(uint64_t guid) const;
    // The gender (0 male, 1 female, 0xFF unknown) of a name query answer (GetFriendInfo's class name for the gender).
    uint8_t socialGender(uint64_t guid) const;
    void showFriends();                                         // ShowFriends: CMSG_CONTACT_LIST (7)
    void addFriend(const std::string& name, const std::string& note = std::string());  // AddFriend
    void removeFriend(const std::string& name);                 // RemoveFriend(name)
    void setFriendNotes(const std::string& name, const std::string& note);  // SetFriendNotes(name, note)
    void addOrRemoveFriend(const std::string& name, const std::string& note);  // AddOrRemoveFriend (/friend)
    void addIgnore(const std::string& name);                    // AddIgnore
    void delIgnore(const std::string& name);                    // DelIgnore
    void addOrDelIgnore(const std::string& name);               // AddOrDelIgnore (/ignore)
    const FriendInfo* friendByName(const std::string& name) const;
    bool isIgnored(const std::string& name) const;
    // --- Who (SendWho / GetNumWhoResults / GetWhoInfo / SetWhoToUI / SortWho) ---
    using WhoRec = social::WhoRec;
    // The names the who tags search (AreaTable zones without a parent, ChrRaces / ChrClasses): set by the world
    // screen (it has text.pak / glue.pak); without them a tag matches nothing.
    void setWhoLookup(social::WhoLookup lookup) { whoLookup_ = std::move(lookup); }
    bool hasWhoLookup() const { return static_cast<bool>(whoLookup_.zones); }
    void sendWho(const std::string& text);                      // SendWho (social::parseWhoText)
    void setWhoToUi(bool on) { whoToUi_ = on; }                 // SetWhoToUI (WhoFrame OnShow 1 / OnHide 0)
    const std::vector<WhoRec>& whoList() const { return who_; }
    uint32_t whoTotal() const { return whoTotal_; }
    uint32_t whoVersion() const { return whoVersion_; }        // WHO_LIST_UPDATE (the frame opens on the Who tab)
    // The results of a /who for the chat (SetWhoToUI off: all of them): the world screen prints them with its
    // names (WHO_LIST_FORMAT / WHO_LIST_GUILD_FORMAT, WHO_NUM_RESULTS) - taken once.
    bool takeWhoChat(std::vector<WhoRec>& out, uint32_t& count);
    void sortWho(int key);                                      // SortWho: 0 zone, 1 level, 2 class, 3 group, 4 name,
                                                                // 5 race, 6 guild
    // --- Guild ---
    using GuildMember = social::RosterMember;
    using GuildRank = social::RosterRank;
    bool isInGuild() const { return guildId() != 0; }          // IsInGuild: PLAYER_GUILDID
    uint32_t guildId() const;                                   // PLAYER_GUILDID (UNIT_END + 3)
    uint32_t guildRankIndex() const;                            // PLAYER_GUILDRANK (UNIT_END + 4)
    bool isGuildLeader() const { return isInGuild() && guildRankIndex() == 0; }  // IsGuildLeader
    uint32_t guildRights() const;                               // rights[player rank] (the Can* functions)
    bool guildCan(uint32_t right) const { return isInGuild() && (guildRights() & right) != 0; }
    const std::vector<GuildMember>& guildRoster() const { return roster_.members; }  // sorted (SortGuildRoster)
    uint32_t guildMemberCount() const { return roster_.memberCount; }  // the roster's count (all of them)
    uint32_t guildOnlineCount() const { return roster_.online; }
    uint32_t guildNumRanks();                                   // GuildControlGetNumRanks (the guild query)
    const GuildRank& guildRank(uint32_t rank) const;            // the roster / MSG_GUILD_PERMISSIONS rights
    uint8_t guildBankTabs() const { return guildTabs_; }       // GetNumGuildBankTabs (MSG_GUILD_PERMISSIONS)
    const std::string& guildMotd() const { return roster_.motd; }  // CURRENT_GUILD_MOTD
    const std::string& guildInfoText() const { return roster_.info; }  // GetGuildInfoText
    uint32_t guildVersion() const { return guildVersion_; }    // GUILD_ROSTER_UPDATE / PLAYER_GUILD_UPDATE / GUILD_MOTD
    bool guildShowOffline() const { return guildShowOffline_; }  // CVar guildShowOffline (default 1)
    void setGuildShowOffline(bool on);                          // SetGuildRosterShowOffline
    void sortGuildRoster(int key);                              // SortGuildRoster: 0 rank, 1 level, 2 name, 3 zone,
                                                                // 4 class, 5 group, 6 online, 7 note
    void guildRosterRequest();                                  // GuildRoster: CMSG_GUILD_ROSTER, once per 5 s
    void guildInvite(const std::string& name);                  // GuildInvite
    void guildUninvite(const std::string& name);                // GuildUninvite
    void guildPromote(const std::string& name);                 // GuildPromote
    void guildDemote(const std::string& name);                  // GuildDemote
    void guildSetLeader(const std::string& name);               // GuildSetLeader
    void guildSetMotd(const std::string& text);                 // GuildSetMOTD
    void guildSetInfoText(const std::string& text);             // SetGuildInfoText
    void guildSetPublicNote(const std::string& name, const std::string& note);   // GuildRosterSetPublicNote
    void guildSetOfficerNote(const std::string& name, const std::string& note);  // GuildRosterSetOfficerNote
    void guildLeave();                                          // GuildLeave
    void guildDisband();                                        // GuildDisband
    void guildInfoRequest();                                    // GuildInfo: CMSG_GUILD_INFO (the chat lines)
    // GuildControlSaveRank: CMSG_GUILD_RANK (rank, rights, name <= 15, gold per day x 10000, 6 tab flags / slots).
    void guildSaveRank(uint32_t rank, uint32_t rights, const std::string& name, uint32_t goldPerDay,
                       const uint32_t tabFlags[6], const uint32_t tabSlots[6]);
    void guildAddRank(const std::string& name);                 // GuildControlAddRank (below 10 ranks)
    void guildDelRank();                                        // GuildControlDelRank (above 5 ranks)
    void guildEventLogQuery();                                  // QueryGuildEventLog
    const std::vector<social::EventLogEntry>& guildEventLog() const { return eventLog_; }
    uint32_t guildEventLogVersion() const { return eventLogVersion_; }  // GUILD_EVENT_LOG_UPDATE
    uint32_t guildEventLogAtMs() const { return eventLogAtMs_; }  // when it came (the entries' age)
    // GUILD_INVITE_REQUEST (SMSG_GUILD_INVITE) -> StaticPopup GUILD_INVITE; AcceptGuild / DeclineGuild.
    uint32_t guildInviteSeq() const { return guildInviteSeq_; }
    const std::string& guildInviter() const { return guildInviter_; }
    const std::string& guildInviteName() const { return guildInviteGuild_; }
    void acceptGuild();
    void declineGuild();
    // An invite not answered yet (AcceptGuild / DeclineGuild clear it) - a world screen made after a map
    // transfer shows it again instead of replaying guildInviteSeq (as hasPendingGroupInvite).
    bool hasPendingGuildInvite() const { return guildInvitePending_; }
    // The guild cache (SMSG_GUILD_QUERY_RESPONSE): nullptr while unknown - asked (CMSG_GUILD_QUERY), again after 5 s.
    const social::GuildQuery* guildQuery(uint32_t guildId);
    const social::GuildQuery* ownGuild() { return guildQuery(guildId()); }  // GetGuildInfo("player")
    uint32_t guildCacheVersion() const { return guildCacheVersion_; }  // an answer came
    // CVar guildMemberNotify (default "0": the guild sign-on / off lines are not shown) - PSP: wowee.cfg guildnotify.
    void setGuildMemberNotify(bool on) { guildNotify_ = on; }
    // The AFK / DND switch of FriendsFrameStatusDropDown (SendChatMessage("", "AFK" / "DND")); /afk, /dnd with a text.
    void sendAwayFlag(bool dnd, const std::string& text = std::string());
    uint32_t friendSoundSeq() const { return friendSoundSeq_; }  // FRIENDJOINGAME (a friend / member came online)
    void socialUpdate();  // every frame (from chatUpdate): names that came, the login roster, pending lines

private:
    bool handleSocialPacket(game::LogicalOpcode op, network::Packet& packet);
    void socialLine(const std::string& text);                   // news: a system chat line (GameLine)
    void socialError(const std::string& text);                  // a refusal: the red error line (GameLine)
    bool guildRosterSend();                                     // CMSG_GUILD_ROSTER unless one went out < 5 s ago
    // A line with the name (after its query).
    void friendStatusLine(uint64_t guid, const char* fmt, int nameArgs, GameLine kind = GameLine::News);
    void sortFriends();
    void sortIgnores();
    void sortRoster();
    void sortWhoList();
    void askName(uint64_t guid);
    void sendGuildPermissionsQuery();                           // MSG_GUILD_PERMISSIONS (a guild / rank change)
    void guildCommandByName(game::LogicalOpcode op, const std::string& name);
    std::vector<FriendInfo> friends_;
    std::vector<uint64_t> ignores_;
    uint32_t friendsVersion_ = 0, ignoresVersion_ = 0;
    struct PendingLine {                                        // a friend line waiting for the name
        uint64_t guid = 0;
        const char* fmt = nullptr;
        int nameArgs = 1;
        uint32_t deadlineMs = 0;
        GameLine kind = GameLine::News;
    };
    std::vector<PendingLine> pendingLines_;
    bool namesPending_ = false;                                 // FRIENDLIST / IGNORELIST_UPDATE after the names
    std::unordered_map<uint64_t, uint8_t> socialGenders_;       // name query answers (peeked)
    std::unordered_set<uint64_t> socialNameAsked_;
    social::WhoLookup whoLookup_;
    std::vector<WhoRec> who_;
    uint32_t whoTotal_ = 0, whoVersion_ = 0, whoChatCount_ = 0;
    bool whoToUi_ = false, whoChatPending_ = false;
    std::vector<WhoRec> whoChat_;
    uint8_t whoSortKey_ = 4;                                    // SortWho's key (by name at first)
    bool whoDesc_ = false;
    social::Roster roster_;
    bool rosterKnown_ = false;
    uint8_t rosterSortKey_ = 2;                                 // SortGuildRoster's key (by name at first)
    bool rosterDesc_ = false;
    bool guildShowOffline_ = true, guildNotify_ = false;
    uint32_t rosterSentMs_ = 0;                                // the last CMSG_GUILD_ROSTER (0 = none yet)
    bool rosterWanted_ = false;                                 // the session's own roster request is due
    uint32_t guildVersion_ = 0, lastGuildId_ = 0, lastGuildRank_ = 0xFFFFFFFFu;
    uint8_t guildTabs_ = 0;
    uint32_t worldEnterMs_ = 0;                                // AnimTimerFrame's GuildRoster() 15 s after the UI
    bool loginRosterDone_ = false;
    std::vector<social::EventLogEntry> eventLog_;
    uint32_t eventLogVersion_ = 0, eventLogAtMs_ = 0;
    uint32_t guildInviteSeq_ = 0;
    std::string guildInviter_, guildInviteGuild_;
    bool guildInvitePending_ = false;
    struct GuildCacheEntry {
        social::GuildQuery q;
        uint32_t lastUse = 0;
    };
    std::vector<GuildCacheEntry> guildCache_;                   // <= 32 guilds (PSP), least used out
    std::unordered_map<uint32_t, uint32_t> guildAskedMs_;       // CMSG_GUILD_QUERY sent (retry after 5 s)
    uint32_t guildCacheVersion_ = 0, guildCacheTick_ = 0;
    uint32_t socialLogs_ = 0, friendSoundSeq_ = 0;
    // social (end)
    // trade and duels: the trade (TradeFrame), the duels and the requests the
    // player answers (TRADE, CONFIRM_SUMMON, QUEST_ACCEPT, shared quests) - upstream's flow and the port's own rules
    // (psp_world_session_trade_duel.cpp; the packets in psp_trade_duel_parse.hpp). The world screen polls the
    // state below (the *Seq counters change with every event the original's frames listen to).
public:
    // --- Trade (InitiateTrade, ClickTradeButton, SetTradeMoney, AcceptTrade, CancelTradeAccept, CloseTrade) ---
    struct TradeMine {   // our offer's slot: the bag item we put up (shown locked while on the table)
        uint64_t item = 0;  // 0 = empty
        uint8_t bag = 0, slot = 0;
    };
    void initiateTrade(uint64_t guid);
    bool tradeAsked() const { return tradeAsked_; }           // TRADE_REQUEST: somebody asks us (the TRADE popup)
    uint32_t tradeAskSeq() const { return tradeAskSeq_; }
    const std::string& tradeAskerName();
    void tradeAnswerRequest(bool accept);                    // BeginTrade / CancelTrade
    bool tradeOpen() const { return tradeWindow_; }
    uint64_t tradePeer() const { return tradePeer_; }
    uint32_t tradeSeq() const { return tradeSeq_; }              // TRADE_SHOW / TRADE_CLOSED
    uint32_t tradeUpdateSeq() const { return tradeUpdateSeq_; }  // TRADE_*_ITEM_CHANGED / *_MONEY
    uint32_t tradeAcceptSeq() const { return tradeAcceptSeq_; }  // TRADE_ACCEPT_UPDATE
    bool tradeCompleted() const { return tradeCompleted_; }      // the last close was TRADE_COMPLETE
    const tradeduel::TradeSlotRecord& tradeSlot(int side, int slot) const;  // the server's view (side 1: theirs)
    const TradeMine& tradeMine(int slot) const;
    uint32_t tradeGold(int side) const { return side ? tradeGold_[1] : tradeGold_[0]; }
    uint32_t tradeSpell(int side) const { return side ? tradeSpell_[1] : tradeSpell_[0]; }
    bool tradeAccepted(int side) const { return side ? tradePeerAccepted_ : tradeSelfAccepted_; }
    bool tradeSetItem(int tradeSlot, uint8_t bag, uint8_t slot);  // ClickTradeButton with a bag item held
    bool tradeAddItem(uint8_t bag, uint8_t slot);                 // UseContainerItem while trading
    void tradeClearItem(int tradeSlot);                           // ClickTradeButton(i, right) - off the table
    bool tradeSetMoney(uint32_t copper);                          // SetTradeMoney
    void tradeAccept();
    void tradeCancelAccept();
    void tradeClose();
    bool tradeItemLocked(uint8_t bag, uint8_t slot) const;        // the bag item is on the trade table
    bool tradeMineItem(int slot, game::ItemDef& out, uint8_t* bag = nullptr,
                       uint8_t* wireSlot = nullptr) const;  // our slot's item, wherever it is now (+ its place)
    bool tradeBindAsk() const { return tradeBindAsk_; }           // TRADE_POTENTIAL_BIND_ENCHANT(canBecomeBound)
    uint32_t tradeBindSeq() const { return tradeBindSeq_; }
    // --- Duels (StartDuel, AcceptDuel, CancelDuel) ---
    void startDuel(uint64_t guid);
    void acceptDuel();
    void cancelDuel();
    bool duelRequested() const { return duel_.requested; }       // DUEL_REQUESTED (the popup)
    uint32_t duelRequestSeq() const { return duel_.requestSeq; }
    const std::string& duelChallengerName() {
        if (duel_.challengerName.empty()) duel_.challengerName = tdNameOf(duel_.challenger);
        return duel_.challengerName;
    }
    bool duelOutOfBounds() const { return duel_.outOfBounds; }   // DUEL_OUTOFBOUNDS / DUEL_INBOUNDS
    uint32_t duelBoundsSeq() const { return duel_.boundsSeq; }
    uint32_t duelFinishedSeq() const { return duel_.finishedSeq; }  // DUEL_FINISHED
    bool duelOpponent(uint64_t guid) const;  // our running duel's opponent (hostile: tradeduel::duelHostile)
    // --- Summons (CONFIRM_SUMMON / CANCEL_SUMMON) ---
    bool summonPending() const { return summon_.pending; }
    uint32_t summonSeq() const { return summon_.seq; }
    uint32_t summonZone() const { return summon_.zoneId; }
    const std::string& summonerName();
    int summonTimeLeftSec() const;
    bool summonCanAccept() const;  // not in combat (CONFIRM_SUMMON's accept button)
    void confirmSummon();
    void cancelSummon();
    // --- Quests: QuestLogPushQuest, MSG_QUEST_PUSH_RESULT, QUEST_ACCEPT_CONFIRM ---
    bool questPushable(uint32_t questId) const;  // GetQuestLogPushable (the quest query's QUEST_FLAGS_SHARABLE)
    void pushQuestToParty(uint32_t questId);
    void questPushDecline();  // QuestFrame "Decline" on a quest a player offers (call before declineQuest)
    struct QuestConfirm {
        uint32_t questId = 0;
        std::string title, name;
        uint64_t starter = 0;
        bool pending = false;
        uint32_t seq = 0;
    };
    const QuestConfirm& questConfirm() const { return questConfirm_; }
    const std::string& questConfirmName();
    bool questLogFull() const;  // MAX_QUESTS (QUEST_ACCEPT_LOG_FULL)
    void confirmAcceptQuest();
    void dismissQuestConfirm();
    bool takeTradeDuelInfo(std::string& out);  // the yellow UIErrorsFrame line: the duel countdown
    void tradeDuelTick();                       // every frame: the duel countdown, the lines waiting for a name

private:
    bool handleTradeDuelPacket(game::LogicalOpcode op, network::Packet& packet);
    void tradeStatus(const tradeduel::TradeStatusRead& s);
    void tradeExtended(const tradeduel::TradeExtendedRead& x);
    void tradeUnlockAll();
    void tradeCloseWindow(bool completed, bool cancelServer);
    void tradeForgetRequest();
    std::string tdNameOrQuery(uint64_t guid);
    void tradeClearLocal(uint8_t slot);
    bool tradePlace(int tradeSlot, uint8_t bag, uint8_t slot, uint64_t itemGuid);
    const char* tradeItemRefusal(uint8_t bag, uint8_t slot, int tradeSlot, const game::ItemDef& item,
                                 uint64_t itemGuid) const;
    void sendTradeEmpty(game::LogicalOpcode op);
    uint64_t wireItemGuid(uint8_t bag, uint8_t slot) const;
    uint32_t itemInstanceFlags(uint64_t itemGuid) const;
    void sendDuelAnswer(bool accept);
    void tdLine(const char* key, const std::string& name);
    void tdLineNamed(const char* key, uint64_t guid);
    std::string tdNameOf(uint64_t guid) const;
    struct TdAwait {  // a line waiting for its name
        const char* key;
        uint64_t guid;
        uint32_t atMs;
    };
    struct DuelState {
        uint64_t arbiter = 0, challenger = 0;
        std::string challengerName;
        bool requested = false, outOfBounds = false;
        uint32_t requestSeq = 0, boundsSeq = 0, finishedSeq = 0;
    };
    struct SummonState {
        uint64_t summoner = 0;
        uint32_t zoneId = 0, expiresMs = 0, seq = 0;
        std::string name;
        bool pending = false;
    };
    // the trade request (asking / asked, its player) and the open trade (id, gold, spells,
    // the two sides' records; our bag items on the table, the acceptances)
    bool tradeAsking_ = false, tradeAsked_ = false, tradeWindow_ = false, tradeCompleted_ = false;
    bool tradeSelfAccepted_ = false, tradePeerAccepted_ = false, tradeBindAsk_ = false;
    uint64_t tradeRequestGuid_ = 0, tradePeer_ = 0;
    uint32_t tradeId_ = 0, tradeAskMs_ = 0, tradeAskSeq_ = 0;
    std::string tradeAskerName_;
    uint32_t tradeGold_[2] = {}, tradeSpell_[2] = {};
    std::array<std::array<tradeduel::TradeSlotRecord, tradeduel::kTradeSlots>, 2> tradeView_{};
    std::array<TradeMine, tradeduel::kTradeSlots> tradeMine_{};
    uint32_t tradeSeq_ = 0, tradeUpdateSeq_ = 0, tradeAcceptSeq_ = 0, tradeBindSeq_ = 0;
    DuelState duel_;
    uint32_t duelCountdown_ = 0, duelCountdownNextMs_ = 0;
    SummonState summon_;
    QuestConfirm questConfirm_;
    std::unordered_map<uint32_t, uint32_t> questShareFlags_;  // quest -> QuestFlags (SMSG_QUEST_QUERY_RESPONSE)
    std::deque<std::string> tdInfo_;
    std::vector<TdAwait> tdAwait_;
    uint32_t tdLogs_ = 0;
    // trade and duels (end)
    // pets (psp_world_session_pets.cpp): the pet, the stable and the pet talents (upstream's pet half, AzerothCore's
    // layouts; the rules in the .cpp): SMSG_PET_SPELLS
    // (the bar, the spells, the cooldowns), SMSG_PET_MODE, _ACTION_FEEDBACK, _BROKEN, _RENAMEABLE,
    // _LEARNED_SPELL / _UNLEARNED_SPELL, _GUIDS, _CAST_FAILED, _TAME_FAILURE, _NAME_QUERY_RESPONSE, _NAME_INVALID,
    // the pet's cooldowns (SMSG_SPELL_COOLDOWN / SMSG_COOLDOWN_EVENT for its guid), the pet talents
    // (SMSG_TALENTS_INFO type 1), the stable (MSG_LIST_STABLED_PETS, SMSG_STABLE_RESULT) and the senders - CMSG_PET_ACTION
    // / _SET_ACTION / _SPELL_AUTOCAST / _ABANDON / _RENAME / _STOP_ATTACK / _NAME_QUERY / _LEARN_TALENT, CMSG_STABLE_PET /
    // _UNSTABLE_PET / _STABLE_SWAP_PET / _BUY_STABLE_SLOT, MSG_LIST_STABLED_PETS. Lines: GlobalStrings keys, failures
    // the red error line. Names follow the Lua API the frames call.
public:
    static constexpr int kPetSlots = 10;  // NUM_PET_ACTION_SLOTS
    struct PetEvent {                     // the Lua events the frames listen to
        enum Kind : uint8_t {
            Renameable,      // PET_RENAMEABLE (SMSG_PET_RENAMEABLE): StaticPopup RENAME_PET
            DismissStart,    // PET_DISMISS_START
            UiClose,         // PET_UI_CLOSE (a new hunter pet)
            StableShow,      // PET_STABLE_SHOW (MSG_LIST_STABLED_PETS)
            StableUpdate,    // PET_STABLE_UPDATE
            StableClosed,    // PET_STABLE_CLOSED (ClosePetStables)
            TalentsReset,    // TALENTS_INVOLUNTARILY_RESET (pet): StaticPopup TALENTS_INVOLUNTARILY_RESET_PET
            ForceDeclension, // PET_FORCE_NAME_DECLENSION (SMSG_PET_NAME_INVALID with declined names)
        };
        Kind kind = Renameable;
        uint32_t arg = 0;
    };
    struct StablePet {  // one stabled pet (0 = the active pet)
        uint32_t petNumber = 0, level = 0, entry = 0;
        std::string name;
        uint8_t flag = 0;  // 1 the active pet, 2 in a stable slot
    };
    uint64_t petGuid() const { return petGuids_.empty() ? 0 : petGuids_[0]; }  // GetPetGUID / UnitGUID("pet")
    std::shared_ptr<game::Entity> petUnit() const;  // the pet's entity while it is in sight (UnitExists("pet"))
    uint32_t petMode() const { return petMode_; }  // react | command << 8 | flags << 16
    uint8_t petReact() const { return (petMode_ & 0x8000000u) ? 0 : static_cast<uint8_t>(petMode_ & 0xFF); }
    bool petAttacking() const { return petAttacking_; }  // PET_ATTACK_START / _STOP
    uint32_t petActionSlot(int i) const { return i >= 0 && i < kPetSlots ? petBar_[i] : 0; }
    const std::vector<uint32_t>& petSpellList() const { return petSpellList_; }  // packed, as SMSG_PET_SPELLS sent them
    uint16_t petFamily() const { return petFamily_; }  // SMSG_PET_SPELLS' creature family (0 = none sent)
    uint32_t petVersion() const { return petVersion_; }  // bumped by every PET_BAR_UPDATE / UNIT_PET / mode change
    const std::string& petName() const { return petName_; }  // the name query's answer ("" = not known yet)
    bool petHasActionBar() const;          // PetHasActionBar
    bool hasPetUI(bool* hunterPet = nullptr) const;  // HasPetUI
    bool petIsHunterPet() const;           // a hunter with a pet
    bool petCanBeRenamed() const;          // PetCanBeRenamed (UNIT_FIELD_BYTES_2 byte 2 & 1)
    bool petCanBeAbandoned() const;        // PetCanBeAbandoned (& 2)
    bool petCanBeDismissed() const;        // PetCanBeDismissed
    bool petHappiness(int& level, int& damagePercent) const;  // GetPetHappiness (hunter pets)
    bool petExperience(uint32_t& cur, uint32_t& next) const;  // GetPetExperience
    bool petSlotUsable(int slot) const;    // GetPetActionSlotUsable (see the .cpp)
    float petSpellCooldown(uint32_t spellId, float* total = nullptr) const;  // GetPetActionCooldown
    // CastPetAction(index[, unit]): a spell, a reaction or a command of the bar; target 0 =
    // the player's target. PetAttack / PetFollow / PetWait / PetPassiveMode... = petAction(packed, target).
    void castPetAction(int slot, uint64_t target = 0);
    void petAction(uint32_t packed, uint64_t target, bool targetGiven = false);
    void petStopAttack();                  // PetStopAttack (CMSG_PET_STOP_ATTACK while attacking)
    void togglePetAutocast(int slot);      // TogglePetAutocast (CMSG_PET_SPELL_AUTOCAST, bit 30 flipped)
    // PickupPetAction / a drop on a pet button. PSP: the cursor is the screen's (one pet action):
    // petPickup returns what goes on the cursor (0 = nothing; the bar is unchanged); petPlace swaps it with
    // `slot` (CMSG_PET_SET_ACTION) and returns what stays on the cursor (0 = none, `data` = refused).
    uint32_t petPickup(int slot);
    uint32_t petPlace(int slot, uint32_t data);
    void petDismiss();                     // PetDismiss (CMSG_PET_ACTION 0x07000003)
    void petAbandon();                     // PetAbandon (CMSG_PET_ABANDON)
    void petRename(const std::string& name);  // PetRename (CMSG_PET_RENAME without declension)
    bool takePetEvent(PetEvent& out);
    // The pet talents (GetTalentTabInfo(.., isPet), LearnTalent(tab, talent, true)): the one tab of the pet's
    // CreatureFamily.PetTalentType (spells.pak "talents", PetTalentMask), the points of SMSG_TALENTS_INFO type 1.
    bool loadPetTalents();                 // false: no hunter pet with a talent tree / no data
    const game::TalentTabEntry* petTalentTab() const { return petTalentTab_.tabId ? &petTalentTab_ : nullptr; }
    const std::vector<game::TalentEntry>& petTalents() const { return petTalentList_; }
    const game::TalentEntry* petTalentEntry(uint32_t talentId) const;
    uint8_t petTalentRank(uint32_t talentId) const;  // 1-based, 0 = not learned
    const std::unordered_map<uint32_t, uint8_t>& petLearnedTalents() const { return petTalents_; }
    uint32_t petUnspentTalents() const { return petTalentPoints_; }
    uint32_t petTalentVersion() const { return petTalentVersion_; }  // PET_TALENT_UPDATE
    void learnPetTalent(uint32_t talentId);  // CMSG_PET_LEARN_TALENT (pet guid, talent, the next rank from 0)
    // text.pak "pet_text" (psp_pet_text.hpp), read on first use, kept (~3 KB).
    const PetTextData& petText();
    // The creature family of an entry (the creature cache; asked for when missing - the stable's pets). 0 = not known.
    uint32_t creatureFamilyOf(uint32_t entry);
    // The stable (PetStableFrame): the window is the server's MSG_LIST_STABLED_PETS answer.
    uint64_t stableMaster() const { return stableMaster_; }  // IsAtStableMaster
    uint32_t stableSlots() const { return stableSlots_; }    // GetNumStableSlots
    const StablePet& stableCurrent() const { return stableCur_; }  // record 0 (petNumber 0 = none)
    const std::vector<StablePet>& stabledPets() const { return stabled_; }  // GetNumStablePets / records 1..
    int stableSelected() const;            // GetSelectedStablePet - 1: -2 none, -1 the current pet, k the k-th stabled
    void stableSelect(int index);          // kept by pet number
    // ClickStablePet(index + 1) with the PSP's cursor (pickup = PickupStablePet): true = the selection
    // changed (no stable pet on the cursor), false = a drop (stable / unstable / swap sent) or nothing.
    bool stableClick(int index, int* cursor);
    uint32_t stableNextSlotCost();         // GetNextStableSlotCost (StableSlotPrices numSlots + 1)
    bool stableCanBuySlot();               // BuyStableSlot's checks
    void stableBuySlot();                  // CMSG_BUY_STABLE_SLOT
    void stableClose();                    // ClosePetStables
    uint32_t stableVersion() const { return stableVersion_; }
    // Per frame: the stable master out of reach closes the stable (NPC_INTERACT_MAX_DISTANCE).
    void petsUpdate(float dtSec, const glm::vec3& selfPos);

private:
    bool handlePetPacket(game::LogicalOpcode op, network::Packet& packet);
    void handlePetSpells(network::Packet& packet);
    void handleListStabledPets(network::Packet& packet);
    void petSetGuid(uint64_t guid);         // the new pet: UNIT_PET
    void petBarUpdate();                    // PET_BAR_UPDATE
    void petSendSetAction(int other, int slot);  // CMSG_PET_SET_ACTION: (other, bar[other]) first, then (slot, bar[slot])
    void petQueryName();
    void petLine(const std::string& text, GameLine kind);  // a game line (GameLine)
    void petCooldown(uint32_t spellId, uint32_t ms);
    void stableRequestList();               // MSG_LIST_STABLED_PETS (stable master guid)
    std::vector<uint64_t> petGuids_;        // SMSG_PET_GUIDS; [0] = the pet
    uint32_t petBar_[kPetSlots] = {};
    std::vector<uint32_t> petSpellList_;
    uint32_t petMode_ = 0, petDurationMs_ = 0, petVersion_ = 0;
    bool petNameWanted_ = false;            // CMSG_PET_NAME_QUERY to send (petQueryName)
    int petPickFrom_ = -1;                  // the slot PickupPetAction took the cursor's action from
    uint16_t petFamily_ = 0;
    bool petAttacking_ = false;
    std::string petName_;
    std::deque<PetEvent> petEvents_;
    std::unordered_map<uint32_t, std::pair<uint32_t, float>> petCooldowns_;  // spell -> logUptimeMs at the start, length (s)
    std::unordered_map<uint32_t, uint8_t> petTalents_;  // talent -> rank (1-based)
    uint32_t petTalentPoints_ = 0, petTalentVersion_ = 0;
    game::TalentTabEntry petTalentTab_;
    std::vector<game::TalentEntry> petTalentList_;
    int32_t petTalentType_ = -2;  // the type the tab was loaded for (-2 = not loaded)
    PetTextData petText_;
    bool petTextTried_ = false;
    uint64_t stableMaster_ = 0;
    uint32_t stableSlots_ = 0, stableVersion_ = 0;
    int64_t stableSelNumber_ = 0;  // -1 the current pet, else a pet number, 0 none
    StablePet stableCur_;
    std::vector<StablePet> stabled_;
    uint32_t petLogs_ = 0;
    // pets (end)
    // class bars (psp_world_session_classbars.cpp): the class bars and the vehicles (upstream WoWee, AzerothCore's
    // handlers and the port's own rules - see the .cpp): the totems (SMSG_TOTEM_CREATED, GetTotemInfo, DestroyTotem),
    // the shapeshift bar (the learn rule, its order, GetShapeshiftFormInfo, CastShapeshiftForm), the multicast bar
    // (GetMultiCastTotemSpells, SetMultiCastSpell), the bonus bar (GetBonusBarOffset, the override bar's source, the
    // bar it fills), the possession (IsPossessBarVisible, GetPossessInfo), the vehicles (SMSG_PLAYER_VEHICLE_DATA,
    // SMSG_CLIENT_CONTROL_UPDATE, the seat of our SMSG_MONSTER_MOVE_TRANSPORT, VehicleExit, VehicleNextSeat / PrevSeat,
    // UnitSwitchToVehicleSeat, CMSG_SPELLCLICK, CMSG_PLAYER_VEHICLE_ENTER) and the movement of what we control (the
    // mover). Data: text.pak "class_bars" / "class_vehicles" (psp_classbar_data.hpp).
public:
    struct TotemSlotState {  // per slot: guid, (name), duration, start, (icon)
        uint64_t guid = 0;
        uint32_t spellId = 0, durationMs = 0, startMs = 0;
        bool active() const { return spellId != 0; }  // a totem in the slot (upstream TotemSlot::active)
    };
    static constexpr int kTotemSlots = 4;  // MAX_TOTEMS (0-based here: fire, earth, water, air)
    const TotemSlotState& totemSlot(int slot) const;
    uint32_t totemTimeLeftMs(int slot) const;  // GetTotemTimeLeft
    void destroyTotem(int slot);               // DestroyTotem: CMSG_TOTEM_DESTROYED (the server removes the totem)
    uint32_t totemVersion() const { return cbarTotemVersion_; }  // PLAYER_TOTEM_UPDATE
    // The shapeshift bar: the known spells the learn rule lists (stances, forms, presences, paladin auras), in its
    // order.
    const std::vector<uint32_t>& shapeshiftForms();
    bool shapeshiftFormActive(uint32_t spellId) const;  // GetShapeshiftFormInfo's isActive (our form / its aura)
    void castShapeshiftForm(int index);                 // CastShapeshiftForm(index + 1): casts the spell
    uint8_t shapeshiftFormId() const;                   // UNIT_FIELD_BYTES_2 byte 3
    // The multicast bar (MultiCastActionBarFrame): the known totems of a slot (by name).
    const std::vector<uint32_t>& multiCastSpells(int slot);
    bool hasMultiCastBar();                             // HasMultiCastActionBar: a slot with known totems
    bool multiCastCallKnown(int page) const;            // HasMultiCastActionPage: the call spell is known
    bool setMultiCastSpell(int action, uint32_t spellId);  // SetMultiCastSpell; false = refused (not the slot's)
    // GetBonusBarOffset and the bar its override fills (actions 121..132).
    int bonusBarOffset() const;
    uint32_t overrideBits() const { return cbarOverrideBits_; }
    bool overrideIsPet() const { return (cbarOverrideBits_ & 1u) != 0; }  // the moved unit's actions (else spells)
    uint32_t overrideSlot(int i) const { return i >= 0 && i < 12 ? cbarOverride_[i] : 0; }
    uint32_t possessSpell() const { return cbarPossessSpell_; }
    bool possessBarVisible() const;                    // IsPossessBarVisible
    bool possessCanCancel() const;                     // GetPossessInfo(2)'s enabled (not SPELL_ATTR0_NO_AURA_CANCEL)
    void cancelPossess();                              // PossessButton_OnClick(2): VehicleExit or CancelUnitBuff
    bool petActionsOnBonusBar() const { return overrideIsPet(); }  // PetHasActionBar is false then
    // Vehicles.
    bool inVehicle() const { return cbarVehicleGuid_ != 0; }
    uint64_t vehicleGuid() const { return cbarVehicleGuid_; }
    int8_t vehicleSeatIndex() const { return cbarVehicleSeat_; }
    const cbar::VehicleKit* vehicleKit() const { return cbarKit_.valid ? &cbarKit_ : nullptr; }
    const cbar::VehicleSeatRec* vehicleSeat() const;
    uint32_t unitVehicleId(uint64_t guid) const;        // a unit's vehicle kit (UPDATEFLAG_VEHICLE / PLAYER_VEHICLE_DATA)
    bool vehicleUi() const;                             // UnitHasVehicleUI("player")
    bool canExitVehicle() const;                        // CanExitVehicle
    bool canSwitchVehicleSeats() const;                 // CanSwitchVehicleSeats
    bool controllingVehicle() const;                    // UnitControllingVehicle("player"): we move it
    bool vehicleAimAdjustable() const;                  // IsVehicleAimAngleAdjustable
    float vehicleAimAngle() const { return cbarAim_; }  // VehicleAimGetAngle (radians)
    void vehicleAim(float deltaRad);                    // VehicleAimIncrement / the pitch buttons (clamped to the
                                                        // vehicle's PitchMin..PitchMax, sent with MSG_MOVE_SET_PITCH)
    void vehicleExit();                                 // VehicleExit
    void vehicleNextSeat(bool next);                    // VehicleNextSeat / VehiclePrevSeat
    void vehicleSwitchSeat(int seat);                   // UnitSwitchToVehicleSeat("player", seat + 1)
    uint64_t vehicleSeatOccupant(int seat) const;       // UnitVehicleSeatInfo's occupant (0 = free)
    bool vehicleSkinNatural() const;                    // UnitVehicleSkin("player") == "Natural" (else "Mechanical")
    void spellClick(uint64_t guid);                     // CMSG_SPELLCLICK (a unit with UNIT_NPC_FLAG_SPELLCLICK)
    void enterPlayerVehicle(uint64_t guid);             // CMSG_PLAYER_VEHICLE_ENTER (a player's two-seat mount)
    void ejectPassenger(uint64_t guid);                 // CMSG_CONTROLLER_EJECT_PASSENGER
    uint32_t vehicleVersion() const { return cbarVehicleVersion_; }  // UNIT_ENTERED_VEHICLE / _EXITED / VEHICLE_UPDATE
    // The mover (SMSG_CLIENT_CONTROL_UPDATE): what the client moves - us (0) or the unit we drive / possess.
    uint64_t moverGuid() const { return cbarMover_; }
    bool ownControlLost() const { return cbarControlLost_; }  // the server took our own controls (allow 0)
    // WorldScreen::sendMove while a vehicle / a possessed unit is ours to move: the packets carry its guid; false = the
    // movement must not go at all (a passenger: AzerothCore takes a packet without ONTRANSPORT off the vehicle).
    bool movementGuid(uint64_t& guid) const;
    float moverRunSpeed() const;                        // the controlled unit's run speed (its block / speed changes)
    // Per frame (the world screen): the own charm / form / auras -> the possession, the override bar; the totem slots.
    void classBarsUpdate(float dtSec);
    // A one-time exit place (SMSG_MONSTER_MOVE with TransportExit for us): the world screen moves the player there.
    bool takeVehicleExitPlace(glm::vec3& serverPos, float& orientation);
    const cbar::ClassBarData& classBarData();           // text.pak "class_bars", read on first use and kept (~6 KB)

private:
    bool handleClassBarsPacket(game::LogicalOpcode op, network::Packet& packet);
    void classBarsUpdateBlock(const game::UpdateBlock& b);
    void classBarsForget(uint64_t guid);
    void classBarsResetWorld();
    void cbarEnterVehicle(uint64_t vehicle, int8_t seat, const char* src);
    void cbarLeaveVehicle(const char* src);
    void cbarLoadKit(uint32_t vehicleId);
    void cbarRebuildLists();
    void cbarUpdateOverride();
    bool cbarForceSpeed(network::Packet& packet, game::Opcode ack, uint64_t guid);
    void cbarSendAim();
    bool cbarAimPending_ = false;
    uint32_t cbarAimSentMs_ = 0;
    TotemSlotState cbarTotems_[kTotemSlots];
    uint32_t cbarTotemVersion_ = 0;
    cbar::ClassBarData cbarData_;
    bool cbarDataTried_ = false;
    std::vector<uint32_t> cbarForms_;                 // the shapeshift bar (spell ids, in order)
    std::vector<uint8_t> cbarFormIds_;                // their forms (SPELL_AURA_MOD_SHAPESHIFT's value, 0 = none)
    std::vector<uint32_t> cbarTotemLists_[kTotemSlots];
    uint32_t cbarSpellsVersion_ = ~0u;                // knownSpellsVersion() the lists were built for
    uint32_t cbarOverrideBits_ = 0;
    uint32_t cbarOverride_[12] = {};                  // actions 121..132 (pet packed actions or spell ids)
    uint32_t cbarPossessSpell_ = 0;
    uint64_t cbarVehicleGuid_ = 0;
    int8_t cbarVehicleSeat_ = -1;
    uint32_t cbarKitId_ = 0;                          // the Vehicle.dbc id cbarKit_ holds
    cbar::VehicleKit cbarKit_;
    std::unordered_map<uint64_t, uint32_t> cbarUnitVehicle_;  // unit -> Vehicle.dbc id
    std::unordered_map<uint64_t, std::pair<uint64_t, int8_t>> cbarRiderSeat_;  // a rider -> its vehicle, its seat
    uint32_t cbarVehicleVersion_ = 0;
    uint64_t cbarMover_ = 0;
    bool cbarControlLost_ = false;
    float cbarAim_ = 0.0f;
    float cbarMoverRun_ = 0.0f;
    bool cbarExitPending_ = false;
    glm::vec3 cbarExitPos_{0.0f};
    float cbarExitO_ = 0.0f;
    uint32_t cbarLogs_ = 0;
    // class bars (end)
    // glyphs, guild bank, channels: the glyph sockets (psp_world_session_glyphs.cpp),
    // the guild bank (psp_world_session_guildbank.cpp) and the chat channels (psp_world_session_channels.cpp) as
    // the 3.3.5a
    // client keeps them; the packets in psp_glyph_bank_parse.hpp (gbk). The world screen polls the state below - the
    // *Seq / *Version counters are the original's events (USE_GLYPH, GLYPH_ADDED.., GUILDBANKFRAME_OPENED / _CLOSED,
    // GUILDBANKBAGSLOTS_CHANGED, GUILDBANK_UPDATE_*, CHANNEL_UI_UPDATE, CHANNEL_ROSTER_UPDATE, ...).
public:
    // --- Glyphs (GetGlyphSocketInfo / PlaceGlyphInSocket / GlyphMatchesSocket / RemoveGlyphFromSocket) ---
    void setGlyphData(const gbk::GlyphData* d) { glyphData_ = d; }  // text.pak "glyph_data" (the world screen's)
    bool hasGlyphData() const { return glyphData_ != nullptr; }
    gbk::GlyphSocket glyphSocket(int socket) const;              // GetGlyphSocketInfo(socket + 1), the active group
    // SpellIsTargeting with a glyph (UseContainerItem: USE_GLYPH, then CGItem_C::Use waits for a socket).
    bool glyphTargeting() const { return glyphUse_.active; }
    uint32_t glyphTargetingSpell() const { return glyphUse_.spellId; }
    uint32_t glyphTargetingEntry() const { return glyphUse_.entry; }
    bool glyphMatchesSocket(int socket) const;                   // GlyphMatchesSocket(socket + 1)
    bool placeGlyph(int socket);                                 // PlaceGlyphInSocket: false = refused (the red line)
    void cancelGlyphTargeting();                                 // SpellStopTargeting
    void removeGlyph(int socket);                                // RemoveGlyphFromSocket: CMSG_REMOVE_GLYPH
    uint32_t useGlyphSeq() const { return useGlyphSeq_; }        // USE_GLYPH (UIParent: OpenGlyphFrame)
    struct GlyphEvent {
        uint8_t kind = 0;    // 1 GLYPH_ADDED, 2 GLYPH_REMOVED, 3 GLYPH_UPDATED
        uint8_t socket = 0;  // 0..5
    };
    bool takeGlyphEvent(GlyphEvent& out);
    void glyphUpdate();                                          // every frame: the glyph fields -> the events
    // --- Guild bank (GuildBankFrame) ---
    void openGuildBank(uint64_t banker);                         // the vault's use: CMSG_GUILD_BANKER_ACTIVATE
    void closeGuildBank();                                       // CloseGuildBankFrame (GUILDBANKFRAME_CLOSED)
    bool guildBankOpen() const { return gbBanker_ != 0; }
    uint32_t guildBankOpenSeq() const { return gbOpenSeq_; }     // GUILDBANKFRAME_OPENED
    uint32_t guildBankVersion() const { return gbVersion_; }     // BAGSLOTS_CHANGED / UPDATE_TABS / UPDATE_MONEY
    uint8_t guildBankTab() const { return gbTab_; }              // GetCurrentGuildBankTab() - 1
    void setGuildBankTab(uint8_t tab) { if (tab < gbk::kBankTabs) gbTab_ = tab; }  // SetCurrentGuildBankTab
    void queryGuildBankTab(uint8_t tab);                         // QueryGuildBankTab: CMSG_GUILD_BANK_QUERY_TAB
    const gbk::BankItem* guildBankItem(uint8_t tab, uint8_t slot) const;  // nullptr: empty / not received
    uint8_t guildBankNumTabs() const;                            // GetNumGuildBankTabs
    std::string guildBankTabName(uint8_t tab) const;             // GetGuildBankTabInfo's name ("Tab N" when empty)
    std::string guildBankTabIcon(uint8_t tab) const;             // the icon's file name (INV_Misc_QuestionMark)
    uint32_t guildBankTabFlags(uint8_t tab) const;               // the rank's rights on it (gbk::kTab*)
    uint32_t guildBankTabWithdrawals(uint8_t tab) const;         // the rank's stacks per day
    uint32_t guildBankWithdrawalsLeft() const { return gbWithdrawalsLeft_; }  // the last list's
    uint64_t guildBankMoney() const { return gbMoney_; }
    int64_t guildBankWithdrawMoney() const { return gbWithdrawMoney_; }  // GetGuildBankWithdrawMoney (-1 unlimited)
    bool canWithdrawGuildBankMoney() const;                      // CanWithdrawGuildBankMoney
    bool canGuildBankRepair() const;                             // CanGuildBankRepair
    uint32_t guildBankTabCost() const;                           // GetGuildBankTabCost (0: all bought)
    // CMSG_GUILD_BANK_SWAP_ITEMS: a bag item into a bank slot (0xFF: anywhere), a bank item into a bag slot,
    // a bank item to another bank slot, AutoStoreGuildBankItem. False: refused here.
    bool guildBankDeposit(uint8_t tab, uint8_t slot, uint8_t bag, uint8_t bagSlot, uint32_t split = 0);
    bool guildBankWithdraw(uint8_t tab, uint8_t slot, uint8_t bag, uint8_t bagSlot, uint32_t split = 0);
    bool guildBankMove(uint8_t tab, uint8_t slot, uint8_t srcTab, uint8_t srcSlot, uint32_t split = 0);
    bool guildBankAutoStore(uint8_t tab, uint8_t slot);
    void guildBankDepositMoney(uint32_t copper);                 // DepositGuildBankMoney
    void guildBankWithdrawMoney(uint32_t copper);                // WithdrawGuildBankMoney
    void guildBankBuyTab();                                      // BuyGuildBankTab
    void guildBankSetTabInfo(uint8_t tab, const std::string& name, const std::string& icon);  // SetGuildBankTabInfo
    void guildBankQueryLog(uint8_t tab);                         // QueryGuildBankLog (6 = the money log)
    const std::vector<gbk::BankLogEntry>& guildBankLog(uint8_t tab) const;
    uint32_t guildBankLogVersion() const { return gbLogVersion_; }  // GUILDBANKLOG_UPDATE
    uint32_t guildBankLogAtMs(uint8_t tab) const { return tab <= gbk::kBankTabs ? gbLogAtMs_[tab] : 0u; }
    void guildBankQueryText(uint8_t tab);                        // QueryGuildBankText
    const std::string& guildBankText(uint8_t tab) const;         // GetGuildBankText
    uint32_t guildBankTextSeq() const { return gbTextSeq_; }     // GUILDBANK_UPDATE_TEXT
    uint32_t guildBankTextChangedSeq() const { return gbTextChangedSeq_; }  // GUILDBANK_TEXT_CHANGED (its tab:)
    uint8_t guildBankTextChangedTab() const { return gbTextChangedTab_; }
    void guildBankSetText(uint8_t tab, const std::string& text); // SetGuildBankText
    std::string guildBankName(uint64_t guid);                    // a log entry's name ("" while its query is out)
    void guildBankUpdate(const glm::vec3& selfPos);              // every frame: the vault out of reach closes it
    // --- Chat channels (ChannelFrame / ChatFrame) ---
    struct ChatChannel {             // a joined channel (listed from YOU_JOINED until YOU_LEFT)
        int number = 0;              // 1..10: its place in the join order
        std::string name;            // the full name ("General - <zone>")
        uint32_t channelId = 0;      // ChatChannels id (0 = custom)
        uint8_t flags = 0;           // the channel flags of YOU_JOINED (CHANNEL_FLAG_*)
        uint32_t members = 0;        // the last SMSG_CHANNEL_LIST's count
    };
    // ChatChannels.dbc (text.pak "chat_chan") and the zone the joins are formatted for (the world screen's).
    void setChatChannelData(std::vector<gbk::ChatChannelRow> rows, std::string cityName);
    void setChannelZone(uint32_t zoneId, const std::string& zoneName, bool city);
    bool hasChatChannelData() const { return !chanRows_.empty(); }
    uint32_t channelZoneId() const { return chanZoneId_; }
    const std::vector<gbk::ChatChannelRow>& chatChannelRows() const { return chanRows_; }
    const std::vector<ChatChannel>& chatChannels() const { return channels_; }
    const ChatChannel* chatChannelByNumber(int number) const;
    const ChatChannel* chatChannelByName(const std::string& name) const;
    int channelNumberOf(const std::string& name) const;          // the chat line's number (0: not listed)
    uint32_t channelVersion() const { return chanVersion_; }      // CHANNEL_UI_UPDATE
    // JoinPermanentChannel / JoinChannelByName: a ChatChannels shortcut ("General") joins the
    // zone's channel. Returns false when the name is invalid (CHAT_INVALID_NAME_NOTICE).
    bool joinChannel(const std::string& name, const std::string& password = std::string());
    void leaveChannel(const std::string& nameOrNumber);          // LeaveChannelByName
    void listChannel(const std::string& nameOrNumber);           // ListChannelByName: CMSG_CHANNEL_LIST
    // The commands on a channel's member (ChannelInvite, ChannelKick, ChannelBan, ChannelUnban, ChannelModerator,
    // ChannelUnmoderator, ChannelMute, ChannelUnmute, SetChannelOwner): string channel, string player.
    void channelMemberCommand(game::LogicalOpcode op, const std::string& channel, const std::string& player);
    // Owner / announcements / moderate (string channel), password (string channel, string password).
    void channelSimpleCommand(game::LogicalOpcode op, const std::string& channel, const std::string& arg = std::string());
    void declineChannelInvite(const std::string& channel);       // DeclineInvite
    bool sendChannelMessage(int number, const std::string& text);  // SendChatMessage(text, "CHANNEL", nil, number)
    // The roster of the channel FriendsFrame shows (SetSelectedDisplayChannel -> CMSG_CHANNEL_DISPLAY_LIST).
    void requestChannelRoster(const std::string& channel);
    const std::string& rosterChannel() const { return rosterChannel_; }
    const std::vector<gbk::ChannelMember>& channelRoster() const { return roster2_; }
    uint8_t rosterChannelFlags() const { return rosterChannelFlags_; }
    uint32_t channelRosterVersion() const { return rosterVersion_; }  // CHANNEL_ROSTER_UPDATE
    uint8_t ownChannelFlags() const;                             // our member flags in the roster's channel
    std::string channelMemberName(uint64_t guid);                // the roster's name ("" while its query is out)
    // CHANNEL_INVITE_REQUEST / CHANNEL_PASSWORD_REQUEST for the StaticPopups (taken once).
    bool takeChannelInvite(std::string& channel, std::string& inviter);
    bool takeChannelPasswordRequest(std::string& channel);
    void channelUpdate();                                        // every frame: the world-entry joins, the names

private:
    // glyphs
    ItemUse glyphUseItem(const PendingItemUse& use);             // useItemAt's glyph branch (item-use hook)
    const gbk::GlyphData* glyphData_ = nullptr;
    PendingItemUse glyphUse_;
    uint32_t useGlyphSeq_ = 0;
    uint32_t glyphSeen_[gbk::kGlyphSockets] = {};
    std::deque<GlyphEvent> glyphEvents_;
    // guild bank
    bool handleGuildBankPacket(game::LogicalOpcode op, network::Packet& packet);
    void guildBankError(uint32_t code);                          // a refusal: the red error line
    void guildBankReset();                                       // a new guild
    void guildBankAskWithdrawMoney();                            // MSG_GUILD_BANK_MONEY_WITHDRAWN
    bool guildBankMoveAllowed(int from, int to, bool swap);      // the rank's tab rights (-1 = the bags)
    uint64_t gbBanker_ = 0, gbPending_ = 0;
    uint32_t gbOpenSeq_ = 0, gbVersion_ = 0, gbLogVersion_ = 0, gbTextSeq_ = 0, gbTextChangedSeq_ = 0;
    uint8_t gbTab_ = 0, gbTextChangedTab_ = 0, gbNumTabs_ = 0;
    bool gbTabsKnown_ = false;
    uint64_t gbMoney_ = 0;
    uint32_t gbWithdrawalsLeft_ = 0;
    int64_t gbWithdrawMoney_ = -1;
    std::vector<gbk::BankTabInfo> gbTabs_;
    std::vector<std::vector<gbk::BankItem>> gbItems_;            // [tab][slot], made on the first list of a tab
    std::vector<gbk::BankLogEntry> gbLog_[gbk::kBankTabs + 1];
    uint32_t gbLogAtMs_[gbk::kBankTabs + 1] = {};
    std::string gbText_[gbk::kBankTabs];
    uint32_t gbGuildSeen_ = 0, gbLogs_ = 0;
    // channels
    bool handleChannelPacket(game::LogicalOpcode op, network::Packet& packet);
    bool channelNoticeReady(const gbk::ChannelNotify& n, const std::string& key);  // false: a name is still asked
    ChatChannel* channelSlotFor(uint32_t id, const std::string& name);
    std::string channelDisplay(const ChatChannel& c) const;    // arg4: "N. Name"
    void channelChatLine(const std::string& text, game::ChatType type);
    std::vector<gbk::ChatChannelRow> chanRows_;
    std::string chanCityName_, chanZoneName_;
    uint32_t chanZoneId_ = 0;
    bool chanZoneCity_ = false;
    std::vector<ChatChannel> channels_;                          // <= 10 (ChatFrame.lua's channel limit), join order
    void renumberChannels();                                     // number = place in channels_ + 1
    void setAutoJoin(uint32_t channelId, bool on);               // the world entry's auto-join set (wowee.cfg)
    uint32_t chanVersion_ = 0, autoJoinMask_ = 0;
    bool chanMaskRead_ = false, chanWorldJoinDone_ = false;  // the auto-join set read; joined once per world entry
    std::string rosterChannel_;
    std::vector<gbk::ChannelMember> roster2_;
    uint8_t rosterChannelFlags_ = 0;
    uint32_t rosterVersion_ = 0;
    struct ChanPendingLine {                                     // a notice waiting for its player names
        gbk::ChannelNotify n;
        std::string key;
        uint32_t deadlineMs = 0;
    };
    std::vector<ChanPendingLine> chanPending_;
    std::string chanInvite_, chanInviter_, chanPasswordAsk_;
    uint64_t chanInviterGuid_ = 0;
    uint32_t chanInviteMs_ = 0;
    bool chanInviteNew_ = false, chanPasswordNew_ = false;
    std::string chanListShow_;                                   // /chatlist asked: the list goes to the chat
    uint32_t chanLogs_ = 0;
    // glyphs, guild bank, channels (end)
};

} // namespace wowee::platform::psp
