#include "rendering/psp/psp_collision_bounds_index.hpp"
#include <cassert>
#include <random>
#include <cmath>
#include <cstdio>
using namespace wowee::rendering::psp;
float entry(const CollisionBoundsIndex::Entry& e,glm::vec3 o,glm::vec3 d,float limit) {
    float a=0,b=limit;
    for(int i=0;i<3;++i) {
        if(d[i]==0){if(o[i]<e.lo[i] || o[i]>e.hi[i])return limit;}
        else {float l=(e.lo[i]-o[i])/d[i],h=(e.hi[i]-o[i])/d[i];if(l>h)std::swap(l,h);a=std::max(a,l);b=std::min(b,h);if(a>b)return limit;}
    }
    return a;
}
int main() {
    CollisionBoundsIndex index;std::vector<CollisionBoundsIndex::Entry> boxes;
    std::mt19937 rng(42);std::uniform_real_distribution<float> pos(-100,100),size(.1f,8.f);
    for(unsigned i=0;i<4096;++i){glm::vec3 p(pos(rng),pos(rng),pos(rng));boxes.push_back({p,p+glm::vec3(size(rng),size(rng),size(rng)),i});}
    index.build(boxes);size_t total=0;
    for(int q=0;q<10000;++q){
        glm::vec3 o(pos(rng),pos(rng),pos(rng)),d(pos(rng)/50,pos(rng)/50,pos(rng)/50);
        if(q%3==0)d[q%3]=0; // axis-parallel, non-unit and negative rays
        float ref=60.f,best=60.f;
        for(auto& e:boxes)ref=std::min(ref,entry(e,o,d,ref));
        index.rayQuery(o,d,best,[&](const auto& e){++total;best=std::min(best,entry(e,o,d,best));});
        assert(std::fabs(ref-best)<.0001f);
    }
    // A nearest hit must prune distant boxes and an empty index is harmless.
    boxes.clear();for(unsigned i=0;i<1000;++i)boxes.push_back({glm::vec3(i*4.f,-1,-1),glm::vec3(i*4.f+1,1,1),i});
    index.build(boxes);float best=5000;int visited=0;
    index.rayQuery(glm::vec3(-1,0,0),glm::vec3(1,0,0),best,[&](const auto& e){++visited;best=entry(e,glm::vec3(-1,0,0),glm::vec3(1,0,0),best);});
    assert(best==1 && visited==1);
    index.clear();index.rayQuery(glm::vec3(0),glm::vec3(0),best,[&](const auto&){assert(false);});
    std::printf("Camera ray: 10000 reference comparisons passed; %zu candidates instead of 40960000 full-scan entries; nearest-hit pruning passed.\n",total);
}
