// Transports: how the port moves ships, zeppelins and lifts, as plain code (no PSP calls:
// tools/psp/host_tests/transport_path_test.cpp runs it on the PC). The ship model follows the public server side
// (AzerothCore TransportMgr::GeneratePath, MotionTransport::Update / CalculateSegmentPos, Movement::Spline), so a
// hull is where the server keeps it at the same path time.
//
//  - GAMEOBJECT_TYPE_MO_TRANSPORT (15: ships, zeppelins, the icebreaker): MoTransportPath. The object's template
//    (SMSG_GAMEOBJECT_QUERY_RESPONSE) gives data0 taxiPathId, data1 moveSpeed, data2 accelRate;
//    TaxiPathNode.dbc gives the path. It is cut into runs (a new run where the map changes or after a node with
//    flag 0x1); the ship passes through a run's inner nodes on a Catmull-Rom spline (the run's first and last node
//    are left out, as GeneratePath drops them), stops at nodes with flag 0x2 (wait = Delay s), and every leg is
//    timed with the speed and acceleration: cruise and brake into a run's first stop, accelerate - cruise - brake
//    between stops, accelerate and cruise out of the last one, a constant speed through a run without stops. The
//    runs follow each other in time. GAMEOBJECT_LEVEL (the server's period) sets the cycle: the flying time is
//    scaled so the cycle matches it, the waits are kept. The place at a time: (time % period) -> run -> leg ->
//    distance along the spline -> position, and the facing atan2(-dy, -dx) of the spline's derivative (the hull's
//    bow is model -X).
//  - GAMEOBJECT_TYPE_TRANSPORT (11: lifts, tram cars, moving arena pillars): ElevatorAnim.
//    TransportAnimation.dbc keys (TransportID = the object's entry) by time, linear between keys, the offset
//    turned by the object's GAMEOBJECT_PARENTROTATION and added to its place; the period is the last key's time.
//    GAMEOBJECT_LEVEL 0 = a loop; else a lift with two rests (0 .. LEVEL in state 0, LEVEL .. period in state 1)
//    driven by the object's state. A state change never makes the lift jump: it heads for the rest of its new
//    state along its own keys, forward or backward from where it is.
//  - The time: the movement block's UPDATEFLAG_TRANSPORT u32 minus our clock when the create block came, plus the
//    clock now.
#pragma once

#include "rendering/psp/psp_transport_format.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wowee::rendering::psp {

// A run's Catmull-Rom spline through its nodes (AzerothCore Movement::SplineBase in ModeCatmullrom): m nodes make
// m - 1 spans; the end spans are steered by the virtual points of SplineBase::InitCatmullRom (before the first node:
// the node minus (1, 0, 0); after the last node: the node again). A span's length is 3 chords (STEPS_PER_SEGMENT);
// a fraction of the whole length picks the span and its t linearly in length (Spline::computeIndex).
class TransportSpline {
public:
    void init(const std::vector<glm::vec3>& nodes);  // the nodes the spline passes through
    size_t nodeCount() const { return nodes_; }
    size_t spanCount() const { return len_.size(); }
    float length() const { return total_; }
    float spanLength(size_t i) const { return i < len_.size() ? len_[i] : 0.0f; }
    // The length from the first node to node `node`.
    float lengthTo(uint32_t node) const;
    // The place at a fraction of the length and the (unnormalised) derivative there.
    void evaluate(float fraction, glm::vec3& pos, glm::vec3& dir) const;

private:
    void spanAt(float fraction, uint32_t& span, float& t) const;
    glm::vec3 position(uint32_t span, float t) const;
    glm::vec3 derivative(uint32_t span, float t) const;
    std::vector<glm::vec3> p_;  // virtual point, the nodes, virtual point
    std::vector<float> len_;
    size_t nodes_ = 0;
    float total_ = 0.0f;
};

struct MoTransportPlace {
    uint32_t mapId = 0;
    glm::vec3 pos{0.0f};      // server coordinates
    float orientation = 0.0f;  // server yaw of the hull (atan2(-dy, -dx))
    uint32_t segment = 0;
    bool waiting = false;      // at a stop
};

class MoTransportPath {
public:
    struct Node {
        glm::vec3 pos;
        uint32_t mapId;
        uint32_t flags;
        uint32_t delaySec;
    };
    struct Stop {
        uint32_t timeMs;  // arrival, path time
        float dist;       // along the segment's spline
        uint32_t waitMs;
        float legSec;     // the leg that ends here, at the template's speed
    };
    struct Segment {
        uint32_t mapId = 0;
        TransportSpline spline;
        uint32_t startMs = 0, endMs = 0;
        std::vector<Stop> stops;  // the stops, then the segment's end (wait 0)
    };

    // Nodes in NodeIndex order; speed (yd/s) and acceleration (yd/s^2) from the template.
    bool build(const Node* nodes, size_t count, float speed, float accel);
    // The server's period (GAMEOBJECT_LEVEL): the flying time is scaled to fit it. 0 keeps the summed one.
    void setPeriod(uint32_t periodMs);
    uint32_t period() const { return periodMs_; }
    uint32_t builtPeriod() const { return builtMs_; }
    // The place at a time (modulo the period): false when the path is empty.
    bool evaluate(uint32_t timeMs, MoTransportPlace& out) const;
    const std::vector<Segment>& segments() const { return segs_; }
    // Leg distance at tt seconds of a leg of leg seconds (the three profiles; tests).
    float firstLegDistance(float tt, float leg) const;   // cruise, then brake into a stop
    float midLegDistance(float tt, float leg) const;     // accelerate, cruise, brake
    float lastLegDistance(float tt, float leg) const;    // accelerate, then cruise

private:
    void finish(Segment& seg, const std::vector<glm::vec3>& points,
                const std::vector<std::pair<uint32_t, uint32_t>>& stops);
    void layout();                         // the stops' path times from the legs and the time scale
    float brakeLegSec(float dist) const;   // into the first stop (or out of the last: same profile)
    float midLegSec(float dist) const;
    std::vector<Segment> segs_;
    float v_ = 1.0f, a_ = 1.0f;
    float flySec_ = 0.0f;   // the summed leg times
    uint32_t waitMs_ = 0;   // the summed waits
    float scale_ = 1.0f;    // template seconds per path second while moving (1 = the template's own timing)
    uint32_t periodMs_ = 0, builtMs_ = 0;
    mutable float lastYaw_ = 0.0f;  // kept while the derivative is degenerate
};

// GAMEOBJECT_TYPE_TRANSPORT's helper: its keys, and for a lift with two rests the ride it is on (where on its key
// timeline it started, when, which way and up to where).
class ElevatorAnim {
public:
    void setKeys(const TransportAnimKey* pos, size_t posCount, const TransportRotKey* rot, size_t rotCount);
    bool empty() const { return pos_.empty(); }
    uint32_t period() const { return pos_.empty() ? 0u : pos_.back().timeMs; }
    // The object first seen: settled at the rest of its state (a loop just runs on the path time).
    void settle(uint32_t pathTime, uint8_t state, uint32_t level);
    // The object's state changed to `newState` at path time `pathTime`.
    void onStateChange(uint32_t pathTime, int32_t newState, uint32_t level);
    // The offset (before the parent rotation) and the rotation key at path time `pathTime`.
    void evaluate(uint32_t pathTime, uint32_t level, glm::vec3& offset, glm::quat& rot, bool& hasRot);
    // The place on the key timeline (0 .. period) at path time `pathTime`.
    uint32_t localTime(uint32_t pathTime, uint32_t level) const;

private:
    glm::vec3 offsetAt(uint32_t t);  // before the parent rotation
    bool rotationAt(uint32_t t, glm::quat& q);
    std::vector<TransportAnimKey> pos_;
    std::vector<TransportRotKey> rot_;
    uint32_t posCursor_ = 0, rotCursor_ = 0;
    int32_t state_ = -1;
    // The ride: from rideFrom_ at path time rideStart_, forward (rideBack_ false) or backward, ending at rideTo_.
    uint32_t rideFrom_ = 0, rideTo_ = 0, rideStart_ = 0;
    bool rideBack_ = false;
};

// GAMEOBJECT_PARENTROTATION (x, y, z, w) as the rotation a lift's path is turned by.
glm::vec3 rotateByParent(const glm::vec4& quatXyzw, const glm::vec3& v);

}  // namespace wowee::rendering::psp
