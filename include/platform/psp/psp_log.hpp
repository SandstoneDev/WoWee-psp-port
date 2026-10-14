// PSP session log (session.log next to the EBOOT).
//
// Rules this log follows (lessons from the sibling PSP ports):
//  - One descriptor, opened once on the main thread; writes are buffered and
//    committed on the main thread (logFlush) - a card write costs ~2 ms.
//  - Error lines are committed immediately; the buffered tail of a hard
//    crash is lost, so the last line of the file is not where it died.
//  - Lines: "[sec.ms] L TAG: key=value ...", English, <= 120 chars.
//  - No Info lines in the per-frame path.
#pragma once

#include <cstdarg>
#include <cstdint>

namespace wowee::platform::psp {

enum class LogLevel : int { Error = 0, Warn = 1, Info = 2, Debug = 3 };

// Fixed tag list - grep-able, one event per line.
namespace tag {
inline constexpr const char* Boot  = "BOOT";
inline constexpr const char* Sys   = "SYS";
inline constexpr const char* Mem   = "MEM";
inline constexpr const char* Fs    = "FS";
inline constexpr const char* Ctrl  = "CTRL";
inline constexpr const char* Gfx   = "GFX";
inline constexpr const char* Net   = "NET";
inline constexpr const char* Reuse = "REUSE";
inline constexpr const char* Loop  = "LOOP";
inline constexpr const char* Exit  = "EXIT";
} // namespace tag

// Opens the log (truncates) and writes the header block. Returns false when
// the file could not be opened; logging then goes nowhere but costs nothing.
bool logOpen(const char* path);
void logSetLevel(LogLevel maxLevel);
// Sync mode commits every line to the card at once: used during boot, where a
// crash would otherwise take the buffered tail - and the answer - with it.
// ~2 ms per line on hardware, so it is switched off before the main loop.
void logSetSync(bool sync);
void logWrite(LogLevel level, const char* tagName, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));
void logWriteV(LogLevel level, const char* tagName, const char* fmt, va_list ap);
// Commit the buffer to the card. Main thread only.
void logFlush();
void logClose();
// Opens the closed log again for appending (no header, nothing truncated) - a line after the teardown
// (systemRelaunch refused). Returns false when it could not be opened.
bool logReopen(const char* path);
// Milliseconds since the log was opened.
uint32_t logUptimeMs();
// For the on-device autopilot: one listener sees every line written (after the level filter, the
// whole formatted line without the newline, <= 120 chars). It must not log, allocate or block - it only looks.
// nullptr (the default) = off: the cost is one pointer test per line.
using LogListener = void (*)(const char* line, int len);
void logSetListener(LogListener fn);

} // namespace wowee::platform::psp

#define PSP_LOGE(t, ...) ::wowee::platform::psp::logWrite(::wowee::platform::psp::LogLevel::Error, t, __VA_ARGS__)
#define PSP_LOGW(t, ...) ::wowee::platform::psp::logWrite(::wowee::platform::psp::LogLevel::Warn, t, __VA_ARGS__)
#define PSP_LOGI(t, ...) ::wowee::platform::psp::logWrite(::wowee::platform::psp::LogLevel::Info, t, __VA_ARGS__)
#define PSP_LOGD(t, ...) ::wowee::platform::psp::logWrite(::wowee::platform::psp::LogLevel::Debug, t, __VA_ARGS__)
