// Chat. Read-only view over the "emotetext" blob of characters.pak
// (format: psp_model_format.hpp, EmoteTextHeader). Header-only and free of
// platform headers: the PSP chat (src/ui/psp/ui_world_chat.cpp) and
// psp_char_conv --verify read the blob with the same code.
//
// What it answers is what upstream EmoteRegistry answers from the DBCs:
// command -> EmotesText id (getEmoteDbcId), id -> the four texts
// (textFor / textByDbcId).
#pragma once

#include "rendering/psp/psp_model_format.hpp"

#include <cstdint>
#include <cstring>
#include <string>

namespace wowee::rendering::psp {

// Lower-case key of a typed command: no leading '/', ASCII and Cyrillic
// (U+0410..U+042F, U+0401) folded to lower case, everything else kept. The
// converter stores its aliases through the same function.
inline std::string emoteCommandKey(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size() && in[i] == '/') ++i;
    for (; i < in.size(); ++i) {
        const auto c = static_cast<unsigned char>(in[i]);
        if (c >= 'A' && c <= 'Z') {
            out.push_back(static_cast<char>(c + 32));
        } else if (c == 0xD0 && i + 1 < in.size()) {
            const auto d = static_cast<unsigned char>(in[i + 1]);
            if (d >= 0x90 && d <= 0x9F) {          // U+0410..U+041F -> U+0430..U+043F
                out.push_back(static_cast<char>(0xD0));
                out.push_back(static_cast<char>(d + 0x20));
            } else if (d >= 0xA0 && d <= 0xAF) {   // U+0420..U+042F -> U+0440..U+044F
                out.push_back(static_cast<char>(0xD1));
                out.push_back(static_cast<char>(d - 0x20));
            } else if (d == 0x81) {                // U+0401 -> U+0451
                out.push_back(static_cast<char>(0xD1));
                out.push_back(static_cast<char>(0x91));
            } else {
                out.push_back(static_cast<char>(c));
                out.push_back(static_cast<char>(d));
            }
            ++i;
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

class EmoteTextView {
public:
    // Checks the blob (magic, sizes, bounds, pool terminated, aliases
    // sorted); on failure the view stays empty and *why says what broke.
    bool attach(const void* blob, uint32_t size, const char** why = nullptr) {
        *this = EmoteTextView();
        auto fail = [&](const char* w) {
            if (why) *why = w;
            *this = EmoteTextView();
            return false;
        };
        if (!blob || size < sizeof(EmoteTextHeader)) return fail("short");
        const auto* h = static_cast<const EmoteTextHeader*>(blob);
        if (std::memcmp(h->magic, "WETX", 4) != 0) return fail("magic");
        if (h->version != 1 || h->slots != kEtSlotCount) return fail("version");
        const uint64_t need = sizeof(EmoteTextHeader) + uint64_t{h->rowCount} * sizeof(EmoteTextRow) +
                              uint64_t{h->aliasCount} * sizeof(EmoteTextAlias) + h->poolSize;
        if (need > size) return fail("size");
        const auto* base = static_cast<const uint8_t*>(blob);
        rows_ = reinterpret_cast<const EmoteTextRow*>(base + sizeof(EmoteTextHeader));
        aliases_ = reinterpret_cast<const EmoteTextAlias*>(rows_ + h->rowCount);
        pool_ = reinterpret_cast<const char*>(aliases_ + h->aliasCount);
        rowCount_ = h->rowCount;
        aliasCount_ = h->aliasCount;
        poolSize_ = h->poolSize;
        if (poolSize_ == 0 || pool_[poolSize_ - 1] != '\0') return fail("pool");
        for (uint32_t i = 0; i < rowCount_; ++i) {
            if (i && rows_[i].textEmoteId <= rows_[i - 1].textEmoteId) return fail("rows_unsorted");
            for (uint32_t s = 0; s < kEtSlotCount; ++s)
                if (rows_[i].text[s] != kEmoteTextNone && rows_[i].text[s] >= poolSize_) return fail("text_offset");
        }
        for (uint32_t i = 0; i < aliasCount_; ++i) {
            if (aliases_[i].command >= poolSize_ || aliases_[i].row >= rowCount_) return fail("alias");
            if (i && std::strcmp(pool_ + aliases_[i - 1].command, pool_ + aliases_[i].command) >= 0)
                return fail("aliases_unsorted");
        }
        return true;
    }

    bool empty() const { return rowCount_ == 0; }
    uint32_t rowCount() const { return rowCount_; }
    uint32_t aliasCount() const { return aliasCount_; }
    uint32_t poolSize() const { return poolSize_; }
    const EmoteTextRow& row(uint32_t i) const { return rows_[i]; }
    const EmoteTextAlias& alias(uint32_t i) const { return aliases_[i]; }
    // A pool string, or nullptr for kEmoteTextNone.
    const char* str(uint32_t offset) const {
        return offset == kEmoteTextNone || offset >= poolSize_ ? nullptr : pool_ + offset;
    }
    const char* text(uint32_t rowIndex, EmoteTextSlot slot) const { return str(rows_[rowIndex].text[slot]); }

    // Row of a lower-case command key (emoteCommandKey), or -1.
    int findCommand(const std::string& key) const {
        int lo = 0, hi = static_cast<int>(aliasCount_) - 1;
        while (lo <= hi) {
            const int mid = (lo + hi) / 2;
            const int c = std::strcmp(pool_ + aliases_[mid].command, key.c_str());
            if (c == 0) return aliases_[mid].row;
            if (c < 0) lo = mid + 1;
            else hi = mid - 1;
        }
        return -1;
    }
    // Row of an EmotesText id, or -1.
    int findId(uint32_t textEmoteId) const {
        int lo = 0, hi = static_cast<int>(rowCount_) - 1;
        while (lo <= hi) {
            const int mid = (lo + hi) / 2;
            const uint32_t id = rows_[mid].textEmoteId;
            if (id == textEmoteId) return mid;
            if (id < textEmoteId) lo = mid + 1;
            else hi = mid - 1;
        }
        return -1;
    }

private:
    const EmoteTextRow* rows_ = nullptr;
    const EmoteTextAlias* aliases_ = nullptr;
    const char* pool_ = nullptr;
    uint32_t rowCount_ = 0, aliasCount_ = 0, poolSize_ = 0;
};

// upstream EmoteRegistry replacePlaceholders: every "%s" -> *target ("" when
// there is none).
inline std::string emoteReplaceAll(const char* text, const std::string* target) {
    std::string out;
    for (const char* p = text; *p; ++p) {
        if (p[0] == '%' && p[1] == 's') {
            if (target) out += *target;
            ++p;
        } else {
            out.push_back(*p);
        }
    }
    return out;
}

// upstream EmoteRegistry::textByDbcId with a target: the first "%s" is the
// sender, the rest the target.
inline std::string emoteReplaceSenderTarget(const char* text, const std::string& sender, const std::string& target) {
    std::string out;
    bool first = true;
    for (const char* p = text; *p; ++p) {
        if (p[0] == '%' && p[1] == 's') {
            out += first ? sender : target;
            first = false;
            ++p;
        } else {
            out.push_back(*p);
        }
    }
    return out;
}

} // namespace wowee::rendering::psp
