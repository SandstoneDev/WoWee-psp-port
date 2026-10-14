// Tutorials - data/tutorial.pak reader (tools/psp/build_tutorial_pak.py,
// layout in psp_tutorial_format.hpp).
//
// PSP (card open-file limit): the pak is never kept open. open()
// reads the records, elements and strings (~20-40 KB) into RAM at world entry
// and closes it; art is read on demand by loadTextures() - one open, the
// entries, close - when a tutorial window is about to show.
#pragma once

#include "platform/psp/psp_texture.hpp"
#include "platform/psp/psp_tutorial_format.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace wowee::platform::psp {

class TutorialDb {
public:
    bool open();  // logs tutorial_pak ok= / tutorial_pak_missing
    bool loaded() const { return !tables_.recs.empty(); }
    const TutorialTables& tables() const { return tables_; }
    const TutorialRec* find(uint32_t id) const { return tables_.find(id); }
    // Ids that have a text, ascending (the "completed tutorials" walk).
    const std::vector<uint32_t>& ids() const { return ids_; }
    float scale() const { return tables_.header.scalePct * 0.01f; }
    // Reads the named WTEX entries into out[i] (textureRelease them later);
    // a missing entry leaves out[i].pixels null. Returns how many loaded.
    int loadTextures(const char* const* names, Texture* out, int n);

private:
    std::string path_;
    TutorialTables tables_;
    std::vector<uint32_t> ids_;
};

} // namespace wowee::platform::psp
