#include "rendering/psp/psp_equipment.hpp"

#include "core/geoset_rules.hpp"
#include "platform/psp/psp_log.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>  // std::sort / std::find
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace wowee::rendering::psp {

namespace pp = wowee::platform::psp;

// The sorted pak through PakIndex (TOC on the
// card), an older one through Pak; records, layers and Item.dbc blocks cached
// within budgets (psp_equipment.hpp ItemBank).
namespace {
constexpr uint32_t kMissingMax = 1024;  // remembered unknown displays / table blocks, then forgotten

// The location cache's slot of a key (prefix << 24 | id).
uint32_t locationSlot(uint32_t key) { return (key * 2654435761u) >> 23 & (ItemBank::kLocations - 1u); }
uint32_t locationKey(char prefix, uint32_t id) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(prefix)) << 24) | (id & 0xFFFFFFu);
}
}  // namespace

bool ItemBank::open(const char* path) {
    close();
    // data/emblems.pak beside items.pak (the guild tabard emblems, opened when a guild tabard is dressed).
    std::snprintf(emblemsPath_, sizeof(emblemsPath_), "%s", path ? path : "");
    if (char* slash = std::strrchr(emblemsPath_, '/')) slash[1] = '\0';
    else emblemsPath_[0] = '\0';
    std::strncat(emblemsPath_, "emblems.pak", sizeof(emblemsPath_) - std::strlen(emblemsPath_) - 1);
    if (index_.open(path)) {
        // Where entries are on the card, remembered (the TOC is there too).
        locations_ = static_cast<Location*>(std::calloc(kLocations, sizeof(Location)));
        loadBodyRules();
        logStats("open");
        return true;
    }
    // An older items.pak (unsorted, no "~fence").
    const bool ok = pak_.open(path);
    if (ok) {
        loadBodyRules();  // the body rules
        logStats("open");
    }
    return ok;
}

void ItemBank::close() {
    index_.close();
    pak_.close();
    freeCaches();
    emblems_.close();  // the guild emblems
    emblemsState_ = 0;
    emblemReads_ = emblemMisses_ = 0;
}

// The guild emblem parts (psp_emblem_format.hpp) from data/emblems.pak.
bool ItemBank::emblemsAvailable() {
    if (emblemsState_ == 0) {
        emblemsState_ = emblemsPath_[0] && emblems_.open(emblemsPath_) ? 1 : -1;
        PSP_LOGI(pp::tag::Gfx, "emblems_pak ok=%d records=%u ram_kb=%u", emblemsState_ > 0 ? 1 : 0,
                 static_cast<unsigned>(emblemsState_ > 0 ? emblems_.count() : 0u),
                 static_cast<unsigned>(emblemsState_ > 0 ? emblems_.ramBytes() / 1024u : 0u));
    }
    return emblemsState_ > 0;
}

void* ItemBank::loadEmblemLayer(uint8_t part, int section, uint16_t code, uint32_t& size) {
    size = 0;
    char name[32];
    if (!emblem::layerName(name, sizeof(name), part, section, code) || !emblemsAvailable()) return nullptr;
    pp::PakEntry e{};
    void* blob = emblems_.find(name, e) ? emblems_.load(e) : nullptr;
    if (!blob) {
        if (++emblemMisses_ <= 20) PSP_LOGW(pp::tag::Gfx, "emblem_layer_missing %s", name);
        return nullptr;
    }
    ++emblemReads_;
    size = e.size;
    return blob;
}

void ItemBank::freeCaches() {
    std::vector<Slot>().swap(slots_);
    slotOf_.clear();
    missing_.clear();
    for (auto& [id, l] : layers_) pp::pakFree(l.blob);
    layers_.clear();
    layerBytes_ = 0;
    for (TableBlock& t : table_) {
        if (t.blob) pp::pakFree(t.blob);
        t = TableBlock{};
    }
    tableMissing_.clear();
    tick_ = 0;
    recordReads_ = recordHits_ = layerReads_ = layerHits_ = tableReads_ = tableHits_ = 0;
    // the location cache
    std::free(locations_);
    locations_ = nullptr;
    locationHits_ = 0;
    bodyRules_ = false;
    std::memset(raceFlags_, 0, sizeof(raceFlags_));
    std::vector<HelmetVisRow>().swap(helmetVis_);
}

// "b_rules" - ChrRaces Flags and HelmetGeosetVisData (psp_item_format.hpp BodyRulesHeader).
void ItemBank::loadBodyRules() {
    uint32_t size = 0;
    void* blob = loadNamed(kBodyRulesName, size);
    BodyRulesHeader h{};
    if (blob && size >= sizeof(h)) std::memcpy(&h, blob, sizeof(h));
    if (!blob || std::memcmp(h.magic, "WBRL", 4) != 0 || h.version != 1 ||
        size < sizeof(h) + static_cast<uint32_t>(h.helmetVisRows) * sizeof(HelmetVisRow)) {
        if (blob) pp::pakFree(blob);
        PSP_LOGW(pp::tag::Gfx, "item_bank_no_body_rules (an older items.pak: helm hides the hair only, "
                               "feet painted for every race)");
        return;
    }
    std::memcpy(raceFlags_, h.raceFlags, sizeof(raceFlags_));
    helmetVis_.resize(h.helmetVisRows);
    if (h.helmetVisRows)
        std::memcpy(helmetVis_.data(), static_cast<const uint8_t*>(blob) + sizeof(h),
                    h.helmetVisRows * sizeof(HelmetVisRow));
    pp::pakFree(blob);
    bodyRules_ = true;
}

const HelmetVisRow* ItemBank::helmetVis(uint32_t id) const {
    if (!id) return nullptr;
    for (const HelmetVisRow& r : helmetVis_)
        if (r.id == id) return &r;
    return nullptr;
}

// Find an entry once, remember where it is.
bool ItemBank::locate(char prefix, uint32_t id) {
    if (!index_.isOpen() || !locations_) return false;
    const uint32_t key = locationKey(prefix, id);
    Location& l = locations_[locationSlot(key)];
    if (l.key == key) return true;
    char name[24];
    std::snprintf(name, sizeof(name), "%c_%u", prefix, static_cast<unsigned>(id));
    pp::PakEntry e{};
    if (!index_.find(name, e)) return false;
    l = Location{key, e.offset, e.size};
    return true;
}

void* ItemBank::loadEntry(char prefix, uint32_t id, uint32_t& size) {
    if (index_.isOpen() && locations_) {
        const uint32_t key = locationKey(prefix, id);
        const Location& l = locations_[locationSlot(key)];
        if (l.key == key) {
            ++locationHits_;
        } else if (!locate(prefix, id)) {
            return nullptr;
        }
        const Location& at = locations_[locationSlot(key)];
        size = at.size;
        return index_.load(pp::PakEntry{nullptr, at.offset, at.size, 0});
    }
    char name[24];
    std::snprintf(name, sizeof(name), "%c_%u", prefix, static_cast<unsigned>(id));
    return loadNamed(name, size);
}

namespace {
// Sorts ids by their entry name ("<prefix>_<id>" in strcmp order: the TOC's), each name written once.
template <typename T>
void sortByName(T* ids, uint32_t n) {
    struct Named {
        T id;
        char digits[12];
    };
    Named named[64];
    if (n > 64) n = 64;
    for (uint32_t i = 0; i < n; ++i) {
        named[i].id = ids[i];
        std::snprintf(named[i].digits, sizeof(named[i].digits), "%u", static_cast<unsigned>(ids[i]));
    }
    std::sort(named, named + n, [](const Named& a, const Named& b) { return std::strcmp(a.digits, b.digits) < 0; });
    for (uint32_t i = 0; i < n; ++i) ids[i] = named[i].id;
}
}  // namespace

void ItemBank::prefetchRecords(const uint32_t* displayIds, uint32_t count) {
    if (!isOpen() || !displayIds) return;
    uint32_t want[24];
    uint32_t n = 0;
    for (uint32_t i = 0; i < count && n < 24; ++i) {
        const uint32_t id = displayIds[i];
        if (!id || slotOf_.count(id) || missing_.count(id)) continue;
        if (std::find(want, want + n, id) != want + n) continue;
        want[n++] = id;
    }
    sortByName(want, n);
    for (uint32_t i = 0; i < n; ++i) slotFor(want[i]);
}

void ItemBank::prefetchLayers(const uint16_t* layerIds, uint32_t count) {
    if (!index_.isOpen() || !layerIds) return;
    uint16_t want[64];
    uint32_t n = 0;
    for (uint32_t i = 0; i < count && n < 64; ++i) {
        const uint16_t id = layerIds[i];
        if (id == kNoItemLayer || layers_.count(id)) continue;
        if (std::find(want, want + n, id) != want + n) continue;
        want[n++] = id;
    }
    sortByName(want, n);
    for (uint32_t i = 0; i < n; ++i) locate('q', want[i]);
}

void ItemBank::setBudget(uint32_t recordSlots, uint32_t layerCacheBytes) {
    // The slot array never grows past the capacity it was given (pointers into it stay valid); a new count
    // takes effect when it is empty.
    if (recordSlots < 32) recordSlots = 32;
    if (recordSlots > 1024) recordSlots = 1024;
    if (slots_.empty()) recordSlots_ = recordSlots;
    layerBudget_ = layerCacheBytes;
}

// A blob by name from whichever pak is open (64-byte aligned, free with pakFree).
void* ItemBank::loadNamed(const char* name, uint32_t& size) {
    if (index_.isOpen()) {
        pp::PakEntry e{};
        if (!index_.find(name, e)) return nullptr;
        size = e.size;
        return index_.load(e);
    }
    const pp::PakEntry* e = pak_.isOpen() ? pak_.find(name) : nullptr;
    if (!e) return nullptr;
    size = e->size;
    return pak_.load(e);
}

ItemBank::Slot* ItemBank::slotFor(uint32_t displayId) {
    auto it = slotOf_.find(displayId);
    if (it != slotOf_.end()) {
        Slot& s = slots_[it->second];
        s.lastUse = ++tick_;
        ++recordHits_;
        return &s;
    }
    if (missing_.count(displayId)) return nullptr;
    uint32_t size = 0;
    void* blob = loadEntry('i', displayId, size);  // its place remembered
    if (!blob || size < sizeof(ItemDisplay) || std::memcmp(blob, "WITM", 4) != 0) {
        if (blob) pp::pakFree(blob);
        if (missing_.size() >= kMissingMax) missing_.clear();
        missing_.insert(displayId);
        return nullptr;
    }
    ++recordReads_;
    if (slots_.capacity() == 0) slots_.reserve(recordSlots_);
    Slot* s = nullptr;
    if (slots_.size() < slots_.capacity()) {
        slots_.emplace_back();
        s = &slots_.back();
    } else {  // the least recently used slot gives way
        for (Slot& c : slots_)
            if (!s || c.lastUse < s->lastUse) s = &c;
        slotOf_.erase(s->displayId);
    }
    s->displayId = displayId;
    s->lastUse = ++tick_;
    std::memcpy(&s->display, blob, sizeof(s->display));
    // Version 4 ends with the body tail (ItemBodyInfo, the record's last 8 bytes).
    s->hasBody = s->display.version >= kItemFormatBody && size >= sizeof(ItemDisplay) + sizeof(ItemBodyInfo);
    if (s->hasBody)
        std::memcpy(&s->body, static_cast<const uint8_t*>(blob) + size - sizeof(ItemBodyInfo), sizeof(s->body));
    else
        s->body = ItemBodyInfo{};
    const uint32_t tail = s->hasBody ? static_cast<uint32_t>(sizeof(ItemBodyInfo)) : 0u;
    // The model tail of a version 2 record (M10c paks have none).
    s->hasModels =
        s->display.version >= kItemFormatAttach && size >= sizeof(ItemDisplay) + sizeof(ItemModels) + tail;
    if (s->hasModels)
        std::memcpy(&s->models, static_cast<const uint8_t*>(blob) + sizeof(ItemDisplay), sizeof(s->models));
    else
        std::memset(&s->models, 0, sizeof(s->models));
    pp::pakFree(blob);
    slotOf_[displayId] = static_cast<uint16_t>(s - slots_.data());
    if (recordReads_ <= 2 || (recordReads_ & 255u) == 0) logStats("records");
    return s;
}

const ItemDisplay* ItemBank::get(uint32_t displayId) {
    if (!displayId || !isOpen()) return nullptr;
    Slot* s = slotFor(displayId);
    return s ? &s->display : nullptr;
}

const ItemModels* ItemBank::models(uint32_t displayId) {
    if (!displayId || !isOpen()) return nullptr;
    const Slot* s = slotFor(displayId);
    return s && s->hasModels && s->models.kind != kItemModelNone ? &s->models : nullptr;
}

const ItemBodyInfo* ItemBank::body(uint32_t displayId) {  // the body tail
    if (!displayId || !isOpen()) return nullptr;
    const Slot* s = slotFor(displayId);
    return s && s->hasBody ? &s->body : nullptr;
}

void* ItemBank::loadModel(uint16_t id, uint32_t& size) {
    char name[16];
    std::snprintf(name, sizeof(name), "a_%u", static_cast<unsigned>(id));
    return loadNamed(name, size);
}

void* ItemBank::loadLayer(uint16_t id, uint32_t& size) {
    auto it = layers_.find(id);
    if (it == layers_.end()) {
        uint32_t got = 0;
        void* blob = loadEntry('q', id, got);  // its place remembered (prefetchLayers)
        if (!blob) return nullptr;
        ++layerReads_;
        if (layerBudget_ == 0 || got > layerBudget_ / 4u) {  // not worth a place: the caller takes the read itself
            size = got;
            return blob;
        }
        while (layerBytes_ + got > layerBudget_ && !layers_.empty()) {
            auto oldest = layers_.begin();
            for (auto c = layers_.begin(); c != layers_.end(); ++c)
                if (c->second.lastUse < oldest->second.lastUse) oldest = c;
            layerBytes_ -= oldest->second.size;
            pp::pakFree(oldest->second.blob);
            layers_.erase(oldest);
        }
        it = layers_.emplace(id, Layer{blob, got, 0}).first;
        layerBytes_ += got;
    } else {
        ++layerHits_;
    }
    it->second.lastUse = ++tick_;
    // The caller owns what it gets (pakFree), as before: a copy of the cached blob.
    void* copy = std::malloc(it->second.size);
    if (!copy) return nullptr;
    std::memcpy(copy, it->second.blob, it->second.size);
    size = it->second.size;
    return copy;
}

void* ItemBank::loadTexture(uint16_t id, uint32_t& size) {
    char name[16];
    std::snprintf(name, sizeof(name), "t_%u", static_cast<unsigned>(id));
    return loadNamed(name, size);
}

bool ItemBank::itemDisplay(uint32_t itemId, uint32_t& displayId, uint8_t& inventoryType) {
    displayId = 0;
    inventoryType = 0;
    if (!itemId || !isOpen()) return false;
    const uint32_t block = itemId / kItemTableBlock;
    TableBlock* t = nullptr;
    for (TableBlock& c : table_)
        if (c.blob && c.block == block) t = &c;
    if (t) {
        ++tableHits_;
    } else {
        if (tableMissing_.count(block)) return false;
        uint32_t size = 0;
        void* blob = loadEntry('u', block, size);  // its place remembered (a crowd re-reads blocks)
        ItemTableHeader th{};
        if (blob && size >= sizeof(th)) std::memcpy(&th, blob, sizeof(th));
        if (!blob || std::memcmp(th.magic, "WITB", 4) != 0 || th.firstItem != block * kItemTableBlock ||
            th.count > kItemTableBlock || size < sizeof(th) + th.count * 4u) {
            if (blob) pp::pakFree(blob);
            if (tableMissing_.size() >= kMissingMax) tableMissing_.clear();
            tableMissing_.insert(block);
            return false;
        }
        ++tableReads_;
        for (TableBlock& c : table_)  // an empty place, else the least recently used
            if (!t || !c.blob || (t->blob && c.lastUse < t->lastUse)) {
                t = &c;
                if (!c.blob) break;
            }
        if (t->blob) pp::pakFree(t->blob);
        t->blob = blob;
        t->size = size;
        t->block = block;
    }
    t->lastUse = ++tick_;
    ItemTableHeader th;
    std::memcpy(&th, t->blob, sizeof(th));
    const uint32_t k = itemId - th.firstItem;
    if (k >= th.count) return false;
    uint32_t v;
    std::memcpy(&v, static_cast<const uint8_t*>(t->blob) + sizeof(th) + k * 4u, 4);
    if (!v) return false;
    displayId = v & 0xFFFFFFu;
    inventoryType = static_cast<uint8_t>(v >> 24);
    return displayId != 0;
}

// + the Item.dbc SheatheType from the table block's u8 tail (psp_item_format.hpp
// kItemTableSheath) - the block itemDisplay just cached.
bool ItemBank::itemDisplay(uint32_t itemId, uint32_t& displayId, uint8_t& inventoryType, uint8_t& sheath) {
    sheath = kSheathUnknown;
    if (!itemDisplay(itemId, displayId, inventoryType)) return false;
    const uint32_t block = itemId / kItemTableBlock;
    for (const TableBlock& t : table_) {
        if (!t.blob || t.block != block) continue;
        ItemTableHeader th;
        std::memcpy(&th, t.blob, sizeof(th));
        const uint32_t k = itemId - th.firstItem;
        if ((th.reserved & kItemTableSheath) && k < th.count && t.size >= sizeof(th) + th.count * 5u)
            sheath = static_cast<const uint8_t*>(t.blob)[sizeof(th) + th.count * 4u + k];
        break;
    }
    return true;
}

uint32_t ItemBank::ramBytes() const {
    uint32_t b = index_.isOpen() ? index_.ramBytes() : 0u;
    if (pak_.isOpen()) b += pak_.count() * static_cast<uint32_t>(sizeof(pp::PakEntry) + 8u + 2u);  // records, names ~8, index
    b += static_cast<uint32_t>(slots_.capacity() * sizeof(Slot));
    b += static_cast<uint32_t>((slotOf_.size() + missing_.size() + layers_.size() + tableMissing_.size()) * 16u);
    b += layerBytes_;
    for (const TableBlock& t : table_) b += t.blob ? t.size : 0u;
    // The location cache and "b_rules"
    if (locations_) b += kLocations * static_cast<uint32_t>(sizeof(Location));
    b += static_cast<uint32_t>(helmetVis_.capacity() * sizeof(HelmetVisRow));
    if (emblemsState_ > 0) b += emblems_.ramBytes();  // the guild emblems
    return b;
}

void ItemBank::logStats(const char* at) const {
    uint32_t tableBlocks = 0;
    for (const TableBlock& t : table_) tableBlocks += t.blob ? 1u : 0u;
    // + loc_hits (entries read without the TOC), body_rules (the pak's "b_rules").
    PSP_LOGI(pp::tag::Gfx,
             "item_bank at=%s indexed=%d ram_kb=%u toc_kb=%u toc_lookups=%u toc_reads=%u recs=%u/%u rec_reads=%u "
             "rec_hits=%u layers=%u layer_kb=%u layer_reads=%u layer_hits=%u table=%u table_reads=%u table_hits=%u "
             "loc_hits=%u body_rules=%d",
             at, index_.isOpen() ? 1 : 0, static_cast<unsigned>(ramBytes() / 1024u),
             static_cast<unsigned>((index_.isOpen() ? index_.ramBytes() : 0u) / 1024u),
             static_cast<unsigned>(index_.lookups()), static_cast<unsigned>(index_.blockReads()),
             static_cast<unsigned>(slots_.size()), static_cast<unsigned>(recordSlots_),
             static_cast<unsigned>(recordReads_), static_cast<unsigned>(recordHits_),
             static_cast<unsigned>(layers_.size()), static_cast<unsigned>(layerBytes_ / 1024u),
             static_cast<unsigned>(layerReads_), static_cast<unsigned>(layerHits_), static_cast<unsigned>(tableBlocks),
             static_cast<unsigned>(tableReads_), static_cast<unsigned>(tableHits_),
             static_cast<unsigned>(locationHits_), bodyRules_ ? 1 : 0);
}

// outfitOf, applyEquipmentGeosets, equipmentLayers and capeTexture are in
// psp_equipment_dress.cpp (clean-room implementation of equipment_dress.md).

// ---------------------------------------------------------------------------
// Item models on attachment points.

namespace {

// upstream appearance_composer.cpp (anonymous namespace), unchanged.
constexpr uint32_t kAttachShield = 0;
constexpr uint32_t kAttachRightHand = 1;
constexpr uint32_t kAttachLeftHand = 2;
constexpr uint32_t kAttachBack = 12;
constexpr uint32_t kAttachSheathShield = 28;
constexpr uint32_t kAttachHipWeaponLeft = 32;
constexpr uint32_t kAttachHipWeaponRight = 33;
constexpr uint32_t kAttachHelm = 11;     // core::kAttachHelm (helm_visual.hpp)
constexpr uint32_t kNoAttach = 0xFFFFFFFFu;
// game::EquipSlot / game::InvType (game/inventory.hpp) values used here.
constexpr int kSlotHead = 0, kSlotShoulders = 2, kSlotMainHand = 15, kSlotOffHand = 16, kSlotRanged = 17;
constexpr uint8_t kInvOneHand = 13, kInvShield = 14, kInvTwoHand = 17, kInvMainHand = 21;

uint32_t weaponAttachment(bool sheathed, int slot, uint8_t inventoryType) {
    if (!sheathed) {
        if (inventoryType == kInvShield) return kAttachShield;
        return slot == kSlotOffHand ? kAttachLeftHand : kAttachRightHand;
    }

    if (inventoryType == kInvTwoHand) return kAttachBack;
    if (inventoryType == kInvShield) return kAttachSheathShield;
    if (inventoryType == kInvOneHand || inventoryType == kInvMainHand) {
        // Main hand on the left hip, drawn across the body.
        return slot == kSlotOffHand ? kAttachHipWeaponRight : kAttachHipWeaponLeft;
    }

    // Holdables and other items with no sheath position are hidden, matching
    // the original client rather than pinning books/orbs to an arbitrary bone.
    return kNoAttach;
}

// upstream weaponLocalTransform: identity unless a sheathed two-hander.
bool weaponLocalTransform(bool sheathed, uint8_t inventoryType, float out[12]) {
    if (!sheathed || inventoryType != kInvTwoHand) return false;
    glm::mat4 transform(1.0f);
    transform = glm::translate(transform, glm::vec3(-0.03f, -0.10f, 0.0f));
    transform = glm::rotate(transform, glm::radians(33.0f), glm::vec3(1, 0, 0));
    transform = glm::rotate(transform, glm::radians(90.0f), glm::vec3(0, 1, 0));
    transform = glm::rotate(transform, glm::radians(90.0f), glm::vec3(1, 0, 0));
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 3; ++r) out[c * 3 + r] = transform[c][r];
    return true;
}

} // namespace

std::vector<AttachPick> equipmentAttachments(const Equipment& eq, uint8_t race, uint8_t sex, AttachRules rules,
                                             bool sheathed, ItemBank& items) {
    std::vector<AttachPick> out;
    const int variant = itemVariant(race, sex);
    // upstream attachWeapon: a new model at a point replaces the one there.
    auto put = [&](const AttachPick& p) {
        for (auto& o : out)
            if (o.point == p.point) {
                o = p;
                return;
            }
        out.push_back(p);
    };
    auto pick = [&](int slot, int side, uint32_t point) -> AttachPick {
        AttachPick p;
        p.point = static_cast<uint16_t>(point);
        p.slot = static_cast<uint8_t>(slot);
        p.side = static_cast<uint8_t>(side);
        p.displayId = eq.displayId[slot];
        if (const ItemModels* m = items.models(eq.displayId[slot])) {
            p.model = m->model[side][variant];
            p.texture = m->texture[side][variant];
        }
        return p;
    };
    auto kindOf = [&](int slot) -> uint8_t {
        const ItemModels* m = eq.displayId[slot] ? items.models(eq.displayId[slot]) : nullptr;
        return m ? m->kind : static_cast<uint8_t>(kItemModelNone);
    };
    // Helm (HEAD slot, core::resolveHelmVisual): racial model, else base.
    if (eq.displayId[kSlotHead] && kindOf(kSlotHead) == kItemModelHelm) {
        const AttachPick p = pick(kSlotHead, 0, kAttachHelm);
        if (p.model != kNoItemModel) put(p);
    }
    // Shoulders: LeftModel at 5, RightModel at 6 (setOnlinePlayerEquipment).
    if (eq.displayId[kSlotShoulders] && kindOf(kSlotShoulders) == kItemModelShoulder) {
        for (int side = 0; side < 2; ++side) {
            const AttachPick p = pick(kSlotShoulders, side, side == 0 ? 5u : 6u);
            if (p.model != kNoItemModel) put(p);
        }
    }
    auto isWeaponKind = [](uint8_t k) { return k == kItemModelWeapon || k == kItemModelShield; };
    if (rules == AttachRules::Online) {
        // MAIN_HAND -> 1 (right hand), OFF_HAND -> 2 (left hand).
        for (int slot : {kSlotMainHand, kSlotOffHand}) {
            if (!eq.displayId[slot] || !isWeaponKind(kindOf(slot))) continue;
            const AttachPick p = pick(slot, 0, slot == kSlotMainHand ? kAttachRightHand : kAttachLeftHand);
            if (p.model != kNoItemModel) put(p);
        }
        return out;
    }
    // AppearanceComposer::loadEquippedWeapons.
    bool rightHandFilled = false;
    for (int slot : {kSlotMainHand, kSlotOffHand}) {
        if (!eq.displayId[slot]) continue;
        const uint32_t point = weaponAttachment(sheathed, slot, eq.invType[slot]);
        if (point == kNoAttach || !isWeaponKind(kindOf(slot))) continue;
        AttachPick p = pick(slot, 0, point);
        if (p.model == kNoItemModel) continue;
        p.hasLocal = weaponLocalTransform(sheathed, eq.invType[slot], p.local);
        put(p);
        if (slot == kSlotMainHand) rightHandFilled = true;
    }
    // Ranged in the right hand when the main hand is empty (on the back sheathed).
    if (!rightHandFilled && eq.displayId[kSlotRanged] && isWeaponKind(kindOf(kSlotRanged))) {
        AttachPick p = pick(kSlotRanged, 0, sheathed ? kAttachBack : kAttachRightHand);
        if (p.model != kNoItemModel) {
            p.hasLocal = weaponLocalTransform(sheathed, kInvTwoHand, p.local);
            put(p);
        }
    }
    return out;
}

// equipmentAttachmentsBySheath is in psp_weapon_placement.cpp (clean-room
// implementation of weapon_placement.md / weapon_glow.md).

} // namespace wowee::rendering::psp
