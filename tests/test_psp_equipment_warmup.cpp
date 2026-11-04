#include "rendering/psp/psp_equipment_warmup.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <set>
using namespace wowee::rendering::psp;
struct Gear {uint32_t displayId[4]{};bool operator==(const Gear& b)const{return std::equal(displayId,displayId+4,b.displayId);}};
struct Bank {
    std::set<uint32_t> records;std::set<uint16_t> layers;unsigned reads=0;bool pending=false;
    bool equipmentRecordCached(uint32_t id){return !id||records.count(id);}
    bool equipmentLayerCached(uint16_t id){return layers.count(id);}
    bool warmEquipmentRecord(uint32_t id){if(pending)return false;++reads;records.insert(id);return true;}
    bool warmEquipmentLayer(uint16_t id){if(pending)return false;++reads;layers.insert(id);return true;}
};
int main(){
    Gear gear{{1,2,0,3}};Bank bank;EquipmentWarmup<Gear> prep;unsigned frames=0,builds=0;
    auto layers=[&]{++builds;return std::vector<uint16_t>{10,11,10};};
    for(;;){auto before=bank.reads;bool ready=prep.step(gear,1,0,bank,layers);assert(bank.reads-before<=1);++frames;if(ready)break;assert(frames<12);}
    assert(frames==7&&builds==1&&bank.reads==5);
    for(int n=0;n<1000;++n)assert(prep.step(gear,1,0,bank,layers));
    assert(bank.reads==5&&builds==1);
    gear.displayId[1]=4;bank.pending=true;assert(!prep.step(gear,1,0,bank,layers));
    assert(!bank.records.count(4)); // pending I/O does not commit an incomplete outfit
    bank.pending=false;while(!prep.step(gear,1,0,bank,layers)){}
    assert(bank.records.count(4)&&builds==2);
    // A newer outfit replaces in-flight preparation before it can be committed.
    gear.displayId[0]=5;assert(!prep.step(gear,1,0,bank,layers));
    gear.displayId[0]=6;assert(!prep.step(gear,1,0,bank,layers));
    while(!prep.step(gear,1,0,bank,layers)){}
    assert(bank.records.count(6));
    auto before=builds;assert(!prep.step(gear,2,1,bank,layers));assert(builds==before+1);
    std::puts("Equipment warmup passed: one new dependency per frame, pending I/O, cached hits, outfit/race changes, no early commit.");
}
