// PSP mapping of the level-3 particle GE output (FxGeOut) to the public PSPSDK sceGu calls
// (particle-runtime.md section 2, "Integration boundary"). PSP build only.
// Integration (docs/PSP_PORT_ARCHITECTURE.md): textures through the port's textureBind (it keeps the
// GU_TEXTURE_2D state), the display-list budget of the particle passes (kListReserveFx), the frame profiler.
#pragma once

#include "rendering/psp/psp_particles_l3.hpp"

#include <cstdint>

namespace wowee {
namespace rendering {
namespace psp {

class FxGuOut : public FxGeOut {
public:
    // listReserve: bytes of the display list a draw must leave free (psp_particles.hpp kListReserveFx).
    explicit FxGuOut(uint32_t listReserve) : listReserve_(listReserve) {}
    FxVertex* reserve(int maxVerts) override;
    void submit(const FxDrawState& st, FxVertex* verts, int count) override;
    // Draws dropped for a full display list since the last call (the frame lines' list_full).
    uint32_t takeListFull() {
        const uint32_t n = listFull_;
        listFull_ = 0;
        return n;
    }

private:
    uint32_t listReserve_;
    uint32_t listFull_ = 0;
};

}  // namespace psp
}  // namespace rendering
}  // namespace wowee
