// The client's languages on the card (docs/PSP_PORT_ARCHITECTURE.md).
//
// The PC converter (Kiln) bakes the player's client: the primary language into data/, every other language's own
// paks (text, ui, glue, framexml, tutorial, spells, craft, taxi, wotlk, sounds, emotetext ...) into
// data/locale/<xxYY>/ under the same file names, and data/locales.txt (psp_locale_rules.hpp). At boot the running
// language is chosen once (wowee.cfg locale, else the file's default, else the primary); when it is not the
// primary, fsPath() hands out data/locale/<xxYY>/<name> for every data/<name> that folder has - the one place every
// pak path is built (Pak / PakIndex / the sound bank's parts / the streamer / the movies take fsPath's output), so
// paks opened later at run time follow too. The language is the logon challenge's locale as well (the server's
// texts) and FrameXML's GetLocale().
//
// Switching: the original keeps the CVar "locale" and asks for a restart (InterfaceOptionsLanguagesPanel ->
// CLIENT_RESTART_ALERT). The PSP does the same: the pick goes to wowee.cfg locale= (configSet + the settings'
// configSave), then "Restart" ends the run through the normal exit path and main() tries to start the EBOOT again
// (systemRelaunch, wowee.cfg relaunch=1) - the XMB when the firmware refuses. Live switching would have to rebuild
// every cache that holds text or art (the UI kit, the spell / tutorial / item icon tables, the windows' art) - not
// done: one missed cache = a mixed-language screen, and re-loading the kit mid-run fragments the heap.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace wowee::platform::psp {

// Once, after configLoad() and before the first pak is opened (main_psp.cpp). Reads data/locales.txt, picks the
// running language, lists data/locale/<it>/ (logged: "locale active= primary= extras= src=" and "locale_files").
void localeInit();

const std::string& localeActive();                  // the language this run uses
const std::string& localePrimary();                 // the language of data/
const std::vector<std::string>& localeInstalled();  // the primary, then the extras with a folder on the card
// The language the next start will use (wowee.cfg as it is now in memory).
std::string localePending();

// fsPath()'s hook: true (out = the localized file's absolute path) when rel = "data/<name>" and the running
// language's folder has <name>. Thread-safe after localeInit (the list is read-only; the first use of each file is
// logged once: "locale_use name=").
bool localeResolve(const char* rel, char* out, size_t cap);

// `name`'s file of an installed language, by-passing the override: data/<name> for the primary, else
// data/locale/<code>/<name>. False when it did not fit.
bool localePakPath(const std::string& code, const char* name, char* out, size_t cap);
// The port's own strings are Russian until the string table. A language whose ui.pak fonts
// have no Cyrillic (the enUS client's FRIZQT__.TTF) takes the fonts of an installed language's ui.pak that has
// (main_psp.cpp: the UI kit; the world map's own font) - the same typefaces, Latin-1 included. "" = none taken.
void localeSetFontPak(const char* absPath);
const char* localeFontPak();

// The player picked `code` (an installed language): wowee.cfg locale=code in memory (configSet; the caller's
// configSave writes it). Returns true when it differs from the running language - a restart applies it.
bool localePick(const std::string& code);

// "Restart" was chosen: main() relaunches the EBOOT after its normal teardown (systemRelaunch).
void localeRequestRestart();
bool localeRestartRequested();

} // namespace wowee::platform::psp
