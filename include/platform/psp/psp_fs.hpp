// PSP filesystem helpers.
//
// The current directory is a property of the THREAD on the PSP: threads made
// with sceKernelCreateThread have none (0x8002032C NOCWD). Everything outside
// the main thread must use absolute paths, so the game directory is captured
// once from argv[0] and every path is built from it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace wowee::platform::psp {

// Captures the directory holding EBOOT.PBP ("ms0:/PSP/GAME/WOWEEPSP/").
bool fsInit(const char* argv0);
const char* fsBaseDir();
// The base dir without its device ("/PSP/GAME/WOWEEPSP/"), for std::filesystem:
// libstdc++ does not treat "ms0:/..." as absolute and prepends the cwd
// (weakly_canonical then throws), while pspsdk resolves "/..." on the cwd's
// device. Use this for any path handed to upstream code that uses <filesystem>.
std::string fsBaseDirPosix();

// base dir + relative path into out. Returns false when it did not fit.
bool fsPath(char* out, size_t cap, const char* rel);

bool fsExists(const char* absPath);
bool fsIsDir(const char* absPath);
int64_t fsFileSize(const char* absPath);  // -1 when missing

} // namespace wowee::platform::psp
