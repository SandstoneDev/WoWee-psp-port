#include "rendering/psp/psp_bigwmo_format.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
using namespace wowee::rendering::psp;
int main() {
    // The logout log is in Undercity group 7 (throne). Original WMO metadata:
    // 0x2040 = portal interior AND explicit exterior lighting. It must follow
    // the zone profile, unlike an ordinary underground 0x2000 interior.
    assert(bigWmoGroupUsesZoneLight(0x2040u));
    assert(!bigWmoGroupUsesZoneLight(0x2000u));
    assert(bigWmoGroupUsesZoneLight(kWmoGroupExterior));
    assert(bigWmoGroupUsesZoneLight(kWmoGroupExteriorLit));
    assert(!bigWmoGroupUsesZoneLight(kWmoGroupInterior|kWmoGroupHasDoodads));
    assert(bigWmoGroupUsesZoneLight(kWmoGroupInterior|kWmoGroupExteriorLit|kWmoGroupHasDoodads));
    std::puts("WMO lighting passed: roofed exterior-lit interiors use zone lighting, baked underground interiors retained.");
}
