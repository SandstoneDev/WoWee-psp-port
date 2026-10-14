// The world screen's transports on the PSP:
//   - the game objects whose display is a WMO ("gw_<display>": groups + doodad set 0) - ships, zeppelins, the
//     icebreaker, an arena's pillars, doors, a battlefield's walls - drawn where the object is, turned by
//     its orientation (upstream EntitySpawner::spawnOnlineGameObject's WMO branch);
//   - the moving ones: GAMEOBJECT_TYPE_TRANSPORT (11: the capital cities' lifts, the underground tram's cars)
//     by TransportAnimation.dbc ("ta_<entry>"), GAMEOBJECT_TYPE_MO_TRANSPORT (15: ships, zeppelins) by the template's
//     TaxiPathNode path ("tpaths") - psp_transport_path.cpp, timed as the server times them; the clock is the
//     movement block's path time plus our clock since (the session's TransportObject);
//   - a transport's collision is a floor and walls (the moving parts' WCOL meshes, as the buildings'), the camera
//     stops at its hull;
//   - riding (upstream GameHandler::updateM2TransportBoarding): the player boards a transport when the ground
//     under it is the transport's (as its collision step finds it), keeps it while in the air inside its bounds,
//     and leaves it when standing on other ground (also next to a docked ship); the place is kept in the
//     transport's frame and the transport carries it (an elevator moves its riders too), the facing turns with
//     it, the camera keeps its own yaw (upstream's camera has no transport coupling); every movement packet then
//     carries MOVEMENTFLAG_ONTRANSPORT, the transport's guid and the place in its frame (MovementInfo, upstream's
//     writeMovementPayload), and a heartbeat goes out every 0.5 s while riding (upstream GameHandler::update's
//     moveHeartbeatInterval_), which also tells the server of a boarding or a leaving;
//   - others on a transport (the session's riders) are drawn where it carries them this frame.
// Three more RAM-only ModelRenderers (as the game objects' one), one per pak ("slots"):
//   data/transports.pak       the transport WMOs (world/wmo/transports/..., gunships) + ta_ / tpaths - small TOC;
//   data/gameobjects_wmo.pak  the other WMO displays (doors, walls, buildings) - opened only while such an object is
//                             near (its TOC is ~2.3k entries), closed 10 s after the last one left;
//   data/gameobjects.pak      the M2 transports (lifts, tram cars), through goPak_.
// A set change loads the new set before dropping the old one; a moving transport rewrites its placements every
// frame (ModelRenderer::movePlacements: a few dozen parts). wowee.cfg transports=0 turns all of it off (the game
// objects' renderer then shows the M2 lifts unmoved, as in 115).
#include "ui/psp/ui_world_screen.hpp"

#include "core/coordinates.hpp"
#include "platform/psp/psp_bigheap.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_transport_parse.hpp"
#include "rendering/psp/psp_transport_format.hpp"
#include "rendering/psp/psp_transport_path.hpp"

#include <pspthreadman.h>

#include <malloc.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_set>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
namespace rp = wowee::rendering::psp;

namespace {
constexpr int kSlotTransports = 0, kSlotStatic = 1, kSlotM2 = 2, kSlots = 3;
constexpr float kWmoRange = 320.0f;  // yd + the object's radius: a ship (~60 yd long) is seen coming in to the dock
constexpr float kM2Range = 160.0f;   // a lift or a tram car (the doodads' reach)
constexpr uint32_t kLostMs = 15000;  // riding a transport that left the map: held this long for the transfer
constexpr uint32_t kStaticIdleMs = 10000;  // gameobjects_wmo.pak closes this long after its last object left
constexpr uint8_t kTypeDoor = 0, kTypeTransport = 11, kTypeMoTransport = 15;
constexpr uint32_t kIdFlip = 0x20000000u;  // Slot::idBit (placeParts' ids keep bit 30)
constexpr uint8_t kReloads = 6;            // a set's reloads for its missing parts (30 s)

// finite and fmod-wrapped (the loops never ended at |a| > ~3e7 or inf, a NaN went through).
float wrapAngle(float a) { return pp::cleanTransportAngle(a); }

glm::vec3 rotZ(float yaw, const glm::vec3& v) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    return glm::vec3(c * v.x - s * v.y, s * v.x + c * v.y, v.z);
}

float yawOf(const glm::quat& q) {
    return std::atan2(2.0f * (q.w * q.z + q.x * q.y), 1.0f - 2.0f * (q.y * q.y + q.z * q.z));
}

// Whether the heap still gives totalKb (as blocks of at most 512 KB) with one block of largestKb among them: the
// models load as separate pak blobs. Every block is written (gcc drops a malloc/free pair it can see through).
bool heapFits(uint32_t totalKb, uint32_t largestKb) {
    std::array<void*, 64> held{};
    size_t n = 0;
    uint32_t got = 0;
    auto take = [&](uint32_t kb) {
        void* volatile p = pp::heapProbeAlloc(static_cast<size_t>(kb) * 1024u);  // as malloc, uncounted
        if (!p) return false;
        static_cast<volatile char*>(p)[0] = 0;
        held[n++] = p;
        got += kb;
        return true;
    };
    bool ok = take(std::max<uint32_t>(1u, largestKb));
    while (ok && got < totalKb && n < held.size()) ok = take(std::min<uint32_t>(512u, totalKb - got));
    ok = ok && got >= totalKb;
    for (size_t i = 0; i < n; ++i) std::free(held[i]);
    return ok;
}
}  // namespace

struct WorldScreen::TransportsUi {
    struct Wmo {
        int slot = kSlotTransports;
        rp::TransportWmoHeader h{};
        std::vector<rp::TransportPart> parts;
        uint32_t needKb = 0;      // its models + collision (pak entry sizes) x 1.5 for the textures: the memory gate
        uint32_t maxEntryKb = 0;  // the largest of them (one block of the heap)
    };
    struct Keys {
        std::vector<rp::TransportAnimKey> pos;
        std::vector<rp::TransportRotKey> rot;
    };
    struct Obj {
        uint64_t guid = 0;
        uint32_t entry = 0, display = 0;
        uint8_t type = 0;
        bool transport = false;    // GAMEOBJECT_TYPE_TRANSPORT / _MO_TRANSPORT: moves, carries riders
        const Wmo* wmo = nullptr;  // a WMO display (else an M2 of gameobjects.pak)
        rp::GameObjectDisplayRec m2{};
        bool m2ok = false;
        // the session's fields
        glm::vec3 spawn{0.0f};
        float spawnYaw = 0.0f, scale = 1.0f;
        glm::vec4 parentRot{0.0f, 0.0f, 0.0f, 1.0f};
        uint32_t level = 0, timeOffset = 0, stateSerial = 0, stateChangeMs = 0;
        uint8_t state = 0, prevState = 0;
        bool hasTime = false;
        // motion
        rp::ElevatorAnim anim;
        bool animTried = false, animOk = false, settled = false;
        uint32_t animSerial = 0;
        rp::MoTransportPath* path = nullptr;
        bool pathTried = false;
        uint32_t pathLevel = ~0u;
        uint32_t pathRetryMs = 0;  // tpaths could not be read - asked again from then
        // this frame
        glm::vec3 pos{0.0f};
        float yaw = 0.0f;
        bool onMap = true, placed = false;
        glm::vec3 boxMin{0.0f}, boxMax{0.0f};  // model space (unscaled)
        float radius = 1.0f;
        uint32_t gen = 0;
        int slot() const { return wmo ? wmo->slot : kSlotM2; }
    };
    // One pak and its renderer.
    struct Slot {
        const char* file = "";
        const char* what = "";
        pp::Pak ownPak;
        pp::Pak* pak = nullptr;    // ownPak, or goPak_ for the M2 slot
        int state = 0;             // 0 not tried, 1 open, -1 none
        rp::ModelRenderer r;
        std::vector<uint64_t> set;  // the resident objects, in placement order
        std::vector<rp::DoodadPlacement> placed;
        uint32_t tileFlip = 0;
        int tileA = 0, tileB = 0;
        bool has = false, touched = false, reserved = false;
        uint32_t idleSinceMs = 0;
        // no memory for a set's placements - the old set stays until retryMs; parts whose model
        // did not load - the set is placed again at reloadMs (0: none due), reloads times since it changed; idBit
        // alternates the parts' uniqueIds at every placing (a part kept from the old set asks for its model again).
        uint32_t retryMs = 0, reloadMs = 0, idBit = 0;
        uint8_t reloads = 0;
        int curTile() const { return tileFlip ? tileB : tileA; }
    };
    std::array<Slot, kSlots> slots;
    std::unordered_map<uint32_t, Wmo> wmos;  // by display
    std::unordered_set<uint32_t> noWmo;
    std::unordered_map<uint32_t, Keys> keys;  // by entry
    std::unordered_set<uint32_t> noKeys;
    std::unordered_map<uint32_t, std::unique_ptr<rp::MoTransportPath>> paths;  // by entry
    std::unordered_set<uint32_t> noPath;
    std::map<uint64_t, Obj> objs;  // by guid: the placement order
    uint32_t seenTrVersion = ~0u, seenGoVersion = ~0u, gen = 0, seenMap = ~0u;
    int seenGoState = -2;
    // riding
    uint64_t riding = 0;
    glm::vec3 local{0.0f};
    float localO = 0.0f;
    bool placePending = false;  // the server put us on it (login / transfer): hold until it is placed
    bool beatDue = false;       // boarded / left since the last heartbeat
    uint32_t heldSinceMs = 0;
    uint32_t logs = 0, frameLogMs = 0, boardings = 0, memRetryMs = 0;
    bool off = false;

    bool anyResident() const {
        for (const Slot& s : slots)
            if (!s.set.empty()) return true;
        return false;
    }
};

void WorldScreen::destroyTransportsUi(TransportsUi* p) { delete p; }

WorldScreen::TransportsUi& WorldScreen::transportsUi() {
    if (!tr_) {
        tr_ = std::unique_ptr<TransportsUi, void (*)(TransportsUi*)>(new TransportsUi, &WorldScreen::destroyTransportsUi);
        TransportsUi& tr = *tr_;
        tr.off = !pp::configGetInt("transports", 1);
        const char* files[kSlots] = {"data/transports.pak", "data/gameobjects_wmo.pak", "data/gameobjects.pak"};
        const char* whats[kSlots] = {"wmo", "wmo_static", "m2"};
        const char* tags[kSlots] = {"trw", "trs", "trm"};
        for (int i = 0; i < kSlots; ++i) {
            TransportsUi::Slot& s = tr.slots[i];
            s.file = files[i];
            s.what = whats[i];
            s.pak = i == kSlotM2 ? &goPak_ : &s.ownPak;
            s.tileA = -3000 - 2 * i;  // pseudo-tiles: outside 0..63 and the game objects' -2000 / -2001
            s.tileB = -3001 - 2 * i;
            s.r.vramOn = false;  // RAM only: the doodads own the eDRAM
            s.r.logTag = tags[i];
        }
    }
    return *tr_;
}

bool WorldScreen::transportRiding() const { return tr_ && tr_->riding != 0; }

namespace {

using TrUi = WorldScreen::TransportsUi;

bool openSlot(TrUi& tr, int i) {
    TrUi::Slot& s = tr.slots[i];
    if (s.state == 0) {
        char path[256];
        pp::fsPath(path, sizeof(path), s.file);
        s.state = s.ownPak.open(path) ? 1 : -1;
        if (s.state > 0) {
            s.r.reserveTables(s.ownPak);
            s.reserved = true;
            s.touched = true;
            PSP_LOGI(pp::tag::Gfx, "transports_open file=%s entries=%u", s.file, static_cast<unsigned>(s.ownPak.count()));
        } else if (i == kSlotTransports) {
            PSP_LOGW(pp::tag::Gfx, "transports_off (no %s): transport WMOs and lift / ship paths absent", s.file);
        } else {
            PSP_LOGW(pp::tag::Gfx, "transports_static_off (no %s): WMO doors / walls absent", s.file);
        }
    }
    return s.state > 0;
}

// The display's "gw_" record (once per display): transports.pak first, then - for a display that is no M2 of
// gameobjects.pak - gameobjects_wmo.pak.
const TrUi::Wmo* wmoFor(TrUi& tr, uint32_t display, bool tryStatic) {
    if (!display || tr.noWmo.count(display)) return nullptr;
    auto it = tr.wmos.find(display);
    if (it != tr.wmos.end()) return &it->second;
    char name[24];
    std::snprintf(name, sizeof(name), "gw_%u", static_cast<unsigned>(display));
    int slot = -1;
    const pp::PakEntry* e = nullptr;
    if (openSlot(tr, kSlotTransports) && (e = tr.slots[kSlotTransports].ownPak.find(name))) slot = kSlotTransports;
    if (slot < 0) {
        if (!tryStatic) return nullptr;  // asked again once gameobjects.pak's index is known
        if (openSlot(tr, kSlotStatic) && (e = tr.slots[kSlotStatic].ownPak.find(name))) slot = kSlotStatic;
    }
    if (slot < 0) {
        tr.noWmo.insert(display);
        return nullptr;
    }
    TrUi::Slot& s = tr.slots[slot];
    void* blob = s.ownPak.load(e);
    s.touched = true;
    const auto* h = static_cast<const rp::TransportWmoHeader*>(blob);
    if (!blob || e->size < sizeof(*h) || std::memcmp(h->magic, "WTRW", 4) != 0 ||
        sizeof(*h) + h->partCount * sizeof(rp::TransportPart) != e->size) {
        if (blob) pp::pakFree(blob);
        tr.noWmo.insert(display);
        return nullptr;
    }
    auto& w = tr.wmos[display];
    w.slot = slot;
    w.h = *h;
    const auto* p = reinterpret_cast<const rp::TransportPart*>(static_cast<const uint8_t*>(blob) + sizeof(*h));
    w.parts.assign(p, p + h->partCount);
    pp::pakFree(blob);
    std::unordered_set<uint16_t> models;
    uint32_t bytes = 0, largest = 0;
    for (const auto& part : w.parts) {
        if (!models.insert(part.model).second) continue;
        for (const char* prefix : {"m_", "k_"}) {
            std::snprintf(name, sizeof(name), "%s%u", prefix, static_cast<unsigned>(part.model));
            if (const pp::PakEntry* me = s.ownPak.find(name)) {
                bytes += me->size;
                largest = std::max(largest, me->size);
            }
        }
    }
    w.needKb = bytes / 1024u * 3u / 2u + 1u;
    w.maxEntryKb = largest / 1024u + 1u;
    if (++tr.logs <= 80)
        PSP_LOGI(pp::tag::Gfx, "transport_wmo display=%u pak=%s groups=%u parts=%u kb=%u need_kb=%u",
                 static_cast<unsigned>(display), s.what, static_cast<unsigned>(w.h.groupCount),
                 static_cast<unsigned>(w.parts.size()),
                 static_cast<unsigned>((w.parts.size() * sizeof(rp::TransportPart) + 1023) / 1024),
                 static_cast<unsigned>(w.needKb));
    return &w;
}

const TrUi::Keys* keysFor(TrUi& tr, uint32_t entry) {
    if (tr.noKeys.count(entry)) return nullptr;
    auto it = tr.keys.find(entry);
    if (it != tr.keys.end()) return &it->second;
    if (!openSlot(tr, kSlotTransports)) return nullptr;
    TrUi::Slot& s = tr.slots[kSlotTransports];
    char name[24];
    std::snprintf(name, sizeof(name), "ta_%u", static_cast<unsigned>(entry));
    const pp::PakEntry* e = s.ownPak.find(name);
    void* blob = e ? s.ownPak.load(e) : nullptr;
    s.touched = s.touched || blob;
    const auto* h = static_cast<const rp::TransportAnimHeader*>(blob);
    if (!blob || e->size < sizeof(*h) || std::memcmp(h->magic, "WTRA", 4) != 0 ||
        sizeof(*h) + h->posCount * sizeof(rp::TransportAnimKey) + h->rotCount * sizeof(rp::TransportRotKey) != e->size) {
        if (blob) pp::pakFree(blob);
        tr.noKeys.insert(entry);
        return nullptr;
    }
    auto& k = tr.keys[entry];
    const auto* pk = reinterpret_cast<const rp::TransportAnimKey*>(static_cast<const uint8_t*>(blob) + sizeof(*h));
    k.pos.assign(pk, pk + h->posCount);
    const auto* rk = reinterpret_cast<const rp::TransportRotKey*>(pk + h->posCount);
    k.rot.assign(rk, rk + h->rotCount);
    pp::pakFree(blob);
    return &k;
}

// An MO_TRANSPORT's path from its template (built once per entry: the path's nodes kept).
// only its slice of tpaths is read - the header, the path table, its own nodes (was: the whole
// entry, ~60 KB, for every transport); only a path that is not there or does not build is remembered as missing - a
// failed read or a short heap sets `retry` (the caller asks again later; was: the ship stood for the session).
rp::MoTransportPath* pathFor(TrUi& tr, uint32_t entry, const pp::WorldSession::TransportTemplate& tpl, bool& retry) {
    retry = false;
    auto it = tr.paths.find(entry);
    if (it != tr.paths.end()) return it->second.get();
    if (tr.noPath.count(entry)) return nullptr;
    if (!openSlot(tr, kSlotTransports)) return nullptr;  // no transports.pak: nothing moves (transports_off)
    TrUi::Slot& s = tr.slots[kSlotTransports];
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    const pp::PakEntry* e = s.ownPak.find("tpaths");
    std::vector<rp::MoTransportPath::Node> nodes;
    rp::TaxiPathSetHeader h{};
    if (e) {
        s.touched = true;
        if (!s.ownPak.readPart(e, 0, &h, sizeof(h))) {
            retry = true;
        } else if (std::memcmp(h.magic, "WTPN", 4) == 0 &&
                   sizeof(h) + h.pathCount * sizeof(rp::TaxiPathRef) +
                           static_cast<uint64_t>(h.nodeCount) * sizeof(rp::TaxiPathNodeRec) == e->size) {
            const uint32_t refBytes = h.pathCount * static_cast<uint32_t>(sizeof(rp::TaxiPathRef));  // <= 11 KB
            auto* refs = static_cast<rp::TaxiPathRef*>(std::malloc(std::max<uint32_t>(1u, refBytes)));
            if (!refs || !s.ownPak.readPart(e, sizeof(h), refs, refBytes)) {
                retry = true;
            } else {
                const auto* ref = std::lower_bound(refs, refs + h.pathCount, tpl.taxiPathId,
                                                   [](const rp::TaxiPathRef& r, uint32_t id) { return r.pathId < id; });
                if (ref != refs + h.pathCount && ref->pathId == tpl.taxiPathId && ref->count &&
                    ref->first <= h.nodeCount && ref->count <= h.nodeCount - ref->first) {
                    const uint32_t count = ref->count;
                    const uint32_t at = static_cast<uint32_t>(sizeof(h)) + refBytes +
                                        ref->first * static_cast<uint32_t>(sizeof(rp::TaxiPathNodeRec));
                    const uint32_t bytes = count * static_cast<uint32_t>(sizeof(rp::TaxiPathNodeRec));
                    auto* recs = static_cast<rp::TaxiPathNodeRec*>(std::malloc(bytes));
                    if (!recs || !s.ownPak.readPart(e, at, recs, bytes)) {
                        retry = true;
                    } else {
                        nodes.reserve(count);
                        for (uint32_t i = 0; i < count; ++i) {
                            const rp::TaxiPathNodeRec& n = recs[i];
                            nodes.push_back({glm::vec3(n.pos[0], n.pos[1], n.pos[2]), n.mapId, n.flags, n.delaySec});
                        }
                    }
                    std::free(recs);
                }
            }
            std::free(refs);
        }
    }
    if (retry) {
        if (++tr.logs <= 160)
            PSP_LOGW(pp::tag::Gfx, "transport_path_retry entry=%u path=%u (tpaths not read: heap / card; again in 5 s)",
                     static_cast<unsigned>(entry), static_cast<unsigned>(tpl.taxiPathId));
        return nullptr;
    }
    auto path = std::make_unique<rp::MoTransportPath>();
    if (nodes.empty() || !path->build(nodes.data(), nodes.size(), static_cast<float>(tpl.moveSpeed),
                                      static_cast<float>(tpl.accelRate))) {
        tr.noPath.insert(entry);
        PSP_LOGW(pp::tag::Gfx, "transport_path_missing entry=%u path=%u nodes=%u (rebake transports.pak with "
                 "--transport-all-paths?)", static_cast<unsigned>(entry), static_cast<unsigned>(tpl.taxiPathId),
                 static_cast<unsigned>(nodes.size()));
        return nullptr;
    }
    size_t stops = 0;
    for (const auto& seg : path->segments()) stops += seg.stops.size() - 1;
    PSP_LOGI(pp::tag::Gfx, "transport_path entry=%u path=%u nodes=%u segments=%u stops=%u built_ms=%u speed=%u "
             "accel=%u ms=%u", static_cast<unsigned>(entry), static_cast<unsigned>(tpl.taxiPathId),
             static_cast<unsigned>(nodes.size()), static_cast<unsigned>(path->segments().size()),
             static_cast<unsigned>(stops), static_cast<unsigned>(path->builtPeriod()),
             static_cast<unsigned>(tpl.moveSpeed), static_cast<unsigned>(tpl.accelRate),
             static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u));
    return (tr.paths[entry] = std::move(path)).get();
}

// The object's parts placed where it is this frame (world = place + Rz(yaw) * scale * part).
void placeParts(const TrUi::Obj& o, std::vector<rp::DoodadPlacement>& out) {
    const float c = std::cos(o.yaw), s = std::sin(o.yaw), k = o.scale;
    const glm::mat3 R(glm::vec3(c * k, s * k, 0.0f), glm::vec3(-s * k, c * k, 0.0f), glm::vec3(0.0f, 0.0f, k));
    const uint32_t salt = static_cast<uint32_t>(o.guid & 0xFFFFFFFFu) * 2654435761u ^ static_cast<uint32_t>(o.guid >> 32);
    if (o.wmo) {
        for (size_t i = 0; i < o.wmo->parts.size(); ++i) {
            const rp::TransportPart& p = o.wmo->parts[i];
            rp::DoodadPlacement d{};
            d.uniqueId = (salt + static_cast<uint32_t>(i) * 40503u) | 0x40000000u;
            d.model = p.model;
            const glm::vec3 pos = o.pos + R * glm::vec3(p.position[0], p.position[1], p.position[2]);
            const glm::mat3 A(glm::vec3(p.axes[0], p.axes[1], p.axes[2]), glm::vec3(p.axes[3], p.axes[4], p.axes[5]),
                              glm::vec3(p.axes[6], p.axes[7], p.axes[8]));
            const glm::mat3 W = R * A;
            for (int a = 0; a < 3; ++a) d.position[a] = pos[a];
            for (int col = 0; col < 3; ++col)
                for (int row = 0; row < 3; ++row) d.axes[col * 3 + row] = W[col][row];
            const glm::vec3 sc = o.pos + R * glm::vec3(p.sphere[0], p.sphere[1], p.sphere[2]);
            d.sphere[0] = sc.x;
            d.sphere[1] = sc.y;
            d.sphere[2] = sc.z;
            d.sphere[3] = std::max(0.25f, p.sphere[3] * k);
            out.push_back(d);
        }
        return;
    }
    rp::DoodadPlacement d{};
    d.uniqueId = salt | 0x40000000u;
    d.model = o.m2.model;
    for (int a = 0; a < 3; ++a) d.position[a] = o.pos[a];
    const float axes[9] = {c * k, s * k, 0.0f, -s * k, c * k, 0.0f, 0.0f, 0.0f, k};
    std::memcpy(d.axes, axes, sizeof(axes));
    const glm::vec3 cc = o.pos + R * glm::vec3(o.m2.centerCm[0] * 0.01f, o.m2.centerCm[1] * 0.01f,
                                               o.m2.centerCm[2] * 0.01f);
    d.sphere[0] = cc.x;
    d.sphere[1] = cc.y;
    d.sphere[2] = cc.z;
    d.sphere[3] = std::max(0.25f, o.m2.radiusCm * 0.01f * k);
    out.push_back(d);
}

// The object's place in its model frame: inside its box (grown by `grow` yd, `up` yd more above).
bool insideBox(const TrUi::Obj& o, const glm::vec3& world, float grow, float up) {
    const glm::vec3 l = rotZ(-o.yaw, world - o.pos) / std::max(0.01f, o.scale);
    return l.x >= o.boxMin.x - grow && l.x <= o.boxMax.x + grow && l.y >= o.boxMin.y - grow &&
           l.y <= o.boxMax.y + grow && l.z >= o.boxMin.z - grow && l.z <= o.boxMax.z + up;
}

}  // namespace

bool WorldScreen::transportOwns(uint64_t guid, uint32_t displayId) {
    if (!tr_ || tr_->off) return false;
    auto it = tr_->objs.find(guid);
    if (it != tr_->objs.end()) return it->second.transport || it->second.wmo;
    if (goState_ == 0) return false;  // gameobjects.pak's index not read yet
    if (goState_ == 1) {              // an M2 display is the game objects' (an M2 transport is in objs already)
        auto rec = std::lower_bound(goIndex_.begin(), goIndex_.end(), displayId,
                                    [](const rp::GameObjectDisplayRec& a, uint32_t id) { return a.displayId < id; });
        if (rec != goIndex_.end() && rec->displayId == displayId) return false;
    }
    return wmoFor(*tr_, displayId, true) != nullptr;
}

void WorldScreen::updateTransports(float dtSec) {
    (void)dtSec;
    TransportsUi& tr = transportsUi();
    if (tr.off) return;
    const auto& trObjs = session_.transportObjects();
    if (trObjs.empty() && tr.objs.empty() && session_.gameObjects().empty()) return;
    PROF_SCOPE("transports");
    const uint32_t mapId = session_.worldEntry().mapId;
    // An M2 display of gameobjects.pak (known once its index is: updateGameObjects) is no WMO - gameobjects_wmo.pak
    // is not opened for it.
    auto isM2 = [&](uint32_t display) {
        auto rec = std::lower_bound(goIndex_.begin(), goIndex_.end(), display,
                                    [](const rp::GameObjectDisplayRec& a, uint32_t id) { return a.displayId < id; });
        return rec != goIndex_.end() && rec->displayId == display;
    };
    // 1. The objects, when the session's change (fields copied: the frame below reads only ours).
    // (Also when gameobjects.pak's index comes up: the M2 transports' models are known from then.)
    if (session_.transportObjectsVersion() != tr.seenTrVersion || session_.gameObjectsVersion() != tr.seenGoVersion ||
        mapId != tr.seenMap || goState_ != tr.seenGoState) {
        tr.seenTrVersion = session_.transportObjectsVersion();
        tr.seenGoVersion = session_.gameObjectsVersion();
        tr.seenMap = mapId;
        tr.seenGoState = goState_;
        ++tr.gen;
        auto modelOf = [&](TransportsUi::Obj& o) {
            o.wmo = nullptr;
            o.m2ok = false;
            if (goState_ == 1 && isM2(o.display)) {
                auto rec = std::lower_bound(goIndex_.begin(), goIndex_.end(), o.display,
                                            [](const rp::GameObjectDisplayRec& a, uint32_t id) { return a.displayId < id; });
                o.m2 = *rec;
                o.m2ok = true;
                const glm::vec3 c(rec->centerCm[0] * 0.01f, rec->centerCm[1] * 0.01f, rec->centerCm[2] * 0.01f);
                const float r = rec->radiusCm * 0.01f;
                o.boxMin = c - r;
                o.boxMax = c + r;
                o.radius = r + glm::length(c);
                return;
            }
            o.wmo = wmoFor(tr, o.display, goState_ != 0);
            if (o.wmo) {
                for (int a = 0; a < 3; ++a) {
                    o.boxMin[a] = o.wmo->h.bmin[a];
                    o.boxMax[a] = o.wmo->h.bmax[a];
                }
                o.radius = o.wmo->h.radius + glm::length(glm::vec3(o.wmo->h.center[0], o.wmo->h.center[1],
                                                                   o.wmo->h.center[2]));
            }
        };
        for (const auto& [guid, t] : trObjs) {
            TransportsUi::Obj& o = tr.objs[guid];
            const bool fresh = o.gen == 0 || o.entry != t.entry;
            if (fresh) o = TransportsUi::Obj{};
            o.guid = guid;
            o.gen = tr.gen;
            o.entry = t.entry;
            o.type = t.type;
            o.transport = t.type == kTypeTransport || t.type == kTypeMoTransport;
            o.spawn = t.pos;
            o.spawnYaw = wrapAngle(t.orientation);  // the session's is clean already
            o.scale = t.scale;
            o.parentRot = t.parentRot;
            o.level = t.level;
            o.timeOffset = t.timeOffsetMs;
            o.hasTime = t.hasTime;
            o.state = t.state;
            o.prevState = t.prevState;
            o.stateSerial = t.stateSerial;
            o.stateChangeMs = t.stateChangeMs;
            if (fresh || o.display != t.displayId || (!o.wmo && !o.m2ok)) {
                o.display = t.displayId;
                modelOf(o);
            }
        }
        for (const auto& go : session_.gameObjects()) {  // the other WMO objects (doors, walls, bridges...)
            if (!go.displayId) continue;
            auto it = tr.objs.find(go.guid);
            if (it != tr.objs.end() && it->second.gen == tr.gen) continue;  // a transport, done above
            // PSP: an open WMO door (GAMEOBJECT_TYPE_DOOR, GO_STATE_ACTIVE) is left out - drawn shut it would also
            // block the way; its opening is the game-object use code's.
            if (go.type == kTypeDoor && go.state == 0) continue;
            if (it == tr.objs.end() && (goState_ == 0 || (goState_ == 1 && isM2(go.displayId)) ||
                                        !wmoFor(tr, go.displayId, true)))
                continue;
            TransportsUi::Obj& o = tr.objs[go.guid];
            const bool fresh = o.gen == 0;
            o.guid = go.guid;
            o.gen = tr.gen;
            o.entry = go.entry;
            o.type = go.type;
            o.transport = false;
            o.spawn = go.pos;
            o.spawnYaw = wrapAngle(go.orientation);  // the raw orientation (NaN / huge)
            o.scale = go.scale;
            if (fresh || o.display != go.displayId) {
                o.display = go.displayId;
                modelOf(o);
            }
        }
        for (auto it = tr.objs.begin(); it != tr.objs.end();) {
            if (it->second.gen == tr.gen && (it->second.wmo || it->second.m2ok)) {
                ++it;
                continue;
            }
            if (it->first == tr.riding && it->second.gen != tr.gen) {
                PSP_LOGI(pp::tag::Gfx, "transport_leave guid=%08X why=gone", static_cast<unsigned>(tr.riding));
                tr.riding = 0;
                session_.setOwnTransport(0, glm::vec3(0.0f), 0.0f);
            }
            it = tr.objs.erase(it);
        }
    }
    // 2. Where each is this frame.
    const uint32_t clock = pp::WorldSession::transportClockMs();
    const uint32_t nowMs = pp::logUptimeMs();
    bool anyMoved = false;
    for (auto& [guid, o] : tr.objs) {
        const glm::vec3 oldPos = o.pos;
        const float oldYaw = o.yaw;
        o.pos = o.spawn;
        o.yaw = o.spawnYaw;
        o.onMap = true;
        if (o.type == kTypeMoTransport) {
            if (!o.pathTried && nowMs >= o.pathRetryMs) {
                if (const auto* tpl = session_.transportTemplate(o.entry, guid)) {
                    bool retry = false;  // tpaths not read now - again in 5 s
                    o.path = pathFor(tr, o.entry, *tpl, retry);
                    o.pathTried = !retry;
                    if (retry) o.pathRetryMs = nowMs + 5000;
                }
            }
            if (o.path) {
                if (o.pathLevel != o.level) {
                    o.path->setPeriod(o.level);
                    o.pathLevel = o.level;
                    PSP_LOGI(pp::tag::Gfx, "transport_period entry=%u level_ms=%u built_ms=%u", static_cast<unsigned>(o.entry),
                             static_cast<unsigned>(o.level), static_cast<unsigned>(o.path->builtPeriod()));
                }
                rp::MoTransportPlace pl;
                if (o.hasTime && o.path->evaluate(clock + o.timeOffset, pl)) {
                    o.pos = pl.pos;
                    o.yaw = pl.orientation;
                    o.onMap = pl.mapId == mapId;
                }
            }
        } else if (o.type == kTypeTransport && o.hasTime) {
            if (!o.animTried) {
                o.animTried = true;
                if (const auto* k = keysFor(tr, o.entry)) {
                    o.anim.setKeys(k->pos.data(), k->pos.size(), k->rot.data(), k->rot.size());
                    o.animOk = !o.anim.empty();
                    if (++tr.logs <= 80)
                        PSP_LOGI(pp::tag::Gfx, "transport_anim entry=%u keys=%u rot=%u period_ms=%u level=%u state=%u",
                                 static_cast<unsigned>(o.entry), static_cast<unsigned>(k->pos.size()),
                                 static_cast<unsigned>(k->rot.size()), static_cast<unsigned>(o.anim.period()),
                                 static_cast<unsigned>(o.level), static_cast<unsigned>(o.state));
                }
            }
            if (o.animOk) {
                const uint32_t pathTime = clock + o.timeOffset;
                if (!o.settled) {
                    o.anim.settle(pathTime, o.state, o.level);
                    o.settled = true;
                    o.animSerial = o.stateSerial;
                } else if (o.animSerial != o.stateSerial) {
                    o.anim.onStateChange(o.stateChangeMs + o.timeOffset, o.state, o.level);
                    o.animSerial = o.stateSerial;
                }
                glm::vec3 off(0.0f);
                glm::quat q(1.0f, 0.0f, 0.0f, 0.0f);
                bool hasRot = false;
                o.anim.evaluate(pathTime, o.level, off, q, hasRot);
                o.pos = o.spawn + rp::rotateByParent(o.parentRot, off);
                if (hasRot) o.yaw = wrapAngle(o.spawnYaw + yawOf(q));
            }
        }
        if (o.placed && (o.pos != oldPos || o.yaw != oldYaw)) anyMoved = true;
    }
    // 3. The resident sets: in reach of the player (the one we ride always), on this map.
    std::array<std::vector<uint64_t>, kSlots> sets;
    for (auto& [guid, o] : tr.objs) {
        o.placed = false;
        if (!o.onMap || (!o.wmo && !o.m2ok)) continue;
        const float reach = (o.wmo ? kWmoRange : kM2Range) + o.radius * o.scale;
        const float dx = o.pos.x - playerX_, dy = o.pos.y - playerY_;
        if (dx * dx + dy * dy > reach * reach && guid != tr.riding) continue;
        sets[o.slot()].push_back(guid);
    }
    // The memory gate (heap ~44 MB, ~1-3 MB free in the world): a WMO object coming into reach loads only when its
    // models fit with trreserve KB (wowee.cfg, default 768) to spare - a ship is ~1 MB. Else it stays out (the one
    // we ride never does) and is tried again in 5 s.
    static const uint32_t reserveKb = static_cast<uint32_t>(std::max(0, pp::configGetInt("trreserve", 768)));
    for (int si : {kSlotTransports, kSlotStatic}) {
        auto& set = sets[si];
        const auto& was = tr.slots[si].set;
        if (set == was) continue;
        uint32_t needKb = 0, blockKb = 0;
        std::unordered_set<uint32_t> displays;
        for (uint64_t g : was) {  // resident already (find - [] made an empty one of a gone one)
            auto wo = tr.objs.find(g);
            if (wo != tr.objs.end()) displays.insert(wo->second.display);
        }
        std::vector<uint64_t> added;
        for (uint64_t g : set) {
            if (std::find(was.begin(), was.end(), g) != was.end()) continue;
            added.push_back(g);
            const auto& o = tr.objs[g];
            if (displays.insert(o.display).second) {
                needKb += o.wmo->needKb;
                blockKb = std::max(blockKb, o.wmo->maxEntryKb);
            }
        }
        if (needKb && nowMs >= tr.memRetryMs && !heapFits(needKb + reserveKb, blockKb)) {
            tr.memRetryMs = nowMs + 5000;
            PSP_LOGW(pp::tag::Mem, "transport_lowmem pak=%s need_kb=%u block_kb=%u reserve_kb=%u objects=%u",
                     tr.slots[si].what, static_cast<unsigned>(needKb), static_cast<unsigned>(blockKb),
                     static_cast<unsigned>(reserveKb), static_cast<unsigned>(added.size()));
        }
        if (needKb && nowMs < tr.memRetryMs)
            for (uint64_t g : added)
                if (g != tr.riding) set.erase(std::find(set.begin(), set.end(), g));
    }
    // 4. Each renderer: a new set loads (then the old one goes), a moving one moves.
    for (int si = 0; si < kSlots; ++si) {
        TransportsUi::Slot& s = tr.slots[si];
        auto& set = sets[si];
        if (si == kSlotM2) {
            if (goState_ != 1) continue;
            if (!s.reserved) {  // the model tables at the pak's size (no reallocation under the grid's pointers)
                s.r.reserveTables(*s.pak);
                s.reserved = true;
            }
        } else {
            if (!set.empty() && s.state == 0) openSlot(tr, si);  // gameobjects_wmo.pak again (closed while idle)
            if (s.state <= 0) {
                s.set.clear();
                continue;
            }
        }
        // the old set while its placements could not be made (no memory: until retryMs) -
        // the objects of it still here (drawn, moved, holding the player).
        auto keepOld = [&] {
            set.clear();
            for (uint64_t g : s.set) {
                auto ko = tr.objs.find(g);
                if (ko != tr.objs.end() && (ko->second.wmo || ko->second.m2ok)) set.push_back(g);
            }
        };
        auto place = [&] {
            s.placed.clear();
            for (uint64_t g : set) placeParts(tr.objs[g], s.placed);
            for (auto& d : s.placed) d.uniqueId ^= s.idBit;
        };
        const bool hold = nowMs < s.retryMs;  // no new tile until then
        if (hold && set != s.set) keepOld();
        // a set with parts whose model did not load is placed again (reloadMs).
        const bool reload = !hold && set == s.set && s.reloadMs && nowMs >= s.reloadMs;
        const bool swap = !hold && (set != s.set || reload);
        if (swap) s.idBit ^= kIdFlip;  // the new tile's uniqueIds differ from the old one's: every part asks again
        place();
        if (swap) {
            const uint64_t t0 = sceKernelGetSystemTimeWide();
            const int newTile = s.tileFlip ? s.tileA : s.tileB, oldTile = s.curTile();
            s.r.viewDistance = std::max(models_.viewDistance, 300.0f);
            bool ok = true;
            if (!set.empty() || s.has) {
                // its result counts (was ignored: the old tile went, the set was taken as
                // placed - an object of it stayed out until the set changed again).
                ok = s.r.setPlacements(*s.pak, newTile, newTile, s.placed.data(),
                                       static_cast<uint32_t>(s.placed.size()));
                if (ok) {
                    if (s.has) s.r.unloadTile(*s.pak, oldTile, oldTile);  // + forgetFailed: the failed may load later
                    s.r.flushGrid();
                    s.pak->releaseFd();
                    s.tileFlip ^= 1u;
                    s.has = true;
                }
            }
            const auto& st = s.r.stats();
            if (!ok) {
                // No memory for the placements: the old set stays until the retry, nothing new comes in meanwhile
                // (the gate above waits for memRetryMs).
                PSP_LOGW(pp::tag::Mem, "transport_lowmem pak=%s placements=%u objects=%u (set not placed; the old "
                         "one stays, again in 5 s)", s.what, static_cast<unsigned>(s.placed.size()),
                         static_cast<unsigned>(set.size()));
                s.retryMs = nowMs + 5000;
                tr.memRetryMs = std::max(tr.memRetryMs, s.retryMs);
                s.idBit ^= kIdFlip;  // the old tile's ids
                keepOld();
                place();
                if (s.has && !s.placed.empty())
                    s.r.movePlacements(s.curTile(), s.curTile(), s.placed.data(),
                                       static_cast<uint32_t>(s.placed.size()));
            } else {
                // Parts whose model did not load (ModelRenderer::model gave up: a short heap) - placed again in 5 s,
                // kReloads times at most after a set change (unloadTile let the failed ones be read again).
                const uint32_t missing =
                    s.placed.size() > st.instances ? static_cast<uint32_t>(s.placed.size()) - st.instances : 0u;
                if (!reload) s.reloads = 0;
                s.reloadMs = 0;
                if (missing && s.reloads < kReloads) {
                    ++s.reloads;
                    s.reloadMs = nowMs + 5000;
                }
                s.set = set;
                if (++tr.logs <= 160)
                    PSP_LOGI(pp::tag::Gfx, "transports_set %s objects=%u parts=%u models=%u textures=%u kb=%u ms=%u "
                             "heap_used_kb=%u missing=%u reload=%u", s.what, static_cast<unsigned>(set.size()),
                             static_cast<unsigned>(s.placed.size()), static_cast<unsigned>(st.models),
                             static_cast<unsigned>(st.textures), static_cast<unsigned>(st.memoryKb),
                             static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u),
                             static_cast<unsigned>(pp::heapUsedKb()), static_cast<unsigned>(missing),
                             static_cast<unsigned>(reload ? s.reloads : 0u));
            }
        } else if (anyMoved && s.has && !s.placed.empty()) {
            s.r.movePlacements(s.curTile(), s.curTile(), s.placed.data(), static_cast<uint32_t>(s.placed.size()));
        }
        for (uint64_t g : set) tr.objs[g].placed = true;
        if (si == kSlotM2) continue;
        if (s.touched) {
            s.ownPak.releaseFd();  // the card's few descriptors - read only when an object or a set is new
            s.touched = false;
        }
        // gameobjects_wmo.pak: its TOC (~2.3k entries) only while one of its objects is near.
        if (si == kSlotStatic) {
            if (!s.set.empty()) {
                s.idleSinceMs = 0;
            } else if (!s.idleSinceMs) {
                s.idleSinceMs = nowMs;
            } else if (s.state > 0 && nowMs - s.idleSinceMs > kStaticIdleMs) {
                // (The objects keep their "gw_" records: the pak opens again when one comes near.)
                s.r.clear();
                s.ownPak.close();
                s.state = 0;
                s.has = false;
                s.reserved = false;
                s.set.clear();
                s.idleSinceMs = 0;
                s.reloadMs = 0;
                s.reloads = 0;
                PSP_LOGI(pp::tag::Gfx, "transports_static_close");
            }
        }
    }
    // 5. Riders: drawn where their transport carries them (the creatures' and players' entities).
    for (const auto& [guid, r] : session_.transportRiders()) {
        auto it = tr.objs.find(r.transport);
        if (it == tr.objs.end() || !it->second.placed) continue;
        const TransportsUi::Obj& o = it->second;
        glm::vec3 local = r.local;
        if (r.moveMs) {
            const float f =
                std::clamp(static_cast<float>(clock - r.moveStartMs) / static_cast<float>(r.moveMs), 0.0f, 1.0f);
            local = r.from + (r.local - r.from) * f;
        }
        const glm::vec3 world = o.pos + rotZ(o.yaw, local);
        auto e = session_.entities().getEntity(guid);
        if (!e) continue;
        const glm::vec3 c = core::coords::serverToCanonical(world);
        e->setPosition(c.x, c.y, c.z, core::coords::serverToCanonicalYaw(wrapAngle(o.yaw + r.localO)));
    }
    // A sample every 10 s while something is near.
    if (tr.anyResident() && nowMs >= tr.frameLogMs) {
        tr.frameLogMs = nowMs + 10000;
        const TransportsUi::Obj* closest = nullptr;
        float best = 1e30f;
        for (const auto& [guid, o] : tr.objs) {
            if (!o.placed || !o.transport) continue;
            const float d = glm::length(glm::vec2(o.pos.x - playerX_, o.pos.y - playerY_));
            if (d < best) {
                best = d;
                closest = &o;
            }
        }
        PSP_LOGI(pp::tag::Gfx, "transport_frame objects=%u wmo=%u static=%u m2=%u riding=%08X near_entry=%u dist=%.1f "
                 "x=%.1f y=%.1f z=%.1f o=%.2f trw_kb=%u trs_kb=%u trm_kb=%u", static_cast<unsigned>(tr.objs.size()),
                 static_cast<unsigned>(tr.slots[kSlotTransports].set.size()),
                 static_cast<unsigned>(tr.slots[kSlotStatic].set.size()), static_cast<unsigned>(tr.slots[kSlotM2].set.size()),
                 static_cast<unsigned>(tr.riding & 0xFFFFFFFFu), closest ? static_cast<unsigned>(closest->entry) : 0u,
                 closest ? best : 0.0f, closest ? closest->pos.x : 0.0f, closest ? closest->pos.y : 0.0f,
                 closest ? closest->pos.z : 0.0f, closest ? closest->yaw : 0.0f,
                 static_cast<unsigned>(tr.slots[kSlotTransports].r.stats().memoryKb),
                 static_cast<unsigned>(tr.slots[kSlotStatic].r.stats().memoryKb),
                 static_cast<unsigned>(tr.slots[kSlotM2].r.stats().memoryKb));
    }
}

void WorldScreen::drawTransports(const rp::TerrainCamera& cam, uint32_t fogABGR) {
    if (!tr_ || tr_->off) return;
    PROF_SCOPE("draw.transports");
    for (auto& s : tr_->slots)
        if (!s.set.empty()) s.r.draw(cam, fogABGR);
}

bool WorldScreen::transportFloor(float x, float y, float maxZ, float& z) const {
    if (!tr_ || tr_->off) return false;
    bool found = false;
    float best = -1e30f, h = 0.0f;
    for (const auto& s : tr_->slots) {
        if (!s.set.empty() && s.r.floorHeight(x, y, maxZ, h) && h > best) {
            best = h;
            found = true;
        }
    }
    if (found) z = best;
    return found;
}

void WorldScreen::transportWalls(glm::vec3& to, float feetZ) const {
    if (!tr_ || tr_->off) return;
    for (const auto& s : tr_->slots)
        if (!s.set.empty()) s.r.collideWalls(to, feetZ);
}

float WorldScreen::transportCameraRay(const glm::vec3& origin, const glm::vec3& dir, float maxDist) const {
    if (!tr_ || tr_->off) return maxDist;
    float best = maxDist;
    for (int si : {kSlotTransports, kSlotStatic}) {  // WMO groups (upstream's camera stops at WMOs only)
        const auto& s = tr_->slots[si];
        if (!s.set.empty()) best = s.r.raycastBuildings(origin, dir, best);
    }
    return best;
}

bool WorldScreen::transportRemotePos(uint64_t guid, glm::vec3& pos, bool& moving) const {
    if (!tr_ || tr_->off) return false;
    const auto& riders = session_.transportRiders();
    auto it = riders.find(guid);
    if (it == riders.end()) return false;
    auto ot = tr_->objs.find(it->second.transport);
    if (ot == tr_->objs.end() || !ot->second.placed) return false;
    auto e = session_.entities().getEntity(guid);
    if (!e) return false;
    pos = core::coords::canonicalToServer(glm::vec3(e->getX(), e->getY(), e->getZ()));  // set this frame
    using MF = game::MovementFlags;
    const uint32_t dirBits = static_cast<uint32_t>(MF::FORWARD) | static_cast<uint32_t>(MF::BACKWARD) |
                             static_cast<uint32_t>(MF::STRAFE_LEFT) | static_cast<uint32_t>(MF::STRAFE_RIGHT);
    const uint32_t clock = pp::WorldSession::transportClockMs();
    moving = (it->second.moveFlags & dirBits) != 0 ||
             (it->second.moveMs && clock - it->second.moveStartMs < it->second.moveMs);
    return true;
}

// sendMove: riding -> MOVEMENTFLAG_ONTRANSPORT, the transport's guid and the place in its frame (x y z o), the time
// (the client writes its own clock there; upstream sendMovement: transportTime = time), seat -1 (no vehicle).
void WorldScreen::transportMoveInfo(game::MovementInfo& mi) {
    if (!tr_ || !tr_->riding) return;
    auto it = tr_->objs.find(tr_->riding);
    if (it == tr_->objs.end() || !it->second.placed) {
        if (!tr_->placePending) return;
        // Not placed yet (a transfer on a ship): the server's own place on it.
        mi.flags |= static_cast<uint32_t>(game::MovementFlags::ONTRANSPORT);
        mi.transportGuid = tr_->riding;
        mi.transportX = tr_->local.x;
        mi.transportY = tr_->local.y;
        mi.transportZ = tr_->local.z;
        mi.transportO = tr_->localO;
        mi.transportTime = pp::WorldSession::transportClockMs();
        mi.transportSeat = -1;
        return;
    }
    const TransportsUi::Obj& o = it->second;
    const glm::vec3 local = rotZ(-o.yaw, glm::vec3(mi.x, mi.y, mi.z) - o.pos);
    mi.flags |= static_cast<uint32_t>(game::MovementFlags::ONTRANSPORT);
    mi.transportGuid = tr_->riding;
    mi.transportX = local.x;
    mi.transportY = local.y;
    mi.transportZ = local.z;
    mi.transportO = wrapAngle(mi.orientation - o.yaw);
    mi.transportTime = pp::WorldSession::transportClockMs();
    mi.transportSeat = -1;
}

bool WorldScreen::transportCarry(float dtSec) {
    (void)dtSec;
    if (!tr_ || tr_->off) return false;
    TransportsUi& tr = *tr_;
    uint64_t g = 0;
    glm::vec3 l(0.0f);
    float lo = 0.0f;
    if (session_.takeOwnTransportPlace(g, l, lo)) {  // the server's word: we stand on it there
        tr.riding = g;
        tr.local = l;
        tr.localO = lo;
        tr.placePending = true;
        tr.heldSinceMs = pp::logUptimeMs();
        session_.setOwnTransport(g, l, lo);
        PSP_LOGI(pp::tag::Gfx, "transport_board guid=%08X x=%.2f y=%.2f z=%.2f why=server", static_cast<unsigned>(g),
                 l.x, l.y, l.z);
    }
    if (!tr.riding) return false;
    const uint32_t nowMs = pp::logUptimeMs();
    auto it = tr.objs.find(tr.riding);
    const bool known = it != tr.objs.end();
    if (!known || !it->second.placed) {
        // Its place is not known yet (a transfer on a ship: its create block comes after ours), or it left this map
        // (the server moves us with it: SMSG_TRANSFER_PENDING): held where we are for a while, then let go. A
        // transport that is here but cannot be drawn (no model) is let go at once.
        const bool offMap = known && !it->second.onMap;
        const bool hold = offMap || (tr.placePending && !known);
        if (!tr.heldSinceMs) tr.heldSinceMs = nowMs;
        if (!hold || nowMs - tr.heldSinceMs > kLostMs) {
            PSP_LOGW(pp::tag::Gfx, "transport_leave guid=%08X why=%s", static_cast<unsigned>(tr.riding),
                     offMap ? "off_map" : known ? "not_placed" : "unknown");
            tr.riding = 0;
            tr.placePending = false;
            tr.heldSinceMs = 0;
            session_.setOwnTransport(0, glm::vec3(0.0f), 0.0f);
            return false;
        }
        return true;  // hold still (no input, no gravity) until it is placed again
    }
    tr.heldSinceMs = 0;
    const TransportsUi::Obj& o = it->second;
    const glm::vec3 world = o.pos + rotZ(o.yaw, tr.local);
    const float facing = wrapAngle(o.yaw + tr.localO);
    const float dyaw = wrapAngle(facing - facing_);
    const glm::vec3 delta = world - glm::vec3(playerX_, playerY_, playerZ_);
    playerX_ = world.x;
    playerY_ = world.y;
    playerZ_ = world.z;
    facing_ = facing;
    sentFacing_ += dyaw;  // the transport's turn is no turn of ours to tell the server
    if (camInit_) {
        smoothedCam_ += delta;  // the camera's easing rides along (no lag behind a ship)
        shownZ_ += delta.z;     // the drawn feet too (a lift does not "climb steps")
    }
    if (tr.placePending) {
        tr.placePending = false;
        PSP_LOGI(pp::tag::Gfx, "transport_placed guid=%08X x=%.1f y=%.1f z=%.1f", static_cast<unsigned>(tr.riding),
                 world.x, world.y, world.z);
    }
    return false;
}

void WorldScreen::transportAfterMove() {
    if (!tr_ || tr_->off) return;
    TransportsUi& tr = *tr_;
    if (tr.placePending) return;  // held: the place is the server's until the transport is placed
    const glm::vec3 me(playerX_, playerY_, playerZ_);
    uint64_t now = 0;
    if (!airborne_) {
        // Standing on a transport's floor (its collision under the feet) -> that transport.
        float tz = 0.0f;
        if (transportFloor(me.x, me.y, me.z + 0.25f, tz) && std::fabs(tz - me.z) < 0.05f) {
            float bestVol = 1e30f;
            for (const auto& [guid, o] : tr.objs) {
                if (!o.transport || !o.placed || !insideBox(o, me, 1.0f, 3.0f)) continue;
                const glm::vec3 ext = (o.boxMax - o.boxMin) * o.scale;
                const float vol = ext.x * ext.y * ext.z;
                if (vol < bestVol) {
                    bestVol = vol;
                    now = guid;
                }
            }
        }
    } else if (tr.riding) {
        // In the air (a jump on the deck): still riding while inside its bounds.
        auto it = tr.objs.find(tr.riding);
        if (it != tr.objs.end() && it->second.placed && insideBox(it->second, me, 2.0f, 8.0f)) now = tr.riding;
    }
    if (now != tr.riding) {
        const uint64_t was = tr.riding;
        tr.riding = now;
        if (now) {
            ++tr.boardings;
            const auto& o = tr.objs[now];
            PSP_LOGI(pp::tag::Gfx, "transport_board guid=%08X entry=%u type=%u x=%.1f y=%.1f z=%.1f n=%u",
                     static_cast<unsigned>(now), static_cast<unsigned>(o.entry), static_cast<unsigned>(o.type), me.x,
                     me.y, me.z, static_cast<unsigned>(tr.boardings));
        } else {
            PSP_LOGI(pp::tag::Gfx, "transport_leave guid=%08X x=%.1f y=%.1f z=%.1f", static_cast<unsigned>(was), me.x,
                     me.y, me.z);
        }
    }
    if (tr.riding) {
        const auto& o = tr.objs[tr.riding];
        tr.local = rotZ(-o.yaw, me - o.pos);
        tr.localO = wrapAngle(facing_ - o.yaw);
    }
    if (session_.ownTransport() != tr.riding) tr.beatDue = true;
    session_.setOwnTransport(tr.riding, tr.local, tr.localO);
    // While riding (and once after leaving) a heartbeat every 0.5 s, on the movement's own heartbeat timer: the
    // server hears a boarding or a leaving within that time, also from a player standing still on a lift.
    const uint32_t nowMs = pp::logUptimeMs();
    if ((tr.riding || tr.beatDue) && nowMs >= nextHeartbeatMs_) {
        nextHeartbeatMs_ = nowMs + 500;
        tr.beatDue = false;
        sendMove(game::Opcode::MSG_MOVE_HEARTBEAT, moving_);
    }
}

}  // namespace wowee::ui::psp
