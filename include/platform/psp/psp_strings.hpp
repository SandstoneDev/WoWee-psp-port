// The interface's strings (docs/PSP_PORT_ARCHITECTURE.md).
//
// Two kinds, and no client text in the EBOOT:
//   - CLIENT strings: the client's own (GlobalStrings.lua, GlueStrings.lua as GLUE_<KEY>, DBC names as DBC_*),
//     read from the running language's data/strings.pak (fsPath: data/locale/<xxYY>/strings.pak when that language
//     has one) - tools/psp/build_strings_pak.py, baked by Kiln from the player's client. The EBOOT names the keys it
//     uses (strings/cs_*.inc); at boot only their texts are read (a few KB to ~100 KB; the pak's other ~9000 never
//     reach RAM). Missing in the running language's pak -> the primary's (data/strings.pak) -> the key itself (logged
//     once at boot: "strings_missing").
//   - OWN strings: the port's (Wi-Fi, PSP keyboard, test aids ...): en + ru compiled in (strings/ps_*.inc), ru for
//     ruRU, en for every other language.
// Lookups return pointers that live for the whole run (no copy, no allocation): CLIENT_STR(GLUE_LOGIN), OWN_STR(ID).
// Forms: CLIENT_FMT(KEY, args...) / OWN_FMT(ID, args...) -> std::string (psp_text_format.hpp textf: %s %d %1$s ...).
// Adding a key: one line in the area's own .inc (see the port's docs) - the enum and the key's name come
// from that line; tools/psp/check_strings_keys.py tells whether a baked strings.pak has every CLIENT key.
#pragma once

#include "platform/psp/psp_text_format.hpp"

#include <cstdint>
#include <string>

namespace wowee::platform::psp {

enum class CS : uint16_t {
#define WOWEE_CS(key) key,
#include "platform/psp/strings/cs_list.inc"
#undef WOWEE_CS
    kCount
};

enum class PS : uint16_t {
#define WOWEE_PS(id, en, ru) id,
#include "platform/psp/strings/ps_list.inc"
#undef WOWEE_PS
    kCount
};

// Once at boot, after localeInit() (the running language's paks) - main_psp.cpp.
bool stringsInit();

const char* cs(CS id);      // the client's text (never null)
const char* csKey(CS id);   // its key ("GLUE_LOGIN")
const char* ps(PS id);      // the port's own text (never null)
bool csMissing(CS id);      // no pak had it (cs() gives the key)
uint32_t stringsResidentBytes();

template <typename... A>
std::string csf(CS id, const A&... args) {
    return text::textf(cs(id), args...);
}
template <typename... A>
std::string psf(PS id, const A&... args) {
    return text::textf(ps(id), args...);
}

}  // namespace wowee::platform::psp

#define CLIENT_STR(key) (::wowee::platform::psp::cs(::wowee::platform::psp::CS::key))
#define CLIENT_FMT(key, ...) (::wowee::platform::psp::csf(::wowee::platform::psp::CS::key, __VA_ARGS__))
#define OWN_STR(id) (::wowee::platform::psp::ps(::wowee::platform::psp::PS::id))
#define OWN_FMT(id, ...) (::wowee::platform::psp::psf(::wowee::platform::psp::PS::id, __VA_ARGS__))
