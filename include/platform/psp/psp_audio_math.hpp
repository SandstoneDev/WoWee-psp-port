#pragma once
#include <cstdint>
namespace wowee::platform::psp {
// Overlap the tail with the beginning, then resume after the overlapped head.
// A 100 ms crossfade avoids a discontinuity without per-sample division.
inline uint32_t loopCrossfadeFrames(uint32_t frames,uint32_t rate) {
    const uint32_t quarter=frames/4, tenth=rate/10;
    return quarter<tenth ? quarter : tenth;
}
inline int loopCrossfadePcm(int tail,int head,uint32_t offset,uint32_t scale) {
    const uint32_t weight=(offset*scale)>>9; // scale = floor(2^24 / overlap)
    return tail+((head-tail)*static_cast<int>(weight>32768 ? 32768 : weight)>>15);
}
inline int interpolatePcm(int a,int b,uint32_t fraction) {
    // 15-bit fraction keeps the product within signed 32-bit on Allegrex.
    return a+((b-a)*static_cast<int>(fraction>>1)>>15);
}
// SFX and music use separate hardware channels, so reserve headroom for their sum.
// One division per block replaces expensive per-sample 64-bit soft clipping.
inline int mixBlockGain(int32_t peak, int previous) {
    constexpr int ceiling = 19000;
    const int target = peak > ceiling ? static_cast<int>(static_cast<int64_t>(ceiling)*32768/peak) : 32768;
    const int release = previous + 256; // roughly 0.1 s at 512 frames/block
    return target < release ? target : release < 32768 ? release : 32768;
}
inline int16_t scaleMixedPcm(int32_t sample, int gain) {
    // Twelve full-scale voices times Q15 gain still fit signed 64-bit.
    return static_cast<int16_t>((static_cast<int64_t>(sample)*gain)/32768);
}
inline int16_t limitPcm(int32_t sample) {
    const int32_t magnitude=sample<0 ? -sample : sample;
    if (magnitude<=24576) return static_cast<int16_t>(sample);
    const int32_t excess=magnitude-24576;
    const int32_t limited=24576+static_cast<int32_t>(static_cast<int64_t>(excess)*8191/(excess+8191));
    return static_cast<int16_t>(sample<0 ? -limited : limited);
}
}
