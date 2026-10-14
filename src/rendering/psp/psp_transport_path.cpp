// Transport motion (ships, zeppelins, lifts) - see psp_transport_path.hpp.
// Plain code, no PSP calls (the host test runs it on the PC).
#include "rendering/psp/psp_transport_path.hpp"

#include <algorithm>
#include <cmath>

namespace wowee::rendering::psp {

namespace {

// The Catmull-Rom basis, one row per control point (cubic coefficients t^3, t^2, t, 1): AzerothCore Spline.cpp
// s_catmullRomCoeffs (the same coefficients as upstream math/spline.cpp CatmullRomSpline::evalSegment).
constexpr float kBasis[4][4] = {
    {-0.5f, 1.0f, -0.5f, 0.0f},
    {1.5f, -2.5f, 0.0f, 1.0f},
    {-1.5f, 2.0f, 0.5f, 0.0f},
    {0.5f, -0.5f, 0.0f, 0.0f},
};
// Its derivative (t^2, t, 1): AzerothCore C_Evaluate_Derivative.
constexpr float kBasisD[4][3] = {
    {-1.5f, 2.0f, -0.5f},
    {4.5f, -5.0f, 0.0f},
    {-4.5f, 4.0f, 0.5f},
    {1.5f, -1.0f, 0.0f},
};
// Chords per span when measuring its length: AzerothCore Spline.h STEPS_PER_SEGMENT.
constexpr int kStepsPerSpan = 3;
// A derivative shorter than this (squared) gives no facing: upstream CatmullRomSpline::orientationFromTangent.
constexpr float kMinTangentSq = 1e-6f;

// Path seconds -> ms, truncated as AzerothCore GeneratePath does (uint32(curPathTime * IN_MILLISECONDS)).
uint32_t toMs(double sec) { return sec > 0.0 ? static_cast<uint32_t>(sec * 1000.0) : 0u; }

}  // namespace

// ---- TransportSpline ----------------------------------------------------------------------------------------

void TransportSpline::init(const std::vector<glm::vec3>& nodes) {
    nodes_ = nodes.size();
    p_.clear();
    len_.clear();
    total_ = 0.0f;
    if (nodes.empty()) return;
    // SplineBase::InitCatmullRom (not cyclic, orientation 0): a virtual point before the first node and the last
    // node repeated after it.
    p_.reserve(nodes.size() + 2);
    p_.push_back(nodes.front() - glm::vec3(1.0f, 0.0f, 0.0f));
    p_.insert(p_.end(), nodes.begin(), nodes.end());
    p_.push_back(nodes.back());
    if (nodes.size() < 2) return;
    const size_t spans = nodes.size() - 1;
    len_.resize(spans);
    for (size_t i = 0; i < spans; ++i) {
        // SplineBase::SegLengthCatmullRom: the chords at t = 1/3, 2/3, 1 from the span's start node.
        glm::vec3 prev = p_[i + 1];
        double sum = 0.0;
        for (int k = 1; k <= kStepsPerSpan; ++k) {
            const glm::vec3 q = position(static_cast<uint32_t>(i), static_cast<float>(k) / kStepsPerSpan);
            sum += glm::length(q - prev);
            prev = q;
        }
        len_[i] = static_cast<float>(sum);
        total_ += len_[i];
    }
}

float TransportSpline::lengthTo(uint32_t node) const {
    float sum = 0.0f;
    for (uint32_t i = 0; i < node && i < len_.size(); ++i) sum += len_[i];
    return sum;
}

void TransportSpline::spanAt(float fraction, uint32_t& span, float& t) const {
    // Spline::computeIndex / computeIndexInBounds: the first span whose end is not before the distance.
    const size_t count = len_.size();
    const float dist = total_ * fraction;
    float acc = 0.0f;
    span = 0;
    while (span + 1 < count && acc + len_[span] < dist) {
        acc += len_[span];
        ++span;
    }
    t = len_[span] > 0.0f ? (dist - acc) / len_[span] : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
}

glm::vec3 TransportSpline::position(uint32_t span, float t) const {
    glm::vec3 out(0.0f);
    for (uint32_t i = 0; i < 4; ++i) {
        const float* m = kBasis[i];
        const float w = ((m[0] * t + m[1]) * t + m[2]) * t + m[3];
        out += w * p_[span + i];
    }
    return out;
}

glm::vec3 TransportSpline::derivative(uint32_t span, float t) const {
    glm::vec3 out(0.0f);
    for (uint32_t i = 0; i < 4; ++i) {
        const float* m = kBasisD[i];
        const float w = (m[0] * t + m[1]) * t + m[2];
        out += w * p_[span + i];
    }
    return out;
}

void TransportSpline::evaluate(float fraction, glm::vec3& pos, glm::vec3& dir) const {
    if (len_.empty()) {
        pos = p_.empty() ? glm::vec3(0.0f) : p_[1];
        dir = glm::vec3(0.0f);
        return;
    }
    const float f = fraction >= 0.0f ? (fraction < 1.0f ? fraction : 1.0f) : 0.0f;
    uint32_t span = 0;
    float t = 0.0f;
    spanAt(f, span, t);
    pos = position(span, t);
    dir = derivative(span, t);
}

// ---- MoTransportPath ----------------------------------------------------------------------------------------
// The leg times are AzerothCore GeneratePath's TimeTo branches; the distance within a leg is
// MotionTransport::CalculateSegmentPos (accelerate at accelRate up to moveSpeed, cruise, brake mirrored).

float MoTransportPath::brakeLegSec(float dist) const {
    const float tv = v_ / a_;                // time to full speed
    const float brake = v_ * 0.5f * tv;      // distance to full speed
    return dist <= brake ? std::sqrt(2.0f * dist / a_) : (dist - brake) / v_ + tv;
}

float MoTransportPath::midLegSec(float dist) const {
    const float tv = v_ / a_;
    const float brake = v_ * 0.5f * tv;
    if (dist * 0.5f <= brake) return 2.0f * std::sqrt(dist / a_);
    return (dist - brake * 2.0f) / v_ + tv * 2.0f;
}

void MoTransportPath::finish(Segment& seg, const std::vector<glm::vec3>& points,
                             const std::vector<std::pair<uint32_t, uint32_t>>& stops) {
    // The ship passes through the run's inner nodes: GeneratePath drops the node of a teleport / map change, the
    // node after it, and the path's first and last node. A run of two nodes or less keeps them (no inner node).
    const bool inner = points.size() >= 3;
    seg.spline.init(inner ? std::vector<glm::vec3>(points.begin() + 1, points.end() - 1) : points);
    seg.stops.clear();
    float prev = 0.0f;
    for (const auto& [index, waitMs] : stops) {
        // A stop on a run's first or last node is left out (that node is not on the spline).
        if (!inner || index + 1 >= points.size()) break;
        const float d = seg.spline.lengthTo(index - 1);
        const float leg = seg.stops.empty() ? brakeLegSec(d - prev) : midLegSec(d - prev);
        seg.stops.push_back(Stop{0, d, waitMs, leg});
        flySec_ += leg;
        waitMs_ += waitMs;
        prev = d;
    }
    const float total = seg.spline.length();
    const float leg = seg.stops.empty() ? (total - prev) / v_ : brakeLegSec(total - prev);
    seg.stops.push_back(Stop{0, total, 0, leg});
    flySec_ += leg;
}

void MoTransportPath::layout() {
    // The runs follow each other in time; a leg lasts its template time / scale_, a wait its Delay.
    double cur = 0.0;
    for (Segment& seg : segs_) {
        seg.startMs = toMs(cur);
        for (Stop& s : seg.stops) {
            cur += static_cast<double>(s.legSec) / scale_;
            s.timeMs = toMs(cur);
            cur += s.waitMs * 0.001;
        }
        seg.endMs = seg.stops.back().timeMs;
    }
}

bool MoTransportPath::build(const Node* nodes, size_t count, float speed, float accel) {
    segs_.clear();
    builtMs_ = 0;
    periodMs_ = 0;
    flySec_ = 0.0f;
    waitMs_ = 0;
    scale_ = 1.0f;
    v_ = speed > 0.01f ? speed : 0.01f;
    a_ = accel > 0.01f ? accel : 0.01f;
    std::vector<glm::vec3> points;
    std::vector<std::pair<uint32_t, uint32_t>> stops;  // (point index in the run, wait ms)
    bool prevTeleport = false;
    for (size_t i = 0; i < count; ++i) {
        const Node& n = nodes[i];
        if (segs_.empty() || n.mapId != segs_.back().mapId || prevTeleport) {
            if (!segs_.empty()) finish(segs_.back(), points, stops);
            segs_.emplace_back();
            segs_.back().mapId = n.mapId;
            points.clear();
            stops.clear();
        }
        if ((n.flags & 2u) && !points.empty())
            stops.emplace_back(static_cast<uint32_t>(points.size()), n.delaySec * 1000u);
        points.push_back(n.pos);
        prevTeleport = (n.flags & 1u) != 0;
    }
    if (!segs_.empty() && !points.empty()) finish(segs_.back(), points, stops);
    if (segs_.empty()) return false;
    layout();
    builtMs_ = segs_.back().endMs;
    periodMs_ = builtMs_;
    return builtMs_ > 0;
}

void MoTransportPath::setPeriod(uint32_t periodMs) {
    // The server's period: as upstream TransportPathRepository::taxiRouteSpeedFor retimes a route to it, only the
    // flying is scaled - the waits are the nodes' own. A period that leaves no flying time keeps our timing.
    // No period from the server keeps our own sum.
    scale_ = 1.0f;
    if (periodMs > waitMs_ && flySec_ > 0.0f)
        scale_ = flySec_ / (static_cast<float>(periodMs - waitMs_) * 0.001f);
    layout();
    periodMs_ = periodMs ? periodMs : builtMs_;
}

float MoTransportPath::firstLegDistance(float tt, float leg) const {
    float dec = v_ / a_;
    if (leg < dec) dec = leg;
    const float cruise = leg - dec;
    if (tt <= cruise) return tt * v_;
    const float v0 = a_ * dec;
    const float t2 = tt - cruise;
    return cruise * v_ + ((v0 + v0) - a_ * t2) * 0.5f * t2;
}

float MoTransportPath::midLegDistance(float tt, float leg) const {
    const float tv = v_ / a_;
    float acc = leg * 0.5f;
    if (tv <= acc) acc = tv;
    if (leg - acc < tt) {
        const float vp = a_ * acc;
        const float t2 = tt - (leg - acc);
        return (leg - 2.0f * acc) * v_ + (vp * 2.0f - a_ * t2) * 0.5f * t2 + vp * 0.5f * acc;
    }
    if (acc < tt) return (tt - tv) * v_ + v_ * 0.5f * tv;
    const float vp = a_ * tt;
    return tt * vp * 0.5f;
}

float MoTransportPath::lastLegDistance(float tt, float leg) const {
    const float tv = v_ / a_;
    const float m = tv <= leg ? tv : leg;
    if (tt <= m) return a_ * tt * 0.5f * tt;
    return (tt - tv) * v_ + v_ * 0.5f * tv;
}

bool MoTransportPath::evaluate(uint32_t timeMs, MoTransportPlace& out) const {
    if (periodMs_ == 0 || segs_.empty()) return false;
    const uint32_t t = timeMs % periodMs_;
    // A time past the last run's end (ms truncation, or a server period longer than the path) is that end.
    size_t k = 0;
    while (k + 1 < segs_.size() && t >= segs_[k].endMs) ++k;
    const Segment& seg = segs_[k];
    const size_t n = seg.stops.size();
    uint32_t depart = seg.startMs;
    float base = 0.0f, dist = 0.0f;
    bool waiting = false;
    size_t j = 0;
    for (; j + 1 < n; ++j) {
        const Stop& s = seg.stops[j];
        if (t < s.timeMs) break;
        if (t - s.timeMs < s.waitMs) {
            dist = s.dist;
            waiting = true;
            break;
        }
        base = s.dist;
        depart = s.timeMs + s.waitMs;
    }
    if (!waiting) {
        const float leg = seg.stops[j].legSec;
        const float tt = std::min(static_cast<float>(t >= depart ? t - depart : 0u) * 0.001f * scale_, leg);
        const bool endsAtStop = j != n - 1;
        float d;
        if (j == 0) d = endsAtStop ? firstLegDistance(tt, leg) : tt * v_;
        else d = endsAtStop ? midLegDistance(tt, leg) : lastLegDistance(tt, leg);
        dist = std::min(d + base, seg.stops[j].dist);
    }
    const float total = seg.spline.length();
    float frac = total > 0.0f ? dist / total : 0.0f;
    if (!(frac >= 0.0f)) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    glm::vec3 dir(0.0f);
    seg.spline.evaluate(frac, out.pos, dir);
    // MotionTransport::Update: atan2(dir.y, dir.x) + pi of the spline's derivative.
    if (glm::dot(dir, dir) >= kMinTangentSq) lastYaw_ = std::atan2(-dir.y, -dir.x);
    out.orientation = lastYaw_;
    out.mapId = seg.mapId;
    out.segment = static_cast<uint32_t>(k);
    out.waiting = waiting;
    return true;
}

// ---- ElevatorAnim -------------------------------------------------------------------------------------------
// A lift with two rests: state 0 rides 0 .. LEVEL and rests at LEVEL, state 1 rides LEVEL .. period and rests at
// the period (= key time 0) - AzerothCore StaticTransport::Update. Our own rule for a state change: the lift never
// jumps; it heads for the rest of its new state along its keys - forward when that rest ends the half it is in,
// backward when it starts it (a lift half way up turns back down) - at the keys' own pace.

void ElevatorAnim::setKeys(const TransportAnimKey* pos, size_t posCount, const TransportRotKey* rot, size_t rotCount) {
    pos_.assign(pos, pos + posCount);
    rot_.assign(rot, rot + rotCount);
    posCursor_ = rotCursor_ = 0;
    state_ = -1;
    rideFrom_ = rideTo_ = rideStart_ = 0;
    rideBack_ = false;
}

uint32_t ElevatorAnim::localTime(uint32_t pathTime, uint32_t level) const {
    if (pos_.empty()) return pathTime;
    const uint32_t period = pos_.back().timeMs;
    if (level == 0 || state_ < 0) return period ? pathTime % period : 0u;
    const int32_t since = static_cast<int32_t>(pathTime - rideStart_);
    const uint64_t elapsed = since > 0 ? static_cast<uint64_t>(since) : 0u;
    if (!rideBack_) return static_cast<uint32_t>(std::min<uint64_t>(rideFrom_ + elapsed, rideTo_));
    return rideFrom_ >= rideTo_ + elapsed ? static_cast<uint32_t>(rideFrom_ - elapsed) : rideTo_;
}

void ElevatorAnim::settle(uint32_t pathTime, uint8_t state, uint32_t level) {
    // PSP: the object is first seen at the rest of its state (no ride is under way when we first see it).
    state_ = state;
    const uint32_t lv = std::min(level, period());
    rideFrom_ = rideTo_ = state == 0 ? lv : 0u;
    rideStart_ = pathTime;
    rideBack_ = false;
}

void ElevatorAnim::onStateChange(uint32_t pathTime, int32_t newState, uint32_t level) {
    if (pos_.empty() || level == 0) {
        state_ = newState;
        return;
    }
    if (state_ < 0) {
        settle(pathTime, static_cast<uint8_t>(newState), level);
        return;
    }
    if ((newState == 0) == (state_ == 0)) return;
    const uint32_t period = pos_.back().timeMs;
    const uint32_t lv = std::min(level, period);
    uint32_t at = localTime(pathTime, level);
    if (newState == 0) {
        // Target: LEVEL. In the first half (0 .. LEVEL) ride on to it; in the second, ride back to it.
        if (at >= period) at = 0;
        rideTo_ = lv;
        rideBack_ = at > lv;
    } else if (at == 0) {
        rideTo_ = 0;  // already at the rest of state 1
        rideBack_ = false;
    } else {
        // Target: the period (key time 0). In the second half ride on to it; in the first, ride back to 0.
        rideBack_ = at < lv;
        rideTo_ = rideBack_ ? 0u : period;
    }
    rideFrom_ = at;
    rideStart_ = pathTime;
    state_ = newState;
}

glm::vec3 ElevatorAnim::offsetAt(uint32_t t) {
    const uint32_t n = static_cast<uint32_t>(pos_.size());
    if (n < 2) return glm::vec3(0.0f);
    if (posCursor_ >= n) posCursor_ = 0;
    uint32_t next = posCursor_ + 1 == n ? 0u : posCursor_ + 1;
    // At most one lap over the keys (keys out of order would never end the search).
    for (uint32_t guard = 0; guard <= n; ++guard) {
        if (pos_[posCursor_].timeMs <= t && t < pos_[next].timeMs) break;
        posCursor_ = next;
        next = next + 1 == n ? 0u : next + 1;
    }
    // AzerothCore TransportAnimation::GetAnimNode + StaticTransport::RelocateToProgress: linear between keys.
    const TransportAnimKey& a = pos_[posCursor_];
    const TransportAnimKey& b = pos_[next];
    const float span = static_cast<float>(b.timeMs - a.timeMs);
    float f = span > 0.0f ? static_cast<float>(t - a.timeMs) / span : 0.0f;
    if (!(f >= 0.0f)) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    const float g = 1.0f - f;
    return glm::vec3(a.pos[0] * g + b.pos[0] * f, a.pos[1] * g + b.pos[1] * f, a.pos[2] * g + b.pos[2] * f);
}

bool ElevatorAnim::rotationAt(uint32_t t, glm::quat& q) {
    const uint32_t n = static_cast<uint32_t>(rot_.size());
    if (n < 2) return false;
    if (rotCursor_ >= n) rotCursor_ = 0;
    uint32_t next = rotCursor_ + 1 == n ? 0u : rotCursor_ + 1;
    // AzerothCore TransportAnimation::GetAnimRotation: after the last key the next one is the first, at the
    // position keys' period.
    auto endOf = [&](uint32_t i) { return rot_[i].timeMs != 0 ? rot_[i].timeMs : period(); };
    for (uint32_t guard = 0; guard <= n; ++guard) {
        if (rot_[rotCursor_].timeMs <= t && t < endOf(next)) break;
        rotCursor_ = next;
        next = next + 1 == n ? 0u : next + 1;
    }
    const TransportRotKey& a = rot_[rotCursor_];
    const TransportRotKey& b = rot_[next];
    const float span = static_cast<float>(endOf(next) - a.timeMs);
    float f = span > 0.0f ? static_cast<float>(t - a.timeMs) / span : 0.0f;
    if (!(f >= 0.0f)) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    const glm::quat qa(a.quat[3], a.quat[0], a.quat[1], a.quat[2]);
    const glm::quat qb(b.quat[3], b.quat[0], b.quat[1], b.quat[2]);
    q = glm::slerp(qa, qb, f);
    return true;
}

void ElevatorAnim::evaluate(uint32_t pathTime, uint32_t level, glm::vec3& offset, glm::quat& rot, bool& hasRot) {
    uint32_t t = localTime(pathTime, level);
    const uint32_t period = this->period();
    if (period && t >= period) t = 0;  // the period is key time 0 again
    offset = offsetAt(t);
    hasRot = rotationAt(t, rot);
}

glm::vec3 rotateByParent(const glm::vec4& q, const glm::vec3& v) {
    // The rotation matrix of the unit quaternion (x, y, z, w), applied to v.
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float y2 = y * 2.0f, z2 = z * 2.0f, wx2 = w * x * 2.0f, xx2 = x * x * 2.0f;
    const float m0 = 1.0f - (z * z2 + y * y2), m1 = x * y2 + w * z2, m2 = x * z2 - w * y2;
    const float m3 = x * y2 - w * z2, m4 = 1.0f - (z * z2 + xx2), m5 = y * z2 + wx2;
    const float m6 = x * z2 + w * y2, m7 = y * z2 - wx2, m8 = 1.0f - (y * y2 + xx2);
    return glm::vec3(v.x * m0 + v.y * m3 + v.z * m6, v.x * m1 + v.y * m4 + v.z * m7, v.x * m2 + v.y * m5 + v.z * m8);
}

}  // namespace wowee::rendering::psp
