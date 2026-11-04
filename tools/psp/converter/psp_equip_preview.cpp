// psp_equip_preview - PC check of the PSP equipment path (M10c).
//
//   psp_equip_preview --chars <characters.pak> --items <items.pak> --extracted <dir>
//                     --race 1 --sex 0 --class 1 [--skin 0 --face 0 --hair 1] --out <body.ppm>
//
// Runs the PSP's own src/rendering/psp/psp_equipment.cpp (applyEquipmentGeosets,
// equipmentLayers, ItemBank) over a PC implementation of platform::psp::Pak,
// with the starter outfit of CharStartOutfit.dbc for race/class/sex as the
// equipment, and composites the 128x128 body exactly like CharacterModel::dress
// (same blit). Prints the geosets of groups 4/5/8/13/15/18 bare vs dressed and
// writes the body texture before (<out>.bare.ppm) and after (<out>).
//
// Also prints what the PSP attaches (equipmentAttachments, the same
// code as CharacterModel): per helm/shoulder/weapon the point, its bone in
// "n_", the model "a_<n>" (checked with itemModelValid), its skin "t_<n>" and
// where the point is in the Stand pose ("p_<race>_<sex>"). Flags:
//   --local [--sheathed]  the own player's rules (AppearanceComposer), default
//                         the other players' (setOnlinePlayerEquipment)
//   --display <slot>=<id> put a display id on a slot (repeatable), e.g. 15=1542
//   --verify-items        read every "i_" / "a_" of items.pak the PSP way
//                         (no --extracted needed with --no-outfit)
//   --no-outfit           skip CharStartOutfit (only --display)
//   --item <slot>=<item>  an item id through the Item.dbc
//                         table of items.pak (ItemBank::itemDisplay), repeatable
// The sorted items.pak opens through a PC
// PakIndex (below) - the PSP's own lookup over the TOC on disk.
// --matrix [...] - every Item.dbc display on all 20
// race/sex models, the PSP's code against upstream's own equipment code run
// on the PC (equip_matrix/); see --matrix alone
// for the options.

#include "core/geoset_rules.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"
#include "rendering/psp/psp_equipment.hpp"
#include "rendering/psp/psp_model_format.hpp"
#include "rendering/psp/psp_skeleton.hpp"

#include "conv_common.hpp"

#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

// --- PC stand-ins for the PSP platform pieces psp_equipment.cpp uses. ---
namespace wowee::platform::psp {

void logWrite(LogLevel, const char* tagName, const char* fmt, ...) {
    std::va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "%s: ", tagName);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    va_end(ap);
}

namespace {
struct PakHeader {
    char magic[4];
    uint32_t version, count, tocOffset;
};
std::vector<std::FILE*> g_files;
std::FILE* fileOf(int fd) { return fd >= 0 && fd < static_cast<int>(g_files.size()) ? g_files[fd] : nullptr; }
} // namespace

Pak::~Pak() { close(); }

bool Pak::open(const char* absPath) {
    close();
    std::FILE* f = std::fopen(absPath, "rb");
    if (!f) return false;
    PakHeader h;
    if (std::fread(&h, sizeof(h), 1, f) != 1 || std::memcmp(h.magic, "WPAK", 4) != 0) {
        std::fclose(f);
        return false;
    }
    // The disk records (PakDiskEntry), the in-memory PakEntry pointing at their names.
    std::vector<PakDiskEntry> disk(h.count);
    std::fseek(f, static_cast<long>(h.tocOffset), SEEK_SET);
    if (std::fread(disk.data(), sizeof(PakDiskEntry), h.count, f) != h.count) {
        std::fclose(f);
        return false;
    }
    char* pool = static_cast<char*>(std::malloc(h.count * sizeof(PakEntry) + h.count * 48u));
    toc_ = reinterpret_cast<PakEntry*>(pool);
    block_ = pool;
    char* names = pool + h.count * sizeof(PakEntry);
    for (uint32_t i = 0; i < h.count; ++i) {
        disk[i].name[47] = '\0';
        std::memcpy(names + i * 48u, disk[i].name, 48);
        toc_[i] = PakEntry{names + i * 48u, disk[i].offset, disk[i].size, disk[i].kind};
    }
    count_ = h.count;
    g_files.push_back(f);
    fd_ = static_cast<int>(g_files.size() - 1);
    return true;
}

void Pak::close() {
    if (std::FILE* f = fileOf(fd_)) {
        std::fclose(f);
        g_files[fd_] = nullptr;
    }
    fd_ = -1;
    std::free(block_);
    block_ = nullptr;
    toc_ = nullptr;
    count_ = 0;
}

const PakEntry* Pak::find(const char* name) const {
    for (uint32_t i = 0; i < count_; ++i)
        if (std::strcmp(toc_[i].name, name) == 0) return &toc_[i];
    return nullptr;
}

void* Pak::load(const PakEntry* e) {
    std::FILE* f = fileOf(fd_);
    if (!f || !e) return nullptr;
    void* p = std::malloc(e->size ? e->size : 1);
    std::fseek(f, static_cast<long>(e->offset), SEEK_SET);
    if (std::fread(p, 1, e->size, f) != e->size) {
        std::free(p);
        return nullptr;
    }
    return p;
}

void pakFree(void* block) { std::free(block); }

// PakIndex on the PC - the sorted items.pak read the way the PSP reads it (the
// lookup is psp_pak_index.hpp pakIndexLookup, the same code; the file through stdio instead of sceIo).
namespace {
bool seekTo(std::FILE* f, uint64_t offset) {
#if defined(_WIN32)
    return _fseeki64(f, static_cast<long long>(offset), SEEK_SET) == 0;
#else
    return fseeko(f, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}
} // namespace

PakIndex::~PakIndex() { close(); }

bool PakIndex::open(const char* absPath) {
    close();
    std::FILE* f = std::fopen(absPath, "rb");
    if (!f) return false;
    PakHeader h{};
    PakDiskEntry last{};
    bool ok = std::fread(&h, sizeof(h), 1, f) == 1 && std::memcmp(h.magic, "WPAK", 4) == 0 && h.version == 1 &&
              h.count >= 1 && seekTo(f, h.tocOffset + uint64_t{h.count - 1u} * sizeof(PakDiskEntry)) &&
              std::fread(&last, sizeof(last), 1, f) == 1;
    last.name[sizeof(last.name) - 1] = '\0';
    ok = ok && std::strcmp(last.name, kPakFenceName) == 0 && last.size >= sizeof(PakFenceHeader) &&
         last.size <= 1024u * 1024u;
    if (ok) {
        fence_ = static_cast<uint8_t*>(std::malloc(last.size));
        ok = fence_ && seekTo(f, last.offset) && std::fread(fence_, 1, last.size, f) == last.size;
    }
    PakFenceHeader fh{};
    if (ok) {
        std::memcpy(&fh, fence_, sizeof(fh));
        ok = std::memcmp(fh.magic, "WFNC", 4) == 0 && fh.version == 1 && fh.step >= 1 && fh.step <= 1024 &&
             fh.records == h.count && fh.fences == (h.count + fh.step - 1u) / fh.step &&
             sizeof(fh) + fh.fences * 4u <= last.size;
    }
    if (ok) {
        blocks_ = static_cast<PakDiskEntry*>(std::malloc(3u * fh.step * sizeof(PakDiskEntry)));
        ok = blocks_ != nullptr;
    }
    if (!ok) {
        std::fclose(f);
        close();
        return false;
    }
    g_files.push_back(f);
    fd_ = static_cast<int>(g_files.size() - 1);
    records_ = h.count;
    tocOffset_ = h.tocOffset;
    step_ = fh.step;
    fences_ = fh.fences;
    fenceSize_ = last.size;
    ramBytes_ = fenceSize_ + 3u * step_ * static_cast<uint32_t>(sizeof(PakDiskEntry));
    return true;
}

void PakIndex::close() {
    if (std::FILE* f = fileOf(fd_)) {
        std::fclose(f);
        g_files[fd_] = nullptr;
    }
    fd_ = -1;
    std::free(fence_);
    fence_ = nullptr;
    std::free(blocks_);
    blocks_ = nullptr;
    fenceSize_ = records_ = tocOffset_ = step_ = fences_ = fileId_ = ramBytes_ = 0;
    cachedFirst_[0] = cachedFirst_[1] = ~0u;
    cachedN_[0] = cachedN_[1] = 0;
    lru_ = 0;
}

const char* PakIndex::fenceName(uint32_t i) const {
    uint32_t o;
    std::memcpy(&o, fence_ + sizeof(PakFenceHeader) + i * 4u, 4);
    return reinterpret_cast<const char*>(fence_ + o);
}

// The PSP's two cached TOC blocks (psp_pak_index.cpp readBlock), so toc_reads= counts what the PSP would read.
bool PakIndex::readBlock(uint32_t first, uint32_t n, PakDiskEntry* dst) {
    for (int s = 0; s < 2; ++s) {
        if (cachedFirst_[s] == first && cachedN_[s] == n) {
            std::memcpy(dst, blocks_ + s * step_, n * sizeof(PakDiskEntry));
            lru_ = static_cast<uint32_t>(s ^ 1);
            return true;
        }
    }
    std::FILE* f = fileOf(fd_);
    const uint32_t s = lru_;
    PakDiskEntry* slot = blocks_ + s * step_;
    if (!f || !seekTo(f, tocOffset_ + uint64_t{first} * sizeof(PakDiskEntry)) ||
        std::fread(slot, sizeof(PakDiskEntry), n, f) != n) {
        cachedFirst_[s] = ~0u;
        return false;
    }
    ++blockReads_;
    cachedFirst_[s] = first;
    cachedN_[s] = n;
    lru_ = s ^ 1u;
    std::memcpy(dst, slot, n * sizeof(PakDiskEntry));
    return true;
}

bool PakIndex::find(const char* name, PakEntry& out) {
    if (fd_ < 0 || !name) return false;
    ++lookups_;
    PakDiskEntry d{};
    const bool hit = pakIndexLookup(
        name, records_, step_, fences_, [this](uint32_t i) { return fenceName(i); },
        [this](uint32_t first, uint32_t n, PakDiskEntry* dst) { return readBlock(first, n, dst); },
        blocks_ + 2u * step_, d);
    if (!hit) return false;
    std::memcpy(name_, d.name, sizeof(name_));
    name_[sizeof(name_) - 1] = '\0';
    out = PakEntry{name_, d.offset, d.size, d.kind};
    return true;
}

void* PakIndex::load(const PakEntry& e) {
    std::FILE* f = fileOf(fd_);
    if (!f) return nullptr;
    void* p = std::malloc(e.size ? e.size : 1);
    if (!seekTo(f, e.offset) || std::fread(p, 1, e.size, f) != e.size) {
        std::free(p);
        return nullptr;
    }
    return p;
}

} // namespace wowee::platform::psp

using namespace wowee;
namespace pp = wowee::platform::psp;
namespace rp = wowee::rendering::psp;

namespace {

// Same as psp_character.cpp blitLayer.
void blitLayer(uint32_t* canvas, const uint8_t* blob, uint32_t size, bool base) {
    if (size < sizeof(rp::LayerHeader) + 1024 || std::memcmp(blob, "WLAY", 4) != 0) return;
    const auto* lh = reinterpret_cast<const rp::LayerHeader*>(blob);
    if (size < sizeof(rp::LayerHeader) + 1024u + static_cast<uint32_t>(lh->w) * lh->h) return;
    const uint8_t* clutBytes = blob + sizeof(rp::LayerHeader);
    const uint8_t* px = clutBytes + 1024;
    for (int y = 0; y < lh->h; ++y) {
        const int cy = lh->y + y;
        if (cy >= rp::kBodyTexSize) break;
        for (int x = 0; x < lh->w; ++x) {
            const int cx = lh->x + x;
            if (cx >= rp::kBodyTexSize) break;
            uint32_t s;
            std::memcpy(&s, clutBytes + px[y * lh->w + x] * 4u, 4);
            uint32_t& d = canvas[cy * rp::kBodyTexSize + cx];
            if (base) {
                d = s | 0xFF000000u;
                continue;
            }
            const uint32_t a = s >> 24;
            if (a == 0) continue;
            if (a == 255) {
                d = s | 0xFF000000u;
                continue;
            }
            uint32_t out = 0xFF000000u;
            for (int c = 0; c < 24; c += 8) {
                const uint32_t sc = (s >> c) & 0xFF, dc = (d >> c) & 0xFF;
                out |= ((sc * a + dc * (255 - a)) / 255) << c;
            }
            d = out;
        }
    }
}

void writePpm(const std::string& path, const std::vector<uint32_t>& c) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", rp::kBodyTexSize, rp::kBodyTexSize);
    for (uint32_t p : c) {
        const uint8_t rgb[3] = {static_cast<uint8_t>(p), static_cast<uint8_t>(p >> 8), static_cast<uint8_t>(p >> 16)};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
}

// Inventory type -> visible slot (EquipSlot order), for a starter outfit.
int slotFor(uint8_t inv) {
    switch (inv) {
        case 1: return 0;
        case 2: return 1;
        case 3: return 2;
        case 4: return 3;
        case 5: case 20: return 4;
        case 6: return 5;
        case 7: return 6;
        case 8: return 7;
        case 9: return 8;
        case 10: return 9;
        case 16: return 14;
        case 19: return 18;
        case 13: case 17: case 21: return 15;
        case 14: case 22: case 23: return 16;
        case 15: case 25: case 26: return 17;
        default: return -1;
    }
}

// Every "i_" record and the models/skins it names, read the PSP way.
int verifyItemsPak(const std::string& path, rp::ItemBank& bank) {
    pp::Pak pak;
    if (!pak.open(path.c_str())) return 1;
    int errors = 0;
    size_t records = 0, withModels = 0;
    std::set<uint16_t> models, skins;
    for (uint32_t i = 0; i < pak.count(); ++i) {
        const pp::PakEntry* e = pak.entry(i);
        if (std::strncmp(e->name, "i_", 2) != 0) continue;
        const uint32_t id = static_cast<uint32_t>(std::strtoul(e->name + 2, nullptr, 10));
        ++records;
        if (!bank.get(id)) {
            std::fprintf(stderr, "verify: %s unreadable\n", e->name);
            ++errors;
            continue;
        }
        const rp::ItemModels* m = bank.models(id);
        if (!m) continue;
        ++withModels;
        for (int side = 0; side < 2; ++side)
            for (int v = 0; v < rp::kItemVariants; ++v) {
                if (m->model[side][v] != rp::kNoItemModel) models.insert(m->model[side][v]);
                if (m->texture[side][v] != rp::kNoItemLayer) skins.insert(m->texture[side][v]);
            }
    }
    size_t modelBytes = 0;
    for (uint16_t id : models) {
        uint32_t size = 0;
        void* blob = bank.loadModel(id, size);
        if (!blob || !rp::itemModelValid(blob, size)) {
            std::fprintf(stderr, "verify: a_%u missing or invalid\n", id);
            ++errors;
        } else {
            rp::ModelHeader h;
            std::memcpy(&h, blob, sizeof(h));
            for (uint32_t b = 0; b < h.batchCount[0]; ++b) {
                rp::ModelBatch mb;
                std::memcpy(&mb, static_cast<const uint8_t*>(blob) + h.batchOffset + b * sizeof(mb), sizeof(mb));
                if (mb.texture != rp::kNoTexture && mb.texture != rp::kTexItemSkin) skins.insert(mb.texture);
            }
            modelBytes += size;
        }
        if (blob) pp::pakFree(blob);
    }
    for (uint16_t id : skins) {
        uint32_t size = 0;
        void* blob = bank.loadTexture(id, size);
        if (!blob || size < 32 || std::memcmp(blob, "WTEX", 4) != 0) {
            std::fprintf(stderr, "verify: t_%u missing or not WTEX\n", id);
            ++errors;
        }
        if (blob) pp::pakFree(blob);
    }
    std::printf("verify items: %zu records, %zu with models, %zu models (%zu KB), %zu skins, %d errors\n", records,
                withModels, models.size(), modelBytes / 1024, skins.size(), errors);
    return errors;
}

// What CharacterModel attaches for this equipment.
int printAttachments(pp::Pak& chars, rp::ItemBank& bank, const rp::Equipment& eq, int race, int sex, bool local,
                     bool sheathed) {
    char name[32];
    std::snprintf(name, sizeof(name), "p_%d_%d", race, sex);
    const pp::PakEntry* pe = chars.find(name);
    auto* pb = pe ? static_cast<uint8_t*>(chars.load(pe)) : nullptr;
    std::vector<rp::AttachPoint> points;
    if (pb && pe->size >= sizeof(rp::AttachTable) && std::memcmp(pb, "WATP", 4) == 0) {
        rp::AttachTable t;
        std::memcpy(&t, pb, sizeof(t));
        if (pe->size >= sizeof(t) + t.count * sizeof(rp::AttachPoint)) {
            points.resize(t.count);
            std::memcpy(points.data(), pb + sizeof(t), t.count * sizeof(rp::AttachPoint));
        }
    }
    if (pb) pp::pakFree(pb);
    // n_ bone count (a point's bone must index it).
    std::snprintf(name, sizeof(name), "n_%d_%d", race, sex);
    uint32_t nBones = 0;
    if (const pp::PakEntry* ne = chars.find(name)) {
        auto* nb = static_cast<uint8_t*>(chars.load(ne));
        rp::SkinView view;
        if (nb && view.init(nb, ne->size)) nBones = view.h->boneCount;
        if (nb) pp::pakFree(nb);
    }
    int errors = 0;
    std::printf("attachment points p_%d_%d: %zu (n_ bones %u)\n", race, sex, points.size(), nBones);
    auto standPos = [](const rp::AttachPoint& p, float out[3]) {
        for (int r = 0; r < 3; ++r)
            out[r] = p.stand[r] * p.position[0] + p.stand[3 + r] * p.position[1] + p.stand[6 + r] * p.position[2] +
                     p.stand[9 + r];
    };
    for (const auto& p : points) {
        float w[3];
        standPos(p, w);
        const bool used = p.id <= 2 || p.id == 5 || p.id == 6 || p.id == 11 || p.id == 12 || p.id == 28 ||
                          p.id == 32 || p.id == 33;
        if (p.bone != rp::kNoAttachBone && p.bone >= nBones) {
            std::printf("  point %u: bone %u out of range\n", p.id, p.bone);
            ++errors;
        }
        if (used)
            std::printf("  point %2u bone %5d stand (%.2f %.2f %.2f)\n", p.id,
                        p.bone == rp::kNoAttachBone ? -1 : static_cast<int>(p.bone), w[0], w[1], w[2]);
    }
    const auto picks = rp::equipmentAttachments(eq, static_cast<uint8_t>(race), static_cast<uint8_t>(sex),
                                                local ? rp::AttachRules::Local : rp::AttachRules::Online, sheathed,
                                                bank);
    std::printf("attachments (%s rules%s): %zu\n", local ? "local" : "online", sheathed ? ", sheathed" : "",
                picks.size());
    for (const auto& a : picks) {
        const rp::AttachPoint* p = nullptr;
        for (const auto& q : points)
            if (q.id == a.point) p = &q;
        uint32_t size = 0;
        void* blob = bank.loadModel(a.model, size);
        const bool ok = blob && rp::itemModelValid(blob, size);
        uint32_t tris = 0, batches = 0;
        if (ok) {
            rp::ModelHeader h;
            std::memcpy(&h, blob, sizeof(h));
            batches = h.batchCount[0];
            for (uint32_t b = 0; b < batches; ++b) {
                rp::ModelBatch mb;
                std::memcpy(&mb, static_cast<const uint8_t*>(blob) + h.batchOffset + b * sizeof(mb), sizeof(mb));
                tris += (h.flags & rp::kModelStrips) ? mb.indexCount - 2 : mb.indexCount / 3;
            }
        }
        if (blob) pp::pakFree(blob);
        float w[3] = {0, 0, 0};
        if (p) standPos(*p, w);
        std::printf("  slot %2u display %u side %u -> point %2u %s model a_%u %s (%u KB, %u batches, %u tris) skin %s%s\n",
                    a.slot, a.displayId, a.side, a.point, p ? "" : "(NO POINT: not drawn)", a.model,
                    ok ? "ok" : "INVALID", size / 1024, batches, tris,
                    a.texture != rp::kNoItemLayer ? ("t_" + std::to_string(a.texture)).c_str() : "none",
                    a.hasLocal ? " +sheath transform" : "");
        if (p) std::printf("      at stand (%.2f %.2f %.2f)\n", w[0], w[1], w[2]);
        if (!ok) ++errors;
    }
    for (int slot : {0, 2, 15, 16, 17})
        if (eq.displayId[slot] && !bank.models(eq.displayId[slot]))
            std::printf("  slot %d display %u: no item model baked (attach_missing on the PSP)\n", slot,
                        eq.displayId[slot]);
    return errors;
}

std::string groups(const std::unordered_set<uint16_t>& g) {
    std::set<uint16_t> s;
    for (uint16_t v : g)
        if (v / 100 == 4 || v / 100 == 5 || v / 100 == 8 || v / 100 == 13 || v / 100 == 15 || v / 100 == 18 ||
            v < 100)
            s.insert(v);
    std::string out;
    for (uint16_t v : s) out += std::to_string(v) + " ";
    return out;
}

} // namespace

namespace eqm {
int runMatrix(int argc, char** argv);  // equip_matrix/matrix_driver.cpp
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)  // --matrix runs on its own
        if (std::strcmp(argv[i], "--matrix") == 0) return eqm::runMatrix(argc, argv);
    std::string chars, items, root, out = "body.ppm";
    int race = 1, sex = 0, cls = 1, skin = 0, face = 0, hair = 1;
    bool local = false, sheathed = false, verifyItems = false, outfit = true;  // attachments
    std::vector<std::string> displays;
    std::vector<std::string> itemArgs;  // --item <slot>=<item id> through the Item.dbc table
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool v = i + 1 < argc;
        if (a == "--chars" && v) chars = argv[++i];
        else if (a == "--items" && v) items = argv[++i];
        else if (a == "--extracted" && v) root = argv[++i];
        else if (a == "--out" && v) out = argv[++i];
        else if (a == "--race" && v) race = std::atoi(argv[++i]);
        else if (a == "--sex" && v) sex = std::atoi(argv[++i]);
        else if (a == "--class" && v) cls = std::atoi(argv[++i]);
        else if (a == "--skin" && v) skin = std::atoi(argv[++i]);
        else if (a == "--face" && v) face = std::atoi(argv[++i]);
        else if (a == "--hair" && v) hair = std::atoi(argv[++i]);
        else if (a == "--display" && v) displays.push_back(argv[++i]);
        else if (a == "--item" && v) itemArgs.push_back(argv[++i]);  // an item id through the Item.dbc table
        else if (a == "--local") local = true;
        else if (a == "--sheathed") sheathed = true;
        else if (a == "--verify-items") verifyItems = true;
        else if (a == "--no-outfit") outfit = false;
    }
    pp::Pak cp;
    rp::ItemBank bank;
    if (!cp.open(chars.c_str()) || !bank.open(items.c_str())) {
        std::fprintf(stderr, "cannot open %s / %s\n", chars.c_str(), items.c_str());
        return 1;
    }
    int verifyErrors = verifyItems ? verifyItemsPak(items, bank) : 0;
    // Starter outfit (CharStartOutfit.dbc, 3.3.5a: u8 race/class/sex/outfit at
    // byte 4, 24 item ids, 24 display ids, 24 inventory types).
    rp::Equipment eq;
    if (outfit) {
        std::vector<uint8_t> dbc;
        if (!pspconv::readFile(root + "/dbfilesclient/charstartoutfit.dbc", dbc) || dbc.size() < 20) return 1;
        uint32_t hdr[5];
        std::memcpy(hdr, dbc.data(), 20);
        for (uint32_t r = 0; r < hdr[1]; ++r) {
            const uint8_t* rec = dbc.data() + 20 + r * hdr[3];
            if (rec[4] != race || rec[5] != cls || rec[6] != sex) continue;
            for (int k = 0; k < 24; ++k) {
                uint32_t d, t;
                std::memcpy(&d, rec + 104 + 4 * k, 4);
                std::memcpy(&t, rec + 200 + 4 * k, 4);
                const int s = slotFor(static_cast<uint8_t>(t));
                if (d && d != 0xFFFFFFFFu && s >= 0 && !eq.displayId[s]) {
                    eq.displayId[s] = d;
                    eq.invType[s] = static_cast<uint8_t>(t);
                }
            }
            break;
        }
    }
    // --display <slot>=<displayId>[:<inventoryType>].
    for (const std::string& d : displays) {
        unsigned slot = 0, id = 0, inv = 0;
        if (std::sscanf(d.c_str(), "%u=%u:%u", &slot, &id, &inv) < 2 || slot >= 19) continue;
        eq.displayId[slot] = id;
        eq.invType[slot] = static_cast<uint8_t>(inv);
    }
    // --item <slot>=<item id> - what another player's PLAYER_VISIBLE_ITEM entry resolves to on
    // the PSP (ItemBank::itemDisplay, the Item.dbc table of items.pak).
    for (const std::string& d : itemArgs) {
        unsigned slot = 0, item = 0;
        if (std::sscanf(d.c_str(), "%u=%u", &slot, &item) < 2 || slot >= 19) continue;
        uint32_t displayId = 0;
        uint8_t inv = 0;
        const bool ok = bank.itemDisplay(item, displayId, inv);
        std::printf("item %u -> %s display %u inventory type %u (slot %u)\n", item, ok ? "Item.dbc" : "NOT IN TABLE",
                    displayId, inv, slot);
        if (!ok) {
            ++verifyErrors;
            continue;
        }
        eq.displayId[slot] = displayId;
        eq.invType[slot] = inv;
    }
    std::printf("outfit race=%d class=%d sex=%d:", race, cls, sex);
    for (int s = 0; s < 19; ++s)
        if (eq.displayId[s]) std::printf(" [%d]%u/inv%u%s", s, eq.displayId[s], eq.invType[s], bank.get(eq.displayId[s]) ? "" : "(not baked)");
    std::printf("\n");

    // Model geosets (static model "c_": one u16 per batch).
    char name[32];
    std::snprintf(name, sizeof(name), "c_%d_%d", race, sex);
    const pp::PakEntry* me = cp.find(name);
    auto* mb = me ? static_cast<uint8_t*>(cp.load(me)) : nullptr;
    if (!mb) return 1;
    const auto* mh = reinterpret_cast<const rp::ModelHeader*>(mb);
    std::unordered_set<uint16_t> modelGeosets;
    for (uint32_t i = 0; i < mh->batchCount[0]; ++i) {
        uint16_t g;
        std::memcpy(&g, mb + mh->geosetOffset + i * 2, 2);
        modelGeosets.insert(g);
    }
    pp::pakFree(mb);

    // Appearance: body layers + hair scalp.
    std::snprintf(name, sizeof(name), "a_%d_%d", race, sex);
    const pp::PakEntry* ae = cp.find(name);
    auto* app = ae ? static_cast<uint8_t*>(cp.load(ae)) : nullptr;
    if (!app) return 1;
    const auto* ah = reinterpret_cast<const rp::AppearanceHeader*>(app);
    uint16_t layers[rp::kBodyLayers];
    std::memcpy(layers, app + ah->bodyOffset + ((skin * ah->faces + face) * rp::kBodyLayers) * 2u, sizeof(layers));
    uint16_t scalp = 1;
    std::memcpy(&scalp, app + ah->hairGeosetOffset + hair * 2u, 2);
    if (!scalp) scalp = 1;
    pp::pakFree(app);

    auto bare = core::bareCharacterGeosets(scalp, 1, 1, 1, static_cast<uint8_t>(race));
    bare.insert(core::kGeosetNoCape);
    auto dressed = bare;
    rp::applyEquipmentGeosets(dressed, scalp, hair > 0, eq, static_cast<uint8_t>(sex), bank, modelGeosets);
    std::printf("geosets bare:    %s\ngeosets dressed: %s\n", groups(bare).c_str(), groups(dressed).c_str());
    const uint16_t cape = rp::capeTexture(eq, static_cast<uint8_t>(sex), bank);
    uint32_t capeSize = 0;
    void* capeBlob = cape != rp::kNoItemLayer ? bank.loadTexture(cape, capeSize) : nullptr;
    std::printf("cape texture %s (%u bytes)\n", cape != rp::kNoItemLayer ? ("t_" + std::to_string(cape)).c_str() : "none",
                capeSize);
    if (capeBlob) pp::pakFree(capeBlob);

    std::vector<uint32_t> canvas(rp::kBodyTexSize * rp::kBodyTexSize, 0xFF808080u);
    for (int i = 0; i < rp::kBodyLayers; ++i) {
        if (layers[i] == rp::kNoLayer) continue;
        std::snprintf(name, sizeof(name), "l_%u", layers[i]);
        const pp::PakEntry* le = cp.find(name);
        auto* lb = le ? static_cast<uint8_t*>(cp.load(le)) : nullptr;
        if (!lb) continue;
        blitLayer(canvas.data(), lb, le->size, i == 0);
        pp::pakFree(lb);
    }
    writePpm(out + ".bare.ppm", canvas);
    const auto ids = rp::equipmentLayers(eq, static_cast<uint8_t>(sex), bank);
    for (uint16_t id : ids) {
        uint32_t size = 0;
        auto* lb = static_cast<uint8_t*>(bank.loadLayer(id, size));
        if (!lb) continue;
        const auto* lh = reinterpret_cast<const rp::LayerHeader*>(lb);
        std::printf("  layer q_%u at %u,%u %ux%u\n", id, lh->x, lh->y, lh->w, lh->h);
        blitLayer(canvas.data(), lb, size, false);
        pp::pakFree(lb);
    }
    writePpm(out, canvas);
    std::printf("item layers %zu -> %s\n", ids.size(), out.c_str());
    verifyErrors += printAttachments(cp, bank, eq, race, sex, local, sheathed);  // what the PSP attaches
    bank.logStats("end");  // what the PSP's ItemBank would hold and read for this
    return verifyErrors ? 3 : 0;
}
