#include "platform/psp/psp_system.hpp"

#include <kubridge.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <psploadexec.h>
#include <psppower.h>
#include <pspsysmem.h>
#include <pspthreadman.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <new>

#include "platform/psp/psp_bigheap.hpp"
#include "platform/psp/psp_log.hpp"
#include "sdbg.h"  // SandstoneDebug crash catcher (psp/sandstone-debug)

namespace wowee::platform::psp {

namespace {

volatile bool g_running = true;
SystemInfo g_info;

int exitCallback(int, int, void*) {
    g_running = false;
    return 0;
}

int callbackThread(SceSize, void*) {
    int cbid = sceKernelCreateCallback("wowee_exit_cb", exitCallback, nullptr);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

bool onEmulator() {
    // PPSSPP exposes a pseudo-device the console does not have.
    unsigned out = 0;
    return sceIoDevctl("kemulator:", 0x00000003, nullptr, 0, &out, sizeof(out)) >= 0;
}

const char* modelName(int model) {
    switch (model) {
        case 0: return "PSP-1000";
        case 1: return "PSP-2000";
        case 2: return "PSP-3000";
        case 3: return "PSP-3000(04g)";
        case 4: return "PSP-Go";
        case 6: return "PSP-3000(07g)";
        case 8: return "PSP-3000(09g)";
        case 10: return "PSP-E1000";
        default: return "PSP-?";
    }
}

} // namespace

void systemStartExitCallback() {
    SceUID th = sceKernelCreateThread("wowee_exit_th", callbackThread, 0x11, 0x1000, 0, nullptr);
    if (th >= 0) sceKernelStartThread(th, 0, nullptr);
}

bool systemRunning() { return g_running; }
void systemRequestExit() { g_running = false; }

const SystemInfo& systemProbe() {
    g_info.emulator = onEmulator();
    g_info.model = kuKernelGetModel();
    g_info.fwVersion = static_cast<uint32_t>(sceKernelDevkitVersion());
    g_info.cpuMHz = scePowerGetCpuClockFrequencyInt();
    g_info.busMHz = scePowerGetBusClockFrequencyInt();
    g_info.partFreeKB = static_cast<uint32_t>(sceKernelTotalFreeMemSize() / 1024);
    g_info.partMaxBlockKB = static_cast<uint32_t>(sceKernelMaxFreeMemSize() / 1024);
    return g_info;
}

const SystemInfo& systemInfo() { return g_info; }

void systemBanner(char* out, size_t cap) {
    const SystemInfo& s = g_info;
    std::snprintf(out, cap, "%s gen=%d fw=%x.%02x cpu=%d/%d emu=%d",
                  modelName(s.model), s.model + 1,
                  static_cast<unsigned>((s.fwVersion >> 24) & 0xFF),
                  static_cast<unsigned>((s.fwVersion >> 16) & 0xFF),
                  s.cpuMHz, s.busMHz, s.emulator ? 1 : 0);
}

int systemSetClock(int cpuMHz) {
    int bus = cpuMHz / 2;
    scePowerSetClockFrequency(cpuMHz, cpuMHz, bus);
    g_info.cpuMHz = scePowerGetCpuClockFrequencyInt();
    g_info.busMHz = scePowerGetBusClockFrequencyInt();
    return g_info.cpuMHz;
}

void systemExit() {
    // The crash catcher's handlers must leave the firmware's lists before the
    // module goes (sdbg.h: else the console dies on the way out).
    sdbg_shutdown();
    sceKernelExitGame();
    for (;;) sceKernelSleepThread();
}

int systemRelaunch(const char* ebootPath) {
    if (!ebootPath || !*ebootPath) return -1;
    sdbg_shutdown();  // as systemExit: the catcher's handlers leave the firmware's lists first (idempotent)
    static char arg[256];
    std::snprintf(arg, sizeof(arg), "%s", ebootPath);
    SceKernelLoadExecParam param{};
    param.size = sizeof(param);
    param.args = static_cast<SceSize>(std::strlen(arg) + 1);  // argv[0] of the next run (fsInit reads it)
    param.argp = arg;
    param.key = nullptr;
    return sceKernelLoadExec(arg, &param);
}


namespace {
const char* g_lastMemStep = "boot";
// the largest block one malloc gives now - either pool of the big-block heap (psp_bigheap.hpp; with
// bigheap=0 newlib's binary search as before: 64 KB steps, volatile + a write so gcc keeps the malloc/free pair -
// a "largest_kb=65472" on hardware was a dropped pair).
uint32_t largestMallocKb() { return heapLargestKb(); }
void oomHandler() {
    std::set_new_handler(nullptr);  // one report; the next failure throws/aborts
    struct mallinfo mi = mallinfo();
    PSP_LOGE(tag::Mem, "oom new failed after=%s heap_used_kb=%u arena_kb=%u largest_kb=%u part_free_kb=%u",
             g_lastMemStep, static_cast<unsigned>(heapUsedKb()), static_cast<unsigned>(mi.arena / 1024),
             static_cast<unsigned>(largestMallocKb()), static_cast<unsigned>(sceKernelMaxFreeMemSize() / 1024));
    bigheapLogPools();
    logFlush();
}
}  // namespace

// heap_used_kb = both pools (the region's reservation not counted), largest_kb = the largest block one
// malloc gets (either pool); the "bigheap ..." line after it splits them (psp_bigheap.hpp).
void logMemory(const char* where) {
    g_lastMemStep = where;
    bigheapPoll();
    struct mallinfo mi = mallinfo();
    PSP_LOGI(tag::Mem, "mem at=%s heap_used_kb=%u arena_kb=%u largest_kb=%u part_free_kb=%u", where,
             static_cast<unsigned>(heapUsedKb()), static_cast<unsigned>(mi.arena / 1024),
             static_cast<unsigned>(largestMallocKb()), static_cast<unsigned>(sceKernelMaxFreeMemSize() / 1024));
    bigheapLogPools();
}

namespace {
uint32_t g_peakUsedKb = 0, g_minLargestKb = ~0u, g_watchTicks = 0;
char g_peakAt[48] = "-", g_minAt[48] = "-";
}  // namespace

void memWatchTick(const char* where) {
    bigheapPoll();
    struct mallinfo mi = mallinfo();
    const uint32_t used = heapUsedKb(), largest = largestMallocKb();
    if (used > g_peakUsedKb) {
        g_peakUsedKb = used;
        std::snprintf(g_peakAt, sizeof(g_peakAt), "%s", where);
    }
    if (largest < g_minLargestKb) {
        g_minLargestKb = largest;
        std::snprintf(g_minAt, sizeof(g_minAt), "%s", where);
    }
    ++g_watchTicks;
    PSP_LOGI(tag::Mem, "mem_watch at=%s used_kb=%u arena_kb=%u largest_kb=%u peak_used_kb=%u min_largest_kb=%u", where,
             static_cast<unsigned>(used), static_cast<unsigned>(mi.arena / 1024), static_cast<unsigned>(largest),
             static_cast<unsigned>(g_peakUsedKb), static_cast<unsigned>(g_minLargestKb));
    bigheapLogPools();
}

void memWatchSummary() {
    if (g_watchTicks == 0) return;
    PSP_LOGI(tag::Mem, "mem_peak ticks=%u peak_used_kb=%u at=%s min_largest_kb=%u at=%s",
             static_cast<unsigned>(g_watchTicks), static_cast<unsigned>(g_peakUsedKb), g_peakAt,
             static_cast<unsigned>(g_minLargestKb), g_minAt);
    bigheapLogPeak();
}

void installOomHandler() { std::set_new_handler(&oomHandler); }

} // namespace wowee::platform::psp
