#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_osk.hpp"
#include "platform/psp/psp_prof.hpp"

#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspthreadman.h>
#include <psputils.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace wowee::platform::psp {

namespace {

constexpr int kBufWidth = 512;
constexpr uint32_t kFbBytes = kBufWidth * kScreenH * 2;  // 16-bit colour
constexpr uint32_t kZbBytes = kBufWidth * kScreenH * 2;  // 16-bit depth

// VRAM offsets relative to the start of VRAM, as sceGu expects.
constexpr uint32_t kFb0 = 0;
constexpr uint32_t kFb1 = kFb0 + kFbBytes;
constexpr uint32_t kZb = kFb1 + kFbBytes;

// Uncached VRAM mirror for CPU writes.
constexpr uintptr_t kVramUncached = 0x44000000u;

alignas(64) unsigned int g_list[128 * 1024];  // 512 KB display list (was 256 KB)
// An overflow showed on hardware as a yellow flash top left and a hang opening the spell book in a busy village; the list's
// use at frameEnd (sceGuCheckList, bytes) - its peak per log interval; past the end it overwrites what follows
// g_list and the GE runs garbage on the hardware (PPSSPP shrugs it off).
uint32_t g_listPeak = 0, g_listPeakAll = 0, g_listNextLogMs = 0;
uint32_t g_listFullM2 = 0, g_listFullNpc = 0, g_listFullUi = 0;  // frames a pass stopped short

void* g_drawBuf = reinterpret_cast<void*>(kFb0);
bool g_inited = false;
uint32_t g_geWaitUs = 0;
int g_frameInterval = 1;          // vblanks per flip (displaySetFrameInterval)
unsigned g_lastFlipVcount = 0;

struct Vert2D {
    uint32_t color;
    int16_t x, y, z;
    int16_t pad;
};

} // namespace

bool displayInit() {
    sceGuInit();
    sceGuStart(GU_DIRECT, g_list);
    sceGuDrawBuffer(GU_PSM_5551, reinterpret_cast<void*>(kFb0), kBufWidth);
    sceGuDispBuffer(kScreenW, kScreenH, reinterpret_cast<void*>(kFb1), kBufWidth);
    sceGuDepthBuffer(reinterpret_cast<void*>(kZb), kBufWidth);
    sceGuOffset(2048 - kScreenW / 2, 2048 - kScreenH / 2);
    sceGuViewport(2048, 2048, kScreenW, kScreenH);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, kScreenW, kScreenH);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDepthFunc(GU_GEQUAL);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuFrontFace(GU_CW);
    sceGuShadeModel(GU_SMOOTH);
    sceGuDisable(GU_CULL_FACE);
    sceGuEnable(GU_CLIP_PLANES);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);

    g_drawBuf = reinterpret_cast<void*>(kFb0);

    // Once only: InitEx clears the whole target buffer on every call.
    pspDebugScreenInitEx(reinterpret_cast<void*>(kVramUncached + kFb1),
                         PSP_DISPLAY_PIXEL_FORMAT_5551, 0);
    pspDebugScreenEnableBackColor(0);
    g_inited = true;
    return true;
}

void displayShutdown() {
    if (!g_inited) return;
    sceGuDisplay(GU_FALSE);
    sceGuTerm();
    g_inited = false;
}

// frameSkipNextClear / frameCleared.
namespace {
bool g_skipNextClear = false;
bool g_clearedThisFrame = true;
}  // namespace

void frameSkipNextClear() { g_skipNextClear = true; }
bool frameCleared() { return g_clearedThisFrame; }

void frameBegin(uint32_t clearABGR) {
    sceGuStart(GU_DIRECT, g_list);
    prof::frameBegin();
    sceGuClearColor(clearABGR);
    sceGuClearDepth(0);
    // one-shot - the world's sky lays colour and depth itself.
    g_clearedThisFrame = !g_skipNextClear;
    g_skipNextClear = false;
    if (g_clearedThisFrame) sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
}

void drawRect(int x, int y, int w, int h, uint32_t abgr) {
    auto* v = static_cast<Vert2D*>(sceGuGetMemory(2 * sizeof(Vert2D)));
    v[0] = {abgr, static_cast<int16_t>(x), static_cast<int16_t>(y), 0, 0};
    v[1] = {abgr, static_cast<int16_t>(x + w), static_cast<int16_t>(y + h), 0, 0};
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, nullptr, v);
    sceGuEnable(GU_DEPTH_TEST);
}

uint32_t displayListFree() {
    const int used = sceGuCheckList();
    return used >= 0 && static_cast<uint32_t>(used) < sizeof(g_list) ? static_cast<uint32_t>(sizeof(g_list)) - used : 0u;
}

void displayListFull(const char* pass) {
    if (pass[0] == 'm') ++g_listFullM2;
    else if (pass[0] == 'n') ++g_listFullNpc;
    else ++g_listFullUi;
}

void frameEnd(OverlayFn overlay, void* user) {
    prof::frameListBuilt();
    {
        const int used = sceGuCheckList();
        if (used > 0 && static_cast<uint32_t>(used) > g_listPeak) g_listPeak = static_cast<uint32_t>(used);
        const uint32_t now = logUptimeMs();
        if (now >= g_listNextLogMs) {
            g_listNextLogMs = now + 10000u;
            if (g_listPeak > g_listPeakAll) g_listPeakAll = g_listPeak;
            PSP_LOGI(tag::Gfx, "ge_list peak_kb=%u max_kb=%u of %u full m2=%u npc=%u ui=%u",
                     static_cast<unsigned>(g_listPeak / 1024u), static_cast<unsigned>(g_listPeakAll / 1024u),
                     static_cast<unsigned>(sizeof(g_list) / 1024u), static_cast<unsigned>(g_listFullM2),
                     static_cast<unsigned>(g_listFullNpc), static_cast<unsigned>(g_listFullUi));
            g_listPeak = 0;
            g_listFullM2 = g_listFullNpc = g_listFullUi = 0;
        }
    }
    sceGuFinish();
    uint64_t t0 = sceKernelGetSystemTimeWide();
    sceGuSync(0, 0);
    g_geWaitUs = static_cast<uint32_t>(sceKernelGetSystemTimeWide() - t0);
    prof::frameGeDone();
    // System dialogs draw here: after our list finished, before the swap.
    oskAfterGeSync();
    if (overlay) {
        pspDebugScreenSetBase(reinterpret_cast<u32*>(kVramUncached + reinterpret_cast<uintptr_t>(g_drawBuf)));
        overlay(user);
    }
    if (g_frameInterval <= 1) {
        sceDisplayWaitVblankStart();
    } else {
        // 30 fps cap: a frame that fits in one vblank is held for the next,
        // so the picture keeps an even pace instead of alternating 60/30.
        const unsigned target = g_lastFlipVcount + static_cast<unsigned>(g_frameInterval);
        do {
            sceDisplayWaitVblankStart();
        } while (static_cast<int>(sceDisplayGetVcount() - target) < 0);
    }
    g_lastFlipVcount = sceDisplayGetVcount();
    g_drawBuf = sceGuSwapBuffers();
    prof::frameEnd();
}

void displaySetFrameInterval(int vblanks) { g_frameInterval = vblanks < 1 ? 1 : (vblanks > 4 ? 4 : vblanks); }

int displayFrameInterval() { return g_frameInterval; }

void textAt(int col, int row, uint32_t abgr, const char* fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    pspDebugScreenSetXY(col, row);
    pspDebugScreenSetTextColor(abgr);
    pspDebugScreenPuts(buf);
}

uint32_t lastGeWaitUs() { return g_geWaitUs; }

namespace {
constexpr uint32_t kVramStaticBase = kZb + kZbBytes;  // after both frame buffers and depth
uint32_t g_vramTop = kVramStaticBase;
// freed ranges {offset, bytes}, 16-byte granular, merged, first-fit.
struct VramHole {
    uint32_t off, bytes;
};
constexpr int kMaxHoles = 64;
VramHole g_holes[kMaxHoles];
int g_holeCount = 0;
uint32_t g_vramFreed = 0;  // bytes in holes
uint32_t g_vramReservedTop = 0;  // lighting - kept off the top of eDRAM (glow buffer)
} // namespace

const void* vramStore(const void* data, uint32_t bytes) {
    // a freed range first (streaming evictions leave holes).
    const uint32_t need = (bytes + 15u) & ~15u;
    for (int i = 0; i < g_holeCount; ++i) {
        if (g_holes[i].bytes < need) continue;
        const uint32_t off = g_holes[i].off;
        g_holes[i].off += need;
        g_holes[i].bytes -= need;
        g_vramFreed -= need;
        if (g_holes[i].bytes == 0) g_holes[i] = g_holes[--g_holeCount];
        std::memcpy(reinterpret_cast<void*>(kVramUncached + off), data, bytes);
        return reinterpret_cast<const uint8_t*>(sceGeEdramGetAddr()) + off;
    }
    const uint32_t size = sceGeEdramGetSize() - g_vramReservedTop;  // lighting (glow buffer)
    const uint32_t off = (g_vramTop + 15u) & ~15u;
    if (off + bytes > size) return nullptr;
    std::memcpy(reinterpret_cast<void*>(kVramUncached + off), data, bytes);
    g_vramTop = off + bytes;
    return reinterpret_cast<const uint8_t*>(sceGeEdramGetAddr()) + off;
}

void vramReset() {
    g_vramTop = kVramStaticBase;
    g_holeCount = 0;
    g_vramFreed = 0;
}

void vramFree(const void* gePtr, uint32_t bytes) {
    if (!gePtr || !bytes) return;
    const uint32_t off = static_cast<uint32_t>(static_cast<const uint8_t*>(gePtr) -
                                               reinterpret_cast<const uint8_t*>(sceGeEdramGetAddr()));
    uint32_t len = (bytes + 15u) & ~15u;
    if (off < kVramStaticBase || off + len > g_vramTop + 15u) return;  // not ours
    uint32_t start = off;
    // Merge with touching holes.
    for (int i = 0; i < g_holeCount;) {
        if (g_holes[i].off + g_holes[i].bytes == start || start + len == g_holes[i].off) {
            const uint32_t s = std::min(start, g_holes[i].off);
            len += g_holes[i].bytes;
            g_vramFreed -= g_holes[i].bytes;
            start = s;
            g_holes[i] = g_holes[--g_holeCount];
            i = 0;
            continue;
        }
        ++i;
    }
    if (start + len >= g_vramTop) {  // the top block: give it back to the bump pointer
        g_vramTop = start;
        return;
    }
    if (g_holeCount < kMaxHoles) {
        g_holes[g_holeCount++] = VramHole{start, len};
        g_vramFreed += len;
    }  // else: leaked until the next vramReset (64 holes is far more than seen)
}

uint32_t vramUsedBytes() { return g_vramTop - kVramStaticBase - g_vramFreed; }

// lighting - the glow pass renders into its own small buffer at the
// top of eDRAM and samples the frame being drawn.
void* displayDrawBuffer() { return g_drawBuf; }

const uint16_t* displayShownPixels(int* stride) {
    // sceGuSwapBuffers handed back the next draw buffer: the other one is on screen.
    const uintptr_t shown = reinterpret_cast<uintptr_t>(g_drawBuf) == kFb0 ? kFb1 : kFb0;
    if (stride) *stride = static_cast<int>(kBufWidth);
    return reinterpret_cast<const uint16_t*>(kVramUncached + shown);
}

uint32_t vramReserveTop(uint32_t bytes) {
    const uint32_t size = sceGeEdramGetSize();
    const uint32_t need = (bytes + 15u) & ~15u;
    if (need <= g_vramReservedTop) return size - g_vramReservedTop;
    // Only while the bump allocator is below it (models reset and refill
    // later, under the new limit); 0 = not now.
    if (need > size - kVramStaticBase || g_vramTop > size - need) return 0;
    g_vramReservedTop = need;
    return size - need;
}

} // namespace wowee::platform::psp
