#pragma once
#include <cstdint>
namespace wowee::rendering::psp {
// PSP: keep event-specific classification disabled until independently verified.
// Ordinary server entities and their normal aura processing remain available.
inline constexpr bool isHallowsFireHelper(uint32_t) { return false; }
inline constexpr bool isHallowsFireAura(uint32_t) { return false; }
}
