// lighting - the realm clock for the day/night light.
//
// GameHandler is not built for the PSP; these are its handlers for the game
// time, unchanged in what they read (game_handler_callbacks.cpp
// handleLoginSetTimeSpeed, game_handler_packets.cpp SMSG_GAMETIME_SET /
// SMSG_GAMETIME_UPDATE / SMSG_GAMESPEED_SET): a packed time
// (unpackWowPackedTime) -> hours since midnight, plus the time scale.
// SMSG_SERVERTIME (a unix timestamp) is left alone, as upstream does.
#include "platform/psp/psp_world_session.hpp"

#include "game/packed_time.hpp"
#include "platform/psp/psp_log.hpp"

namespace wowee::platform::psp {

bool WorldSession::handleTimePacket(game::LogicalOpcode op, network::Packet& packet) {
    using game::LogicalOpcode;
    const bool speed = op == LogicalOpcode::SMSG_LOGIN_SETTIMESPEED || op == LogicalOpcode::SMSG_GAMESPEED_SET;
    const bool time = op == LogicalOpcode::SMSG_GAMETIME_SET || op == LogicalOpcode::SMSG_GAMETIME_UPDATE;
    if (!speed && !time) return false;
    // Upstream: settimespeed needs 8 bytes, the others read what is there.
    if (!packet.hasRemaining(speed ? 8u : 4u)) {
        if (++timeLogs_ <= 20)
            PSP_LOGW(tag::Net, "game_time_short op=%d size=%u", static_cast<int>(op),
                     static_cast<unsigned>(packet.getSize()));
        return true;
    }
    const game::WowDate now = game::unpackWowPackedTime(packet.readUInt32());
    gameTime_ = static_cast<float>(now.hour) + static_cast<float>(now.minute) / 60.0f;
    if (speed) timeSpeed_ = packet.readFloat();
    gameTimeMs_ = logUptimeMs();
    if (++timeLogs_ <= 20)
        PSP_LOGI(tag::Net, "game_time op=%s hour=%d min=%d speed=%.5f",
                 op == LogicalOpcode::SMSG_LOGIN_SETTIMESPEED ? "settimespeed"
                 : op == LogicalOpcode::SMSG_GAMESPEED_SET    ? "gamespeed"
                                                              : "gametime",
                 now.hour, now.minute, static_cast<double>(timeSpeed_));
    return true;
}

} // namespace wowee::platform::psp
