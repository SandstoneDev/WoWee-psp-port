#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_locale.hpp"  // data/locale/<xxYY>/

#include <pspiofilemgr.h>

#include <cstdio>
#include <cstring>

namespace wowee::platform::psp {

namespace {
char g_base[256] = "ms0:/PSP/GAME/WOWEEPSP/";
}

bool fsInit(const char* argv0) {
    if (!argv0 || !*argv0) return false;
    const char* slash = std::strrchr(argv0, '/');
    if (!slash) return false;
    size_t n = static_cast<size_t>(slash - argv0) + 1;
    if (n >= sizeof(g_base)) return false;
    std::memcpy(g_base, argv0, n);
    g_base[n] = '\0';
    return true;
}

const char* fsBaseDir() { return g_base; }

std::string fsBaseDirPosix() {
    const char* colon = std::strchr(g_base, ':');
    return colon ? std::string(colon + 1) : std::string(g_base);
}

bool fsPath(char* out, size_t cap, const char* rel) {
    // data/<name> of a language other than data/'s = data/locale/<xxYY>/<name> when that folder
    // has it - every pak path is built here, so the paks opened later at run time follow too.
    if (localeResolve(rel, out, cap)) return true;
    int n =std::snprintf(out, cap, "%s%s", g_base, rel ? rel : "");
    return n > 0 && static_cast<size_t>(n) < cap;
}

bool fsExists(const char* absPath) {
    SceIoStat st;
    return sceIoGetstat(absPath, &st) >= 0;
}

bool fsIsDir(const char* absPath) {
    SceIoStat st;
    if (sceIoGetstat(absPath, &st) < 0) return false;
    return FIO_S_ISDIR(st.st_mode);
}

int64_t fsFileSize(const char* absPath) {
    SceIoStat st;
    if (sceIoGetstat(absPath, &st) < 0) return -1;
    return static_cast<int64_t>(st.st_size);
}

} // namespace wowee::platform::psp
