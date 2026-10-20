// WoWee PSP entry point - Milestone 1 (boot skeleton).
//
// Boots, identifies the console, opens session.log, reports memory, checks
// the filesystem, runs the upstream-reuse self-test, brings up the GE and the
// controller, then runs a test screen until HOME -> Exit.
// No game code path runs yet; see docs/PSP_PORT_ARCHITECTURE.md for the plan.

#include "rendering/psp/psp_terrain.hpp"  // TerrainRenderer::poolInit
#include "platform/psp/psp_audio.hpp"
#include "platform/psp/psp_autotest.hpp"  // the on-device autopilot
#include "platform/psp/psp_public_safe.hpp"  // PSP: the debug aids only on a dev server
#include "platform/psp/psp_bigheap.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_prof.hpp"
#include "ui/psp/ui_prof_overlay.hpp"
#include "ui/psp/ui_autotest_overlay.hpp"  // the autopilot's band and summary
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_input.hpp"
#include "platform/psp/psp_locale.hpp"  // the client's languages
#include "platform/psp/psp_strings.hpp"  // interface strings
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_login_store.hpp"
#include "platform/psp/psp_net.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_system.hpp"
#include "platform/psp/psp_splash.hpp"  // the community QR splash
#include "sdbg.h"  // SandstoneDebug crash catcher
#include "platform/psp/psp_world_session.hpp"
#include "platform/psp/reuse_probe.hpp"
#include "ui/psp/ui_kit.hpp"
#include "ui/psp/ui_login_screen.hpp"
#include "ui/psp/ui_renderer.hpp"

#include <malloc.h>

#include <pspctrl.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspkernel.h>
#include <pspthreadman.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>

PSP_MODULE_INFO("WoWeePSP", PSP_MODULE_USER, 0, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(256);
// Heap = the whole user partition minus the threshold. The threshold stays in
// the partition for things that do not come from the heap: thread stacks,
// network modules, system dialogs (netconf/OSK need 1-2 MB contiguous).
PSP_HEAP_SIZE_KB(-1);
// 4096 -> 2560 KB. The PSP-3000's heap was only 42.8 MB (the
// emulator's 47.9 MB) and the world ran out of it; at world entry the
// partition outside the heap had 3.5 MB free with 4 MB reserved, so 2.5 MB
// still leaves ~2 MB for the netconf / OSK dialogs (they need 1-2 MB).
PSP_HEAP_THRESHOLD_SIZE_KB(2560);

using namespace wowee::platform::psp;

// the program image's ends (the linker's symbols) - its size is heap the PSP lacks.
extern "C" char __executable_start[];
extern "C" char _end[];

namespace {

// program image + boot heap on the PSP-3000 (KB) - hardware logs of builds 82..113
// (boot_done heap_used + largest against the image of the same code, +-100 KB): 44 303 + ~4 750 at 82, 43 099 +
// ~6 000 at 110, 42 332 + ~6 700 at 113. The heap_budget line at boot measures it again (sum_kb on the device).
constexpr int kHwHeapPlusImageKb = 49050;

struct TestScreenState {
    uint32_t frame = 0;
    float fps = 0.0f;
    uint32_t frameUs = 0;
    ReuseProbeResult reuse;
    bool dataDirPresent = false;
    char banner[96] = {};
};

struct ColorVert {
    uint32_t color;
    float x, y, z;
};

void drawTestScene(const TestScreenState& st, const PadState& pad) {
    // 3D: one spinning triangle through sceGum (checks the transform path).
    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadIdentity();
    sceGumPerspective(60.0f, static_cast<float>(kScreenW) / kScreenH, 0.5f, 100.0f);
    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    ScePspFVector3 pos = {pad.ax * 1.5f, -pad.ay * 1.0f, -3.0f};
    sceGumTranslate(&pos);
    sceGumRotateY(st.frame * 0.03f);

    auto* tri = static_cast<ColorVert*>(sceGuGetMemory(3 * sizeof(ColorVert)));
    tri[0] = {0xFF0000FFu, 0.0f, 1.0f, 0.0f};    // red   (0xAABBGGRR)
    tri[1] = {0xFF00FF00u, -1.0f, -0.8f, 0.0f};  // green
    tri[2] = {0xFFFF0000u, 1.0f, -0.8f, 0.0f};   // blue
    sceGuDisable(GU_TEXTURE_2D);
    sceGumDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, 3, nullptr, tri);

    // 2D: status bar + one box per face button held.
    drawRect(0, 0, kScreenW, 12, 0xC0301808u);
    const uint32_t buttons[] = {PSP_CTRL_CROSS, PSP_CTRL_CIRCLE, PSP_CTRL_SQUARE, PSP_CTRL_TRIANGLE,
                                PSP_CTRL_LTRIGGER, PSP_CTRL_RTRIGGER, PSP_CTRL_START, PSP_CTRL_SELECT,
                                PSP_CTRL_UP, PSP_CTRL_DOWN, PSP_CTRL_LEFT, PSP_CTRL_RIGHT};
    for (int i = 0; i < 12; ++i) {
        bool on = (pad.held & buttons[i]) != 0;
        drawRect(8 + i * 22, 248, 18, 16, on ? 0xFF20E0FFu : 0xFF404040u);
    }
    // Reuse self-test verdict as a colour block, readable without the log.
    drawRect(440, 248, 32, 16, st.reuse.failed == 0 ? 0xFF20C020u : 0xFF2020E0u);
}

void overlay(void* user) {
    const auto& st = *static_cast<const TestScreenState*>(user);
    const auto& si = systemInfo();
    textAt(0, 0, 0xFFFFFFFFu, "WoWee PSP %s  build %d", WOWEE_PSP_RELEASE, WOWEE_PSP_BUILD);
    textAt(0, 2, 0xFFC0C0C0u, "%s", st.banner);
    textAt(0, 3, 0xFFC0C0C0u, "partition free %u KB  max block %u KB",
           static_cast<unsigned>(si.partFreeKB), static_cast<unsigned>(si.partMaxBlockKB));
    textAt(0, 4, 0xFFC0C0C0u, "fps %.1f  frame %u us  ge wait %u us", st.fps,
           static_cast<unsigned>(st.frameUs), static_cast<unsigned>(lastGeWaitUs()));
    textAt(0, 5, st.reuse.failed ? 0xFF4040FFu : 0xFF40FF40u, "upstream reuse probe: %d/%d ok%s%s",
           st.reuse.checks - st.reuse.failed, st.reuse.checks,
           st.reuse.firstFailure ? "  first fail: " : "", st.reuse.firstFailure ? st.reuse.firstFailure : "");
    textAt(0, 6, 0xFFC0C0C0u, "data dir: %s", st.dataDirPresent ? "present" : "absent (ok for M1)");
    const auto& pad = inputState();
    textAt(0, 8, 0xFFC0C0C0u, "pad %08X  nub %3u,%3u", static_cast<unsigned>(pad.held),
           static_cast<unsigned>(pad.rawX), static_cast<unsigned>(pad.rawY));
    textAt(0, 29, 0xFF80FFFFu, "HOME to exit");
}

} // namespace

int main(int argc, char* argv[]) {
    systemStartExitCallback();
    // Upstream WorldSocket pumps on its own std::thread unless told otherwise;
    // the PSP build keeps networking on the main thread (update() pumps).
    setenv("WOWEE_NET_ASYNC_PUMP", "0", 1);

    fsInit(argc > 0 ? argv[0] : nullptr);
    char path[256];
    fsPath(path, sizeof(path), "session.log");
    bool logOk = logOpen(path);
    logSetSync(true);  // boot: every line reaches the card before the next step

    // Crash catcher (SandstoneDebug): a kernel
    // module catches CPU exceptions on the device and writes CRASH.TXT (+
    // ARMED.TXT) next to the EBOOT - registers, the main thread's stack chain
    // and the tail of this log. Map the addresses with
    // psp/sandstone-debug/tools/symbolicate.py CRASH.TXT psp/bin/wowee_psp.map
    // or psp-addr2line -e psp/bin/wowee_psp.debug.elf. Needs a CFW (ARK).
    {
        static char prxPath[256], reportPath[256];
        fsPath(prxPath, sizeof(prxPath), "sdbg.prx");
        fsPath(reportPath, sizeof(reportPath), "CRASH.TXT");
        sdbg_cfg cfg{};
        cfg.prx_path = prxPath;
        cfg.report_path = reportPath;
        cfg.build = WOWEE_PSP_BUILD;
        cfg.enable = 1;
        const int armed = sdbg_init(&cfg);
        PSP_LOGI(tag::Boot, "sdbg armed=%d prx=%s report=%s", armed, prxPath, reportPath);
    }

    const SystemInfo& si = systemProbe();
    TestScreenState st;
    systemBanner(st.banner, sizeof(st.banner));

    PSP_LOGI(tag::Boot, "wowee_psp release=%s build=%d gcc=%s", WOWEE_PSP_RELEASE, WOWEE_PSP_BUILD, __VERSION__);
    PSP_LOGI(tag::Sys, "%s", st.banner);
    PSP_LOGI(tag::Mem, "part_free_kb=%u part_max_block_kb=%u", static_cast<unsigned>(si.partFreeKB),
             static_cast<unsigned>(si.partMaxBlockKB));

    // Heap probe that touches what it claims: allocate, write every page, free.
    {
        const size_t probeBytes = 8u * 1024u * 1024u;
        auto* p = static_cast<volatile uint8_t*>(std::malloc(probeBytes));
        const bool ok = p != nullptr;
        if (ok) {
            for (size_t i = 0; i < probeBytes; i += 4096) p[i] = static_cast<uint8_t>(i >> 12);
            std::free(const_cast<uint8_t*>(p));
        }
        PSP_LOGI(tag::Mem, "heap_touch_8mb=%s", ok ? "ok" : "fail");
    }

    PSP_LOGI(tag::Fs, "base=%s log=%s", fsBaseDir(), logOk ? "ok" : "fail");
    fsPath(path, sizeof(path), "data");
    st.dataDirPresent = fsIsDir(path);
    PSP_LOGI(tag::Fs, "data_dir=%s", st.dataDirPresent ? "present" : "absent");

    st.reuse = runReuseProbe();
    PSP_LOGI(tag::Reuse, "checks=%d failed=%d", st.reuse.checks, st.reuse.failed);

    inputInit();
    PSP_LOGI(tag::Ctrl, "mode=analog");

    displayInit();
    prof::init();
    PSP_LOGI(tag::Gfx, "psm=5551 fb=2x480x272 depth=16 list_kb=256");

    // Settings on the card (server, account, Wi-Fi profile).
    fsPath(path, sizeof(path), "wowee.cfg");
    const bool cfgOk = configLoad(path);
    // autotest.txt next to the EBOOT - the autopilot; its "set" lines are this run's cfg values
    // (before anything reads a key). Without the file: one failed open, nothing else.
    autotestBoot();
    // PSP: the dev-server decision of the debug / automation aids - devhosts= and an address in server= now
    // (the autopilot already made it with its own server= when autotest.txt is there: then this is a no-op).
    publicSafeBoot();
#ifdef WOWEE_PSP_SPLASH
    // the community QR splash (psp_splash.hpp) - before anything loads; skipped by the automated runs.
    // HOME -> Exit on it: the same teardown as the end of main() (nothing else has started yet).
    if (!splashRun()) {
        PSP_LOGI(tag::Exit, "reason=home_on_splash");
        displayShutdown();
        logClose();
        systemExit();
    }
#endif
    // the running language (wowee.cfg locale, data/locales.txt) before the first pak opens -
    // from here on fsPath() hands out data/locale/<xxYY>/<name> for the paks that language has its own copy of.
    localeInit();
    // the interface's strings - the client's from strings.pak (that language's, then the primary's),
    // the port's own en / ru - before any screen draws text.
    stringsInit();
    // the emulator's heap is ~5 MB larger than the PSP-3000's (42.3 MB
    // free at boot on hardware) - out-of-memory hid in PPSSPP. Hold
    // the difference on the emulator so it runs out where the device does
    // (wowee.cfg emuheapkb = the device's free heap at boot, 0 = off).
    // emuheapkb absent = auto - the device's heap follows the code: the boot heap +
    // the program image (text + data + bss, __executable_start.._end) stayed at ~49 050 KB on the PSP-3000 over many
    // builds (boot_done heap_used + largest fell from 44 303 KB to 42 332 KB as the code grew) - every KB of code is
    // a KB of heap less. A fixed 43 840 left the emulator ~2.5 MB richer than the device.
    // 0 = off, > 0 = that many KB as before. The line heap_budget (every boot) has the numbers to check it against.
    const int imageKb = static_cast<int>((reinterpret_cast<uintptr_t>(_end) -
                                          reinterpret_cast<uintptr_t>(__executable_start)) / 1024u);
    uint32_t ballastHeldKb = 0;  // the big-block heap's small_used_kb leaves it out
    if (si.emulator) {
        int deviceKb = configGetInt("emuheapkb", -1);
        if (deviceKb < 0) deviceKb = kHwHeapPlusImageKb - imageKb;  // auto
        uint32_t freeKb = 0;
        for (uint32_t lo = 0, hi = 64u * 1024u; hi - lo > 64u;) {  // largest block (volatile probe)
            const uint32_t mid = (lo + hi) / 2u;
            void* volatile p = std::malloc(static_cast<size_t>(mid) * 1024u);
            if (p) {
                static_cast<volatile char*>(p)[0] = 1;
                std::free(p);
                lo = mid;
            } else {
                hi = mid;
            }
            freeKb = lo;
        }
        if (deviceKb > 0 && freeKb > static_cast<uint32_t>(deviceKb)) {
            const uint32_t ballastKb = freeKb - static_cast<uint32_t>(deviceKb);
            void* volatile ballast = std::malloc(static_cast<size_t>(ballastKb) * 1024u);
            if (ballast) {
                static_cast<volatile char*>(ballast)[0] = 1;  // kept for the whole run
                ballastHeldKb = ballastKb;
            }
            PSP_LOGI(tag::Mem, "emu_ballast kb=%u (emulator free %u KB -> device %d KB) image_kb=%d auto=%d",
                     static_cast<unsigned>(ballastKb), static_cast<unsigned>(freeKb), deviceKb, imageKb,
                     configGetInt("emuheapkb", -1) < 0 ? 1 : 0);
        }
    }
    // the big-block heap region - taken now, after the ballast (the emulator and the PSP-3000 carve
    // it from the same heap) and before the audio / loader / network threads allocate (wowee.cfg bigheap=0: off).
    bigheapInit(ballastHeldKb);
    // the terrain pool - one block for the whole run, taken while the heap is empty
    // (chunk textures, tiles, the streamer's read buffers: never newlib again, so neither fragmenting nor starved).
    wowee::rendering::psp::TerrainRenderer::poolInit();
    wowee::ui::psp::LoginSettings login;
    login.server = configGet("server");
    login.port = static_cast<uint16_t>(configGetInt("port", 3724));
    login.account = configGet("account");
    login.password = configGet("password");
    login.credentialsInCfg = !login.account.empty() || !login.password.empty();
    // PSP: no password in wowee.cfg - the login saved outside the game folder (ms0:/WOWEE), for this
    // server; a different account in wowee.cfg wins (then its password is typed on the login screen).
    if (login.password.empty()) {
        StoredLogin saved;
        auto sameName = [](const std::string& a, const std::string& b) {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
                       return std::toupper(x) == std::toupper(y);
                   });
        };
        if (loginStoreGet(login.server, saved) && (login.account.empty() || sameName(login.account, saved.account))) {
            login.account = saved.account;
            login.authHash = saved.authHash;
        }
    }
    login.locale = localeActive();  // the logon challenge's locale = the running language (server texts)
    login.netProfile = configGetInt("netprofile", 0);
    login.netSsid = configGet("netssid");
    login.autoLogin = configGetInt("autologin", 0) != 0;
    login.autoOsk = configGetInt("autoosk", 0) != 0;
    // PSP: the login opens the system connection dialog (netdialog=1, the default); automated runs (the
    // autopilot, autologin=1, an autopad script) have nobody to answer it - they keep the saved-profile
    // connection (netprofile / netssid), the same rule as the splash.
    login.netDialog = configGetInt("netdialog", 1) != 0;
    {
        const bool autotest = autotestActive();
        const bool autopad = !configGet("autopad", "").empty();
        if (login.netDialog && (autotest || login.autoLogin || autopad)) {
            login.netDialog = false;
            PSP_LOGI(tag::Net, "netconf skipped (automation) autotest=%d autologin=%d autopad=%d", autotest ? 1 : 0,
                     login.autoLogin ? 1 : 0, autopad ? 1 : 0);
        }
    }
    PSP_LOGI(tag::Fs, "config=%s server=%s port=%u account=%s netprofile=%d netssid=%s netdialog=%d",
             cfgOk ? "ok" : "absent", login.server.c_str(), static_cast<unsigned>(login.port), login.account.c_str(),
             login.netProfile, login.netSsid.c_str(), login.netDialog ? 1 : 0);
    // Frame limit for every screen (the world's settings menu changes it live).
    displaySetFrameInterval(configGetInt("framecap", 60) <= 30 ? 2 : 1);
    // Sound (M12): threads + channels once per session (psp_audio.hpp).
    if (configGetInt("audio", 1)) audioInit();

    // Protocol tables (expansion profile + 3.3.5a opcodes) from upstream's
    // Data/expansions/wotlk, copied next to the EBOOT by deploy_psp.ps1.
    {
        std::string dataRoot = fsBaseDirPosix() + "Data";  // see fsBaseDirPosix()
        uint64_t t0 = sceKernelGetSystemTimeWide();
        bool ok = wowee::platform::psp::WorldSession::initTables(dataRoot, "wotlk");
        PSP_LOGI(tag::Net, "world_tables=%s ms=%u", ok ? "ok" : "missing",
                 static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u));
    }

    // UI from the original interface art (data/ui.pak).
    Pak uiPak;
    wowee::ui::psp::UiKit kit;
    wowee::ui::psp::UiRenderer ui;
    fsPath(path, sizeof(path), "data/ui.pak");
    bool uiOk = false;
    if (uiPak.open(path)) {
        uint64_t t0 = sceKernelGetSystemTimeWide();
        uiOk = kit.load(uiPak);
        uint32_t ms = static_cast<uint32_t>((sceKernelGetSystemTimeWide() - t0) / 1000u);
        PSP_LOGI(tag::Gfx, "ui_pak entries=%u loaded_kb=%u missing=%d ms=%u ok=%d",
                 static_cast<unsigned>(uiPak.count()), static_cast<unsigned>(kit.loadedBytes() / 1024),
                 kit.missing(), static_cast<unsigned>(ms), uiOk ? 1 : 0);
        uiPak.close();  // everything needed is resident now
        // the port's own strings are Russian until the string table - a language whose fonts
        // have no Cyrillic (the enUS client's FRIZQT__.TTF: every Russian label a row of boxes) takes the fonts of
        // an installed language's ui.pak that has (the same typefaces; the art stays this language's).
        if (uiOk && !kit.font(wowee::ui::psp::kFontNormal).coversCyrillic()) {
            bool taken = false;
            for (const std::string& code : localeInstalled()) {
                if (code == localeActive() || !localePakPath(code, "ui.pak", path, sizeof(path))) continue;
                Pak other;
                if (other.open(path) && kit.reloadFonts(other)) {
                    localeSetFontPak(path);
                    taken = true;
                }
                other.close();
                if (taken) break;
            }
            PSP_LOGI(tag::Gfx, "ui_fonts no_cyrillic=1 from=%s", taken ? localeFontPak() : "-");
        }
    } else {
        PSP_LOGW(tag::Gfx, "ui_pak=absent fallback=m1_test_screen");
    }
    wowee::ui::psp::LoginScreen screen(kit, login);
    struct mallinfo mi = mallinfo();
    PSP_LOGI(tag::Mem, "heap_arena_kb=%u heap_used_kb=%u", static_cast<unsigned>(mi.arena / 1024),
             static_cast<unsigned>(heapUsedKb()));  // both pools of the big-block heap
    logFlush();

    installOomHandler();  // a failed new logs the heap before the end
    logMemory("boot_done");
    {
        // the heap budget's anchor - program image + boot heap (used + largest free
        // block) is the constant kHwHeapPlusImageKb on the device (the emulator: its ballast makes it so).
        struct mallinfo bm = mallinfo();
        // newlib's own numbers - the big-block region counts as used, its free space not as largest.
        const uint32_t usedKb = static_cast<uint32_t>(bm.uordblks / 1024);
        const uint32_t largestKb = heapSmallLargestKb();
        PSP_LOGI(tag::Mem, "heap_budget image_kb=%d heap_kb=%u sum_kb=%u const_kb=%d emu=%d", imageKb,
                 static_cast<unsigned>(usedKb + largestKb), static_cast<unsigned>(usedKb + largestKb) + imageKb,
                 kHwHeapPlusImageKb, si.emulator ? 1 : 0);
    }
    PSP_LOGI(tag::Boot, "boot_done enter_main_loop");
    logSetSync(false);
    uint64_t last = sceKernelGetSystemTimeWide();
    uint64_t fpsWindowStart = last;
    uint32_t fpsFrames = 0;

    while (systemRunning()) {
        const PadState& pad = inputUpdate();
        autotestTick();  // the autopilot (it takes a START+SELECT chord as its pause)
        // Profiler overlay / capture: the world's debug window (SELECT is
        // the cursor mode there).
        // PSP: no START+SELECT exit any more - too easy to press by mistake; the game exits by HOME
        // (the firmware's exit callback) or the in-game menu's exit button.

        if (uiOk) {
            const float dt = st.frameUs > 0 && st.frameUs < 200000u ? st.frameUs / 1e6f : 1.0f / 60.0f;
            {
                PROF_SCOPE("update");
                screen.update(pad, dt);
            }
            if (screen.wantsExit()) {
                PSP_LOGI(tag::Exit, "reason=ui_exit_button");
                systemRequestExit();
            }
            wowee::ui::psp::FrameStats fs;
            fs.fps = st.fps;
            fs.frameUs = st.frameUs;
            fs.geWaitUs = lastGeWaitUs();
            fs.build = WOWEE_PSP_BUILD;
            frameBegin(0xFF000000u);
            {
                PROF_SCOPE("draw");
                screen.draw(ui, fs);
            }
            wowee::ui::psp::drawAutotestOverlay(ui, kit);  // the autopilot's band / summary
            if (prof::overlayOn()) {
                PROF_GE_PASS("prof");
                wowee::ui::psp::drawProfOverlay(ui, kit);
            }
            frameEnd(nullptr, nullptr);
        } else {
            frameBegin(0xFF402010u);
            drawTestScene(st, pad);
            frameEnd(overlay, &st);
        }

        uint64_t now = sceKernelGetSystemTimeWide();
        st.frameUs = static_cast<uint32_t>(now - last);
        last = now;
        ++st.frame;
        ++fpsFrames;
        if (now - fpsWindowStart >= 1000000u) {
            st.fps = fpsFrames * 1000000.0f / static_cast<float>(now - fpsWindowStart);
            fpsFrames = 0;
            fpsWindowStart = now;
        }
    }

    memWatchSummary();  // the session's peak heap use / smallest free block
    PSP_LOGI(tag::Exit, "frames=%u", static_cast<unsigned>(st.frame));
    netStop();
    kit.release();
    displayShutdown();
    // The restart button after a language change - the EBOOT once more, after the same teardown as an
    // exit (wowee.cfg relaunch=0: straight to the XMB). A refusal comes back at once: one line, then the XMB.
    if (localeRestartRequested() && configGetInt("relaunch", 1) != 0 && argc > 0 && argv[0]) {
        PSP_LOGI(tag::Exit, "relaunch path=%s", argv[0]);
        logClose();
        const int rc = systemRelaunch(argv[0]);
        fsPath(path, sizeof(path), "session.log");
        if (logReopen(path)) PSP_LOGW(tag::Exit, "relaunch refused rc=%08X -> xmb", static_cast<unsigned>(rc));
    }
    logClose();
    systemExit();
}
