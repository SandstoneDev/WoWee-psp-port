#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"  // the descriptor budget counts the log's

#include <pspiofilemgr.h>
#include <pspkerneltypes.h>
#include <psputils.h>
#include <pspthreadman.h>

#include <cstdio>
#include <cstring>
#include "sdbg.h"  // SandstoneDebug: log ring for CRASH.TXT

namespace wowee::platform::psp {

namespace {

constexpr int kLineMax = 120;
constexpr int kBufSize = 16 * 1024;
constexpr int kFlushAt = 12 * 1024;

SceUID g_fd = -1;
char g_buf[kBufSize];
int g_len = 0;
LogLevel g_maxLevel = LogLevel::Debug;
uint64_t g_t0 = 0;
uint32_t g_dropped = 0;
bool g_sync = false;
LogListener g_listener = nullptr;  // the autopilot (psp_autotest.cpp)

uint64_t nowUs() { return sceKernelGetSystemTimeWide(); }

void commit() {
    if (g_fd < 0 || g_len == 0) return;
    int off = 0;
    while (off < g_len) {
        int w = sceIoWrite(g_fd, g_buf + off, g_len - off);
        if (w <= 0) break;
        off += w;
    }
    g_len = 0;
}

void append(const char* s, int n) {
    if (n <= 0) return;
    if (g_len + n > kBufSize) commit();
    if (n > kBufSize) { ++g_dropped; return; }
    std::memcpy(g_buf + g_len, s, static_cast<size_t>(n));
    g_len += n;
}

char levelChar(LogLevel l) {
    switch (l) {
        case LogLevel::Error: return 'E';
        case LogLevel::Warn:  return 'W';
        case LogLevel::Info:  return 'I';
        default:              return 'D';
    }
}

} // namespace

bool logOpen(const char* path) {
    g_t0 = nowUs();
    g_len = 0;
    // Remove first: PPSSPP did not always honour O_TRUNC, leaving the previous
    // run's tail glued after this one's lines.
    sceIoRemove(path);
    g_fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (g_fd < 0) return false;
    pakNoteOpen(g_fd, path);  // in the descriptor budget (pak_open_list)
    static const char bom[] = "\xEF\xBB\xBF";
    append(bom, 3);
    return true;
}

bool logReopen(const char* path) {
    if (g_fd >= 0) return true;
    g_fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (g_fd < 0) return false;
    pakNoteOpen(g_fd, path);
    return true;
}

void logSetLevel(LogLevel maxLevel) { g_maxLevel = maxLevel; }
void logSetListener(LogListener fn) { g_listener = fn; }
void logSetSync(bool sync) {
    g_sync = sync;
    if (sync) commit();
}

uint32_t logUptimeMs() { return static_cast<uint32_t>((nowUs() - g_t0) / 1000u); }

void logWriteV(LogLevel level, const char* tagName, const char* fmt, va_list ap) {
    if (static_cast<int>(level) > static_cast<int>(g_maxLevel)) return;
    char line[kLineMax + 32];
    uint32_t ms = logUptimeMs();
    int n = std::snprintf(line, sizeof(line), "[%6u.%03u] %c %s: ",
                          static_cast<unsigned>(ms / 1000u), static_cast<unsigned>(ms % 1000u),
                          levelChar(level), tagName);
    if (n < 0) return;
    int room = kLineMax - n;
    if (room > 0) {
        int m = std::vsnprintf(line + n, static_cast<size_t>(room) + 1, fmt, ap);
        if (m > room) {
            // Truncated: mark it so a reader does not mistake it for the full line.
            n = kLineMax;
            line[n - 2] = '~';
            line[n - 1] = '~';
        } else if (m > 0) {
            n += m;
        }
    }
    if (g_listener) g_listener(line, n);  // the autopilot's waits (psp_autotest.cpp)
    line[n++] = '\n';
    append(line, n);
    sdbg_log_append(line, n);  // the crash catcher's ring: survives a crash between card commits
    if (g_sync || level == LogLevel::Error || g_len >= kFlushAt) commit();
}

void logWrite(LogLevel level, const char* tagName, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    logWriteV(level, tagName, fmt, ap);
    va_end(ap);
}

void logFlush() { commit(); }

void logClose() {
    if (g_dropped) {
        logWrite(LogLevel::Warn, tag::Exit, "log_dropped=%u", static_cast<unsigned>(g_dropped));
    }
    commit();
    if (g_fd >= 0) {
        pakNoteClose(g_fd);
        sceIoClose(g_fd);
        g_fd = -1;
    }
}

} // namespace wowee::platform::psp
