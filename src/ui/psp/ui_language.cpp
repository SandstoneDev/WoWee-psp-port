// Choosing the client's language - see include/ui/psp/ui_language.hpp.
#include "ui/psp/ui_language.hpp"

#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_locale.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_strings.hpp"
#include "platform/psp/psp_strings_table.hpp"
#include "ui/psp/ui_glue.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
namespace lc = wowee::platform::psp::locale;

namespace {

// The selector's strings are the client's (lc::Str order; Language has none - it is always the
// English word, lc::text).
constexpr pp::CS kStrKeys[] = {pp::CS::kCount,
                               pp::CS::OPTION_TOOLTIP_LOCALE,
                               pp::CS::OPTION_RESTART_REQUIREMENT,
                               pp::CS::GLUE_RESTART,
                               pp::CS::OKAY,
                               pp::CS::ACCEPT,
                               pp::CS::BACK};
constexpr size_t kStrCount = sizeof(kStrKeys) / sizeof(kStrKeys[0]);

// A language that is not running: its texts from its own strings.pak (localePakPath - data/locale/<xxYY>/ or the
// primary's data/), read the first time that language is picked (the restart notice speaks the picked language
// first), kept for the run (a few hundred bytes). A pak that is missing or lacks a key gives "".
struct OtherLanguage {
    std::string code;
    std::string pool;
    std::vector<int32_t> at;  // lc::Str -> offset in pool, -1 = none
};

bool pakRead(void* ctx, uint32_t offset, void* dst, uint32_t size) {
    auto* p = static_cast<std::pair<pp::Pak*, const pp::PakEntry*>*>(ctx);
    return p->first->readPart(p->second, offset, dst, size);
}

const OtherLanguage& otherLanguage(const std::string& code) {
    static OtherLanguage s_cache[kLanguageMax];
    for (const OtherLanguage& o : s_cache)
        if (o.code == code) return o;
    OtherLanguage* o = &s_cache[0];
    for (OtherLanguage& c : s_cache)
        if (c.code.empty()) {
            o = &c;
            break;
        }
    o->code = code;
    o->pool.clear();
    o->at.assign(kStrCount, -1);
    std::vector<pp::text::WstrWant> wants;
    for (size_t i = 0; i < kStrCount; ++i)
        if (kStrKeys[i] != pp::CS::kCount)
            wants.push_back(pp::text::WstrWant{pp::text::fnv1a64(pp::csKey(kStrKeys[i])), static_cast<uint32_t>(i)});
    char path[256];
    pp::Pak pak;
    uint32_t found = 0;
    if (pp::localePakPath(code, "strings.pak", path, sizeof(path)) && pak.open(path)) {
        if (const pp::PakEntry* e = pak.find("strings")) {
            std::pair<pp::Pak*, const pp::PakEntry*> ctx{&pak, e};
            found = pp::text::wstrLoad(&pakRead, &ctx, e->size, wants, o->pool, o->at).found;
        }
        pak.close();
    }
    PSP_LOGI(pp::tag::Fs, "language_strings code=%s found=%u of=%u", code.c_str(), static_cast<unsigned>(found),
             static_cast<unsigned>(wants.size()));
    return *o;
}

// lc::textFor's lookup: the running language's texts are resident (stringsInit), another's come from its pak.
const char* lookupText(const std::string& code, lc::Str id) {
    const size_t i = static_cast<size_t>(id);
    if (i >= kStrCount || kStrKeys[i] == pp::CS::kCount) return "";
    if (code == pp::localeActive()) return pp::cs(kStrKeys[i]);  // resident (a missing one shows its key)
    const OtherLanguage& o = otherLanguage(code);
    return o.at[i] >= 0 ? o.pool.c_str() + o.at[i] : "";
}

}  // namespace

std::string languageName(const Font& font, const std::string& code) {
    return lc::displayName(code, [&](const char* s) { return font.hasGlyphs(s); });
}

const char* languageText(const Font& font, lc::Str id, const std::string& code) {
    return lc::textFor(id, code, pp::localeActive(), &lookupText, [&](const char* s) { return font.hasGlyphs(s); });
}

std::string languageRestartNotice(const Font& font, const std::string& picked) {
    return lc::restartNotice(pp::localeActive(), picked, &lookupText,
                             [&](const char* s) { return font.hasGlyphs(s); });
}

bool languageChoiceOffered() { return pp::localeInstalled().size() > 1; }

void LanguageFrame::show(FocusRing& focus) {
    shown_ = true;
    const auto& v = pp::localeInstalled();
    const std::string pending = pp::localePending();
    size_t sel = 0;
    for (size_t i = 0; i < v.size() && i < static_cast<size_t>(kLanguageMax); ++i)
        if (v[i] == pending) sel = i;
    focus.setFocus(kIdLanguage0 + static_cast<uint32_t>(sel));
    PSP_LOGI(pp::tag::Loop, "language_frame shown n=%u running=%s pending=%s", static_cast<unsigned>(v.size()),
             pp::localeActive().c_str(), pending.c_str());
}

void LanguageFrame::hide() {
    shown_ = false;
    releaseArt();
}

// UI-CheckBox-Check from the running language's ui.pak (fsPath), only while the frame is up (a few KB).
void LanguageFrame::loadArt() {
    if (artTried_) return;
    artTried_ = true;
    char path[256];
    pp::fsPath(path, sizeof(path), "data/ui.pak");
    pp::Pak pak;
    if (pak.open(path)) {
        const pp::PakEntry* e = pak.find("ql_check");
        void* blob = e ? pak.load(e) : nullptr;
        if (blob && !pp::textureFromBlob(blob, e->size, check_, true)) pp::pakFree(blob);
        pak.close();
    }
    PSP_LOGI(pp::tag::Gfx, "language_art check=%d", check_.pixels ? 1 : 0);
}

void LanguageFrame::releaseArt() {
    if (check_.pixels) pp::textureRelease(check_);
    check_ = pp::Texture{};
    artTried_ = false;
}

// GlueDialog's / the settings window's look: the screen dimmed as behind RealmList (alpha 0.75), the UI-DialogBox
// backdrop over a black ground (its art is translucent - nothing of the screen may show through), the title inside
// the frame, the buttons as wide as the longest name needs, the pad hints under them.
void LanguageFrame::draw(UiRenderer& ui, const UiKit& kit, FocusRing& focus, bool glue) {
    if (!shown_) return;
    loadArt();
    const auto& v = pp::localeInstalled();
    const int n = std::min(static_cast<int>(v.size()), kLanguageMax);
    const Font& fn = kit.font(kFontNormal);
    const Font& fs = kit.font(kFontSmall);
    const uint32_t gold = glue ? kGlueGold : kColGold;
    const std::string& running = pp::localeActive();
    const std::string pending = pp::localePending();

    // The title is "Language" in English in every language - found by anyone, whatever is running.
    const std::string title = lc::text(lc::Str::Language, "enUS");
    std::string names[kLanguageMax];
    float nameW = 0.0f;
    for (int i = 0; i < n; ++i) {
        names[i] = languageName(fn, v[static_cast<size_t>(i)]);
        nameW = std::max(nameW, static_cast<float>(fn.measure(names[i].c_str())));
    }
    const char* accept = languageText(fs, lc::Str::Accept, running);
    const char* back = languageText(fs, lc::Str::Back, running);
    constexpr float kCheck = 18.0f, kPadX = 8.0f, kBtnH = 24.0f, kGlyph = 12.0f;
    const float hintW = 2.0f * (kGlyph + 3.0f) + static_cast<float>(fs.measure(accept) + fs.measure(back)) + 16.0f;
    // The name centred, the check mark's room on both sides of it.
    const float btnW = std::max(140.0f, nameW + 2.0f * (kCheck + 2.0f * kPadX));
    const float titleW = static_cast<float>(fn.measure(title.c_str()));
    const float w = std::min(static_cast<float>(pp::kScreenW) - 16.0f, std::max({btnW, titleW, hintW}) + 40.0f);
    const float rowsH = static_cast<float>(pp::kScreenH) - 92.0f;
    const float step = std::min(kBtnH + 3.0f, rowsH / static_cast<float>(std::max(1, n)));
    const float h = 38.0f + static_cast<float>(n) * step + 30.0f;
    const Rect frame{(pp::kScreenW - w) * 0.5f, (pp::kScreenH - h) * 0.5f, w, h};

    ui.fill(Rect{0, 0, static_cast<float>(pp::kScreenW), static_cast<float>(pp::kScreenH)}, 0xBF000000u);
    ui.fill(Rect{frame.x + 4, frame.y + 4, frame.w - 8, frame.h - 8}, 0xFF000000u);
    kit.panel(ui, frame);
    ui.textShadow(fn, frame.x + w * 0.5f, frame.y + 13.0f, title.c_str(), gold, Align::Center);

    for (int i = 0; i < n; ++i) {
        const Rect b{frame.x + (w - btnW) * 0.5f, frame.y + 36.0f + static_cast<float>(i) * step, btnW,
                     std::min(kBtnH, step)};
        const uint32_t id = kIdLanguage0 + static_cast<uint32_t>(i);
        const bool focused =
            glue ? glueButton(ui, kit, focus, id, b, "", true, kFontNormal) : kit.button(ui, focus, id, b, "");
        const bool chosen = v[static_cast<size_t>(i)] == pending;  // the drop-down's checked row (the CVar's value)
        if (chosen && check_.pixels)
            ui.image(check_, Rect{b.x + kPadX, b.y + (b.h - kCheck) * 0.5f, kCheck, kCheck});
        const float down = focused && focus.isPressed(id) ? 1.0f : 0.0f;  // the pressed face, as the buttons' labels
        const float ty = b.y + (b.h - static_cast<float>(fn.lineHeight())) * 0.5f + down;
        ui.textShadow(fn, b.x + b.w * 0.5f, ty, names[i].c_str(), chosen || focused ? kColWhite : gold, Align::Center);
    }

    // ✕ ACCEPT   ○ BACK
    float hx = frame.x + (w - hintW + 16.0f) * 0.5f;
    const float hy = frame.y + h - 22.0f;
    const float hty = hy + (kGlyph - static_cast<float>(fs.lineHeight())) * 0.5f;
    kit.glyphFill(ui, kPadCross, Rect{hx, hy, kGlyph, kGlyph});
    hx += kGlyph + 3.0f;
    ui.textShadow(fs, hx, hty, accept, gold);
    hx += static_cast<float>(fs.measure(accept)) + 16.0f;
    kit.glyphFill(ui, kPadCircle, Rect{hx, hy, kGlyph, kGlyph});
    hx += kGlyph + 3.0f;
    ui.textShadow(fs, hx, hty, back, gold);
}

int LanguageFrame::pressed(uint32_t activated) const {
    const int n = std::min(static_cast<int>(pp::localeInstalled().size()), kLanguageMax);
    if (!shown_ || activated < kIdLanguage0 || activated >= kIdLanguage0 + static_cast<uint32_t>(n)) return 0;
    return static_cast<int>(activated - kIdLanguage0) + 1;
}

} // namespace wowee::ui::psp
