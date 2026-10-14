// The session half of the class bars and the vehicles. Upstream WoWee has the totems, the plain forms and the
// possession by aura; the rest follows AzerothCore's handlers or the port's own rules (marked "Own"):
//   - the totems (upstream SpellHandler::handleTotemCreated, lua_GetTotemInfo / DestroyTotem): SMSG_TOTEM_CREATED
//     fills the slot, GetTotemTimeLeft = start + duration - now, DestroyTotem sends CMSG_TOTEM_DESTROYED and the slot
//     empties when the server removes the totem (its object goes); own rule: a slot shows on the multicast bar when
//     the player knows a totem spell for it;
//   - the shapeshift bar: the spells baked by the learn rule (text.pak "class_bars": the attribute names of
//     AzerothCore's SharedDefines.h), in StanceBarOrder (own rule for spells without one); a form is active when
//     UNIT_FIELD_BYTES_2's form is the spell's (upstream GetShapeshiftFormInfo), a bar spell that is no form while we
//     have its aura (own rule); CastShapeshiftForm casts it;
//   - the multicast bar: per totem slot the known SPELL_ATTR7_CAN_BE_MULTI_CAST spells whose TotemCategory fills it
//     (AzerothCore sends only the active rank of a spell), sorted by name (own); SetMultiCastSpell takes a spell of the
//     slot's list (own) and places the action (CMSG_SET_ACTION_BUTTON);
//   - the bonus bar: GetBonusBarOffset = 5 while the override bar has a source (own rule, psp_classbar_parse.hpp),
//     else the form's BonusActionBar (upstream);
//   - the possession: our first aura of a possession aura type while we charm a unit (upstream possessAuraSpellId);
//     own rule: else, while we move the unit we charm, the spell that created it (UNIT_CREATED_BY_SPELL);
//   - the vehicles: SMSG_PLAYER_VEHICLE_DATA / UPDATEFLAG_VEHICLE give a unit its vehicle kit; our own
//     SMSG_MONSTER_MOVE_TRANSPORT onto a unit (AzerothCore Vehicle::AddPassenger) or our create block on one seats us,
//     a later one changes the seat, our SMSG_MONSTER_MOVE with TransportExit (AzerothCore Unit::_ExitVehicle) takes us
//     off; the seat flags gate VehicleExit / the seat switches; VehicleExit sends CMSG_REQUEST_VEHICLE_EXIT (upstream,
//     for a driver too), a seat switch CMSG_REQUEST_VEHICLE_PREV / NEXT / SWITCH_SEAT (AzerothCore takes them from any
//     passenger);
//   - the mover (SMSG_CLIENT_CONTROL_UPDATE): allow 1 for another unit = we move it (our movement packets carry its
//     guid, its speed changes are ACKed with it: AzerothCore SetClientControl -> SetMover), allow 0 for us = our
//     controls are gone (upstream handleClientControlUpdate).
// PSP differences: the vehicles' aim is the pitch only (no power); a passenger sends no movement (AzerothCore roots
// it); the seat indicator's occupants are the passengers we saw.
#include "platform/psp/psp_world_session.hpp"

#include "core/coordinates.hpp"
#include "game/spline_packet.hpp"
#include "game/update_field_table.hpp"
#include "platform/psp/psp_classbar_parse.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_strings.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_spell_db.hpp"
#include "platform/psp/psp_transport_parse.hpp"

#include <pspkernel.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wowee::platform::psp {

namespace {

// 3.3.5a UpdateFields.h (OBJECT_END 6, UNIT_END 148) - not all in upstream's field table.
constexpr uint16_t kFieldEntry = 3;           // OBJECT_FIELD_ENTRY
constexpr uint16_t kFieldCharm = 6;           // UNIT_FIELD_CHARM (guid)
constexpr uint16_t kFieldCreatedBySpell = 81; // UNIT_CREATED_BY_SPELL = OBJECT_END + 0x4B
constexpr uint16_t kFieldBytes2 = 122;        // UNIT_FIELD_BYTES_2 (byte 3: the shapeshift form)
constexpr uint16_t kFieldPlayerBytes2 = 1229; // PLAYER_FIELD_BYTES2 = UNIT_END + 0x439 (low u16: override spells id)

// The spline flags (AzerothCore MoveSplineFlag).
constexpr uint32_t kSplineTransportExit = 0x01000000;

// A unit's guid (a creature 0xF130, a vehicle 0xF150, a pet 0xF140, a player 0x0000) - not a game object's (a WMO
// transport 0xF120 / 0x1FC0, a game object 0xF110): only units carry vehicle passengers.
bool isUnitGuid(uint64_t g) {
    const uint32_t high = static_cast<uint32_t>(g >> 48);
    return g != 0 && (high == 0x0000 || high == 0xF130 || high == 0xF150 || high == 0xF140);
}

uint32_t nowMs() { return static_cast<uint32_t>(sceKernelGetSystemTimeWide() / 1000u); }

}  // namespace

// --- Data --------------------------------------------------------------------------------------------------------------

const cbar::ClassBarData& WorldSession::classBarData() {
    if (cbarDataTried_) return cbarData_;
    cbarDataTried_ = true;
    char path[256];
    fsPath(path, sizeof(path), "data/text.pak");
    Pak pak;
    if (pak.open(path)) {
        if (const PakEntry* e = pak.find("class_bars")) {
            if (void* blob = pak.load(e)) {
                if (!cbarData_.parse(static_cast<const uint8_t*>(blob), e->size))
                    PSP_LOGW(tag::Net, "class_bars bad size=%u", static_cast<unsigned>(e->size));
                pakFree(blob);
            }
        }
        pak.close();
    }
    PSP_LOGI(tag::Net, "class_bars loaded=%d stance=%u totem=%u possess=%u forms=%u overrides=%u cats=%u",
             cbarData_.loaded ? 1 : 0, static_cast<unsigned>(cbarData_.stance.size()),
             static_cast<unsigned>(cbarData_.totem.size()), static_cast<unsigned>(cbarData_.possess.size()),
             static_cast<unsigned>(cbarData_.forms.size()), static_cast<unsigned>(cbarData_.overrides.size()),
             static_cast<unsigned>(cbarData_.totemCats.size()));
    return cbarData_;
}

// The vehicle's rows of "class_vehicles" (read whole for the lookup, ~73 KB for a moment, then only one VehicleKit).
void WorldSession::cbarLoadKit(uint32_t vehicleId) {
    if (vehicleId == cbarKitId_ && (cbarKit_.valid || vehicleId == 0)) return;
    cbarKitId_ = vehicleId;
    cbarKit_ = cbar::VehicleKit{};
    if (vehicleId == 0) return;
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    char path[256];
    fsPath(path, sizeof(path), "data/text.pak");
    Pak pak;
    bool found = false;
    uint32_t bytes = 0;
    if (pak.open(path)) {
        if (const PakEntry* e = pak.find("class_vehicles")) {
            bytes = e->size;
            if (void* blob = pak.load(e)) {
                cbar::VehicleTables t;
                if (t.parse(static_cast<const uint8_t*>(blob), e->size)) found = cbarKit_.load(t, vehicleId);
                pakFree(blob);
            }
        }
        pak.close();
    }
    ++cbarVehicleVersion_;
    PSP_LOGI(tag::Net, "vehicle_kit id=%u found=%d seats=%d ind=%u flags=%08X kb=%u ms=%u", static_cast<unsigned>(vehicleId),
             found ? 1 : 0, cbarKit_.seatCount(), static_cast<unsigned>(cbarKit_.vehicle.uiIndicator),
             static_cast<unsigned>(cbarKit_.vehicle.flags), static_cast<unsigned>((bytes + 1023u) / 1024u),
             static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u));
}

// --- Totems -------------------------------------------------------------------------------------------------------------

const WorldSession::TotemSlotState& WorldSession::totemSlot(int slot) const {
    static const TotemSlotState kNone{};
    return slot >= 0 && slot < kTotemSlots ? cbarTotems_[slot] : kNone;
}

uint32_t WorldSession::totemTimeLeftMs(int slot) const {
    const TotemSlotState& t = totemSlot(slot);
    if (!t.active()) return 0;
    const uint32_t end = t.startMs + t.durationMs;
    const uint32_t now = nowMs();
    return (t.durationMs && static_cast<int32_t>(end - now) > 0) ? end - now : 0u;
}

// DestroyTotem (upstream GameHandler::destroyTotem): CMSG_TOTEM_DESTROYED for a slot with a totem; the slot empties
// when the server removes the totem (AzerothCore Totem::UnSummon -> the object goes: classBarsForget).
void WorldSession::destroyTotem(int slot) {
    if (slot < 0 || slot >= kTotemSlots || !cbarTotems_[slot].active()) return;
    if (state_ != State::InWorld || !socket_) return;
    network::Packet p(game::wireOpcode(game::LogicalOpcode::CMSG_TOTEM_DESTROYED));
    cbar::writeTotemDestroyed(p, static_cast<uint8_t>(slot));
    socket_->send(p);
    PSP_LOGI(tag::Net, "totem_destroy slot=%d", slot);
}

// --- The shapeshift and multicast lists ------------------------------------------------------------------------------

void WorldSession::cbarRebuildLists() {
    cbarSpellsVersion_ = knownSpellsVersion();
    const cbar::ClassBarData& d = classBarData();
    cbarForms_.clear();
    cbarFormIds_.clear();
    for (auto& l : cbarTotemLists_) l.clear();
    struct Form {
        uint32_t id;
        int32_t order;
    };
    std::vector<Form> forms;
    // The known list holds one rank of a spell (AzerothCore SendInitialSpells sends the active ranks,
    // SMSG_SUPERCEDED_SPELL swaps them): every spell is listed as it is.
    for (uint32_t id : knownSpells_) {
        int32_t order = 0;
        if (d.stanceOrder(id, order)) forms.push_back({id, order});
        const uint32_t slots = d.totemSlots(id);
        for (int k = 0; k < kTotemSlots; ++k)
            if (slots & (1u << k)) cbarTotemLists_[k].push_back(id);
    }
    std::sort(forms.begin(), forms.end(),
              [](const Form& a, const Form& b) { return cbar::stanceBefore(a.id, a.order, b.id, b.order); });
    for (const Form& f : forms) {
        cbarForms_.push_back(f.id);
        // The form the spell takes: its SPELL_AURA_MOD_SHAPESHIFT effect's misc value (0 = no form).
        uint8_t form = 0;
        if (const SpellTip* tip = spellDb_ ? spellDb_->tip(f.id) : nullptr)
            for (const SpellTipEffect& e : tip->eff)
                if (e.aura == 36 && e.miscValue > 0 && e.miscValue < 256) form = static_cast<uint8_t>(e.miscValue);
        cbarFormIds_.push_back(form);
    }
    // Own rule: a slot's totems by name (then by spell id).
    for (auto& l : cbarTotemLists_)
        std::sort(l.begin(), l.end(), [this](uint32_t a, uint32_t b) {
            static const std::string kNone;
            const SpellInfo* x = spellInfo(a);
            const SpellInfo* y = spellInfo(b);
            const std::string& nx = x ? x->name : kNone;
            const std::string& ny = y ? y->name : kNone;
            return nx != ny ? nx < ny : a < b;
        });
    if (++cbarLogs_ <= 20)
        PSP_LOGI(tag::Net, "class_bars_lists forms=%u totems=%u/%u/%u/%u", static_cast<unsigned>(cbarForms_.size()),
                 static_cast<unsigned>(cbarTotemLists_[0].size()), static_cast<unsigned>(cbarTotemLists_[1].size()),
                 static_cast<unsigned>(cbarTotemLists_[2].size()), static_cast<unsigned>(cbarTotemLists_[3].size()));
}

const std::vector<uint32_t>& WorldSession::shapeshiftForms() {
    if (cbarSpellsVersion_ != knownSpellsVersion()) cbarRebuildLists();
    return cbarForms_;
}

const std::vector<uint32_t>& WorldSession::multiCastSpells(int slot) {
    static const std::vector<uint32_t> kNone;
    if (slot < 0 || slot >= kTotemSlots) return kNone;
    if (cbarSpellsVersion_ != knownSpellsVersion()) cbarRebuildLists();
    return cbarTotemLists_[slot];
}

uint8_t WorldSession::shapeshiftFormId() const { return static_cast<uint8_t>((selfField(kFieldBytes2) >> 24) & 0xFFu); }

// GetShapeshiftFormInfo's isActive. A form spell: UNIT_FIELD_BYTES_2's form is its form (upstream lua_unit_api.cpp
// GetShapeshiftFormInfo). Own rule for a bar spell that takes no form (a paladin aura): while we have its aura.
bool WorldSession::shapeshiftFormActive(uint32_t spellId) const {
    for (size_t i = 0; i < cbarForms_.size() && i < cbarFormIds_.size(); ++i)
        if (cbarForms_[i] == spellId && cbarFormIds_[i] != 0) return cbarFormIds_[i] == shapeshiftFormId();
    for (const auto& a : playerAuras_)
        if (!a.isEmpty() && a.spellId == spellId) return true;
    return false;
}

void WorldSession::castShapeshiftForm(int index) {
    const std::vector<uint32_t>& forms = shapeshiftForms();
    if (index < 0 || index >= static_cast<int>(forms.size())) return;
    castSpell(forms[static_cast<size_t>(index)], playerGuid_, false);  // a form is cast on ourselves
    PSP_LOGI(tag::Net, "shapeshift_cast index=%d spell=%u", index, static_cast<unsigned>(forms[static_cast<size_t>(index)]));
}

bool WorldSession::hasMultiCastBar() {
    for (int k = 0; k < kTotemSlots; ++k)
        if (!multiCastSpells(k).empty()) return true;
    return false;
}

bool WorldSession::multiCastCallKnown(int page) const {
    return page >= 0 && page < 3 && knownSpells_.count(cbar::kCallSpells[page]) != 0;
}

// SetMultiCastSpell(action, spell). Own rule: 0 clears the action; else the spell must be one of the slot's totems
// (GetMultiCastTotemSpells of the action's slot) - else refused with the own line SESSION_TOTEM_SLOT; the action is
// placed with CMSG_SET_ACTION_BUTTON (AzerothCore HandleSetActionButtonOpcode).
bool WorldSession::setMultiCastSpell(int action, uint32_t spellId) {
    const int slot = cbar::multiCastActionSlot(action);
    if (slot < 0) return false;
    if (spellId == 0) {
        setActionBarSlot(action, game::ActionBarSlot::SPELL, 0);
        return true;
    }
    const std::vector<uint32_t>& list = multiCastSpells(slot);
    if (std::find(list.begin(), list.end(), spellId) == list.end()) {
        addUiError(OWN_STR(SESSION_TOTEM_SLOT));
        return false;
    }
    setActionBarSlot(action, game::ActionBarSlot::SPELL, spellId);
    return true;
}

// --- The bonus bar and the possession -----------------------------------------------------------------------------------

int WorldSession::bonusBarOffset() const {
    const int formBonus = spellDb_ ? spellDb_->bonusBar(shapeshiftFormId()) : 0;
    return cbar::bonusBarOffset(cbarOverrideBits_, formBonus);
}

bool WorldSession::possessBarVisible() const { return cbarPossessSpell_ != 0; }

bool WorldSession::possessCanCancel() const {
    if (!cbarPossessSpell_) return false;
    if (const SpellInfo* si = spellInfo(cbarPossessSpell_)) return (si->rec.attributes & cbar::kAttr0NoAuraCancel) == 0;
    return true;
}

// PossessButton_OnClick(2): VehicleExit when we drive a vehicle we may leave, else CancelUnitBuff("player", name) -
// CMSG_CANCEL_AURA of the possession spell (AzerothCore HandleCancelAuraOpcode stops a channel
// of it too: Mind Control, Eye of Kilrogg).
void WorldSession::cancelPossess() {
    if (controllingVehicle() && canExitVehicle()) {
        vehicleExit();
        return;
    }
    if (!cbarPossessSpell_ || !possessCanCancel()) return;
    cancelAura(cbarPossessSpell_);
    PSP_LOGI(tag::Net, "possess_cancel spell=%u", static_cast<unsigned>(cbarPossessSpell_));
}

void WorldSession::cbarUpdateOverride() {
    const uint64_t charm = static_cast<uint64_t>(selfField(kFieldCharm)) | (static_cast<uint64_t>(selfField(kFieldCharm + 1)) << 32);
    const cbar::ClassBarData& d = classBarData();
    // The possession (upstream possessAuraSpellId): while we charm a unit, our first aura of a possession aura type;
    // own rule: else, while we move the unit we charm, the spell that created it.
    uint32_t possess = 0;
    if (charm != 0) {
        for (const auto& a : playerAuras_) {
            if (a.isEmpty()) continue;
            const SpellInfo* si = spellInfo(a.spellId);
            if (si && (cbar::isPossessAura(si->rec.aura[0]) || cbar::isPossessAura(si->rec.aura[1]) ||
                       cbar::isPossessAura(si->rec.aura[2]))) {
                possess = a.spellId;
                break;
            }
        }
        if (!possess && cbarMover_ == charm)
            if (auto e = entities_.getEntity(charm)) possess = e->getField(kFieldCreatedBySpell);
    }
    if (possess != cbarPossessSpell_) {
        PSP_LOGI(tag::Net, "possess_spell %u -> %u charm=%08X mover=%08X", static_cast<unsigned>(cbarPossessSpell_),
                 static_cast<unsigned>(possess), static_cast<unsigned>(charm & 0xFFFFFFFFu),
                 static_cast<unsigned>(cbarMover_ & 0xFFFFFFFFu));
        cbarPossessSpell_ = possess;
    }
    // The override bar (own rule, psp_classbar_parse.hpp): the first source present fills it.
    uint32_t bits = 0;
    uint32_t bar[12] = {};
    const uint64_t pet = petGuid();
    const uint32_t ov = selfField(kFieldPlayerBytes2) & 0xFFFFu;
    const uint32_t* spells = ov ? d.overrideSpells(ov) : nullptr;
    const uint32_t* presets = d.formPresets(shapeshiftFormId());
    if (cbarMover_ != 0 && cbarMover_ != playerGuid_ && pet == cbarMover_) {
        bits = cbar::kOverridePet;  // the unit we move: its actions (SMSG_PET_SPELLS)
        for (int i = 0; i < kPetSlots; ++i) bar[i] = petActionSlot(i);
    } else if (spells) {
        bits = cbar::kOverrideSpells;  // PLAYER_FIELD_BYTES2's OverrideSpellData row
        for (int i = 0; i < 10; ++i) bar[i] = spells[i];
    } else if (presets) {
        bits = cbar::kOverrideFormPreset;  // the form's preset spells
        for (int i = 0; i < 8; ++i) bar[i] = presets[i];
    }
    if (bits != cbarOverrideBits_ || std::memcmp(bar, cbarOverride_, sizeof(bar)) != 0) {
        if (bits != cbarOverrideBits_)
            PSP_LOGI(tag::Net, "bonus_override bits=%u -> %u offset=%d", static_cast<unsigned>(cbarOverrideBits_),
                     static_cast<unsigned>(bits), cbar::bonusBarOffset(bits, 0));
        cbarOverrideBits_ = bits;
        std::memcpy(cbarOverride_, bar, sizeof(bar));
    }
}

// --- Vehicles ----------------------------------------------------------------------------------------------------------

uint32_t WorldSession::unitVehicleId(uint64_t guid) const {
    auto it = cbarUnitVehicle_.find(guid);
    return it == cbarUnitVehicle_.end() ? 0u : it->second;
}

const cbar::VehicleSeatRec* WorldSession::vehicleSeat() const {
    return cbarKit_.valid && cbarVehicleSeat_ >= 0 ? cbarKit_.seat(cbarVehicleSeat_) : nullptr;
}

bool WorldSession::vehicleUi() const {
    const cbar::VehicleSeatRec* s = vehicleSeat();
    return s && (s->flags & cbar::kSeatHasVehicleUi);
}

// the seat of the vehicle we are in (as its passenger or as the unit we move) has the flag.
bool WorldSession::canExitVehicle() const {
    const cbar::VehicleSeatRec* s = vehicleSeat();
    return s && (s->flags & cbar::kSeatCanEnterOrExit);
}

bool WorldSession::canSwitchVehicleSeats() const {
    const cbar::VehicleSeatRec* s = vehicleSeat();
    return s && (s->flags & cbar::kSeatCanSwitch);
}

bool WorldSession::controllingVehicle() const {
    return cbarVehicleGuid_ != 0 && cbarMover_ == cbarVehicleGuid_;
}

bool WorldSession::vehicleAimAdjustable() const {
    return cbarKit_.valid && (cbarKit_.vehicle.flags & cbar::kVehicleAimAngle) && controllingVehicle();
}

void WorldSession::vehicleAim(float deltaRad) {
    if (!vehicleAimAdjustable()) return;
    const float lo = cbarKit_.vehicle.pitchMin, hi = std::max(cbarKit_.vehicle.pitchMin, cbarKit_.vehicle.pitchMax);
    const float a = std::clamp(cbarAim_ + deltaRad, lo, hi);
    if (std::fabs(a - cbarAim_) < 1e-4f) return;
    cbarAim_ = a;
    cbarAimPending_ = true;  // MSG_MOVE_SET_PITCH at most every 150 ms (classBarsUpdate sends the last one)
    cbarSendAim();
}

void WorldSession::cbarSendAim() {
    if (!cbarAimPending_ || nowMs() - cbarAimSentMs_ < 150u) return;
    cbarAimPending_ = false;
    cbarAimSentMs_ = nowMs();
    game::MovementInfo info = lastOwnMove_;
    info.pitch = cbarAim_;
    sendMovement(game::Opcode::MSG_MOVE_SET_PITCH, info);
}

// VehicleExit: the seat must allow it (VehicleSeatFlags CAN_ENTER_OR_EXIT; own rule: else nothing happens), then
// CMSG_REQUEST_VEHICLE_EXIT, no body - upstream sends it for every seat; AzerothCore HandleRequestVehicleExit checks the
// same seat flag and takes a driver off too.
void WorldSession::vehicleExit() {
    if (!inVehicle() || state_ != State::InWorld || !socket_) return;
    if (!canExitVehicle()) {
        PSP_LOGI(tag::Net, "vehicle_exit refused vehicle=%08X", static_cast<unsigned>(cbarVehicleGuid_ & 0xFFFFFFFFu));
        return;
    }
    socket_->send(network::Packet(game::wireOpcode(game::LogicalOpcode::CMSG_REQUEST_VEHICLE_EXIT)));
    PSP_LOGI(tag::Net, "vehicle_exit %s vehicle=%08X", controllingVehicle() ? "driver" : "passenger",
             static_cast<unsigned>(cbarVehicleGuid_ & 0xFFFFFFFFu));
}

// VehicleNextSeat / VehiclePrevSeat: the seat must allow switching; CMSG_REQUEST_VEHICLE_NEXT / PREV_SEAT (no body)
// from any seat (AzerothCore HandleChangeSeatsOnControlledVehicle takes them from a driver too).
void WorldSession::vehicleNextSeat(bool next) {
    if (!inVehicle() || !canSwitchVehicleSeats() || state_ != State::InWorld || !socket_) return;
    socket_->send(network::Packet(game::wireOpcode(next ? game::LogicalOpcode::CMSG_REQUEST_VEHICLE_NEXT_SEAT
                                                         : game::LogicalOpcode::CMSG_REQUEST_VEHICLE_PREV_SEAT)));
    PSP_LOGI(tag::Net, "vehicle_seat_%s driver=%d", next ? "next" : "prev", controllingVehicle() ? 1 : 0);
}

// UnitSwitchToVehicleSeat: a free seat of a vehicle that lets us switch; CMSG_REQUEST_VEHICLE_SWITCH_SEAT (the
// vehicle's packed guid, int8 seat) from any seat.
void WorldSession::vehicleSwitchSeat(int seat) {
    if (!inVehicle() || seat < 0 || seat > 7 || seat == cbarVehicleSeat_ || state_ != State::InWorld || !socket_) return;
    if (!cbarKit_.seat(seat) || vehicleSeatOccupant(seat) != 0 || !canSwitchVehicleSeats()) return;
    network::Packet p(game::wireOpcode(game::LogicalOpcode::CMSG_REQUEST_VEHICLE_SWITCH_SEAT));
    cbar::writeSwitchSeat(p, cbarVehicleGuid_, static_cast<int8_t>(seat));
    socket_->send(p);
    PSP_LOGI(tag::Net, "vehicle_seat_switch seat=%d driver=%d", seat, controllingVehicle() ? 1 : 0);
}

// UnitVehicleSkin("player") (own rule, psp_classbar_parse.hpp): by the vehicle's creature type.
bool WorldSession::vehicleSkinNatural() const {
    if (!cbarVehicleGuid_) return false;
    auto e = entities_.getEntity(cbarVehicleGuid_);
    if (!e) return false;
    const game::CreatureQueryResponseData* info = creatureInfo(e->getField(kFieldEntry));
    return info && cbar::vehicleSkinNatural(info->creatureType);
}

// UnitVehicleSeatInfo's occupant: us on our seat, else a unit we saw on it (its ONTRANSPORT block / monster move).
uint64_t WorldSession::vehicleSeatOccupant(int seat) const {
    if (!inVehicle()) return 0;
    if (seat == cbarVehicleSeat_) return playerGuid_;
    for (const auto& [guid, rs] : cbarRiderSeat_)
        if (rs.first == cbarVehicleGuid_ && rs.second == seat) return guid;
    return 0;
}

void WorldSession::spellClick(uint64_t guid) {
    if (!guid || state_ != State::InWorld || !socket_) return;
    network::Packet p(game::wireOpcode(game::LogicalOpcode::CMSG_SPELLCLICK));
    cbar::writeGuid64(p, guid);
    socket_->send(p);
    PSP_LOGI(tag::Net, "spellclick guid=%08X", static_cast<unsigned>(guid & 0xFFFFFFFFu));
}

void WorldSession::enterPlayerVehicle(uint64_t guid) {
    if (!guid || state_ != State::InWorld || !socket_) return;
    network::Packet p(game::wireOpcode(game::LogicalOpcode::CMSG_PLAYER_VEHICLE_ENTER));
    cbar::writeGuid64(p, guid);
    socket_->send(p);
    PSP_LOGI(tag::Net, "vehicle_enter_player guid=%08X", static_cast<unsigned>(guid & 0xFFFFFFFFu));
}

void WorldSession::ejectPassenger(uint64_t guid) {
    if (!guid || state_ != State::InWorld || !socket_) return;
    network::Packet p(game::wireOpcode(game::LogicalOpcode::CMSG_CONTROLLER_EJECT_PASSENGER));
    cbar::writeGuid64(p, guid);
    socket_->send(p);
}

void WorldSession::cbarEnterVehicle(uint64_t vehicle, int8_t seat, const char* src) {
    const bool same = vehicle == cbarVehicleGuid_;
    if (same && seat == cbarVehicleSeat_) return;
    cbarVehicleGuid_ = vehicle;
    cbarVehicleSeat_ = seat;
    if (!same) cbarAim_ = 0.0f;
    cbarLoadKit(unitVehicleId(vehicle));
    ++cbarVehicleVersion_;
    const cbar::VehicleSeatRec* s = vehicleSeat();
    PSP_LOGI(tag::Net, "vehicle_enter vehicle=%08X kit=%u seat=%d flags=%08X ui=%d src=%s",
             static_cast<unsigned>(vehicle & 0xFFFFFFFFu), static_cast<unsigned>(cbarKitId_), static_cast<int>(seat),
             static_cast<unsigned>(s ? s->flags : 0u), vehicleUi() ? 1 : 0, src);
}

void WorldSession::cbarLeaveVehicle(const char* src) {
    if (!cbarVehicleGuid_) return;
    PSP_LOGI(tag::Net, "vehicle_exit_done vehicle=%08X src=%s", static_cast<unsigned>(cbarVehicleGuid_ & 0xFFFFFFFFu), src);
    cbarVehicleGuid_ = 0;
    cbarVehicleSeat_ = -1;
    cbarAim_ = 0.0f;
    cbarKit_ = cbar::VehicleKit{};
    cbarKitId_ = 0;
    ++cbarVehicleVersion_;
}

bool WorldSession::takeVehicleExitPlace(glm::vec3& serverPos, float& orientation) {
    if (!cbarExitPending_) return false;
    cbarExitPending_ = false;
    serverPos = cbarExitPos_;
    orientation = cbarExitO_;
    return true;
}

// --- The mover -----------------------------------------------------------------------------------------------------------

bool WorldSession::movementGuid(uint64_t& guid) const {
    guid = playerGuid_;
    if (cbarMover_ != 0 && cbarMover_ != playerGuid_) {
        guid = cbarMover_;  // a vehicle we drive, a unit we possess: its movement
        return true;
    }
    if (cbarControlLost_) return false;  // the server moves us (charmed, a fear...)
    return cbarVehicleGuid_ == 0;        // a passenger moves with the vehicle (rooted)
}

float WorldSession::moverRunSpeed() const { return cbarMoverRun_ > 0.01f ? cbarMoverRun_ : 7.0f; }

// SMSG_FORCE_*_SPEED_CHANGE for the unit we move: ACKed with its guid and its movement (the mover's own counter).
bool WorldSession::cbarForceSpeed(network::Packet& packet, game::Opcode ack, uint64_t guid) {
    if (!packet.hasRemaining(4)) return false;
    const uint32_t counter = packet.readUInt32();
    const size_t remaining = packet.getRemainingSize();
    if (remaining >= 8) packet.readUInt32();
    else if (remaining >= 5) packet.readUInt8();
    if (!packet.hasRemaining(4)) return false;
    const float speed = packet.readFloat();
    if (!std::isfinite(speed) || speed < 0.1f || speed > 100.0f) return true;
    if (socket_) {
        network::Packet p(game::wireOpcode(ack));
        p.writePackedGuid(guid);
        p.writeUInt32(counter);
        game::MovementInfo wire = lastOwnMove_;
        wire.time = nowMs();
        game::MovementPacket::writeMovementPayload(p, wire);
        p.writeFloat(speed);
        socket_->send(p);
    }
    if (ack == game::Opcode::CMSG_FORCE_RUN_SPEED_CHANGE_ACK) cbarMoverRun_ = speed;
    PSP_LOGI(tag::Net, "mover_speed guid=%08X speed=%.2f counter=%u", static_cast<unsigned>(guid & 0xFFFFFFFFu),
             static_cast<double>(speed), static_cast<unsigned>(counter));
    return true;
}

// --- Packets ---------------------------------------------------------------------------------------------------------------

bool WorldSession::handleClassBarsPacket(game::LogicalOpcode op, network::Packet& packet) {
    using game::LogicalOpcode;
    if (state_ != State::InWorld) return false;
    switch (op) {
        case LogicalOpcode::SMSG_TOTEM_CREATED: {
            cbar::TotemCreated t;
            if (!cbar::readTotemCreated(packet, t)) return true;
            if (t.slot >= kTotemSlots) return true;
            TotemSlotState& s = cbarTotems_[t.slot];
            s.guid = t.guid;
            s.spellId = t.spellId;
            s.durationMs = t.durationMs;
            s.startMs = nowMs();
            ++cbarTotemVersion_;
            PSP_LOGI(tag::Net, "totem_created slot=%u spell=%u ms=%u", static_cast<unsigned>(t.slot),
                     static_cast<unsigned>(t.spellId), static_cast<unsigned>(t.durationMs));
            return true;
        }
        case LogicalOpcode::SMSG_PLAYER_VEHICLE_DATA: {
            uint64_t guid = 0;
            uint32_t id = 0;
            if (!cbar::readPlayerVehicleData(packet, guid, id)) return true;
            if (id) cbarUnitVehicle_[guid] = id;
            else cbarUnitVehicle_.erase(guid);
            if (guid == cbarVehicleGuid_) cbarLoadKit(id);
            ++cbarVehicleVersion_;
            PSP_LOGI(tag::Net, "vehicle_data guid=%08X id=%u", static_cast<unsigned>(guid & 0xFFFFFFFFu),
                     static_cast<unsigned>(id));
            return true;
        }
        case LogicalOpcode::SMSG_CLIENT_CONTROL_UPDATE: {
            uint64_t guid = 0;
            uint8_t allow = 0;
            if (!cbar::readClientControl(packet, guid, allow)) return true;
            const uint64_t before = cbarMover_;
            if (guid == playerGuid_) {
                cbarControlLost_ = allow == 0;
                if (allow) cbarMover_ = 0;
            } else if (allow) {
                cbarMover_ = guid;  // the unit is ours to move (a vehicle we drive, a possessed unit)
                cbarMoverRun_ = 0.0f;
            } else if (guid == cbarMover_) {
                cbarMover_ = 0;
            }
            // No CMSG_SET_ACTIVE_MOVER here: upstream sends it on world entry only (AzerothCore just logs a mismatch).
            if (cbarMover_ != before && ++cbarLogs_ <= 40)
                PSP_LOGI(tag::Net, "mover_change %08X -> %08X", static_cast<unsigned>(before & 0xFFFFFFFFu),
                         static_cast<unsigned>(cbarMover_ & 0xFFFFFFFFu));
            ++cbarVehicleVersion_;
            PSP_LOGI(tag::Net, "client_control guid=%08X allow=%u mover=%08X lost=%d",
                     static_cast<unsigned>(guid & 0xFFFFFFFFu), static_cast<unsigned>(allow),
                     static_cast<unsigned>(cbarMover_ & 0xFFFFFFFFu), cbarControlLost_ ? 1 : 0);
            return true;
        }
        case LogicalOpcode::SMSG_ON_CANCEL_EXPECTED_RIDE_VEHICLE_AURA:
            // AzerothCore Unit::Mount sends it with SMSG_PLAYER_VEHICLE_DATA when a player becomes a vehicle - not an
            // exit; the seat itself comes with the movement. Nothing to send.
            PSP_LOGI(tag::Net, "vehicle_cancel_expected in=%d", inVehicle() ? 1 : 0);
            return true;
        case LogicalOpcode::SMSG_MONSTER_MOVE_TRANSPORT: {
            // Ours onto a unit = a vehicle seat (AzerothCore Vehicle::AddPassenger's spline with TransportEnter); a
            // rider of a vehicle keeps its seat for the indicator. A peek - the transport code reads the rest.
            const size_t pos = packet.getReadPos();
            uint64_t mover = 0, transport = 0;
            int8_t seat = -1;
            const bool ok = readMonsterMoveTransportHead(packet, mover, transport, seat);
            packet.setReadPos(pos);
            if (!ok || !isUnitGuid(transport)) return false;
            if (mover == playerGuid_) {
                cbarEnterVehicle(transport, seat, "move");
                return true;
            }
            cbarRiderSeat_[mover] = {transport, seat};
            return false;
        }
        case LogicalOpcode::SMSG_MONSTER_MOVE: {
            // Our own spline while seated: TransportExit (AzerothCore Unit::_ExitVehicle) - off the vehicle, to its end.
            if (!inVehicle()) return false;
            const size_t pos = packet.getReadPos();
            if (!packet.hasFullPackedGuid()) return false;
            const uint64_t guid = packet.readPackedGuid();
            packet.setReadPos(pos);
            if (guid != playerGuid_) return false;
            game::MonsterMoveData d;
            network::Packet copy = packet;
            if (!game::MonsterMoveParser::parse(copy, d)) return false;
            if (!(d.splineFlags & kSplineTransportExit)) return false;
            cbarExitPos_ = d.hasDest ? glm::vec3(d.destX, d.destY, d.destZ) : glm::vec3(d.x, d.y, d.z);
            cbarExitO_ = d.moveType == 4 ? cleanTransportAngle(d.facingAngle) : 0.0f;
            cbarExitPending_ = true;
            cbarLeaveVehicle("spline");
            return true;
        }
        case LogicalOpcode::SMSG_FORCE_RUN_SPEED_CHANGE:
        case LogicalOpcode::SMSG_FORCE_RUN_BACK_SPEED_CHANGE:
        case LogicalOpcode::SMSG_FORCE_WALK_SPEED_CHANGE:
        case LogicalOpcode::SMSG_FORCE_SWIM_SPEED_CHANGE:
        case LogicalOpcode::SMSG_FORCE_SWIM_BACK_SPEED_CHANGE:
        case LogicalOpcode::SMSG_FORCE_FLIGHT_SPEED_CHANGE:
        case LogicalOpcode::SMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE:
        case LogicalOpcode::SMSG_FORCE_TURN_RATE_CHANGE:
        case LogicalOpcode::SMSG_FORCE_PITCH_RATE_CHANGE: {
            // Only the unit we move (else the mount code takes it: ours, the other players' prediction).
            if (cbarMover_ == 0 || cbarMover_ == playerGuid_) return false;
            const size_t pos = packet.getReadPos();
            if (!packet.hasFullPackedGuid()) return false;
            const uint64_t guid = packet.readPackedGuid();
            if (guid != cbarMover_) {
                packet.setReadPos(pos);
                return false;
            }
            using game::Opcode;
            Opcode ack = Opcode::CMSG_FORCE_RUN_SPEED_CHANGE_ACK;
            switch (op) {
                case LogicalOpcode::SMSG_FORCE_RUN_BACK_SPEED_CHANGE: ack = Opcode::CMSG_FORCE_RUN_BACK_SPEED_CHANGE_ACK; break;
                case LogicalOpcode::SMSG_FORCE_WALK_SPEED_CHANGE: ack = Opcode::CMSG_FORCE_WALK_SPEED_CHANGE_ACK; break;
                case LogicalOpcode::SMSG_FORCE_SWIM_SPEED_CHANGE: ack = Opcode::CMSG_FORCE_SWIM_SPEED_CHANGE_ACK; break;
                case LogicalOpcode::SMSG_FORCE_SWIM_BACK_SPEED_CHANGE: ack = Opcode::CMSG_FORCE_SWIM_BACK_SPEED_CHANGE_ACK; break;
                case LogicalOpcode::SMSG_FORCE_FLIGHT_SPEED_CHANGE: ack = Opcode::CMSG_FORCE_FLIGHT_SPEED_CHANGE_ACK; break;
                case LogicalOpcode::SMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE: ack = Opcode::CMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE_ACK; break;
                case LogicalOpcode::SMSG_FORCE_TURN_RATE_CHANGE: ack = Opcode::CMSG_FORCE_TURN_RATE_CHANGE_ACK; break;
                case LogicalOpcode::SMSG_FORCE_PITCH_RATE_CHANGE: ack = Opcode::CMSG_FORCE_PITCH_RATE_CHANGE_ACK; break;
                default: break;
            }
            return cbarForceSpeed(packet, ack, guid);
        }
        default:
            return false;
    }
}

// Every update block: a unit's vehicle kit, the run speed of the unit we move, our seat in a create block (logged in
// on a vehicle), the riders' seats; our charm-related fields are read per frame (classBarsUpdate).
void WorldSession::classBarsUpdateBlock(const game::UpdateBlock& b) {
    using game::UpdateType;
    const bool create = b.updateType == UpdateType::CREATE_OBJECT || b.updateType == UpdateType::CREATE_OBJECT2;
    if (create && (b.objectType == game::ObjectType::UNIT || b.objectType == game::ObjectType::PLAYER) &&
        (b.updateFlags & 0x0080u)) {  // UPDATEFLAG_VEHICLE
        if (b.vehicleId) cbarUnitVehicle_[b.guid] = b.vehicleId;
        if (b.guid == cbarVehicleGuid_) cbarLoadKit(b.vehicleId);
    }
    if (b.hasMovement && b.guid == cbarMover_ && b.runSpeed > 0.01f && b.runSpeed < 200.0f) cbarMoverRun_ = b.runSpeed;
    if (!b.hasMovement || !(create || b.updateType == UpdateType::MOVEMENT)) return;
    const bool onUnit = b.onTransport && isUnitGuid(b.transportGuid);
    if (b.guid == playerGuid_) {
        if (onUnit) cbarEnterVehicle(b.transportGuid, b.transportSeat, "block");
        return;
    }
    if (onUnit) cbarRiderSeat_[b.guid] = {b.transportGuid, b.transportSeat};
    else cbarRiderSeat_.erase(b.guid);
}

void WorldSession::classBarsForget(uint64_t guid) {
    cbarUnitVehicle_.erase(guid);
    cbarRiderSeat_.erase(guid);
    for (TotemSlotState& t : cbarTotems_)
        if (guid != 0 && t.guid == guid) {  // the totem object is gone (destroyed, recalled): the slot empties
            t = TotemSlotState{};
            ++cbarTotemVersion_;
        }
    if (guid != 0 && guid == cbarVehicleGuid_) cbarLeaveVehicle("despawn");
    if (guid != 0 && guid == cbarMover_) cbarMover_ = 0;
}

void WorldSession::classBarsResetWorld() {
    for (TotemSlotState& t : cbarTotems_) t = TotemSlotState{};
    ++cbarTotemVersion_;
    cbarUnitVehicle_.clear();
    cbarRiderSeat_.clear();
    cbarLeaveVehicle("new_world");
    cbarMover_ = 0;
    cbarControlLost_ = false;
    cbarExitPending_ = false;
}

void WorldSession::classBarsUpdate(float dtSec) {
    (void)dtSec;
    if (state_ != State::InWorld) return;
    if (cbarSpellsVersion_ != knownSpellsVersion()) cbarRebuildLists();
    cbarUpdateOverride();
    cbarSendAim();
    // A totem whose time ran out (TotemButton_Update: duration > 0; the object is despawned by the server).
    const uint32_t now = nowMs();
    for (TotemSlotState& t : cbarTotems_)
        if (t.active() && t.durationMs && static_cast<int32_t>(now - (t.startMs + t.durationMs)) > 2000) {
            t = TotemSlotState{};
            ++cbarTotemVersion_;
        }
    // Dead: off any vehicle (AzerothCore exits a dead passenger; the spline may not come).
    if (inVehicle() && life() != Life::Alive) cbarLeaveVehicle("dead");
}

}  // namespace wowee::platform::psp
