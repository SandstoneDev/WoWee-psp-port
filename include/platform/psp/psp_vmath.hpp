// VFPU scalar math for hot paths (docs/PSP_PORT_ARCHITECTURE.md; the facts:
// PPSSPP's "VFPU math" write-up, ppsspp.org/blog/vfpu-math-re, PR #22343).
//
// What the hardware does (bit-exact model recovered by PPSSPP, tested over all
// 2^32 inputs; Sony: VFPU-Instruction_Manual p.358 vsin abs err < 4.8e-7 for
// |x| < 32, p.310 vrsq rel err < 7.3e-7, p.177 vlog2 abs err < 3.1e-5; latency
// 7 / pitch 1 cycle each, vrot.p 8/2). NOT trusted from the emulator: our
// PPSSPP 1.20.4 (JIT) returned vrsq exactly equal to 1/sqrt (the log's
// vmath_err rsqrt_rel=0) although it ships fp64's tables - the numbers are the
// PSP-3000's (log vmath_bench / vmath_err):
//   - vrcp / vrsq / vsqrt / vexp2 / vlog2 / vsin / vcos / vasin are a minimax
//     quadratic interpolator: 128 segments by the top 7 mantissa bits, the
//     result TRUNCATED to ~22 bits (float has 24) - biased toward zero, never
//     rounded. Fine for one-shot results (normalising a quaternion, a colour, a
//     rotation), wrong for anything accumulated frame after frame (yaw, timers,
//     integration) - keep those on the FPU.
//   - vsin / vcos / vasin work in quarter turns: 4.0 = one full circle (not
//     2 pi); Sony's manual: the argument falls apart at |x| >= 32 - reduce first.
//   - vrot = sin + cos in one instruction (a rotation pair).
//   - One VFPU instruction replaces newlib's software sinf/cosf (hundreds of
//     cycles) and the FPU's sqrt.s + div.s (28 + 28 cycles, FPU manual table 3-1).
// Rules:
//   - only from threads created with PSP_THREAD_ATTR_VFPU (the main thread has
//     it: main_psp.cpp PSP_MAIN_THREAD_ATTR); the streamer / audio threads do
//     NOT - never call these from them;
//   - registers: column 7 (S700..S733) only - Sony's libvfpu uses s000-s233
//     and guarantees s300-s733 (libvfpu-Overview p.3), libgum_vfpu / libgmo use
//     M0-M3, nothing of Sony's touches M6/M7; our libpspgum is the FPU build;
//   - vmath::enabled = wowee.cfg vfpumath (1 default, 0 = the FPU / libm path,
//     A/B on hardware); off the PSP (converters, host tests) it is always the
//     FPU / libm path, so baked data and PC checks never see VFPU results.
#pragma once

#include <cmath>
#include <cstdint>

namespace wowee::platform::psp::vmath {

inline bool enabled = true;  // wowee.cfg vfpumath

#if defined(__psp__)
#define WOWEE_VMATH_VFPU 1
#endif

// 1 / sqrt(x), x > 0 (vrsq; 0 -> +inf like the FPU path).
inline float rsqrt(float x) {
#ifdef WOWEE_VMATH_VFPU
    if (enabled) {
        float r;
        __asm__ volatile("mtv %1, S700\n vrsq.s S700, S700\n mfv %0, S700\n" : "=r"(r) : "r"(x));
        return r;
    }
#endif
    return 1.0f / std::sqrt(x);
}

// 1 / x (vrcp).
inline float rcp(float x) {
#ifdef WOWEE_VMATH_VFPU
    if (enabled) {
        float r;
        __asm__ volatile("mtv %1, S700\n vrcp.s S700, S700\n mfv %0, S700\n" : "=r"(r) : "r"(x));
        return r;
    }
#endif
    return 1.0f / x;
}

// sqrt(x) (vsqrt).
inline float sqrt(float x) {
#ifdef WOWEE_VMATH_VFPU
    if (enabled) {
        float r;
        __asm__ volatile("mtv %1, S700\n vsqrt.s S700, S700\n mfv %0, S700\n" : "=r"(r) : "r"(x));
        return r;
    }
#endif
    return std::sqrt(x);
}

// Radians into [-pi, pi] (the VFPU wants small quarter-turn arguments).
inline float wrapPi(float rad) {
    const float turns = rad * 0.15915494f;  // 1 / (2 pi)
    const float k = static_cast<float>(static_cast<int>(turns + (turns >= 0.0f ? 0.5f : -0.5f)));
    // Cody-Waite: 2 pi in two parts (6.28125 is exact in 8 bits), so k * 2pi
    // loses nothing for |k| up to ~2^15 (9.7e-5 error at 1500 rad before).
    return (rad - k * 6.28125f) - k * 1.9353071795864769e-3f;
}

// sin and cos of an angle in radians (vrot: one instruction for both).
inline void sincos(float rad, float& s, float& c) {
#ifdef WOWEE_VMATH_VFPU
    if (enabled) {
        const float a = wrapPi(rad);
        float rs, rc;
        __asm__ volatile(
            "mtv %2, S700\n"
            "vcst.s S701, VFPU_2_PI\n"   // radians -> quarter turns
            "vmul.s S700, S700, S701\n"
            "vrot.p C710, S700, [s, c]\n"
            "mfv %0, S710\n"
            "mfv %1, S711\n"
            : "=r"(rs), "=r"(rc)
            : "r"(a));
        s = rs;
        c = rc;
        return;
    }
#endif
    s = std::sin(rad);
    c = std::cos(rad);
}

inline float sin(float rad) {
    float s, c;
    sincos(rad, s, c);
    return s;
}

inline float cos(float rad) {
    float s, c;
    sincos(rad, s, c);
    return c;
}

// 2^x and log2(x) (x > 0); pow(b, e) = 2^(e log2 b), b > 0.
inline float exp2(float x) {
#ifdef WOWEE_VMATH_VFPU
    if (enabled) {
        float r;
        __asm__ volatile("mtv %1, S700\n vexp2.s S700, S700\n mfv %0, S700\n" : "=r"(r) : "r"(x));
        return r;
    }
#endif
    return std::exp2(x);
}

inline float log2(float x) {
#ifdef WOWEE_VMATH_VFPU
    if (enabled) {
        float r;
        __asm__ volatile("mtv %1, S700\n vlog2.s S700, S700\n mfv %0, S700\n" : "=r"(r) : "r"(x));
        return r;
    }
#endif
    return std::log2(x);
}

inline float pow(float b, float e) {
    return exp2(e * log2(b));
}

// Hardware A/B at world entry: FPU / libm vs VFPU timings and the largest
// differences (psp_vmath.cpp; log line vmath_bench). PSP only.
void bench();

}  // namespace wowee::platform::psp::vmath
