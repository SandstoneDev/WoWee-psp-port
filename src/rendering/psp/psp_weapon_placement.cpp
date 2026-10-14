// Where helm, shoulders, weapons, shields and ranged items sit on a character by its sheath state (EBOOT runtime +
// PC tools). Clean-room implementation (cleanroom/clean/equip) of the spec weapon_placement.md section 3 (3.2-3.4)
// with the per-pick glow of weapon_glow.md section 3; declarations in psp_equipment.hpp, the sheath point table
// (sheathPointOf) in psp_item_format.hpp.
#include "rendering/psp/psp_equipment.hpp"

#include <cmath>

namespace wowee::rendering::psp {

namespace {

// M2 attachment ids used (public M2 attachment list).
constexpr uint16_t kAttachShield = 0;
constexpr uint16_t kAttachHandRight = 1;
constexpr uint16_t kAttachHandLeft = 2;
constexpr uint16_t kAttachShoulderLeft = 5;
constexpr uint16_t kAttachShoulderRight = 6;
constexpr uint16_t kAttachHelm = 11;
constexpr uint16_t kAttachBack = 12;
constexpr uint16_t kAttachSheathShield = 28;
constexpr uint16_t kAttachHipLeft = 32;
constexpr uint16_t kAttachHipRight = 33;

constexpr int kSlotHead = 0;
constexpr int kSlotShoulders = 2;
constexpr int kSlotMainHand = 15;
constexpr int kSlotRanged = 17;

// 3x3 row-major helpers (float, as the spec's transform is float data).
struct Mat3 {
    float m[3][3];
};

Mat3 mul(const Mat3& a, const Mat3& b) {
    Mat3 r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            float s = 0.0f;
            for (int k = 0; k < 3; ++k) s += a.m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    }
    return r;
}

Mat3 rotX(float rad) {
    const float c = std::cos(rad);
    const float s = std::sin(rad);
    return Mat3{{{1.0f, 0.0f, 0.0f}, {0.0f, c, -s}, {0.0f, s, c}}};
}

Mat3 rotY(float rad) {
    const float c = std::cos(rad);
    const float s = std::sin(rad);
    return Mat3{{{c, 0.0f, s}, {0.0f, 1.0f, 0.0f}, {-s, 0.0f, c}}};
}

// A slot whose display has a weapon / shield model (3.2 "Weapon slots").
struct WeaponModel {
    const ItemModels* models = nullptr;
    uint16_t ownGlow = 0;
    uint8_t hint = kSheathUnknown;
};

bool weaponModelOf(const Equipment& eq, int slot, ItemBank& items, WeaponModel& out) {
    const uint32_t displayId = eq.displayId[slot];
    if (displayId == 0) return false;
    const ItemDisplay* rec = items.get(displayId);
    if (rec == nullptr) return false;
    const ItemModels* m = items.models(displayId);
    if (m == nullptr || (m->kind != kItemModelWeapon && m->kind != kItemModelShield)) return false;
    out.models = m;
    if (rec->version >= kItemFormatWeapons) {
        out.ownGlow = m->itemVisual;
        out.hint = m->sheathHint;
    } else {
        out.ownGlow = 0;
        out.hint = kSheathUnknown;
    }
    return true;
}

// The common pick content; false when the model for (side, variant) is none (the pick does not exist).
bool makePick(AttachPick& p, uint16_t point, int slot, int side, const Equipment& eq, const ItemModels& m, int v) {
    const uint16_t model = m.model[side][v];
    if (model == 0xFFFF) return false;
    p = AttachPick{};
    p.point = point;
    p.model = model;
    p.texture = m.texture[side][v];
    p.slot = static_cast<uint8_t>(slot);
    p.side = static_cast<uint8_t>(side);
    p.displayId = eq.displayId[slot];
    p.hasLocal = false;
    for (float& f : p.local) f = 0.0f;
    p.itemVisual = 0;
    return true;
}

// Putting: a pick at a point that already holds an entry replaces it in place; otherwise it is appended.
void put(std::vector<AttachPick>& out, const AttachPick& p) {
    for (AttachPick& e : out) {
        if (e.point == p.point) {
            e = p;
            return;
        }
    }
    out.push_back(p);
}

}  // namespace

void twoHandBackTransform(float out[12]) {
    const float kDegToRad = 3.14159265358979323846f / 180.0f;
    const Mat3 r = mul(mul(rotX(33.0f * kDegToRad), rotY(90.0f * kDegToRad)), rotX(90.0f * kDegToRad));
    for (int c = 0; c < 3; ++c) {
        for (int row = 0; row < 3; ++row) out[c * 3 + row] = r.m[row][c];
    }
    out[9] = -0.03f;
    out[10] = -0.10f;
    out[11] = 0.0f;
}

uint16_t legacyPutAwayPoint(int slot, uint8_t invType, bool& hasLocal) {
    hasLocal = false;
    if (slot != 15 && slot != 16) return kNoSheathPoint;
    switch (invType) {
        case 17:  // two-hand
            hasLocal = true;
            return kAttachBack;
        case 14:  // shield
            return kAttachSheathShield;
        case 13:  // one-hand
        case 21:  // main hand
            return slot == 15 ? kAttachHipLeft : kAttachHipRight;
        default:
            return kNoSheathPoint;
    }
}

std::vector<AttachPick> equipmentAttachmentsBySheath(const Equipment& worn, uint8_t race, uint8_t sex,
                                                     uint8_t sheathState, ItemBank& items,
                                                     const SheathAttachOptions& options) {
    std::vector<AttachPick> out;
    if (!items.isOpen()) return out;
    // equipment_dress.md 3.1: item models are drawn from the shown equipment (idempotent for an already shown copy).
    const Equipment eq = shownEquipment(worn);
    const int v = itemVariant(race, sex);
    AttachPick p;

    // Helm.
    if (eq.displayId[kSlotHead] != 0) {
        const ItemModels* m = items.models(eq.displayId[kSlotHead]);
        if (m != nullptr && m->kind == kItemModelHelm && makePick(p, kAttachHelm, kSlotHead, 0, eq, *m, v)) {
            put(out, p);
        }
    }
    // Shoulders.
    if (eq.displayId[kSlotShoulders] != 0) {
        const ItemModels* m = items.models(eq.displayId[kSlotShoulders]);
        if (m != nullptr && m->kind == kItemModelShoulder) {
            if (makePick(p, kAttachShoulderLeft, kSlotShoulders, 0, eq, *m, v)) put(out, p);
            if (makePick(p, kAttachShoulderRight, kSlotShoulders, 1, eq, *m, v)) put(out, p);
        }
    }

    // Hands: 0 = main hand (slot 15), 1 = off hand (slot 16).
    for (int hand = 0; hand < 2; ++hand) {
        const int slot = kSlotMainHand + hand;
        WeaponModel w;
        if (!weaponModelOf(eq, slot, items, w)) continue;

        uint8_t type = eq.sheath[hand];
        if (type == kSheathUnknown) type = w.hint;
        const bool typeKnown = type != kSheathUnknown;

        uint16_t point = kNoSheathPoint;
        bool hasLocal = false;
        if (hand == 1 && options.creature && eq.displayId[kSlotMainHand] != 0 && eq.invType[kSlotMainHand] == 17) {
            continue;  // a creature's off hand under a two-hand weapon
        } else if (sheathState == kSheathStateMelee && hand == 0) {
            point = kAttachHandRight;
        } else if (sheathState == kSheathStateMelee && eq.invType[slot] == 14) {
            point = kAttachShield;
        } else if (sheathState == kSheathStateMelee) {
            point = kAttachHandLeft;
        } else if (typeKnown) {
            point = sheathPointOf(hand, type);
        } else {
            point = legacyPutAwayPoint(slot, eq.invType[slot], hasLocal);
        }
        if (point == kNoSheathPoint) continue;
        if (!makePick(p, point, slot, 0, eq, *w.models, v)) continue;
        if (hasLocal) {
            p.hasLocal = true;
            twoHandBackTransform(p.local);
        }
        p.itemVisual = w.ownGlow != 0 ? w.ownGlow : eq.enchantVisual[hand];
        put(out, p);
    }

    // Ranged (slot 17); never glows.
    {
        WeaponModel w;
        if (weaponModelOf(eq, kSlotRanged, items, w)) {
            const uint8_t inv = eq.invType[kSlotRanged];
            uint16_t point = kNoSheathPoint;
            bool hasLocal = false;
            if (sheathState == kSheathStateRanged && (inv == 25 || inv == 26)) {
                point = kAttachHandRight;
            } else if (sheathState == kSheathStateRanged) {
                point = kAttachHandLeft;
            } else if (options.rangedOnBack && eq.displayId[kSlotMainHand] == 0 && sheathState == kSheathStateMelee) {
                point = kAttachHandRight;
            } else if (options.rangedOnBack && eq.displayId[kSlotMainHand] == 0) {
                point = kAttachBack;
                hasLocal = true;
            }
            if (point != kNoSheathPoint && makePick(p, point, kSlotRanged, 0, eq, *w.models, v)) {
                if (hasLocal) {
                    p.hasLocal = true;
                    twoHandBackTransform(p.local);
                }
                p.itemVisual = 0;
                put(out, p);
            }
        }
    }
    return out;
}

}  // namespace wowee::rendering::psp
