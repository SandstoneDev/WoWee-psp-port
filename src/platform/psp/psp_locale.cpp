// the client's languages - see include/platform/psp/psp_locale.hpp.
#include "platform/psp/psp_locale.hpp"

#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_locale_rules.hpp"
#include "platform/psp/psp_log.hpp"

#include <pspiofilemgr.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

namespace wowee::platform::psp {

namespace lc = locale;

namespace {

constexpr int kMaxFiles = 512;  // data/locale/<xxYY>/ holds ~20 paks; a stray copy of data/ stops here
constexpr int kMaxDepth = 2;    // <name> and <sub>/<name> (e.g. movies/<name>.sbt)

lc::LocaleSet g_set;
std::string g_active;
std::vector<std::string> g_installed;
// The running language's folder: lower-case keys (sorted, lc::overrideIndex) and the names as the card has them.
std::vector<std::string> g_keys;
std::vector<std::string> g_names;
std::vector<unsigned char> g_logged;  // "locale_use" once per file
bool g_restart = false;
std::string g_fontPak;  // the ui.pak the fonts came from when the running one's lack Cyrillic

bool readSmallFile(const char* path, std::string& text) {
    const SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return false;
    char buf[512];
    int n;
    while ((n = sceIoRead(fd, buf, sizeof(buf))) > 0 && text.size() < 16 * 1024)
        text.append(buf, static_cast<size_t>(n));
    sceIoClose(fd);
    return true;
}

// Every file under dir (absolute, ending in '/'), as "name" / "sub/name".
void listFiles(const std::string& dir, const std::string& prefix, int depth, std::vector<std::string>& out) {
    const SceUID d = sceIoDopen(dir.c_str());
    if (d < 0) return;
    SceIoDirent e;
    for (;;) {
        std::memset(&e, 0, sizeof(e));  // the firmware reads d_private: it must be 0
        if (sceIoDread(d, &e) <= 0) break;
        if (e.d_name[0] == '.' || static_cast<int>(out.size()) >= kMaxFiles) continue;
        if (FIO_S_ISDIR(e.d_stat.st_mode)) {
            if (depth + 1 < kMaxDepth) listFiles(dir + e.d_name + "/", prefix + e.d_name + "/", depth + 1, out);
        } else {
            out.push_back(prefix + e.d_name);
        }
    }
    sceIoDclose(d);
}

bool dirExists(const std::string& code) {
    char path[256];
    std::snprintf(path, sizeof(path), "%sdata/locale/%s", fsBaseDir(), code.c_str());
    return fsIsDir(path);
}

} // namespace

void localeInit() {
    const std::string cfg = configGet("locale", "");
    char path[256];
    std::snprintf(path, sizeof(path), "%sdata/locales.txt", fsBaseDir());
    std::string text, warn;
    const bool hadFile = readSmallFile(path, text);
    if (!hadFile || !lc::parseList(text, cfg, g_set, &warn)) g_set = lc::primaryOnly(cfg);
    if (!warn.empty()) PSP_LOGW(tag::Fs, "locale_list file=%d ignored=%s", hadFile ? 1 : 0, warn.c_str());
    // An extra without its folder on the card (a copy cut short, a folder deleted by hand) is not offered.
    std::vector<std::string> extras;
    for (const std::string& e : g_set.extras) {
        if (dirExists(e)) extras.push_back(e);
        else PSP_LOGW(tag::Fs, "locale_dir_missing code=%s (not offered)", e.c_str());
    }
    g_set.extras = extras;
    if (!g_set.def.empty() && !lc::isInstalled(g_set, g_set.def)) g_set.def.clear();
    g_installed = lc::installed(g_set);
    lc::Source src = lc::Source::Primary;
    g_active = lc::choose(g_set, cfg, &src);

    std::string ex;
    for (const std::string& e : g_set.extras) ex += (ex.empty() ? "" : ",") + e;
    PSP_LOGI(tag::Fs, "locale active=%s primary=%s extras=%s src=%s default=%s file=%d cfg=%s", g_active.c_str(),
             g_set.primary.c_str(), ex.empty() ? "-" : ex.c_str(), lc::sourceName(src),
             g_set.def.empty() ? "-" : g_set.def.c_str(), g_set.fromFile ? 1 : 0, cfg.empty() ? "-" : cfg.c_str());

    g_keys.clear();
    g_names.clear();
    if (g_active == g_set.primary) return;  // data/ as it is
    std::vector<std::string> files;
    listFiles(std::string(fsBaseDir()) + "data/locale/" + g_active + "/", "", 0, files);
    std::vector<std::pair<std::string, std::string>> byKey;
    for (const std::string& f : files) byKey.emplace_back(lc::lowerCopy(f), f);
    std::sort(byKey.begin(), byKey.end());
    byKey.erase(std::unique(byKey.begin(), byKey.end(),
                            [](const auto& a, const auto& b) { return a.first == b.first; }),
                byKey.end());
    for (auto& k : byKey) {
        g_keys.push_back(std::move(k.first));
        g_names.push_back(std::move(k.second));
    }
    g_logged.assign(g_keys.size(), 0);
    PSP_LOGI(tag::Fs, "locale_files dir=data/locale/%s n=%u", g_active.c_str(), static_cast<unsigned>(g_names.size()));
    // The overrides, once: as many names a line as fit the log's 120 characters.
    std::string line;
    for (size_t i = 0; i <= g_names.size(); ++i) {
        if (i == g_names.size() || line.size() + g_names[i].size() + 1 > 84) {
            if (!line.empty()) PSP_LOGI(tag::Fs, "locale_files %s", line.c_str());
            line.clear();
        }
        if (i < g_names.size()) line += (line.empty() ? "" : ",") + g_names[i];
    }
    if (g_names.empty()) PSP_LOGW(tag::Fs, "locale_dir_empty code=%s (the primary's paks are used)", g_active.c_str());
}

const std::string& localeActive() { return g_active; }
const std::string& localePrimary() { return g_set.primary; }
const std::vector<std::string>& localeInstalled() { return g_installed; }

std::string localePending() { return lc::choose(g_set, configGet("locale", ""), nullptr); }

bool localeResolve(const char* rel, char* out, size_t cap) {
    const int i = lc::overrideIndex(g_keys, rel);
    if (i < 0) return false;
    const int n = std::snprintf(out, cap, "%sdata/locale/%s/%s", fsBaseDir(), g_active.c_str(), g_names[i].c_str());
    if (n <= 0 || static_cast<size_t>(n) >= cap) return false;  // too long: the primary's file (fsPath as before)
    if (!g_logged[i]) {
        g_logged[i] = 1;
        PSP_LOGI(tag::Fs, "locale_use name=%s", g_names[i].c_str());
    }
    return true;
}

bool localePakPath(const std::string& code, const char* name, char* out, size_t cap) {
    const int n = code == g_set.primary
                      ? std::snprintf(out, cap, "%sdata/%s", fsBaseDir(), name)
                      : std::snprintf(out, cap, "%sdata/locale/%s/%s", fsBaseDir(), code.c_str(), name);
    return n > 0 && static_cast<size_t>(n) < cap;
}

void localeSetFontPak(const char* absPath) { g_fontPak = absPath ? absPath : ""; }
const char* localeFontPak() { return g_fontPak.c_str(); }

bool localePick(const std::string& code) {
    if (!lc::isInstalled(g_set, code)) return false;
    const std::string had = configGet("locale", "");
    if (had != code) configSet("locale", code);
    PSP_LOGI(tag::Fs, "locale_pick code=%s running=%s cfg_was=%s", code.c_str(), g_active.c_str(),
             had.empty() ? "-" : had.c_str());
    return code != g_active;
}

void localeRequestRestart() {
    g_restart = true;
    PSP_LOGI(tag::Exit, "locale_restart to=%s", localePending().c_str());
    logFlush();
}

bool localeRestartRequested() { return g_restart; }

} // namespace wowee::platform::psp
