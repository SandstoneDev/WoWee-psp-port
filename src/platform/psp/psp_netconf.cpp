#include "platform/psp/psp_netconf.hpp"

#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_osk.hpp"

#include <psputility.h>
#include <psputility_netconf.h>
#include <psputility_sysparam.h>
#include <pspsysmem.h>

#include <cstring>

namespace wowee::platform::psp {

namespace {

// The dialog reads these while it runs: they must outlive netconfBegin().
pspUtilityNetconfData g_data;
pspUtilityNetconfAdhoc g_adhoc;  // unused by CONNECTAP, but the field gets a valid (zeroed) block

bool g_active = false;
bool g_shutdownSent = false;
bool g_finished = false;
int g_result = 0;

} // namespace

bool netconfBegin() {
    if (g_active || oskActive()) return false;  // one system dialog at a time
    // Only start from NONE (a dialog left mid-shutdown refuses the next InitStart).
    const int status = sceUtilityNetconfGetStatus();
    if (status > 0) {
        PSP_LOGW(tag::Net, "netconf_busy status=%d", status);
        logFlush();
        return false;
    }
    std::memset(&g_adhoc, 0, sizeof(g_adhoc));
    std::memset(&g_data, 0, sizeof(g_data));
    g_data.base.size = sizeof(g_data);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &g_data.base.language);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_BUTTON_SWAP, &g_data.base.buttonSwap);
    // Dialog threads: the same priorities as the keyboard (psp_osk.cpp), all below the main thread (0x20).
    g_data.base.graphicsThread = 17;
    g_data.base.accessThread = 19;
    g_data.base.fontThread = 18;
    g_data.base.soundThread = 16;
    g_data.action = PSP_NETCONF_ACTION_CONNECTAP;
    g_data.adhocparam = &g_adhoc;
    g_data.hotspot = 0;  // no "Internet Browser: Start" (captive portal) connections
    g_data.wifisp = 0;

    // Like the keyboard, the dialog allocates from the user partition after InitStart returns 0.
    const unsigned freeKb = static_cast<unsigned>(sceKernelTotalFreeMemSize() / 1024);
    const unsigned blockKb = static_cast<unsigned>(sceKernelMaxFreeMemSize() / 1024);
    const int r = sceUtilityNetconfInitStart(&g_data);
    PSP_LOGI(tag::Net, "netconf_begin r=%08X lang=%d swap=%d part_free_kb=%u part_block_kb=%u",
             static_cast<unsigned>(r), g_data.base.language, g_data.base.buttonSwap, freeKb, blockKb);
    logFlush();
    if (r < 0) return false;
    g_active = true;
    g_shutdownSent = false;
    g_finished = false;
    g_result = 0;
    return true;
}

bool netconfActive() { return g_active; }

void netconfAfterGeSync() {
    if (!g_active) return;
    // Status values as in psp_osk.cpp (pspdev names shifted against the firmware's):
    //   2 running  -> Update(1) every frame
    //   3 finished -> ShutdownStart(), exactly once
    //   4 closing  -> keep pumping
    //   0 none     -> closed: read the result (before the shutdown: not up yet)
    const int st = sceUtilityNetconfGetStatus();
    switch (st) {
        case 2:
            sceUtilityNetconfUpdate(1);
            break;
        case 3:
            if (!g_shutdownSent) {
                g_shutdownSent = true;
                sceUtilityNetconfShutdownStart();
            }
            break;
        case 0:
            if (!g_shutdownSent) break;
            g_active = false;
            g_finished = true;
            g_result = g_data.base.result;
            PSP_LOGI(tag::Net, "netconf_result base=%d hotspot_connected=%d", g_result, g_data.hotspot_connected);
            logFlush();
            break;
        default:
            if (st < 0) {  // the utility lost the dialog: report it instead of waiting forever
                PSP_LOGE(tag::Net, "netconf_status err=%08X", static_cast<unsigned>(st));
                g_active = false;
                g_finished = true;
                g_result = st;
                logFlush();
            }
            break;  // 1 init / 4 closing: keep pumping every frame
    }
}

bool netconfFinished(int& result) {
    if (!g_finished) return false;
    g_finished = false;
    result = g_result;
    return true;
}

} // namespace wowee::platform::psp
