// flight paths - the session half.
//
// Upstream, ported without GameHandler (logic unchanged unless marked PSP):
//   - MovementHandler::handleShowTaxiNodes (SMSG_SHOWTAXINODES: the known-node mask, the window, closeGossip,
//     buildTaxiCostMap), buildTaxiCostMap (BFS over TaxiPath through known nodes only), getTaxiCostTo /
//     hasTaxiRouteTo / getTaxiRouteTo, activateTaxi (dismount, the route, CMSG_ACTIVATETAXIEXPRESS for more
//     than one hop / CMSG_ACTIVATETAXI, the window closes on the request, a heartbeat, the client path built
//     as pending data), handleActivateTaxiReply (success: the flight starts - beginTaxiFlightMotion; failure:
//     the error), closeTaxi, startClientTaxiPath (TaxiPathNode points of every hop, the arc length of each
//     Catmull-Rom segment), beginTaxiFlightMotion, updateClientTaxi (the reply timeout, 32 yd/s along the curve
//     paced by arc length, the orientation eased at 3/s, a deferred server completion consumed in the landing
//     radius), finishClientTaxiFlight (the landing at the destination's TaxiNodes position, or where the server
//     stopped us beyond 300 yd; MSG_MOVE_STOP + HEARTBEAT), isNearTaxiDestination, sanitizeMovementForTaxi
//     (the flight's movement block has no movement flags), forceClearTaxiAndMovementState (a teleport);
//   - GameHandler: SMSG_TAXINODE_STATUS (taxiNpcHasRoutes_), SMSG_NEW_TAXI_PATH, SMSG_DISMOUNT during a flight
//     (UNIT_FLAG_TAXI_FLIGHT still set or not near the landing: deferred, else the flight ends where we are),
//     updateTaxiAndMountState (landing cooldown, start grace, the taxi-mount safety), the 0.25 s taxi
//     heartbeat (HEARTBEAT_INTERVAL_TAXI - sent by the world screen), closeIfTooFar for the taxi window;
//   - EntityController: detectPlayerMountChange's taxi guard (the mount field cleared mid-flight), the own create
//     block with UNIT_FLAG_TAXI_FLIGHT (logged in during a flight).
// PSP differences (marked PSP in the code):
//   - the tables are data/taxi.pak (TaxiNodes / TaxiPath / TaxiPathNode of one continent, baked by
//     tools/psp/build_taxi_pak.py), read for the map the player is on when the window opens and freed once the
//     window is closed and no flight is on; a flight reads only its hops' TaxiPathNode points (pakRead by range);
//   - positions stay in server coordinates (the PSP world screen's), upstream converts to canonical - the same
//     curve, x and y swapped;
//   - the flight's mount is the server's UNIT_FIELD_MOUNTDISPLAYID (WorldSession::SendDoFlight -> Unit::Mount,
//     as the original client reads it), shown by the existing mount code; upstream guesses a display from TaxiNodes /
//     the gryphon model (applyTaxiMountForCurrentNode) - here only taxiMountActive_ is kept. A mount field that
//     arrives inside the dismount grace while a flight starts is taken (the grace is for our own dismount);
//   - upstream's English chat lines ("Requesting flight to ...", "Discovered flight path: ...", "No flight path
//     available ...") are log lines; the lines shown are GlobalStrings ruRU by the game line rule
//     (psp_game_line.hpp): ERR_NEWTAXIPATH is news (system chat, as upstream's line), ERR_TAXI* red by the reply's
//     code (upstream shows one English line for every failure);
//   - no mount pitch / roll (upstream's taxiOrientationCallback): the PSP draws a yaw only;
//   - logged in mid-flight (no client path): our own SMSG_MONSTER_MOVE spline is followed (upstream applies it
//     to the player entity and syncs the renderer from it while on a taxi; the PSP has no own entity), and the
//     server's dismount ends it.
#include "platform/psp/psp_world_session.hpp"

#include "core/coordinates.hpp"
#include "game/protocol_constants.hpp"
#include "game/update_field_table.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_strings.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_system.hpp"

#include <pspkernel.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>

// The upstream Entity spline (inlined) trips GCC 15's false -Wmaybe-uninitialized (as psp_world_session_move.cpp).
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"

namespace wowee::platform::psp {

using game::LogicalOpcode;

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kTaxiActivateReplyTimeoutSeconds = 8.0f;  // upstream MovementHandler
constexpr float kAuthoritativeLandingSnapDist = 300.0f;   // upstream finishClientTaxiFlight

// data/taxi.pak "taxi_<map>" (tools/psp/build_taxi_pak.py).
struct TaxiPakHeader {
    char magic[4];  // "WTXI"
    uint32_t version, mapId, nodeCount, edgeCount, waypointCount;
    float taxiMinX, taxiMinY, taxiMaxX, taxiMaxY;
    uint32_t namesSize, reserved;
};
struct TaxiPakNode {
    uint32_t id;
    float x, y, z;
    uint32_t nameOff, mountH, mountA;
};
struct TaxiPakEdge {
    uint16_t from, to;
    uint32_t pathId, cost, wpFirst, wpCount;
};
static_assert(sizeof(TaxiPakHeader) == 48 && sizeof(TaxiPakNode) == 28 && sizeof(TaxiPakEdge) == 20,
              "taxi.pak records");

// ActivateTaxiReply (the client's enum) -> GlobalStrings (strings.pak).
const char* taxiReplyText(uint32_t code) {
    switch (code) {
        case 1: return CLIENT_STR(ERR_TAXIUNSPECIFIEDSERVERERROR);
        case 2: return CLIENT_STR(ERR_TAXINOSUCHPATH);
        case 3: return CLIENT_STR(ERR_TAXINOTENOUGHMONEY);
        case 4: return CLIENT_STR(ERR_TAXITOOFARAWAY);
        case 5: return CLIENT_STR(ERR_TAXINOVENDORNEARBY);
        case 6: return CLIENT_STR(ERR_TAXINOTVISITED);
        case 7: return CLIENT_STR(ERR_TAXIPLAYERBUSY);
        case 8: return CLIENT_STR(ERR_TAXIPLAYERALREADYMOUNTED);
        case 9: return CLIENT_STR(ERR_TAXIPLAYERSHAPESHIFTED);
        case 10: return CLIENT_STR(ERR_TAXIPLAYERMOVING);
        case 11: return CLIENT_STR(ERR_TAXISAMENODE);
        case 12: return CLIENT_STR(ERR_TAXINOTSTANDING);
        default: return CLIENT_STR(ERR_TAXIUNSPECIFIEDSERVERERROR);
    }
}

// upstream MovementHandler::evalTaxiCatmullRom.
glm::vec3 evalTaxiCatmullRom(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3,
                             float t) {
    const float t2 = t * t;
    const float t3 = t2 * t;
    return 0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                   (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

bool openTaxiPak(Pak& pak) {
    char path[256];
    fsPath(path, sizeof(path), "data/taxi.pak");
    return pak.open(path);
}

}  // namespace

// --- Tables ----------------------------------------------------------------------------------

// PSP: upstream loadTaxiDbc, one continent out of data/taxi.pak.
bool WorldSession::loadTaxiMap(uint32_t mapId) {
    if (taxiMapId_ == mapId && !taxiNodes_.empty()) return true;
    releaseTaxiData();
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    Pak pak;
    if (!openTaxiPak(pak)) {
        PSP_LOGW(tag::Net, "taxi_pak missing (data/taxi.pak: tools/psp/build_taxi_pak.py)");
        return false;
    }
    char name[32];
    std::snprintf(name, sizeof(name), "taxi_%u", static_cast<unsigned>(mapId));
    const PakEntry* e = pak.find(name);
    auto* blob = e ? static_cast<const uint8_t*>(pak.load(e)) : nullptr;
    const uint32_t blobSize = e ? e->size : 0u;  // (the TOC goes with close())
    std::snprintf(name, sizeof(name), "taxiwp_%u", static_cast<unsigned>(mapId));
    const PakEntry* w = pak.find(name);
    if (w) {
        taxiWpOffset_ = w->offset;
        taxiWpCount_ = w->size / 12u;
    }
    pak.close();
    if (!blob) {
        PSP_LOGW(tag::Net, "taxi_pak no map=%u", static_cast<unsigned>(mapId));
        return false;
    }
    TaxiPakHeader h;
    std::memcpy(&h, blob, sizeof(h));
    const size_t need = sizeof(h) + h.nodeCount * sizeof(TaxiPakNode) + h.edgeCount * sizeof(TaxiPakEdge) + h.namesSize;
    if (std::memcmp(h.magic, "WTXI", 4) != 0 || h.version != 1 || blobSize < need) {
        PSP_LOGW(tag::Net, "taxi_pak bad map=%u size=%u", static_cast<unsigned>(mapId), static_cast<unsigned>(blobSize));
        pakFree(const_cast<uint8_t*>(blob));
        return false;
    }
    const uint8_t* p = blob + sizeof(h);
    taxiNodes_.resize(h.nodeCount);
    for (uint32_t i = 0; i < h.nodeCount; ++i, p += sizeof(TaxiPakNode)) {
        TaxiPakNode n;
        std::memcpy(&n, p, sizeof(n));
        TaxiNode& t = taxiNodes_[i];
        t.id = n.id;
        t.x = n.x;
        t.y = n.y;
        t.z = n.z;
        t.nameOff = n.nameOff;
        t.mountCreature[0] = n.mountH;
        t.mountCreature[1] = n.mountA;
    }
    taxiEdges_.resize(h.edgeCount);
    for (uint32_t i = 0; i < h.edgeCount; ++i, p += sizeof(TaxiPakEdge)) {
        TaxiPakEdge ed;
        std::memcpy(&ed, p, sizeof(ed));
        taxiEdges_[i] = TaxiEdge{ed.from, ed.to, ed.pathId, ed.cost, ed.wpFirst, ed.wpCount};
    }
    taxiNames_.assign(reinterpret_cast<const char*>(p), reinterpret_cast<const char*>(p) + h.namesSize);
    if (taxiNames_.empty() || taxiNames_.back() != '\0') taxiNames_.push_back('\0');
    taxiBounds_[0] = h.taxiMinX;
    taxiBounds_[1] = h.taxiMinY;
    taxiBounds_[2] = h.taxiMaxX;
    taxiBounds_[3] = h.taxiMaxY;
    taxiMapId_ = mapId;
    pakFree(const_cast<uint8_t*>(blob));
    PSP_LOGI(tag::Net, "taxi_load map=%u nodes=%u edges=%u waypoints=%u kb=%u ms=%u", static_cast<unsigned>(mapId),
             static_cast<unsigned>(h.nodeCount), static_cast<unsigned>(h.edgeCount), static_cast<unsigned>(taxiWpCount_),
             static_cast<unsigned>((taxiDataBytes() + 1023u) / 1024u),
             static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u));
    return true;
}

uint32_t WorldSession::taxiDataBytes() const {
    return static_cast<uint32_t>(taxiNodes_.capacity() * sizeof(TaxiNode) + taxiEdges_.capacity() * sizeof(TaxiEdge) +
                                 taxiNames_.capacity() + taxiClientPath_.capacity() * sizeof(glm::vec3) +
                                 taxiClientSegmentArcLengths_.capacity() * sizeof(float) +
                                 (taxiCostMap_.size() + taxiPrevMap_.size()) * 16u);
}

void WorldSession::releaseTaxiData() {
    if (taxiNodes_.empty() && taxiEdges_.empty()) return;
    const uint32_t kb = (taxiDataBytes() + 1023u) / 1024u;
    std::vector<TaxiNode>().swap(taxiNodes_);
    std::vector<TaxiEdge>().swap(taxiEdges_);
    std::vector<char>().swap(taxiNames_);
    taxiCostMap_.clear();
    taxiPrevMap_.clear();
    taxiMapId_ = ~0u;
    PSP_LOGI(tag::Net, "taxi_free kb=%u", static_cast<unsigned>(kb));
}

const WorldSession::TaxiNode* WorldSession::taxiNode(uint32_t id) const {
    auto it = std::lower_bound(taxiNodes_.begin(), taxiNodes_.end(), id,
                               [](const TaxiNode& n, uint32_t v) { return n.id < v; });
    return it != taxiNodes_.end() && it->id == id ? &*it : nullptr;
}

const char* WorldSession::taxiNodeName(const TaxiNode& n) const {
    return n.nameOff < taxiNames_.size() ? taxiNames_.data() + n.nameOff : "";
}

bool WorldSession::taxiMapBounds(float& minX, float& minY, float& maxX, float& maxY) const {
    if (taxiNodes_.empty() || taxiBounds_[2] <= taxiBounds_[0] || taxiBounds_[3] <= taxiBounds_[1]) return false;
    minX = taxiBounds_[0];
    minY = taxiBounds_[1];
    maxX = taxiBounds_[2];
    maxY = taxiBounds_[3];
    return true;
}

bool WorldSession::isKnownTaxiNode(uint32_t nodeId) const {
    if (nodeId == 0 || nodeId > game::TLK_TAXI_MASK_SIZE * 32u) return false;
    const uint32_t idx = nodeId - 1;
    return (knownTaxiMask_[idx / 32] & (1u << (idx % 32))) != 0;
}

bool WorldSession::taxiNpcHasRoutes(uint64_t guid) const {
    auto it = taxiNpcHasRoutes_.find(guid);
    return it != taxiNpcHasRoutes_.end() && it->second;
}

// --- Packets ---------------------------------------------------------------------------------

bool WorldSession::handleTaxiPacket(LogicalOpcode op, network::Packet& packet) {
    if (state_ != State::InWorld) return false;
    switch (op) {
        case LogicalOpcode::SMSG_SHOWTAXINODES: handleShowTaxiNodes(packet); return true;
        case LogicalOpcode::SMSG_ACTIVATETAXIREPLY: handleActivateTaxiReply(packet); return true;
        case LogicalOpcode::SMSG_NEW_TAXI_PATH:
            // news -> a system chat line (game line rule; upstream: a system chat line too).
            gameLine(GameLine::News, CLIENT_STR(ERR_NEWTAXIPATH));
            PSP_LOGI(tag::Net, "taxi_new_path");
            return true;
        case LogicalOpcode::SMSG_TAXINODE_STATUS:
            // guid(8) + status(1): 1 = the flight master has routes for us (upstream taxiNpcHasRoutes_).
            if (packet.hasRemaining(9)) {
                const uint64_t npc = packet.readUInt64();
                const uint8_t status = packet.readUInt8();
                taxiNpcHasRoutes_[npc] = status != 0;
                if (++taxiLogs_ <= 60)
                    PSP_LOGI(tag::Net, "taxi_node_status npc=%08X status=%u", static_cast<unsigned>(npc & 0xFFFFFFFFu),
                             static_cast<unsigned>(status));
            }
            return true;
        case LogicalOpcode::SMSG_DISMOUNT: {
            // upstream GameHandler SMSG_DISMOUNT: only ours during a flight is ours to judge here (the rest:
            // the mount field says the same on a 3.3.5a server, as before).
            if (!onTaxiFlight_ || !packet.hasRemaining(1)) return false;
            const size_t rp = packet.getReadPos();
            const uint64_t guid = packet.readPackedGuid();
            if (guid != playerGuid_) {
                packet.setReadPos(rp);
                return false;
            }
            const bool still = serverStillTaxiing(nullptr);
            const bool near = isNearTaxiDestination();
            PSP_LOGI(tag::Net, "taxi_dismount_srv still=%d near=%d client=%d", still ? 1 : 0, near ? 1 : 0,
                     taxiClientActive_ ? 1 : 0);
            if (taxiClientActive_ && (still || !near)) {
                deferServerTaxiCompletion();  // premature (CMaNGOS): land in the landing radius
                return true;
            }
            finishClientTaxiFlight(false);  // authoritative: the server stopped the flight
            return true;
        }
        default: return false;
    }
}

// upstream MovementHandler::handleShowTaxiNodes.
void WorldSession::handleShowTaxiNodes(network::Packet& packet) {
    game::ShowTaxiNodesData data;
    if (!game::ShowTaxiNodesParser::parse(packet, data)) {
        PSP_LOGW(tag::Net, "taxi_show_bad size=%u", static_cast<unsigned>(packet.getSize()));
        return;
    }
    const bool loaded = loadTaxiMap(worldEntry_.mapId);  // PSP: the continent the player is on
    if (taxiMaskInitialized_) {
        for (uint32_t i = 0; i < game::TLK_TAXI_MASK_SIZE; ++i) {
            const uint32_t newBits = data.nodeMask[i] & ~knownTaxiMask_[i];
            for (uint32_t bit = 0; bit < 32 && newBits; ++bit) {
                if (!(newBits & (1u << bit))) continue;
                const uint32_t nodeId = i * 32 + bit + 1;
                const TaxiNode* n = taxiNode(nodeId);
                // upstream: "Discovered flight path: <name>" (English, chat) - the PSP logs it.
                PSP_LOGI(tag::Net, "taxi_discovered node=%u name=%s", static_cast<unsigned>(nodeId),
                         n ? taxiNodeName(*n) : "?");
            }
        }
    }
    for (uint32_t i = 0; i < game::TLK_TAXI_MASK_SIZE; ++i) knownTaxiMask_[i] = data.nodeMask[i];
    taxiMaskInitialized_ = true;
    currentTaxiData_ = data;
    taxiNpcGuid_ = data.npcGuid;
    taxiWindowOpen_ = true;
    closeGossip();
    buildTaxiCostMap();
    uint32_t known = 0;
    for (const auto& n : taxiNodes_) known += isKnownTaxiNode(n.id) ? 1u : 0u;
    PSP_LOGI(tag::Net, "taxi_open npc=%08X nearest=%u map=%u loaded=%d nodes=%u known=%u reachable=%u",
             static_cast<unsigned>(data.npcGuid & 0xFFFFFFFFu), static_cast<unsigned>(data.nearestNode),
             static_cast<unsigned>(worldEntry_.mapId), loaded ? 1 : 0, static_cast<unsigned>(taxiNodes_.size()),
             static_cast<unsigned>(known), static_cast<unsigned>(taxiCostMap_.empty() ? 0 : taxiCostMap_.size() - 1));
}

// upstream MovementHandler::buildTaxiCostMap.
void WorldSession::buildTaxiCostMap() {
    taxiCostMap_.clear();
    taxiPrevMap_.clear();
    const uint32_t startNode = currentTaxiData_.nearestNode;
    if (startNode == 0) return;
    struct AdjEntry {
        uint32_t node;
        uint32_t cost;
    };
    std::unordered_map<uint32_t, std::vector<AdjEntry>> adj;
    for (const auto& edge : taxiEdges_) adj[edge.from].push_back({edge.to, edge.cost});
    std::deque<uint32_t> queue;
    queue.push_back(startNode);
    taxiCostMap_[startNode] = 0;
    while (!queue.empty()) {
        const uint32_t cur = queue.front();
        queue.pop_front();
        for (const auto& next : adj[cur]) {
            // Flights only route through nodes the player has discovered (ERR_TAXINOTVISITED otherwise).
            if (!currentTaxiData_.isNodeKnown(next.node)) continue;
            if (taxiCostMap_.find(next.node) == taxiCostMap_.end()) {
                taxiCostMap_[next.node] = taxiCostMap_[cur] + next.cost;
                taxiPrevMap_[next.node] = cur;
                queue.push_back(next.node);
            }
        }
    }
}

uint32_t WorldSession::getTaxiCostTo(uint32_t destNodeId) const {
    auto it = taxiCostMap_.find(destNodeId);
    return it != taxiCostMap_.end() ? it->second : 0;
}

bool WorldSession::hasTaxiRouteTo(uint32_t destNodeId) const {
    return destNodeId != currentTaxiData_.nearestNode && taxiCostMap_.find(destNodeId) != taxiCostMap_.end();
}

std::vector<uint32_t> WorldSession::getTaxiRouteTo(uint32_t destNodeId) const {
    std::vector<uint32_t> route;
    if (!hasTaxiRouteTo(destNodeId)) return route;
    const uint32_t startNode = currentTaxiData_.nearestNode;
    uint32_t cur = destNodeId;
    while (cur != startNode) {
        route.push_back(cur);
        auto it = taxiPrevMap_.find(cur);
        if (it == taxiPrevMap_.end() || route.size() > taxiPrevMap_.size()) return {};
        cur = it->second;
    }
    route.push_back(startNode);
    std::reverse(route.begin(), route.end());
    return route;
}

void WorldSession::taxiQueryAvailableNodes(uint64_t npcGuid) {
    if (state_ != State::InWorld || !socket_ || npcGuid == 0) return;
    network::Packet p(game::wireOpcode(LogicalOpcode::CMSG_TAXIQUERYAVAILABLENODES));
    p.writeUInt64(npcGuid);
    socket_->send(p);
    PSP_LOGI(tag::Net, "taxi_query npc=%08X", static_cast<unsigned>(npcGuid & 0xFFFFFFFFu));
}

// upstream MovementHandler::closeTaxi.
void WorldSession::closeTaxi() {
    taxiWindowOpen_ = false;
    if (taxiActivatePending_ || onTaxiFlight_ || taxiClientActive_) return;
    taxiMountActive_ = false;
    taxiActivatePending_ = false;
    onTaxiFlight_ = false;
    taxiLandingCooldown_ = 2.0f;
}

void WorldSession::taxiSendMove(game::Opcode opcode) {
    // upstream sanitizeMovementForTaxi: no direction / turn / pitch / fall / swim flags, no fall data.
    game::MovementInfo mi;
    mi.flags = 0;
    mi.x = taxiPos_.x;
    mi.y = taxiPos_.y;
    mi.z = taxiPos_.z;
    mi.orientation = taxiOrientation_;
    sendMovement(opcode, mi);
}

// upstream MovementHandler::activateTaxi.
void WorldSession::activateTaxi(uint32_t destNodeId) {
    if (!socket_ || state_ != State::InWorld) return;
    if (taxiActivatePending_ || onTaxiFlight_) return;
    const uint32_t startNode = currentTaxiData_.nearestNode;
    if (startNode == 0 || destNodeId == 0 || startNode == destNodeId) return;
    taxiDestNodeId_ = destNodeId;
    if (isMounted()) {
        PSP_LOGI(tag::Net, "taxi_activate dismount display=%u", static_cast<unsigned>(currentMountDisplayId_));
        dismount();
    }
    const TaxiNode* dest = taxiNode(destNodeId);
    taxiDestName_ = dest ? taxiNodeName(*dest) : "";
    // The route the cost map worked out (through discovered nodes only).
    const std::vector<uint32_t> path = getTaxiRouteTo(destNodeId);
    if (path.size() < 2) {
        // upstream: "No flight path available to that destination." (chat) - the PSP logs it.
        PSP_LOGW(tag::Net, "taxi_activate no_route start=%u dest=%u", static_cast<unsigned>(startNode),
                 static_cast<unsigned>(destNodeId));
        return;
    }
    const uint32_t totalCost = getTaxiCostTo(destNodeId);
    // More than one hop: CMSG_ACTIVATETAXIEXPRESS with the whole path (CMSG_ACTIVATETAXI names one TaxiPath).
    if (path.size() > 2) socket_->send(game::ActivateTaxiExpressPacket::build(taxiNpcGuid_, path));
    else socket_->send(game::ActivateTaxiPacket::build(taxiNpcGuid_, startNode, destNodeId));
    char route[160];
    size_t o = 0;
    for (size_t i = 0; i < path.size() && o + 12 < sizeof(route); ++i)
        o += static_cast<size_t>(std::snprintf(route + o, sizeof(route) - o, i ? "-%u" : "%u",
                                               static_cast<unsigned>(path[i])));
    PSP_LOGI(tag::Net, "taxi_activate npc=%08X start=%u dest=%u hops=%u cost=%u express=%d path=%s",
             static_cast<unsigned>(taxiNpcGuid_ & 0xFFFFFFFFu), static_cast<unsigned>(startNode),
             static_cast<unsigned>(destNodeId), static_cast<unsigned>(path.size() - 1), static_cast<unsigned>(totalCost),
             path.size() > 2 ? 1 : 0, route);
    // The window closes on the request, not on the reply (upstream).
    taxiWindowOpen_ = false;
    taxiActivatePending_ = true;
    taxiActivateTimer_ = 0.0f;
    // Nothing starts here (the server may refuse): the path is built as pending data.
    if (lastOwnMove_.time != 0) {  // where the player stands (the last movement sent), no movement flags
        game::MovementInfo mi = lastOwnMove_;
        mi.flags = 0;
        sendMovement(game::Opcode::MSG_MOVE_HEARTBEAT, mi);
    }
    startClientTaxiPath(path);
}

// PSP: the TaxiPathNode points of the route's hops, read by range out of "taxiwp_<map>".
bool WorldSession::loadTaxiWaypoints(const std::vector<uint32_t>& pathNodes) {
    if (taxiWpCount_ == 0) return false;
    Pak pak;
    if (!openTaxiPak(pak)) return false;
    bool any = false;
    for (size_t i = 0; i + 1 < pathNodes.size(); ++i) {
        const TaxiEdge* edge = nullptr;
        for (const auto& e : taxiEdges_)
            if (e.from == pathNodes[i] && e.to == pathNodes[i + 1]) {
                edge = &e;
                break;
            }
        if (!edge) {
            PSP_LOGW(tag::Net, "taxi_path none from=%u to=%u", static_cast<unsigned>(pathNodes[i]),
                     static_cast<unsigned>(pathNodes[i + 1]));
            continue;
        }
        if (edge->wpCount == 0 || edge->wpFirst + edge->wpCount > taxiWpCount_) {
            PSP_LOGW(tag::Net, "taxi_path no_points path=%u", static_cast<unsigned>(edge->pathId));
            continue;
        }
        std::vector<float> pts(edge->wpCount * 3u);
        if (!pakRead(pak.fd(), taxiWpOffset_ + edge->wpFirst * 12u, pts.data(), edge->wpCount * 12u)) continue;
        for (uint32_t k = 0; k < edge->wpCount; ++k)
            taxiClientPath_.push_back(glm::vec3(pts[k * 3], pts[k * 3 + 1], pts[k * 3 + 2]));
        any = true;
    }
    pak.close();
    return any;
}

// upstream MovementHandler::startClientTaxiPath.
void WorldSession::startClientTaxiPath(const std::vector<uint32_t>& pathNodes) {
    taxiClientPath_.clear();
    taxiClientIndex_ = 0;
    taxiClientActive_ = false;
    taxiClientSegmentProgress_ = 0.0f;
    loadTaxiWaypoints(pathNodes);
    if (taxiClientPath_.size() < 2) {
        taxiClientPath_.clear();
        for (uint32_t nodeId : pathNodes)
            if (const TaxiNode* n = taxiNode(nodeId)) taxiClientPath_.push_back(glm::vec3(n->x, n->y, n->z));
    }
    if (taxiClientPath_.size() < 2) {
        PSP_LOGW(tag::Net, "taxi_path too_short points=%u", static_cast<unsigned>(taxiClientPath_.size()));
        return;
    }
    // The real arc length of each Catmull-Rom segment (16 samples): the pacing matches the server's spline.
    taxiClientSegmentArcLengths_.clear();
    taxiClientSegmentArcLengths_.reserve(taxiClientPath_.size() - 1);
    constexpr int kArcLengthSamples = 16;
    float total = 0.0f;
    for (size_t i = 0; i + 1 < taxiClientPath_.size(); i++) {
        const glm::vec3 p0 = (i > 0) ? taxiClientPath_[i - 1] : taxiClientPath_[i];
        const glm::vec3 p1 = taxiClientPath_[i];
        const glm::vec3 p2 = taxiClientPath_[i + 1];
        const glm::vec3 p3 = (i + 2 < taxiClientPath_.size()) ? taxiClientPath_[i + 2] : taxiClientPath_[i + 1];
        float arcLength = 0.0f;
        glm::vec3 prev = p1;
        for (int s = 1; s <= kArcLengthSamples; s++) {
            const glm::vec3 cur = evalTaxiCatmullRom(p0, p1, p2, p3, static_cast<float>(s) / kArcLengthSamples);
            arcLength += glm::length(cur - prev);
            prev = cur;
        }
        taxiClientSegmentArcLengths_.push_back(arcLength);
        total += arcLength;
    }
    PSP_LOGI(tag::Net, "taxi_path points=%u len=%.0f sec=%.1f kb=%u", static_cast<unsigned>(taxiClientPath_.size()),
             static_cast<double>(total), static_cast<double>(total / taxiClientSpeed_),
             static_cast<unsigned>((taxiClientPath_.size() * 16u + 1023u) / 1024u));
}

// upstream MovementHandler::beginTaxiFlightMotion.
void WorldSession::beginTaxiFlightMotion() {
    if (taxiClientPath_.size() < 2) return;
    const glm::vec3 start = taxiClientPath_[0];
    glm::vec3 dir(0.0f);
    float dirLenSq = 0.0f;
    for (size_t i = 1; i < taxiClientPath_.size(); i++) {
        dir = taxiClientPath_[i] - start;
        dirLenSq = glm::dot(dir, dir);
        if (dirLenSq >= 1e-6f) break;
    }
    if (dirLenSq >= 1e-6f) taxiOrientation_ = std::atan2(dir.y, dir.x);  // PSP: server yaw (0 = north, CCW)
    taxiPos_ = start;
    taxiServerCompletionPending_ = false;
    taxiClientActive_ = true;
    taxiFlightStartMs_ = logUptimeMs();
    PSP_LOGI(tag::Net, "taxi_flight start points=%u x=%.1f y=%.1f z=%.1f", static_cast<unsigned>(taxiClientPath_.size()),
             static_cast<double>(start.x), static_cast<double>(start.y), static_cast<double>(start.z));
    logMemory("taxi_flight");
}

// upstream MovementHandler::finishClientTaxiFlight.
void WorldSession::finishClientTaxiFlight(bool snapToFinalWaypoint) {
    glm::vec3 landingPos(0.0f);
    bool haveLandingPos = false;
    if (const TaxiNode* dest = taxiNode(taxiDestNodeId_)) {
        landingPos = glm::vec3(dest->x, dest->y, dest->z);  // TaxiNodes.dbc: the true destination
        haveLandingPos = true;
    } else if (!taxiClientPath_.empty()) {
        landingPos = taxiClientPath_.back();
        haveLandingPos = true;
    }
    float gap = 0.0f;
    if (!snapToFinalWaypoint && haveLandingPos) {
        gap = glm::length(landingPos - taxiPos_);
        if (gap <= kAuthoritativeLandingSnapDist) snapToFinalWaypoint = true;
    }
    if (snapToFinalWaypoint && haveLandingPos) taxiPos_ = landingPos;
    // PSP: the screen puts the player where the flight left them (taxiPos_: the client path, or our server spline).
    taxiClientActive_ = false;
    taxiServerSplineActive_ = false;
    onTaxiFlight_ = false;
    taxiLandingCooldown_ = 2.0f;
    taxiMountActive_ = false;
    currentMountDisplayId_ = 0;  // upstream: the taxi mount goes at once (mountCallback(0))
    mountAuraSpellId_ = 0;
    taxiClientPath_.clear();
    taxiClientPath_.shrink_to_fit();
    taxiClientSegmentArcLengths_.clear();
    taxiClientSegmentArcLengths_.shrink_to_fit();
    taxiDestNodeId_ = 0;
    taxiServerCompletionPending_ = false;
    if (socket_) {
        taxiSendMove(game::Opcode::MSG_MOVE_STOP);
        taxiSendMove(game::Opcode::MSG_MOVE_HEARTBEAT);
    }
    taxiLanded_ = true;
    taxiLandPos_ = taxiPos_;
    PSP_LOGI(tag::Net, "taxi_land snap=%d gap=%.1f x=%.1f y=%.1f z=%.1f sec=%.1f", snapToFinalWaypoint ? 1 : 0,
             static_cast<double>(gap), static_cast<double>(taxiPos_.x), static_cast<double>(taxiPos_.y),
             static_cast<double>(taxiPos_.z),
             static_cast<double>(taxiFlightStartMs_ ? (logUptimeMs() - taxiFlightStartMs_) / 1000.0f : 0.0f));
    taxiFlightStartMs_ = 0;
}

bool WorldSession::isNearTaxiDestination(float maxDistance) const {
    if (!taxiClientActive_ || taxiClientPath_.empty()) return false;
    const glm::vec3 d = taxiPos_ - taxiClientPath_.back();
    return glm::dot(d, d) <= maxDistance * maxDistance;
}

// The server's own UNIT_FLAG_TAXI_FLIGHT: from this update block when it carries the flags, else our fields.
bool WorldSession::serverStillTaxiing(const game::UpdateBlock* b) const {
    const uint16_t f = game::fieldIndex(game::UF::UNIT_FIELD_FLAGS);
    if (b) {
        auto it = b->fields.find(f);
        if (it != b->fields.end()) return (it->second & game::UNIT_FLAG_TAXI_FLIGHT) != 0;
    }
    return (selfField(f) & game::UNIT_FLAG_TAXI_FLIGHT) != 0;
}

// upstream MovementHandler::handleActivateTaxiReply.
void WorldSession::handleActivateTaxiReply(network::Packet& packet) {
    game::ActivateTaxiReplyData data;
    if (!game::ActivateTaxiReplyParser::parse(packet, data)) return;
    PSP_LOGI(tag::Net, "taxi_reply result=%u pending=%d", static_cast<unsigned>(data.result), taxiActivatePending_ ? 1 : 0);
    if (!taxiActivatePending_) return;  // a stray reply
    if (data.result == 0) {
        onTaxiFlight_ = true;
        taxiStartGrace_ = std::max(taxiStartGrace_, 2.0f);
        taxiWindowOpen_ = false;
        taxiActivatePending_ = false;
        taxiActivateTimer_ = 0.0f;
        taxiMountActive_ = true;  // PSP: the mount itself is the server's UNIT_FIELD_MOUNTDISPLAYID
        beginTaxiFlightMotion();
        if (socket_) taxiSendMove(game::Opcode::MSG_MOVE_HEARTBEAT);
        return;
    }
    if (onTaxiFlight_ || taxiClientActive_) {
        taxiActivatePending_ = false;
        taxiActivateTimer_ = 0.0f;
        return;
    }
    addUiError(taxiReplyText(data.result));  // upstream: "Cannot take that flight path." for every code
    taxiActivatePending_ = false;
    taxiActivateTimer_ = 0.0f;
    taxiClientPath_.clear();
    taxiClientIndex_ = 0;
    taxiClientSegmentProgress_ = 0.0f;
    taxiDestNodeId_ = 0;
    taxiMountActive_ = false;
    onTaxiFlight_ = false;
}

// --- Update blocks / own spline ---------------------------------------------------------------

// EntityController: the own create block with UNIT_FLAG_TAXI_FLIGHT (logged in mid-flight).
void WorldSession::taxiUpdateBlock(const game::UpdateBlock& b) {
    if (playerGuid_ == 0 || b.guid != playerGuid_) return;
    const bool create =
        b.updateType == game::UpdateType::CREATE_OBJECT || b.updateType == game::UpdateType::CREATE_OBJECT2;
    if (!create) return;
    if (serverStillTaxiing(&b) && !onTaxiFlight_ && taxiLandingCooldown_ <= 0.0f) {
        onTaxiFlight_ = true;
        taxiStartGrace_ = std::max(taxiStartGrace_, 2.0f);
        taxiMountActive_ = true;
        if (b.hasMovement) {
            taxiPos_ = glm::vec3(b.x, b.y, b.z);
            taxiOrientation_ = b.orientation;
        }
        PSP_LOGI(tag::Net, "taxi_login_in_flight x=%.1f y=%.1f", static_cast<double>(taxiPos_.x),
                 static_cast<double>(taxiPos_.y));
    }
}

// EntityController::detectPlayerMountChange's taxi guard. True: the field is taken here (mountUpdateBlock stops).
bool WorldSession::taxiOwnMountField(uint32_t newDisplayId, const game::UpdateBlock& b) {
    if (newDisplayId != 0) {
        // PSP: the flight's mount (SendDoFlight -> Mount) arrives right after our dismount for the flight - inside
        // the dismount grace, which is for our own dismount only.
        if ((onTaxiFlight_ || taxiActivatePending_) && currentMountDisplayId_ != newDisplayId) {
            dismountGraceRemaining_ = 0.0f;
            PSP_LOGI(tag::Net, "taxi_mount display=%u", static_cast<unsigned>(newDisplayId));
        }
        return false;  // the mount code takes it
    }
    if (!onTaxiFlight_) return false;
    const bool still = serverStillTaxiing(&b);
    if (taxiClientActive_ && (still || !isNearTaxiDestination())) {
        deferServerTaxiCompletion();  // premature: keep flying to the landing radius
        PSP_LOGI(tag::Net, "taxi_mount_clear deferred still=%d", still ? 1 : 0);
        return true;
    }
    finishClientTaxiFlight(false);  // the server ended the flight ahead of our spline
    return false;
}

// PSP: our own SMSG_MONSTER_MOVE (the server's flight spline). Logged against the client path; followed when there
// is no client path (logged in mid-flight).
void WorldSession::taxiOwnMonsterMove(const game::MonsterMoveData& d) {
    float len = 0.0f;
    glm::vec3 prev(d.x, d.y, d.z);
    for (const auto& w : d.waypoints) {
        len += glm::length(glm::vec3(w.x, w.y, w.z) - prev);
        prev = glm::vec3(w.x, w.y, w.z);
    }
    if (d.hasDest) len += glm::length(glm::vec3(d.destX, d.destY, d.destZ) - prev);
    if (++taxiLogs_ <= 60)
        PSP_LOGI(tag::Net, "taxi_srv_spline type=%u points=%u ms=%u len=%.0f flags=%x taxi=%d client=%d",
                 static_cast<unsigned>(d.moveType), static_cast<unsigned>(d.waypoints.size() + (d.hasDest ? 1u : 0u)),
                 static_cast<unsigned>(d.duration), static_cast<double>(len), static_cast<unsigned>(d.splineFlags),
                 onTaxiFlight_ ? 1 : 0, taxiClientActive_ ? 1 : 0);
    if (!onTaxiFlight_ || taxiClientActive_ || !d.hasDest) return;
    // Entity's Catmull-Rom (upstream startMoveAlongPath), fed server coordinates as they come.
    std::vector<std::array<float, 3>> path;
    path.reserve(d.waypoints.size() + 2);
    path.push_back({d.x, d.y, d.z});
    for (const auto& w : d.waypoints) path.push_back({w.x, w.y, w.z});
    path.push_back({d.destX, d.destY, d.destZ});
    taxiServerSpline_.setPosition(d.x, d.y, d.z, taxiOrientation_);
    taxiServerSpline_.startMoveAlongPath(path, taxiOrientation_, static_cast<float>(d.duration) / 1000.0f);
    taxiServerSplineActive_ = true;
}

// upstream forceClearTaxiAndMovementState (taxi part): a teleport ends any flight.
void WorldSession::forceClearTaxi(const char* why) {
    if (!onTaxiFlight_ && !taxiActivatePending_ && !taxiClientActive_ && !taxiMountActive_) return;
    PSP_LOGI(tag::Net, "taxi_clear why=%s", why);
    if (taxiMountActive_) currentMountDisplayId_ = 0;
    taxiActivatePending_ = false;
    taxiActivateTimer_ = 0.0f;
    taxiClientActive_ = false;
    taxiServerSplineActive_ = false;
    taxiClientPath_.clear();
    taxiServerCompletionPending_ = false;
    taxiStartGrace_ = 0.0f;
    onTaxiFlight_ = false;
    taxiMountActive_ = false;
}

// --- Per frame -------------------------------------------------------------------------------

// upstream MovementHandler::updateClientTaxi.
void WorldSession::updateClientTaxi(float dtSec) {
    if (taxiActivatePending_) {
        taxiActivateTimer_ += dtSec;
        if (taxiActivateTimer_ > kTaxiActivateReplyTimeoutSeconds) {
            PSP_LOGW(tag::Net, "taxi_reply timeout=%.0f", static_cast<double>(kTaxiActivateReplyTimeoutSeconds));
            taxiActivatePending_ = false;
            taxiActivateTimer_ = 0.0f;
            taxiClientPath_.clear();
            taxiClientIndex_ = 0;
            taxiClientSegmentProgress_ = 0.0f;
            taxiMountActive_ = false;
            onTaxiFlight_ = false;
        }
    }
    if (!taxiClientActive_ || taxiClientPath_.size() < 2) return;
    if (taxiClientIndex_ + 1 >= taxiClientPath_.size()) {
        finishClientTaxiFlight(true);
        return;
    }
    float remainingDistance = taxiClientSegmentProgress_ + (taxiClientSpeed_ * dtSec);
    glm::vec3 start(0.0f), end(0.0f), dir(0.0f);
    float segmentLen = 0.0f, t = 0.0f;
    while (true) {
        if (taxiClientIndex_ + 1 >= taxiClientPath_.size()) {
            finishClientTaxiFlight(true);
            return;
        }
        start = taxiClientPath_[taxiClientIndex_];
        end = taxiClientPath_[taxiClientIndex_ + 1];
        dir = end - start;
        const float segLenSq = glm::dot(dir, dir);
        if (segLenSq < 1e-4f) {
            taxiClientIndex_++;
            continue;
        }
        segmentLen = (taxiClientIndex_ < taxiClientSegmentArcLengths_.size())
                         ? taxiClientSegmentArcLengths_[taxiClientIndex_]
                         : std::sqrt(segLenSq);
        if (remainingDistance >= segmentLen) {
            remainingDistance -= segmentLen;
            taxiClientIndex_++;
            taxiClientSegmentProgress_ = 0.0f;
            continue;
        }
        taxiClientSegmentProgress_ = remainingDistance;
        t = taxiClientSegmentProgress_ / segmentLen;
        break;
    }
    const glm::vec3 p0 = (taxiClientIndex_ > 0) ? taxiClientPath_[taxiClientIndex_ - 1] : start;
    const glm::vec3 p1 = start;
    const glm::vec3 p2 = end;
    const glm::vec3 p3 = (taxiClientIndex_ + 2 < taxiClientPath_.size()) ? taxiClientPath_[taxiClientIndex_ + 2] : end;
    const glm::vec3 nextPos = evalTaxiCatmullRom(p0, p1, p2, p3, t);
    const float t2 = t * t;
    glm::vec3 tangent = 0.5f * ((-p0 + p2) + 2.0f * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t +
                                3.0f * (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t2);
    if (glm::dot(tangent, tangent) < 1e-8f) {
        tangent = dir;
        if (glm::dot(tangent, tangent) < 1e-8f) tangent = glm::vec3(std::cos(taxiOrientation_), std::sin(taxiOrientation_), 0.0f);
    }
    const float targetOrientation = std::atan2(tangent.y, tangent.x);
    float orientDiff = targetOrientation - taxiOrientation_;
    while (orientDiff > kPi) orientDiff -= 2.0f * kPi;
    while (orientDiff < -kPi) orientDiff += 2.0f * kPi;
    taxiOrientation_ += orientDiff * std::min(1.0f, dtSec * 3.0f);
    while (taxiOrientation_ > kPi) taxiOrientation_ -= 2.0f * kPi;
    while (taxiOrientation_ < -kPi) taxiOrientation_ += 2.0f * kPi;
    taxiPos_ = nextPos;
    // A server completion that came early: consumed inside the landing radius.
    if (taxiServerCompletionPending_ && isNearTaxiDestination()) finishClientTaxiFlight(true);
}

void WorldSession::updateTaxi(float dtSec, const glm::vec3& selfPos) {
    if (state_ != State::InWorld) return;
    PROF_SCOPE("taxi");
    // upstream forceClearTaxiAndMovementState on a teleport (the death code acknowledges it).
    if (teleportPending_) forceClearTaxi("teleport");
    updateClientTaxi(dtSec);
    // PSP: our own server spline (logged in mid-flight).
    if (taxiServerSplineActive_) {
        taxiServerSpline_.updateMovement(dtSec);
        // Within its duration only (Entity dead-reckons past the end: not for a flight).
        const bool on = taxiServerSpline_.isActivelyMoving();
        const glm::vec3 np = on ? glm::vec3(taxiServerSpline_.getX(), taxiServerSpline_.getY(), taxiServerSpline_.getZ())
                                : glm::vec3(taxiServerSpline_.getLatestX(), taxiServerSpline_.getLatestY(),
                                            taxiServerSpline_.getLatestZ());
        const glm::vec3 d = np - taxiPos_;
        if (d.x * d.x + d.y * d.y > 1e-4f) taxiOrientation_ = std::atan2(d.y, d.x);
        taxiPos_ = np;
        if (!on) taxiServerSplineActive_ = false;
    }
    // GameHandler::updateTaxiAndMountState.
    if (taxiLandingCooldown_ > 0.0f) taxiLandingCooldown_ -= dtSec;
    if (taxiStartGrace_ > 0.0f) taxiStartGrace_ -= dtSec;
    if (!onTaxiFlight_ && taxiMountActive_) {
        if (taxiStartGrace_ > 0.0f || serverStillTaxiing(nullptr) || taxiClientActive_ || taxiActivatePending_) {
            onTaxiFlight_ = true;
        } else {
            taxiMountActive_ = false;
            if (socket_) {
                taxiSendMove(game::Opcode::MSG_MOVE_STOP);
                taxiSendMove(game::Opcode::MSG_MOVE_HEARTBEAT);
            }
            PSP_LOGI(tag::Net, "taxi_dismount_cleanup");
        }
    }
    // PSP: in flight without a client path and the server no longer flying us (its spline done, the flag
    // cleared): the flight is over where we are.
    if (onTaxiFlight_ && !taxiClientActive_ && !taxiActivatePending_ && !taxiServerSplineActive_ &&
        taxiStartGrace_ <= 0.0f && !serverStillTaxiing(nullptr))
        finishClientTaxiFlight(false);
    // upstream closeIfTooFar (NPC_INTERACT_MAX_DISTANCE, 2D) for the taxi window.
    if (taxiWindowOpen_ && taxiNpcGuid_) {
        if (auto npc = entities_.getEntity(taxiNpcGuid_)) {
            const glm::vec3 p = core::coords::canonicalToServer(glm::vec3(npc->getX(), npc->getY(), npc->getZ()));
            const float dx = selfPos.x - p.x, dy = selfPos.y - p.y;
            if (std::sqrt(dx * dx + dy * dy) > game::NPC_INTERACT_MAX_DISTANCE) {
                PSP_LOGI(tag::Net, "taxi_window_far");
                closeTaxi();
            }
        }
    }
    // The tables only while the window is open or a flight is on.
    if (!taxiWindowOpen_ && !taxiActivatePending_ && !onTaxiFlight_ && !taxiClientActive_) releaseTaxiData();
    // One sample every 10 s in flight for the hardware log.
    if (taxiClientActive_ || taxiServerSplineActive_) {
        const uint32_t nowMs = logUptimeMs();
        if (nowMs >= taxiFrameLogMs_) {
            taxiFrameLogMs_ = nowMs + 10000u;
            PSP_LOGI(tag::Net, "taxi_frame idx=%u/%u x=%.0f y=%.0f z=%.0f srv=%d kb=%u", static_cast<unsigned>(taxiClientIndex_),
                     static_cast<unsigned>(taxiClientPath_.size()), static_cast<double>(taxiPos_.x),
                     static_cast<double>(taxiPos_.y), static_cast<double>(taxiPos_.z), taxiServerSplineActive_ ? 1 : 0,
                     static_cast<unsigned>((taxiDataBytes() + 1023u) / 1024u));
        }
    }
}

bool WorldSession::taxiDrivesPlayer(glm::vec3& pos, float& orientation) const {
    if (!taxiClientActive_ && !(onTaxiFlight_ && taxiServerSplineActive_)) return false;
    pos = taxiPos_;
    orientation = taxiOrientation_;
    return true;
}

bool WorldSession::takeTaxiLanding(glm::vec3& pos) {
    if (!taxiLanded_) return false;
    taxiLanded_ = false;
    pos = taxiLandPos_;
    return true;
}

}  // namespace wowee::platform::psp
