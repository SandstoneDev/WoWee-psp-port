// PSP mapping of FxGeOut to sceGu (public PSPSDK API). Compiled for the PSP only.
// Integration (docs/PSP_PORT_ARCHITECTURE.md): the port's textureBind, display-list budget and profiler;
// sceGumDrawArray so the identity model matrix the particle pass loads is in effect.
#include "rendering/psp/psp_particles_l3_gu.hpp"

#ifdef __PSP__
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_texture.hpp"

#include <pspgu.h>
#include <pspgum.h>

namespace wowee {
namespace rendering {
namespace psp {

namespace {
int guFactor(FxFactor f) {
    switch (f) {
        case FxFactor::SrcAlpha: return GU_SRC_ALPHA;
        case FxFactor::OneMinusSrcAlpha: return GU_ONE_MINUS_SRC_ALPHA;
        case FxFactor::DstColor:  // as a source factor: the destination colour
        case FxFactor::SrcColor:  // as a destination factor: the source colour
            return GU_OTHER_COLOR;
        case FxFactor::Fixed:
        default: return GU_FIX;
    }
}
int guPrim(FxPrim p) {
    switch (p) {
        case FxPrim::Triangles: return GU_TRIANGLES;
        case FxPrim::TriangleStrip: return GU_TRIANGLE_STRIP;
        case FxPrim::Sprites:
        default: return GU_SPRITES;
    }
}
}  // namespace

FxVertex* FxGuOut::reserve(int maxVerts) {
    if (maxVerts <= 0) return nullptr;
    const uint32_t bytes = static_cast<uint32_t>(maxVerts) * static_cast<uint32_t>(sizeof(FxVertex));
    if (platform::psp::displayListFree() < listReserve_ + bytes + 256u) {  // Integration: the list's budget
        ++listFull_;
        return nullptr;
    }
    return static_cast<FxVertex*>(sceGuGetMemory(static_cast<int>(bytes)));
}

void FxGuOut::submit(const FxDrawState& st, FxVertex* verts, int count) {
    if (!verts || count <= 0) return;
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, guFactor(st.src), guFactor(st.dst), st.srcFix, st.dstFix);
    if (st.alphaTest) {
        sceGuEnable(GU_ALPHA_TEST);
        sceGuAlphaFunc(GU_GREATER, st.alphaRef, 0xFF);
    } else {
        sceGuDisable(GU_ALPHA_TEST);
    }
    // Integration: the port's binding (nullptr disables texturing; it tracks GU_TEXTURE_2D itself).
    platform::psp::textureBind(static_cast<const platform::psp::Texture*>(st.texture));
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    namespace prof = platform::psp::prof;
    prof::count(prof::kDraws, 1);
    prof::count(prof::kVertices, static_cast<uint32_t>(count));
    if (st.prim == FxPrim::Triangles) prof::count(prof::kTriangles, static_cast<uint32_t>(count / 3));
    sceGumDrawArray(guPrim(st.prim), GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, count,
                    nullptr, verts);
}

}  // namespace psp
}  // namespace rendering
}  // namespace wowee
#endif  // __PSP__
