// The glows on weapons - see include/rendering/psp/psp_weapon_fx.hpp.
// Upstream: AppearanceComposer::applyEnchantVisuals, CharacterRenderer::
// attachWeaponEffect and its matrix update (the effect = weaponMat x T(the weapon's attachment of the slot));
// the particles, ribbons and meshes: the shared engine (psp_particles.cpp, M2Renderer's rules).
#include "rendering/psp/psp_weapon_fx.hpp"

#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_texture.hpp"

#include <pspkernel.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

namespace wowee::rendering::psp {

namespace pp = wowee::platform::psp;

struct WeaponGlows::Loaded {
    void* blob = nullptr;  // items.pak's "ifx_<n>" (pakFree)
    uint32_t bytes = 0;
    FxView view;
    uint32_t lastFrame = 0;
    int users = 0;         // slots playing it
    ~Loaded() {
        if (blob) pp::pakFree(blob);
    }
};

struct WeaponGlows::Tex {
    pp::Texture tex;
    uint32_t bytes = 0;
    uint32_t lastFrame = 0, triedFrame = 0;
    int refs = 0;          // loaded effects using it
    bool ok = false;
    bool broken = false;   // not in the pak / unreadable: not tried again (a full cache is tried again when drawn)
    ~Tex() {
        if (ok) pp::textureRelease(tex);
    }
};

struct WeaponGlows::Slot {
    uint64_t key = 0;
    bool used = false;
    uint32_t seen = 0;
    Loaded* fx = nullptr;
    uint16_t effect = 0;
    float acc[kFxEmitterSlots] = {};
    int16_t lanes[kFxMaxRibbons] = {-1, -1, -1, -1};
    float pos[3] = {0, 0, 0};
    float axes[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    float timeMs = 0.0f;
    float d2 = 0.0f;
    uint8_t owner = 0;
};

WeaponGlows::WeaponGlows() = default;

WeaponGlows::~WeaponGlows() { close(); }

bool WeaponGlows::open(const WeaponFxConfig& cfg, ItemBank& items) {
    close();
    cfg_ = cfg;
    cfg_.maxInstances = std::clamp(cfg_.maxInstances, 1, static_cast<int>(kWeaponOwnerEnd - kWeaponOwnerBase));
    cfg_.distance = std::clamp(cfg_.distance, 5.0f, 100.0f);
    cfg_.cacheBytes = std::clamp<uint32_t>(cfg_.cacheBytes, 16u * 1024u, 1024u * 1024u);
    cfg_.maxPx = std::clamp(cfg_.maxPx, 4, 128);
    if (!items.isOpen()) return false;
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    uint32_t size = 0;
    void* blob = items.loadRecord("ivis", size);
    if (!blob) {
        PSP_LOGW(pp::tag::Gfx, "weaponfx_none (items.pak has no \"ivis\": rebake it with the psp_item_conv tool)");
        return false;
    }
    if (!itemVisualTableValid(blob, size)) {
        pp::pakFree(blob);
        PSP_LOGE(pp::tag::Gfx, "weaponfx_bad ivis size=%u (rebake items.pak)", static_cast<unsigned>(size));
        return false;
    }
    table_.assign(static_cast<const uint8_t*>(blob), static_cast<const uint8_t*>(blob) + size);
    pp::pakFree(blob);
    if (!particleEngine().acquireShared()) {
        PSP_LOGE(pp::tag::Gfx, "weaponfx_off (no memory for the particle pool)");
        table_.clear();
        return false;
    }
    engine_ = true;
    slots_.reset(new (std::nothrow) Slot[cfg_.maxInstances]);
    if (!slots_) {
        close();
        return false;
    }
    for (int i = 0; i < cfg_.maxInstances; ++i) slots_[i].owner = static_cast<uint8_t>(kWeaponOwnerBase + i);
    wants_.reserve(static_cast<size_t>(cfg_.maxInstances));
    list_.reserve(static_cast<size_t>(cfg_.maxInstances));
    items_ = &items;
    ItemVisualTableHeader h;
    std::memcpy(&h, table_.data(), sizeof(h));
    PSP_LOGI(pp::tag::Gfx,
             "weaponfx on visuals=%u enchants=%u effects=%u textures=%u table_kb=%u inst=%d dist=%.0f kb=%u px=%d "
             "share=%d ms=%u",
             static_cast<unsigned>(h.visualCount), static_cast<unsigned>(h.enchantCount),
             static_cast<unsigned>(h.effectCount), static_cast<unsigned>(h.textureCount),
             static_cast<unsigned>((table_.size() + 1023u) / 1024u), cfg_.maxInstances,
             static_cast<double>(cfg_.distance), static_cast<unsigned>(cfg_.cacheBytes / 1024u), cfg_.maxPx,
             particleEngine().weaponShare(), static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u));
    return true;
}

void WeaponGlows::releaseSlot(Slot& s) {
    if (!s.used) return;
    particleEngine().freeOwner(s.owner);
    for (int16_t& l : s.lanes) {
        particleEngine().freeLane(l);
        l = -1;
    }
    if (s.fx) {
        if (s.fx->users > 0) --s.fx->users;
        s.fx->lastFrame = frame_;  // its blob may be in this frame's display list
    }
    s.used = false;
    s.fx = nullptr;
    s.key = 0;
}

void WeaponGlows::close() {
    if (slots_) {
        for (int i = 0; i < cfg_.maxInstances; ++i) releaseSlot(slots_[i]);
        slots_.reset();
    }
    if (engine_) particleEngine().releaseShared();
    engine_ = false;
    loaded_.clear();
    textures_.clear();
    broken_.clear();
    noRoom_.clear();
    table_.clear();
    wants_.clear();
    list_.clear();
    cacheBytes_ = 0;
    items_ = nullptr;
}

uint16_t WeaponGlows::enchantVisual(uint32_t enchantField) const {
    return table_.empty() ? 0 : visibleEnchantVisual(table_.data(), enchantField);
}

// LRU: unreferenced textures first, then effects no slot plays; nothing the GE may still read (drawn in the last
// 2 frames) goes.
bool WeaponGlows::makeRoom(uint32_t need) {
    while (cacheBytes_ + need > cfg_.cacheBytes) {
        auto tv = textures_.end();
        for (auto it = textures_.begin(); it != textures_.end(); ++it)
            if (it->second->refs == 0 && frame_ - it->second->lastFrame > 2 &&
                (tv == textures_.end() || it->second->lastFrame < tv->second->lastFrame))
                tv = it;
        if (tv != textures_.end()) {
            cacheBytes_ -= std::min(cacheBytes_, tv->second->bytes);
            textures_.erase(tv);
            ++evicts_;
            continue;
        }
        auto ev = loaded_.end();
        for (auto it = loaded_.begin(); it != loaded_.end(); ++it)
            if (it->second->users == 0 && frame_ - it->second->lastFrame > 2 &&
                (ev == loaded_.end() || it->second->lastFrame < ev->second->lastFrame))
                ev = it;
        if (ev == loaded_.end()) return false;
        const Loaded& L = *ev->second;
        for (uint32_t i = 0; i < L.view.h->textureCount; ++i) {
            auto t = textures_.find(L.view.textures[i]);
            if (t != textures_.end() && t->second->refs > 0) --t->second->refs;
        }
        cacheBytes_ -= std::min(cacheBytes_, L.bytes);
        loaded_.erase(ev);
        ++evicts_;
    }
    return true;
}

// One "ift_<n>" into `t` (the cache must have the room).
static bool readGlowTexture(ItemBank& items, uint16_t id, pp::Texture& tex, uint32_t& bytes) {
    char name[16];
    std::snprintf(name, sizeof(name), "ift_%u", static_cast<unsigned>(id));
    uint32_t size = 0;
    void* blob = items.loadRecord(name, size);
    if (!blob) return false;
    if (!pp::textureFromBlob(blob, size, tex, true)) {
        pp::pakFree(blob);
        return false;
    }
    bytes = size;
    return true;
}

const pp::Texture* WeaponGlows::texture(uint16_t id) {
    auto it = textures_.find(id);
    if (it == textures_.end()) return nullptr;
    Tex& t = *it->second;
    t.lastFrame = frame_;
    if (!t.ok && !t.broken && t.triedFrame != frame_ && items_) {  // the cache was full at load: once a frame
        t.triedFrame = frame_;
        if (makeRoom(8u * 1024u)) {
            if (readGlowTexture(*items_, id, t.tex, t.bytes)) {
                t.ok = true;
                cacheBytes_ += t.bytes;
            } else {
                t.broken = true;
            }
        }
    }
    return t.ok ? &t.tex : nullptr;
}

const pp::Texture* WeaponGlows::texFn(void* user, uint16_t id) { return static_cast<WeaponGlows*>(user)->texture(id); }

bool WeaponGlows::readable(uint16_t id) const {
    if (broken_.count(id)) return false;
    const auto w = noRoom_.find(id);
    return w == noRoom_.end() || frame_ - w->second >= 60u;
}

// An effect into the cache (its textures with it), or nullptr (not now / broken).
WeaponGlows::Loaded* WeaponGlows::effect(uint16_t id, bool allowLoad) {
    auto found = loaded_.find(id);
    if (found != loaded_.end()) {
        found->second->lastFrame = frame_;
        return found->second.get();
    }
    if (!allowLoad || !items_ || !readable(id)) return nullptr;
    PROF_SCOPE("weaponfx.load");
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    char name[16];
    std::snprintf(name, sizeof(name), "ifx_%u", static_cast<unsigned>(id));
    uint32_t size = 0;
    void* blob = items_->loadRecord(name, size);
    if (!blob || !spellFxValid(blob, size) || !spellFxExt(blob)) {
        if (blob) pp::pakFree(blob);
        broken_.insert(id);
        if (++fails_ <= 10) PSP_LOGW(pp::tag::Gfx, "weaponfx_bad effect=ifx_%u (rebake items.pak)", static_cast<unsigned>(id));
        return nullptr;
    }
    if (size > cfg_.cacheBytes) {  // Never fits (enchantfxkb) - not read again (makeRoom would empty the cache)
        pp::pakFree(blob);
        broken_.insert(id);
        if (++fails_ <= 10)
            PSP_LOGW(pp::tag::Gfx, "weaponfx_big effect=ifx_%u bytes=%u cache=%u", static_cast<unsigned>(id),
                     static_cast<unsigned>(size), static_cast<unsigned>(cfg_.cacheBytes));
        return nullptr;
    }
    if (!makeRoom(size)) {  // everything in use: tried again ~1 s later
        pp::pakFree(blob);
        if (noRoom_.size() >= 64) noRoom_.clear();
        noRoom_[id] = frame_;
        return nullptr;
    }
    noRoom_.erase(id);
    auto L = std::make_unique<Loaded>();
    L->blob = blob;
    L->bytes = size;
    L->view.set(blob);
    L->lastFrame = frame_;
    cacheBytes_ += size;
    uint32_t texRead = 0;
    for (uint32_t i = 0; i < L->view.h->textureCount; ++i) {
        const uint16_t tid = L->view.textures[i];
        auto it = textures_.find(tid);
        if (it == textures_.end()) {
            auto te = std::make_unique<Tex>();
            te->lastFrame = frame_;
            te->triedFrame = frame_;
            if (makeRoom(8u * 1024u)) {
                if (readGlowTexture(*items_, tid, te->tex, te->bytes)) {
                    te->ok = true;
                    cacheBytes_ += te->bytes;
                    ++texRead;
                } else {
                    te->broken = true;
                }
            }
            it = textures_.emplace(tid, std::move(te)).first;
        }
        ++it->second->refs;
    }
    Loaded* out = L.get();
    loaded_.emplace(id, std::move(L));
    if (++loads_ <= 40)
        PSP_LOGI(pp::tag::Gfx, "weaponfx_load effect=ifx_%u kb=%u emitters=%u ribbons=%u groups=%u tex_read=%u cache_kb=%u ms=%u",
                 static_cast<unsigned>(id), static_cast<unsigned>((size + 1023u) / 1024u),
                 static_cast<unsigned>(out->view.h->emitterCount), static_cast<unsigned>(out->view.ribbonCount),
                 static_cast<unsigned>(out->view.h->groupCount), static_cast<unsigned>(texRead),
                 static_cast<unsigned>(cacheBytes_ / 1024u),
                 static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u));
    return out;
}

void WeaponGlows::frame(const std::vector<Request>& requests, const TerrainCamera& cam, float dtSec) {
    if (!slots_ || table_.empty()) return;
    ++frame_;
    const uint32_t nowMs = pp::logUptimeMs();
    bool anyUsed = false;
    for (int i = 0; i < cfg_.maxInstances; ++i) anyUsed = anyUsed || slots_[i].used;
    if (requests.empty() && !anyUsed) {
        log(nowMs);
        return;
    }
    PROF_SCOPE("weaponfx");
    const uint32_t t0 = sceKernelGetSystemTimeLow();
    const float dt = std::clamp(dtSec, 0.0f, 0.25f);
    // The effects wanted: the own weapons first, then the nearest characters'; each effect of a visual at the
    // weapon's attachment of its slot (upstream: weaponMat x T(offset), the weapon's orientation).
    constexpr size_t kMaxRequests = 64;
    const Request* order[kMaxRequests];
    size_t nreq = 0;
    const float lim2 = cfg_.distance * cfg_.distance;
    for (const Request& r : requests)
        if (nreq < kMaxRequests && (r.own || r.d2 <= lim2)) order[nreq++] = &r;
    std::sort(order, order + nreq, [](const Request* a, const Request* b) {
        if (a->own != b->own) return a->own;
        return a->d2 < b->d2;
    });
    wants_.clear();
    for (size_t k = 0; k < nreq && wants_.size() < static_cast<size_t>(cfg_.maxInstances); ++k) {
        const WeaponGlow& g = order[k]->glow;
        const ItemVisualRec* rec = itemVisualFind(table_.data(), g.itemVisual);
        if (!rec) continue;
        for (int vs = 0; vs < kItemVisualSlots && wants_.size() < static_cast<size_t>(cfg_.maxInstances); ++vs) {
            if (rec->effect[vs] == 0xFFFF) continue;
            Want w;
            // A key per (character, weapon slot, visual slot): the same effect keeps its slot (and particles).
            w.key = (order[k]->owner * 0x9E3779B97F4A7C15ull) ^ (static_cast<uint64_t>(g.slot) << 8) ^
                    static_cast<uint64_t>(vs + 1);
            w.effect = rec->effect[vs];
            const float* p = (g.points.found & (1u << vs)) ? g.points.position[vs] : nullptr;
            for (int r = 0; r < 3; ++r) {
                float v = g.world[9 + r];
                if (p) v += g.world[r] * p[0] + g.world[3 + r] * p[1] + g.world[6 + r] * p[2];
                w.pos[r] = v;
            }
            std::memcpy(w.axes, g.world, sizeof(w.axes));
            w.d2 = order[k]->own ? 0.0f : order[k]->d2;
            wants_.push_back(w);
        }
    }
    // Slots: kept when wanted again (moved to the weapon's place of this frame), freed when not, new ones take a
    // free slot (one effect read a frame).
    for (int i = 0; i < cfg_.maxInstances; ++i) slots_[i].seen = 0;
    for (const Want& w : wants_)
        for (int i = 0; i < cfg_.maxInstances; ++i) {
            Slot& s = slots_[i];
            if (!s.used || s.key != w.key || s.effect != w.effect) continue;
            s.seen = frame_;
            std::memcpy(s.pos, w.pos, sizeof(s.pos));
            std::memcpy(s.axes, w.axes, sizeof(s.axes));
            s.d2 = w.d2;
            break;
        }
    for (int i = 0; i < cfg_.maxInstances; ++i)
        if (slots_[i].used && slots_[i].seen != frame_) releaseSlot(slots_[i]);
    int readsLeft = 1;
    for (const Want& w : wants_) {
        bool have = false;
        for (int i = 0; i < cfg_.maxInstances && !have; ++i) have = slots_[i].used && slots_[i].key == w.key;
        if (have) continue;
        int free = -1;
        for (int i = 0; i < cfg_.maxInstances && free < 0; ++i)
            if (!slots_[i].used) free = i;
        if (free < 0) break;
        const bool resident = loaded_.count(w.effect) != 0;
        if (!resident && !readable(w.effect)) continue;  // Broken / no room lately - no read spent
        if (!resident && readsLeft <= 0) continue;  // read next frame
        if (!resident) --readsLeft;
        Loaded* L = effect(w.effect, true);
        if (!L) continue;
        Slot& s = slots_[free];
        s.used = true;
        s.key = w.key;
        s.effect = w.effect;
        s.seen = frame_;
        s.fx = L;
        ++L->users;
        for (float& a : s.acc) a = 0.0f;
        std::memcpy(s.pos, w.pos, sizeof(s.pos));
        std::memcpy(s.axes, w.axes, sizeof(s.axes));
        s.d2 = w.d2;
        s.timeMs = 0.0f;  // upstream: a new effect instance starts its sequence 0 at 0
        for (int r = 0; r < kFxMaxRibbons; ++r)
            s.lanes[r] = r < L->view.ribbonCount ? static_cast<int16_t>(particleEngine().allocLane()) : static_cast<int16_t>(-1);
    }
    // This frame's instances.
    ParticleEngine& eng = particleEngine();
    list_.clear();
    int used = 0;
    for (int i = 0; i < cfg_.maxInstances; ++i) used += slots_[i].used ? 1 : 0;
    const int share = eng.weaponShare();
    const int perInstance = used ? std::max(1, share / used) : 1;
    for (int i = 0; i < cfg_.maxInstances; ++i) {
        Slot& s = slots_[i];
        if (!s.used) continue;
        s.timeMs += dt * 1000.0f;
        if (s.timeMs > 3.6e6f) s.timeMs = std::fmod(s.timeMs, static_cast<float>(s.fx->view.h->periodMs));
        FxInstance in;
        in.fx = &s.fx->view;
        in.pos = glm::vec3(s.pos[0], s.pos[1], s.pos[2]);
        std::memcpy(in.axes, s.axes, sizeof(in.axes));
        float scale = 0.0f;
        for (int c = 0; c < 3; ++c)
            scale = std::max(scale, std::sqrt(s.axes[c * 3] * s.axes[c * 3] + s.axes[c * 3 + 1] * s.axes[c * 3 + 1] +
                                              s.axes[c * 3 + 2] * s.axes[c * 3 + 2]));
        in.scale = std::clamp(scale, 0.01f, 10.0f);
        in.timeMs = s.timeMs;
        in.acc = s.acc;
        for (int r = 0; r < kFxMaxRibbons; ++r) in.lanes[r] = s.lanes[r];
        in.owner = s.owner;
        in.mesh = true;
        in.rate = 1.0f;
        in.cap = share > 0 ? std::max(1, perInstance / std::max(1, static_cast<int>(s.fx->view.h->emitterCount))) : 0;
        in.premultiply = true;  // upstream m2_particle.frag: colour x alpha (as the world's emitters)
        in.tex = &WeaponGlows::texFn;
        in.texUser = this;
        list_.push_back(in);
    }
    FxFrameStats st;
    const int n = static_cast<int>(list_.size());
    if (n > 0 && share > 0) {
        eng.step(list_.data(), n, kWeaponOwnerBase, kWeaponOwnerEnd, share, dt, st);
        PROF_GE_PASS("weaponfx");
        eng.draw(list_.data(), n, kWeaponOwnerBase, kWeaponOwnerEnd, cam, cfg_.maxPx, st);
        PROF_GE_PASS("char");  // what follows is the world's particles / a character render again
    }
    // What the GE may still read stays cached two more frames.
    for (int i = 0; i < cfg_.maxInstances; ++i) {
        if (!slots_[i].used) continue;
        Loaded* L = slots_[i].fx;
        L->lastFrame = frame_;
        for (uint32_t t = 0; t < L->view.h->textureCount; ++t) {
            auto it = textures_.find(L->view.textures[t]);
            if (it != textures_.end()) it->second->lastFrame = frame_;
        }
    }
    last_ = st;
    lastInstances_ = static_cast<uint32_t>(n);
    lastRequests_ = static_cast<uint32_t>(requests.size());
    peakLive_ = std::max(peakLive_, st.live);
    if (n > 0) lastActiveMs_ = nowMs;
    const uint32_t us = sceKernelGetSystemTimeLow() - t0;
    usSum_ += us;
    usMax_ = std::max(usMax_, us);
    ++usFrames_;
    log(nowMs);
}

void WeaponGlows::log(uint32_t nowMs) {
    if (nowMs < nextLogMs_) return;
    nextLogMs_ = nowMs + 10000u;
    if (nowMs - lastActiveMs_ > 10000u && logs_ > 0) return;  // quiet while no glow is near
    ++logs_;
    const uint32_t avg = usFrames_ ? usSum_ / usFrames_ : 0u;
    PSP_LOGI(pp::tag::Gfx,
             "weaponfx_frame inst=%u req=%u live=%u peak=%u sprites=%u draws=%u mesh=%u ribbon_verts=%u list_full=%u "
             "kb=%u effects=%u textures=%u loads=%u fails=%u evict=%u us_avg=%u us_max=%u frames=%u",
             static_cast<unsigned>(lastInstances_), static_cast<unsigned>(lastRequests_),
             static_cast<unsigned>(last_.live), static_cast<unsigned>(peakLive_), static_cast<unsigned>(last_.sprites),
             static_cast<unsigned>(last_.draws), static_cast<unsigned>(last_.meshDraws),
             static_cast<unsigned>(last_.ribbonVerts), static_cast<unsigned>(last_.listFull),
             static_cast<unsigned>(cacheBytes_ / 1024u), static_cast<unsigned>(loaded_.size()),
             static_cast<unsigned>(textures_.size()), static_cast<unsigned>(loads_), static_cast<unsigned>(fails_),
             static_cast<unsigned>(evicts_), static_cast<unsigned>(avg), static_cast<unsigned>(usMax_),
             static_cast<unsigned>(usFrames_));
    usSum_ = usMax_ = usFrames_ = 0;
    peakLive_ = 0;
}

} // namespace wowee::rendering::psp
