// The loading screen (include/ui/psp/ui_loading_screen.hpp).
#include "ui/psp/ui_loading_screen.hpp"

#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_system.hpp"  // logMemory

#include <cstdio>
#include <cstring>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

LoadingHook g_hook = nullptr;
void* g_hookUser = nullptr;

bool loadTex(pp::Pak& pak, const char* name, pp::Texture& t) {
    const pp::PakEntry* e = pak.find(name);
    void* blob = e ? pak.load(e) : nullptr;
    if (blob && !pp::textureFromBlob(blob, e->size, t, true)) {
        pp::pakFree(blob);
        return false;
    }
    return blob != nullptr;
}

// Loading-BarBorder is 512 x 64: the opening the fill shows through is x 36..476, y 22..42.
constexpr float kBarW = 320.0f, kBarH = 40.0f;  // PSP: 0.625 of the art
constexpr float kBarX = (pp::kScreenW - kBarW) * 0.5f, kBarY = pp::kScreenH - kBarH - 16.0f;
constexpr float kBarS = kBarW / 512.0f;
constexpr float kFillX = kBarX + 36.0f * kBarS, kFillW = 440.0f * kBarS;
constexpr float kFillY = kBarY + 22.0f * kBarS, kFillH = 20.0f * kBarS;

}  // namespace

void loadingSetHook(LoadingHook fn, void* user) {
    g_hook = fn;
    g_hookUser = user;
}

void loadingStep(float frac) {
    if (g_hook) g_hook(g_hookUser, frac);
}

void LoadingScreen::begin(uint32_t mapId) {
    if (active_ && mapId == mapId_) return;
    releaseArt();
    active_ = true;
    mapId_ = mapId;
    progress_ = 0.0f;
    wide_ = false;
    char path[256];
    pp::fsPath(path, sizeof(path), "data/glue.pak");
    pp::Pak pak;
    uint32_t screenId = 0;
    if (pak.open(path)) {
        if (const pp::PakEntry* e = pak.find("maps")) {
            if (void* blob = pak.load(e)) {
                const auto* b = static_cast<const uint8_t*>(blob);
                uint32_t n = 0;
                std::memcpy(&n, b, 4);
                for (uint32_t i = 0; i < n && 4u + 12u * (i + 1) <= e->size; ++i) {
                    uint32_t rec[3];
                    std::memcpy(rec, b + 4 + 12 * i, sizeof(rec));
                    if (rec[0] == mapId) {
                        screenId = rec[1];
                        wide_ = rec[2] != 0;
                        break;
                    }
                }
                pp::pakFree(blob);
            }
        }
        char name[32];
        std::snprintf(name, sizeof(name), "ls_%u", static_cast<unsigned>(screenId));
        if (screenId) loadTex(pak, name, art_);
        loadTex(pak, "bar_border", border_);
        loadTex(pak, "bar_fill", fill_);
        loadTex(pak, "bar_spark", spark_);
        loadTex(pak, "bar_glass", glass_);
        pak.close();
    }
    PSP_LOGI(pp::tag::Gfx, "loading_screen map=%u screen=%u wide=%d art=%d bar=%d kb=%u", static_cast<unsigned>(mapId),
             static_cast<unsigned>(screenId), wide_ ? 1 : 0, art_.pixels ? 1 : 0, border_.pixels ? 1 : 0,
             static_cast<unsigned>((pp::textureBytes(art_) + pp::textureBytes(border_) + pp::textureBytes(fill_) +
                                    pp::textureBytes(spark_) + pp::textureBytes(glass_)) / 1024u));
    pp::logMemory("loading_begin");
}

void LoadingScreen::releaseArt() {
    for (pp::Texture* t : {&art_, &border_, &fill_, &spark_, &glass_})
        if (t->pixels) pp::textureRelease(*t);
}

void LoadingScreen::end() {
    if (!active_) return;
    releaseArt();
    active_ = false;
    mapId_ = ~0u;
    PSP_LOGI(pp::tag::Gfx, "loading_screen end");
}

void LoadingScreen::draw(UiRenderer& ui) const {
    const Rect full{0, 0, static_cast<float>(pp::kScreenW), static_cast<float>(pp::kScreenH)};
    ui.fill(full, 0xFF000000u);
    if (art_.pixels) {
        if (wide_) {
            ui.image(art_, full);
        } else {
            // The 4:3 art at 4:3 between black bars (the 3.3.5a client on a wide screen).
            const float w = pp::kScreenH * 4.0f / 3.0f;
            ui.image(art_, Rect{(pp::kScreenW - w) * 0.5f, 0, w, static_cast<float>(pp::kScreenH)});
        }
    }
    const float fw = kFillW * progress_;
    if (fill_.pixels) {
        if (fw > 0.5f) ui.image(fill_, Rect{kFillX, kFillY, fw, kFillH});
    } else {
        ui.fill(Rect{kFillX, kFillY, kFillW, kFillH}, 0xC0201010u);
        if (fw > 0.5f) ui.fill(Rect{kFillX, kFillY, fw, kFillH}, 0xFF9C6F12u);
    }
    if (spark_.pixels && progress_ > 0.0f && progress_ < 1.0f)
        ui.image(spark_, Rect{kFillX + fw - 5.0f, kFillY - kFillH * 0.5f, 10.0f, kFillH * 2.0f}, 0xFFFFFFFFu,
                 Blend::Add);
    if (glass_.pixels) ui.image(glass_, Rect{kBarX, kBarY, kBarW, kBarH});
    if (border_.pixels) ui.image(border_, Rect{kBarX, kBarY, kBarW, kBarH});
}

} // namespace wowee::ui::psp
