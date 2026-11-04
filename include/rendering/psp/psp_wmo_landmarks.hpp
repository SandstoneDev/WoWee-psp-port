#pragma once
#include <algorithm>
#include <cctype>
#include <string>
namespace wowee::rendering::psp {
// Small explicit whitelist: landmarks, not arbitrary decoration or all statues.
inline bool isWmoLandmark(std::string path) {
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) {
        return c == '\\' ? '/' : static_cast<char>(std::tolower(c));
    });
    const std::string prefix="world/generic/human/passive doodads/statues/";
    if (path.compare(0, prefix.size(), prefix) != 0) return false;
    const auto dot=path.find_last_of('.');
    if (dot==std::string::npos || (path.substr(dot)!=".m2" && path.substr(dot)!=".mdx")) return false;
    const auto name=path.substr(prefix.size(), dot-prefix.size());
    return name=="statuealleria" || name=="statuedanath" || name=="statuekhadgar" ||
           name=="statuekurdran" || name=="statueturalyon";
}
}
