#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
namespace wowee::rendering::psp {
// One new record/layer per step; already-cached dependencies cost no extra
// frames. The visible outfit is not changed until preparation is complete.
template<class Gear> class EquipmentWarmup {
public:
    template<class Bank,class Layers> bool step(const Gear& gear,uint8_t race,uint8_t sex,Bank& bank,Layers&& layers) {
        if(!known_ || !(wanted_==gear) || race_!=race || sex_!=sex) {
            wanted_=gear;race_=race;sex_=sex;known_=true;phase_=0;slot_=layer_=0;layers_.clear();
        }
        if(phase_==0) {
            while(slot_<sizeof(gear.displayId)/sizeof(gear.displayId[0])) {
                const auto id=gear.displayId[slot_];
                if(bank.equipmentRecordCached(id)){++slot_;continue;}
                if(bank.warmEquipmentRecord(id))++slot_;
                ++yields;return false;
            }
            phase_=1;
        }
        if(phase_==1){layers_=layers();phase_=2;++yields;return false;}
        while(layer_<layers_.size()) {
            const auto id=layers_[layer_];
            if(bank.equipmentLayerCached(id)){++layer_;continue;}
            if(bank.warmEquipmentLayer(id))++layer_;
            ++yields;return false;
        }
        return true;
    }
    uint32_t yields=0;
private:
    Gear wanted_{};bool known_=false;uint8_t race_=0,sex_=0,phase_=0;
    unsigned slot_=0;std::size_t layer_=0;std::vector<uint16_t> layers_;
};
}
