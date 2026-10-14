// PSP controller: buttons + analog nub, sampled once per frame.
#pragma once

#include <string>

#include <cstdint>

namespace wowee::platform::psp {

struct PadState {
    uint32_t held = 0;      // PSP_CTRL_* bitmask
    uint32_t pressed = 0;   // went down this frame
    uint32_t released = 0;  // went up this frame
    float ax = 0.0f;        // analog nub, [-1, 1] after dead zone
    float ay = 0.0f;
    uint8_t rawX = 128;
    uint8_t rawY = 128;
};

void inputInit();
const PadState& inputUpdate();
const PadState& inputState();
// Test aid: wowee.cfg autopad=<script> plays PSP buttons as if pressed - for emulator runs while
// another PPSSPP holds the remote debugger port (presses would land in that one). Emulator-only key, like
// autologin; remove it after the test. Tokens, space-separated:
//   @name      wait for inputScriptSignal("name") (the login screen sends "world" when the loading screen ends)
//   w<ms>      wait
//   <b>[:ms]   hold a button ms (default 120), then 120 ms released; b = x o s t (Cross Circle Square
//              Triangle), u d l r (D-pad), L R, start, select
//   +<b> -<b>  hold / release a button until told otherwise (R + the nub drags a window)
//   n<x>,<y>:<ms>  the nub at x,y in [-1, 1] (y -1 = up) for ms, then centred
// Every step is logged (autopad step=...).
void inputScriptSignal(const char* name);
// PSP (devcmd "pad"): more autopad steps at run time, appended to the script.
void inputScriptRun(const std::string& script);
// The on-device autopilot: the script has nothing left to play (no step, no hold running).
bool inputScriptIdle();
// Drops what the script still had to play and lets go of its buttons and the nub (the owner takes the pad).
void inputScriptStop();
// While on, a real START + SELECT chord (both down within ~6 frames) is taken from the pad - the game
// never sees it (no menu, no cursor) - and counted for inputTakeStartSelectChord(); START and
// SELECT alone reach the game ~6 frames late (the chord window). Off (the default): the pad as before, no delay.
void inputFilterStartSelect(bool on);
bool inputTakeStartSelectChord();  // true once per chord
// The buttons really held this frame (no script buttons, no filter).
uint32_t inputRealHeld();

} // namespace wowee::platform::psp
