#include "platform/psp/psp_osk.hpp"

#include "platform/psp/psp_log.hpp"

#include <psputility.h>
#include <psputility_osk.h>
#include <psputility_sysparam.h>
#include <pspsysmem.h>

#include <cstring>

namespace wowee::platform::psp {

namespace {

// 512 (was 128) - a letter's body takes 500 characters (SendMailBodyEditBox letters="500");
// the other callers pass their own smaller limits.
constexpr int kMaxText = 512;

// The dialog reads these while it runs: they must outlive oskBegin().
SceUtilityOskParams g_params;
SceUtilityOskData g_data;
uint16_t g_desc[kMaxText];
uint16_t g_in[kMaxText];
uint16_t g_out[kMaxText];

bool g_active = false;
bool g_shutdownSent = false;
bool g_finished = false;
bool g_accepted = false;
std::string g_result;
int g_owner = 0;  // 0 = the plain oskBegin / oskFinished

// UTF-8 -> UTF-16 (BMP only, which covers Latin and Cyrillic).
void toUtf16(const std::string& s, uint16_t* out, int cap) {
    int n = 0;
    const auto* p = reinterpret_cast<const unsigned char*>(s.c_str());
    while (*p && n < cap - 1) {
        uint32_t c = *p++;
        if (c >= 0xC0 && c < 0xE0 && (*p & 0xC0) == 0x80) {
            c = ((c & 0x1F) << 6) | (*p++ & 0x3F);
        } else if (c >= 0xE0 && c < 0xF0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80) {
            c = ((c & 0x0F) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F);
            p += 2;
        } else if (c >= 0x80) {
            c = '?';
        }
        out[n++] = static_cast<uint16_t>(c);
    }
    out[n] = 0;
}

std::string toUtf8(const uint16_t* s) {
    std::string r;
    for (; *s; ++s) {
        uint32_t c = *s;
        if (c < 0x80) {
            r += static_cast<char>(c);
        } else if (c < 0x800) {
            r += static_cast<char>(0xC0 | (c >> 6));
            r += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            r += static_cast<char>(0xE0 | (c >> 12));
            r += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            r += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return r;
}

} // namespace

bool oskBegin(const std::string& title, const std::string& initial, int maxChars, OskCharset charset) {
    if (g_active) return false;
    // Only start from NONE: a dialog left mid-shutdown answers 0x80110001 and
    // keeps doing so until the console reboots (seen on hardware).
    const int status = sceUtilityOskGetStatus();
    if (status > 0) {
        PSP_LOGW(tag::Sys, "osk_busy status=%d", status);
        logFlush();
        return false;
    }
    if (maxChars < 1) maxChars = 1;
    if (maxChars > kMaxText - 1) maxChars = kMaxText - 1;

    toUtf16(title, g_desc, kMaxText);
    toUtf16(initial, g_in, kMaxText);
    std::memset(g_out, 0, sizeof(g_out));

    std::memset(&g_data, 0, sizeof(g_data));
    switch (charset) {
        case OskCharset::LatinLetters:
            g_data.language = PSP_UTILITY_OSK_LANGUAGE_ENGLISH;
            g_data.inputtype = PSP_UTILITY_OSK_INPUTTYPE_LATIN_LOWERCASE | PSP_UTILITY_OSK_INPUTTYPE_LATIN_UPPERCASE;
            break;
        case OskCharset::Cyrillic:
            g_data.language = PSP_UTILITY_OSK_LANGUAGE_RUSSIAN;
            g_data.inputtype = PSP_UTILITY_OSK_INPUTTYPE_RUSSIAN_LOWERCASE | PSP_UTILITY_OSK_INPUTTYPE_RUSSIAN_UPPERCASE;
            break;
        case OskCharset::Digits:  // the money boxes
            g_data.language = PSP_UTILITY_OSK_LANGUAGE_ENGLISH;
            g_data.inputtype = PSP_UTILITY_OSK_INPUTTYPE_LATIN_DIGIT;
            break;
        case OskCharset::Text:
            // chat - a chat line needs '/', digits and punctuation
            // too (Utility Overview p.40-41: SELECT cycles the input types).
            g_data.language = PSP_UTILITY_OSK_LANGUAGE_RUSSIAN;
            g_data.inputtype = PSP_UTILITY_OSK_INPUTTYPE_RUSSIAN_LOWERCASE | PSP_UTILITY_OSK_INPUTTYPE_RUSSIAN_UPPERCASE |
                               PSP_UTILITY_OSK_INPUTTYPE_LATIN_LOWERCASE | PSP_UTILITY_OSK_INPUTTYPE_LATIN_UPPERCASE |
                               PSP_UTILITY_OSK_INPUTTYPE_LATIN_DIGIT | PSP_UTILITY_OSK_INPUTTYPE_LATIN_SYMBOL;
            break;
        default:
            // Russian keyboard with Latin available on SELECT (Sony Utility
            // Overview p.40-41); letters only, the way the game's character names are.
            g_data.language = PSP_UTILITY_OSK_LANGUAGE_RUSSIAN;
            g_data.inputtype = PSP_UTILITY_OSK_INPUTTYPE_RUSSIAN_LOWERCASE | PSP_UTILITY_OSK_INPUTTYPE_RUSSIAN_UPPERCASE |
                               PSP_UTILITY_OSK_INPUTTYPE_LATIN_LOWERCASE | PSP_UTILITY_OSK_INPUTTYPE_LATIN_UPPERCASE;
            break;
    }
    g_data.lines = 1;
    g_data.unk_24 = 1;
    g_data.desc = g_desc;
    g_data.intext = g_in;
    g_data.outtextlength = kMaxText;
    g_data.outtextlimit = maxChars;
    g_data.outtext = g_out;

    std::memset(&g_params, 0, sizeof(g_params));
    g_params.base.size = sizeof(g_params);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &g_params.base.language);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_BUTTON_SWAP, &g_params.base.buttonSwap);
    // Dialog threads (pspsdk sample values), all below our main thread (0x20).
    g_params.base.graphicsThread = 17;
    g_params.base.accessThread = 19;
    g_params.base.fontThread = 18;
    g_params.base.soundThread = 16;
    g_params.datacount = 1;
    g_params.data = &g_data;

    // The dialog allocates from the user partition AFTER InitStart returns 0;
    // short memory shows up only as a dialog stuck in INIT. Record the headroom.
    const unsigned freeKb = static_cast<unsigned>(sceKernelTotalFreeMemSize() / 1024);
    const unsigned blockKb = static_cast<unsigned>(sceKernelMaxFreeMemSize() / 1024);
    int r = sceUtilityOskInitStart(&g_params);
    PSP_LOGW(tag::Sys, "osk_init r=%08X max=%d charset=%d part_free_kb=%u part_block_kb=%u",
             static_cast<unsigned>(r), maxChars, static_cast<int>(charset), freeKb, blockKb);
    logFlush();
    if (r < 0) return false;
    g_active = true;
    g_shutdownSent = false;
    g_finished = false;
    g_accepted = false;
    g_result.clear();
    g_owner = 0;
    return true;
}

bool oskActive() { return g_active; }

void oskAfterGeSync() {
    if (!g_active) return;
    // pspdev's names are shifted against Sony's (Utility-Reference p.11-15):
    //   2 VISIBLE  = Sony RUNNING  -> Update(1) every frame (1 = 60 fps)
    //   3 QUIT     = Sony FINISHED -> ShutdownStart(), exactly once
    //   4 FINISHED = Sony SHUTDOWN -> still closing, keep pumping
    //   0 NONE                     -> closed: read the result
    // INIT may be skipped (NONE -> RUNNING); NONE before shutdown just means
    // the dialog has not come up yet.
    const int st = sceUtilityOskGetStatus();
    switch (st) {
        case 2:
            sceUtilityOskUpdate(1);
            break;
        case 3:
            if (!g_shutdownSent) {
                g_shutdownSent = true;
                sceUtilityOskShutdownStart();
            }
            break;
        case 0:
            if (!g_shutdownSent) break;
            g_active = false;
            g_finished = true;
            // base.result: 0 OK, 1 cancelled, 2 aborted. d.result UNCHANGED (0)
            // with text in the buffer is a valid "Enter without edits".
            g_accepted = g_params.base.result == 0 && g_data.result != PSP_UTILITY_OSK_RESULT_CANCELLED;
            g_result = toUtf8(g_out);
            PSP_LOGW(tag::Sys, "osk_done base=%d result=%d len=%u", g_params.base.result, g_data.result,
                     static_cast<unsigned>(g_result.size()));
            logFlush();
            break;
        default:
            break;  // 1 INIT / 4 closing: keep pumping every frame
    }
}

bool oskFinished(bool& accepted, std::string& textUtf8) {
    if (!g_finished || g_owner != 0) return false;  // an owned session is its owner's
    g_finished = false;
    accepted = g_accepted;
    textUtf8 = g_result;
    return true;
}

// the same dialog, its text kept for `owner`.
bool oskBeginFor(int owner, const std::string& title, const std::string& initial, int maxChars, OskCharset charset) {
    if (!oskBegin(title, initial, maxChars, charset)) return false;
    g_owner = owner;
    return true;
}

bool oskFinishedFor(int owner, bool& accepted, std::string& textUtf8) {
    if (!g_finished || g_owner != owner) return false;
    g_finished = false;
    g_owner = 0;
    accepted = g_accepted;
    textUtf8 = g_result;
    return true;
}

} // namespace wowee::platform::psp
