// String formatting for the client's own strings (strings.pak, psp_strings.hpp) - pure, shared
// with the host test (tools/psp/host_tests/strings_test.cpp).
//
//   - textf(pattern, args...): the forms the client's Lua strings use (string.format + the client's positional "%n$"):
//     %s %d %i %u %x %X %c %f %g %e %%, flags "-+ #0", width, precision, "%2$s". A form's argument may be of
//     another kind (a number for %s, a string for %d): it is printed sensibly, never undefined; a missing argument
//     prints nothing; extra arguments are ignored. ruRU reorders arguments ("%2$s %1$d-...") where enUS does
//     not ("Level %d %s") - the caller passes them in the client's order and both come out right.
//   - uiPlain(text): the client's UI escapes the PSP text renderer does not draw - |cAARRGGBB ... |r (colour), |T..|t
//     (texture), |H..|h[..]|h (link: its text kept), "||" -> "|", |n -> newline.
//   - fnv1a64(key): the key hash of strings.pak (tools/psp/build_strings_pak.py).
//   - plural(text) / plural(text, n) / pluralForm(forms, n): the client's plural markup "|4one:many;" (enUS, two
//     forms) / "|4one:few:many;" (ruRU, three) - the one resolver of the port (it replaced ~13 local copies).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>

namespace wowee::platform::psp::text {

constexpr uint64_t fnv1a64(const char* s) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (; s && *s; ++s) h = (h ^ static_cast<uint8_t>(*s)) * 0x100000001B3ull;
    return h;
}

struct TextArg {
    enum Kind : uint8_t { Str, Int, UInt, Dbl } kind = Str;
    const char* s = "";
    long long i = 0;
    unsigned long long u = 0;
    double d = 0.0;

    TextArg(const char* v) : kind(Str), s(v ? v : "") {}  // NOLINT: implicit by design (textf's arguments)
    TextArg(const std::string& v) : kind(Str), s(v.c_str()) {}  // NOLINT
    template <typename T,
              typename std::enable_if<std::is_integral<T>::value && std::is_signed<T>::value, int>::type = 0>
    TextArg(T v) : kind(Int), i(static_cast<long long>(v)) {}  // NOLINT
    template <typename T,
              typename std::enable_if<std::is_integral<T>::value && !std::is_signed<T>::value, int>::type = 0>
    TextArg(T v) : kind(UInt), u(static_cast<unsigned long long>(v)) {}  // NOLINT
    TextArg(float v) : kind(Dbl), d(v) {}  // NOLINT
    TextArg(double v) : kind(Dbl), d(v) {}  // NOLINT
};

inline std::string textFormat(const char* fmt, const TextArg* args, size_t n) {
    std::string out;
    if (!fmt) return out;
    size_t next = 0;  // the next sequential argument
    char spec[32];
    char buf[96];
    for (const char* p = fmt; *p;) {
        if (*p != '%') {
            out += *p++;
            continue;
        }
        const char* start = p++;
        if (*p == '%') {
            out += '%';
            ++p;
            continue;
        }
        // %n$ - a positional argument (1-based)
        size_t argIndex = next;
        bool positional = false;
        {
            const char* q = p;
            unsigned v = 0;
            while (*q >= '0' && *q <= '9') v = v * 10 + static_cast<unsigned>(*q++ - '0');
            if (q != p && *q == '$' && v > 0) {
                argIndex = v - 1;
                positional = true;
                p = q + 1;
            }
        }
        // flags, width, precision (no '*'), length modifiers dropped
        size_t sl = 0;
        spec[sl++] = '%';
        while (*p && std::strchr("-+ #0", *p) && sl < 8) spec[sl++] = *p++;
        while (*p >= '0' && *p <= '9' && sl < 16) spec[sl++] = *p++;
        if (*p == '.') {
            spec[sl++] = *p++;
            while (*p >= '0' && *p <= '9' && sl < 24) spec[sl++] = *p++;
        }
        while (*p == 'l' || *p == 'h' || *p == 'z' || *p == 'j' || *p == 't' || *p == 'L' || *p == 'q') ++p;
        const char conv = *p;
        if (!conv || !std::strchr("sdiuxXcfgeEG", conv)) {  // not a form: copy it as written
            out.append(start, static_cast<size_t>(p - start));
            continue;
        }
        ++p;
        if (!positional) ++next;
        if (argIndex >= n) continue;  // a missing argument prints nothing
        const TextArg& a = args[argIndex];
        int len = 0;
        switch (conv) {
            case 's': {
                if (a.kind == TextArg::Str) {
                    spec[sl++] = 's';
                    spec[sl] = 0;
                    // a long string: no width or precision cuts it into a fixed buffer
                    if (sl == 2) {
                        out += a.s;
                        continue;
                    }
                    std::string tmp;
                    const int need = std::snprintf(nullptr, 0, spec, a.s);
                    if (need > 0) {
                        tmp.resize(static_cast<size_t>(need) + 1);
                        std::snprintf(&tmp[0], tmp.size(), spec, a.s);
                        tmp.resize(static_cast<size_t>(need));
                    }
                    out += tmp;
                    continue;
                }
                if (a.kind == TextArg::Dbl) {
                    len = std::snprintf(buf, sizeof(buf), "%g", a.d);
                } else if (a.kind == TextArg::Int) {
                    len = std::snprintf(buf, sizeof(buf), "%lld", a.i);
                } else {
                    len = std::snprintf(buf, sizeof(buf), "%llu", a.u);
                }
                break;
            }
            case 'c': {
                long long c = '?';
                if (a.kind == TextArg::Int) c = a.i;
                else if (a.kind == TextArg::UInt) c = static_cast<long long>(a.u);
                else if (a.kind == TextArg::Str && *a.s) c = *a.s;
                buf[0] = static_cast<char>(c);
                len = 1;
                break;
            }
            case 'f':
            case 'g':
            case 'e':
            case 'E':
            case 'G': {
                const double v = a.kind == TextArg::Dbl ? a.d : a.kind == TextArg::Int ? static_cast<double>(a.i)
                                 : a.kind == TextArg::UInt                              ? static_cast<double>(a.u)
                                                                                        : 0.0;
                spec[sl++] = conv;
                spec[sl] = 0;
                len = std::snprintf(buf, sizeof(buf), spec, v);
                break;
            }
            default: {  // d i u x X
                if (a.kind == TextArg::Str) {  // a string where a number was meant: the string
                    out += a.s;
                    continue;
                }
                spec[sl++] = 'l';
                spec[sl++] = 'l';
                spec[sl++] = conv == 'i' ? 'd' : conv;
                spec[sl] = 0;
                if (conv == 'u' && a.kind == TextArg::Int && a.i < 0) spec[sl - 1] = 'd';  // -1 for %u: "-1"
                if (conv == 'd' || conv == 'i' || spec[sl - 1] == 'd') {
                    const long long v = a.kind == TextArg::Int    ? a.i
                                        : a.kind == TextArg::UInt ? static_cast<long long>(a.u)
                                                                  : static_cast<long long>(a.d);
                    len = std::snprintf(buf, sizeof(buf), spec, v);
                } else {
                    // a negative int for %x: its 32-bit pattern (the codes the port prints are 32-bit)
                    const unsigned long long v =
                        a.kind == TextArg::UInt ? a.u
                        : a.kind == TextArg::Int
                            ? (a.i < 0 ? static_cast<unsigned long long>(static_cast<uint32_t>(a.i))
                                       : static_cast<unsigned long long>(a.i))
                            : static_cast<unsigned long long>(a.d);
                    len = std::snprintf(buf, sizeof(buf), spec, v);
                }
                break;
            }
        }
        if (len > 0) out.append(buf, static_cast<size_t>(len < static_cast<int>(sizeof(buf)) ? len : sizeof(buf) - 1));
    }
    return out;
}

inline std::string textf(const char* fmt) { return textFormat(fmt, nullptr, 0); }

template <typename... A>
std::string textf(const char* fmt, const A&... a) {
    const TextArg args[] = {TextArg(a)...};
    return textFormat(fmt, args, sizeof...(A));
}

// The client's UI escapes the PSP renderer does not draw (see the top).
inline std::string uiPlain(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c != '|' || i + 1 >= in.size()) {
            out += c;
            continue;
        }
        const char n = in[i + 1];
        if (n == '|') {
            out += '|';
            ++i;
        } else if (n == 'n') {
            out += '\n';
            ++i;
        } else if (n == 'r') {
            ++i;
        } else if (n == 'c' && i + 9 < in.size()) {
            i += 9;  // |cAARRGGBB
        } else if (n == 'T') {  // |T...|t
            const size_t e = in.find("|t", i + 2);
            i = e == std::string::npos ? in.size() : e + 1;
        } else if (n == 'H') {  // |H...|h[text]|h -> [text]
            const size_t e = in.find("|h", i + 2);
            if (e == std::string::npos) {
                i = in.size();
            } else {
                const size_t e2 = in.find("|h", e + 2);
                out += in.substr(e + 2, (e2 == std::string::npos ? in.size() : e2) - (e + 2));
                i = e2 == std::string::npos ? in.size() : e2 + 1;
            }
        } else {
            out += c;
        }
    }
    return out;
}

// --- The client's plural markup "|4one:many;" / "|4one:few:many;" --------------------------------------------------
// enUS writes two forms (SECONDS "|4Second:Seconds;", DAYS_ABBR "%d |4Day:Days;"), ruRU three (GetText's rule).
// The form is chosen by a number n:
//   - two forms: n == 1 -> one, else many (as upstream's resolvePluralEscapes, src/ui/plural_escape.cpp);
//   - three forms: n % 10 == 1 && n % 100 != 11 -> one; n % 10 in 2..4 && n % 100 not in 12..14 -> few; else many;
//   - any other count of forms: the last one;
//   - no number: the last form (the plural - a bare SECONDS is a heading, not a count).
// A negative n counts as its magnitude. `|3-N(word)` (declension) is not plural markup and is left alone here.

// "one:many" / "one:few:many" (the body of the markup, or of the spell text's "$l...;") -> the form for n.
inline std::string pluralForm(const std::string& forms, long n, bool haveNumber = true) {
    size_t count = 1;
    for (const char c : forms) count += c == ':';
    size_t pick = count - 1;
    if (haveNumber) {
        const unsigned long v = n < 0 ? 0ul - static_cast<unsigned long>(n) : static_cast<unsigned long>(n);
        const unsigned long m10 = v % 10ul, m100 = v % 100ul;
        if (count == 2) {
            pick = v == 1ul ? 0 : 1;
        } else if (count == 3) {
            const bool one = m10 == 1ul && m100 != 11ul;
            const bool few = m10 >= 2ul && m10 <= 4ul && (m100 < 12ul || m100 > 14ul);
            pick = one ? 0 : few ? 1 : 2;
        }
    }
    size_t b = 0;
    for (; pick > 0; --pick) b = forms.find(':', b) + 1;
    const size_t e = forms.find(':', b);
    return forms.substr(b, e == std::string::npos ? std::string::npos : e - b);
}

// The last run of digits in text (anywhere: "%d more daily |4quest:quests;" has words in between) -> n; false when
// there is none. A run longer than nine digits keeps its last nine plus 1e9 (n % 100 stays, n == 1 cannot happen).
inline bool pluralLastNumber(const std::string& text, long& n) {
    size_t e = text.size();
    while (e > 0 && (text[e - 1] < '0' || text[e - 1] > '9')) --e;
    if (e == 0) return false;
    size_t b = e;
    while (b > 0 && text[b - 1] >= '0' && text[b - 1] <= '9') --b;
    const bool longRun = e - b > 9;
    if (longRun) b = e - 9;
    n = 0;
    for (size_t k = b; k < e; ++k) n = n * 10 + (text[k] - '0');
    if (longRun) n += 1000000000L;
    return true;
}

namespace detail {
// fixed: every marker takes n; else the last number of the text resolved so far (as upstream).
inline std::string pluralResolve(const std::string& s, bool fixed, long n) {
    if (s.find("|4") == std::string::npos) return s;
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] != '|' || i + 1 >= s.size()) {
            out += s[i++];
            continue;
        }
        if (s[i + 1] == '|') {  // an escaped "|" ("||4" is a "|" and a "4", not the markup): kept for uiPlain
            out.append(s, i, 2);
            i += 2;
            continue;
        }
        const size_t colon = s.find(':', i + 2), semi = s.find(';', i + 2);
        // PSP: enUS CALENDAR_ERROR_(GUILD_)EVENTS_EXCEEDED close the markup with nothing ("%d |4created event:created
        // events."): its forms run to the end of the text (or the next "|"), a final "." stays after the chosen form.
        const size_t bar = s.find('|', i + 2);
        if (s[i + 1] == '4' && colon != std::string::npos && semi == std::string::npos &&
            (bar == std::string::npos || colon < bar)) {
            size_t end = bar == std::string::npos ? s.size() : bar;
            const bool dot = end > colon + 1 && s[end - 1] == '.';
            long v = n;
            const bool have = fixed || pluralLastNumber(out, v);
            out += pluralForm(s.substr(i + 2, end - (dot ? 1 : 0) - (i + 2)), v, have);
            if (dot) out += '.';
            i = end;
            continue;
        }
        // not the markup, or a malformed one (no ':' before a ';'): left as written, as upstream does
        if (s[i + 1] != '4' || colon == std::string::npos || semi == std::string::npos || colon > semi) {
            out += s[i++];
            continue;
        }
        long v = n;
        const bool have = fixed || pluralLastNumber(out, v);
        out += pluralForm(s.substr(i + 2, semi - (i + 2)), v, have);
        i = semi + 1;
    }
    return out;
}
}  // namespace detail

// Every marker of an already formatted text, for the last number before it ("1 |4Second:Seconds;" -> "1 Second").
inline std::string plural(const std::string& text) { return detail::pluralResolve(text, false, 0); }

// Every marker for n given by the caller (a client form resolved before its "%d" is filled in).
inline std::string plural(const std::string& text, long n) { return detail::pluralResolve(text, true, n); }

}  // namespace wowee::platform::psp::text
