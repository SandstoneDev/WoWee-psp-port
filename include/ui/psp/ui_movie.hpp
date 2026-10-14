// Cinematics: the original's movie frames over the PSMF player (platform/psp/psp_movie.hpp).
//
//   - GlueXML MovieFrame (MovieFrame.lua): MovieList[version][index] - version 1 the original game (the logo, then
//     its intro), 2 the first expansion (nil, its intro), 3 the second expansion (nil, its intro); a nil
//     video goes on to the next index, the end of the list hides the frame (SetGlueScreen("login")); volume
//     MovieVolumeList[version] 100 / 150 / 250. ESCAPE (START) hides, SPACE / ENTER (Circle) stop the movie (the
//     next one plays). The intro on the first start: the client's CVar "movie" ("Show movie on startup", 1 until
//     the intro played) shows the "movie" glue screen with the client's expansion level (3) before the login.
//   - FrameXML MovieFrame (PLAY_MOVIE <- SMSG_TRIGGER_MOVIE): the Movie.dbc id -> its file (data/movies/movies.txt,
//     baked by tools/psp/movie_conv.py from Movie.dbc / MovieVariation.dbc / FileData.dbc), volume 150 (the
//     FrameXML default: the event gives none), GameMovieFinished after it whatever the end.
//   - The subtitles (MovieFrameSubtitleString; the client's CVar movieSubtitle "Show movie subtitles", off by
//     default -> wowee.cfg moviesubtitle=0/1): Interface\Cinematics\<name>_1024.sbt of the locale MPQ, copied next
//     to the movie as data/movies/<name>.sbt; fade in MOVIE_CAPTION_FADE_TIME (1 s), out in half of it.
//   - GlueXML CinematicsFrame (AccountLogin.xml): CINEMATICS on the login screen -> a dialog with one button per
//     expansion (GetClientExpansionLevel() = 3), any other key closes it.
// PSP: a movie is a blocking call (its own loop; psp_movie.hpp) - the caller's screen is frozen meanwhile.
#pragma once

#include "platform/psp/psp_movie.hpp"
#include "ui/psp/ui_focus.hpp"
#include "ui/psp/ui_kit.hpp"
#include "ui/psp/ui_renderer.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace wowee::ui::psp {

// Cinematics ids: the frames 0xE0 MovieFrame, 0xE1 CinematicsFrame (the glue has no window stack - they
// name the frames in the log); the buttons 0x7700 AccountLoginCinematicsButton, 0x7701..0x7703 CinematicsButton1..3.
inline constexpr uint8_t kWinMovieFrame = 0xE0, kWinCinematicsFrame = 0xE1;
inline constexpr uint32_t kIdCinematicsButton = 0x7700;
inline constexpr uint32_t kIdCinematicsMovie1 = 0x7701;  // + version - 1
inline constexpr int kClientExpansionLevel = 3;          // GetClientExpansionLevel(): the second expansion (3.3.5a)

// CINEMATICS and the expansions' names are GlueStrings keys (GLUE_STR, strings.pak).

class MovieFrame {
public:
    explicit MovieFrame(const UiKit& kit) : kit_(kit) {}

    // GlueXML: plays MovieList[version] (1..3) and returns when the list ends or is hidden.
    void playGlue(UiRenderer& ui, int version);
    // FrameXML: plays the Movie.dbc id; `pump` (may be null) runs every pass of the player and keeps the world
    // session alive (false stops the movie). The caller sends GameMovieFinished after it.
    platform::psp::MovieEnd playGame(UiRenderer& ui, uint32_t movieId, bool (*pump)(void*), void* user);

    // The intro on the first start: wowee.cfg movie (default 1); introPlayed() writes movie=0.
    static bool introWanted();
    static void introPlayed();
    static bool exists(const char* name);      // data/movies/<name>.pmf
    static bool glueAvailable(int version);    // a movie of MovieList[version] is on the card

private:
    struct Line {
        uint32_t startMs, endMs;
        std::string text;
    };
    platform::psp::MovieEnd play(UiRenderer& ui, const char* name, int volume, bool (*pump)(void*), void* user);
    void loadSubtitles(const char* name);
    static void overlay(void* self, uint32_t ms);
    static bool pumpThrough(void* self);

    const UiKit& kit_;
    UiRenderer* ui_ = nullptr;
    std::vector<Line> lines_;
    bool (*pump_)(void*) = nullptr;
    void* pumpUser_ = nullptr;
};

// GlueXML CinematicsFrame: modal while shown (the login screen's widgets go inert).
class CinematicsFrame {
public:
    void show(FocusRing& focus);
    void hide() { shown_ = false; }
    bool shown() const { return shown_; }
    void draw(UiRenderer& ui, const UiKit& kit, FocusRing& focus) const;
    // The version (1..3) a pressed button plays, else 0.
    int pressed(uint32_t activated) const;

private:
    bool shown_ = false;
};

} // namespace wowee::ui::psp
