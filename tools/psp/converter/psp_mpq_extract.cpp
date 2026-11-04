// psp_mpq_extract - read-only extraction of selected MPQ folders (PC side).
//
//   psp_mpq_extract --data <client Data dir> --out <extracted> [--locale ruRU]
//                   --prefix "Item\TextureComponents\" [--prefix ...] [--force] [--dry-run]
//
// Why not upstream tools/asset_extract: it opens archives with
// SFileOpenArchive(path, 0, ...) = read/write, which fails while the
// client runs (it holds the MPQs). This opens them MPQ_OPEN_READ_ONLY with
// write sharing allowed on the handle (we never write), so nothing is ever
// written to the archives and the client can stay open.
//
// Archive order = upstream Extractor::discoverArchives for 3.3.5a:
// common, common-2, expansion, the 3.3.5a expansion (seq below), the locale archives, then
// patch / patch-<loc>, patch-2 / patch-<loc>-2, ... Files are enumerated
// from the highest priority archive down and the first one seen wins
// (upstream enumerateFilesImpl). Output paths are lowercase with '/', like
// the existing extracted tree (conv_common.hpp lower()). Only files whose
// normalised path starts with one of the prefixes are written; existing files
// are kept unless --force. No manifest.json is touched.

#include <StormLib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string lowerStr(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Client path -> lowercase with '\' (upstream normalizeWowPath).
std::string normWow(std::string s) {
    std::replace(s.begin(), s.end(), '/', '\\');
    return lowerStr(s);
}

std::vector<std::string> archiveChain(const std::string& dataDir, const std::string& locale) {
    std::map<std::string, std::string> caseMap;  // lower relative path -> real relative path
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dataDir, ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string rel = fs::relative(it->path(), dataDir, ec).generic_string();
        caseMap[lowerStr(rel)] = rel;
    }
    const std::string loc = lowerStr(locale);
    std::vector<std::string> seq = {"common.mpq", "common-2.mpq", "expansion.mpq", "lichking.mpq"};
    if (!loc.empty())
        for (const char* n : {"backup-", "base-", "locale-", "speech-", "expansion-locale-", "expansion-speech-",
                              "lichking-locale-", "lichking-speech-"})
            seq.push_back(loc + "/" + n + loc + ".mpq");
    std::vector<std::string> suffixes = {""};
    for (int i = 2; i <= 9; ++i) suffixes.push_back("-" + std::to_string(i));
    for (char c = 'a'; c <= 'z'; ++c) suffixes.push_back(std::string("-") + c);
    for (const auto& s : suffixes) {
        seq.push_back("patch" + s + ".mpq");
        if (!loc.empty()) seq.push_back(loc + "/patch-" + loc + s + ".mpq");
    }
    std::vector<std::string> out;
    for (const auto& s : seq)
        if (auto it = caseMap.find(s); it != caseMap.end()) out.push_back((fs::path(dataDir) / it->second).string());
    return out;
}

} // namespace

int main(int argc, char** argv) {
    std::string data, out, locale = "ruRU";
    std::vector<std::string> prefixes;
    std::vector<std::string> names;  // --names: exact client paths, looked up by hash (no listfile needed)
    bool force = false, dryRun = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--data" && i + 1 < argc) data = argv[++i];
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--locale" && i + 1 < argc) locale = argv[++i];
        else if (a == "--prefix" && i + 1 < argc) prefixes.push_back(normWow(argv[++i]));
        else if (a == "--names" && i + 1 < argc) {
            std::ifstream in(argv[++i]);
            for (std::string line; std::getline(in, line);) {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                if (line.rfind("missing ", 0) == 0) line = line.substr(8);  // psp_sound_conv --list-missing lines
                if (!line.empty()) names.push_back(line);
            }
        }
        else if (a == "--force") force = true;
        else if (a == "--dry-run") dryRun = true;
    }
    if (data.empty() || out.empty() || (prefixes.empty() && names.empty())) {
        std::fprintf(stderr,
                     "usage: psp_mpq_extract --data <Data> --out <extracted> [--locale ruRU] --prefix <client\\path\\> "
                     "[--prefix ...] [--names <file of paths>] [--force] [--dry-run]\n"
                     "  --names: files the archives' own listfiles do not name (psp_sound_conv --list-missing)\n");
        return 2;
    }
    const auto archives = archiveChain(data, locale);
    if (archives.empty()) {
        std::fprintf(stderr, "no MPQ archives under %s\n", data.c_str());
        return 1;
    }
    std::printf("%zu archives (read-only)\n", archives.size());
    std::set<std::string> seen;
    size_t written = 0, kept = 0, failed = 0, bytes = 0;
    // One archived file -> <out>/<lower path>; the newest archive wins (`seen`).
    auto extractOne = [&](HANDLE mpq, const std::string& name) -> size_t {
        const std::string norm = normWow(name);
        if (!SFileHasFile(mpq, name.c_str()) || !seen.insert(norm).second) return 0;
        std::string rel = norm;
        std::replace(rel.begin(), rel.end(), '\\', '/');
        const fs::path dst = fs::path(out) / rel;
        if (!force && fs::exists(dst)) {
            ++kept;
            return 0;
        }
        if (dryRun) return 1;
        HANDLE f = nullptr;
        if (!SFileOpenFileEx(mpq, name.c_str(), SFILE_OPEN_FROM_MPQ, &f)) {
            ++failed;
            return 0;
        }
        const DWORD size = SFileGetFileSize(f, nullptr);
        std::vector<char> buf(size);
        DWORD got = 0;
        bool ok = size != SFILE_INVALID_SIZE;
        if (ok && size) ok = SFileReadFile(f, buf.data(), size, &got, nullptr) && got == size;
        SFileCloseFile(f);
        if (!ok) {
            ++failed;
            return 0;
        }
        std::error_code ec;
        fs::create_directories(dst.parent_path(), ec);
        std::ofstream o(dst, std::ios::binary);
        o.write(buf.data(), static_cast<std::streamsize>(buf.size()));
        if (!o) {
            ++failed;
            return 0;
        }
        ++written;
        bytes += buf.size();
        return 1;
    };
    for (auto it = archives.rbegin(); it != archives.rend(); ++it) {
        HANDLE mpq = nullptr;
        if (!SFileOpenArchive(it->c_str(), 0, MPQ_OPEN_READ_ONLY | STREAM_FLAG_WRITE_SHARE, &mpq)) {
            std::fprintf(stderr, "  cannot open %s (error %u)\n", it->c_str(), static_cast<unsigned>(GetLastError()));
            continue;
        }
        size_t here = 0;
        for (const auto& n : names) here += extractOne(mpq, n);
        SFILE_FIND_DATA fd;
        HANDLE find = SFileFindFirstFile(mpq, "*", &fd, nullptr);
        if (find) {
            do {
                const std::string name = fd.cFileName;
                const std::string norm = normWow(name);
                bool wanted = false;
                for (const auto& p : prefixes) wanted = wanted || norm.rfind(p, 0) == 0;
                if (wanted) here += extractOne(mpq, name);
            } while (SFileFindNextFile(find, &fd));
            SFileFindClose(find);
        }
        SFileCloseArchive(mpq);
        std::printf("  %s: %zu files\n", fs::path(*it).filename().string().c_str(), here);
    }
    std::printf("%s %zu files (%zu KB), kept %zu existing, failed %zu\n", dryRun ? "would write" : "wrote",
                dryRun ? seen.size() - kept : written, bytes / 1024, kept, failed);    return failed ? 1 : 0;
}
