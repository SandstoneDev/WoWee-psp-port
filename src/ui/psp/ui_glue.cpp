// Shared glue-screen pieces (include/ui/psp/ui_glue.hpp).
#include "ui/psp/ui_glue.hpp"

#include "auth/auth_opcodes.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
namespace text = wowee::platform::psp::text;
using game::Class;

namespace {

void loadTex(pp::Pak& pak, const char* name, pp::Texture& t) {
    if (t.pixels) return;
    const pp::PakEntry* e = pak.find(name);
    void* blob = e ? pak.load(e) : nullptr;
    if (blob && !pp::textureFromBlob(blob, e->size, t, true)) pp::pakFree(blob);
}

}  // namespace

// --- GlueArt -------------------------------------------------------------------------------

void GlueArt::load() {
    if (tried) return;
    tried = true;
    char path[256];
    pp::Pak pak;
    pp::fsPath(path, sizeof(path), "data/glue.pak");
    if (pak.open(path)) {
        loadTex(pak, "gl_cs_hl", csHl);
        loadTex(pak, "gl_rot_l", rotL);
        loadTex(pak, "gl_rot_r", rotR);
        loadTex(pak, "gl_alert", alert);
        loadTex(pak, "gl_col_tab", colTab);
        loadTex(pak, "gl_rl_hl", rlHl);
        loadTex(pak, "gl_rl_frame", rlFrame);
        loadTex(pak, "gl_btn_up_b", btnUpB);  // CharacterSelect_DeathKnightSwap
        loadTex(pak, "gl_btn_down_b", btnDownB);
        loadTex(pak, "gl_btn_hl_b", btnHlB);
        if (const pp::PakEntry* e = pak.find("zones")) {
            zones = pak.load(e);
            zonesSize = zones ? e->size : 0;
        }
        pak.close();
    } else {
        PSP_LOGW(pp::tag::Gfx, "glue_pak missing (plain glue screens)");
    }
    pp::fsPath(path, sizeof(path), "data/ui.pak");
    if (pak.open(path)) {
        loadTex(pak, "scr_up", scrUp);
        loadTex(pak, "scr_dn", scrDn);
        loadTex(pak, "scr_up_dis", scrUpDis);
        loadTex(pak, "scr_dn_dis", scrDnDis);
        pak.close();
    }
    uint32_t bytes = zonesSize;
    for (const pp::Texture* t : {&csHl, &rotL, &rotR, &alert, &colTab, &rlHl, &rlFrame, &scrUp, &scrDn, &scrUpDis,
                                 &scrDnDis, &btnUpB, &btnDownB, &btnHlB})
        if (t->pixels) bytes += pp::textureBytes(*t);
    PSP_LOGI(pp::tag::Gfx, "glue_art kb=%u realm_frame=%d highlight=%d zones=%u", static_cast<unsigned>(bytes / 1024u),
             rlFrame.pixels ? 1 : 0, csHl.pixels ? 1 : 0,
             static_cast<unsigned>(zonesSize >= 4 ? *static_cast<const uint32_t*>(zones) : 0u));
}

void GlueArt::release() {
    for (pp::Texture* t : {&csHl, &rotL, &rotR, &alert, &colTab, &rlHl, &rlFrame, &scrUp, &scrDn, &scrUpDis,
                           &scrDnDis, &btnUpB, &btnDownB, &btnHlB})
        if (t->pixels) pp::textureRelease(*t);
    if (zones) pp::pakFree(zones);
    zones = nullptr;
    zonesSize = 0;
    tried = false;
}

const char* GlueArt::zoneName(uint32_t areaId) const {
    if (!zones || zonesSize < 4) return "";
    const auto* b = static_cast<const uint8_t*>(zones);
    uint32_t n = 0;
    std::memcpy(&n, b, 4);
    if (4u + 8u * n > zonesSize) return "";
    uint32_t lo = 0, hi = n;  // sorted by area id
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        uint32_t id = 0, off = 0;
        std::memcpy(&id, b + 4 + 8 * mid, 4);
        std::memcpy(&off, b + 8 + 8 * mid, 4);
        if (id == areaId) return off < zonesSize ? reinterpret_cast<const char*>(b + off) : "";
        if (id < areaId) lo = mid + 1;
        else hi = mid;
    }
    return "";
}

// --- Widgets -------------------------------------------------------------------------------

namespace {
const GlueArt* g_blueButtons = nullptr;  // CharacterSelect_DeathKnightSwap
}  // namespace

void glueButtonsBlue(const GlueArt* art) { g_blueButtons = art && art->btnUpB.pixels ? art : nullptr; }

bool glueButton(UiRenderer& ui, const UiKit& kit, FocusRing& focus, uint32_t id, const Rect& r, const char* label,
                bool enabled, UiFont font) {
    // UiKit::button with the label font chosen (GlueButtonSmallTemplate: GlueFontNormalSmall).
    const bool focused = enabled && focus.addStop(id, r);
    const bool pressed = focused && focus.isPressed(id);
    const pp::Texture* face = &kit.tex(pressed && kit.has(kTexBtnDown) ? kTexBtnDown : kTexBtnUp);
    const pp::Texture* hl = kit.has(kTexBtnHl) ? &kit.tex(kTexBtnHl) : nullptr;
    if (const GlueArt* b = g_blueButtons) {  // the death knight's blue buttons
        face = pressed && b->btnDownB.pixels ? &b->btnDownB : &b->btnUpB;
        if (b->btnHlB.pixels) hl = &b->btnHlB;
    }
    if (face->pixels) ui.nineSlice(*face, r, 1.0f, enabled ? kColWhite : 0xFF808080u);
    else ui.fill(r, 0xFF101060u);
    if (focused && hl) ui.nineSlice(*hl, r, 1.0f, kColWhite, Blend::Add);
    const Font& f = kit.font(font);
    const float ty = r.y + (r.h - static_cast<float>(f.lineHeight())) * 0.5f + (pressed ? 1.0f : 0.0f);
    ui.textShadow(f, r.x + r.w * 0.5f, ty, label, !enabled ? kColGrey : focused ? kColWhite : kGlueGold, Align::Center);
    return focused;
}

float glueWrapped(UiRenderer& ui, const Font& f, float cx, float y, float maxW, const std::string& text, uint32_t col,
                  float lineH, bool draw) {
    std::string line, word;
    auto emit = [&]() {
        if (draw && !line.empty()) ui.text(f, cx, y, line.c_str(), col, Align::Center);
        y += lineH;
        line.clear();
    };
    for (size_t i = 0; i <= text.size(); ++i) {
        const char ch = i < text.size() ? text[i] : ' ';
        if (ch != ' ' && ch != '\n') {
            word += ch;
            continue;
        }
        if (!word.empty()) {
            const std::string tryLine = line.empty() ? word : line + " " + word;
            if (!line.empty() && static_cast<float>(f.measure(tryLine.c_str())) > maxW) {
                emit();
                line = word;
            } else {
                line = tryLine;
            }
            word.clear();
        }
        if (ch == '\n') emit();
    }
    if (!line.empty()) emit();
    return y;
}

// --- GlueDialog ----------------------------------------------------------------------------

void GlueDialog::show(std::string text, const char* button1, const char* button2, bool alert) {
    text_ = std::move(text);
    b1_ = button1;
    b2_ = button2;
    alert_ = alert;
    shown_ = true;
    focusSet_ = false;
}

void GlueDialog::draw(UiRenderer& ui, const UiKit& kit, FocusRing& focus, const GlueArt* art) {
    if (!shown_) return;
    if (!focusSet_) {
        focus.setFocus(b1_ ? kGlueDlgButton1 : kGlueDlgButton2);
        focusSet_ = true;
    }
    // GlueDialogBackground 512 x 256 units, the height grown to the text (GlueDialog_Show):
    // PSP 320 wide, text GlueFontNormalLarge -> kFontNormal, 280 wide.
    const Font& fn = kit.font(kFontNormal);
    const float lineH = static_cast<float>(fn.lineHeight());
    const float textW = alert_ ? 236.0f : 280.0f;
    const float textH = glueWrapped(ui, fn, 0, 0, textW, text_, 0, lineH, false);
    const bool buttons = b1_ || b2_;
    const float h = std::max(alert_ ? 80.0f : 60.0f, 16.0f + textH + (buttons ? 40.0f : 12.0f));
    const Rect dlg{80.0f, (pp::kScreenH - h) * 0.5f, 320.0f, h};
    ui.fill(Rect{0, 0, static_cast<float>(pp::kScreenW), static_cast<float>(pp::kScreenH)},
            opaque_ ? 0xBF000000u : 0x60000000u);  // opaque = RealmList's dim
    if (opaque_) ui.fill(Rect{dlg.x + 4, dlg.y + 4, dlg.w - 8, dlg.h - 8}, 0xFF000000u);
    kit.panel(ui, dlg);
    float cx = dlg.x + dlg.w * 0.5f;
    if (alert_) {
        if (art && art->alert.pixels) ui.image(art->alert, Rect{dlg.x + 14, dlg.y + 14, 28, 28});
        cx += 18.0f;
    }
    glueWrapped(ui, fn, cx, dlg.y + 14, textW, text_, kGlueGold, lineH);
    const float by = dlg.y + dlg.h - 32;
    if (b1_ && b2_) {
        glueButton(ui, kit, focus, kGlueDlgButton1, Rect{cx - 112, by, 106, 22}, b1_);
        glueButton(ui, kit, focus, kGlueDlgButton2, Rect{cx + 6, by, 106, 22}, b2_);
    } else if (b1_ || b2_) {
        glueButton(ui, kit, focus, b1_ ? kGlueDlgButton1 : kGlueDlgButton2, Rect{cx - 60, by, 120, 22},
                   b1_ ? b1_ : b2_);
    }
}

uint32_t GlueDialog::pressed(uint32_t activated) const {
    if (!shown_) return 0;
    if (activated == kGlueDlgButton1 && b1_) return kGlueDlgButton1;
    if (activated == kGlueDlgButton2 && b2_) return kGlueDlgButton2;
    return 0;
}

// --- Strings -------------------------------------------------------------------------------

const char* className(Class cls, bool female) {
    switch (cls) {  // ChrClasses.dbc Name_lang / Name_female_lang (the baker gives the male one where it is empty)
        case Class::WARRIOR: return female ? CLIENT_STR(DBC_CLASS_1_FEMALE) : CLIENT_STR(DBC_CLASS_1);
        case Class::PALADIN: return female ? CLIENT_STR(DBC_CLASS_2_FEMALE) : CLIENT_STR(DBC_CLASS_2);
        case Class::HUNTER: return female ? CLIENT_STR(DBC_CLASS_3_FEMALE) : CLIENT_STR(DBC_CLASS_3);
        case Class::ROGUE: return female ? CLIENT_STR(DBC_CLASS_4_FEMALE) : CLIENT_STR(DBC_CLASS_4);
        case Class::PRIEST: return female ? CLIENT_STR(DBC_CLASS_5_FEMALE) : CLIENT_STR(DBC_CLASS_5);
        case Class::DEATH_KNIGHT: return female ? CLIENT_STR(DBC_CLASS_6_FEMALE) : CLIENT_STR(DBC_CLASS_6);
        case Class::SHAMAN: return female ? CLIENT_STR(DBC_CLASS_7_FEMALE) : CLIENT_STR(DBC_CLASS_7);
        case Class::MAGE: return female ? CLIENT_STR(DBC_CLASS_8_FEMALE) : CLIENT_STR(DBC_CLASS_8);
        case Class::WARLOCK: return female ? CLIENT_STR(DBC_CLASS_9_FEMALE) : CLIENT_STR(DBC_CLASS_9);
        case Class::DRUID: return female ? CLIENT_STR(DBC_CLASS_11_FEMALE) : CLIENT_STR(DBC_CLASS_11);
    }
    return "?";
}

std::string loginError(const std::string& reason) {
    using auth::AuthResult;
    using pp::CS;
    // GlueStrings.lua LOGIN_* (AccountLogin's dialog); the long HTML pages as text without their web links
    // (tools/psp/build_strings_pak.py html_text).
    struct Map {
        AuthResult r;
        CS key;
    };
    static const Map kMap[] = {
        {AuthResult::ACCOUNT_BANNED, CS::GLUE_LOGIN_BANNED},
        {AuthResult::ACCOUNT_INVALID, CS::GLUE_LOGIN_UNKNOWN_ACCOUNT},
        {AuthResult::PASSWORD_INVALID, CS::GLUE_LOGIN_INCORRECT_PASSWORD},
        {AuthResult::ALREADY_ONLINE, CS::GLUE_LOGIN_ALREADYONLINE},
        {AuthResult::OUT_OF_CREDIT, CS::GLUE_LOGIN_NOTIME},
        {AuthResult::BUSY, CS::GLUE_LOGIN_DBBUSY},
        {AuthResult::BUILD_INVALID, CS::GLUE_LOGIN_BADVERSION},
        {AuthResult::BUILD_UPDATE, CS::GLUE_LOGIN_BADVERSION},
        {AuthResult::INVALID_SERVER, CS::GLUE_LOGIN_FAILED},
        {AuthResult::ACCOUNT_SUSPENDED, CS::GLUE_LOGIN_SUSPENDED},
        {AuthResult::ACCESS_DENIED, CS::GLUE_LOGIN_FAILED},
        {AuthResult::PARENTAL_CONTROL, CS::GLUE_LOGIN_PARENTALCONTROL},
        {AuthResult::LOCK_ENFORCED, CS::GLUE_LOGIN_LOCKED_ENFORCED},
        {AuthResult::TRIAL_EXPIRED, CS::GLUE_LOGIN_TRIAL_EXPIRED},
        {AuthResult::ANTI_INDULGENCE, CS::GLUE_LOGIN_ANTI_INDULGENCE},
        {AuthResult::EXPIRED, CS::GLUE_LOGIN_EXPIRED},
        {AuthResult::NO_GAME_ACCOUNT, CS::GLUE_LOGIN_NO_GAME_ACCOUNT},
        {AuthResult::GAME_ACCOUNT_LOCKED, CS::GLUE_LOGIN_GAME_ACCOUNT_LOCKED},
        {AuthResult::UNLOCKABLE_LOCK, CS::GLUE_LOGIN_UNLOCKABLE_LOCK},
        {AuthResult::CONVERSION_REQUIRED, CS::GLUE_LOGIN_CONVERSION_REQUIRED},
    };
    for (const Map& m : kMap)
        if (reason.find(auth::getAuthResultString(m.r)) != std::string::npos) return text::uiPlain(pp::cs(m.key));
    if (reason.find("Disconnected") != std::string::npos || reason.find("Not connected") != std::string::npos)
        return GLUE_STR(LOGIN_SERVER_DOWN);
    if (reason.find("incompatible") != std::string::npos || reason.find("verification") != std::string::npos ||
        reason.find("invalid") != std::string::npos)
        return GLUE_STR(LOGIN_SRP_ERROR);
    return text::uiPlain(GLUE_STR(LOGIN_FAILED));
}

void glueVersionText(UiRenderer& ui, const UiKit& kit, int build) {
    // AccountLoginVersion (BOTTOMLEFT 0,10, GlueFontNormalSmall): VERSION_TEMPLATE "%s %s (%s) (%s)\n%s".
    // GetBuildInfo: versionType VERSION, version, internalVersion, buildType "Release", date - PSP: the port's build
    // in place of the date.
    const Font& fs = kit.font(kFontSmall);
    const std::string v = CLIENT_FMT(GLUE_VERSION_TEMPLATE, GLUE_STR(VERSION), "3.3.5", "12340", "Release",
                                     OWN_FMT(PSP_BUILD_LINE, build));
    const size_t nl = v.find('\n');
    const float lh = static_cast<float>(fs.lineHeight());
    ui.textShadow(fs, 3, pp::kScreenH - 4 - 2 * lh, v.substr(0, nl).c_str(), kGlueGold);
    if (nl != std::string::npos) ui.textShadow(fs, 3, pp::kScreenH - 4 - lh, v.substr(nl + 1).c_str(), kGlueGold);
}

void glueLogo(UiRenderer& ui, const UiKit& kit) {
    // TOPLEFT 3,-7 of 1024 x 768 -> PSP 128 x 64 (the art is baked at half its 512 x 256).
    if (kit.has(kTexLogo)) ui.image(kit.tex(kTexLogo), Rect{1, 2, 128, 64});
}

} // namespace wowee::ui::psp
