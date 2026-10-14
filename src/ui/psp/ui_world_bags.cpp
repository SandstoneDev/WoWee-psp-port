// The bags of the original - ContainerFrame.xml / .lua and the bag
// bar of MainMenuBarBagButtons.xml (the backpack like the original).
//
//   - the bags entry opens every container (OpenAllBags), the merchant opens the
//     backpack (OpenBackpack); each is a ContainerFrame: the backpack on
//     UI-BackpackBackground (money at TOPRIGHT -2,-216), a bag built from
//     UI-Bag-Components by ContainerFrame_GenerateFrame (the top with the
//     first row - or the "+2" top, the middle rows, the bottom), the bag's
//     portrait, its name, the close button (UIPanelCloseButton);
//   - the frames stack from the bottom right, a new column to the left when
//     the screen is full (updateContainerFrameAnchors), 0.55 of their units;
//   - the slots in the original's order: slot 1 top left, the last bottom
//     right (ContainerFrameItemButtonTemplate: icon, count);
//   - the bag bar under them: CharacterBag3..0Slot and the backpack button,
//     Cross shows / hides that container, Square moves bags as before;
//   - Cross on an item: UseContainerItem (equip / sell at a merchant ...),
//     Square: pick up / put down; Circle closes them all; the X one frame.
#include "ui/psp/ui_world_screen.hpp"

#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"  // the client's GlobalStrings
#include "platform/psp/psp_texture.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

constexpr float kS = 0.55f;                       // tools/psp/build_ui_pak.py BAG_S
constexpr float kW = 192.0f;                      // CONTAINER_WIDTH
constexpr float kRight = 478.0f, kBottom = 240.0f;  // the stack's corner: right edge, above the bag bar
constexpr float kTop = 2.0f;
constexpr int kColumns = 4;                       // NUM_CONTAINER_COLUMNS

// ContainerFrame_GenerateFrame for a bag (not the backpack): the piece heights in frame units and
// their rows in UI-Bag-Components (512 high).
struct BagLayout {
    float topSrc0 = 0, topSrc1 = 0, topH = 0;
    float midSrc0 = 0, midH = 0;
    float height = 0;
    bool middle = false;
};
BagLayout bagLayout(int size) {
    BagLayout l;
    const int rows = (size + kColumns - 1) / kColumns;
    if (size % kColumns == 2) {  // isPlusTwoBag
        l.topSrc0 = 97;
        l.topSrc1 = 169;
        l.topH = 72;
    } else if (rows == 1) {
        l.topSrc0 = 2;
        l.topSrc1 = 86;
        l.topH = 86;
    } else {
        l.topSrc0 = 2;
        l.topSrc1 = 94;
        l.topH = 94;
    }
    const int remaining = rows - 1;
    if (rows > 1) {
        l.middle = true;
        l.midSrc0 = 181;  // firstRowTexCoordOffset 0.353515625
        l.midH = static_cast<float>(remaining * 41 - 9);
    }
    l.height = l.topH + l.midH + 10;  // + the bottom (TexCoords 0.330..0.3496, 10 high)
    return l;
}

}  // namespace

WorldScreen::BagArt::~BagArt() { release(); }

void WorldScreen::BagArt::release() {
    for (pp::Texture* t : {&backpack, &comps, &backpackBtn, &emptyBag})
        if (t->pixels) pp::textureRelease(*t);
    tried = false;
}

void WorldScreen::loadBagArt() {
    if (bagArt_.tried && bagArt_.artRetry.due()) bagArt_.tried = false;  // failed - again at the next open
    if (bagArt_.tried) return;
    bagArt_.tried = true;
    UiArtPak art("bags");  // ui_art_fail when the pak / a texture finds no memory
    art.load("bag_backpack", bagArt_.backpack);
    art.load("bag_comps", bagArt_.comps);
    art.load("bag_btn_backpack", bagArt_.backpackBtn);
    art.load("pd_slot_bag", bagArt_.emptyBag);
    if (art.failed()) bagArt_.artRetry.fail();
    art.close();
    PSP_LOGI(pp::tag::Gfx, "bag_art backpack=%d comps=%d retry=%d", bagArt_.backpack.pixels ? 1 : 0,
             bagArt_.comps.pixels ? 1 : 0, art.failed() ? 1 : 0);
}

bool WorldScreen::bagSlotId(uint32_t id, int& container, int& slot) {
    if (id < kIdBagSlot0 || id >= kIdBagSlot0 + 5 * 36) return false;
    container = static_cast<int>((id - kIdBagSlot0) / 36);
    slot = static_cast<int>((id - kIdBagSlot0) % 36);
    return true;
}

void WorldScreen::openBags(bool all) {
    const auto& inv = session_.getInventory();
    bagOpen_[0] = true;
    for (int b = 0; b < 4; ++b) bagOpen_[b + 1] = all && inv.getBagSize(b) > 0;
    if (!windowOpen(kWinBags)) openWindow(kWinBags);
}

void WorldScreen::drawContainerFrames(UiRenderer& ui, bool focusable) {
    loadBagArt();
    PROF_SCOPE("inventory.bags");
    const auto& inv = session_.getInventory();
    const Font& fs = kit_.font(kFontSmall);
    const float lh = static_cast<float>(fs.lineHeight());
    loadNpcArt(false);  // the close button, the coins

    // updateContainerFrameAnchors: the backpack first, then the bags, upwards; a new column when full.
    float colRight = kRight, bottom = kBottom;
    int shown = 0;
    for (int c = 0; c < 5; ++c) {
        const int size = c == 0 ? game::Inventory::BACKPACK_SLOTS : inv.getBagSize(c - 1);
        if (!bagOpen_[c] || size <= 0) continue;
        const BagLayout l = c == 0 ? BagLayout{} : bagLayout(size);
        const float hUnits = c == 0 ? 240.0f : l.height;  // BACKPACK_HEIGHT
        const float h = hUnits * kS, w = kW * kS;
        if (shown > 0 && bottom - h < kTop) {
            colRight -= w;
            bottom = kBottom;
        }
        const float fx = colRight - w, fy = bottom - h;
        bottom = fy;
        ++shown;

        // $parentPortrait (40 x 40 at 7,-5) under the art: the bag's icon (SetBagPortraitTexture).
        const Rect por{fx + 7 * kS, fy + 5 * kS, 40 * kS, 40 * kS};
        if (c == 0) {
            if (bagArt_.backpackBtn.pixels) ui.image(bagArt_.backpackBtn, por);
        } else {
            const auto& bag = inv.getEquipSlot(static_cast<game::EquipSlot>(game::Inventory::FIRST_BAG_EQUIP_SLOT + c - 1));
            if (!bag.empty())
                if (const pp::Texture* t = itemIcons().byDisplay(bag.item.displayInfoId, spellFrame_)) ui.image(*t, por);
        }
        // The art.
        if (c == 0) {
            if (bagArt_.backpack.pixels) ui.image(bagArt_.backpack, Rect{fx, fy, w, 256 * kS});
            else kit_.panel(ui, Rect{fx, fy, w, h});
        } else if (bagArt_.comps.pixels) {
            const pp::Texture& t = bagArt_.comps;
            const float tw = static_cast<float>(t.uw);
            ui.image(t, Rect{fx, fy, w, l.topH * kS}, SrcRect{0, l.topSrc0 * kS, tw, (l.topSrc1 - l.topSrc0) * kS});
            float y = fy + l.topH * kS;
            if (l.middle) {
                ui.image(t, Rect{fx, y, w, l.midH * kS}, SrcRect{0, l.midSrc0 * kS, tw, l.midH * kS});
                y += l.midH * kS;
            }
            ui.image(t, Rect{fx, y, w, 10 * kS}, SrcRect{0, 169 * kS, tw, 10 * kS});
        } else {
            kit_.panel(ui, Rect{fx, fy, w, h});
        }
        // $parentName (TOPLEFT 47,-10, 112 x 12, GameFontHighlight).
        {
            std::string name = CLIENT_STR(BACKPACK_TOOLTIP);
            if (c > 0) {
                const auto& bag = inv.getEquipSlot(static_cast<game::EquipSlot>(game::Inventory::FIRST_BAG_EQUIP_SLOT + c - 1));
                name = bag.empty() ? std::string() : bag.item.name;
            }
            const Rect nr{fx + 47 * kS, fy + 10 * kS + (12 * kS - lh) * 0.5f, 112 * kS, lh};
            if (static_cast<float>(fs.measure(name.c_str())) > nr.w) {
                ui.textMarquee(fs, nr, name.c_str(), kColWhite);
            } else {
                ui.textShadow(fs, nr.x, nr.y, name.c_str(), kColWhite);
            }
        }
        // $parentCloseButton (TOPRIGHT 0,-1).
        {
            const Rect cr{fx + w - 32 * kS, fy + 1 * kS, 32 * kS, 32 * kS};
            const uint32_t id = kIdBagClose0 + static_cast<uint32_t>(c);
            const bool f = focusable && winFocus_.addStop(id, cr);
            const pp::Texture& t = f && winFocus_.isPressed(id) && npcArt_.closeDown.pixels ? npcArt_.closeDown
                                                                                           : npcArt_.close;
            if (t.pixels) ui.image(t, cr);
            if (f) ui.fill(cr, 0x40FFFFFFu);
        }
        // Money (the backpack only: TOPRIGHT -2,-216).
        if (c == 0) drawMoneyFrame(ui, fx + (kW - 6) * kS, fy + 216 * kS, static_cast<uint32_t>(session_.getMoneyCopper()), true);

        // The items: Item1 bottom right (the last slot), rows upwards, columns to the left.
        const float baseY = c == 0 ? 208.0f : hUnits - 9.0f;
        for (int k = 0; k < size && k < 36; ++k) {
            const int slot = size - 1 - k;
            const float brx = kW - 12.0f - static_cast<float>(k % kColumns) * 42.0f;
            const float bry = baseY - static_cast<float>(k / kColumns) * 41.0f;
            const Rect r{fx + (brx - 37.0f) * kS, fy + (bry - 37.0f) * kS, 37.0f * kS, 37.0f * kS};
            const uint32_t id = kIdBagSlot0 + static_cast<uint32_t>(c * 36 + slot);
            const game::ItemSlot& s = c == 0 ? inv.getBackpackSlot(slot) : inv.getBagSlot(c - 1, slot);
            const bool f = focusable && winFocus_.addStop(id, r);
            uint8_t hb = 0, hs = 0;
            const bool wired = inventoryIdToWire(id, hb, hs);  // also for the letter's lock
            const bool held = invHeld_.active && wired && hb == invHeld_.bag && hs == invHeld_.slot;
            const bool locked = held || (wired && bankMailItemLocked(hb, hs)) ||  // on the letter
                                (wired && auctionItemLocked(hb, hs)) ||  // in the sell slot
                                (wired && session_.tradeItemLocked(hb, hs));  // on the trade table
            if (!s.empty()) {
                drawItemButton(ui, r, s.item.displayInfoId, s.item.stackCount, -1, locked ? 0xFF606060u : 0xFFFFFFFFu);
                drawItemCooldownOverlay(ui, r, s.item.itemId);  // ContainerFrame_UpdateCooldown
            }
            if (f) {
                ui.fill(r, 0x50FFFFFFu);  // ButtonHilight-Square
                if (!s.empty()) {
                    invTipId_ = id;
                    invTipRect_ = Rect{r.x + ui.offsetX(), r.y + ui.offsetY(), r.w, r.h};  // screen: drawn after the windows
                }
            }
        }
    }

    drawBankBagFrames(ui, focusable, colRight, bottom, shown);  // the bank's bags (ToggleBag 5..11)

    // The bag bar (MainMenuBarBagButtons): CharacterBag3Slot .. CharacterBag0Slot, then the backpack.
    const float by = 248.0f, bs = 18.0f, bp = 22.0f;  // right of the action bar (it ends at x 377)
    float bx = 478.0f - bp;
    {
        const Rect r{bx, by - 2, bp, bp};
        const bool f = focusable && winFocus_.addStop(0x6030 /* kIdInvBackpackTab */, r);
        if (bagArt_.backpackBtn.pixels) ui.image(bagArt_.backpackBtn, r);
        if (spellArt_.slot.pixels) {
            const float cc = r.w * 64.0f / 37.0f;
            ui.image(spellArt_.slot, Rect{r.x + r.w * 0.5f - cc * 0.5f, r.y + r.h * 0.5f - cc * 0.5f, cc, cc});
        }
        if (f) ui.fill(r, 0x50FFFFFFu);
        if (bagOpen_[0]) ui.fill(Rect{r.x, r.y + r.h, r.w, 1}, kColGold);
    }
    for (int b = 0; b < 4; ++b) {
        bx -= bs + 1;
        const Rect r{bx, by, bs, bs};
        const uint32_t id = 0x6010 + static_cast<uint32_t>(game::Inventory::FIRST_BAG_EQUIP_SLOT + b);  // kIdInvEquip0 + 19..22
        const auto& bag = inv.getEquipSlot(static_cast<game::EquipSlot>(game::Inventory::FIRST_BAG_EQUIP_SLOT + b));
        const bool f = focusable && winFocus_.addStop(id, r);
        if (!bag.empty()) {
            drawItemButton(ui, r, bag.item.displayInfoId, 1, -1, 0xFFFFFFFFu);
            if (f) {
                invTipId_ = id;
                invTipRect_ = Rect{r.x + ui.offsetX(), r.y + ui.offsetY(), r.w, r.h};  // screen: drawn after the windows
            }
        } else {
            if (bagArt_.emptyBag.pixels) ui.image(bagArt_.emptyBag, r);
            if (spellArt_.slot.pixels) {
                const float cc = r.w * 64.0f / 37.0f;
                ui.image(spellArt_.slot, Rect{r.x + r.w * 0.5f - cc * 0.5f, r.y + r.h * 0.5f - cc * 0.5f, cc, cc});
            }
        }
        if (f) ui.fill(r, 0x50FFFFFFu);
        if (bagOpen_[b + 1] && inv.getBagSize(b) > 0) ui.fill(Rect{r.x, r.y + r.h + 1, r.w, 1}, kColGold);
    }
}

}  // namespace wowee::ui::psp
