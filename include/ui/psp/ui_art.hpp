// Memory diet (docs/PSP_PORT_ARCHITECTURE.md): window art from data/ui.pak with a failure trail.
//
// PSP: a window that loads its art when it opens (merchant, bags, quest log, character, loot, tooltips, the item icons)
// used to fall back to its plain variant without a word when ui.pak could not be opened (its TOC needs ~15 KB of heap
// and a descriptor) or a texture had no memory - hardware hw4: after the NPC prefetch took the heap, the merchant and
// the bags drew plain for the whole session and the log had no merchant_art / bag_art line at all. UiArtPak opens the
// pak and loads textures; every failure memory or a descriptor caused logs
//   ui_art_fail win=<window> name=<entry | ui.pak> heap_kb= largest_kb= free_kb=
// (20 lines a session at most) and failed() tells the window to try again at the next window open (UiArtRetry). An
// entry missing from the pak (an older bake) is no failure - there is nothing to retry. The original client has no such
// path: its art is always there (a 2-4 GB address space, textures streamed by the engine).
#pragma once

#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_texture.hpp"

#include <cstdint>

namespace wowee::ui::psp {

class UiArtPak {
public:
    // Opens data/ui.pak (a failure is logged with `win`).
    explicit UiArtPak(const char* win);
    ~UiArtPak() { close(); }
    UiArtPak(const UiArtPak&) = delete;
    UiArtPak& operator=(const UiArtPak&) = delete;

    bool isOpen() const { return open_; }
    platform::psp::Pak& pak() { return pak_; }
    // Loads `name` into t (a texture already there stays: a retry loads only what is missing). False: missing from
    // the pak (no failure) or failed (logged, failed() set). bytes (optional): += the entry's size when loaded.
    bool load(const char* name, platform::psp::Texture& t, bool owns = true, uint32_t* bytes = nullptr);
    // The pak did not open or a present entry could not be loaded: the window should try again at the next open.
    bool failed() const { return failed_; }
    void close();

    // A failure seen outside UiArtPak (another pak, a blob of its own): the same log line.
    static void logFail(const char* win, const char* name);

private:
    platform::psp::Pak pak_;
    const char* win_;
    bool open_ = false;
    bool failed_ = false;
};

// The window-open serial: WorldScreen::openWindow bumps it, a loader that failed tries again once it changed.
uint32_t uiArtOpenSerial();
void uiArtNoteWindowOpen();

struct UiArtRetry {
    uint32_t serial = 0;
    bool pending = false;
    void fail() {
        pending = true;
        serial = uiArtOpenSerial();
    }
    // True once per failure, at the first call after a window opened since it: the loader clears its "tried" flag and
    // loads again (only what is missing).
    bool due() {
        if (!pending || serial == uiArtOpenSerial()) return false;
        pending = false;
        return true;
    }
};

}  // namespace wowee::ui::psp
