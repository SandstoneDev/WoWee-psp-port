// Memory budget: attachcap's crowd queue with the NPCs in it.
//
// attachcap (psp_attachments.hpp) is one limit for the item models of every character that is not ours - other
// players and NPCs alike. The crowd turn (WorldScreen::remoteCrowdTurn, every 250 ms) looked only at
// the players: the nearest player the cap left without models got them when there was room, and only a farther
// player gave way - so a player 5 yd away stayed bare while eight NPCs 20 yd away held the cap. Now every holder is
// in one queue: the nearest one waiting (capped, nothing queued for it) gets its models; when the cap is full the
// farthest holder with models - player or NPC - more than hysteresisYd farther gives way. An NPC counts npcBiasYd
// farther than it stands (near players matter more than far NPCs; a near NPC still beats a far player). Past the cap
// (the memory governor lowered it) the farthest holder gives way even when nobody waits ("shed").
// PSP only (the client loads every model of everyone). Pure logic: tools/psp/host_tests/memory_budget_test.cpp.
#pragma once

#include <cstddef>
#include <cstdint>

namespace wowee::rendering::psp {

struct CrowdHolder {
    uint64_t guid = 0;
    bool npc = false;
    float dist = 0.0f;     // yd from the player
    uint32_t count = 0;    // item models attached now
    uint32_t capped = 0;   // picks attachcap (or a give-way) left off
    bool busy = false;     // a dress / attach is already queued for it: not picked this turn
};

struct CrowdPick {
    int need = -1;      // index of the holder that gets its models (-1 none)
    int yield = -1;     // index of the holder that gives way first (-1 none)
    bool shed = false;  // the yield is only to get under the cap (nobody waiting)
};

inline float crowdEffectiveYd(const CrowdHolder& h, float npcBiasYd) { return h.dist + (h.npc ? npcBiasYd : 0.0f); }

// inUse / cap: attachmentInUseBytes() / attachmentCapBytes().
inline CrowdPick crowdTurnPick(const CrowdHolder* h, size_t n, uint32_t inUse, uint32_t cap, float npcBiasYd,
                               float hysteresisYd) {
    CrowdPick out;
    float needYd = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        if (h[i].busy || !h[i].capped) continue;
        const float yd = crowdEffectiveYd(h[i], npcBiasYd);
        if (out.need < 0 || yd < needYd) {
            out.need = static_cast<int>(i);
            needYd = yd;
        }
    }
    const bool over = inUse > cap;
    if (out.need < 0 && !over) return out;
    if (inUse < cap) return out;  // room: the waiting one gets its models
    // Full: the farthest holder with models gives way - farther than the waiting one by the hysteresis (as
    // the players' turn). Over the cap: the farthest holder gives way whoever waits (the caller attaches the waiting
    // one only once the heap of models is under the cap again).
    float farYd = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        if (static_cast<int>(i) == out.need || h[i].busy || !h[i].count) continue;
        const float yd = crowdEffectiveYd(h[i], npcBiasYd);
        if (!over && yd <= needYd + hysteresisYd) continue;
        if (out.yield < 0 || yd > farYd) {
            out.yield = static_cast<int>(i);
            farYd = yd;
        }
    }
    out.shed = over && out.yield >= 0;
    return out;
}

}  // namespace wowee::rendering::psp
