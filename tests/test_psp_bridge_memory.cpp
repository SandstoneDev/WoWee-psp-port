#include "rendering/psp/psp_model_instance.hpp"
#include "platform/psp/psp_mem_governor.hpp"
#include <cassert>
#include <cstdio>
#include <vector>
using namespace wowee::rendering::psp;
struct Model {};
struct PreviousInstance {
 float x,y,z,r,reach;const DoodadPlacement* p;const Model* m;
 glm::mat3 inverse{1};glm::vec3 lo{},hi{};bool collision=false;
};
int main(){
 using Instance=ModelInstance<Model>;
 static_assert(sizeof(Instance)<sizeof(PreviousInstance));
 std::vector<Instance> instances(17584);
 std::vector<ModelCollisionTransform> transforms(512);
 for(size_t i=0;i<transforms.size();++i)instances[i].collision=static_cast<uint32_t>(i+1);
 for(size_t i=512;i<instances.size();++i)assert(!instances[i].collision);
 for(size_t i=0;i<512;++i)transforms[instances[i].collision-1].collisionLo.x=static_cast<float>(i);
 // Reordering render instances cannot invalidate a collision transform index.
 std::swap(instances[0],instances[17583]);assert(transforms[instances[17583].collision-1].collisionLo.x==0);
 const size_t oldBytes=instances.size()*sizeof(PreviousInstance);
 const size_t newBytes=instances.size()*sizeof(Instance)+transforms.size()*sizeof(ModelCollisionTransform);
 assert(oldBytes>newBytes+512u*1024u);
 using namespace wowee::platform::psp;
 MemGovernor gov;MemGovernorConfig config;config.cutKb=4096;config.restoreKb=6144;config.emergencyKb=2048;config.tickMs=250;
 gov.configure(config);assert(!gov.decide(0,true,false).dir);
 auto decision=gov.decide(250,false,false,false);assert(decision.dir==1&&decision.step==kMemStepParticles);
 // Failed contiguous block checks are emergency pressure: don't wait two seconds per cleanup step.
 decision=gov.decide(500,false,false,false);assert(decision.dir==1&&decision.step==kMemStepAttachments);
 decision=gov.decide(750,false,false,false);assert(decision.dir==1&&decision.step==kMemStepWmoDoodads);
 assert(!gov.decide(1000,true,true).dir); // no immediate restore/reload churn
 std::printf("Sparse collision storage: %zu -> %zu bytes (%zu KiB saved in host fixture); emergency cleanup/hysteresis passed.\n",oldBytes,newBytes,(oldBytes-newBytes)/1024);
}
