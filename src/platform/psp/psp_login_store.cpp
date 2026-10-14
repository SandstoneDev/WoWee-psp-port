// PSP: the saved login outside the game folder (include/platform/psp/psp_login_store.hpp).
//
// File: "WWLS" | version 1 | nonce 16 | tag 20 | body. body = the text below XORed with SHA1(key | nonce | block#)
// blocks; tag = HMAC-SHA1(key, nonce | body); key = SHA1("wowee-login-v1" | OpenPSID). The text: one line per
// server, "server<TAB>account<TAB>hash as 40 hex digits".
#include "platform/psp/psp_login_store.hpp"

#include "auth/crypto.hpp"
#include "platform/psp/psp_log.hpp"

#include <pspiofilemgr.h>
#include <pspopenpsid.h>
#include <pspthreadman.h>
#include <psputils.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace wowee::platform::psp {
namespace {

constexpr const char* kDir = "ms0:/WOWEE";
constexpr const char* kPath = "ms0:/WOWEE/account.dat";
constexpr uint8_t kMagic[4] = {'W', 'W', 'L', 'S'};
constexpr uint8_t kVersion = 1;
constexpr size_t kNonce = 16, kTag = 20, kHead = 4 + 1 + kNonce + kTag;

using Bytes = std::vector<uint8_t>;

Bytes deviceKey() {
    static Bytes key;
    if (!key.empty()) return key;
    PspOpenPSID psid;
    std::memset(&psid, 0, sizeof(psid));
    const int r = sceOpenPSIDGetOpenPSID(&psid);
    if (r < 0) PSP_LOGW(tag::Net, "login_store psid r=%08X (key without it)", static_cast<unsigned>(r));
    const char* label = "wowee-login-v1";
    Bytes seed(label, label + std::strlen(label));
    seed.insert(seed.end(), psid.data, psid.data + sizeof(psid.data));
    key = auth::Crypto::sha1(seed);
    return key;
}

void xorStream(const Bytes& key, const uint8_t* nonce, Bytes& data) {
    for (size_t off = 0, block = 0; off < data.size(); off += 20, ++block) {
        Bytes in(key);
        in.insert(in.end(), nonce, nonce + kNonce);
        for (int i = 0; i < 4; ++i) in.push_back(static_cast<uint8_t>(block >> (8 * i)));
        const Bytes ks = auth::Crypto::sha1(in);
        for (size_t i = 0; i < 20 && off + i < data.size(); ++i) data[off + i] ^= ks[i];
    }
}

Bytes tagOf(const Bytes& key, const uint8_t* nonce, const Bytes& body) {
    Bytes msg(nonce, nonce + kNonce);
    msg.insert(msg.end(), body.begin(), body.end());
    return auth::Crypto::hmacSHA1(key, msg);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

struct Entry {
    std::string server, account, hashHex;
};

// The plain text of the file; false when there is none or it does not open with this console's key.
bool readEntries(std::vector<Entry>& out) {
    out.clear();
    const SceUID fd = sceIoOpen(kPath, PSP_O_RDONLY, 0);
    if (fd < 0) return false;
    Bytes file;
    uint8_t buf[512];
    for (int n; (n = sceIoRead(fd, buf, sizeof(buf))) > 0 && file.size() < 64 * 1024;) file.insert(file.end(), buf, buf + n);
    sceIoClose(fd);
    if (file.size() < kHead || std::memcmp(file.data(), kMagic, 4) != 0 || file[4] != kVersion) {
        PSP_LOGW(tag::Net, "login_store unreadable bytes=%u", static_cast<unsigned>(file.size()));
        return false;
    }
    const uint8_t* nonce = file.data() + 5;
    const Bytes mac(file.begin() + 5 + kNonce, file.begin() + kHead);
    Bytes body(file.begin() + kHead, file.end());
    const Bytes key = deviceKey();
    if (tagOf(key, nonce, body) != mac) {
        PSP_LOGW(tag::Net, "login_store other_console_or_damaged");  // copied from another PSP: no saved login
        return false;
    }
    xorStream(key, nonce, body);
    const std::string text(body.begin(), body.end());
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        const size_t t1 = line.find('\t'), t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        Entry e{line.substr(0, t1), line.substr(t1 + 1, t2 - t1 - 1), line.substr(t2 + 1)};
        if (e.hashHex.size() == 40 && !e.server.empty() && !e.account.empty()) out.push_back(std::move(e));
    }
    return true;
}

bool writeEntries(const std::vector<Entry>& entries) {
    std::string text;
    for (const Entry& e : entries) text += e.server + "\t" + e.account + "\t" + e.hashHex + "\n";
    Bytes body(text.begin(), text.end());
    uint8_t nonce[kNonce];
    {
        Bytes seed = deviceKey();
        const uint64_t t = sceKernelGetSystemTimeWide();
        for (int i = 0; i < 8; ++i) seed.push_back(static_cast<uint8_t>(t >> (8 * i)));
        seed.insert(seed.end(), body.begin(), body.end());
        const Bytes h = auth::Crypto::sha1(seed);
        std::memcpy(nonce, h.data(), kNonce);
    }
    const Bytes key = deviceKey();
    xorStream(key, nonce, body);
    const Bytes mac = tagOf(key, nonce, body);
    Bytes file(kMagic, kMagic + 4);
    file.push_back(kVersion);
    file.insert(file.end(), nonce, nonce + kNonce);
    file.insert(file.end(), mac.begin(), mac.end());
    file.insert(file.end(), body.begin(), body.end());
    sceIoMkdir(kDir, 0777);  // fails harmlessly when it is there
    const SceUID fd = sceIoOpen(kPath, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        PSP_LOGW(tag::Net, "login_store write_open r=%08X", static_cast<unsigned>(fd));
        return false;
    }
    const int w = sceIoWrite(fd, file.data(), file.size());
    sceIoClose(fd);
    return w == static_cast<int>(file.size());
}

std::string hexOf(const Bytes& b) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (uint8_t c : b) {
        s += d[c >> 4];
        s += d[c & 15];
    }
    return s;
}

Bytes bytesOf(const std::string& hex) {
    Bytes b;
    auto v = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    for (size_t i = 0; i + 1 < hex.size(); i += 2) b.push_back(static_cast<uint8_t>(v(hex[i]) * 16 + v(hex[i + 1])));
    return b;
}

}  // namespace

const char* loginStorePath() { return kPath; }

std::vector<uint8_t> loginAuthHash(const std::string& account, const std::string& password) {
    std::string s = account + ":" + password;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    Bytes h = auth::Crypto::sha1(s);
    std::fill(s.begin(), s.end(), '\0');
    return h;
}

bool loginStoreGet(const std::string& server, StoredLogin& out) {
    std::vector<Entry> entries;
    if (!readEntries(entries)) return false;
    const std::string want = lower(server);
    for (const Entry& e : entries) {
        if (lower(e.server) != want) continue;
        out.account = e.account;
        out.authHash = bytesOf(e.hashHex);
        return out.authHash.size() == 20;
    }
    return false;
}

bool loginStorePut(const std::string& server, const StoredLogin& login) {
    if (server.empty() || login.account.empty() || login.authHash.size() != 20) return false;
    std::vector<Entry> entries;
    readEntries(entries);  // none, or another console's file: started anew
    const std::string want = lower(server);
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const Entry& e) { return lower(e.server) == want; }),
                  entries.end());
    entries.push_back(Entry{server, login.account, hexOf(login.authHash)});
    const bool ok = writeEntries(entries);
    PSP_LOGI(tag::Net, "login_store saved ok=%d servers=%u path=%s", ok ? 1 : 0, static_cast<unsigned>(entries.size()),
             kPath);
    return ok;
}

bool loginStoreForget(const std::string& server) {
    std::vector<Entry> entries;
    if (!readEntries(entries)) return false;
    const std::string want = lower(server);
    const size_t before = entries.size();
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const Entry& e) { return lower(e.server) == want; }),
                  entries.end());
    if (entries.size() == before) return true;
    const bool ok = writeEntries(entries);
    PSP_LOGI(tag::Net, "login_store forgot ok=%d servers=%u", ok ? 1 : 0, static_cast<unsigned>(entries.size()));
    return ok;
}

}  // namespace wowee::platform::psp
