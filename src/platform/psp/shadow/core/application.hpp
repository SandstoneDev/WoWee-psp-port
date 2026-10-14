// PSP build only: SHADOWS upstream include/core/application.hpp.
//
// psp/CMakeLists.txt puts src/platform/psp/shadow ahead of include/, so
// `#include "core/application.hpp"` in upstream code resolves here. The real
// header drags in the SDL3 window, Vulkan, the ImGui UI and the world loader;
// game code only needs a few Application services (today: the expansion
// registry, through include/game/game_utils.hpp).
//
// Keep this to what game code actually calls. When upstream game code starts
// calling another Application method, the PSP build fails to compile - add
// the method here, backed by the PSP platform, rather than un-shadowing.
#pragma once

#include "game/expansion_profile.hpp"

namespace wowee::core {

class Application {
public:
    static Application& getInstance();

    game::ExpansionRegistry* getExpansionRegistry() { return expansionRegistry_; }
    void setExpansionRegistry(game::ExpansionRegistry* registry) { expansionRegistry_ = registry; }

private:
    game::ExpansionRegistry* expansionRegistry_ = nullptr;
};

} // namespace wowee::core
