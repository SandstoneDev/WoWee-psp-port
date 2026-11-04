// Sounds - footfall ($FSD) animation events into WSKN / WSKA blobs
// (include/rendering/psp/psp_model_format.hpp SkinEventTable). The PSP fires
// a footstep when an animation's clock crosses one of them - upstream
// FootstepDriver::shouldTriggerFootstepEvent with
// CharacterRenderer::getFootstepEventTimes.
//
// Reading follows upstream M2Loader::load (3.3.5a: 36-byte M2Event, identifier
// "$FSD", its M2TrackBase timestamps = one array per sequence) with one PSP
// difference: a sequence whose keyframes live in an external .anim (flag 0x20
// clear) keeps its timestamp array in that .anim too - the array offset is
// into the .anim, like the bone tracks M2Loader::loadAnimFile patches.
// Upstream reads it from the .m2 bytes regardless (usually rejected by its
// bounds check, else garbage), so here the .anim is tried first and a list
// is only kept when it is plausible (<= 64 stamps, ascending, within the
// sequence). Called after bakeSkinned with the same sequence list; a blob
// without any footfall is left byte-identical.
#pragma once

#include "model_bake.hpp"
#include "rendering/psp/psp_model_format.hpp"

#include <cstring>
#include <string>
#include <vector>

namespace wowee::pspconv {

namespace fsd_detail {

inline uint32_t rd32(const std::vector<uint8_t>& b, size_t off) {
    if (off + 4 > b.size()) return 0;
    uint32_t v;
    std::memcpy(&v, b.data() + off, 4);
    return v;
}

// Timestamps of sequence `seq` of every "$FSD" event, from `src` (the .m2 or
// the sequence's .anim). False when a list is implausible for that file.
inline bool readStamps(const std::vector<uint8_t>& m2, const std::vector<uint8_t>& src, uint32_t seq,
                       uint32_t durationMs, std::vector<uint32_t>& out) {
    out.clear();
    if (m2.size() < 264 || std::memcmp(m2.data(), "MD20", 4) != 0) return false;
    const uint32_t version = rd32(m2, 4);
    const uint32_t nEvents = rd32(m2, 256), ofsEvents = rd32(m2, 260);  // 3.3.5a header (version 264)
    if (version < 264 || nEvents == 0 || nEvents >= 512 || ofsEvents == 0) return true;
    bool ok = true;
    for (uint32_t e = 0; e < nEvents; ++e) {
        const size_t base = static_cast<size_t>(ofsEvents) + e * 36u;
        if (base + 36 > m2.size()) break;
        if (std::memcmp(m2.data() + base, "$FSD", 4) != 0) continue;
        const uint32_t nArrays = rd32(m2, base + 28), ofsArrays = rd32(m2, base + 32);
        if (seq >= nArrays) continue;
        const uint32_t nTs = rd32(m2, ofsArrays + seq * 8u), ofsTs = rd32(m2, ofsArrays + seq * 8u + 4u);
        if (nTs == 0) continue;
        if (nTs > 64 || static_cast<uint64_t>(ofsTs) + nTs * 4ull > src.size()) {
            ok = false;
            continue;
        }
        for (uint32_t i = 0; i < nTs; ++i) {
            const uint32_t t = rd32(src, ofsTs + i * 4u);
            if (t > durationMs + 1 || (i > 0 && t < rd32(src, ofsTs + (i - 1) * 4u))) {
                ok = false;
                break;
            }
            out.push_back(t);
        }
    }
    if (!ok) out.clear();
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return ok;
}

} // namespace fsd_detail

// Footfalls of one sequence (ms from its start, ascending).
inline std::vector<uint32_t> footstepEvents(const std::string& root, const std::string& m2Path,
                                            const std::vector<uint8_t>& m2Bytes, const pipeline::M2Model& model,
                                            int seq) {
    std::vector<uint32_t> out;
    if (seq < 0 || static_cast<size_t>(seq) >= model.sequences.size()) return out;
    const auto& s = model.sequences[seq];
    if (!(s.flags & 0x20)) {
        std::vector<uint8_t> anim;
        if (readFile(root + "/" + lower(animPath(m2Path, s.id, s.variationIndex)), anim) &&
            fsd_detail::readStamps(m2Bytes, anim, static_cast<uint32_t>(seq), s.duration, out) && !out.empty())
            return out;
    }
    // Embedded (flag 0x20), or no plausible list in the .anim: upstream's read.
    if (!fsd_detail::readStamps(m2Bytes, m2Bytes, static_cast<uint32_t>(seq), s.duration, out)) out.clear();
    return out;
}

// Appends the SkinEventTable of `seqs` (the sequence list bakeSkinned got, in
// its order) and sets kSkinHasEvents. Returns the time stamps written (0 =
// blob untouched).
inline size_t appendFootstepEvents(std::vector<uint8_t>& blob, const std::string& root, const std::string& m2Path,
                                   const std::vector<uint8_t>& m2Bytes, const pipeline::M2Model& model,
                                   const std::vector<int>& seqs) {
    namespace fmt = wowee::rendering::psp;
    if (blob.size() < sizeof(fmt::SkinHeader)) return 0;
    fmt::SkinHeader h;
    std::memcpy(&h, blob.data(), sizeof(h));
    if (h.sequenceCount != seqs.size()) return 0;
    std::vector<uint16_t> first;
    std::vector<uint32_t> times;
    for (int s : seqs) {
        first.push_back(static_cast<uint16_t>(times.size()));
        const std::vector<uint32_t> ev = footstepEvents(root, m2Path, m2Bytes, model, s);
        times.insert(times.end(), ev.begin(), ev.end());
    }
    first.push_back(static_cast<uint16_t>(times.size()));
    if (times.empty() || times.size() > 65535) return 0;
    alignTo(blob, 4);
    const uint32_t off = static_cast<uint32_t>(blob.size());
    fmt::SkinEventTable t{};
    std::memcpy(t.magic, "WFSD", 4);
    t.sequenceCount = h.sequenceCount;
    t.count = static_cast<uint16_t>(times.size());
    put(blob, t);
    for (uint16_t f : first) put(blob, f);
    alignTo(blob, 4);
    for (uint32_t v : times) put(blob, v);
    h.flags |= fmt::kSkinHasEvents;
    h.reserved[1] = off;
    std::memcpy(blob.data(), &h, sizeof(h));
    // Self-check with the PSP's reader.
    uint32_t n = 0;
    for (size_t s = 0; s < seqs.size(); ++s) {
        const uint32_t* p = fmt::skinFootsteps(reinterpret_cast<const fmt::SkinHeader*>(blob.data()),
                                               static_cast<uint32_t>(blob.size()), static_cast<uint32_t>(s), n);
        if (n != static_cast<uint32_t>(first[s + 1] - first[s]) || (n && p[0] != times[first[s]])) {
            std::fprintf(stderr, "  footstep events: self-check failed at sequence %zu\n", s);
            return 0;
        }
    }
    return times.size();
}

} // namespace wowee::pspconv
