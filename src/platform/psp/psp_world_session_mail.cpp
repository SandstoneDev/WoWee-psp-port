// the bank and the mailbox for the PSP world session - upstream
// InventoryHandler's bank and mail halves (src/game/inventory_handler.cpp: openBank, closeBank,
// buyBankSlot, depositItem, withdrawItem, handleShowBank, handleBuyBankSlotResult; openMailbox,
// closeMailbox, refreshMailList, sendMail, attachItemFromBackpack / attachItemFromBag,
// detachMailAttachment, clearMailAttachments, mailTakeMoney, mailTakeItem, mailReturnToSender,
// mailDelete, mailMarkAsRead, handleShowMailbox, handleMailListResult, handleSendMailResult),
// GameHandler::interactWithGameObject for a mailbox (CMSG_GAMEOBJ_USE, then openMailbox),
// GameHandler::getMailSenderName / getMailDisplaySubject and SocialHandler::takeInboxTextItem, without
// GameHandler. Upstream's packet builders and the 3.3.5a mail list parser are used unchanged
// (world_packets_economy.cpp: GetMailListPacket, SendMailPacket, Mail*Packet, BuyBankSlotPacket,
// AutoBankItemPacket, AutoStoreBankItemPacket, BankerActivatePacket, GameObjectUsePacket,
// PacketParsers::parseMailList).
//
// The way in: the banker's gossip option already sends CMSG_BANKER_ACTIVATE (psp_world_session_npc.cpp,
// upstream QuestHandler::selectGossipOption) and the server answers SMSG_SHOW_BANK; a mailbox is a game
// object the PSP never kept - its create block is read here for the type (GAMEOBJECT_TYPE_MAILBOX) and
// the position, and the world screen uses the nearest one on L + up (ui_world_npc.cpp).
//
// PSP differences (marked PSP: below):
//   - SMSG_BUY_BANK_SLOT_RESULT is read as the servers send it (AzerothCore / TrinityCore Player.h
//     BuyBankSlotResult: 0 too many, 1 no money, 2 not a banker, 3 OK); upstream takes 0 for success;
//   - the lines are GlobalStrings ruRU (ERR_* on the red line, ERR_MAIL_SENT as a system chat line by the
//     frame - the game line rule, psp_game_line.hpp) instead of upstream's English chat lines;
//   - the mailbox type and place come from the object's own fields (GAMEOBJECT_BYTES_1 byte 1, the
//     stationary position) - the PSP keeps no game object entities nor SMSG_GAMEOBJECT_QUERY cache;
//   - CMSG_MAIL_CREATE_TEXT_ITEM carries the open mailbox (upstream writes 0, which AzerothCore's
//     CanOpenMailBox refuses);
//   - an item already attached is not attached twice (the original locks it in the bag);
//   - no item refund warning (MAIL_LOCK_SEND_ITEMS: the PSP keeps no refund info).
#include "platform/psp/psp_world_session.hpp"

#include "core/coordinates.hpp"
#include "game/packet_parsers.hpp"
#include "game/protocol_constants.hpp"
#include "game/update_field_table.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_strings.hpp"  // the client's strings and the port's own

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace wowee::platform::psp {

using game::LogicalOpcode;

namespace {

constexpr uint8_t kGoTypeMailbox = 19;  // GAMEOBJECT_TYPE_MAILBOX
constexpr size_t kMaxMailEvents = 32;

// SMSG_SEND_MAIL_RESULT action (MailResponseType, SharedDefines.h).
constexpr uint32_t kMailSend = 0, kMailMoneyTaken = 1, kMailItemTaken = 2, kMailReturnedToSender = 3,
                   kMailDeleted = 4, kMailMadePermanent = 5;
constexpr uint32_t kMailErrEquipError = 1;  // MAIL_ERR_EQUIP_ERROR: an InventoryResult follows

// MailResponseResult -> the client's own line (GlobalStrings.lua; strings.pak). PSP: upstream mailResultText
// writes English; 5 and 17 have no ERR_MAIL_* string of their own - the nearest ones.
const char* mailErrorText(uint32_t error) {
    switch (error) {
        case 2: return CLIENT_STR(ERR_MAIL_TO_SELF);
        case 3: return CLIENT_STR(ERR_NOT_ENOUGH_MONEY);
        case 4: return CLIENT_STR(ERR_MAIL_TARGET_NOT_FOUND);
        case 5: return CLIENT_STR(ERR_PLAYER_WRONG_FACTION);
        case 6: return CLIENT_STR(ERR_MAIL_DATABASE_ERROR);
        case 14: return CLIENT_STR(ERR_RESTRICTED_ACCOUNT);
        case 15: return CLIENT_STR(ERR_MAIL_REACHED_CAP);
        case 16: return CLIENT_STR(ERR_MAIL_WRAPPED_COD);
        case 17: return CLIENT_STR(ERR_USER_SQUELCHED);
        case 18: return CLIENT_STR(ERR_MAIL_TOO_MANY_ATTACHMENTS);
        case 19: return CLIENT_STR(ERR_MAIL_INVALID_ATTACHMENT);
        case 21: return CLIENT_STR(ERR_MAIL_ATTACHMENT_EXPIRED);
        default: return nullptr;
    }
}

// The original's AUCTION_*_MAIL_SUBJECT by the subject's response field (AuctionHouseMgr MailAuctionAnswers:
// outbid, won, successful, expired, cancelled to bidder, cancelled, sale pending). PSP: upstream
// formatAuctionMailSubject writes English prefixes.
const char* auctionSubjectFormat(uint32_t response) {
    switch (response) {
        case 0: return CLIENT_STR(AUCTION_OUTBID_MAIL_SUBJECT);
        case 1: return CLIENT_STR(AUCTION_WON_MAIL_SUBJECT);
        case 2: return CLIENT_STR(AUCTION_SOLD_MAIL_SUBJECT);
        case 3: return CLIENT_STR(AUCTION_EXPIRED_MAIL_SUBJECT);
        case 4:
        case 5: return CLIENT_STR(AUCTION_REMOVED_MAIL_SUBJECT);
        case 6: return CLIENT_STR(AUCTION_INVOICE_MAIL_SUBJECT);
        default: return "%s";
    }
}

}  // namespace

// --- Dispatch ----------------------------------------------------------------------------

bool WorldSession::handleBankMailPacket(LogicalOpcode op, network::Packet& packet) {
    switch (op) {
        case LogicalOpcode::SMSG_SHOW_BANK:  // upstream handleShowBank
            if (packet.hasRemaining(8)) openBank(packet.readUInt64());
            return true;
        case LogicalOpcode::SMSG_BUY_BANK_SLOT_RESULT: {  // upstream handleBuyBankSlotResult
            if (!packet.hasRemaining(4)) return true;
            const uint32_t result = packet.readUInt32();
            PSP_LOGI(tag::Net, "bank_buy_slot result=%u slots=%d", static_cast<unsigned>(result), purchasedBankBagSlots());
            // PSP: BuyBankSlotResult as the cores define it (upstream: 0 = success).
            switch (result) {
                case 3: pushMailEvent("PLAYERBANKBAGSLOTS_CHANGED"); break;              // ERR_BANKSLOT_OK
                case 0: addUiError(CLIENT_STR(ERR_BANKSLOT_FAILED_TOO_MANY)); break;
                case 1: addUiError(CLIENT_STR(ERR_BANKSLOT_INSUFFICIENT_FUNDS)); break;
                case 2: addUiError(CLIENT_STR(ERR_BANKSLOT_NOTBANKER)); break;
                default: break;
            }
            return true;
        }
        case LogicalOpcode::SMSG_SHOW_MAILBOX:  // upstream handleShowMailbox
            if (!packet.hasRemaining(8)) return true;
            mailboxGuid_ = packet.readUInt64();
            mailboxOpen_ = true;
            pushMailEvent("MAIL_SHOW");
            PSP_LOGI(tag::Net, "mail_show guid=%08X src=server", static_cast<unsigned>(mailboxGuid_ & 0xFFFFFFFFu));
            // (setHasNewMail(false) may go here too - upstream clears it in openMailbox only)
            refreshMailList();
            return true;
        case LogicalOpcode::SMSG_MAIL_LIST_RESULT: handleMailListResult(packet); return true;
        case LogicalOpcode::SMSG_SEND_MAIL_RESULT: handleSendMailResult(packet); return true;
        default: return false;
    }
}

void WorldSession::pushMailEvent(const char* name, int64_t arg) {
    mailEvents_.push_back(MailEvent{name, arg});
    while (mailEvents_.size() > kMaxMailEvents) mailEvents_.pop_front();
}

bool WorldSession::takeMailEvent(MailEvent& out) {
    if (mailEvents_.empty()) return false;
    out = mailEvents_.front();
    mailEvents_.pop_front();
    return true;
}

// --- Bank --------------------------------------------------------------------------------

void WorldSession::openBank(uint64_t guid) {  // upstream InventoryHandler::openBank (3.3.5a: 28 + 7)
    bankerGuid_ = guid;
    bankOpen_ = true;
    effectiveBankSlots_ = 28;
    effectiveBankBagSlots_ = 7;
    pushMailEvent("BANKFRAME_OPENED");
    PSP_LOGI(tag::Net, "bank_show guid=%08X slots=%d", static_cast<unsigned>(guid & 0xFFFFFFFFu),
             purchasedBankBagSlots());
}

void WorldSession::closeBank() {
    if (!bankOpen_) return;
    bankOpen_ = false;
    bankerGuid_ = 0;
    pushMailEvent("BANKFRAME_CLOSED");
}

// PSP: the client sends this itself for a banker the player right-clicks that has no gossip menu
// (the NPC flags decide, as for the vendor in ui_world_npc.cpp); upstream sends it only from the gossip option.
void WorldSession::activateBanker(uint64_t npcGuid) {
    if (state_ != State::InWorld || !socket_ || npcGuid == 0) return;
    socket_->send(game::BankerActivatePacket::build(npcGuid));
    PSP_LOGI(tag::Net, "bank_activate guid=%08X", static_cast<unsigned>(npcGuid & 0xFFFFFFFFu));
}

void WorldSession::buyBankSlot() {
    if (state_ != State::InWorld || !socket_ || bankerGuid_ == 0) return;
    socket_->send(game::BuyBankSlotPacket::build(bankerGuid_));
}

int WorldSession::purchasedBankBagSlots() const {
    // upstream EntityController: PLAYER_BYTES_2 byte 2 -> Inventory::setPurchasedBankBagSlots.
    return static_cast<int>((selfField(game::fieldIndex(game::UF::PLAYER_BYTES_2)) >> 16) & 0xFFu);
}

void WorldSession::depositItem(uint8_t srcBag, uint8_t srcSlot) {
    if (state_ != State::InWorld || !socket_) return;
    // upstream: CMSG_AUTOBANK_ITEM - the server picks the first free slot of the whole bank (bank bags too).
    socket_->send(game::AutoBankItemPacket::build(srcBag, srcSlot));
    if (++mailLogs_ <= 80)
        PSP_LOGI(tag::Net, "bank_deposit src=%u:%u", static_cast<unsigned>(srcBag), static_cast<unsigned>(srcSlot));
}

void WorldSession::withdrawItem(uint8_t srcBag, uint8_t srcSlot) {
    if (state_ != State::InWorld || !socket_) return;
    // upstream: CMSG_AUTOSTORE_BANK_ITEM - the server picks the first free slot of all the bags.
    socket_->send(game::AutoStoreBankItemPacket::build(srcBag, srcSlot));
    if (++mailLogs_ <= 80)
        PSP_LOGI(tag::Net, "bank_withdraw src=%u:%u", static_cast<unsigned>(srcBag), static_cast<unsigned>(srcSlot));
}

// --- Mailboxes in sight -----------------------------------------------------------------

void WorldSession::bankMailUpdateBlock(const game::UpdateBlock& b) {
    using game::UpdateType;
    const bool create = b.updateType == UpdateType::CREATE_OBJECT || b.updateType == UpdateType::CREATE_OBJECT2;
    if (!create || b.objectType != game::ObjectType::GAMEOBJECT || !b.hasMovement) return;
    auto it = b.fields.find(game::fieldIndex(game::UF::GAMEOBJECT_BYTES_1));
    if (it == b.fields.end() || ((it->second >> 8) & 0xFFu) != kGoTypeMailbox) return;  // byte 1: the type
    for (auto& m : mailboxes_) {
        if (m.guid != b.guid) continue;
        m.pos = glm::vec3(b.x, b.y, b.z);
        return;
    }
    mailboxes_.push_back(MailboxObject{b.guid, glm::vec3(b.x, b.y, b.z)});
    if (mailboxes_.size() <= 40)
        PSP_LOGI(tag::Net, "mailbox_seen guid=%08X x=%.1f y=%.1f n=%u", static_cast<unsigned>(b.guid & 0xFFFFFFFFu),
                 b.x, b.y, static_cast<unsigned>(mailboxes_.size()));
}

void WorldSession::bankMailForget(uint64_t guid) {
    mailboxes_.erase(std::remove_if(mailboxes_.begin(), mailboxes_.end(),
                                    [guid](const MailboxObject& m) { return m.guid == guid; }),
                     mailboxes_.end());
}

bool WorldSession::nearestMailbox(const glm::vec3& selfPos, float maxDist, uint64_t& guid, float& dist) const {
    guid = 0;
    dist = maxDist;
    for (const auto& m : mailboxes_) {
        const glm::vec3 d = m.pos - selfPos;
        const float r = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (r <= dist) {
            dist = r;
            guid = m.guid;
        }
    }
    return guid != 0;
}

// upstream GameHandler::interactWithGameObject, the mailbox branch: CMSG_GAMEOBJ_USE, then the client opens
// the frame itself (openMailbox: CMSG_GET_MAIL_LIST). No CMSG_GAMEOBJ_REPORT_USE for a mailbox (upstream).
void WorldSession::useMailbox(uint64_t goGuid) {
    if (state_ != State::InWorld || !socket_ || goGuid == 0) return;
    socket_->send(game::GameObjectUsePacket::build(goGuid));
    openMailbox(goGuid);
}

// PSP: the client's frames close when the player walks away (upstream closeIfTooFar for the NPC windows).
void WorldSession::bankMailUpdate(float dtSec, const glm::vec3& selfPos) {
    (void)dtSec;
    if (bankOpen_ && bankerGuid_ != 0) {
        if (auto npc = entities_.getEntity(bankerGuid_)) {
            const glm::vec3 p = core::coords::canonicalToServer(glm::vec3(npc->getX(), npc->getY(), npc->getZ()));
            const float dx = selfPos.x - p.x, dy = selfPos.y - p.y;
            if (std::sqrt(dx * dx + dy * dy) > game::NPC_INTERACT_MAX_DISTANCE) {
                PSP_LOGI(tag::Net, "bank_far dist=%.1f", std::sqrt(dx * dx + dy * dy));
                closeBank();
            }
        }
    }
    if (mailboxOpen_ && mailboxGuid_ != 0) {
        for (const auto& m : mailboxes_) {
            if (m.guid != mailboxGuid_) continue;
            const float dx = selfPos.x - m.pos.x, dy = selfPos.y - m.pos.y;
            if (std::sqrt(dx * dx + dy * dy) > game::NPC_INTERACT_MAX_DISTANCE) {
                PSP_LOGI(tag::Net, "mail_far dist=%.1f", std::sqrt(dx * dx + dy * dy));
                closeMailbox();
            }
            break;
        }
    }
}

// --- Mail --------------------------------------------------------------------------------

void WorldSession::openMailbox(uint64_t guid) {
    mailboxGuid_ = guid;
    mailboxOpen_ = true;
    setHasNewMail(false);  // upstream InventoryHandler::openMailbox (the minimap's new-mail flag)
    clearMailAttachments();
    pushMailEvent("MAIL_SHOW");
    PSP_LOGI(tag::Net, "mail_show guid=%08X src=client", static_cast<unsigned>(guid & 0xFFFFFFFFu));
    refreshMailList();
}

void WorldSession::closeMailbox() {
    if (!mailboxOpen_) return;
    mailboxOpen_ = false;
    mailboxGuid_ = 0;
    clearMailAttachments();
    pushMailEvent("MAIL_CLOSED");
}

void WorldSession::refreshMailList() {
    if (!mailboxOpen_ || mailboxGuid_ == 0) return;
    if (state_ != State::InWorld || !socket_) return;
    socket_->send(game::GetMailListPacket::build(mailboxGuid_));
}

uint32_t WorldSession::mailListAgeSec() const {
    return mailListMs_ == 0 ? 0u : (logUptimeMs() - mailListMs_) / 1000u;
}

void WorldSession::handleMailListResult(network::Packet& packet) {
    // GetInboxNumItems' second value: the list's first uint32 (the parser reads it and drops it).
    const size_t at = packet.getReadPos();
    mailTotalCount_ = packet.hasRemaining(4) ? packet.readUInt32() : 0u;
    packet.setReadPos(at);
    // The 3.3.5a parser (upstream owner_.getPacketParsers() is the 3.3.5a set on the PSP).
    game::WotlkPacketParsers parsers;
    if (!parsers.parseMailList(packet, mailInbox_)) return;
    mailListMs_ = logUptimeMs();
    ++mailVersion_;
    pushMailEvent("MAIL_INBOX_UPDATE");
    uint32_t items = 0;
    for (const auto& mail : mailInbox_) {
        // upstream: a player's name is queried once; the UI reads it through getMailSenderName.
        if (mail.messageType == 0 && mail.senderGuid != 0 && !nameOf(mail.senderGuid) &&
            nameQueried_.insert(mail.senderGuid).second)
            socket_->send(game::NameQueryPacket::build(mail.senderGuid));
        if (mail.messageType == 2) {
            game::AuctionMailSubject auction;
            if (game::parseAuctionMailSubject(mail.subject, auction)) ensureItemInfo(auction.itemEntry);
        }
        if (mail.messageType == 3 && mail.senderEntry != 0) queryCreature(mail.senderEntry, 0);  // PSP: the name
        for (const auto& att : mail.attachments) {
            if (att.itemId != 0) ensureItemInfo(att.itemId);
            ++items;
        }
    }
    if (++mailLogs_ <= 80)
        PSP_LOGI(tag::Net, "mail_list shown=%u total=%u items=%u", static_cast<unsigned>(mailInbox_.size()),
                 static_cast<unsigned>(mailTotalCount_), static_cast<unsigned>(items));
}

void WorldSession::handleSendMailResult(network::Packet& packet) {
    if (!packet.hasRemaining(12)) return;
    const uint32_t mailId = packet.readUInt32();
    const uint32_t action = packet.readUInt32();
    const uint32_t error = packet.readUInt32();
    // The cores write an InventoryResult after MAIL_ERR_EQUIP_ERROR (and the item after an item taken).
    const uint32_t equipError = error == kMailErrEquipError && packet.hasRemaining(4) ? packet.readUInt32() : 0u;
    packet.skipAll();
    PSP_LOGI(tag::Net, "mail_result id=%u action=%u err=%u equip=%u", static_cast<unsigned>(mailId),
             static_cast<unsigned>(action), static_cast<unsigned>(error), static_cast<unsigned>(equipError));
    auto mailIt = std::find_if(mailInbox_.begin(), mailInbox_.end(),
                               [mailId](const game::MailMessage& m) { return m.messageId == mailId; });
    // PSP: the original's line on the red line (upstream: an English chat line).
    auto refusal = [&]() {
        if (error == kMailErrEquipError) {
            addUiError(inventoryErrorString(static_cast<uint8_t>(equipError)));
            return;
        }
        const char* why = mailErrorText(error);
        addUiError(why ? std::string(why) : OWN_FMT(MAIL_ERROR_CODE, error));
    };
    if (action == kMailSend) {
        if (error == 0) {
            clearMailAttachments();
            pushMailEvent("MAIL_SEND_SUCCESS");  // the frame says ERR_MAIL_SENT and resets (SendMailFrame_Reset)
            pushMailEvent("MAIL_SUCCESS");
        } else {
            refusal();
            pushMailEvent("MAIL_FAILED", error);
        }
    } else if (action == kMailItemTaken) {
        if (error == 0) fireBagUpdates();
        else refusal();
    } else if (action == kMailMoneyTaken) {
        if (error == 0) {
            if (mailIt != mailInbox_.end()) mailIt->money = 0;
            pushMailEvent("MAIL_INBOX_UPDATE");
        } else {
            refusal();
        }
    } else if (action == kMailDeleted || action == kMailReturnedToSender) {
        if (error == 0) {
            if (mailIt != mailInbox_.end()) mailInbox_.erase(mailIt);
            pushMailEvent("MAIL_INBOX_UPDATE");
        } else {
            refusal();
        }
    } else if (action == kMailMadePermanent && error != 0) {
        refusal();
    }
    ++mailVersion_;
    refreshMailList();  // upstream: the list again after every result
}

std::string WorldSession::getMailSenderName(const game::MailMessage& mail) {
    switch (mail.messageType) {
        case 0:  // a player: the name once its query is back
            if (!mail.senderName.empty()) return mail.senderName;
            if (mail.senderGuid == 0) break;
            if (const std::string* n = nameOf(mail.senderGuid)) return *n;
            if (nameQueried_.insert(mail.senderGuid).second && socket_)
                socket_->send(game::NameQueryPacket::build(mail.senderGuid));
            break;
        case 2:  // the auction house: PSP - its AuctionHouse.dbc name is the window's (ui.pak "mail_ah")
            return std::string();
        case 3:  // a creature
            if (const auto* info = creatureInfo(mail.senderEntry); info && !info->name.empty()) return info->name;
            queryCreature(mail.senderEntry, 0);
            break;
        default:  // 4 a game object: PSP - no game object query cache
            break;
    }
    return CLIENT_STR(UNKNOWN);
}

std::string WorldSession::getMailDisplaySubject(const game::MailMessage& mail) {
    if (mail.messageType != 2) return mail.subject;
    game::AuctionMailSubject auction;
    if (!game::parseAuctionMailSubject(mail.subject, auction)) return mail.subject;
    ensureItemInfo(auction.itemEntry);
    const auto* info = getItemInfo(auction.itemEntry);
    const std::string itemName = info && info->valid && !info->name.empty()
                                     ? info->name
                                     : OWN_FMT(ITEM_NUMBER, auction.itemEntry);
    return text::textf(auctionSubjectFormat(auction.response), itemName);  // the client's form (strings.pak)
}

void WorldSession::refuseSend(const std::string& reason, const char* logLine) {
    // upstream: the line, and MAIL_FAILED so the frame enables its Send button again.
    PSP_LOGW(tag::Net, "mail_send refused=%s", logLine);
    addUiError(reason);
    pushMailEvent("MAIL_FAILED", 0);
}

void WorldSession::sendMail(const std::string& recipient, const std::string& subject, const std::string& body,
                            uint64_t money, uint64_t cod) {
    // PSP: the refusals in the port's own words (upstream's English; the original never gets here with them).
    if (state_ != State::InWorld) {
        refuseSend(OWN_STR(MAIL_SEND_NOT_NOW), "not in world");
        return;
    }
    if (!socket_) {
        refuseSend(OWN_STR(MAIL_SEND_NO_CONNECTION), "no socket");
        return;
    }
    if (mailboxGuid_ == 0) {
        refuseSend(OWN_STR(MAIL_SEND_NOT_AT_MAILBOX), "no mailbox");
        return;
    }
    if (recipient.empty()) {
        refuseSend(CLIENT_STR(ERR_MAIL_TARGET_NOT_FOUND), "no recipient");
        return;
    }
    std::vector<uint64_t> itemGuids;
    for (const auto& att : mailAttachments_)
        if (att.occupied()) itemGuids.push_back(att.itemGuid);
    // 3.3.5a: SendMailPacket (upstream owner_.getPacketParsers()->buildSendMail), 12 attachments.
    socket_->send(game::SendMailPacket::build(mailboxGuid_, recipient, subject, body, money, cod, itemGuids));
    PSP_LOGI(tag::Net, "mail_send to_len=%u subject_len=%u body_len=%u money=%u cod=%u attachments=%u",
             static_cast<unsigned>(recipient.size()), static_cast<unsigned>(subject.size()),
             static_cast<unsigned>(body.size()), static_cast<unsigned>(money), static_cast<unsigned>(cod),
             static_cast<unsigned>(itemGuids.size()));
    clearMailAttachments();  // upstream: the draft's attachments go with the letter
}

void WorldSession::noteMailAttach() {  // upstream notifyMailComposeChanged
    ++mailVersion_;
    pushMailEvent("MAIL_SEND_INFO_UPDATE");
}

bool WorldSession::attachItemFromBackpack(int backpackIndex) {
    if (backpackIndex < 0 || backpackIndex >= inventory_->getBackpackSize()) return false;
    const auto& slot = inventory_->getBackpackSlot(backpackIndex);
    if (slot.empty()) return false;
    const uint64_t itemGuid = backpackSlotGuids_[static_cast<size_t>(backpackIndex)];
    if (itemGuid == 0) return false;
    for (const auto& a : mailAttachments_)
        if (a.itemGuid == itemGuid) return false;  // PSP: already on the letter (locked in the original)
    for (auto& a : mailAttachments_) {
        if (a.occupied()) continue;
        a.itemGuid = itemGuid;
        a.item = slot.item;
        a.srcBag = 0xFF;
        a.srcSlot = static_cast<uint8_t>(game::Inventory::NUM_EQUIP_SLOTS + backpackIndex);
        noteMailAttach();
        return true;
    }
    return false;
}

bool WorldSession::attachItemFromBag(int bagIndex, int slotIndex) {
    if (bagIndex < 0 || bagIndex >= game::Inventory::NUM_BAG_SLOTS) return false;
    if (slotIndex < 0 || slotIndex >= inventory_->getBagSize(bagIndex)) return false;
    const auto& slot = inventory_->getBagSlot(bagIndex, slotIndex);
    if (slot.empty()) return false;
    const uint64_t itemGuid = getBagItemGuid(bagIndex, slotIndex);
    if (itemGuid == 0) return false;
    for (const auto& a : mailAttachments_)
        if (a.itemGuid == itemGuid) return false;  // PSP: already on the letter
    for (auto& a : mailAttachments_) {
        if (a.occupied()) continue;
        a.itemGuid = itemGuid;
        a.item = slot.item;
        a.srcBag = static_cast<uint8_t>(game::Inventory::FIRST_BAG_EQUIP_SLOT + bagIndex);
        a.srcSlot = static_cast<uint8_t>(slotIndex);
        noteMailAttach();
        return true;
    }
    return false;
}

bool WorldSession::detachMailAttachment(int attachIndex) {
    if (attachIndex < 0 || attachIndex >= MAIL_MAX_ATTACHMENTS) return false;
    mailAttachments_[static_cast<size_t>(attachIndex)] = MailAttachSlot{};
    noteMailAttach();
    return true;
}

void WorldSession::clearMailAttachments() {
    bool any = false;
    for (auto& a : mailAttachments_) {
        any |= a.occupied();
        a = MailAttachSlot{};
    }
    if (any) noteMailAttach();
}

int WorldSession::getMailAttachmentCount() const {
    int n = 0;
    for (const auto& a : mailAttachments_)
        if (a.occupied()) ++n;
    return n;
}

bool WorldSession::isItemAttachedToMail(uint8_t bag, uint8_t slot) const {
    for (const auto& a : mailAttachments_)
        if (a.occupied() && a.srcBag == bag && a.srcSlot == slot) return true;
    return false;
}

void WorldSession::mailTakeMoney(uint32_t mailId) {
    if (state_ != State::InWorld || !socket_ || mailboxGuid_ == 0) return;
    socket_->send(game::MailTakeMoneyPacket::build(mailboxGuid_, mailId));
}

void WorldSession::mailTakeItem(uint32_t mailId, uint32_t itemGuidLow) {
    if (state_ != State::InWorld || !socket_ || mailboxGuid_ == 0) return;
    socket_->send(game::MailTakeItemPacket::build(mailboxGuid_, mailId, itemGuidLow));
}

void WorldSession::mailReturnToSender(uint32_t mailId) {
    if (state_ != State::InWorld || !socket_ || mailboxGuid_ == 0) return;
    socket_->send(game::MailReturnToSenderPacket::build(mailboxGuid_, mailId));
    pushMailEvent("CLOSE_INBOX_ITEM", mailId);  // upstream: the letter being read is this one
}

void WorldSession::mailDelete(uint32_t mailId) {
    if (state_ != State::InWorld || !socket_ || mailboxGuid_ == 0) return;
    // upstream MailDeletePacket with template 0.
    socket_->send(game::MailDeletePacket::build(mailboxGuid_, mailId, 0));
    pushMailEvent("CLOSE_INBOX_ITEM", mailId);
}

void WorldSession::mailMarkAsRead(uint32_t mailId) {
    if (state_ != State::InWorld || !socket_ || mailboxGuid_ == 0) return;
    socket_->send(game::MailMarkAsReadPacket::build(mailboxGuid_, mailId));
    // upstream: locally too (the server does not send the list again for this); the flag before the event.
    bool marked = false;
    for (auto& mail : mailInbox_) {
        if (mail.messageId != mailId || mail.read) continue;
        mail.read = true;
        mail.flags |= 0x01u;  // MAIL_CHECK_MASK_READ
        ++mailVersion_;
        pushMailEvent("MAIL_INBOX_UPDATE");
        marked = true;
        break;
    }
    // upstream's tail: no unread letter left -> the minimap's new-mail flag goes.
    if (marked) {
        const bool anyUnread = std::any_of(mailInbox_.begin(), mailInbox_.end(),
                                           [](const game::MailMessage& m) { return !m.read; });
        if (anyUnread != hasNewMail()) setHasNewMail(anyUnread);
    }
}

void WorldSession::takeInboxTextItem(uint32_t mailId) {
    if (state_ != State::InWorld || !socket_) return;
    network::Packet packet(game::wireOpcode(LogicalOpcode::CMSG_MAIL_CREATE_TEXT_ITEM));
    packet.writeUInt64(mailboxGuid_);  // PSP: the open mailbox (upstream 0 - AzerothCore refuses it)
    packet.writeUInt32(mailId);
    socket_->send(packet);
}

}  // namespace wowee::platform::psp
