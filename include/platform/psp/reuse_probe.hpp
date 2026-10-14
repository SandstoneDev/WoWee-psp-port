// Boot-time self-test of upstream WoWee code compiled unchanged for the PSP.
// Proves the reuse path (C++20, glm, libstdc++ on newlib) on the device, not
// only that it links. Each check has its own result so a failure names itself.
#pragma once

namespace wowee::platform::psp {

struct ReuseProbeResult {
    int checks = 0;
    int failed = 0;
    const char* firstFailure = nullptr;
};

// Runs every probe and logs one REUSE line per check.
ReuseProbeResult runReuseProbe();

} // namespace wowee::platform::psp
