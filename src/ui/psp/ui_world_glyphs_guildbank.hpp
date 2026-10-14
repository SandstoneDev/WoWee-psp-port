// The state and art of GlyphFrame
// (ui_world_glyphs.cpp), GuildBankFrame (ui_world_guildbank.cpp) and ChannelFrame (ui_world_channels.cpp) -
// WorldScreen::GgbUi, made on first use (WorldScreen::ggbUi). The art is read from ui.pak while its frame shows and freed
// after (the pak is opened and closed at once); text.pak's records only while they are needed. Internal to those files.
#pragma once

#include "ui/psp/ui_world_screen.hpp"

#include "platform/psp/psp_glyph_bank_parse.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_texture.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wowee::ui::psp {

namespace gbk = wowee::platform::psp::gbk;

// Widget ids 0x8E00-0x8FFF: glyphs 0x8E00.., channels 0x8E40.., the guild bank 0x8F00...
enum : uint32_t {
    kIdGlyphTab = 0x8E00,       // PlayerTalentFrameTab4 (PlayerGlyphTabTemplate)
    kIdGlyphSocket0 = 0x8E01,   // GlyphFrameGlyph1..6 .. 0x8E06
    kIdGlyphYes = 0x8E08,       // StaticPopup CONFIRM_GLYPH_PLACEMENT / CONFIRM_REMOVE_GLYPH
    kIdGlyphNo = 0x8E09,
    kIdChRow0 = 0x8E40,         // ChannelButton1..20 .. 0x8E53
    kIdChMember0 = 0x8E58,      // ChannelMemberButton1..22 .. 0x8E6D
    kIdChNew = 0x8E70,          // ChannelFrameNewButton (ADD)
    kIdChJoin,                  // ChannelFrameJoinButton (JOIN)
    kIdChListUp,
    kIdChListDown,
    kIdChRosterUp,
    kIdChRosterDown,
    kIdChDName = 0x8E78,        // ChannelFrameDaughterFrame: the name / password boxes, OK, Cancel, the close button
    kIdChDPassword,
    kIdChDOk,
    kIdChDCancel,
    kIdChDClose,
    kIdChDrop0 = 0x8E80,        // a dropdown list's rows .. 0x8E8F
    kIdChPopup1 = 0x8E90,       // the channel popups' buttons / edit box
    kIdChPopup2,
    kIdChPopupEdit,
    kIdGbClose = 0x8F00,        // GuildBankFrame's close button
    kIdGbMode1 = 0x8F01,        // GuildBankFrameTab1..4 (bank / log / money log / tab info) .. 0x8F04
    kIdGbTab0 = 0x8F08,         // GuildBankTab1..6 (the side tabs) .. 0x8F0D
    kIdGbSlot0 = 0x8F10,        // GuildBankColumnNButtonM: slot 0..97 .. 0x8F71
    kIdGbDeposit = 0x8F80,      // GuildBankFrameDepositButton
    kIdGbWithdraw,              // GuildBankFrameWithdrawButton
    kIdGbPurchase,              // GuildBankFramePurchaseButton
    kIdGbInfoSave,              // GuildBankInfoSaveButton
    kIdGbInfoEdit,              // GuildBankTabInfoEditBox
    kIdGbLogUp,
    kIdGbLogDown,
    kIdGbPopupOk = 0x8F90,      // StaticPopup GUILDBANK_WITHDRAW / _DEPOSIT / CONFIRM_BUY_GUILDBANK_TAB
    kIdGbPopupCancel,
    kIdGbMoneyGold,             // MoneyInputFrame's boxes
    kIdGbMoneySilver,
    kIdGbMoneyCopper,
    kIdGbTpName = 0x8FA0,       // GuildBankPopupFrame: the name box, OK, Cancel, the icon list's arrows
    kIdGbTpOk,
    kIdGbTpCancel,
    kIdGbTpUp,
    kIdGbTpDown,
    kIdGbTpIcon0 = 0x8FB0,      // GuildBankPopupButton1..16 .. 0x8FBF
};

struct WorldScreen::GgbUi {
    ~GgbUi();
    // --- GlyphFrame ---
    bool glyphTab = false;                   // GLYPH_TALENT_TAB selected
    platform::psp::Texture glyFrame, glyParts, glyLocked, glyGlow, glySparkle;
    bool glyArtTried = false, glyGlowTried = false;
    uint32_t glyArtBytes = 0;
    platform::psp::Texture rune[21];         // ui.pak gly_rune_0..20 (0 = UI-Glyph-Rune1), read when shown
    bool runeTried[21] = {};
    gbk::GlyphData glyphData;                // text.pak "glyph_data" (kept: the session reads it)
    bool glyphDataTried = false;
    float elapsed[gbk::kGlyphSockets] = {};  // GlyphFrameGlyph_OnUpdate: self.elapsed / self.tintElapsed
    float tint[gbk::kGlyphSockets] = {};
    float glowT = -1.0f;                     // GlyphFrame_PulseGlow: 0..1.6 s, -1 off
    struct Sparkle {
        float t = 0.0f, dur = 0.0f;
        int size = 0;
        bool on = false;
    } sparkle[gbk::kGlyphSockets];
    uint32_t useGlyphSeen = 0;
    int confirm = 0, confirmSocket = -1;     // 1 CONFIRM_GLYPH_PLACEMENT, 2 CONFIRM_REMOVE_GLYPH
    std::string confirmText;
    struct TipLine {
        std::string text;
        uint32_t color = 0xFFFFFFFFu;
        bool wrap = false;
    };
    std::vector<TipLine> tip;                // the focused socket's GameTooltip:SetGlyph (after the windows)
    Rect tipRect{};
    uint32_t lastMs = 0;
    void freeGlyphArt();
    // --- GuildBankFrame ---
    platform::psp::Texture gbFrame, gbCols, gbEmblem, gbTab, gbNewTab, gbNameBd, gbSlot;
    bool gbArtTried = false;
    uint32_t gbArtBytes = 0;
    int gbMode = 1;                          // GuildBankFrame.mode: 1 bank, 2 log, 3 money log, 4 tab info
    bool gbShown = false, gbRaisedUi = false, gbOpenedBags = false, gbGuard = false;
    uint32_t gbOpenSeen = 0, gbTextSeen = 0, gbTextChangedSeen = 0, gbLogSeen = 0;
    struct Held {                            // a vault item on the cursor (the client's cursor type 0xB)
        bool active = false;
        uint8_t tab = 0, slot = 0;
    } gbHeld;
    int gbLogScroll = 0;
    int gbPopup = 0;                         // 1 GUILDBANK_WITHDRAW, 2 GUILDBANK_DEPOSIT, 3 CONFIRM_BUY_GUILDBANK_TAB
    uint32_t gbMoney[3] = {};                // MoneyInputFrame: gold, silver, copper
    int gbOskField = 0;                      // the money box / the info / the tab name the keyboard is for
    std::string gbInfoText;                  // GuildBankTabInfoEditBox (edited)
    bool gbInfoEdited = false;
    int gbInfoScroll = 0;
    std::unordered_map<std::string, uint32_t> tabIconDisplay;  // the side tabs' icons -> a display id (gb_icons)
    std::string tabIconsKey;                 // the icon names they were read for
    // GuildBankPopupFrame (the tab's name and icon, the guild leader's right click on a tab)
    std::string tpName;
    int tpIconSel = -1, tpIconOffset = 0;
    std::vector<std::pair<uint32_t, std::string>> tpIcons;  // (display id, name) while the popup is open
    uint32_t tipEntry = 0, tipCount = 0;     // the focused vault item's tooltip
    Rect tipItemRect{};
    std::string tipTitle;                    // a side tab's tooltip (its name / BUY_GUILDBANK_TAB)
    void freeBankArt();
    // --- ChannelFrame ---
    platform::psp::Texture chFrame, chRowBg, chHl, chOwner, chMod, chTitle;
    bool chArtTried = false;
    uint32_t chArtBytes = 0;
    int chSelected = -1;                     // GetSelectedDisplayChannel - 1 (a display row), -1 none
    std::string chSelectedName;
    int chListOffset = 0, chRosterOffset = 0;
    bool collapsed[3] = {};                  // CollapseChannelHeader: GROUP, WORLD, CUSTOM
    struct DisplayRow {                      // GetChannelDisplayInfo
        std::string name;
        bool header = false, collapsed = false, active = false;
        int category = 0;                    // 0 group, 1 world, 2 custom
        int number = 0, count = 0;
    };
    std::vector<DisplayRow> rows;
    int dropKind = 0;                        // 1 ChannelListDropDown, 2 ChannelRosterDropDown
    int dropRow = -1;                        // the channel row / roster index the list is for
    Rect dropAnchor{};
    uint32_t dropFocusMs = 0;
    int daughter = 0;                        // ChannelFrameDaughterFrame: 1 new channel, 2 join
    std::string dName, dPassword;
    int chOskField = 0;
    int chPopup = 0;                         // 1 CHAT_CHANNEL_INVITE, 2 CHAT_CHANNEL_PASSWORD, 3 CHANNEL_PASSWORD,
                                             // 4 CHANNEL_INVITE
    std::string chPopupChannel, chPopupInviter, chPopupEdit;
    uint32_t chPopupAtMs = 0;
    bool chPopupAnswered = false;
    uint32_t zoneSeen = 0;
    std::unordered_map<uint32_t, std::string> zoneNames;
    std::vector<uint32_t> capitals;          // text.pak "chat_chan": AreaTable flag 0x100
    bool chanDataTried = false;
    std::string chTipTitle, chTipText;
    Rect chTipRect{};
    void freeChannelArt();
};

// One ui.pak texture into t (the blob stays the texture's); adds its size to `bytes`.
inline bool ggbLoad(platform::psp::Pak& pak, const char* name, platform::psp::Texture& t, uint32_t& bytes) {
    const platform::psp::PakEntry* e = pak.find(name);
    void* blob = e ? pak.load(e) : nullptr;
    if (!blob) return false;
    if (!platform::psp::textureFromBlob(blob, e->size, t, true)) {
        platform::psp::pakFree(blob);
        return false;
    }
    t.name = "ggb";
    bytes += e->size;
    return true;
}

}  // namespace wowee::ui::psp
