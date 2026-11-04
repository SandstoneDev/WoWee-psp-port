#include "ui/psp/ui_settings_menu.hpp"
#include "ui/psp/ui_help_lines.hpp"
#include "ui/psp/ui_art.hpp"
#include "ui/psp/ui_graphics_presets.hpp"
#include <array>
#include <cmath>

#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_frame_budget.hpp"
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
#include <vector>

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
    {{"graphicspreset", "", SettingKind::Enum, 0, 5, 1, "Graphics", "Display", "", "", 5, ""},
     &GraphicsSettings::graphicspreset, PS::SET_GRAPHICS_PRESET, kOwnLabel, PS::SET_GRAPHICS_PRESET_TIP,
     PS::SET_GRAPHICS_PRESET_CHOICES,kNoText,kNoText},
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
    {{"classicfog", "", SettingKind::Bool, 0, 1, 1, "Graphics", "Ground", "", "", 1, ""},
     &GraphicsSettings::classicfog, PS::SET_CLASSIC_FOG, kOwnLabel, PS::SET_CLASSIC_FOG_TIP, kNoText, kNoText, kNoText},
    {{"framecap", "", SettingKind::Int, 30, 60, 30, "Graphics", "Display", "", "", 30, ""},
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
    {{"spellfx", "", SettingKind::Enum, 0, 2, 1, "Graphics", "", "", "", 1, ""},
     &GraphicsSettings::spellfx, PS::SET_SPELLS, kOwnLabel, PS::SET_SPELLS_TIP, PS::SET_SPELLS_CHOICES, kNoText, kNoText},
    {{"spellfxreal", "", SettingKind::Enum, 0, 1, 1, "Graphics", "", "", "", 1, "spellfx"},
     &GraphicsSettings::spellfxreal, PS::SET_SPELL_QUALITY, kOwnLabel, PS::SET_SPELL_QUALITY_TIP, PS::SET_SPELL_QUALITY_CHOICES, kNoText, kNoText},
    {{"levelupfx", "", SettingKind::Bool, 0, 1, 1, "Graphics", "", "", "", 1, "spellfx"},
     &GraphicsSettings::levelupfx, PS::SET_LEVELFX, kOwnLabel, PS::SET_LEVELFX_TIP, kNoText, kNoText, kNoText},
    {{"camfollow", "", SettingKind::Bool, 0, 1, 1, "Graphics", "", "", "", 1, ""},
     &GraphicsSettings::camfollow, PS::SET_CAMFOLLOW, kOwnLabel, PS::SET_CAMFOLLOW_TIP, kNoText, kNoText, kNoText},
    {{"camsmartfollow", "", SettingKind::Enum, 0, 1, 1, "Graphics", "", "", "", 1, "camfollow"},
     &GraphicsSettings::camsmartfollow, PS::SET_CAMERA, kOwnLabel, PS::SET_CAMERA_TIP, PS::SET_CAMERA_CHOICES, kNoText, kNoText},
    {{"uinavigation", "", SettingKind::Enum, 0, 1, 1, "Graphics", "", "", "", 1, ""},
     &GraphicsSettings::uinavigation, PS::SET_UI_NAV, kOwnLabel, PS::SET_UI_NAV_TIP, PS::SET_UI_NAV_CHOICES, kNoText, kNoText},
    {{"barassignment", "", SettingKind::Enum, 0, 1, 1, "Graphics", "", "", "", 1, ""},
     &GraphicsSettings::barassignment, PS::SET_ASSIGNMENT, kOwnLabel, PS::SET_ASSIGNMENT_TIP, PS::SET_ASSIGNMENT_CHOICES, kNoText, kNoText},
    {{"brightness", "", SettingKind::Int, 50, 150, 10, "Graphics", "", "", "", 100, ""},
     &GraphicsSettings::brightness, PS::SET_BRIGHTNESS, kOwnLabel, PS::SET_BRIGHTNESS_TIP, kNoText, kNoText, kNoText},
    {{"water", "", SettingKind::Bool, 0, 1, 1, "Graphics", "", "", "", 1, ""},
     &GraphicsSettings::water, PS::SET_WATER, kOwnLabel, PS::SET_WATER_TIP, kNoText, kNoText, kNoText},
    {{"particles", "", SettingKind::Bool, 0, 1, 1, "Graphics", "", "", "", 1, ""},
     &GraphicsSettings::particles, PS::SET_PARTICLES, kOwnLabel, PS::SET_PARTICLES_TIP, kNoText, kNoText, kNoText},
    {{"weather", "", SettingKind::Bool, 0, 1, 1, "Graphics", "", "", "", 1, ""},
     &GraphicsSettings::weather, PS::SET_WEATHER, kOwnLabel, PS::SET_WEATHER_TIP, kNoText, kNoText, kNoText},
    {{"ambientvol", "", SettingKind::Int, 0, 100, 5, "Audio", "", "", "", 35, ""},
     &GraphicsSettings::ambientvol, PS::SET_AMBIENT, kOwnLabel, PS::SET_SOUND_TIP, kNoText, PS::SET_UNIT_PERCENT, PS::PORT_OFF_LC},
    {{"weathervol", "", SettingKind::Int, 0, 100, 5, "Audio", "", "", "", 35, ""},
     &GraphicsSettings::weathervol, PS::SET_WEATHER_AUDIO, kOwnLabel, PS::SET_SOUND_TIP, kNoText, PS::SET_UNIT_PERCENT, PS::PORT_OFF_LC},
};
constexpr size_t kRowCount = sizeof(kRows) / sizeof(kRows[0]);

constexpr uint32_t kIdReset = 1;
constexpr uint32_t kIdClose = 2;
constexpr uint32_t kIdPromptRestart = 3;
constexpr uint32_t kIdPromptOk = 4;
constexpr uint32_t kIdChoice0 = 300;
constexpr uint32_t kIdCategory0 = 20;
constexpr uint32_t kIdRow0 = 100;
constexpr uint32_t kIdLangRow = kIdRow0 + static_cast<uint32_t>(kRowCount);
constexpr int kPages = 7;
constexpr int kLanguagePage = 6;
const PS kPageLabels[kPages] = {PS::SET_PAGE_GRAPHICS,PS::SET_PAGE_SOUND,PS::SET_PAGE_CONTROLS,
    PS::SET_PAGE_CAMERA,PS::SET_PAGE_INTERFACE,PS::SET_PAGE_NETWORK,PS::SET_PAGE_LANGUAGES};
const char* kPageKeys[kPages] = {
    "framecap brightness graphicspreset terrainsharp sharpdist terraindetail m2view classicfog weather playerview npcview playermax npcmax spellfx spellfxreal levelupfx particles water",
    "musicvol sfxvol ambientvol weathervol",
    "targetall autoloot afksit",
    "camfollow camsmartfollow",
    "uinavigation barassignment perfhud",
    "remotepredict",
    ""
};
const std::vector<size_t>& pageRows(int page, bool language) {
    static const auto tables=[](){
        std::array<std::vector<size_t>,kPages*2> result;
        for(int lang=0;lang<2;++lang)for(int p=0;p<kPages;++p) {
            auto& rows=result[lang*kPages+p];
            // Keep authored section order rather than the order of the schema.
            std::string keys=kPageKeys[p];
            for(size_t pos=0;pos<keys.size();) {
                const size_t end=keys.find(' ',pos);
                const std::string key=keys.substr(pos,end==std::string::npos?end:end-pos);
                for(size_t i=0;i<kRowCount;++i)if(key==kRows[i].desc.key){rows.push_back(i);break;}
                if(end==std::string::npos)break;
                pos=end+1;
            }
            if(p==kLanguagePage && lang)rows.push_back(kRowCount);
        }
        return result;
    }();
    return tables[(language?kPages:0)+page];
}
PS sectionOf(const char* key) {
    const std::string k=std::string(" ")+key+" ";
    auto in=[&](const char* list){return std::string(list).find(k)!=std::string::npos;};
    if(in(" framecap brightness "))return PS::SET_PAGE_DISPLAY;
    if(in(" graphicspreset "))return PS::SET_SECTION_BASE;
    if(in(" terrainsharp sharpdist terraindetail "))return PS::SET_SECTION_TEXTURES;
    if(in(" m2view classicfog weather "))return PS::SET_SECTION_ENVIRONMENT;
    if(in(" playerview npcview playermax npcmax "))return PS::SET_SECTION_UNITS;
    if(in(" spellfx spellfxreal levelupfx particles water "))return PS::SET_PAGE_EFFECTS;
    if(in(" camfollow camsmartfollow "))return PS::SET_PAGE_CAMERA;
    if(in(" targetall autoloot afksit "))return PS::SET_PAGE_CONTROLS;
    if(in(" uinavigation barassignment perfhud "))return PS::SET_PAGE_INTERFACE;
    if(in(" remotepredict "))return PS::SET_PAGE_NETWORK;
    if(in(" musicvol sfxvol ambientvol weathervol "))return PS::SET_PAGE_SOUND;
    return PS::SET_PAGE_GAMEPLAY;
}

PS rowSection(size_t row) {
    static const auto sections=[](){std::array<PS,kRowCount> result{};
        for(size_t i=0;i<kRowCount;++i)result[i]=sectionOf(kRows[i].desc.key);return result;}();
    return row<kRowCount?sections[row]:PS::SET_PAGE_LANGUAGES;
}

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
    if (std::strcmp(r.desc.key,"framecap")==0) return pp::frameTargetFps(v);
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

SettingsMenu::~SettingsMenu(){releaseArt();}
void SettingsMenu::releaseArt(){
    for(auto* art:{&headerArt_,&borderArt_,&dropBoxArt_,&dropButtonArt_,&dropButtonDownArt_,&dropHighlightArt_,
                  &dropBgArt_,&checkArt_,&checkboxArt_,&categoryArt_,&sliderBgArt_,&sliderBorderArt_,&sliderThumbArt_})
        if(art->pixels)pp::textureRelease(*art);
    artTried_=false;
}

bool SettingsMenu::openDropdown(size_t row){
    if(row>=kRowCount || !rowEnabled(row) || kRows[row].field==&GraphicsSettings::graphicspreset || kRows[row].desc.kind!=SettingKind::Enum)return false;
    dropdownRow_=static_cast<int>(row);dropdownSelection_=values_.*(kRows[row].field);
    focus_.cancelPresses();focus_.setFocus(kIdChoice0+static_cast<uint32_t>(dropdownSelection_));return true;
}
bool SettingsMenu::updateDropdown(const pp::PadState& pad,float dtSec){
    const size_t row=static_cast<size_t>(dropdownRow_);const Row& item=kRows[row];
    const int choices=choiceCount(pp::ps(item.choices));
    const uint32_t directions=PSP_CTRL_UP|PSP_CTRL_DOWN|PSP_CTRL_LEFT|PSP_CTRL_RIGHT;
    const uint32_t press=settingRepeat_.step(pad.held&directions,pad.pressed&directions,dtSec,.5f,.14f,.14f);
    if(press&(PSP_CTRL_UP|PSP_CTRL_DOWN)){
        dropdownSelection_=std::clamp(dropdownSelection_+((press&PSP_CTRL_DOWN)?1:-1),0,choices-1);
        focus_.setFocus(kIdChoice0+static_cast<uint32_t>(dropdownSelection_));
    }
    if(pad.pressed&(PSP_CTRL_CIRCLE|PSP_CTRL_TRIANGLE)){
        dropdownRow_=-1;focus_.cancelPresses();focus_.setFocus(kIdRow0+static_cast<uint32_t>(row));return false;
    }
    pp::PadState input=pad;input.held&=~directions;input.pressed&=~directions;
    focus_.update(input,dtSec);
    const uint32_t act=focus_.activated();
    if(act>=kIdChoice0 && act<kIdChoice0+static_cast<uint32_t>(choices)){
        const int selected=static_cast<int>(act-kIdChoice0);
        const bool changed=commitValue(row,selected);
        dropdownRow_=-1;
        focus_.cancelPresses();focus_.setFocus(kIdRow0+static_cast<uint32_t>(row));return changed;
    }
    return false;
}

int SettingsMenu::value(const char* key) const {
    const Row* r = findRow(key);
    return r ? values_.*(r->field) : 0;
}

bool SettingsMenu::setValue(const char* key, int v) {
    const Row* r = findRow(key);
    if (!r) return false;
    return commitValue(static_cast<size_t>(r-kRows),clampToRow(*r,v));
}

// Upstream GameScreen::loadSettings: every value clamped to its row.
void SettingsMenu::load() {
    for (const Row& r : kRows)
        values_.*(r.field) = clampToRow(r, pp::configGetInt(r.desc.key, static_cast<int>(r.desc.defaultValue)));
    // No new key: migrate existing manual configs without replacing their values.
    // New installs and existing adaptive configs select Adaptive by default.
    const int savedPreset=pp::configGetInt("graphicspreset",-1);
    if(savedPreset<0)values_.graphicspreset=pp::configGetInt("adaptive30",1)?kGraphicsAdaptive:kGraphicsCustom;
    applyGraphicsPreset(values_,values_.graphicspreset);
    const GraphicsSettings& s = values_;
    PSP_LOGI(pp::tag::Gfx,
             "settings_load m2view=%d playerview=%d playermax=%d npcview=%d npcmax=%d sharp=%d sharpdist=%d detail=%d fps=%d "
             "predict=%d perfhud=%d afk=%d",
             s.m2view, s.playerview, s.playermax, s.npcview, s.npcmax, s.terrainsharp, s.sharpdist, s.terraindetail, s.framecap,
             s.remotepredict, s.perfhud, s.afksit);
}

void SettingsMenu::open(const GraphicsSettings& live) {
    dropdownRow_=-1;
    values_ = live;
    if(values_.graphicspreset==kGraphicsAdaptive)applyGraphicsPreset(values_,kGraphicsAdaptive);
    for (const Row& r : kRows) values_.*(r.field) = clampToRow(r, values_.*(r.field));
    open_ = true;
    page_ = 0;layoutPage_=-1;scrollY_=0;
    settingRepeat_ = UiDpadRepeat{};
    prompt_ = false;
    picker_.hide();
    focus_.setFocus(kIdCategory0);
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
    page_ = kLanguagePage;
    focus_.setFocus(kIdLangRow);
    picker_.show(focus_);
    PSP_LOGI(pp::tag::Gfx, "settings_language_list shown=1");
    return true;
}

void SettingsMenu::dismiss() {
    dropdownRow_=-1;
    picker_.hide();
    prompt_ = false;
    open_ = false;releaseArt();
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
    dropdownRow_=-1;
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
    open_ = false;releaseArt();
    int changed = 0;
    for (const Row& r : kRows) {
        const std::string now = settingNumberText(values_.*(r.field));
        const std::string had = pp::configGet(r.desc.key, "");
        const bool isDefault = values_.*(r.field) == static_cast<int>(r.desc.defaultValue);
        if (had.empty() ? isDefault : had == now) continue;
        pp::configSet(r.desc.key, now);
        ++changed;
    }
    // Keep the legacy key synchronized for older builds, but presets own it now.
    const std::string adaptive=settingNumberText(values_.adaptive30);
    if(pp::configGet("adaptive30","")!=adaptive){pp::configSet("adaptive30",adaptive);++changed;}
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
    applyGraphicsPreset(values_,kGraphicsAdaptive);
    focus_.setFocus(kIdRow0);
    PSP_LOGI(pp::tag::Gfx, "settings_reset");
}

bool SettingsMenu::rowEnabled(size_t row) const {
    if(row>=kRowCount || !graphicsPresetAllows(values_.graphicspreset,kRows[row].desc.key))return false;
    return settingEnabled(kRows[row].desc,
                          [this](const std::string& key) { return settingNumberText(value(key.c_str())); });
}

bool SettingsMenu::commitValue(size_t row,int v) {
    if(row>=kRowCount || !rowEnabled(row))return false;
    const Row& r=kRows[row];v=clampToRow(r,v);
    if(values_.*(r.field)==v)return false;
    if(r.field==&GraphicsSettings::graphicspreset)applyGraphicsPreset(values_,v);
    else {values_.*(r.field)=v;graphicsSettingEdited(values_,r.desc.key);}
    return true;
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
    if (!commitValue(row,nv)) return false;
    if (++setLogs_ <= 60) PSP_LOGI(pp::tag::Gfx, "settings_set key=%s value=%d", r.desc.key, nv);
    return true;
}

bool SettingsMenu::update(const pp::PadState& pad, float dtSec) {
    if (!open_) return false;
    helpTime_+=std::clamp(dtSec,0.f,.1f);
    if (prompt_) return updatePrompt(pad, dtSec);
    if (picker_.shown()) return updatePicker(pad, dtSec);
    if(dropdownRow_>=0)return updateDropdown(pad,dtSec);
    pp::PadState p = pad;
    constexpr uint32_t directions = PSP_CTRL_LEFT | PSP_CTRL_RIGHT | PSP_CTRL_UP | PSP_CTRL_DOWN;
    p.pressed = (p.pressed & ~directions) | settingRepeat_.step(p.held & directions, p.pressed & directions, dtSec, .5f, .14f, .14f);
    const uint32_t before = focus_.focused();
    const bool onCategory = before >= kIdCategory0 && before < kIdCategory0 + kPages;
    if (onCategory && (p.pressed & PSP_CTRL_RIGHT)) {
        const auto& rows = pageRows(page_, languageRow());
        for (size_t row : rows) if (row == kRowCount || rowEnabled(row)) {
            focus_.setFocus(kIdRow0 + static_cast<uint32_t>(row)); break;
        }
        p.pressed &= ~directions; p.held &= ~directions;
    } else if ((pad.pressed & PSP_CTRL_SQUARE) && !onCategory) {
        focus_.setFocus(kIdCategory0 + static_cast<uint32_t>(page_));
        p.pressed &= ~directions; p.held &= ~directions;
    }
    int turn=(pad.pressed & PSP_CTRL_RTRIGGER) ? 1 : (pad.pressed & PSP_CTRL_LTRIGGER) ? -1 : 0;
    if (turn) {
        page_=(page_+turn+kPages)%kPages;
        const auto& rows=pageRows(page_,languageRow());
        uint32_t selected=kIdClose;
        for (size_t row:rows) if (row==kRowCount || rowEnabled(row)) { selected=kIdRow0+static_cast<uint32_t>(row); break; }
        focus_.setFocus(onCategory ? kIdCategory0 + static_cast<uint32_t>(page_) : selected);
        p.pressed &= ~directions;
        p.held &= ~directions;
    }
    if(before>=kIdRow0 && before<=kIdLangRow && (p.pressed&(PSP_CTRL_UP|PSP_CTRL_DOWN))) {
        const auto& rows=pageRows(page_,languageRow());
        const auto found=std::find(rows.begin(),rows.end(),static_cast<size_t>(before-kIdRow0));
        if(found!=rows.end()) {
            const int direction=(p.pressed&PSP_CTRL_DOWN)?1:-1;
            int next=static_cast<int>(found-rows.begin())+direction;
            while(next>=0 && next<static_cast<int>(rows.size()) && rows[next]<kRowCount && !rowEnabled(rows[next]))next+=direction;
            focus_.setFocus(next<0?kIdCategory0+static_cast<uint32_t>(page_):next>=static_cast<int>(rows.size())?kIdClose:kIdRow0+static_cast<uint32_t>(rows[next]));
            p.pressed&=~directions;p.held&=~directions;
        }
    }
    pp::PadState navigation = p;
    // Left/right edits a value without moving focus to another row or footer button.
    if (before >= kIdRow0 && before <= kIdLangRow) {
        navigation.held &= ~(PSP_CTRL_LEFT | PSP_CTRL_RIGHT);
        navigation.pressed &= ~(PSP_CTRL_LEFT | PSP_CTRL_RIGHT);
    }
    // Settings owns repeat timing. Pass only its pulses to FocusRing so the
    // focus controller cannot start a second, faster repeat from raw held input.
    navigation.held=(navigation.held & ~directions)|(navigation.pressed & directions);
    focus_.update(navigation, dtSec);
    bool changed = false;
    const uint32_t f = focus_.focused();
    if (f >= kIdCategory0 && f < kIdCategory0 + kPages) page_ = static_cast<int>(f-kIdCategory0);
    const bool onRow = f >= kIdRow0 && f < kIdRow0 + kRowCount;
    if (onRow && rowEnabled(f - kIdRow0)) {
        if (p.pressed & PSP_CTRL_LEFT) changed |= step(f - kIdRow0, -1, false);
        if (p.pressed & PSP_CTRL_RIGHT) changed |= step(f - kIdRow0, 1, false);
    }
    if (f == kIdLangRow && languageRow()) {  // nothing to apply now - the language waits for a restart
        if (p.pressed & PSP_CTRL_LEFT) stepLanguage(-1);
        if (p.pressed & PSP_CTRL_RIGHT) stepLanguage(1);
    }
    const uint32_t act = focus_.activated();
    if (act >= kIdCategory0 && act < kIdCategory0 + kPages) {
        page_ = static_cast<int>(act - kIdCategory0);
        for(size_t row : pageRows(page_,languageRow())) if(row==kRowCount || rowEnabled(row)) {
            focus_.setFocus(kIdRow0 + static_cast<uint32_t>(row)); break;
        }
    } else if (act >= kIdRow0 && act < kIdRow0 + kRowCount && rowEnabled(act - kIdRow0)) {
        if(!openDropdown(act-kIdRow0))changed |= step(act - kIdRow0, 1, true);
    } else if (act == kIdLangRow && languageRow()) {
        picker_.show(focus_);  // the list (Left / Right still step)
    } else if (act == kIdReset) {
        restoreDefaults();
        changed = true;
    }
    if (act == kIdClose || (pad.pressed & (PSP_CTRL_TRIANGLE | PSP_CTRL_CIRCLE))) close();
    return changed;
}

void SettingsMenu::draw(UiRenderer& ui, const UiKit& kit, const FrameStats& stats) {
    (void)stats;
    if(!open_)return;
    if(prompt_){drawPrompt(ui,kit);return;}
    if(!artTried_){
        artTried_=true;UiArtPak art("settings");art.load("sp_header",headerArt_);art.load("sp_border",borderArt_);
        art.load("wm_dd",dropBoxArt_);art.load("wm_dd_btn",dropButtonArt_);art.load("wm_dd_btn_dn",dropButtonDownArt_);
        art.load("npc_titlehl",dropHighlightArt_);art.load("wm_dd_bg",dropBgArt_);art.load("wm_dd_check",checkArt_);
        art.load("cb_up",checkboxArt_);art.load("ql_titlehl",categoryArt_);
        art.load("settings_slider_bg",sliderBgArt_);art.load("settings_slider_border",sliderBorderArt_);art.load("settings_slider_thumb",sliderThumbArt_);
    }
    inert_.setFocus(~0u);inert_.update(pp::PadState{},0.f);
    FocusRing& fr=(picker_.shown() || dropdownRow_>=0)?inert_:focus_;
    const Font& fn=kit.font(kFontNormal);const Font& fs=kit.font(kFontSmall);
    auto frame=[&](const Rect& r,uint32_t background){
        ui.fill(Rect{r.x+4,r.y+4,r.w-8,r.h-8},background);
        if(borderArt_.pixels)ui.nineSlice(borderArt_,r,.45f);
        else if(kit.has(kTexDlgBorder))ui.nineSlice(kit.tex(kTexDlgBorder),r,.7f);
    };
    frame(Rect{6,12,468,254},0x80000000u);
    const Rect sidebar{14,35,104,197},panel{124,35,342,197},viewport{132,41,324,185};
    frame(sidebar,0x50000000u);frame(panel,0x30000000u);
    // The client's UI-DialogBox-Header is the same spiked metal title frame as
    // the reference. Reuse its texture instead of drawing a plain text heading.
    const Rect title{182,0,116,32};
    if(headerArt_.pixels)ui.image(headerArt_,title);else frame(title,0xFF171717u);
    ui.textShadow(fn,240,title.y+(title.h-fn.lineHeight())*.5f,OWN_STR(SET_TITLE),kColGold,Align::Center);
    for(int page=0;page<kPages;++page){
        const Rect row{sidebar.x+4,sidebar.y+5+17.f*page,sidebar.w-8,16};
        const bool focused=fr.addStop(kIdCategory0+static_cast<uint32_t>(page),row);
        if(page==page_){
            if(categoryArt_.pixels)ui.image(categoryArt_,row,0xFF20D0BEu,Blend::Add);
            else ui.fill(row,0x6040B0A0u);
        }else if(focused && categoryArt_.pixels)ui.image(categoryArt_,row,0xFF20D0BEu,Blend::Add);
        ui.pushClip(row);ui.textShadow(fs,row.x+5,row.y+(row.h-fs.lineHeight())*.5f,pp::ps(kPageLabels[page]),(focused || page==page_)?kColWhite:kColGold);ui.popClip();
    }
    const auto& rows=pageRows(page_,languageRow());
    const float rowH=std::max(18.f,static_cast<float>(fs.lineHeight()+6));
    constexpr float headingH=15.f;
    if(layoutPage_!=page_ || layoutFontRevision_!=kit.fontRevision() || layoutRowH_!=rowH){
        layoutHeight_=buildSettingsLayout(rows,[](size_t row){return static_cast<int>(rowSection(row));},rowH,headingH,layout_);
        for(const auto& entry:layout_)if(entry.row<kRowCount && kRows[entry.row].field==&GraphicsSettings::graphicspreset){
            layoutHeight_+=growSettingsRow(layout_,entry.row,std::max(52.f,rowH+34.f));break;
        }
        layoutPage_=page_;layoutFontRevision_=kit.fontRevision();layoutRowH_=rowH;scrollY_=0;
    }
    const uint32_t focusedId=fr.focused();
    for(const auto& entry:layout_)if(focusedId==kIdRow0+entry.row)
        scrollY_=settingsScrollTo(scrollY_,entry.top,entry.height,viewport.h,layoutHeight_);
    scrollY_=std::clamp(scrollY_,0.f,std::max(0.f,layoutHeight_-viewport.h));
    const char* tip=dropdownRow_>=0?pp::ps(kRows[dropdownRow_].tip):"";
    ui.pushClip(viewport);
    // The reference separates the top Display block from a larger options
    // block. Keep later subsections inside that block with quiet divider lines,
    // rather than surrounding every short group with another heavy frame.
    size_t firstEnd=0;
    while(firstEnd<layout_.size() && (firstEnd==0 || !layout_[firstEnd].heading))++firstEnd;
    if(firstEnd>0){
        const float firstBottom=layout_[firstEnd-1].top+layout_[firstEnd-1].height;
        frame(Rect{viewport.x,viewport.y-scrollY_,viewport.w-6,firstBottom},0x18000000u);
        if(firstEnd<layout_.size())frame(Rect{viewport.x,viewport.y+firstBottom-scrollY_,viewport.w-6,layoutHeight_-firstBottom},0x18000000u);
    }
    for(const auto& entry:layout_)if(entry.heading){
        const float top=viewport.y+entry.top-headingH-scrollY_;
        if(top+headingH>viewport.y && top<viewport.y+viewport.h){
            ui.textShadow(fs,viewport.x+8,top+3,pp::ps(static_cast<PS>(entry.section)),kColWhite);
            ui.fill(Rect{viewport.x+8,top+headingH-2,viewport.w-22,1},0x505D625Au);
        }
    }
    for(const auto& entry:layout_){
        const size_t i=entry.row;if(i>=kRowCount)continue;
        const float y=viewport.y+entry.top-scrollY_;
        if(y<viewport.y || y+entry.height>viewport.y+viewport.h)continue;
        const Row& item=kRows[i];const Rect row{viewport.x+6,y,viewport.w-18,entry.height};
        const bool enabled=rowEnabled(i);
        const bool focused=enabled && fr.addStop(kIdRow0+static_cast<uint32_t>(i),row);
        if(focused)tip=pp::ps(item.tip);
        if(item.field==&GraphicsSettings::graphicspreset){
            ui.textShadow(fs,row.x+2,row.y+6,labelOf(item),kColWhite);
            char current[64];valueText(item,values_.graphicspreset,current,sizeof(current));
            ui.textShadow(fn,row.x+row.w*.5f,row.y+6,current,kColGold,Align::Center);
            const Rect bar{row.x+8,row.y+28,row.w-16,14};
            if(sliderBgArt_.pixels)ui.tile(sliderBgArt_,Rect{bar.x+2,bar.y+4,bar.w-4,6});
            if(sliderBorderArt_.pixels)ui.nineSlice(sliderBorderArt_,bar,.65f);
            else ui.fill(Rect{bar.x+3,bar.y+7,bar.w-6,2},0xFF585858u);
            const float travel=bar.w-12;
            const Rect thumb{bar.x+6+travel*values_.graphicspreset/5.f-8,bar.y-1,16,16};
            if(sliderThumbArt_.pixels)ui.image(sliderThumbArt_,thumb);
            else ui.fill(Rect{thumb.x+7,thumb.y+4,2,8},kColGold);
            continue;
        }
        const float ty=row.y+(rowH-fs.lineHeight())*.5f;
        ui.pushClip(Rect{row.x+2,row.y,row.w-121,row.h});
        ui.textShadow(fs,row.x+2,ty,labelOf(item),enabled?kColGold:kColGrey);ui.popClip();
        const Rect field{row.x+row.w-115,row.y+2,112,row.h-4};
        char val[64];valueText(item,values_.*(item.field),val,sizeof(val));
        if(item.desc.kind==SettingKind::Int){
            const float range=item.desc.maxValue-item.desc.minValue;
            const float amount=range>0?std::clamp((values_.*(item.field)-item.desc.minValue)/range,0.f,1.f):0;
            const Rect slider{field.x,field.y,75,field.h};
            if(sliderBgArt_.pixels)ui.tile(sliderBgArt_,Rect{slider.x+2,slider.y+4,slider.w-4,std::max(1.f,slider.h-8)});
            if(sliderBorderArt_.pixels)ui.nineSlice(sliderBorderArt_,slider,.65f);
            else ui.fill(Rect{slider.x+3,slider.y+slider.h*.5f,slider.w-6,2},0xFF585858u);
            const Rect thumb{slider.x+6+(slider.w-12)*amount-8,slider.y+(slider.h-16)*.5f,16,16};
            if(sliderThumbArt_.pixels)ui.image(sliderThumbArt_,thumb,enabled?kColWhite:kColGrey);
            else ui.fill(Rect{thumb.x+7,thumb.y+4,2,8},enabled?kColGold:kColGrey);
            ui.pushClip(Rect{field.x+78,field.y,field.w-79,field.h});
            ui.textShadow(fs,field.x+field.w-1,ty,val,enabled?kColWhite:kColGrey,Align::Right);ui.popClip();
        }else if(item.desc.kind==SettingKind::Bool){
            const Rect check{field.x+field.w-20,row.y+(rowH-18)*.5f,18,18};
            if(checkboxArt_.pixels)ui.image(checkboxArt_,check,enabled?kColWhite:kColGrey);
            if(values_.*(item.field) && checkArt_.pixels)ui.image(checkArt_,check,enabled?kColWhite:kColGrey);
            ui.textShadow(fs,check.x-5,ty,val,enabled?kColWhite:kColGrey,Align::Right);
        }else{
            if(focused)dropdownAnchor_=field;
            if(dropBoxArt_.pixels){
                const auto& t=dropBoxArt_;const float scale=field.h/t.uh,left=8*scale,right=7*scale;
                ui.image(t,Rect{field.x,field.y,left,field.h},SrcRect{0,0,8.f,static_cast<float>(t.uh)});
                ui.image(t,Rect{field.x+left,field.y,field.w-left-right,field.h},SrcRect{8,0,static_cast<float>(t.uw-15),static_cast<float>(t.uh)});
                ui.image(t,Rect{field.x+field.w-right,field.y,right,field.h},SrcRect{static_cast<float>(t.uw-7),0,7,static_cast<float>(t.uh)});
            }else ui.fill(field,0xD0101010u);
            const auto& arrow=(dropdownRow_==static_cast<int>(i) && dropButtonDownArt_.pixels)?dropButtonDownArt_:dropButtonArt_;
            if(arrow.pixels)ui.image(arrow,Rect{field.x+field.w-17,field.y+(field.h-16)*.5f,16,16},enabled?kColWhite:kColGrey);
            ui.pushClip(Rect{field.x+6,field.y,field.w-25,field.h});
            ui.textShadow(fs,field.x+field.w*.5f-5,ty,val,enabled?kColWhite:kColGrey,Align::Center);ui.popClip();
        }
    }
    if(page_==kLanguagePage){
        const Rect row{viewport.x+6,viewport.y+headingH,viewport.w-18,22};
        if(languageRow()){
            const bool focused=fr.addStop(kIdLangRow,row);
            if(focused)tip=languageText(fs,lc::Str::Tooltip,pp::localeActive());
            ui.textShadow(fs,row.x+3,row.y+6,"Language",kColGold);
            const auto& langs=pp::localeInstalled();
            const std::string val=langSel_>=0 && langSel_<static_cast<int>(langs.size())?languageName(fn,langs[langSel_]):"?";
            ui.pushClip(Rect{row.x+100,row.y,row.w-103,row.h});
            ui.textShadow(fs,row.x+row.w-3,row.y+6,val.c_str(),kColWhite,Align::Right);ui.popClip();
        }else ui.textShadow(fs,row.x+3,row.y+6,"Language: installed client locale",kColWhite);
    }
    ui.popClip();
    if(layoutHeight_>viewport.h){
        const float thumb=std::max(14.f,viewport.h*viewport.h/layoutHeight_);
        const float y=viewport.y+(viewport.h-thumb)*scrollY_/(layoutHeight_-viewport.h);
        ui.fill(Rect{panel.x+panel.w-7,viewport.y,3,viewport.h},0xFF303030u);
        ui.fill(Rect{panel.x+panel.w-7,y,3,thumb},0xFF9F9F9Fu);
    }
    constexpr float helpY=171,helpWidth=88,helpHeight=54;
    if(helpText_!=tip || helpFontRevision_!=kit.fontRevision() || helpScale_!=ui.scale()){
        helpText_=tip;helpFontRevision_=kit.fontRevision();helpScale_=ui.scale();helpTime_=0;
        helpLines_=helpLines(helpText_,helpWidth,[&](const std::string& text){return kit.controlHintWidth(fs,text.c_str());});
    }
    const float helpLineH=static_cast<float>(std::max(1,fs.lineHeight()));
    const size_t linesPerPage=std::max<size_t>(1,static_cast<size_t>(helpHeight/helpLineH));
    const size_t helpPages=(helpLines_.size()+linesPerPage-1)/linesPerPage;
    const size_t firstLine=helpPages?((static_cast<size_t>(helpTime_/5.f)%helpPages)*linesPerPage):0;
    ui.pushClip(Rect{22,helpY,helpWidth,helpHeight});
    for(size_t n=0;n<linesPerPage && firstLine+n<helpLines_.size();++n)
        kit.controlHint(ui,fs,22,helpY+helpLineH*n,helpLines_[firstLine+n].c_str(),kColGreen);
    ui.popClip();
    kit.button(ui,fr,kIdReset,Rect{20,238,98,22},CLIENT_STR(DEFAULTS));
    kit.button(ui,fr,kIdClose,Rect{354,238,106,22},CLIENT_STR(CLOSE));
    kit.controlHint(ui,fs,128,238,"L/R: categories",kColGrey);
    kit.controlHint(ui,fs,128,250,"{square}: categories",kColGrey);
    if(dropdownRow_>=0){
        const Row& item=kRows[dropdownRow_];const int count=choiceCount(pp::ps(item.choices));
        const float height=count*18.f+12;
        const Rect list{std::min(dropdownAnchor_.x,326.f),std::min(dropdownAnchor_.y+dropdownAnchor_.h-1,232.f-height),136,height};
        ui.fill(list,0xF0101010u);
        if(dropBgArt_.pixels)ui.tile(dropBgArt_,list);
        if(borderArt_.pixels)ui.nineSlice(borderArt_,list,.45f);
        for(int choice=0;choice<count;++choice){
            const Rect row{list.x+5,list.y+6+18*choice,list.w-10,18};
            const bool focused=focus_.addStop(kIdChoice0+static_cast<uint32_t>(choice),row);
            if(focused && dropHighlightArt_.pixels)ui.image(dropHighlightArt_,row,0xFFFFFFFFu,Blend::Add);
            if(values_.*(item.field)==choice && checkArt_.pixels)ui.image(checkArt_,Rect{row.x,row.y+1,16,16});
            char label[64];valueText(item,choice,label,sizeof(label));
            ui.pushClip(Rect{row.x+18,row.y,row.w-20,row.h});
            ui.textShadow(fs,row.x+18,row.y+(row.h-fs.lineHeight())*.5f,label,kColWhite);ui.popClip();
        }
    }
    picker_.draw(ui,kit,focus_,false);
    Rect control;
    if(!picker_.shown() && focus_.focusedRect(control))kit.glyph(ui,kPadCross,
        Rect{std::min(control.x+control.w+1,pp::kScreenW-11.f),control.y+(control.h-10)*.5f,10,10});
    kit.cursor(ui,focus_);
}

} // namespace wowee::ui::psp
