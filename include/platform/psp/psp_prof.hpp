// In-game profiler for real PSP hardware (docs/PSP_PROFILER.md).
//
// Design from the Sony SDK docs and our earlier ports (GTASA_PSP, source-psp,
// psptrace, SPP_EXP):
//  - CPU zones: PROF_SCOPE("name") - RAII, 1 us sceKernelGetSystemTimeLow,
//    inclusive + self time accumulated per frame, main thread only.
//  - GE passes: PROF_GE_PASS("name") emits sceGuSignal(CONTINUE, id); the
//    SIGNAL/FINISH callbacks (interrupt context) only store a timestamp. A
//    pass's GE time = next signal - its signal (parser position; the parser
//    runs just ahead of drawing, GU_DIRECT feeds it while the CPU builds).
//  - Frame split: CPU build | GE tail (sceGuSync wait) | vblank wait, plus CPU
//    load from the kernel idle clock (sceKernelReferSystemStatus).
//  - Counters: draws, triangles, vertices, texture binds per frame.
//  - Output: overlay (SELECT), a 2-second summary in session.log, and a
//    capture of N frames of events (L+R+SELECT) written once, after the
//    capture, to ms0:/PSP/GAME/WOWEEPSP/prof/profNNN.bin; tools/psp/
//    prof2chrome.py turns it into Chrome/Perfetto JSON + a self-time table.
// Never writes to the Memory Stick during a frame; PPSSPP numbers are
// yes/no only (its GE timing is not real).
#pragma once

#include <cstdint>

namespace wowee::platform::psp::prof {

enum Counter : uint8_t { kDraws, kTriangles, kVertices, kTexBinds, kCounterCount };

constexpr int kMaxZones = 128;  // was 48, then 80; 128 since 82 zones were seen in use
constexpr int kMaxGePasses = 32;  // was 16, then 24; 22 passes seen: 32

// Interns a zone / GE pass name (pointer to a string literal) once.
uint16_t zoneId(const char* name);
uint16_t gePassId(const char* name);

void zoneEnter(uint16_t id);
void zoneLeave(uint16_t id);
void geMark(uint16_t passId);  // emits the GE signal
void count(Counter c, uint32_t n);

struct Scope {
    explicit Scope(uint16_t id) : id_(id) { zoneEnter(id); }
    ~Scope() { zoneLeave(id_); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    uint16_t id_;
};

// Frame hooks (psp_display / main loop).
void init();
void frameBegin();      // right after sceGuStart
void frameListBuilt();  // right before sceGuFinish
void frameGeDone();     // after sceGuSync
void frameEnd();        // after vblank + swap

// Controls.
void toggleOverlay();
bool overlayOn();
void requestCapture(int frames);
bool capturing();

// Averages over the last summary window (for the overlay and the log).
struct Summary {
    float fps = 0;
    float frameMs = 0, buildMs = 0, geTailMs = 0, vblankMs = 0;
    float cpuLoad = 0;  // 0..1, from the kernel idle clock
    int passCount = 0;
    const char* passName[kMaxGePasses] = {};
    float passMs[kMaxGePasses] = {};
    float geBusyMs = 0;  // first signal -> FINISH
    int zoneCount = 0;
    const char* zoneName[kMaxZones] = {};
    float zoneSelfMs[kMaxZones] = {};
    float zoneInclMs[kMaxZones] = {};
    float zoneCalls[kMaxZones] = {};
    float counters[kCounterCount] = {};
    uint32_t heapUsedKb = 0, heapFreeBlockKb = 0;
    uint32_t captureFramesLeft = 0;
};
const Summary& summary();

} // namespace wowee::platform::psp::prof

#define WOWEE_PROF_CAT2(a, b) a##b
#define WOWEE_PROF_CAT(a, b) WOWEE_PROF_CAT2(a, b)
#define PROF_SCOPE(name)                                                                                   \
    static const uint16_t WOWEE_PROF_CAT(prof_zone_, __LINE__) = ::wowee::platform::psp::prof::zoneId(name); \
    ::wowee::platform::psp::prof::Scope WOWEE_PROF_CAT(prof_scope_, __LINE__)(WOWEE_PROF_CAT(prof_zone_, __LINE__))
#define PROF_GE_PASS(name)                                                                                      \
    do {                                                                                                        \
        static const uint16_t prof_pass_ = ::wowee::platform::psp::prof::gePassId(name);                        \
        ::wowee::platform::psp::prof::geMark(prof_pass_);                                                       \
    } while (0)
