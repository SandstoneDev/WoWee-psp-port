// PSP: "public safe" (docs/PSP_PORT_ARCHITECTURE.md, owner 13.10) - the debug and automation aids run only
// against a dev server (psp_public_safe_core.hpp: the devhosts= list, private addresses only). Locked elsewhere, and
// no wowee.cfg key unlocks them: the GM window (gmtools), devcmd.txt (devcmd), the autopilot (autotest.txt), autopad,
// the auto realm / character create / world entry of autologin, autotarget, the Debug window's disconnect button.
// The player's own typed chat is never filtered. Local-only aids (the profiler overlay, logs, render toggles) stay.
//
// Log: "public_safe on=<1 locked|0 dev> host=<ip> reason=<...> at=<boot|logon|realm>" at each decision.
#pragma once

#include <cstdint>

namespace wowee::platform::psp {

// After configLoad(): reads devhosts= and judges the cfg server= text when it is an address (no network yet).
// serverOverride: the autopilot's "set server=" value (nullptr = wowee.cfg's). Once per run: the first call decides.
void publicSafeBoot(const char* serverOverride = nullptr);
// Right after a connect to the logon server ("logon") or the realm ("realm"): the address the socket used
// (publicSafeNoteConnect) decides; Public latches for the rest of the run.
void publicSafeOnConnect(const char* at, const char* hostText);
// psp_socket_wrap.cpp: every connect's IPv4 address (host order), 0 for a non-IPv4 one.
void publicSafeNoteConnect(uint32_t ipHostOrder);

// True only when every server of this run so far is a dev server. Before any decision (a DNS name in server=,
// nothing connected yet) false: the aids wait.
bool devToolsAllowed();
// True once a public server was seen (the aids are off for good this run).
bool publicSafeLocked();

}  // namespace wowee::platform::psp
