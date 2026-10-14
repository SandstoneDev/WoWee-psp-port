// psp_emblem_format.hpp - guild tabard emblem values, part codes and data/emblems.pak entry names.
//
// Clean-room implementation of specs/world/emblem.md (v2), sections 2 and 3.1-3.3. Header only; <cstddef>,
// <cstdint>, <cstdio>; platform-free. The emblem step of body composition (emblem.md 3.4) is the equipment group's
// (specs/equip/body_layers.md R6) and uses the types below.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace wowee {
namespace rendering {
namespace psp {
namespace emblem {

// The five guild emblem values, in the order the world screen copies them from the guild query response.
// -1 = not set.
struct Emblem {
    int32_t style = -1, color = -1, borderStyle = -1, borderColor = -1, background = -1;

    // 3.1: true when none of the five values equals -1 (other negative values count as known).
    bool valid() const {
        return style != -1 && color != -1 && borderStyle != -1 && borderColor != -1 && background != -1;
    }

    // The spec writes "= default" (a C++20 defaulted comparison); this header targets C++17, so the comparison of
    // all five values is spelled out. Same meaning.
    bool operator==(const Emblem& o) const {
        return style == o.style && color == o.color && borderStyle == o.borderStyle && borderColor == o.borderColor &&
               background == o.background;
    }
    bool operator!=(const Emblem& o) const { return !(*this == o); }
};

enum Part : uint8_t { kItem = 0, kBackground = 1, kBorder = 2, kEmblem = 3 };

constexpr int kTorsoUpper = 3, kTorsoLower = 4;  // body section numbers
constexpr int kLayerBackground = 2, kLayerBorder = 3, kLayerEmblem = 4;  // layer slots within a section

// 3.2: code of a border or emblem part = style * 32 + colour, truncated to 16 bits (modular arithmetic, so any int
// input is well defined).
constexpr uint16_t code(int style, int color) {
    return static_cast<uint16_t>(static_cast<uint32_t>(style) * 32u + static_cast<uint32_t>(color));
}

// 3.2: the range in which the code is unique and fits 16 bits.
inline bool codeFits(int32_t style, int32_t color) {
    return style >= 0 && style < 2000 && color >= 0 && color < 32;
}

// 3.3: entry name of one emblem layer in data/emblems.pak. Returns false (writing nothing) for a null buffer, a
// buffer shorter than 16, a section other than 3 / 4 or a part other than background / border / emblem.
inline bool layerName(char* out, size_t n, uint8_t part, int section, uint16_t c) {
    if (!out || n < 16) return false;
    char letter;
    if (section == kTorsoUpper) letter = 'u';
    else if (section == kTorsoLower) letter = 'l';
    else return false;
    const unsigned v = c;
    switch (part) {
    case kBackground:
        std::snprintf(out, n, "gb_%c_%u", letter, v);
        return true;
    case kBorder:
        std::snprintf(out, n, "gr_%c_%u_%u", letter, v / 32u, v % 32u);
        return true;
    case kEmblem:
        std::snprintf(out, n, "ge_%c_%u_%u", letter, v / 32u, v % 32u);
        return true;
    default:
        return false;
    }
}

}  // namespace emblem
}  // namespace psp
}  // namespace rendering
}  // namespace wowee
