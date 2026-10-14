// M10b: creatures and NPCs in the world screen. Kept apart from
// ui_world_screen.cpp (M10a works there) - that file only calls
// loadCreatures() after the doodads and creatures_.draw() after them.
//
// Until live units exist (M10a: SMSG_UPDATE_OBJECT -> creatures_.setUnits),
// the world DB spawns baked around the login tile can be shown instead - a
// DEBUG FALLBACK of our own server: they do not move, do not despawn and are
// not the server's units. wowee.cfg:
//   npcs=0        no creatures at all (pak not even opened)
//   npcstatic=0   no static DB spawns (live units only). 0 by default - the
//                 pak is baked from the DBCs for any server; =1 only with a pak baked with --spawns (our DB)
//   npcview=70    view distance, yards
//   npcmax=24     nearest units drawn
//   npcbudget=6144  KB of models + textures kept resident (was 3072)
//   npcheapkb=1536  a read needs its own block + this many KB free in total after it
//                   (was 768 KB in one block)
//   npcarena=1      NPC models / skins / textures from the world arena's high end (0 = newlib)
//   npcprefetchkb=3072  the world-entry prefetch keeps this many KB free in total (the entry's next
//                   consumers first: a terrain tile, the big WMO the player stands in, window art, Lua)
//   npcanim=1     skinned NPC animation (0 = the static Stand model, A/B)
//   npcanimdist=45  pose every frame nearer, every 2nd / 4th frame beyond x1 / x2
//   npcposes=12   at most this many NPC poses evaluated per frame (0 = no cap)
//   npcbones=2000 bone matrix uploads per frame for NPCs (display list guard)
//   npcanimspeed=1 Walk/Run at the spline speed / M2 movingSpeed (0 = upstream 1.0)
//   creatures from the client's tables:
//   npctocreads=2 TOC block reads (8 KB) per frame for on-demand lookups of displays / models / textures
//   npcequip=1    helm, shoulders and main-hand weapon of humanoid NPCs from items.pak (0 = none, A/B)
//   npcequipdist=25  yards; npcequipmax=8  nearest NPCs dressed
//
// creature anim - syncCreatures hands over what upstream's
// creature sync loop (Application::syncRenderInstancesToEntities) reads: the
// entity's spline-evaluated position (the destination in the dead-reckoning
// overrun window), the feet on our floor while a ground unit moves
// (movingEntityFloor), moving / walking / swimming / flying, dead, and the
// UNIT_NPC_EMOTESTATE animation (Emotes.dbc from characters.pak).
#include "ui/psp/ui_world_screen.hpp"

#include "core/coordinates.hpp"
#include "game/update_field_table.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_system.hpp"  // logMemory

#include <pspthreadman.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <vector>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

// creature anim - upstream movingEntityFloor (application.cpp):
// the reachable floor closest to the server Z (1.5 yd up, 3 yd down), terrain
// first; buildings and props only when the terrain is not within 0.35 yd.
std::optional<float> movingEntityFloor(const rendering::psp::TerrainRenderer& terrain,
                                       const rendering::psp::ModelRenderer& models, float x, float y, float z) {
    constexpr float kMaxStepUp = 1.5f, kMaxGroundDrop = 3.0f;
    const float probeZ = z + kMaxStepUp;
    std::optional<float> best;
    auto consider = [&](bool ok, float f) {
        if (!ok || f > probeZ || f < z - kMaxGroundDrop) return;
        if (!best || std::fabs(f - z) < std::fabs(*best - z)) best = f;
    };
    float h = 0.0f;
    consider(terrain.heightAt(x, y, h), h);
    if (best && std::fabs(*best - z) <= 0.35f) return best;
    float m = 0.0f;
    consider(models.floorHeight(x, y, probeZ, m), m);
    return best;
}

// An item ENTRY -> its ItemDisplayInfo id (upstream EntitySpawner::tryAttachCreatureVirtualWeapons
// resolveDisplayInfoId: Item.dbc entry -> DisplayID). The client's Item.dbc table baked into items.pak (ItemBank::itemDisplay, a few cached 2 KB
// blocks); 0 when the item is not in it (a server's custom item) - the display row itself is checked when
// the NPC is dressed (ItemBank::get). syncCreatures calls it for every unit with a non-zero virtual item field.
uint32_t npcItemDisplayId(rendering::psp::ItemBank& items, uint32_t itemEntry) {
    uint32_t displayId = 0;
    uint8_t inventoryType = 0;
    return items.itemDisplay(itemEntry, displayId, inventoryType) ? displayId : 0;
}

// upstream tryAttachCreatureVirtualWeapons: the main-hand weapon of an NPC from the first non-zero of its
// candidate fields (3.3.5a UNIT_VIRTUAL_ITEM_SLOT_ID = 56, 57, 58; the other bases are other expansions'),
// resolved through Item.dbc (npcItemDisplayId); the off hand is never attached (upstream clears it).
uint32_t npcMainHandDisplay(rendering::psp::ItemBank& items, const game::Entity& e) {
    static const uint16_t kCandidateBases[] = {56, 57, 58, 70, 148, 149, 150, 151, 152};
    for (uint16_t base : kCandidateBases) {
        const uint32_t v = e.getField(base);
        if (!v) continue;
        if (const uint32_t display = npcItemDisplayId(items, v)) return display;
    }
    return 0;
}

// Every virtual item of an NPC as the original 3.3.5a client hangs them -
// UNIT_VIRTUAL_ITEM_SLOT_ID 56 / 57 / 58 = the main hand, the off hand (a weapon, a shield, a held item) and the
// ranged weapon, each through Item.dbc (its DisplayID and InventoryType: where it hangs). PSP: upstream's candidate
// scan above (the first resolvable of the bases of several expansions as the main hand, the off hand cleared) put a
// shield-only NPC's shield in its right hand on a 3.3.5a realm; the 3.3.5a layout is the one these servers send. The
// candidate scan stays for a unit without any of the three (another core's layout): its main hand as before.
void npcVirtualItems(rendering::psp::ItemBank& items, const game::Entity& e, rendering::psp::CreatureUnit& u) {
    const uint32_t entries[3] = {e.getField(56), e.getField(57), e.getField(58)};
    if (!entries[0] && !entries[1] && !entries[2]) {
        u.mainHand = npcMainHandDisplay(items, e);
        return;
    }
    uint32_t* const display[3] = {&u.mainHand, &u.offHand, &u.ranged};
    uint8_t* const inv[3] = {&u.mainHandInv, &u.offHandInv, &u.rangedInv};
    for (int k = 0; k < 3; ++k) {
        uint32_t d = 0;
        uint8_t t = 0;
        if (entries[k] && items.itemDisplay(entries[k], d, t)) {
            *display[k] = d;
            if (t) *inv[k] = t;
        }
    }
}

} // namespace

void WorldScreen::loadCreatures(int tileX, int tileY) {
    if (!pp::configGetInt("npcs", 1)) return;
    char path[256];
    pp::fsPath(path, sizeof(path), "data/creatures.pak");
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    if (!creatures_.open(path)) {
        PSP_LOGW(pp::tag::Gfx, "creatures_missing");
        return;
    }
    creatures_.viewDistance = static_cast<float>(pp::configGetInt("npcview", 70));
    creatures_.maxDrawn = static_cast<uint32_t>(pp::configGetInt("npcmax", 24));
    creatures_.memoryBudgetKb = static_cast<uint32_t>(pp::configGetInt("npcbudget", 6144));  // was 3072
    // The margin is total free after the read (was one block), 1536 (was
    // 768); npcprefetchkb - the world-entry prefetch's margin (3072).
    creatures_.heapReserveKb = static_cast<uint32_t>(std::clamp(pp::configGetInt("npcheapkb", 1536), 0, 8192));
    creatures_.prefetchReserveKb = static_cast<uint32_t>(std::clamp(pp::configGetInt("npcprefetchkb", 3072), 0, 16384));
    creatures_.setAnimate(pp::configGetInt("npcanim", 1) != 0);  // creature anim
    creatures_.animDistance = static_cast<float>(pp::configGetInt("npcanimdist", 45));
    creatures_.maxPoses = static_cast<uint32_t>(pp::configGetInt("npcposes", 12));
    creatures_.maxBoneUploads = static_cast<uint32_t>(pp::configGetInt("npcbones", 2000));
    creatures_.moveAnimRate = pp::configGetInt("npcanimspeed", 1) != 0;  // walk/run
    // the on-demand index and the NPC equipment (items_ opens with the character).
    creatures_.tocReadsPerFrame = static_cast<uint32_t>(std::max(1, pp::configGetInt("npctocreads", 2)));
    creatures_.equipOn = pp::configGetInt("npcequip", 1) != 0;
    creatures_.equipDistance = static_cast<float>(pp::configGetInt("npcequipdist", 25));
    creatures_.maxEquipped = static_cast<uint32_t>(std::max(0, pp::configGetInt("npcequipmax", 8)));
    creatures_.setItemBank(&items_);
    // without our server's DB spawns only the main thread reads creatures.pak (the
    // tile streamer takes its descriptor for npcstatic=1 only) - a rare pak: its descriptor goes back after 5 s
    // without a read (updateMemoryGovernor) and to an open that found none (pakFdRelieve).
    creatures_.setRareFd(pp::configGetInt("npcstatic", 0) == 0);
    creatures_.setWorldArena(pp::configGetInt("npcarena", 1) != 0);  // NPC blobs in the world arena
    size_t units = 0;
    if (pp::configGetInt("npcstatic", 0)) {  // our server's DB spawns, off by default
        std::vector<rendering::psp::CreatureUnit> list;
        const uint32_t mapId = session_.worldEntry().mapId;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy) creatures_.appendStaticSpawns(mapId, tileX + dx, tileY + dy, list);
        creatures_.setUnits(list.data(), list.size());
        units = list.size();
    }
    // The nearest units' models and textures now, behind the world load,
    // instead of a few per frame once the world shows.
    const uint32_t reads = creatures_.prefetch(playerX_, playerY_, playerZ_);
    PSP_LOGI(pp::tag::Gfx, "npc_load static_units=%u reads=%u kb=%u ms=%u anim=%d animdist=%d poses=%u budget=%u rate=%d",
             static_cast<unsigned>(units), static_cast<unsigned>(reads),
             static_cast<unsigned>(creatures_.stats().memoryKb),
             static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u), creatures_.animate() ? 1 : 0,
             static_cast<int>(creatures_.animDistance), static_cast<unsigned>(creatures_.maxPoses),
             static_cast<unsigned>(creatures_.memoryBudgetKb), creatures_.moveAnimRate ? 1 : 0);
    // what the open pak costs in RAM at world entry (index + nothing resident yet).
    PSP_LOGI(pp::tag::Gfx, "npc_index_open index_kb=%u toc_reads=%u equip=%d equip_dist=%d equip_max=%u",
             static_cast<unsigned>(creatures_.indexRamBytes() / 1024u),
             static_cast<unsigned>(creatures_.tocReadsPerFrame), creatures_.equipOn ? 1 : 0,
             static_cast<int>(creatures_.equipDistance), static_cast<unsigned>(creatures_.maxEquipped));
    pp::logMemory("npc_index_open");
    pp::logFlush();
}

void WorldScreen::syncCreatures() {
    if (!creatures_.isOpen()) return;
    creatures_.update(lastDt_);  // creature anim clocks
    const uint16_t fDisplay = game::fieldIndex(game::UF::UNIT_FIELD_DISPLAYID);
    const uint16_t fScale = game::fieldIndex(game::UF::OBJECT_FIELD_SCALE_X);
    const uint16_t fHealth = game::fieldIndex(game::UF::UNIT_FIELD_HEALTH);
    const uint16_t fMaxHealth = game::fieldIndex(game::UF::UNIT_FIELD_MAXHEALTH);
    const uint16_t fDynFlags = game::fieldIndex(game::UF::UNIT_DYNAMIC_FLAGS);
    const uint16_t fEmoteState = game::fieldIndex(game::UF::UNIT_NPC_EMOTESTATE);
    // the sheath state - 3.3.5a UNIT_FIELD_BYTES_2 = OBJECT_END + 0x74 = 122 (as ui_world_sheath.cpp;
    // the UF table has no entry for it).
    constexpr uint16_t fBytes2 = 122;
    const float floorReach2 = creatures_.viewDistance * creatures_.viewDistance;
    liveUnits_.clear();
    for (const auto& [guid, e] : session_.entities().getEntities()) {
        if (e->getType() != game::ObjectType::UNIT) continue;
        rendering::psp::CreatureUnit u;
        u.guid = guid;
        u.displayId = e->getField(fDisplay);
        if (!u.displayId) continue;
        // Upstream spawnOnlineCreature: canonical -> render (= server) position,
        // yaw from the canonical orientation. The creature sync loop
        // draws the destination during the dead-reckoning overrun window.
        const bool inOverrun = e->isEntityMoving() && !e->isActivelyMoving();
        const glm::vec3 p = core::coords::canonicalToServer(
            inOverrun ? glm::vec3(e->getLatestX(), e->getLatestY(), e->getLatestZ())
                      : glm::vec3(e->getX(), e->getY(), e->getZ()));
        u.x = p.x;
        u.y = p.y;
        u.z = p.z;
        u.orientation = core::coords::canonicalToServerYaw(e->getOrientation());
        const uint32_t bits = e->getField(fScale);
        float scale = 1.0f;
        if (bits) std::memcpy(&scale, &bits, sizeof(scale));
        u.scale = (scale > 0.01f && scale < 100.0f) ? scale : 1.0f;
        // creature anim - the state upstream's creature loop reads.
        using MF = game::MovementFlags;
        const uint32_t mf = session_.unitMoveFlags(guid);
        u.moving = e->isActivelyMoving();
        u.walking = session_.unitWalkAnim(guid);  // PSP: npc-walk - the spline's gait, else the WALKING flag
        u.swimming = (mf & static_cast<uint32_t>(MF::SWIMMING)) != 0;
        u.flying = session_.unitFlyAnim(guid);  // PSP: npc-walk - + CAN_FLY / a Flying spline (upstream: FLYING)
        if (u.moving) u.speed = session_.unitSplineSpeed(guid);  // walk/run (animation rate)
        u.dead = game::isUnitCorpseState(e->getField(fHealth), e->getField(fMaxHealth), e->getField(fDynFlags));
        if (const uint32_t st = e->getField(fEmoteState)) {
            auto it = emoteTable_.find(st);
            u.emoteAnim = it != emoteTable_.end() ? (it->second & 0xFFFFu) : 0u;
        }
        if (u.moving && !u.swimming && !u.flying) {
            const float dx = p.x - playerX_, dy = p.y - playerY_;
            if (dx * dx + dy * dy <= floorReach2)
                if (auto f = movingEntityFloor(terrain_, models_, p.x, p.y, p.z)) u.z = *f;
        }
        // the main-hand weapon (upstream tryAttachCreatureVirtualWeapons), units near us only.
        // + the off hand, the ranged weapon, their InventoryType and the sheath state
        // (UNIT_FIELD_BYTES_2 byte 0: SHEATH_STATE_UNARMED = in the sheaths).
        if (creatures_.equipOn) {
            const float dx = p.x - playerX_, dy = p.y - playerY_;
            const float reach = creatures_.equipDistance + 10.0f;
            if (dx * dx + dy * dy <= reach * reach) {
                npcVirtualItems(items_, *e, u);
                u.sheathed = (e->getField(fBytes2) & 0xFFu) == 0u;
                u.sheathState = static_cast<uint8_t>(std::min<uint32_t>(e->getField(fBytes2) & 0xFFu, 2u));  // batch 5 merge
            }
        }
        liveUnits_.push_back(u);
    }
    appendMountUnits(liveUnits_);  // mounts (ui_world_mount.cpp): the mounts under players
    appendClassBarUnits(liveUnits_);  // the swapped models, the unit we move (ui_world_classbars.cpp)
    appendGameObjectUnits(liveUnits_);  // the animated game objects (ui_world_gouse.cpp)
    if (liveUnits_.empty() && !liveCreatures_) return;  // keep the static fallback
    if (!liveCreatures_) {
        liveCreatures_ = true;
        PSP_LOGI(pp::tag::Gfx, "npc_live units=%u (static spawns dropped)", static_cast<unsigned>(liveUnits_.size()));
        // the first live units are what world entry shows (no static spawns by default) -
        // the nearest ones load now, without the per-frame budget, under the loading screen's hold.
        creatures_.setUnits(liveUnits_.data(), liveUnits_.size());
        const uint64_t t0 = sceKernelGetSystemTimeWide();
        const uint32_t reads = creatures_.prefetch(playerX_, playerY_, playerZ_);
        PSP_LOGI(pp::tag::Gfx, "npc_live_prefetch reads=%u kb=%u ms=%u", static_cast<unsigned>(reads),
                 static_cast<unsigned>(creatures_.stats().memoryKb),
                 static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u));
        return;
    }
    creatures_.setUnits(liveUnits_.data(), liveUnits_.size());
}

// creature anim - SMSG_EMOTE from an NPC (upstream emote callback,
// creature branch): a state emote loops and is kept, a one-shot plays once and
// resumes it; a text-only emote (no AnimID) and a cancel (0) change nothing.
void WorldScreen::playCreatureEmote(uint64_t guid, uint32_t emoteId) {
    auto it = emoteTable_.find(emoteId);
    if (!emoteId || it == emoteTable_.end() || !(it->second & 0xFFFFu)) return;
    creatures_.playEmote(guid, it->second & 0xFFFFu, (it->second & 0x10000u) != 0);
}

} // namespace wowee::ui::psp
