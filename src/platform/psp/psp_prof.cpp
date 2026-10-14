#include "platform/psp/psp_prof.hpp"

#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_bigheap.hpp"
#include "platform/psp/psp_log.hpp"

#include <pspgu.h>
#include <pspiofilemgr.h>
#include <pspsysmem.h>
#include <pspthreadman.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

namespace wowee::platform::psp::prof {

namespace {

constexpr int kMaxDepth = 16;
constexpr int kWindowFrames = 60;       // summary window
constexpr int kMaxSignals = 64;         // GE signals per frame
constexpr uint32_t kCaptureEvents = 40000;  // 12 B each: 480 KB, allocated on request

inline uint32_t now() { return sceKernelGetSystemTimeLow(); }

// Capture event (12 bytes, little-endian, as written to the .bin).
struct Event {
    uint32_t t0, t1;
    uint16_t id;
    uint8_t depth;
    uint8_t kind;  // 0 zone, 1 GE pass, 2 frame, 3 phase (id 0 built, 1 GE done), 4 counter (t0 = value)
};
static_assert(sizeof(Event) == 12, "capture event is 12 bytes");

struct State {
    const char* zoneNames[kMaxZones] = {};
    int zoneCount = 0;
    const char* passNames[kMaxGePasses] = {};
    int passCount = 0;

    // Current frame.
    uint32_t incl[kMaxZones] = {}, self[kMaxZones] = {}, calls[kMaxZones] = {};
    uint16_t stackId[kMaxDepth] = {};
    uint32_t stackT[kMaxDepth] = {}, stackChild[kMaxDepth] = {};
    int depth = 0;
    uint32_t counters[kCounterCount] = {};
    uint32_t tPrevEnd = 0, tFrame = 0, tBuilt = 0, tGe = 0;  // frame = previous end -> this end (includes update)
    uint32_t idleUs = 0;
    uint64_t idleLast = 0;

    // Window sums.
    double wIncl[kMaxZones] = {}, wSelf[kMaxZones] = {}, wCalls[kMaxZones] = {};
    double wCounters[kCounterCount] = {};
    double wPass[kMaxGePasses] = {};
    double wFrame = 0, wBuild = 0, wTail = 0, wVbl = 0, wGeBusy = 0, wIdle = 0;
    int wFrames = 0;
    uint32_t lastLogMs = 0;

    bool overlay = false;
    Event* cap = nullptr;
    uint32_t capCount = 0;
    int capFramesLeft = 0;
    uint32_t capFrames = 0;
    uint32_t frameIndex = 0;

    Summary sum;
};

State g;

// GE callbacks (interrupt context): timestamps only.
volatile uint32_t g_sigT[kMaxSignals];
volatile uint16_t g_sigId[kMaxSignals];
volatile int g_sigN = 0;
volatile uint32_t g_finishT = 0;

void onSignal(int id) {
    const int n = g_sigN;
    if (n < kMaxSignals) {
        g_sigT[n] = now();
        g_sigId[n] = static_cast<uint16_t>(id);
        g_sigN = n + 1;
    }
}

void onFinish(int) { g_finishT = now(); }

inline void capture(uint32_t t0, uint32_t t1, uint16_t id, uint8_t depth, uint8_t kind) {
    if (g.cap && g.capFramesLeft > 0 && g.capCount < kCaptureEvents) g.cap[g.capCount++] = Event{t0, t1, id, depth, kind};
}

uint16_t intern(const char* name, const char** table, int& count, int max) {
    for (int i = 0; i < count; ++i)
        if (table[i] == name || std::strcmp(table[i], name) == 0) return static_cast<uint16_t>(i);
    if (count >= max) return static_cast<uint16_t>(max - 1);
    table[count] = name;
    return static_cast<uint16_t>(count++);
}

uint64_t idleClockUs() {
    SceKernelSystemStatus st;
    std::memset(&st, 0, sizeof(st));
    st.size = sizeof(st);
    if (sceKernelReferSystemStatus(&st) < 0) return 0;
    return (static_cast<uint64_t>(st.idleClocks.hi) << 32) | st.idleClocks.low;
}

void dumpCapture() {
    char dir[256], path[288];
    fsPath(dir, sizeof(dir), "prof");
    sceIoMkdir(dir, 0777);
    for (int n = 0; n < 1000; ++n) {
        std::snprintf(path, sizeof(path), "%s/prof%03d.bin", dir, n);
        SceIoStat st;
        if (sceIoGetstat(path, &st) < 0) break;
    }
    const uint32_t t0 = now();
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        uint32_t hdr[8] = {0x46525057u /* "WPRF" */, 1u, static_cast<uint32_t>(WOWEE_PSP_BUILD), 0u,
                           static_cast<uint32_t>(g.zoneCount), static_cast<uint32_t>(g.passCount), g.capCount,
                           g.capFrames};
        sceIoWrite(fd, hdr, sizeof(hdr));
        auto writeNames = [&](const char** names, int n) {
            for (int i = 0; i < n; ++i) {
                const uint8_t len = static_cast<uint8_t>(std::strlen(names[i]) > 255 ? 255 : std::strlen(names[i]));
                sceIoWrite(fd, &len, 1);
                sceIoWrite(fd, names[i], len);
            }
        };
        writeNames(g.zoneNames, g.zoneCount);
        writeNames(g.passNames, g.passCount);
        sceIoWrite(fd, g.cap, g.capCount * sizeof(Event));
        sceIoClose(fd);  // FAT commits on close
    }
    PSP_LOGI(tag::Sys, "prof_capture file=%s events=%u frames=%u write_ms=%u", path, static_cast<unsigned>(g.capCount),
             static_cast<unsigned>(g.capFrames), static_cast<unsigned>((now() - t0) / 1000u));
    logFlush();
    std::free(g.cap);
    g.cap = nullptr;
    g.capCount = 0;
}

void publish() {
    Summary& s = g.sum;
    const double n = g.wFrames > 0 ? g.wFrames : 1;
    s.frameMs = static_cast<float>(g.wFrame / n / 1000.0);
    s.fps = s.frameMs > 0 ? 1000.0f / s.frameMs : 0;
    s.buildMs = static_cast<float>(g.wBuild / n / 1000.0);
    s.geTailMs = static_cast<float>(g.wTail / n / 1000.0);
    s.vblankMs = static_cast<float>(g.wVbl / n / 1000.0);
    s.geBusyMs = static_cast<float>(g.wGeBusy / n / 1000.0);
    s.cpuLoad = g.wFrame > 0 ? static_cast<float>(1.0 - g.wIdle / g.wFrame) : 0;
    s.passCount = g.passCount;
    for (int i = 0; i < g.passCount; ++i) {
        s.passName[i] = g.passNames[i];
        s.passMs[i] = static_cast<float>(g.wPass[i] / n / 1000.0);
    }
    s.zoneCount = g.zoneCount;
    for (int i = 0; i < g.zoneCount; ++i) {
        s.zoneName[i] = g.zoneNames[i];
        s.zoneSelfMs[i] = static_cast<float>(g.wSelf[i] / n / 1000.0);
        s.zoneInclMs[i] = static_cast<float>(g.wIncl[i] / n / 1000.0);
        s.zoneCalls[i] = static_cast<float>(g.wCalls[i] / n);
    }
    for (int c = 0; c < kCounterCount; ++c) s.counters[c] = static_cast<float>(g.wCounters[c] / n);
    s.heapUsedKb = heapUsedKb();  // both pools of the big-block heap
    s.heapFreeBlockKb = static_cast<uint32_t>(sceKernelMaxFreeMemSize() / 1024);
    s.captureFramesLeft = static_cast<uint32_t>(g.capFramesLeft > 0 ? g.capFramesLeft : 0);

    std::memset(g.wIncl, 0, sizeof(g.wIncl));
    std::memset(g.wSelf, 0, sizeof(g.wSelf));
    std::memset(g.wCalls, 0, sizeof(g.wCalls));
    std::memset(g.wCounters, 0, sizeof(g.wCounters));
    std::memset(g.wPass, 0, sizeof(g.wPass));
    g.wFrame = g.wBuild = g.wTail = g.wVbl = g.wGeBusy = g.wIdle = 0;
    g.wFrames = 0;

    // Log every ~2 s; lines stay under the 120-char log limit.
    const uint32_t ms = logUptimeMs();
    if (ms - g.lastLogMs < 2000) return;
    g.lastLogMs = ms;
    PSP_LOGI(tag::Gfx, "prof fps=%.1f frame=%.2f build=%.2f ge_tail=%.2f vbl=%.2f ge_busy=%.2f cpu=%.0f%%", s.fps,
             s.frameMs, s.buildMs, s.geTailMs, s.vblankMs, s.geBusyMs, s.cpuLoad * 100.0f);
    char line[120];
    int len = std::snprintf(line, sizeof(line), "ge");
    for (int i = 0; i < s.passCount && len < 100; ++i)
        len += std::snprintf(line + len, sizeof(line) - len, " %s=%.2f", s.passName[i], s.passMs[i]);
    PSP_LOGI(tag::Gfx, "prof_%s", line);
    // Zones by self time, a few per line.
    int order[kMaxZones];
    for (int i = 0; i < s.zoneCount; ++i) order[i] = i;
    for (int i = 0; i < s.zoneCount; ++i)
        for (int j = i + 1; j < s.zoneCount; ++j)
            if (s.zoneSelfMs[order[j]] > s.zoneSelfMs[order[i]]) std::swap(order[i], order[j]);
    len = 0;
    for (int k = 0; k < s.zoneCount && k < 12; ++k) {
        const int i = order[k];
        if (len > 80) {
            PSP_LOGI(tag::Gfx, "prof_zone%s", line);
            len = 0;
        }
        len += std::snprintf(line + len, sizeof(line) - len, " %s=%.2f/%.2f", s.zoneName[i], s.zoneSelfMs[i],
                             s.zoneInclMs[i]);
    }
    if (len) PSP_LOGI(tag::Gfx, "prof_zone%s", line);
    PSP_LOGI(tag::Gfx, "prof_cnt draws=%.0f tris=%.0f verts=%.0f binds=%.0f heap_kb=%u free_blk_kb=%u",
             s.counters[kDraws], s.counters[kTriangles], s.counters[kVertices], s.counters[kTexBinds],
             static_cast<unsigned>(s.heapUsedKb), static_cast<unsigned>(s.heapFreeBlockKb));
}

} // namespace

uint16_t zoneId(const char* name) { return intern(name, g.zoneNames, g.zoneCount, kMaxZones); }
uint16_t gePassId(const char* name) { return intern(name, g.passNames, g.passCount, kMaxGePasses); }

void zoneEnter(uint16_t id) {
    if (g.depth >= kMaxDepth) {
        ++g.depth;  // too deep: counted, not timed
        return;
    }
    g.stackId[g.depth] = id;
    g.stackChild[g.depth] = 0;
    g.stackT[g.depth] = now();
    ++g.depth;
}

void zoneLeave(uint16_t id) {
    if (g.depth <= 0) return;
    --g.depth;
    if (g.depth >= kMaxDepth) return;
    const uint32_t t = now();
    const uint32_t dur = t - g.stackT[g.depth];
    g.incl[id] += dur;
    g.self[id] += dur > g.stackChild[g.depth] ? dur - g.stackChild[g.depth] : 0;
    ++g.calls[id];
    if (g.depth > 0) g.stackChild[g.depth - 1] += dur;
    capture(g.stackT[g.depth], t, id, static_cast<uint8_t>(g.depth), 0);
}

void geMark(uint16_t passId) { sceGuSignal(GU_BEHAVIOR_CONTINUE, passId); }

void count(Counter c, uint32_t n) { g.counters[c] += n; }

void init() {
    sceGuSetCallback(GU_CALLBACK_SIGNAL, onSignal);
    sceGuSetCallback(GU_CALLBACK_FINISH, onFinish);
    g.idleLast = idleClockUs();
    gePassId("list");  // pass 0: from the list start to the first marker
    PSP_LOGI(tag::Sys, "prof_init zones_max=%d passes_max=%d", kMaxZones, kMaxGePasses);
}

void frameBegin() {
    g.tFrame = now();
    g_sigN = 0;
    g_finishT = 0;
    const uint64_t idle = idleClockUs();
    g.idleUs = static_cast<uint32_t>(idle - g.idleLast);
    g.idleLast = idle;
    geMark(0);
}

void frameListBuilt() { g.tBuilt = now(); }

void frameGeDone() {
    g.tGe = now();
    const int n = g_sigN;
    const uint32_t fin = g_finishT ? g_finishT : g.tGe;
    for (int i = 0; i < n; ++i) {
        const uint32_t t0 = g_sigT[i];
        const uint32_t t1 = i + 1 < n ? g_sigT[i + 1] : fin;
        const uint16_t id = g_sigId[i];
        if (id < kMaxGePasses) g.wPass[id] += t1 - t0;
        capture(t0, t1, id, 0, 1);
    }
    if (n > 0) g.wGeBusy += fin - g_sigT[0];
}

void frameEnd() {
    const uint32_t t = now();
    if (!g.tPrevEnd) g.tPrevEnd = g.tFrame;
    g.wFrame += t - g.tPrevEnd;
    g.wBuild += g.tBuilt - g.tPrevEnd;  // update + display-list build
    g.wTail += g.tGe - g.tBuilt;
    g.wVbl += t - g.tGe;
    g.wIdle += g.idleUs;
    for (int i = 0; i < g.zoneCount; ++i) {
        g.wIncl[i] += g.incl[i];
        g.wSelf[i] += g.self[i];
        g.wCalls[i] += g.calls[i];
    }
    for (int c = 0; c < kCounterCount; ++c) g.wCounters[c] += g.counters[c];
    if (g.cap && g.capFramesLeft > 0) {
        capture(g.tPrevEnd, t, static_cast<uint16_t>(g.frameIndex), 0, 2);
        capture(g.tBuilt, g.tBuilt, 0, 0, 3);
        capture(g.tGe, g.tGe, 1, 0, 3);
        for (int c = 0; c < kCounterCount; ++c) capture(g.counters[c], 0, static_cast<uint16_t>(c), 0, 4);
        ++g.capFrames;
        if (--g.capFramesLeft == 0 || g.capCount >= kCaptureEvents - 64) {
            g.capFramesLeft = 0;
            dumpCapture();
        }
    }
    std::memset(g.incl, 0, sizeof(g.incl));
    std::memset(g.self, 0, sizeof(g.self));
    std::memset(g.calls, 0, sizeof(g.calls));
    std::memset(g.counters, 0, sizeof(g.counters));
    g.tPrevEnd = t;
    ++g.frameIndex;
    if (++g.wFrames >= kWindowFrames) publish();
}

void toggleOverlay() { g.overlay = !g.overlay; }
bool overlayOn() { return g.overlay; }

void requestCapture(int frames) {
    if (g.cap) return;
    g.cap = static_cast<Event*>(std::malloc(kCaptureEvents * sizeof(Event)));
    if (!g.cap) {
        PSP_LOGW(tag::Sys, "prof_capture_nomem");
        return;
    }
    g.capCount = 0;
    g.capFrames = 0;
    g.capFramesLeft = frames;
    PSP_LOGI(tag::Sys, "prof_capture_start frames=%d", frames);
}

bool capturing() { return g.cap != nullptr; }

const Summary& summary() { return g.sum; }

} // namespace wowee::platform::psp::prof
