// PSP system bring-up: exit callback, console identification, memory report,
// CPU clock. Everything here runs on the main thread during boot.
#pragma once

#include <cstddef>
#include <cstdint>

namespace wowee::platform::psp {

struct SystemInfo {
    bool emulator = false;        // PPSSPP ("kemulator:" device exists)
    int  model = -1;              // kuKernelGetModel(): 0 = PSP-1000, 1 = 2000, 2 = 3000, ...
    uint32_t fwVersion = 0;       // sceKernelDevkitVersion(), e.g. 0x06060010
    int  cpuMHz = 0;
    int  busMHz = 0;
    uint32_t partFreeKB = 0;      // user partition free after heap reservation
    uint32_t partMaxBlockKB = 0;  // largest free partition block
};

// Starts the HOME-button exit callback thread. Call first in main():
// thread stacks come from the user partition, not the heap.
void systemStartExitCallback();

// True until HOME -> Exit (or systemRequestExit) was requested.
bool systemRunning();
void systemRequestExit();

// Fills SystemInfo; safe on hardware and PPSSPP.
const SystemInfo& systemProbe();
const SystemInfo& systemInfo();

// "PSP-3000 (gen 3) fw 6.61 cpu 333/166 MHz emu=0" style banner.
void systemBanner(char* out, size_t cap);

// Hardware crash at world entry: one log line with the heap in use,
// the largest block malloc can still give (probed), and the partition outside
// the heap. `where` names the step.
void logMemory(const char* where);
// PSP (owner: "measure how much memory we have left"): the in-world watch - every call (the world
// screen, every 10 s) logs "mem_watch" with the heap in use, the largest free block, and the session's peak
// use / smallest largest block so far; memWatchSummary() logs "mem_peak" once at exit (the postmortem line).
void memWatchTick(const char* where);
void memWatchSummary();
// std::set_new_handler: an allocation that fails logs heap numbers (and the
// last logMemory step) before the program ends - instead of a silent drop
// to the XMB.
void installOomHandler();

// Best-effort clock set (222/266/333 only). Returns the resulting CPU MHz.
int systemSetClock(int cpuMHz);

// Tear down and return to the XMB. Does not return.
[[noreturn]] void systemExit();

// A language change: start the EBOOT at `ebootPath` again - LoadExecForUser
// sceKernelLoadExec, after the crash catcher left the firmware's lists (as systemExit). Returns only when the
// firmware refused (the error code): stock firmware refuses a game started from the Memory Stick
// (uOFW loadexec.c: sceKernelBootFrom() >= 0x30 -> 0x80020149) before anything is torn down, so the caller
// still exits normally. The caller has closed the log and stopped the network first.
int systemRelaunch(const char* ebootPath);

} // namespace wowee::platform::psp
