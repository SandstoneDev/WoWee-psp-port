// tutorials - data/tutorial.pak reader, see psp_tutorial_db.hpp.
#include "platform/psp/psp_tutorial_db.hpp"

#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"

#include <pspkernel.h>

namespace wowee::platform::psp {

bool TutorialDb::open() {
    char path[256];
    fsPath(path, sizeof(path), "data/tutorial.pak");
    path_ = path;
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    Pak pak;
    if (!pak.open(path)) {
        PSP_LOGW(tag::Fs, "tutorial_pak_missing (tutorial windows off)");
        return false;
    }
    const PakEntry* eh = pak.find("thdr");
    const PakEntry* es = pak.find("tstr");
    const PakEntry* eu = pak.find("tui");
    void* h = eh ? pak.load(eh) : nullptr;
    void* s = es ? pak.load(es) : nullptr;
    void* u = eu ? pak.load(eu) : nullptr;
    std::string err;
    const bool ok = h && s &&
                    parseTutorialTables(h, eh->size, s, es->size, u, u ? eu->size : 0u, tables_, &err);
    pakFree(h);
    pakFree(s);
    pakFree(u);
    pak.close();
    if (!ok) {
        tables_ = TutorialTables{};
        PSP_LOGE(tag::Fs, "tutorial_pak bad err=%s", err.empty() ? "entries" : err.c_str());
        return false;
    }
    ids_.clear();
    for (const TutorialRec& r : tables_.recs)
        if (r.textOff != kTutNoString) ids_.push_back(r.id);
    PSP_LOGI(tag::Fs, "tutorial_pak ok=1 n=%u texts=%u images=%u scale=%u kb=%u ms=%u",
             static_cast<unsigned>(tables_.recs.size()), static_cast<unsigned>(ids_.size()),
             static_cast<unsigned>(tables_.header.imageCount), static_cast<unsigned>(tables_.header.scalePct),
             static_cast<unsigned>((tables_.strings.size() + tables_.recs.size() * sizeof(TutorialRec) +
                                    tables_.elems.size() * sizeof(TutorialElem)) / 1024u),
             static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u));
    return true;
}

int TutorialDb::loadTextures(const char* const* names, Texture* out, int n) {
    if (path_.empty() || n <= 0) return 0;
    Pak pak;
    if (!pak.open(path_.c_str())) return 0;
    int loaded = 0;
    for (int i = 0; i < n; ++i) {
        if (!names[i] || out[i].pixels) continue;
        const PakEntry* e = pak.find(names[i]);
        void* blob = e ? pak.load(e) : nullptr;
        if (!blob) continue;
        if (!textureFromBlob(blob, e->size, out[i], true)) {
            pakFree(blob);
            continue;
        }
        out[i].name = "tut";
        ++loaded;
    }
    pak.close();
    return loaded;
}

} // namespace wowee::platform::psp
