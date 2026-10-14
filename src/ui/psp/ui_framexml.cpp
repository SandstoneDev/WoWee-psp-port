// FrameXML on the PSP (include/ui/psp/ui_framexml.hpp).
#include "ui/psp/ui_framexml.hpp"

#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_locale.hpp"  // GetLocale()
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_prof.hpp"

#include <pspthreadman.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {
// PSP: the Allegrex FPU traps on NaN/inf compares when its exceptions are
// enabled (a silent death). Scripts divide
// by values that are 0 before the first update fields (health / max), so
// every call into Lua and the layout runs with the FCR31 enable bits cleared;
// the cause bits are cleared before the old enables come back (writing a set
// cause with its enable set raises at once). The draw list drops non-finite
// items (fx_runtime), so no NaN reaches the renderer.
struct FpuQuiet {
    uint32_t saved = 0;
    FpuQuiet() {
        asm volatile("cfc1 %0, $31" : "=r"(saved));
        const uint32_t v = saved & ~((0x1Fu << 7) | (0x3Fu << 12));
        asm volatile("ctc1 %0, $31" : : "r"(v));
    }
    ~FpuQuiet() {
        const uint32_t v = saved & ~(0x3Fu << 12);
        asm volatile("ctc1 %0, $31" : : "r"(v));
    }
};
}  // namespace

FrameXmlUi::FrameXmlUi(const UiKit& kit) : kit_(kit) {}

FrameXmlUi::~FrameXmlUi() {
    fx_.shutdown();
    for (auto& t : tex_) pp::textureRelease(t);
}

bool FrameXmlUi::readChunk(void* ctx, uint32_t index, std::vector<uint8_t>& out) {
    auto* self = static_cast<FrameXmlUi*>(ctx);
    if (index >= self->chunks_.size() || self->chunks_[index].empty()) return false;
    out = self->chunks_[index];
    return true;
}

bool FrameXmlUi::load(fx::FxGameApi* api) {
    FpuQuiet fpu;
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    const std::string file = pp::configGet("framexmlpak", "framexml.pak");
    char path[256];
    std::snprintf(path, sizeof(path), "data/%s", file.c_str());
    char abs[256];
    pp::fsPath(abs, sizeof(abs), path);
    // One open, everything read into RAM, closed at once (the card's
    // open-file limit - no new descriptor stays open).
    pp::Pak pak;
    if (!pak.open(abs)) {
        PSP_LOGW(pp::tag::Gfx, "framexml_missing path=%s", path);
        return false;
    }
    const pp::PakEntry* te = pak.find("fx_tree");
    void* treeBlob = te ? pak.load(te) : nullptr;
    if (!treeBlob) {
        PSP_LOGW(pp::tag::Gfx, "framexml_missing entry=fx_tree");
        return false;
    }
    std::vector<uint8_t> tree(static_cast<const uint8_t*>(treeBlob), static_cast<const uint8_t*>(treeBlob) + te->size);
    pp::pakFree(treeBlob);
    fx::FxHeader h{};
    std::memcpy(&h, tree.data(), std::min<size_t>(sizeof(h), tree.size()));
    // Lua chunks (bytecode) into RAM; freed once the load program has run.
    chunks_.assign(h.chunkCount, {});
    uint32_t luaBytes = 0;
    for (uint32_t c = 0; c < h.chunkCount && tree.size() >= sizeof(h); ++c) {
        char name[16];
        std::snprintf(name, sizeof(name), "l_%u", static_cast<unsigned>(c));
        const pp::PakEntry* e = pak.find(name);
        void* b = e ? pak.load(e) : nullptr;
        if (!b) continue;
        chunks_[c].assign(static_cast<const uint8_t*>(b), static_cast<const uint8_t*>(b) + e->size);
        luaBytes += e->size;
        pp::pakFree(b);
    }
    // Textures, up to the budget (the rest draw as nothing, logged).
    const uint32_t texBudget = static_cast<uint32_t>(std::clamp(pp::configGetInt("fxtexkb", 3072), 256, 16384)) * 1024u;
    tex_.assign(h.textureCount, pp::Texture{});
    uint32_t loaded = 0, skipped = 0;
    for (uint32_t t = 0; t < h.textureCount; ++t) {
        char name[16];
        std::snprintf(name, sizeof(name), "x_%u", static_cast<unsigned>(t));
        const pp::PakEntry* e = pak.find(name);
        if (!e) continue;
        if (texBytes_ + e->size > texBudget) {
            ++skipped;
            continue;
        }
        void* b = pak.load(e);
        if (!b) continue;
        if (!pp::textureFromBlob(b, e->size, tex_[t], true)) {
            pp::pakFree(b);
            continue;
        }
        tex_[t].name = "fx";
        texBytes_ += e->size;
        ++loaded;
    }
    pak.close();
    PSP_LOGI(pp::tag::Gfx, "framexml_tex n=%u kb=%u skipped=%u lua_chunks=%u lua_kb=%u",
             static_cast<unsigned>(loaded), static_cast<unsigned>(texBytes_ / 1024), static_cast<unsigned>(skipped),
             static_cast<unsigned>(h.chunkCount), static_cast<unsigned>(luaBytes / 1024));

    fx::FxConfig cfg;
    scale_ = static_cast<float>(std::clamp(pp::configGetInt("fxscale", 60), 30, 150)) / 100.0f;
    cfg.uiScale = scale_;
    cfg.screenW = 480.0f;
    cfg.screenH = 272.0f;
    cfg.luaBudgetKb = static_cast<uint32_t>(std::clamp(pp::configGetInt("luamem", 3072), 512, 16384));
    cfg.gcBudgetMs = static_cast<float>(std::clamp(pp::configGetInt("luagcus", 1000), 100, 8000)) / 1000.0f;
    cfg.apiFallback = pp::configGetInt("fxfallback", 1) != 0;
    cfg.locale = pp::localeActive();  // GetLocale()
    maxStops_ = static_cast<uint32_t>(std::clamp(pp::configGetInt("fxstops", 40), 8, 56));
    const bool ok = fx_.load(std::move(tree), readChunk, this, api, cfg);
    if (ok) fx_.preloadHandlers();
    chunks_.clear();
    chunks_.shrink_to_fit();
    const fx::FxStats s = fx_.stats();
    PSP_LOGI(pp::tag::Gfx, "framexml_ready ok=%d ms=%u lua_kb=%u base_kb=%u native_kb=%u errors=%u missing_api=%u",
             ok ? 1 : 0, static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u),
             static_cast<unsigned>(s.luaKb), static_cast<unsigned>(s.luaBaseKb),
             static_cast<unsigned>(s.nativeBytes / 1024), static_cast<unsigned>(s.luaErrors),
             static_cast<unsigned>(s.missingApi));
    nextLogMs_ = pp::logUptimeMs() + 10000;
    return ok;
}

void FrameXmlUi::update(float dtSec) {
    if (!fx_.loaded()) return;
    PROF_SCOPE("ui.lua");
    FpuQuiet fpu;
    fx_.updateLua(dtSec);
}

void FrameXmlUi::stepGc() {
    if (!fx_.loaded()) return;
    PROF_SCOPE("ui.lua");
    FpuQuiet fpu;
    fx_.stepGc();
    // Summary every 10 s (no per-frame lines).
    const fx::FxStats s = fx_.stats();
    gcMsSum_ += s.gcMsLast;
    luaMsSum_ += s.luaMsLast;
    framesMsSum_ += s.layoutMsLast;
    ++frames_;
    const uint32_t now = pp::logUptimeMs();
    if (now >= nextLogMs_ && frames_) {
        const float n = static_cast<float>(frames_);
        PSP_LOGI(pp::tag::Mem, "lua_mem kb=%u peak_kb=%u fails=%u lua_gc_ms=%.2f gc_cycles=%u lua_ms=%.2f frames_ms=%.2f",
                 static_cast<unsigned>(s.luaKb), static_cast<unsigned>(s.luaPeakKb), static_cast<unsigned>(s.luaFails),
                 static_cast<double>(gcMsSum_ / n), static_cast<unsigned>(s.gcCycles),
                 static_cast<double>(luaMsSum_ / n), static_cast<double>(framesMsSum_ / n));
        PSP_LOGI(pp::tag::Gfx, "fx_frame items=%u onupdate=%u events=%u calls=%u errors=%u missing_api=%u dyn_tex=%u",
                 static_cast<unsigned>(s.drawItems), static_cast<unsigned>(s.onUpdateFrames),
                 static_cast<unsigned>(s.eventsFired), static_cast<unsigned>(s.handlerCalls),
                 static_cast<unsigned>(s.luaErrors), static_cast<unsigned>(s.missingApi),
                 static_cast<unsigned>(s.dynTextures));
        // script time per top-level frame, us per rendered frame (top 6).
        fx_.takeFrameTimes(frameTimes_);
        for (size_t k = 0; k < frameTimes_.size() && k < 6; ++k) {
            const fx::FxFrameTime& t = frameTimes_[k];
            const char* nm = fx_.objAt(t.obj).name ? fx_.nameOf(t.obj) : "?";
            PSP_LOGI(pp::tag::Gfx, "fx_frame name=%.40s us=%.1f calls=%u", nm,
                     static_cast<double>(static_cast<float>(t.us) / n), static_cast<unsigned>(t.calls));
        }
        gcMsSum_ = luaMsSum_ = framesMsSum_ = 0;
        frames_ = 0;
        nextLogMs_ = now + 10000;
    }
}

void FrameXmlUi::buildDrawList() {
    if (!fx_.loaded()) return;
    PROF_SCOPE("ui.frames");
    FpuQuiet fpu;
    fx_.buildDrawList();
    fx_.clickables(clickables_, maxStops_);  // fxstops
}

void FrameXmlUi::registerLayers(UiLayers& layers) {
    // One client per strata FrameXML uses (the draw list is sorted by strata),
    // so other clients (windows, cursor) interleave by strata.
    for (uint8_t s = kUiBackground; s <= kUiTooltip; ++s) {
        UiLayers::Client c;
        c.name = "framexml";
        c.strata = s;
        c.level = 0;
        c.ctx = this;
        c.draw = drawCb;
        c.hit = hitCb;
        layers.add(c);
    }
}

void FrameXmlUi::drawCb(void* ctx, UiRenderer& ui, uint8_t strata) {
    static_cast<FrameXmlUi*>(ctx)->drawStrata(ui, strata);
}

bool FrameXmlUi::hitCb(void* ctx, float x, float y, uint8_t strata) {
    const auto* self = static_cast<const FrameXmlUi*>(ctx);
    for (const auto& c : self->clickables_) {
        if (self->fx_.objAt(c.obj).strata != strata) continue;
        if (x >= c.x && y >= c.y && x < c.x + c.w && y < c.y + c.h) return true;
    }
    return false;
}

bool FrameXmlUi::hit(float x, float y) const {
    for (const auto& c : clickables_)
        if (x >= c.x && y >= c.y && x < c.x + c.w && y < c.y + c.h) return true;
    return false;
}

const Font& FrameXmlUi::fontFor(int fontObject, float heightUnits) const {
    // PSP: the baked fonts are FRIZQT 10/13 px and MORPHEUS 18 px (ui.pak);
    // a font object maps to the nearest by its on-screen height.
    const float px = heightUnits * scale_;
    const fx::FxFont* f = fx_.font(fontObject);
    if (f && std::strstr(fx_.string(f->file), "MORPHEUS") && px >= 12.0f) return kit_.font(kFontTitle);
    return px < 11.5f ? kit_.font(kFontSmall) : kit_.font(kFontNormal);
}

void FrameXmlUi::drawStrata(UiRenderer& ui, uint8_t strata) {
    const auto& list = fx_.drawList();
    if (list.empty()) return;
    const auto& clips = fx_.clips();
    int clipOn = -1;
    for (const auto& d : list) {
        if (d.strata != strata) continue;
        if (d.clip != clipOn) {
            if (clipOn >= 0) ui.popClip();
            clipOn = d.clip;
            if (clipOn >= 0) {
                const auto& c = clips[static_cast<size_t>(clipOn)];
                ui.pushClip(Rect{c.x0, c.y0, c.x1 - c.x0, c.y1 - c.y0});
            }
        }
        const Rect r{d.x, d.y, d.w, d.h};
        switch (d.kind) {
            case fx::FxDrawItem::Fill:
                ui.fill(r, d.color);
                break;
            case fx::FxDrawItem::Image: {
                const pp::Texture* tp = textureOf(d.texture);  // + game icons
                if (!tp) {
                    uint8_t kind = 0;
                    uint32_t id = 0;
                    if (fx_.dynTexInfo(d.texture, kind, id)) ui.fill(r, (d.color & 0xFF000000u) | 0x00303030u);
                    break;
                }
                const pp::Texture& t = *tp;
                const float uw = t.uw, uh = t.uh;
                ui.image(t, r, SrcRect{d.u0 * uw, d.v0 * uh, (d.u1 - d.u0) * uw, (d.v1 - d.v0) * uh}, d.color,
                         d.add ? Blend::Add : Blend::Alpha);
                break;
            }
            case fx::FxDrawItem::Tile:
                if (d.texture >= 0 && static_cast<size_t>(d.texture) < tex_.size() && tex_[d.texture].pixels)
                    ui.tile(tex_[d.texture], r, d.color);
                break;
            case fx::FxDrawItem::NineSlice: {
                if (d.texture < 0 || static_cast<size_t>(d.texture) >= tex_.size() || !tex_[d.texture].pixels) break;
                const pp::Texture& t = tex_[d.texture];
                const float cell = t.slice[0] > 0 ? static_cast<float>(t.slice[0]) : 1.0f;
                ui.nineSlice(t, r, d.edge / cell, d.color);
                break;
            }
            case fx::FxDrawItem::Text: {
                if (!d.text) break;
                const Font& f = fontFor(d.font, d.edge);
                const float y = d.y + (d.h - static_cast<float>(f.lineHeight())) * 0.5f;
                const Align a = d.justify == 0 ? Align::Left : (d.justify == 2 ? Align::Right : Align::Center);
                const float x = d.justify == 0 ? d.x : (d.justify == 2 ? d.x + d.w : d.x + d.w * 0.5f);
                const fx::FxFont* ff = fx_.font(d.font);
                if (ff && (ff->shadowX || ff->shadowY)) ui.textShadow(f, x, y, d.text, d.color, a);
                else ui.text(f, x, y, d.text, d.color, a);
                break;
            }
            case fx::FxDrawItem::Cooldown:
                drawCooldown(ui, r, d.edge, d.color);
                break;
            default: break;
        }
    }
    if (clipOn >= 0) ui.popClip();
}

// a baked texture, or a game one through the adapter (spell icon);
// an item has no icon on the card yet - the baked question mark stands in.
const pp::Texture* FrameXmlUi::textureOf(int32_t index) {
    if (index >= 0 && static_cast<size_t>(index) < tex_.size()) return tex_[index].pixels ? &tex_[index] : nullptr;
    uint8_t kind = 0;
    uint32_t id = 0;
    if (!fx_.dynTexInfo(index, kind, id)) return nullptr;
    if (dynFn_)
        if (const pp::Texture* t = dynFn_(dynCtx_, kind, id)) return t;
    if (kind != fx::kFxDynItem) return nullptr;
    if (questionMark_ == -2) questionMark_ = fx_.textureByPath("Interface\\Icons\\INV_Misc_QuestionMark");
    return questionMark_ >= 0 && static_cast<size_t>(questionMark_) < tex_.size() && tex_[questionMark_].pixels
               ? &tex_[questionMark_]
               : nullptr;
}

// The Cooldown model's sweep (same drawing as the built-in bar,
// WorldScreen::drawCooldown): dark, clockwise from 12 o'clock over the part
// still to run, as horizontal spans; the angle table per size is computed once.
void FrameXmlUi::drawCooldown(UiRenderer& ui, const Rect& r, float left, uint32_t color) {
    const float frac = std::clamp(left, 0.0f, 1.0f);
    if (frac <= 0.0f) return;
    if (frac >= 0.999f) {
        ui.fill(r, color);
        return;
    }
    const int w = std::clamp(static_cast<int>(r.w), 1, 64), h = std::clamp(static_cast<int>(r.h), 1, 64);
    static std::unordered_map<int, std::vector<float>> tables;
    std::vector<float>& ang = tables[w * 256 + h];
    if (ang.empty()) {
        ang.resize(static_cast<size_t>(w * h));
        for (int i = 0; i < h; ++i)
            for (int j = 0; j < w; ++j) {
                float a = std::atan2((j + 0.5f) - w * 0.5f, h * 0.5f - (i + 0.5f));
                if (a < 0.0f) a += 6.2831853f;
                ang[static_cast<size_t>(i * w + j)] = a;
            }
    }
    const float end = 6.2831853f * (1.0f - frac);
    for (int i = 0; i < h; ++i) {
        const float* row = &ang[static_cast<size_t>(i * w)];
        int runStart = -1;
        for (int j = 0; j <= w; ++j) {
            const bool dark = j < w && row[j] >= end;
            if (dark && runStart < 0) runStart = j;
            if (!dark && runStart >= 0) {
                ui.fill(Rect{r.x + static_cast<float>(runStart), r.y + static_cast<float>(i),
                             static_cast<float>(j - runStart), 1.0f},
                        color);
                runStart = -1;
            }
        }
    }
}

void FrameXmlUi::pointer(float x, float y) {
    FpuQuiet fpu;
    if (fx_.loaded()) fx_.pointerMove(x, y);
}

void FrameXmlUi::pointerButton(bool down) {
    FpuQuiet fpu;
    if (fx_.loaded()) fx_.pointerButton(down, false);
}

void FrameXmlUi::declareStops(FocusRing& ring) {
    stopObj_.clear();
    for (const auto& c : clickables_) {
        if (stopObj_.size() >= maxStops_) break;
        const uint32_t id = kFocusBase + static_cast<uint32_t>(stopObj_.size());
        stopObj_.push_back(c.obj);
        ring.addStop(id, Rect{c.x, c.y, c.w, c.h});
    }
}

bool FrameXmlUi::activate(uint32_t focusId, bool right) {
    if (focusId < kFocusBase || focusId >= kFocusBase + stopObj_.size()) return false;
    const uint32_t obj = stopObj_[focusId - kFocusBase];
    PROF_SCOPE("ui.lua");
    FpuQuiet fpu;
    fx_.clickObject(obj, right);
    return true;
}

// a Lua call from the world (the Menu button opening a FrameXML panel).
bool FrameXmlUi::runLua(const char* code, const char* name) {
    if (!fx_.loaded()) return false;
    PROF_SCOPE("ui.lua");
    FpuQuiet fpu;
    return fx_.runString(code, name);
}

namespace {
struct BenchApi : fx::FxGameApi {
    fx::FxGameApi* inner;
    explicit BenchApi(fx::FxGameApi* a) : inner(a) {}
    bool unit(const char* t, fx::FxUnit& o) override { return inner->unit(t, o); }
    double timeSec() override { return inner->timeSec(); }
    uint64_t nowUs() override { return sceKernelGetSystemTimeWide(); }
    void log(const char* l) override { inner->log(l); }
    float textWidth(int f, float h, const char* s) override { return inner->textWidth(f, h, s); }
};
}  // namespace

void FrameXmlUi::runBench(fx::FxGameApi* api) {
    FpuQuiet fpu;
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    BenchApi b(api);
    const auto res = fx::FxUi::bench(&b, 2000);
    for (const auto& r : res)
        PSP_LOGI(pp::tag::Sys, "lua_bench test=%s us=%.3f n=%u", r.name, static_cast<double>(r.usPerOp),
                 static_cast<unsigned>(r.ops));
    PSP_LOGI(pp::tag::Sys, "lua_bench total_ms=%u lua_number=%u",
             static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u),
             static_cast<unsigned>(fx::FxUi::luaNumberSize()));
}

}  // namespace wowee::ui::psp
