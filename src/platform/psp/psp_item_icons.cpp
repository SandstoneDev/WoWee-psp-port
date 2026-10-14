// data/itemicons.pak reader - see include/platform/psp/psp_item_icons.hpp.
#include "platform/psp/psp_item_icons.hpp"

#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"

#include <pspiofilemgr.h>
#include <pspkernel.h>

#include <cstdlib>
#include <cstring>
#include <malloc.h>

namespace wowee::platform::psp {

namespace {
constexpr uint32_t kCloseAfterMs = 3000;
}  // namespace

ItemIconDb::~ItemIconDb() {
    for (auto& [slot, ic] : icons_) textureRelease(ic.tex);
    if (fd_ >= 0) {
        pakNoteClose(fd_);
        sceIoClose(fd_);
    }
}

bool ItemIconDb::open() {
    char path[256];
    fsPath(path, sizeof(path), "data/itemicons.pak");
    Pak pak;
    transient_ = false;
    if (!pak.open(path)) {
        // on the card but not opened (no memory for the TOC, no descriptor) - not "missing".
        transient_ = fsExists(path);
        if (transient_) PSP_LOGW(tag::Fs, "itemicons_pak open_failed (items without icons until the next window open)");
        else PSP_LOGW(tag::Fs, "itemicons_pak missing (items drawn without icons)");
        return false;
    }
    const PakEntry* eh = pak.find("ihdr");
    const PakEntry* ed = pak.find("dmap");
    const PakEntry* ei = pak.find("icons");
    void* h = eh ? pak.load(eh) : nullptr;
    if (eh && !h) transient_ = true;  // the header had no memory (or the read failed): try again later
    bool ok = false;
    if (h && ed && ei && eh->size >= 24 && std::memcmp(h, "WIIC", 4) == 0) {
        uint32_t v[5];
        std::memcpy(v, static_cast<const uint8_t*>(h) + 4, sizeof(v));
        // version, count, stride, max display id, fixed slots
        ok = v[0] == 1 && v[1] > 4 && v[2] >= 64 && ed->size == (v[3] + 1u) * 2u && ei->size == v[1] * v[2];
        if (ok) {
            iconCount_ = v[1];
            iconStride_ = v[2];
            maxDisplay_ = v[3];
            dmapOff_ = ed->offset;
            iconsOff_ = ei->offset;
        }
    }
    pakFree(h);
    pak.close();  // MFILE rule: reopened for reads, closed when idle
    if (!ok) {
        iconsOff_ = 0;
        PSP_LOGE(tag::Fs, "itemicons_pak bad");
        return false;
    }
    path_ = path;
    PSP_LOGI(tag::Fs, "itemicons_pak icons=%u max_display=%u", static_cast<unsigned>(iconCount_),
             static_cast<unsigned>(maxDisplay_));
    return true;
}

bool ItemIconDb::ensureOpen() {
    if (fd_ >= 0) return true;
    if (path_.empty()) return false;
    fd_ = pakOpenRetry(path_.c_str());  // + one retry after a rare pak gave a descriptor back
    if (fd_ < 0) {
        PSP_LOGE(tag::Fs, "itemicons_pak open err=%08X", static_cast<unsigned>(fd_));
        fd_ = -1;
        return false;
    }
    pakNoteOpen(fd_, path_.c_str());  // in the descriptor budget
    return true;
}

bool ItemIconDb::readRaw(uint32_t offset, void* dst, uint32_t size) {
    if (!ensureOpen()) return false;
    lastReadMs_ = logUptimeMs();
    return pakRead(fd_, offset, dst, size);
}

void ItemIconDb::tick(uint32_t nowMs) {
    if (fd_ >= 0 && nowMs - lastReadMs_ > kCloseAfterMs) {
        pakNoteClose(fd_);
        sceIoClose(fd_);
        fd_ = -1;
    }
}

const Texture* ItemIconDb::byDisplay(uint32_t displayId, uint32_t frame) {
    if (!isOpen()) return nullptr;
    uint16_t slot = kQuestionMark;
    auto it = slotOf_.find(displayId);
    if (it != slotOf_.end()) {
        slot = it->second;
    } else {
        if (displayId > 0 && displayId <= maxDisplay_) {
            uint16_t s = 0xFFFF;
            if (readRaw(dmapOff_ + displayId * 2u, &s, 2) && s != 0xFFFF && s < iconCount_) slot = s;
        }
        if (slotOf_.size() < 4096) slotOf_[displayId] = slot;
    }
    return bySlot(slot, frame);
}

const Texture* ItemIconDb::bySlot(uint16_t slot, uint32_t frame) {
    if (!isOpen() || slot >= iconCount_) return nullptr;
    auto it = icons_.find(slot);
    if (it != icons_.end()) {
        it->second.lastFrame = frame;
        return it->second.failed ? nullptr : &it->second.tex;
    }
    if (icons_.size() >= kMaxIcons) {  // evict the oldest not drawn in the last two frames
        auto victim = icons_.end();
        for (auto i = icons_.begin(); i != icons_.end(); ++i)
            if (frame - i->second.lastFrame > 2 && (victim == icons_.end() || i->second.lastFrame < victim->second.lastFrame))
                victim = i;
        if (victim == icons_.end()) return nullptr;  // all in use: skip this frame
        textureRelease(victim->second.tex);
        icons_.erase(victim);
    }
    Icon& ic = icons_[slot];
    ic.lastFrame = frame;
    void* block = memalign(64, iconStride_);
    if (!block || !readRaw(iconsOff_ + slot * iconStride_, block, iconStride_)) {
        std::free(block);
        ic.failed = true;
        return nullptr;
    }
    sceKernelDcacheWritebackRange(block, iconStride_);
    if (!textureFromBlob(block, iconStride_, ic.tex, true)) {
        std::free(block);
        ic.failed = true;
        return nullptr;
    }
    ++iconsLoaded_;
    return &ic.tex;
}

}  // namespace wowee::platform::psp
