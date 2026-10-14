// MovieFrame / CinematicsFrame of the original (include/ui/psp/ui_movie.hpp).
#include "ui/psp/ui_movie.hpp"

#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "ui/psp/ui_glue.hpp"

#include <pspiofilemgr.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

// GlueXML MovieFrame.lua MovieList[version][index].video (the PSP keeps one resolution: 480 x 272 made from the
// _1024 file) and MovieVolumeList[version].
const char* const kGlueList[3][2] = {
    {"Logo", "WOW_Intro"},      // [1] the base game
    {nullptr, "WOW_Intro_BC"},  // [2] the first expansion (index 1: video = nil)
    {nullptr, "WOW_Intro_LK"},  // [3] the second expansion (index 1: video = nil)
};
const int kGlueVolume[3] = {100, 150, 250};
constexpr int kGameVolume = 150;  // FrameXML MovieFrame_PlayMovie: volume = volume or 150 (PLAY_MOVIE gives none)
constexpr uint32_t kFadeInMs = 1000, kFadeOutMs = 500;  // MOVIE_CAPTION_FADE_TIME, half of it to hide

void moviePath(char* out, size_t cap, const char* name, const char* ext) {
    char rel[96];
    std::snprintf(rel, sizeof(rel), "data/movies/%s.%s", name, ext);
    pp::fsPath(out, cap, rel);
}

// A small file read whole: open, read, close (no descriptor stays open).
bool readSmall(const char* path, std::string& out, uint32_t maxBytes) {
    const SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return false;
    const SceOff size = sceIoLseek(fd, 0, PSP_SEEK_END);
    bool ok = size > 0 && size <= static_cast<SceOff>(maxBytes);
    if (ok) {
        out.resize(static_cast<size_t>(size));
        sceIoLseek(fd, 0, PSP_SEEK_SET);
        ok = sceIoRead(fd, &out[0], static_cast<SceSize>(size)) == static_cast<int>(size);
    }
    sceIoClose(fd);
    return ok;
}

// An SBT time, "HH:MM:SS:cc" or "HH:MM:SS.cc" -> ms (the last field read as hundredths in both forms: the
// FotLK file writes a decimal fraction there, the others :00 / :25 / :50 and up to :23).
bool parseTime(const char*& s, uint32_t& ms) {
    uint32_t f[4] = {0, 0, 0, 0};
    int n = 0;
    while (*s == ' ' || *s == '\t') ++s;
    while (n < 4) {
        if (*s < '0' || *s > '9') return false;
        uint32_t v = 0;
        while (*s >= '0' && *s <= '9') v = v * 10u + static_cast<uint32_t>(*s++ - '0');
        f[n++] = v;
        if (n < 4 && (*s == ':' || *s == '.')) ++s;
        else break;
    }
    if (n != 4) return false;
    ms = ((f[0] * 60u + f[1]) * 60u + f[2]) * 1000u + f[3] * 10u;
    return true;
}

}  // namespace

bool MovieFrame::introWanted() { return pp::configGetInt("movie", 1) != 0; }

void MovieFrame::introPlayed() {
    // The client writes SET movie "0" to Config.wtf after its first start.
    pp::configSet("movie", "0");
    int keys = 0;
    const bool ok = pp::configSave(&keys);
    PSP_LOGI(pp::tag::Fs, "movie_intro_cfg saved=%d keys=%d", ok ? 1 : 0, keys);
}

bool MovieFrame::exists(const char* name) {
    char path[256];
    moviePath(path, sizeof(path), name, "pmf");
    return pp::fsExists(path);
}

bool MovieFrame::glueAvailable(int version) {
    if (version < 1 || version > 3) return false;
    for (const char* name : kGlueList[version - 1])
        if (name && exists(name)) return true;
    return false;
}

void MovieFrame::loadSubtitles(const char* name) {
    lines_.clear();
    if (pp::configGetInt("moviesubtitle", 0) == 0) return;  // CVar movieSubtitle, off by default
    char path[256];
    moviePath(path, sizeof(path), name, "sbt");
    std::string text;
    if (!readSmall(path, text, 64u * 1024u)) {
        PSP_LOGI(pp::tag::Gfx, "movie_subtitles name=%s lines=0 (no %s)", name, path);
        return;
    }
    // Blocks of "start - end" then the text lines, a blank line between them (UTF-8, a BOM, CRLF).
    const char* s = text.c_str();
    if (std::strncmp(s, "\xEF\xBB\xBF", 3) == 0) s += 3;
    Line cur{0, 0, {}};
    bool inText = false;
    while (*s) {
        const char* e = s;
        while (*e && *e != '\n') ++e;
        std::string row(s, static_cast<size_t>(e - s));
        s = *e ? e + 1 : e;
        while (!row.empty() && (row.back() == '\r' || row.back() == ' ' || row.back() == '\t')) row.pop_back();
        size_t lead = 0;
        while (lead < row.size() && (row[lead] == ' ' || row[lead] == '\t')) ++lead;
        row.erase(0, lead);
        if (row.empty()) {
            if (inText && !cur.text.empty()) lines_.push_back(cur);
            inText = false;
            continue;
        }
        const char* p = row.c_str();
        uint32_t a = 0, b = 0;
        if (!inText && parseTime(p, a)) {
            while (*p == ' ' || *p == '\t' || *p == '-') ++p;
            if (parseTime(p, b) && b > a) {
                cur = Line{a, b, {}};
                inText = true;
            }
            continue;
        }
        if (inText) cur.text += (cur.text.empty() ? "" : "\n") + row;
    }
    if (inText && !cur.text.empty()) lines_.push_back(cur);
    std::sort(lines_.begin(), lines_.end(), [](const Line& x, const Line& y) { return x.startMs < y.startMs; });
    PSP_LOGI(pp::tag::Gfx, "movie_subtitles name=%s lines=%u", name, static_cast<unsigned>(lines_.size()));
}

// MovieFrameSubtitleString over the picture: the line shown at `ms` (a later one replaces the one fading out),
// MovieSubtitleFont (gold, outline + shadow) in the bottom band (the original's 800 x 138 units at TOP -630).
void MovieFrame::overlay(void* self, uint32_t ms) {
    auto* f = static_cast<MovieFrame*>(self);
    if (f->lines_.empty() || !f->ui_) return;
    const Line* cur = nullptr;
    float alpha = 0.0f;
    for (const Line& l : f->lines_) {
        if (ms < l.startMs) break;
        if (ms < l.endMs) {
            cur = &l;
            alpha = std::min(1.0f, static_cast<float>(ms - l.startMs) / static_cast<float>(kFadeInMs));
        } else if (ms < l.endMs + kFadeOutMs) {
            cur = &l;
            alpha = 1.0f - static_cast<float>(ms - l.endMs) / static_cast<float>(kFadeOutMs);
        }
    }
    if (!cur || alpha <= 0.02f) return;
    UiRenderer& ui = *f->ui_;
    const Font& fn = f->kit_.font(kFontNormal);
    const float lineH = static_cast<float>(fn.lineHeight());
    constexpr float kMaxW = 400.0f, kCx = pp::kScreenW * 0.5f, kBandMid = 246.0f;
    const float h = glueWrapped(ui, fn, kCx, 0.0f, kMaxW, cur->text, 0, lineH, false);
    const float y = std::max(150.0f, kBandMid - h * 0.5f);
    const uint32_t a = static_cast<uint32_t>(alpha * 255.0f) << 24;
    ui.beginFrame();
    static const float kOutline[5][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {1, 1}};
    for (const auto& o : kOutline) glueWrapped(ui, fn, kCx + o[0], y + o[1], kMaxW, cur->text, a, lineH);
    glueWrapped(ui, fn, kCx, y, kMaxW, cur->text, a | (kGlueGold & 0x00FFFFFFu), lineH);
    ui.endFrame();
}

bool MovieFrame::pumpThrough(void* self) {
    auto* f = static_cast<MovieFrame*>(self);
    return f->pump_ ? f->pump_(f->pumpUser_) : true;
}

pp::MovieEnd MovieFrame::play(UiRenderer& ui, const char* name, int volume, bool (*pump)(void*), void* user) {
    char path[256];
    moviePath(path, sizeof(path), name, "pmf");
    ui_ = &ui;
    pump_ = pump;
    pumpUser_ = user;
    loadSubtitles(name);
    pp::MovieHooks hooks;
    hooks.pump = &MovieFrame::pumpThrough;
    hooks.overlay = &MovieFrame::overlay;
    hooks.user = this;
    PSP_LOGI(pp::tag::Loop, "movie_frame win=0x%02X name=%s volume=%d", kWinMovieFrame, name, volume);
    const pp::MovieEnd end = pp::moviePlay(path, volume, hooks);
    lines_.clear();
    lines_.shrink_to_fit();
    pump_ = nullptr;
    return end;
}

// MovieFrame_OnShow -> MovieFrame_PlayMovie(self, 1): a nil video plays the next index (PlayNextMovie), the end of
// the list (or ESCAPE) hides the frame; a movie that cannot start is skipped like a nil one.
void MovieFrame::playGlue(UiRenderer& ui, int version) {
    if (version < 1 || version > 3) return;
    for (int index = 1; index <= 2; ++index) {
        const char* name = kGlueList[version - 1][index - 1];
        if (!name) continue;
        const pp::MovieEnd end = play(ui, name, kGlueVolume[version - 1], nullptr, nullptr);
        if (end == pp::MovieEnd::SkipAll || end == pp::MovieEnd::Exit) break;
    }
}

// MovieFrame_PlayMovie(self, name, volume): the Movie.dbc id resolved through data/movies/movies.txt.
pp::MovieEnd MovieFrame::playGame(UiRenderer& ui, uint32_t movieId, bool (*pump)(void*), void* user) {
    char path[256];
    pp::fsPath(path, sizeof(path), "data/movies/movies.txt");
    std::string index;
    char name[48] = {};
    if (readSmall(path, index, 16u * 1024u)) {
        const char* s = index.c_str();
        while (*s && !name[0]) {
            unsigned id = 0;
            char n[48] = {};
            int vol = 0;
            if (*s != '#' && std::sscanf(s, "%u %47s %d", &id, n, &vol) >= 2 && id == movieId)
                std::snprintf(name, sizeof(name), "%s", n);
            while (*s && *s != '\n') ++s;
            if (*s) ++s;
        }
    }
    if (!name[0]) {
        PSP_LOGW(pp::tag::Gfx, "movie_unknown id=%u (no row in %s)", static_cast<unsigned>(movieId), path);
        return pp::MovieEnd::Missing;
    }
    return play(ui, name, kGameVolume, pump, user);
}

// --- CinematicsFrame (AccountLogin.xml) -------------------------------------------------------------------------

void CinematicsFrame::show(FocusRing& focus) {
    shown_ = true;
    focus.setFocus(kIdCinematicsMovie1);
    PSP_LOGI(pp::tag::Loop, "cinematics_frame win=0x%02X shown", kWinCinematicsFrame);
}

// CinematicsBackground (272 x numMovies * 40 + 70 units, UI-DialogBox backdrop) at the centre, CINEMATICS in
// GlueFontNormalLarge at TOP -16, CinematicsButton1..3 (GlueButtonTemplateBlue 240 x 45, TOP -50, then 5 units
// overlapping); PSP: ~0.66 of the units (GlueDialog's scale), the buttons wider for their labels.
void CinematicsFrame::draw(UiRenderer& ui, const UiKit& kit, FocusRing& focus) const {
    if (!shown_) return;
    constexpr int kCount = kClientExpansionLevel;
    constexpr float kW = 200.0f, kBtnW = 170.0f, kBtnH = 28.0f, kStep = kBtnH - 3.0f;
    const float h = 34.0f + kCount * kStep + 14.0f;
    const Rect frame{(pp::kScreenW - kW) * 0.5f, (pp::kScreenH - h) * 0.5f, kW, h};
    kit.panel(ui, frame);
    const Font& fn = kit.font(kFontNormal);
    ui.textShadow(fn, frame.x + kW * 0.5f, frame.y + 11.0f, GLUE_STR(CINEMATICS), kGlueGold, Align::Center);
    static const char* const kLabels[3] = {GLUE_STR(WORLD_OF_WARCRAFT), GLUE_STR(BURNING_CRUSADE),
                                           GLUE_STR(WRATH_OF_THE_LICH_KING)};
    for (int i = 0; i < kCount; ++i) {
        const Rect b{frame.x + (kW - kBtnW) * 0.5f, frame.y + 32.0f + static_cast<float>(i) * kStep, kBtnW, kBtnH};
        glueButton(ui, kit, focus, kIdCinematicsMovie1 + static_cast<uint32_t>(i), b, kLabels[i], true, kFontNormal);
    }
}

int CinematicsFrame::pressed(uint32_t activated) const {
    if (!shown_ || activated < kIdCinematicsMovie1 ||
        activated >= kIdCinematicsMovie1 + static_cast<uint32_t>(kClientExpansionLevel))
        return 0;
    return static_cast<int>(activated - kIdCinematicsMovie1) + 1;
}

} // namespace wowee::ui::psp
