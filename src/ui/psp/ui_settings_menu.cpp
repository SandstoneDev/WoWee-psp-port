#include "ui/psp/ui_settings_menu.hpp"

#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_locale.hpp"  // the row "Language"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"  // the texts
#include "platform/psp/psp_system.hpp"  // systemRequestExit (RESTART)
#include "ui/psp/ui_glue.hpp"           // glueWrapped (the restart notice)
#include "ui/psp/ui_language.hpp"
#include "ui/psp/ui_login_screen.hpp"  // FrameStats

#include <pspctrl.h>
#include <pspthreadman.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

using pp::CS;
using pp::PS;
constexpr PS kNoText = PS::kCount;    // ps() gives "" for it
constexpr CS kOwnLabel = CS::kCount;  // the row's label is its own string

// A schema row (upstream SettingDesc) bound to its field (upstream
// FieldBinding), plus how the PSP prints the number.
// The texts are looked up when drawn (the running language) - the port's own strings
// (ps_b8_port.inc), the client's where it has the option (AUTO_LOOT_DEFAULT_TEXT, MUSIC_VOLUME, SOUND_VOLUME);
// desc's label / tooltip / choices stay "".
struct Row {
    SettingDesc desc;
    int GraphicsSettings::*field;
    PS label;
    CS clientLabel;  // kOwnLabel: `label` is shown
    PS tip;
    PS choices;   // an Enum's labels, '|'-separated in index order (the same count in every language)
    PS unit;      // after an Int value (kNoText: none)
    PS zeroText;  // an Int at 0 reads as this instead (kNoText: the number)
};

// The index is what wowee.cfg keeps for an Enum (upstream rule): add choices
// at the end, never reorder. terrainsharp stays 0/1 as before the menu.
const Row kRows[] = {
    {{"m2view", "", SettingKind::Int, 40, 240, 20, "Graphics", "World", "", "", 160, ""},
     &GraphicsSettings::m2view, PS::SET_M2VIEW, kOwnLabel, PS::SET_M2VIEW_TIP, kNoText, PS::SET_UNIT_YD, kNoText},
    {{"playerview", "", SettingKind::Int, 30, 200, 10, "Graphics", "", "", "", 120, ""},
     &GraphicsSettings::playerview, PS::SET_PLAYERVIEW, kOwnLabel, PS::SET_PLAYERVIEW_TIP, kNoText, PS::SET_UNIT_YD,
     kNoText},
    {{"playermax", "", SettingKind::Int, 5, 50, 5, "Graphics", "", "", "", 10, ""},
     &GraphicsSettings::playermax, PS::SET_PLAYERMAX, kOwnLabel, PS::SET_PLAYERMAX_TIP, kNoText, kNoText, kNoText},
    {{"npcview", "", SettingKind::Int, 20, 150, 10, "Graphics", "", "", "", 70, ""},
     &GraphicsSettings::npcview, PS::SET_NPCVIEW, kOwnLabel, PS::SET_NPCVIEW_TIP, kNoText, PS::SET_UNIT_YD, kNoText},
    {{"npcmax", "", SettingKind::Int, 0, 64, 4, "Graphics", "", "", "", 24, ""},
     &GraphicsSettings::npcmax, PS::SET_NPCMAX, kOwnLabel, PS::SET_NPCMAX_TIP, kNoText, kNoText, PS::SET_ZERO_NONE},
    {{"terrainsharp", "", SettingKind::Enum, 0, 1, 1, "Graphics", "Ground", "", "", 0, ""},
     &GraphicsSettings::terrainsharp, PS::SET_FLOOR, kOwnLabel, PS::SET_FLOOR_TIP, PS::SET_FLOOR_CHOICES, kNoText,
     kNoText},
    {{"sharpdist", "", SettingKind::Int, 30, 240, 30, "Graphics", "", "", "", 120, "terrainsharp"},
     &GraphicsSettings::sharpdist, PS::SET_SHARPDIST, kOwnLabel, PS::SET_SHARPDIST_TIP, kNoText, PS::SET_UNIT_YD,
     kNoText},
    {{"terraindetail", "", SettingKind::Bool, 0, 1, 1, "Graphics", "", "", "", 1, ""},
     &GraphicsSettings::terraindetail, PS::SET_DETAIL, kOwnLabel, PS::SET_DETAIL_TIP, kNoText, kNoText, kNoText},
    {{"framecap", "", SettingKind::Int, 30, 60, 30, "Graphics", "Display", "", "", 60, ""},
     &GraphicsSettings::framecap, PS::SET_FRAMECAP, kOwnLabel, PS::SET_FRAMECAP_TIP, kNoText, PS::SET_UNIT_FPS,
     kNoText},
    {{"remotepredict", "", SettingKind::Bool, 0, 1, 1, "Graphics", "Network", "", "", 1, ""},
     &GraphicsSettings::remotepredict, PS::SET_PREDICT, kOwnLabel, PS::SET_PREDICT_TIP, kNoText, kNoText, kNoText},
    {{"perfhud", "", SettingKind::Bool, 0, 1, 1, "Graphics", "", "", "", 0, ""},
     &GraphicsSettings::perfhud, PS::SET_PERFHUD, kOwnLabel, PS::SET_PERFHUD_TIP, kNoText, kNoText, kNoText},
    {{"afksit", "", SettingKind::Int, 0, 600, 60, "Graphics", "Character", "", "", 300, ""},
     &GraphicsSettings::afksit, PS::SET_AFKSIT, kOwnLabel, PS::SET_AFKSIT_TIP, kNoText, PS::SET_UNIT_SEC,
     PS::PORT_OFF_LC},
    // targets (hostile only by default, upstream Tab's rule)
    {{"targetall", "", SettingKind::Enum, 0, 1, 1, "Gameplay", "Targeting", "", "", 0, ""},
     &GraphicsSettings::targetall, PS::SET_TARGETS, kOwnLabel, PS::SET_TARGETS_TIP, PS::SET_TARGETS_CHOICES, kNoText,
     kNoText},
    // The client's interface option AUTO_LOOT_DEFAULT_TEXT.
    {{"autoloot", "", SettingKind::Enum, 0, 1, 1, "Gameplay", "Loot", "", "", 0, ""},
     &GraphicsSettings::autoloot, kNoText, CS::AUTO_LOOT_DEFAULT_TEXT, PS::SET_AUTOLOOT_TIP, PS::SET_OFF_ON, kNoText,
     kNoText},
    // The zone music volume, the
    // client's MUSIC_VOLUME (upstream settings music volume 30 on the PSP).
    {{"musicvol", "", SettingKind::Int, 0, 100, 10, "Audio", "Audio", "", "", 30, ""},
     &GraphicsSettings::musicvol, kNoText, CS::MUSIC_VOLUME, PS::SET_MUSIC_TIP, kNoText, PS::SET_UNIT_PERCENT,
     PS::PORT_OFF_LC},
    // sounds - upstream settings "sound effects volume" (0-100), the client's SOUND_VOLUME.
    {{"sfxvol", "", SettingKind::Int, 0, 100, 10, "Audio", "Audio", "", "", 100, ""},
     &GraphicsSettings::sfxvol, kNoText, CS::SOUND_VOLUME, PS::SET_SOUND_TIP, kNoText, PS::SET_UNIT_PERCENT,
     PS::PORT_OFF_LC},
};
constexpr size_t kRowCount = sizeof(kRows) / sizeof(kRows[0]);

constexpr uint32_t kIdReset = 1;
constexpr uint32_t kIdClose = 2;
constexpr uint32_t kIdPromptRestart = 3;  // the restart notice's RESTART
constexpr uint32_t kIdPromptOk = 4;       // its OKAY
constexpr uint32_t kIdRow0 = 100;
constexpr uint32_t kIdLangRow = kIdRow0 + static_cast<uint32_t>(kRowCount);  // "Language", after the rest

namespace lc = wowee::platform::psp::locale;

const Row* findRow(const char* key) {
    for (const Row& r : kRows)
        if (std::strcmp(r.desc.key, key) == 0) return &r;
    return nullptr;
}

const char* labelOf(const Row& r) { return r.clientLabel != kOwnLabel ? pp::cs(r.clientLabel) : pp::ps(r.label); }

int choiceCount(const char* choices) {
    int n = 1;
    for (const char* c = choices; *c; ++c) n += *c == '|';
    return n;
}

// Upstream clampedToSchema: a Bool keeps 0/1, the rest the row's range.
int clampToRow(const Row& r, int v) {
    if (r.desc.kind == SettingKind::Bool) return v ? 1 : 0;
    if (r.desc.kind == SettingKind::Enum) return std::clamp(v, 0, choiceCount(pp::ps(r.choices)) - 1);
    return std::clamp(v, static_cast<int>(r.desc.minValue), static_cast<int>(r.desc.maxValue));
}

void valueText(const Row& r, int v, char* out, size_t n) {
    switch (r.desc.kind) {
        case SettingKind::Bool:
            std::snprintf(out, n, "%s", v ? OWN_STR(PORT_ON) : OWN_STR(PORT_OFF));
            return;
        case SettingKind::Enum: {
            const char* c = pp::ps(r.choices);
            for (int i = 0; i < v && c; ++i) {
                c = std::strchr(c, '|');
                if (c) ++c;
            }
            const char* end = c ? std::strchr(c, '|') : nullptr;
            const int len = c ? (end ? static_cast<int>(end - c) : static_cast<int>(std::strlen(c))) : 1;
            std::snprintf(out, n, "%.*s", len, c ? c : "?");
            return;
        }
        default:
            if (v == 0 && r.zeroText != kNoText) std::snprintf(out, n, "%s", pp::ps(r.zeroText));
            else std::snprintf(out, n, "%d%s", v, pp::ps(r.unit));
            return;
    }
}

} // namespace

int SettingsMenu::value(const char* key) const {
    const Row* r = findRow(key);
    return r ? values_.*(r->field) : 0;
}

bool SettingsMenu::setValue(const char* key, int v) {
    const Row* r = findRow(key);
    if (!r) return false;
    values_.*(r->field) = clampToRow(*r, v);
    return true;
}

// Upstream GameScreen::loadSettings: every value clamped to its row.
void SettingsMenu::load() {
    for (const Row& r : kRows)
        values_.*(r.field) = clampToRow(r, pp::configGetInt(r.desc.key, static_cast<int>(r.desc.defaultValue)));
    const GraphicsSettings& s = values_;
    PSP_LOGI(pp::tag::Gfx,
             "settings_load m2view=%d playerview=%d playermax=%d npcview=%d npcmax=%d sharp=%d sharpdist=%d detail=%d fps=%d "
             "predict=%d perfhud=%d afk=%d",
             s.m2view, s.playerview, s.playermax, s.npcview, s.npcmax, s.terrainsharp, s.sharpdist, s.terraindetail, s.framecap,
             s.remotepredict, s.perfhud, s.afksit);
}

void SettingsMenu::open(const GraphicsSettings& live) {
    values_ = live;
    for (const Row& r : kRows) values_.*(r.field) = clampToRow(r, values_.*(r.field));
    open_ = true;
    prompt_ = false;
    picker_.hide();
    focus_.setFocus(kIdRow0);
    // The row shows what the next start uses (a pick not yet restarted shows as picked).
    const auto& langs = pp::localeInstalled();
    const std::string pending = pp::localePending();
    langSel_ = 0;
    for (size_t i = 0; i < langs.size(); ++i)
        if (langs[i] == pending) langSel_ = static_cast<int>(i);
    langOpenSel_ = langSel_;
    PSP_LOGI(pp::tag::Gfx, "settings_open");
}

// The autopilot's look at the language list - nothing picked, nothing saved.
bool SettingsMenu::openLanguagePicker() {
    if (!open_ || prompt_ || !languageRow()) return false;
    focus_.setFocus(kIdLangRow);
    picker_.show(focus_);
    PSP_LOGI(pp::tag::Gfx, "settings_language_list shown=1");
    return true;
}

void SettingsMenu::dismiss() {
    picker_.hide();
    prompt_ = false;
    open_ = false;
    PSP_LOGI(pp::tag::Gfx, "settings_dismiss saved=0");
}

// The row "Language" - as InterfaceOptionsLanguagesPanel, only with more than one language.
bool SettingsMenu::languageRow() const { return languageChoiceOffered(); }

void SettingsMenu::stepLanguage(int dir) {
    const int n = static_cast<int>(pp::localeInstalled().size());
    if (n < 2) return;
    langSel_ = (langSel_ + dir + n) % n;
    if (++setLogs_ <= 60)
        PSP_LOGI(pp::tag::Gfx, "settings_set key=locale value=%s",
                 pp::localeInstalled()[static_cast<size_t>(langSel_)].c_str());
}

// PSP: saved once here, not per change (upstream saveCallback per control).
// Only keys whose value differs from wowee.cfg are written; a key the file
// lacks is written only when it is not the default, so an untouched menu
// leaves the file alone.
void SettingsMenu::close() {
    if (picker_.shown()) {  // Circle / SELECT on the language list close the list, the menu stays
        picker_.hide();
        focus_.setFocus(kIdLangRow);
        return;
    }
    if (prompt_) {  // the restart notice goes (OKAY / Circle / SELECT) - the language waits for a start
        prompt_ = false;
        open_ = false;
        PSP_LOGI(pp::tag::Gfx, "settings_restart_later code=%s", promptCode_.c_str());
        return;
    }
    PROF_SCOPE("settings.save");
    open_ = false;
    int changed = 0;
    for (const Row& r : kRows) {
        const std::string now = settingNumberText(values_.*(r.field));
        const std::string had = pp::configGet(r.desc.key, "");
        const bool isDefault = values_.*(r.field) == static_cast<int>(r.desc.defaultValue);
        if (had.empty() ? isDefault : had == now) continue;
        pp::configSet(r.desc.key, now);
        ++changed;
    }
    // The language picked in this opening -> wowee.cfg locale (the original's SetCVar("locale")), saved
    // with the rest; one that is not the running language brings up the restart notice (CLIENT_RESTART_ALERT).
    bool restart = false;
    const auto& langs = pp::localeInstalled();
    if (languageRow() && langSel_ != langOpenSel_ && langSel_ >= 0 && langSel_ < static_cast<int>(langs.size())) {
        promptCode_ = langs[static_cast<size_t>(langSel_)];
        const bool cfgDiffers = pp::configGet("locale", "") != promptCode_;
        restart = pp::localePick(promptCode_);
        if (cfgDiffers) ++changed;
    }
    if (!changed) {
        PSP_LOGI(pp::tag::Gfx, "settings_close changed=0");
    } else {
        const uint64_t t0 = sceKernelGetSystemTimeWide();
        int written = 0;
        const bool ok = pp::configSave(&written);
        PSP_LOGI(pp::tag::Fs, "settings_save ok=%d keys=%d ms=%u", ok ? 1 : 0, written,
                 static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u));
        pp::logFlush();
    }
    if (restart) {
        prompt_ = true;
        open_ = true;  // the notice is the menu's last screen: the world keeps its input away until it goes
        focus_.setFocus(kIdPromptRestart);
        PSP_LOGI(pp::tag::Gfx, "settings_restart_prompt code=%s running=%s", promptCode_.c_str(),
                 pp::localeActive().c_str());
    }
}

// The language list over the menu - a pick other than the menu's opening value is saved at once and the
// restart notice comes up (as the login screen's list); the opening value again just closes the list. Triangle (and
// the world's Circle / SELECT, via close()) close it.
bool SettingsMenu::updatePicker(const pp::PadState& pad, float dtSec) {
    focus_.update(pad, dtSec);
    const uint32_t act = focus_.activated();
    if (const int k = picker_.pressed(act)) {
        picker_.hide();
        focus_.setFocus(kIdLangRow);
        langSel_ = k - 1;
        PSP_LOGI(pp::tag::Gfx, "settings_set key=locale value=%s (list)",
                 pp::localeInstalled()[static_cast<size_t>(langSel_)].c_str());
        if (langSel_ != langOpenSel_) close();
    } else if (pad.pressed & PSP_CTRL_TRIANGLE) {
        picker_.hide();
        focus_.setFocus(kIdLangRow);
    }
    return false;
}

// The notice's buttons - RESTART ends the run (main() relaunches the EBOOT), OKAY or Triangle = later.
bool SettingsMenu::updatePrompt(const pp::PadState& pad, float dtSec) {
    focus_.update(pad, dtSec);
    const uint32_t act = focus_.activated();
    if (act == kIdPromptRestart) {
        prompt_ = false;
        open_ = false;
        pp::localeRequestRestart();
        pp::systemRequestExit();
    } else if (act == kIdPromptOk || (pad.pressed & PSP_CTRL_TRIANGLE)) {
        close();
    }
    return false;
}

// StaticPopup's place in the menu's frame: the client's OPTION_RESTART_REQUIREMENT (the picked language's line, then
// the running one's), RESTART and OKAY in the picked language.
void SettingsMenu::drawPrompt(UiRenderer& ui, const UiKit& kit) {
    const Font& fn = kit.font(kFontNormal);
    const float lineH = static_cast<float>(fn.lineHeight());
    const std::string text = languageRestartNotice(fn, promptCode_);
    const float textW = 390.0f;  // OPTION_RESTART_REQUIREMENT ruRU on one line
    const float textH = glueWrapped(ui, fn, 0, 0, textW, text, 0, lineH, false);
    const float h = 16.0f + textH + 44.0f;
    const Rect dlg{(pp::kScreenW - 420.0f) * 0.5f, (pp::kScreenH - h) * 0.5f, 420.0f, h};
    ui.fill(Rect{0, 0, static_cast<float>(pp::kScreenW), static_cast<float>(pp::kScreenH)}, 0xBF000000u);
    ui.fill(Rect{dlg.x + 4, dlg.y + 4, dlg.w - 8, dlg.h - 8}, 0xFF000000u);  // nothing of the world shows through
    kit.panel(ui, dlg);
    glueWrapped(ui, fn, dlg.x + dlg.w * 0.5f, dlg.y + 14.0f, textW, text, kColGold, lineH);
    const float by = dlg.y + dlg.h - 34.0f;
    kit.button(ui, focus_, kIdPromptRestart, Rect{dlg.x + dlg.w * 0.5f - 136.0f, by, 130.0f, 22.0f},
               languageText(fn, lc::Str::Restart, promptCode_));
    kit.button(ui, focus_, kIdPromptOk, Rect{dlg.x + dlg.w * 0.5f + 6.0f, by, 130.0f, 22.0f},
               languageText(fn, lc::Str::Okay, promptCode_));
    kit.cursor(ui, focus_);
}

// Upstream restoreSchemaDefaults: every row back to its defaultValue.
void SettingsMenu::restoreDefaults() {
    for (const Row& r : kRows) values_.*(r.field) = clampToRow(r, static_cast<int>(r.desc.defaultValue));
    PSP_LOGI(pp::tag::Gfx, "settings_reset");
}

bool SettingsMenu::rowEnabled(size_t row) const {
    return settingEnabled(kRows[row].desc,
                          [this](const std::string& key) { return settingNumberText(value(key.c_str())); });
}

// One press: an Int moves a step (X/O wraps past the end), a Bool flips, an
// Enum takes the next choice.
bool SettingsMenu::step(size_t row, int dir, bool wrap) {
    const Row& r = kRows[row];
    const int v = values_.*(r.field);
    int nv = v;
    if (r.desc.kind == SettingKind::Bool) {
        nv = v ? 0 : 1;
    } else if (r.desc.kind == SettingKind::Enum) {
        const int n = choiceCount(pp::ps(r.choices));
        nv = (v + dir + n) % n;
    } else {
        const int lo = static_cast<int>(r.desc.minValue), hi = static_cast<int>(r.desc.maxValue);
        nv = v + dir * static_cast<int>(r.desc.step);
        if (wrap && nv > hi) nv = lo;
        nv = std::clamp(nv, lo, hi);
    }
    if (nv == v) return false;
    values_.*(r.field) = nv;
    if (++setLogs_ <= 60) PSP_LOGI(pp::tag::Gfx, "settings_set key=%s value=%d", r.desc.key, nv);
    return true;
}

bool SettingsMenu::update(const pp::PadState& pad, float dtSec) {
    if (!open_) return false;
    if (prompt_) return updatePrompt(pad, dtSec);
    if (picker_.shown()) return updatePicker(pad, dtSec);
    focus_.update(pad, dtSec);
    bool changed = false;
    const uint32_t f = focus_.focused();
    const bool onRow = f >= kIdRow0 && f < kIdRow0 + kRowCount;
    if (onRow && rowEnabled(f - kIdRow0)) {
        if (pad.pressed & PSP_CTRL_LEFT) changed |= step(f - kIdRow0, -1, false);
        if (pad.pressed & PSP_CTRL_RIGHT) changed |= step(f - kIdRow0, 1, false);
    }
    if (f == kIdLangRow && languageRow()) {  // nothing to apply now - the language waits for a restart
        if (pad.pressed & PSP_CTRL_LEFT) stepLanguage(-1);
        if (pad.pressed & PSP_CTRL_RIGHT) stepLanguage(1);
    }
    const uint32_t act = focus_.activated();
    if (act >= kIdRow0 && act < kIdRow0 + kRowCount && rowEnabled(act - kIdRow0)) {
        changed |= step(act - kIdRow0, 1, true);
    } else if (act == kIdLangRow && languageRow()) {
        picker_.show(focus_);  // the list (Left / Right still step)
    } else if (act == kIdReset) {
        restoreDefaults();
        changed = true;
    }
    if (act == kIdClose || (pad.pressed & PSP_CTRL_TRIANGLE)) close();
    return changed;
}

void SettingsMenu::draw(UiRenderer& ui, const UiKit& kit, const FrameStats& stats) {
    if (!open_) return;
    if (prompt_) {  // the restart notice alone
        drawPrompt(ui, kit);
        return;
    }
    // Under the language list the rows take no focus (its own buttons do).
    inert_.setFocus(~0u);
    inert_.update(pp::PadState{}, 0.0f);
    FocusRing& fr = picker_.shown() ? inert_ : focus_;
    const Rect dlg{40, 6, 400, 260};
    kit.panel(ui, dlg);
    const Font& ft = kit.font(kFontTitle);
    const Font& fn = kit.font(kFontNormal);
    const Font& fs = kit.font(kFontSmall);
    ui.textShadow(ft, dlg.x + dlg.w * 0.5f, dlg.y + 9, OWN_STR(SET_TITLE), kColGold, Align::Center);
    char line[96];
    std::snprintf(line, sizeof(line), "%.1f fps", stats.fps);
    ui.textShadow(fs, dlg.x + dlg.w - 16, dlg.y + 14, line, kColGrey, Align::Right);

    // rows share the space above the buttons once there are more
    // than ten (17 px each up to then, as before).
    const size_t rows = kRowCount + (languageRow() ? 1u : 0u);  // + "Language"
    const float kRowH = std::min(17.0f, (dlg.h - 36.0f - 50.0f) / static_cast<float>(rows));
    const char* tip = "";
    for (size_t i = 0; i < kRowCount; ++i) {
        const Row& r = kRows[i];
        const Rect row{dlg.x + 12, dlg.y + 36 + kRowH * static_cast<float>(i), dlg.w - 24, kRowH};
        const bool enabled = rowEnabled(i);
        // Greyed rather than hidden (upstream): the panel keeps its shape.
        const bool focused = enabled && fr.addStop(kIdRow0 + static_cast<uint32_t>(i), row);
        if (focused) {
            ui.fill(row, 0x30FFFFFFu);
            tip = pp::ps(r.tip);
        }
        const float ty = row.y + (kRowH - fn.lineHeight()) * 0.5f;
        const uint32_t labelCol = !enabled ? kColGrey : focused ? kColWhite : kColGold;
        ui.textShadow(fn, row.x + 6, ty, labelOf(r), labelCol);
        char val[48];
        valueText(r, values_.*(r.field), val, sizeof(val));
        if (focused) {
            std::snprintf(line, sizeof(line), "<  %s  >", val);
            ui.textShadow(fn, row.x + row.w - 6, ty, line, kColWhite, Align::Right);
        } else {
            ui.textShadow(fn, row.x + row.w - 6, ty, val, enabled ? kColWhite : kColGrey, Align::Right);
        }
    }
    if (languageRow()) {
        // "Language" - always in English (findable whatever language runs), the languages by
        // their own names (psp_locale_rules.hpp), the client's tooltip.
        const Rect row{dlg.x + 12, dlg.y + 36 + kRowH * static_cast<float>(kRowCount), dlg.w - 24, kRowH};
        const bool focused = fr.addStop(kIdLangRow, row);
        const std::string& running = pp::localeActive();
        if (focused) {
            ui.fill(row, 0x30FFFFFFu);
            tip = languageText(fs, lc::Str::Tooltip, running);
        }
        const float ty = row.y + (kRowH - fn.lineHeight()) * 0.5f;
        const std::string label = lc::text(lc::Str::Language, "enUS");  // always "Language", in English
        ui.textShadow(fn, row.x + 6, ty, label.c_str(), focused ? kColWhite : kColGold);
        const auto& langs = pp::localeInstalled();
        const bool known = langSel_ >= 0 && langSel_ < static_cast<int>(langs.size());
        const std::string val = known ? languageName(fn, langs[static_cast<size_t>(langSel_)]) : std::string("?");
        if (focused) {
            std::snprintf(line, sizeof(line), "<  %s  >", val.c_str());
            ui.textShadow(fn, row.x + row.w - 6, ty, line, kColWhite, Align::Right);
        } else {
            ui.textShadow(fn, row.x + row.w - 6, ty, val.c_str(), kColWhite, Align::Right);
        }
    }
    ui.text(fs, dlg.x + 18, dlg.y + 36 + kRowH * static_cast<float>(rows) + 4, tip, kColGreen);
    const float by = dlg.y + dlg.h - 34;
    kit.button(ui, fr, kIdReset, Rect{dlg.x + 20, by, 170, 22}, CLIENT_STR(DEFAULTS));
    kit.button(ui, fr, kIdClose, Rect{dlg.x + dlg.w - 190, by, 170, 22}, CLIENT_STR(CLOSE));
    picker_.draw(ui, kit, focus_, false);  // the world's look (the settings window's buttons)
    kit.cursor(ui, focus_);
}

} // namespace wowee::ui::psp
