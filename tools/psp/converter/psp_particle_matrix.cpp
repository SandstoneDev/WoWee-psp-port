// psp_particle_matrix - the M2 particle matrix on the PC (docs/PSP_PORT_ARCHITECTURE.md), in the spirit of
// psp_equip_preview --matrix.
//
//   psp_particle_matrix --extracted <dir> --list <m2 list> --stats <out.md> [--tsv <out.tsv>]
//       every M2 of the list (paths relative to --extracted): its particle / ribbon emitters
//       (m2_particle_raw.hpp) - flag / type / blend / curve / field counts per source (doodads, game objects =
//       GameObjectDisplayInfo.dbc, spell effects = spells/), the fields each count touches, upstream's ribbon
//       stride bug.
//   psp_particle_matrix --extracted <dir> --compare <model list> --out <dir> [--seconds 6] [--fps 30]
//       per model of the list: the dense reference (the level-3 rules over the exact tracks,
//       ptc_clean_reference.hpp) against the engine's code (src/rendering/psp/psp_particles_l3.cpp over the WSFX
//       blob that particle_bake.hpp bakes) and upstream's rules (ptclevel3=0): positions / velocities / sizes /
//       colours over time, plus a picture per model (<out>/<name>.ppm: reference | PSP | upstream).
#include "conv_common.hpp"
#include "m2_particle_raw.hpp"
#include "ptc_matrix_compare.hpp"
#include "rendering/psp/psp_lighting_ge.hpp"  // FrameLight (the PC: no tint)

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace wowee::pspconv;
namespace pipeline = wowee::pipeline;
namespace rendering = wowee::rendering;

namespace {

std::string g_root;

bool readGame(const std::string& rel, std::vector<uint8_t>& out) { return readFile(g_root + "/" + lower(rel), out); }

// Minimal WDBC: the string column `col` of every record.
std::vector<std::string> dbcStrings(const std::string& path, int col) {
    std::vector<std::string> out;
    std::vector<uint8_t> b;
    if (!readFile(path, b) || b.size() < 20 || std::memcmp(b.data(), "WDBC", 4) != 0) return out;
    uint32_t n, f, rs, ss;
    std::memcpy(&n, &b[4], 4);
    std::memcpy(&f, &b[8], 4);
    std::memcpy(&rs, &b[12], 4);
    std::memcpy(&ss, &b[16], 4);
    const size_t strBase = 20 + size_t(n) * rs;
    if (strBase + ss > b.size()) return out;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t o;
        std::memcpy(&o, &b[20 + size_t(i) * rs + col * 4u], 4);
        if (o < ss) out.emplace_back(reinterpret_cast<const char*>(&b[strBase + o]));
    }
    return out;
}

std::string normModel(std::string p) {
    p = lower(p);
    const size_t dot = p.rfind('.');
    if (dot != std::string::npos && (p.substr(dot) == ".mdx" || p.substr(dot) == ".mdl")) p = p.substr(0, dot) + ".m2";
    return p;
}

std::vector<std::string> readList(const std::string& path) {
    std::vector<std::string> out;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    char line[1024];
    while (std::fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
        if (!s.empty() && s[0] != '#') out.push_back(lower(s));
    }
    std::fclose(f);
    return out;
}

// --- stats ------------------------------------------------------------------------------------------

struct Hist {
    std::map<std::string, size_t> n;
    void add(const std::string& k, size_t v = 1) { n[k] += v; }
};

struct SourceStats {
    size_t models = 0, modelsWithEmitters = 0, modelsWithRibbons = 0, emitters = 0, ribbons = 0;
    size_t flagBits[32] = {};
    Hist type, blend, ptype, hot, tiles, colorKeys, alphaKeys, scaleKeys, headKeys, tailKeys, misc, interp, ribbonMisc,
        midTs, enabled;
    std::vector<std::string> examples;  // first models per misc key
    std::map<std::string, std::string> example;
};

const char* srcName(int s) { return s == 0 ? "doodads" : s == 1 ? "gameobjects" : "spellfx"; }

bool pow2(unsigned v) { return v && !(v & (v - 1)); }

int runStats(const std::vector<std::string>& list, const std::string& outMd, const std::string& outTsv) {
    std::set<std::string> goModels, spellModels;
    for (auto& s : dbcStrings(g_root + "/dbfilesclient/gameobjectdisplayinfo.dbc", 1)) goModels.insert(normModel(s));
    for (auto& s : dbcStrings(g_root + "/dbfilesclient/spellvisualeffectname.dbc", 2)) spellModels.insert(normModel(s));
    std::printf("gameobject models %zu, spell effect models %zu\n", goModels.size(), spellModels.size());
    SourceStats st[3];
    FILE* tsv = outTsv.empty() ? nullptr : std::fopen(outTsv.c_str(), "wb");
    if (tsv)
        std::fprintf(tsv, "source\tmodel\temitter\ttype\tflags\tblend\trows\tcols\tcolorKeys\talphaKeys\tscaleKeys\theadKeys\t"
                          "tailKeys\tlut\tlifeVary\trateVary\ttail\ttwinkleP\ttwinkleMin\ttwinkleMax\tburst\tdrag\tspin\tspinVary\t"
                          "baseSpin\twind\twindTime\tfollow\tscaleVary\tenabledKeys\tspeed0\tvr0\thr0\tgravity0\tlife0\trate0\t"
                          "areaL0\tareaW0\tzSource0\tgeom\trecursion\tspline\n");
    size_t done = 0, unreadable = 0;
    for (const std::string& rel : list) {
        if (++done % 1000 == 0) std::fprintf(stderr, "  %zu / %zu\n", done, list.size());
        int src = 0;
        if (rel.rfind("spells/", 0) == 0 || spellModels.count(rel)) src = 2;
        else if (goModels.count(rel) || rel.rfind("world/goober/", 0) == 0) src = 1;
        std::vector<uint8_t> m2;
        if (!readGame(rel, m2)) {
            ++unreadable;
            continue;
        }
        m2raw::Model pre;
        if (!pre.parse(m2)) {
            ++unreadable;
            continue;
        }
        std::vector<uint8_t> anim;
        if (pre.seqInAnim(0) && !pre.sequences.empty()) {
            char tail[32];
            std::snprintf(tail, sizeof(tail), "%04u-%02u.anim", pre.sequences[0].id, pre.sequences[0].variation);
            readGame(rel.substr(0, rel.size() - 3) + tail, anim);
        }
        m2raw::Model m;
        m.parse(m2, anim.empty() ? nullptr : &anim);
        SourceStats& S = st[src];
        ++S.models;
        if (!m.emitters.empty()) ++S.modelsWithEmitters;
        if (!m.ribbons.empty()) ++S.modelsWithRibbons;
        S.emitters += m.emitters.size();
        S.ribbons += m.ribbons.size();
        auto ex = [&](const std::string& k) {
            S.misc.add(k);
            if (!S.example.count(k)) S.example[k] = rel;
        };
        if (m.ribbons.size() > 1) ex("ribbons: model with >1 ribbon (upstream misreads 2nd+: stride 0xAC vs 0xB0)");
        for (size_t ei = 0; ei < m.emitters.size(); ++ei) {
            const auto& e = m.emitters[ei];
            for (int b = 0; b < 32; ++b)
                if (e.flags & (1u << b)) ++S.flagBits[b];
            S.type.add(std::to_string(e.type));
            S.blend.add(std::to_string(e.blend));
            S.ptype.add(std::to_string(e.particleType));
            S.hot.add(std::to_string(e.headOrTail));
            S.tiles.add(std::to_string(e.rows) + "x" + std::to_string(e.cols) + (pow2(e.rows) && pow2(e.cols) ? "" : " (not pow2)"));
            S.colorKeys.add(std::to_string(e.color.ts.size()));
            S.alphaKeys.add(std::to_string(e.alpha.ts.size()));
            S.scaleKeys.add(std::to_string(e.scale.ts.size()));
            S.headKeys.add(std::to_string(e.headCell.ts.size()));
            S.tailKeys.add(std::to_string(e.tailCell.ts.size()));
            if (e.color.ts.size() == 3) S.midTs.add(std::to_string(e.color.ts[1]));
            const bool lut = m2raw::lutEligible(e);
            if (lut) ex("lookup-table path: scale.y = scale.x, scale vary ignored");
            if (e.lifeVary != 0) ex("lifespanVary != 0");
            if (e.rateVary != 0) ex("emissionRateVary != 0");
            if (e.scaleVary.x != 0 || e.scaleVary.y != 0) ex("scaleVary != 0");
            if (e.tailLength != 0) ex("tailLength != 0");
            if (e.twinklePercent < 1.0f) ex("twinklePercent < 1 (hidden by the twinkle table)");
            if (e.twinkleMin != 1.0f || e.twinkleMax != 1.0f) ex("twinkleScale != (1,1)");
            if (e.twinkleMin == 0.0f && e.twinkleMax == 0.0f) ex("twinkleScale (0,0): every quad has size 0");
            if (e.burst != 0) ex("burstMultiplier != 0");
            if (e.drag != 0) ex("drag != 0");
            if (e.spin != 0 || e.spinVary != 0) ex("spin / spinVary != 0 (rotated quads)");
            if (e.baseSpin != 0 && e.spin == 0 && e.spinVary == 0) ex("baseSpin != 0 with spin = spinVary = 0 (no rotation)");
            if (e.baseSpinVary != 0) ex("baseSpinVary != 0");
            if (e.tumbleMin != glm::vec3(0) || e.tumbleMax != glm::vec3(0)) ex("tumble != 0 (model particles only)");
            if (e.wind != glm::vec3(0) && e.windTime > 0) ex("wind (vector, windTime > 0)");
            if (e.followSpeed1 != e.followSpeed2 || e.followScale1 != 0 || e.followScale2 != 0) ex("follow speeds/scales set");
            if (!e.geometryModel.empty()) ex("geometry model (model particles)");
            if (!e.recursionModel.empty()) ex("recursion model");
            if (!e.spline.empty()) ex("spline points");
            if (e.colorIndex) ex("particleColorIndex != 0 (ParticleColor.dbc)");
            if (e.tileRotation) ex("textureTileRotation != 0 (unused)");
            const uint32_t enk = m.keyCount(e.enabledIn, 0);
            S.enabled.add(enk ? "keys in seq 0" : (e.enabledIn.any ? "keys only in other seqs" : "none"));
            if (enk) {
                float minv = 1e9f;
                for (float t = 0; t <= (m.sequences.empty() ? 0.0f : float(m.sequences[0].duration)); t += 50.0f)
                    minv = std::min(minv, m.sampleF(e.enabledIn, 0, t, 1.0f));
                if (minv < 0.5f) ex("enabledIn = 0 at some time of seq 0 (emission gated)");
            }
            const m2raw::Track* tr[] = {&e.speed, &e.speedVar, &e.vRange, &e.hRange, &e.gravity, &e.life, &e.rate, &e.areaL, &e.areaW, &e.zSource};
            for (auto* t : tr) {
                if (t->any) S.interp.add(std::to_string(t->interp));
                if (t->gseq >= 0) ex("track on a global sequence");
                if (m.keyCount(*t, 0) > 1) ex("track animated in seq 0");
            }
            const float sv = m.sampleF(e.speedVar, 0, 0, 0), zs = m.sampleF(e.zSource, 0, 0, 0);
            const float aL = m.sampleF(e.areaL, 0, 0, 0), aW = m.sampleF(e.areaW, 0, 0, 0);
            const float g = m.sampleF(e.gravity, 0, 0, 0);
            if (sv != 0) ex("speedVariation != 0");
            if (zs != 0) ex("zSource != 0 (radial from (0,0,zSource))");
            if (aL != 0 || aW != 0) ex(e.type == 2 ? "sphere radius (areaL/areaW) != 0" : "plane area (areaL/areaW) != 0");
            if (g < 0) ex("gravity < 0 (rises)");
            if (g == 0) ex("gravity 0");
            if (g > 0) ex("gravity > 0");
            if (tsv)
                std::fprintf(tsv, "%s\t%s\t%zu\t%u\t%08x\t%u\t%u\t%u\t%zu\t%zu\t%zu\t%zu\t%zu\t%d\t%g\t%g\t%g\t%g\t%g\t%g\t%g\t%g\t%g\t%g\t%g\t"
                                  "%g,%g,%g\t%g\t%g/%g/%g/%g\t%g,%g\t%u\t%g\t%g\t%g\t%g\t%g\t%g\t%g\t%g\t%g\t%s\t%s\t%zu\n",
                             srcName(src), rel.c_str(), ei, e.type, e.flags, e.blend, e.rows, e.cols, e.color.ts.size(),
                             e.alpha.ts.size(), e.scale.ts.size(), e.headCell.ts.size(), e.tailCell.ts.size(), lut ? 1 : 0,
                             e.lifeVary, e.rateVary, e.tailLength, e.twinklePercent, e.twinkleMin, e.twinkleMax, e.burst, e.drag,
                             e.spin, e.spinVary, e.baseSpin, e.wind.x, e.wind.y, e.wind.z, e.windTime, e.followSpeed1,
                             e.followScale1, e.followSpeed2, e.followScale2, e.scaleVary.x, e.scaleVary.y, enk,
                             m.sampleF(e.speed, 0, 0, 0), m.sampleF(e.vRange, 0, 0, 0), m.sampleF(e.hRange, 0, 0, 0), g,
                             m.sampleF(e.life, 0, 0, 0), m.sampleF(e.rate, 0, 0, 0), aL, aW, zs, e.geometryModel.c_str(),
                             e.recursionModel.c_str(), e.spline.size());
        }
        for (const auto& r : m.ribbons) {
            if (r.gravity != 0) S.ribbonMisc.add("gravity != 0");
            if (m.keyCount(r.color, 0) > 1) S.ribbonMisc.add("colour animated in seq 0");
            if (m.keyCount(r.alpha, 0) > 1) S.ribbonMisc.add("alpha animated in seq 0");
            if (m.keyCount(r.above, 0) > 1 || m.keyCount(r.below, 0) > 1) S.ribbonMisc.add("heights animated in seq 0");
            if (m.keyCount(r.texSlot, 0) > 0) S.ribbonMisc.add("texSlot keys");
            if (m.keyCount(r.visibility, 0) > 1) S.ribbonMisc.add("visibility animated in seq 0");
            if (r.materials.size() > 1) S.ribbonMisc.add("several materials (one pass each)");
            if (r.edgeLifetime < 0.25f) S.ribbonMisc.add("edgeLifetime < 0.25 (raised to 0.25)");
            if (r.rows * r.cols > 1) S.ribbonMisc.add("texture tiles > 1");
            if (r.bone == 0xFFFFFFFFu) S.ribbonMisc.add("bone -1 (upstream drops it)");
        }
    }
    if (tsv) std::fclose(tsv);
    FILE* f = std::fopen(outMd.c_str(), "wb");
    if (!f) return 1;
    std::fprintf(f, "# psp_particle_matrix --stats\n\nM2 read: %zu (unreadable %zu)\n\n", done - unreadable, unreadable);
    std::fprintf(f, "| | doodads | gameobjects | spellfx |\n|---|---|---|---|\n");
    std::fprintf(f, "| models | %zu | %zu | %zu |\n", st[0].models, st[1].models, st[2].models);
    std::fprintf(f, "| with particle emitters | %zu | %zu | %zu |\n", st[0].modelsWithEmitters, st[1].modelsWithEmitters,
                 st[2].modelsWithEmitters);
    std::fprintf(f, "| emitters | %zu | %zu | %zu |\n", st[0].emitters, st[1].emitters, st[2].emitters);
    std::fprintf(f, "| with ribbons | %zu | %zu | %zu |\n", st[0].modelsWithRibbons, st[1].modelsWithRibbons, st[2].modelsWithRibbons);
    std::fprintf(f, "| ribbons | %zu | %zu | %zu |\n\n", st[0].ribbons, st[1].ribbons, st[2].ribbons);
    std::fprintf(f, "## M2 flag bits (emitters)\n\n| bit | doodads | gameobjects | spellfx |\n|---|---|---|---|\n");
    for (int b = 0; b < 32; ++b)
        if (st[0].flagBits[b] || st[1].flagBits[b] || st[2].flagBits[b])
            std::fprintf(f, "| 0x%X | %zu | %zu | %zu |\n", 1u << b, st[0].flagBits[b], st[1].flagBits[b], st[2].flagBits[b]);
    auto table = [&](const char* title, Hist SourceStats::*h) {
        std::set<std::string> keys;
        for (auto& s : st)
            for (auto& [k, v] : (s.*h).n) keys.insert(k);
        std::fprintf(f, "\n## %s\n\n| value | doodads | gameobjects | spellfx |\n|---|---|---|---|\n", title);
        for (auto& k : keys) {
            std::fprintf(f, "| %s |", k.c_str());
            for (auto& s : st) {
                auto it = (s.*h).n.find(k);
                std::fprintf(f, " %zu |", it == (s.*h).n.end() ? size_t(0) : it->second);
            }
            std::fprintf(f, "\n");
        }
    };
    table("emitterType", &SourceStats::type);
    table("blendingType", &SourceStats::blend);
    table("particleType byte (0x2C)", &SourceStats::ptype);
    table("headOrTail byte (0x2D)", &SourceStats::hot);
    table("texture tiles rows x cols", &SourceStats::tiles);
    table("colour FBlock keys", &SourceStats::colorKeys);
    table("alpha FBlock keys", &SourceStats::alphaKeys);
    table("scale FBlock keys", &SourceStats::scaleKeys);
    table("headCell FBlock keys", &SourceStats::headKeys);
    table("tailCell FBlock keys", &SourceStats::tailKeys);
    table("3-key colour middle timestamp (fixed16)", &SourceStats::midTs);
    table("enabledIn track", &SourceStats::enabled);
    table("track interpolation (any key)", &SourceStats::interp);
    table("field use (emitters / models)", &SourceStats::misc);
    table("ribbons", &SourceStats::ribbonMisc);
    std::fprintf(f, "\n## examples\n\n");
    for (auto& s : st)
        for (auto& [k, v] : s.example) std::fprintf(f, "- %s: `%s`\n", k.c_str(), v.c_str());
    std::fclose(f);
    std::printf("stats: %s\n", outMd.c_str());
    return 0;
}

// --- compare ----------------------------------------------------------------------------------------------------

std::vector<PakItem> g_texItems;  // the bank's textures (not written: the comparison needs only the effects)

float g_bakeFps = 15.0f;   // --bake-fps / --bake-frames: the bake's curve sampling (the PSP's: 15 / 48)
int g_bakeFrames = 48;
bool g_top = false;        // --top: the pictures from above (the "ring" seen on hardware)

int runCompare(const std::vector<std::string>& models, const std::string& outDir, float seconds, float fps) {
    namespace pm = wowee::pspconv::ptcmatrix;
    namespace sfx = wowee::rendering::psp;
    namespace sim = wowee::rendering::psp::ptcsim;
    TextureBank bank(g_root, 64, g_texItems);
    std::set<std::string> missing;
    FxReadFn read = [](const std::string& gp, std::vector<uint8_t>& out) { return readGame(gp, out); };
    FILE* tsv = std::fopen((outDir + "/ptc_compare.tsv").c_str(), "wb");
    if (tsv)
        std::fprintf(tsv, "model\temitters\tref_born\tpsp_born\tup_born\tmatched\tpos_err_max\tpos_err_mean\tvel_err_max\t"
                          "half_err_max\trgba_err_max\tvis_diff\tref_half\tup_half\tref_vz\tup_vz\tref_horiz\tup_horiz\t"
                          "ref_live\tup_live\twhy\n");
    const float dt = 1.0f / std::max(fps, 1.0f);
    const int frames = static_cast<int>(seconds * fps + 0.5f);
    for (const std::string& rel : models) {
        pm::CompareStats cs;
        cs.model = rel;
        std::vector<uint8_t> bytes;
        if (!readGame(rel, bytes)) {
            cs.why = "missing";
        } else {
            pipeline::M2Model m = pipeline::M2Loader::load(bytes);
            const auto cls = rendering::classifyM2Model(rel, m.boundMin, m.boundMax, m.vertices.size(), m.particleEmitters.size());
            FxBakeOptions fo;  // TOOL-2: mesh off, forceV2, the model's class, --bake-fps / --bake-frames
            fo.mesh = false;
            fo.forceV2 = true;
            fo.cls = ptcClassOf(cls);
            fo.maxEmitters = sfx::kFxMaxEmitters;
            fo.fps = g_bakeFps;
            fo.maxFrames = g_bakeFrames;
            std::vector<uint8_t> blobBytes;
            FxBakeStats st;
            if (!bakeFx(rel, bytes, m, fo, bank, read, missing, blobBytes, st)) {
                cs.why = "not baked: " + st.why;
            } else if (!st.level3) {
                cs.why = "no level-3 data";
            } else {
                pm::Blob B;
                B.bytes = std::move(blobBytes);
                B.set();
                sfx::FxView view;
                std::vector<uint8_t> anim;
                m2raw::Model pre;
                pre.parse(bytes);
                if (pre.seqInAnim(0) && !pre.sequences.empty()) {
                    char tail[32];
                    std::snprintf(tail, sizeof(tail), "%04u-%02u.anim", pre.sequences[0].id, pre.sequences[0].variation);
                    readGame(rel.substr(0, rel.size() - 3) + tail, anim);
                }
                m2raw::Model raw;
                raw.parse(bytes, anim.empty() ? nullptr : &anim);
                cs.emitters = B.h->emitterCount;
                cs.refEmitters = static_cast<int>(raw.emitters.size());
                // TOOL-3: the bone poses in full precision (upstream's sampler, sequence 0 at the wrapped time).
                std::vector<glm::mat4> poses;
                float posesAt = -1.0f;
                const ptcref::BoneSampler bones = [&](int bone, float tMs, sim::M34& out) {
                    sim::identity(out);
                    if (bone < 0 || static_cast<size_t>(bone) >= m.bones.size()) return;
                    if (tMs != posesAt) {
                        fxPoseBones(m, tMs, poses);
                        posesAt = tMs;
                    }
                    const glm::mat4& p = poses[static_cast<size_t>(bone)];
                    for (int c = 0; c < 3; ++c)
                        for (int r = 0; r < 3; ++r) out.l[c * 3 + r] = p[c][r];
                    for (int r = 0; r < 3; ++r) out.t[r] = p[3][r];
                };
                pm::UpSim us;
                us.init(B, 1234u);
                std::vector<pm::Snap> c;
                std::vector<pm::Snap> pic[2][3];
                const int picFrames[2] = {static_cast<int>(1.5f * fps), frames - 1};
                std::set<uint32_t> refSerials, engSerials;
                auto toSnap = [](const std::vector<ptcref::SimParticle>& in, std::vector<pm::Snap>& out) {
                    out.clear();
                    for (const auto& p : in) {
                        pm::Snap s{};
                        s.serial = static_cast<int>(p.serial);
                        s.emitter = p.emitter;
                        s.pos = p.pos;
                        s.vel = p.vel;
                        s.half[0] = p.half[0];
                        s.half[1] = p.half[1];
                        for (int k = 0; k < 4; ++k) s.rgba[k] = p.rgba[k];
                        s.visible = p.visible;
                        out.push_back(s);
                    }
                };
                ptcref::RunConfig rc;
                rc.seconds = static_cast<float>(frames) / std::max(fps, 1.0f);
                rc.fps = fps;
                ptcref::CompareStats rs;
                const auto hook = [&](int f, float tMs, const std::vector<ptcref::SimParticle>& eng,
                                      const std::vector<ptcref::SimParticle>& ref) {
                    us.frame(tMs, dt);
                    us.snap(c);
                    for (const auto& r : ref) {
                        refSerials.insert(r.serial);
                        if (!r.visible) continue;
                        cs.refHalfMean += r.half[0];
                        cs.refVz += r.vel.z;
                        cs.refHoriz += std::sqrt(r.vel.x * r.vel.x + r.vel.y * r.vel.y);
                        ++cs.refN;
                    }
                    for (const auto& e : eng) engSerials.insert(e.serial);
                    for (const auto& r : c) {
                        cs.upHalfMean += r.half[0];
                        cs.upVz += r.vel.z;
                        cs.upHoriz += std::sqrt(r.vel.x * r.vel.x + r.vel.y * r.vel.y);
                        ++cs.upN;
                    }
                    cs.refLive += ref.size();
                    cs.upLive += c.size();
                    ++cs.frames;
                    for (int k = 0; k < 2; ++k)
                        if (f == picFrames[k]) {
                            toSnap(ref, pic[k][0]);
                            toSnap(eng, pic[k][1]);
                            pic[k][2] = c;
                        }
                };
                if (!sfx::fxViewFromBlob(B.bytes.data(), static_cast<uint32_t>(B.bytes.size()), view) ||
                    !ptcref::runCompare(raw, st.emitterIndex, bones, B.h->periodMs, view, rc, rs, hook)) {
                    cs.why = "compare failed";
                } else {
                    cs.matched = static_cast<size_t>(rs.matched);
                    cs.posErrMax = rs.posErrMax;
                    cs.posErrSum = rs.posErrSum;
                    cs.velErrMax = rs.velErrMax;
                    cs.halfErrMax = rs.halfErrMax;
                    cs.rgbaErrMax = rs.colErrMax;
                    cs.visDiff = static_cast<double>(rs.visibilityMismatch);
                    cs.refBorn = refSerials.size();
                    cs.pspBorn = engSerials.size();
                    cs.upBorn = 0;
                    // The picture: reference | PSP | upstream at 1.5 s and at the end, the camera 1.8 x the size away.
                    glm::vec3 lo(1e9f), hi(-1e9f);
                    for (int k = 0; k < 2; ++k)
                        for (const auto& s : pic[k][0])
                            if (s.visible) {
                                lo = glm::min(lo, s.pos);
                                hi = glm::max(hi, s.pos);
                            }
                    if (lo.x > hi.x) lo = hi = glm::vec3(0.0f);
                    lo = glm::min(lo, glm::vec3(-0.5f));
                    hi = glm::max(hi, glm::vec3(0.5f));
                    const glm::vec3 center = (lo + hi) * 0.5f;
                    const float ext = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
                    pm::View v;
                    v.center = center;
                    v.eye = g_top ? center + glm::vec3(0.0f, -0.01f, std::max(ext * 1.6f, 3.0f))
                                  : center + glm::vec3(0.0f, -std::max(ext * 1.6f, 3.0f), ext * 0.25f);
                    const int pw = 220, ph = 220;
                    v.focal = (ph * 0.5f) / std::tan(glm::radians(27.0f));
                    pm::Image im;
                    im.init(pw * 3, ph * 2, 0.06f);
                    for (int k = 0; k < 2; ++k)
                        for (int col = 0; col < 3; ++col) pm::drawPanel(im, col * pw, k * ph, pw, ph, v, pic[k][col]);
                    std::string name = rel;
                    for (char& ch : name)
                        if (ch == '/' || ch == '\\' || ch == ' ') ch = '_';
                    im.savePpm(outDir + "/" + name + (g_top ? ".top.ppm" : ".ppm"));
                }
            }
        }
        if (tsv) {
            std::fprintf(tsv, "%s\t%d/%d\t%zu\t%zu\t%zu\t%zu\t%.5f\t%.5f\t%.5f\t%.5f\t%.4f\t%.0f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.1f\t%.1f\t%s\n",
                         cs.model.c_str(), cs.emitters, cs.refEmitters, cs.refBorn, cs.pspBorn, cs.upBorn, cs.matched, cs.posErrMax,
                         cs.matched ? cs.posErrSum / cs.matched : 0.0, cs.velErrMax, cs.halfErrMax, cs.rgbaErrMax, cs.visDiff,
                         cs.refN ? cs.refHalfMean / cs.refN : 0.0, cs.upN ? cs.upHalfMean / cs.upN : 0.0, cs.refN ? cs.refVz / cs.refN : 0.0,
                         cs.upN ? cs.upVz / cs.upN : 0.0, cs.refN ? cs.refHoriz / cs.refN : 0.0, cs.upN ? cs.upHoriz / cs.upN : 0.0,
                         cs.frames ? cs.refLive / cs.frames : 0.0, cs.frames ? cs.upLive / cs.frames : 0.0, cs.why.c_str());
            std::fflush(tsv);
        }
        std::printf("%s: emitters %d, born ref %zu psp %zu, matched %zu, pos err max %.4f vel %.4f half %.4f rgba %.3f vis %.0f %s\n",
                    rel.c_str(), cs.emitters, cs.refBorn, cs.pspBorn, cs.matched, cs.posErrMax, cs.velErrMax, cs.halfErrMax,
                    cs.rgbaErrMax, cs.visDiff, cs.why.c_str());
    }
    if (tsv) std::fclose(tsv);
    return 0;
}

// --- dump (debugging one model) --------------------------------------------------------------------------------
int runDump(const std::string& rel) {
    std::vector<uint8_t> bytes;
    if (!readGame(rel, bytes)) return 1;
    m2raw::Model pre;
    pre.parse(bytes);
    std::vector<uint8_t> anim;
    if (pre.seqInAnim(0) && !pre.sequences.empty()) {
        char tail[32];
        std::snprintf(tail, sizeof(tail), "%04u-%02u.anim", pre.sequences[0].id, pre.sequences[0].variation);
        const bool ok = readGame(rel.substr(0, rel.size() - 3) + tail, anim);
        std::printf("seq0 in .anim %s: %s\n", tail, ok ? "read" : "MISSING");
    }
    m2raw::Model m;
    m.parse(bytes, anim.empty() ? nullptr : &anim);
    std::printf("%s: seqs %zu (seq0 id %u dur %u flags %x), gseq %zu, emitters %zu, ribbons %zu\n", rel.c_str(), m.sequences.size(),
                m.sequences.empty() ? 0 : m.sequences[0].id, m.sequences.empty() ? 0 : m.sequences[0].duration,
                m.sequences.empty() ? 0 : m.sequences[0].flags, m.globalSeq.size(), m.emitters.size(), m.ribbons.size());
    auto tr = [&](const char* n, const m2raw::Track& t) {
        std::printf("    %s: interp %u gseq %d seqs %u keys0 %u v(0)=%g v(500)=%g v(1500)=%g\n", n, t.interp, t.gseq, t.tsCount,
                    m.keyCount(t, 0), m.sampleF(t, 0, 0, -999), m.sampleF(t, 0, 500, -999), m.sampleF(t, 0, 1500, -999));
    };
    for (size_t i = 0; i < m.emitters.size(); ++i) {
        const auto& e = m.emitters[i];
        std::printf("  emitter %zu: type %u flags %08x blend %u bone %u tex %u rows %u cols %u lifeVary %g rateVary %g tail %g "
                    "twinkle %g/%g/%g-%g spin %g/%g base %g/%g drag %g wind %g,%g,%g/%g scaleVary %g,%g burst %g lut %d\n",
                    i, e.type, e.flags, e.blend, e.bone, e.texture, e.rows, e.cols, e.lifeVary, e.rateVary, e.tailLength,
                    e.twinkleSpeed, e.twinklePercent, e.twinkleMin, e.twinkleMax, e.spin, e.spinVary, e.baseSpin, e.baseSpinVary,
                    e.drag, e.wind.x, e.wind.y, e.wind.z, e.windTime, e.scaleVary.x, e.scaleVary.y, e.burst, m2raw::lutEligible(e) ? 1 : 0);
        tr("rate", e.rate);
        tr("life", e.life);
        tr("speed", e.speed);
        tr("speedVar", e.speedVar);
        tr("vRange", e.vRange);
        tr("hRange", e.hRange);
        tr("gravity", e.gravity);
        tr("areaL", e.areaL);
        tr("areaW", e.areaW);
        tr("zSource", e.zSource);
        tr("enabledIn", e.enabledIn);
        auto blk = [&](const char* n, const m2raw::FBlock& f) {
            std::printf("    %s: ts", n);
            for (auto t : f.ts) std::printf(" %u", t);
            std::printf(" | v");
            for (auto v : f.v) std::printf(" %g", v);
            std::printf("\n");
        };
        blk("color", e.color);
        blk("alpha", e.alpha);
        blk("scale", e.scale);
        blk("head", e.headCell);
        blk("tail", e.tailCell);
    }
    for (size_t i = 0; i < m.ribbons.size(); ++i) {
        const auto& r = m.ribbons[i];
        std::printf("  ribbon %zu: bone %u tex %zu mat %zu eps %g life %g grav %g rows %u cols %u\n", i, r.bone, r.textures.size(),
                    r.materials.size(), r.edgesPerSecond, r.edgeLifetime, r.gravity, r.rows, r.cols);
        tr("color", r.color);
        tr("alpha", r.alpha);
        tr("above", r.above);
        tr("below", r.below);
        tr("visibility", r.visibility);
        tr("texSlot", r.texSlot);
    }
    return 0;
}

}  // namespace

namespace wowee::rendering::psp {
// The PC has no world light: lit particles take no tint (psp_particles_l3.cpp asks for frameLight()).
FrameLight& frameLight() {
    static FrameLight light;
    return light;
}
}  // namespace wowee::rendering::psp

int main(int argc, char** argv) {
    std::string list, stats, tsv, compare, out;
    float seconds = 6.0f, fps = 30.0f;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--extracted") g_root = next();
        else if (a == "--list") list = next();
        else if (a == "--stats") stats = next();
        else if (a == "--tsv") tsv = next();
        else if (a == "--compare") compare = next();
        else if (a == "--dump") return g_root.empty() ? 2 : runDump(lower(next()));
        else if (a == "--out") out = next();
        else if (a == "--seconds") seconds = static_cast<float>(std::atof(next().c_str()));
        else if (a == "--fps") fps = static_cast<float>(std::atof(next().c_str()));
        else if (a == "--bake-fps") g_bakeFps = static_cast<float>(std::atof(next().c_str()));
        else if (a == "--bake-frames") g_bakeFrames = std::atoi(next().c_str());
        else if (a == "--top") g_top = true;
    }
    if (g_root.empty()) {
        std::fprintf(stderr, "usage: psp_particle_matrix --extracted <dir> --list <m2 list> --stats <out.md> [--tsv <out.tsv>]\n");
        return 2;
    }
    if (!stats.empty()) return runStats(readList(list), stats, tsv);
    if (!compare.empty()) return runCompare(readList(compare), out.empty() ? std::string(".") : out, seconds, fps);
    return 2;
}
