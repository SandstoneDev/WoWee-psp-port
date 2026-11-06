#pragma once

#include "platform/psp/psp_fs.hpp"

#include <cstdio>
#include <cstring>

namespace wowee::platform::psp {

// audio.pak stores client-relative names such as sound/music/zonemusic/...
// Accept a flat data/music tree and the client-relative layout used by the
// converter for compatibility with existing EBOOTs. Both paths resolve locally.
inline bool musicFilePath(char* out, size_t cap, const char* track) {
    if (!out || cap == 0 || !track || !*track) return false;
    constexpr char kClientMusicPrefix[] = "sound/music/";
    const char* direct = std::strncmp(track, kClientMusicPrefix, sizeof(kClientMusicPrefix) - 1) == 0
                             ? track + sizeof(kClientMusicPrefix) - 1
                             : track;
    char rel[256], candidate[256];
    int n = std::snprintf(rel, sizeof(rel), "data/music/%s", direct);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(rel) || !fsPath(candidate, sizeof(candidate), rel)) return false;
    if (fsExists(candidate)) {
        n = std::snprintf(out, cap, "%s", candidate);
        return n > 0 && static_cast<size_t>(n) < cap;
    }
    if (direct != track) {
        n = std::snprintf(rel, sizeof(rel), "data/music/%s", track);
        if (n > 0 && static_cast<size_t>(n) < sizeof(rel) && fsPath(candidate, sizeof(candidate), rel) &&
            fsExists(candidate)) {
            n = std::snprintf(out, cap, "%s", candidate);
            return n > 0 && static_cast<size_t>(n) < cap;
        }
    }
    return false;
}

}  // namespace wowee::platform::psp
