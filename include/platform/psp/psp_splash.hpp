// The community QR splash: one picture at start, before the login screen and the intro movie.
//
// Compiled in only with the CMake option WOWEE_PSP_SPLASH (default ON); there is no wowee.cfg key to turn it off.
// Automated runs never wait on it: with the on-device autopilot (autotest.txt), autologin=1 or an autopad script the
// splash is skipped and the log says so.
#pragma once

namespace wowee::platform::psp {

// Shows the splash until a button is pressed (main thread, after displayInit() and the configuration).
// Returns false when HOME -> Exit was chosen while it was shown: the caller ends the run.
bool splashRun();

} // namespace wowee::platform::psp
