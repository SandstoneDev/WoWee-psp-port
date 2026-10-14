// Freeing bag space: destroying an item as the 3.3.5a client does it.
//
// The original: the item is picked up and put down on the world (WorldFrame, outside every frame); the client fires
// DELETE_ITEM_CONFIRM (name, quality) and UIParent shows StaticPopup "DELETE_ITEM" (quality < 3: YES / NO) or
// "DELETE_GOOD_ITEM" (quality >= 3, rare and better: YES only once the edit box says DELETE_ITEM_CONFIRM_STRING);
// OnAccept = DeleteCursorItem (CMSG_DESTROYITEM, the whole stack), OnCancel / Escape = ClearCursor,
// and the popup hides when the cursor no longer holds the item (OnUpdate).
//
// PSP: the cursor item is the Square pick-up (invHeld_); "put down on the world" = Cross or Square with the cursor
// over no window and no button. The edit box opens the system keyboard (Cross); the confirm string is compared
// with the Cyrillic letters folded to upper case too, on both sides (PSP: the keyboard's case switch is awkward;
// the client's strupper folds ASCII only); the keyboard offers Cyrillic when the running language's confirm string
// has it, else Latin letters. The alert icon (showAlert): the auction code's StaticPopup::alert.
#include "ui/psp/ui_world_screen.hpp"

#include "game/inventory_slots.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_osk.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_strings.hpp"  // the client's GlobalStrings
#include "platform/psp/psp_texture.hpp"

#include <pspctrl.h>

#include <cstdio>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

constexpr uint32_t kIdDelYes = 0x8C00, kIdDelNo = 0x8C01, kIdDelEdit = 0x8C02;  // this window's widget id range
constexpr int kOskOwnerDelete = 0x8C;

// strupper of the client (ASCII) + the Cyrillic lower case U+0430..U+044F / U+0451 (UTF-8, two bytes) - see the header.
std::string upperRu(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 'a' && c <= 'z') {
            out += static_cast<char>(c - 32);
        } else if (c == 0xD0 && i + 1 < s.size()) {  // U+0400..U+043F
            const unsigned char d = static_cast<unsigned char>(s[i + 1]);
            if (d >= 0xB0 && d <= 0xBF) {  // U+0430..U+043F -> U+0410..U+041F (D0 90..9F)
                out += static_cast<char>(0xD0);
                out += static_cast<char>(d - 0x20);
            } else {
                out += static_cast<char>(c);
                out += static_cast<char>(d);
            }
            ++i;
        } else if (c == 0xD1 && i + 1 < s.size()) {  // U+0440..U+047F
            const unsigned char d = static_cast<unsigned char>(s[i + 1]);
            if (d >= 0x80 && d <= 0x8F) {  // U+0440..U+044F -> U+0420..U+042F (D0 A0..AF)
                out += static_cast<char>(0xD0);
                out += static_cast<char>(d + 0x20);
            } else if (d == 0x91) {  // U+0451 -> U+0401 (D0 81)
                out += static_cast<char>(0xD0);
                out += static_cast<char>(0x81);
            } else {
                out += static_cast<char>(c);
                out += static_cast<char>(d);
            }
            ++i;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

// DELETE_ITEM_CONFIRM_STRING of the running language, typed: both sides folded.
bool confirmTyped(const std::string& typed) {
    return upperRu(typed) == upperRu(CLIENT_STR(DELETE_ITEM_CONFIRM_STRING));
}

}  // namespace

// The item at a wire position (bag 0xFF: equipment 0..22, the backpack 23..38, the bank 39..; a worn bag 19..22 or
// a bank bag: its slots).
const game::ItemDef* WorldScreen::itemAtWire(uint8_t bag, uint8_t slot) const {
    namespace sl = game::slots;
    const auto& inv = session_.getInventory();
    const game::ItemSlot* s = nullptr;
    if (bag == sl::kNoContainer) {
        if (slot < game::Inventory::NUM_EQUIP_SLOTS) s = &inv.getEquipSlot(static_cast<game::EquipSlot>(slot));
        else if (slot >= sl::kBackpackFirst && slot < sl::kBackpackFirst + sl::kBackpackCount)
            s = &inv.getBackpackSlot(slot - sl::kBackpackFirst);
        else if (slot >= sl::kBankGeneralFirst && slot < sl::kBankGeneralFirst + sl::bankGeneralCount() &&
                 slot - sl::kBankGeneralFirst < game::Inventory::BANK_SLOTS)
            s = &inv.getBankSlot(slot - sl::kBankGeneralFirst);
    } else if (bag >= sl::kWornBagFirst && bag < sl::kWornBagFirst + sl::kWornBagCount) {
        const int b = bag - sl::kWornBagFirst;
        if (slot < inv.getBagSize(b)) s = &inv.getBagSlot(b, slot);
    } else if (bag >= sl::bankBagFirst() && bag < sl::bankBagFirst() + sl::bankBagCount()) {
        const int b = bag - sl::bankBagFirst();
        if (slot < inv.getBankBagSize(b)) s = &inv.getBankBagSlot(b, slot);
    }
    return s && !s->empty() ? &s->item : nullptr;
}

// Cross / Square with the cursor over nothing (no window, no button) while an item is held: WorldFrame's click with
// an item on the cursor -> DELETE_ITEM_CONFIRM. True = handled (the press is ours).
bool WorldScreen::deleteItemPad(const pp::PadState& pad) {
    if (!invHeld_.active || !(pad.pressed & (PSP_CTRL_CROSS | PSP_CTRL_SQUARE))) return false;
    if (windowOpen(kWinDeleteItem) || winFocus_.focused() != 0) return false;
    const float cx = winFocus_.cursorX(), cy = winFocus_.cursorY();
    for (const auto& [w, r] : winRect_)
        if (windowOpen(w) && cx >= r.x && cx < r.x + r.w && cy >= r.y && cy < r.y + r.h) return false;
    const game::ItemDef* it = itemAtWire(invHeld_.bag, invHeld_.slot);
    if (!it) {
        invHeld_ = InvHeld{};
        return true;
    }
    if (del_.alert.pixels) pp::textureRelease(del_.alert);
    del_ = DeleteItemState{};
    del_.bag = invHeld_.bag;
    del_.slot = invHeld_.slot;
    del_.itemId = it->itemId;
    del_.name = it->name;
    del_.good = static_cast<int>(it->quality) >= 3;  // UIParent: arg2 >= 3 -> DELETE_GOOD_ITEM
    del_.open = true;
    openWindow(kWinDeleteItem);
    PSP_LOGI(pp::tag::Ctrl, "delete_confirm item=%u bag=%u slot=%u quality=%d good=%d", static_cast<unsigned>(del_.itemId),
             static_cast<unsigned>(del_.bag), static_cast<unsigned>(del_.slot), static_cast<int>(it->quality),
             del_.good ? 1 : 0);
    return true;
}

bool WorldScreen::drawDeleteItemPopup(UiRenderer& ui, uint8_t w) {
    if (w != kWinDeleteItem) return false;
    // OnUpdate: the cursor no longer holds that item -> the popup goes.
    if (!invHeld_.active || invHeld_.bag != del_.bag || invHeld_.slot != del_.slot) {
        closeWindow(kWinDeleteItem);
        return true;
    }
    if (!del_.alertTried) {  // showAlert = 1 of DELETE_ITEM / _GOOD_ITEM
        del_.alertTried = true;
        char path[256];
        pp::fsPath(path, sizeof(path), "data/ui.pak");
        pp::Pak pak;
        if (pak.open(path)) {
            const pp::PakEntry* e = pak.find("mail_alert");
            void* blob = e ? pak.load(e) : nullptr;
            if (blob && !pp::textureFromBlob(blob, e->size, del_.alert, true)) pp::pakFree(blob);
            pak.close();
        }
    }
    bool accepted = false;
    std::string text;
    if (del_.osk && pp::oskFinishedFor(kOskOwnerDelete, accepted, text)) {
        del_.osk = false;
        if (accepted) del_.typed = text;
    }
    static char buf[400];
    std::snprintf(buf, sizeof(buf), "%s",
                  (del_.good ? CLIENT_FMT(DELETE_GOOD_ITEM, del_.name) : CLIENT_FMT(DELETE_ITEM, del_.name)).c_str());
    StaticPopup sp;
    sp.text = buf;
    sp.button1 = CLIENT_STR(YES);
    sp.button1Id = kIdDelYes;
    sp.button1Enabled = !del_.good || confirmTyped(del_.typed);  // EditBoxOnTextChanged
    sp.button2 = CLIENT_STR(NO);
    sp.button2Id = kIdDelNo;
    if (del_.good) {
        sp.editText = del_.typed.c_str();
        sp.editId = kIdDelEdit;
    }
    sp.focusable = uiMode_ && !settings_.isOpen() && !del_.osk;
    sp.alert = del_.alert.pixels ? &del_.alert : nullptr;
    drawStaticPopup(ui, sp);
    return true;
}

bool WorldScreen::deleteItemAction(uint32_t act) {
    if (act != kIdDelYes && act != kIdDelNo && act != kIdDelEdit) return false;
    if (act == kIdDelEdit) {
        if (!del_.osk) {
            const char* word = CLIENT_STR(DELETE_ITEM_CONFIRM_STRING);
            bool cyrillic = false;
            for (const char* p = word; *p; ++p) cyrillic |= static_cast<unsigned char>(*p) >= 0x80;
            del_.osk = pp::oskBeginFor(kOskOwnerDelete, word, del_.typed, 32,
                                       cyrillic ? pp::OskCharset::Cyrillic : pp::OskCharset::LatinLetters);
        }
        return true;
    }
    if (act == kIdDelYes) {
        // DeleteCursorItem: the whole stack - only if that slot still holds the item the popup named.
        const game::ItemDef* it = itemAtWire(del_.bag, del_.slot);
        if (it && it->itemId == del_.itemId && (!del_.good || confirmTyped(del_.typed))) {
            session_.destroyItem(del_.bag, del_.slot, 0);
            invStatus_ = OWN_FMT(INV_DESTROYED, del_.name);
            invStatusMs_ = pp::logUptimeMs();
        }
    }
    invHeld_ = InvHeld{};  // OnAccept / OnCancel: the cursor is empty after either
    del_.open = false;
    closeWindow(kWinDeleteItem);
    return true;
}

// The autopilot's "open delete [item]" - DELETE_ITEM / DELETE_GOOD_ITEM for an item of the
// backpack or the worn bags (that item id, else the first item of the backpack) as if picked up with Square and put
// down on the world; "close delete" - the No button (OnCancel). The autopilot never presses Yes.
bool WorldScreen::devDeleteItem(bool open, uint32_t itemId) {
    namespace sl = game::slots;
    if (!open) {
        if (!windowOpen(kWinDeleteItem)) return false;
        return deleteItemAction(kIdDelNo);
    }
    if (windowOpen(kWinDeleteItem)) return true;
    uint8_t bag = sl::kNoContainer, slot = 0;
    const game::ItemDef* it = nullptr;
    for (int i = 0; i < sl::kBackpackCount && !it; ++i) {
        const auto* c = itemAtWire(sl::kNoContainer, static_cast<uint8_t>(sl::kBackpackFirst + i));
        if (c && (!itemId || c->itemId == itemId)) {
            it = c;
            slot = static_cast<uint8_t>(sl::kBackpackFirst + i);
        }
    }
    const auto& inv = session_.getInventory();
    for (int b = 0; b < sl::kWornBagCount && !it; ++b)
        for (int i = 0; i < inv.getBagSize(b) && !it; ++i) {
            const auto* c = itemAtWire(static_cast<uint8_t>(sl::kWornBagFirst + b), static_cast<uint8_t>(i));
            if (c && (!itemId || c->itemId == itemId)) {
                it = c;
                bag = static_cast<uint8_t>(sl::kWornBagFirst + b);
                slot = static_cast<uint8_t>(i);
            }
        }
    if (!it) return false;
    if (!uiMode_) setUiMode(true);
    invHeld_ = InvHeld{true, bag, slot};
    if (del_.alert.pixels) pp::textureRelease(del_.alert);
    del_ = DeleteItemState{};
    del_.bag = bag;
    del_.slot = slot;
    del_.itemId = it->itemId;
    del_.name = it->name;
    del_.good = static_cast<int>(it->quality) >= 3;
    del_.open = true;
    openWindow(kWinDeleteItem);
    PSP_LOGI(pp::tag::Ctrl, "delete_confirm item=%u bag=%u slot=%u quality=%d good=%d dev=1",
             static_cast<unsigned>(del_.itemId), static_cast<unsigned>(bag), static_cast<unsigned>(slot),
             static_cast<int>(it->quality), del_.good ? 1 : 0);
    return true;
}

// Every frame (updateInventory): closed another way (START = Escape) -> ClearCursor, as OnCancel.
void WorldScreen::deleteItemTick() {
    if (del_.open && !windowOpen(kWinDeleteItem)) {
        del_.open = false;
        invHeld_ = InvHeld{};
    }
    if (!windowOpen(kWinDeleteItem) && del_.alertTried) {  // the icon only while the popup shows
        if (del_.alert.pixels) pp::textureRelease(del_.alert);
        del_.alert = pp::Texture{};
        del_.alertTried = false;
    }
}

}  // namespace wowee::ui::psp
