// FrameXML on the PSP - data/framexml.pak (psp_framexml_conv) read
// once at world entry and closed, the runtime (fx_runtime.hpp) drawn on the
// GE UI renderer through the layer manager (ui_layers.hpp), input from the
// cursor mode (SELECT) and the focus ring.
//
// wowee.cfg:
//   framexml=0|1     off by default: the current HUD stays until hardware says yes
//   framexmlpak=     file under data/ (default framexml.pak)
//   fxscale=60       screen px per 100 UI units (UIParent = 480/0.6 x 272/0.6)
//   luamem=3072      KB, Lua allocator budget
//   luagcus=1000     us of GC steps per frame (x2 under memory pressure)
//   fxtexkb=3072     KB of FrameXML textures loaded at most
//   fxfallback=1     unknown API names answer a no-op (upstream WOWEE_LUA_API_FALLBACK)
//   luabench=0       Lua micro benchmark at world entry (lua_bench lines; off by default)
//   fxstops=40       clickable FrameXML frames on the focus ring (8..56)
// Log: framexml_load frames= regions= kb= lua_kb= ms=, framexml_missing path=,
// framexml_tex n= kb= skipped=, lua_mem kb= peak_kb= gc_ms= ..., lua_bench test= us=.
#pragma once

#include "platform/psp/psp_texture.hpp"
#include "ui/psp/fx_runtime.hpp"
#include "ui/psp/ui_focus.hpp"
#include "ui/psp/ui_kit.hpp"
#include "ui/psp/ui_layers.hpp"
#include "ui/psp/ui_renderer.hpp"

#include <cstdint>
#include <vector>

namespace wowee::ui::psp {

class FrameXmlUi {
public:
    // Focus ring ids (batch 3 range of task A: 0x6600-0x66FF).
    static constexpr uint32_t kFocusBase = 0x6600;
    static constexpr uint32_t kMaxStops = 24;

    explicit FrameXmlUi(const UiKit& kit);
    ~FrameXmlUi();
    FrameXmlUi(const FrameXmlUi&) = delete;
    FrameXmlUi& operator=(const FrameXmlUi&) = delete;

    // Reads data/<framexmlpak>, runs the load program. false: missing or
    // broken (logged), the caller keeps its own HUD.
    bool load(fx::FxGameApi* api);
    bool loaded() const { return fx_.loaded(); }

    // Per frame: events and OnUpdate (zone ui.lua), GC (after drawing).
    void update(float dtSec);
    void stepGc();
    // Layout + draw list (zone ui.frames); then the layers call drawStrata.
    void buildDrawList();
    void registerLayers(UiLayers& layers);
    void drawStrata(UiRenderer& ui, uint8_t strata);

    // Cursor mode: pointer in screen px; the focus ring gets the clickable
    // frames as stops; a stop activated with Cross clicks its frame.
    void pointer(float x, float y);
    void pointerButton(bool down);
    void declareStops(FocusRing& ring);
    bool activate(uint32_t focusId, bool right = false);  // Right = the right button
    bool hit(float x, float y) const;

    fx::FxUi& fx() { return fx_; }
    // Game textures (spell icons from spells.pak) the adapter
    // resolves at draw time: nullptr = not available this frame.
    using DynTexFn = const platform::psp::Texture* (*)(void* ctx, uint8_t kind, uint32_t id);
    bool runLua(const char* code, const char* name);  // FPU quiet, zone ui.lua
    void setDynTextures(DynTexFn fn, void* ctx) {
        dynFn_ = fn;
        dynCtx_ = ctx;
    }
    const Font& fontFor(int fontObject, float heightUnits) const;
    // lua_bench lines (fresh state, no pak needed).
    static void runBench(fx::FxGameApi* api);

private:
    static bool readChunk(void* ctx, uint32_t index, std::vector<uint8_t>& out);
    static void drawCb(void* ctx, UiRenderer& ui, uint8_t strata);
    static bool hitCb(void* ctx, float x, float y, uint8_t strata);

    const UiKit& kit_;
    fx::FxUi fx_;
    std::vector<platform::psp::Texture> tex_;
    std::vector<std::vector<uint8_t>> chunks_;  // freed after the load program
    std::vector<fx::FxUi::Clickable> clickables_;
    std::vector<uint32_t> stopObj_;             // focus id - kFocusBase -> widget
    float scale_ = 0.6f;
    uint32_t nextLogMs_ = 0;
    uint32_t texBytes_ = 0;
    float gcMsSum_ = 0, luaMsSum_ = 0, framesMsSum_ = 0;
    uint32_t frames_ = 0;
    // Game textures, cooldown sweeps, focus stops, frame times.
    DynTexFn dynFn_ = nullptr;
    void* dynCtx_ = nullptr;
    int32_t questionMark_ = -2;  // baked INV_Misc_QuestionMark (-1 none, -2 not looked up)
    uint32_t maxStops_ = 40;
    std::vector<fx::FxFrameTime> frameTimes_;
    void drawCooldown(UiRenderer& ui, const Rect& r, float left, uint32_t color);
    const platform::psp::Texture* textureOf(int32_t index);
};

}  // namespace wowee::ui::psp
