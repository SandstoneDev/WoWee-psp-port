// PSP (owner 14.10): the saved login lives OUTSIDE the game folder - ms0:/WOWEE/account.dat at the root of the memory
// stick - so a copied or shared game folder never carries anybody's account. The file is made by the first login
// that succeeds (the account and the password typed on the login screen, or the old wowee.cfg lines, which are then
// removed from wowee.cfg and its .bak).
//
// Per logon server it keeps the account name and the SRP credential hash SHA1(UPPER(account):UPPER(password)) - what
// the logon proof is computed from (AuthHandler::authenticateWithHash) - never the password itself. The file is
// sealed with a key made from this console's OpenPSID: copied to another PSP it does not open (read as "no saved
// login"). It is not a vault: whoever holds this PSP can log in with it, as with any saved login.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wowee::platform::psp {

struct StoredLogin {
    std::string account;
    std::vector<uint8_t> authHash;  // 20 bytes
};

// The saved login for this logon server (host name as in wowee.cfg server=, case-insensitive).
bool loginStoreGet(const std::string& server, StoredLogin& out);
// Saves (replaces) the server's login; the other servers' entries stay.
bool loginStorePut(const std::string& server, const StoredLogin& login);
// Drops the server's login (the server refused the account or the password).
bool loginStoreForget(const std::string& server);
// SHA1(UPPER(account):UPPER(password)) - the same as SRP::computeAuthHash.
std::vector<uint8_t> loginAuthHash(const std::string& account, const std::string& password);
const char* loginStorePath();

}  // namespace wowee::platform::psp
