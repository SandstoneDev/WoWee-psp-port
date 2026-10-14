// the server's movies and cinematics.
//
// Upstream (game_handler_packets.cpp) plays neither: SMSG_TRIGGER_MOVIE is answered at once with
// CMSG_COMPLETE_MOVIE, SMSG_TRIGGER_CINEMATIC with CMSG_NEXT_CINEMATIC_CAMERA. The original client:
//  - SMSG_TRIGGER_MOVIE (uint32 Movie.dbc id) fires PLAY_MOVIE; FrameXML MovieFrame plays it and calls
//    GameMovieFinished() when it ends, is skipped or cannot start -> CMSG_COMPLETE_MOVIE (no body). The PSP world
//    screen does the same (ui_movie.cpp) - so the reply now comes after the movie, not before it.
//  - SMSG_TRIGGER_CINEMATIC (uint32 CinematicSequences.dbc id) starts the flyby (CINEMATIC_START): the client
//    says the camera began (CMSG_NEXT_CINEMATIC_CAMERA - the server then moves the player's visibility along the
//    camera path, AzerothCore CinematicMgr) and, at its end or on StopCinematic (ESCAPE), CMSG_COMPLETE_CINEMATIC
//    (the server puts the visibility back). The PSP has no flyby yet: the world
//    screen sends both at once - the original when the player skips the flyby straight away. (Upstream never
//    completes it: AzerothCore then keeps the visibility on the camera path for the flyby's length + 10 s, the
//    NPCs around a new character vanish for a minute or two.)
#include "platform/psp/psp_world_session.hpp"

#include "platform/psp/psp_log.hpp"

namespace wowee::platform::psp {

bool WorldSession::handleMoviePacket(game::LogicalOpcode op, network::Packet& packet) {
    using game::LogicalOpcode;
    if (op != LogicalOpcode::SMSG_TRIGGER_MOVIE && op != LogicalOpcode::SMSG_TRIGGER_CINEMATIC) return false;
    // Kept in any state: a new character's flyby comes right after the login (the world screen takes it once the
    // loading screen is gone); the replies go out only in the world.
    if (!packet.hasRemaining(4)) return true;
    const uint32_t id = packet.readUInt32();
    if (op == LogicalOpcode::SMSG_TRIGGER_MOVIE) {
        pendingMovie_ = id;
        PSP_LOGI(tag::Net, "trigger_movie id=%u", static_cast<unsigned>(id));
    } else {
        pendingCinematic_ = id;
        PSP_LOGI(tag::Net, "trigger_cinematic id=%u", static_cast<unsigned>(id));
    }
    return true;
}

bool WorldSession::takeMovie(uint32_t& movieId) {
    if (!pendingMovie_) return false;
    movieId = pendingMovie_;
    pendingMovie_ = 0;
    return true;
}

bool WorldSession::takeCinematic(uint32_t& sequenceId) {
    if (!pendingCinematic_) return false;
    sequenceId = pendingCinematic_;
    pendingCinematic_ = 0;
    return true;
}

namespace {
// An empty client packet (upstream builds them the same way: Packet(wireOpcode(op)), skipped when unmapped).
bool sendEmpty(network::WorldSocket* socket, game::LogicalOpcode op) {
    const uint16_t wire = game::wireOpcode(op);
    if (!socket || wire == 0xFFFF) return false;
    socket->send(network::Packet(wire));
    return true;
}
}  // namespace

void WorldSession::movieFinished() {
    if (state_ != State::InWorld) return;
    const bool sent = sendEmpty(socket_.get(), game::LogicalOpcode::CMSG_COMPLETE_MOVIE);
    PSP_LOGI(tag::Net, "complete_movie sent=%d", sent ? 1 : 0);
}

void WorldSession::cinematicCameraStarted() {
    if (state_ != State::InWorld) return;
    const bool sent = sendEmpty(socket_.get(), game::LogicalOpcode::CMSG_NEXT_CINEMATIC_CAMERA);
    PSP_LOGI(tag::Net, "next_cinematic_camera sent=%d", sent ? 1 : 0);
}

void WorldSession::cinematicFinished() {
    if (state_ != State::InWorld) return;
    const bool sent = sendEmpty(socket_.get(), game::LogicalOpcode::CMSG_COMPLETE_CINEMATIC);
    PSP_LOGI(tag::Net, "complete_cinematic sent=%d", sent ? 1 : 0);
}

} // namespace wowee::platform::psp
