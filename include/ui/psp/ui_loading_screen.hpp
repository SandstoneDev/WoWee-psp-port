// The original's loading screen (the login / realms / characters audit, #6) - the map's art from
// LoadingScreens.dbc (Map.dbc LoadingScreenID; the "...Wide" art where the client has one, the 4:3
// art between black bars like the 3.3.5a client on a wide screen) and the LoadingBar
// (Interface\Glues\LoadingBar: border, fill, spark, glass), filled from the world's load stages.
//
// PSP memory: the art (512 x 256 T8, ~129 KB) and the bar (~10 KB) come from data/glue.pak
// (tools/psp/build_glue_pak.py) when the loading starts and are freed when it ends - nothing of it
// stays while the world runs. Without glue.pak the screen is black with a plain bar.
//
// The world loads synchronously (WorldScreen's constructor, ~4.6 s on hardware): it reports its
// stages through loadingStep(); the owner of the loading screen installs a hook that draws a whole
// frame there (LoginScreen), so the bar moves while the frame used to freeze.
#pragma once

#include "platform/psp/psp_texture.hpp"
#include "ui/psp/ui_kit.hpp"
#include "ui/psp/ui_renderer.hpp"

#include <cstdint>

namespace wowee::ui::psp {

class LoadingScreen {
public:
    LoadingScreen() = default;
    LoadingScreen(const LoadingScreen&) = delete;
    LoadingScreen& operator=(const LoadingScreen&) = delete;
    ~LoadingScreen() { end(); }

    // Shows the loading screen of `mapId` (art loaded now). Again with another map: the art is swapped.
    void begin(uint32_t mapId);
    // Frees everything.
    void end();
    bool active() const { return active_; }
    uint32_t mapId() const { return mapId_; }
    void setProgress(float frac) { progress_ = frac < 0.0f ? 0.0f : (frac > 1.0f ? 1.0f : frac); }
    float progress() const { return progress_; }
    // A whole screen (between ui.beginFrame / endFrame of the caller).
    void draw(UiRenderer& ui) const;

private:
    void releaseArt();
    platform::psp::Texture art_, border_, fill_, spark_, glass_;
    bool wide_ = false;
    bool active_ = false;
    uint32_t mapId_ = ~0u;
    float progress_ = 0.0f;
};

// Progress of a long synchronous load (0..1). No-op without a hook.
using LoadingHook = void (*)(void* user, float frac);
void loadingSetHook(LoadingHook fn, void* user);
void loadingStep(float frac);

} // namespace wowee::ui::psp
