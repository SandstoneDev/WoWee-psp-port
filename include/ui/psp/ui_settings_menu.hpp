// Graphics settings menu over the world (SELECT -> Menu -> Graphics).
//
// Upstream: ui/settings_schema.hpp describes every setting once (SettingDesc:
// key, label, kind, range, step, default, enabledWhen) and SettingsPanel
// draws those rows, clamps a value to its row's range (clampedToSchema),
// applies each change at once (applySettingSideEffects), greys a row whose
// enabledWhen fails, restores the rows' defaults (restoreSchemaDefaults) and
// clamps what the file says on load (GameScreen::loadSettings). The PSP uses
// the same SettingDesc rows and upstream's settingEnabled/settingNumberText.
//
// PSP: the rows are the PSP renderer's knobs (the desktop's are Vulkan
// passes the GE does not have), labels are the port's own strings (
// ps_b8_port.inc, en / ru) or the client's option names, the widgets are the
// ui_kit's and driven by the D-pad; values are whole numbers; the file is
// wowee.cfg, written once when the menu closes and only for keys that differ
// (upstream saves settings.cfg on every change - a Memory Stick write per
// press would stall frames).
#pragma once

#include "platform/psp/psp_input.hpp"
#include "ui/psp/ui_focus.hpp"
#include "ui/psp/ui_kit.hpp"
#include "ui/psp/ui_language.hpp"  // LanguageFrame
#include "ui/settings_schema.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace wowee::ui::psp {

struct FrameStats;

// What the world screen applies (WorldScreen::applySettings). One field per
// row of the menu; the row table binds key -> field (upstream FieldBinding).
struct GraphicsSettings {
    int m2view = 160;        // doodads/buildings view distance, yd (ModelRenderer::viewDistance)
    int playerview = 120;    // other players, yd
    int npcview = 70;        // creatures, yd (CreatureRenderer::viewDistance)
    int npcmax = 24;         // nearest creatures drawn
    int terrainsharp = 0;    // floor mips: 0 AUTO, 1 by distance (sharp)
    int sharpdist = 120;     // yd of level 0 on the sharp floor
    int terraindetail = 1;   // near detail pass
    int framecap = 60;       // 30 or 60 fps
    int remotepredict = 1;   // other players predicted from the last packet (PSP)
    int afksit = 300;        // s without input -> sit (0 = off)
    int targetall = 0;       // D-pad right cycles 0 = hostile units only (upstream Tab), 1 = every unit
    int autoloot = 0;        // AUTO_LOOT_DEFAULT_TEXT - loot without the window
    int musicvol = 30;       // Zone music volume, % (0 = off; was wowee.cfg only)
    int sfxvol = 100;        // Sounds - effects volume, % (psp_audio sfxSetVolume)
};

class SettingsMenu {
public:
    // wowee.cfg -> values, each clamped to its row's range.
    void load();

    bool isOpen() const { return open_; }
    // Shows `live` (what the world draws now: the L + D-pad tuning keys change
    // some of these without the menu).
    void open(const GraphicsSettings& live);
    // Writes the keys that differ from wowee.cfg, then hides the menu. After a language change the menu
    // stays up as the restart notice (OPTION_RESTART_REQUIREMENT, RESTART / OKAY); close() again dismisses it.
    void close();
    // PSP (the autopilot's "open language" / "close settings"): the language list over the open menu
    // (false without the "Language" row: one language on the card), and the menu gone WITHOUT saving anything
    // (wowee.cfg stays as it is; close() would write the keys whose live value differs from the file).
    bool openLanguagePicker();
    void dismiss();

    // Input while open. Returns true when a value changed (apply it now).
    bool update(const platform::psp::PadState& pad, float dtSec);
    void draw(UiRenderer& ui, const UiKit& kit, const FrameStats& stats);

    const GraphicsSettings& values() const { return values_; }

    // Upstream settingValue / setSettingValue by key, as whole numbers.
    int value(const char* key) const;
    bool setValue(const char* key, int v);

private:
    bool step(size_t row, int dir, bool wrap);
    bool rowEnabled(size_t row) const;
    void restoreDefaults();
    // The row "Language" after the schema rows (the original's Interface Options -> Languages panel,
    // there only with more than one language installed). Not a schema row: its choices are the card's languages
    // and its key (locale) is a code; DEFAULTS leaves it (not a graphics setting, and it needs a restart).
    // Cross opens LanguageFrame (a pick other than the menu's opening value saves and brings up the notice at once,
    // as the login screen's), Left / Right step through the languages (saved when the menu closes).
    bool languageRow() const;
    void stepLanguage(int dir);
    bool updatePicker(const platform::psp::PadState& pad, float dtSec);
    bool updatePrompt(const platform::psp::PadState& pad, float dtSec);
    void drawPrompt(UiRenderer& ui, const UiKit& kit);

    GraphicsSettings values_;
    FocusRing focus_;
    FocusRing inert_;          // The rows under the language list take no focus
    LanguageFrame picker_;     // Cross on "Language" - the same list as the login screen's
    bool open_ = false;
    uint32_t setLogs_ = 0;
    int langSel_ = 0;          // index in localeInstalled()
    int langOpenSel_ = 0;      // at open(): what the next start uses (wowee.cfg)
    bool prompt_ = false;      // the restart notice is up (open_ stays true under it)
    std::string promptCode_;   // the language picked
};

} // namespace wowee::ui::psp
