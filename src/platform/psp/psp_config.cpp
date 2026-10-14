#include "platform/psp/psp_config.hpp"

#include <pspiofilemgr.h>

#include <cstdlib>
#include <map>
#include <set>

namespace wowee::platform::psp {

namespace {

std::map<std::string, std::string> g_values;
std::set<std::string> g_dirty;  // configSet since the last configSave
std::set<std::string> g_erased;  // configErase since the last configSave
std::string g_path;             // what configLoad read (configSave writes it back)
bool g_loaded = false;

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

bool readFile(const char* path, std::string& text) {
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return false;
    char buf[1024];
    int n;
    while ((n = sceIoRead(fd, buf, sizeof(buf))) > 0 && text.size() < 64 * 1024) text.append(buf, static_cast<size_t>(n));
    sceIoClose(fd);
    return true;
}

// One write of the whole buffer (the file is a few hundred bytes).
bool writeFile(const char* path, const std::string& text) {
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return false;
    const int n = text.empty() ? 0 : sceIoWrite(fd, text.data(), text.size());
    sceIoClose(fd);
    return n == static_cast<int>(text.size());
}

} // namespace

bool configLoad(const char* absPath) {
    g_values.clear();
    g_dirty.clear();
    g_path = absPath;
    g_loaded = false;
    std::string text;
    if (!readFile(absPath, text)) return false;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF) text.erase(0, 3);  // UTF-8 BOM

    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = eol == std::string::npos ? text.size() : eol + 1;
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        g_values[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    g_loaded = true;
    return true;
}

bool configLoaded() { return g_loaded; }

std::string configGet(const char* key, const char* fallback) {
    auto it = g_values.find(key);
    return it == g_values.end() ? std::string(fallback) : it->second;
}

int configGetInt(const char* key, int fallback) {
    auto it = g_values.find(key);
    if (it == g_values.end() || it->second.empty()) return fallback;
    return std::atoi(it->second.c_str());
}

void configSet(const char* key, const std::string& value) {
    g_values[key] = value;
    g_dirty.insert(key);
}

void configOverride(const char* key, const std::string& value) { g_values[key] = value; }

void configErase(const char* key) {
    g_values.erase(key);
    g_dirty.erase(key);
    g_erased.insert(key);
}

bool configSave(int* keysWritten) {
    if (keysWritten) *keysWritten = 0;
    if (g_path.empty()) return false;
    if (g_dirty.empty() && g_erased.empty()) return true;
    std::string old;
    const bool had = readFile(g_path.c_str(), old);
    const char* eol = old.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::string out, backup;  // backup: the old file without the erased keys' lines
    std::set<std::string> written;
    size_t pos = 0;
    while (pos < old.size()) {
        size_t end = old.find('\n', pos);
        const size_t next = end == std::string::npos ? old.size() : end + 1;
        const std::string raw = old.substr(pos, next - pos);
        pos = next;
        const std::string line = trim(raw.substr(0, raw.find('\n')));
        const size_t eq = line.find('=');
        if (!line.empty() && line[0] != '#' && eq != std::string::npos) {
            std::string key = trim(line.substr(0, eq));
            if (key.size() >= 3 && static_cast<unsigned char>(key[0]) == 0xEF) key.erase(0, 3);  // BOM on line 1
            if (g_erased.count(key)) continue;
            backup += raw;
            if (g_dirty.count(key) && !written.count(key)) {
                out += key + "=" + g_values[key] + eol;
                written.insert(key);
                continue;
            }
        } else {
            backup += raw;
        }
        out += raw;
    }
    if (!out.empty() && out.back() != '\n') out += eol;
    bool header = false;
    for (const auto& key : g_dirty) {
        if (written.count(key)) continue;
        if (!header) {
            out += std::string("# graphics settings menu (L+SELECT in the world)") + eol;
            header = true;
        }
        out += key + "=" + g_values[key] + eol;
        written.insert(key);
    }
    // The old file first: a write cut short (battery, card pulled) leaves the
    // server/account/password lines recoverable from wowee.cfg.bak.
    if (had && !writeFile((g_path + ".bak").c_str(), backup)) return false;
    if (!writeFile(g_path.c_str(), out)) return false;
    if (keysWritten) *keysWritten = static_cast<int>(written.size() + g_erased.size());
    g_dirty.clear();
    g_erased.clear();
    return true;
}

} // namespace wowee::platform::psp
