// ItemBank's lookup of the NPC races' helm models
// ("hr_<display>", include/rendering/psp/psp_item_npc_race.hpp) - additive: asked only for a race outside the 10
// playable ones (CreatureRenderer::updateEquipment); a pak without these records answers false (no helm, as before).
#include "rendering/psp/psp_equipment.hpp"
#include "rendering/psp/psp_item_npc_race.hpp"

#include "platform/psp/psp_log.hpp"

#include <cstring>
#include <string>

namespace wowee::rendering::psp {

namespace pp = wowee::platform::psp;

namespace {
constexpr size_t kNpcRaceHelmCacheMax = 256;  // displays remembered (a few KB), then forgotten
uint32_t g_npcRaceHelmLogs = 0;
}  // namespace

bool ItemBank::npcRaceHelm(uint32_t displayId, uint8_t race, uint8_t sex, uint16_t& model, uint16_t& texture) {
    model = kNoItemModel;
    texture = kNoItemLayer;
    if (!displayId || !isOpen()) return false;
    auto it = npcRaceHelms_.find(displayId);
    if (it == npcRaceHelms_.end()) {
        if (npcRaceHelms_.size() >= kNpcRaceHelmCacheMax) npcRaceHelms_.clear();
        std::vector<uint64_t> variants;
        const std::string name = kNpcRaceHelmPrefix + std::to_string(displayId);
        uint32_t size = 0;
        if (void* blob = loadNamed(name.c_str(), size)) {
            if (npcRaceHelmValid(blob, size)) {
                NpcRaceHelmHeader h;
                std::memcpy(&h, blob, sizeof(h));
                for (uint32_t k = 0; k < h.count; ++k) {
                    NpcRaceHelmVariant v;
                    std::memcpy(&v, static_cast<const uint8_t*>(blob) + sizeof(h) + k * sizeof(v), sizeof(v));
                    variants.push_back((static_cast<uint64_t>(v.race) << 40) | (static_cast<uint64_t>(v.sex) << 32) |
                                       (static_cast<uint64_t>(v.model) << 16) | v.texture);
                }
            }
            pp::pakFree(blob);
            if (++g_npcRaceHelmLogs <= 20)
                PSP_LOGI(pp::tag::Gfx, "npc_race_helm display=%u variants=%u", static_cast<unsigned>(displayId),
                         static_cast<unsigned>(variants.size()));
        }
        it = npcRaceHelms_.emplace(displayId, std::move(variants)).first;
    }
    for (uint64_t v : it->second) {
        if (static_cast<uint8_t>(v >> 40) != race || static_cast<uint8_t>((v >> 32) & 0xFFu) != (sex ? 1u : 0u)) continue;
        model = static_cast<uint16_t>((v >> 16) & 0xFFFFu);
        texture = static_cast<uint16_t>(v & 0xFFFFu);
        return model != kNoItemModel;
    }
    return false;
}

}  // namespace wowee::rendering::psp
