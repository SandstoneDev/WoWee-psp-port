// The client's languages (docs/PSP_PORT_ARCHITECTURE.md). Pure functions shared by the PSP
// (psp_locale.cpp) and the host test (tools/psp/host_tests/locale_test.cpp):
//
//   - data/locales.txt, written by the PC converter (Kiln) next to the paks:
//         # Kiln locales v1
//         primary enUS        exactly one: the language baked into data/
//         extra ruRU          zero or more: data/locale/<xxYY>/ holds that language's own paks
//         default ruRU        the language a fresh card starts in
//     unknown lines are ignored (later versions may add keys); no file (an older card) = the primary only;
//   - which language runs: wowee.cfg locale=xxYY when it names the primary or an extra, else the file's
//     default, else the primary (the original keeps the choice in the CVar "locale", WTF\Config.wtf);
//   - a pak's file under data/locale/<xxYY>/ when that folder has one of that name (fsPath, psp_fs.cpp);
//   - the languages' own names (endonyms) for the selector, with an ASCII name when the font lacks a glyph;
//   - the few strings of the selector itself (the client's, from each language's strings.pak).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace wowee::platform::psp::locale {

// "enUS", "ruRU": two lower-case letters, two upper-case letters.
inline bool codeValid(const std::string& c) {
    return c.size() == 4 && c[0] >= 'a' && c[0] <= 'z' && c[1] >= 'a' && c[1] <= 'z' && c[2] >= 'A' && c[2] <= 'Z' &&
           c[3] >= 'A' && c[3] <= 'Z';
}

struct LocaleSet {
    std::string primary;              // data/
    std::vector<std::string> extras;  // data/locale/<code>/, in the file's order, no duplicates, never the primary
    std::string def;                  // "" when the file named none (or one not installed)
    bool fromFile = false;            // false: no usable locales.txt - the primary only
};

// The installed languages, the primary first.
inline std::vector<std::string> installed(const LocaleSet& s) {
    std::vector<std::string> v;
    if (!s.primary.empty()) v.push_back(s.primary);
    v.insert(v.end(), s.extras.begin(), s.extras.end());
    return v;
}

inline bool isInstalled(const LocaleSet& s, const std::string& code) {
    return !code.empty() && (code == s.primary || std::find(s.extras.begin(), s.extras.end(), code) != s.extras.end());
}

// Parses locales.txt. Returns false (out = the primary-only fallback with `fallbackPrimary`) when the text has no
// valid "primary" line. warn (optional) collects what was ignored, for the log.
inline bool parseList(const std::string& text, const std::string& fallbackPrimary, LocaleSet& out,
                      std::string* warn = nullptr) {
    out = LocaleSet{};
    std::string def;
    std::vector<std::string> extras;
    auto note = [&](const std::string& w) {
        if (warn) *warn += (warn->empty() ? "" : ",") + w;
    };
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF) line.erase(0, 3);  // UTF-8 BOM
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t a = 0;
        while (a < line.size() && (line[a] == ' ' || line[a] == '\t')) ++a;
        line.erase(0, a);
        if (line.empty() || line[0] == '#') continue;
        const size_t sp = line.find_first_of(" \t");
        if (sp == std::string::npos) continue;
        const std::string key = line.substr(0, sp);
        std::string val = line.substr(sp + 1);
        size_t b = 0;
        while (b < val.size() && (val[b] == ' ' || val[b] == '\t')) ++b;
        val.erase(0, b);
        if (key != "primary" && key != "extra" && key != "default") continue;  // a later version's key
        if (!codeValid(val)) {
            note("bad_" + key);
            continue;
        }
        if (key == "primary") {
            if (out.primary.empty()) out.primary = val;
            else if (out.primary != val) note("second_primary");
        } else if (key == "extra") {
            if (std::find(extras.begin(), extras.end(), val) == extras.end()) extras.push_back(val);
        } else if (def.empty()) {
            def = val;
        }
    }
    if (out.primary.empty()) {
        note("no_primary");
        out = LocaleSet{};
        out.primary = codeValid(fallbackPrimary) ? fallbackPrimary : "ruRU";
        return false;
    }
    for (const std::string& e : extras)
        if (e != out.primary) out.extras.push_back(e);
    out.fromFile = true;
    if (!def.empty() && isInstalled(out, def)) out.def = def;
    else if (!def.empty()) note("default_not_installed");
    return true;
}

// No locales.txt (a card baked before Kiln): the only language is the one the card has always run in - wowee.cfg
// locale (the logon challenge's locale until now), else ruRU.
inline LocaleSet primaryOnly(const std::string& cfgLocale) {
    LocaleSet s;
    s.primary = codeValid(cfgLocale) ? cfgLocale : "ruRU";
    return s;
}

enum class Source { Cfg, Default, Primary };
inline const char* sourceName(Source s) {
    return s == Source::Cfg ? "cfg" : s == Source::Default ? "default" : "primary";
}

// The language this run uses.
inline std::string choose(const LocaleSet& s, const std::string& cfgLocale, Source* src = nullptr) {
    if (isInstalled(s, cfgLocale)) {
        if (src) *src = Source::Cfg;
        return cfgLocale;
    }
    if (!s.def.empty() && isInstalled(s, s.def)) {
        if (src) *src = Source::Default;
        return s.def;
    }
    if (src) *src = Source::Primary;
    return s.primary;
}

// --- Pak overrides ---------------------------------------------------------------------------------------------

inline char lowerAscii(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

inline std::string lowerCopy(const std::string& s) {
    std::string o(s);
    for (char& c : o) c = lowerAscii(c);
    return o;
}

// The files of data/locale/<code>/ as "name" / "sub/name", lower case, sorted (FAT32 is case-blind).
inline void sortNames(std::vector<std::string>& names) {
    for (std::string& n : names) n = lowerCopy(n);
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
}

// rel = a path under the game folder ("data/text.pak"). Returns the index in `names` of the localized file that
// replaces it, or -1 (not under data/, not in the folder, or a path into data/locale/ itself).
inline int overrideIndex(const std::vector<std::string>& names, const char* rel) {
    if (!rel || names.empty()) return -1;
    if (std::strncmp(rel, "data/", 5) != 0 && std::strncmp(rel, "Data/", 5) != 0) return -1;
    const char* rest = rel + 5;
    if (!*rest) return -1;
    std::string key(rest);
    key = lowerCopy(key);
    if (key.compare(0, 7, "locale/") == 0) return -1;
    auto it = std::lower_bound(names.begin(), names.end(), key);
    if (it == names.end() || *it != key) return -1;
    return static_cast<int>(it - names.begin());
}

// --- Names ------------------------------------------------------------------------------------------------------

struct LocaleName {
    const char* code;
    const char* native;  // the language's own name (UTF-8)
    const char* ascii;   // when the font has no glyph for a letter of `native`
};

// The client locales of 3.3.5a (GetExistingLocales); enCN / enTW (English voice packs) are not baked by Kiln.
inline const LocaleName* findName(const std::string& code) {
    static const LocaleName kNames[] = {
        {"enUS", "English", "English (enUS)"},
        {"enGB", "English (UK)", "English (UK)"},
        {"deDE", "Deutsch", "German (deDE)"},
        {"frFR", "Français", "French (frFR)"},
        {"esES", "Español", "Spanish (esES)"},
        {"esMX", "Español (México)", "Spanish (esMX)"},
        {"ruRU", "Русский", "Russian (ruRU)"},
        {"koKR", "한국어", "Korean (koKR)"},
        {"zhCN", "简体中文", "Chinese (zhCN)"},
        {"zhTW", "繁體中文", "Chinese (zhTW)"},
    };
    for (const LocaleName& n : kNames)
        if (code == n.code) return &n;
    return nullptr;
}

// The selector's label of `code`: its own name when `drawable(native)` (every letter has a glyph), else the
// ASCII name; an unknown code shows as itself.
template <typename Drawable>
std::string displayName(const std::string& code, Drawable&& drawable) {
    const LocaleName* n = findName(code);
    if (!n) return code;
    return drawable(n->native) ? std::string(n->native) : std::string(n->ascii);
}

// --- The selector's own strings ---------------------------------------------------------------------------------

// The client's GlobalStrings.lua / GlueStrings.lua of each language, read from that language's
// strings.pak by the caller's lookup (ui_language.cpp: OPTION_TOOLTIP_LOCALE, OPTION_RESTART_REQUIREMENT, RESTART
// of the glue, OKAY, ACCEPT, BACK) - none is compiled in. The one exception is the word on the language button, the
// settings row and the picker's title: "Language" in English whatever runs (found by anyone).
enum class Str { Language, Tooltip, RestartRequired, Restart, Okay, Accept, Back };

// The compiled-in text: "Language" (English, every language); "" for the strings that come from strings.pak.
inline const char* text(Str id, const std::string& /*code*/) { return id == Str::Language ? "Language" : ""; }

// A string in `code`'s language - lookup(code, id) gives that language's text as a pointer that stays valid ("" when
// its strings.pak has none, never null) - or the running language's when `drawable` says the font cannot show it
// (or `code` has none).
template <typename Lookup, typename Drawable>
const char* textFor(Str id, const std::string& code, const std::string& running, Lookup&& lookup,
                    Drawable&& drawable) {
    if (id == Str::Language) return text(id, code);
    const char* t = lookup(code, id);
    if (code == running || (*t && drawable(t))) return t;
    return lookup(running, id);
}

// The restart notice after a language was picked: the new language's line first (the player chose a language they
// read), then the running one's when it differs.
template <typename Lookup, typename Drawable>
std::string restartNotice(const std::string& running, const std::string& picked, Lookup&& lookup,
                          Drawable&& drawable) {
    std::string s = textFor(Str::RestartRequired, picked, running, lookup, drawable);
    const std::string other = lookup(running, Str::RestartRequired);
    if (!other.empty() && other != s) s += (s.empty() ? "" : "\n") + other;
    return s;
}

}  // namespace wowee::platform::psp::locale
