// chat. The receive and send path of upstream's ChatHandler
// (src/game/chat_handler.cpp) for the PSP WorldSession: ChatHandler itself is
// tied to GameHandler (owner_), which the PSP does not build, so its steps are
// repeated here the way M10a repeats the EntityController's - with upstream's
// packet classes unchanged (MessageChatParser, TextEmoteParser,
// MessageChatPacket, TextEmotePacket, MotdParser, decodeAddonChatPayload,
// resolveTextTokens, repeatsRecentLine).
//
// Not carried over (no GameHandler state behind them on the PSP): channels
// (SMSG_CHANNEL_NOTIFY / auto-join), the party and guild rosters as name
// sources, the officer-rank filter, AFK/DND auto-replies, chat bubbles, the
// addon events and the chat log file.
#include "platform/psp/psp_world_session.hpp"

#include "core/application.hpp"  // shadowed on PSP (src/platform/psp/shadow)
#include "game/game_utils.hpp"
#include "game/text_tokens.hpp"
#include "game/update_field_table.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"  // the client's strings

#include <pspthreadman.h>

#include <cctype>
#include <cstring>

namespace wowee::platform::psp {

namespace {

uint64_t nowUs() { return sceKernelGetSystemTimeWide(); }

// upstream ChatHandler::kNameWaitSeconds
constexpr uint64_t kNameWaitUs = 1000ull * 1000;
// PSP: the window keeps ~50 lines; more than this waiting between two frames
// means nobody is reading them.
constexpr size_t kMaxQueued = 64;

// upstream chat_handler.cpp isMonsterChatType
bool isMonsterChatType(game::ChatType type) {
    using game::ChatType;
    switch (type) {
        case ChatType::MONSTER_SAY:
        case ChatType::MONSTER_PARTY:
        case ChatType::MONSTER_YELL:
        case ChatType::MONSTER_WHISPER:
        case ChatType::MONSTER_EMOTE:
        case ChatType::RAID_BOSS_EMOTE:
        case ChatType::RAID_BOSS_WHISPER:
            return true;
        default:
            return false;
    }
}

// upstream chat_handler.cpp isPlayerGuid
bool isPlayerGuid(uint64_t guid) { return guid != 0 && (guid >> 48) == 0; }

// upstream ChatHandler::deliverChatMessage: the BG/arena queue announcer
// filter (server modules on ChromieCraft/AzerothCore), unchanged.
bool isQueueAnnouncerSpam(const std::string& msg) {
    auto containsCI = [&](const char* needle) {
        const size_t nlen = std::strlen(needle);
        if (msg.size() < nlen) return false;
        const size_t last = msg.size() - nlen;
        for (size_t i = 0; i <= last; ++i) {
            bool match = true;
            for (size_t j = 0; j < nlen; ++j) {
                unsigned char a = static_cast<unsigned char>(msg[i + j]);
                unsigned char b = static_cast<unsigned char>(needle[j]);
                if (std::tolower(a) != std::tolower(b)) {
                    match = false;
                    break;
                }
            }
            if (match) return true;
        }
        return false;
    };
    if (containsCI("queue announcer") || containsCI("queue status")) return true;
    if (msg.find("|c") == std::string::npos) return false;
    static const char* kBracketTokens[] = {"2x2|r", "3x3|r", "5x5|r", "2v2|r", "3v3|r", "5v5|r",
                                           "2X2|r", "3X3|r", "5X5|r", "2V2|r", "3V3|r", "5V5|r"};
    bool hasBracket = false;
    for (const char* t : kBracketTokens)
        if (msg.find(t) != std::string::npos) {
            hasBracket = true;
            break;
        }
    return hasBracket && (containsCI("joined") || containsCI("exited") || containsCI(" left ") ||
                          containsCI("entered") || containsCI("queue"));
}

// upstream deliverChatMessage: addon-to-addon whispers are dropped.
bool isAddonWhisper(const std::string& msg) {
    return msg.size() >= 3 &&
           (msg.rfind("GS_", 0) == 0 || msg.rfind("DVNE", 0) == 0 || msg.rfind("oRA", 0) == 0 ||
            msg.rfind("BWVQ", 0) == 0 || msg.rfind("AVR", 0) == 0 || msg.rfind('\t', 0) == 0 ||
            (msg.size() > 4 && static_cast<unsigned char>(msg[0]) > 127));
}

game::MessageChatData systemLine(const std::string& text) {
    game::MessageChatData msg;
    msg.type = game::ChatType::SYSTEM;
    msg.language = game::ChatLanguage::UNIVERSAL;
    msg.message = text;
    return msg;
}

} // namespace

const game::Character* WorldSession::activeCharacter() const {
    for (const auto& c : characters_)
        if (c.guid == playerGuid_) return &c;
    return nullptr;
}

// upstream ChatHandler::knownPlayerName / the name steps of deliverChatMessage:
// the name cache, then the entity nearby (player or unit).
std::string WorldSession::knownChatName(uint64_t guid) const {
    if (guid == 0) return {};
    auto it = names_.find(guid);
    if (it != names_.end()) return it->second;
    if (guid == playerGuid_) {
        if (const game::Character* me = activeCharacter()) return me->name;
    }
    if (auto e = entities_.getEntity(guid)) {
        if (auto* u = dynamic_cast<const game::Unit*>(e.get()))
            if (!u->getName().empty()) return u->getName();
    }
    return {};
}

bool WorldSession::handleChatPacket(game::LogicalOpcode op, network::Packet& packet) {
    using game::LogicalOpcode;
    const bool inWorld = state_ == State::InWorld;
    switch (op) {
        case LogicalOpcode::SMSG_MESSAGECHAT:
            if (inWorld) handleMessageChat(packet);
            return true;
        case LogicalOpcode::SMSG_GM_MESSAGECHAT: {
            // upstream: the same header as SMSG_MESSAGECHAT plus the GM's
            // name; strip it and parse as a regular line.
            if (!inWorld || !packet.hasRemaining(21)) return true;
            const uint8_t type = packet.readUInt8();
            const uint32_t lang = packet.readUInt32();
            const uint64_t senderGuid = packet.readUInt64();
            const uint32_t unk = packet.readUInt32();
            const uint32_t gmNameLen = packet.readUInt32();
            if (!packet.hasRemaining(gmNameLen)) return true;
            packet.setReadPos(packet.getReadPos() + gmNameLen);
            network::Packet regular(0);
            regular.writeUInt8(type);
            regular.writeUInt32(lang);
            regular.writeUInt64(senderGuid);
            regular.writeUInt32(unk);
            const auto& raw = packet.getData();
            const size_t pos = packet.getReadPos();
            if (pos < raw.size()) regular.writeBytes(raw.data() + pos, raw.size() - pos);
            handleMessageChat(regular);
            return true;
        }
        case LogicalOpcode::SMSG_TEXT_EMOTE:
            if (inWorld) handleTextEmote(packet);
            return true;
        // PSP: the client's own lines in the running language (upstream writes English here).
        // the client's strings (GlobalStrings ERR_CHAT_PLAYER_NOT_FOUND_S /
        // ERR_CHAT_PLAYER_AMBIGUOUS_S; from strings.pak).
        case LogicalOpcode::SMSG_CHAT_PLAYER_NOT_FOUND: {
            const std::string name = packet.readString();
            if (!name.empty()) addSystemChatMessage(CLIENT_FMT(ERR_CHAT_PLAYER_NOT_FOUND_S, name));
            return true;
        }
        case LogicalOpcode::SMSG_CHAT_PLAYER_AMBIGUOUS: {
            const std::string name = packet.readString();
            if (!name.empty()) addSystemChatMessage(CLIENT_FMT(ERR_CHAT_PLAYER_AMBIGUOUS_S, name));
            return true;
        }
        // upstream shows these as UI errors (red text mid-screen). So does
        // the PSP now (UIErrorsFrame; were chat lines while the PSP had no error frame), once - upstream
        // calls addUIError and raiseUiError, which adds the line twice. Strings: ERR_CHAT_WRONG_FACTION,
        // ERR_NOT_IN_GROUP; SMSG_CHAT_RESTRICTED carries the reason byte (AzerothCore
        // ChatRestrictionType: 0 ERR_CHAT_RESTRICTED, 1 ERR_CHAT_THROTTLED, 2 ERR_USER_SQUELCHED,
        // 3 ERR_YELL_RESTRICTED) - upstream ignores it and writes one English line.
        case LogicalOpcode::SMSG_CHAT_WRONG_FACTION:
            addUiError(CLIENT_STR(ERR_CHAT_WRONG_FACTION));
            return true;
        case LogicalOpcode::SMSG_CHAT_NOT_IN_PARTY:
            addUiError(CLIENT_STR(ERR_NOT_IN_GROUP));
            return true;
        case LogicalOpcode::SMSG_CHAT_RESTRICTED: {
            const uint8_t why = packet.hasRemaining(1) ? packet.readUInt8() : 0;
            switch (why) {
                case 1: addUiError(CLIENT_STR(ERR_CHAT_THROTTLED)); break;
                case 2: addUiError(CLIENT_STR(ERR_USER_SQUELCHED)); break;
                case 3: addUiError(CLIENT_STR(ERR_YELL_RESTRICTED)); break;
                default:
                    addUiError(CLIENT_STR(ERR_CHAT_RESTRICTED));
                    break;
            }
            return true;
        }
        case LogicalOpcode::SMSG_DEFENSE_MESSAGE:
            if (packet.hasRemaining(5)) {
                packet.readUInt32();  // zone id
                const std::string msg = packet.readString();
                if (!msg.empty()) addSystemChatMessage("[Defense] " + msg);
            }
            return true;
        case LogicalOpcode::SMSG_SERVER_MESSAGE:
            if (packet.hasRemaining(4)) {
                const uint32_t msgType = packet.readUInt32();
                const std::string msg = packet.readString();
                if (!msg.empty()) {
                    const char* prefix = "[Server] ";
                    switch (msgType) {
                        case 1: prefix = "[Shutdown] "; break;
                        case 2: prefix = "[Restart] "; break;
                        case 4: prefix = "[Shutdown cancelled] "; break;
                        case 5: prefix = "[Restart cancelled] "; break;
                        default: break;
                    }
                    addSystemChatMessage(prefix + msg);
                }
            }
            return true;
        case LogicalOpcode::SMSG_CHAT_SERVER_MESSAGE:
            if (packet.hasRemaining(4)) {
                packet.readUInt32();  // message type
                const std::string msg = packet.readString();
                if (!msg.empty()) addSystemChatMessage("[Announcement] " + msg);
            }
            return true;
        case LogicalOpcode::SMSG_AREA_TRIGGER_MESSAGE:
            if (packet.hasRemaining(4)) {
                packet.readUInt32();  // length
                const std::string msg = packet.readString();
                // The server sends these when it refuses an area trigger (AzerothCore Player::Satisfy: the level,
                // item, quest or group requirements of an instance entrance) -> the red error line by the port's
                // rule (psp_game_line.hpp); upstream shows it on both lines.
                if (!msg.empty()) gameLine(GameLine::Refused, msg);
            }
            return true;
        case LogicalOpcode::SMSG_NOTIFICATION: {
            // upstream GameHandler::handleNotification
            const std::string msg = packet.readString();
            if (!msg.empty()) addSystemChatMessage(msg);
            return true;
        }
        case LogicalOpcode::SMSG_MOTD: {
            // upstream ChatHandler::handleMotd (the blank spacer line after
            // the block is left out - PSP: six lines on screen).
            game::MotdData data;
            if (game::MotdParser::parse(packet, data))
                for (const auto& line : data.lines) addSystemChatMessage("MOTD: " + line);
            return true;
        }
        default:
            return false;
    }
}

void WorldSession::handleMessageChat(network::Packet& packet) {
    PROF_SCOPE("chat.rx");
    game::MessageChatData data;
    // upstream: owner_.getPacketParsers()->parseMessageChat, which for 3.3.5a
    // is the base MessageChatParser.
    if (!game::MessageChatParser::parse(packet, data)) {
        PSP_LOGW(tag::Net, "chat_rx_bad size=%u", static_cast<unsigned>(packet.getSize()));
        return;
    }
    if (++chatRxLogs_ <= 60)
        PSP_LOGI(tag::Net, "chat_rx type=%s len=%u", game::getChatTypeString(data.type),
                 static_cast<unsigned>(data.message.size()));
    // The client made its own WHISPER_INFORM row when it sent the whisper.
    if (data.type == game::ChatType::WHISPER_INFORM) return;
    // Our own lines come back from the server: the local echo is already shown.
    if (data.senderGuid == playerGuid_ && data.senderGuid != 0) {
        if (data.type == game::ChatType::WHISPER && !data.senderName.empty()) lastWhisperSender_ = data.senderName;
        return;
    }
    deliverChatMessage(std::move(data), false);
}

void WorldSession::deliverChatMessage(game::MessageChatData data, bool alreadyWaited) {
    using game::ChatType;
    if (data.senderName.empty() && data.senderGuid != 0) {
        data.senderName = knownChatName(data.senderGuid);
        if (data.senderName.empty()) {
            // Ask, and hold the line up to a second rather than show it
            // without a name (upstream: a whisper is from nobody nearby).
            if (isPlayerGuid(data.senderGuid) && nameQueried_.insert(data.senderGuid).second && socket_)
                socket_->send(game::NameQueryPacket::build(data.senderGuid));
            if (!alreadyWaited) {
                const uint64_t guid = data.senderGuid;
                chatAwaitingName_.push_back({std::move(data), guid, nowUs() + kNameWaitUs});
                return;
            }
        }
    }
    // The player an NPC's line is written for, when that is someone else.
    if (isMonsterChatType(data.type) && data.message.find('$') != std::string::npos &&
        isPlayerGuid(data.receiverGuid) && data.receiverGuid != playerGuid_ &&
        knownChatName(data.receiverGuid).empty()) {
        if (nameQueried_.insert(data.receiverGuid).second && socket_)
            socket_->send(game::NameQueryPacket::build(data.receiverGuid));
        if (!alreadyWaited) {
            const uint64_t guid = data.receiverGuid;
            chatAwaitingName_.push_back({std::move(data), guid, nowUs() + kNameWaitUs});
            return;
        }
    }
    if (data.message.empty()) return;
    if ((data.type == ChatType::ACHIEVEMENT || data.type == ChatType::GUILD_ACHIEVEMENT) && data.achievementId) {
        // upstream links the achievement's name (Achievement.dbc); the PSP reads it from data/wotlk.pak
        // (upstream's own fallback "Achievement #id" without the pak).
        const std::string link = "[" + achievementName(data.achievementId) + "]";
        size_t pos = 0;
        while ((pos = data.message.find("$a", pos)) != std::string::npos) {
            data.message.replace(pos, 2, link);
            pos += link.size();
        }
    }
    // Addon traffic never reaches the visible chat.
    std::string addonPrefix, addonPayload;
    if (game::decodeAddonChatPayload(data, addonPrefix, addonPayload)) return;
    // Monster lines: %s is the creature's name.
    if (!data.senderName.empty() && isMonsterChatType(data.type)) {
        size_t pos = data.message.find("%s");
        while (pos != std::string::npos) {
            data.message.replace(pos, 2, data.senderName);
            pos = data.message.find("%s", pos + data.senderName.size());
        }
    }
    if (isQueueAnnouncerSpam(data.message)) return;
    if ((data.type == ChatType::WHISPER || data.type == ChatType::WHISPER_INFORM) && isAddonWhisper(data.message))
        return;
    // $-tokens of what an NPC said, for whoever it is written for
    // (upstream monsterLineSubject).
    if (isMonsterChatType(data.type)) {
        game::TextSubject subject;
        if (data.receiverGuid == 0 || data.receiverGuid == playerGuid_) {
            if (const game::Character* me = activeCharacter()) {
                subject.name = me->name;
                subject.className = game::getClassName(me->characterClass);
                subject.raceName = game::getRaceName(me->race);
                subject.gender = me->gender;
            }
        } else if (isPlayerGuid(data.receiverGuid)) {
            const std::string n = knownChatName(data.receiverGuid);
            if (!n.empty()) subject.name = n;
        } else if (!data.receiverName.empty()) {
            subject.name = data.receiverName;
        }
        data.message = game::resolveTextTokens(data.message, subject);
    }
    if (data.type == ChatType::WHISPER && !data.senderName.empty()) lastWhisperSender_ = data.senderName;
    // Spam filter (upstream CVar spamFilter, on by default): the same line
    // from the same player within 30 s.
    const bool fromAPlayer = data.type == ChatType::SAY || data.type == ChatType::YELL ||
                             data.type == ChatType::PARTY || data.type == ChatType::RAID ||
                             data.type == ChatType::GUILD || data.type == ChatType::OFFICER ||
                             data.type == ChatType::WHISPER || data.type == ChatType::EMOTE ||
                             data.type == ChatType::CHANNEL;
    if (fromAPlayer) {
        const double now = static_cast<double>(nowUs()) / 1e6;
        if (game::repeatsRecentLine(recentChatLines_, data.senderGuid, data.message, now)) return;
        recentChatLines_.push_back({.senderGuid = data.senderGuid, .text = data.message, .at = now});
        while (recentChatLines_.size() > 64) recentChatLines_.pop_front();
    }
    addLocalChatMessage(data);
}

void WorldSession::chatUpdate() {
    if (chatAwaitingName_.empty()) return;
    // Released when the name landed (SMSG_NAME_QUERY_RESPONSE filled names_)
    // or the wait ran out (upstream flushChatAwaitingName /
    // expireChatAwaitingName). Delivered outside the walk: delivery can queue.
    const uint64_t now = nowUs();
    std::vector<game::MessageChatData> ready;
    for (auto it = chatAwaitingName_.begin(); it != chatAwaitingName_.end();) {
        if (now >= it->deadlineUs || !knownChatName(it->guid).empty()) {
            ready.push_back(std::move(it->data));
            it = chatAwaitingName_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& d : ready) deliverChatMessage(std::move(d), true);
}

void WorldSession::handleTextEmote(network::Packet& packet) {
    PROF_SCOPE("chat.rx");
    game::TextEmoteData data;
    if (!game::TextEmoteParser::parse(packet, data, game::isClassicLikeExpansion())) return;
    if (data.senderGuid == playerGuid_ && data.senderGuid != 0) return;  // our own: the window echoed it
    TextEmoteHeard heard;
    heard.sender = data.senderGuid;
    heard.senderName = knownChatName(data.senderGuid);
    heard.textEmoteId = data.textEmoteId;
    heard.targetName = data.targetName;
    if (heard.senderName.empty()) {
        // upstream: ask for the name and drop the line (the animation comes
        // separately in SMSG_EMOTE).
        if (isPlayerGuid(data.senderGuid) && nameQueried_.insert(data.senderGuid).second && socket_)
            socket_->send(game::NameQueryPacket::build(data.senderGuid));
        return;
    }
    if (auto e = entities_.getEntity(data.senderGuid))
        heard.gender = static_cast<uint8_t>((e->getField(game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0)) >> 16) & 0xFF);
    if (++chatRxLogs_ <= 60)
        PSP_LOGI(tag::Net, "chat_rx type=TEXT_EMOTE id=%u", static_cast<unsigned>(data.textEmoteId));
    textEmotesHeard_.push_back(std::move(heard));
    while (textEmotesHeard_.size() > kMaxQueued) textEmotesHeard_.pop_front();
}

void WorldSession::sendChatMessage(game::ChatType type, const std::string& message, const std::string& target) {
    if (state_ != State::InWorld || !socket_ || message.empty()) return;
    // upstream: the player's faction language (AzerothCore rejects a wrong
    // one) - the Horde race ids 2, 5, 6, 8, 10.
    const game::Character* me = activeCharacter();
    const auto race = me ? static_cast<uint8_t>(me->race) : uint8_t{1};
    const bool horde = race == 2 || race == 5 || race == 6 || race == 8 || race == 10;
    const game::ChatLanguage language = horde ? game::ChatLanguage::ORCISH : game::ChatLanguage::COMMON;
    socket_->send(game::MessageChatPacket::build(type, language, message, target));
    if (++chatTxLogs_ <= 60)
        PSP_LOGI(tag::Net, "chat_tx type=%s len=%u", game::getChatTypeString(type),
                 static_cast<unsigned>(message.size()));
    // The local echo, so the line shows at once.
    game::MessageChatData echo;
    echo.senderGuid = playerGuid_;
    echo.language = language;
    echo.message = message;
    echo.senderName = knownChatName(playerGuid_);
    if (type == game::ChatType::WHISPER) {
        echo.type = game::ChatType::WHISPER_INFORM;
        echo.receiverName = target;
    } else {
        echo.type = type;
    }
    if (type == game::ChatType::CHANNEL) echo.channelName = target;
    addLocalChatMessage(echo);
}

void WorldSession::sendTextEmoteAt(uint32_t textEmoteId, uint64_t targetGuid) {
    if (state_ != State::InWorld || !socket_) return;
    socket_->send(game::TextEmotePacket::build(textEmoteId, targetGuid));
}

void WorldSession::addSystemChatMessage(const std::string& text) {
    if (!text.empty()) addLocalChatMessage(systemLine(text));
}

void WorldSession::addLocalChatMessage(const game::MessageChatData& msg) {
    chatLines_.push_back(msg);
    while (chatLines_.size() > kMaxQueued) chatLines_.pop_front();
}

bool WorldSession::takeChatLine(game::MessageChatData& out) {
    if (chatLines_.empty()) return false;
    out = std::move(chatLines_.front());
    chatLines_.pop_front();
    return true;
}

// upstream GameHandler::addUIError -> UI_ERROR_MESSAGE; the world screen
// drains these into UIErrorsFrame (WorldScreen::uiErrorsTick).
void WorldSession::addUiError(const std::string& text) {
    if (text.empty()) return;
    uiErrors_.push_back(text);
    while (uiErrors_.size() > kMaxQueued) uiErrors_.pop_front();
}

bool WorldSession::takeUiError(std::string& out) {
    if (uiErrors_.empty()) return false;
    out = std::move(uiErrors_.front());
    uiErrors_.pop_front();
    return true;
}

bool WorldSession::takeTextEmote(TextEmoteHeard& out) {
    if (textEmotesHeard_.empty()) return false;
    out = std::move(textEmotesHeard_.front());
    textEmotesHeard_.pop_front();
    return true;
}

} // namespace wowee::platform::psp
