// PSP: Warden without the x86 module (docs/PSP_PORT_ARCHITECTURE.md, docs/WARDEN_PSP.md plan (c)).
//
// Upstream (src/game/warden_handler.cpp) builds every answer itself and runs the downloaded module under Unicorn only
// to call its entry point - the result is never used. The PSP keeps the answers and drops the emulator: this is
// upstream's WardenHandler::handleWardenData state machine for 3.3.5a (build 12340) over upstream's WardenCrypto
// (SHA1Randx + RC4), with the memory checks read from the client executable on the card by its section table (no image).
// No PSP calls here - the host test (tools/psp/host_tests/warden_host_test.sh) runs it against a simulated
// AzerothCore (Warden.cpp / WardenWin.cpp) and the client executable. RAM: two RC4 states, the .cr entries, the section
// table and one reply (< 2 KB besides the packet itself).
//
// Differences from upstream, all PSP-forced or AzerothCore-checked:
//  - the module is counted, not kept (no Unicorn, no image): MODULE_MISSING -> chunks -> MODULE_OK as upstream;
//  - PAGE_A / PAGE_B / DRIVER / MODULE "not found" = 0xE9 - what AzerothCore / TrinityCore WardenWin::HandleData
//    accept as a pass (upstream sends 0x00); "found" stays upstream's 0x4A (upstream's known wanted patterns / DLLs);
//    no HMAC brute-force scan of the image (7.7 MB on the card - seconds per check);
//  - MPQ: upstream's known door hashes only (no MPQs on the card), else 0x01 not found;
//  - the 1.12.1 / Turtle paths (fallback hash, VMaNGOS runtime globals, Turtle patches) are not ported.
#pragma once

#include "game/warden_crypto.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wowee::platform::psp::warden {

// ---- .cr: pre-computed HASH_REQUEST answers (upstream WardenHandler::loadWardenCRFile) --------------------------------
// [4 memoryRead][4 pageScanCheck][9 check opcodes] then 68-byte entries [16 seed][20 reply][16 clientKey][16 serverKey].
constexpr size_t kCrHeader = 17;
constexpr size_t kCrEntry = 68;
constexpr size_t kCrMaxEntries = 64;
// Check opcode order of the header (CMaNGOS WindowsScanType): MEM MODULE PAGE_A PAGE_B MPQ LUA PROC DRIVER TIMING.
enum CheckType : uint8_t { CT_MEM, CT_MODULE, CT_PAGE_A, CT_PAGE_B, CT_MPQ, CT_LUA, CT_PROC, CT_DRIVER, CT_TIMING, CT_UNKNOWN };
const char* checkTypeName(CheckType t);

struct CrEntry {
    uint8_t seed[16];
    uint8_t reply[20];
    uint8_t clientKey[16];
    uint8_t serverKey[16];
};
struct CrSet {
    uint8_t opcodes[9] = {};
    std::vector<CrEntry> entries;
};
bool parseCr(const uint8_t* data, size_t n, CrSet& out);

// ---- the client executable through its section table ---------------------------------------------------------------
// The client's memory as upstream's WardenMemory flattens it (SizeOfImage zeros, the headers, min(raw, virtual) bytes of
// each section), but read from the file piece by piece: a read touches only the file bytes it needs.
using ReadAt = bool (*)(void* ctx, uint32_t fileOffset, uint8_t* out, uint32_t n);
class ExeImage {
public:
    bool open(ReadAt fn, void* ctx);  // parses the PE32 headers (a few hundred bytes)
    bool isOpen() const { return fn_ != nullptr; }
    void close() { fn_ = nullptr; }
    // va: absolute (>= ImageBase) or an RVA, as upstream. False when outside the image or a file read failed.
    bool read(uint32_t va, uint32_t n, uint8_t* out) const;
    uint32_t imageBase() const { return imageBase_; }
    uint32_t imageSize() const { return imageSize_; }
    int sectionCount() const { return sectionCount_; }

private:
    struct Section {
        uint32_t va, size, raw;  // RVA, bytes taken from the file (min(raw, virtual)), file offset
    };
    static constexpr int kMaxSections = 16;
    ReadAt fn_ = nullptr;
    void* ctx_ = nullptr;
    uint32_t imageBase_ = 0, imageSize_ = 0, headerSize_ = 0;
    Section sections_[kMaxSections] = {};
    int sectionCount_ = 0;
};

// The bytes a running 12340 client holds where its file holds something else (docs/WARDEN_PSP.md: of the 34 memory
// checks of AzerothCore's warden_checks, 33 are the file's bytes, one is the "login state" of a client in the world),
// and the KUSER_SHARED_DATA page upstream fakes (Windows 7 SP1 x86). Overlaid on the image's bytes.
bool runtimeOverlay(uint32_t va, uint32_t n, uint8_t* out);  // true when any byte came from the table
bool kuserRead(uint32_t va, uint32_t n, uint8_t* out);       // false outside 0x7FFE0000..+0x1000
// The memory check's read: KUSER, else the image with the runtime table on top. False = unmapped (0xE9).
bool readClientMemory(const ExeImage& img, uint32_t va, uint8_t n, uint8_t* out);

// ---- the platform half -----------------------------------------------------------------------------------------------
class Env {
public:
    virtual ~Env() = default;
    // The .cr of a module (md5 as 32 lowercase hex digits); false when there is none.
    virtual bool loadCr(const char* md5hex, CrSet& out) = 0;
    // MEM_CHECK: the client's memory (readClientMemory over its executable); false = unmapped / no executable.
    virtual bool readMemory(uint32_t va, uint8_t n, uint8_t* out) = 0;
    // MPQ check: SHA1 of a client data file; false = not found.
    virtual bool hashClientFile(const std::string& path, uint8_t sha1[20]) = 0;
    virtual uint32_t tickMs() = 0;  // TIMING: GetTickCount()'s role
    virtual void log(const char* line) = 0;
};

// Upstream's known MPQ answers (knownDoorHashes) - the PSP has no MPQs to hash.
bool knownClientFileHash(const std::string& path, uint8_t sha1[20]);

// ---- the state machine -----------------------------------------------------------------------------------------------
enum class State : uint8_t { Off, WaitModuleUse, WaitModuleCache, WaitHashRequest, Checks, Silent };
const char* stateName(State s);

class Handler {
public:
    explicit Handler(Env& env) : env_(env) {}
    // A new world session: K is the 40-byte SRP session key (the RC4 keys come from it on the first packet).
    void begin(const std::vector<uint8_t>& sessionKey);
    void end();
    // One SMSG_WARDEN_DATA body as received (encrypted). Each reply (a CMSG_WARDEN_DATA body, encrypted) goes to out.
    // Every packet is decrypted whatever the state: a skipped one would leave the RC4 stream out of step for the rest
    // of the session.
    void onServerData(const uint8_t* data, size_t n, std::vector<std::vector<uint8_t>>& out);
    State state() const { return state_; }

    struct Stats {
        uint32_t packets = 0, rounds = 0, checks = 0, failedReads = 0;
        bool hashAnswered = false;
    };
    const Stats& stats() const { return stats_; }

    // The reply body of CHEAT_CHECKS_REQUEST for a decrypted request (exposed for the host test).
    bool buildChecksReply(const uint8_t* req, size_t n, std::vector<uint8_t>& reply);

private:
    void logf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void send(std::vector<uint8_t> plain, std::vector<std::vector<uint8_t>>& out);
    CheckType decode(uint8_t raw, uint8_t xorByte) const;

    Env& env_;
    std::vector<uint8_t> sessionKey_;
    std::unique_ptr<game::WardenCrypto> crypto_;
    State state_ = State::Off;
    uint8_t moduleMd5_[16] = {};
    uint32_t moduleSize_ = 0, moduleGot_ = 0;
    CrSet cr_;
    bool haveCr_ = false;
    Stats stats_;
};

void toHex(const uint8_t* p, size_t n, char* out);  // lowercase, out holds 2n + 1

}  // namespace wowee::platform::psp::warden
