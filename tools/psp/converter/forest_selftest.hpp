// psp_model_conv --selftest <dir>: the forest LOD code on a
// procedural tree, no client data needed. A trunk (8-sided cylinder, opaque bark)
// and a canopy of leaf cards (alpha-tested quads, the way the client's trees are built)
// go through buildLod2 + ImpostorBaker + WmdlBuilder into <dir>/selftest.pak
// (m_0, i_0, x_0..2, d_0_0), which verifyPak then reads back; the impostor
// atlas is dumped as <dir>/impostor_0.png. Fails when LOD 2 does not shrink
// the tree, a view is empty or full, a cell margin is not transparent, or the
// pak does not verify.
#pragma once

#include "forest_bake.hpp"
#include "rendering/psp/psp_impostor_math.hpp"

#include <random>

namespace wowee::pspconv {

namespace selftest {

inline pipeline::BLPImage picture(int w, int h, bool leaf) {
    pipeline::BLPImage img;
    img.width = w;
    img.height = h;
    img.data.resize(static_cast<size_t>(w) * h * 4);
    std::mt19937 rng(leaf ? 7u : 11u);
    std::uniform_real_distribution<float> jitter(-0.08f, 0.08f);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &img.data[(static_cast<size_t>(y) * w + x) * 4];
            const float u = (x + 0.5f) / w, v = (y + 0.5f) / h;
            if (leaf) {
                // Five leaf blobs: alpha 255 inside, 0 outside (alpha key).
                const float cx[5] = {0.3f, 0.7f, 0.5f, 0.25f, 0.72f}, cy[5] = {0.3f, 0.3f, 0.55f, 0.75f, 0.74f};
                bool in = false;
                for (int k = 0; k < 5; ++k) {
                    const float dx = u - cx[k], dy = v - cy[k];
                    in = in || dx * dx + dy * dy < 0.035f;
                }
                const float g = 0.45f + jitter(rng);
                p[0] = static_cast<uint8_t>(quant(0.18f + jitter(rng), 255));
                p[1] = static_cast<uint8_t>(quant(g, 255));
                p[2] = static_cast<uint8_t>(quant(0.12f, 255));
                p[3] = in ? 255 : 0;
            } else {
                const float stripe = 0.35f + 0.1f * std::sin(u * 40.0f) + jitter(rng);
                p[0] = static_cast<uint8_t>(quant(stripe, 255));
                p[1] = static_cast<uint8_t>(quant(stripe * 0.7f, 255));
                p[2] = static_cast<uint8_t>(quant(stripe * 0.45f, 255));
                p[3] = 255;
            }
        }
    }
    return img;
}

inline pipeline::M2Vertex vtx(glm::vec3 p, glm::vec3 n, glm::vec2 uv) {
    pipeline::M2Vertex v{};
    v.position = p;
    v.normal = n;
    v.texCoords[0] = v.texCoords[1] = uv;
    v.boneWeights[0] = 255;
    return v;
}

// An opaque post with one branch along model +x, baked, then looked at the way
// the PSP draws it (psp_impostors.cpp): for cameras around a placement with
// several yaws, the view the PSP picks must show the branch on the side of
// the quad where it is in the world (quad +u = (-ty, tx), t = towards camera).
inline bool orientationTest(const ForestOptions& opt) {
    std::vector<pipeline::M2Vertex> verts;
    ForestBatch b;
    auto box = [&](glm::vec3 lo, glm::vec3 hi) {
        const uint32_t base = static_cast<uint32_t>(verts.size());
        for (int i = 0; i < 8; ++i)
            verts.push_back(vtx(glm::vec3(i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z),
                                glm::vec3(0, 0, 1), {0.5f, 0.5f}));
        const uint32_t f[36] = {0, 1, 3, 0, 3, 2, 4, 5, 7, 4, 7, 6, 0, 1, 5, 0, 5, 4,
                                2, 3, 7, 2, 7, 6, 0, 2, 6, 0, 6, 4, 1, 3, 7, 1, 7, 5};
        for (uint32_t k : f) b.tris.push_back(base + k);
    };
    box({-0.3f, -0.3f, 0.0f}, {0.3f, 0.3f, 8.0f});  // post
    box({0.0f, -0.3f, 5.0f}, {4.0f, 0.3f, 6.0f});   // branch along +x
    b.ob.blend = fmt::kBlendOpaque;
    b.ob.texture = fmt::kNoTexture;
    ImpostorBaker baker("");
    ImpostorResult ir;
    if (!baker.bake({b}, verts, opt, ir)) return false;
    // Branch side per view: coverage centroid of the branch rows (z 5..6)
    // minus that of the post alone (z 1..4), in u of the inner cell.
    const int cell = ir.h.cellSize, m = ir.h.margin, inner = cell - 2 * m, aw = ir.atlas.width;
    auto centroid = [&](int view, float zLo, float zHi) {
        const int x0 = (view % ir.h.cols) * cell + m, y0 = (view / ir.h.cols) * cell + m;
        double sum = 0.0, n = 0.0;
        for (int y = 0; y < inner; ++y) {
            const float z = ir.h.zMax - (y + 0.5f) / inner * (ir.h.zMax - ir.h.zMin);
            if (z < zLo || z > zHi) continue;
            for (int x = 0; x < inner; ++x)
                if (ir.atlas.data[(static_cast<size_t>(y0 + y) * aw + x0 + x) * 4 + 3] > 128) {
                    sum += (x + 0.5) / inner;
                    n += 1.0;
                }
        }
        return n > 0 ? sum / n : 0.5;
    };
    auto side = [&](int view) { return centroid(view, 5.1f, 5.9f) - centroid(view, 1.0f, 4.0f); };
    constexpr float kPi = 3.14159265358979f;
    int checked = 0, failed = 0;
    for (float yawDeg : {0.0f, 37.0f, 90.0f, 200.0f, -120.0f}) {
        const float yaw = yawDeg * kPi / 180.0f;
        const glm::vec2 xAxis(std::cos(yaw), std::sin(yaw));  // placement's model x in world (= branch)
        for (int c = 0; c < 16; ++c) {
            const float ca = 2.0f * kPi * (c + 0.3f) / 16.0f;
            const glm::vec2 t(std::cos(ca), std::sin(ca));  // towards the camera
            const int view = rendering::psp::impostorView(t.x, t.y, xAxis.x, xAxis.y, ir.h.views);
            const glm::vec2 right(-t.y, t.x);
            const float expect = glm::dot(xAxis, right);
            if (std::fabs(expect) < 0.5f) continue;  // branch mostly along the view: no side
            ++checked;
            const double got = side(view);
            if ((expect > 0.0f) != (got > 0.0) || std::fabs(got) < 0.05) {
                ++failed;
                std::fprintf(stderr, "orientation: yaw %.0f cam %d view %d expect %+.2f got %+.3f\n", yawDeg, c, view,
                             expect, got);
            }
        }
    }
    const rendering::psp::ImpostorCellUv uv = rendering::psp::impostorCellUv(ir.h, 5);
    const bool uvOk = uv.u0 == ((static_cast<uint32_t>(cell) + m) << 15) / static_cast<uint32_t>(aw) &&
                      uv.v1 == ((2u * cell - m) << 15) / static_cast<uint32_t>(ir.atlas.height);
    std::printf("selftest orientation: %d camera/yaw cases, %d wrong, cell uv %s\n", checked, failed,
                uvOk ? "ok" : "BAD");
    return checked > 20 && failed == 0 && uvOk;
}

} // namespace selftest

inline bool forestSelfTest(const std::string& dir, ForestOptions opt) {
    using selftest::vtx;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::vector<pipeline::M2Vertex> verts;
    ForestBatch trunk, leaves;
    // Trunk: 8 sides, radius 0.5, height 7.
    constexpr float kPi = 3.14159265358979f;
    for (int i = 0; i <= 8; ++i) {
        const float a = 2.0f * kPi * static_cast<float>(i) / 8.0f;
        const glm::vec3 n(std::cos(a), std::sin(a), 0.0f);
        verts.push_back(vtx(n * 0.5f, n, {i / 8.0f, 1.0f}));
        verts.push_back(vtx(n * 0.35f + glm::vec3(0, 0, 7.0f), n, {i / 8.0f, 0.0f}));
    }
    for (uint32_t i = 0; i < 8; ++i) {
        const uint32_t a = i * 2, b = a + 1, c = a + 2, d = a + 3;
        trunk.tris.insert(trunk.tris.end(), {a, c, b, b, c, d});
    }
    trunk.ob.texture = 0;
    trunk.ob.blend = fmt::kBlendOpaque;
    trunk.ob.flags = fmt::kBatchWrapU | fmt::kBatchWrapV;
    trunk.texName = "selftest/bark.blp";
    // Canopy: 220 cards of 2.2 x 2.2 yd in an ellipsoid around z = 8.
    std::mt19937 rng(1234u);
    std::uniform_real_distribution<float> u01(0.0f, 1.0f);
    for (int c = 0; c < 220; ++c) {
        glm::vec3 p;
        do {
            p = glm::vec3(u01(rng) * 2 - 1, u01(rng) * 2 - 1, u01(rng) * 2 - 1);
        } while (glm::dot(p, p) > 1.0f);
        const glm::vec3 centre = p * glm::vec3(4.5f, 4.5f, 3.0f) + glm::vec3(0, 0, 8.5f);
        glm::vec3 n = glm::normalize(glm::vec3(u01(rng) - 0.5f, u01(rng) - 0.5f, u01(rng) - 0.5f) + p * 0.8f);
        glm::vec3 t = glm::normalize(glm::cross(n, std::fabs(n.z) < 0.9f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0)));
        const glm::vec3 bt = glm::cross(n, t);
        const float s = 1.1f * (0.7f + 0.6f * u01(rng));
        const uint32_t base = static_cast<uint32_t>(verts.size());
        verts.push_back(vtx(centre - t * s - bt * s, n, {0, 0}));
        verts.push_back(vtx(centre + t * s - bt * s, n, {1, 0}));
        verts.push_back(vtx(centre - t * s + bt * s, n, {0, 1}));
        verts.push_back(vtx(centre + t * s + bt * s, n, {1, 1}));
        leaves.tris.insert(leaves.tris.end(), {base, base + 1, base + 2, base + 2, base + 1, base + 3});
    }
    leaves.ob.texture = 1;
    leaves.ob.blend = fmt::kBlendAlphaKey;
    leaves.texName = "selftest/leaf.blp";
    const std::vector<ForestBatch> batches = {trunk, leaves};
    const size_t lod0 = (trunk.tris.size() + leaves.tris.size()) / 3;

    std::vector<PakItem> items;
    TextureBank tex(dir, 128, items);
    ImpostorBaker baker(dir);
    const pipeline::BLPImage bark = selftest::picture(64, 64, false), leaf = selftest::picture(64, 64, true);
    baker.preload(trunk.texName, bark);
    baker.preload(leaves.texName, leaf);
    if (tex.add(trunk.texName, bark) != 0 || tex.add(leaves.texName, leaf) != 1) return false;

    WmdlBuilder o;
    for (const ForestBatch& b : batches) {
        std::vector<uint16_t> idx(b.tris.begin(), b.tris.end());
        if (!o.add(idx, 0, static_cast<uint32_t>(idx.size()), 0, b.uvSet, b.ob)) return false;
    }
    Lod2Stats st;
    std::vector<ForestBatch> lod2Batches;
    const size_t lod2 = buildLod2(batches, verts, opt, o, st, &lod2Batches);
    ImpostorResult ir, irLod2;
    if (!baker.bake(batches, verts, opt, ir) || !baker.bake(lod2Batches, verts, opt, irLod2)) {
        std::fprintf(stderr, "selftest: impostor bake failed\n");
        return false;
    }
    // LOD 2 seen from the same views (the rasterizer as a checker): the
    // decimated canopy must cover about as much as the full one.
    writePng(dir + "/lod2_views.png", irLod2.atlas.width, irLod2.atlas.height, irLod2.atlas.data);
    ir.h.texture = static_cast<uint16_t>(tex.add("impostor:selftest", ir.atlas, std::max(ir.atlas.width, ir.atlas.height)));
    WmdlInfo info;
    std::vector<uint8_t> model = o.build(verts, fmt::kModelFoliage, false, info);
    fmt::ModelHeader mh;
    std::memcpy(&mh, model.data(), sizeof(mh));
    std::printf("selftest: lod0 %zu tris, lod2 %zu tris (%.0f%%), cards kept %zu of %zu, batches %zu+%zu+%zu, "
                "strips=%d vertices=%u\n",
                lod0, lod2, 100.0 * static_cast<double>(lod2) / static_cast<double>(lod0), st.cardsKept, st.cardsTotal,
                info.batches[0], info.batches[1], info.batches[2], info.strips ? 1 : 0, mh.vertexCount);
    std::printf("selftest: impostor %dx%d, %u views, cell %u margin %u, axis (%.2f %.2f) z %.2f..%.2f half %.2f\n",
                ir.atlas.width, ir.atlas.height, ir.h.views, ir.h.cellSize, ir.h.margin, ir.h.axis[0], ir.h.axis[1],
                ir.h.zMin, ir.h.zMax, ir.h.halfWidth);
    bool ok = true;
    if (!lod2 || lod2 * 2 > lod0 || !(mh.flags & fmt::kModelLod2) || mh.lod2BatchCount != info.batches[2]) {
        std::fprintf(stderr, "selftest: LOD 2 missing or not below half of LOD 0\n");
        ok = false;
    }
    for (int v = 0; v < ir.h.views; ++v) {
        std::printf("  view %d coverage lod0 %.0f%% lod2 %.0f%%\n", v, 100.0f * ir.coverage[v],
                    100.0f * irLod2.coverage[v]);
        if (ir.coverage[v] < 0.05f || ir.coverage[v] > 0.95f) ok = false;
        if (std::fabs(irLod2.coverage[v] - ir.coverage[v]) > 0.3f * ir.coverage[v]) ok = false;
    }
    // Cell margins must stay transparent (no bleeding between views).
    const int cell = ir.h.cellSize, aw = ir.atlas.width;
    for (int y = 0; y < ir.atlas.height; ++y)
        for (int x = 0; x < aw; ++x) {
            const int cx = x % cell, cy = y % cell;
            const bool edge = cx < ir.h.margin || cy < ir.h.margin || cx >= cell - ir.h.margin || cy >= cell - ir.h.margin;
            if (edge && ir.atlas.data[(static_cast<size_t>(y) * aw + x) * 4 + 3] != 0) {
                std::fprintf(stderr, "selftest: margin texel (%d,%d) not transparent\n", x, y);
                return false;
            }
        }
    items.push_back({"m_0", kKindModel, std::move(model)});
    items.push_back({"i_0", kKindModel,
                     std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(&ir.h), reinterpret_cast<const uint8_t*>(&ir.h + 1))});
    fmt::DoodadTileHeader th{};
    std::memcpy(th.magic, "WDOD", 4);
    th.version = fmt::kModelFormatVersion;
    items.push_back({"d_0_0", kKindDoodads,
                     std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(&th), reinterpret_cast<const uint8_t*>(&th + 1))});
    const std::string pak = dir + "/selftest.pak";
    if (!writePak(pak, items)) return false;
    if (verifyPak(pak, dir) != 0) ok = false;
    if (!selftest::orientationTest(opt)) ok = false;
    std::printf("selftest %s\n", ok ? "OK" : "FAILED");
    return ok;
}

} // namespace wowee::pspconv
