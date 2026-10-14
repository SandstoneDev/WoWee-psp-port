// PSP (docs/PSP_PORT_ARCHITECTURE.md): the on-device autopilot's script - autotest.txt next to the EBOOT.
// The parser is header-only and platform-free (tools/psp/host_tests/autotest_host_test.sh); the runner that plays it
// on the PSP is src/platform/psp/psp_autotest.cpp.
//
// One step a line; '#' comments, blank lines, a UTF-8 BOM and CR line ends are fine. Lines:
//   set <key>=<value>        before the first step only: a wowee.cfg key for this run (the file on the card is
//                            never written - configOverride)
//   block <id> <ru>|<en>     a named test block: shown on screen, one PASS / FAIL row in the report
//   look <ru>|<en>           what the owner should look at now (stays until the next look / block); in captions
//                            "||" is a '|' of the text, no '|' = one text for both languages
//   sleep <s>                seconds (fractions fine)
//   wait <text>[|<alt>...] [<timeout s>]   passes when a log line containing one of the texts was logged since the
//                            block's last action step (say / pad / mark / ui / open / close / target / pos / mem) or
//                            the block's start - a "wait" after a "sleep" still sees the answer to the "say" before
//                            it; default timeout 120 s (max 3600); a timeout fails the block; <= 64 waits a block
//   seen <text>              passes when such a line was logged at any time since boot (boot lines: heap_budget ...)
//   expect_not <text>        the block fails if such a line appears anywhere in the block (from its start)
//   mem <name>               "mem at=<name>" (logMemory); "mem peak" = the mem_peak / bigheap_peak lines
//   shot <name>              a 480x272 BMP of the screen -> autotest/<name>.bmp (wowee.cfg autotestshot=0: skipped)
//   moved <yards>            the last two "devcmd_pos" lines (step "pos") are at least that far apart
//   end                      the run ends here (lines after it are ignored)
//   the devcmd verbs (ui_world_devtools.cpp), run by the world screen: say <text> | pad <autopad tokens> |
//   mark <name> | ui 0|1 | open <window> | close <window> | target next|clear | pos
// Prefixes: "try <step>" - its failure is logged but does not fail the block; "always <step>" - runs even after the
// block failed (the rest of a failed block is skipped otherwise). Steps before the first "block" form the block
// "start".
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace wowee::platform::psp::autotest {

enum class Verb : uint8_t {
    Say, Pad, Mark, Ui, Open, Close, Target, Pos,  // the devcmd verbs
    Block, Look, Sleep, Wait, Seen, ExpectNot, Mem, Shot, Moved, End,
};

inline const char* verbName(Verb v) {
    switch (v) {
        case Verb::Say: return "say";
        case Verb::Pad: return "pad";
        case Verb::Mark: return "mark";
        case Verb::Ui: return "ui";
        case Verb::Open: return "open";
        case Verb::Close: return "close";
        case Verb::Target: return "target";
        case Verb::Pos: return "pos";
        case Verb::Block: return "block";
        case Verb::Look: return "look";
        case Verb::Sleep: return "sleep";
        case Verb::Wait: return "wait";
        case Verb::Seen: return "seen";
        case Verb::ExpectNot: return "expect_not";
        case Verb::Mem: return "mem";
        case Verb::Shot: return "shot";
        case Verb::Moved: return "moved";
        case Verb::End: return "end";
    }
    return "?";
}

// Run by the world screen (WorldScreen::devCommand): they need a world.
inline bool isWorldVerb(Verb v) {
    return v == Verb::Say || v == Verb::Ui || v == Verb::Open || v == Verb::Close || v == Verb::Target ||
           v == Verb::Pos;
}
// Do something: a wait's window starts at the block's last one (mem too: its "bigheap" line).
inline bool isActionVerb(Verb v) { return isWorldVerb(v) || v == Verb::Pad || v == Verb::Mark || v == Verb::Mem; }

// The runner watches a block's waits in a fixed array (the log listener may run on another thread).
inline constexpr int kMaxBlockWaits = 64;

struct Step {
    Verb verb = Verb::End;
    std::string arg;       // the rest of the line (say text, pad tokens, window, mark / mem / shot name, wait texts)
    std::string ru, en;    // block / look captions
    uint32_t ms = 0;       // sleep: length; wait: timeout
    float yards = 0.0f;    // moved
    bool optional = false; // try
    bool always = false;   // always
    int line = 0;          // 1-based line of the file
    int block = 0;         // index in Script::blocks
};

struct Block {
    std::string id, ru, en;
    int firstStep = 0;   // index of its "block" step (-1: the implicit "start" block)
    int stepCount = 0;   // steps after the "block" line
    std::vector<std::string> forbidden;  // expect_not texts
};

struct Script {
    std::vector<std::pair<std::string, std::string>> sets;  // set key=value
    std::vector<Step> steps;
    std::vector<Block> blocks;
    std::vector<std::string> seen;    // the texts of every "seen" step (the log listener watches them from boot)
    std::vector<std::string> errors;  // "line N: ..." - such lines are skipped
    uint32_t sleepMs = 0;             // sum of the sleeps
    uint32_t padMs = 0;               // sum of the pad steps' own time (estimate)
    uint32_t waitMaxMs = 0;           // sum of the wait timeouts (the worst case)
    bool hasEnd = false;
};

inline std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

// "<ru>|<en>": no '|' - the same text for both; "||" is a '|' of the text (the client's markup named in a caption).
inline void splitCaption(const std::string& s, std::string& ru, std::string& en) {
    std::string part[2];
    int k = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '|' && i + 1 < s.size() && s[i + 1] == '|') {
            part[k] += '|';
            ++i;
        } else if (s[i] == '|' && k == 0) {
            k = 1;
        } else {
            part[k] += s[i];
        }
    }
    ru = trim(part[0]);
    en = k ? trim(part[1]) : ru;
    if (ru.empty()) ru = en;
    if (en.empty()) en = ru;
}

// A whole token as a non-negative number ("12", "0.5"); false for anything else.
inline bool parseNumber(const std::string& t, double& out) {
    if (t.empty()) return false;
    char* end = nullptr;
    out = std::strtod(t.c_str(), &end);
    return end && *end == 0 && out >= 0.0 && std::isfinite(out);
}

// `line` contains one of the '|'-separated texts of `alts` (empty texts never match).
inline bool matchAny(const char* line, const char* alts) {
    if (!line || !alts) return false;
    const char* p = alts;
    char one[128];
    while (*p) {
        const char* bar = std::strchr(p, '|');
        const size_t n = bar ? static_cast<size_t>(bar - p) : std::strlen(p);
        if (n > 0 && n < sizeof(one)) {
            std::memcpy(one, p, n);
            one[n] = 0;
            if (std::strstr(line, one)) return true;
        }
        if (!bar) break;
        p = bar + 1;
    }
    return false;
}

// "devcmd_pos x=<x> y=<y> z=<z> ..." -> the position.
inline bool parsePos(const char* line, float& x, float& y, float& z) {
    const char* p = line ? std::strstr(line, "devcmd_pos ") : nullptr;
    if (!p) return false;
    const char* px = std::strstr(p, " x=");
    const char* py = std::strstr(p, " y=");
    const char* pz = std::strstr(p, " z=");
    if (!px || !py || !pz) return false;
    x = std::strtof(px + 3, nullptr);
    y = std::strtof(py + 3, nullptr);
    z = std::strtof(pz + 3, nullptr);
    return true;
}

// The time an autopad script takes by itself (psp_input.hpp: b[:ms] holds ms then 120 ms released, w<ms>,
// n<x>,<y>:<ms>); false with `why` for a token the autopad does not know.
inline bool padScriptMs(const std::string& tokens, uint32_t& ms, std::string& why) {
    ms = 0;
    size_t i = 0;
    while (i < tokens.size()) {
        while (i < tokens.size() && tokens[i] == ' ') ++i;
        size_t j = i;
        while (j < tokens.size() && tokens[j] != ' ') ++j;
        if (j == i) break;
        const std::string t = tokens.substr(i, j - i);
        i = j;
        if (t[0] == '@') {
            why = "autopad signal waits (@) are not for the autopilot: " + t;
            return false;
        }
        if (t[0] == 'w' && t.size() > 1) {
            double v = 0;
            if (!parseNumber(t.substr(1), v)) {
                why = "bad wait token " + t;
                return false;
            }
            ms += static_cast<uint32_t>(v);
            continue;
        }
        if (t[0] == 'n' && t.size() > 1 && t.find(',') != std::string::npos) {
            const size_t c = t.find(':');
            ms += c == std::string::npos ? 300u : static_cast<uint32_t>(std::atoi(t.c_str() + c + 1));
            continue;
        }
        const bool latch = t[0] == '+' || t[0] == '-';
        const size_t c = t.find(':');
        const std::string b = t.substr(latch ? 1 : 0, (c == std::string::npos ? t.size() : c) - (latch ? 1 : 0));
        static const char* const kButtons[] = {"x", "o", "s", "t", "u", "d", "l", "r", "L", "R", "start", "select"};
        bool known = false;
        for (const char* k : kButtons) known |= b == k;
        if (!known) {
            why = "unknown pad button " + t;
            return false;
        }
        if (!latch) ms += (c == std::string::npos ? 120u : static_cast<uint32_t>(std::atoi(t.c_str() + c + 1))) + 120u;
    }
    if (tokens.find_first_not_of(' ') == std::string::npos) {
        why = "pad without buttons";
        return false;
    }
    return true;
}

inline Script parse(const std::string& textIn) {
    Script sc;
    std::string text = textIn;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        text.erase(0, 3);
    auto error = [&sc](int line, const std::string& what) {
        sc.errors.push_back("line " + std::to_string(line) + ": " + what);
    };
    auto currentBlock = [&sc]() -> int {
        if (sc.blocks.empty()) {
            Block b;
            b.id = "start";
            b.ru = b.en = "start";
            b.firstStep = -1;
            sc.blocks.push_back(b);
        }
        return static_cast<int>(sc.blocks.size()) - 1;
    };
    auto currentBlockIfAny = [&sc]() -> int { return sc.blocks.empty() ? 0 : static_cast<int>(sc.blocks.size()) - 1; };
    size_t pos = 0;
    int lineNo = 0;
    while (pos < text.size() && !sc.hasEnd) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        const std::string line = trim(text.substr(pos, eol - pos));
        pos = eol + 1;
        ++lineNo;
        if (line.empty() || line[0] == '#') continue;
        Step st;
        st.line = lineNo;
        std::string rest = line;
        for (;;) {  // prefixes
            if (rest.compare(0, 4, "try ") == 0) {
                st.optional = true;
                rest = trim(rest.substr(4));
            } else if (rest.compare(0, 7, "always ") == 0) {
                st.always = true;
                rest = trim(rest.substr(7));
            } else {
                break;
            }
        }
        const size_t sp = rest.find_first_of(" \t");
        const std::string verb = rest.substr(0, sp);
        const std::string arg = sp == std::string::npos ? std::string() : trim(rest.substr(sp + 1));
        st.arg = arg;
        if (verb == "set") {
            const size_t eq = arg.find('=');
            if (!sc.steps.empty()) error(lineNo, "set after the first step (ignored)");
            else if (eq == std::string::npos || trim(arg.substr(0, eq)).empty()) error(lineNo, "set needs key=value");
            else sc.sets.emplace_back(trim(arg.substr(0, eq)), trim(arg.substr(eq + 1)));
            continue;
        }
        bool ok = true;
        if (verb == "say") {
            st.verb = Verb::Say;
            ok = !arg.empty();
            if (!ok) error(lineNo, "say without text");
        } else if (verb == "pad") {
            st.verb = Verb::Pad;
            uint32_t ms = 0;
            std::string why;
            ok = padScriptMs(arg, ms, why);
            if (ok) sc.padMs += ms;
            else error(lineNo, why);
        } else if (verb == "mark") {
            st.verb = Verb::Mark;
            ok = !arg.empty();
            if (!ok) error(lineNo, "mark without a name");
        } else if (verb == "ui") {
            st.verb = Verb::Ui;
            ok = arg == "0" || arg == "1";
            if (!ok) error(lineNo, "ui takes 0 or 1");
        } else if (verb == "open" || verb == "close") {
            st.verb = verb == "open" ? Verb::Open : Verb::Close;
            ok = !arg.empty();
            if (!ok) error(lineNo, verb + " without a window");
        } else if (verb == "target") {
            st.verb = Verb::Target;
            ok = arg == "next" || arg == "clear";
            if (!ok) error(lineNo, "target takes next or clear");
        } else if (verb == "pos") {
            st.verb = Verb::Pos;
        } else if (verb == "block") {
            st.verb = Verb::Block;
            const size_t s2 = arg.find_first_of(" \t");
            const std::string id = arg.substr(0, s2);
            const std::string cap = s2 == std::string::npos ? std::string() : trim(arg.substr(s2 + 1));
            ok = !id.empty() && !cap.empty();
            if (!ok) {
                error(lineNo, "block needs <id> <ru>|<en>");
            } else {
                for (const Block& b : sc.blocks)
                    if (b.id == id) error(lineNo, "block id used twice: " + id);
                st.arg = id;
                splitCaption(cap, st.ru, st.en);
            }
        } else if (verb == "look") {
            st.verb = Verb::Look;
            ok = !arg.empty();
            if (ok) splitCaption(arg, st.ru, st.en);
            else error(lineNo, "look without text");
        } else if (verb == "sleep") {
            st.verb = Verb::Sleep;
            double s = 0;
            ok = parseNumber(arg, s) && s <= 3600.0;
            if (ok) {
                st.ms = static_cast<uint32_t>(s * 1000.0 + 0.5);
                sc.sleepMs += st.ms;
            } else {
                error(lineNo, "sleep takes seconds (0..3600)");
            }
        } else if (verb == "wait") {
            st.verb = Verb::Wait;
            st.ms = 120000;
            std::string texts = arg;
            const size_t last = arg.find_last_of(" \t");
            double s = 0;
            if (last != std::string::npos && parseNumber(arg.substr(last + 1), s)) {
                st.ms = static_cast<uint32_t>((s > 3600.0 ? 3600.0 : s) * 1000.0 + 0.5);
                texts = trim(arg.substr(0, last));
            } else if (last == std::string::npos && parseNumber(arg, s)) {
                texts.clear();  // "wait 30": a timeout without a text
            }
            st.arg = texts;
            ok = !texts.empty() && texts.size() < 120;
            if (ok) {
                int waits = 0;
                const int b = currentBlockIfAny();
                for (const Step& s : sc.steps) waits += s.verb == Verb::Wait && s.block == b ? 1 : 0;
                if (waits >= kMaxBlockWaits) {
                    ok = false;
                    error(lineNo, "more than 64 waits in one block");
                }
            } else {
                error(lineNo, "wait needs a text (< 120 bytes) [timeout s]");
            }
            if (ok) sc.waitMaxMs += st.ms;
        } else if (verb == "seen") {
            st.verb = Verb::Seen;
            ok = !arg.empty() && arg.size() < 120;
            if (ok) {
                bool have = false;
                for (const std::string& s : sc.seen) have |= s == arg;
                if (!have) sc.seen.push_back(arg);
            } else {
                error(lineNo, "seen needs a text (< 120 bytes)");
            }
        } else if (verb == "expect_not") {
            st.verb = Verb::ExpectNot;
            ok = !arg.empty() && arg.size() < 120;
            if (ok) sc.blocks[static_cast<size_t>(currentBlock())].forbidden.push_back(arg);
            else error(lineNo, "expect_not needs a text (< 120 bytes)");
        } else if (verb == "mem") {
            st.verb = Verb::Mem;
            ok = !arg.empty() && arg.find(' ') == std::string::npos && arg.size() < 40;
            if (!ok) error(lineNo, "mem needs a name (no spaces, < 40)");
        } else if (verb == "shot") {
            st.verb = Verb::Shot;
            ok = !arg.empty() && arg.size() <= 32;
            for (char c : arg)
                ok = ok && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                            c == '-');
            if (!ok) error(lineNo, "shot needs a file name [A-Za-z0-9_-], <= 32");
        } else if (verb == "moved") {
            st.verb = Verb::Moved;
            double y = 1.0;
            ok = arg.empty() || parseNumber(arg, y);
            st.yards = static_cast<float>(y);
            if (!ok) error(lineNo, "moved takes yards");
        } else if (verb == "end") {
            st.verb = Verb::End;
            sc.hasEnd = true;
        } else {
            error(lineNo, "unknown verb: " + verb);
            ok = false;
        }
        if (!ok) continue;
        if (st.verb == Verb::Block) {
            Block b;
            b.id = st.arg;
            b.ru = st.ru;
            b.en = st.en;
            b.firstStep = static_cast<int>(sc.steps.size());
            sc.blocks.push_back(b);
            st.block = static_cast<int>(sc.blocks.size()) - 1;
        } else {
            st.block = currentBlock();
            ++sc.blocks[static_cast<size_t>(st.block)].stepCount;
        }
        sc.steps.push_back(st);
    }
    // The implicit "start" block that ended up empty (only sets / errors before the first block) is dropped.
    if (!sc.blocks.empty() && sc.blocks[0].firstStep < 0 && sc.blocks[0].stepCount == 0 && sc.blocks[0].forbidden.empty()) {
        sc.blocks.erase(sc.blocks.begin());
        for (Step& s : sc.steps) --s.block;
    }
    return sc;
}

}  // namespace wowee::platform::psp::autotest
