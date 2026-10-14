// PSP display / GE bring-up (sceGu).
//
// VRAM layout (2 MB): two 16-bit colour buffers (480x272, stride 512, 5551 so
// the stencil bit exists later) + a 16-bit depth buffer = 816 KB; the rest is
// left for textures.
//
// Hardware rules this layer follows (see docs/PSP_PORT_ARCHITECTURE.md):
//  - The GE runs behind the CPU on hardware (PPSSPP hides it): vertex memory
//    handed to sceGuDrawArray is taken from the display list (sceGuGetMemory)
//    and never rewritten in the same frame.
//  - CPU writes into VRAM (debug text) only after sceGuSync.
//  - Vertex colour is 0xAABBGGRR (R in the low byte).
#pragma once

#include <cstdint>

namespace wowee::platform::psp {

constexpr int kScreenW = 480;
constexpr int kScreenH = 272;

bool displayInit();
void displayShutdown();

// Starts the frame's display list and clears colour + depth.
void frameBegin(uint32_t clearABGR);

// The next frameBegin() leaves the screen
// uncleared (one-shot; the frame after clears again unless asked anew). For a
// caller that covers every pixel, colour and depth 0, itself - the world's sky
// (wowee.cfg skyfast=1): the clear is a whole-screen pass for nothing there.
// frameCleared() says whether this frame's frameBegin did clear; a caller that
// asked and then cannot cover the screen must clear itself.
void frameSkipNextClear();
bool frameCleared();

// Finishes the list, waits for the GE, runs `overlay` (CPU text into the
// finished frame), waits for vblank and flips.
using OverlayFn = void (*)(void* user);
void frameEnd(OverlayFn overlay, void* user);

// A yellow flash and a hang on hardware (a busy village, the spell book open - the 256 KB display
// list ran past its end, 241 KB measured on the emulator there): the list is 512 KB, and the passes keep out of its
// end: the M2 pass stops below kListReserveM2 bytes free, the skinned NPCs below kListReserveNpc, the interface
// below kListReserveUi (dropped draws are counted: ge_list_full in the 10 s ge_list line).
constexpr uint32_t kListReserveM2 = 160 * 1024;
constexpr uint32_t kListReserveNpc = 96 * 1024;
constexpr uint32_t kListReserveUi = 8 * 1024;
uint32_t displayListFree();       // bytes left in this frame's display list
void displayListFull(const char* pass);  // a pass stopped short (counted per pass, logged every 10 s)

// 2D helpers (screen pixels, drawn with depth test off).
void drawRect(int x, int y, int w, int h, uint32_t abgr);

// Debug text (only valid inside an OverlayFn). Columns/rows of the 8x8 font.
// Colour is 0xAABBGGRR like the GE.
void textAt(int col, int row, uint32_t abgr, const char* fmt, ...)
    __attribute__((format(printf, 4, 5)));

// Microseconds the last frame spent waiting for the GE (sceGuSync).
uint32_t lastGeWaitUs();

// Frame limit: flip no sooner than `vblanks` vblanks after the previous flip
// (1 = 60 fps, as always; 2 = 30 fps). wowee.cfg framecap, settings menu.
void displaySetFrameInterval(int vblanks);
int displayFrameInterval();

// Static data for the GE in eDRAM after the frame/depth buffers (SPP_EXP:
// vertices read from VRAM are faster than from main RAM). Returns a pointer
// the GE can use, or nullptr when the ~1.2 MB are used up. The data is copied
// in (uncached); vramReset() forgets every allocation.
const void* vramStore(const void* data, uint32_t bytes);
void vramReset();
uint32_t vramUsedBytes();
// Tile streaming frees the eDRAM copy of an evicted model; the
// range is reused first-fit by later vramStore calls (vramReset forgets the
// free list too). `gePtr` / `bytes` exactly as vramStore took and returned.
// Only between frames (after sceGuSync), like every VRAM write here.
void vramFree(const void* gePtr, uint32_t bytes);

// Lighting (glow pass). The buffer being drawn (VRAM-relative, as
// sceGuDrawBufferList takes it) and a block kept at the top of eDRAM that
// vramStore never hands out: returns its VRAM-relative offset, or 0 when the
// models already sit there (call before they load).
void* displayDrawBuffer();
// The autopilot's "shot": the frame on screen now - 16-bit 5551 pixels (R in the low bits) through
// the uncached VRAM mirror, `stride` pixels a row. Between frames only (after frameEnd, before frameBegin).
const uint16_t* displayShownPixels(int* stride);
uint32_t vramReserveTop(uint32_t bytes);

} // namespace wowee::platform::psp
