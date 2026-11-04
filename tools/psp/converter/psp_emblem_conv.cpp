// psp_emblem_conv - bakes the guild tabard emblems for the PSP characters.
//
//   psp_emblem_conv --extracted <dir> --out <emblems.pak> [--verify] [--only <n>] [--require-all]
//
// Reads <extracted>/textures/guildemblems/{background,border,emblem}_*_{tu,tl}_u.blp (the 3.3.5a data: 51
// backgrounds, 6 x 17 + 4 x 4 borders, 170 x 17 emblems, each for TorsoUpper and TorsoLower - 6 118 files, common.MPQ;
// extract them first: psp_mpq_extract --prefix textures\guildemblems\) and writes one WLAY per file, exactly as
// psp_item_conv bakes an item's body layer (model_bake.hpp resample to half the section rectangle, the median-cut
// 256-colour CLUT, placed on the 128 body canvas at the section): the PSP pastes them as layers 2..4 of the torso
// sections (include/rendering/psp/psp_emblem_format.hpp: layers 2..4 of the torso sections). The pak is sorted with the "~fence"
// record (conv_common.hpp SortedPakWriter, step 32: the PSP keeps 3 TOC blocks of 32 records - 6 KB - while the
// index is open) and named by psp_emblem_format.hpp layerName ("gb_u_5", "gr_l_3_12", "ge_u_169_16").
// --verify reads the pak back the way the PSP reads it (pakIndexLookup) and checks every layer.
// --only <n> bakes the first n files of each kind (a small test pak).
// The set is checked for completeness before baking (6 118 files: 102 backgrounds, 236 borders,
// 5 780 emblems, every part in both TU and TL) - an incomplete extraction gets a loud warning (the PSP would draw those
// tabards without the missing parts), --require-all makes it an error (exit 1, no pak written).

#include "pipeline/blp_loader.hpp"
#include "platform/psp/psp_pak_index.hpp"
#include "rendering/psp/psp_emblem_format.hpp"
#include "rendering/psp/psp_item_format.hpp"
#include "rendering/psp/psp_model_format.hpp"

#include "conv_common.hpp"
#include "model_bake.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

using namespace wowee;
using namespace wowee::pspconv;
namespace pp = wowee::platform::psp;
namespace em = wowee::rendering::psp::emblem;

namespace {

constexpr uint32_t kKindLayer = 8;  // as psp_item_conv's "q_" layers
constexpr uint32_t kStep = 32;

struct Source {
    std::string path;
    uint8_t part = em::kItem;
    int section = 0;
    uint16_t code = 0;
    std::string name;
};

// background_05_tu_u.blp, border_03_12_tl_u.blp, emblem_169_16_tu_u.blp (lower case: psp_mpq_extract writes so).
bool parseName(const std::string& file, Source& s) {
    int a = -1, b = -1;
    char sec[3] = {};
    if (std::sscanf(file.c_str(), "background_%d_%2[tlu]_u.blp", &a, sec) == 2) {
        s.part = em::kBackground;
        b = 0;
    } else if (std::sscanf(file.c_str(), "border_%d_%d_%2[tlu]_u.blp", &a, &b, sec) == 3) {
        s.part = em::kBorder;
    } else if (std::sscanf(file.c_str(), "emblem_%d_%d_%2[tlu]_u.blp", &a, &b, sec) == 3) {
        s.part = em::kEmblem;
    } else {
        return false;
    }
    if (std::strcmp(sec, "tu") == 0) s.section = em::kTorsoUpper;
    else if (std::strcmp(sec, "tl") == 0) s.section = em::kTorsoLower;
    else return false;
    if (s.part == em::kBackground) {
        if (a < 0 || a > 0xFFFF) return false;
        s.code = static_cast<uint16_t>(a);
    } else {
        if (!em::codeFits(a, b)) return false;
        s.code = em::code(a, b);
    }
    char name[48];
    if (!em::layerName(name, sizeof(name), s.part, s.section, s.code)) return false;
    s.name = name;
    return true;
}

// The 3.3.5a set (common.MPQ textures\guildemblems\): 51 backgrounds, 6 x 17 + 4 x 4 borders,
// 170 x 17 emblems, each for TorsoUpper and TorsoLower.
constexpr int kWantBackgrounds = 51 * 2, kWantBorders = (6 * 17 + 4 * 4) * 2, kWantEmblems = 170 * 17 * 2;
constexpr int kWantTotal = kWantBackgrounds + kWantBorders + kWantEmblems;
static_assert(kWantTotal == 6118, "the 3.3.5a guild emblem set");

// The source file name of a part (as psp_mpq_extract writes it: lower case).
std::string fileName(uint8_t part, int section, uint16_t c) {
    const char* sec = section == em::kTorsoUpper ? "tu" : "tl";
    char n[64];
    if (part == em::kBackground) std::snprintf(n, sizeof(n), "background_%02u_%s_u.blp", unsigned{c}, sec);
    else
        std::snprintf(n, sizeof(n), "%s_%02u_%02u_%s_u.blp", part == em::kBorder ? "border" : "emblem", c / 32u,
                      c % 32u, sec);
    return n;
}

// Is every part of the extracted set there - each kind's count and every part in both sections
// (a tabard pastes TU and TL)? Prints the result; a gap is a loud warning on stderr. Returns at least how many files
// are missing (0 = complete).
int checkComplete(const std::vector<Source>& all, const std::string& dir) {
    int have[4] = {};
    std::map<std::pair<uint8_t, uint16_t>, int> sections;  // (part, code) -> bit 1 TorsoUpper, bit 2 TorsoLower
    for (const Source& s : all) {
        ++have[s.part];
        sections[{s.part, s.code}] |= s.section == em::kTorsoUpper ? 1 : 2;
    }
    std::vector<std::string> unpaired;
    for (const auto& [key, bits] : sections)
        if (bits != 3) unpaired.push_back(fileName(key.first, bits == 1 ? em::kTorsoLower : em::kTorsoUpper, key.second));
    const int want[4] = {0, kWantBackgrounds, kWantBorders, kWantEmblems};
    int missing = 0;
    for (int p = 1; p < 4; ++p) missing += std::max(0, want[p] - have[p]);
    missing = std::max(missing, static_cast<int>(unpaired.size()));
    if (missing == 0) {
        std::printf("complete set: %zu files (backgrounds %d, borders %d, emblems %d), every part in TU and TL\n",
                    all.size(), have[em::kBackground], have[em::kBorder], have[em::kEmblem]);
        return 0;
    }
    std::fprintf(stderr,
                 "\n*** WARNING: incomplete guild emblem set in %s: %zu of %d files (backgrounds %d/%d, borders %d/%d, "
                 "emblems %d/%d), %zu parts in one section only",
                 dir.c_str(), all.size(), kWantTotal, have[em::kBackground], kWantBackgrounds, have[em::kBorder],
                 kWantBorders, have[em::kEmblem], kWantEmblems, unpaired.size());
    for (size_t i = 0; i < unpaired.size() && i < 8; ++i)
        std::fprintf(stderr, "%s%s", i == 0 ? " - missing " : ", ", unpaired[i].c_str());
    std::fprintf(stderr, "%s\n*** the PSP draws those guild tabards without the missing parts (emblem_layer_missing);"
                         " extract them again: psp_mpq_extract --prefix textures\\guildemblems\\ (--require-all: fail)\n\n",
                 unpaired.size() > 8 ? ", ..." : "");
    return missing;
}

bool bakeLayer(const std::string& path, int section, std::vector<uint8_t>& blob) {
    std::vector<uint8_t> bytes;
    if (!readFile(path, bytes)) return false;
    const pipeline::BLPImage img = pipeline::BLPLoader::load(bytes);
    if (img.data.empty()) return false;
    const auto* r = rendering::psp::kItemRegionRect256[section];
    const int w = r[2] / 2, h = r[3] / 2;
    const std::vector<Rgb> px = resample(img, w, h);
    std::vector<uint8_t> idx;
    std::array<Rgb, 256> pal;
    quantize(px, idx, pal);
    rendering::psp::LayerHeader lh{};
    std::memcpy(lh.magic, "WLAY", 4);
    lh.w = static_cast<uint16_t>(w);
    lh.h = static_cast<uint16_t>(h);
    lh.x = static_cast<uint16_t>(r[0] / 2);
    lh.y = static_cast<uint16_t>(r[1] / 2);
    blob.assign(reinterpret_cast<const uint8_t*>(&lh), reinterpret_cast<const uint8_t*>(&lh + 1));
    for (const Rgb& p : pal) put<uint32_t>(blob, to8888(p));
    blob.insert(blob.end(), idx.begin(), idx.end());
    return true;
}

bool wlayValid(const std::vector<uint8_t>& b, int section) {
    if (b.size() < sizeof(rendering::psp::LayerHeader) + 1024 || std::memcmp(b.data(), "WLAY", 4) != 0) return false;
    rendering::psp::LayerHeader lh;
    std::memcpy(&lh, b.data(), sizeof(lh));
    const auto* r = rendering::psp::kItemRegionRect256[section];
    return lh.w == r[2] / 2 && lh.h == r[3] / 2 && lh.x == r[0] / 2 && lh.y == r[1] / 2 &&
           b.size() == sizeof(lh) + 1024u + size_t{lh.w} * lh.h;
}

// The written pak read back as the PSP does it: the TOC sorted, the fence, every name through pakIndexLookup.
int verifyPak(const std::string& path, const std::vector<Source>& sources) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        std::fprintf(stderr, "verify: cannot open %s\n", path.c_str());
        return 1;
    }
    auto readAt = [&](uint64_t off, void* dst, size_t n) {
#if defined(_WIN32)
        if (_fseeki64(f, static_cast<long long>(off), SEEK_SET) != 0) return false;
#else
        if (fseeko(f, static_cast<off_t>(off), SEEK_SET) != 0) return false;
#endif
        return std::fread(dst, 1, n, f) == n;
    };
    int errors = 0;
    auto err = [&](const std::string& what) {
        if (++errors <= 40) std::fprintf(stderr, "verify: %s\n", what.c_str());
    };
    uint32_t head[4] = {};
    std::vector<pp::PakDiskEntry> toc;
    std::vector<uint8_t> fence;
    pp::PakFenceHeader fh{};
    if (!readAt(0, head, sizeof(head)) || std::memcmp(head, "WPAK", 4) != 0 || head[2] < 2) {
        err("not a WPAK");
    } else {
        toc.resize(head[2]);
        if (!readAt(head[3], toc.data(), toc.size() * sizeof(pp::PakDiskEntry))) err("TOC short");
        for (auto& e : toc) e.name[sizeof(e.name) - 1] = '\0';
        if (std::strcmp(toc.back().name, pp::kPakFenceName) != 0) err("no ~fence last");
        fence.resize(toc.back().size);
        if (!readAt(toc.back().offset, fence.data(), fence.size())) err("fence unreadable");
        if (fence.size() >= sizeof(fh)) std::memcpy(&fh, fence.data(), sizeof(fh));
        if (std::memcmp(fh.magic, "WFNC", 4) != 0 || fh.records != toc.size() || fh.step != kStep) err("fence header");
    }
    if (errors) {
        std::fclose(f);
        return 3;
    }
    for (size_t i = 1; i < toc.size(); ++i)
        if (std::strcmp(toc[i - 1].name, toc[i].name) >= 0) err(std::string("TOC order at ") + toc[i].name);
    auto fenceName = [&](uint32_t i) -> const char* {
        uint32_t o;
        std::memcpy(&o, fence.data() + sizeof(pp::PakFenceHeader) + i * 4u, 4);
        return o < fence.size() ? reinterpret_cast<const char*>(fence.data() + o) : "";
    };
    std::vector<pp::PakDiskEntry> scratch(fh.step);
    std::vector<uint8_t> blob;
    size_t ok = 0;
    for (const Source& s : sources) {
        pp::PakDiskEntry e{};
        const bool found = pp::pakIndexLookup(
            s.name.c_str(), fh.records, fh.step, fh.fences, fenceName,
            [&](uint32_t first, uint32_t n, pp::PakDiskEntry* dst) {
                return readAt(head[3] + uint64_t{first} * sizeof(pp::PakDiskEntry), dst, n * sizeof(pp::PakDiskEntry));
            },
            scratch.data(), e);
        if (!found) {
            err("lookup failed: " + s.name);
            continue;
        }
        blob.resize(e.size);
        if (!readAt(e.offset, blob.data(), blob.size()) || !wlayValid(blob, s.section)) {
            err("bad layer: " + s.name);
            continue;
        }
        ++ok;
    }
    std::fclose(f);
    std::printf("verify: %zu / %zu layers found and valid, %zu records, %u fences (step %u), %d errors\n", ok,
                sources.size(), toc.size(), fh.fences, fh.step, errors);
    return errors ? 3 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string root, out;
    bool verify = false;
    bool requireAll = false;  // --require-all
    int only = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--extracted" && i + 1 < argc) root = argv[++i];
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--verify") verify = true;
        else if (a == "--only" && i + 1 < argc) only = std::atoi(argv[++i]);
        else if (a == "--require-all") requireAll = true;  // an incomplete set is an error
        else {
            std::fprintf(stderr, "usage: psp_emblem_conv --extracted <dir> --out <emblems.pak> [--verify] [--only n] "
                                 "[--require-all]\n");
            return 2;
        }
    }
    if (root.empty() || out.empty()) {
        std::fprintf(stderr, "usage: psp_emblem_conv --extracted <dir> --out <emblems.pak> [--verify] [--only n] "
                             "[--require-all]\n");
        return 2;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const std::filesystem::path dir = std::filesystem::path(root) / "textures" / "guildemblems";
    std::error_code ec;
    std::vector<Source> sources;
    std::vector<Source> all;  // every part found (before --only), for the completeness check
    std::map<uint8_t, int> perPart;
    for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator();
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        Source s;
        const std::string file = lower(it->path().filename().string());
        if (!parseName(file, s)) {
            if (file.size() > 4 && file.compare(file.size() - 4, 4, ".blp") == 0)
                std::fprintf(stderr, "skip %s (not a tabard part name)\n", file.c_str());
            continue;
        }
        all.push_back(s);  // for the completeness check
        if (only > 0 && perPart[s.part] >= only * 2) continue;
        ++perPart[s.part];
        s.path = it->path().string();
        sources.push_back(std::move(s));
    }
    if (sources.empty()) {
        std::fprintf(stderr, "%s: no guild emblem textures (extract textures\\guildemblems\\ first)\n",
                     dir.string().c_str());
        return 1;
    }
    // An incomplete extraction used to bake silently (a pak without most of the parts).
    const int missing = checkComplete(all, dir.string());
    if (missing && requireAll) {
        std::fprintf(stderr, "error: --require-all: the guild emblem set is incomplete (%d+ files missing), %s not "
                             "written\n", missing, out.c_str());
        return 1;
    }
    std::sort(sources.begin(), sources.end(), [](const Source& a, const Source& b) { return a.name < b.name; });
    SortedPakWriter pak(out, kStep);
    size_t bytes = 0, failed = 0;
    std::vector<uint8_t> blob;
    for (const Source& s : sources) {
        if (!bakeLayer(s.path, s.section, blob)) {
            std::fprintf(stderr, "failed %s\n", s.path.c_str());
            ++failed;
            continue;
        }
        bytes += blob.size();
        if (!pak.add(s.name, kKindLayer, blob.data(), blob.size())) {
            std::fprintf(stderr, "pak write failed at %s\n", s.name.c_str());
            return 1;
        }
    }
    if (!pak.finish()) {
        std::fprintf(stderr, "pak finish failed: %s\n", out.c_str());
        return 1;
    }
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("wrote %s: %zu layers (backgrounds %d, borders %d, emblems %d), %zu B payload, %zu failed, %.1f s\n",
                out.c_str(), sources.size() - failed, perPart[em::kBackground], perPart[em::kBorder],
                perPart[em::kEmblem], bytes, failed, sec);
    if (missing)  // again after the per-file output, so it is not lost in the log
        std::fprintf(stderr, "*** WARNING: %s is incomplete: %zu of %d guild emblem files extracted (see above)\n",
                     out.c_str(), all.size(), kWantTotal);
    if (failed) return 1;
    if (verify) return verifyPak(out, sources);
    return 0;
}
