// Forest LOD on the PSP (docs/PSP_PORT_ARCHITECTURE.md).
//
// PSP: not upstream. The desktop M2Renderer draws every tree with its skin's
// submesh LODs (40 / 80 / 150 yd) on a GPU with vertex budget to spare; on
// the GE a dense forest costs ~10 ms of model time (hardware,
// ~11-16M vertices/s), most of it on far trees. psp_model_conv (forest_bake.hpp)
// bakes for every foliage model a far LOD 2 mesh (kModelLod2) and an impostor
// "i_<n>": `views` pictures of the model in one T8 atlas. Per instance:
//   dist <= LOD 1 distance       LOD 0            (unchanged)
//   ... <= treelod2dist          LOD 1            (unchanged, M2 low skin)
//   ... <= impostordist          LOD 2 mesh       (treelod 2)
//   farther                      impostor quad    (impostors 1/2/3)
// Impostors: one camera-facing quad (cylindrical: it turns about the model's
// up axis) per tree with the baked view nearest to the camera direction in
// the model's frame; grouped by model, so one texture bind and one
// sceGuDrawArray per model, vertices camera-relative floats in the display
// list (no per-instance matrix). Alpha test like the leaf batches.
#include "rendering/psp/psp_models.hpp"
#include "rendering/psp/psp_cpu_fast.hpp"
#include "rendering/psp/psp_impostor_math.hpp"

#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_prof.hpp"

#include <pspgu.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace wowee::rendering::psp {

namespace pp = wowee::platform::psp;
namespace prof = wowee::platform::psp::prof;

namespace {

// u16 UV (32768 = 1.0 in 3D mode, sceGuTexScale 1), 8888 colour, float xyz.
struct ImpVertex {
    uint16_t u, v;
    uint32_t color;
    float x, y, z;
};
static_assert(sizeof(ImpVertex) == 20, "impostor vertex is 20 bytes");
constexpr int kImpVertexType = GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D;

// Display-list budget: 6 vertices (120 bytes) per quad; past this many the
// far trees keep their meshes (the list is 256 KB, psp_display.cpp).
constexpr size_t kMaxImpostors = 1024;

const ScePspFMatrix4 kIdentity = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};

const char* impostorModeName(int m) {
    switch (m) {
        case 1: return "quads";
        case 2: return "sprites";
        case 3: return "tinted";
        default: return "off";
    }
}

} // namespace

void ModelRenderer::loadForestConfig() {
    forest.treeLod = std::clamp(pp::configGetInt("treelod", forest.treeLod), 0, 2);
    forest.lod2Distance =
        static_cast<float>(std::clamp(pp::configGetInt("treelod2dist", static_cast<int>(forest.lod2Distance)), 12, 400));
    forest.impostors = std::clamp(pp::configGetInt("impostors", forest.impostors), 0, 3);
    forest.impostorDistance = static_cast<float>(
        std::clamp(pp::configGetInt("impostordist", static_cast<int>(forest.impostorDistance)), 20, 400));
    PSP_LOGI(pp::tag::Gfx, "forest_cfg treelod=%d lod2dist=%.0f impostors=%d impdist=%.0f", forest.treeLod,
             forest.lod2Distance, forest.impostors, forest.impostorDistance);
    // cpufast 1 new paths, 0 the older paths, 2 alternate every 10 s.
    g_cpuFastMode = std::clamp(pp::configGetInt("cpufast", 1), 0, 2);
    g_cpuFast = g_cpuFastMode != 0;
    g_cpuStats = CpuFastStats{};
    PSP_LOGI(pp::tag::Gfx, "cpu_fast_cfg mode=%d", g_cpuFastMode);
    guardLod = std::clamp(pp::configGetInt("guardlod", guardLod), 0, 1);
    PSP_LOGI(pp::tag::Gfx, "guard_lod_cfg on=%d", guardLod);
}

void ModelRenderer::logForestStats() const {
    PSP_LOGI(pp::tag::Gfx, "m2_forest treelod=%d lod2d=%.0f imp=%s impd=%.0f lod1=%u lod2=%u imp_n=%u imp_draws=%u imp_models=%u",
             forest.treeLod, forest.lod2Distance, impostorModeName(forest.impostors), forest.impostorDistance,
             static_cast<unsigned>(stats_.lowLod), static_cast<unsigned>(stats_.lod2),
             static_cast<unsigned>(stats_.impostors), static_cast<unsigned>(stats_.impostorDraws),
             static_cast<unsigned>(stats_.impostorModels));
    // instances of the last frame that took the split LOD 0 for the guard band (guardlod=1).
    PSP_LOGI(pp::tag::Gfx, "m2_guard on=%d near=%u", guardLod, static_cast<unsigned>(stats_.guardNear));
    // the work of the last ~10 s (per frame), then the mode
    // for the next 10 s ("next=" - cpufast=2 flips it here).
    const CpuFastStats& s = g_cpuStats;
    const float n = s.frames ? static_cast<float>(s.frames) : 1.0f;
    const bool was = g_cpuFast;
    if (g_cpuFastMode == 2) g_cpuFast = !g_cpuFast;
    PSP_LOGI(pp::tag::Gfx, "cpu_fast mode=%d on=%d next=%d frames=%u h=%.1f/%.0f col=%.1f cells=%.0f insts=%.0f memo=%.1f nostall=%.0f",
             g_cpuFastMode, was ? 1 : 0, g_cpuFast ? 1 : 0, static_cast<unsigned>(s.frames), s.heightCalls / n,
             s.heightTests / n, s.colQueries / n, s.colCells / n, s.colInsts / n, s.colMemoHits / n, s.drawsNoStall / n);
    g_cpuStats = CpuFastStats{};
}

// Distance LOD (upstream M2Renderer picks skin submesh levels at 40/80/150 yd;
// the PSP bakes its own, docs/PSP_PORT_ARCHITECTURE.md).
uint8_t ModelRenderer::pickLod(const Inst& in, float dist) const {
    const ModelHeader& h = *in.m->h;
    const bool tree = (h.flags & kModelFoliage) != 0;
    if (tree && forest.treeLod >= 2 && (h.flags & kModelLod2) && h.lod2BatchCount && dist > forest.lod2Distance)
        return 2;
    // Near-split models: the split LOD 0 only when the camera is within
    // 12 yd of the bounding sphere (see kModelNearSplit).
    const bool nearSplit = (h.flags & kModelNearSplit) != 0;
    const float lodDist = nearSplit ? 12.0f : 30.0f + in.r * 3.0f;
    if (!h.batchCount[1] || dist <= lodDist) return 0;
    // treelod 0 (A/B): trees keep their full mesh, not the M2 low skin; the
    // near split stays (a GE clipping fix, not a detail level).
    if (tree && forest.treeLod == 0 && !nearSplit) return 0;
    return 1;
}

bool ModelRenderer::pickImpostor(const Inst& in, float dist) {
    if (forest.impostors <= 0 || dist <= forest.impostorDistance || impostorList_.size() >= kMaxImpostors) return false;
    impostorList_.push_back(&in);
    return true;
}

void ModelRenderer::loadImpostor(pp::Pak& pak, uint16_t id, Model& m) {
    char name[16];
    std::snprintf(name, sizeof(name), "i_%u", static_cast<unsigned>(id));
    const pp::PakEntry* e = pak.find(name);
    if (!e) return;  // older pak, or a model too small for one
    void* blob = pak.load(e);
    if (!blob) return;
    const auto* h = static_cast<const ImpostorHeader*>(blob);
    const char* why = nullptr;
    const int rows = h->cols ? (h->views + h->cols - 1) / h->cols : 0;
    if (e->size < sizeof(ImpostorHeader) || std::memcmp(h->magic, "WIMP", 4) != 0 || h->version != kImpostorVersion)
        why = "header";
    else if ((h->views != 4 && h->views != 8 && h->views != 16) || h->cols != 4 || h->cellSize < 16 ||
             (h->cellSize & (h->cellSize - 1)) || h->margin * 2 >= h->cellSize)
        why = "layout";
    // Finite and ordered (the FPU traps on NaN compares: check first).
    else if (!std::isfinite(h->zMin) || !std::isfinite(h->zMax) || !std::isfinite(h->halfWidth) ||
             !std::isfinite(h->axis[0]) || !std::isfinite(h->axis[1]) || !(h->zMax > h->zMin) || !(h->halfWidth > 0.0f) ||
             !(m.h->radius > 0.0f))
        why = "extents";
    else if (!loadTexture(pak, h->texture, false))  // Never halved (the atlas grid)
        why = "texture";
    else if (textures_[h->texture].t.w != h->cols * h->cellSize || textures_[h->texture].t.h != rows * h->cellSize)
        why = "atlas_size";
    if (why) {
        static int logged = 0;
        if (++logged <= 20) PSP_LOGE(pp::tag::Gfx, "impostor_bad name=%s why=%s", name, why);
        pp::pakFree(blob);
        return;
    }
    m.impBlob = blob;
    m.imp = h;
    memoryBytes_ += e->size;
    ++stats_.impostorModels;
}

void ModelRenderer::drawImpostors(const TerrainCamera& cam) {
    stats_.impostors = static_cast<uint32_t>(impostorList_.size());
    if (impostorList_.empty()) return;
    PROF_SCOPE("m2.impostor");
    PROF_GE_PASS("m2.impostor");
    const bool fast = g_cpuFast;
    if (fast) {
        // the same sort on {model, instance} pairs - the
        // comparator gets the same answers, so std::sort makes the same
        // order (ties too) without loading an Inst at every comparison.
        impKeys_.clear();
        for (const Inst* in : impostorList_) impKeys_.push_back(ImpKey{in->m, in});
        std::sort(impKeys_.begin(), impKeys_.end(), [](const ImpKey& a, const ImpKey& b) { return a.m < b.m; });
        for (size_t k = 0; k < impKeys_.size(); ++k) impostorList_[k] = impKeys_[k].in;
    } else {
        std::sort(impostorList_.begin(), impostorList_.end(), [](const Inst* a, const Inst* b) { return a->m < b->m; });
    }
    const bool sprites = forest.impostors == 2;
    const uint32_t color = forest.impostors == 3 ? 0xFF5050FFu : 0xFFFFFFFFu;  // ABGR: debug tint red
    const int perQuad = sprites ? 2 : 6;

    sceGuSetMatrix(GU_MODEL, &kIdentity);  // vertices are camera-relative already
    sceGuDisable(GU_BLEND);
    sceGuEnable(GU_ALPHA_TEST);
    sceGuAlphaFunc(GU_GREATER, 0x66, 0xFF);  // as the leaf batches (upstream m2.frag 0.4)
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    for (size_t g = 0; g < impostorList_.size();) {
        const Model& m = *impostorList_[g]->m;
        size_t end = g + 1;
        while (end < impostorList_.size() && impostorList_[end]->m == &m) ++end;
        const ImpostorHeader& ih = *m.imp;
        const size_t n = end - g;
        const pp::Texture* atlas = texture(ih.texture);
        if (!atlas) {  // No atlas resident - no untextured (black) quads
            g = end;
            continue;
        }
        pp::textureBind(atlas);
        sceGuTexWrap(GU_CLAMP, GU_CLAMP);
        auto* out = static_cast<ImpVertex*>(sceGuGetMemory(static_cast<int>(n * perQuad * sizeof(ImpVertex))));
        ImpVertex* v = out;
        // a view's cell UVs once per model and frame (four
        // u32 divides each; views <= 16), not once per tree.
        ImpostorCellUv uvCache[16];
        uint32_t uvHave = 0;
        for (size_t k = g; k < end; ++k) {
            const Inst& in = *impostorList_[k];
            const DoodadPlacement& p = *in.p;
            const float* A = p.axes;  // column-major: x axis A[0..2], y A[3..5], z A[6..8]
            const float bx = p.position[0] - cam.pos.x + A[0] * ih.axis[0] + A[3] * ih.axis[1] + A[6] * ih.zMin;
            const float by = p.position[1] - cam.pos.y + A[1] * ih.axis[0] + A[4] * ih.axis[1] + A[7] * ih.zMin;
            const float bz = p.position[2] - cam.pos.z + A[2] * ih.axis[0] + A[5] * ih.axis[1] + A[8] * ih.zMin;
            const float hgt = ih.zMax - ih.zMin;
            const float ux = A[6] * hgt, uy = A[7] * hgt, uz = A[8] * hgt;  // base -> top
            // Towards the camera in the ground plane (the camera is at the origin).
            float tx = -(bx + ux * 0.5f), ty = -(by + uy * 0.5f);
            const float tl = tx * tx + ty * ty;
            const float inv = tl > 1e-6f ? 1.0f / std::sqrt(tl) : 0.0f;
            tx *= inv;
            ty *= inv;
            // Half width in world units: the placement's uniform scale = world
            // sphere radius / model radius.
            const float hw = ih.halfWidth * (in.r / m.h->radius);
            const float rx = -ty * hw, ry = tx * hw;  // picture +x (view i right = (-sin, cos))
            // Nearest view: camera direction in the model's frame.
            const int view = impostorView(tx, ty, A[0], A[1], ih.views);
            ImpostorCellUv uv;
            if (fast && view >= 0 && view < 16) {
                if (!(uvHave & (1u << view))) {
                    uvCache[view] = impostorCellUv(ih, view);
                    uvHave |= 1u << view;
                }
                uv = uvCache[view];
            } else {
                uv = impostorCellUv(ih, view);
            }
            const ImpVertex bl{uv.u0, uv.v1, color, bx - rx, by - ry, bz};
            const ImpVertex br{uv.u1, uv.v1, color, bx + rx, by + ry, bz};
            const ImpVertex tl2{uv.u0, uv.v0, color, bx - rx + ux, by - ry + uy, bz + uz};
            const ImpVertex tr{uv.u1, uv.v0, color, bx + rx + ux, by + ry + uy, bz + uz};
            if (sprites) {
                // GE sprite in 3D: a screen-aligned rectangle between two
                // transformed corners (impostors 2, A/B against quads).
                *v++ = tl2;
                *v++ = br;
            } else {
                *v++ = tl2;
                *v++ = tr;
                *v++ = bl;
                *v++ = bl;
                *v++ = tr;
                *v++ = br;
            }
        }
        const int count = static_cast<int>(n * perQuad);
        sceGuDrawArray(sprites ? GU_SPRITES : GU_TRIANGLES, kImpVertexType, count, nullptr, out);
        ++stats_.impostorDraws;
        ++stats_.batches;
        stats_.triangles += static_cast<uint32_t>(n * 2);
        prof::count(prof::kDraws, 1);
        prof::count(prof::kVertices, static_cast<uint32_t>(count));
        prof::count(prof::kTriangles, static_cast<uint32_t>(n * 2));
        g = end;
    }
    sceGuDisable(GU_ALPHA_TEST);
    PROF_GE_PASS("m2");  // the blended pass that follows is model time again
}

} // namespace wowee::rendering::psp
