#ifdef NDEBUG
#undef NDEBUG
#endif
#include "rendering/psp/psp_collision_bounds_index.hpp"
#include "rendering/psp/psp_collision_stream_policy.hpp"
#include "rendering/psp/psp_collision_neighborhood.hpp"
#include <cassert>
#include <cstdio>
#include <random>
using namespace wowee::rendering::psp;

int main() {
    std::mt19937 rng(71);
    std::uniform_real_distribution<float> xy(-300,300), z(-80,80), extent(0,40), size(.1f,10);
    std::vector<CollisionBoundsIndex::Entry> entries;
    for (uint32_t i=0; i<2281; ++i) {
        const glm::vec3 lo(xy(rng),xy(rng),z(rng));
        entries.push_back({lo,lo+glm::vec3(extent(rng),extent(rng),extent(rng)),i});
    }
    // Long chunks spanning the tree, zero-size bounds, and stacked floors.
    entries.push_back({{-400,-400,-90},{400,400,-85},2281});
    entries.push_back({{0,0,0},{0,0,0},2282});
    entries.push_back({{-2,-2,-41},{2,2,-40},2283});
    entries.push_back({{-2,-2,54},{2,2,55},2284});
    CollisionBoundsIndex index;
    index.build(entries);
    uint64_t examined=0, bruteChecks=0;
    for (int q=0; q<12000; ++q) {
        const glm::vec3 pos(xy(rng),xy(rng),z(rng));
        const float scale=size(rng), radius=size(rng);
        std::vector<uint32_t> expected,actual;
        auto near=[&](const CollisionBoundsIndex::Entry& e) {
            const float dx=std::max({e.lo.x-pos.x,pos.x-e.hi.x,0.f});
            const float dy=std::max({e.lo.y-pos.y,pos.y-e.hi.y,0.f});
            return collisionStreamNearby(dx,dy,e.lo.z,e.hi.z,pos.z,scale,radius);
        };
        for (const auto& e:entries) { ++bruteChecks; if (near(e)) expected.push_back(e.id); }
        const float r=(radius+0.001f)/scale;
        index.query(pos-glm::vec3(r,r,8.001f/scale),pos+glm::vec3(r,r,3.001f/scale),[&](const auto& e) {
            ++examined; if (near(e)) actual.push_back(e.id);
        });
        std::sort(expected.begin(),expected.end()); std::sort(actual.begin(),actual.end());
        assert(expected==actual);
        std::vector<uint32_t> production;
        collisionNeighborhood(index,pos,scale,radius,[&](uint32_t id,float){production.push_back(id);});
        std::sort(production.begin(),production.end());
        assert(expected==production);
        // Check inclusive face contact at both vertical limits and horizontal radius.
        if (q<1000) {
            const auto& e=entries[q];
            for (float feet:{e.lo.z-3.f/scale,e.hi.z+8.f/scale}) {
                const glm::vec3 p(e.lo.x-radius/scale,e.lo.y,feet);
                bool got=false;
                index.query(p-glm::vec3(r,r,8.001f/scale),p+glm::vec3(r,r,3.001f/scale),
                    [&](const auto& hit) { if(hit.id==e.id) got=true; });
                const float dx=e.lo.x-p.x;
                if(collisionStreamNearby(dx,0,e.lo.z,e.hi.z,p.z,scale,radius)) assert(got);
            }
        }
    }
    assert(examined<bruteChecks/20);
    // Exact stacked-floor selection and reload/empty ownership.
    std::vector<uint32_t> hits;
    index.query({-1,-1,-42},{1,1,-39},[&](const auto& e){hits.push_back(e.id);});
    assert(std::find(hits.begin(),hits.end(),2283)!=hits.end());
    assert(std::find(hits.begin(),hits.end(),2284)==hits.end());
    std::printf("12000 queries match brute force; candidates %llu/%llu; index %zu bytes\n",
        static_cast<unsigned long long>(examined),static_cast<unsigned long long>(bruteChecks),index.bytes());
    index.clear();
    assert(index.bytes()==0);
    index.query({-1000,-1000,-1000},{1000,1000,1000},[](const auto&){assert(false);});
    index.build({{{0,0,0},{1,1,1},55}});
    unsigned count=0;
    index.query({1,1,1},{2,2,2},[&](const auto& e){assert(e.id==55);++count;});
    assert(count==1);
}
