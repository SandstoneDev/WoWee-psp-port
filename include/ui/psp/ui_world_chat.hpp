// Chat window of the world screen (src/ui/psp/ui_world_chat.cpp).
//
// Receives: the lines WorldSession's chat path hands over (upstream
// ChatHandler history) and the text emotes it heard, written with the
// EmotesText table ("emotetext" in characters.pak, else a small built-in
// table). Shows the last `chatlines` lines bottom left, fading after
// `chatfade` seconds; in the cursor mode (SELECT) every line is shown and the
// history scrolls. Sends: a click on the chat box opens the system
// keyboard (psp_osk); the typed line goes out as upstream's chat panel sends
// it (/s /y /w /r /e, /dance... -> CMSG_TEXT_EMOTE).
#pragma once

#include "platform/psp/psp_input.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_strings.hpp"  // The client's slash commands (SLASH_*)
#include "platform/psp/psp_texture.hpp"  // UI errors: the GM icon
#include "platform/psp/psp_world_session.hpp"
#include "rendering/psp/psp_emote_text.hpp"
#include "ui/psp/ui_focus.hpp"
#include "ui/psp/ui_kit.hpp"
#include "ui/psp/ui_renderer.hpp"

#include <cstdint>
#include <deque>
#include <initializer_list>
#include <string>
#include <vector>

namespace wowee::ui::psp {

class WorldChat {
public:
    WorldChat(const UiKit& kit, platform::psp::WorldSession& session);
    ~WorldChat();
    WorldChat(const WorldChat&) = delete;
    WorldChat& operator=(const WorldChat&) = delete;

    // "emotetext" from characters.pak (kept in RAM, ~50-100 KB); without it
    // the built-in table (PSP) answers the common emotes.
    void loadTable(platform::psp::Pak& charPak);
    // The pad the world may see: idle while the keyboard is open and until
    // every button is up after it closed (the keyboard's X must not jump).
    platform::psp::PadState filterPad(const platform::psp::PadState& pad);
    // Session lines -> window.
    void update(float dtSec);
    // Cursor mode: a widget of the chat was activated (true = handled).
    bool activate(uint32_t id);
    // The chat pass. cursorMode: all lines, background, the chat buttons.
    void draw(UiRenderer& ui, FocusRing& focus, bool cursorMode);
    bool inputActive() const;
    // The unit a text emote is aimed at (upstream: the current target); none
    // until the PSP selects targets.
    void setTarget(uint64_t guid, const std::string& name) {
        targetGuid_ = guid;
        targetName_ = name;
    }

public:
    struct LinkSpan {  // dressup: an item link's "[name]" in a wrapped line (bytes); public: the transfer carry
        uint32_t itemId = 0;
        uint16_t start = 0, len = 0;
    };

private:
    struct Line {
        std::string text;
        uint32_t color;   // 0xAABBGGRR, alpha from the fade
        uint32_t bornMs;
        std::vector<LinkSpan> links{};  // dressup: the item links in it
    };
    void openInput();
    void submit(const std::string& input);
    void addMessage(const game::MessageChatData& m);
    void addTextEmote(const platform::psp::WorldSession::TextEmoteHeard& t);
    void addLine(const std::string& text, uint32_t color);
    bool sendEmote(const std::string& key, const std::string& shown);
    const char* commandOfRow(int row) const;

    const UiKit& kit_;
    platform::psp::WorldSession& session_;
    std::deque<Line> lines_;          // wrapped lines, oldest first
    int scroll_ = 0;                  // lines scrolled back from the newest (cursor mode)
    int visibleLines_ = 6;            // wowee.cfg chatlines
    float fadeSec_ = 30.0f;           // wowee.cfg chatfade (0 = never)
    bool waitRelease_ = false;
    game::ChatType stickyType_ = game::ChatType::SAY;  // upstream: the chat type dropdown
    std::string stickyTarget_;        // whisper target of the sticky type
    uint64_t targetGuid_ = 0;
    std::string targetName_;
    void* tableBlob_ = nullptr;       // "emotetext" (pakFree)
    rendering::psp::EmoteTextView table_;

    // Chat button (owner: "Chat" on the window bar, back to the game
    // after sending). The bar (ui_world_windows.cpp) draws the button with
    // this id; activate() opens the keyboard as before.
public:
    static constexpr uint32_t kBarButtonId = 0x5A01;
    // True once after a typed line was accepted and sent (the world screen
    // leaves the cursor mode).
    bool takeSent() {
        const bool s = sent_;
        sent_ = false;
        return s;
    }

private:
    bool sent_ = false;

    // UI errors: the GM / DEV flag is the GM icon (ui.pak "chat_blizz",
    // UI-ChatIcon-Blizz, loaded on the first GM line, ~1 KB); kIconMark stands for it in a line's text
    // (cleanMarkup turns control bytes into spaces, so it comes from nowhere else).
    static constexpr char kIconMark = '\x01';
    bool gmIcon();  // loads the icon once; false = none (the flag is CHAT_FLAG_GM)
    static float iconWidth(const Font& f);
    void drawLine(UiRenderer& ui, const Font& f, float x, float y, const std::string& text, uint32_t abgr);
    int channelNumber(const std::string& channelName);
    platform::psp::Texture gmIcon_;
    bool gmIconTried_ = false;
    std::vector<std::string> channels_;  // PSP: channel numbers in the order first heard

    // party: UnitPopup WHISPER (ChatFrame_SendTell): the input opens on a whisper to `name`.
public:
    void openWhisper(const std::string& name);
    // inspect: SLASH_INSPECT1..4 ("/ins", "/inspect": InspectUnit("target")) -
    // true once; the world screen opens InspectFrame on its target.
    bool takeInspect() {
        const bool b = inspectWanted_;
        inspectWanted_ = false;
        return b;
    }

    // dressup: the item links of the lines ("|Hitem:<id>:...|h[name]|h" - ChatFrame's
    // hyperlinks) take the cursor in the interface mode (ids kLinkId0..), Cross on one opens the input as the box
    // does; the one under the cursor this frame (its GameTooltip - the original shows it on a click, the PSP
    // where the cursor is - and L + Triangle = Ctrl+click: DressUpItemLink). `anchor`: its screen rect.
    static constexpr uint32_t kLinkId0 = 0x63C0, kLinkIds = 32;
    uint32_t hoverItem(Rect* anchor = nullptr) const {
        if (anchor) *anchor = hoverRect_;
        return hoverItem_;
    }

private:
    bool inspectWanted_ = false;  // inspect
    std::vector<std::pair<uint32_t, std::string>> pendingLinks_;  // addMessage -> addLine: (item, "[name]")
    size_t pendingSkip_ = 0;                                       // bytes of the line's head before the message
    void attachLinks(const std::string& text, size_t added);       // the last `added` lines (of `text`) get their spans
    uint32_t hoverItem_ = 0;
    Rect hoverRect_{};
    // dressup (end)

    // social: ChatFrame.lua's social slash commands (SLASH_GUILD_*, SLASH_OFFICER, SLASH_FRIENDS,
    // SLASH_REMOVEFRIEND, SLASH_IGNORE, SLASH_UNIGNORE, SLASH_WHO, SLASH_CHAT_AFK / _DND). The ones that need a frame
    // are taken once by the world screen (WorldScreen::socialChatRequest): 1 /friends without a name
    // (ToggleFriendsPanel), 2 /who without text, 3 /groster, 4 /gdisband, 5 /ignore / /unignore without a name
    // (ToggleIgnorePanel), 6 /who <text> (WhoFrameEditBox:SetText).
public:
    bool takeSocialRequest(int& code, std::string& arg) {
        if (socialRequest_ == 0) return false;
        code = socialRequest_;
        arg = socialArg_;
        socialRequest_ = 0;
        socialArg_.clear();
        return true;
    }

private:
    bool socialCommand(const std::string& key, const std::string& args);
    // chat channels (ui_world_channels.cpp): SLASH_JOIN / LEAVE / LIST_CHANNEL / CHAT_* and /1../10 (ChatFrame.lua).
    bool channelCommand(const std::string& key, const std::string& args);
    // `key` (rp::emoteCommandKey: no '/', lower case) is one of the client's slash commands - GlobalStrings
    // SLASH_<CMD><n> of the running language (strings.pak; the ruRU client's Cyrillic aliases).
    static bool clientSlash(const std::string& key, std::initializer_list<platform::psp::CS> cmds);
    std::string targetPlayerName() const;  // UnitName("target") when UnitIsPlayer("target"), else ""
    int socialRequest_ = 0;
    std::string socialArg_;
    // social (end)
};

} // namespace wowee::ui::psp
