// Backing for the shadowed core::Application (src/platform/psp/shadow).
#include "core/application.hpp"

namespace wowee::core {

Application& Application::getInstance() {
    static Application instance;
    return instance;
}

} // namespace wowee::core
