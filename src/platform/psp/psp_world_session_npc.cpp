// NPC interaction for the PSP world session - gossip, vendors and
// quests. GameHandler / QuestHandler / InventoryHandler are not built for the
// PSP (they pull in rendering, audio and the addon layer), so their steps are
// mirrored here with upstream's packet builders and parsers unchanged
// (world_packets_world.cpp):
//   - QuestHandler::registerOpcodes: SMSG_GOSSIP_MESSAGE, SMSG_QUESTGIVER_QUEST_LIST,
//     SMSG_GOSSIP_COMPLETE, SMSG_NPC_TEXT_UPDATE, SMSG_QUESTGIVER_QUEST_DETAILS,
//     SMSG_QUESTLOG_FULL, SMSG_QUESTGIVER_REQUEST_ITEMS / OFFER_REWARD,
//     SMSG_QUESTGIVER_STATUS(_MULTIPLE), SMSG_QUESTUPDATE_FAILED(TIMER),
//     SMSG_QUESTGIVER_QUEST_FAILED / INVALID / COMPLETE, SMSG_QUESTUPDATE_ADD_KILL /
//     ADD_ITEM / COMPLETE / ADD_PVP_KILL, SMSG_QUEST_QUERY_RESPONSE;
//   - QuestHandler's public API (selectGossipOption, selectGossipQuest,
//     acceptQuest, completeQuest, chooseQuestReward, abandonQuest,
//     requestQuestQuery...) and its quest-log sync from the PLAYER_QUEST_LOG_*
//     fields (resyncQuestLogFromServerSlots, applyQuestStateFromFields,
//     applyPackedKillCountsFromFields);
//   - InventoryHandler's vendor: openVendor, closeVendor, buyItem, sellItem,
//     repairAll, handleListInventory, SMSG_BUY_ITEM / SMSG_BUY_FAILED /
//     SMSG_SELL_ITEM;
//   - GameHandler::interactWithNpc (CMSG_GOSSIP_HELLO only to units with NPC
//     flags), GameHandler::update's pending-accept timeouts, login quest
//     resync and closeIfTooFar, EntityController's CMSG_QUESTGIVER_STATUS_QUERY
//     on a questgiver's creation.
// The PSP is 3.3.5a only: owner_.getPacketParsers() is the 3.3.5a set there,
// whose gossip / quest-details / questgiver-status readers are the static
// parsers and readUInt8 used here directly (questLogStride 5, 25 slots).
// Addon events (GOSSIP_SHOW, QUEST_DETAIL...) are not fired: there is no Lua
// yet - task A's adapter reads the same state through the getters.
// Client-made lines (upstream addSystemChatMessage / addUIError) go to the chat
// in Russian (PSP: no FrameXML strings).
#include "platform/psp/psp_world_session.hpp"

#include "core/coordinates.hpp"
#include "game/protocol_constants.hpp"
#include "game/quest_progress.hpp"
#include "game/quest_query_layout.hpp"
#include "game/quest_text.hpp"
#include "game/update_field_table.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"


#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>

namespace wowee::platform::psp {

using game::LogicalOpcode;
using game::QuestGiverStatus;

namespace {

// 3.3.5a: upstream PacketParsers::questLogStride() and maxQuestLogSlots().
constexpr uint8_t kQuestLogStride = 5;
constexpr int kMaxQuestLogSlots = 25;
constexpr float kQuestGiverRequeryIntervalSec = 1.0f;  // upstream QuestHandler
constexpr size_t kMaxItemInfos = 1024;                 // PSP: bound on the name cache


// PSP (wowee.cfg questutf8, default 1): upstream's quest-text heuristics count
// only ASCII letters as text and ASCII capitals as a title's first letter, so
// on a ruRU realm (the PSP logs in as ruRU, quest_template_locale) every
// Cyrillic title, objective and description of SMSG_QUEST_QUERY_RESPONSE was
// dropped as "not text": quests loaded at login stayed "Quest #id" with no
// objectives. With the key on, a UTF-8 letter counts too. 0 = upstream.
bool g_utf8QuestText = true;

bool utf8Letter(const std::string& s, size_t i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    return c >= 0xC0 && i + 1 < s.size() && (static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80;
}

// Cyrillic capitals: U+0410..U+042F (D0 90..D0 AF) and U+0401 (D0 81).
bool utf8Upper(const std::string& s) {
    if (s.size() < 2 || static_cast<unsigned char>(s[0]) != 0xD0) return false;
    const unsigned char b = static_cast<unsigned char>(s[1]);
    return (b >= 0x90 && b <= 0xAF) || b == 0x81;
}

bool utf8Lower(const std::string& s) {
    if (s.size() < 2) return false;
    const unsigned char a = static_cast<unsigned char>(s[0]), b = static_cast<unsigned char>(s[1]);
    return (a == 0xD0 && b >= 0xB0 && b <= 0xBF) || (a == 0xD1 && b >= 0x80 && b <= 0x8F) || (a == 0xD1 && b == 0x91);
}

// --- Copied from upstream quest_handler.cpp (file-static there) --------------
// Only change: isReadableQuestText and the first-letter case tests also accept
// UTF-8 letters when g_utf8QuestText is on (PSP, see above).

bool isReadableQuestText(const std::string& s, size_t minLen, size_t maxLen) {
    if (game::questTextIsReadable(s, minLen, maxLen)) return true;
    if (!g_utf8QuestText) return false;
    // PSP: the same test, a UTF-8 letter counting as the letter.
    if (s.size() < minLen || s.size() > maxLen) return false;
    bool hasAlpha = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') return false;
        if (!hasAlpha && utf8Letter(s, i)) hasAlpha = true;
    }
    return hasAlpha;
}

bool isUpperFirst(const std::string& s) {
    if (std::isupper(static_cast<unsigned char>(s.front())) != 0) return true;
    return g_utf8QuestText && utf8Upper(s);
}

bool isLowerFirst(const std::string& s) {
    if (std::islower(static_cast<unsigned char>(s.front())) != 0) return true;
    return g_utf8QuestText && utf8Lower(s);
}

bool isPlaceholderQuestTitle(const std::string& s) { return s.rfind("Quest #", 0) == 0; }

bool looksLikeQuestDescriptionText(const std::string& s) {
    int spaces = 0;
    int commas = 0;
    for (unsigned char c : s) {
        if (c == ' ') spaces++;
        if (c == ',') commas++;
    }
    const int words = spaces + 1;
    if (words > 8) return true;
    if (commas > 0 && words > 5) return true;
    if (s.find(". ") != std::string::npos) return true;
    if (s.find(':') != std::string::npos && words > 5) return true;
    return false;
}

bool isStrongQuestTitle(const std::string& s) {
    if (!isReadableQuestText(s, 6, 72)) return false;
    if (looksLikeQuestDescriptionText(s)) return false;
    return isUpperFirst(s);
}

int scoreQuestTitle(const std::string& s) {
    if (!isReadableQuestText(s, 4, 72)) return -1000;
    if (looksLikeQuestDescriptionText(s)) return -1000;
    int score = 0;
    score += static_cast<int>(std::min<size_t>(s.size(), 32));
    if (isUpperFirst(s)) score += 20;
    if (isLowerFirst(s)) score -= 20;
    if (s.find(' ') != std::string::npos) score += 8;
    if (s.find('.') != std::string::npos) score -= 18;
    if (s.find('!') != std::string::npos || s.find('?') != std::string::npos) score -= 6;
    return score;
}

bool readCStringAt(const std::vector<uint8_t>& data, size_t start, std::string& out, size_t& nextPos) {
    out.clear();
    if (start >= data.size()) return false;
    size_t i = start;
    while (i < data.size()) {
        uint8_t b = data[i++];
        if (b == 0) {
            nextPos = i;
            return true;
        }
        out.push_back(static_cast<char>(b));
    }
    return false;
}

struct QuestQueryTextCandidate {
    std::string title;
    std::string objectives;
    std::string description;
    std::string completionText;
    int score = -1000;
};

std::string normalizeQuestText(std::string s, bool singleLine) {
    s = game::normalizeWowTextTokens(std::move(s));
    if (singleLine) {
        std::string out;
        out.reserve(s.size());
        bool lastSpace = false;
        for (char ch : s) {
            if (ch == '\n' || ch == '\r' || ch == ' ') {
                if (!lastSpace && !out.empty()) out.push_back(' ');
                lastSpace = true;
            } else {
                out.push_back(ch);
                lastSpace = false;
            }
        }
        while (!out.empty() && out.back() == ' ') out.pop_back();
        s = std::move(out);
    }
    return s;
}

bool questStringRunFitsObjectives(const std::vector<uint8_t>& data, size_t stringsStart, int nStrings,
                                  bool wotlkLayout);

// upstream pickQuestCompletedLine = questCompletedText (quest_text.hpp) with
// this file's readability test (PSP: UTF-8 aware).
std::string pickQuestCompletedLine(const std::string& endText, const std::string& completedText) {
    constexpr size_t kMin = 8;
    constexpr size_t kMax = 600;
    std::string chosen;
    if (isReadableQuestText(completedText, kMin, kMax)) chosen = completedText;
    else if (isReadableQuestText(endText, kMin, kMax)) chosen = endText;
    if (chosen.empty()) return {};
    return normalizeQuestText(std::move(chosen), false);
}

QuestQueryTextCandidate pickBestQuestQueryTexts(const std::vector<uint8_t>& data, bool classicHint) {
    QuestQueryTextCandidate best;
    if (data.size() <= 9) return best;
    std::vector<size_t> seedOffsets;
    const size_t base = 8;
    const size_t classicOffset = base + 40u * 4u;
    const size_t wotlkOffset = game::kWotlkQuestQueryStringsOffset;
    if (classicHint) {
        seedOffsets.push_back(classicOffset);
        seedOffsets.push_back(wotlkOffset);
    } else {
        seedOffsets.push_back(wotlkOffset);
        seedOffsets.push_back(classicOffset);
    }
    for (size_t off : seedOffsets) {
        if (off < data.size()) {
            std::string title;
            size_t next = off;
            if (readCStringAt(data, off, title, next)) {
                QuestQueryTextCandidate c;
                c.title = normalizeQuestText(title, true);
                c.score = scoreQuestTitle(c.title) + 20;  // Prefer expected struct offsets
                const bool wotlkSeed = (off == wotlkOffset);
                if (questStringRunFitsObjectives(data, off, wotlkSeed ? 5 : 4, wotlkSeed)) c.score += 100;
                std::string s2;
                size_t n2 = next;
                if (readCStringAt(data, next, s2, n2) && isReadableQuestText(s2, 8, 600)) {
                    c.objectives = normalizeQuestText(s2, false);
                }
                {
                    std::string s3, s4, s5;
                    size_t n3 = n2, n4 = n2, n5 = n2;
                    if (readCStringAt(data, n2, s3, n3)) {
                        if (isReadableQuestText(s3, 8, 4096)) c.description = normalizeQuestText(s3, false);
                        if (readCStringAt(data, n3, s4, n4)) {
                            readCStringAt(data, n4, s5, n5);
                            c.completionText = pickQuestCompletedLine(s4, s5);
                        }
                    }
                }
                if (c.score > best.score) best = c;
            }
        }
    }
    // Fallback: scan packet for best printable C-string title candidate.
    for (size_t start = 8; start < data.size(); ++start) {
        std::string title;
        size_t next = start;
        if (!readCStringAt(data, start, title, next)) continue;
        QuestQueryTextCandidate c;
        c.title = normalizeQuestText(title, true);
        c.score = scoreQuestTitle(c.title);
        if (c.score < 0) continue;
        std::string s2, s3;
        size_t n2 = next, n3 = next;
        size_t afterObj = next;
        if (readCStringAt(data, next, s2, n2)) {
            if (isReadableQuestText(s2, 8, 600)) {
                c.objectives = normalizeQuestText(s2, false);
                afterObj = n2;
            } else if (readCStringAt(data, n2, s3, n3) && isReadableQuestText(s3, 8, 600)) {
                c.objectives = normalizeQuestText(s3, false);
                afterObj = n3;
            }
        }
        if (!c.objectives.empty()) {
            std::string d3, d4, d5;
            size_t m3 = afterObj, m4 = afterObj, m5 = afterObj;
            if (readCStringAt(data, afterObj, d3, m3)) {
                if (isReadableQuestText(d3, 8, 4096)) c.description = normalizeQuestText(d3, false);
                if (readCStringAt(data, m3, d4, m4)) {
                    readCStringAt(data, m4, d5, m5);
                    c.completionText = pickQuestCompletedLine(d4, d5);
                }
            }
        }
        if (c.score > best.score) best = c;
    }
    return best;
}

struct QuestQueryObjectives {
    struct Kill { int32_t npcOrGoId; uint32_t required; };
    struct Item { uint32_t itemId; uint32_t required; };
    std::array<Kill, 4> kills{};
    std::array<Item, 6> items{};
    bool valid = false;
};

uint32_t readU32At(const std::vector<uint8_t>& d, size_t pos) {
    return static_cast<uint32_t>(d[pos]) | (static_cast<uint32_t>(d[pos + 1]) << 8) |
           (static_cast<uint32_t>(d[pos + 2]) << 16) | (static_cast<uint32_t>(d[pos + 3]) << 24);
}

int32_t decodeQuestNpcOrGo(uint32_t raw) {
    if ((raw & 0x80000000u) != 0) return -static_cast<int32_t>(raw & 0x7FFFFFFFu);
    return static_cast<int32_t>(raw);
}

bool plausibleQuestObjective(int32_t id, uint32_t required) {
    const uint32_t magnitude = id < 0 ? static_cast<uint32_t>(-static_cast<int64_t>(id)) : static_cast<uint32_t>(id);
    return magnitude <= 0x00FFFFFFu && required <= 0x0000FFFFu;
}

QuestQueryObjectives tryParseQuestObjectivesAt(const std::vector<uint8_t>& data, size_t startPos, int nStrings,
                                               bool wotlkLayout) {
    QuestQueryObjectives out;
    size_t pos = startPos;
    for (int si = 0; si < nStrings; ++si) {
        while (pos < data.size() && data[pos] != 0) ++pos;
        if (pos >= data.size()) return out;
        ++pos;
    }
    if (wotlkLayout) {
        for (int i = 0; i < 4; ++i) {
            if (pos + 16 > data.size()) return out;
            out.kills[i].npcOrGoId = decodeQuestNpcOrGo(readU32At(data, pos));
            pos += 4;
            out.kills[i].required = readU32At(data, pos);
            pos += 4;
            pos += 8;  // ItemDrop + source count (not inventory objectives)
            if (!plausibleQuestObjective(out.kills[i].npcOrGoId, out.kills[i].required)) return {};
        }
        for (int i = 0; i < 6; ++i) {
            if (pos + 8 > data.size()) return out;
            out.items[i].itemId = readU32At(data, pos);
            pos += 4;
            out.items[i].required = readU32At(data, pos);
            pos += 4;
            if (!plausibleQuestObjective(static_cast<int32_t>(out.items[i].itemId), out.items[i].required)) return {};
        }
    } else {
        for (int i = 0; i < 4; ++i) {
            if (pos + 16 > data.size()) return out;
            out.kills[i].npcOrGoId = decodeQuestNpcOrGo(readU32At(data, pos));
            pos += 4;
            out.kills[i].required = readU32At(data, pos);
            pos += 4;
            out.items[i].itemId = readU32At(data, pos);
            pos += 4;
            out.items[i].required = readU32At(data, pos);
            pos += 4;
            if (!plausibleQuestObjective(out.kills[i].npcOrGoId, out.kills[i].required) ||
                !plausibleQuestObjective(static_cast<int32_t>(out.items[i].itemId), out.items[i].required))
                return {};
        }
    }
    out.valid = true;
    return out;
}

bool questStringRunFitsObjectives(const std::vector<uint8_t>& data, size_t stringsStart, int nStrings,
                                  bool wotlkLayout) {
    return tryParseQuestObjectivesAt(data, stringsStart, nStrings, wotlkLayout).valid;
}

QuestQueryObjectives extractQuestQueryObjectives(const std::vector<uint8_t>& data, uint8_t questLogStride) {
    if (data.size() < 16) return {};
    const size_t base = 8;
    if (questLogStride >= 5) return tryParseQuestObjectivesAt(data, base + 63u * 4u, 5, true);  // 3.3.5a
    if (questLogStride == 4) return tryParseQuestObjectivesAt(data, base + 41u * 4u, 4, false);
    return tryParseQuestObjectivesAt(data, base + 37u * 4u, 4, false);
}
// --- end of the upstream copy -------------------------------------------------

// PSP: money with the client's coin letters (GOLD_AMOUNT_SYMBOL / SILVER_ / COPPER_: "1g 5s 3c").
std::string moneyText(uint32_t copper) {
    const uint32_t g = copper / 10000u, s = (copper / 100u) % 100u, c = copper % 100u;
    const std::string cc = std::to_string(c) + cs(CS::COPPER_AMOUNT_SYMBOL);
    const std::string sc = std::to_string(s) + cs(CS::SILVER_AMOUNT_SYMBOL) + " " + cc;
    if (g) return std::to_string(g) + cs(CS::GOLD_AMOUNT_SYMBOL) + " " + sc;
    return s ? sc : cc;
}

// Lower-case ASCII only (upstream's option-text tests).
std::string lowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

}  // namespace

// --- Dispatch -----------------------------------------------------------------------

bool WorldSession::handleNpcPacket(LogicalOpcode op, network::Packet& packet) {
    if (state_ != State::InWorld) return false;
    switch (op) {
        // PSP: names for the NPC windows until task C's item cache (the
        // session's own handler below still gets the packet).
        case LogicalOpcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE: {
            network::Packet copy = packet;
            game::ItemQueryResponseData d;
            if (game::ItemQueryResponseParser::parse(copy, d) && d.entry != 0 &&
                (npcItems_.size() < kMaxItemInfos || npcItems_.count(d.entry))) {
                NpcItemInfo& info = npcItems_[d.entry];
                info.name = d.name;
                info.quality = d.quality;
                info.sellPrice = d.sellPrice;
                info.maxStack = d.maxStack;
            }
            return false;
        }
        case LogicalOpcode::SMSG_GOSSIP_MESSAGE: handleGossipMessage(packet); return true;
        case LogicalOpcode::SMSG_QUESTGIVER_QUEST_LIST: handleQuestgiverQuestList(packet); return true;
        case LogicalOpcode::SMSG_GOSSIP_COMPLETE:
            gossipWindowOpen_ = false;
            currentGossip_ = game::GossipMessageData{};
            questGreeting_.clear();
            return true;
        case LogicalOpcode::SMSG_NPC_TEXT_UPDATE: handleNpcTextUpdate(packet); return true;
        case LogicalOpcode::SMSG_QUESTGIVER_QUEST_DETAILS: handleQuestDetails(packet); return true;
        case LogicalOpcode::SMSG_QUESTLOG_FULL: {
            addSystemChatMessage(psf(PS::QUEST_LOG_FULL_COUNT, kMaxQuestLogSlots));
            // Roll back the optimistic local entries the server refused.
            for (const auto& [pendingQuestId, npcGuid] : pendingQuestAcceptNpcGuids_) {
                (void)npcGuid;
                if (findQuestLogSlotIndexFromServer(pendingQuestId) < 0)
                    std::erase_if(questLog_, [&](const QuestLogEntry& q) { return q.questId == pendingQuestId; });
            }
            pendingQuestAcceptTimeouts_.clear();
            pendingQuestAcceptNpcGuids_.clear();
            return true;
        }
        case LogicalOpcode::SMSG_QUESTGIVER_REQUEST_ITEMS: handleQuestRequestItems(packet); return true;
        case LogicalOpcode::SMSG_QUESTGIVER_OFFER_REWARD: handleQuestOfferReward(packet); return true;
        case LogicalOpcode::SMSG_QUESTGIVER_STATUS:
            if (packet.hasRemaining(9)) {
                const uint64_t npcGuid = packet.readUInt64();
                const uint8_t status = packet.readUInt8();  // 3.3.5a readQuestGiverStatus
                npcQuestStatus_[npcGuid] = static_cast<QuestGiverStatus>(status);
                if (++questStatusLogs_ <= 60)
                    PSP_LOGI(tag::Net, "quest_status npc=%08X status=%u", static_cast<unsigned>(npcGuid & 0xFFFFFFFFu),
                             static_cast<unsigned>(status));
            }
            return true;
        case LogicalOpcode::SMSG_QUESTGIVER_STATUS_MULTIPLE: {
            if (!packet.hasRemaining(4)) return true;
            const uint32_t count = packet.readUInt32();
            for (uint32_t i = 0; i < count; ++i) {
                if (!packet.hasRemaining(9)) break;
                const uint64_t npcGuid = packet.readUInt64();
                const uint8_t status = packet.readUInt8();
                npcQuestStatus_[npcGuid] = static_cast<QuestGiverStatus>(status);
                if (++questStatusLogs_ <= 60)
                    PSP_LOGI(tag::Net, "quest_status npc=%08X status=%u", static_cast<unsigned>(npcGuid & 0xFFFFFFFFu),
                             static_cast<unsigned>(status));
            }
            return true;
        }
        case LogicalOpcode::SMSG_QUESTUPDATE_FAILED:
        case LogicalOpcode::SMSG_QUESTUPDATE_FAILEDTIMER:
            if (packet.hasRemaining(4)) {
                const uint32_t questId = packet.readUInt32();
                std::string questTitle;
                for (const auto& q : questLog_)
                    if (q.questId == questId && !q.title.empty()) { questTitle = q.title; break; }
                if (op == LogicalOpcode::SMSG_QUESTUPDATE_FAILED)
                    addSystemChatMessage(questTitle.empty() ? std::string(ps(PS::QUEST_FAILED))
                                                            : psf(PS::QUEST_FAILED_TITLE, questTitle));
                else
                    addSystemChatMessage(questTitle.empty() ? std::string(ps(PS::QUEST_TIMED_OUT))
                                                            : psf(PS::QUEST_TIMED_OUT_TITLE, questTitle));
                queueSound(SessionSound::QuestFailed);  // UiSoundManager::playQuestFailed
            }
            return true;
        case LogicalOpcode::SMSG_QUESTGIVER_QUEST_FAILED:
            if (packet.hasRemaining(8)) {
                const uint32_t questId = packet.readUInt32();
                const uint32_t reason = packet.readUInt32();
                std::string questTitle;
                for (const auto& q : questLog_)
                    if (q.questId == questId && !q.title.empty()) { questTitle = q.title; break; }
                const char* reasonStr = nullptr;
                switch (reason) {
                    case 1: reasonStr = ps(PS::QUEST_FAIL_CONDITIONS); break;
                    case 2: reasonStr = ps(PS::QUEST_FAIL_INVENTORY_FULL); break;
                    case 3: reasonStr = ps(PS::QUEST_FAIL_TOO_FAR); break;
                    case 4: reasonStr = ps(PS::QUEST_FAIL_BLOCKED); break;
                    case 5: reasonStr = ps(PS::QUEST_FAIL_TIME_OF_DAY); break;
                    case 6: reasonStr = ps(PS::QUEST_FAIL_RACE); break;
                    case 7: reasonStr = ps(PS::QUEST_FAIL_CLASS); break;
                }
                const std::string who = questTitle.empty() ? std::string(ps(PS::QUEST_WORD))
                                                           : psf(PS::QUEST_TITLE_QUOTED, questTitle);
                addSystemChatMessage(reasonStr ? psf(PS::QUEST_NOT_DONE_REASON, who, reasonStr)
                                               : psf(PS::QUEST_NOT_DONE, who));
            }
            return true;
        case LogicalOpcode::SMSG_QUESTGIVER_QUEST_INVALID:
            if (packet.hasRemaining(4)) {
                const uint32_t failReason = packet.readUInt32();
                pendingTurnInRewardRequest_ = false;
                const char* reasonStr = ps(PS::QUEST_INVALID_UNKNOWN);
                switch (failReason) {
                    case 0: reasonStr = ps(PS::QUEST_INVALID_NO_QUEST); break;
                    case 1: reasonStr = ps(PS::QUEST_INVALID_LOW_LEVEL); break;
                    case 4: reasonStr = ps(PS::QUEST_INVALID_MONEY); break;
                    case 5: reasonStr = ps(PS::QUEST_INVALID_INVENTORY_FULL); break;
                    case 13: reasonStr = ps(PS::QUEST_INVALID_ALREADY_ON); break;
                    case 18: reasonStr = ps(PS::QUEST_INVALID_ALREADY_DONE); break;
                    case 19: reasonStr = ps(PS::QUEST_INVALID_TOO_MANY); break;
                }
                PSP_LOGW(tag::Net, "quest_invalid reason=%u", static_cast<unsigned>(failReason));
                if (!pendingQuestAcceptTimeouts_.empty()) {
                    std::vector<uint32_t> pendingQuestIds;
                    for (const auto& pending : pendingQuestAcceptTimeouts_) pendingQuestIds.push_back(pending.first);
                    for (uint32_t questId : pendingQuestIds) {
                        const uint64_t npcGuid = pendingQuestAcceptNpcGuids_.count(questId) != 0
                                                     ? pendingQuestAcceptNpcGuids_[questId] : 0;
                        if (failReason == 13) {
                            std::string fallbackTitle = "Quest #" + std::to_string(questId);
                            std::string fallbackObjectives;
                            if (currentQuestDetails_.questId == questId) {
                                if (!currentQuestDetails_.title.empty()) fallbackTitle = currentQuestDetails_.title;
                                fallbackObjectives = currentQuestDetails_.objectives;
                            }
                            addQuestToLocalLogIfMissing(questId, fallbackTitle, fallbackObjectives);
                            triggerQuestAcceptResync(questId, npcGuid, "already-on-quest");
                        } else if (failReason == 18) {
                            triggerQuestAcceptResync(questId, npcGuid, "already-completed");
                        }
                        clearPendingQuestAccept(questId);
                    }
                }
                if (failReason != 13 && failReason != 18)
                    addSystemChatMessage(psf(PS::QUEST_UNAVAILABLE, reasonStr));
            }
            return true;
        case LogicalOpcode::SMSG_QUESTGIVER_QUEST_COMPLETE:
            if (packet.hasRemaining(4)) {
                const uint32_t questId = packet.readUInt32();
                PSP_LOGI(tag::Net, "quest_complete id=%u", static_cast<unsigned>(questId));
                if (pendingTurnInQuestId_ == questId) {
                    pendingTurnInQuestId_ = 0;
                    pendingTurnInNpcGuid_ = 0;
                    pendingTurnInRewardRequest_ = false;
                }
                for (auto it = questLog_.begin(); it != questLog_.end(); ++it) {
                    if (it->questId == questId) {
                        // upstream: questCompleteCallback toast + UiSoundManager::playQuestComplete
                        // (PSP: the chat line; the sound is task F's API, wired at merge).
                        addSystemChatMessage(psf(PS::QUEST_TURNED_IN, it->title));
                        queueSound(SessionSound::QuestComplete);
                        questLog_.erase(it);
                        break;
                    }
                }
            }
            requeryNearbyQuestGiverStatus();
            return true;
        case LogicalOpcode::SMSG_QUESTUPDATE_ADD_KILL: {
            const size_t rem = packet.getRemainingSize();
            if (rem >= 12) {
                const uint32_t questId = packet.readUInt32();
                clearPendingQuestAccept(questId);
                const uint32_t entry = game::normalizeQuestObjectiveEntry(packet.readUInt32());
                uint32_t count = packet.readUInt32();
                uint32_t reqCount = 0;
                if (packet.hasRemaining(4)) reqCount = packet.readUInt32();
                for (auto& quest : questLog_) {
                    if (quest.questId != questId) continue;
                    if (reqCount == 0) {
                        auto it = quest.killCounts.find(entry);
                        if (it != quest.killCounts.end()) reqCount = it->second.second;
                    }
                    if (reqCount == 0) {
                        for (const auto& obj : quest.killObjectives) {
                            if (obj.npcOrGoId == 0 || obj.required == 0) continue;
                            const uint32_t objEntry =
                                static_cast<uint32_t>(obj.npcOrGoId > 0 ? obj.npcOrGoId : -obj.npcOrGoId);
                            if (objEntry == entry) {
                                reqCount = obj.required;
                                break;
                            }
                        }
                    }
                    if (reqCount == 0) reqCount = count;
                    count = std::min(count, reqCount);
                    quest.killCounts[entry] = {count, reqCount};
                    std::string creatureName;
                    if (const auto* ci = creatureInfo(entry)) creatureName = ci->name;
                    std::string progressMsg = quest.title + ": ";
                    if (!creatureName.empty()) progressMsg += creatureName + " ";
                    progressMsg += std::to_string(count) + "/" + std::to_string(reqCount);
                    addSystemChatMessage(progressMsg);
                    queueSound(SessionSound::QuestUpdate);  // UiSoundManager::playQuestUpdate
                    if (++questUpdateLogs_ <= 60)
                        PSP_LOGI(tag::Net, "quest_update id=%u obj=%u count=%u/%u kind=kill", static_cast<unsigned>(questId),
                                 static_cast<unsigned>(entry), static_cast<unsigned>(count),
                                 static_cast<unsigned>(reqCount));
                    if (reqCount > 0 && count >= reqCount) requeryNearbyQuestGiverStatus();
                    break;
                }
            } else if (rem >= 4) {
                // Swapped mapping fallback: treat as QUESTUPDATE_COMPLETE.
                const uint32_t questId = packet.readUInt32();
                clearPendingQuestAccept(questId);
                for (auto& quest : questLog_) {
                    if (quest.questId == questId) {
                        quest.complete = true;
                        addSystemChatMessage(psf(PS::QUEST_COMPLETE_TITLE, quest.title));
                        break;
                    }
                }
                requeryNearbyQuestGiverStatus();
            }
            return true;
        }
        case LogicalOpcode::SMSG_QUESTUPDATE_ADD_ITEM:
            if (packet.hasRemaining(8)) {
                const uint32_t itemId = packet.readUInt32();
                const uint32_t count = packet.readUInt32();
                npcQueryItem(itemId);
                std::string itemLabel = psf(PS::ITEM_NUMBER, itemId);
                if (const NpcItemInfo* info = npcItemInfo(itemId))
                    if (!info->name.empty()) itemLabel = info->name;
                bool updatedAny = false, maybeCompletedObjective = false;
                for (auto& quest : questLog_) {
                    if (quest.complete) continue;
                    bool tracksItem = quest.requiredItemCounts.count(itemId) > 0 || quest.itemCounts.count(itemId) > 0;
                    if (!tracksItem) {
                        for (const auto& obj : quest.itemObjectives) {
                            if (obj.itemId == itemId && obj.required > 0) {
                                quest.requiredItemCounts.emplace(itemId, obj.required);
                                tracksItem = true;
                                break;
                            }
                        }
                    }
                    if (!tracksItem) continue;
                    // The amount added by this loot, not the new absolute count.
                    const uint32_t required = quest.requiredItemCounts.count(itemId) != 0
                                                  ? quest.requiredItemCounts[itemId]
                                                  : std::numeric_limits<uint32_t>::max();
                    const uint32_t current = quest.itemCounts[itemId];
                    quest.itemCounts[itemId] = current >= required - std::min(required, count) ? required : current + count;
                    updatedAny = true;
                    const bool requiredKnown = required != std::numeric_limits<uint32_t>::max();
                    if (!requiredKnown || quest.itemCounts[itemId] >= required) maybeCompletedObjective = true;
                    if (++questUpdateLogs_ <= 60)
                        PSP_LOGI(tag::Net, "quest_update id=%u obj=%u count=%u/%u kind=item",
                                 static_cast<unsigned>(quest.questId), static_cast<unsigned>(itemId),
                                 static_cast<unsigned>(quest.itemCounts[itemId]),
                                 static_cast<unsigned>(requiredKnown ? required : 0));
                }
                addSystemChatMessage(psf(PS::QUEST_ITEM_GOT, itemLabel, count));
                queueSound(SessionSound::QuestUpdate);
                (void)updatedAny;
                if (maybeCompletedObjective) requeryNearbyQuestGiverStatus();
            }
            return true;
        case LogicalOpcode::SMSG_QUESTUPDATE_COMPLETE: {
            const size_t rem = packet.getRemainingSize();
            if (rem >= 12) {
                const uint32_t questId = packet.readUInt32();
                clearPendingQuestAccept(questId);
                const uint32_t entry = game::normalizeQuestObjectiveEntry(packet.readUInt32());
                uint32_t count = packet.readUInt32();
                uint32_t reqCount = 0;
                if (packet.hasRemaining(4)) reqCount = packet.readUInt32();
                if (reqCount == 0) reqCount = count;
                count = std::min(count, reqCount);
                for (auto& quest : questLog_) {
                    if (quest.questId == questId) {
                        quest.killCounts[entry] = {count, reqCount};
                        addSystemChatMessage(quest.title + ": " + std::to_string(count) + "/" + std::to_string(reqCount));
                        break;
                    }
                }
            } else if (rem >= 4) {
                const uint32_t questId = packet.readUInt32();
                clearPendingQuestAccept(questId);
                PSP_LOGI(tag::Net, "quest_update id=%u obj=0 count=0/0 kind=done", static_cast<unsigned>(questId));
                for (auto& quest : questLog_) {
                    if (quest.questId == questId) {
                        quest.complete = true;
                        addSystemChatMessage(psf(PS::QUEST_COMPLETE_TITLE, quest.title));
                        break;
                    }
                }
                requeryNearbyQuestGiverStatus();
            }
            return true;
        }
        case LogicalOpcode::SMSG_QUEST_QUERY_RESPONSE: handleQuestQueryResponse(packet); return true;
        case LogicalOpcode::SMSG_QUESTUPDATE_ADD_PVP_KILL:
            if (packet.hasRemaining(12)) {
                const uint32_t questId = packet.readUInt32();
                uint32_t count = packet.readUInt32();
                uint32_t reqCount = packet.readUInt32();
                constexpr uint32_t kPvpKillEntry = 0u;
                for (auto& quest : questLog_) {
                    if (quest.questId != questId) continue;
                    if (reqCount == 0) {
                        auto it = quest.killCounts.find(kPvpKillEntry);
                        if (it != quest.killCounts.end()) reqCount = it->second.second;
                    }
                    if (reqCount == 0) {
                        for (const auto& obj : quest.killObjectives)
                            if (obj.npcOrGoId == 0 && obj.required > 0) { reqCount = obj.required; break; }
                    }
                    if (reqCount == 0) reqCount = count;
                    count = std::min(count, reqCount);
                    quest.killCounts[kPvpKillEntry] = {count, reqCount};
                    addSystemChatMessage(psf(PS::QUEST_PVP_KILLS, quest.title, count, reqCount));
                    break;
                }
            }
            return true;
        // --- Vendor (upstream InventoryHandler) ---
        case LogicalOpcode::SMSG_LIST_INVENTORY: handleListInventory(packet); return true;
        case LogicalOpcode::SMSG_BUY_ITEM:
            if (packet.hasRemaining(20)) {
                packet.readUInt64();  // vendor guid
                const uint32_t vendorSlot = packet.readUInt32();
                const uint32_t newCount = packet.readUInt32();
                const uint32_t itemCount = packet.readUInt32();
                for (auto& vi : currentVendorItems_.items) {
                    if (vi.slot != vendorSlot) continue;
                    vi.maxCount = (newCount == 0xFFFFFFFFu) ? -1 : static_cast<int32_t>(newCount);
                    break;
                }
                if (pendingBuyItemId_ != 0) {
                    std::string itemLabel = psf(PS::ITEM_NUMBER, pendingBuyItemId_);
                    if (const NpcItemInfo* info = npcItemInfo(pendingBuyItemId_))
                        if (!info->name.empty()) itemLabel = info->name;
                    std::string msg = psf(PS::VENDOR_PURCHASED, itemLabel);
                    if (itemCount > 1) msg += " x" + std::to_string(itemCount);
                    addSystemChatMessage(msg);
                    PSP_LOGI(tag::Net, "vendor_buy item=%u count=%u ok=1", static_cast<unsigned>(pendingBuyItemId_),
                             static_cast<unsigned>(itemCount));
                }
                pendingBuyItemId_ = 0;
                pendingBuyItemSlot_ = 0;
            }
            return true;
        case LogicalOpcode::SMSG_BUY_FAILED:
            if (packet.hasRemaining(13)) {
                packet.readUInt64();  // vendor guid
                const uint32_t itemIdOrSlot = packet.readUInt32();
                const uint8_t errCode = packet.readUInt8();
                // AzerothCore's BuyResult codes (Item.h) with the client's
                // GlobalStrings on the error line (was upstream's table, off by one).
                CS msgKey = CS::ERR_ITEM_NOT_FOUND;
                switch (errCode) {
                    case 1: case 7: msgKey = CS::ERR_VENDOR_SOLD_OUT; break;
                    case 2: msgKey = CS::ERR_NOT_ENOUGH_MONEY; break;
                    case 4: msgKey = CS::ERR_VENDOR_HATES_YOU; break;
                    case 5: msgKey = CS::ERR_VENDOR_TOO_FAR; break;
                    case 8: msgKey = CS::ERR_ITEM_MAX_COUNT; break;
                    case 11: msgKey = CS::ERR_CANT_EQUIP_RANK; break;
                    case 12: msgKey = CS::ERR_CANT_EQUIP_REPUTATION; break;
                    default: break;
                }
                const char* msg = cs(msgKey);
                raiseInventoryError(msg, errCode);
                PSP_LOGI(tag::Net, "vendor_buy item=%u count=%u ok=0 err=%u",
                         static_cast<unsigned>(pendingBuyItemId_ ? pendingBuyItemId_ : itemIdOrSlot),
                         static_cast<unsigned>(pendingBuyCount_), static_cast<unsigned>(errCode));
                pendingBuyItemId_ = 0;
                pendingBuyItemSlot_ = 0;
            }
            return true;
        case LogicalOpcode::SMSG_SELL_ITEM:
            if (packet.hasRemaining(17)) {
                packet.readUInt64();  // vendor guid
                const uint64_t itemGuid = packet.readUInt64();
                const uint8_t result = packet.readUInt8();
                PSP_LOGI(tag::Net, "vendor_sell item=%08X ok=%d err=%u", static_cast<unsigned>(itemGuid & 0xFFFFFFFFu),
                         result == 0 ? 1 : 0, static_cast<unsigned>(result));
                // upstream: buyback bookkeeping (PSP: no buyback list without
                // task C's inventory) and the error line.
                if (result != 0) {
                    // AzerothCore SellResult -> the client's GlobalStrings, on the error line.
                    CS msgKey = CS::ERR_ITEM_NOT_FOUND;  // 1
                    switch (result) {
                        case 2: msgKey = CS::ERR_VENDOR_NOT_INTERESTED; break;
                        case 3: msgKey = CS::ERR_VENDOR_HATES_YOU; break;
                        case 4: msgKey = CS::ERR_NOT_OWNER; break;              // SELL_ERR_YOU_DONT_OWN_THAT_ITEM
                        case 6: msgKey = CS::ERR_DESTROY_NONEMPTY_BAG; break;  // SELL_ERR_ONLY_EMPTY_BAG
                        case 7: msgKey = CS::ERR_VENDOR_DOESNT_BUY; break;
                        default: break;
                    }
                    const char* msg = cs(msgKey);
                    raiseInventoryError(msg, result);
                }
            }
            return true;
        default: return false;
    }
}

// --- Update blocks -------------------------------------------------------------------

// Called for every update block before the combat code takes the own
// player's: the quest-log fields changing mark the log for a sync
// (upstream EntityController -> applyQuestStateFromFields on every player
// update), a questgiver's creation asks for its status (upstream
// EntityController, "Query quest giver status for NPCs with questgiver flag").
void WorldSession::npcUpdateBlock(const game::UpdateBlock& b) {
    using game::UpdateType;
    const bool create = b.updateType == UpdateType::CREATE_OBJECT || b.updateType == UpdateType::CREATE_OBJECT2;
    if (playerGuid_ != 0 && b.guid == playerGuid_) {
        if (!create && b.updateType != UpdateType::VALUES) return;
        const uint16_t start = game::fieldIndex(game::UF::PLAYER_QUEST_LOG_START);
        if (start == 0xFFFF) return;
        const uint32_t end = static_cast<uint32_t>(start) + kMaxQuestLogSlots * kQuestLogStride;
        for (const auto& [k, v] : b.fields) {
            (void)v;
            if (k >= start && k < end) {
                questFieldsDirty_ = true;
                break;
            }
        }
        return;
    }
    if (create && b.objectType == game::ObjectType::UNIT) {
        auto it = b.fields.find(game::fieldIndex(game::UF::UNIT_NPC_FLAGS));
        if (it != b.fields.end() && (it->second & game::NPC_FLAG_QUESTGIVER)) sendQuestGiverStatusQuery(b.guid);
    }
}

// --- Per frame -------------------------------------------------------------------------

void WorldSession::npcUpdate(float dtSec, const glm::vec3& selfPos) {
    PROF_SCOPE("quest");
    if (state_ != State::InWorld) return;
    static bool cfgRead = false;
    if (!cfgRead) {
        cfgRead = true;
        g_utf8QuestText = configGetInt("questutf8", 1) != 0;
    }
    if (questFieldsDirty_ && selfKnown()) {
        questFieldsDirty_ = false;
        applyQuestStateFromFields();
    }
    // upstream QuestHandler::tickQuestGiverStatusRequery
    if (questGiverRequeryCooldown_ > 0.0f) {
        questGiverRequeryCooldown_ -= dtSec;
        if (questGiverRequeryCooldown_ <= 0.0f) {
            questGiverRequeryCooldown_ = 0.0f;
            if (questGiverRequeryPending_) {
                questGiverRequeryPending_ = false;
                sendQuestGiverStatusQueries();
                questGiverRequeryCooldown_ = kQuestGiverRequeryIntervalSec;
            }
        }
    }
    // upstream GameHandler::update: pending quest-accept timeouts (5 s).
    for (auto it = pendingQuestAcceptTimeouts_.begin(); it != pendingQuestAcceptTimeouts_.end();) {
        it->second -= dtSec;
        if (it->second <= 0.0f) {
            const uint32_t questId = it->first;
            auto guidIt = pendingQuestAcceptNpcGuids_.find(questId);
            const uint64_t npcGuid = guidIt != pendingQuestAcceptNpcGuids_.end() ? guidIt->second : 0;
            triggerQuestAcceptResync(questId, npcGuid, "timeout");
            it = pendingQuestAcceptTimeouts_.erase(it);
            pendingQuestAcceptNpcGuids_.erase(questId);
        } else {
            ++it;
        }
    }
    // upstream: the login quest resync (10 s for the quest slot fields).
    if (loginQuestResync_) {
        loginQuestResyncTimeout_ -= dtSec;
        if (resyncQuestLogFromServerSlots(true)) {
            loginQuestResync_ = false;
        } else if (loginQuestResyncTimeout_ <= 0.0f) {
            loginQuestResync_ = false;
            PSP_LOGW(tag::Net, "quest_log_resync timeout=1");
        }
    }
    // upstream closeIfTooFar (NPC_INTERACT_MAX_DISTANCE, 2D).
    auto tooFar = [&](uint64_t npcGuid) {
        if (npcGuid == 0) return false;
        auto npc = entities_.getEntity(npcGuid);
        if (!npc) return false;
        const glm::vec3 p = core::coords::canonicalToServer(glm::vec3(npc->getX(), npc->getY(), npc->getZ()));
        const float dx = selfPos.x - p.x, dy = selfPos.y - p.y;
        const float distance = std::sqrt(dx * dx + dy * dy);
        if (distance <= game::NPC_INTERACT_MAX_DISTANCE) return false;
        PSP_LOGI(tag::Net, "npc_window_far dist=%.1f", distance);
        return true;
    };
    if (vendorWindowOpen_ && tooFar(currentVendorItems_.vendorGuid)) closeVendor();
    if (gossipWindowOpen_ && tooFar(currentGossip_.npcGuid)) closeGossip();
}

// --- Interaction -------------------------------------------------------------------------

// upstream GameHandler::interactWithNpc: a hello only to a unit with NPC flags
// (a creature with none answers with an empty default gossip).
void WorldSession::interactWithNpc(uint64_t guid) {
    if (state_ != State::InWorld || !socket_ || guid == 0) return;
    if (auto e = entities_.getEntity(guid)) {
        if (e->getField(game::fieldIndex(game::UF::UNIT_NPC_FLAGS)) == 0) {
            PSP_LOGI(tag::Net, "npc_hello skip=no_flags guid=%08X", static_cast<unsigned>(guid & 0xFFFFFFFFu));
            return;
        }
    }
    socket_->send(game::GossipHelloPacket::build(guid));
}

// PSP: the original client talks to a questgiver without the gossip flag with
// CMSG_QUESTGIVER_HELLO (AzerothCore answers CMSG_GOSSIP_HELLO only for
// UNIT_NPC_FLAG_GOSSIP); upstream always sends the gossip hello.
void WorldSession::questgiverHello(uint64_t guid) {
    if (state_ != State::InWorld || !socket_ || guid == 0) return;
    socket_->send(game::QuestgiverHelloPacket::build(guid));
}

// --- Gossip ----------------------------------------------------------------------------------

const std::string& WorldSession::getNpcText(uint32_t textId) const {
    static const std::string empty;
    auto it = npcTextCache_.find(textId);
    return it != npcTextCache_.end() ? it->second : empty;
}

void WorldSession::handleGossipMessage(network::Packet& packet) {
    if (!game::GossipMessageParser::parse(packet, currentGossip_)) return;
    if (questDetailsOpen_) return;  // Don't reopen gossip while viewing quest
    gossipWindowOpen_ = true;
    questGreeting_.clear();
    if (currentGossip_.titleTextId > 0 && !npcTextCache_.count(currentGossip_.titleTextId) && socket_) {
        network::Packet q(game::wireOpcode(LogicalOpcode::CMSG_NPC_TEXT_QUERY));
        q.writeUInt32(currentGossip_.titleTextId);
        q.writeUInt64(currentGossip_.npcGuid);
        socket_->send(q);
    }
    closeVendor();  // Close vendor if gossip opens
    classifyGossipQuests(true);
    if (++npcLogs_ <= 60)
        PSP_LOGI(tag::Net, "gossip_open npc=%08X options=%u quests=%u text=%u",
                 static_cast<unsigned>(currentGossip_.npcGuid & 0xFFFFFFFFu),
                 static_cast<unsigned>(currentGossip_.options.size()),
                 static_cast<unsigned>(currentGossip_.quests.size()), static_cast<unsigned>(currentGossip_.titleTextId));
}

void WorldSession::handleQuestgiverQuestList(network::Packet& packet) {
    if (!packet.hasRemaining(8)) return;
    game::GossipMessageData data;
    data.npcGuid = packet.readUInt64();
    questGreeting_ = game::normalizeWowTextTokens(packet.readString());
    if (packet.hasRemaining(8)) {
        (void)packet.readUInt32();  // emoteDelay
        (void)packet.readUInt32();  // emote
    }
    uint32_t questCount = 0;
    if (packet.hasRemaining(1)) questCount = packet.readUInt8();
    data.quests.reserve(questCount);
    for (uint32_t i = 0; i < questCount; ++i) {
        if (!packet.hasRemaining(12)) break;
        game::GossipQuestItem q;
        q.questId = packet.readUInt32();
        q.questIcon = packet.readUInt32();
        q.questLevel = static_cast<int32_t>(packet.readUInt32());
        if (packet.hasRemaining(5)) {  // 3.3.5a: flags + repeatable
            q.questFlags = packet.readUInt32();
            q.isRepeatable = packet.readUInt8();
        }
        q.title = game::normalizeWowTextTokens(packet.readString());
        if (q.questId != 0) data.quests.push_back(std::move(q));
    }
    currentGossip_ = std::move(data);
    gossipWindowOpen_ = true;
    closeVendor();
    classifyGossipQuests(false);
    if (++npcLogs_ <= 60)
        PSP_LOGI(tag::Net, "gossip_open npc=%08X options=0 quests=%u kind=quest_list",
                 static_cast<unsigned>(currentGossip_.npcGuid & 0xFFFFFFFFu),
                 static_cast<unsigned>(currentGossip_.quests.size()));
}

void WorldSession::classifyGossipQuests(bool updateQuestLog) {
    auto isCompletable = [](uint32_t icon) { return icon == 5 || icon == 6 || icon == 10; };
    auto isIncomplete = [](uint32_t icon) { return icon == 3 || icon == 4; };
    auto isAvailable = [](uint32_t icon) { return icon == 2 || icon == 7 || icon == 8; };
    bool hasAvailable = false, hasReward = false, hasIncomplete = false;
    for (const auto& q : currentGossip_.quests) {
        const bool completable = isCompletable(q.questIcon);
        hasAvailable |= isAvailable(q.questIcon);
        hasReward |= completable;
        hasIncomplete |= isIncomplete(q.questIcon);
        if (updateQuestLog) {
            for (auto& entry : questLog_) {
                if (entry.questId == q.questId) {
                    entry.complete = completable;
                    entry.title = normalizeQuestText(q.title, true);
                    if (q.questLevel > 0 && entry.level <= 0) entry.level = q.questLevel;
                    break;
                }
            }
        }
    }
    if (currentGossip_.npcGuid != 0) {
        QuestGiverStatus status = QuestGiverStatus::NONE;
        if (hasReward) status = QuestGiverStatus::REWARD;
        else if (hasAvailable) status = QuestGiverStatus::AVAILABLE;
        else if (hasIncomplete) status = QuestGiverStatus::INCOMPLETE;
        // Fills a gap only: the server's own status is kept.
        if (status != QuestGiverStatus::NONE) {
            const auto known = npcQuestStatus_.find(currentGossip_.npcGuid);
            if (known == npcQuestStatus_.end() || known->second == QuestGiverStatus::NONE)
                npcQuestStatus_[currentGossip_.npcGuid] = status;
        }
    }
}

void WorldSession::handleNpcTextUpdate(network::Packet& packet) {
    if (!packet.hasRemaining(4)) return;
    const uint32_t textId = packet.readUInt32();
    // 8 entries: probability(4) + text0 + text1 + lang(4) + 6 emote fields(24)
    for (int i = 0; i < 8; ++i) {
        if (!packet.hasRemaining(4)) break;
        packet.readFloat();
        if (!packet.hasData()) break;
        std::string text0 = packet.readString();
        if (!packet.hasData()) break;
        std::string text1 = packet.readString();
        if (!packet.hasRemaining(28)) break;
        packet.setReadPos(packet.getReadPos() + 28);
        if (!text0.empty() && !npcTextCache_.count(textId)) npcTextCache_[textId] = std::move(text0);
        else if (!text1.empty() && !npcTextCache_.count(textId)) npcTextCache_[textId] = std::move(text1);
    }
}

// upstream QuestHandler::selectGossipOption, with the option-text fallbacks
// (vendor / armorer -> CMSG_LIST_INVENTORY, banker, inn; not the auctioneer's and the stable's -
// the server answers those options itself).
// PSP: a ruRU realm sends its gossip in Russian, so "browse ... goods" / "repair" also match the Russian
// stems of "goods" / "repair" (server text, not display text: kept in both locales - the realm's language is
// not the client's); the other English-only matches stay as upstream (the server itself opens a vendor chosen
// from gossip).
void WorldSession::selectGossipOption(uint32_t optionId, const std::string& code) {
    if (state_ != State::InWorld || !socket_ || !gossipWindowOpen_) return;
    socket_->send(game::GossipSelectOptionPacket::build(currentGossip_.npcGuid, currentGossip_.menuId, optionId, code));
    for (size_t i = 0; i < currentGossip_.options.size(); ++i) {
        const auto& opt = currentGossip_.options[i];
        if (opt.id != optionId) continue;
        PSP_LOGI(tag::Net, "gossip_select idx=%u id=%u icon=%u", static_cast<unsigned>(i), static_cast<unsigned>(opt.id),
                 static_cast<unsigned>(opt.icon));
        const std::string& text = opt.text;
        const std::string textLower = lowerAscii(text);
        bool sentBanker = false;
        if (opt.icon == 6) {
            socket_->send(game::BankerActivatePacket::build(currentGossip_.npcGuid));
            sentBanker = true;
        }
        // no CMSG_AUCTION_HELLO here (upstream's "auction" fallback) - the server opens the house
        // itself on this CMSG_GOSSIP_SELECT_OPTION (AzerothCore / TrinityCore Player::OnGossipSelect,
        // GOSSIP_OPTION_AUCTIONEER -> SendAuctionHello) and the client's gossip select sends nothing else; the second
        // MSG_AUCTION_HELLO emptied the lists and the sell slot again.
        if (!sentBanker && (text == "GOSSIP_OPTION_BANKER" || textLower.find("deposit box") != std::string::npos))
            socket_->send(game::BankerActivatePacket::build(currentGossip_.npcGuid));
        const bool isVendor = text == "GOSSIP_OPTION_VENDOR" ||
                              (textLower.find("browse") != std::string::npos &&
                               (textLower.find("goods") != std::string::npos || textLower.find("wares") != std::string::npos)) ||
                              text.find("товар") != std::string::npos;  // PSP: ruRU
        const bool isArmorer = text == "GOSSIP_OPTION_ARMORER" || textLower.find("repair") != std::string::npos ||
                               text.find("почин") != std::string::npos;  // PSP: ruRU
        if (isVendor || isArmorer) {
            if (isArmorer) setVendorCanRepair(true);
            socket_->send(game::ListInventoryPacket::build(currentGossip_.npcGuid));
        }
        // upstream: only while this client draws the gossip window (the PSP always does).
        if (textLower.find("make this inn your home") != std::string::npos ||
            textLower.find("set your home") != std::string::npos)
            socket_->send(game::BinderActivatePacket::build(currentGossip_.npcGuid));
        // no MSG_LIST_STABLED_PETS here (upstream's "stable" / "my pet" fallback) - the server
        // sends the list itself (Player::OnGossipSelect, GOSSIP_OPTION_STABLEPET -> SendStablePet); a second list
        // reset the stable's selection, and "my pet" also matched options that are not the stable's.
        break;
    }
}

void WorldSession::selectGossipQuest(uint32_t questId) {
    if (state_ != State::InWorld || !socket_ || !gossipWindowOpen_) return;
    const QuestLogEntry* activeQuest = nullptr;
    for (const auto& q : questLog_)
        if (q.questId == questId) { activeQuest = &q; break; }
    const bool questInServerLog = findQuestLogSlotIndexFromServer(questId) >= 0;
    if (questInServerLog && !activeQuest) {
        addQuestToLocalLogIfMissing(questId, "Quest #" + std::to_string(questId), "");
        requestQuestQuery(questId, false);
        for (const auto& q : questLog_)
            if (q.questId == questId) { activeQuest = &q; break; }
    }
    PSP_LOGI(tag::Net, "gossip_quest id=%u in_log=%d", static_cast<unsigned>(questId), questInServerLog ? 1 : 0);
    if (questInServerLog) {
        pendingTurnInQuestId_ = questId;
        pendingTurnInNpcGuid_ = currentGossip_.npcGuid;
        pendingTurnInRewardRequest_ = activeQuest ? activeQuest->complete : false;
        socket_->send(game::QuestgiverCompleteQuestPacket::build(currentGossip_.npcGuid, questId));
    } else {
        pendingTurnInQuestId_ = 0;
        pendingTurnInNpcGuid_ = 0;
        pendingTurnInRewardRequest_ = false;
        socket_->send(game::QuestgiverQueryQuestPacket::build(currentGossip_.npcGuid, questId));
    }
    gossipWindowOpen_ = false;
}

void WorldSession::closeGossip() {
    gossipWindowOpen_ = false;
    currentGossip_ = game::GossipMessageData{};
    questGreeting_.clear();
}

// --- Quest details / turn-in -----------------------------------------------------------

void WorldSession::handleQuestDetails(network::Packet& packet) {
    game::QuestDetailsData data;
    if (!game::QuestDetailsParser::parse(packet, data)) {
        PSP_LOGW(tag::Net, "quest_details_bad size=%u", static_cast<unsigned>(packet.getSize()));
        return;
    }
    currentQuestDetails_ = data;
    for (auto& q : questLog_) {
        if (q.questId != data.questId) continue;
        if (!data.title.empty() && (isPlaceholderQuestTitle(q.title) || data.title.size() >= q.title.size()))
            q.title = normalizeQuestText(data.title, true);
        if (!data.objectives.empty() && (q.objectives.empty() || data.objectives.size() > q.objectives.size()))
            q.objectives = normalizeQuestText(data.objectives, false);
        break;
    }
    for (const auto& item : data.rewardChoiceItems) npcQueryItem(item.itemId);
    for (const auto& item : data.rewardItems) npcQueryItem(item.itemId);
    questDetailsOpen_ = true;
    gossipWindowOpen_ = false;
    if (++npcLogs_ <= 60)
        PSP_LOGI(tag::Net, "quest_details id=%u choices=%u rewards=%u", static_cast<unsigned>(data.questId),
                 static_cast<unsigned>(data.rewardChoiceItems.size()), static_cast<unsigned>(data.rewardItems.size()));
}

void WorldSession::acceptQuest() {
    if (!questDetailsOpen_ || state_ != State::InWorld || !socket_) return;
    const uint32_t questId = currentQuestDetails_.questId;
    if (questId == 0) return;
    const uint64_t npcGuid = currentQuestDetails_.npcGuid;
    if (pendingQuestAcceptTimeouts_.count(questId) != 0) {
        triggerQuestAcceptResync(questId, npcGuid, "duplicate-accept");
        declineQuest();
        return;
    }
    const bool inLocalLog = hasQuestInLog(questId);
    if (findQuestLogSlotIndexFromServer(questId) >= 0) {
        addQuestToLocalLogIfMissing(questId, currentQuestDetails_.title, currentQuestDetails_.objectives);
        requestQuestQuery(questId, false);
        if (npcGuid) sendQuestGiverStatusQuery(npcGuid);
        declineQuest();
        return;
    }
    if (inLocalLog) std::erase_if(questLog_, [&](const QuestLogEntry& q) { return q.questId == questId; });
    if (static_cast<int>(questLog_.size()) >= kMaxQuestLogSlots) {
        addSystemChatMessage(psf(PS::QUEST_LOG_FULL_COUNT, kMaxQuestLogSlots));
        return;
    }
    socket_->send(game::QuestgiverAcceptQuestPacket::build(npcGuid, questId));
    pendingQuestAcceptTimeouts_[questId] = 5.0f;
    pendingQuestAcceptNpcGuids_[questId] = npcGuid;
    addQuestToLocalLogIfMissing(questId, currentQuestDetails_.title, currentQuestDetails_.objectives);
    // upstream: setQuestTracked + UiSoundManager::playQuestActivate.
    queueSound(SessionSound::QuestActivate);
    PSP_LOGI(tag::Net, "quest_accept id=%u", static_cast<unsigned>(questId));
    declineQuest();  // upstream dismissQuestDetails
    if (npcGuid) sendQuestGiverStatusQuery(npcGuid);
}

// upstream dismissQuestDetails / declineQuest (no QUEST_FINISHED event: no Lua yet).
void WorldSession::declineQuest() {
    questDetailsOpen_ = false;
    currentQuestDetails_ = game::QuestDetailsData{};
}

void WorldSession::handleQuestRequestItems(network::Packet& packet) {
    game::QuestRequestItemsData data;
    if (!game::QuestRequestItemsParser::parse(packet, data)) {
        PSP_LOGW(tag::Net, "quest_request_bad size=%u", static_cast<unsigned>(packet.getSize()));
        return;
    }
    clearPendingQuestAccept(data.questId);
    if (pendingTurnInRewardRequest_ && data.questId == pendingTurnInQuestId_ && data.npcGuid == pendingTurnInNpcGuid_ &&
        data.isCompletable() && socket_) {
        socket_->send(game::QuestgiverRequestRewardPacket::build(data.npcGuid, data.questId));
        pendingTurnInRewardRequest_ = false;
    }
    currentQuestRequestItems_ = data;
    questRequestItemsOpen_ = true;
    gossipWindowOpen_ = false;
    questDetailsOpen_ = false;
    for (const auto& item : data.requiredItems) npcQueryItem(item.itemId);
    // Server-authoritative turn-in requirements.
    for (auto& q : questLog_) {
        if (q.questId != data.questId) continue;
        q.complete = data.isCompletable();
        q.requiredItemCounts.clear();
        std::string oss;
        if (!data.completionText.empty()) {
            oss += data.completionText;
            if (!data.requiredItems.empty() || data.requiredMoney > 0) oss += "\n\n";
        }
        if (!data.requiredItems.empty()) {
            oss += cs(CS::TURN_IN_ITEMS);
            for (const auto& item : data.requiredItems) {
                std::string itemLabel = psf(PS::ITEM_NUMBER, item.itemId);
                if (const NpcItemInfo* info = npcItemInfo(item.itemId))
                    if (!info->name.empty()) itemLabel = info->name;
                q.requiredItemCounts[item.itemId] = item.count;
                oss += "\n- " + itemLabel + " x" + std::to_string(item.count);
            }
        }
        if (data.requiredMoney > 0) {
            if (!data.requiredItems.empty()) oss += "\n";
            oss += std::string("\n") + cs(CS::REQUIRED_MONEY) + " " + moneyText(data.requiredMoney);
        }
        q.objectives = oss;
        break;
    }
    if (++npcLogs_ <= 60)
        PSP_LOGI(tag::Net, "quest_request id=%u completable=%d items=%u", static_cast<unsigned>(data.questId),
                 data.isCompletable() ? 1 : 0, static_cast<unsigned>(data.requiredItems.size()));
}

void WorldSession::completeQuest() {
    if (!questRequestItemsOpen_ || state_ != State::InWorld || !socket_) return;
    pendingTurnInQuestId_ = currentQuestRequestItems_.questId;
    pendingTurnInNpcGuid_ = currentQuestRequestItems_.npcGuid;
    pendingTurnInRewardRequest_ = currentQuestRequestItems_.isCompletable();
    socket_->send(game::QuestgiverCompleteQuestPacket::build(currentQuestRequestItems_.npcGuid,
                                                             currentQuestRequestItems_.questId));
    PSP_LOGI(tag::Net, "quest_turnin id=%u", static_cast<unsigned>(currentQuestRequestItems_.questId));
    questRequestItemsOpen_ = false;
    currentQuestRequestItems_ = game::QuestRequestItemsData{};
}

void WorldSession::closeQuestRequestItems() {
    pendingTurnInRewardRequest_ = false;
    questRequestItemsOpen_ = false;
    currentQuestRequestItems_ = game::QuestRequestItemsData{};
}

void WorldSession::handleQuestOfferReward(network::Packet& packet) {
    game::QuestOfferRewardData data;
    if (!game::QuestOfferRewardParser::parse(packet, data)) {
        PSP_LOGW(tag::Net, "quest_offer_bad size=%u", static_cast<unsigned>(packet.getSize()));
        return;
    }
    clearPendingQuestAccept(data.questId);
    if (pendingTurnInQuestId_ == data.questId) {
        pendingTurnInQuestId_ = 0;
        pendingTurnInNpcGuid_ = 0;
        pendingTurnInRewardRequest_ = false;
    }
    currentQuestOfferReward_ = data;
    questOfferRewardOpen_ = true;
    questRequestItemsOpen_ = false;
    gossipWindowOpen_ = false;
    questDetailsOpen_ = false;
    for (const auto& item : data.choiceRewards) npcQueryItem(item.itemId);
    for (const auto& item : data.fixedRewards) npcQueryItem(item.itemId);
    if (++npcLogs_ <= 60)
        PSP_LOGI(tag::Net, "quest_offer id=%u choices=%u fixed=%u money=%u", static_cast<unsigned>(data.questId),
                 static_cast<unsigned>(data.choiceRewards.size()), static_cast<unsigned>(data.fixedRewards.size()),
                 static_cast<unsigned>(data.rewardMoney));
}

void WorldSession::chooseQuestReward(uint32_t rewardIndex) {
    if (!questOfferRewardOpen_ || state_ != State::InWorld || !socket_) return;
    const uint64_t npcGuid = currentQuestOfferReward_.npcGuid;
    PSP_LOGI(tag::Net, "quest_reward id=%u idx=%u", static_cast<unsigned>(currentQuestOfferReward_.questId),
             static_cast<unsigned>(rewardIndex));
    socket_->send(game::QuestgiverChooseRewardPacket::build(npcGuid, currentQuestOfferReward_.questId, rewardIndex));
    pendingTurnInQuestId_ = 0;
    pendingTurnInNpcGuid_ = 0;
    closeQuestOfferReward();
    if (npcGuid) sendQuestGiverStatusQuery(npcGuid);
}

void WorldSession::closeQuestOfferReward() {
    pendingTurnInRewardRequest_ = false;
    questOfferRewardOpen_ = false;
    currentQuestOfferReward_ = game::QuestOfferRewardData{};
}

// --- Quest log -----------------------------------------------------------------------------

int WorldSession::questLogIndexOf(uint32_t questId) const {
    for (size_t i = 0; i < questLog_.size(); ++i)
        if (questLog_[i].questId == questId) return static_cast<int>(i) + 1;
    return 0;
}

bool WorldSession::hasQuestInLog(uint32_t questId) const { return questLogIndexOf(questId) != 0; }

int WorldSession::findQuestLogSlotIndexFromServer(uint32_t questId) const {
    if (questId == 0 || selfFields_.empty()) return -1;
    const uint16_t start = game::fieldIndex(game::UF::PLAYER_QUEST_LOG_START);
    if (start == 0xFFFF) return -1;
    for (int slot = 0; slot < kMaxQuestLogSlots; ++slot) {
        auto it = selfFields_.find(static_cast<uint16_t>(start + slot * kQuestLogStride));
        if (it != selfFields_.end() && it->second == questId) return slot;
    }
    return -1;
}

void WorldSession::addQuestToLocalLogIfMissing(uint32_t questId, const std::string& title,
                                               const std::string& objectives) {
    if (questId == 0 || hasQuestInLog(questId)) return;
    QuestLogEntry entry;
    entry.questId = questId;
    entry.title = title.empty() ? ("Quest #" + std::to_string(questId)) : normalizeQuestText(title, true);
    entry.objectives = normalizeQuestText(objectives, false);
    questLog_.push_back(std::move(entry));
}

bool WorldSession::resyncQuestLogFromServerSlots(bool forceQueryMetadata) {
    if (selfFields_.empty()) return false;
    const uint16_t start = game::fieldIndex(game::UF::PLAYER_QUEST_LOG_START);
    if (start == 0xFFFF) return false;
    const uint32_t areaEnd = static_cast<uint32_t>(start) + kMaxQuestLogSlots * kQuestLogStride;
    bool sawQuestArea = false;
    for (const auto& [key, value] : selfFields_) {
        (void)value;
        if (key >= start && key < areaEnd) {
            sawQuestArea = true;
            break;
        }
    }
    if (!sawQuestArea) return false;
    std::unordered_map<uint32_t, bool> serverQuestComplete, serverQuestFailed;
    for (int slot = 0; slot < kMaxQuestLogSlots; ++slot) {
        const uint16_t idField = static_cast<uint16_t>(start + slot * kQuestLogStride);
        auto it = selfFields_.find(idField);
        if (it == selfFields_.end() || it->second == 0) continue;
        const uint32_t questId = it->second;
        bool complete = false, failed = false;
        auto stateIt = selfFields_.find(static_cast<uint16_t>(idField + 1));
        if (stateIt != selfFields_.end()) {
            complete = game::isQuestSlotComplete(kQuestLogStride, stateIt->second);
            failed = game::isQuestSlotFailed(kQuestLogStride, stateIt->second);
        }
        serverQuestComplete[questId] = complete;
        serverQuestFailed[questId] = failed;
    }
    const size_t localBefore = questLog_.size();
    std::erase_if(questLog_, [&](const QuestLogEntry& q) {
        return q.questId == 0 || serverQuestComplete.count(q.questId) == 0;
    });
    const size_t removed = localBefore - questLog_.size();
    size_t added = 0;
    for (const auto& [questId, c] : serverQuestComplete) {
        (void)c;
        if (hasQuestInLog(questId)) continue;
        addQuestToLocalLogIfMissing(questId, "Quest #" + std::to_string(questId), "");
        ++added;
    }
    size_t marked = 0;
    for (auto& quest : questLog_) {
        auto it = serverQuestComplete.find(quest.questId);
        if (it == serverQuestComplete.end()) continue;
        if (it->second && !quest.complete) {
            quest.complete = true;
            ++marked;
        }
        auto fit = serverQuestFailed.find(quest.questId);
        quest.failed = fit != serverQuestFailed.end() && fit->second;
    }
    if (forceQueryMetadata)
        for (const auto& [questId, c] : serverQuestComplete) {
            (void)c;
            requestQuestQuery(questId, false);
        }
    PSP_LOGI(tag::Net, "quest_log_resync server=%u before=%u removed=%u added=%u complete=%u",
             static_cast<unsigned>(serverQuestComplete.size()), static_cast<unsigned>(localBefore),
             static_cast<unsigned>(removed), static_cast<unsigned>(added), static_cast<unsigned>(marked));
    return true;
}

// upstream applyQuestStateFromFields(lastPlayerFields): the PSP's own-player
// field store is selfFields_ (the combat block keeps it).
void WorldSession::applyQuestStateFromFields() {
    const uint16_t start = game::fieldIndex(game::UF::PLAYER_QUEST_LOG_START);
    if (start == 0xFFFF) return;
    for (int slot = 0; slot < kMaxQuestLogSlots; ++slot) {
        const uint16_t idField = static_cast<uint16_t>(start + slot * kQuestLogStride);
        auto idIt = selfFields_.find(idField);
        if (idIt == selfFields_.end() || idIt->second == 0) continue;
        const uint32_t questId = idIt->second;
        if (!hasQuestInLog(questId) && pendingQuestAcceptTimeouts_.count(questId) != 0) {
            addQuestToLocalLogIfMissing(questId, "Quest #" + std::to_string(questId), "");
            requestQuestQuery(questId, false);
            auto pendingIt = pendingQuestAcceptNpcGuids_.find(questId);
            if (pendingIt != pendingQuestAcceptNpcGuids_.end() && pendingIt->second != 0)
                sendQuestGiverStatusQuery(pendingIt->second);
            clearPendingQuestAccept(questId);
        }
        for (auto& quest : questLog_) {
            if (quest.questId == questId) {
                applyPackedKillCountsFromFields(quest);
                break;
            }
        }
        auto stateIt = selfFields_.find(static_cast<uint16_t>(idField + 1));
        if (stateIt == selfFields_.end()) continue;
        if (!game::isQuestSlotComplete(kQuestLogStride, stateIt->second)) continue;
        for (auto& quest : questLog_) {
            if (quest.questId == questId && !quest.complete) {
                quest.complete = true;
                PSP_LOGI(tag::Net, "quest_update id=%u obj=0 count=0/0 kind=fields_done", static_cast<unsigned>(questId));
                break;
            }
        }
    }
}

void WorldSession::applyPackedKillCountsFromFields(QuestLogEntry& quest) {
    if (selfFields_.empty()) return;
    const uint16_t start = game::fieldIndex(game::UF::PLAYER_QUEST_LOG_START);
    if (start == 0xFFFF) return;
    const int slot = findQuestLogSlotIndexFromServer(quest.questId);
    if (slot < 0) return;
    const uint16_t countField1 = static_cast<uint16_t>(start + slot * kQuestLogStride +
                                                       game::questObjectiveCountFieldOffset(kQuestLogStride));
    const uint16_t countField2 = static_cast<uint16_t>(countField1 + 1);
    auto f1It = selfFields_.find(countField1);
    if (f1It == selfFields_.end()) return;
    uint32_t packed2 = 0;
    auto f2It = selfFields_.find(countField2);
    if (f2It != selfFields_.end()) packed2 = f2It->second;
    const auto counts = game::decodeQuestObjectiveCounts(kQuestLogStride, f1It->second, packed2);
    for (int i = 0; i < 4; ++i) {
        const auto& obj = quest.killObjectives[i];
        if (obj.npcOrGoId == 0 || obj.required == 0) continue;
        const uint32_t entryKey = static_cast<uint32_t>(obj.npcOrGoId > 0 ? obj.npcOrGoId : -obj.npcOrGoId);
        if (counts[i] == 0 && quest.killCounts.count(entryKey)) continue;
        quest.killCounts[entryKey] = {std::min(counts[i], obj.required), obj.required};
    }
    for (int i = 0; i < 6; ++i) {
        const auto& obj = quest.itemObjectives[i];
        if (obj.itemId == 0 || obj.required == 0) continue;
        quest.requiredItemCounts.emplace(obj.itemId, obj.required);
    }
}

void WorldSession::clearPendingQuestAccept(uint32_t questId) {
    pendingQuestAcceptTimeouts_.erase(questId);
    pendingQuestAcceptNpcGuids_.erase(questId);
}

void WorldSession::triggerQuestAcceptResync(uint32_t questId, uint64_t npcGuid, const char* reason) {
    if (questId == 0 || !socket_ || state_ != State::InWorld) return;
    PSP_LOGI(tag::Net, "quest_accept_resync id=%u reason=%s", static_cast<unsigned>(questId), reason ? reason : "?");
    requestQuestQuery(questId, true);
    if (npcGuid != 0) {
        sendQuestGiverStatusQuery(npcGuid);
        socket_->send(game::QuestgiverQueryQuestPacket::build(npcGuid, questId));
    }
}

bool WorldSession::requestQuestQuery(uint32_t questId, bool force) {
    if (questId == 0 || state_ != State::InWorld || !socket_) return false;
    if (!force && pendingQuestQueryIds_.count(questId)) return false;
    network::Packet pkt(game::wireOpcode(LogicalOpcode::CMSG_QUEST_QUERY));
    pkt.writeUInt32(questId);
    socket_->send(pkt);
    pendingQuestQueryIds_.insert(questId);
    // PSP: no CMSG_QUEST_POI_QUERY - there is no world map to put the points on.
    return true;
}

void WorldSession::abandonQuest(uint32_t questId) {
    clearPendingQuestAccept(questId);
    const int localIndex = questLogIndexOf(questId) - 1;
    int slotIndex = findQuestLogSlotIndexFromServer(questId);
    if (slotIndex < 0 && localIndex >= 0) slotIndex = localIndex;
    if (slotIndex >= 0 && slotIndex < kMaxQuestLogSlots && state_ == State::InWorld && socket_) {
        network::Packet pkt(game::wireOpcode(LogicalOpcode::CMSG_QUESTLOG_REMOVE_QUEST));
        pkt.writeUInt8(static_cast<uint8_t>(slotIndex));
        socket_->send(pkt);
    }
    PSP_LOGI(tag::Net, "quest_abandon id=%u slot=%d", static_cast<unsigned>(questId), slotIndex);
    if (localIndex >= 0) questLog_.erase(questLog_.begin() + localIndex);
    sendQuestGiverStatusQueries();
}

void WorldSession::handleQuestQueryResponse(network::Packet& packet) {
    if (packet.getSize() < 8) return;
    const uint32_t questId = packet.readUInt32();
    packet.readUInt32();  // questMethod
    const std::vector<uint8_t>& data = packet.getData();
    int32_t questLevel = 0;
    if (data.size() >= 12) {
        questLevel = static_cast<int32_t>(readU32At(data, 8));
        if (questLevel < -1 || questLevel > 255) questLevel = 0;
    }
    int32_t zoneOrSort = 0;
    if (data.size() >= 20) {
        zoneOrSort = static_cast<int32_t>(readU32At(data, 16));  // 3.3.5a offset
        if (zoneOrSort < -100000 || zoneOrSort > 100000) zoneOrSort = 0;
    }
    const QuestQueryTextCandidate parsed = pickBestQuestQueryTexts(data, false);
    const QuestQueryObjectives objs = extractQuestQueryObjectives(data, kQuestLogStride);
    const game::QuestQueryRewardsData rwds = game::QuestQueryRewardsParser::parse(data, kQuestLogStride);
    for (auto& q : questLog_) {
        if (q.questId != questId) continue;
        if (questLevel != 0) q.level = questLevel;
        if (zoneOrSort != 0) q.zoneOrSort = zoneOrSort;
        const int existingScore = scoreQuestTitle(q.title);
        const bool parsedStrong = isStrongQuestTitle(parsed.title);
        const bool parsedLongEnough = parsed.title.size() >= 6;
        const bool notShorterThanExisting = isPlaceholderQuestTitle(q.title) || q.title.empty() ||
                                            parsed.title.size() + 2 >= q.title.size();
        const bool shouldReplaceTitle =
            parsed.score > -1000 && parsedStrong && parsedLongEnough && notShorterThanExisting &&
            (isPlaceholderQuestTitle(q.title) || q.title.empty() || parsed.score >= existingScore + 12);
        if (shouldReplaceTitle && !parsed.title.empty()) q.title = parsed.title;
        if (!parsed.objectives.empty() && (q.objectives.empty() || q.objectives.size() < 16))
            q.objectives = parsed.objectives;
        if (!parsed.completionText.empty()) q.completionText = parsed.completionText;
        if (!parsed.description.empty()) q.description = parsed.description;
        if (objs.valid) {
            std::unordered_set<uint32_t> validKillKeys, validItemKeys;
            for (int i = 0; i < 4; ++i) {
                q.killObjectives[i].npcOrGoId = objs.kills[i].npcOrGoId;
                q.killObjectives[i].required = objs.kills[i].required;
                if (objs.kills[i].npcOrGoId != 0 && objs.kills[i].required != 0)
                    validKillKeys.insert(static_cast<uint32_t>(objs.kills[i].npcOrGoId > 0 ? objs.kills[i].npcOrGoId
                                                                                            : -objs.kills[i].npcOrGoId));
            }
            for (int i = 0; i < 6; ++i) {
                q.itemObjectives[i].itemId = objs.items[i].itemId;
                q.itemObjectives[i].required = objs.items[i].required;
                if (objs.items[i].itemId != 0 && objs.items[i].required != 0) validItemKeys.insert(objs.items[i].itemId);
            }
            std::erase_if(q.killCounts, [&](const auto& v) { return validKillKeys.count(v.first) == 0; });
            std::erase_if(q.itemCounts, [&](const auto& v) { return validItemKeys.count(v.first) == 0; });
            std::erase_if(q.requiredItemCounts, [&](const auto& v) { return validItemKeys.count(v.first) == 0; });
            applyPackedKillCountsFromFields(q);
            for (int i = 0; i < 4; ++i) {
                const int32_t id = objs.kills[i].npcOrGoId;
                if (id == 0 || objs.kills[i].required == 0) continue;
                // PSP: game-object names are not queried (no GO cache) - a generic object word.
                if (id > 0) queryCreature(static_cast<uint32_t>(id), 0);
            }
            for (int i = 0; i < 6; ++i)
                if (objs.items[i].itemId != 0 && objs.items[i].required != 0) npcQueryItem(objs.items[i].itemId);
        }
        if (rwds.valid) {
            q.rewardMoney = rwds.rewardMoney;
            if (rwds.rewardSpellId != 0) q.rewardSpellId = rwds.rewardSpellId;
            if (rwds.xpId != 0) q.rewardXPId = rwds.xpId;
            q.rewardHonor = rwds.rewardHonor;
            q.rewardTalents = rwds.bonusTalents;
            q.rewardArenaPoints = rwds.arenaPoints;
            if (rwds.rewardTitleId != 0) q.rewardTitleId = rwds.rewardTitleId;
            for (size_t i = 0; i < 5; ++i) {
                q.factionRewards[i].factionId = rwds.factionId[i];
                q.factionRewards[i].valueId = rwds.factionValueId[i];
                q.factionRewards[i].override = rwds.factionValueOverride[i];
            }
            q.sourceItemId = rwds.sourceItemId;
            for (int i = 0; i < 4; ++i) {
                q.rewardItems[i].itemId = rwds.itemId[i];
                q.rewardItems[i].count = rwds.itemId[i] != 0 ? rwds.itemCount[i] : 0;
                if (rwds.itemId[i] != 0) npcQueryItem(rwds.itemId[i]);
            }
            for (int i = 0; i < 6; ++i) {
                q.rewardChoiceItems[i].itemId = rwds.choiceItemId[i];
                q.rewardChoiceItems[i].count = rwds.choiceItemId[i] != 0 ? rwds.choiceItemCount[i] : 0;
                if (rwds.choiceItemId[i] != 0) npcQueryItem(rwds.choiceItemId[i]);
            }
        }
        if (++npcLogs_ <= 60)
            PSP_LOGI(tag::Net, "quest_query_rx id=%u title_len=%u obj_valid=%d desc_len=%u", static_cast<unsigned>(questId),
                     static_cast<unsigned>(q.title.size()), objs.valid ? 1 : 0,
                     static_cast<unsigned>(q.description.size()));
        break;
    }
    pendingQuestQueryIds_.erase(questId);
}

// --- Quest giver status -----------------------------------------------------------------

game::QuestGiverStatus WorldSession::getQuestGiverStatus(uint64_t guid) const {
    auto it = npcQuestStatus_.find(guid);
    return it != npcQuestStatus_.end() ? it->second : QuestGiverStatus::NONE;
}

void WorldSession::sendQuestGiverStatusQuery(uint64_t guid) {
    if (!socket_ || guid == 0) return;
    network::Packet p(game::wireOpcode(LogicalOpcode::CMSG_QUESTGIVER_STATUS_QUERY));
    p.writeUInt64(guid);
    socket_->send(p);
}

void WorldSession::requeryNearbyQuestGiverStatus() {
    if (questGiverRequeryCooldown_ > 0.0f) {
        questGiverRequeryPending_ = true;
        return;
    }
    sendQuestGiverStatusQueries();
    questGiverRequeryCooldown_ = kQuestGiverRequeryIntervalSec;
}

void WorldSession::sendQuestGiverStatusQueries() {
    if (!socket_) return;
    const uint16_t fNpcFlags = game::fieldIndex(game::UF::UNIT_NPC_FLAGS);
    for (const auto& [guid, entity] : entities_.getEntities()) {
        if (entity->getType() != game::ObjectType::UNIT) continue;
        if (entity->getField(fNpcFlags) & game::NPC_FLAG_QUESTGIVER) sendQuestGiverStatusQuery(guid);
    }
}

// --- Vendor (upstream InventoryHandler) ---------------------------------------------------

void WorldSession::openVendor(uint64_t npcGuid) {
    if (state_ != State::InWorld || !socket_) return;
    socket_->send(game::ListInventoryPacket::build(npcGuid));
}

void WorldSession::closeVendor() {
    vendorWindowOpen_ = false;
    currentVendorItems_ = game::ListInventoryData{};
    pendingBuyItemId_ = 0;
    pendingBuyItemSlot_ = 0;
}

void WorldSession::handleListInventory(network::Packet& packet) {
    if (!game::ListInventoryParser::parse(packet, currentVendorItems_)) return;
    if (!currentVendorItems_.canRepair && currentVendorItems_.vendorGuid != 0) {
        if (auto e = entities_.getEntity(currentVendorItems_.vendorGuid))
            if (e->getField(game::fieldIndex(game::UF::UNIT_NPC_FLAGS)) & game::NPC_FLAG_REPAIR)
                currentVendorItems_.canRepair = true;
    }
    vendorWindowOpen_ = true;
    closeGossip();
    // upstream: auto-sell grey items / auto-repair (options, off by default;
    // PSP: need task C's bags) and the vendor greeting sound (task F).
    for (const auto& item : currentVendorItems_.items) npcQueryItem(item.itemId);
    PSP_LOGI(tag::Net, "vendor_open n=%u repair=%d npc=%08X", static_cast<unsigned>(currentVendorItems_.items.size()),
             currentVendorItems_.canRepair ? 1 : 0, static_cast<unsigned>(currentVendorItems_.vendorGuid & 0xFFFFFFFFu));
}

void WorldSession::buyItem(uint64_t vendorGuid, uint32_t itemId, uint32_t slot, uint32_t count) {
    if (state_ != State::InWorld || !socket_) return;
    pendingBuyItemId_ = itemId;
    pendingBuyItemSlot_ = slot;
    pendingBuyCount_ = count;
    socket_->send(game::BuyItemPacket::build(vendorGuid, itemId, slot, count));  // 3.3.5a trailing byte included
    PSP_LOGI(tag::Net, "vendor_buy_req item=%u slot=%u count=%u", static_cast<unsigned>(itemId),
             static_cast<unsigned>(slot), static_cast<unsigned>(count));
}

void WorldSession::sellItem(uint64_t vendorGuid, uint64_t itemGuid, uint32_t count) {
    if (state_ != State::InWorld || !socket_) return;
    socket_->send(game::SellItemPacket::build(vendorGuid, itemGuid, count));
    PSP_LOGI(tag::Net, "vendor_sell_req item=%08X count=%u", static_cast<unsigned>(itemGuid & 0xFFFFFFFFu),
             static_cast<unsigned>(count));
}

// PSP: sells the first occupied backpack slot (PLAYER_FIELD_PACK_SLOT_1 + 2 i,
// guid low / high) - the debug check of the sell path until task C's bags.
// Count 0 = the whole stack (the server's "auto sell" case: TrinityCore /
// AzerothCore HandleSellItemOpcode take count 0 as the stack); upstream
// passes the stack count from its inventory.
bool WorldSession::sellBackpackSlot(int index) {
    if (!vendorWindowOpen_ || currentVendorItems_.vendorGuid == 0) return false;
    const uint16_t base = game::fieldIndex(game::UF::PLAYER_FIELD_PACK_SLOT_1);
    if (base == 0xFFFF) return false;
    for (int i = std::max(0, index); i < 16; ++i) {
        const uint64_t lo = selfField(static_cast<uint16_t>(base + 2 * i));
        const uint64_t hi = selfField(static_cast<uint16_t>(base + 2 * i + 1));
        const uint64_t guid = lo | (hi << 32);
        if (guid == 0) continue;
        sellItem(currentVendorItems_.vendorGuid, guid, 0);
        return true;
    }
    addSystemChatMessage(ps(PS::VENDOR_NOTHING_TO_SELL));
    return false;
}

// upstream repairAll without the cost estimate (PSP: no durability data yet;
// the server charges and refuses on its own).
void WorldSession::repairAll(uint64_t vendorGuid, bool useGuildBank) {
    if (state_ != State::InWorld || !socket_) return;
    network::Packet packet(game::wireOpcode(LogicalOpcode::CMSG_REPAIR_ITEM));
    packet.writeUInt64(vendorGuid);
    packet.writeUInt64(0);
    packet.writeUInt8(useGuildBank ? 1 : 0);
    socket_->send(packet);
    PSP_LOGI(tag::Net, "vendor_repair npc=%08X", static_cast<unsigned>(vendorGuid & 0xFFFFFFFFu));
}

// the buyback ring as the server keeps it on the player (AzerothCore
// Player::AddItemToBuyBackSlot: guid fields, price and time per slot). The bank
// bag slots are followed by the 12 buyback guids (upstream applyInventoryFields);
// the prices and times sit after PLAYER_FIELD_COINAGE (+0x1F, +0x2B in 3.3.5a
// UpdateFields.h).
std::vector<WorldSession::BuybackRow> WorldSession::buybackRows() const {
    std::vector<BuybackRow> out;
    const uint16_t bankBag = game::fieldIndex(game::UF::PLAYER_FIELD_BANKBAG_SLOT_1);
    const uint16_t coinage = game::fieldIndex(game::UF::PLAYER_FIELD_COINAGE);
    if (bankBag == 0xFFFF || coinage == 0xFFFF) return out;
    const int base = bankBag + effectiveBankBagSlots_ * 2;
    for (int i = 0; i < 12; ++i) {
        const uint64_t lo = selfField(static_cast<uint16_t>(base + 2 * i));
        const uint64_t hi = selfField(static_cast<uint16_t>(base + 2 * i + 1));
        const uint64_t guid = lo | (hi << 32);
        if (guid == 0) continue;
        BuybackRow r;
        r.wireSlot = 74u + static_cast<uint32_t>(i);
        r.guid = guid;
        r.price = selfField(static_cast<uint16_t>(coinage + 0x1F + i));
        r.time = selfField(static_cast<uint16_t>(coinage + 0x2B + i));
        auto it = onlineItems_.find(guid);
        if (it != onlineItems_.end()) {
            r.itemId = it->second.entry;
            r.count = std::max<uint32_t>(1, it->second.stackCount);
        }
        out.push_back(r);
    }
    return out;
}

void WorldSession::buybackItem(uint32_t wireSlot) {
    if (state_ != State::InWorld || !socket_ || !vendorWindowOpen_ || currentVendorItems_.vendorGuid == 0) return;
    if (wireSlot < 74 || wireSlot >= 86) return;
    pendingBuyItemId_ = 0;
    pendingBuyItemSlot_ = 0;
    socket_->send(game::BuybackItemPacket::build(currentVendorItems_.vendorGuid, wireSlot));
    PSP_LOGI(tag::Net, "vendor_buyback_req slot=%u", static_cast<unsigned>(wireSlot));
}

// upstream sellItemBySlot / sellItemInBag: the whole stack (sellCountForStack).
bool WorldSession::sellItemByGuid(uint64_t itemGuid) {
    if (!vendorWindowOpen_ || currentVendorItems_.vendorGuid == 0 || itemGuid == 0) return false;
    auto it = onlineItems_.find(itemGuid);
    const uint32_t count = it != onlineItems_.end() ? std::max<uint32_t>(1, it->second.stackCount) : 0;
    sellItem(currentVendorItems_.vendorGuid, itemGuid, count);
    return true;
}

game::ItemDef WorldSession::protoItemDef(uint32_t itemId, uint32_t count) {
    OnlineItemInfo o;
    o.entry = itemId;
    o.stackCount = count;
    return buildItemDef(0, o);
}

const NpcItemInfo* WorldSession::npcItemInfo(uint32_t entry) const {
    auto it = npcItems_.find(entry);
    return it != npcItems_.end() ? &it->second : nullptr;
}

void WorldSession::npcQueryItem(uint32_t entry) {
    if (entry == 0 || !socket_ || npcItems_.count(entry) || !npcItemQueried_.insert(entry).second) return;
    socket_->send(game::ItemQueryPacket::build(entry, playerGuid_));  // upstream queryItemInfo: our guid
}

}  // namespace wowee::platform::psp
