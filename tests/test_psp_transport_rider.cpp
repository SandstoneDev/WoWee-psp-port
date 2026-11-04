#include "rendering/psp/psp_transport_rider.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <initializer_list>

using namespace wowee::rendering::psp;

int main() {
    // A fast descending deck leaves the post-motion boarding tolerance even
    // though the player was standing on it immediately before the frame.
    assert(transportDeckContact(true,60.0f,59.875f));
    assert(!transportDeckContact(true,60.0f,55.0f));
    assert(!transportDeckContact(false,60.0f,60.0f)); // never capture a falling/jumping player
    assert(!transportDeckContact(true,61.0f,60.0f));
    assert(!transportDeckContact(true,std::numeric_limits<float>::quiet_NaN(),60.0f));

    // Carrying uses the exact sampled platform transform, independent of the
    // capped character-physics timestep. Test slow hardware, stalls and rotation.
    for (const int fps : {3,9,15,30,60}) {
        const glm::vec3 local(1.5f,-0.75f,0.125f);
        for (int frame=0;frame<fps*30;++frame) {
            const float t=static_cast<float>(frame)/fps;
            const glm::vec3 lift(1500.0f,240.0f,60.0f-40.0f*(1.0f-std::cos(t)));
            const float yaw=t*0.2f;
            const glm::vec3 rider=transportRiderWorld(lift,yaw,local);
            assert(std::fabs((rider.z-lift.z)-local.z)<0.0001f);
            const float c=std::cos(yaw),s=std::sin(yaw);
            const glm::vec3 d=rider-lift;
            assert(std::fabs(c*d.x+s*d.y-local.x)<0.0002f);
            assert(std::fabs(-s*d.x+c*d.y-local.y)<0.0002f);
        }
    }
    // A deck translation changes neither a jump's relative height nor the
    // player's local placement, including a multi-yard low-FPS frame.
    const glm::vec3 localJump(0,0,2.0f);
    const auto top=transportRiderWorld(glm::vec3(0,0,60),0,localJump);
    const auto bottom=transportRiderWorld(glm::vec3(0,0,-40),0,localJump);
    assert(std::fabs((top.z-60)-(bottom.z+40))<0.0001f);

    // Floor ownership is the collided model part, not the smallest overlapping
    // object bounding box. Set-reload ID flips and WMO parts retain ownership.
    constexpr uint64_t first=0xF11000005001ULL, second=0xF11000005002ULL;
    for (uint32_t flip : {0u,0x20000000u}) {
        for (uint32_t part=0;part<12;++part) {
            const uint32_t id=transportPartId(first,part,flip);
            assert(transportOwnsPart(first,12,flip,id));
            assert(!transportOwnsPart(second,12,flip,id));
            assert(!transportOwnsPart(first,12,flip^0x20000000u,id));
        }
    }
    std::puts("Transport rider tests passed: 3/9/15/30/60 FPS, rotation, contact and exact platform ownership.");
}
