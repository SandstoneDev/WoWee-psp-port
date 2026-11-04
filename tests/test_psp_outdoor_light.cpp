#include "rendering/psp/psp_outdoor_light.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
using wowee::rendering::psp::outdoorDiffuseWeight;
int main() {
    // Logged Brill 18:04: almost zero direct light, RGB tint averaged 0.22.
    const float amb = (56.f + 42.f + 59.f) / (3.f * 255.f);
    const float dif = (116.f + 154.f + 196.f) / (3.f * 255.f);
    const float fixed = (amb + dif * outdoorDiffuseWeight(85, .01f, .6f)) / .9511f;
    assert(fixed > .48f && fixed < .52f);
    // Clear daytime and deep night retain their previous direct weighting.
    assert(outdoorDiffuseWeight(85, .8f, .6f) == .8f);
    assert(std::abs(outdoorDiffuseWeight(85, -.9f, .6f) - .54f) < .0001f);
    // Cross the horizon smoothly; authored colors and fog are not altered.
    assert(std::abs(outdoorDiffuseWeight(85, -.00001f, .6f) -
                    outdoorDiffuseWeight(85, .00001f, .6f)) < .0001f);
    // Undercity and all other zones keep the existing calculation.
    for (auto zone : {1497u, 0u, 10u, 130u})
        for (float z : {-.8f, -.01f, 0.f, .01f, .8f})
            assert(outdoorDiffuseWeight(zone,z,.6f) == (z >= 0.f ? z : -z*.6f));
    assert(outdoorDiffuseWeight(85,-.3f,0.f) == 0.f);
    std::puts("Outdoor lighting passed: Brill dusk visibility, horizon continuity, other zones unchanged.");
}
