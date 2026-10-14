// PSP: Warden without the x86 module - psp_warden_core.hpp.
// Upstream: src/game/warden_handler.cpp (handleWardenData, loadWardenCRFile), src/game/warden_memory.cpp (parsePE,
// readMemory, initKuserSharedData). AzerothCore's side: Warden.cpp / WardenWin.cpp (RequestChecks / HandleData).
#include "platform/psp/psp_warden_core.hpp"

#include "platform/psp/psp_sha1_stream.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace wowee::platform::psp::warden {

namespace {

uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}

// Copies the part of [srcBegin, srcBegin + srcLen) (image RVAs, bytes from the file at fileOff) that falls into the
// read window [rva, rva + n).
bool overlayFromFile(ReadAt fn, void* ctx, uint32_t srcBegin, uint32_t srcLen, uint32_t fileOff, uint32_t rva,
                     uint32_t n, uint8_t* out) {
    const uint64_t a = rva > srcBegin ? rva : srcBegin;
    const uint64_t b0 = static_cast<uint64_t>(rva) + n, b1 = static_cast<uint64_t>(srcBegin) + srcLen;
    const uint64_t b = b0 < b1 ? b0 : b1;
    if (a >= b) return true;
    return fn(ctx, fileOff + static_cast<uint32_t>(a - srcBegin), out + (a - rva), static_cast<uint32_t>(b - a));
}

// Running-client values (client 3.3.5a, 12340): see the header. va, bytes.
struct RuntimeValue {
    uint32_t va;
    uint8_t len;
    uint8_t bytes[8];
};
// The login state of a client in the world: the one memory check of AzerothCore's 34 that a running client answers
// differently from its file. Address and bytes are the server's public expected result, AzerothCore
// data/sql/base/db_world/warden_checks.sql row 437 (type 243, address 11287980, length 8, result 04000000903C9F00,
// "Login State").
constexpr RuntimeValue kRuntime[] = {
    {0x00AC3DACu, 8, {0x04, 0x00, 0x00, 0x00, 0x90, 0x3C, 0x9F, 0x00}},
};

// Upstream WardenMemory::initKuserSharedData - Windows 7 SP1 x86 (offset, little-endian value, size).
struct KField {
    uint16_t off;
    uint8_t len;
    uint32_t v;
};
constexpr KField kKuser[] = {
    {0x0000, 4, 0x003F4A00u}, {0x0004, 4, 0x0FA00000u}, {0x0008, 4, 0x6B49D200u}, {0x000C, 4, 0x00000029u},
    {0x0010, 4, 0x00000029u}, {0x0014, 4, 0xA0B71B00u}, {0x0018, 4, 0x01DA5E80u}, {0x001C, 4, 0x01DA5E80u},
    {0x002C, 2, 0x014Cu},     {0x002E, 2, 0x014Cu},     {0x023C, 4, 0x00010001u}, {0x0244, 4, 0x00200000u},
    {0x0264, 4, 1u},          {0x0268, 1, 1u},          {0x026C, 4, 6u},          {0x0270, 4, 1u},
    {0x0276, 1, 1u},          {0x0277, 1, 1u},          {0x027A, 1, 1u},          {0x027C, 1, 1u},
    {0x027D, 1, 1u},          {0x027E, 1, 1u},          {0x0280, 1, 1u},          {0x0281, 1, 1u},
    {0x02D0, 4, 0x0110u},     {0x02D5, 1, 2u},          {0x02D8, 4, 1u},          {0x02E4, 4, 0x003F4900u},
    {0x02E8, 4, 0x000FF000u}, {0x02F8, 1, 0xC3u},       {0x0320, 4, 0x003F4A00u}, {0x0330, 4, 0x4A2F8C15u},
    {0x0334, 4, 0x00001234u},
};
constexpr uint32_t kKuserBase = 0x7FFE0000u, kKuserSize = 0x1000u;

void overlayValue(uint32_t fieldVa, const uint8_t* bytes, uint32_t len, uint32_t va, uint32_t n, uint8_t* out,
                  bool& any) {
    for (uint32_t i = 0; i < len; ++i) {
        const uint32_t at = fieldVa + i;
        if (at >= va && at - va < n) {
            out[at - va] = bytes[i];
            any = true;
        }
    }
}

// Upstream hmacSha1Matches's module names: system DLLs a running client has loaded (answered "found").
constexpr const char* kLoadedModules[] = {"KERNEL32.DLL", "USER32.DLL",  "NTDLL.DLL", "WS2_32.DLL", "WSOCK32.DLL",
                                          "ADVAPI32.DLL", "SHELL32.DLL", "GDI32.DLL", "OPENGL32.DLL", "WINMM.DLL"};

bool hmacMatches(const uint8_t seed[4], const char* text, const uint8_t expected[20]) {
    HmacSha1Stream h;
    h.begin(seed, 4);
    h.update(text, std::strlen(text));
    uint8_t out[20];
    h.final(out);
    return std::memcmp(out, expected, 20) == 0;
}

constexpr uint8_t kNotFound = 0xE9;  // AzerothCore / TrinityCore WardenWin::HandleData: PAGE / DRIVER / MODULE pass
constexpr uint8_t kFound = 0x4A;     // upstream's "found"

}  // namespace

const char* checkTypeName(CheckType t) {
    static const char* const k[] = {"mem", "module", "page_a", "page_b", "mpq", "lua", "proc", "driver", "timing", "unknown"};
    return k[t <= CT_UNKNOWN ? t : CT_UNKNOWN];
}

const char* stateName(State s) {
    switch (s) {
        case State::Off: return "off";
        case State::WaitModuleUse: return "wait_module_use";
        case State::WaitModuleCache: return "wait_module_cache";
        case State::WaitHashRequest: return "wait_hash_request";
        case State::Checks: return "checks";
        case State::Silent: return "silent";
    }
    return "?";
}

void toHex(const uint8_t* p, size_t n, char* out) {
    static const char kH[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        out[i * 2] = kH[p[i] >> 4];
        out[i * 2 + 1] = kH[p[i] & 15];
    }
    out[n * 2] = 0;
}

bool parseCr(const uint8_t* data, size_t n, CrSet& out) {
    if (!data || n < kCrHeader + kCrEntry) return false;
    std::memcpy(out.opcodes, data + 8, 9);
    size_t count = (n - kCrHeader) / kCrEntry;
    if (count > kCrMaxEntries) count = kCrMaxEntries;
    out.entries.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* e = data + kCrHeader + i * kCrEntry;
        CrEntry& c = out.entries[i];
        std::memcpy(c.seed, e, 16);
        std::memcpy(c.reply, e + 16, 20);
        std::memcpy(c.clientKey, e + 36, 16);
        std::memcpy(c.serverKey, e + 52, 16);
    }
    return true;
}

// ---- ExeImage --------------------------------------------------------------------------------------------------------

bool ExeImage::open(ReadAt fn, void* ctx) {
    fn_ = nullptr;
    sectionCount_ = 0;
    uint8_t h[64];
    if (!fn || !fn(ctx, 0, h, 64) || h[0] != 'M' || h[1] != 'Z') return false;
    const uint32_t pe = le32(h + 0x3C);
    uint8_t coff[24];
    if (!fn(ctx, pe, coff, 24) || coff[0] != 'P' || coff[1] != 'E' || coff[2] || coff[3]) return false;
    const uint16_t numSections = le16(coff + 4 + 2);
    const uint16_t optSize = le16(coff + 4 + 16);
    const uint32_t optOfs = pe + 24;
    uint8_t opt[64];
    if (optSize < 64 || !fn(ctx, optOfs, opt, 64) || le16(opt) != 0x10B) return false;  // PE32
    imageBase_ = le32(opt + 28);
    imageSize_ = le32(opt + 56);
    headerSize_ = le32(opt + 60);
    if (headerSize_ > imageSize_) headerSize_ = imageSize_;
    const uint32_t secOfs = optOfs + optSize;
    for (uint16_t i = 0; i < numSections && sectionCount_ < kMaxSections; ++i) {
        uint8_t s[40];
        if (!fn(ctx, secOfs + i * 40u, s, 40)) return false;
        const uint32_t vsize = le32(s + 8), va = le32(s + 12), rawSize = le32(s + 16), rawOff = le32(s + 20);
        if (rawSize == 0 || rawOff == 0 || va >= imageSize_) continue;  // upstream parsePE: nothing to copy
        uint32_t take = rawSize < vsize ? rawSize : vsize;
        if (take > imageSize_ - va) take = imageSize_ - va;
        sections_[sectionCount_++] = Section{va, take, rawOff};
    }
    fn_ = fn;
    ctx_ = ctx;
    return true;
}

bool ExeImage::read(uint32_t va, uint32_t n, uint8_t* out) const {
    if (!fn_) return false;
    uint32_t rva;
    if (va >= imageBase_) rva = va - imageBase_;  // absolute
    else if (va < imageSize_) rva = va;           // an RVA (upstream accepts both)
    else return false;
    if (static_cast<uint64_t>(rva) + n > imageSize_) return false;
    std::memset(out, 0, n);  // BSS and the gaps: zeros
    if (!overlayFromFile(fn_, ctx_, 0, headerSize_, 0, rva, n, out)) return false;
    for (int i = 0; i < sectionCount_; ++i) {
        const Section& s = sections_[i];
        if (!overlayFromFile(fn_, ctx_, s.va, s.size, s.raw, rva, n, out)) return false;
    }
    return true;
}

bool runtimeOverlay(uint32_t va, uint32_t n, uint8_t* out) {
    bool any = false;
    for (const RuntimeValue& r : kRuntime) overlayValue(r.va, r.bytes, r.len, va, n, out, any);
    return any;
}

bool kuserRead(uint32_t va, uint32_t n, uint8_t* out) {
    if (va < kKuserBase || static_cast<uint64_t>(va) + n > static_cast<uint64_t>(kKuserBase) + kKuserSize) return false;
    std::memset(out, 0, n);
    bool any = false;
    for (const KField& f : kKuser) {
        uint8_t b[4] = {static_cast<uint8_t>(f.v), static_cast<uint8_t>(f.v >> 8), static_cast<uint8_t>(f.v >> 16),
                        static_cast<uint8_t>(f.v >> 24)};
        overlayValue(kKuserBase + f.off, b, f.len, va, n, out, any);
    }
    static const char kRoot[] = "C:\\WINDOWS";  // +0x030 NtSystemRoot, UTF-16
    for (uint32_t i = 0; i < 10; ++i) {
        const uint8_t b[2] = {static_cast<uint8_t>(kRoot[i]), 0};
        overlayValue(kKuserBase + 0x30 + i * 2, b, 2, va, n, out, any);
    }
    return true;
}

bool readClientMemory(const ExeImage& img, uint32_t va, uint8_t n, uint8_t* out) {
    if (n == 0) return true;
    if (kuserRead(va, n, out)) return true;
    if (!img.read(va, n, out)) return false;
    runtimeOverlay(va >= img.imageBase() ? va : va + img.imageBase(), n, out);
    return true;
}

bool knownClientFileHash(const std::string& pathIn, uint8_t sha1[20]) {
    // upstream knownDoorHashes (warden_handler.cpp)
    struct Door {
        const char* path;
        uint8_t h[20];
    };
    static const Door kDoors[] = {
        {"world\\lordaeron\\stratholme\\activedoodads\\doors\\nox_door_plague.m2",
         {0xB4,0x45,0x2B,0x6D,0x95,0xC9,0x8B,0x18,0x6A,0x70,0xB0,0x08,0xFA,0x07,0xBB,0xAE,0xF3,0x0D,0xF7,0xA2}},
        {"world\\kalimdor\\onyxiaslair\\doors\\onyxiasgate01.m2",
         {0x75,0x19,0x5E,0x4A,0xED,0xA0,0xBC,0xAF,0x04,0x8C,0xA0,0xE3,0x4D,0x95,0xA7,0x0D,0x4F,0x53,0xC7,0x46}},
        {"world\\generic\\human\\activedoodads\\doors\\deadminedoor02.m2",
         {0x3D,0xFF,0x01,0x1B,0x9A,0xB1,0x34,0xF3,0x7F,0x88,0x50,0x97,0xE6,0x95,0x35,0x1B,0x91,0x95,0x35,0x64}},
        {"world\\kalimdor\\silithus\\activedoodads\\ahnqirajdoor\\ahnqirajdoor02.m2",
         {0xDB,0xD4,0xF4,0x07,0xC4,0x68,0xCC,0x36,0x13,0x4E,0x62,0x1D,0x16,0x01,0x78,0xFD,0xA4,0xD0,0xD2,0x49}},
        {"world\\kalimdor\\diremaul\\activedoodads\\doors\\diremaulsmallinstancedoor.m2",
         {0x0D,0xC8,0xDB,0x46,0xC8,0x55,0x49,0xC0,0xFF,0x1A,0x60,0x0F,0x6C,0x23,0x63,0x57,0xC3,0x05,0x78,0x1A}},
    };
    std::string p = pathIn;
    for (char& c : p) {
        if (c == '/') c = '\\';
        else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    for (const Door& d : kDoors)
        if (p == d.path) {
            std::memcpy(sha1, d.h, 20);
            return true;
        }
    return false;
}

// ---- Handler ---------------------------------------------------------------------------------------------------------

void Handler::logf(const char* fmt, ...) {
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    env_.log(line);
}

void Handler::begin(const std::vector<uint8_t>& sessionKey) {
    end();
    sessionKey_ = sessionKey;
    state_ = State::WaitModuleUse;
}

void Handler::end() {
    crypto_.reset();
    std::fill(sessionKey_.begin(), sessionKey_.end(), 0);
    sessionKey_.clear();
    state_ = State::Off;
    moduleSize_ = moduleGot_ = 0;
    std::memset(moduleMd5_, 0, sizeof(moduleMd5_));
    cr_ = CrSet{};
    haveCr_ = false;
    stats_ = Stats{};
}

CheckType Handler::decode(uint8_t raw, uint8_t xorByte) const {
    const uint8_t d = raw ^ xorByte;
    for (int i = 0; i < 9; ++i)
        if (d == cr_.opcodes[i]) return static_cast<CheckType>(i);
    return CT_UNKNOWN;
}

void Handler::send(std::vector<uint8_t> plain, std::vector<std::vector<uint8_t>>& out) {
    out.push_back(crypto_->encrypt(plain));
}

bool Handler::buildChecksReply(const uint8_t* req, size_t n, std::vector<uint8_t>& reply) {
    // upstream CHEAT_CHECKS_REQUEST: [0x02][string table: (len, bytes)* 0x00][checks][xorByte]
    if (n < 3) return false;
    size_t pos = 1;
    std::vector<std::string> strings;
    while (pos < n) {
        const uint8_t len = req[pos++];
        if (len == 0) break;
        if (pos + len > n) break;
        strings.emplace_back(reinterpret_cast<const char*>(req + pos), len);
        pos += len;
    }
    const uint8_t xorByte = req[n - 1];
    const size_t end = n - 1;
    auto str = [&](uint8_t idx) -> std::string {
        return idx && static_cast<size_t>(idx - 1) < strings.size() ? strings[idx - 1] : std::string();
    };
    std::vector<uint8_t> data;
    data.reserve(256);
    uint16_t count[CT_UNKNOWN + 1] = {}, ok[CT_UNKNOWN + 1] = {};
    bool stopped = false;
    while (pos < end) {
        const CheckType ct = decode(req[pos++], xorByte);
        ++count[ct];
        ++stats_.checks;
        switch (ct) {
            case CT_TIMING: {  // [result 1][GetTickCount]
                data.push_back(0x01);
                put32(data, env_.tickMs());
                ++ok[ct];
                break;
            }
            case CT_MEM: {  // [string idx][u32 address][u8 length] -> [0x00][bytes] / 0xE9
                if (pos + 6 > end) {
                    pos = end;
                    break;
                }
                const uint32_t addr = le32(req + pos + 1);
                const uint8_t len = req[pos + 5];
                pos += 6;
                uint8_t buf[255];
                if (env_.readMemory(addr, len, buf)) {
                    data.push_back(0x00);
                    data.insert(data.end(), buf, buf + len);
                    ++ok[ct];
                } else {
                    data.push_back(kNotFound);  // upstream WARDEN_MEM_CHECK_UNMAPPED
                    ++stats_.failedReads;
                    logf("warden_mem_unmapped addr=%08X len=%u", static_cast<unsigned>(addr), static_cast<unsigned>(len));
                }
                break;
            }
            case CT_PAGE_A:
            case CT_PAGE_B: {  // [4 seed][20 sha1][u32 offset][u8 length]: no image scan on the PSP - not found
                if (pos + 29 > end) {
                    pos = end;
                    data.push_back(kNotFound);
                    break;
                }
                pos += 29;
                data.push_back(kNotFound);
                ++ok[ct];
                break;
            }
            case CT_MPQ: {  // [string idx] -> [0x00][sha1] / 0x01
                if (pos + 1 > end) {
                    pos = end;
                    break;
                }
                const std::string path = str(req[pos++]);
                uint8_t h[20];
                if (!path.empty() && env_.hashClientFile(path, h)) {
                    data.push_back(0x00);
                    data.insert(data.end(), h, h + 20);
                    ++ok[ct];
                } else {
                    data.push_back(0x01);
                    logf("warden_mpq_missing file=%.80s", path.c_str());
                }
                break;
            }
            case CT_LUA: {  // [string idx] -> 0x01 (upstream: the variable is not there)
                if (pos + 1 > end) {
                    pos = end;
                    break;
                }
                ++pos;
                data.push_back(0x01);
                ++ok[ct];
                break;
            }
            case CT_DRIVER: {  // [24 data][string idx] -> not found
                if (pos + 25 > end) {
                    pos = end;
                    break;
                }
                pos += 25;
                data.push_back(kNotFound);
                ++ok[ct];
                break;
            }
            case CT_MODULE: {  // [4 seed][20 hmac of the name] -> found for the system DLLs, else not found
                if (pos + 24 > end) {
                    pos = end;
                    data.push_back(kNotFound);
                    break;
                }
                const uint8_t* p = req + pos;
                pos += 24;
                bool loaded = false;
                for (const char* name : kLoadedModules)
                    if (hmacMatches(p, name, p + 4)) loaded = true;
                data.push_back(loaded ? kFound : kNotFound);
                ++ok[ct];
                break;
            }
            case CT_PROC: {  // [4 seed][20 sha1][idx][idx][u32 offset] -> 0x01 (upstream)
                if (pos + 30 > end) {
                    pos = end;
                    break;
                }
                pos += 30;
                data.push_back(0x01);
                ++ok[ct];
                break;
            }
            default:  // upstream: an unknown type ends the parse
                logf("warden_check_unknown raw=%02X xor=%02X pos=%u", req[pos - 1], xorByte, static_cast<unsigned>(pos - 1));
                pos = end;
                stopped = true;
                break;
        }
    }
    for (int t = 0; t <= CT_UNKNOWN; ++t)
        if (count[t])
            logf("warden_check type=%s n=%u ok=%u", checkTypeName(static_cast<CheckType>(t)),
                 static_cast<unsigned>(count[t]), static_cast<unsigned>(ok[t]));
    // [0x02][u16 length][u32 checksum = XOR of the five words of SHA1(data)][data]
    uint8_t h[20];
    Sha1Stream::digest(data.data(), data.size(), h);
    uint32_t checksum = 0;
    for (int i = 0; i < 5; ++i) checksum ^= le32(h + i * 4);
    reply.clear();
    reply.reserve(7 + data.size());
    reply.push_back(0x02);
    reply.push_back(static_cast<uint8_t>(data.size()));
    reply.push_back(static_cast<uint8_t>(data.size() >> 8));
    put32(reply, checksum);
    reply.insert(reply.end(), data.begin(), data.end());
    return !stopped;
}

void Handler::onServerData(const uint8_t* data, size_t n, std::vector<std::vector<uint8_t>>& out) {
    if (state_ == State::Off || !data || n == 0) return;
    ++stats_.packets;
    if (!crypto_) {  // upstream: the crypto from the session key on the first packet
        crypto_ = std::make_unique<game::WardenCrypto>();
        if (sessionKey_.size() != 40 || !crypto_->initFromSessionKey(sessionKey_)) {
            logf("warden state=%s op=none err=session_key bytes=%u", stateName(state_),
                 static_cast<unsigned>(sessionKey_.size()));
            crypto_.reset();
            state_ = State::Off;
            return;
        }
    }
    const std::vector<uint8_t> dec = crypto_->decrypt(std::vector<uint8_t>(data, data + n));
    const uint8_t op = dec[0];
    switch (op) {
        case 0x00: {  // MODULE_USE [md5 16][key 16][u32 size]
            if (dec.size() < 37) break;
            std::memcpy(moduleMd5_, dec.data() + 1, 16);
            moduleSize_ = le32(dec.data() + 33);
            moduleGot_ = 0;
            char md5[33];
            toHex(moduleMd5_, 16, md5);
            cr_ = CrSet{};
            haveCr_ = env_.loadCr(md5, cr_);
            logf("warden state=%s op=module_use md5=%s size=%u cr=%d", stateName(state_), md5,
                 static_cast<unsigned>(moduleSize_), haveCr_ ? 1 : 0);
            send({0x00}, out);  // MODULE_MISSING: the module comes again (upstream; nothing is cached)
            state_ = State::WaitModuleCache;
            break;
        }
        case 0x01: {  // MODULE_CACHE [u16 size][bytes]: counted, not kept
            if (dec.size() < 3) break;
            const uint16_t chunk = le16(dec.data() + 1);
            if (dec.size() < 3u + chunk) break;
            if (moduleGot_ == 0) logf("warden state=%s op=module_cache first=%u", stateName(state_), chunk);
            moduleGot_ += chunk;
            if (state_ == State::WaitModuleCache && moduleGot_ >= moduleSize_) {
                send({0x01}, out);  // MODULE_OK
                state_ = State::WaitHashRequest;
                logf("warden state=%s op=module_cache done=%u", stateName(state_), static_cast<unsigned>(moduleGot_));
            }
            break;
        }
        case 0x05: {  // HASH_REQUEST [seed 16]
            if (dec.size() < 17) break;
            const CrEntry* match = nullptr;
            if (haveCr_)
                for (const CrEntry& e : cr_.entries)
                    if (std::memcmp(e.seed, dec.data() + 1, 16) == 0) match = &e;
            if (!match) {
                // upstream (3.3.5a): no answer without a .cr entry - AzerothCore then sends no checks at all, a wrong
                // hash would be a penalty.
                char md5[33], seed[33];
                toHex(moduleMd5_, 16, md5);
                toHex(dec.data() + 1, 16, seed);
                logf("warden_unknown md5=%s seed=%s cr=%d", md5, seed, haveCr_ ? 1 : 0);
                state_ = State::Silent;
                break;
            }
            std::vector<uint8_t> r{0x04};
            r.insert(r.end(), match->reply, match->reply + 20);
            send(std::move(r), out);  // HASH_RESULT under the old keys, then the module's key pair
            crypto_->replaceKeys(std::vector<uint8_t>(match->clientKey, match->clientKey + 16),
                                 std::vector<uint8_t>(match->serverKey, match->serverKey + 16));
            state_ = State::Checks;
            stats_.hashAnswered = true;
            logf("warden state=%s op=hash_request answered=1", stateName(state_));
            break;
        }
        case 0x03:  // MODULE_INITIALIZE: no answer (upstream)
            logf("warden state=%s op=module_initialize len=%u", stateName(state_), static_cast<unsigned>(dec.size()));
            break;
        case 0x02: {  // CHEAT_CHECKS_REQUEST
            if (state_ != State::Checks) {
                logf("warden state=%s op=cheat_checks skipped=1", stateName(state_));
                break;
            }
            std::vector<uint8_t> reply;
            buildChecksReply(dec.data(), dec.size(), reply);
            ++stats_.rounds;
            logf("warden state=%s op=cheat_checks round=%u reply=%u", stateName(state_),
                 static_cast<unsigned>(stats_.rounds), static_cast<unsigned>(reply.size()));
            send(std::move(reply), out);
            break;
        }
        default:
            logf("warden state=%s op=%02X len=%u unknown=1", stateName(state_), op, static_cast<unsigned>(dec.size()));
            break;
    }
}

}  // namespace wowee::platform::psp::warden
