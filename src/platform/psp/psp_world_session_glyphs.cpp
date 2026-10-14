// The glyph sockets (the field / DBC rules in psp_glyph_bank_parse.hpp):
//   - GetGlyphSocketInfo: PLAYER_GLYPHS_ENABLED's bit, the socket's GlyphSlot row (type + 1), the glyph's
//     GlyphProperties row (its spell, its icon) - the active talent group's sockets are the player's fields
//     (AzerothCore Player::InitGlyphsForLevel, UpdateFields.h);
//   - UseContainerItem on a glyph (item class 16): USE_GLYPH fires and the item's use waits for a socket
//     (the glyph add-on's GlyphFrame_OnLoad / SpellIsTargeting);
//   - PlaceGlyphInSocket(socket): the waiting use goes out as CMSG_USE_ITEM with the socket as the glyph index
//     (upstream placeGlyphFromBag; AzerothCore HandleUseItemOpcode). The socket / type / unique checks are the
//     server's (AzerothCore Spell::CheckCast APPLY_GLYPH, Spell::EffectApplyGlyph) and come back as a cast failure;
//   - RemoveGlyphFromSocket: CMSG_REMOVE_GLYPH u32 socket (upstream removeGlyphFromSocket);
//   - GLYPH_ADDED / GLYPH_REMOVED / GLYPH_UPDATED: a socket's PLAYER_FIELD_GLYPHS_n changed from what the session
//     last saw (own rule: the session starts from empty sockets in each world; the glyph UI acts on the events only
//     while it is shown).
// PSP: one talent group shown (the PSP has no dual-spec tabs); the GlyphProperties / GlyphSlot rows and the APPLY_GLYPH
// table come from text.pak "glyph_data" (the world screen loads it, setGlyphData).
#include "platform/psp/psp_world_session.hpp"

#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_strings.hpp"
#include "platform/psp/psp_spell_db.hpp"

#include <cstdio>

namespace wowee::platform::psp {

gbk::GlyphSocket WorldSession::glyphSocket(int socket) const {
    if (socket < 0 || socket >= gbk::kGlyphSockets) return gbk::GlyphSocket{};
    const uint32_t slotId = selfField(static_cast<uint16_t>(gbk::kFieldGlyphSlots + socket));
    const uint32_t glyph = selfField(static_cast<uint16_t>(gbk::kFieldGlyphs + socket));
    const uint32_t mask = selfField(gbk::kFieldGlyphsEnabled);
    if (!glyphData_) {  // no text.pak record: the fields alone (type unknown -> major)
        gbk::GlyphSocket s;
        s.enabled = (mask >> socket) & 1u;
        s.slotId = slotId;
        s.glyph = glyph;
        return s;
    }
    return gbk::glyphSocket(*glyphData_, socket, slotId, glyph, mask);
}

bool WorldSession::glyphMatchesSocket(int socket) const {
    if (!glyphUse_.active || !glyphData_ || socket < 0 || socket >= gbk::kGlyphSockets) return false;
    const uint32_t prop = glyphData_->glyphOfSpell(glyphUse_.spellId);
    return gbk::glyphMatchesSocket(*glyphData_, prop, selfField(static_cast<uint16_t>(gbk::kFieldGlyphSlots + socket)));
}

// useItemAt's glyph branch (after its checks): USE_GLYPH, the use waits for a socket.
WorldSession::ItemUse WorldSession::glyphUseItem(const PendingItemUse& use) {
    glyphUse_ = use;
    glyphUse_.active = true;
    ++useGlyphSeq_;
    PSP_LOGI(tag::Net, "glyph_use entry=%u spell=%u prop=%u", static_cast<unsigned>(use.entry),
             static_cast<unsigned>(use.spellId),
             static_cast<unsigned>(glyphData_ ? glyphData_->glyphOfSpell(use.spellId) : 0u));
    return ItemUse::Glyph;
}

void WorldSession::cancelGlyphTargeting() {
    if (glyphUse_.active) PSP_LOGI(tag::Net, "glyph_use_cancel entry=%u", static_cast<unsigned>(glyphUse_.entry));
    glyphUse_ = PendingItemUse{};
}

// PlaceGlyphInSocket: the waiting use goes out with the socket as the glyph index; the targeting ends.
bool WorldSession::placeGlyph(int socket) {
    if (!glyphUse_.active || socket < 0 || socket >= gbk::kGlyphSockets || !socket_ || state_ != State::InWorld)
        return false;
    const uint16_t wire = game::wireOpcode(game::LogicalOpcode::CMSG_USE_ITEM);
    if (wire == 0xFFFF) return false;
    // The item must still be where it was picked (own rule: a moved item ends the targeting).
    game::ItemDef def;
    uint64_t guid = 0;
    if (!itemAtWire(glyphUse_.bag, glyphUse_.slot, def, guid) || guid != glyphUse_.itemGuid) {
        cancelGlyphTargeting();
        return false;
    }
    network::Packet p(wire);
    constexpr uint8_t kCastCount = 0;  // upstream UseItemPacket
    writeUseItemBody(p, glyphUse_.bag, glyphUse_.slot, kCastCount, glyphUse_.spellId, glyphUse_.itemGuid,
                     static_cast<uint32_t>(socket), 0, SpellCastTargets{});
    socket_->send(p);
    lastItemCast_ = glyphUse_;
    lastItemCastMs_ = logUptimeMs();
    PSP_LOGI(tag::Net, "glyph_place socket=%d entry=%u spell=%u count=%u", socket, static_cast<unsigned>(glyphUse_.entry),
             static_cast<unsigned>(glyphUse_.spellId), static_cast<unsigned>(kCastCount));
    glyphUse_ = PendingItemUse{};
    return true;
}

void WorldSession::removeGlyph(int socket) {
    if (socket < 0 || socket >= gbk::kGlyphSockets || !socket_ || state_ != State::InWorld) return;
    const uint16_t wire = game::wireOpcode(game::LogicalOpcode::CMSG_REMOVE_GLYPH);
    if (wire == 0xFFFF) return;
    network::Packet p(wire);
    gbk::writeRemoveGlyph(p, static_cast<uint32_t>(socket));
    socket_->send(p);
    PSP_LOGI(tag::Net, "glyph_remove socket=%d glyph=%u", socket,
             static_cast<unsigned>(selfField(static_cast<uint16_t>(gbk::kFieldGlyphs + socket))));
}

bool WorldSession::takeGlyphEvent(GlyphEvent& out) {
    if (glyphEvents_.empty()) return false;
    out = glyphEvents_.front();
    glyphEvents_.pop_front();
    return true;
}

// The fields' changes -> GLYPH_ADDED / _REMOVED / _UPDATED (own rule: against what the session last saw, empty
// sockets at the start of each world).
void WorldSession::glyphUpdate() {
    if (!selfKnown()) {
        for (uint32_t& g : glyphSeen_) g = 0;
        return;
    }
    for (int i = 0; i < gbk::kGlyphSockets; ++i) {
        const uint32_t g = selfField(static_cast<uint16_t>(gbk::kFieldGlyphs + i));
        if (g != glyphSeen_[i]) {
            GlyphEvent e;
            e.kind = glyphSeen_[i] == 0 ? 1 : g == 0 ? 2 : 3;
            e.socket = static_cast<uint8_t>(i);
            if (glyphEvents_.size() < 8) glyphEvents_.push_back(e);
            PSP_LOGI(tag::Net, "glyph_event kind=%u socket=%d glyph=%u was=%u", static_cast<unsigned>(e.kind), i,
                     static_cast<unsigned>(g), static_cast<unsigned>(glyphSeen_[i]));
        }
        glyphSeen_[i] = g;
    }
    // The waiting use ends when its item is gone (used elsewhere, sold).
    if (glyphUse_.active) {
        game::ItemDef def;
        uint64_t guid = 0;
        if (!itemAtWire(glyphUse_.bag, glyphUse_.slot, def, guid) || guid != glyphUse_.itemGuid) cancelGlyphTargeting();
    }
}

}  // namespace wowee::platform::psp
