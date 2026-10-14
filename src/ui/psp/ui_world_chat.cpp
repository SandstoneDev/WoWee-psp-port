// Chat window of the world screen (see include/ui/psp/ui_world_chat.hpp).
//
// Upstream references:
//  - lines: ChatHandler (src/game/chat_handler.cpp) via the PSP WorldSession
//    (psp_world_session_chat.cpp); colours: the 3.3.5 palette upstream sends
//    FrameXML (addon_manager.cpp kChatColours, UPDATE_CHAT_COLOR);
//  - sending: ChatPanel::sendChatMessage (src/ui/chat_panel.cpp) and the
//    channel commands (src/ui/chat/commands/channel_commands.cpp: /s /y /w
//    /r), then the emote fallthrough (getEmoteText + getEmoteDbcId ->
//    sendTextEmote + a local TEXT_EMOTE line);
//  - text emotes of others: ChatHandler::handleTextEmote +
//    EmoteRegistry::textByDbcId.
// PSP: FrameXML (which prints "X says: ..." on the PC) is not there, so the
// line prefixes are written here; link/colour markup is shown as plain text.
// the lines as ChatFrame_MessageEventHandler writes them with the client's
// CHAT_<TYPE>_GET strings (CHAT_SAY_GET "[Name] says: ...", the AFK / DND / GM flags, leaders, channels) and the
// original's default chat colours (addMessage, chatColor). The client's strings from strings.pak of the
// running language, the port's own from its en/ru table.
#include "ui/psp/ui_world_chat.hpp"

#include "game/text_tokens.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_fs.hpp"  // ui.pak for the GM icon
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_osk.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"
#include "ui/psp/ui_world_sounds.hpp"  // whisper sound
#include "ui/psp/ui_dressup_rules.hpp"  // placeLinks

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <iterator>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
namespace rp = wowee::rendering::psp;
using game::ChatType;

namespace {

enum : uint32_t {
    kIdChatOpen = WorldChat::kBarButtonId,  // a click on the chat box (was a "Chat" button on the window bar)
    kIdChatUp,
    kIdChatDown,
};

constexpr size_t kHistory = 60;        // wrapped lines kept

// the chat frame keeps its lines across a loading screen, as the original's does -
// the world, and this window with it, is rebuilt on every transfer between maps. Handed from the old window to the
// next one of the same session, character and transfer (a fresh login starts empty, as the original's UI reload).
struct ChatCarry {
    const void* session = nullptr;
    uint64_t player = 0;
    uint32_t transfers = 0;  // WorldSession::transfersDone() when the old window went: 0 = not a transfer
    struct CarriedLine {
        std::string text;
        uint32_t color, bornMs;  // the line's own age goes on (a faded line stays faded)
        std::vector<WorldChat::LinkSpan> links{};  // the item links stay hoverable after a transfer
    };
    std::deque<CarriedLine> lines;
    std::vector<std::string> channels;
};
ChatCarry g_chatCarry;
constexpr float kBoxX = 4.0f;
constexpr float kBoxW = 230.0f;        // leaves the bar (bottom right) free
constexpr float kBoxBottom = 240.0f;   // above the control hints
constexpr uint32_t kFadeMs = 3000;     // upstream ScrollingMessageFrame fadeDuration 3 s

constexpr uint32_t rgb255(uint32_t r, uint32_t g, uint32_t b) { return 0xFF000000u | (b << 16) | (g << 8) | r; }

// the original's default ChatTypeInfo colours - the COLORS block of the
// chat-cache.txt the ruRU 3.3.5a client writes (UPDATE_CHAT_COLOR at login); was upstream's
// addon_manager.cpp kChatColours (MONSTER_SAY white, MONSTER_WHISPER pink, the BG colours and the
// unnamed types white differed). CHANNEL1..10 = CHANNEL.
uint32_t chatColor(ChatType t) {
    switch (t) {
        case ChatType::SYSTEM: return rgb255(255, 255, 0);
        case ChatType::SAY: return rgb255(255, 255, 255);
        case ChatType::PARTY: return rgb255(170, 170, 255);
        case ChatType::RAID: return rgb255(255, 127, 0);
        case ChatType::GUILD: return rgb255(64, 255, 64);
        case ChatType::OFFICER: return rgb255(64, 192, 64);
        case ChatType::YELL: return rgb255(255, 64, 64);
        case ChatType::WHISPER:
        case ChatType::WHISPER_FOREIGN:
        case ChatType::WHISPER_INFORM: return rgb255(255, 128, 255);
        case ChatType::EMOTE:
        case ChatType::TEXT_EMOTE: return rgb255(255, 128, 64);
        case ChatType::MONSTER_SAY: return rgb255(255, 255, 159);
        case ChatType::MONSTER_PARTY: return rgb255(170, 170, 255);
        case ChatType::MONSTER_YELL: return rgb255(255, 64, 64);
        case ChatType::MONSTER_WHISPER: return rgb255(255, 181, 235);
        case ChatType::MONSTER_EMOTE: return rgb255(255, 128, 64);
        case ChatType::CHANNEL: return rgb255(255, 192, 192);
        // the session makes CHANNEL_JOIN / _LEAVE / _NOTICE lines only for a listed channel -
        // ChatFrame_OnEvent draws those in the channel's colour (ChatTypeInfo["CHANNEL"..N]); INVITE keeps NOTICE_USER's.
        case ChatType::CHANNEL_JOIN:
        case ChatType::CHANNEL_LEAVE:
        case ChatType::CHANNEL_NOTICE: return rgb255(255, 192, 192);
        case ChatType::CHANNEL_LIST: return rgb255(192, 128, 128);
        case ChatType::CHANNEL_NOTICE_USER: return rgb255(192, 192, 192);
        case ChatType::AFK:
        case ChatType::DND: return rgb255(255, 128, 255);
        case ChatType::IGNORED: return rgb255(255, 0, 0);
        case ChatType::SKILL: return rgb255(85, 85, 255);
        case ChatType::LOOT: return rgb255(0, 170, 0);
        case ChatType::MONEY: return rgb255(255, 255, 0);
        case ChatType::OPENING: return rgb255(128, 128, 255);
        case ChatType::TRADESKILLS: return rgb255(255, 255, 255);
        case ChatType::PET_INFO:
        case ChatType::COMBAT_MISC_INFO: return rgb255(128, 128, 255);
        case ChatType::COMBAT_XP_GAIN: return rgb255(111, 111, 255);
        case ChatType::COMBAT_HONOR_GAIN: return rgb255(224, 202, 10);
        case ChatType::COMBAT_FACTION_CHANGE: return rgb255(128, 128, 255);
        case ChatType::BG_SYSTEM_NEUTRAL: return rgb255(255, 120, 10);
        case ChatType::BG_SYSTEM_ALLIANCE: return rgb255(0, 174, 239);
        case ChatType::BG_SYSTEM_HORDE: return rgb255(255, 0, 0);
        case ChatType::RAID_LEADER: return rgb255(255, 72, 9);
        case ChatType::RAID_WARNING: return rgb255(255, 72, 0);
        case ChatType::RAID_BOSS_EMOTE:
        case ChatType::RAID_BOSS_WHISPER: return rgb255(255, 221, 0);
        case ChatType::FILTERED: return rgb255(255, 0, 0);
        case ChatType::BATTLEGROUND: return rgb255(255, 127, 0);
        case ChatType::BATTLEGROUND_LEADER: return rgb255(255, 219, 183);
        case ChatType::RESTRICTED: return rgb255(255, 0, 0);
        case ChatType::ACHIEVEMENT: return rgb255(255, 255, 0);
        case ChatType::GUILD_ACHIEVEMENT: return rgb255(64, 255, 64);
        case ChatType::PARTY_LEADER: return rgb255(118, 200, 255);
        default: return rgb255(255, 255, 255);
    }
}

// FrameXML: "strsub(type, 1, 7) == "MONSTER" or strsub(type, 1, 9) == "RAID_BOSS"" - no player link.
bool isNpcLine(ChatType t) {
    switch (t) {
        case ChatType::MONSTER_SAY:
        case ChatType::MONSTER_PARTY:
        case ChatType::MONSTER_YELL:
        case ChatType::MONSTER_WHISPER:
        case ChatType::MONSTER_EMOTE:
        case ChatType::RAID_BOSS_EMOTE:
        case ChatType::RAID_BOSS_WHISPER: return true;
        default: return false;
    }
}

// PSP: the client's inline markup as plain text - colours (|cAARRGGBB..|r), links
// (|H..|h[text]|h keeps [text]), textures (|T..|t), |n, and the ruRU grammar
// codes (|3-N(name) declension keeps the name, |4a:b:c; plural: the form for
// the number before it, psp_text_format.hpp). Control characters become spaces.
std::string cleanMarkup(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        const auto c = static_cast<unsigned char>(in[i]);
        if (c == '|' && i + 1 < in.size()) {
            const char n = in[i + 1];
            if (n == '|') {
                out.push_back('|');
                ++i;
                continue;
            }
            if (n == 'c' && i + 9 < in.size()) {
                i += 9;
                continue;
            }
            if (n == 'r' || n == 'h') {
                ++i;
                continue;
            }
            if (n == 'H' || n == 'T') {
                const size_t e = in.find(n == 'H' ? "|h" : "|t", i + 2);
                if (e == std::string::npos) break;
                i = e + 1;
                continue;
            }
            if (n == 'n') {
                out.push_back(' ');
                ++i;
                continue;
            }
            if (n == '3' && i + 2 < in.size() && in[i + 2] == '-') {
                const size_t open = in.find('(', i);
                const size_t close = open == std::string::npos ? open : in.find(')', open);
                if (close != std::string::npos) {
                    out.append(in, open + 1, close - open - 1);
                    i = close;
                    continue;
                }
            }
            if (n == '4') {  // the form for the last number before it (none: the last form)
                const size_t semi = in.find(';', i);
                if (semi != std::string::npos) {
                    long num = 0;
                    const bool have = pp::text::pluralLastNumber(out, num);
                    out += pp::text::pluralForm(in.substr(i + 2, semi - (i + 2)), num, have);
                    i = semi;
                    continue;
                }
            }
        }
        out.push_back(c < 0x20 ? ' ' : static_cast<char>(c));
    }
    return out;
}

// the item links of a raw chat line, in order - the id of "|Hitem:<id>:...|h[name]|h"
// and the "[name]" cleanMarkup leaves of it (ChatFrame's hyperlinks; at most 16 a line).
std::vector<std::pair<uint32_t, std::string>> itemLinksOf(const std::string& in) {
    std::vector<std::pair<uint32_t, std::string>> out;
    size_t at = 0;
    while ((at = in.find("|Hitem:", at)) != std::string::npos && out.size() < 16) {
        size_t p = at + 7;
        uint32_t id = 0;
        while (p < in.size() && in[p] >= '0' && in[p] <= '9' && id < 100000000u)
            id = id * 10u + static_cast<uint32_t>(in[p++] - '0');
        const size_t h = in.find("|h", p);
        if (h == std::string::npos) break;
        const size_t e = in.find("|h", h + 2);
        if (e == std::string::npos) break;
        std::string shown = cleanMarkup(in.substr(h + 2, e - h - 2));
        if (id && !shown.empty()) out.emplace_back(id, std::move(shown));
        at = e + 2;
    }
    return out;
}

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}

// PSP: until characters.pak carries "emotetext", the common emotes: upstream
// EmoteRegistry::loadFallbackEmotes texts, with the EmotesText.dbc ids (the
// upstream fallback has id 0 and so could only animate, never send; "dead",
// which has no EmotesText row, is left out).
struct BuiltinEmote {
    const char* cmd;
    uint16_t id;
    const char* text[rp::kEtSlotCount];  // others target, sender target, others, sender
};
constexpr BuiltinEmote kBuiltin[] = {
    {"applaud", 5, {"%s applauds at %s. Bravo!", "You applaud at %s. Bravo!", "%s applauds. Bravo!", "You applaud. Bravo!"}},
    {"beg", 8, {"%s begs %s. How pathetic.", "You beg %s. How pathetic.", "%s begs everyone around. How pathetic.",
                "You beg everyone around you. How pathetic."}},
    {"bow", 17, {"%s bows down before %s.", "You bow down before %s.", "%s bows down graciously.",
                 "You bow down graciously."}},
    {"cheer", 21, {"%s cheers at %s.", "You cheer at %s.", "%s cheers!", "You cheer!"}},
    {"chicken", 22, {"%s struts around %s. Cluck, Cluck, Chicken!", "With arms flapping, you strut around %s. Cluck, Cluck, Chicken!",
                     "%s struts around. Cluck, Cluck, Chicken!", "With arms flapping, you strut around. Cluck, Cluck, Chicken!"}},
    {"cry", 31, {"%s cries on %s's shoulder.", "You cry on %s's shoulder.", "%s cries.", "You cry."}},
    {"dance", 34, {"%s dances with %s.", "You dance with %s.", "%s bursts into dance.", "You burst into dance."}},
    {"eat", 37, {"%s begins to eat in front of %s.", "You begin to eat in front of %s.", "%s begins to eat.",
                 "You begin to eat."}},
    {"flex", 41, {"%s flexes at %s. Oooooh so strong!", "You flex at %s. Oooooh so strong!",
                  "%s flexes. Oooooh so strong!", "You flex your muscles. Oooooh so strong!"}},
    {"kiss", 58, {"%s blows a kiss to %s.", "You blow a kiss to %s.", "%s blows a kiss into the wind.",
                  "You blow a kiss into the wind."}},
    {"kneel", 59, {"%s kneels before %s.", "You kneel before %s.", "%s kneels down.", "You kneel down."}},
    {"laugh", 60, {"%s laughs at %s.", "You laugh at %s.", "%s laughs.", "You laugh."}},
    {"point", 72, {"%s points at %s.", "You point at %s.", "%s points over yonder.", "You point over yonder."}},
    {"roar", 75, {"%s roars with bestial vigor at %s. So fierce!", "You roar with bestial vigor at %s. So fierce!",
                  "%s roars with bestial vigor. So fierce!", "You roar with bestial vigor. So fierce!"}},
    {"rude", 77, {"%s makes a rude gesture at %s.", "You make a rude gesture at %s.", "%s makes a rude gesture.",
                  "You make a rude gesture."}},
    {"salute", 78, {"%s salutes %s with respect.", "You salute %s with respect.", "%s salutes.", "You salute."}},
    {"shout", 82, {"%s shouts at %s.", "You shout at %s.", "%s shouts.", "You shout."}},
    {"shy", 84, {"%s smiles shyly at %s.", "You smile shyly at %s.", "%s smiles shyly.", "You smile shyly."}},
    {"talk", 93, {"%s talks to %s.", "You talk to %s.", "%s talks.", "You talk."}},
    {"wave", 101, {"%s waves at %s.", "You wave at %s.", "%s waves.", "You wave."}},
    {"work", 105, {"%s begins to work near %s.", "You begin to work near %s.", "%s begins to work.",
                   "You begin to work."}},
    {"train", 264, {"%s lets off a train whistle at %s. Choo Choo!", "You let off a train whistle at %s. Choo Choo!",
                    "%s lets off a train whistle. Choo Choo!", "You let off a train whistle. Choo Choo!"}},
};

// One emote, from the pak table or the built-in one.
struct EmoteHit {
    uint32_t id = 0;
    const char* cmd = nullptr;
    const char* text[rp::kEtSlotCount] = {};
};

} // namespace

WorldChat::WorldChat(const UiKit& kit, pp::WorldSession& session) : kit_(kit), session_(session) {
    visibleLines_ = std::clamp(pp::configGetInt("chatlines", 6), 1, 12);
    fadeSec_ = static_cast<float>(std::clamp(pp::configGetInt("chatfade", 30), 0, 3600));
    // the last window's lines, when it was this session's and this character's.
    if (g_chatCarry.session == &session_ && g_chatCarry.player == session_.playerGuid() && g_chatCarry.transfers != 0 &&
        g_chatCarry.transfers == session_.transfersDone()) {
        for (auto& l : g_chatCarry.lines) lines_.push_back(Line{std::move(l.text), l.color, l.bornMs, std::move(l.links)});
        channels_ = std::move(g_chatCarry.channels);
        PSP_LOGI(pp::tag::Gfx, "chat_carry lines=%u", static_cast<unsigned>(lines_.size()));
    }
    g_chatCarry = ChatCarry{};
}

WorldChat::~WorldChat() {
    if (tableBlob_) pp::pakFree(tableBlob_);
    pp::textureRelease(gmIcon_);
    // the lines go on to the next world's window (a transfer between maps).
    g_chatCarry = ChatCarry{};
    g_chatCarry.session = &session_;
    g_chatCarry.player = session_.playerGuid();
    g_chatCarry.transfers = session_.transfersDone();
    for (Line& l : lines_)
        g_chatCarry.lines.push_back(ChatCarry::CarriedLine{std::move(l.text), l.color, l.bornMs, std::move(l.links)});
    g_chatCarry.channels = std::move(channels_);
}

void WorldChat::loadTable(pp::Pak& charPak) {
    if (tableBlob_) pp::pakFree(tableBlob_);
    tableBlob_ = nullptr;
    table_ = rp::EmoteTextView();
    const uint32_t t0 = pp::logUptimeMs();
    // another language than data/'s has its emote texts in data/locale/<xxYY>/emotetext.pak (Kiln:
    // characters.pak's "emotetext" record alone, psp_char_conv --emotetext-only); data/'s own are in characters.pak.
    pp::Pak localized;
    pp::Pak* src = &charPak;
    char path[256];
    if (pp::fsPath(path, sizeof(path), "data/emotetext.pak") && pp::fsExists(path) && localized.open(path) &&
        localized.find("emotetext"))
        src = &localized;
    const pp::PakEntry* e = src->find("emotetext");
    const char* why = "missing";
    if (e && (tableBlob_ = src->load(e)) != nullptr && !table_.attach(tableBlob_, e->size, &why)) {
        pp::pakFree(tableBlob_);
        tableBlob_ = nullptr;
    }
    if (tableBlob_) {
        PSP_LOGI(pp::tag::Gfx, "chat_table rows=%u aliases=%u kb=%u src=%s ms=%u", static_cast<unsigned>(table_.rowCount()),
                 static_cast<unsigned>(table_.aliasCount()), static_cast<unsigned>(e->size / 1024),
                 src == &localized ? "locale" : "pak", static_cast<unsigned>(pp::logUptimeMs() - t0));
    } else {
        PSP_LOGW(pp::tag::Gfx, "chat_table rows=%u src=builtin why=%s", static_cast<unsigned>(std::size(kBuiltin)),
                 why ? why : "?");
    }
}

// Command -> emote (upstream EmoteRegistry::dbcIdFor / textFor).
static bool findEmote(const rp::EmoteTextView& table, const std::string& key, EmoteHit& hit) {
    if (!table.empty()) {
        const int r = table.findCommand(key);
        if (r < 0) return false;
        hit.id = table.row(static_cast<uint32_t>(r)).textEmoteId;
        for (uint32_t s = 0; s < rp::kEtSlotCount; ++s) hit.text[s] = table.text(static_cast<uint32_t>(r), static_cast<rp::EmoteTextSlot>(s));
        return true;
    }
    for (const auto& b : kBuiltin)
        if (key == b.cmd) {
            hit.id = b.id;
            hit.cmd = b.cmd;
            for (uint32_t s = 0; s < rp::kEtSlotCount; ++s) hit.text[s] = b.text[s];
            return true;
        }
    return false;
}

// EmotesText id -> emote (upstream EmoteRegistry::textByDbcId's lookup).
static bool findEmoteById(const rp::EmoteTextView& table, uint32_t id, EmoteHit& hit) {
    if (!table.empty()) {
        const int r = table.findId(id);
        if (r < 0) return false;
        hit.id = id;
        for (uint32_t s = 0; s < rp::kEtSlotCount; ++s) hit.text[s] = table.text(static_cast<uint32_t>(r), static_cast<rp::EmoteTextSlot>(s));
        // The command upstream's "<name> <command>s." fallback is built from.
        for (uint32_t i = 0; i < table.aliasCount(); ++i)
            if (table.alias(i).row == static_cast<uint32_t>(r) && table.alias(i).source == 0) {
                hit.cmd = table.str(table.alias(i).command);
                break;
            }
        return true;
    }
    for (const auto& b : kBuiltin)
        if (b.id == id) {
            hit.id = b.id;
            hit.cmd = b.cmd;
            for (uint32_t s = 0; s < rp::kEtSlotCount; ++s) hit.text[s] = b.text[s];
            return true;
        }
    return false;
}

// PSP: $g/$n in the ruRU emote texts for the one who emotes (upstream prints
// the DBC string as it is).
static std::string resolveFor(const std::string& text, const std::string& name, uint8_t gender) {
    game::TextSubject subject;
    subject.name = name;
    subject.gender = gender == 0 ? game::Gender::MALE : gender == 1 ? game::Gender::FEMALE : game::Gender::NONBINARY;
    return game::resolveTextTokens(text, subject);
}

pp::PadState WorldChat::filterPad(const pp::PadState& pad) {
    static const pp::PadState kIdle{};
    if (pp::oskActive()) {
        waitRelease_ = true;
        return kIdle;
    }
    bool accepted = false;
    std::string typed;
    if (pp::oskFinished(accepted, typed)) {
        waitRelease_ = true;
        PSP_LOGI(pp::tag::Ctrl, "chat_osk ok=%d len=%u", accepted ? 1 : 0, static_cast<unsigned>(typed.size()));
        if (accepted) submit(typed);
        sent_ = accepted && !typed.empty();  // back to the game after sending
    }
    if (waitRelease_) {
        if (pad.held) return kIdle;
        waitRelease_ = false;
    }
    return pad;
}

bool WorldChat::inputActive() const { return pp::oskActive() || waitRelease_; }

// The client's slash command keys (SLASH_<CMD><n>: "/s" -> "s").
bool WorldChat::clientSlash(const std::string& key, std::initializer_list<pp::CS> cmds) {
    for (pp::CS c : cmds)
        if (key == rp::emoteCommandKey(pp::cs(c))) return true;
    return false;
}

void WorldChat::openInput() {
    if (pp::oskActive()) return;
    std::string mode;
    switch (stickyType_) {
        case ChatType::YELL: mode = CLIENT_STR(CHAT_MSG_YELL); break;
        case ChatType::WHISPER: {  // CHAT_WHISPER_SEND "Tell %s: " without its ": "
            mode = cleanMarkup(CLIENT_FMT(CHAT_WHISPER_SEND, stickyTarget_));
            while (!mode.empty() && (mode.back() == ' ' || mode.back() == ':')) mode.pop_back();
            break;
        }
        case ChatType::PARTY: mode = CLIENT_STR(CHAT_MSG_PARTY); break;
        case ChatType::GUILD: mode = CLIENT_STR(CHAT_MSG_GUILD); break;
        case ChatType::RAID: mode = CLIENT_STR(CHAT_MSG_RAID); break;
        default: mode = CLIENT_STR(CHAT_MSG_SAY); break;
    }
    // The client's chat line takes 255 bytes; the keyboard buffer holds 127 characters.
    if (!pp::oskBegin(mode + "   " + OWN_STR(CHAT_INPUT_HINT), "", 120, pp::OskCharset::Text))
        session_.addSystemChatMessage(OWN_STR(PSP_KEYBOARD_FAILED));
}

// ChatFrame_SendTell from a unit menu - the whisper becomes the chat type
// (as "/w name") and the input opens.
void WorldChat::openWhisper(const std::string& name) {
    if (name.empty()) return;
    stickyType_ = ChatType::WHISPER;
    stickyTarget_ = name;
    openInput();
}

bool WorldChat::activate(uint32_t id) {
    if (id >= kLinkId0 && id < kLinkId0 + kLinkIds) {  // an item link - a click on the box as well
        openInput();
        return true;
    }
    switch (id) {
        case kIdChatOpen: openInput(); return true;
        case kIdChatUp:
            scroll_ = std::min(scroll_ + 3, std::max(0, static_cast<int>(lines_.size()) - visibleLines_));
            return true;
        case kIdChatDown: scroll_ = std::max(0, scroll_ - 3); return true;
        default: return false;
    }
}

// upstream ChatPanel::sendChatMessage + the channel commands, in their order:
// GM dot commands, slash commands (channels first, then the emote
// fallthrough), else the selected chat type.
void WorldChat::submit(const std::string& raw) {
    const std::string input = trim(raw);
    if (input.empty()) return;
    if (input.size() > 1 && input[0] == '.') {
        session_.sendChatMessage(ChatType::SAY, input);
        session_.addSystemChatMessage(OWN_FMT(CHAT_GM_COMMAND_SENT, input));
        return;
    }
    if (input[0] != '/') {
        // a sticky CHANNEL keeps its channel's name as the target (/1 .. /10)
        session_.sendChatMessage(stickyType_, input,
                                 stickyType_ == ChatType::WHISPER || stickyType_ == ChatType::CHANNEL ? stickyTarget_ : "");
        return;
    }
    const size_t sp = input.find(' ');
    const std::string key = rp::emoteCommandKey(input.substr(0, sp));
    const std::string args = sp == std::string::npos ? std::string() : trim(input.substr(sp + 1));
    // Upstream aliases; PSP: + the client's (SLASH_<CMD><n> of the running language - the ruRU client's Cyrillic
    // ones, since the keyboard opens in Cyrillic: SLASH_SAY1, SLASH_YELL1, SLASH_WHISPER1, SLASH_REPLY1, SLASH_EMOTE1).
    auto is = [&](std::initializer_list<const char*> names, std::initializer_list<pp::CS> client = {}) {
        for (const char* n : names)
            if (key == n) return true;
        return clientSlash(key, client);
    };
    auto channel = [&](ChatType t) {
        if (!args.empty()) session_.sendChatMessage(t, args);
        stickyType_ = t;  // upstream setSelectedChatType
    };
    if (is({"s", "say"}, {pp::CS::SLASH_SAY1})) return channel(ChatType::SAY);
    if (is({"y", "yell", "shout"}, {pp::CS::SLASH_YELL1})) return channel(ChatType::YELL);
    // SLASH_PARTY1..5 of the ruRU client (SLASH_PARTY1 / SLASH_PARTY2, "/p", "/party").
    if (is({"p", "party"}, {pp::CS::SLASH_PARTY1, pp::CS::SLASH_PARTY2})) return channel(ChatType::PARTY);
    // SLASH_INVITE / SLASH_UNINVITE / SLASH_PROMOTE (GlobalStrings) - InviteUnit,
    // UninviteUnit, PromoteToLeader(name) as ChatFrame.lua's handlers (the name argument).
    if (is({"i", "inv", "invite"}, {pp::CS::SLASH_INVITE3})) {
        if (!args.empty()) session_.inviteToGroup(args.substr(0, args.find(' ')));
        return;
    }
    if (is({"u", "un", "uninvite", "kick"},
           {pp::CS::SLASH_UNINVITE1, pp::CS::SLASH_UNINVITE2, pp::CS::SLASH_UNINVITE3, pp::CS::SLASH_UNINVITE4})) {
        if (!args.empty()) session_.uninvitePlayer(args.substr(0, args.find(' ')));
        return;
    }
    if (is({"pr", "promote"}, {pp::CS::SLASH_PROMOTE2})) {
        const std::string who = args.substr(0, args.find(' '));
        for (const auto& m : session_.getPartyData().members)
            if (!who.empty() && m.name == who) session_.promoteToLeader(m.guid);
        return;
    }
    if (is({"g", "guild"})) return channel(ChatType::GUILD);
    if (is({"raid", "rsay", "ra"})) return channel(ChatType::RAID);
    if (is({"e", "em", "emote", "me"}, {pp::CS::SLASH_EMOTE1})) {
        // The client's /e (FrameXML SendChatMessage "EMOTE"); not sticky there either.
        if (!args.empty()) session_.sendChatMessage(ChatType::EMOTE, args);
        return;
    }
    if (is({"w", "whisper", "tell", "t"}, {pp::CS::SLASH_WHISPER1})) {
        const size_t s2 = args.find(' ');
        const std::string target = args.substr(0, s2);
        if (target.empty()) return;
        stickyType_ = ChatType::WHISPER;
        stickyTarget_ = target;
        if (s2 != std::string::npos) session_.sendChatMessage(ChatType::WHISPER, trim(args.substr(s2 + 1)), target);
        return;
    }
    if (is({"r", "reply"}, {pp::CS::SLASH_REPLY1})) {
        const std::string& last = session_.lastWhisperSender();
        if (last.empty()) {
            session_.addSystemChatMessage(OWN_STR(CHAT_NO_WHISPER_YET));
            return;
        }
        stickyType_ = ChatType::WHISPER;
        stickyTarget_ = last;
        if (!args.empty()) session_.sendChatMessage(ChatType::WHISPER, args, last);
        return;
    }
    // SLASH_INSPECT1..4 of the ruRU client - InspectUnit("target") (ChatFrame.lua).
    if (is({"ins", "inspect"}, {pp::CS::SLASH_INSPECT2})) {
        inspectWanted_ = true;
        return;
    }
    if (channelCommand(key, args)) return;  // /join /leave /chatlist /1../10 and /chat* commands
    if (socialCommand(key, args)) return;  // the guild / friends / who / AFK commands
    // SLASH_DUEL1..2 - StartDuel(msg) (PSP: no unit argument - the target); SLASH_DUEL_CANCEL1..6 -
    // CancelDuel() (declines a request, forfeits a duel) - ChatFrame.lua's SecureCmdList DUEL / DUEL_CANCEL.
    if (is({"duel"}, {pp::CS::SLASH_DUEL1})) {
        session_.startDuel(session_.target());
        return;
    }
    if (is({"yield", "concede", "forfeit"}, {pp::CS::SLASH_DUEL_CANCEL1, pp::CS::SLASH_DUEL_CANCEL3})) {
        session_.cancelDuel();
        return;
    }
    if (is({"help", "?"}, {pp::CS::SLASH_HELP6})) {
        session_.addSystemChatMessage(OWN_STR(CHAT_HELP_LINE));
        return;
    }
    if (sendEmote(key, input.substr(0, sp))) return;
    // PSP: upstream sends an unknown slash line as plain chat (its dropdown
    // type); the original client answers with a hint instead - so does the PSP.
    session_.addSystemChatMessage(OWN_FMT(CHAT_UNKNOWN_COMMAND, input.substr(0, sp)));
}

// upstream chat_panel.cpp "emote fallthrough": getEmoteText (sender side,
// with the target) + getEmoteDbcId -> sendTextEmote, and a local TEXT_EMOTE
// line. The animation comes back from the server (SMSG_EMOTE, M9c).
bool WorldChat::sendEmote(const std::string& key, const std::string& shown) {
    EmoteHit hit;
    if (key.empty() || !findEmote(table_, key, hit) || hit.id == 0) return false;
    session_.sendTextEmoteAt(hit.id, targetGuid_);
    static uint32_t logs = 0;
    if (++logs <= 60)
        PSP_LOGI(pp::tag::Net, "chat_emote cmd=%.24s id=%u src=%s", key.c_str(), static_cast<unsigned>(hit.id),
                 table_.empty() ? "builtin" : "pak");
    const bool hasTarget = !targetName_.empty();
    const char* base = hit.text[hasTarget ? rp::kEtSenderTarget : rp::kEtSenderNoTarget];
    std::string text;
    if (base) {
        text = rp::emoteReplaceAll(base, hasTarget ? &targetName_ : nullptr);
    } else {
        // upstream textFor fallback; PSP: the client's YOU ("You: /cmd target")
        const std::string you = std::string(CLIENT_STR(YOU)) + ": " + shown;
        text = hasTarget ? you + " " + targetName_ : you;
    }
    const game::Character* me = session_.activeCharacter();
    const uint8_t gender = me ? (me->gender == game::Gender::FEMALE ? 1 : me->gender == game::Gender::MALE ? 0 : 2) : 2;
    game::MessageChatData msg;
    msg.type = ChatType::TEXT_EMOTE;
    msg.language = game::ChatLanguage::COMMON;
    msg.senderGuid = session_.playerGuid();
    msg.message = resolveFor(text, me ? me->name : std::string(), gender);
    session_.addLocalChatMessage(msg);
    return true;
}

void WorldChat::addTextEmote(const pp::WorldSession::TextEmoteHeard& t) {
    // upstream EmoteRegistry::textByDbcId, then handleTextEmote's fallback.
    EmoteHit hit;
    std::string text;
    if (findEmoteById(table_, t.textEmoteId, hit)) {
        const std::string cmd = hit.cmd ? hit.cmd : "emote";
        if (!t.targetName.empty()) {
            text = hit.text[rp::kEtOthersTarget]
                       ? rp::emoteReplaceSenderTarget(hit.text[rp::kEtOthersTarget], t.senderName, t.targetName)
                       : t.senderName + " " + cmd + "s at " + t.targetName + ".";
        } else {
            text = hit.text[rp::kEtOthersNoTarget] ? rp::emoteReplaceAll(hit.text[rp::kEtOthersNoTarget], &t.senderName)
                                                   : t.senderName + " " + cmd + "s.";
        }
    } else {
        // PSP: upstream's "X performs an emote." (the port's own string).
        text = t.targetName.empty() ? OWN_FMT(CHAT_EMOTE_PERFORMED, t.senderName)
                                    : OWN_FMT(CHAT_EMOTE_PERFORMED_AT, t.senderName, t.targetName);
    }
    addLine(cleanMarkup(resolveFor(text, t.senderName, t.gender)), chatColor(ChatType::TEXT_EMOTE));
}

// What FrameXML's ChatFrame_MessageEventHandler prints for each type.
// its own steps with the client's CHAT_<TYPE>_GET strings (GlobalStrings.lua;
// was a Russian approximation; now strings.pak of the running language, the |H..|h links and the
// |3-N() declensions plain - cleanMarkup):
//   - a player's name is his link "[Name]" (CHAT_SAY_GET "%s says: " -> "[Name] says: text");
//     EMOTE and TEXT_EMOTE take the name without brackets; NPC lines (MONSTER_*, RAID_BOSS_*) the plain
//     name (no link); an empty name stays empty, as format() leaves it;
//   - the flag before the name, pflag..playerLink: CHAT_FLAG_AFK, CHAT_FLAG_DND (no space), GM / DEV - the
//     staff icon (UI-ChatIcon-Blizz) and a space; PSP: CHAT_FLAG_GM while ui.pak has no "chat_blizz" (an older
//     ui.pak). The tag -> flag order GM, DEV, DND, AFK is upstream's (chat_handler.cpp chatFlag);
//   - leaders: CHAT_PARTY_LEADER_GET, CHAT_RAID_LEADER_GET, CHAT_BATTLEGROUND_LEADER_GET - the server sends those
//     types for the leader;
//   - a channel line: "[N. Channel] " before the body (arg4 without " - zone"); N - PSP: the order the
//     channels were first heard in (the client numbers the ones it joined; the PSP joins none);
//   - ACHIEVEMENT / GUILD_ACHIEVEMENT: format(arg1, "[Name]"); IGNORED / FILTERED / RESTRICTED: CHAT_*.
// PSP: a GM's whisper stays here (the original hands it to its GM chat add-on, which the PSP has not); no
// "[language] " header (the client adds it for a known language other than its default - the skill check is
// not on the PSP); no {star} raid icons, no timestamps (off by default in the original).
void WorldChat::addMessage(const game::MessageChatData& m) {
    const std::string& who = m.type == ChatType::WHISPER_INFORM ? m.receiverName : m.senderName;  // arg2
    std::string pflag;
    if (m.chatTag & (0x04 | 0x10)) pflag = gmIcon() ? std::string(1, kIconMark) + " " : std::string(CLIENT_STR(CHAT_FLAG_GM));
    else if (m.chatTag & 0x02) pflag = CLIENT_STR(CHAT_FLAG_DND);
    else if (m.chatTag & 0x01) pflag = CLIENT_STR(CHAT_FLAG_AFK);
    const std::string msg = cleanMarkup(m.message);
    pendingLinks_ = itemLinksOf(m.message);  // the line's item links, found again after the wrap
    const bool npc = isNpcLine(m.type);
    // pflag..playerLink.."["..coloredName.."]|h" - or pflag..arg2 for an NPC / no name.
    const std::string name = npc || who.empty() ? pflag + who : pflag + "[" + who + "]";
    // CHAT_<TYPE>_GET with the name, plain (cleanMarkup: the channel links and |3-N() declensions as text).
    auto head = [&](pp::CS get) { return cleanMarkup(pp::csf(get, name)); };
    std::string text;
    switch (m.type) {
        case ChatType::SAY: text = head(pp::CS::CHAT_SAY_GET) + msg; break;
        case ChatType::MONSTER_SAY: text = head(pp::CS::CHAT_MONSTER_SAY_GET) + msg; break;
        case ChatType::YELL: text = head(pp::CS::CHAT_YELL_GET) + msg; break;
        case ChatType::MONSTER_YELL: text = head(pp::CS::CHAT_MONSTER_YELL_GET) + msg; break;
        case ChatType::WHISPER:
        case ChatType::WHISPER_FOREIGN:
        case ChatType::MONSTER_WHISPER: text = head(pp::CS::CHAT_WHISPER_GET) + msg; break;
        case ChatType::WHISPER_INFORM: text = head(pp::CS::CHAT_WHISPER_INFORM_GET) + msg; break;
        case ChatType::EMOTE: text = pflag + who + " " + msg; break;  // CHAT_EMOTE_GET "%s "
        case ChatType::PARTY: text = head(pp::CS::CHAT_PARTY_GET) + msg; break;
        case ChatType::PARTY_LEADER: text = head(pp::CS::CHAT_PARTY_LEADER_GET) + msg; break;
        case ChatType::MONSTER_PARTY: text = head(pp::CS::CHAT_MONSTER_PARTY_GET) + msg; break;
        case ChatType::RAID: text = head(pp::CS::CHAT_RAID_GET) + msg; break;
        case ChatType::RAID_LEADER: text = head(pp::CS::CHAT_RAID_LEADER_GET) + msg; break;
        case ChatType::RAID_WARNING: text = head(pp::CS::CHAT_RAID_WARNING_GET) + msg; break;
        case ChatType::GUILD:  // no sender - GUILD_MOTD_TEMPLATE in the guild colour (ChatFrame_OnEvent)
            text = who.empty() ? msg : head(pp::CS::CHAT_GUILD_GET) + msg;
            break;
        case ChatType::OFFICER: text = head(pp::CS::CHAT_OFFICER_GET) + msg; break;
        case ChatType::BATTLEGROUND: text = head(pp::CS::CHAT_BATTLEGROUND_GET) + msg; break;
        case ChatType::BATTLEGROUND_LEADER: text = head(pp::CS::CHAT_BATTLEGROUND_LEADER_GET) + msg; break;
        case ChatType::CHANNEL: {
            // "|Hchannel:channel:N|h[" .. arg4 .. "]|h " .. CHAT_CHANNEL_GET "%s: "
            const size_t dash = m.channelName.find(" - ");
            const std::string shortName = cleanMarkup(dash == std::string::npos ? m.channelName : m.channelName.substr(0, dash));
            text = "[" + std::to_string(channelNumber(m.channelName)) + ". " + shortName + "] " + name + ": " + msg;
            break;
        }
        case ChatType::AFK: text = head(pp::CS::CHAT_AFK_GET) + msg; break;
        case ChatType::DND: text = head(pp::CS::CHAT_DND_GET) + msg; break;
        case ChatType::ACHIEVEMENT:
        case ChatType::GUILD_ACHIEVEMENT: {
            // format(arg1, "|Hplayer:..|h[" .. coloredName .. "]|h"): the bracketed name into %s.
            const std::string bracketed = "[" + who + "]";
            text = rp::emoteReplaceAll(msg.c_str(), &bracketed);
            break;
        }
        case ChatType::IGNORED: text = cleanMarkup(CLIENT_FMT(CHAT_IGNORED, who)); break;
        case ChatType::FILTERED: text = cleanMarkup(CLIENT_FMT(CHAT_FILTERED, who)); break;
        case ChatType::RESTRICTED: text = CLIENT_STR(CHAT_RESTRICTED); break;
        // SYSTEM, LOOT, MONEY, SKILL, COMBAT_*, BG_SYSTEM_*: arg1 as it is. TEXT_EMOTE: the line already
        // carries the name. MONSTER_EMOTE / RAID_BOSS_*: CHAT_*_GET is "" - the session put the name into %s.
        default: text = msg; break;
    }
    // the links are looked for in the message, not in the CHAT_SAY_GET "[Name] says: " head before it.
    pendingSkip_ = !pendingLinks_.empty() && text.size() >= msg.size() &&
                           text.compare(text.size() - msg.size(), msg.size(), msg) == 0
                       ? text.size() - msg.size()
                       : 0;
    addLine(text, chatColor(m.type));
    pendingLinks_.clear();
    pendingSkip_ = 0;
}

// PSP: the channel's number in the order first heard (FrameXML: the client's channel list).
int WorldChat::channelNumber(const std::string& channelName) {
    if (const int n = session_.channelNumberOf(channelName)) return n;  // the joined channel's
    for (size_t i = 0; i < channels_.size(); ++i)
        if (channels_[i] == channelName) return static_cast<int>(i) + 1;
    channels_.push_back(channelName);
    return static_cast<int>(channels_.size());
}

// "chat_blizz" (UI-ChatIcon-Blizz, 32 x 16) from ui.pak, once, on the first GM / DEV line; ~1 KB kept.
bool WorldChat::gmIcon() {
    if (gmIconTried_) return gmIcon_.pixels != nullptr;
    gmIconTried_ = true;
    char path[256];
    pp::fsPath(path, sizeof(path), "data/ui.pak");
    pp::Pak pak;
    uint32_t bytes = 0;
    if (pak.open(path)) {
        const pp::PakEntry* e = pak.find("chat_blizz");
        void* blob = e ? pak.load(e) : nullptr;
        if (blob && pp::textureFromBlob(blob, e->size, gmIcon_, true)) {
            gmIcon_.name = "chat";
            bytes = e->size;
        } else if (blob) {
            pp::pakFree(blob);
        }
        pak.close();
    }
    PSP_LOGI(pp::tag::Gfx, "chat_gm_icon ok=%d kb=%u", gmIcon_.pixels ? 1 : 0, static_cast<unsigned>((bytes + 1023) / 1024));
    return gmIcon_.pixels != nullptr;
}

// Word wrap to the box (UTF-8, the small font's advances).
void WorldChat::addLine(const std::string& text, uint32_t color) {
    const Font& f = kit_.font(kFontSmall);
    const uint32_t now = pp::logUptimeMs();
    const float maxW = kBoxW - 2.0f;
    int added = 0;
    auto push = [&](const std::string& s) {
        lines_.push_back(Line{s, color, now});
        ++added;
    };
    std::string cur;
    float w = 0.0f;
    size_t breakAt = 0;   // bytes of cur up to and including the last space
    float breakW = 0.0f;  // width of those bytes
    const char* s = text.c_str();
    while (*s) {
        const char* p0 = s;
        const uint32_t cp = utf8Next(&s);
        const Glyph* g = f.find(cp);
        // the GM icon (kIconMark) takes its width in the line.
        const float adv = cp == static_cast<uint32_t>(kIconMark) ? iconWidth(f) : g ? static_cast<float>(g->advance) : 0.0f;
        if (w + adv > maxW && !cur.empty()) {
            if (breakAt > 0) {
                push(cur.substr(0, breakAt));
                cur.erase(0, breakAt);
                w -= breakW;
            } else {
                push(cur);
                cur.clear();
                w = 0.0f;
            }
            breakAt = 0;
            breakW = 0.0f;
            if (cur.empty() && cp == ' ') continue;  // no leading space on the next line
        }
        cur.append(p0, static_cast<size_t>(s - p0));
        w += adv;
        if (cp == ' ') {
            breakAt = cur.size();
            breakW = w;
        }
    }
    if (!cur.empty()) push(cur);
    attachLinks(text, static_cast<size_t>(added));  // the item links' places in the wrapped lines
    while (lines_.size() > kHistory) lines_.pop_front();
    // Scrolled back: the view stays on what the player is reading.
    if (scroll_ > 0) scroll_ = std::min(scroll_ + added, std::max(0, static_cast<int>(lines_.size()) - visibleLines_));
}

void WorldChat::update(float dtSec) {
    PROF_SCOPE("chat");
    game::MessageChatData m;
    while (session_.takeChatLine(m)) {
        if (m.type == ChatType::WHISPER) playUiSound(UiSound::WhisperReceived);  // upstream
        addMessage(m);
    }
    pp::WorldSession::TextEmoteHeard t;
    while (session_.takeTextEmote(t)) addTextEmote(t);
}

// where the pending item links ("[name]") stand in the `added` lines just wrapped, in
// order; a link not found in a line is looked for in the next, one wrapped over a line's end is dropped (its two
// halves are no link any more).
void WorldChat::attachLinks(const std::string& text, size_t added) {
    if (pendingLinks_.empty() || added == 0) return;
    const size_t n = std::min(added, lines_.size()), first = lines_.size() - n;
    std::vector<std::string> wrapped, shown;
    for (size_t i = first; i < lines_.size(); ++i) wrapped.push_back(lines_[i].text);
    for (const auto& link : pendingLinks_) shown.push_back(link.second);
    for (const dressup::LinkPlace& p : dressup::placeLinks(text, wrapped, shown, pendingSkip_)) {
        const std::string& name = shown[p.link];
        if (p.offset + name.size() > 0xFFFFu) continue;
        LinkSpan span;
        span.itemId = pendingLinks_[p.link].first;
        span.start = static_cast<uint16_t>(p.offset);
        span.len = static_cast<uint16_t>(name.size());
        lines_[first + p.line].links.push_back(span);
    }
}

void WorldChat::draw(UiRenderer& ui, FocusRing& focus, bool cursorMode) {
    PROF_SCOPE("chat.draw");
    hoverItem_ = 0;
    const Font& f = kit_.font(kFontSmall);
    const float lh = static_cast<float>(f.lineHeight() > 0 ? f.lineHeight() : 12);
    const float top = kBoxBottom - lh * static_cast<float>(visibleLines_);
    if (!cursorMode) scroll_ = 0;
    if (cursorMode) {
        ui.fill(Rect{kBoxX - 2, top - 2, kBoxW + 4, kBoxBottom - top + 4}, 0x70000000u);
        // layers - above the box (a window on the right no longer
        // covers them).
        kit_.button(ui, focus, kIdChatUp, Rect{kBoxX, top - 24, 52, 20}, OWN_STR(SCROLL_UP),
                    scroll_ < static_cast<int>(lines_.size()) - visibleLines_);
        kit_.button(ui, focus, kIdChatDown, Rect{kBoxX + 56, top - 24, 52, 20}, OWN_STR(SCROLL_DOWN), scroll_ > 0);
        // The "Chat" button is gone - a click (Cross) on the chat box opens the input.
        const Rect box{kBoxX - 2, top - 2, kBoxW + 4, kBoxBottom - top + 4};
        if (focus.addStop(kIdChatOpen, box)) ui.fill(box, 0x18FFFFFFu);
    }
    const uint32_t now = pp::logUptimeMs();
    const int n = static_cast<int>(lines_.size());
    const int last = n - 1 - scroll_;
    int row = 0;
    uint32_t linkStops = 0;
    for (int i = last; i >= 0 && row < visibleLines_; --i, ++row) {
        const Line& l = lines_[static_cast<size_t>(i)];
        uint32_t alpha = 255;
        if (!cursorMode && fadeSec_ > 0.0f) {
            const uint32_t age = now - l.bornMs;
            const uint32_t fadeAt = static_cast<uint32_t>(fadeSec_ * 1000.0f);
            if (age >= fadeAt + kFadeMs) continue;
            if (age > fadeAt) alpha = 255u * (fadeAt + kFadeMs - age) / kFadeMs;
        }
        if (alpha < 8) continue;
        const float y = kBoxBottom - lh * static_cast<float>(row + 1);
        drawLine(ui, f, kBoxX, y, l.text, (l.color & 0x00FFFFFFu) | (alpha << 24));
        // the item links take the cursor (declared after the box: they are on top of it).
        if (cursorMode && !l.links.empty()) {
            auto widthOf = [&](size_t from, size_t len) {
                float w = 0.0f;
                std::string plain;
                for (size_t c = from; c < from + len && c < l.text.size(); ++c) {
                    if (l.text[c] == kIconMark && gmIcon_.pixels) w += iconWidth(f);
                    else plain += l.text[c];
                }
                return w + static_cast<float>(f.measure(plain.c_str()));
            };
            for (const LinkSpan& sp : l.links) {
                if (linkStops >= kLinkIds) break;
                const Rect r{kBoxX + widthOf(0, sp.start), y, widthOf(sp.start, sp.len), lh};
                if (r.x + r.w > kBoxX + kBoxW + 2.0f) continue;
                const uint32_t id = kLinkId0 + linkStops++;
                if (focus.addStop(id, r)) {
                    ui.fill(r, 0x30FFFFFFu);
                    hoverItem_ = sp.itemId;
                    hoverRect_ = Rect{r.x + ui.offsetX(), r.y + ui.offsetY(), r.w, r.h};
                }
            }
        }
    }
}

// "|TInterface\ChatFrame\UI-ChatIcon-Blizz.blp:0:2:0:-3|t" - the line's height (the
// font's pixel size here), twice as wide (the art is 32 x 16).
float WorldChat::iconWidth(const Font& f) { return 2.0f * static_cast<float>(f.pixelSize() > 0 ? f.pixelSize() : 10); }

// A chat line; the GM icon where kIconMark stands.
void WorldChat::drawLine(UiRenderer& ui, const Font& f, float x, float y, const std::string& text, uint32_t abgr) {
    if (text.find(kIconMark) == std::string::npos || !gmIcon_.pixels) {
        ui.textShadow(f, x, y, text.c_str(), abgr);
        return;
    }
    const float lh = static_cast<float>(f.lineHeight() > 0 ? f.lineHeight() : 12);
    const float ih = 0.5f * iconWidth(f);
    size_t from = 0;
    for (;;) {
        const size_t at = text.find(kIconMark, from);
        const std::string seg = text.substr(from, at == std::string::npos ? std::string::npos : at - from);
        if (!seg.empty()) {
            ui.textShadow(f, x, y, seg.c_str(), abgr);
            x += static_cast<float>(f.measure(seg.c_str()));
        }
        if (at == std::string::npos) break;
        ui.image(gmIcon_, Rect{x, y + std::floor((lh - ih) * 0.5f), 2.0f * ih, ih}, (abgr & 0xFF000000u) | 0x00FFFFFFu);
        x += 2.0f * ih;
        from = at + 1;
    }
}

// UnitName("target") of a player target (the commands' default name).
std::string WorldChat::targetPlayerName() const {
    if (targetGuid_ == 0) return std::string();
    auto e = session_.entities().getEntity(targetGuid_);
    if (!e || e->getType() != game::ObjectType::PLAYER) return std::string();
    if (const std::string* n = session_.nameOf(targetGuid_)) return *n;
    return targetName_;
}

// ChatFrame.lua's SlashCmdList handlers of the ruRU client (the aliases: GlobalStrings SLASH_*; build
// 120: the client's own - Cyrillic in ruRU - from strings.pak of the running language).
bool WorldChat::socialCommand(const std::string& key, const std::string& args) {
    using CS = pp::CS;
    auto is = [&](std::initializer_list<const char*> names, std::initializer_list<CS> client = {}) {
        for (const char* n : names)
            if (key == n) return true;
        return clientSlash(key, client);
    };
    auto request = [&](int code, const std::string& arg) {
        socialRequest_ = code;
        socialArg_ = arg;
        return true;
    };
    const std::string first = args.substr(0, args.find(' '));
    // SLASH_GUILD (the ruRU aliases the channel list above lacks) / SLASH_OFFICER: the channels.
    const bool guild = is({"gu", "gc"}, {CS::SLASH_GUILD1, CS::SLASH_GUILD2, CS::SLASH_GUILD4});
    if (guild || is({"o", "osay", "officer"}, {CS::SLASH_OFFICER1, CS::SLASH_OFFICER2, CS::SLASH_OFFICER5})) {
        const ChatType t = guild ? ChatType::GUILD : ChatType::OFFICER;
        if (!args.empty()) session_.sendChatMessage(t, args);
        stickyType_ = t;
        return true;
    }
    if (is({"ginvite", "guildinvite"}, {CS::SLASH_GUILD_INVITE1, CS::SLASH_GUILD_INVITE2})) {  // GuildInvite(msg or the target)
        session_.guildInvite(args.empty() ? targetPlayerName() : args);
        return true;
    }
    if (is({"gremove", "guildremove"}, {CS::SLASH_GUILD_UNINVITE1, CS::SLASH_GUILD_UNINVITE2})) {  // GuildUninvite(msg or the target)
        session_.guildUninvite(args.empty() ? targetPlayerName() : args);
        return true;
    }
    if (is({"guildpromote", "gpromote"}, {CS::SLASH_GUILD_PROMOTE1})) return session_.guildPromote(args), true;
    if (is({"gdemote", "guilddemote"}, {CS::SLASH_GUILD_DEMOTE1, CS::SLASH_GUILD_DEMOTE2}))
        return session_.guildDemote(args), true;
    if (is({"guildleader", "gleader"}, {CS::SLASH_GUILD_LEADER1})) return session_.guildSetLeader(args), true;
    if (is({"gmotd", "guildmotd"}, {CS::SLASH_GUILD_MOTD1, CS::SLASH_GUILD_MOTD2})) return session_.guildSetMotd(args), true;
    if (is({"gquit", "guildquit"}, {CS::SLASH_GUILD_LEAVE1, CS::SLASH_GUILD_LEAVE2})) return session_.guildLeave(), true;
    if (is({"guilddisband", "gdisband"}, {CS::SLASH_GUILD_DISBAND1})) return request(4, std::string());  // CONFIRM_GUILD_DISBAND
    if (is({"ginfo", "guildinfo"})) return session_.guildInfoRequest(), true;
    if (is({"groster", "guildroster"}, {CS::SLASH_GUILD_ROSTER1, CS::SLASH_GUILD_ROSTER2})) return request(3, std::string());
    if (is({"friends", "friend"}, {CS::SLASH_FRIENDS1, CS::SLASH_FRIENDS2})) {  // AddOrRemoveFriend(player, note) / ToggleFriendsPanel
        const std::string target = targetPlayerName();
        if (first.empty() && target.empty()) return request(1, std::string());
        const size_t sp = args.find(' ');
        const std::string note = sp == std::string::npos ? std::string() : trim(args.substr(sp + 1));
        session_.addOrRemoveFriend(first.empty() ? target : first, note);
        return true;
    }
    if (is({"removefriend", "remfriend"}, {CS::SLASH_REMOVEFRIEND1, CS::SLASH_REMOVEFRIEND2}))
        return session_.removeFriend(args), true;
    if (is({"ignore"}, {CS::SLASH_IGNORE1})) {  // AddOrDelIgnore(msg) / ToggleIgnorePanel
        const std::string target = targetPlayerName();
        if (args.empty() && target.empty()) return request(5, std::string());
        session_.addOrDelIgnore(args.empty() ? target : args);
        return true;
    }
    if (is({"unignore"})) {  // DelIgnore(msg) / ToggleIgnorePanel
        const std::string target = targetPlayerName();
        if (args.empty() && target.empty()) return request(5, std::string());
        session_.delIgnore(args.empty() ? target : args);
        return true;
    }
    if (is({"who"}, {CS::SLASH_WHO1})) return args.empty() ? request(2, std::string()) : request(6, args);
    if (is({"afk", "away"}, {CS::SLASH_CHAT_AFK1, CS::SLASH_CHAT_AFK4}))
        return session_.sendAwayFlag(false, args), true;  // SendChatMessage(msg, "AFK")
    if (is({"dnd", "busy"}, {CS::SLASH_CHAT_DND2, CS::SLASH_CHAT_DND3, CS::SLASH_CHAT_DND6}))
        return session_.sendAwayFlag(true, args), true;
    return false;
}

} // namespace wowee::ui::psp
