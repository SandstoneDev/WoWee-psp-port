// vmath::bench - the numbers only the hardware can give (the emulator
// matches the VFPU's results bit for bit, not its timings).
#include "platform/psp/psp_vmath.hpp"

#include "platform/psp/psp_log.hpp"

#include <pspkernel.h>

#include <algorithm>
#include <cmath>

namespace wowee::platform::psp::vmath {

void bench() {
    constexpr int kN = 4096;
    PSP_LOGI(tag::Gfx, "vmath_bench start");  // a crash inside shows as the last line
    const bool was = enabled;
    volatile float sink = 0.0f;
    auto timeIt = [&](auto&& fn) {
        const uint64_t t0 = sceKernelGetSystemTimeWide();
        float acc = 0.0f;
        for (int i = 0; i < kN; ++i) acc += fn(0.01f + static_cast<float>(i) * 0.37f);
        sink = acc;
        return static_cast<unsigned>(sceKernelGetSystemTimeWide() - t0);
    };
    enabled = false;
    const unsigned rsqFpu = timeIt([](float x) { return rsqrt(x); });
    const unsigned sinLibm = timeIt([](float x) { return sin(x); });
    const unsigned powLibm = timeIt([](float x) { return std::pow(x, 1.7f); });
    enabled = true;
    const unsigned rsqVfpu = timeIt([](float x) { return rsqrt(x); });
    const unsigned sinVfpu = timeIt([](float x) { return sin(x); });
    const unsigned powVfpu = timeIt([](float x) { return pow(x, 1.7f); });
    // Largest relative / absolute differences over the same inputs.
    float rsqErr = 0.0f, sinErr = 0.0f, powErr = 0.0f;
    for (int i = 0; i < kN; ++i) {
        const float x = 0.01f + static_cast<float>(i) * 0.37f;
        const float ref = 1.0f / std::sqrt(x);
        rsqErr = std::max(rsqErr, std::fabs(rsqrt(x) - ref) / ref);
        sinErr = std::max(sinErr, std::fabs(sin(x) - std::sin(x)));
        const float pr = std::pow(x, 1.7f);
        powErr = std::max(powErr, std::fabs(pow(x, 1.7f) - pr) / pr);
    }
    enabled = was;
    (void)sink;
    // Two lines: the log cuts a line at ~200 characters.
    PSP_LOGI(tag::Gfx, "vmath_bench n=%d rsqrt_us fpu=%u vfpu=%u sin_us libm=%u vfpu=%u pow_us libm=%u vfpu=%u", kN,
             rsqFpu, rsqVfpu, sinLibm, sinVfpu, powLibm, powVfpu);
    PSP_LOGI(tag::Gfx, "vmath_err rsqrt_rel=%.2e sin_abs=%.2e pow_rel=%.2e on=%d", static_cast<double>(rsqErr),
             static_cast<double>(sinErr), static_cast<double>(powErr), enabled ? 1 : 0);
}

}  // namespace wowee::platform::psp::vmath
