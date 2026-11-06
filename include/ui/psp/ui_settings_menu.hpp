// Settings menu over the world (START -> Settings).
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
#include "ui/psp/ui_settings_layout.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wowee::ui::psp {

struct FrameStats;

// What the world screen applies (WorldScreen::applySettings). One field per
// row of the menu; the row table binds key -> field (upstream FieldBinding).
struct GraphicsSettings {
    int m2view = 160;        // doodads/buildings view distance, yd (ModelRenderer::viewDistance)
    int playerview = 120;    // other players, yd
    int playermax = 10;      // other players drawn with models: the nearest N (PSP, the crowd's memory and GE)
    int npcview = 70;        // creatures, yd (CreatureRenderer::viewDistance)
    int npcmax = 24;         // nearest creatures drawn
    int terrainsharp = 0;    // floor mips: 0 AUTO, 1 by distance (sharp)
    int sharpdist = 120;     // yd of level 0 on the sharp floor
    int classicfog = 1;     // classic low-distance horizon and authored fog density
    int terraindetail = 1;   // near detail pass
    int graphicspreset = 5; // Low / Medium / High / Ultra / Custom / Adaptive
    int adaptive30 = 1;     // adaptive detail at either FPS target; legacy config key retained
    int framecap = 30;       // 30 or 60 fps
    int remotepredict = 1;   // other players predicted from the last packet (PSP)
    int perfhud = 0;         // fps and ping on screen, left of the minimap (PSP)
    int worldhints = 1;      // PSP: action buttons over NPCs, targets, objects and loot
    int afksit = 300;        // s without input -> sit (0 = off)
    int targetall = 0;       // D-pad right cycles 0 = hostile units only (upstream Tab), 1 = every unit
    int autoloot = 0;        // AUTO_LOOT_DEFAULT_TEXT - loot without the window
    int musicvol = 30;       // Zone music volume, % (0 = off; was wowee.cfg only)
    int spellfx = 1;        // off / you and pet / all casters
    int spellfxreal = 1;    // basic / fancy
    int levelupfx = 1;
    int camfollow = 1;
    int camsmartfollow = 0; // PSP: keep the owner's established stick/camera behavior by default.
    int uinavigation = 1;
    int barassignment = 1;
    int brightness = 100;   // software scene adjustment, not LCD backlight
    int water = 1, particles = 1, weather = 1;
    int ambientvol = 35;
    int weathervol = 35;
    int sfxvol = 100;        // Sounds - effects volume, % (psp_audio sfxSetVolume)
};

class SettingsMenu {
public:
    ~SettingsMenu();
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
    void releaseArt();
    bool openDropdown(size_t row);
    bool updateDropdown(const platform::psp::PadState& pad,float dtSec);
    bool commitValue(size_t row,int value);
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

    platform::psp::Texture headerArt_,borderArt_,dropBoxArt_,dropButtonArt_,dropButtonDownArt_,
        dropHighlightArt_,dropBgArt_,checkArt_,checkboxArt_,categoryArt_,sliderBgArt_,sliderBorderArt_,sliderThumbArt_;
    int dropdownRow_=-1,dropdownSelection_=0;
    Rect dropdownAnchor_{};
    bool artTried_=false;
    std::vector<SettingsLayoutRow> layout_;
    int layoutPage_=-1;
    uint32_t layoutFontRevision_=~0u;
    float scrollY_=0,layoutHeight_=0,layoutRowH_=0;
    GraphicsSettings values_;
    int page_ = 0;
    UiDpadRepeat settingRepeat_;
    FocusRing focus_;
    FocusRing inert_;          // The rows under the language list take no focus
    LanguageFrame picker_;     // Cross on "Language" - the same list as the login screen's
    bool open_ = false;
    uint32_t setLogs_ = 0;
    int langSel_ = 0;          // index in localeInstalled()
    int langOpenSel_ = 0;      // at open(): what the next start uses (wowee.cfg)
    bool prompt_ = false;      // the restart notice is up (open_ stays true under it)
    std::string helpText_;
    std::vector<std::string> helpLines_;
    uint32_t helpFontRevision_=~0u;
    float helpScale_=0,helpTime_=0;
    std::string promptCode_;   // the language picked
};

} // namespace wowee::ui::psp
