// data/itemicons.pak reader: the item icons of the original
// (Interface\Icons\<ItemDisplayInfo.InventoryIcon>, 32x32 T8), by display id.
//
// The pak (tools/psp/build_itemicons_pak.py) has a u16 slot per display id
// ("dmap", read per id from the card and remembered - 135 KB would not stay
// resident) and fixed-stride icons read on demand into a small cache, the
// same way SpellDb keeps the spell icons. The file is opened for reads and
// closed after 3 s idle (card fd limit, the MFILE rule).
#pragma once

#include "platform/psp/psp_texture.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace wowee::platform::psp {

class ItemIconDb {
public:
    // Fixed slots (GetCoinIcon and the unknown-item icon of the client).
    enum Fixed : uint16_t { kQuestionMark = 0, kCoinGold = 1, kCoinSilver = 2, kCoinCopper = 3 };

    ~ItemIconDb();
    bool open();  // false: no pak (callers fall back to their old look)
    bool isOpen() const { return iconsOff_ != 0; }
    // Memory diet: the last open() failed although the pak is on the card (no memory for its TOC /
    // header, no descriptor) - worth another try later (WorldScreen::itemIcons, at the next window open).
    bool openFailedTransient() const { return transient_; }

    // The icon of an ItemDisplayInfo id (the question mark for an unknown or
    // missing one), or nullptr (no pak / all cache entries in use this frame).
    const Texture* byDisplay(uint32_t displayId, uint32_t frame);
    const Texture* fixed(Fixed f, uint32_t frame) { return bySlot(f, frame); }
    // GetCoinIcon(amount): gold / silver / copper coins by the largest unit.
    const Texture* coin(uint32_t copper, uint32_t frame) {
        return bySlot(copper >= 10000 ? kCoinGold : copper >= 100 ? kCoinSilver : kCoinCopper, frame);
    }

    void tick(uint32_t nowMs);  // closes the file after 3 s without reads
    uint32_t iconsLoaded() const { return iconsLoaded_; }

private:
    bool transient_ = false;
    const Texture* bySlot(uint16_t slot, uint32_t frame);
    bool ensureOpen();
    bool readRaw(uint32_t offset, void* dst, uint32_t size);

    std::string path_;
    int fd_ = -1;
    uint32_t lastReadMs_ = 0;
    uint32_t iconCount_ = 0, iconStride_ = 0, maxDisplay_ = 0;
    uint32_t dmapOff_ = 0, iconsOff_ = 0;
    std::unordered_map<uint32_t, uint16_t> slotOf_;  // display id -> slot, read so far
    struct Icon {
        Texture tex;
        uint32_t lastFrame = 0;
        bool failed = false;
    };
    std::unordered_map<uint16_t, Icon> icons_;
    uint32_t iconsLoaded_ = 0;
    static constexpr size_t kMaxIcons = 48;
};

}  // namespace wowee::platform::psp
