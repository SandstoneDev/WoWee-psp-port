// PSP: the logon integrity hash from the card - psp_integrity.hpp.
#include "platform/psp/psp_integrity.hpp"

#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"

#include <pspiofilemgr.h>
#include <pspthreadman.h>

#include <cstring>
#include <memory>

namespace wowee::platform::psp {

namespace {

constexpr const char* kExeRel = "data/client/Wow.exe";
constexpr const char* kCacheRel = "data/client/Wow.exe.hmac";
constexpr uint32_t kCacheMagic = 0x31484D57u;  // "WMH1"
constexpr int kBufBytes = 16 * 1024;

// One cached checksum: the salt it was made with and the file it was made from (size + modification time).
struct CacheRecord {
    uint32_t magic;
    uint32_t size;
    uint8_t mtime[16];  // ScePspDateTime of st_mtime as stored by the card
    uint8_t salt[16];
    uint8_t checksum[20];
};

static_assert(sizeof(ScePspDateTime) == sizeof(CacheRecord::mtime), "the card's time stamp is 16 bytes");

bool g_mem = false;
CacheRecord g_memRec{};

bool sameFile(const CacheRecord& r, const SceIoStat& st, const std::array<uint8_t, 16>& salt) {
    return r.magic == kCacheMagic && r.size == static_cast<uint32_t>(st.st_size) &&
           std::memcmp(r.mtime, &st.sce_st_mtime, sizeof(r.mtime)) == 0 &&
           std::memcmp(r.salt, salt.data(), 16) == 0;
}

int readCard(void* ctx, uint8_t* buf, int cap) {
    const SceUID fd = *static_cast<SceUID*>(ctx);
    return sceIoRead(fd, buf, static_cast<SceSize>(cap));
}

}  // namespace

bool integrityHash(const std::array<uint8_t, 16>& salt, const std::vector<uint8_t>& clientPublicKeyA,
                   std::array<uint8_t, 20>& outHash, std::string& outError) {
    char exePath[256], cachePath[256];
    if (!fsPath(exePath, sizeof(exePath), kExeRel) || !fsPath(cachePath, sizeof(cachePath), kCacheRel)) {
        outError = "path";
        return false;
    }
    SceIoStat st;
    std::memset(&st, 0, sizeof(st));
    if (sceIoGetstat(exePath, &st) < 0) {
        outError = std::string("missing: ") + kExeRel;
        PSP_LOGW(tag::Net, "integrity ok=0 reason=missing path=%s", kExeRel);
        return false;
    }
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    // 1. This run already made it.
    if (g_mem && sameFile(g_memRec, st, salt)) {
        integrityFinal(clientPublicKeyA, g_memRec.checksum, outHash);
        PSP_LOGI(tag::Net, "integrity ok=1 src=memory size=%u", static_cast<unsigned>(st.st_size));
        return true;
    }
    // 2. A previous run did (the salt of a server is usually constant - AzerothCore's is).
    {
        const SceUID fd = sceIoOpen(cachePath, PSP_O_RDONLY, 0);
        if (fd >= 0) {
            CacheRecord r{};
            const int n = sceIoRead(fd, &r, sizeof(r));
            sceIoClose(fd);
            if (n == static_cast<int>(sizeof(r)) && sameFile(r, st, salt)) {
                g_memRec = r;
                g_mem = true;
                integrityFinal(clientPublicKeyA, r.checksum, outHash);
                PSP_LOGI(tag::Net, "integrity ok=1 src=cache size=%u ms=%u", static_cast<unsigned>(st.st_size),
                         static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u));
                return true;
            }
        }
    }
    // 3. Streamed from the card.
    SceUID fd = sceIoOpen(exePath, PSP_O_RDONLY, 0);
    if (fd < 0) {
        outError = std::string("open failed: ") + kExeRel;
        PSP_LOGW(tag::Net, "integrity ok=0 reason=open path=%s", kExeRel);
        return false;
    }
    std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[kBufBytes]);
    if (!buf) {
        sceIoClose(fd);
        outError = "no memory";
        PSP_LOGW(tag::Net, "integrity ok=0 reason=nomem");
        return false;
    }
    CacheRecord r{};
    uint64_t bytes = 0;
    const bool ok = integrityChecksumStream(salt, &readCard, &fd, buf.get(), kBufBytes, r.checksum, &bytes);
    sceIoClose(fd);
    if (!ok || bytes != static_cast<uint64_t>(st.st_size)) {
        outError = std::string("read failed: ") + kExeRel;
        PSP_LOGW(tag::Net, "integrity ok=0 reason=read bytes=%u size=%u", static_cast<unsigned>(bytes),
                 static_cast<unsigned>(st.st_size));
        return false;
    }
    r.magic = kCacheMagic;
    r.size = static_cast<uint32_t>(st.st_size);
    std::memcpy(r.mtime, &st.sce_st_mtime, sizeof(r.mtime));
    std::memcpy(r.salt, salt.data(), 16);
    g_memRec = r;
    g_mem = true;
    integrityFinal(clientPublicKeyA, r.checksum, outHash);
    // The cache next to the file (a failed write only costs the next login the stream again).
    const SceUID out = sceIoOpen(cachePath, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    bool cached = false;
    if (out >= 0) {
        cached = sceIoWrite(out, &r, sizeof(r)) == static_cast<int>(sizeof(r));
        sceIoClose(out);
    }
    PSP_LOGI(tag::Net, "integrity ok=1 src=stream size=%u ms=%u cached=%d", static_cast<unsigned>(st.st_size),
             static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u), cached ? 1 : 0);
    return true;
}

}  // namespace wowee::platform::psp
