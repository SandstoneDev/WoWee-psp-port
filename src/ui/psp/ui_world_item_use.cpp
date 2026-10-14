// The world screen's half of using items (the session half is
// psp_world_session_items.cpp, the targets psp_spell_targets.cpp).
//
//   - ○ on an item in a bag = the right click = UseContainerItem (after the merchant / bank / mail branches the
//     windows own): equip it, open it (clams, lockboxes, gifts -> LootFrame), start its quest (QuestFrame), use it
//     (CMSG_USE_ITEM: the Hearthstone, potions, food and drink, bandages, scrolls, quest items). ○ on a worn item
//     with a use spell (a trinket) = UseInventoryItem; without one the PSP's take-off stays.
//   - SpellIsTargeting (a sharpening stone, an oil, an enchant scroll): the spell's icon follows the cursor; ✕ / □ / ○ on an item = SpellTargetItem, START = SpellStopTargeting.
//   - StaticPopup USE_BIND (USE_NO_DROP, OKAY / CANCEL): ConfirmBindOnUse (window 0x10).
//   - item buttons on the action bar (ActionButton.lua): the item's icon, the count (IsConsumableAction /
//     IsStackableAction -> GetActionCount, NumberFontNormal at BOTTOMRIGHT -2,2), the icon 0.4 grey without the item
//     (IsUsableAction), the green border of a worn item (IsEquippedAction: UI-ActionButton-Border, ADD, 0,1,0,0.35),
//     the cooldown sweep (GetActionCooldown -> the item's cooldown); a click = UseAction (equip / use). An item
//     held with □ is placed on a button with ✕ / □ (PickupContainerItem + PlaceAction); □ on an item button takes
//     the action onto the cursor (PickupAction).
//   - the ContainerFrame cooldown sweep of the bag items (ContainerFrame_UpdateCooldown).
//   - the GameTooltip line ITEM_COOLDOWN_TIME / COOLDOWN_ON_LEAVE_COMBAT (the time as below, own rule).
//   - SMSG_STANDSTATE_UPDATE: food and drink sit us down (upstream standStateCallback), without echoing it back.
#include "ui/psp/ui_world_screen.hpp"

#include "game/inventory_slots.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_strings.hpp"  // GlobalStrings keys

#include <algorithm>
#include <cstdio>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
using game::ActionBarSlot;
using ItemUse = pp::WorldSession::ItemUse;

namespace {

// Widget ids (batch 6 table: item-use 0x80xx); window 0x10 (kWinUseBind).
constexpr uint32_t kIdUseBindOk = 0x8000, kIdUseBindCancel = 0x8001;

// The cooldown's time text (own rule, the single-unit idea of FrameXML SecondsToTimeAbbrev): the largest unit only,
// rounded up - DAYS_ABBR / HOURS_ABBR / MINUTES_ABBR / SECONDS_ABBR ("%d" + the unit).
std::string cooldownTime(uint32_t ms) {
    const uint32_t sec = (ms + 999u) / 1000u;
    uint32_t v = sec;
    const char* form = CLIENT_STR(SECONDS_ABBR);
    if (sec >= 86400u) {
        v = (sec + 86399u) / 86400u;
        form = CLIENT_STR(DAYS_ABBR);
    } else if (sec >= 3600u) {
        v = (sec + 3599u) / 3600u;
        form = CLIENT_STR(HOURS_ABBR);
    } else if (sec >= 60u) {
        v = (sec + 59u) / 60u;
        form = CLIENT_STR(MINUTES_ABBR);
    }
    return pp::text::textf(pp::text::plural(form, static_cast<long>(v)).c_str(), static_cast<unsigned>(v));
}

}  // namespace

WorldScreen::ItemUseArt::~ItemUseArt() { pp::textureRelease(border); }

// --- Per frame -----------------------------------------------------------------------------------------------------

void WorldScreen::updateItemUse() {
    if (!itemUseInit_) {
        itemUseInit_ = true;
        PSP_LOGI(pp::tag::Gfx, "item_use_init dbc=%d", items_.isOpen() ? 1 : 0);
    }
    // SMSG_STANDSTATE_UPDATE: the realm sat us down (food, drink) or stood us up.
    uint8_t st = 0;
    while (session_.takeStandState(st)) applyServerStandState(st);
    // USE_BIND_CONFIRM -> StaticPopup_Show("USE_BIND"); a popup with nothing pending goes; Circle / START = cancel.
    if (session_.takeUseBindPrompt()) {
        if (!uiMode_) {
            setUiMode(true);
            itemUseRaisedUi_ = true;
            suppress_ = ~0u;  // what is held now stays dead until released
        }
        openWindow(kWinUseBind);
        winFocus_.setFocus(kIdUseBindCancel);  // binding takes a deliberate second choice (as the equip prompt)
    }
    const bool open = windowOpen(kWinUseBind);
    if (open && !session_.hasPendingUseConfirm()) closeWindow(kWinUseBind);
    if (!open && session_.hasPendingUseConfirm()) session_.cancelUseBind();
    if (itemUseRaisedUi_ && !windowOpen(kWinUseBind) && !session_.isItemTargeting()) {
        itemUseRaisedUi_ = false;
        if (uiMode_ && windows_.empty() && !settings_.isOpen()) {
            setUiMode(false);
            suppress_ = ~0u;
        }
    }
}

// upstream standStateCallback -> the character's stand state (ActivityFSM: sit down, the chair heights, kneel);
// PSP: not echoed back as CMSG_STANDSTATECHANGE (setSitting would). 7 (dead) is the death code's.
void WorldScreen::applyServerStandState(uint8_t st) {
    goNoteStandState(st);  // the game-object use code's chairs
    if (st == 7) return;
    const bool sit = st != 0;
    if (sit && mountActive_) return;
    if (sit != sitting_) {  // (the realm's echo of our own CMSG_STANDSTATECHANGE changes nothing - AFK stays)
        sitting_ = sit;
        afk_ = false;
        PSP_LOGI(pp::tag::Gfx, "stand_state_server state=%u", static_cast<unsigned>(st));
    }
    animator_.setStandState(st);
}

// What the screen shows of a result: the status line of the bag window (PSP), the bags for an item target.
void WorldScreen::itemUseFeedback(ItemUse r, const game::ItemDef* item) {
    const uint32_t now = pp::logUptimeMs();
    auto say = [&](const std::string& s) {
        invStatus_ = s;
        invStatusMs_ = now;
    };
    switch (r) {
        case ItemUse::Equip:
            if (item) say(OWN_FMT(INV_EQUIPPING, item->name));
            break;
        case ItemUse::NoInfo:
            say(OWN_STR(INV_NO_ITEM_INFO));
            break;
        case ItemUse::Readable:
            say(OWN_STR(INV_READ_UNSUPPORTED));  // PSP: ItemTextFrame is not on the PSP yet
            break;
        case ItemUse::PickItem:
            // SpellIsTargeting: the use waits for an item - PSP: the bags open for the pick.
            if (!uiMode_) {
                setUiMode(true);
                itemUseRaisedUi_ = true;
                suppress_ = ~0u;
            }
            if (!windowOpen(kWinBags)) {
                openBags(true);
                openWindow(kWinBags);
            }
            break;
        default:
            break;
    }
}

// --- Bags and the character window ---------------------------------------------------------------------------------

// ○ on a bag item: UseContainerItem's tail (bag 0 = the backpack, 1..4 = a worn bag).
bool WorldScreen::itemUseContainerClick(int bag, int slot, const game::ItemDef& item) {
    if (sitting_) {
        // A spell with a cast time stands us up (the Hearthstone); food sits us down again itself.
        const uint32_t spell = session_.itemUseSpell(item.itemId);
        const pp::SpellInfo* si = spell ? spellDb_.get(spell) : nullptr;
        if (si && si->rec.castMs > 0) setSitting(false, false);
    }
    const ItemUse r = session_.useContainerItem(bag, slot);
    itemUseFeedback(r, &item);
    return true;
}

// ○ on a worn item with a use spell (a trinket, an engineering helm): UseInventoryItem. False: no use spell - the
// PSP's take-off stays.
bool WorldScreen::itemUseWornClick(int equipSlot, const game::ItemDef& item) {
    if (session_.itemUseSpell(item.itemId) == 0) return false;
    const ItemUse r = session_.useItemAt(game::slots::kNoContainer, static_cast<uint8_t>(equipSlot));
    itemUseFeedback(r, &item);
    return true;
}

// SpellTargetItem: any click on an item while an item spell waits for one. True = taken.
bool WorldScreen::itemUseTargetClick(uint32_t id) {
    if (!session_.isItemTargeting()) return false;
    uint8_t bag = 0, slot = 0;
    if (!inventoryIdToWire(id, bag, slot)) return false;
    if (!inventoryItemAt(id)) return true;  // an empty slot: the cursor stays
    const ItemUse r = session_.useOnItem(bag, slot);
    PSP_LOGI(pp::tag::Net, "item_use_target bag=%u slot=%u sent=%d", static_cast<unsigned>(bag),
             static_cast<unsigned>(slot), r == ItemUse::Sent ? 1 : 0);
    return true;
}

bool WorldScreen::itemUseEscape() {  // START: SpellStopTargeting
    if (!session_.isItemTargeting()) return false;
    session_.cancelItemTargeting();
    return true;
}

// ContainerFrame_UpdateCooldown: the sweep over a bag item (none while it waits for the end of combat).
void WorldScreen::drawItemCooldownOverlay(UiRenderer& ui, const Rect& r, uint32_t entry) {
    float rem = 0.0f, total = 0.0f;
    bool hold = false;
    if (entry && session_.itemCooldown(entry, rem, total, hold) && !hold) drawCooldown(ui, r, rem, total);
}

// GameTooltip: ITEM_COOLDOWN_TIME with the time left (own rule: while any time is left), or COOLDOWN_ON_LEAVE_COMBAT
// while on hold; "" = none.
std::string WorldScreen::itemCooldownText(uint32_t entry) const {
    float rem = 0.0f, total = 0.0f;
    bool hold = false;
    if (!entry || !session_.itemCooldown(entry, rem, total, hold)) return std::string();
    if (hold) return CLIENT_STR(COOLDOWN_ON_LEAVE_COMBAT);
    const uint32_t ms = static_cast<uint32_t>(rem * 1000.0f);
    if (ms == 0) return std::string();
    return CLIENT_FMT(ITEM_COOLDOWN_TIME, cooldownTime(ms));
}

uint32_t WorldScreen::itemCooldownTipKey(uint32_t entry) const {  // changes with every shown second
    float rem = 0.0f, total = 0.0f;
    bool hold = false;
    if (!entry || !session_.itemCooldown(entry, rem, total, hold)) return 0;
    return hold ? 0xFFFFFu : static_cast<uint32_t>(rem) + 1u;
}

// --- Action bar item buttons ---------------------------------------------------------------------------------------

// UseAction on an item button.
void WorldScreen::useItemAction(uint32_t entry) {
    const uint32_t spell = session_.itemUseSpell(entry);
    const pp::SpellInfo* si = spell ? spellDb_.get(spell) : nullptr;
    if (sitting_ && si && si->rec.castMs > 0) setSitting(false, false);
    const ItemUse r = session_.useItemByEntry(entry);
    if (r == ItemUse::NoInfo) session_.ensureItemInfo(entry);
    itemUseFeedback(r, nullptr);
}

// GetActionTexture: the item's icon (Item.dbc display -> ItemDisplayInfo icon).
const platform::psp::Texture* WorldScreen::itemActionIcon(uint32_t entry) {
    uint32_t displayId = 0;
    uint8_t invType = 0;
    if (!(items_.isOpen() && items_.itemDisplay(entry, displayId, invType)) || displayId == 0) {
        const auto* info = session_.getItemInfo(entry);
        if (!info) {
            session_.ensureItemInfo(entry);
            return nullptr;
        }
        displayId = info->displayInfoId;
    }
    return displayId ? itemIcons().byDisplay(displayId, spellFrame_) : nullptr;
}

// IsUsableAction for an item: none carried -> 0.4 grey; on hold (a potion in combat) - not usable either.
uint8_t WorldScreen::itemActionUsable(uint32_t entry) {
    if (session_.itemCount(entry) == 0) return 2;
    float rem = 0.0f, total = 0.0f;
    bool hold = false;
    if (session_.itemCooldown(entry, rem, total, hold) && hold) return 2;
    return 0;
}

void WorldScreen::drawItemActionCooldown(UiRenderer& ui, const Rect& r, uint32_t entry) {
    drawItemCooldownOverlay(ui, r, entry);
}

// The count (ActionButton_UpdateCount) and the worn item's border (IsEquippedAction).
void WorldScreen::drawItemActionOverlay(UiRenderer& ui, const Rect& r, uint32_t entry) {
    if (session_.itemEquipped(entry)) {
        if (!itemUseArtTried_) {
            itemUseArtTried_ = true;
            char path[256];
            pp::fsPath(path, sizeof(path), "data/ui.pak");
            pp::Pak pak;
            bool ok = false;
            if (pak.open(path)) {
                if (const pp::PakEntry* e = pak.find("ab_border")) {
                    if (void* blob = pak.load(e)) {
                        ok = pp::textureFromBlob(blob, e->size, itemUseArt_.border, true);
                        if (!ok) pp::pakFree(blob);
                    }
                }
                pak.close();
            }
            PSP_LOGI(pp::tag::Gfx, "item_use_art ok=%d kb=%u", ok ? 1 : 0, ok ? 2u : 0u);
        }
        if (itemUseArt_.border.pixels) {  // $parentBorder 62 x 62 around the 36 button, ADD, (0, 1, 0, 0.35)
            const float s = r.w * 62.0f / 36.0f;
            ui.image(itemUseArt_.border, Rect{r.x + (r.w - s) * 0.5f, r.y + (r.h - s) * 0.5f, s, s}, 0x5900FF00u, Blend::Add);
        }
    }
    if (session_.itemConsumable(entry) || session_.itemStackable(entry)) {
        const uint32_t n = session_.itemCount(entry);
        char b[16];
        if (n > 9999) std::snprintf(b, sizeof(b), "*");
        else std::snprintf(b, sizeof(b), "%u", static_cast<unsigned>(n));
        const Font& fs = kit_.font(kFontSmall);
        ui.textShadow(fs, r.x + r.w + 1.0f, r.y + r.h - static_cast<float>(fs.lineHeight()) + 2.0f, b, kColWhite,
                      Align::Right);
    }
}

// PickupContainerItem + PlaceAction: the item held with Square goes on the button (the item stays where it is);
// the button's old action comes onto the cursor (PlaceAction swaps). True = taken.
bool WorldScreen::placeHeldItemOnAction(int slot) {
    if (!invHeld_.active) return false;
    const auto& inv = session_.getInventory();
    uint32_t entry = 0;
    if (invHeld_.bag == game::slots::kNoContainer) {
        if (invHeld_.slot < game::Inventory::NUM_EQUIP_SLOTS) {
            const auto& s = inv.getEquipSlot(static_cast<game::EquipSlot>(invHeld_.slot));
            if (!s.empty()) entry = s.item.itemId;
        } else {
            const int i = static_cast<int>(invHeld_.slot) - game::slots::kBackpackFirst;
            if (i >= 0 && i < inv.getBackpackSize() && !inv.getBackpackSlot(i).empty())
                entry = inv.getBackpackSlot(i).item.itemId;
        }
    } else {
        const int b = static_cast<int>(invHeld_.bag) - game::slots::kWornBagFirst;
        if (b >= 0 && b < game::Inventory::NUM_BAG_SLOTS && invHeld_.slot < inv.getBagSize(b) &&
            !inv.getBagSlot(b, invHeld_.slot).empty())
            entry = inv.getBagSlot(b, invHeld_.slot).item.itemId;
    }
    invHeld_ = InvHeld{};
    const auto& bar = session_.actionBar();
    if (entry == 0 || slot < 0 || slot >= static_cast<int>(bar.size())) return true;
    const ActionBarSlot old = bar[static_cast<size_t>(slot)];
    session_.setActionBarSlot(slot, ActionBarSlot::ITEM, entry);
    if ((old.type == ActionBarSlot::SPELL || old.type == ActionBarSlot::ITEM) && old.id != 0) {
        bookPick_ = old.id;
        bookPickItem_ = old.type == ActionBarSlot::ITEM;
    }
    PSP_LOGI(pp::tag::Ctrl, "action_item_place slot=%d entry=%u", slot, static_cast<unsigned>(entry));
    return true;
}

// --- USE_BIND and the targeting cursor -----------------------------------------------------------------------------

bool WorldScreen::drawItemUseWindow(UiRenderer& ui, uint8_t w) {
    if (w != kWinUseBind) return false;
    // StaticPopupDialogs["USE_BIND"]: USE_NO_DROP, OKAY / CANCEL (OnAccept ConfirmBindOnUse), hideOnEscape.
    StaticPopup p;
    p.text = CLIENT_STR(USE_NO_DROP);
    p.button1 = CLIENT_STR(OKAY);
    p.button1Id = kIdUseBindOk;
    p.button2 = CLIENT_STR(CANCEL);
    p.button2Id = kIdUseBindCancel;
    p.focusable = uiMode_ && !settings_.isOpen();
    drawStaticPopup(ui, p);
    return true;
}

bool WorldScreen::itemUseWindowAction(uint32_t act) {
    if (act == kIdUseBindOk) {
        closeWindow(kWinUseBind);
        itemUseFeedback(session_.confirmUseBind(), nullptr);
        return true;
    }
    if (act == kIdUseBindCancel) {
        session_.cancelUseBind();
        closeWindow(kWinUseBind);
        return true;
    }
    return false;
}

// The targeting cursor: the item's spell icon at the cursor, as the spell on the cursor (drawSpellCursor).
void WorldScreen::drawItemUseOverlay(UiRenderer& ui) {
    if (!session_.isItemTargeting() || !uiMode_ || settings_.isOpen()) return;
    const pp::Texture* t = spellDb_.icon(session_.itemTargetingSpell(), spellFrame_);
    if (!t) t = itemActionIcon(session_.itemTargetingEntry());
    if (t) ui.image(*t, Rect{winFocus_.cursorX() + 6.0f, winFocus_.cursorY() + 6.0f, 18, 18}, 0xD0FFFFFFu);
}

}  // namespace wowee::ui::psp
