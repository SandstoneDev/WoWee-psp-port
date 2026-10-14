// the helm / cloak switches of the character window (src/ui/psp/ui_world_paperdoll.cpp,
// the Show Helm / Show Cloak check boxes - InterfaceOptionsDisplayPanelShowHelm / -ShowCloak of the
// original), as upstream does them:
//   - SocialHandler::toggleHelm / toggleCloak: CMSG_SHOWING_HELM / CMSG_SHOWING_CLOAK (ShowingHelmPacket /
//     ShowingCloakPacket, one byte), the state flipped at once;
//   - the Lua ShowHelm(value) / ShowCloak(value) (lua_system_api.cpp): the client has only a toggle, so it is sent
//     only when it lands on what was asked for (the realm - AzerothCore HandleShowingHelmOpcode - toggles
//     PLAYER_FLAGS_HIDE_HELM whatever the byte says);
//   - EntityController's PLAYER_FLAGS read: helm shown = !(flags & 0x400), cloak shown = !(flags & 0x800).
// PSP: upstream's English "Helm is now visible." chat line is not written (the original client says nothing);
// the asked state is kept 3 s at most (upstream keeps its copy until the next PLAYER_FLAGS change). The model's
// helm / cloak themselves are the equipment's (the equipment code reads the same flags).
#include "platform/psp/psp_world_session.hpp"

#include "game/update_field_table.hpp"
#include "game/world_packets.hpp"
#include "platform/psp/psp_log.hpp"

namespace wowee::platform::psp {

namespace {
constexpr uint32_t kHideHelm = 0x00000400u;   // PLAYER_FLAGS_HIDE_HELM
constexpr uint32_t kHideCloak = 0x00000800u;  // PLAYER_FLAGS_HIDE_CLOAK
constexpr uint32_t kAskedHoldMs = 3000u;
}  // namespace

bool WorldSession::isHelmVisible() const {
    const bool field = (selfField(game::fieldIndex(game::UF::PLAYER_FLAGS)) & kHideHelm) == 0;
    if (helmAsked_ >= 0) {
        // What was sent stands until the realm's flags say the same (or the hold runs out).
        if ((helmAsked_ != 0) == field || logUptimeMs() - helmAskedMs_ > kAskedHoldMs) helmAsked_ = -1;
        else return helmAsked_ != 0;
    }
    return field;
}

bool WorldSession::isCloakVisible() const {
    const bool field = (selfField(game::fieldIndex(game::UF::PLAYER_FLAGS)) & kHideCloak) == 0;
    if (cloakAsked_ >= 0) {
        if ((cloakAsked_ != 0) == field || logUptimeMs() - cloakAskedMs_ > kAskedHoldMs) cloakAsked_ = -1;
        else return cloakAsked_ != 0;
    }
    return field;
}

// lua ShowHelm: `if (want != isHelmVisible()) toggleHelm()`.
void WorldSession::showHelm(bool show) {
    if (show == isHelmVisible()) return;
    if (state_ != State::InWorld || !socket_) return;  // upstream toggleHelm: not in world
    socket_->send(game::ShowingHelmPacket::build(show));
    helmAsked_ = show ? 1 : 0;
    helmAskedMs_ = logUptimeMs();
    if (++dressupLogs_ <= 40) PSP_LOGI(tag::Net, "show_helm on=%d", show ? 1 : 0);
}

void WorldSession::showCloak(bool show) {
    if (show == isCloakVisible()) return;
    if (state_ != State::InWorld || !socket_) return;
    socket_->send(game::ShowingCloakPacket::build(show));
    cloakAsked_ = show ? 1 : 0;
    cloakAskedMs_ = logUptimeMs();
    if (++dressupLogs_ <= 40) PSP_LOGI(tag::Net, "show_cloak on=%d", show ? 1 : 0);
}

}  // namespace wowee::platform::psp
