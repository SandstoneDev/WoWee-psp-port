#include "platform/psp/psp_audio_math.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <initializer_list>
using namespace wowee::platform::psp;
int main() {
    assert(interpolatePcm(-1000,1000,0)==-1000);
    assert(interpolatePcm(-1000,1000,32768)==0);
    for (uint32_t f=0;f<65536;f+=127) {
        const int value=interpolatePcm(-32768,32767,f);
        assert(value>=-32768 && value<=32767);
    }
    int gain=32768;
    for (int peak : {32767, 12*32768, 65536, 19000, 0}) {
        gain=mixBlockGain(peak,gain);
        assert(gain>0 && gain<=32768);
        assert(scaleMixedPcm(peak,gain)<=19000);
        assert(scaleMixedPcm(-peak,gain)>=-19000);
        assert(scaleMixedPcm(peak,gain)+13107<=32767); // music + SFX headroom
    }
    gain=mixBlockGain(12*32768,32768);
    const int recovering=mixBlockGain(0,gain);
    assert(recovering==gain+256);
    int previous=-32768;
    // All sums attainable by the 12-voice mixer: monotonic, bounded and symmetric.
    for (int value=-12*32768;value<=12*32767;++value) {
        const int limited=limitPcm(value);
        assert(limited>=previous && limited>=-32768 && limited<=32767);
        previous=limited;
        assert(limited==-limitPcm(-value));
    }
    assert(limitPcm(0)==0 && limitPcm(24576)==24576);
    assert(limitPcm(100000)<32767 && limitPcm(100000)>24576);
}
