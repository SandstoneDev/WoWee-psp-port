// GE textures loaded from "WTEX" blobs (tools/psp/build_ui_pak.py).
//
// Storage rules the converter guarantees (GE Users Manual p80-82,
// libgu-Reference p150-151): power-of-two size, swizzled with width ==
// buffer width, rows >= 16 bytes, pixels and CLUT 16-byte aligned. uw/uh is
// the useful picture inside the power-of-two storage.
//
// Blob layout (little-endian, 32-byte header):
//   0 "WTEX"  4 u8 psm, u8 swizzled, u8 flags, u8 mipLevels (extra levels
//             when flags has kTexMipmaps, else 0)
//   8 u16 w, u16 h   12 u16 uw, u16 uh   16 u16 clutCount, u16 reserved
//  20 u8 sliceL, sliceT, sliceR, sliceB  (9-slice margins, content texels)
//  24 u32 clutOffset  28 u32 pixelOffset (from blob start)
// Mip levels (models, M7): level n+1 is half of level n in both axes, same
// psm/CLUT/swizzle, stored right after level n at the next 16-byte boundary.
#pragma once

#include <cstdint>

namespace wowee::platform::psp {

enum TextureFlags : uint8_t {
    kTexHasAlpha = 1u << 0,   // needs blending
    kTexNineSlice = 1u << 1,  // slice margins are meaningful
    kTexLinear = 1u << 2,     // sample GU_LINEAR (stretched art); else GU_NEAREST
    kTexMipmaps = 1u << 3,    // byte 7 = extra mip levels after level 0
};

struct Texture {
    const void* pixels = nullptr;
    const uint32_t* clut = nullptr;  // ABGR8888 entries, or null
    uint16_t w = 0, h = 0;           // storage (power of two)
    uint16_t uw = 0, uh = 0;         // content
    uint16_t clutCount = 0;
    uint8_t psm = 0;                 // GU_PSM_*
    uint8_t swizzled = 0;
    uint8_t flags = 0;
    uint8_t slice[4] = {0, 0, 0, 0}; // L, T, R, B
    uint8_t levels = 1;              // mip levels including level 0 (GE: up to 8)
    const void* mip[8] = {};         // level pixels, mip[0] == pixels
    void* block = nullptr;           // owning allocation (pak blob)
    const char* name = "";
};

// Parses a WTEX blob in place (the texture points into `blob`). Takes
// ownership of blob when owns == true.
bool textureFromBlob(void* blob, uint32_t size, Texture& out, bool owns);
void textureRelease(Texture& t);

// Binds for drawing (skips the GE commands when already bound). nullptr
// disables texturing. PSP: a new binding also sends sceGuTexWrap(GU_CLAMP,
// GU_CLAMP) - set the batch's wrap AFTER every bind, never cache it across
// texture changes (the big-WMO streets drew clamped: one colour).
void textureBind(const Texture* t);
// Forget the bound texture (call at the start of every display list).
void textureResetBinding();

uint32_t textureBytes(const Texture& t);

// For a full heap (the world filled 43.8 MB on hardware): drops mip level 0 of
// an owned, mipmapped texture whose level 0 is at least minBytes - the next
// level becomes the base (half the size each way, 3/4 of the memory freed).
// The texture gets a new compact block; returns its size, or 0 (unchanged).
uint32_t textureDropTopMip(Texture& t, uint32_t minBytes);

} // namespace wowee::platform::psp
