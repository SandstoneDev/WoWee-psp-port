#include "platform/psp/reuse_probe.hpp"

#include "platform/psp/psp_log.hpp"

// Upstream code, compiled unchanged.
#include "math/spline.hpp"
#include "network/packet.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace wowee::platform::psp {

namespace {

struct Checker {
    ReuseProbeResult r;
    void check(bool ok, const char* name) {
        ++r.checks;
        if (!ok) {
            ++r.failed;
            if (!r.firstFailure) r.firstFailure = name;
        }
        logWrite(ok ? LogLevel::Info : LogLevel::Error, tag::Reuse, "check=%s ok=%d", name, ok ? 1 : 0);
    }
};

void probePacket(Checker& c) {
    network::Packet p(0x01ED);  // CMSG_AUTH_SESSION on 3.3.5a, arbitrary here
    p.writeUInt8(0xAB);
    p.writeUInt16(0x1234);
    p.writeUInt32(0xDEADBEEFu);
    p.writeUInt64(0x0102030405060708ull);
    p.writeFloat(1.5f);
    p.writeString("wowee");
    p.writePackedGuid(0x0000123400005678ull);

    // Wire layout is little-endian: the first bytes after 0xAB must be 34 12.
    const auto& d = p.getData();
    c.check(d.size() >= 7 && d[0] == 0xAB && d[1] == 0x34 && d[2] == 0x12 &&
                d[3] == 0xEF && d[4] == 0xBE && d[5] == 0xAD && d[6] == 0xDE,
            "packet_wire_le");

    c.check(p.readUInt8() == 0xAB, "packet_u8");
    c.check(p.readUInt16() == 0x1234, "packet_u16");
    c.check(p.readUInt32() == 0xDEADBEEFu, "packet_u32");
    c.check(p.readUInt64() == 0x0102030405060708ull, "packet_u64");
    c.check(p.readFloat() == 1.5f, "packet_float");
    c.check(p.readString() == "wowee", "packet_string");
    c.check(p.readPackedGuid() == 0x0000123400005678ull, "packet_packed_guid");
    c.check(!p.hasData(), "packet_consumed");
    // Reading past the end answers 0 rather than faulting (upstream contract).
    c.check(p.readUInt32() == 0u, "packet_overread_zero");
}

void probeSpline(Checker& c) {
    std::vector<math::SplineKey> keys = {
        {0, glm::vec3(0.0f, 0.0f, 0.0f)},
        {1000, glm::vec3(10.0f, 0.0f, 0.0f)},
        {2000, glm::vec3(20.0f, 0.0f, 0.0f)},
        {3000, glm::vec3(30.0f, 0.0f, 0.0f)},
    };
    math::CatmullRomSpline s(std::move(keys));
    glm::vec3 mid = s.evaluatePosition(1500);
    // Finite check by exponent bits, not by comparison: the Allegrex FPU traps
    // on compares against inf/NaN and the console dies silently.
    uint32_t bits;
    std::memcpy(&bits, &mid.x, sizeof(bits));
    bool finite = ((bits >> 23) & 0xFFu) != 0xFFu;
    c.check(finite, "spline_finite");
    c.check(finite && mid.x > 14.0f && mid.x < 16.0f, "spline_linear_mid");
    c.check(s.durationMs() == 3000u, "spline_duration");
}

} // namespace

ReuseProbeResult runReuseProbe() {
    Checker c;
    probePacket(c);
    probeSpline(c);
    return c.r;
}

} // namespace wowee::platform::psp
