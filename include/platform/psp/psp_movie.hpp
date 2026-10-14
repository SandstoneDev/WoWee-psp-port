// Cinematics: the PSMF movie player - the client's cinematics (the intros and the in-game
// cinematics), converted on the PC by tools/psp/movie_conv.py into data/movies/<name>.pmf (AVC Main profile
// 480 x 272, 29.97 fps + ATRAC3plus 44.1 kHz stereo, Sony's PSMF Encoder / Composer).
//
// The firmware's decoder through the open pspsdk sceMpeg (libpspmpeg stubs, module PSP_MODULE_AV_MPEGBASE loaded
// for the movie and unloaded after it) - not Sony's libpsmfplayer. Own PSMF header read (stream offset / size,
// first and last timestamps), the sceMpeg ring buffer filled from one file descriptor through pakRead (the card
// lock the world streamer and the sound loader share), AVC decoded by the Media Engine straight into the frame
// buffer the GE draws next (RGB 565 for the movie: 64 levels of green instead of 5551's 32 - the dark cinematics
// band less), ATRAC3plus decoded into 2048-frame blocks that go into the music channel's PCM ring
// (psp_audio moviePcm*: the movie's sound is the clock - a frame shows when the sound reaches its PTS).
//
// The caller's world is frozen, not unloaded: the player blocks the main loop (its own loop of input, hooks,
// reads, decodes and frames) and its memory (~0.5 MB heap: the mpeg work area, a 192-pack ring of the stream, the
// ATRAC buffers) is freed before it returns; VRAM gets nothing new (the two colour buffers are reused).
// Skip: START (the original's ESCAPE / TOGGLEGAMEMENU) or Circle (SPACE / ENTER: StopMovie).
#pragma once

#include <cstdint>

namespace wowee::platform::psp {

enum class MovieEnd : uint8_t {
    Finished,  // played to its end
    SkipNext,  // Circle: MovieFrame_OnKeyUp SPACE / ENTER -> StopMovie (the glue list goes on with the next movie)
    SkipAll,   // START: MovieFrame_OnKeyUp ESCAPE -> MovieFrame:Hide()
    Missing,   // no such file (StartMovie fails)
    Failed,    // the decoder or the memory refused (logged: movie_failed step= r=)
    Exit,      // HOME -> exit during the movie
    Stopped,   // the pump hook said stop (the world connection dropped)
};
const char* movieEndName(MovieEnd e);

struct MovieHooks {
    // Every pass of the player's loop (~60 a second): the owner keeps its connection alive (WorldSession::update
    // + CMSG_KEEP_ALIVE) - a movie holds the main loop for minutes. false stops the movie (MovieEnd::Stopped).
    bool (*pump)(void* user) = nullptr;
    // Each shown frame, inside its display list (frameBegin .. frameEnd), over the movie picture (the colour buffer
    // is RGB 565 then): the subtitles. `ms` = the movie's time of the frame (its first frame = 0).
    void (*overlay)(void* user, uint32_t ms) = nullptr;
    void* user = nullptr;
};

// Plays `absPath` to its end or until skipped and returns how it ended. Blocks. `volume` 0..255 as the client's
// MovieFrame:StartMovie(name, volume) - the sound's gain is volume / 255.
MovieEnd moviePlay(const char* absPath, int volume, const MovieHooks& hooks);

} // namespace wowee::platform::psp
