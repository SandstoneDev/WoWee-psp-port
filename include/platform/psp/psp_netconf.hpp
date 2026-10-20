// PSP: the system network-connection dialog (sceUtilityNetconf, action
// CONNECTAP) - the firmware's own "choose a connection" screen that lists the
// saved Wi-Fi profiles, joins the chosen one and waits for an IP address.
//
// Same frame rule as the on-screen keyboard (psp_osk.hpp): the game keeps
// drawing its frame, netconfAfterGeSync() runs from frameEnd() after the GE
// list finished and before the swap, and the dialog draws on top. Screens
// only call netconfBegin() and poll netconfFinished().
//
// The network modules and sceNet / sceNetInet / sceNetApctl must be up before
// the dialog starts: call netPrepare() (psp_net.hpp) first.
#pragma once

namespace wowee::platform::psp {

// Starts the dialog. Returns false (and logs the SDK code) when it could not
// start - another system dialog is open, or InitStart refused.
bool netconfBegin();
bool netconfActive();
// True once, after the dialog closed. result = the dialog's base.result
// (0 OK, 1 cancelled by the player, 2 aborted, < 0 an error code).
bool netconfFinished(int& result);

// Called by frameEnd() after sceGuSync, before the swap.
void netconfAfterGeSync();

} // namespace wowee::platform::psp
