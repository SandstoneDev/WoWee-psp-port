// Tutorials - the data/tutorial.pak layout and its pure parsing
// helpers (no PSP headers: the host test tools/psp/host_tests/tutorial_test.cpp
// compiles this file with plain g++).
//
// Baked by tools/psp/build_tutorial_pak.py from the client's own files:
//   Interface/FrameXML/GlobalStrings.lua (ruRU)  TUTORIAL<id>, TUTORIAL_TITLE<id>,
//                                                a few button labels
//   Interface/FrameXML/TutorialFrame.lua         TUTORIAL_DATA (id -> layout name),
//                                                DISPLAY_DATA (layouts)
//   Interface/TutorialFrame/*                    UI-TUTORIAL-FRAME pieces + images
//
// WPAK entries (psp_pak.hpp):
//   "thdr"  TutorialPakHeader, count x TutorialRec (by id), elemCount x TutorialElem
//   "tstr"  UTF-8 strings (titles, marked texts), offsets from TutorialRec
//   "tui"   UI labels: key\0value\0 ... (PREV, NEXT, CLOSE, ...)
//   "f_top" "f_bottom" "f_left" "f_right" "f_key" "f_alert" "f_alert_hl"
//   "f_arrow_u" "f_arrow_d" "f_arrow_l" "f_arrow_r"   frame art (WTEX), scaled
//   "timg_<n>"  the layouts' images (WTEX), scaled by scalePct
// Offsets and sizes in TutorialRec / TutorialElem are the ORIGINAL frame units
// (DISPLAY_DATA, 1024x768 UI); the PSP multiplies them by scalePct / 100,
// the scale the art was baked at.
//
// Marked text: the converter splits TUTORIAL<id> into sentences, each led by
// a marker byte; kTutKeySentence marks a sentence about keyboard keys or the
// mouse (the converter's KEY_WORDS). The PSP swaps those for a line with PSP
// button glyphs (ui_tutorial_controls.hpp) - assembleTutorialText.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace wowee::platform::psp {

struct TutorialPakHeader {
    char magic[4];       // "WTUT"
    uint32_t version;    // 1
    uint32_t count;      // records
    uint32_t maxId;
    uint32_t scalePct;   // art scale (PSP px per original unit x 100)
    uint32_t elemCount;
    uint32_t imageCount; // "timg_<n>" entries
    uint32_t reserved;
};
static_assert(sizeof(TutorialPakHeader) == 32, "tutorial.pak header");

enum TutorialAlign : uint8_t {  // the client's anchor points
    kTutTopLeft, kTutTop, kTutTopRight, kTutLeft, kTutCenter, kTutRight, kTutBottomLeft, kTutBottom, kTutBottomRight
};

struct TutorialRec {
    uint16_t id;
    uint8_t tileHeight;      // DISPLAY_DATA tileHeight (middle pieces of 10)
    uint8_t anchor;          // TutorialAlign of anchorData.align
    int16_t anchorX, anchorY;
    int16_t textLeft, textTop, textRight, textBottom;  // textBox offsets
    uint32_t titleOff, textOff;  // into "tstr", 0xFFFFFFFF = none
    uint16_t firstElem, elemCount;
    uint8_t layout;          // index of the layout name (dump only)
    uint8_t flags;           // bit 0: has a key/mouse sentence
    uint16_t reserved;
};
static_assert(sizeof(TutorialRec) == 32, "tutorial.pak record");

enum TutorialElemType : uint8_t { kTutElemImage = 1, kTutElemMouse = 2, kTutElemKey = 3, kTutElemArrow = 4 };
// Key commands (keyDataN.command) the converter knows, TutorialElem::index.
enum TutorialKeyCommand : uint8_t {
    kTutKeyTurnLeft, kTutKeyMoveBackward, kTutKeyTurnRight, kTutKeyMoveForward, kTutKeyJump, kTutKeyOther = 0xFF
};
// Arrows: 0 up, 1 down, 2 left, 3 right (the curved ones are not baked).
struct TutorialElem {
    uint8_t type;      // TutorialElemType
    uint8_t index;     // image number / mouse image (0 left, 1 right, 2 both, 3 wheel) / key command / arrow
    uint8_t align;     // TutorialAlign
    uint8_t scalePct;  // arrows / mouse: the layout's scale x 100 (100 = none)
    int16_t x, y;      // offset from the frame's same point, original units, y up
};
static_assert(sizeof(TutorialElem) == 8, "tutorial.pak element");

constexpr char kTutSentence = '\x01';
constexpr char kTutKeySentence = '\x02';
constexpr char kTutParagraph = '\x03';
constexpr uint32_t kTutNoString = 0xFFFFFFFFu;

// Everything the PSP keeps in RAM: records, elements, the string blob.
struct TutorialTables {
    TutorialPakHeader header{};
    std::vector<TutorialRec> recs;   // sorted by id
    std::vector<TutorialElem> elems;
    std::vector<char> strings;       // "tstr" + a terminating 0
    std::vector<char> ui;            // "tui" + a terminating 0

    const TutorialRec* find(uint32_t id) const {
        size_t lo = 0, hi = recs.size();
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if (recs[mid].id < id) lo = mid + 1;
            else hi = mid;
        }
        return lo < recs.size() && recs[lo].id == id ? &recs[lo] : nullptr;
    }
    const char* str(uint32_t off) const {
        return off != kTutNoString && off < strings.size() ? strings.data() + off : nullptr;
    }
    const char* uiString(const char* key) const {
        const char* p = ui.data();
        const char* end = ui.data() + ui.size();
        while (p < end && *p) {
            const char* v = p + std::strlen(p) + 1;
            if (v >= end) break;
            if (std::strcmp(p, key) == 0) return v;
            p = v + std::strlen(v) + 1;
        }
        return nullptr;
    }
};

// Checks and copies "thdr" / "tstr" / "tui" (tui may be null). False + err on
// anything inconsistent (a bad pak is ignored, never half-used).
inline bool parseTutorialTables(const void* thdr, uint32_t thdrSize, const void* tstr, uint32_t tstrSize,
                                const void* tui, uint32_t tuiSize, TutorialTables& out, std::string* err) {
    auto fail = [&](const char* why) {
        if (err) *err = why;
        return false;
    };
    if (!thdr || thdrSize < sizeof(TutorialPakHeader)) return fail("thdr_short");
    std::memcpy(&out.header, thdr, sizeof(TutorialPakHeader));
    const TutorialPakHeader& h = out.header;
    if (std::memcmp(h.magic, "WTUT", 4) != 0) return fail("magic");
    if (h.version != 1) return fail("version");
    if (h.count > 1024 || h.elemCount > 8192 || h.scalePct == 0 || h.scalePct > 400) return fail("counts");
    const uint64_t need = sizeof(TutorialPakHeader) + uint64_t(h.count) * sizeof(TutorialRec) +
                          uint64_t(h.elemCount) * sizeof(TutorialElem);
    if (thdrSize < need) return fail("thdr_size");
    const auto* p = static_cast<const uint8_t*>(thdr) + sizeof(TutorialPakHeader);
    out.recs.resize(h.count);
    if (h.count) std::memcpy(out.recs.data(), p, h.count * sizeof(TutorialRec));
    p += h.count * sizeof(TutorialRec);
    out.elems.resize(h.elemCount);
    if (h.elemCount) std::memcpy(out.elems.data(), p, h.elemCount * sizeof(TutorialElem));
    out.strings.assign(static_cast<const char*>(tstr), static_cast<const char*>(tstr) + (tstr ? tstrSize : 0));
    out.strings.push_back('\0');
    out.ui.clear();
    if (tui && tuiSize) out.ui.assign(static_cast<const char*>(tui), static_cast<const char*>(tui) + tuiSize);
    out.ui.push_back('\0');
    for (size_t i = 0; i < out.recs.size(); ++i) {
        const TutorialRec& r = out.recs[i];
        if (i && out.recs[i - 1].id >= r.id) return fail("order");
        if (uint32_t(r.firstElem) + r.elemCount > h.elemCount) return fail("elems");
        if ((r.titleOff != kTutNoString && r.titleOff >= out.strings.size() - 1) ||
            (r.textOff != kTutNoString && r.textOff >= out.strings.size() - 1))
            return fail("string");
    }
    for (const TutorialElem& e : out.elems) {
        if (e.type < kTutElemImage || e.type > kTutElemArrow || e.align > kTutBottomRight) return fail("elem");
        if (e.type == kTutElemImage && e.index >= h.imageCount) return fail("image");
    }
    return true;
}

// The text shown for a marked TUTORIAL<id>: sentences joined by spaces,
// paragraphs by '\n'. With `pspLine` (the PSP controls line for this id) and
// `substitute`, key/mouse sentences are dropped and the line stands where the
// first of them was (appended as its own paragraph when the text had none).
// `keySentences` (optional) = how many key/mouse sentences the text has.
inline std::string assembleTutorialText(const char* marked, const char* pspLine, bool substitute,
                                        int* keySentences = nullptr) {
    std::string out;
    int keys = 0;
    bool inserted = false, needSpace = false;
    const bool swap = substitute && pspLine && *pspLine;
    auto put = [&](const char* s, size_t n) {
        if (needSpace && !out.empty() && out.back() != '\n') out += ' ';
        out.append(s, n);
        needSpace = true;
    };
    const char* p = marked ? marked : "";
    while (*p) {
        const char m = *p++;
        const char* s = p;
        while (*p && *p != kTutSentence && *p != kTutKeySentence && *p != kTutParagraph) ++p;
        const size_t n = static_cast<size_t>(p - s);
        if (m == kTutParagraph) {
            if (!out.empty() && out.back() != '\n') out += '\n';
            needSpace = false;
        } else if (m == kTutKeySentence) {
            ++keys;
            if (!swap) put(s, n);
            else if (!inserted) {
                put(pspLine, std::strlen(pspLine));
                inserted = true;
            }
        } else if (m == kTutSentence) {
            put(s, n);
        }  // anything else: not a marker, the bytes before the next one are dropped
    }
    if (swap && !inserted) {
        if (!out.empty() && out.back() != '\n') out += '\n';
        out += pspLine;
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    if (keySentences) *keySentences = keys;
    return out;
}

// Top-left of a w x h box whose `align` point sits at the frame's (W x H)
// same point plus (dx, dy) - PSP pixels, y down (the client's y up is negated by the
// caller). The client's SetPoint(align, frame, align, x, y).
inline void tutorialPlace(uint8_t align, float W, float H, float w, float h, float dx, float dy, float& x,
                          float& y) {
    const int col = align % 3, row = align / 3;  // 0 left/top, 1 centre, 2 right/bottom
    x = (col == 0 ? 0.0f : col == 1 ? (W - w) * 0.5f : W - w) + dx;
    y = (row == 0 ? 0.0f : row == 1 ? (H - h) * 0.5f : H - h) + dy;
}

} // namespace wowee::platform::psp
