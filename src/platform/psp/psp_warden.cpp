// PSP: Warden on the world session - the card half of psp_warden_core.hpp.
//
// SMSG_WARDEN_DATA is taken in every state, before any other handler (was: dropped - the server's MODULE_USE comes
// right after SMSG_AUTH_RESPONSE, and a packet not decrypted leaves the RC4 stream out of step for the session).
// The player's own files on the card, never in git:
//   data/client/Wow.exe                         - the 12340 client the memory checks read (<= 255 bytes a check)
//   data/client/warden_cache/<module md5>.cr    - the HASH_REQUEST answers (tools/psp/make_warden_cr.py, AzerothCore)
// No .cr for the server's module: silence on HASH_REQUEST (upstream), "warden_unknown md5= seed=" in the log - the
// server then sends no checks. No client executable: the memory checks answer 0xE9 (unmapped), logged.
#include "platform/psp/psp_world_session.hpp"

#include "game/opcode_table.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_warden_core.hpp"

#include <pspiofilemgr.h>
#include <pspthreadman.h>

#include <cstdio>
#include <cstring>

namespace wowee::platform::psp {

namespace wd = warden;

namespace {

class CardEnv final : public wd::Env {
public:
    ~CardEnv() override { closeExe(); }

    bool loadCr(const char* md5hex, wd::CrSet& out) override {
        char rel[96], path[256];
        std::snprintf(rel, sizeof(rel), "data/client/warden_cache/%s.cr", md5hex);
        if (!fsPath(path, sizeof(path), rel)) return false;
        const SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
        if (fd < 0) return false;
        uint8_t buf[wd::kCrHeader + wd::kCrEntry * 8];  // AzerothCore's .cr has one entry
        const int n = sceIoRead(fd, buf, sizeof(buf));
        sceIoClose(fd);
        return n > 0 && wd::parseCr(buf, static_cast<size_t>(n), out);
    }

    bool readMemory(uint32_t va, uint8_t n, uint8_t* out) override {
        if (!openExe()) return false;
        if (wd::readClientMemory(img_, va, n, out)) return true;
        // A suspended PSP closes the card's descriptors: once more with a new one.
        closeExe();
        return openExe() && wd::readClientMemory(img_, va, n, out);
    }

    bool hashClientFile(const std::string& path, uint8_t sha1[20]) override {
        return wd::knownClientFileHash(path, sha1);
    }

    uint32_t tickMs() override { return static_cast<uint32_t>(sceKernelGetSystemTimeWide() / 1000u); }

    void log(const char* line) override { PSP_LOGI(tag::Net, "%s", line); }

    void closeExe() {
        if (fd_ >= 0) sceIoClose(fd_);
        fd_ = -1;
        img_.close();
    }

private:
    static bool readAt(void* ctx, uint32_t off, uint8_t* out, uint32_t n) {
        const SceUID fd = *static_cast<SceUID*>(ctx);
        if (sceIoLseek32(fd, static_cast<int>(off), PSP_SEEK_SET) != static_cast<int>(off)) return false;
        return sceIoRead(fd, out, n) == static_cast<int>(n);
    }

    bool openExe() {
        if (img_.isOpen()) return true;
        if (exeMissingLogged_) return false;
        char path[256];
        if (!fsPath(path, sizeof(path), "data/client/Wow.exe")) return false;
        fd_ = sceIoOpen(path, PSP_O_RDONLY, 0);
        if (fd_ < 0 || !img_.open(&CardEnv::readAt, &fd_)) {
            PSP_LOGW(tag::Net, "warden_exe missing=1 path=data/client/Wow.exe (memory checks answer unmapped)");
            if (fd_ >= 0) sceIoClose(fd_);
            fd_ = -1;
            exeMissingLogged_ = true;
            return false;
        }
        PSP_LOGI(tag::Net, "warden_exe base=%08X image=%08X sections=%d", static_cast<unsigned>(img_.imageBase()),
                 static_cast<unsigned>(img_.imageSize()), img_.sectionCount());
        return true;
    }

    SceUID fd_ = -1;
    wd::ExeImage img_;
    bool exeMissingLogged_ = false;
};

}  // namespace

struct WorldSession::WardenState {
    CardEnv env;
    wd::Handler handler{env};
};

void WorldSession::wardenBegin() {
    if (!warden_) warden_ = std::make_shared<WardenState>();
    warden_->env.closeExe();
    warden_->handler.begin(sessionKey_);
}

void WorldSession::wardenEnd() {
    if (!warden_) return;
    const auto& st = warden_->handler.stats();
    if (st.packets)
        PSP_LOGI(tag::Net, "warden_end state=%s packets=%u rounds=%u checks=%u failed_reads=%u hash=%d",
                 wd::stateName(warden_->handler.state()), static_cast<unsigned>(st.packets),
                 static_cast<unsigned>(st.rounds), static_cast<unsigned>(st.checks),
                 static_cast<unsigned>(st.failedReads), st.hashAnswered ? 1 : 0);
    warden_->handler.end();
    warden_->env.closeExe();
}

void WorldSession::handleWardenData(network::Packet& packet) {
    if (!warden_ || !socket_) return;
    const std::vector<uint8_t>& body = packet.getData();
    std::vector<std::vector<uint8_t>> replies;
    warden_->handler.onServerData(body.data(), body.size(), replies);
    for (const auto& r : replies) {
        network::Packet out(game::wireOpcode(game::LogicalOpcode::CMSG_WARDEN_DATA));
        out.writeBytes(r.data(), r.size());
        socket_->send(out);
    }
    packet.skipAll();
}

}  // namespace wowee::platform::psp
