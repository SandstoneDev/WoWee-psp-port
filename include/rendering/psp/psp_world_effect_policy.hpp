#pragma once
#include <cstdint>
namespace wowee::rendering::psp {
// Hallow's End: server fire helpers are effect anchors, not visible creatures.
inline constexpr bool isHallowsFireHelper(uint32_t entry) { return entry==23686; }
inline constexpr bool isHallowsFireAura(uint32_t spell) {
    return spell==42074 || spell==42075 || spell==42971 || spell==43184;
}
}
