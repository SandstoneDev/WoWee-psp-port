// The game objects' models (every one needs its model - a mailbox once had none on hardware). The session keeps the objects in sight (psp_world_session_gameobject.cpp); their
// displays are GameObjectDisplayInfo M2s baked into data/gameobjects.pak (psp_model_conv --gameobjects, index
// "gidx"). Here: a second ModelRenderer (RAM only - the doodads own the eDRAM), fed the objects within kGoRange
// as one pseudo-tile, rebuilt when the set changes (every 0.5 s at most) - the new set loads before the old one
// goes, so models in both stay. Upstream: EntitySpawner::spawnOnlineGameObject (orientation and scale, M2 only
// here: the WMO displays - transports, some doors - come later). No collision yet (walk through a mailbox).
// wowee.cfg gameobjects=0 turns them off. Log: gameobjects_open, gameobjects_set n= models= kb= missing=.
#include "ui/psp/ui_world_screen.hpp"

#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_prof.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
namespace rp = wowee::rendering::psp;

namespace {
constexpr float kGoRange = 110.0f;   // yd: further than the doodads' 160 x reach only for big ones - objects are small
constexpr float kGoMoveYd = 16.0f;   // yd walked before the set is looked at again
constexpr uint32_t kGoEveryMs = 500;
constexpr int kGoTileA = -2000, kGoTileB = -2001;  // the two pseudo-tiles (outside the 0..63 grid), in turn
}  // namespace

void WorldScreen::updateGameObjects() {
    if (goState_ < 0) return;
    if (goState_ == 0) {
        goState_ = -1;
        if (!pp::configGetInt("gameobjects", 1)) return;
        char path[256];
        pp::fsPath(path, sizeof(path), "data/gameobjects.pak");
        if (!goPak_.open(path)) {
            PSP_LOGW(pp::tag::Gfx, "gameobjects_off (no data/gameobjects.pak)");
            return;
        }
        const pp::PakEntry* e = goPak_.find("gidx");
        void* blob = e ? goPak_.load(e) : nullptr;
        const auto* h = static_cast<const rp::GameObjectIndexHeader*>(blob);
        if (!blob || e->size < sizeof(*h) || std::memcmp(h->magic, "WGOI", 4) != 0 ||
            sizeof(*h) + h->count * sizeof(rp::GameObjectDisplayRec) > e->size) {
            PSP_LOGW(pp::tag::Gfx, "gameobjects_off (bad gidx)");
            if (blob) pp::pakFree(blob);
            goPak_.close();
            return;
        }
        const auto* r = reinterpret_cast<const rp::GameObjectDisplayRec*>(static_cast<const uint8_t*>(blob) + sizeof(*h));
        goIndex_.assign(r, r + h->count);
        pp::pakFree(blob);
        goModels_.vramOn = false;
        goModels_.logTag = "go";
        goModels_.viewDistance = models_.viewDistance;
        goModels_.reserveTables(goPak_);
        goPak_.releaseFd();  // read only when the set changes - the descriptor goes back meanwhile
        goState_ = 1;
        PSP_LOGI(pp::tag::Gfx, "gameobjects_open displays=%u index_kb=%u", static_cast<unsigned>(goIndex_.size()),
                 static_cast<unsigned>(goIndex_.size() * sizeof(rp::GameObjectDisplayRec) / 1024u));
    }
    const uint32_t now = pp::logUptimeMs();
    const uint32_t version = session_.gameObjectsVersion();
    const float moved = std::fabs(playerX_ - goLastX_) + std::fabs(playerY_ - goLastY_);
    if (now < goNextMs_ || (version == goSeenVersion_ && moved < kGoMoveYd)) return;
    goNextMs_ = now + kGoEveryMs;
    goSeenVersion_ = version;
    goLastX_ = playerX_;
    goLastY_ = playerY_;
    PROF_SCOPE("gameobjects");

    std::vector<rp::DoodadPlacement> placed;
    uint32_t missing = 0;
    for (const auto& go : session_.gameObjects()) {
        if (go.displayId == 0) continue;
        if (transportOwns(go.guid, go.displayId)) continue;  // WMO displays, lifts, ships
        if (goAnimatedDisplay(go.displayId)) continue;  // drawn animated by creatures_
        const float dx = go.pos.x - playerX_, dy = go.pos.y - playerY_;
        if (dx * dx + dy * dy > kGoRange * kGoRange) continue;
        auto rec = std::lower_bound(goIndex_.begin(), goIndex_.end(), go.displayId,
                                    [](const rp::GameObjectDisplayRec& a, uint32_t id) { return a.displayId < id; });
        if (rec == goIndex_.end() || rec->displayId != go.displayId) {
            ++missing;  // a WMO display, or not in this client's GameObjectDisplayInfo
            continue;
        }
        // upstream: the model turned by the orientation around Z, scaled (EntitySpawner / M2Renderer instance).
        const float c = std::cos(go.orientation), s = std::sin(go.orientation), k = go.scale;
        rp::DoodadPlacement p{};
        p.uniqueId = static_cast<uint32_t>(go.guid & 0xFFFFFFFFu) ^ static_cast<uint32_t>(go.guid >> 32);
        p.model = rec->model;
        p.position[0] = go.pos.x;
        p.position[1] = go.pos.y;
        p.position[2] = go.pos.z;
        const float axes[9] = {c * k, s * k, 0.0f, -s * k, c * k, 0.0f, 0.0f, 0.0f, k};  // column-major Rz * k
        std::memcpy(p.axes, axes, sizeof(axes));
        const float cx = rec->centerCm[0] * 0.01f, cy = rec->centerCm[1] * 0.01f, cz = rec->centerCm[2] * 0.01f;
        p.sphere[0] = go.pos.x + (c * cx - s * cy) * k;
        p.sphere[1] = go.pos.y + (s * cx + c * cy) * k;
        p.sphere[2] = go.pos.z + cz * k;
        p.sphere[3] = std::max(0.25f, rec->radiusCm * 0.01f * k);
        placed.push_back(p);
    }
    auto same = [](const rp::DoodadPlacement& a, const rp::DoodadPlacement& b) {
        return std::memcmp(&a, &b, sizeof(a)) == 0;
    };
    if (placed.size() == goPlaced_.size() && std::equal(placed.begin(), placed.end(), goPlaced_.begin(), same)) return;
    // The new set as the other pseudo-tile, then the old one dropped (models in both stay resident).
    const int newTile = goTile_ ? kGoTileA : kGoTileB, oldTile = goTile_ ? kGoTileB : kGoTileA;
    goModels_.viewDistance = models_.viewDistance;  // the graphics menu's model distance
    goModels_.setPlacements(goPak_, newTile, newTile, placed.data(), static_cast<uint32_t>(placed.size()));
    goModels_.unloadTile(goPak_, oldTile, oldTile);  // none the first time
    goPak_.releaseFd();  // until the next set
    goModels_.flushGrid();
    goTile_ ^= 1u;
    goPlaced_.swap(placed);
    goMissing_ = missing;
    const auto& st = goModels_.stats();
    if (++goLogs_ <= 40)
        PSP_LOGI(pp::tag::Gfx, "gameobjects_set n=%u models=%u textures=%u kb=%u missing=%u in_sight=%u",
                 static_cast<unsigned>(goPlaced_.size()), static_cast<unsigned>(st.models),
                 static_cast<unsigned>(st.textures), static_cast<unsigned>(st.memoryKb), static_cast<unsigned>(missing),
                 static_cast<unsigned>(session_.gameObjects().size()));
}

void WorldScreen::drawGameObjects(const rp::TerrainCamera& cam, uint32_t fogABGR) {
    if (goState_ <= 0 || goPlaced_.empty()) return;
    PROF_SCOPE("draw.gameobjects");
    goModels_.draw(cam, fogABGR);
}

}  // namespace wowee::ui::psp
