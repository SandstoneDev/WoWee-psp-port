#include "rendering/psp/psp_fishing_visual.hpp"
// The game objects' models (every one needs its model - a mailbox once had none on hardware). The session keeps the objects in sight (psp_world_session_gameobject.cpp); their
// displays are GameObjectDisplayInfo M2s baked into data/gameobjects.pak (psp_model_conv --gameobjects, index
// "gidx"). Here: a second ModelRenderer (RAM only - the doodads own the eDRAM), fed the objects within kGoRange
// as one pseudo-tile, rebuilt when the set changes (every 0.5 s at most) - the new set loads before the old one
// goes, so models in both stay. Upstream: EntitySpawner::spawnOnlineGameObject (orientation and scale, M2 only
// here: the WMO displays - transports, some doors - come later). No collision yet (walk through a mailbox).
// wowee.cfg gameobjects=0 turns them off. Log: gameobjects_open, gameobjects_set n= models= kb= missing=.
#include "ui/psp/ui_world_screen.hpp"

#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_bigheap.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_prof.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <pspgu.h>

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
    const uint64_t bobber=session_.fishingBobber();
    if(bobber!=fishingVisualGuid_) {
        fishingVisualGuid_=bobber;fishingSpawnMs_=bobber ? pp::logUptimeMs() : 0;
    }
    if (goPak_.isOpen()) goModels_.retryCollisions(goPak_);
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
    bool needsModel=false;
    for(const auto& p:placed)if(!goModels_.modelKnown(p.model)){needsModel=true;break;}
    if(needsModel && !pp::heapGivesKb(4096)) {
        // Keep the current set intact and retry after cleanup; don't allocate
        // new object textures into the world's last remaining heap block.
        goSeenVersion_=~version;
        return;
    }
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
    const uint64_t bobber=session_.fishingBobber();
    goModels_.visualOffset(static_cast<uint32_t>(bobber)^static_cast<uint32_t>(bobber>>32),fishingBobberVisualOffset());
    goModels_.draw(cam, fogABGR);
}

float WorldScreen::fishingBobberVisualOffset() const {
    return rp::fishingBobberDip(pp::logUptimeMs(),session_.bobberBiteMs());
}

void WorldScreen::drawFishingLine(const rp::TerrainCamera& cam) {
    if(!session_.isChanneling())return;
    const auto* bobber=session_.findGameObject(session_.fishingBobber());
    if(!bobber)return;
    float point[3];
    if(!player_.weaponTip(15,point))return; // no stale hand position or invented floating line
    const glm::vec3 tip(point[0],point[1],point[2]);
    const glm::vec3 end=bobber->pos+glm::vec3(0,0,fishingBobberVisualOffset()+.05f);
    const glm::mat4 proj=glm::perspective(glm::radians(cam.fovyDeg),480.f/272.f,cam.nearZ,cam.farZ);
    const glm::mat4 view=glm::lookAt(glm::vec3(0),cam.forward,cam.up),identity(1.f);
    ScePspFMatrix4 m;
    std::memcpy(&m,&proj[0][0],sizeof(m));sceGuSetMatrix(GU_PROJECTION,&m);
    std::memcpy(&m,&view[0][0],sizeof(m));sceGuSetMatrix(GU_VIEW,&m);
    std::memcpy(&m,&identity[0][0],sizeof(m));sceGuSetMatrix(GU_MODEL,&m);
    struct Vertex {uint32_t color;float x,y,z;};
    constexpr int segments=12;
    auto* vertices=static_cast<Vertex*>(sceGuGetMemory((segments+1)*sizeof(Vertex)));
    if(!vertices)return;
    for(int i=0;i<=segments;++i) {
        const glm::vec3 p=rp::fishingLinePoint(tip,end,i/static_cast<float>(segments))-cam.pos;
        vertices[i]={0xFF8A9A9Au,p.x,p.y,p.z};
    }
    sceGuDisable(GU_TEXTURE_2D);sceGuDisable(GU_LIGHTING);sceGuDisable(GU_BLEND);sceGuDisable(GU_ALPHA_TEST);sceGuDisable(GU_FOG);
    sceGuEnable(GU_DEPTH_TEST);sceGuDepthMask(GU_TRUE);
    sceGuDrawArray(GU_LINE_STRIP,GU_COLOR_8888|GU_VERTEX_32BITF|GU_TRANSFORM_3D,segments+1,nullptr,vertices);
    sceGuDepthMask(GU_FALSE);sceGuEnable(GU_TEXTURE_2D);pp::textureResetBinding();
}

void WorldScreen::drawFishingSplash(const rp::TerrainCamera& cam) {
    const uint64_t guid=session_.fishingBobber();
    const auto* bobber=guid?session_.findGameObject(guid):nullptr;
    if(!bobber || pp::displayListFree()<8192)return;
    const uint32_t now=pp::logUptimeMs(),bite=session_.bobberBiteMs();
    uint32_t stamp=bite && now-bite<850u ? bite : (fishingVisualGuid_==guid?fishingSpawnMs_:0);
    if(!stamp || now-stamp>=850u)return;
    const float age=(now-stamp)/1000.f;
    const glm::mat4 proj=glm::perspective(glm::radians(cam.fovyDeg),480.f/272.f,cam.nearZ,cam.farZ);
    const glm::mat4 view=glm::lookAt(glm::vec3(0),cam.forward,cam.up),identity(1.f);
    ScePspFMatrix4 m;
    std::memcpy(&m,&proj[0][0],sizeof(m));sceGuSetMatrix(GU_PROJECTION,&m);
    std::memcpy(&m,&view[0][0],sizeof(m));sceGuSetMatrix(GU_VIEW,&m);
    std::memcpy(&m,&identity[0][0],sizeof(m));sceGuSetMatrix(GU_MODEL,&m);
    struct V {uint32_t color;float x,y,z;};
    constexpr int sides=16,drops=8,maxVertices=2*sides*6+drops*6;
    auto* v=static_cast<V*>(sceGuGetMemory(maxVertices*sizeof(V)));if(!v)return;
    int n=0;
    const glm::vec3 origin=bobber->pos-cam.pos+glm::vec3(0,0,.04f);
    auto emit=[&](glm::vec3 p,uint32_t c){v[n++]={c,p.x,p.y,p.z};};
    for(int ring=0;ring<2;++ring) {
        const float t=(age-ring*.12f)/.72f;if(t<0 || t>=1)continue;
        const float radius=.06f+.7f*t,width=.025f*(1-t);
        const uint32_t c=(static_cast<uint32_t>(170*(1-t))<<24)|0x00E8E2C8u;
        for(int k=0;k<sides;++k) {
            const float a=k*6.2831853f/sides,b=(k+1)*6.2831853f/sides;
            const glm::vec3 da(std::cos(a),std::sin(a),0),db(std::cos(b),std::sin(b),0);
            const auto p0=origin+da*(radius-width),p1=origin+da*(radius+width),p2=origin+db*(radius-width),p3=origin+db*(radius+width);
            emit(p0,c);emit(p1,c);emit(p2,c);emit(p2,c);emit(p1,c);emit(p3,c);
        }
    }
    const float t=age/.6f;
    if(t<1) {
        const glm::vec3 right=glm::normalize(glm::cross(cam.forward,cam.up))*.026f,up=cam.up*.045f;
        const uint32_t c=(static_cast<uint32_t>(220*(1-t))<<24)|0x00F4EADCu;
        for(int k=0;k<drops;++k) {
            const float a=k*6.2831853f/drops;
            const auto p=origin+glm::vec3(std::cos(a)*t*.45f,std::sin(a)*t*.45f,.4f*std::sin(3.14159265f*t));
            emit(p-right-up,c);emit(p+right-up,c);emit(p-right+up,c);emit(p-right+up,c);emit(p+right-up,c);emit(p+right+up,c);
        }
    }
    sceGuDisable(GU_TEXTURE_2D);sceGuDisable(GU_LIGHTING);sceGuDisable(GU_ALPHA_TEST);sceGuDisable(GU_FOG);sceGuDisable(GU_CULL_FACE);
    sceGuEnable(GU_DEPTH_TEST);sceGuDepthMask(GU_TRUE);sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD,GU_SRC_ALPHA,GU_ONE_MINUS_SRC_ALPHA,0,0);
    sceGuDrawArray(GU_TRIANGLES,GU_COLOR_8888|GU_VERTEX_32BITF|GU_TRANSFORM_3D,n,nullptr,v);
    sceGuDisable(GU_BLEND);sceGuDepthMask(GU_FALSE);sceGuEnable(GU_TEXTURE_2D);pp::textureResetBinding();
}

}  // namespace wowee::ui::psp
