// the on-device autopilot - psp_autotest.hpp.
//
// Owner (07.10): "a special build that tests everything by itself on the real PSP; I only watch". The script
// (autotest.txt, psp_autotest_script.hpp) is played step by step from the main loop; the world screen runs the devcmd
// verbs (autotestServiceWorld); a log listener (psp_log.hpp logSetListener) answers the waits, catches the fatal lines
// and keeps the memory lines for the report. Nothing here runs without the file: autotestBoot() is one sceIoOpen.
//
// Per block: PASS unless a step failed (a wait timed out, the world refused a command, "seen" / "moved" did not hold),
// an expect_not text was logged, or a fatal line came (oom new failed, the character's death, a bad free of the big
// heap). A failed block skips its remaining steps except those marked "always"; a death is answered with ".revive"
// first. The run goes on with the next block either way. A CPU crash ends the program (CRASH.TXT, as always) - the
// report has every block finished before it.
//
// Log: autotest_start / autotest_set key= / autotest_parse_error / autotest_block_start id= / autotest_step n= verb=
// ok= ms= line= / autotest_fail / autotest_block id= result= ms= / autotest_pause on= / autotest_shot / autotest_end.
// The listener ignores the autotest_ lines themselves.
#include "platform/psp/psp_autotest.hpp"

#include "platform/psp/psp_autotest_script.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_input.hpp"
#include "platform/psp/psp_locale.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_public_safe.hpp"  // PSP: the autopilot runs only against a dev server
#include "platform/psp/psp_system.hpp"

#include <pspctrl.h>
#include <pspiofilemgr.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace wowee::platform::psp {

namespace at = autotest;

namespace {

constexpr int kLine = 128;     // a log line is <= 120 chars
constexpr uint32_t kWorldTimeoutMs = 600000;  // a devcmd verb waits this long for a world (loading screens)
constexpr uint32_t kHintMs = 20000;           // "START+SELECT - pause" at the start

// Fatal in any block. 1 and 2: the character died (corpse / ghost) - answered with ".revive".
constexpr const char* kFatal[] = {"oom new failed", "corpse_reclaim_delay", "player_dead ", "bigheap_badfree"};
constexpr int kFatalCount = 4;
// The report's summary lines: the last such line of the block.
constexpr const char* kCapture[] = {"mem at=", "bigheap big_used_kb", "mem_governor ev=status", "mem_peak",
                                    "bigheap_peak", "heap_budget", "bigheap_init", "bigheap_small"};
constexpr int kCaptureCount = 8;

struct Result {
    int block = 0;
    bool pass = false;
    uint32_t ms = 0;
};

struct Runner {
    at::Script sc;
    bool ru = true;
    char txtPath[256] = {}, donePath[256] = {}, reportPath[256] = {};

    // --- written by the log listener --------------------------------------------------------------------------
    // The waits of the current block (step indices; a fixed array - the listener may run on another thread) and the
    // time each one's text was last logged (0 = never; | 1).
    int blockWaits[at::kMaxBlockWaits] = {};
    volatile int blockWaitCount = 0;
    std::vector<uint32_t> waitHitMs;
    volatile bool fatalHit[kFatalCount] = {};
    char fatalLine[kLine] = {};
    std::vector<uint8_t> seenHit;
    volatile bool forbidHit = false;
    char forbidLine[kLine] = {};
    char capture[kCaptureCount][kLine] = {};
    volatile uint32_t captureBits = 0;  // captured during the current block
    float pos[2][3] = {};
    volatile int posCount = 0;

    // --- the run ------------------------------------------------------------------------------------------------
    bool running = false, finished = false, summary = false, paused = false;
    uint32_t startMs = 0, pausedTotal = 0, pauseStart = 0, hintUntil = 0, endMs = 0;
    size_t step = 0;
    bool stepStarted = false, stepArmed = false;
    uint32_t stepClk = 0, stepWall = 0;
    uint32_t actionMs = 0;  // the start of the block's last action step (or the block's start): a wait's window
    std::string failWhy;  // the current step's failure
    volatile int block = -1;
    uint32_t blockWall = 0, blockPaused0 = 0;
    bool blockFailed = false, blockSkipping = false;
    bool fatalHandled = false;  // this block's fatal line was answered (checkFatal)
    std::string blockReason;
    int look = -1;  // step index of the shown look
    // A devcmd verb for the world screen; owner 1 = the current step, 2 = the ".revive" after a death.
    std::string worldLine;
    int worldOwner = 0;
    bool worldPending = false, worldDone = false, worldOk = false;
    bool reviveNeeded = false;
    std::vector<Result> results;
    int pass = 0, fail = 0;
    AutotestView view;
};

Runner* g = nullptr;
// PSP: the autopilot refused (a public server): the overlay's one-line notice until this time (0 = none).
uint32_t g_refusedUntil = 0;
constexpr uint32_t kRefusedNoticeMs = 30000;

void refuse(const char* at) {
    PSP_LOGW(tag::Sys, "autotest_refused public=1 at=%s", at);
    g_refusedUntil = (logUptimeMs() + kRefusedNoticeMs) | 1u;
    logFlush();
}

uint32_t clk(const Runner& r) {
    const uint32_t now = logUptimeMs();
    return now - r.pausedTotal - (r.paused ? now - r.pauseStart : 0u);
}

void copyLine(char* dst, const char* src) {
    size_t n = 0;
    while (n < static_cast<size_t>(kLine - 1) && src[n]) ++n;
    std::memcpy(dst, src, n);
    dst[n] = 0;
}

void listener(const char* lineIn, int len) {
    Runner* r = g;
    if (!r || !r->running || r->finished) return;
    char line[kLine];
    const int n = len < kLine - 1 ? (len > 0 ? len : 0) : kLine - 1;
    std::memcpy(line, lineIn, static_cast<size_t>(n));
    line[n] = 0;
    if (std::strstr(line, ": autotest_")) return;  // our own lines
    const uint32_t now = logUptimeMs() | 1u;
    const int nw = r->blockWaitCount;
    for (int k = 0; k < nw && k < at::kMaxBlockWaits; ++k) {
        const size_t idx = static_cast<size_t>(r->blockWaits[k]);
        if (idx < r->waitHitMs.size() && at::matchAny(line, r->sc.steps[idx].arg.c_str())) r->waitHitMs[idx] = now;
    }
    for (int i = 0; i < kFatalCount; ++i)
        if (std::strstr(line, kFatal[i])) {
            if (!r->fatalHit[0] && !r->fatalHit[1] && !r->fatalHit[2] && !r->fatalHit[3]) copyLine(r->fatalLine, line);
            r->fatalHit[i] = true;
        }
    const int b = r->block;
    if (b >= 0 && b < static_cast<int>(r->sc.blocks.size()) && !r->forbidHit)
        for (const std::string& f : r->sc.blocks[static_cast<size_t>(b)].forbidden)
            if (std::strstr(line, f.c_str())) {
                copyLine(r->forbidLine, line);
                r->forbidHit = true;
                break;
            }
    for (size_t i = 0; i < r->sc.seen.size(); ++i)
        if (!r->seenHit[i] && std::strstr(line, r->sc.seen[i].c_str())) r->seenHit[i] = 1;
    for (int i = 0; i < kCaptureCount; ++i)
        if (std::strstr(line, kCapture[i])) {
            copyLine(r->capture[i], line);
            r->captureBits = r->captureBits | (1u << i);
        }
    float x = 0, y = 0, z = 0;
    if (at::parsePos(line, x, y, z)) {
        std::memcpy(r->pos[0], r->pos[1], sizeof(r->pos[0]));
        r->pos[1][0] = x;
        r->pos[1][1] = y;
        r->pos[1][2] = z;
        r->posCount = r->posCount + 1;
    }
}

// The report: one open-append-close per write (a few per block) - a crash keeps every block finished before it.
void reportWrite(const Runner& r, bool truncate, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
void reportWrite(const Runner& r, bool truncate, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= static_cast<int>(sizeof(buf))) n = static_cast<int>(sizeof(buf)) - 1;
    const SceUID fd = sceIoOpen(r.reportPath, PSP_O_WRONLY | PSP_O_CREAT | (truncate ? PSP_O_TRUNC : PSP_O_APPEND),
                                0777);
    if (fd < 0) return;
    sceIoWrite(fd, buf, static_cast<SceSize>(n));
    sceIoClose(fd);
}

void reportCaptures(const Runner& r, uint32_t bits) {
    for (int i = 0; i < kCaptureCount; ++i)
        if ((bits & (1u << i)) && r.capture[i][0]) reportWrite(r, false, "    %s\n", r.capture[i]);
}

const at::Block& blockOf(const Runner& r, int i) { return r.sc.blocks[static_cast<size_t>(i)]; }
const char* caption(const Runner& r, const std::string& ru, const std::string& en) {
    return (r.ru ? ru : en).c_str();
}

void startBlock(Runner& r, int i) {
    r.blockWall = logUptimeMs();
    r.blockPaused0 = r.pausedTotal;
    r.blockFailed = false;
    r.blockSkipping = false;
    r.fatalHandled = false;
    r.blockReason.clear();
    r.forbidHit = false;
    r.forbidLine[0] = 0;
    for (int k = 0; k < kFatalCount; ++k) r.fatalHit[k] = false;
    r.fatalLine[0] = 0;
    r.captureBits = 0;
    r.look = -1;
    r.actionMs = r.blockWall;
    r.blockWaitCount = 0;
    int nw = 0;
    const at::Block& b = blockOf(r, i);
    for (int k = 0; k < b.stepCount && nw < at::kMaxBlockWaits; ++k) {
        const int idx = b.firstStep + 1 + k;
        if (idx >= 0 && idx < static_cast<int>(r.sc.steps.size()) && r.sc.steps[static_cast<size_t>(idx)].verb == at::Verb::Wait)
            r.blockWaits[nw++] = idx;
    }
    r.blockWaitCount = nw;
    r.block = i;
    PSP_LOGI(tag::Sys, "autotest_block_start id=%s n=%d/%d", blockOf(r, i).id.c_str(), i + 1,
             static_cast<int>(r.sc.blocks.size()));
}

void finishBlock(Runner& r) {
    if (r.block < 0) return;
    const int i = r.block;
    std::string reason = r.blockReason;
    bool pass = !r.blockFailed;
    bool fatal = false;
    for (int k = 0; k < kFatalCount; ++k) fatal |= r.fatalHit[k];
    if (fatal) {
        pass = false;
        reason = std::string("fatal: ") + r.fatalLine;
    } else if (r.forbidHit) {
        pass = false;
        if (reason.empty()) reason = std::string("expect_not: ") + r.forbidLine;
    }
    const uint32_t ms = logUptimeMs() - r.blockWall;
    const uint32_t pausedMs = r.pausedTotal - r.blockPaused0;
    r.results.push_back(Result{i, pass, ms});
    (pass ? r.pass : r.fail) += 1;
    PSP_LOGI(tag::Sys, "autotest_block id=%s result=%s ms=%u paused_ms=%u", blockOf(r, i).id.c_str(),
             pass ? "PASS" : "FAIL", static_cast<unsigned>(ms), static_cast<unsigned>(pausedMs));
    if (!pass) PSP_LOGW(tag::Sys, "autotest_fail id=%s why=%s", blockOf(r, i).id.c_str(), reason.c_str());
    reportWrite(r, false, "block %d/%d id=%s result=%s ms=%u paused_ms=%u reason=%s\n", i + 1,
                static_cast<int>(r.sc.blocks.size()), blockOf(r, i).id.c_str(), pass ? "PASS" : "FAIL",
                static_cast<unsigned>(ms), static_cast<unsigned>(pausedMs), reason.empty() ? "-" : reason.c_str());
    reportCaptures(r, r.captureBits);
    r.block = -1;
}

bool renameDone(Runner& r) {
    sceIoRemove(r.donePath);
    if (sceIoRename(r.txtPath, r.donePath) >= 0) return true;
    // Refused (some firmwares want the new name without the folder): the copy, then the original goes.
    std::string text;
    const SceUID in = sceIoOpen(r.txtPath, PSP_O_RDONLY, 0);
    if (in < 0) return false;
    char buf[1024];
    int got;
    while ((got = sceIoRead(in, buf, sizeof(buf))) > 0 && text.size() < 64 * 1024) text.append(buf, static_cast<size_t>(got));
    sceIoClose(in);
    const SceUID fd = sceIoOpen(r.donePath, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return false;
    const int n = sceIoWrite(fd, text.data(), static_cast<SceSize>(text.size()));
    sceIoClose(fd);
    if (n != static_cast<int>(text.size())) return false;
    return sceIoRemove(r.txtPath) >= 0;
}

void finishRun(Runner& r, const char* why) {
    if (r.finished) return;
    finishBlock(r);
    r.blockWaitCount = 0;
    memWatchSummary();  // mem_peak / bigheap_peak (the listener keeps them for the report)
    r.finished = true;
    r.summary = true;
    r.endMs = logUptimeMs();
    const bool renamed = renameDone(r);
    PSP_LOGI(tag::Sys, "autotest_end why=%s pass=%d fail=%d blocks=%d ms=%u paused_ms=%u renamed=%d", why, r.pass,
             r.fail, static_cast<int>(r.sc.blocks.size()), static_cast<unsigned>(r.endMs - r.startMs),
             static_cast<unsigned>(r.pausedTotal), renamed ? 1 : 0);
    reportWrite(r, false, "\nsummary pass=%d fail=%d blocks=%d ms=%u paused_ms=%u end=%s renamed=%d\n", r.pass, r.fail,
                static_cast<int>(r.sc.blocks.size()), static_cast<unsigned>(r.endMs - r.startMs),
                static_cast<unsigned>(r.pausedTotal), why, renamed ? 1 : 0);
    reportCaptures(r, ~0u);
    for (const Result& res : r.results)
        reportWrite(r, false, "  %-4s %s\n", res.pass ? "PASS" : "FAIL", blockOf(r, res.block).id.c_str());
    logSetListener(nullptr);
    logFlush();
}

void togglePause(Runner& r) {
    const uint32_t now = logUptimeMs();
    r.paused = !r.paused;
    if (r.paused) {
        r.pauseStart = now;
        inputScriptStop();  // the owner has the pad: nothing held, no nub
    } else {
        r.pausedTotal += now - r.pauseStart;
    }
    PSP_LOGI(tag::Sys, "autotest_pause on=%d step=%u", r.paused ? 1 : 0, static_cast<unsigned>(r.step + 1));
}

// A 480x272 24-bit BMP of the frame on screen into autotest/<name>.bmp (16 rows a write; ~390 KB).
bool shot(const std::string& name, uint32_t& ms) {
    const uint32_t t0 = logUptimeMs();
    char dir[256], path[320];
    fsPath(dir, sizeof(dir), "autotest");
    sceIoMkdir(dir, 0777);
    std::snprintf(path, sizeof(path), "%s/%s.bmp", dir, name.c_str());
    int stride = 0;
    const uint16_t* px = displayShownPixels(&stride);
    constexpr int kW = kScreenW, kH = kScreenH, kRowBytes = kW * 3, kRows = 16;
    auto* buf = static_cast<uint8_t*>(std::malloc(kRowBytes * kRows));  // malloc: no new_handler (no oom line)
    if (!buf) return false;
    const SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        std::free(buf);
        return false;
    }
    uint8_t h[54] = {'B', 'M'};
    auto put32 = [&h](int at, uint32_t v) {
        for (int k = 0; k < 4; ++k) h[at + k] = static_cast<uint8_t>(v >> (8 * k));
    };
    put32(2, 54u + kRowBytes * kH);
    put32(10, 54);
    put32(14, 40);
    put32(18, kW);
    put32(22, kH);  // bottom-up
    h[26] = 1;
    h[28] = 24;
    put32(34, kRowBytes * kH);
    put32(38, 2835);
    put32(42, 2835);
    bool ok = sceIoWrite(fd, h, sizeof(h)) == static_cast<int>(sizeof(h));
    for (int y0 = kH - 1; ok && y0 >= 0; y0 -= kRows) {
        int rows = 0;
        for (int y = y0; y > y0 - kRows && y >= 0; --y, ++rows) {
            const uint16_t* src = px + y * stride;
            uint8_t* dst = buf + rows * kRowBytes;
            for (int x = 0; x < kW; ++x) {
                const uint32_t p = src[x];
                const uint32_t r5 = p & 31u, g5 = (p >> 5) & 31u, b5 = (p >> 10) & 31u;
                dst[x * 3 + 0] = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
                dst[x * 3 + 1] = static_cast<uint8_t>((g5 << 3) | (g5 >> 2));
                dst[x * 3 + 2] = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
            }
        }
        ok = sceIoWrite(fd, buf, static_cast<SceSize>(rows * kRowBytes)) == rows * kRowBytes;
    }
    sceIoClose(fd);
    std::free(buf);
    ms = logUptimeMs() - t0;
    return ok;
}

void requestWorld(Runner& r, const std::string& line, int owner) {
    r.worldLine = line;
    r.worldOwner = owner;
    r.worldPending = true;
    r.worldDone = false;
    r.worldOk = false;
}

// 1 done, 0 running, -1 failed (r.failWhy).
int runStep(Runner& r, const at::Step& st) {
    const bool first = !r.stepArmed;
    r.stepArmed = true;
    const uint32_t age = clk(r) - r.stepClk;
    switch (st.verb) {
        case at::Verb::Block:
            finishBlock(r);
            startBlock(r, st.block);
            return 1;
        case at::Verb::Look:
            r.look = static_cast<int>(r.step);
            PSP_LOGI(tag::Sys, "autotest_look block=%s step=%u", blockOf(r, st.block).id.c_str(),
                     static_cast<unsigned>(r.step + 1));
            return 1;
        case at::Verb::Sleep:
            return age >= st.ms ? 1 : 0;
        case at::Verb::Wait: {
            // The listener stamps every line with this step's text (the block's waits); one since the block's last
            // action (say / pad / mem ...) or its start passes.
            const uint32_t hit = r.step < r.waitHitMs.size() ? r.waitHitMs[r.step] : 0u;
            if (hit && static_cast<int32_t>(hit - r.actionMs) >= 0) return 1;
            if (age >= st.ms) {
                r.failWhy = "wait " + std::to_string(st.ms / 1000u) + " s: " + st.arg;
                return -1;
            }
            return 0;
        }
        case at::Verb::Seen:
            for (size_t i = 0; i < r.sc.seen.size(); ++i)
                if (r.sc.seen[i] == st.arg && r.seenHit[i]) return 1;
            r.failWhy = "not seen: " + st.arg;
            return -1;
        case at::Verb::ExpectNot:
            return 1;  // the listener watches it from the block's start; judged when the block ends
        case at::Verb::Mem:
            if (st.arg == "peak") memWatchSummary();
            else logMemory(st.arg.c_str());  // the script outlives the run: the name stays valid (g_lastMemStep)
            return 1;
        case at::Verb::Shot: {
            if (configGetInt("autotestshot", 1) == 0) {
                PSP_LOGI(tag::Sys, "autotest_shot name=%s skipped=1", st.arg.c_str());
                return 1;
            }
            uint32_t ms = 0;
            const bool ok = shot(st.arg, ms);
            PSP_LOGI(tag::Sys, "autotest_shot name=%s ok=%d ms=%u", st.arg.c_str(), ok ? 1 : 0, static_cast<unsigned>(ms));
            return 1;  // a missing picture never fails a block
        }
        case at::Verb::Moved: {
            if (r.posCount < 2) {
                r.failWhy = "moved: fewer than two pos lines";
                return -1;
            }
            const float dx = r.pos[1][0] - r.pos[0][0], dy = r.pos[1][1] - r.pos[0][1];
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d >= st.yards) return 1;
            char why[96];
            std::snprintf(why, sizeof(why), "moved %.1f yd < %.1f", static_cast<double>(d), static_cast<double>(st.yards));
            r.failWhy = why;
            return -1;
        }
        case at::Verb::End:
            return 1;
        case at::Verb::Mark:
            PSP_LOGI(tag::Ctrl, "devcmd_mark name=%s", st.arg.c_str());
            return 1;
        case at::Verb::Pad:
            if (first) {
                inputScriptRun(st.arg);
                return 0;
            }
            if (inputScriptIdle()) return 1;
            if (age >= 60000u) {
                inputScriptStop();
                r.failWhy = "pad did not finish in 60 s";
                return -1;
            }
            return 0;
        case at::Verb::Say:
        case at::Verb::Ui:
        case at::Verb::Open:
        case at::Verb::Close:
        case at::Verb::Target:
        case at::Verb::Pos:
            if (first) {
                requestWorld(r, st.arg.empty() ? std::string(at::verbName(st.verb))
                                               : std::string(at::verbName(st.verb)) + " " + st.arg, 1);
                return 0;
            }
            if (r.worldDone && r.worldOwner == 1) {
                r.worldOwner = 0;
                if (r.worldOk) return 1;
                r.failWhy = "world refused: " + r.worldLine;
                return -1;
            }
            if (age >= kWorldTimeoutMs) {
                r.worldPending = false;
                r.worldOwner = 0;
                r.failWhy = "no world for 600 s: " + r.worldLine;
                return -1;
            }
            return 0;
    }
    return 1;
}

void beginStep(Runner& r) {
    const at::Step& st = r.sc.steps[r.step];
    r.stepStarted = true;
    r.stepArmed = false;
    r.stepClk = clk(r);
    r.stepWall = logUptimeMs();
    if (at::isActionVerb(st.verb)) r.actionMs = r.stepWall;  // the next waits look from here
    r.failWhy.clear();
}

void endStep(Runner& r, const at::Step& st, bool ok) {
    PSP_LOGI(tag::Sys, "autotest_step n=%u verb=%s ok=%d ms=%u line=%d%s", static_cast<unsigned>(r.step + 1),
             at::verbName(st.verb), ok ? 1 : 0, static_cast<unsigned>(clk(r) - r.stepClk), st.line,
             st.optional ? " try=1" : "");
    if (!ok) {
        PSP_LOGW(tag::Sys, "autotest_step_fail n=%u why=%s", static_cast<unsigned>(r.step + 1), r.failWhy.c_str());
        if (!st.optional && r.block >= 0 && !r.blockFailed) {
            r.blockFailed = true;
            r.blockSkipping = true;
            r.blockReason = "step " + std::to_string(r.step + 1) + " (line " + std::to_string(st.line) + ") " +
                            at::verbName(st.verb) + ": " + r.failWhy;
        }
    }
    ++r.step;
    r.stepStarted = false;
}

// Every frame in a block: a fatal line fails it; a death is answered with ".revive" before anything else.
void checkFatal(Runner& r) {
    if (r.block < 0) return;
    bool fatal = false;
    for (int k = 0; k < kFatalCount; ++k) fatal |= r.fatalHit[k];
    if (!fatal || r.fatalHandled) return;
    r.fatalHandled = true;
    if (!r.blockFailed) r.blockReason = std::string("fatal: ") + r.fatalLine;
    r.blockFailed = true;
    r.blockSkipping = true;
    if (r.fatalHit[1] || r.fatalHit[2]) r.reviveNeeded = true;  // even when the block had failed before
    PSP_LOGW(tag::Sys, "autotest_fatal block=%s revive=%d", blockOf(r, r.block).id.c_str(), r.reviveNeeded ? 1 : 0);
    if (r.stepStarted) {  // the step under way is given up (a wait would run to its timeout, a world verb hang on)
        const at::Step& st = r.sc.steps[r.step];
        if (r.worldOwner == 1) {
            r.worldPending = false;
            r.worldOwner = 0;
        }
        if (st.verb == at::Verb::Pad) inputScriptStop();
        r.failWhy = "given up (fatal)";
        endStep(r, st, false);
    }
}

}  // namespace

bool autotestBoot() {
    char path[256];
    fsPath(path, sizeof(path), "autotest.txt");
    const SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return false;
    std::string text;
    char buf[1024];
    int n;
    while ((n = sceIoRead(fd, buf, sizeof(buf))) > 0 && text.size() < 64 * 1024) text.append(buf, static_cast<size_t>(n));
    sceIoClose(fd);
    // PSP (public safe): the server this run would use (the script's own "set server=" first) must be a dev
    // server - else nothing of the script applies (not even its "set" lines) and the game starts as a normal one.
    {
        const at::Script probe = at::parse(text);
        const char* server = nullptr;
        for (const auto& kv : probe.sets)
            if (kv.first == "server") server = kv.second.c_str();
        publicSafeBoot(server);
        if (!devToolsAllowed()) {
            refuse("boot");
            return false;
        }
    }
    g = new Runner;
    Runner& r = *g;
    r.sc = at::parse(text);  // the text itself is not kept (the parsed steps are)
    std::snprintf(r.txtPath, sizeof(r.txtPath), "%s", path);
    fsPath(r.donePath, sizeof(r.donePath), "autotest.done");
    fsPath(r.reportPath, sizeof(r.reportPath), "autotest_report.txt");
    for (const auto& kv : r.sc.sets) {
        configOverride(kv.first.c_str(), kv.second);
        PSP_LOGI(tag::Sys, "autotest_set key=%s", kv.first.c_str());  // values not logged (a password could be one)
    }
    for (const std::string& e : r.sc.errors) PSP_LOGW(tag::Sys, "autotest_parse_error %s", e.c_str());
    r.seenHit.assign(r.sc.seen.size(), 0);
    r.waitHitMs.assign(r.sc.steps.size(), 0u);
    logSetListener(&listener);
    inputFilterStartSelect(true);
    PSP_LOGI(tag::Sys, "autotest_start steps=%u blocks=%u sets=%u errors=%u sleep_s=%u pad_s=%u",
             static_cast<unsigned>(r.sc.steps.size()), static_cast<unsigned>(r.sc.blocks.size()),
             static_cast<unsigned>(r.sc.sets.size()), static_cast<unsigned>(r.sc.errors.size()),
             static_cast<unsigned>(r.sc.sleepMs / 1000u), static_cast<unsigned>(r.sc.padMs / 1000u));
    reportWrite(r, true, "# WoWee PSP autotest report - build %d, %u blocks, %u steps (autotest.txt)\n",
                WOWEE_PSP_BUILD, static_cast<unsigned>(r.sc.blocks.size()), static_cast<unsigned>(r.sc.steps.size()));
    for (const std::string& e : r.sc.errors) reportWrite(r, false, "parse error: %s\n", e.c_str());
    // The listener runs from here on (boot lines for "seen"); the steps from the first frame of the main loop.
    r.running = true;
    return true;
}

bool autotestActive() { return g && (!g->finished || g->summary); }

void autotestTick() {
    Runner* rp = g;
    if (!rp) return;
    Runner& r = *rp;
    const uint32_t now = logUptimeMs();
    if (r.startMs == 0) {
        r.startMs = now | 1u;
        r.hintUntil = now + kHintMs;
        r.ru = localeActive() == "ruRU";
    }
    // PSP: a public server seen during the run (the realm's address) ends it at once - no step more.
    if (!r.finished && publicSafeLocked()) {
        refuse("run");
        inputScriptStop();
        finishRun(r, "public_server");
        r.summary = false;  // the one-line notice instead of the summary panel
        inputFilterStartSelect(false);
        return;
    }
    if (inputTakeStartSelectChord() && !r.finished) togglePause(r);
    if (r.finished) {
        if (r.summary && (inputState().pressed & PSP_CTRL_CROSS)) {
            r.summary = false;
            inputFilterStartSelect(false);  // START and SELECT reach the game without delay again
            PSP_LOGI(tag::Sys, "autotest_summary closed");
        }
        return;
    }
    if (r.paused) return;
    checkFatal(r);
    for (int guard = 0; guard < 64 && !r.finished && !r.paused; ++guard) {
        if (r.reviveNeeded) {
            if (!r.worldPending && r.worldOwner != 2) {
                requestWorld(r, "say .revive", 2);
                return;
            }
            if (r.worldOwner == 2 && r.worldDone) {
                r.worldOwner = 0;
                r.reviveNeeded = false;
                PSP_LOGI(tag::Sys, "autotest_revive ok=%d", r.worldOk ? 1 : 0);
            } else {
                return;
            }
        }
        if (r.step >= r.sc.steps.size()) {
            finishRun(r, "steps_done");
            return;
        }
        const at::Step& st = r.sc.steps[r.step];
        if (!r.stepStarted) {
            if (r.blockSkipping && st.verb != at::Verb::Block && st.verb != at::Verb::End && !st.always) {
                PSP_LOGI(tag::Sys, "autotest_step n=%u verb=%s ok=skip line=%d", static_cast<unsigned>(r.step + 1),
                         at::verbName(st.verb), st.line);
                ++r.step;
                continue;
            }
            beginStep(r);
        }
        const int res = runStep(r, st);
        if (res == 0) return;
        endStep(r, st, res > 0);
        if (st.verb == at::Verb::End) {
            finishRun(r, "end");
            return;
        }
    }
}

void autotestServiceWorld(AutotestWorldExec exec, void* ctx) {
    Runner* r = g;
    if (!r || !exec || !r->worldPending || r->paused || r->finished) return;
    if (!devToolsAllowed()) return;  // PSP: no verb reaches a world that is not a dev server's
    r->worldPending = false;
    r->worldOk = exec(ctx, r->worldLine);
    r->worldDone = true;
}

const char* autotestRefusedNotice() {
    if (!g_refusedUntil) return nullptr;
    if (static_cast<int32_t>(g_refusedUntil - logUptimeMs()) <= 0) {
        g_refusedUntil = 0;
        return nullptr;
    }
    return "refused";
}

const AutotestView* autotestView() {
    Runner* rp = g;
    if (!rp || rp->startMs == 0) return nullptr;
    Runner& r = *rp;
    if (r.finished && !r.summary) return nullptr;
    AutotestView& v = r.view;
    const uint32_t now = logUptimeMs();
    v.paused = r.paused;
    v.finished = r.finished;
    v.pauseHint = r.paused || static_cast<int32_t>(r.hintUntil - now) > 0;
    v.blocks = static_cast<int>(r.sc.blocks.size());
    v.steps = static_cast<int>(r.sc.steps.size());
    v.step = static_cast<int>(r.step < r.sc.steps.size() ? r.step + 1 : r.sc.steps.size());
    v.elapsedMs = (r.finished ? r.endMs : now) - r.startMs;
    const int b = r.block;
    v.block = b >= 0 ? b + 1 : static_cast<int>(r.results.size());
    v.blockCaption = b >= 0 ? caption(r, blockOf(r, b).ru, blockOf(r, b).en) : "";
    v.lookCaption = r.look >= 0 ? caption(r, r.sc.steps[static_cast<size_t>(r.look)].ru, r.sc.steps[static_cast<size_t>(r.look)].en)
                                : "";
    v.stepVerb = r.step < r.sc.steps.size() ? at::verbName(r.sc.steps[r.step].verb) : "";
    v.resultCount = static_cast<int>(r.results.size());
    v.passCount = r.pass;
    v.failCount = r.fail;
    return &v;
}

const char* AutotestView::resultId(int i) const {
    return g && i >= 0 && i < static_cast<int>(g->results.size()) ? blockOf(*g, g->results[static_cast<size_t>(i)].block).id.c_str() : "";
}
const char* AutotestView::resultCaption(int i) const {
    if (!g || i < 0 || i >= static_cast<int>(g->results.size())) return "";
    const at::Block& b = blockOf(*g, g->results[static_cast<size_t>(i)].block);
    return caption(*g, b.ru, b.en);
}
bool AutotestView::resultPass(int i) const {
    return g && i >= 0 && i < static_cast<int>(g->results.size()) && g->results[static_cast<size_t>(i)].pass;
}
uint32_t AutotestView::resultMs(int i) const {
    return g && i >= 0 && i < static_cast<int>(g->results.size()) ? g->results[static_cast<size_t>(i)].ms : 0u;
}

}  // namespace wowee::platform::psp
