// Talking to NPCs on the world screen - gossip, vendors, quests.
//
// L + D-pad up (free in the layout) = upstream's right-click on a
// unit (GameScreen): the target (or, PSP, the nearest NPC in reach when there
// is none) -> a dead unit is left to the loot hook (task E), a unit with NPC
// flags gets the hello. PSP (wowee.cfg npchello=1, default): the hello is
// chosen by the NPC flags - the lowest flag bit with a window of its own
// (gossip -> CMSG_GOSSIP_HELLO, questgiver -> CMSG_QUESTGIVER_HELLO, vendor ->
// CMSG_LIST_INVENTORY, ...: the port's own order), because AzerothCore answers
// each service by its own opcode; npchello=0 = upstream (always CMSG_GOSSIP_HELLO).
// PSP: no hello past wowee.cfg npcrange yards (the server refuses silently
// beyond its INTERACTION_DISTANCE; upstream sends and logs the silence).
//
// The windows are a debug check of the session's NPC layer (task A's
// FrameXML replaces them): they live in the window stack
// (ui_world_windows.cpp), open by themselves when the server answers (the
// cursor comes up, as for the death window), Cross = choose, Circle = close
// (which tells the session: closeGossip / closeVendor / declineQuest / ...).
// Texts are the server's with upstream's $-tokens resolved for our character.
// The ! / ? marks over questgivers follow upstream's questGiverMarker.
#include "ui/psp/ui_world_screen.hpp"

#include "core/coordinates.hpp"
#include "game/protocol_constants.hpp"
#include "game/quest_giver_status.hpp"
#include "game/text_tokens.hpp"
#include "game/update_field_table.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"

#include <pspctrl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
using Life = pp::WorldSession::Life;

namespace {

// Widget ids: task D's range 0x6100-0x61FF.
enum : uint32_t {
    kIdTextUp = 0x6101,
    kIdTextDown,
    kIdPagePrev,
    kIdPageNext,
    kIdGoodbye,
    kIdSell,
    kIdRepair,
    kIdAccept,
    kIdDecline,
    kIdComplete,
    kIdCancel,
    kIdTakeReward,
    kIdAbandon,
    kIdRow0 = 0x6120,     // list rows 0x6120..0x612F
    kIdChoice0 = 0x6130,  // reward choices 0x6130..0x6135
};
constexpr int kRowsGossip = 16, kRowsVendor = 6, kRowsLog = 7;  // gossip scrolls, 16 rows
constexpr float kRowH = 22.0f;
const Rect kPanel{40, 12, 400, 212};  // above the bar (y 228)

// "1g 5s 3c" with the client's coin letters (GOLD_AMOUNT_SYMBOL / SILVER_ / COPPER_).
std::string money(uint32_t copper) {
    const unsigned g = copper / 10000u, s = (copper / 100u) % 100u, c = copper % 100u;
    const std::string cc = std::to_string(c) + CLIENT_STR(COPPER_AMOUNT_SYMBOL);
    const std::string sc = std::to_string(s) + CLIENT_STR(SILVER_AMOUNT_SYMBOL) + " " + cc;
    if (g) return std::to_string(g) + CLIENT_STR(GOLD_AMOUNT_SYMBOL) + " " + sc;
    return s ? sc : cc;
}

// Item quality colours (0xAABBGGRR), upstream's ITEM_QUALITY_COLORS.
uint32_t qualityColor(uint32_t q) {
    switch (q) {
        case 0: return 0xFF9D9D9Du;
        case 2: return 0xFF00FF1Eu;
        case 3: return 0xFFDD7000u;
        case 4: return 0xFFEE35A3u;
        case 5: return 0xFF0080FFu;
        default: return kColWhite;
    }
}

// Word wrap (UTF-8, the small font's advances; '\n' breaks), as the chat window.
std::vector<std::string> wrap(const Font& f, const std::string& text, float maxW) {
    std::vector<std::string> out;
    std::string cur;
    float w = 0.0f;
    size_t breakAt = 0;
    float breakW = 0.0f;
    const char* s = text.c_str();
    while (*s) {
        const char* p0 = s;
        const uint32_t cp = utf8Next(&s);
        if (cp == '\r') continue;
        if (cp == '\n') {
            out.push_back(cur);
            cur.clear();
            w = 0.0f;
            breakAt = 0;
            breakW = 0.0f;
            continue;
        }
        const Glyph* g = f.find(cp);
        const float adv = g ? static_cast<float>(g->advance) : 0.0f;
        if (w + adv > maxW && !cur.empty()) {
            if (breakAt > 0) {
                out.push_back(cur.substr(0, breakAt));
                cur.erase(0, breakAt);
                w -= breakW;
            } else {
                out.push_back(cur);
                cur.clear();
                w = 0.0f;
            }
            breakAt = 0;
            breakW = 0.0f;
            if (cur.empty() && cp == ' ') continue;
        }
        cur.append(p0, static_cast<size_t>(s - p0));
        w += adv;
        if (cp == ' ') {
            breakAt = cur.size();
            breakW = w;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

}  // namespace

// --- Per frame ---------------------------------------------------------------------

void WorldScreen::updateNpc(const pp::PadState& pad, float dtSec) {
    PROF_SCOPE("npc.dialog");
    if (!npc_.init) {
        npc_.init = true;
        npc_.hello = pp::configGetInt("npchello", 1);
        npc_.range = static_cast<float>(std::clamp(pp::configGetInt("npcrange", 6), 0, 40));
        npc_.marks = pp::configGetInt("questmarks", 1) != 0;
        npc_.markDist = static_cast<float>(std::clamp(pp::configGetInt("questmarkdist", 40), 5, 200));
        PSP_LOGI(pp::tag::Ctrl, "npc_cfg hello=%d range=%.0f marks=%d markdist=%.0f questutf8=%d", npc_.hello,
                 npc_.range, npc_.marks ? 1 : 0, npc_.markDist, pp::configGetInt("questutf8", 1));
    }
    session_.npcUpdate(dtSec, glm::vec3(playerX_, playerY_, playerZ_));
    // The game pad (idle while the cursor is up): L + D-pad up, R not held
    // (R + D-pad = emotes).
    if ((pad.held & PSP_CTRL_LTRIGGER) && !(pad.held & PSP_CTRL_RTRIGGER) && (pad.pressed & PSP_CTRL_UP))
        interactTarget();
    syncNpcWindows();
    updateMerchant();  // MerchantFrame (backpack with it, error line)
    updateBankMail(dtSec);  // BankFrame / MailFrame (session events, windows, keyboard)
    updateAuction();  // AuctionFrame (session events, windows, keyboard)
    if (npcArt_.frame.pixels && !windowOpen(kWinGossip) && !windowOpen(kWinQuestDetails) &&
        !windowOpen(kWinQuestProgress) && !windowOpen(kWinQuestReward))
        npcArt_.release(false);  // the frame art only while a conversation is open
}

// upstream GameScreen right-click on a unit + GameHandler::interactWithNpc.
void WorldScreen::interactTarget() {
    const uint32_t nowMs = pp::logUptimeMs();
    auto say = [&](const char* text) {
        npc_.error = text;
        npc_.errorUntilMs = nowMs + 2000u;
    };
    if (session_.life() == Life::Dead) return;  // upstream: only spirit healers talk to the dead
    uint64_t guid = session_.target();
    // loot first, as upstream's right-click does for a
    // dead unit (GameScreen: health 0 -> lootTarget).
    if (guid != 0 && tryLootTarget(guid)) return;
    // (Looting used to fail:) with a living target or none, the
    // nearest corpse with loot for us within a few yards is looted - the
    // corpse need not be targeted first (the PSP has no mouse to click it).
    {
        uint64_t best = 0;
        float bestD = 8.0f;
        const glm::vec3 me(playerX_, playerY_, playerZ_);
        for (uint64_t g : session_.lootableCorpses()) {
            glm::vec3 p;
            float h = 0.0f;
            if (!unitPosition(g, p, h)) continue;
            const glm::vec3 d = p - me;
            const float dist = std::sqrt(glm::dot(d, d));
            if (dist < bestD) {
                bestD = dist;
                best = g;
            }
        }
        if (best != 0 && tryLootTarget(best)) return;
    }
    const uint16_t fNpcFlags = game::fieldIndex(game::UF::UNIT_NPC_FLAGS);
    const glm::vec3 self(playerX_, playerY_, playerZ_);
    auto distTo = [&](uint64_t g) {
        glm::vec3 p;
        float h = 0.0f;
        if (!unitPosition(g, p, h)) return 1e30f;
        const glm::vec3 d = p - self;
        return std::sqrt(glm::dot(d, d));
    };
    // a mailbox in reach nearer than the target is the one used (no target: below, against the
    // nearest NPC).
    if (guid != 0 && tryUseGameObject(distTo(guid))) return;  // the hovered object, if nearer
    if (guid != 0 && tryUseMailbox(distTo(guid))) return;
    // a friendly player's two-seat mount (CMSG_PLAYER_VEHICLE_ENTER)
    if (guid != 0 && (guid >> 48) == 0 && classBarInteract(guid, 0)) return;
    // PSP: no target (or a player) - the nearest living unit with NPC flags
    // in reach becomes the target (there is no mouse to click one with).
    auto e = guid != 0 ? session_.entities().getEntity(guid) : nullptr;
    if (!e || e->getType() != game::ObjectType::UNIT) {
        uint64_t best = 0;
        float bestD = npc_.range > 0.0f ? npc_.range : 8.0f;
        for (const auto& [g, ent] : session_.entities().getEntities()) {
            if (ent->getType() != game::ObjectType::UNIT || ent->getField(fNpcFlags) == 0) continue;
            UnitView u;
            if (!unitView(g, u) || u.dead) continue;
            const float d = distTo(g);
            if (d < bestD) {
                bestD = d;
                best = g;
            }
        }
        if (tryUseGameObject(best != 0 ? bestD : 1e30f)) return;  // the hovered object
        if (tryUseMailbox(best != 0 ? bestD : 1e30f)) return;  // a mailbox nearer than any NPC
        if (best == 0) {
            say(OWN_STR(NPC_NONE_NEAR));
            return;
        }
        guid = best;
        session_.setTarget(guid);
        e = session_.entities().getEntity(guid);
        if (!e) return;
    }
    UnitView u;
    if (unitView(guid, u) && u.dead) return;  // (loot: task E's hook above)
    const uint32_t flags = e->getField(fNpcFlags);
    const float dist = distTo(guid);
    const char* how = "none";
    if (flags == 0) {
        // upstream: a living unit with no NPC flags is attacked by the
        // right-click fallback. PSP: L + D-pad right attacks; here only a hint.
        say(OWN_STR(NPC_NOTHING_TO_SAY));
    } else if (npc_.range > 0.0f && dist > npc_.range) {
        say(CLIENT_STR(ERR_OUT_OF_RANGE));
        how = "far";
    } else if (npc_.hello == 0) {
        session_.interactWithNpc(guid);  // upstream: always CMSG_GOSSIP_HELLO
        how = "gossip";
    } else {
        // The port's own rule: the lowest NPC flag bit that has a window of its own decides (the flag values are
        // AzerothCore's NPCFlags; the opcode each one is served by is the AzerothCore handler named beside it).
        // None of them: the server's gossip (it lists every service the NPC has).
        enum Hello : uint8_t { kGossip, kQuest, kVendor, kTaxi, kBank, kBattle, kAuction, kClick, kMail };
        static constexpr struct {
            uint32_t bits;
            Hello hello;
        } kOrder[] = {
            {game::NPC_FLAG_GOSSIP, kGossip},             // NPCHandler HandleGossipHelloOpcode
            {game::NPC_FLAG_QUESTGIVER, kQuest},          // QuestHandler HandleQuestgiverHelloOpcode
            {game::NPC_FLAG_ANY_VENDOR, kVendor},         // ItemHandler HandleListInventoryOpcode
            {game::NPC_FLAG_FLIGHT_MASTER, kTaxi},        // TaxiHandler HandleTaxiQueryAvailableNodes
            {game::NPC_FLAG_BANKER, kBank},               // BankHandler HandleBankerActivateOpcode
            {0x00100000u, kBattle},                       // UNIT_NPC_FLAG_BATTLEMASTER: HandleBattlemasterHelloOpcode
            {game::NPC_FLAG_AUCTIONEER, kAuction},        // AuctionHouseHandler HandleAuctionHelloOpcode
            {0x01000000u, kClick},                        // UNIT_NPC_FLAG_SPELLCLICK: SpellHandler HandleSpellClick
            {0x04000000u, kMail},                         // UNIT_NPC_FLAG_MAILBOX: MailHandler (the NPC is a mailbox)
        };
        how = nullptr;
        for (const auto& o : kOrder) {
            if (!(flags & o.bits)) continue;
            switch (o.hello) {
                case kGossip: session_.interactWithNpc(guid); how = "gossip"; break;
                case kQuest: session_.questgiverHello(guid); how = "questgiver"; break;
                case kVendor: session_.openVendor(guid); how = "vendor"; break;
                case kTaxi: session_.taxiQueryAvailableNodes(guid); how = "taxi"; break;
                case kBank: session_.activateBanker(guid); how = "banker"; break;
                case kBattle: session_.battlemasterHello(guid); how = "battlemaster"; break;
                case kAuction: session_.auctionHello(guid); how = "auctioneer"; break;
                case kClick: if (classBarInteract(guid, flags)) how = "spellclick"; break;
                case kMail: session_.openMailbox(guid); how = "mailbox"; break;
            }
            if (how) break;
        }
        if (!how) {
            session_.interactWithNpc(guid);  // trainers, innkeepers...: the server's gossip
            how = "gossip";
        }
    }
    if (++npc_.logs <= 60)
        PSP_LOGI(pp::tag::Net, "npc_interact guid=%08X flags=%X how=%s dist=%.1f",
                 static_cast<unsigned>(guid & 0xFFFFFFFFu), static_cast<unsigned>(flags), how, dist);
}

// The session says which NPC windows are open; the stack follows it. A
// window Circle took off the stack is closed in the session too.
void WorldScreen::syncNpcWindows() {
    // GossipFrame_OnEvent - a lone taxi option is chosen at once, the gossip is not shown.
    const bool want[5] = {session_.isGossipWindowOpen() && !taxiGossipAutoSelect(), session_.isVendorWindowOpen(),
                          session_.isQuestDetailsOpen(),
                          session_.isQuestRequestItemsOpen(), session_.isQuestOfferRewardOpen()};
    // Session windows in NpcUi::shown order.
    const uint8_t kSessionWins[5] = {kWinGossip, kWinVendor, kWinQuestDetails, kWinQuestProgress, kWinQuestReward};
    const uint32_t nowMs = pp::logUptimeMs();
    for (int i = 0; i < 5; ++i) {
        const uint8_t w = kSessionWins[i];
        if (want[i] && !npc_.shown[i]) {
            openWindow(w);
            npc_.shown[i] = true;
            npc_.page = 0;
            npc_.scroll = 0;
            npc_.rewardChoice = -1;
            npc_.guard = true;
            if (!uiMode_) {
                setUiMode(true);
                npc_.raisedCursor = true;
                suppress_ = ~0u;  // everything held now stays dead until released
            }
            uint32_t focus = kIdRow0;
            if (w == kWinQuestDetails) focus = kIdAccept;
            else if (w == kWinQuestProgress) focus = kIdComplete;
            else if (w == kWinQuestReward) focus = kIdTakeReward;
            winFocus_.setFocus(focus);
        } else if (want[i] && npc_.shown[i] && !windowOpen(w)) {
            npc_.shown[i] = false;  // Circle: the player closed it
            switch (i) {
                case 0: session_.closeGossip(); break;
                case 1: session_.closeVendor(); break;
                case 2:
                    session_.questPushDecline();  // a player's quest - MSG_QUEST_PUSH_RESULT 3
                    session_.declineQuest();
                    break;
                case 3: session_.closeQuestRequestItems(); break;
                default: session_.closeQuestOfferReward(); break;
            }
        } else if (!want[i] && npc_.shown[i]) {
            closeWindow(w);
            npc_.shown[i] = false;
        }
    }
    // A new gossip menu in the open window: back to its first page.
    if (want[0]) {
        const auto& g = session_.getCurrentGossip();
        const uint32_t sig = g.menuId ^ (g.titleTextId << 3) ^ (static_cast<uint32_t>(g.options.size()) << 20) ^
                             (static_cast<uint32_t>(g.quests.size()) << 26) ^ static_cast<uint32_t>(g.npcGuid);
        if (sig != npc_.gossipSig) {
            npc_.gossipSig = sig;
            npc_.page = 0;
            npc_.scroll = 0;
        }
    }
    // The cursor came up for these windows: down again once they are all gone
    // (after a short hold - the next window of a conversation is on its way).
    if (npc_.raisedCursor) {
        bool any = false;
        for (bool s : npc_.shown) any |= s;
        if (!uiMode_) {
            npc_.raisedCursor = false;
        } else if (!any && windows_.empty() && !settings_.isOpen() && nowMs >= npc_.holdCursorUntilMs) {
            setUiMode(false);
            npc_.raisedCursor = false;
            suppress_ = ~0u;  // the Circle / Cross that closed it must not walk
        }
    }
}

// --- Actions ----------------------------------------------------------------------------

bool WorldScreen::npcWindowAction(uint32_t act) {
    if (act == kIdNpcQuestLog) {
        closeWindow(kWinMenu);
        if (windowOpen(kWinQuestLog)) {
            closeWindow(kWinQuestLog);
        } else {
            openWindow(kWinQuestLog);
            npc_.abandonArmed = false;
            npc_.scroll = 0;
            winFocus_.setFocus(kIdRow0);
        }
        return true;
    }
    if (act < 0x6101 || act > 0x61FF) return false;
    // A Cross held since before the window came up: its release is not a click.
    if (npc_.guard) return true;
    // The next window of a conversation (details after a quest pick, the next
    // quest of a chain after a reward) is on its way: keep the cursor up for
    // it. Leaving the conversation lets it drop at once.
    const bool leaving = act == kIdGoodbye || act == kIdDecline || act == kIdCancel;
    npc_.holdCursorUntilMs = leaving ? 0u : pp::logUptimeMs() + 1500u;
    const uint8_t top = windows_.empty() ? 0 : windows_.back();
    auto pageRows = [&](int total, int perPage) {
        if (act == kIdPagePrev && npc_.page > 0) --npc_.page;
        if (act == kIdPageNext && (npc_.page + 1) * perPage < total) ++npc_.page;
    };
    // The frame scrolls in pixels (the plain windows in lines).
    const bool pixelScroll = npcArt_.frame.pixels && (top == kWinGossip || top == kWinQuestDetails ||
                                                      top == kWinQuestProgress || top == kWinQuestReward);
    if (act == kIdTextUp) {
        npc_.scroll = std::max(0, npc_.scroll - (pixelScroll ? 36 : 3));
        return true;
    }
    if (act == kIdTextDown) {
        npc_.scroll += pixelScroll ? 36 : 3;  // clamped when drawn
        return true;
    }
    if (act == 0x6140) {  // the frame's close button = Circle (the session closes with it)
        if (top) closeWindow(top);
        return true;
    }
    if (top == kWinGossip) {
        const auto& g = session_.getCurrentGossip();
        const int total = static_cast<int>(g.quests.size() + g.options.size());
        pageRows(total, kRowsGossip);
        if (act == kIdGoodbye) session_.closeGossip();
        if (act >= kIdRow0 && act < kIdRow0 + kRowsGossip) {
            const int idx = npc_.page * kRowsGossip + static_cast<int>(act - kIdRow0);
            // The client's gossip frame lists the quests first, then the options.
            if (idx < static_cast<int>(g.quests.size())) {
                session_.selectGossipQuest(g.quests[static_cast<size_t>(idx)].questId);
            } else if (idx < total) {
                const auto& opt = g.options[static_cast<size_t>(idx) - g.quests.size()];
                // PSP: a coded option (box text asking for a code) is sent
                // without one - no keyboard prompt for it yet.
                session_.selectGossipOption(opt.id);
            }
        }
        return true;
    }
    if (top == kWinVendor) {
        const auto& v = session_.getVendorItems();
        pageRows(static_cast<int>(v.items.size()), kRowsVendor);
        if (act >= kIdRow0 && act < kIdRow0 + kRowsVendor) {
            const size_t idx = static_cast<size_t>(npc_.page * kRowsVendor) + (act - kIdRow0);
            if (idx < v.items.size()) {
                const auto& it = v.items[idx];
                session_.buyItem(v.vendorGuid, it.itemId, it.slot, 1);  // upstream BuyMerchantItem default count
            }
        }
        if (act == kIdSell) session_.sellBackpackSlot(0);
        if (act == kIdRepair) session_.repairAll(v.vendorGuid, false);
        return true;
    }
    if (top == kWinQuestDetails) {
        if (act == kIdAccept) session_.acceptQuest();
        if (act == kIdDecline) {
            session_.questPushDecline();  // a player's quest - MSG_QUEST_PUSH_RESULT 3
            session_.declineQuest();
        }
        return true;
    }
    if (top == kWinQuestProgress) {
        if (act == kIdComplete) session_.completeQuest();
        if (act == kIdCancel) session_.closeQuestRequestItems();
        return true;
    }
    if (top == kWinQuestReward) {
        const auto& r = session_.getQuestOfferReward();
        if (act >= kIdChoice0 && act < kIdChoice0 + 6) npc_.rewardChoice = static_cast<int>(act - kIdChoice0);
        if (act == kIdCancel) session_.closeQuestOfferReward();  // QuestFrameCancelButton
        if (act == kIdTakeReward && !r.choiceRewards.empty() &&
            (npc_.rewardChoice < 0 || npc_.rewardChoice >= static_cast<int>(r.choiceRewards.size()))) {
            npc_.error = CLIENT_STR(ERR_QUEST_MUST_CHOOSE);  // QuestRewardCompleteButton_OnClick
            npc_.errorUntilMs = pp::logUptimeMs() + 2000u;
        }
        if (act == kIdTakeReward) {
            if (r.choiceRewards.empty()) {
                session_.chooseQuestReward(0);
            } else if (npc_.rewardChoice >= 0 && npc_.rewardChoice < static_cast<int>(r.choiceRewards.size())) {
                // upstream sends the item's original reward slot (choiceSlot).
                session_.chooseQuestReward(r.choiceRewards[static_cast<size_t>(npc_.rewardChoice)].choiceSlot);
            }
        }
        return true;
    }
    if (top == kWinQuestLog) {
        const auto& log = session_.getQuestLog();
        const int total = static_cast<int>(log.size());
        if (act == kIdPagePrev && npc_.logPage > 0) --npc_.logPage;
        if (act == kIdPageNext && (npc_.logPage + 1) * kRowsLog < total) ++npc_.logPage;
        if (act >= kIdRow0 && act < kIdRow0 + kRowsLog) {
            npc_.logSel = npc_.logPage * kRowsLog + static_cast<int>(act - kIdRow0);
            npc_.scroll = 0;
            npc_.abandonArmed = false;
        }
        if (act == kIdAbandon && npc_.logSel >= 0 && npc_.logSel < total) {
            if (!npc_.abandonArmed) {
                npc_.abandonArmed = true;  // second press confirms
            } else {
                session_.abandonQuest(log[static_cast<size_t>(npc_.logSel)].questId);
                npc_.abandonArmed = false;
                npc_.logSel = 0;
            }
        }
        return true;
    }
    return false;
}

// --- Drawing ----------------------------------------------------------------------------

bool WorldScreen::drawNpcWindow(UiRenderer& ui, uint8_t w) {
    if (w != kWinGossip && w != kWinVendor && w != kWinQuestDetails && w != kWinQuestProgress &&
        w != kWinQuestReward && w != kWinQuestLog)
        return false;
    if (drawNpcFrame(ui, w)) return true;  // the original frame (gossip and quests)
    if (drawMerchantWindow(ui, w)) return true;  // MerchantFrame
    if (drawQuestLogWindow(ui, w)) return true;  // QuestLogFrame
    PROF_SCOPE("npc.dialog");
    const Font& fs = kit_.font(kFontSmall);
    const Font& fn = kit_.font(kFontNormal);
    const float lh = static_cast<float>(fs.lineHeight() > 0 ? fs.lineHeight() : 12);
    const bool top = !windows_.empty() && windows_.back() == w;
    const int slot = w == kWinQuestLog ? 5 : w - kWinGossip;

    game::TextSubject subject;  // upstream resolveTextTokens(text, gameHandler)
    if (const game::Character* me = session_.activeCharacter()) {
        subject.name = me->name;
        subject.className = game::getClassName(me->characterClass);
        subject.raceName = game::getRaceName(me->race);
        subject.gender = me->gender;
    }
    auto tokens = [&](const std::string& s) { return game::resolveTextTokens(s, subject); };
    auto itemName = [&](uint32_t id) {
        const pp::NpcItemInfo* info = session_.npcItemInfo(id);
        return info && !info->name.empty() ? info->name : OWN_FMT(ITEM_NUMBER, id);
    };
    // A focusable list row: dark strip, lighter when focused, left-aligned text.
    auto row = [&](uint32_t id, const Rect& r, const std::string& text, uint32_t col, bool enabled = true) {
        const bool focused = enabled && top && winFocus_.addStop(id, r);
        ui.fill(r, focused ? 0xA0806040u : 0x60000000u);
        ui.pushClip(r);
        ui.textShadow(fs, r.x + 4, r.y + (r.h - lh) * 0.5f, text.c_str(), enabled ? col : kColGrey);
        ui.popClip();
    };
    auto button = [&](uint32_t id, const Rect& r, const char* label, bool enabled = true) {
        kit_.button(ui, winFocus_, id, r, label, enabled && top);
    };
    // Wrapped text box with up / down buttons when it does not fit (npc_.scroll, top window only).
    auto textBox = [&](const Rect& r, const std::string& text) {
        if (npc_.wrapSrc[slot] != text) {
            npc_.wrapSrc[slot] = text;
            npc_.wrapLines[slot] = wrap(fs, text, r.w - 58.0f);
        }
        const auto& lines = npc_.wrapLines[slot];
        const int visible = std::max(1, static_cast<int>(r.h / lh));
        const int maxScroll = std::max(0, static_cast<int>(lines.size()) - visible);
        int scroll = top ? std::min(npc_.scroll, maxScroll) : 0;
        if (top) npc_.scroll = scroll;
        for (int i = 0; i < visible && scroll + i < static_cast<int>(lines.size()); ++i)
            ui.textShadow(fs, r.x, r.y + lh * static_cast<float>(i), lines[static_cast<size_t>(scroll + i)].c_str(),
                          kColWhite);
        if (maxScroll > 0) {
            button(kIdTextUp, Rect{r.x + r.w - 52, r.y, 52, 20}, OWN_STR(SCROLL_UP), scroll > 0);
            button(kIdTextDown, Rect{r.x + r.w - 52, r.y + 24, 52, 20}, OWN_STR(SCROLL_DOWN), scroll < maxScroll);
        }
    };
    auto title = [&](const Rect& r, const std::string& text) {
        ui.pushClip(Rect{r.x + 8, r.y + 4, r.w - 16, 20});
        ui.textShadow(fn, r.x + r.w * 0.5f, r.y + 8, text.c_str(), kColGold, Align::Center);
        ui.popClip();
    };
    auto npcName = [&](uint64_t guid) {
        UnitView u;
        return unitView(guid, u) && !u.name.empty() ? u.name : std::string("NPC");
    };
    auto rewardLines = [&](std::string& t, uint32_t money_, uint32_t xp, const std::vector<game::QuestRewardItem>& fixed,
                           const std::vector<game::QuestRewardItem>* choices) {
        if (money_ == 0 && xp == 0 && fixed.empty() && (!choices || choices->empty())) return;
        t += std::string("\n\n") + CLIENT_STR(QUEST_REWARDS) + ":";
        if (money_) t += std::string("\n  ") + CLIENT_STR(MONEY_COLON) + " " + money(money_);
        if (xp) t += std::string("\n  ") + CLIENT_STR(EXPERIENCE_COLON) + " " + std::to_string(xp);
        for (const auto& it : fixed)
            t += "\n  " + itemName(it.itemId) + (it.count > 1 ? " x" + std::to_string(it.count) : std::string());
        if (choices && !choices->empty()) {
            t += std::string("\n") + CLIENT_STR(REWARD_CHOICES);
            for (const auto& it : *choices)
                t += "\n  " + itemName(it.itemId) + (it.count > 1 ? " x" + std::to_string(it.count) : std::string());
        }
    };
    const Rect& r = kPanel;
    const float bottom = r.y + r.h - 30;

    if (w == kWinGossip) {
        const auto& g = session_.getCurrentGossip();
        kit_.panel(ui, r);
        title(r, npcName(g.npcGuid));
        std::string greet = session_.getQuestGreeting();
        if (greet.empty() && g.titleTextId) greet = session_.getNpcText(g.titleTextId);
        textBox(Rect{r.x + 14, r.y + 28, r.w - 28, lh * 3}, tokens(greet));
        const int total = static_cast<int>(g.quests.size() + g.options.size());
        float y = r.y + 30 + lh * 3 + 4;
        for (int i = 0; i < kRowsGossip; ++i) {
            const int idx = npc_.page * kRowsGossip + i;
            if (idx >= total) break;
            std::string text;
            uint32_t col = kColWhite;
            if (idx < static_cast<int>(g.quests.size())) {
                const auto& q = g.quests[static_cast<size_t>(idx)];
                // Quest icons as upstream classifyGossipQuests: 5/6/10 turn-in,
                // 3/4 in progress, 2/7/8 available.
                const bool reward = q.questIcon == 5 || q.questIcon == 6 || q.questIcon == 10;
                const bool incomplete = q.questIcon == 3 || q.questIcon == 4;
                text = std::string(reward || incomplete ? "? " : "! ") + tokens(q.title);
                if (q.questLevel > 0) text += " (" + std::to_string(q.questLevel) + ")";
                col = incomplete ? kColGrey : kColGold;
            } else {
                const auto& o = g.options[static_cast<size_t>(idx) - g.quests.size()];
                text = tokens(o.text);
                if (o.boxMoney) text += " (" + money(o.boxMoney) + ")";
            }
            row(kIdRow0 + static_cast<uint32_t>(i), Rect{r.x + 14, y, r.w - 28, kRowH - 2}, text, col);
            y += kRowH;
        }
        button(kIdPagePrev, Rect{r.x + 14, bottom, 60, 22}, "<", npc_.page > 0);
        button(kIdGoodbye, Rect{r.x + r.w * 0.5f - 60, bottom, 120, 22}, CLIENT_STR(GOODBYE));
        button(kIdPageNext, Rect{r.x + r.w - 74, bottom, 60, 22}, ">", (npc_.page + 1) * kRowsGossip < total);
    } else if (w == kWinVendor) {
        const auto& v = session_.getVendorItems();
        kit_.panel(ui, r);
        title(r, npcName(v.vendorGuid));
        const std::string cash = std::string(CLIENT_STR(MONEY_COLON)) + " " + money(session_.selfField(game::fieldIndex(game::UF::PLAYER_FIELD_COINAGE)));
        ui.textShadow(fs, r.x + r.w - 14, r.y + 28, cash.c_str(), kColWhite, Align::Right);
        ui.textShadow(fs, r.x + 14, r.y + 28, OWN_STR(NPC_VENDOR_BUY_HINT), kColGrey);
        float y = r.y + 44;
        const int total = static_cast<int>(v.items.size());
        for (int i = 0; i < kRowsVendor; ++i) {
            const int idx = npc_.page * kRowsVendor + i;
            if (idx >= total) break;
            const auto& it = v.items[static_cast<size_t>(idx)];
            const pp::NpcItemInfo* info = session_.npcItemInfo(it.itemId);
            std::string text = itemName(it.itemId);
            if (it.stackCount > 1) text += " x" + std::to_string(it.stackCount);
            if (it.maxCount >= 0) text += " [" + std::to_string(it.maxCount) + "]";
            const Rect rr{r.x + 14, y, r.w - 28, kRowH - 2};
            row(kIdRow0 + static_cast<uint32_t>(i), rr, text, qualityColor(info ? info->quality : 1), it.maxCount != 0);
            const std::string price = it.extendedCost ? std::string(CLIENT_STR(TOKENS)) : money(it.buyPrice);
            ui.textShadow(fs, rr.x + rr.w - 4, rr.y + (rr.h - lh) * 0.5f, price.c_str(), kColGold, Align::Right);
            y += kRowH;
        }
        button(kIdPagePrev, Rect{r.x + 14, bottom, 40, 22}, "<", npc_.page > 0);
        button(kIdSell, Rect{r.x + 60, bottom, 150, 22}, OWN_STR(NPC_SELL_BACKPACK));
        button(kIdRepair, Rect{r.x + 216, bottom, 124, 22}, OWN_STR(NPC_REPAIR_ALL), v.canRepair);
        button(kIdPageNext, Rect{r.x + r.w - 54, bottom, 40, 22}, ">", (npc_.page + 1) * kRowsVendor < total);
    } else if (w == kWinQuestDetails) {
        const auto& d = session_.getQuestDetails();
        kit_.panel(ui, r);
        title(r, tokens(d.title));
        std::string t = d.details;
        if (!d.objectives.empty()) t += std::string("\n\n") + CLIENT_STR(QUEST_OBJECTIVES) + ":\n" + d.objectives;
        rewardLines(t, d.rewardMoney, d.rewardXp, d.rewardItems, &d.rewardChoiceItems);
        textBox(Rect{r.x + 14, r.y + 28, r.w - 28, bottom - r.y - 32}, tokens(t));
        button(kIdAccept, Rect{r.x + 40, bottom, 140, 22}, CLIENT_STR(ACCEPT));
        button(kIdDecline, Rect{r.x + r.w - 180, bottom, 140, 22}, CLIENT_STR(DECLINE));
    } else if (w == kWinQuestProgress) {
        const auto& q = session_.getQuestRequestItems();
        kit_.panel(ui, r);
        title(r, tokens(q.title));
        std::string t = q.completionText;
        if (!q.requiredItems.empty()) {
            t += std::string("\n\n") + CLIENT_STR(TURN_IN_ITEMS);
            for (const auto& it : q.requiredItems) t += "\n  " + itemName(it.itemId) + " x" + std::to_string(it.count);
        }
        if (q.requiredMoney) t += std::string("\n\n") + CLIENT_STR(REQUIRED_MONEY) + " " + money(q.requiredMoney);
        textBox(Rect{r.x + 14, r.y + 28, r.w - 28, bottom - r.y - 32}, tokens(t));
        button(kIdComplete, Rect{r.x + 40, bottom, 140, 22}, CLIENT_STR(COMPLETE_QUEST), q.isCompletable());
        button(kIdCancel, Rect{r.x + r.w - 180, bottom, 140, 22}, CLIENT_STR(CANCEL));
    } else if (w == kWinQuestReward) {
        const auto& q = session_.getQuestOfferReward();
        kit_.panel(ui, r);
        title(r, tokens(q.title));
        std::string t = q.rewardText;
        rewardLines(t, q.rewardMoney, q.rewardXp, q.fixedRewards, nullptr);
        const int nChoices = std::min<int>(6, static_cast<int>(q.choiceRewards.size()));
        const float boxH = nChoices > 0 ? lh * 6 : bottom - r.y - 32;
        textBox(Rect{r.x + 14, r.y + 28, r.w - 28, boxH}, tokens(t));
        if (nChoices > 0) {
            const float y0 = r.y + 32 + lh * 6;
            ui.textShadow(fs, r.x + 14, y0, CLIENT_STR(REWARD_CHOOSE), kColGold);
            for (int i = 0; i < nChoices; ++i) {
                const auto& it = q.choiceRewards[static_cast<size_t>(i)];
                const pp::NpcItemInfo* info = session_.npcItemInfo(it.itemId);
                std::string text = (npc_.rewardChoice == i ? "> " : "") + itemName(it.itemId);
                if (it.count > 1) text += " x" + std::to_string(it.count);
                const float cx = r.x + 14 + static_cast<float>(i % 2) * ((r.w - 28) * 0.5f);
                const float cy = y0 + lh + 2 + static_cast<float>(i / 2) * kRowH;
                row(kIdChoice0 + static_cast<uint32_t>(i), Rect{cx, cy, (r.w - 28) * 0.5f - 4, kRowH - 2}, text,
                    npc_.rewardChoice == i ? kColGreen : qualityColor(info ? info->quality : 1));
            }
        }
        button(kIdTakeReward, Rect{r.x + r.w * 0.5f - 90, bottom, 180, 22}, CLIENT_STR(COMPLETE_QUEST),
               nChoices == 0 || npc_.rewardChoice >= 0);
    } else {  // kWinQuestLog
        const Rect lr{8, 8, 464, 216};
        const auto& log = session_.getQuestLog();
        const int total = static_cast<int>(log.size());
        kit_.panel(ui, lr);
        title(lr, std::string(CLIENT_STR(QUEST_LOG)) + " (" + std::to_string(total) + "/25)");
        if (npc_.logSel >= total) npc_.logSel = total - 1;
        if (npc_.logPage * kRowsLog >= total) npc_.logPage = 0;
        float y = lr.y + 30;
        for (int i = 0; i < kRowsLog; ++i) {
            const int idx = npc_.logPage * kRowsLog + i;
            if (idx >= total) break;
            const auto& q = log[static_cast<size_t>(idx)];
            std::string text = (idx == npc_.logSel ? "> " : "");
            if (q.level > 0) text += "[" + std::to_string(q.level) + "] ";
            text += tokens(q.title);
            const uint32_t col = q.failed ? kColRed : q.complete ? kColGreen : kColGold;
            row(kIdRow0 + static_cast<uint32_t>(i), Rect{lr.x + 10, y, 196, kRowH - 2}, text, col);
            y += kRowH;
        }
        if (total == 0) ui.textShadow(fs, lr.x + 14, lr.y + 34, CLIENT_STR(QUESTLOG_NO_QUESTS_TEXT), kColGrey);
        const float lb = lr.y + lr.h - 30;
        button(kIdPagePrev, Rect{lr.x + 10, lb, 60, 22}, "<", npc_.logPage > 0);
        button(kIdPageNext, Rect{lr.x + 146, lb, 60, 22}, ">", (npc_.logPage + 1) * kRowsLog < total);
        if (npc_.logSel >= 0 && npc_.logSel < total) {
            const auto& q = log[static_cast<size_t>(npc_.logSel)];
            std::string t = q.title;
            if (q.failed) t += std::string(" (") + CLIENT_STR(FAILED) + ")";
            else if (q.complete) t += std::string(" (") + CLIENT_STR(COMPLETE) + ")";
            t += "\n";
            // Objective lines (upstream questObjectiveLine; PSP: the client's ERR_QUEST_ADD_KILL_SII form).
            bool any = false;
            for (const auto& ko : q.killObjectives) {
                if (ko.npcOrGoId == 0 || ko.required == 0) continue;
                const uint32_t entry = static_cast<uint32_t>(ko.npcOrGoId > 0 ? ko.npcOrGoId : -ko.npcOrGoId);
                std::string name;
                if (ko.npcOrGoId > 0)
                    if (const auto* ci = session_.creatureInfo(entry)) name = ci->name;
                auto kc = q.killCounts.find(entry);
                const uint32_t have = kc != q.killCounts.end() ? kc->second.first : 0;
                if (ko.npcOrGoId > 0)
                    t += "\n- " + CLIENT_FMT(ERR_QUEST_ADD_KILL_SII, name.empty() ? std::string(CLIENT_STR(CREATURE)) : name,
                                             static_cast<unsigned>(have), static_cast<unsigned>(ko.required));
                else
                    t += "\n- " + OWN_FMT(NPC_OBJECT_PROGRESS, static_cast<unsigned>(have), static_cast<unsigned>(ko.required));
                any = true;
            }
            for (const auto& io : q.itemObjectives) {
                if (io.itemId == 0 || io.required == 0) continue;
                auto ic = q.itemCounts.find(io.itemId);
                const uint32_t have = ic != q.itemCounts.end() ? ic->second : 0;
                t += "\n- " + itemName(io.itemId) + ": " + std::to_string(have) + "/" + std::to_string(io.required);
                any = true;
            }
            if (!q.objectives.empty()) t += (any ? "\n\n" : "\n") + q.objectives;
            if (q.complete && !q.completionText.empty()) t += "\n\n" + q.completionText;
            if (!q.description.empty()) t += "\n\n" + q.description;
            std::vector<game::QuestRewardItem> fixed, choices;
            for (const auto& it : q.rewardItems) if (it.itemId) fixed.push_back(it);
            for (const auto& it : q.rewardChoiceItems) if (it.itemId) choices.push_back(it);
            rewardLines(t, q.rewardMoney > 0 ? static_cast<uint32_t>(q.rewardMoney) : 0u, 0, fixed, &choices);
            textBox(Rect{lr.x + 214, lr.y + 30, lr.w - 224, lh * 12}, tokens(t));
            button(kIdAbandon, Rect{lr.x + 214, lb, 180, 22}, npc_.abandonArmed ? OWN_STR(NPC_ABANDON_CONFIRM) : CLIENT_STR(ABANDON_QUEST_ABBREV));
        }
    }
    // A press seen on one of our widgets arms the clicks (see npc_.guard).
    const uint32_t f = winFocus_.focused();
    if (top && f >= 0x6100 && f <= 0x61FF && winFocus_.isPressed(f)) npc_.guard = false;
    return true;
}

// ! / ? over questgivers (upstream questGiverMarker: gold, grey when dim),
// above the NPC names (ui_world_combat.cpp drawNpcNames).
void WorldScreen::drawQuestMarkers(UiRenderer& ui) {
    PROF_SCOPE("quest");
    if (npc_.marks && creaturesOn_) {
        const float maxDist = std::min(npc_.markDist, creatures_.viewDistance + 10.0f);
        int drawn = 0;
        for (const auto& [guid, status] : session_.getNpcQuestStatuses()) {
            const game::QuestGiverMarker m = game::questGiverMarker(status);
            if (!m.symbol || drawn >= 16) continue;
            glm::vec3 pos;
            float h = 0.0f, sx = 0.0f, sy = 0.0f;
            if (!unitPosition(guid, pos, h)) continue;
            const glm::vec3 d = pos - glm::vec3(playerX_, playerY_, playerZ_);
            if (glm::dot(d, d) > maxDist * maxDist) continue;
            UnitView u;
            if (unitView(guid, u) && u.dead) continue;
            if (!projectHud(pos + glm::vec3(0, 0, h + 0.4f), sx, sy)) continue;
            ui.textShadow(kit_.font(kFontTitle), sx, sy - 34, m.symbol, m.dim ? kColGrey : kColGold, Align::Center);
            ++drawn;
        }
    }
    if (!npc_.error.empty()) {
        if (pp::logUptimeMs() < npc_.errorUntilMs)
            ui.textShadow(kit_.font(kFontNormal), pp::kScreenW * 0.5f, 118, npc_.error.c_str(), kColRed, Align::Center);
        else
            npc_.error.clear();
    }
}

}  // namespace wowee::ui::psp
