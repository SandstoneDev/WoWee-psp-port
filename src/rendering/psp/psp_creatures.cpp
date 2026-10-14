#include "rendering/psp/psp_creatures.hpp"
#include "platform/psp/psp_prof.hpp"

#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_bigheap.hpp"
#include "platform/psp/psp_log.hpp"
#include "rendering/animation/animation_ids.hpp"        // Creature anim
#include "rendering/animation/melee_anim_chains.hpp"   // Creature anim
#include "rendering/psp/psp_attachments.hpp"           // NPC equipment
#include "rendering/psp/psp_equipment.hpp"             // NPC equipment
#include "rendering/psp/psp_character.hpp"             // batch 5 merge: CharacterModel::sheathClient
#include "rendering/psp/psp_frustum.hpp"
#include "rendering/psp/psp_lighting_ge.hpp"  // Lighting

#include <pspgu.h>
#include <pspgum.h>
#include <pspthreadman.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace wowee::rendering::psp {

namespace pp = wowee::platform::psp;
namespace prof = wowee::platform::psp::prof;

namespace {

constexpr int kVertexType = GU_INDEX_16BIT | GU_TEXTURE_16BIT | GU_COLOR_5650 | GU_VERTEX_16BIT | GU_TRANSFORM_3D;
constexpr int kStripType = GU_TEXTURE_16BIT | GU_COLOR_5650 | GU_VERTEX_16BIT | GU_TRANSFORM_3D;
constexpr uint32_t kKeepFrames = 2;  // a resource drawn this recently may still be read by the GE
constexpr int32_t kDisplayAbsent = -2;  // displayIdx_ value for "not in the pak"
// Creature anim - display-list bytes after which no more skinned
// NPCs are drawn in a frame (the list is 256 KB; the player, other players,
// HUD and UI come after the NPCs).
// The display list is 512 KB - the skinned units stop at kListReserveNpc bytes free (psp_display.hpp).

bool isBlended(uint8_t blend) { return blend >= kBlendAlpha; }

// Memory diet (docs/PSP_PORT_ARCHITECTURE.md): the fair-sharing rule - one block of the entry's size
// and `marginKb` KB still free in total after it (pp::heapRoomFor; was one block of size + margin: on the fragmented
// hw5 heap - 11 MB free, 3 MB largest - npcheapkb 3072 never passed and no NPC model ever loaded).
bool heapRoomFor(uint32_t bytes, uint32_t marginKb) { return pp::heapRoomFor(bytes, marginKb); }

// "<prefix><number>" -> number, or -1.
long entryNumber(const char* name, const char* prefix) {
    const size_t n = std::strlen(prefix);
    if (std::strncmp(name, prefix, n) != 0 || name[n] < '0' || name[n] > '9') return -1;
    char* end = nullptr;
    const long v = std::strtol(name + n, &end, 10);
    return (end && *end == '\0') ? v : -1;
}

} // namespace

CreatureRenderer::~CreatureRenderer() { close(); }

// a sorted pak (every display of the DBCs) opens through PakIndex - nothing is read but
// its fence; an older pak through Pak (TOC in RAM, as before). Displays are found when a unit needs them.
bool CreatureRenderer::open(const char* absPath) {
    close();
    entryCache_.assign(kEntryCacheSlots, EntrySlot{});
    displays_.clear();
    displays_.reserve(kDisplayPool);  // Display* in visible_ stay valid: the pool never grows past this
    if (index_.open(absPath)) {
        PSP_LOGI(pp::tag::Gfx, "npc_pak sorted records=%u index_kb=%u pool=%u entry_cache=%u",
                 static_cast<unsigned>(index_.count()), static_cast<unsigned>(index_.ramBytes() / 1024u),
                 static_cast<unsigned>(kDisplayPool), static_cast<unsigned>(kEntryCacheSlots));
        return true;
    }
    if (!pak_.open(absPath)) return false;
    uint32_t displays = 0, models = 0, skins = 0;
    for (uint32_t i = 0; i < pak_.count(); ++i) {
        const char* name = pak_.entry(i)->name;
        displays += entryNumber(name, "d_") >= 0 ? 1u : 0u;
        models += entryNumber(name, "m_") >= 0 ? 1u : 0u;
        skins += entryNumber(name, "n_") >= 0 ? 1u : 0u;
    }
    PSP_LOGI(pp::tag::Gfx, "npc_pak unsorted (older bake) displays=%u models=%u skins=%u records=%u",
             static_cast<unsigned>(displays), static_cast<unsigned>(models), static_cast<unsigned>(skins),
             static_cast<unsigned>(pak_.count()));
    return displays > 0;
}

void CreatureRenderer::close() {
    for (auto& [id, m] : models_)
        if (m.blob) pp::pakFree(m.blob);
    for (auto& [id, t] : textures_)
        if (t.loaded) pp::textureRelease(t.t);
    for (auto& [id, k] : skins_)  // Creature anim
        if (k.blob) pp::pakFree(k.blob);
    skins_.clear();
    seats_.clear();    // Mounts
    anims_.clear();    // releases the NPC equipment too
    models_.clear();
    textures_.clear();
    displays_.clear();
    freeDisplays_.clear();
    displayIdx_.clear();
    units_.clear();
    visible_.clear();
    entryCache_.clear();
    memoryBytes_ = 0;
    stats_ = CreatureStats{};
    index_.close();
    pak_.close();
}

uint32_t CreatureRenderer::releaseModels() {
    const uint32_t held = memoryBytes_;
    for (auto& [id, m] : models_)
        if (m.blob) pp::pakFree(m.blob);
    for (auto& [id, t] : textures_)
        if (t.loaded) pp::textureRelease(t.t);
    for (auto& [id, k] : skins_)
        if (k.blob) pp::pakFree(k.blob);
    models_.clear();
    textures_.clear();
    skins_.clear();
    seats_.clear();
    for (auto& [g, a] : anims_) {  // as evict(): the states rebind (and dress again) when their unit is seen
        a.skin = -1;
        a.attach.reset();
        a.attachKey = 0;
        a.poseValid = false;
        a.dirty = true;
    }
    visible_.clear();
    memoryBytes_ = 0;
    return held;
}

// near_starve_flush: as evict() with no budget - every model / texture / skin no unit drew for
// idleFrames frames goes (the states bound to a skin rebind, as evict()). Returns the bytes freed.
uint32_t CreatureRenderer::releaseUnused(uint32_t idleFrames) {
    const uint32_t before = memoryBytes_;
    const uint32_t keep = std::max(idleFrames, kKeepFrames);
    std::vector<uint16_t> ids;
    for (const auto& [id, k] : skins_)
        if (k.blob && k.lastUsed + keep < frame_) ids.push_back(id);
    for (uint16_t id : ids) {
        auto it = skins_.find(id);
        pp::pakFree(it->second.blob);
        memoryBytes_ -= std::min(memoryBytes_, it->second.bytes);
        skins_.erase(it);
        seats_.erase(id);
        for (auto& [g, a] : anims_)
            if (a.skin == static_cast<int32_t>(id)) {
                a.skin = -1;
                a.attach.reset();
                a.attachKey = 0;
            }
    }
    ids.clear();
    for (const auto& [id, m] : models_)
        if (m.blob && m.lastUsed + keep < frame_) ids.push_back(id);
    for (uint16_t id : ids) {
        auto it = models_.find(id);
        pp::pakFree(it->second.blob);
        memoryBytes_ -= std::min(memoryBytes_, it->second.bytes);
        models_.erase(it);
        if (!skins_.count(id)) seats_.erase(id);
    }
    ids.clear();
    for (const auto& [id, t] : textures_)
        if (t.loaded && t.lastUsed + keep < frame_) ids.push_back(id);
    for (uint16_t id : ids) {
        auto it = textures_.find(id);
        pp::textureRelease(it->second.t);
        memoryBytes_ -= std::min(memoryBytes_, it->second.bytes);
        textures_.erase(it);
    }
    return before - memoryBytes_;
}

uint32_t CreatureRenderer::indexRamBytes() const {
    if (index_.isOpen()) return index_.ramBytes();
    return pak_.isOpen() ? pak_.count() * static_cast<uint32_t>(sizeof(pp::PakEntry) + 8u) : 0u;  // records + ~names
}

// ---- entries on demand ---------------------------------

bool CreatureRenderer::tocBudgetLeft() const {
    return !budgetToc_ || !index_.isOpen() || index_.blockReads() - frameTocReads0_ < tocReadsPerFrame;
}

// The place of "<kind>_<id>" in the pak: the entry cache, else a lookup (PakIndex: at most one TOC block
// read; Pak: its TOC in RAM). A missing entry is not cached (the caller marks its resource failed).
bool CreatureRenderer::findEntry(EntryKind kind, uint32_t id, pp::PakEntry& out) {
    static const char* const kPrefix[] = {"d_", "m_", "n_", "p_", "x_"};
    const uint32_t key = (static_cast<uint32_t>(kind) << 24) | (id & 0xFFFFFFu);
    EntrySlot* slot = entryCache_.empty() ? nullptr : &entryCache_[(key * 2654435761u >> 7) % kEntryCacheSlots];
    if (slot && slot->key == key) {
        out = pp::PakEntry{kPrefix[kind], slot->offset, slot->size, 0};
        return true;
    }
    char name[24];
    std::snprintf(name, sizeof(name), "%s%u", kPrefix[kind], static_cast<unsigned>(id));
    ++lookups_;
    bool ok = false;
    if (index_.isOpen()) {
        const uint32_t before = index_.blockReads();
        ok = index_.find(name, out);
        if (index_.blockReads() != before) ++lookupMisses_;
    } else if (const pp::PakEntry* e = pak_.isOpen() ? pak_.find(name) : nullptr) {
        out = *e;
        ok = true;
    }
    if (ok && slot) *slot = EntrySlot{key, out.offset, out.size};
    return ok;
}

void* CreatureRenderer::loadEntry(const pp::PakEntry& e) {
    if (index_.isOpen()) return index_.load(e);
    return pak_.isOpen() ? pak_.load(&e) : nullptr;
}

// a record by name (the animated game objects' list "goanim").
bool CreatureRenderer::readRecord(const char* name, std::vector<uint8_t>& out) {
    out.clear();
    pp::PakEntry e{};
    bool ok = false;
    if (index_.isOpen()) {
        ok = index_.find(name, e);
    } else if (const pp::PakEntry* pe = pak_.isOpen() ? pak_.find(name) : nullptr) {
        e = *pe;
        ok = true;
    }
    void* blob = ok ? loadEntry(e) : nullptr;
    if (!blob) return false;
    out.assign(static_cast<const uint8_t*>(blob), static_cast<const uint8_t*>(blob) + e.size);
    pp::pakFree(blob);
    return true;
}

void CreatureRenderer::setUnits(const CreatureUnit* units, size_t count) {
    // Keep the display lookups of units that stay (same guid and display).
    std::vector<Unit> next(count);
    for (size_t i = 0; i < count; ++i) {
        next[i].u = units[i];
        if (i < units_.size() && units_[i].u.guid == units[i].guid && units_[i].u.displayId == units[i].displayId)
            next[i].display = units_[i].display;
    }
    units_.swap(next);
}

size_t CreatureRenderer::appendStaticSpawns(uint32_t mapId, int tileX, int tileY, std::vector<CreatureUnit>& out) {
    char name[32];
    std::snprintf(name, sizeof(name), "s_%u_%d_%d", static_cast<unsigned>(mapId), tileX, tileY);
    pp::PakEntry e{};
    bool found = false;
    if (index_.isOpen()) {
        found = index_.find(name, e);
    } else if (const pp::PakEntry* pe = pak_.isOpen() ? pak_.find(name) : nullptr) {
        e = *pe;
        found = true;
    }
    auto* blob = found ? static_cast<uint8_t*>(loadEntry(e)) : nullptr;
    if (!blob) return 0;
    const auto* h = reinterpret_cast<const CreatureSpawnHeader*>(blob);
    size_t added = 0;
    if (e.size >= sizeof(CreatureSpawnHeader) && std::memcmp(h->magic, "WCSP", 4) == 0 &&
        h->version == kModelFormatVersion && sizeof(CreatureSpawnHeader) + h->count * sizeof(CreatureSpawn) <= e.size) {
        const auto* s = reinterpret_cast<const CreatureSpawn*>(blob + sizeof(CreatureSpawnHeader));
        for (uint32_t i = 0; i < h->count; ++i) {
            CreatureUnit u;
            // The GUID the server would send: HIGHGUID_UNIT, entry, low guid.
            u.guid = (0xF130ull << 48) | (static_cast<uint64_t>(s[i].entry & 0xFFFFFFu) << 24) | (s[i].guid & 0xFFFFFFu);
            u.displayId = s[i].displayId;
            u.x = s[i].position[0];
            u.y = s[i].position[1];
            u.z = s[i].position[2];
            u.orientation = s[i].orientation;
            u.scale = s[i].scale > 0.0f ? s[i].scale : 1.0f;
            out.push_back(u);
            ++added;
        }
    } else {
        PSP_LOGE(pp::tag::Gfx, "wcsp_bad name=%s size=%u", name, static_cast<unsigned>(e.size));
    }
    pp::pakFree(blob);
    return added;
}

bool CreatureRenderer::spawnEntry(uint32_t mapId, int tileX, int tileY, uint32_t& offset, uint32_t& size) {
    char name[32];
    std::snprintf(name, sizeof(name), "s_%u_%d_%d", static_cast<unsigned>(mapId), tileX, tileY);
    pp::PakEntry e{};
    if (index_.isOpen()) {
        if (!index_.find(name, e)) return false;
    } else if (const pp::PakEntry* pe = pak_.isOpen() ? pak_.find(name) : nullptr) {
        e = *pe;
    } else {
        return false;
    }
    offset = e.offset;
    size = e.size;
    return true;
}

uint32_t CreatureRenderer::prefetch(float x, float y, float z) {
    std::vector<std::pair<float, Unit*>> near;
    for (Unit& un : units_) {
        const float dx = un.u.x - x, dy = un.u.y - y, dz = un.u.z - z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > viewDistance * viewDistance) continue;
        near.emplace_back(d2, &un);
    }
    std::sort(near.begin(), near.end(),
              [](const std::pair<float, Unit*>& a, const std::pair<float, Unit*>& b) { return a.first < b.first; });
    if (near.size() > maxDrawn) near.resize(maxDrawn);
    stats_.loads = 0;
    uint32_t budget = 0xFFFFFFFFu;
    // PSP (the emulator with the device's heap, 40.9 MB: world entry's 78 reads / 2.1 MB at once ran the heap
    // out - "oom new failed" ends the game): every read asks roomFor(); the first that finds no room ends the prefetch,
    // the rest comes frame by frame under the memory governor.
    // PSP (hardware: the prefetch read 2.4 MB down to the 768 KB margin and the next allocation
    // failed): the prefetch keeps prefetchReserveKb free in total (npcprefetchkb, 3072) - the room the next consumers
    // of world entry need (a terrain tile, the big WMO the player stands in, a window's ui.pak art, Lua); the rest of
    // the NPCs come frame by frame under heapReserveKb.
    struct Prefetching {
        bool& on;
        explicit Prefetching(bool& b) : on(b) { on = true; }
        ~Prefetching() { on = false; }
    } scope(prefetching_);
    for (const auto& p : near) {
        if (heapHeldFrame_ == frame_) break;
        Unit& un = *p.second;
        if (un.display == -1) un.display = findDisplay(un.u.displayId, true);
        if (un.display >= 0) ensure(displays_[static_cast<size_t>(un.display)], budget);
    }
    if (heapHeldFrame_ == frame_)
        PSP_LOGW(pp::tag::Gfx, "npc_prefetch_stop low_heap loads=%u", static_cast<unsigned>(stats_.loads));
    stats_.memoryKb = memoryBytes_ / 1024u;
    return stats_.loads;
}

// the display's slot in the pool - known, or (lookup) its record found in the pak and a
// slot taken for it (the least recently used display not drawn in the last frames gives its slot up when the
// pool is full). -2: not in the pak (remembered); -1: not now (no lookup, the TOC budget or a full pool).
int32_t CreatureRenderer::findDisplay(uint32_t displayId, bool lookup) {
    auto it = displayIdx_.find(displayId);
    if (it != displayIdx_.end()) return it->second;
    if (!lookup || !tocBudgetLeft()) return -1;
    pp::PakEntry e{};
    if (!findEntry(kEntryDisplay, displayId, e)) {
        displayIdx_.emplace(displayId, kDisplayAbsent);
        return kDisplayAbsent;
    }
    int32_t slot = -1;
    if (!freeDisplays_.empty()) {
        slot = freeDisplays_.back();
        freeDisplays_.pop_back();
    } else if (displays_.size() < kDisplayPool) {
        slot = static_cast<int32_t>(displays_.size());
        displays_.emplace_back();
    } else {
        evictDisplays();
        if (freeDisplays_.empty()) return -1;
        slot = freeDisplays_.back();
        freeDisplays_.pop_back();
    }
    Display& d = displays_[static_cast<size_t>(slot)];
    d = Display{};
    d.id = displayId;
    d.lastUsed = frame_;
    displayIdx_[displayId] = slot;
    return slot;
}

int32_t CreatureRenderer::findDisplay(uint32_t displayId) const {
    auto it = displayIdx_.find(displayId);
    return it == displayIdx_.end() ? -1 : it->second;
}

// Frees the slots of displays not used for a while (oldest first, a quarter of the pool at most), the units
// that pointed at them look theirs up again. The negative cache is cleared when it grows.
void CreatureRenderer::evictDisplays() {
    std::vector<std::pair<uint32_t, int32_t>> old;
    for (size_t i = 0; i < displays_.size(); ++i) {
        const Display& d = displays_[i];
        if (d.id == 0 || d.lastUsed + kKeepFrames + 30u >= frame_) continue;
        if (std::find(freeDisplays_.begin(), freeDisplays_.end(), static_cast<int32_t>(i)) != freeDisplays_.end())
            continue;
        old.emplace_back(d.lastUsed, static_cast<int32_t>(i));
    }
    std::sort(old.begin(), old.end());
    if (old.size() > kDisplayPool / 4) old.resize(kDisplayPool / 4);
    for (const auto& [last, slot] : old) {
        Display& d = displays_[static_cast<size_t>(slot)];
        displayIdx_.erase(d.id);
        d = Display{};
        freeDisplays_.push_back(slot);
        ++displayEvictions_;
    }
    if (!old.empty())
        for (Unit& un : units_)
            if (un.display >= 0 &&
                std::find(freeDisplays_.begin(), freeDisplays_.end(), un.display) != freeDisplays_.end())
                un.display = -1;
    if (displayIdx_.size() > 4096) {  // mostly "not in the pak" answers by now
        for (auto it = displayIdx_.begin(); it != displayIdx_.end();)
            it = it->second == kDisplayAbsent ? displayIdx_.erase(it) : std::next(it);
    }
}

// See the header. One failed probe holds the rest of the frame (no probe per candidate).
bool CreatureRenderer::roomFor(uint32_t size) {
    if (!heapReserveKb) return true;
    if (heapHeldFrame_ == frame_) return false;
    // Its own block + the margin in total (the world-entry prefetch keeps the larger one).
    if (heapRoomFor(size, prefetching_ ? std::max(heapReserveKb, prefetchReserveKb) : heapReserveKb)) return true;
    heapHeldFrame_ = frame_;
    ++stats_.heapHeld;
    return false;
}

bool CreatureRenderer::loadDisplay(Display& d) {
    PROF_SCOPE("npc.load");
    pp::PakEntry e{};
    const bool found = findEntry(kEntryDisplay, d.id, e);
    if (found && !roomFor(e.size)) return false;  // Waits, not failed
    auto* blob = found ? static_cast<uint8_t*>(loadEntry(e)) : nullptr;
    ++stats_.loads;
    if (!blob) {
        d.failed = true;
        return false;
    }
    std::memcpy(&d.h, blob, std::min<uint32_t>(e.size, sizeof(CreatureDisplayHeader)));
    if (e.size < sizeof(CreatureDisplayHeader) || std::memcmp(d.h.magic, "WCDI", 4) != 0 ||
        d.h.version != kModelFormatVersion || sizeof(CreatureDisplayHeader) + d.h.batchCount > e.size) {
        PSP_LOGE(pp::tag::Gfx, "wcdi_bad display=%u size=%u", static_cast<unsigned>(d.id), static_cast<unsigned>(e.size));
        d.failed = true;
    } else {
        d.active.assign(blob + sizeof(CreatureDisplayHeader), blob + sizeof(CreatureDisplayHeader) + d.h.batchCount);
        // the humanoid's equipment (helm / shoulders from items.pak at run time).
        if (const CreatureDisplayEquip* q = creatureDisplayEquip(blob, e.size)) {
            d.equip = *q;
            d.hasEquip = true;
        }
        d.loaded = true;
    }
    pp::pakFree(blob);
    return d.loaded;
}

bool CreatureRenderer::loadModel(uint16_t id) {
    PROF_SCOPE("npc.load");
    pp::PakEntry e{};
    const bool found = findEntry(kEntryModel, id, e);
    if (found && !roomFor(e.size)) return false;  // Waits, not failed
    Model& m = models_[id];
    auto* blob = found ? static_cast<uint8_t*>(loadEntry(e)) : nullptr;
    ++stats_.loads;
    if (!blob) {
        m.failed = true;
        return false;
    }
    const auto* h = reinterpret_cast<const ModelHeader*>(blob);
    const uint32_t batches = static_cast<uint32_t>(h->batchCount[0]) + h->batchCount[1];
    bool ok = e.size >= sizeof(ModelHeader) && std::memcmp(h->magic, "WMDL", 4) == 0 &&
              h->version == kModelFormatVersion && h->vertexOffset % 16 == 0 && h->indexOffset % 2 == 0 &&
              h->vertexOffset + h->vertexCount * sizeof(ModelVertex) <= e.size &&
              h->indexOffset + h->indexCount * 2u <= e.size && h->batchOffset + batches * sizeof(ModelBatch) <= e.size;
    const auto* b = reinterpret_cast<const ModelBatch*>(blob + h->batchOffset);
    const uint32_t limit = (h->flags & kModelStrips) ? h->vertexCount : h->indexCount;
    for (uint32_t i = 0; ok && i < batches; ++i) ok = b[i].indexStart + b[i].indexCount <= limit;
    if (!ok) {
        PSP_LOGE(pp::tag::Gfx, "npc_wmdl_bad model=%u size=%u", static_cast<unsigned>(id), static_cast<unsigned>(e.size));
        pp::pakFree(blob);
        m.failed = true;
        return false;
    }
    m.blob = blob;
    m.h = h;
    m.verts = reinterpret_cast<const ModelVertex*>(blob + h->vertexOffset);
    m.indices = reinterpret_cast<const uint16_t*>(blob + h->indexOffset);
    m.batches = b;
    m.bytes = e.size;
    m.lastUsed = frame_;
    memoryBytes_ += m.bytes;
    return true;
}

bool CreatureRenderer::loadTexture(uint16_t id) {
    PROF_SCOPE("npc.load");
    pp::PakEntry e{};
    const bool found = findEntry(kEntryTexture, id, e);
    if (found && !roomFor(e.size)) return false;  // Waits, not failed (ensure() holds the unit)
    Tex& t = textures_[id];
    void* blob = found ? loadEntry(e) : nullptr;
    ++stats_.loads;
    if (!blob || !pp::textureFromBlob(blob, e.size, t.t, true)) {
        if (blob) pp::pakFree(blob);
        t.failed = true;  // drawn white from now on
        return false;
    }
    t.loaded = true;
    t.bytes = e.size;
    t.lastUsed = frame_;
    memoryBytes_ += t.bytes;
    return true;
}

bool CreatureRenderer::ensure(Display& d, uint32_t& budget) {
    d.lastUsed = frame_;
    if (d.failed) return false;
    if (!d.loaded) {
        if (!budget || !tocBudgetLeft()) return false;
        --budget;
        if (!loadDisplay(d)) return false;
    }
    // Creature anim - the skinned model ("n_") instead of the
    // static one when the pak has it; a bad or mismatching one falls back.
    if (d.skinMode == 0) {
        if (!animate_) {
            d.skinMode = 2;
        } else {
            auto it = skins_.find(d.h.model);
            if (it != skins_.end() && it->second.failed) {
                d.skinMode = 2;
            } else if (it == skins_.end() || !it->second.blob) {
                if (!budget || !tocBudgetLeft()) return false;
                --budget;
                const bool ok = loadSkin(d.h.model);
                if (!ok && heapHeldFrame_ == frame_) return false;  // No room yet - not the static model
                d.skinMode = ok ? 1 : 2;
            } else {
                d.skinMode = 1;
            }
        }
    }
    const ModelBatch* batchList = nullptr;
    uint32_t batches = 0;
    if (d.skinMode == 1) {
        Skin* sk = skinOf(d.h.model);
        if (!sk || (!sk->blob && !sk->failed)) {  // evicted since: read it again
            if (!budget || !tocBudgetLeft()) return false;
            --budget;
            if (!loadSkin(d.h.model) && heapHeldFrame_ == frame_) return false;  // No room yet
            sk = skinOf(d.h.model);
        }
        if (sk && sk->blob && sk->view.h->batchCount != d.h.batchCount) {
            PSP_LOGW(pp::tag::Gfx, "npc_skin_mismatch display=%u", static_cast<unsigned>(d.id));
            d.skinMode = 2;
        } else if (!sk || sk->failed || !sk->blob) {
            d.skinMode = 2;
        } else {
            batchList = sk->view.batches;
            batches = sk->view.h->batchCount;
        }
    }
    if (d.skinMode == 2) {
        auto it = models_.find(d.h.model);
        if (it != models_.end() && it->second.failed) {
            d.failed = true;  // neither "n_" nor "m_": never drawn (not white)
            return false;
        }
        if (it == models_.end() || !it->second.blob) {
            if (!budget || !tocBudgetLeft()) return false;
            --budget;
            if (!loadModel(d.h.model)) return false;
            it = models_.find(d.h.model);
        }
        batchList = it->second.batches;
        batches = static_cast<uint32_t>(it->second.h->batchCount[0]) + it->second.h->batchCount[1];
    }
    if (!d.texturesKnown) {
        if (batches != d.h.batchCount) {
            PSP_LOGE(pp::tag::Gfx, "npc_mask_mismatch display=%u", static_cast<unsigned>(d.id));
            d.failed = true;
            return false;
        }
        for (uint32_t i = 0; i < batches; ++i) {
            if (!d.active[i]) continue;
            uint16_t id = batchList[i].texture;
            // a cape batch takes the cape texture (slot 2) when the display has one.
            if (d.active[i] == kBatchCape && d.h.slotTexture[2] != kNoTexture) id = d.h.slotTexture[2];
            if (id >= kTexSlot && id != kNoTexture) id = d.h.slotTexture[id - kTexSlot];
            if (id < kTexSlot && std::find(d.textures.begin(), d.textures.end(), id) == d.textures.end())
                d.textures.push_back(id);
        }
        d.texturesKnown = true;
    }
    for (uint16_t id : d.textures) {
        auto it = textures_.find(id);
        if (it != textures_.end() && (it->second.loaded || it->second.failed)) continue;
        if (!budget || !tocBudgetLeft()) return false;
        --budget;
        if (!loadTexture(id) && heapHeldFrame_ == frame_) return false;  // No room yet - not white
    }
    return true;
}

const pp::Texture* CreatureRenderer::texture(const Display& d, uint16_t id, uint8_t active) const {
    if (active == kBatchCape && d.h.slotTexture[2] != kNoTexture) id = d.h.slotTexture[2];
    if (id >= kTexSlot && id != kNoTexture) id = d.h.slotTexture[id - kTexSlot];
    if (id >= kTexSlot) return nullptr;
    auto it = textures_.find(id);
    if (it == textures_.end() || !it->second.loaded) return nullptr;
    return &it->second.t;
}

// The model's UV scale/offset are set once per model by the caller.
void CreatureRenderer::setBatchState(const Display& d, const ModelBatch& b, uint8_t active) {
    pp::textureBind(texture(d, b.texture, active));
    sceGuTexWrap((b.flags & kBatchWrapU) ? GU_REPEAT : GU_CLAMP, (b.flags & kBatchWrapV) ? GU_REPEAT : GU_CLAMP);
    switch (b.blend) {
        case kBlendOpaque: sceGuDisable(GU_ALPHA_TEST); break;
        case kBlendAlphaKey:
            sceGuEnable(GU_ALPHA_TEST);
            sceGuAlphaFunc(GU_GREATER, 0x66, 0xFF);  // upstream m2.frag: 0.4
            break;
        case kBlendAlpha: sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0); break;
        case kBlendAdd: sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0xFFFFFFFFu); break;
        default: sceGuBlendFunc(GU_ADD, GU_DST_COLOR, GU_FIX, 0, 0); break;  // modulate
    }
}

void CreatureRenderer::drawBatch(const Model& m, const ModelBatch& b) {
    prof::count(prof::kDraws, 1);
    prof::count(prof::kVertices, b.indexCount);
    if (m.h->flags & kModelStrips)
        sceGuDrawArray(GU_TRIANGLE_STRIP, kStripType, static_cast<int>(b.indexCount), nullptr, m.verts + b.indexStart);
    else
        sceGuDrawArray(GU_TRIANGLES, kVertexType, static_cast<int>(b.indexCount), m.indices + b.indexStart, m.verts);
    ++stats_.batches;
    const uint32_t tris = (m.h->flags & kModelStrips) ? (b.indexCount > 2 ? b.indexCount - 2 : 0) : b.indexCount / 3;
    stats_.triangles += tris;
    prof::count(prof::kTriangles, tris);
}

// One unit, every shown batch of one pass.
void CreatureRenderer::drawUnit(const Visible& v, bool blendPass) {
    if (v.portraitOnly) return;
    if (v.anim) {  // Creature anim
        if (!v.skipped) drawSkinnedUnit(v, blendPass);
        return;
    }
    auto mit = models_.find(v.d->h.model);
    if (mit == models_.end() || !mit->second.blob) return;
    const Model& m = mit->second;
    const uint32_t first = v.lod ? m.h->batchCount[0] : 0;
    const uint32_t count = v.lod ? m.h->batchCount[1] : m.h->batchCount[0];
    bool matrixSet = false;
    for (uint32_t i = first; i < first + count; ++i) {
        const ModelBatch& b = m.batches[i];
        if (!v.d->active[i] || isBlended(b.blend) != blendPass) continue;
        if (v.hull && b.blend != kBlendOpaque) continue;  // Outline - solid batches only
        if (!matrixSet) {
            sceGuSetMatrix(GU_MODEL, &v.mtx);
            sceGuTexScale(m.h->uvScale[0], m.h->uvScale[1]);
            sceGuTexOffset(m.h->uvMin[0], m.h->uvMin[1]);
            matrixSet = true;
        }
        if (v.hull) pp::textureBind(nullptr);  // The outline colour, no texture
        else setBatchState(*v.d, b, v.d->active[i]);
        drawBatch(m, b);
    }
}

void CreatureRenderer::evict() {
    const uint32_t budget = memoryBudgetKb * 1024u;
    if (memoryBytes_ <= budget) return;
    struct Cand {
        uint32_t lastUsed;
        uint8_t kind;  // 0 model, 1 texture, 2 skinned model
        uint16_t id;
    };
    std::vector<Cand> c;
    for (const auto& [id, m] : models_)
        if (m.blob && m.lastUsed + kKeepFrames < frame_) c.push_back({m.lastUsed, 0, id});
    for (const auto& [id, t] : textures_)
        if (t.loaded && t.lastUsed + kKeepFrames < frame_) c.push_back({t.lastUsed, 1, id});
    for (const auto& [id, k] : skins_)  // Creature anim
        if (k.blob && k.lastUsed + kKeepFrames < frame_) c.push_back({k.lastUsed, 2, id});
    std::sort(c.begin(), c.end(), [](const Cand& a, const Cand& b) { return a.lastUsed < b.lastUsed; });
    for (const Cand& k : c) {
        if (memoryBytes_ <= budget) break;
        if (k.kind == 2) {  // Creature anim; states on it rebind when seen again
            auto it = skins_.find(k.id);
            pp::pakFree(it->second.blob);
            memoryBytes_ -= it->second.bytes;
            skins_.erase(it);
            seats_.erase(k.id);  // its points are read again with it
            for (auto& [g, a] : anims_)
                if (a.skin == static_cast<int32_t>(k.id)) {
                    a.skin = -1;
                    a.attach.reset();  // dressed again on the next bind
                    a.attachKey = 0;
                }
        } else if (k.kind == 0) {
            auto it = models_.find(k.id);
            pp::pakFree(it->second.blob);
            memoryBytes_ -= it->second.bytes;
            models_.erase(it);
            if (!skins_.count(k.id)) seats_.erase(k.id);
        } else {
            auto it = textures_.find(k.id);
            pp::textureRelease(it->second.t);
            memoryBytes_ -= it->second.bytes;
            textures_.erase(it);
        }
    }
}

void CreatureRenderer::draw(const TerrainCamera& cam, uint32_t fogColorABGR) {
    ++frame_;
    stats_.loads = 0;
    stats_.drawn = stats_.culled = stats_.waiting = stats_.lowLod = stats_.batches = stats_.triangles = 0;
    stats_.skinned = stats_.posed = stats_.boneUploads = stats_.moving = stats_.skipped = 0;  // Creature anim
    stats_.equipped = 0;
    skinDrawer_.boneUploads = 0;
    stats_.units = static_cast<uint32_t>(units_.size());
    frameTocReads0_ = index_.blockReads();  // this frame's TOC budget
    stats_.tocReads = 0;
    if (units_.empty() || !isOpen()) return;

    PROF_SCOPE("npc");
    PROF_GE_PASS("npc");
    budgetToc_ = true;
    const float aspect = static_cast<float>(pp::kScreenW) / pp::kScreenH;
    const glm::mat4 proj = glm::perspective(glm::radians(cam.fovyDeg), aspect, cam.nearZ, cam.farZ);
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), cam.forward, cam.up);
    const Frustum fr = makeFrustum(proj * view);
    const float reach = std::min(viewDistance, cam.farZ * 0.95f);

    static const uint16_t kCullZone = prof::zoneId("npc.cull");
    prof::zoneEnter(kCullZone);
    // Candidates by feet distance (nothing loaded is needed for that), nearest
    // first, so the load budget and the cap go to the closest units.
    // a unit's display is looked up only once it passes this coarse test (the TOC
    // budget per frame applies; a unit not looked up yet waits).
    visible_.clear();
    near_.clear();
    const float coarse = reach + 12.0f;
    for (Unit& un : units_) {
        const glm::vec3 c(un.u.x - cam.pos.x, un.u.y - cam.pos.y, un.u.z + 1.0f - cam.pos.z);
        const float d2 = glm::dot(c, c);
        // Coarse sphere before anything is loaded: the load budget goes to
        // units the camera can see (the exact test follows once the model is in).
        const bool keep = portraitUnit_ != 0 && un.u.guid == portraitUnit_;  // The portrait's unit
        if (!keep && (d2 > coarse * coarse || fr.sphereOutside(c, 8.0f * std::max(1.0f, un.u.scale)))) {
            ++stats_.culled;
            continue;
        }
        near_.emplace_back(d2, &un);
    }
    // Nearest first for the lookups too (a mount first: its rider is drawn regardless).
    std::sort(near_.begin(), near_.end(), [](const std::pair<float, Unit*>& a, const std::pair<float, Unit*>& b) {
        return a.second->u.mount != b.second->u.mount ? a.second->u.mount : a.first < b.first;
    });
    for (const auto& [d2, unp] : near_) {
        Unit& un = *unp;
        // twice maxDrawn candidates at most - the rest are not drawn this frame, so they
        // take no lookup and no slot of the display pool (a crowd would churn it). The portrait's unit and the
        // mounts always pass.
        const bool keep = (portraitUnit_ != 0 && un.u.guid == portraitUnit_) || un.u.mount;
        if (!keep && visible_.size() >= 2u * maxDrawn) {
            ++stats_.culled;
            continue;
        }
        if (un.display == -1) un.display = findDisplay(un.u.displayId, true);
        if (un.display < 0) {
            ++stats_.waiting;
            continue;
        }
        Display& dsp = displays_[static_cast<size_t>(un.display)];
        dsp.lastUsed = frame_;  // a candidate's slot is never given up in this frame
        visible_.push_back(Visible{&un, &dsp, std::sqrt(d2), 0, {}});
    }
    uint32_t budget = loadsPerFrame;
    size_t kept = 0;
    for (size_t i = 0; i < visible_.size(); ++i) {
        Visible v = visible_[i];
        const bool keep = portraitUnit_ != 0 && v.unit->u.guid == portraitUnit_;
        if (kept >= maxDrawn && !keep) {
            ++stats_.culled;
            continue;
        }
        if (!ensure(*v.d, budget)) {
            ++stats_.waiting;
            continue;
        }
        const CreatureUnit& u = v.unit->u;
        const float scale = unitScale(u, *v.d);  // Mounts (rawScale)
        const float cs = std::cos(u.orientation), sn = std::sin(u.orientation);
        const glm::vec3 ax(cs * scale, sn * scale, 0.0f), ay(-sn * scale, cs * scale, 0.0f), az(0.0f, 0.0f, scale);
        if (v.d->skinMode == 1) {  // Creature anim - GE skinning, own culling/matrix
            const Skin& sk = *skinOf(v.d->h.model);
            const SkinHeader& sh = *sk.view.h;
            const glm::vec3 feet(u.x, u.y, u.z);
            const glm::vec3 rel = feet + ax * sh.center[0] + ay * sh.center[1] + az * sh.center[2] - cam.pos;
            // Bind-pose sphere + 25 %: animated limbs reach beyond it.
            const float r = sh.radius * scale * 1.25f;
            const float dist = std::max(0.0f, glm::length(rel) - r);
            if (dist > reach || fr.sphereOutside(rel, r) || kept >= maxDrawn) {
                if (!keep) {
                    ++stats_.culled;
                    continue;
                }
                v.portraitOnly = true;  // Posed for the portrait, not drawn in the world
            }
            v.dist = v.portraitOnly ? 0.0f : dist;  // The portrait animates at full rate
            v.lod = ((sh.flags & kSkinHasLod) && sk.lod0Batches < sh.batchCount && dist > lodDistance) ? 1 : 0;
            // The bone matrices carry the dequantisation: rotation x scale
            // and the feet only (CharacterModel::draw).
            const glm::vec3 t = feet - cam.pos;
            v.mtx.x = {ax.x, ax.y, 0.0f, 0.0f};
            v.mtx.y = {ay.x, ay.y, 0.0f, 0.0f};
            v.mtx.z = {0.0f, 0.0f, az.z, 0.0f};
            v.mtx.w = {t.x, t.y, t.z, 1.0f};
            AnimUnit& a = anims_[u.guid];
            a.lastSeen = frame_;
            if (a.skin != static_cast<int32_t>(v.d->h.model)) bindAnim(a, *v.unit, sk);
            v.anim = &a;
            visible_[kept++] = v;
            continue;
        }
        const Model& m = models_[v.d->h.model];
        const ModelHeader& h = *m.h;
        const glm::vec3 center = glm::vec3(u.x, u.y, u.z) + ax * h.center[0] + ay * h.center[1] + az * h.center[2];
        const glm::vec3 rel = center - cam.pos;
        const float r = h.radius * scale;
        const float dist = std::max(0.0f, glm::length(rel) - r);
        if (dist > reach || fr.sphereOutside(rel, r) || kept >= maxDrawn) {
            if (!keep) {
                ++stats_.culled;
                continue;
            }
            v.portraitOnly = true;
        }
        v.dist = dist;
        v.lod = (h.batchCount[1] && dist > lodDistance) ? 1 : 0;
        v.mtx.x = {ax.x * h.half[0], ax.y * h.half[0], 0.0f, 0.0f};
        v.mtx.y = {ay.x * h.half[1], ay.y * h.half[1], 0.0f, 0.0f};
        v.mtx.z = {0.0f, 0.0f, az.z * h.half[2], 0.0f};
        v.mtx.w = {rel.x, rel.y, rel.z, 1.0f};
        visible_[kept++] = v;
    }
    visible_.resize(kept);
    budgetToc_ = false;
    stats_.tocReads = index_.blockReads() - frameTocReads0_;
    prof::zoneLeave(kCullZone);

    // Creature anim - drive and pose the skinned units kept.
    if (animate_) {
        PROF_SCOPE("npc.anim");
        posing_.clear();
        for (const Visible& v : visible_) {
            if (!v.anim) continue;
            AnimUnit& a = *v.anim;
            const Skin& sk = *skinOf(v.d->h.model);
            const bool driven = v.unit->u.driven;  // Mounts - the caller's animation
            if (!driven) driveAnim(a, *v.unit, sk);
            else if (a.drivenPending) {
                skinPlay(sk.view, a.s, a.drivenAnim, a.drivenLoop);
                a.drivenPending = false;
                a.dirty = true;
            }
            const float rate = (moveAnimRate && !driven) ? moveRate(a, *v.unit, sk) : 1.0f;  // walk/run
            if (skinUpdate(sk.view, a.s, frameDt_ * rate)) a.dirty = true;
            if (a.wasMoving) ++stats_.moving;
            // Upstream CharacterRenderer boneInterval: 1 near, 2 beyond 45 yd,
            // 4 beyond 90 (animDistance x 1 / x 2), staggered by guid.
            uint32_t interval = 1;
            if (animDistance > 0.0f && !driven)
                interval = v.dist > animDistance * 2.0f ? 4u : (v.dist > animDistance ? 2u : 1u);
            const uint32_t phase = static_cast<uint32_t>(v.unit->u.guid) & 3u;
            if (!a.poseValid || (a.dirty && (interval == 1 || ((frame_ + phase) % interval) == 0)))
                posing_.push_back(&v);
        }
        // PSP: at most maxPoses per frame (wowee.cfg npcposes, 0 = all), the
        // longest-waiting first; a unit never posed yet always is (it would
        // draw garbage). The others keep last frame's pose.
        if (maxPoses && posing_.size() > maxPoses) {
            // Mounts - a driven unit (the own mount) counts as never posed: always kept.
            auto must = [](const Visible* x) { return !x->anim->poseValid || x->unit->u.driven; };
            std::stable_sort(posing_.begin(), posing_.end(), [&](const Visible* x, const Visible* y) {
                const bool nx = must(x), ny = must(y);
                return nx != ny ? nx : x->anim->lastPose < y->anim->lastPose;
            });
            size_t keep = maxPoses;
            while (keep < posing_.size() && must(posing_[keep])) ++keep;
            posing_.resize(keep);
        }
        for (const Visible* v : posing_) {
            AnimUnit& a = *v->anim;
            const Skin& sk = *skinOf(v->d->h.model);
            skinPose(sk.view, a.s, a.scratch.data(), a.ge.data());
            a.poseValid = true;
            a.dirty = false;
            a.lastPose = frame_;
            ++stats_.posed;
        }
    }

    // Used this frame: never evicted before the GE is done with them.
    for (const Visible& v : visible_) {
        if (v.anim) {  // Creature anim
            skinOf(v.d->h.model)->lastUsed = frame_;
            ++stats_.skinned;
        } else {
            models_[v.d->h.model].lastUsed = frame_;
        }
        v.d->lastUsed = frame_;
        for (uint16_t id : v.d->textures) {
            auto it = textures_.find(id);
            if (it != textures_.end()) it->second.lastUsed = frame_;
        }
        if (v.lod) ++stats_.lowLod;
    }
    updateEquipment();  // dress / undress the nearest humanoids (at most one per frame)

    pp::textureResetBinding();
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDisable(GU_CULL_FACE);  // M2 batches are two-sided (upstream: cull none)
    sceGuShadeModel(GU_SMOOTH);
    lightBegin(cam, fogColorABGR);  // Lighting (fog from the bands, light tint)
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_ALPHA_TEST);

    // Opaque pass grouped by display and LOD: batch state once, then a
    // matrix and a draw per unit (ModelRenderer).
    order_.resize(visible_.size());
    for (size_t i = 0; i < order_.size(); ++i) order_[i] = static_cast<uint32_t>(i);
    std::sort(order_.begin(), order_.end(), [&](uint32_t a, uint32_t b) {
        const Visible& va = visible_[a];
        const Visible& vb = visible_[b];
        return va.d != vb.d ? va.d < vb.d : va.lod < vb.lod;
    });
    for (size_t g = 0; g < order_.size();) {
        const Visible& head = visible_[order_[g]];
        size_t end = g + 1;
        while (end < order_.size() && visible_[order_[end]].d == head.d && visible_[order_[end]].lod == head.lod) ++end;
        if (head.anim) {  // Creature anim - drawn below, nearest first
            g = end;
            continue;
        }
        const Model& m = models_[head.d->h.model];
        const uint32_t first = head.lod ? m.h->batchCount[0] : 0;
        const uint32_t count = head.lod ? m.h->batchCount[1] : m.h->batchCount[0];
        sceGuTexScale(m.h->uvScale[0], m.h->uvScale[1]);
        sceGuTexOffset(m.h->uvMin[0], m.h->uvMin[1]);
        for (uint32_t i = first; i < first + count; ++i) {
            const ModelBatch& b = m.batches[i];
            if (!head.d->active[i] || isBlended(b.blend)) continue;
            setBatchState(*head.d, b, head.d->active[i]);
            for (size_t k = g; k < end; ++k) {
                if (visible_[order_[k]].portraitOnly) continue;
                sceGuSetMatrix(GU_MODEL, &visible_[order_[k]].mtx);
                drawBatch(m, b);
            }
        }
        g = end;
    }
    // Creature anim - skinned units one by one (their own bone
    // matrices), nearest first. PSP: every sceGuBoneMatrix is 13 words in
    // the 256 KB display list (psp_display.cpp) - past maxBoneUploads or a
    // list fuller than kSkinListLimit the farther skinned units are skipped
    // this frame (counted in npc_anim_frame skipped=), never the list overrun.
    uint32_t portraitOnly = 0;
    bool listFull = false;
    for (Visible& v : visible_) {
        if (v.portraitOnly) ++portraitOnly;
        if (!v.anim || v.portraitOnly) continue;
        const bool listLow = pp::displayListFree() < pp::kListReserveNpc;  // the NPCs' reserve (was 192 KB used)
        listFull = listFull || listLow;
        if (skinDrawer_.boneUploads >= maxBoneUploads || listLow) {
            v.skipped = true;
            ++stats_.skipped;
            continue;
        }
        drawSkinnedUnit(v, false);
    }
    stats_.drawn = static_cast<uint32_t>(visible_.size()) - stats_.skipped - portraitOnly;
    drawEquipment(cam);  // helm / shoulders / weapon on the drawn humanoids

    // Blended batches: far to near, depth test on, depth writes off.
    sceGuDisable(GU_ALPHA_TEST);
    sceGuEnable(GU_BLEND);
    sceGuDepthMask(GU_TRUE);
    for (size_t i = visible_.size(); i-- > 0;) {
        if (pp::displayListFree() < pp::kListReserveNpc) {  // Never into the interface's reserve
            listFull = true;
            continue;
        }
        drawUnit(visible_[i], true);
    }
    sceGuDepthMask(GU_FALSE);
    sceGuDisable(GU_BLEND);
    if (listFull) pp::displayListFull("npc");

    lightEnd();  // Lighting
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    pp::textureResetBinding();

    evict();
    stats_.displays = stats_.models = stats_.textures = 0;
    for (const Display& d : displays_) stats_.displays += d.loaded ? 1 : 0;
    for (const auto& [id, m] : models_) stats_.models += m.blob ? 1 : 0;
    for (const auto& [id, t] : textures_) stats_.textures += t.loaded ? 1 : 0;
    stats_.memoryKb = memoryBytes_ / 1024u;
    // Creature anim - forget states of units not seen for ~10 s.
    stats_.boneUploads = skinDrawer_.boneUploads;
    stats_.skins = 0;
    for (const auto& [id, k] : skins_) stats_.skins += k.blob ? 1 : 0;
    if ((frame_ & 127u) == 0) {
        for (auto it = anims_.begin(); it != anims_.end();) {
            if (it->second.lastSeen + 600u < frame_) it = anims_.erase(it);
            else ++it;
        }
    }
    stats_.animStates = static_cast<uint32_t>(anims_.size());
    // One sample every 10 s for the hardware log (PPSSPP cannot give these).
    const uint32_t nowMs = pp::logUptimeMs();
    if (nowMs >= nextLogMs_) {
        nextLogMs_ = nowMs + 10000;
        PSP_LOGI(pp::tag::Gfx,
                 "npc_frame units=%u drawn=%u culled=%u waiting=%u lod1=%u batches=%u tris=%u loads=%u "
                 "res=%u/%u/%u kb=%u",
                 static_cast<unsigned>(stats_.units), static_cast<unsigned>(stats_.drawn),
                 static_cast<unsigned>(stats_.culled), static_cast<unsigned>(stats_.waiting),
                 static_cast<unsigned>(stats_.lowLod), static_cast<unsigned>(stats_.batches),
                 static_cast<unsigned>(stats_.triangles), static_cast<unsigned>(stats_.loads),
                 static_cast<unsigned>(stats_.displays), static_cast<unsigned>(stats_.models),
                 static_cast<unsigned>(stats_.textures), static_cast<unsigned>(stats_.memoryKb));
        // Creature anim (a line of its own: npc_frame is at the 120-char limit).
        PSP_LOGI(pp::tag::Gfx,
                 "npc_anim_frame on=%d skinned=%u posed=%u bone_uploads=%u skipped=%u moving=%u skins=%u "
                 "states=%u list_kb=%d heap_held=%u",
                 animate_ ? 1 : 0, static_cast<unsigned>(stats_.skinned), static_cast<unsigned>(stats_.posed),
                 static_cast<unsigned>(stats_.boneUploads), static_cast<unsigned>(stats_.skipped),
                 static_cast<unsigned>(stats_.moving), static_cast<unsigned>(stats_.skins),
                 static_cast<unsigned>(stats_.animStates), sceGuCheckList() / 1024,
                 static_cast<unsigned>(stats_.heapHeld));
        // the on-demand index (display pool, lookups, TOC reads) and the NPC equipment.
        PSP_LOGI(pp::tag::Gfx,
                 "npc_index pool=%u/%u known=%u lookups=%u toc_reads=%u evicted=%u index_kb=%u equip=%u "
                 "attach_kb=%u",
                 static_cast<unsigned>(displays_.size() - freeDisplays_.size()), static_cast<unsigned>(kDisplayPool),
                 static_cast<unsigned>(displayIdx_.size()), static_cast<unsigned>(lookups_),
                 static_cast<unsigned>(lookupMisses_), static_cast<unsigned>(displayEvictions_),
                 static_cast<unsigned>(indexRamBytes() / 1024u), static_cast<unsigned>(stats_.equipped),
                 static_cast<unsigned>(attachmentCacheBytes() / 1024u));
    }
}

// ---- Portraits ----------------------------------------------------
//
// The client renders the unit's model into the round portrait of the target
// frame and the NPC windows. PSP: once the world is drawn, the target is drawn
// a second time into a small VRAM target, looking at the front of the upper
// part of its bind-pose box (the head of a biped, the head and neck of a
// beast), no fog; the UI then draws that 64 x 64 texture under the frame art,
// whose round hole masks it. The shared depth buffer's top-left 64 x 64 is
// cleared and reused - the 3D pass is over by then.
bool CreatureRenderer::drawPortrait(uint64_t guid, uint32_t vramRel, const TerrainCamera& cam) {
    if (vramRel == 0) return false;
    const Visible* v = nullptr;
    for (const Visible& x : visible_)
        if (x.unit->u.guid == guid) {
            v = &x;
            break;
        }
    if (!v || v->skipped) return false;
    const CreatureUnit& u = v->unit->u;
    const float scale = unitScale(u, *v->d);  // Mounts (rawScale)
    glm::vec3 c, half;
    if (v->anim) {
        const SkinHeader& sh = *skinOf(v->d->h.model)->view.h;
        c = glm::vec3(sh.center[0], sh.center[1], sh.center[2]);
        half = glm::vec3(sh.half[0], sh.half[1], sh.half[2]);
    } else {
        auto mit = models_.find(v->d->h.model);
        if (mit == models_.end() || !mit->second.blob) return false;
        const ModelHeader& h = *mit->second.h;
        c = glm::vec3(h.center[0], h.center[1], h.center[2]);
        half = glm::vec3(h.half[0], h.half[1], h.half[2]);
    }
    // Model space: +x forward, +z up. The head: the front-upper part.
    const float cs = std::cos(u.orientation), sn = std::sin(u.orientation);
    auto toWorld = [&](const glm::vec3& m) {
        return glm::vec3(u.x + (cs * m.x - sn * m.y) * scale, u.y + (sn * m.x + cs * m.y) * scale, u.z + m.z * scale);
    };
    const bool tall = half.z >= half.x;  // a biped / an upright model; else a beast
    const glm::vec3 headM(c.x + (tall ? 0.0f : half.x * 0.55f), c.y, c.z + half.z * (tall ? 0.72f : 0.45f));
    const glm::vec3 head = toWorld(headM);
    const float size = std::max(0.25f, (tall ? half.z * 0.42f : std::max(half.z, half.x) * 0.55f) * scale);
    const glm::vec3 fwd(cs, sn, 0.0f);
    const glm::vec3 eye = head + fwd * (size * 2.6f) + glm::vec3(0.0f, 0.0f, size * 0.25f);

    // Target, viewport, clear.
    sceGuDrawBufferList(GU_PSM_5650, reinterpret_cast<void*>(static_cast<uintptr_t>(vramRel)), 64);
    sceGuOffset(2048 - 32, 2048 - 32);
    sceGuViewport(2048, 2048, 64, 64);
    sceGuScissor(0, 0, 64, 64);
    sceGuClearColor(0xFF101010u);
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadIdentity();
    sceGumPerspective(40.0f, 1.0f, 0.05f, 60.0f);
    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();
    ScePspFVector3 e = {eye.x - cam.pos.x, eye.y - cam.pos.y, eye.z - cam.pos.z};
    ScePspFVector3 t = {head.x - cam.pos.x, head.y - cam.pos.y, head.z - cam.pos.z};
    ScePspFVector3 up = {0.0f, 0.0f, 1.0f};
    sceGumLookAt(&e, &t, &up);
    sceGumUpdateMatrix();

    pp::textureResetBinding();
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuShadeModel(GU_SMOOTH);
    lightBegin(cam, 0xFF101010u);
    sceGuDisable(GU_FOG);
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_ALPHA_TEST);
    if (v->anim) {
        drawSkinnedUnit(*const_cast<Visible*>(v), false);
    } else {
        drawUnit(*v, false);
    }
    sceGuEnable(GU_BLEND);
    sceGuDepthMask(GU_TRUE);
    drawUnit(*v, true);
    sceGuDepthMask(GU_FALSE);
    sceGuDisable(GU_BLEND);
    lightEnd();
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    pp::textureResetBinding();

    // Back to the frame.
    sceGuDrawBufferList(GU_PSM_5551, pp::displayDrawBuffer(), 512);
    sceGuOffset(2048 - pp::kScreenW / 2, 2048 - pp::kScreenH / 2);
    sceGuViewport(2048, 2048, pp::kScreenW, pp::kScreenH);
    sceGuScissor(0, 0, pp::kScreenW, pp::kScreenH);
    sceGuTexFlush();
    sceGuTexSync();  // texture reads wait for this render target's drawing (GE Users Manual §13.8)
    return true;
}

bool CreatureRenderer::drawOutline(uint64_t guid, uint32_t colorABGR, const TerrainCamera& cam,
                                   uint32_t fogColorABGR) {
    const Visible* src = nullptr;
    for (const Visible& x : visible_)
        if (x.unit->u.guid == guid) {
            src = &x;
            break;
        }
    if (!src || src->skipped || src->portraitOnly) return false;  // Not drawn in the world
    // The hull: the model x k around its bind-pose centre (the rim ~ 4 % of
    // its size, at least a few centimetres for a small creature).
    const CreatureUnit& u = src->unit->u;
    const float scale = unitScale(u, *src->d);  // Mounts (rawScale)
    glm::vec3 c;
    float radius;
    if (src->anim) {
        const SkinHeader& sh = *skinOf(src->d->h.model)->view.h;
        c = glm::vec3(sh.center[0], sh.center[1], sh.center[2]);
        radius = sh.radius;
    } else {
        auto mit = models_.find(src->d->h.model);
        if (mit == models_.end() || !mit->second.blob) return false;
        c = glm::vec3(0.0f);  // its matrix is centred already (translation = the box centre)
        radius = mit->second.h->radius;
    }
    const float k = 1.0f + std::max(0.035f, 0.05f / std::max(0.2f, radius * scale));
    Visible hull = *src;
    hull.hull = true;
    ScePspFMatrix4& m = hull.mtx;
    const glm::vec3 ax(m.x.x, m.x.y, m.x.z), ay(m.y.x, m.y.y, m.y.z), az(m.z.x, m.z.y, m.z.z);
    const glm::vec3 ac = ax * c.x + ay * c.y + az * c.z;  // the centre's offset from the matrix origin
    m.x.x *= k; m.x.y *= k; m.x.z *= k;
    m.y.x *= k; m.y.y *= k; m.y.z *= k;
    m.z.x *= k; m.z.y *= k; m.z.z *= k;
    m.w.x += ac.x * (1.0f - k);
    m.w.y += ac.y * (1.0f - k);
    m.w.z += ac.z * (1.0f - k);

    // 1: the hull in one colour (GE lighting with no lights: the ambient
    // material is the colour), depth tested, not written, blended.
    pp::textureResetBinding();
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_FOG);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuEnable(GU_LIGHTING);
    for (int l = 0; l < 4; ++l) sceGuDisable(GU_LIGHT0 + l);
    sceGuColorMaterial(0);
    sceGuAmbient(0xFFFFFFFFu);
    sceGuAmbientColor(colorABGR);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuDepthMask(GU_TRUE);
    if (hull.anim) drawSkinnedUnit(hull, false);
    else drawUnit(hull, false);
    sceGuDepthMask(GU_FALSE);
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_LIGHTING);
    sceGuColorMaterial(GU_AMBIENT | GU_DIFFUSE | GU_SPECULAR);

    // 2: the model again, exactly as drawn (same depth: GEQUAL passes).
    pp::textureResetBinding();
    lightBegin(cam, fogColorABGR);
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_ALPHA_TEST);
    if (src->anim) drawSkinnedUnit(*src, false);
    else drawUnit(*src, false);
    sceGuEnable(GU_BLEND);
    sceGuDepthMask(GU_TRUE);
    drawUnit(*src, true);
    sceGuDepthMask(GU_FALSE);
    sceGuDisable(GU_BLEND);
    lightEnd();
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    pp::textureResetBinding();
    return true;
}

// ---- Creature anim ----------------------------------------------
// Skinned creature models ("n_<model>", WSKN) and the per-unit animation
// state. Selection logic: upstream Application::syncRenderInstancesToEntities
// (creature loop) + AnimationCallbackHandler / EntitySpawnCallbackHandler
// (death, respawn, swing, hit reaction, state emotes); CharacterRenderer
// semantics via psp_skinned.

void CreatureRenderer::update(float dtSec) {
    // A hitch (loading screen) must not skip half an animation; NaN -> 0.
    frameDt_ = dtSec > 0.0f ? std::min(dtSec, 0.25f) : 0.0f;
}

void CreatureRenderer::setAnimate(bool on) {
    if (on == animate_) return;
    animate_ = on;
    for (Display& d : displays_) {
        d.skinMode = 0;  // decided again at the next ensure()
        d.texturesKnown = false;
        d.textures.clear();
    }
}

bool CreatureRenderer::useSkin(const Display& d) const {
    if (!animate_) return false;
    const Skin* sk = skinOf(d.h.model);
    return !sk || !sk->failed;
}

CreatureRenderer::Skin* CreatureRenderer::skinOf(int32_t model) {
    if (model < 0) return nullptr;
    auto it = skins_.find(static_cast<uint16_t>(model));
    return it == skins_.end() ? nullptr : &it->second;
}

const CreatureRenderer::Skin* CreatureRenderer::skinOf(int32_t model) const {
    if (model < 0) return nullptr;
    auto it = skins_.find(static_cast<uint16_t>(model));
    return it == skins_.end() ? nullptr : &it->second;
}

// "n_<id>" looked up and read; a model without one (or a bad one) is remembered as
// failed (drawn static from "m_").
bool CreatureRenderer::loadSkin(uint16_t id) {
    PROF_SCOPE("npc.load");
    pp::PakEntry e{};
    const bool found = findEntry(kEntrySkin, id, e);
    if (found && !roomFor(e.size)) return false;  // Waits, not failed (ensure() holds the unit)
    Skin& k = skins_[id];
    void* blob = found ? loadEntry(e) : nullptr;
    if (found) ++stats_.loads;
    SkinView view;
    if (!blob || !view.init(blob, e.size) || view.h->sequenceCount == 0) {
        if (blob) pp::pakFree(blob);
        if (found)
            PSP_LOGE(pp::tag::Gfx, "npc_skin_bad model=%u size=%u", static_cast<unsigned>(id),
                     static_cast<unsigned>(e.size));
        k = Skin{};
        k.failed = true;
        return false;
    }
    k.blob = blob;
    k.view = view;
    k.lod0Batches = view.h->batchCount;
    if ((view.h->flags & kSkinHasLod) && view.h->reserved[0] > 0 && view.h->reserved[0] <= view.h->batchCount)
        k.lod0Batches = view.h->reserved[0];
    k.bytes = e.size;
    k.lastUsed = frame_;
    k.failed = false;
    memoryBytes_ += k.bytes;
    // Lazy loads happen in the frame: a bounded count of lines.
    if (++skinLogs_ <= 40)
        PSP_LOGI(pp::tag::Gfx, "npc_anim_load model=%u bones=%u seqs=%u subbatches=%u lod0=%u/%u kb=%u",
                 static_cast<unsigned>(id), static_cast<unsigned>(view.h->boneCount),
                 static_cast<unsigned>(view.h->sequenceCount), static_cast<unsigned>(view.h->subBatchCount),
                 static_cast<unsigned>(k.lod0Batches), static_cast<unsigned>(view.h->batchCount),
                 static_cast<unsigned>(k.bytes / 1024u));
    return true;
}

CreatureRenderer::AnimUnit* CreatureRenderer::animFor(uint64_t guid) {
    auto it = anims_.find(guid);
    return it == anims_.end() ? nullptr : &it->second;
}

const CreatureRenderer::AnimUnit* CreatureRenderer::animFor(uint64_t guid) const {
    auto it = anims_.find(guid);
    return it == anims_.end() ? nullptr : &it->second;
}

namespace {

// PSP: the original client holds an NPC in its ready stance while in combat;
// upstream's creature loop has no combat idle (used only when task B asks).
uint32_t readyAnim(const SkinView& view) {
    for (uint32_t id : {anim::READY_1H, anim::READY_2H, anim::READY_2H_LOOSE, anim::READY_UNARMED})
        if (skinHas(view, id)) return id;
    return anim::STAND;
}

// Upstream creature sync: Walk (4) with the WALKING flag, else Run (5);
// Swim (42) / FlyForward (135) in water / air. PSP: npc-walk - a model without
// that sequence takes the ground gait (FlyForward / Swim -> Walk or Run, Walk <->
// Run) instead of the blob's first sequence (skinPlay's fallback); a levitating
// ground model on a Flying spline no longer slides in its Stand.
uint32_t moveAnim(const CreatureUnit& u, const SkinView& view) {
    if (u.flying && skinHas(view, anim::FLY_FORWARD)) return anim::FLY_FORWARD;
    if (u.swimming && skinHas(view, anim::SWIM)) return anim::SWIM;
    if (u.walking) return skinHas(view, anim::WALK) || !skinHas(view, anim::RUN) ? anim::WALK : anim::RUN;
    return skinHas(view, anim::RUN) || !skinHas(view, anim::WALK) ? anim::RUN : anim::WALK;
}

} // namespace

// Idle loop of a unit that does not move (upstream creature sync: FlyIdle /
// SwimIdle / the retained state emote when the model has it / Stand).
uint32_t CreatureRenderer::idleAnim(const AnimUnit& a, const Unit& un, const Skin& sk) const {
    const CreatureUnit& u = un.u;
    if (u.flying && skinHas(sk.view, anim::FLY_IDLE)) return anim::FLY_IDLE;      // PSP: npc-walk - else Stand
    if (u.swimming && skinHas(sk.view, anim::SWIM_IDLE)) return anim::SWIM_IDLE;  // PSP: npc-walk - else Stand
    if (a.emote && skinHas(sk.view, a.emote)) return a.emote;
    if (a.ready) return readyAnim(sk.view);
    return anim::STAND;
}

// First sight of a unit with this model (or the model changed): its state
// joins the blob. Upstream EntitySpawner::playCreatureSpawnPose: dead ->
// Death, a state emote the model has -> loop it, else Stand (Birth/Spawn are
// not baked: the PSP binds when a unit first shows on screen, not at spawn).
void CreatureRenderer::bindAnim(AnimUnit& a, const Unit& un, const Skin& sk) {
    const CreatureUnit& u = un.u;
    const uint32_t bones = sk.view.h->boneCount;
    a.skin = -1;
    for (const auto& [id, s] : skins_)
        if (&s == &sk) a.skin = id;
    a.scratch.resize(bones);
    a.ge.resize(bones);
    a.attach.reset();  // dressed again for this model
    a.attachKey = 0;
    a.s = SkinAnimState{};
    a.s.rng ^= static_cast<uint32_t>(u.guid * 2654435761u);  // fidgets out of step
    a.dead = u.dead || a.deadEvent;
    if (u.emoteAnim) a.emote = u.emoteAnim;
    a.fieldEmote = u.emoteAnim;
    a.wasMoving = !a.dead && u.moving;
    a.wasWalking = u.walking;
    a.wasSwimming = u.swimming;
    a.wasFlying = u.flying;
    a.lastX = u.x;
    a.lastY = u.y;
    a.lastZ = u.z;
    if (u.driven) {  // Mounts - the caller's request (Stand until there is one)
        a.loopAnim = a.drivenPending ? a.drivenAnim : anim::STAND;
        skinPlay(sk.view, a.s, a.loopAnim, a.drivenPending ? a.drivenLoop : true);
        a.drivenPending = false;
        a.poseValid = false;
        a.dirty = true;
        return;
    }
    if (a.dead) {
        // PSP: a corpse that comes into view lies dead (the original client);
        // upstream replays the whole Death fall on spawn.
        skinPlay(sk.view, a.s, anim::DEATH, false);
        skinHoldEnd(sk.view, a.s);
        a.loopAnim = anim::DEATH;
    } else {
        a.loopAnim = a.wasMoving ? moveAnim(u, sk.view) : idleAnim(a, un, sk);
        skinPlay(sk.view, a.s, a.loopAnim, true);
        // PSP: a crowd of NPCs would breathe in step; start somewhere in the loop.
        const uint32_t dur = sk.view.sequences[a.s.sequence].durationMs;
        if (dur > 0) a.s.timeMs = static_cast<float>((a.s.rng >> 8) % dur);
    }
    a.poseValid = false;
    a.dirty = true;
}

// Per frame, per drawn unit: upstream's creature sync loop - the animation
// changes only on a locomotion state transition (moving / swimming / flying,
// walking while moving), never over Death; death and respawn from the unit
// flag (upstream npcDeath/RespawnCallback); a changed UNIT_NPC_EMOTESTATE
// like upstream's emote callback (state branch).
void CreatureRenderer::driveAnim(AnimUnit& a, const Unit& un, const Skin& sk) {
    const CreatureUnit& u = un.u;
    const SkinView& view = sk.view;
    // Death / respawn.
    if (u.dead && a.deadEvent) a.deadEvent = false;  // the fields agree now
    const bool deadNow = u.dead || a.deadEvent;
    if (deadNow != a.dead) {
        a.dead = deadNow;
        if (deadNow) {
            skinPlay(view, a.s, anim::DEATH, false);
            a.loopAnim = anim::DEATH;
        } else {
            // Upstream respawn: RISE one-shot when the model has it, else Stand.
            a.wasMoving = false;
            a.loopAnim = idleAnim(a, un, sk);
            if (skinHas(view, anim::RISE)) skinPlay(view, a.s, anim::RISE, false, a.loopAnim);
            else skinPlay(view, a.s, a.loopAnim, true);
        }
        a.dirty = true;
    }
    // Position-only locomotion (upstream positionOnlyLocomotion): the unit
    // moved without a spline (a MOVEMENT update). PSP: a jump over 6 yd
    // (upstream's largeCorrection) is a teleport, not a step.
    const float dx = u.x - a.lastX, dy = u.y - a.lastY, dz = std::fabs(u.z - a.lastZ);
    const float planar2 = dx * dx + dy * dy;
    a.lastX = u.x;
    a.lastY = u.y;
    a.lastZ = u.z;
    const bool positionOnly =
        !u.moving && (planar2 > 0.03f * 0.03f || dz > 0.08f) && planar2 <= 36.0f && dz <= 3.0f;
    if (a.dead) return;  // upstream: nothing overrides Death

    // A new UNIT_NPC_EMOTESTATE (upstream emote callback, state branch):
    // kept across movement; played now when the unit stands.
    if (u.emoteAnim != a.fieldEmote) {
        a.fieldEmote = u.emoteAnim;
        a.emote = u.emoteAnim;
        if (!a.wasMoving) {
            a.loopAnim = idleAnim(a, un, sk);
            skinPlay(view, a.s, a.loopAnim, true);
            a.dirty = true;
        }
    }

    const bool isMovingNow = u.moving || positionOnly;
    const bool stateChanged = isMovingNow != a.wasMoving || u.swimming != a.wasSwimming ||
                              u.flying != a.wasFlying || (u.walking != a.wasWalking && isMovingNow);
    if (!stateChanged) return;
    a.wasMoving = isMovingNow;
    a.wasSwimming = u.swimming;
    a.wasFlying = u.flying;
    a.wasWalking = u.walking;
    a.loopAnim = isMovingNow ? moveAnim(u, view) : idleAnim(a, un, sk);
    // Upstream plays the loop at once, cutting a one-shot short (an attack
    // while chasing); the PSP fades into it (M2 blendTime).
    skinPlay(view, a.s, a.loopAnim, true);
    a.dirty = true;
}

void CreatureRenderer::drawSkinnedUnit(const Visible& v, bool blendPass) {
    const Skin* skp = skinOf(v.d->h.model);
    if (!skp || !skp->blob) return;
    const Skin& sk = *skp;
    const SkinView& view = sk.view;
    const uint32_t nb = view.h->batchCount;
    const bool hasLod = sk.lod0Batches < nb;
    const uint32_t first = (v.lod && hasLod) ? sk.lod0Batches : 0;
    const uint32_t end = (!v.lod && hasLod) ? sk.lod0Batches : nb;
    bool set = false;
    // a helm on the head that hides the hair (AnimUnit::helmHidesHair) - the scalp variant out
    // (group 0, ids 2..99), the bald cap (1) in, as applyEquipmentGeosets does for a player.
    const bool noHair = v.anim && v.anim->attach && v.anim->helmHidesHair && view.geosets && items_ && equipOn &&
                        attachmentsEnabled();
    for (uint32_t i = first; i < end; ++i) {
        const ModelBatch& b = view.batches[i];
        uint8_t active = v.d->active[i];
        if (noHair) {
            const uint16_t g = view.geosets[i];
            if (g > 1 && g < 100) active = 0;
            else if (g == 1 && !active) active = 1;
        }
        if (!active || isBlended(b.blend) != blendPass) continue;
        if (v.hull && b.blend != kBlendOpaque) continue;  // Outline - solid batches only
        if (!set) {
            sceGuSetMatrix(GU_MODEL, &v.mtx);
            sceGuTexScale(view.h->uvScale[0], view.h->uvScale[1]);
            sceGuTexOffset(view.h->uvMin[0], view.h->uvMin[1]);
            skinDrawer_.begin();
            set = true;
        }
        if (v.hull) pp::textureBind(nullptr);  // The outline colour, no texture
        else setBatchState(*v.d, b, active);  // the bald cap's own state (was the baked mask)
        const uint32_t tris = skinDrawer_.drawBatch(view, b, v.anim->ge.data());
        ++stats_.batches;
        stats_.triangles += tris;
    }
}

// ---- NPC equipment ------------------------------------
// The original client draws a humanoid NPC's helm and shoulders as item models (and its weapon from
// UNIT_VIRTUAL_ITEM_SLOT_ID); upstream's world path attaches only the main-hand weapon (EntitySpawner::
// tryAttachCreatureVirtualWeapons, at 1) - its portrait dresses helm and shoulders (getHumanoidEquipment).
// PSP: the players' path - equipmentAttachments(Online rules: helm 11, shoulders 5/6, main hand 1) and
// CharacterAttachments (the shared, reference-counted item cache of items.pak, attachmax) - for the nearest
// maxEquipped skinned humanoids within equipDistance, at most one dressed per frame (a few card reads).

void CreatureRenderer::updateEquipment() {
    if (!items_ || !items_->isOpen() || !equipOn) {
        for (auto& [g, a] : anims_)
            if (a.attach) {
                a.attach.reset();
                a.attachKey = 0;
            }
        return;
    }
    PROF_SCOPE("npc.equip");
    uint32_t dressed = 0;
    bool loadedOne = false;
    for (const Visible& v : visible_) {  // nearest first (the cull order)
        if (!v.anim || v.portraitOnly) continue;
        AnimUnit& a = *v.anim;
        const Display& d = *v.d;
        const bool humanoid = (d.h.flags & kCreatureHumanoid) != 0;
        const uint32_t helm = d.hasEquip ? d.equip.item[0] : 0u, shoulder = d.hasEquip ? d.equip.item[1] : 0u;
        const uint32_t weapon = humanoid ? v.unit->u.mainHand : 0u;  // upstream: humanoid displays only
        // the off hand and the ranged weapon too (the original client shows them; upstream clears the
        // off hand), each hung by its InventoryType and the sheath state - the players' rules (AttachRules::Local).
        const CreatureUnit& cu = v.unit->u;
        const uint32_t offHand = humanoid ? cu.offHand : 0u, ranged = humanoid ? cu.ranged : 0u;
        const bool wants = humanoid && (helm || shoulder || weapon || offHand || ranged);
        const bool inRange = v.dist <= equipDistance && dressed < maxEquipped;
        const bool keepRange = v.dist <= equipDistance + 5.0f;  // hysteresis: no flicker at the edge
        if (!wants || (!inRange && !(a.attach && keepRange))) {
            if (a.attach) {
                a.attach.reset();
                a.attachKey = 0;
            }
            continue;
        }
        ++dressed;
        // the key covers every virtual item, their types and the sheath state.
        uint64_t key = (static_cast<uint64_t>(d.id) << 32) | weapon;
        key ^= (static_cast<uint64_t>(offHand) * 0x9E3779B97F4A7C15ull) ^ (static_cast<uint64_t>(ranged) << 21) ^
               (static_cast<uint64_t>(cu.mainHandInv) << 56) ^ (static_cast<uint64_t>(cu.offHandInv) << 48) ^
               (static_cast<uint64_t>(cu.rangedInv) << 40) ^ (static_cast<uint64_t>(cu.sheathState & 3u) << 62);
        // attachcap left models off - tried again every ~2 s while there is room.
        // while the crowd queue runs, when it gives this NPC its turn (attachRetry).
        const bool queued = frame_ - crowdQueueFrame_ <= 120u && crowdQueueFrame_ != 0;
        const bool capRetry = a.attach && a.attachKey == key && attachCapSkipped(*a.attach) &&
                              (queued ? a.retryNow
                                      : frame_ - a.capRetryFrame > 120u && attachmentInUseBytes() < attachmentCapBytes());
        if (a.attach && a.attachKey == key && !capRetry) continue;
        if (loadedOne) continue;  // one dressing per frame
        const Seats* points = seatsOf(d.h.model);
        if (!points || points->points.empty()) {
            a.attachKey = key;  // no points baked (an older pak): nothing to hang them on
            continue;
        }
        Equipment eq;
        eq.displayId[0] = helm;
        eq.invType[0] = 1;       // INVTYPE_HEAD
        eq.displayId[2] = shoulder;
        eq.invType[2] = 3;       // INVTYPE_SHOULDER
        eq.displayId[15] = weapon;
        eq.invType[15] = cu.mainHandInv;  // Item.dbc's (was 13 always: the online rules' hand)
        eq.displayId[16] = offHand;       // a weapon, a shield, a held-in-off-hand item
        eq.invType[16] = cu.offHandInv;
        eq.displayId[17] = ranged;        // in the right hand with the main hand empty
        eq.invType[17] = cu.rangedInv;
        // the players' rules (AttachRules::Local: shield on 0, off hand on 2, the sheath points by
        // InventoryType with SHEATH_STATE_UNARMED) - the original client hangs an NPC's virtual items the way it hangs a
        // player's; upstream's NPC path: the main hand on 1, always held.
        // With sheathclient (default) the placement by the sheath
        // state - Item.dbc sheath types, the ranged state, and no off hand under a creature's two-hand item
        // (SheathAttachOptions::creature).
        auto picks = CharacterModel::sheathClient
                         ? equipmentAttachmentsBySheath(eq, d.equip.race, d.equip.sex, cu.sheathState, *items_,
                                                        SheathAttachOptions{false, true})
                         : equipmentAttachments(eq, d.equip.race, d.equip.sex, AttachRules::Local, cu.sheathed,
                                                *items_);
        // the helm of an NPC race (goblin, vrykul, tuskarr, ...): the client loads
        // <LeftModel>_<ChrRaces.ClientPrefix><M|F>.m2; items.pak's columns hold the 10 playable races and the base
        // name, the base file missing - the "hr_<display>" variant (ItemBank::npcRaceHelm), on the helm point 11.
        if (helm && itemVariant(d.equip.race, d.equip.sex) == kItemVariants - 1) {
            bool helmThere = false;
            for (const AttachPick& p : picks) helmThere = helmThere || p.slot == 0;
            uint16_t raceModel = kNoItemModel, raceTexture = kNoItemLayer;
            if (!helmThere && items_->npcRaceHelm(helm, d.equip.race, d.equip.sex, raceModel, raceTexture)) {
                AttachPick p;
                p.point = 11;  // core::kAttachHelm
                p.slot = 0;
                p.displayId = helm;
                p.model = raceModel;
                p.texture = raceTexture;
                picks.push_back(p);
            }
        }
        if (!a.attach) a.attach = std::make_shared<CharacterAttachments>();
        a.attach->set(picks, points->points, *items_, false);  // within attachcap (not essential)
        a.attachKey = key;
        a.capRetryFrame = frame_;
        a.retryNow = false;
        // the helm is on the head (nothing left off) and hides the hair - the draw swaps the scalp.
        bool helmPicked = false;
        for (const AttachPick& p : picks) helmPicked = helmPicked || p.slot == 0;
        const ItemDisplay* helmRec = helm ? items_->get(helm) : nullptr;
        bool hidesHair = helmRec && helmRec->hidesHair[d.equip.sex ? 1 : 0];
        // With items.pak's b_rules, the helm's HelmetGeosetVisData hair mask by the NPC's
        // race bit (the helmet_visibility rule, as psp_body_rules.hpp helmetGeosets for players) - hidesHair is "any
        // race".
        if (helmRec && items_->hasBodyRules()) {
            if (const ItemBodyInfo* b = items_->body(helm)) {
                const HelmetVisRow* row = items_->helmetVis(b->helmetVis[d.equip.sex ? 1 : 0]);
                hidesHair = row && d.equip.race < 32 && (row->hide[0] & (1u << d.equip.race)) != 0;
            }
        }
        a.helmHidesHair = helmPicked && !attachCapSkipped(*a.attach) && hidesHair;
        loadedOne = true;
        if (++equipLogs_ <= 40)
            PSP_LOGI(pp::tag::Gfx, "npc_equip display=%u helm=%u shoulder=%u weapon=%u off=%u ranged=%u sheathed=%d "
                                   "picks=%u attached=%u capped=%u no_hair=%d kb=%u",
                     static_cast<unsigned>(d.id), static_cast<unsigned>(helm), static_cast<unsigned>(shoulder),
                     static_cast<unsigned>(weapon), static_cast<unsigned>(offHand), static_cast<unsigned>(ranged),
                     cu.sheathed ? 1 : 0, static_cast<unsigned>(picks.size()), static_cast<unsigned>(a.attach->count()),
                     static_cast<unsigned>(attachCapSkipped(*a.attach)), a.helmHidesHair ? 1 : 0,
                     static_cast<unsigned>(attachmentCacheBytes() / 1024u));
    }
}

// ---- attachcap's crowd queue (psp_creatures.hpp) ----------------------------------
void CreatureRenderer::attachHolders(float x, float y, float z, std::vector<AttachHolder>& out) {
    out.clear();
    crowdQueueFrame_ = frame_ ? frame_ : 1u;
    for (const auto& [guid, a] : anims_) {
        if (!a.attach) continue;
        const uint32_t count = static_cast<uint32_t>(a.attach->count()), capped = attachCapSkipped(*a.attach);
        if (!count && !capped) continue;
        const Unit* un = unitOf(guid);
        if (!un) continue;
        const float dx = un->u.x - x, dy = un->u.y - y, dz = un->u.z - z;
        out.push_back(AttachHolder{guid, std::sqrt(dx * dx + dy * dy + dz * dz), count, capped});
    }
}

bool CreatureRenderer::attachYield(uint64_t guid) {
    auto it = anims_.find(guid);
    if (it == anims_.end() || !it->second.attach || !it->second.attach->count() || !items_) return false;
    AnimUnit& a = it->second;
    const Unit* un = unitOf(guid);
    if (!un || un->display < 0 || static_cast<size_t>(un->display) >= displays_.size()) return false;
    const Seats* points = seatsOf(displays_[static_cast<size_t>(un->display)].h.model);
    if (!points) return false;
    a.attach->giveWay(points->points, *items_);
    a.helmHidesHair = false;  // the helm went with the rest: the hair again
    a.retryNow = false;
    return true;
}

bool CreatureRenderer::attachRetry(uint64_t guid) {
    auto it = anims_.find(guid);
    if (it == anims_.end() || !it->second.attach) return false;
    it->second.retryNow = true;
    return true;
}
// ---- (end of the crowd queue) ----

void CreatureRenderer::drawEquipment(const TerrainCamera& cam) {
    if (!items_ || !equipOn || !attachmentsEnabled()) return;
    for (const Visible& v : visible_) {
        if (!v.anim || v.portraitOnly || v.skipped || !v.anim->attach || !v.anim->attach->count()) continue;
        if (pp::displayListFree() < pp::kListReserveNpc) {
            pp::displayListFull("npc");
            break;
        }
        const AnimUnit& a = *v.anim;
        if (!a.poseValid) continue;
        // The unit's matrix without the vertex dequantisation (rotation x scale, feet relative to the
        // camera): CharacterModel's for its own attachments.
        BoneMatrix cm;
        const float mm[12] = {v.mtx.x.x, v.mtx.x.y, 0.0f, v.mtx.y.x, v.mtx.y.y, 0.0f,
                              0.0f,      0.0f,      v.mtx.z.z, v.mtx.w.x, v.mtx.w.y, v.mtx.w.z};
        std::memcpy(cm.m, mm, sizeof(mm));
        a.attach->draw(cm, a.scratch.data(), static_cast<uint32_t>(a.scratch.size()), 1.0f);
        ++stats_.equipped;
    }
    (void)cam;
    // CharacterAttachments leaves blending off, depth writes on; the creature pass state again.
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_BLEND);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
}

// ---- API for combat / events (task B) ----

bool CreatureRenderer::hasAnimation(uint64_t guid, uint32_t animId) const {
    const AnimUnit* a = animFor(guid);
    if (!a) return false;
    const Skin* sk = skinOf(a->skin);
    return sk && sk->blob && skinHas(sk->view, animId);
}

bool CreatureRenderer::playOneShot(uint64_t guid, uint32_t animId) {
    AnimUnit* a = animFor(guid);
    if (!a || a->dead || !hasAnimation(guid, animId)) return false;
    skinPlay(skinOf(a->skin)->view, a->s, animId, false, a->loopAnim);
    a->dirty = true;
    return true;
}

bool CreatureRenderer::playAttack(uint64_t guid) {
    // Upstream npcSwingCallback: meleeAnimChain(Generic), else ATTACK_UNARMED.
    // PSP: a model with none of them keeps its loop (upstream would fall
    // back to the model's first sequence).
    for (uint32_t id : anim::meleeAnimChain(anim::MeleeChain::Generic))
        if (playOneShot(guid, id)) return true;
    return playOneShot(guid, anim::ATTACK_UNARMED);
}

bool CreatureRenderer::playWound(uint64_t guid, uint32_t animId) {
    return playOneShot(guid, animId);  // upstream: only when the model has it
}

void CreatureRenderer::setDead(uint64_t guid, bool dead) {
    AnimUnit& a = anims_[guid];
    if (!a.lastSeen) a.lastSeen = frame_;  // pruned like any unseen state
    // Respawn before the fields: driveAnim stands the unit up once its flag
    // is clear too (it stays dead while the fields say so).
    a.deadEvent = dead;
}

void CreatureRenderer::playEmote(uint64_t guid, uint32_t animId, bool state) {
    AnimUnit* a = animFor(guid);
    if (!a) return;
    if (!state) {
        if (animId) playOneShot(guid, animId);
        return;
    }
    a->emote = animId;
    const Skin* sk = skinOf(a->skin);
    if (!sk || a->dead || a->wasMoving || !sk->blob) return;
    const SkinView& view = sk->view;
    // A standing unit (swimming/flying idles keep theirs until they change).
    a->loopAnim = (animId && skinHas(view, animId)) ? animId : (a->ready ? readyAnim(view) : anim::STAND);
    skinPlay(view, a->s, a->loopAnim, true);
    a->dirty = true;
}

void CreatureRenderer::setCombatReady(uint64_t guid, bool ready) {
    AnimUnit* a = animFor(guid);
    if (!a || a->ready == ready) return;
    a->ready = ready;
    // Applied at the next idle transition; a standing unit switches now.
    const Skin* sk = skinOf(a->skin);
    if (!sk || a->dead || a->wasMoving || !sk->blob) return;
    const SkinView& view = sk->view;
    uint32_t want = anim::STAND;
    if (a->emote && skinHas(view, a->emote)) want = a->emote;
    else if (ready) want = readyAnim(view);
    if (want == a->loopAnim) return;
    a->loopAnim = want;
    skinPlay(view, a->s, want, true);
    a->dirty = true;
}

// walk/run - the original client's stride match (see the header): the
// locomotion loop advances by the ground covered, not by the clock.
float CreatureRenderer::moveRate(const AnimUnit& a, const Unit& un, const Skin& sk) const {
    const CreatureUnit& u = un.u;
    if (!a.wasMoving || a.dead || u.speed <= 0.1f || a.s.sequence < 0) return 1.0f;
    if (a.s.animId != a.loopAnim) return 1.0f;  // a one-shot over the loop plays at its own pace
    const float seqSpeed = sk.view.sequences[a.s.sequence].movingSpeed * (u.scale > 0.01f ? u.scale : 1.0f);
    if (!(seqSpeed > 0.1f && seqSpeed < 100.0f)) return 1.0f;  // no stride baked (or not a mover)
    return std::clamp(u.speed / seqSpeed, 0.5f, 2.0f);
}

// ---- Mounts -------------------------------------------------------
// The mount under a player is an ordinary unit of this renderer (one more
// skinned NPC within npcbudget); these give the world screen what upstream's
// AnimationController / EntitySpawner read off the mount's CharacterRenderer
// instance: its sequences, its animation clock, the seat attachment.

const CreatureRenderer::Unit* CreatureRenderer::unitOf(uint64_t guid) const {
    for (const Unit& un : units_)
        if (un.u.guid == guid) return &un;
    return nullptr;
}

const CreatureRenderer::Seats* CreatureRenderer::seatsOf(uint16_t model) {
    auto found = seats_.find(model);
    if (found != seats_.end()) return &found->second;
    pp::PakEntry e{};
    if (!findEntry(kEntryPoints, model, e)) {
        seats_[model].loaded = true;  // no "p_": remembered (an empty table)
        return &seats_[model];
    }
    Seats& s = seats_[model];
    s.loaded = true;
    auto* blob = static_cast<uint8_t*>(loadEntry(e));
    ++stats_.loads;
    if (!blob) return &s;
    const auto* h = reinterpret_cast<const AttachTable*>(blob);
    if (e.size >= sizeof(AttachTable) && std::memcmp(h->magic, "WATP", 4) == 0 && h->version == kModelFormatVersion &&
        sizeof(AttachTable) + uint64_t{h->count} * sizeof(AttachPoint) <= e.size) {
        const auto* p = reinterpret_cast<const AttachPoint*>(blob + sizeof(AttachTable));
        s.points.assign(p, p + h->count);
    } else {
        PSP_LOGE(pp::tag::Gfx, "mount_seats_bad model=%u size=%u", static_cast<unsigned>(model),
                 static_cast<unsigned>(e.size));
    }
    pp::pakFree(blob);
    return &s;
}

bool CreatureRenderer::preload(uint32_t displayId, uint32_t* bytes) {
    if (bytes) *bytes = 0;
    const int32_t idx = findDisplay(displayId, true);
    if (idx < 0) return false;
    Display& d = displays_[static_cast<size_t>(idx)];
    uint32_t budget = 0xFFFFFFFFu;
    if (!ensure(d, budget)) return false;
    seatsOf(d.h.model);
    if (bytes) {
        uint32_t b = 0;
        if (d.skinMode == 1) {
            if (const Skin* sk = skinOf(d.h.model)) b += sk->bytes;
        } else {
            b += models_[d.h.model].bytes;
        }
        for (uint16_t id : d.textures) {
            auto it = textures_.find(id);
            if (it != textures_.end()) b += it->second.bytes;
        }
        *bytes = b;
    }
    // Resident from now on: not evicted before it is drawn (kKeepFrames).
    if (d.skinMode == 1) {
        if (Skin* sk = skinOf(d.h.model)) sk->lastUsed = frame_;
    } else {
        models_[d.h.model].lastUsed = frame_;
    }
    for (uint16_t id : d.textures) {
        auto it = textures_.find(id);
        if (it != textures_.end()) it->second.lastUsed = frame_;
    }
    d.lastUsed = frame_;
    return true;
}

bool CreatureRenderer::displaySequences(uint32_t displayId, std::vector<SequenceInfo>& out) const {
    out.clear();
    const int32_t idx = findDisplay(displayId);
    if (idx < 0) return false;
    const Display& d = displays_[static_cast<size_t>(idx)];
    const Skin* sk = skinOf(d.h.model);
    if (!d.loaded || d.skinMode != 1 || !sk || !sk->blob) return false;
    const SkinView& v = sk->view;
    for (uint32_t i = 0; i < v.h->sequenceCount; ++i) {
        const AnimSequence& q = v.sequences[i];
        SequenceInfo s;
        s.id = q.animId;
        s.variation = q.variation;
        s.flags = q.flags;
        s.durationMs = q.durationMs;
        s.movingSpeed = q.movingSpeed;
        s.frequency = q.frequency;
        s.replayMax = q.replayMax;
        out.push_back(s);
    }
    return true;
}

bool CreatureRenderer::displayHasAnimation(uint32_t displayId, uint32_t animId) const {
    const int32_t idx = findDisplay(displayId);
    if (idx < 0) return false;
    const Display& d = displays_[static_cast<size_t>(idx)];
    const Skin* sk = skinOf(d.h.model);
    if (!d.loaded || d.skinMode != 1 || !sk || !sk->blob) return false;
    return skinHas(sk->view, animId);
}

void CreatureRenderer::playDriven(uint64_t guid, uint32_t animId, bool loop) {
    AnimUnit& a = anims_[guid];
    if (!a.lastSeen) a.lastSeen = frame_;  // pruned like any unseen state
    if (const Skin* sk = skinOf(a.skin); sk && sk->blob) {
        skinPlay(sk->view, a.s, animId, loop);
        a.loopAnim = animId;
        a.drivenPending = false;
        a.dirty = true;
        return;
    }
    a.drivenAnim = animId;
    a.drivenLoop = loop;
    a.drivenPending = true;
}

bool CreatureRenderer::animationState(uint64_t guid, uint32_t& animId, float& timeMs, float& durationMs) const {
    const AnimUnit* a = animFor(guid);
    if (!a || a->s.sequence < 0) return false;
    const Skin* sk = skinOf(a->skin);
    if (!sk || !sk->blob) return false;
    const SkinView& v = sk->view;
    if (a->s.sequence >= static_cast<int>(v.h->sequenceCount)) return false;
    animId = a->s.animId;
    timeMs = a->s.timeMs;
    durationMs = static_cast<float>(v.sequences[a->s.sequence].durationMs);
    return true;
}

bool CreatureRenderer::seatPoint(uint64_t guid, glm::vec3& out) const {
    const Unit* un = unitOf(guid);
    if (!un || un->display < 0) return false;
    const Display& d = displays_[static_cast<size_t>(un->display)];
    auto st = seats_.find(d.h.model);
    if (!d.loaded || st == seats_.end() || !st->second.loaded) return false;
    const std::vector<AttachPoint>& pts = st->second.points;
    const AnimUnit* a = animFor(guid);
    // This frame's pose when the unit was drawn skinned, else the Stand matrix.
    const bool posed = a && a->poseValid && a->lastSeen == frame_ && a->skin == static_cast<int32_t>(d.h.model) &&
                       d.skinMode == 1;
    for (uint16_t id : kMountSeatAttachIds) {  // upstream: the first of 0, 5, 6, 7, 8 the model resolves
        for (const AttachPoint& p : pts) {
            if (p.id != id) continue;
            const float* m = (posed && p.bone < a->scratch.size()) ? a->scratch[p.bone].m : p.stand;
            float local[3];
            for (int r = 0; r < 3; ++r)
                local[r] = m[r] * p.position[0] + m[3 + r] * p.position[1] + m[6 + r] * p.position[2] + m[9 + r];
            const CreatureUnit& u = un->u;
            const float s = unitScale(u, d);
            const float cs = std::cos(u.orientation), sn = std::sin(u.orientation);
            out = glm::vec3(u.x + (cs * local[0] - sn * local[1]) * s, u.y + (sn * local[0] + cs * local[1]) * s,
                            u.z + local[2] * s);
            return true;
        }
    }
    return false;
}

bool CreatureRenderer::mountHeights(uint32_t displayId, float& seatZ, float& riderHeight) {
    seatZ = -1.0f;
    riderHeight = 1.8f;  // upstream's default
    const int32_t idx = findDisplay(displayId, true);
    if (idx < 0) return false;
    Display& d = displays_[static_cast<size_t>(idx)];
    if (!d.loaded && !d.failed && !loadDisplay(d)) return false;
    if (!d.loaded) return false;
    const Seats* s = seatsOf(d.h.model);
    if (!s) return false;
    for (const AttachPoint& p : s->points) {
        if (p.id == 0) seatZ = p.position[2];
        if (p.id == kAttachRiderHeight) riderHeight = p.position[2];
    }
    return !s->points.empty();
}

// a vehicle seat (VehicleSeat.AttachmentID of the vehicle's model) - seatPoint's math for one id.
bool CreatureRenderer::attachPoint(uint64_t guid, int32_t attachId, glm::vec3& out, float* riderHeight, float* yaw) {
    const Unit* un = unitOf(guid);
    if (!un || un->display < 0) return false;
    const Display& d = displays_[static_cast<size_t>(un->display)];
    if (!d.loaded) return false;
    const Seats* st = seatsOf(d.h.model);
    if (!st) return false;
    const CreatureUnit& u = un->u;
    const float s = unitScale(u, d);
    if (yaw) *yaw = u.orientation;
    if (riderHeight) {
        *riderHeight = 1.8f;
        for (const AttachPoint& p : st->points)
            if (p.id == kAttachRiderHeight) *riderHeight = p.position[2] * s;
    }
    if (attachId < 0) {
        out = glm::vec3(u.x, u.y, u.z);
        return true;
    }
    const AnimUnit* a = animFor(guid);
    const bool posed = a && a->poseValid && a->lastSeen == frame_ && a->skin == static_cast<int32_t>(d.h.model) &&
                       d.skinMode == 1;
    for (const AttachPoint& p : st->points) {
        if (static_cast<int32_t>(p.id) != attachId) continue;
        const float* m = (posed && p.bone < a->scratch.size()) ? a->scratch[p.bone].m : p.stand;
        float local[3];
        for (int r = 0; r < 3; ++r)
            local[r] = m[r] * p.position[0] + m[3 + r] * p.position[1] + m[6 + r] * p.position[2] + m[9 + r];
        const float cs = std::cos(u.orientation), sn = std::sin(u.orientation);
        out = glm::vec3(u.x + (cs * local[0] - sn * local[1]) * s, u.y + (sn * local[0] + cs * local[1]) * s,
                        u.z + local[2] * s);
        return true;
    }
    return false;
}

float CreatureRenderer::drawScale(uint64_t guid) const {
    const Unit* un = unitOf(guid);
    if (!un) return 1.0f;
    if (un->display < 0 || !displays_[static_cast<size_t>(un->display)].loaded) return un->u.scale;
    return unitScale(un->u, displays_[static_cast<size_t>(un->display)]);
}

} // namespace wowee::rendering::psp
