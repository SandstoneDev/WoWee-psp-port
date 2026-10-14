#include "platform/psp/psp_input.hpp"

#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_public_safe.hpp"  // PSP: autopad only against a dev server

#include <pspctrl.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace wowee::platform::psp {

namespace {

PadState g_pad;

// The nub rests anywhere within roughly +-30 of centre on worn units.
constexpr int kDeadZone = 32;

float axis(uint8_t raw) {
    int v = static_cast<int>(raw) - 128;
    if (v > -kDeadZone && v < kDeadZone) return 0.0f;
    float span = 127.0f - kDeadZone;
    float f = (v > 0 ? v - kDeadZone : v + kDeadZone) / span;
    if (f > 1.0f) f = 1.0f;
    if (f < -1.0f) f = -1.0f;
    return f;
}

// the autopad test script (psp_input.hpp).
struct Script {
    bool loaded = false;
    std::vector<std::string> steps;
    size_t next = 0;
    uint32_t until = 0;   // the current step ends at this uptime
    uint32_t pulse = 0;   // buttons of the current step
    uint32_t latched = 0; // +b
    float nx = 0.0f, ny = 0.0f;
    bool nub = false;
    std::string waitFor;  // @name
    std::vector<std::string> signals;
} g_script;

// the autopilot's START + SELECT chord (psp_input.hpp inputFilterStartSelect).
constexpr int kChordFrames = 6;
struct ChordFilter {
    bool on = false;
    bool latched = false;     // a chord is down: START / SELECT stay hidden until both are up
    int chords = 0;           // not taken yet
    uint32_t line[kChordFrames] = {};  // START / SELECT of the last frames (the delay line)
    int pos = 0;
} g_chord;
uint32_t g_realHeld = 0;

uint32_t buttonBit(const std::string& b) {
    if (b == "x") return PSP_CTRL_CROSS;
    if (b == "o") return PSP_CTRL_CIRCLE;
    if (b == "s") return PSP_CTRL_SQUARE;
    if (b == "t") return PSP_CTRL_TRIANGLE;
    if (b == "u") return PSP_CTRL_UP;
    if (b == "d") return PSP_CTRL_DOWN;
    if (b == "l") return PSP_CTRL_LEFT;
    if (b == "r") return PSP_CTRL_RIGHT;
    if (b == "L") return PSP_CTRL_LTRIGGER;
    if (b == "R") return PSP_CTRL_RTRIGGER;
    if (b == "start") return PSP_CTRL_START;
    if (b == "select") return PSP_CTRL_SELECT;
    return 0;
}

// Advances the script; returns the buttons it holds this frame (the nub in g_script.nx/ny when nub).
uint32_t scriptButtons() {
    Script& s = g_script;
    if (!s.loaded) {
        s.loaded = true;
        const std::string v = configGet("autopad", "");
        size_t i = 0;
        while (i < v.size()) {
            while (i < v.size() && v[i] == ' ') ++i;
            size_t j = i;
            while (j < v.size() && v[j] != ' ') ++j;
            if (j > i) s.steps.push_back(v.substr(i, j - i));
            i = j;
        }
        if (!s.steps.empty()) PSP_LOGI(tag::Ctrl, "autopad steps=%u", static_cast<unsigned>(s.steps.size()));
    }
    if (s.next >= s.steps.size() && s.until == 0) return s.latched;
    const uint32_t now = logUptimeMs();
    if (!s.waitFor.empty()) {
        for (const std::string& g : s.signals)
            if (g == s.waitFor) s.waitFor.clear();
        if (!s.waitFor.empty()) return s.latched;
    }
    if (s.until != 0 && now < s.until) return s.latched | s.pulse;
    s.until = 0;
    s.pulse = 0;
    s.nub = false;
    if (s.next >= s.steps.size()) {
        PSP_LOGI(tag::Ctrl, "autopad done");
        return s.latched;
    }
    const std::string t = s.steps[s.next++];
    PSP_LOGI(tag::Ctrl, "autopad step=%s", t.c_str());
    if (t[0] == '@') {
        s.waitFor = t.substr(1);
    } else if (t[0] == 'w') {
        s.until = now + static_cast<uint32_t>(std::atoi(t.c_str() + 1));
    } else if (t[0] == '+' || t[0] == '-') {
        const uint32_t b = buttonBit(t.substr(1));
        s.latched = t[0] == '+' ? (s.latched | b) : (s.latched & ~b);
    } else if (t[0] == 'n') {
        float x = 0, y = 0;
        int ms = 300;
        std::sscanf(t.c_str() + 1, "%f,%f:%d", &x, &y, &ms);
        s.nx = x;
        s.ny = y;
        s.nub = true;
        s.until = now + static_cast<uint32_t>(ms);
    } else {
        const size_t c = t.find(':');
        const int ms = c == std::string::npos ? 120 : std::atoi(t.c_str() + c + 1);
        s.pulse = buttonBit(t.substr(0, c));
        s.until = now + static_cast<uint32_t>(ms);
        s.steps.insert(s.steps.begin() + static_cast<std::ptrdiff_t>(s.next), "w120");  // released before the next step
    }
    return s.latched | s.pulse;
}

} // namespace

void inputScriptRun(const std::string& v) {
    Script& s = g_script;
    s.loaded = true;
    size_t i = 0, added = 0;
    while (i < v.size()) {
        while (i < v.size() && v[i] == ' ') ++i;
        size_t j = i;
        while (j < v.size() && v[j] != ' ') ++j;
        if (j > i) {
            s.steps.push_back(v.substr(i, j - i));
            ++added;
        }
        i = j;
    }
    PSP_LOGI(tag::Ctrl, "autopad run steps=%u", static_cast<unsigned>(added));
}

void inputScriptSignal(const char* name) {
    if (!g_script.loaded || g_script.steps.empty()) return;
    g_script.signals.emplace_back(name);
    PSP_LOGI(tag::Ctrl, "autopad signal=%s", name);
}

bool inputScriptIdle() {
    const Script& s = g_script;
    return s.next >= s.steps.size() && s.until == 0 && s.waitFor.empty();
}

void inputScriptStop() {
    Script& s = g_script;
    const size_t left = s.steps.size() > s.next ? s.steps.size() - s.next : 0;
    s.steps.clear();
    s.next = 0;
    s.until = 0;
    s.pulse = 0;
    s.latched = 0;
    s.nub = false;
    s.waitFor.clear();
    s.loaded = true;
    PSP_LOGI(tag::Ctrl, "autopad stop dropped=%u", static_cast<unsigned>(left));
}

void inputFilterStartSelect(bool on) {
    g_chord = ChordFilter{};
    g_chord.on = on;
}

bool inputTakeStartSelectChord() {
    if (g_chord.chords <= 0) return false;
    --g_chord.chords;
    return true;
}

uint32_t inputRealHeld() { return g_realHeld; }

void inputInit() {
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
}

const PadState& inputUpdate() {
    SceCtrlData d;
    sceCtrlPeekBufferPositive(&d, 1);
    g_realHeld = d.Buttons;
    // the autopad test script adds its buttons and the nub (psp_input.hpp).
    // PSP (public safe): only against a dev server - undecided (server= is a DNS name, nothing connected yet)
    // it waits; once a public server was seen the script is dropped for good (autopad stop dropped=...).
    if (publicSafeLocked()) {
        Script& s = g_script;
        if (!s.loaded) {  // never started: the cfg script is not even read
            s.loaded = true;
            if (!configGet("autopad", "").empty()) PSP_LOGW(tag::Ctrl, "autopad_refused public=1");
        } else if (s.next < s.steps.size() || s.latched || s.nub || s.until) {
            PSP_LOGW(tag::Ctrl, "autopad_refused public=1");
            inputScriptStop();
        }
    } else if (devToolsAllowed()) {
        d.Buttons |= scriptButtons();
    }
    if (g_script.nub && devToolsAllowed()) {
        d.Lx = static_cast<unsigned char>(128.0f + g_script.nx * 127.0f);
        d.Ly = static_cast<unsigned char>(128.0f + g_script.ny * 127.0f);  // y -1 = up
    }
    if (g_chord.on) {  // START / SELECT through the delay line; a real chord is taken out
        constexpr uint32_t kSS = PSP_CTRL_START | PSP_CTRL_SELECT;
        ChordFilter& c = g_chord;
        if (c.latched) {
            if ((g_realHeld & kSS) == 0) c.latched = false;
        } else if ((g_realHeld & kSS) == kSS) {
            c.latched = true;
            ++c.chords;
            for (uint32_t& b : c.line) b = 0;  // the first of the two never reaches the game
        }
        const uint32_t in = c.latched ? 0u : (d.Buttons & kSS);
        const uint32_t out = c.latched ? 0u : c.line[c.pos];
        c.line[c.pos] = in;
        c.pos = (c.pos + 1) % kChordFrames;
        d.Buttons = (d.Buttons & ~kSS) | out;
    }
    uint32_t prev = g_pad.held;
    g_pad.held = d.Buttons;
    g_pad.pressed = d.Buttons & ~prev;
    g_pad.released = prev & ~d.Buttons;
    g_pad.rawX = d.Lx;
    g_pad.rawY = d.Ly;
    g_pad.ax = axis(d.Lx);
    g_pad.ay = axis(d.Ly);
    return g_pad;
}

const PadState& inputState() { return g_pad; }

} // namespace wowee::platform::psp
