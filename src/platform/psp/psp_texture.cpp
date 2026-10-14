#include "platform/psp/psp_texture.hpp"
#include "platform/psp/psp_prof.hpp"

#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"

#include <pspgu.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

#include <pspkernel.h>

namespace wowee::platform::psp {

namespace {

const Texture* g_bound = nullptr;
bool g_texEnabled = false;

bool validPsm(uint8_t psm) {
    switch (psm) {
        case GU_PSM_5551:
        case GU_PSM_4444:
        case GU_PSM_8888:
        case GU_PSM_T4:
        case GU_PSM_T8: return true;
        default: return false;
    }
}

uint32_t bitsPerPixel(uint8_t psm) {
    switch (psm) {
        case GU_PSM_T4: return 4;
        case GU_PSM_T8: return 8;
        case GU_PSM_8888: return 32;
        default: return 16;
    }
}

bool isPow2(uint32_t v) { return v && !(v & (v - 1)); }

template <typename T>
T rd(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

} // namespace

bool textureFromBlob(void* blob, uint32_t size, Texture& out, bool owns) {
    auto* b = static_cast<uint8_t*>(blob);
    if (!b || size < 32 || std::memcmp(b, "WTEX", 4) != 0) return false;
    Texture t;
    t.psm = b[4];
    t.swizzled = b[5];
    t.flags = b[6];
    t.w = rd<uint16_t>(b + 8);
    t.h = rd<uint16_t>(b + 10);
    t.uw = rd<uint16_t>(b + 12);
    t.uh = rd<uint16_t>(b + 14);
    t.clutCount = rd<uint16_t>(b + 16);
    std::memcpy(t.slice, b + 20, 4);
    uint32_t clutOff = rd<uint32_t>(b + 24);
    uint32_t pixOff = rd<uint32_t>(b + 28);
    uint32_t pixBytes = static_cast<uint32_t>(t.w) * t.h * bitsPerPixel(t.psm) / 8;

    const bool paletted = t.psm == GU_PSM_T4 || t.psm == GU_PSM_T8;
    if (!validPsm(t.psm) || !isPow2(t.w) || !isPow2(t.h) || t.w > 512 || t.h > 512 ||
        pixOff % 16 || pixOff + pixBytes > size || (paletted && (clutOff % 16 || t.clutCount == 0 ||
                                                                  clutOff + t.clutCount * 4u > size))) {
        PSP_LOGE(tag::Gfx, "wtex_bad psm=%u w=%u h=%u size=%u", t.psm, t.w, t.h, static_cast<unsigned>(size));
        return false;
    }
    t.pixels = b + pixOff;
    t.mip[0] = t.pixels;
    if (t.flags & kTexMipmaps) {
        uint32_t off = pixOff + pixBytes;
        const uint32_t extra = b[7] < 7 ? b[7] : 7;
        for (uint32_t l = 1; l <= extra; ++l) {
            const uint32_t w = t.w >> l, h = t.h >> l;
            const uint32_t bytes = w * h * bitsPerPixel(t.psm) / 8;
            off = (off + 15u) & ~15u;
            if (w * bitsPerPixel(t.psm) / 8 < 16 || h < 8 || off + bytes > size) break;
            t.mip[l] = b + off;
            t.levels = static_cast<uint8_t>(l + 1);
            off += bytes;
        }
    }
    t.clut = paletted ? reinterpret_cast<const uint32_t*>(b + clutOff) : nullptr;
    t.block = owns ? blob : nullptr;
    out = t;
    return true;
}

void textureRelease(Texture& t) {
    if (g_bound == &t) g_bound = nullptr;
    if (t.block) pakFree(t.block);
    t = Texture{};
}

void textureResetBinding() {
    g_bound = nullptr;
    g_texEnabled = false;
    sceGuDisable(GU_TEXTURE_2D);
}

void textureBind(const Texture* t) {
    if (!t) {
        if (g_texEnabled) {
            sceGuDisable(GU_TEXTURE_2D);
            g_texEnabled = false;
        }
        return;
    }
    if (!g_texEnabled) {
        sceGuEnable(GU_TEXTURE_2D);
        g_texEnabled = true;
    }
    if (g_bound == t) return;
    g_bound = t;
    prof::count(prof::kTexBinds, 1);
    if (t->clut) {
        sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
        sceGuClutLoad(t->clutCount / 8, t->clut);  // count in 8-entry blocks
    }
    sceGuTexMode(t->psm, t->levels - 1, 0, t->swizzled);
    for (int l = 0; l < t->levels; ++l) sceGuTexImage(l, t->w >> l, t->h >> l, t->w >> l, t->mip[l]);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    int filt = (t->flags & kTexLinear) ? GU_LINEAR : GU_NEAREST;
    if (t->levels > 1) {
        sceGuTexFilter(GU_LINEAR_MIPMAP_NEAREST, GU_LINEAR);
        sceGuTexLevelMode(GU_TEXTURE_AUTO, 0.0f);
    } else {
        sceGuTexFilter(filt, filt);
    }
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
}

uint32_t textureDropTopMip(Texture& t, uint32_t minBytes) {
    if (!t.block || t.levels < 2 || !t.mip[1]) return 0;
    const uint32_t bpp = bitsPerPixel(t.psm);
    if (static_cast<uint32_t>(t.w) * t.h * bpp / 8 < minBytes) return 0;
    const uint32_t clutBytes = t.clut ? t.clutCount * 4u : 0u;
    // New layout: 32-byte WTEX header, CLUT, then levels 1..n (16-aligned each).
    uint32_t size = 32 + ((clutBytes + 15u) & ~15u);
    for (int l = 1; l < t.levels; ++l) size += (((t.w >> l) * (t.h >> l) * bpp / 8) + 15u) & ~15u;
    auto* nb = static_cast<uint8_t*>(memalign(64, (size + 63u) & ~63u));
    if (!nb) return 0;
    std::memset(nb, 0, 32);
    std::memcpy(nb, "WTEX", 4);
    nb[4] = t.psm;
    nb[5] = t.swizzled;
    nb[6] = static_cast<uint8_t>(t.levels > 2 ? t.flags : (t.flags & ~kTexMipmaps));
    nb[7] = static_cast<uint8_t>(t.levels > 2 ? t.levels - 2 : 0);
    const uint16_t w = static_cast<uint16_t>(t.w >> 1), h = static_cast<uint16_t>(t.h >> 1);
    const uint16_t uw = static_cast<uint16_t>(std::max(1, t.uw >> 1)), uh = static_cast<uint16_t>(std::max(1, t.uh >> 1));
    std::memcpy(nb + 8, &w, 2);
    std::memcpy(nb + 10, &h, 2);
    std::memcpy(nb + 12, &uw, 2);
    std::memcpy(nb + 14, &uh, 2);
    std::memcpy(nb + 16, &t.clutCount, 2);
    std::memcpy(nb + 20, t.slice, 4);
    uint32_t off = 32;
    const uint32_t clutOff = t.clut ? off : 0u;
    if (t.clut) {
        std::memcpy(nb + off, t.clut, clutBytes);
        off += (clutBytes + 15u) & ~15u;
    }
    const uint32_t pixOff = off;
    std::memcpy(nb + 24, &clutOff, 4);
    std::memcpy(nb + 28, &pixOff, 4);
    for (int l = 1; l < t.levels; ++l) {
        const uint32_t bytes = (t.w >> l) * (t.h >> l) * bpp / 8;
        std::memcpy(nb + off, t.mip[l], bytes);
        off += (bytes + 15u) & ~15u;
    }
    sceKernelDcacheWritebackRange(nb, size);
    Texture nt;
    if (!textureFromBlob(nb, size, nt, true)) {
        std::free(nb);
        return 0;
    }
    nt.name = t.name;
    textureRelease(t);
    t = nt;
    return size;
}

uint32_t textureBytes(const Texture& t) {
    return static_cast<uint32_t>(t.w) * t.h * bitsPerPixel(t.psm) / 8 + t.clutCount * 4u;
}

} // namespace wowee::platform::psp
