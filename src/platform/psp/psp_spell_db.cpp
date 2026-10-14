// data/spells.pak reader - see include/platform/psp/psp_spell_db.hpp.
#include "platform/psp/psp_spell_db.hpp"

#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"

#include <pspiofilemgr.h>
#include <pspkernel.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

namespace wowee::platform::psp {

namespace {
constexpr uint32_t kCloseAfterMs = 3000;
const std::string kEmpty;
}  // namespace

SpellDb::~SpellDb() {
    for (auto& [slot, ic] : icons_) textureRelease(ic.tex);
    if (fd_ >= 0) {
        pakNoteClose(fd_);
        sceIoClose(fd_);
    }
}

bool SpellDb::open() {
    char path[256];
    fsPath(path, sizeof(path), "data/spells.pak");
    path_ = path;
    Pak pak;
    if (!pak.open(path)) {
        PSP_LOGW(tag::Fs, "spells_pak missing (no spell names / icons)");
        return false;
    }
    const PakEntry* eh = pak.find("shdr");
    const PakEntry* ed = pak.find("sdata");
    const PakEntry* ei = pak.find("icons");
    const PakEntry* ef = pak.find("cfail");
    void* h = eh ? pak.load(eh) : nullptr;
    if (!h || !ed || eh->size < sizeof(SpellPakHeader)) {
        pakFree(h);
        PSP_LOGE(tag::Fs, "spells_pak bad");
        return false;
    }
    std::memcpy(&hdr_, h, sizeof(hdr_));
    const uint32_t dirBytes = hdr_.bucketCount * 12u;
    const uint32_t slotBytes = (hdr_.iconMaxId + 1u) * 2u;
    if (std::memcmp(hdr_.magic, "WSPL", 4) != 0 || hdr_.version != 1 || hdr_.recSize != sizeof(SpellRec) ||
        eh->size < sizeof(hdr_) + dirBytes + slotBytes) {
        pakFree(h);
        PSP_LOGE(tag::Fs, "spells_pak header_bad ver=%u rec=%u", static_cast<unsigned>(hdr_.version),
                 static_cast<unsigned>(hdr_.recSize));
        return false;
    }
    const auto* p = static_cast<const uint8_t*>(h) + sizeof(hdr_);
    dir_.resize(hdr_.bucketCount);
    std::memcpy(dir_.data(), p, dirBytes);
    iconSlots_.resize(hdr_.iconMaxId + 1u);
    std::memcpy(iconSlots_.data(), p + dirBytes, slotBytes);
    pakFree(h);
    sdataOff_ = ed->offset;
    iconsOff_ = ei ? ei->offset : 0;
    if (ef && ef->size > 1024) {
        if (void* f = pak.load(ef)) {
            failText_.assign(static_cast<const char*>(f), static_cast<const char*>(f) + ef->size);
            failText_.push_back('\0');
            pakFree(f);
        }
    }
    if (const PakEntry* es = pak.find("sforms"); es && es->size >= sizeof(forms_)) {
        if (void* f = pak.load(es)) {
            std::memcpy(forms_, f, sizeof(forms_));
            // + 64 x u16 SpellShapeshiftForm.Flags (a rebaked pak).
            if (es->size >= sizeof(forms_) + sizeof(formFlags_)) {
                std::memcpy(formFlags_, static_cast<const uint8_t*>(f) + sizeof(forms_), sizeof(formFlags_));
                formFlagsLoaded_ = true;
            }
            pakFree(f);
        }
    }
    // "suse" header only (WSUS, version, bucketShift, bucketCount, recSize, 0); the
    // directory and the records are read with their bucket (loadUse).
    useOff_ = useBuckets_ = useShift_ = 0;
    if (const PakEntry* eu = pak.find("suse"); eu && eu->size >= 24) {
        uint32_t h[6] = {};
        if (pakRead(pak.fd(), eu->offset, h, sizeof(h)) && std::memcmp(h, "WSUS", 4) == 0 && h[1] == 1 &&
            h[4] == sizeof(SpellUseRec) && h[2] == hdr_.bucketShift && eu->size >= 24u + h[3] * 8u) {
            useOff_ = eu->offset;
            useBuckets_ = h[3];
            useShift_ = h[2];
        }
    }
    openExtras(pak);  // talents + skill lines
    openTips(pak);    // "stip" + the name tables (headers only)
    pak.close();
    PSP_LOGI(tag::Fs, "spells_pak buckets=%u icons=%u stride=%u fail_texts=%d", static_cast<unsigned>(hdr_.bucketCount),
             static_cast<unsigned>(hdr_.iconCount), static_cast<unsigned>(hdr_.iconStride), failText_.empty() ? 0 : 1);
    PSP_LOGI(tag::Fs, "spells_pak use=%d form_flags=%d", useOff_ != 0 ? 1 : 0, formFlagsLoaded_ ? 1 : 0);
    return true;
}

bool SpellDb::ensureOpen() {
    if (fd_ >= 0) return true;
    if (path_.empty()) return false;
    fd_ = pakOpenRetry(path_.c_str());  // + one retry after a rare pak gave a descriptor back
    if (fd_ < 0) {
        PSP_LOGE(tag::Fs, "spells_pak open err=%08X", static_cast<unsigned>(fd_));
        fd_ = -1;
        return false;
    }
    pakNoteOpen(fd_, path_.c_str());  // in the descriptor budget
    return true;
}

bool SpellDb::readRaw(uint32_t offset, void* dst, uint32_t size) {
    if (!ensureOpen()) return false;
    lastReadMs_ = logUptimeMs();
    return pakRead(fd_, offset, dst, size);
}

void SpellDb::tick(uint32_t nowMs) {
    if (fd_ >= 0 && nowMs - lastReadMs_ > kCloseAfterMs) {
        pakNoteClose(fd_);
        sceIoClose(fd_);
        fd_ = -1;
    }
}

bool SpellDb::readBucket(uint32_t b) {
    if (b == lastBucket_) return true;
    lastBucket_ = ~0u;
    const Bucket& d = dir_[b];
    if (d.count == 0 || d.size < 4 + d.count * sizeof(SpellRec)) return false;
    lastBucketBuf_.assign(d.size + 1, 0);
    if (!readRaw(sdataOff_ + d.offset, lastBucketBuf_.data(), d.size)) {
        PSP_LOGE(tag::Fs, "spells_pak read_fail bucket=%u", static_cast<unsigned>(b));
        return false;
    }
    ++bucketsRead_;
    lastBucket_ = b;
    return true;
}

void SpellDb::loadBucket(uint32_t b, const uint32_t* ids, size_t n) {
    if (b >= dir_.size() || !readBucket(b)) {
        for (size_t k = 0; k < n; ++k) missing_.insert(ids[k]);
        return;
    }
    const Bucket& d = dir_[b];
    const uint8_t* buf = lastBucketBuf_.data();
    const auto* recs = reinterpret_cast<const SpellRec*>(buf + 4);
    const char* strings = reinterpret_cast<const char*>(buf + 4 + d.count * sizeof(SpellRec));
    const uint32_t strBytes = d.size - 4 - d.count * static_cast<uint32_t>(sizeof(SpellRec));
    auto str = [&](uint32_t off) { return off < strBytes ? std::string(strings + off) : std::string(); };
    std::vector<uint32_t> added;  // their "suse" records (loadUse)
    for (size_t k = 0; k < n; ++k) {
        const uint32_t id = ids[k];
        if (cache_.count(id)) continue;
        // Records are sorted by id: binary search.
        uint32_t lo = 0, hi = d.count;
        while (lo < hi) {
            const uint32_t mid = (lo + hi) / 2;
            if (recs[mid].id < id) lo = mid + 1;
            else hi = mid;
        }
        if (lo >= d.count || recs[lo].id != id) {
            missing_.insert(id);
            continue;
        }
        SpellInfo& s = cache_[id];
        std::memcpy(&s.rec, &recs[lo], sizeof(SpellRec));
        s.name = str(recs[lo].nameOff);
        s.rank = str(recs[lo].rankOff);
        if (useOff_) added.push_back(id);
    }
    if (!added.empty()) loadUse(b, added.data(), added.size());
}

// the "suse" records of the spells just cached (sorted ids of one bucket): the 8-byte
// directory entry, then the bucket's records + reagents (~1-3 KB), released at once.
void SpellDb::loadUse(uint32_t b, const uint32_t* ids, size_t n) {
    if (!useOff_ || b >= useBuckets_ || b != (ids[0] >> useShift_)) return;
    struct {
        uint32_t offset;
        uint16_t count, reagents;
    } d{};
    if (!readRaw(useOff_ + 24u + b * 8u, &d, sizeof(d)) || d.count == 0) return;
    const uint32_t bytes = d.count * static_cast<uint32_t>(sizeof(SpellUseRec)) + d.reagents * 4u;
    std::vector<uint8_t> buf(bytes);
    if (!readRaw(useOff_ + d.offset, buf.data(), bytes)) {
        PSP_LOGE(tag::Fs, "spells_pak use_read_fail bucket=%u", static_cast<unsigned>(b));
        return;
    }
    const auto* recs = reinterpret_cast<const SpellUseRec*>(buf.data());
    const auto* reag = reinterpret_cast<const uint32_t*>(buf.data() + d.count * sizeof(SpellUseRec));
    for (size_t k = 0; k < n; ++k) {
        uint32_t lo = 0, hi = d.count;
        while (lo < hi) {
            const uint32_t mid = (lo + hi) / 2;
            if (recs[mid].id < ids[k]) lo = mid + 1;
            else hi = mid;
        }
        if (lo >= d.count || recs[lo].id != ids[k]) continue;  // nothing to check beyond the record
        auto it = cache_.find(ids[k]);
        if (it == cache_.end()) continue;
        const SpellUseRec& u = recs[lo];
        SpellInfo& s = it->second;
        s.stances = u.stances;
        s.stancesNot = u.stancesNot;
        s.manaPct = u.manaPct;
        s.useFlags = u.flags;
        std::memcpy(s.auraState, u.auraState, sizeof(s.auraState));
        if (u.reagentCount && u.firstReagent + u.reagentCount <= d.reagents)
            s.reagents.assign(reag + u.firstReagent, reag + u.firstReagent + u.reagentCount);
    }
}

const SpellInfo* SpellDb::get(uint32_t spellId) {
    if (spellId == 0 || dir_.empty()) return nullptr;
    auto it = cache_.find(spellId);
    if (it != cache_.end()) return &it->second;
    if (missing_.count(spellId)) return nullptr;
    loadBucket(spellId >> hdr_.bucketShift, &spellId, 1);
    it = cache_.find(spellId);
    return it != cache_.end() ? &it->second : nullptr;
}

void SpellDb::prefetch(const std::vector<uint32_t>& ids) {
    if (dir_.empty()) return;
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    const uint32_t before = bucketsRead_;
    std::vector<uint32_t> sorted(ids);  // by id = by bucket
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    for (size_t i = 0; i < sorted.size();) {
        const uint32_t b = sorted[i] >> hdr_.bucketShift;
        size_t j = i;
        while (j < sorted.size() && (sorted[j] >> hdr_.bucketShift) == b) ++j;
        loadBucket(b, &sorted[i], j - i);
        i = j;
    }
    PSP_LOGI(tag::Fs, "spells_prefetch ids=%u buckets=%u cached=%u ms=%u", static_cast<unsigned>(ids.size()),
             static_cast<unsigned>(bucketsRead_ - before), static_cast<unsigned>(cache_.size()),
             static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u));
}

const std::string& SpellDb::name(uint32_t spellId) {
    const SpellInfo* s = get(spellId);
    return s ? s->name : kEmpty;
}

const Texture* SpellDb::icon(uint32_t spellId, uint32_t frame) {
    const SpellInfo* s = get(spellId);
    if (!s || s->rec.iconSlot == 0xFFFF || s->rec.iconSlot >= hdr_.iconCount || iconsOff_ == 0) return nullptr;
    return iconBySlot(s->rec.iconSlot, frame);  // shared with iconById
}

const Texture* SpellDb::iconBySlot(uint16_t slot, uint32_t frame) {
    if (slot == 0xFFFF || slot >= hdr_.iconCount || iconsOff_ == 0) return nullptr;
    auto it = icons_.find(slot);
    if (it != icons_.end()) {
        it->second.lastFrame = frame;
        return it->second.failed ? nullptr : &it->second.tex;
    }
    if (icons_.size() >= kMaxIcons) {  // evict the oldest not drawn in the last two frames
        auto victim = icons_.end();
        for (auto i = icons_.begin(); i != icons_.end(); ++i)
            if (frame - i->second.lastFrame > 2 && (victim == icons_.end() || i->second.lastFrame < victim->second.lastFrame))
                victim = i;
        if (victim == icons_.end()) return nullptr;  // all in use: skip this frame
        textureRelease(victim->second.tex);
        icons_.erase(victim);
    }
    Icon& ic = icons_[slot];
    ic.lastFrame = frame;
    const uint32_t size = hdr_.iconStride;
    void* block = memalign(64, size);
    if (!block || !readRaw(iconsOff_ + slot * size, block, size)) {
        std::free(block);
        ic.failed = true;
        return nullptr;
    }
    sceKernelDcacheWritebackRange(block, size);
    if (!textureFromBlob(block, size, ic.tex, true)) {
        std::free(block);
        ic.failed = true;
        return nullptr;
    }
    ++iconsLoaded_;
    return &ic.tex;
}

const char* SpellDb::castFailText(uint8_t result) const {
    if (failText_.size() <= 1024) return nullptr;
    uint32_t off = 0;
    std::memcpy(&off, failText_.data() + result * 4u, 4);
    if (off == 0 || off >= failText_.size()) return nullptr;
    return failText_.data() + off;
}

// --- GameTooltip data ---------------------------------------------------------------------------------------
// WBBT (tools/psp/build_spells_pak.py bbt): "WBBT" u32 version 1, bucketShift, bucketCount, count, 0; the
// bucket directory (u32 offset from the entry, u32 n); a bucket = n ids, n + 1 offsets, the records. Only the
// 24-byte headers are read at open; a lookup reads 8 bytes, the bucket's index (<= 2 KB) and the record.

void SpellDb::openTips(Pak& pak) {
    static const char* kNames[kTipTableCount] = {"stip", "tform", "ttotem", "tsub", "tsubm", "tench", "tgem",
                                                 "tset", "tlimit"};
    int found = 0;
    for (int t = 0; t < kTipTableCount; ++t) {
        tipTables_[t] = TipTable{};
        const PakEntry* e = pak.find(kNames[t]);
        uint32_t h[6] = {};
        if (!e || e->size < sizeof(h) || !pakRead(pak.fd(), e->offset, h, sizeof(h))) continue;
        if (std::memcmp(h, "WBBT", 4) != 0 || h[1] != 1 || h[2] > 31 || e->size < sizeof(h) + h[3] * 8u) continue;
        tipTables_[t] = TipTable{e->offset, e->size, h[2], h[3]};
        ++found;
    }
    PSP_LOGI(tag::Fs, "spells_pak tips=%d tables=%d of %d", hasTips() ? 1 : 0, found, static_cast<int>(kTipTableCount));
}

bool SpellDb::tipBlob(SpellTipTable table, uint32_t id, std::vector<uint8_t>& out) {
    out.clear();
    if (table >= kTipTableCount) return false;
    const TipTable& t = tipTables_[table];
    if (t.offset == 0) return false;
    const uint32_t b = id >> t.shift;
    if (b >= t.buckets) return false;
    uint32_t d[2] = {};
    if (!readRaw(t.offset + 24u + b * 8u, d, sizeof(d)) || d[1] == 0 || d[1] > 4096u || d[0] >= t.size) return false;
    const uint32_t n = d[1];
    std::vector<uint32_t> idx(2u * n + 1u);
    if (!readRaw(t.offset + d[0], idx.data(), static_cast<uint32_t>(idx.size() * 4u))) return false;
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        if (idx[mid] < id) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= n || idx[lo] != id) return false;
    const uint32_t from = idx[n + lo], to = idx[n + lo + 1];
    const uint32_t base = d[0] + static_cast<uint32_t>(idx.size() * 4u);
    if (to < from || to - from > 65536u || base + to > t.size) return false;
    out.resize(to - from);
    ++tipReads_;
    return to == from || readRaw(t.offset + base + from, out.data(), to - from);
}

std::string SpellDb::tipName(SpellTipTable table, uint32_t id) {
    std::vector<uint8_t> b;
    if (!tipBlob(table, id, b)) return std::string();
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

const SpellTip* SpellDb::tip(uint32_t spellId) {
    for (size_t i = 0; i < tipCache_.size(); ++i) {
        if (tipCache_[i]->id != spellId) continue;
        if (i + 1 != tipCache_.size()) {  // most recent last (the unique_ptr moves, the record does not)
            auto e = std::move(tipCache_[i]);
            tipCache_.erase(tipCache_.begin() + static_cast<std::ptrdiff_t>(i));
            tipCache_.push_back(std::move(e));
        }
        return tipCache_.back().get();
    }
    std::vector<uint8_t> b;
    if (!tipBlob(kTipSpell, spellId, b) || b.size() < 12) return nullptr;
    const uint8_t* p = b.data();
    const uint8_t* end = p + b.size();
    auto u32 = [&](uint32_t& v) {
        if (p + 4 > end) return false;
        std::memcpy(&v, p, 4);
        p += 4;
        return true;
    };
    SpellTip s;
    uint32_t mask = 0, lens = 0;
    u32(s.id);
    u32(mask);
    u32(lens);
    // The scalar fields in bit order (build_spells_pak.py TIP_FIELDS).
    uint32_t f[29] = {};
    for (int i = 0; i < 29; ++i)
        if ((mask & (1u << i)) && !u32(f[i])) return nullptr;
    auto fl = [](uint32_t u) {
        float v;
        std::memcpy(&v, &u, 4);
        return v;
    };
    s.dur = static_cast<int32_t>(f[0]);
    s.durPerLevel = static_cast<int32_t>(f[1]);
    s.durMax = static_cast<int32_t>(f[2]);
    s.procChance = f[3];
    s.procCharges = f[4];
    s.maxTargets = f[5];
    s.stack = f[6];
    s.maxTargetLevel = f[7];
    s.maxLevel = f[8];
    s.baseLevel = f[9];
    s.spellLevel = f[10];
    s.attrEx2 = f[11];
    s.attrEx3 = f[12];
    s.attrEx6 = f[13];
    s.manaPerSecond = f[14];
    s.manaPerLevel = f[15];
    s.runes[0] = static_cast<uint8_t>(f[16]);
    s.runes[1] = static_cast<uint8_t>(f[16] >> 8);
    s.runes[2] = static_cast<uint8_t>(f[16] >> 16);
    s.eqClass = static_cast<int32_t>(f[17]) - 1;  // baked + 1
    s.eqSubMask = f[18];
    s.eqInvMask = f[19];
    s.totem[0] = f[20];
    s.totem[1] = f[21];
    s.totemCat[0] = f[22];
    s.totemCat[1] = f[23];
    s.spellFocus = f[24];
    s.minRangeHostile = fl(f[25]);
    s.maxRangeHostile = fl(f[26]);
    s.rangeFlags = f[27];
    for (int k = 0; k < 3; ++k) {
        if (!(mask & (1u << (29 + k)))) continue;
        uint32_t em = 0;
        if (!u32(em)) return nullptr;
        uint32_t ev[15] = {};
        for (int j = 0; j < 15; ++j)
            if ((em & (1u << j)) && !u32(ev[j])) return nullptr;
        SpellTipEffect& e = s.eff[k];
        e.effect = static_cast<uint8_t>(ev[0]);
        e.aura = static_cast<uint16_t>(ev[0] >> 16);
        e.basePoints = static_cast<int32_t>(ev[1]);
        e.dieSides = static_cast<int32_t>(ev[2]);
        e.realPointsPerLevel = fl(ev[3]);
        e.pointsPerCombo = fl(ev[4]);
        e.amplitude = ev[5];
        e.radius = fl(ev[6]);
        e.radiusMax = fl(ev[7]);
        e.chainTarget = ev[8];
        e.multipleValue = fl(ev[9]);
        e.miscValue = static_cast<int32_t>(ev[10]);
        e.triggerSpell = ev[11];
        e.damageMultiplier = (em & (1u << 12)) ? fl(ev[12]) : 1.0f;  // 1.0 is not baked
        e.bonusMultiplier = fl(ev[13]);
        e.miscValueB = static_cast<int32_t>(ev[14]);
    }
    const uint32_t descLen = lens & 0xFFFFu, varLen = lens >> 16;
    if (p + descLen + varLen > end) return nullptr;
    s.desc.assign(reinterpret_cast<const char*>(p), descLen);
    s.var.assign(reinterpret_cast<const char*>(p + descLen), varLen);
    s.id = spellId;
    if (tipCache_.size() >= 12) tipCache_.erase(tipCache_.begin());
    tipCache_.push_back(std::make_unique<SpellTip>(std::move(s)));
    return tipCache_.back().get();
}

} // namespace wowee::platform::psp
