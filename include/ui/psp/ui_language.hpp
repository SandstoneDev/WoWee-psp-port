// Choosing the client's language (docs/PSP_PORT_ARCHITECTURE.md, platform/psp/psp_locale.hpp).
//
// The original offers it in Interface Options -> Languages (InterfaceOptionsLanguagesPanel, registered only when
// GetExistingLocales() has more than one): a drop-down of the installed locales (the CVar's value checked), the CVar
// "locale", then CLIENT_RESTART_ALERT. The PSP offers it where the player can reach it without a world:
//   - the login screen (AccountLogin): "Language" in the right-hand column of glue buttons -> LanguageFrame, then a
//     GlueDialog OPTION_RESTART_REQUIREMENT with RESTART / OKAY;
//   - the world's settings menu (SELECT -> Menu -> Graphics): the row "Language" (Cross opens the same LanguageFrame,
//     Left / Right step through the languages), the restart notice in the menu's frame (ui_settings_menu.cpp).
// LanguageFrame is the drop-down as a modal list on the client's art: the dialog backdrop (UI-DialogBox, the glue
// dialogs' and the settings window's), the screen behind dimmed (RealmList's 0.75) and nothing of it showing
// through, one glue button per language sized to the longest name, the chosen one with UI-CheckBox-Check (ui.pak
// "ql_check", the check boxes' art) and its name in the highlight colour, the pad hints ✕ ACCEPT / ○ BACK.
// Shown only when the card has more than one language (data/locales.txt with an extra whose folder exists).
// Names are the languages' own (psp_locale_rules.hpp), with the ASCII name when the font has no glyph for a letter.
// The selector's strings are the client's, from the strings.pak of the language they speak - the
// running one's resident, a picked one's read from its own pak (data/locale/<xxYY>/) when first needed; "Language"
// itself stays English (owner).
#pragma once

#include "platform/psp/psp_locale_rules.hpp"
#include "platform/psp/psp_texture.hpp"
#include "ui/psp/ui_focus.hpp"
#include "ui/psp/ui_kit.hpp"
#include "ui/psp/ui_renderer.hpp"

#include <cstdint>
#include <string>

namespace wowee::ui::psp {

// IDs: the login screen's button, the frame's buttons (+ index in localeInstalled()).
inline constexpr uint32_t kIdLanguageButton = 0x7710;
inline constexpr uint32_t kIdLanguage0 = 0x7711;
inline constexpr int kLanguageMax = 12;

// `code`'s name for the selector, drawable in `font`.
std::string languageName(const Font& font, const std::string& code);
// One of the selector's strings in `code`'s language, or the running language's when `font` cannot draw it.
const char* languageText(const Font& font, platform::psp::locale::Str id, const std::string& code);
// OPTION_RESTART_REQUIREMENT after `picked` was chosen: in that language, then in the running one.
std::string languageRestartNotice(const Font& font, const std::string& picked);
// More than one language on the card: the selector is offered.
bool languageChoiceOffered();

// The drop-down as a modal frame (glue screens and the world's settings menu).
class LanguageFrame {
public:
    ~LanguageFrame() { releaseArt(); }
    void show(FocusRing& focus);
    void hide();  // the check mark's texture goes with it
    bool shown() const { return shown_; }
    // glue: glue buttons (GlueButtonTemplate) and the glue gold; else the world's UIPanelButton look (kit.button).
    void draw(UiRenderer& ui, const UiKit& kit, FocusRing& focus, bool glue);
    // The language a button picks (index in localeInstalled() + 1), 0 for anything else.
    int pressed(uint32_t activated) const;

private:
    void loadArt();
    void releaseArt();
    bool shown_ = false;
    bool artTried_ = false;
    platform::psp::Texture check_;  // UI-CheckBox-Check
};

} // namespace wowee::ui::psp
