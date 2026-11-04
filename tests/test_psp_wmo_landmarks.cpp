#include "rendering/psp/psp_bigwmo_format.hpp"
#include "rendering/psp/psp_wmo_landmarks.hpp"
#include <cassert>
#include <array>
using namespace wowee::rendering::psp;
int main() {
    for (const char* name:{"statuealleria","statuedanath","statuekhadgar","statuekurdran","statueturalyon"}) {
        assert(isWmoLandmark(std::string("world/generic/human/passive doodads/statues/")+name+".m2"));
    }
    assert(isWmoLandmark("WORLD\\GENERIC\\HUMAN\\PASSIVE DOODADS\\STATUES\\STATUEKHADGAR.MDX"));
    assert(!isWmoLandmark("world/generic/human/passive doodads/statues/statuelion.m2"));
    assert(!isWmoLandmark("world/detail/statuekhadgar.m2"));
    assert(!isWmoLandmark("world/generic/human/passive doodads/statues/statuekhadgar.blp"));
    alignas(BigWmoHeader) std::array<uint8_t,136+4+16*76> b{};
    auto* h=reinterpret_cast<BigWmoHeader*>(b.data());
    uint32_t count=99;
    assert(!bigWmoLandmarks(b.data(),b.size(),count) && count==0); // old pack
    h->reserved=136;
    uint32_t n=8; std::memcpy(b.data()+136,&n,4);
    auto* d=reinterpret_cast<BigWmoDoodad*>(b.data()+140);d[0].model=674;d[7].index=3344;
    assert(bigWmoLandmarks(b.data(),140+8*76,count)==d && count==8 && d[7].index==3344);
    assert(!bigWmoLandmarks(b.data(),140+8*76-1,count)); // truncation
    n=17;std::memcpy(b.data()+136,&n,4);assert(!bigWmoLandmarks(b.data(),b.size(),count));
    h->reserved=0xfffffffc;assert(!bigWmoLandmarks(b.data(),b.size(),count));
    h->reserved=137;assert(!bigWmoLandmarks(b.data(),b.size(),count));
    assert(!bigWmoLandmarks(nullptr,0,count));
}
