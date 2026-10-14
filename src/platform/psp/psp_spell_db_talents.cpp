// the talent and skill line part of data/spells.pak (SpellDb), see
// include/platform/psp/psp_talent_format.hpp and tools/psp/build_spells_pak.py.
//
// "skills" (SkillLine.dbc, ~10 KB) is read with the header at world entry and
// kept: the spell book and trainer tabs ask for it every rebuild. "talents"
// (all classes, ~50 KB) is only located; the session reads it once, keeps
// the player's class (~5 KB) and drops the rest (upstream loadTalentDbc
// keeps every class - PSP: RAM).
#include "platform/psp/psp_spell_db.hpp"

#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"

#include <algorithm>
#include <cstring>

namespace wowee::platform::psp {

void SpellDb::openExtras(Pak& pak) {
    skills_.clear();
    talentsOff_ = talentsSize_ = 0;
    generalIconId_ = 0;
    if (const PakEntry* e = pak.find("skills"); e && e->size >= sizeof(SkillPakHeader)) {
        if (void* blob = pak.load(e)) {
            const auto* p = static_cast<const uint8_t*>(blob);
            SkillPakHeader h;
            std::memcpy(&h, p, sizeof(h));
            const uint32_t recBytes = h.count * static_cast<uint32_t>(sizeof(SkillLineRec));
            if (std::memcmp(h.magic, "WSKL", 4) == 0 && h.version == 1 && h.recSize == sizeof(SkillLineRec) &&
                e->size >= sizeof(h) + recBytes) {
                const char* strings = reinterpret_cast<const char*>(p + sizeof(h) + recBytes);
                const uint32_t strBytes = e->size - static_cast<uint32_t>(sizeof(h)) - recBytes;
                skills_.reserve(h.count);
                for (uint32_t i = 0; i < h.count; ++i) {
                    SkillLineRec r;
                    std::memcpy(&r, p + sizeof(h) + i * sizeof(SkillLineRec), sizeof(r));
                    SkillLineInfo s;
                    s.id = r.id;
                    s.iconId = r.iconId;
                    s.category = r.category;
                    if (r.nameOff < strBytes) {
                        const char* b = strings + r.nameOff;
                        const void* z = std::memchr(b, 0, strBytes - r.nameOff);
                        s.name.assign(b, z ? static_cast<size_t>(static_cast<const char*>(z) - b) : strBytes - r.nameOff);
                    }
                    skills_.push_back(std::move(s));
                }
                std::sort(skills_.begin(), skills_.end(),
                          [](const SkillLineInfo& a, const SkillLineInfo& b) { return a.id < b.id; });
                generalIconId_ = h.generalIconId;
            } else {
                PSP_LOGE(tag::Fs, "spells_pak skills_bad ver=%u rec=%u", static_cast<unsigned>(h.version),
                         static_cast<unsigned>(h.recSize));
            }
            pakFree(blob);
        }
    }
    if (const PakEntry* e = pak.find("talents"); e && e->size >= sizeof(TalentPakHeader)) {
        talentsOff_ = e->offset;
        talentsSize_ = e->size;
    }
    PSP_LOGI(tag::Fs, "spells_pak skills=%u talents_kb=%u", static_cast<unsigned>(skills_.size()),
             static_cast<unsigned>(talentsSize_ / 1024u));
}

const SpellDb::SkillLineInfo* SpellDb::skillLine(uint32_t id) const {
    auto it = std::lower_bound(skills_.begin(), skills_.end(), id,
                               [](const SkillLineInfo& s, uint32_t v) { return s.id < v; });
    return it != skills_.end() && it->id == id ? &*it : nullptr;
}

bool SpellDb::readTalents(std::vector<uint8_t>& out) {
    out.clear();
    if (talentsSize_ == 0) return false;
    out.resize(talentsSize_ + 1, 0);  // + a terminating zero for the strings
    if (!readRaw(talentsOff_, out.data(), talentsSize_)) {
        PSP_LOGE(tag::Fs, "spells_pak read_fail talents");
        out.clear();
        return false;
    }
    return true;
}

const Texture* SpellDb::iconById(uint32_t spellIconId, uint32_t frame) {
    if (spellIconId >= iconSlots_.size()) return nullptr;
    return iconBySlot(iconSlots_[spellIconId], frame);
}

}  // namespace wowee::platform::psp
