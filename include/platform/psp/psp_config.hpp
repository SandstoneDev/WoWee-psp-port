// wowee.cfg next to the EBOOT: plain "key=value" lines, '#' comments.
// Settings the owner (or the deploy script) sets on the card without a
// rebuild. Keys used today: server, port, account, password, netprofile, netssid,
// netdialog (1 = the system connection dialog at login, 0 = the saved profile).
// The graphics settings menu writes its keys back (configSet + configSave).
//
// The password line is plain text on the memory stick: the first login that succeeds moves the account into
// ms0:/WOWEE/account.dat (psp_login_store.hpp, outside the game folder) and erases both lines from wowee.cfg.
#pragma once

#include <string>

namespace wowee::platform::psp {

bool configLoad(const char* absPath);
bool configLoaded();
std::string configGet(const char* key, const char* fallback = "");
int configGetInt(const char* key, int fallback);

// Changes a value in memory and marks it for the next configSave().
void configSet(const char* key, const std::string& value);
// A value for this run only - never marked, never written by configSave() (the autopilot's
// "set key=value" lines in autotest.txt; wowee.cfg on the card stays as it is).
void configOverride(const char* key, const std::string& value);
// PSP (owner 14.10, psp_login_store.hpp): drops the key - in memory, and every line of it at the next configSave(),
// in wowee.cfg and in the wowee.cfg.bak that save writes (the old password must not stay in the backup).
void configErase(const char* key);
// Rewrites the file configLoad() read: a marked key's line is replaced where
// it stands, a key the file lacks is appended, every other line (comments,
// the password) is kept byte for byte. The previous file is kept as
// wowee.cfg.bak first. Returns false when nothing could be written.
bool configSave(int* keysWritten = nullptr);

} // namespace wowee::platform::psp
