// Window art from data/ui.pak with a failure trail - see
// include/ui/psp/ui_art.hpp.
#include "ui/psp/ui_art.hpp"

#include "platform/psp/psp_bigheap.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {
uint32_t g_openSerial = 1;
uint32_t g_failLogs = 0;
}  // namespace

uint32_t uiArtOpenSerial() { return g_openSerial; }
void uiArtNoteWindowOpen() { ++g_openSerial; }

void UiArtPak::logFail(const char* win, const char* name) {
    if (++g_failLogs > 20) return;
    PSP_LOGW(pp::tag::Mem, "ui_art_fail win=%s name=%s heap_kb=%u largest_kb=%u free_kb=%u", win ? win : "?",
             name ? name : "?", static_cast<unsigned>(pp::heapUsedKb()), static_cast<unsigned>(pp::heapLargestKb()),
             static_cast<unsigned>(pp::heapFreeKb()));
}

UiArtPak::UiArtPak(const char* win) : win_(win) {
    char path[256];
    pp::fsPath(path, sizeof(path), "data/ui.pak");
    open_ = pak_.open(path);
    if (!open_) {
        failed_ = true;
        logFail(win_, "ui.pak");
    }
}

void UiArtPak::close() {
    if (open_) pak_.close();
    open_ = false;
}

bool UiArtPak::load(const char* name, pp::Texture& t, bool owns, uint32_t* bytes) {
    if (t.pixels) return true;  // a retry: already there
    if (!open_) return false;
    const pp::PakEntry* e = pak_.find(name);
    if (!e) return false;  // not in this bake: nothing to retry
    void* blob = pak_.load(e);
    if (!blob) {
        failed_ = true;
        logFail(win_, name);
        return false;
    }
    if (!pp::textureFromBlob(blob, e->size, t, owns)) {
        pp::pakFree(blob);
        failed_ = true;
        logFail(win_, name);
        return false;
    }
    if (bytes) *bytes += e->size;
    return true;
}

}  // namespace wowee::ui::psp
