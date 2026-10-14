// Class bars: data/text.pak "class_bars" and "class_vehicles" (tools/psp/build_text_pak.py class_bars /
// class_vehicles) - rows of the game's own tables the class bars and the vehicles read (Spell.dbc,
// SpellShapeshiftForm.dbc, OverrideSpellData.dbc, TotemCategory.dbc, SummonProperties.dbc, Vehicle.dbc, VehicleSeat.dbc,
// VehicleUIIndSeat.dbc), never the server's database:
//   "class_bars" (kept, ~6 KB): "WCBR", u32 version 1;
//     u32 n + n x {u32 spell, i32 StanceBarOrder}  - the spells a learn puts on the shapeshift bar (sorted by spell);
//     u32 n + n x {u32 spell, u32 slot mask}       - SPELL_ATTR7_CAN_BE_MULTI_CAST spells: the totem slots their
//                                                    TotemCategory fills (bit k = 0-based slot k);
//     u32 n + n x {u32 spell, u32 flags}           - control spells (not read by the session: it takes the possession
//                                                    from the aura types in spells.pak);
//     u32 n + n x {u32 form, u32 PresetSpellID[8]}  - the forms with preset spells;
//     u32 n + n x {u32 id, u32 Spells[10]}          - OverrideSpellData;
//     u32 n + n x {u32 id, u32 type, u32 mask}      - TotemCategory (not read by the session).
//   "class_vehicles" (read when a vehicle is entered, only its rows kept): "WCBV", u32 version 1;
//     u32 n + n x Vehicle (64 B), u32 n + n x Seat (64 B), u32 n + n x IndicatorSeat (16 B) - below.
// Header-only and free of PSP headers: tools/psp/host_tests/classbars_test.cpp parses synthetic blobs with it.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace wowee::platform::psp::cbar {

struct VehicleRec {  // Vehicle.dbc
    uint32_t id = 0, flags = 0;
    float turnSpeed = 0.0f, pitchSpeed = 0.0f, pitchMin = 0.0f, pitchMax = 0.0f;
    uint32_t seat[8] = {};       // SeatID[8] (VehicleSeat ids, 0 = none)
    uint32_t uiIndicator = 0;    // VehicleUIIndicatorID (the seat indicator's background)
    uint32_t powerDisplay = 0;   // PowerDisplayID[0]
};
static_assert(sizeof(VehicleRec) == 64, "WCBV vehicle");

struct VehicleSeatRec {  // VehicleSeat.dbc
    uint32_t id = 0, flags = 0, flagsB = 0;
    int32_t attachmentId = -1;   // the attachment of the vehicle's model the passenger sits on (-1 = the model's origin)
    float offset[3] = {};        // AttachmentOffset
    uint32_t rideAnimStart = 0, rideAnimLoop = 0;  // the passenger's animations (AnimationData ids, 0 = none)
    uint32_t abilityDisplay = 0; // VehicleAbilityDisplay (1 = the vehicle's actions on the bar)
    int32_t uiSkin = -1;         // UiSkin (kept in the bake; UnitVehicleSkin follows an own rule)
    float cameraOffset[3] = {};  // CameraOffset
    float passengerYaw = 0.0f;   // PassengerYaw (radians)
    uint32_t vehicleRideAnimLoop = 0;  // the vehicle's own animation while the seat is taken (0 = none)
};
static_assert(sizeof(VehicleSeatRec) == 64, "WCBV seat");

struct IndicatorSeatRec {  // VehicleUIIndSeat.dbc
    uint32_t indicator = 0, seatIndex = 0;  // VehicleUIIndicatorID, the seat (1-based)
    float x = 0.0f, y = 0.0f;               // XPos / YPos (0..1 of the indicator's square)
};
static_assert(sizeof(IndicatorSeatRec) == 16, "WCBV indicator seat");

struct SpellOrder {
    uint32_t spell;
    int32_t order;
};
struct SpellFlags {
    uint32_t spell;
    uint32_t flags;
};
struct FormPresets {
    uint32_t form;
    uint32_t spell[8];
};
struct OverrideSpells {
    uint32_t id;
    uint32_t spell[10];
};
struct TotemCategoryRec {
    uint32_t id, type, mask;
};

constexpr uint32_t kPossessAura = 1, kPossessSummon = 2, kPossessVehicleAura = 4, kPossessHideBar = 8,
                   kPossessCantCancel = 16;

namespace detail {
class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    bool u32(uint32_t& v) {
        if (o_ + 4 > n_) return false;
        std::memcpy(&v, p_ + o_, 4);
        o_ += 4;
        return true;
    }
    template <typename T>
    bool table(std::vector<T>& out, uint32_t limit) {
        uint32_t count = 0;
        if (!u32(count) || count > limit) return false;
        if (o_ + static_cast<size_t>(count) * sizeof(T) > n_) return false;
        out.resize(count);
        if (count) std::memcpy(out.data(), p_ + o_, static_cast<size_t>(count) * sizeof(T));
        o_ += static_cast<size_t>(count) * sizeof(T);
        return true;
    }
    bool magic(const char* m) {
        if (o_ + 4 > n_ || std::memcmp(p_ + o_, m, 4) != 0) return false;
        o_ += 4;
        return true;
    }

private:
    const uint8_t* p_;
    size_t n_, o_ = 0;
};
template <typename T>
const T* findById(const std::vector<T>& v, uint32_t id) {
    auto it = std::lower_bound(v.begin(), v.end(), id, [](const T& a, uint32_t b) { return a.id < b; });
    return it != v.end() && it->id == id ? &*it : nullptr;
}
}  // namespace detail

// "class_bars": the spell lists (sorted by spell id when baked; sorted again here, an older bake may not be).
struct ClassBarData {
    std::vector<SpellOrder> stance;
    std::vector<SpellFlags> totem, possess;
    std::vector<FormPresets> forms;
    std::vector<OverrideSpells> overrides;
    std::vector<TotemCategoryRec> totemCats;
    bool loaded = false;

    bool parse(const uint8_t* p, size_t n) {
        *this = ClassBarData{};
        detail::Reader r(p, n);
        uint32_t version = 0;
        if (!r.magic("WCBR") || !r.u32(version) || version != 1) return false;
        if (!r.table(stance, 20000) || !r.table(totem, 20000) || !r.table(possess, 20000) || !r.table(forms, 64) ||
            !r.table(overrides, 4096) || !r.table(totemCats, 4096))
            return false;
        std::sort(stance.begin(), stance.end(), [](const SpellOrder& a, const SpellOrder& b) { return a.spell < b.spell; });
        auto bySpell = [](const SpellFlags& a, const SpellFlags& b) { return a.spell < b.spell; };
        std::sort(totem.begin(), totem.end(), bySpell);
        std::sort(possess.begin(), possess.end(), bySpell);
        std::sort(overrides.begin(), overrides.end(), [](const OverrideSpells& a, const OverrideSpells& b) { return a.id < b.id; });
        std::sort(totemCats.begin(), totemCats.end(),
                  [](const TotemCategoryRec& a, const TotemCategoryRec& b) { return a.id < b.id; });
        loaded = true;
        return true;
    }
    // False: not a shapeshift bar spell; order = Spell.dbc StanceBarOrder.
    bool stanceOrder(uint32_t spell, int32_t& order) const {
        auto it = std::lower_bound(stance.begin(), stance.end(), spell,
                                   [](const SpellOrder& a, uint32_t b) { return a.spell < b; });
        if (it == stance.end() || it->spell != spell) return false;
        order = it->order;
        return true;
    }
    uint32_t totemSlots(uint32_t spell) const { return flagsOf(totem, spell); }
    uint32_t possessFlags(uint32_t spell) const { return flagsOf(possess, spell); }
    const uint32_t* formPresets(uint32_t form) const {
        for (const FormPresets& f : forms)
            if (f.form == form) return f.spell;
        return nullptr;
    }
    const uint32_t* overrideSpells(uint32_t id) const {
        auto it = std::lower_bound(overrides.begin(), overrides.end(), id,
                                   [](const OverrideSpells& a, uint32_t b) { return a.id < b; });
        return it != overrides.end() && it->id == id ? it->spell : nullptr;
    }
private:
    static uint32_t flagsOf(const std::vector<SpellFlags>& v, uint32_t spell) {
        auto it = std::lower_bound(v.begin(), v.end(), spell, [](const SpellFlags& a, uint32_t b) { return a.spell < b; });
        return it != v.end() && it->spell == spell ? it->flags : 0u;
    }
};

// "class_vehicles": parsed whole only while a vehicle is looked up (the caller keeps the rows it needs).
struct VehicleTables {
    std::vector<VehicleRec> vehicles;
    std::vector<VehicleSeatRec> seats;
    std::vector<IndicatorSeatRec> indicatorSeats;

    bool parse(const uint8_t* p, size_t n) {
        *this = VehicleTables{};
        detail::Reader r(p, n);
        uint32_t version = 0;
        if (!r.magic("WCBV") || !r.u32(version) || version != 1) return false;
        if (!r.table(vehicles, 65536) || !r.table(seats, 65536) || !r.table(indicatorSeats, 65536)) return false;
        std::sort(vehicles.begin(), vehicles.end(), [](const VehicleRec& a, const VehicleRec& b) { return a.id < b.id; });
        std::sort(seats.begin(), seats.end(), [](const VehicleSeatRec& a, const VehicleSeatRec& b) { return a.id < b.id; });
        return true;
    }
    const VehicleRec* vehicle(uint32_t id) const { return detail::findById(vehicles, id); }
    const VehicleSeatRec* seat(uint32_t id) const { return detail::findById(seats, id); }
};

// One vehicle as the session keeps it: the Vehicle row, its eight seats (id 0 = none) and the indicator's seats.
struct VehicleKit {
    VehicleRec vehicle;
    VehicleSeatRec seats[8];
    std::vector<IndicatorSeatRec> indicatorSeats;
    bool valid = false;

    bool load(const VehicleTables& t, uint32_t vehicleId) {
        *this = VehicleKit{};
        const VehicleRec* v = t.vehicle(vehicleId);
        if (!v) return false;
        vehicle = *v;
        for (int i = 0; i < 8; ++i)
            if (const VehicleSeatRec* s = vehicle.seat[i] ? t.seat(vehicle.seat[i]) : nullptr) seats[i] = *s;
        for (const IndicatorSeatRec& s : t.indicatorSeats)
            if (vehicle.uiIndicator && s.indicator == vehicle.uiIndicator) indicatorSeats.push_back(s);
        valid = true;
        return true;
    }
    // UnitVehicleSeatCount: the seats with a VehicleSeat row (PSP: no accessory vehicles' seats added).
    int seatCount() const {
        int n = 0;
        for (const VehicleSeatRec& s : seats) n += s.id ? 1 : 0;
        return n;
    }
    const VehicleSeatRec* seat(int index) const {
        return index >= 0 && index < 8 && seats[index].id ? &seats[index] : nullptr;
    }
};

}  // namespace wowee::platform::psp::cbar
