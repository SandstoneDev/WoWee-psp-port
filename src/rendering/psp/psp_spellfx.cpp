// Spell effects - see include/rendering/psp/psp_spellfx.hpp.
// Upstream: src/rendering/spell_visual_system.cpp (what / where / how
// long), src/rendering/m2_renderer_particles.cpp (emitParticles, updateParticles,
// renderM2Particles), m2_renderer_render.cpp (a spell effect's batches).
// the particles, meshes and ribbons moved to the shared engine
// (src/rendering/psp/psp_particles.cpp, generalised from this file).
#include "rendering/psp/psp_spellfx.hpp"

#include "platform/psp/psp_config.hpp"  // spellfxpremul
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_system.hpp"
#include "platform/psp/psp_texture.hpp"
#include "rendering/psp/psp_character.hpp"  // CharacterModel::attachmentPosition (defined here)

#include <pspgu.h>
#include <pspgum.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

namespace wowee::rendering::psp {

namespace pp = wowee::platform::psp;
namespace prof = wowee::platform::psp::prof;

namespace {

constexpr uint32_t kCloseAfterMs = 3000;     // the card's open-file limit (as spells.pak)
constexpr float kMaxDuration = 5.0f;         // upstream SPELL_VISUAL_MAX_DURATION
constexpr float kDefaultDuration = 2.0f;     // upstream SPELL_VISUAL_DEFAULT_DURATION
// upstream SpellVisualSystem::applyEffectHeightOffset (the same words as the attachment).
glm::vec3 heightOffset(const glm::vec3& feet, uint8_t attach) {
    switch (attach) {
        case kFxAttachHand: return feet + glm::vec3(0.0f, 0.0f, 0.8f);
        case kFxAttachChest: return feet + glm::vec3(0.0f, 0.0f, 1.0f);
        case kFxAttachHead: return feet + glm::vec3(0.0f, 0.0f, 1.6f);
        default: return feet;
    }
}

}  // namespace

struct SpellFxRenderer::Loaded {
    void* blob = nullptr;
    uint32_t bytes = 0;
    const uint8_t* base = nullptr;
    const SpellFxHeader* h = nullptr;
    const SpellFxTrack* tracks = nullptr;
    const SpellFxGroup* groups = nullptr;
    const SpellFxEmitter* emitters = nullptr;
    const uint16_t* textures = nullptr;
    const SpellFxVertex* verts = nullptr;
    FxView view;  // the engine's view of the blob (v1 or v2)
    uint32_t lastFrame = 0;
    int users = 0;  // active instances
    ~Loaded() { std::free(blob); }
};

struct SpellFxRenderer::TexEntry {
    pp::Texture tex;
    uint32_t bytes = 0;
    uint32_t lastFrame = 0;
    int refs = 0;        // loaded effects using it
    uint32_t triedFrame = 0;
    bool ok = false;
    bool broken = false;  // read / parse failed: not retried (a full cache is retried while drawn)
    ~TexEntry() {
        if (ok) pp::textureRelease(tex);
    }
};

SpellFxRenderer::SpellFxRenderer() = default;

SpellFxRenderer::~SpellFxRenderer() {
    reset();  // the instances' particles and ribbon lanes back to the engine
    active_.clear();
    loaded_.clear();
    textures_.clear();
    if (fd_ >= 0) {
        pp::pakNoteClose(fd_);
        sceIoClose(fd_);
    }
    if (engine_) particleEngine().releaseShared();
}

bool SpellFxRenderer::open(const SpellFxConfig& cfg) {
    cfg_ = cfg;
    cfg_.maxActive = std::clamp(cfg_.maxActive, 1, 8);
    cfg_.maxSprites = std::clamp(cfg_.maxSprites, 0, 256);
    cfg_.maxPx = std::clamp(cfg_.maxPx, 4, 256);
    cfg_.cacheBytes = std::clamp<uint32_t>(cfg_.cacheBytes, 64u * 1024u, 4096u * 1024u);
    char path[256];
    pp::fsPath(path, sizeof(path), "data/spellfx.pak");
    path_ = path;
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    pp::Pak pak;
    if (!pak.open(path)) {
        PSP_LOGW(pp::tag::Fs, "spellfx_pak missing (psp_spellfx_conv, bake data/spellfx.pak): effects off");
        return false;
    }
    const pp::PakEntry* ev = pak.find("vis");
    void* vb = ev ? pak.load(ev) : nullptr;
    SpellFxVisualTable vt{};
    if (vb && ev->size >= sizeof(vt)) std::memcpy(&vt, vb, sizeof(vt));
    if (!vb || std::memcmp(vt.magic, "WSFV", 4) != 0 || vt.version != kSpellFxVersion ||
        ev->size < sizeof(vt) + vt.count * sizeof(SpellFxVisual) || vt.effectCount > 0xFFFE ||
        vt.textureCount > 0xFFFE) {
        pp::pakFree(vb);
        PSP_LOGE(pp::tag::Fs, "spellfx_pak bad vis (rebake)");
        return false;
    }
    visuals_.resize(vt.count);
    std::memcpy(visuals_.data(), static_cast<const uint8_t*>(vb) + sizeof(vt), vt.count * sizeof(SpellFxVisual));
    pp::pakFree(vb);
    char nm[16];
    effOff_.assign(vt.effectCount, 0);
    effSize_.assign(vt.effectCount, 0);
    for (uint32_t n = 0; n < vt.effectCount; ++n) {
        std::snprintf(nm, sizeof(nm), "e_%u", static_cast<unsigned>(n));
        if (const pp::PakEntry* e = pak.find(nm)) {
            effOff_[n] = e->offset;
            effSize_[n] = e->size;
        }
    }
    texOff_.assign(vt.textureCount, 0);
    texSize_.assign(vt.textureCount, 0);
    for (uint32_t n = 0; n < vt.textureCount; ++n) {
        std::snprintf(nm, sizeof(nm), "x_%u", static_cast<unsigned>(n));
        if (const pp::PakEntry* e = pak.find(nm)) {
            texOff_[n] = e->offset;
            texSize_[n] = e->size;
        }
    }
    pak.close();
    // the particles live in the shared engine's pool (this user's share = spellfxsprites).
    if (!particleEngine().acquireShared()) {
        PSP_LOGE(pp::tag::Gfx, "spellfx_off (no memory for the particle pool)");
        visuals_.clear();
        return false;
    }
    engine_ = true;
    cfg_.maxSprites = particleEngine().spellShare();
    cfg_.premultiply = pp::configGetInt("spellfxpremul", 0) != 0;  // 0 = the straight colour
    active_.reserve(static_cast<size_t>(cfg_.maxActive) + 2u);
    const uint32_t indexKb = static_cast<uint32_t>(
        (visuals_.size() * sizeof(SpellFxVisual) + (effOff_.size() + texOff_.size()) * 8u + 1023u) / 1024u);
    PSP_LOGI(pp::tag::Gfx,
             "spellfx on visuals=%u effects=%u textures=%u index_kb=%u cache_kb=%u max=%d sprites=%d px=%d ms=%u",
             static_cast<unsigned>(vt.count), static_cast<unsigned>(vt.effectCount),
             static_cast<unsigned>(vt.textureCount), static_cast<unsigned>(indexKb),
             static_cast<unsigned>(cfg_.cacheBytes / 1024u), cfg_.maxActive, cfg_.maxSprites, cfg_.maxPx,
             static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u));
    pp::logMemory("spellfx_open");
    return true;
}

const SpellFxVisual* SpellFxRenderer::findVisual(uint32_t visualId) const {
    auto it = std::lower_bound(visuals_.begin(), visuals_.end(), visualId,
                               [](const SpellFxVisual& v, uint32_t id) { return v.visual < id; });
    return (it != visuals_.end() && it->visual == visualId) ? &*it : nullptr;
}

bool SpellFxRenderer::readRaw(uint32_t offset, void* dst, uint32_t size) {
    if (fd_ < 0) {
        fd_ = pp::pakOpenRetry(path_.c_str());  // + one retry after a rare pak gave one back
        if (fd_ < 0) {
            PSP_LOGE(pp::tag::Fs, "spellfx_pak open err=%08X", static_cast<unsigned>(fd_));
            fd_ = -1;
            return false;
        }
        pp::pakNoteOpen(fd_, path_.c_str());  // in the descriptor budget
    }
    lastReadMs_ = pp::logUptimeMs();
    return pp::pakRead(fd_, offset, dst, size);
}

void SpellFxRenderer::closeIdle(uint32_t nowMs) {
    if (fd_ >= 0 && nowMs - lastReadMs_ > kCloseAfterMs) {
        pp::pakNoteClose(fd_);
        sceIoClose(fd_);
        fd_ = -1;
    }
}

// LRU: unreferenced textures first, then effects no instance uses; nothing the GE may still
// read (drawn in the last 2 frames) goes.
bool SpellFxRenderer::makeRoom(uint32_t need) {
    while (cacheBytes_ + need > cfg_.cacheBytes) {
        auto tv = textures_.end();
        for (auto it = textures_.begin(); it != textures_.end(); ++it)
            if (it->second->refs == 0 && frame_ - it->second->lastFrame > 2 &&
                (tv == textures_.end() || it->second->lastFrame < tv->second->lastFrame))
                tv = it;
        if (tv != textures_.end()) {
            cacheBytes_ -= tv->second->bytes;
            textures_.erase(tv);
            ++evictCache_;
            continue;
        }
        auto ev = loaded_.end();
        for (auto it = loaded_.begin(); it != loaded_.end(); ++it)
            if (it->second->users == 0 && frame_ - it->second->lastFrame > 2 &&
                (ev == loaded_.end() || it->second->lastFrame < ev->second->lastFrame))
                ev = it;
        if (ev == loaded_.end()) return false;
        const Loaded& L = *ev->second;
        for (uint32_t i = 0; i < L.h->textureCount; ++i) {
            auto t = textures_.find(L.textures[i]);
            if (t != textures_.end() && t->second->refs > 0) --t->second->refs;
        }
        cacheBytes_ -= L.bytes;
        loaded_.erase(ev);
        ++evictCache_;
    }
    return true;
}

SpellFxRenderer::Loaded* SpellFxRenderer::load(uint16_t effect, uint32_t visualId, const char* kind) {
    auto found = loaded_.find(effect);
    if (found != loaded_.end()) {
        found->second->lastFrame = frame_;
        return found->second.get();
    }
    if (effect >= effOff_.size() || effSize_[effect] < sizeof(SpellFxHeader)) return nullptr;
    PROF_SCOPE("spellfx.load");
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    const uint32_t size = effSize_[effect];
    if (!makeRoom(size)) {
        if (++full_ <= 20)
            PSP_LOGW(pp::tag::Gfx, "spellfx_full visual=%u effect=%u need_kb=%u cache_kb=%u", static_cast<unsigned>(visualId),
                     static_cast<unsigned>(effect), static_cast<unsigned>((size + 1023u) / 1024u),
                     static_cast<unsigned>(cacheBytes_ / 1024u));
        return nullptr;
    }
    void* blob = memalign(64, (size + 63u) & ~63u);
    if (!blob || !readRaw(effOff_[effect], blob, size) || !spellFxValid(blob, size)) {
        std::free(blob);
        PSP_LOGW(pp::tag::Gfx, "spellfx_bad effect=%u (rebake spellfx.pak)", static_cast<unsigned>(effect));
        return nullptr;
    }
    sceKernelDcacheWritebackRange(blob, size);
    auto L = std::make_unique<Loaded>();
    L->blob = blob;
    L->bytes = size;
    L->base = static_cast<const uint8_t*>(blob);
    L->h = reinterpret_cast<const SpellFxHeader*>(L->base);
    L->tracks = reinterpret_cast<const SpellFxTrack*>(L->base + L->h->trackOffset);
    L->groups = reinterpret_cast<const SpellFxGroup*>(L->base + L->h->groupOffset);
    L->emitters = reinterpret_cast<const SpellFxEmitter*>(L->base + L->h->emitterOffset);
    L->textures = reinterpret_cast<const uint16_t*>(L->base + L->h->textureOffset);
    L->verts = reinterpret_cast<const SpellFxVertex*>(L->base + L->h->vertexOffset);
    L->view.set(blob);
    L->lastFrame = frame_;
    cacheBytes_ += size;
    uint32_t texRead = 0, texKb = 0;
    for (uint32_t i = 0; i < L->h->textureCount; ++i) {
        const uint16_t id = L->textures[i];
        auto it = textures_.find(id);
        if (it == textures_.end()) {
            auto te = std::make_unique<TexEntry>();
            te->lastFrame = frame_;
            if (readTexture(id, *te)) {
                ++texRead;
                texKb += te->bytes;
            }
            it = textures_.emplace(id, std::move(te)).first;
        }
        ++it->second->refs;
    }
    Loaded* out = L.get();
    loaded_.emplace(effect, std::move(L));
    ++loads_;
    const uint32_t ms = static_cast<uint32_t>((sceKernelGetSystemTimeWide() - t0) / 1000u);
    if (loads_ <= 200)
        PSP_LOGI(pp::tag::Gfx, "spellfx_load visual=%u effect=%u kind=%s kb=%u tex=%u tex_kb=%u groups=%u emitters=%u "
                 "ms=%u cache_kb=%u",
                 static_cast<unsigned>(visualId), static_cast<unsigned>(effect), kind,
                 static_cast<unsigned>((size + 1023u) / 1024u), static_cast<unsigned>(texRead),
                 static_cast<unsigned>((texKb + 1023u) / 1024u), static_cast<unsigned>(out->h->groupCount),
                 static_cast<unsigned>(out->h->emitterCount), static_cast<unsigned>(ms),
                 static_cast<unsigned>(cacheBytes_ / 1024u));
    if (++memLogs_ <= 3) pp::logMemory("spellfx_load");
    return out;
}

// One texture into the cache (64-aligned, D-cache written back for the GE).
bool SpellFxRenderer::readTexture(uint16_t id, TexEntry& te) {
    te.triedFrame = frame_;
    if (id >= texOff_.size() || texSize_[id] < 32) {
        te.broken = true;
        return false;
    }
    const uint32_t ts = texSize_[id];
    if (!makeRoom(ts)) return false;  // retried when drawn
    void* tb = memalign(64, (ts + 63u) & ~63u);
    if (!tb || !readRaw(texOff_[id], tb, ts)) {
        std::free(tb);
        te.broken = true;
        return false;
    }
    sceKernelDcacheWritebackRange(tb, ts);
    if (!pp::textureFromBlob(tb, ts, te.tex, true)) {
        std::free(tb);
        te.broken = true;
        return false;
    }
    te.ok = true;
    te.bytes = ts;
    cacheBytes_ += ts;
    return true;
}

const pp::Texture* SpellFxRenderer::texture(uint16_t id) {
    auto it = textures_.find(id);
    if (it == textures_.end()) return nullptr;
    TexEntry& te = *it->second;
    te.lastFrame = frame_;
    if (!te.ok && !te.broken && te.triedFrame != frame_) readTexture(id, te);  // the cache was full at load
    return te.ok ? &te.tex : nullptr;
}

void SpellFxRenderer::playPrecast(uint32_t visualId, const glm::vec3& feet, uint32_t castMs, uint64_t caster, bool own,
                                  SpellFxHost& host) {
    if (!isOpen() || visualId == 0) return;
    const SpellFxVisual* v = findVisual(visualId);
    if (!v) {
        if (missingLogged_.size() < 64 && missingLogged_.insert(visualId).second)
            PSP_LOGI(pp::tag::Gfx, "spellfx_missing visual=%u (not baked)", static_cast<unsigned>(visualId));
        return;
    }
    // upstream: no precast kit (or its model failed) -> the cast kit.
    Loaded* L = v->precast != kFxNone ? load(v->precast, visualId, "precast") : nullptr;
    if (!L) {
        play(visualId, feet, false, caster, own, host);
        return;
    }
    uint8_t attach = caster ? L->h->attach : 0;
    glm::vec3 pos;
    if (!(attach && host.attachmentWorld(caster, attach, pos))) {
        pos = heightOffset(feet, L->h->attach);
        attach = 0;
    }
    // upstream: the server cast time when known (Hearthstone 10 s), else the model's duration.
    float duration;
    if (castMs >= 500) {
        duration = std::clamp(static_cast<float>(castMs) / 1000.0f, 0.5f, 30.0f);
    } else {
        const float d = static_cast<float>(L->h->durationMs);
        duration = d > 100.0f ? std::clamp(d / 1000.0f, 0.5f, kMaxDuration) : kDefaultDuration;
    }
    spawn(v->precast, visualId, pos, duration, true, own, attach, caster);
    if (attach == kFxAttachHand) {  // upstream: a mirror copy on the left hand
        glm::vec3 left = feet;
        host.attachmentWorld(caster, kFxAttachLeftHand, left);
        spawn(v->precast, visualId, left, duration, true, own, kFxAttachLeftHand, caster);
    }
}

void SpellFxRenderer::play(uint32_t visualId, const glm::vec3& feet, bool impact, uint64_t caster, bool own,
                           SpellFxHost& host) {
    if (!isOpen() || visualId == 0) return;
    const SpellFxVisual* v = findVisual(visualId);
    if (!v) {
        if (missingLogged_.size() < 64 && missingLogged_.insert(visualId).second)
            PSP_LOGI(pp::tag::Gfx, "spellfx_missing visual=%u (not baked)", static_cast<unsigned>(visualId));
        return;
    }
    // upstream: the cast or impact path, falling back to the other.
    uint16_t effect = impact ? v->impact : v->cast;
    if (effect == kFxNone) effect = impact ? v->cast : v->impact;
    if (effect == kFxNone) return;
    Loaded* L = load(effect, visualId, impact ? "impact" : "cast");
    if (!L) return;
    // Only a cast kit follows the caster's bones (upstream: impacts stay where they land).
    uint8_t attach = (!impact && caster) ? L->h->attach : 0;
    glm::vec3 pos;
    if (!(attach && host.attachmentWorld(caster, attach, pos))) {
        pos = heightOffset(feet, L->h->attach);
        attach = 0;
    }
    const float d = static_cast<float>(L->h->durationMs);
    const float duration = d > 100.0f ? std::clamp(d / 1000.0f, 0.5f, kMaxDuration) : kDefaultDuration;
    spawn(effect, visualId, pos, duration, false, own, attach, caster);
    if (attach == kFxAttachHand) {
        glm::vec3 left = feet;
        host.attachmentWorld(caster, kFxAttachLeftHand, left);
        spawn(effect, visualId, left, duration, false, own, kFxAttachLeftHand, caster);
    }
}

void SpellFxRenderer::spawn(uint16_t effect, uint32_t visualId, const glm::vec3& pos, float duration, bool precast,
                            bool own, uint8_t attach, uint64_t caster) {
    auto lit = loaded_.find(effect);
    if (lit == loaded_.end()) return;
    // PSP: at most maxActive effects - another caster's first, then the one nearest its end.
    if (active_.size() >= static_cast<size_t>(cfg_.maxActive)) {
        size_t victim = 0;
        float best = -1.0f;
        for (size_t i = 0; i < active_.size(); ++i) {
            const Instance& a = active_[i];
            const float score = (a.own ? 0.0f : 2.0f) + a.elapsed / std::max(a.duration, 0.01f);
            if (score > best) {
                best = score;
                victim = i;
            }
        }
        removeAt(victim);
        ++evictFx_;
    }
    Instance in;
    in.effect = effect;
    in.visual = visualId;
    in.duration = duration;
    in.precast = precast;
    in.own = own;
    in.attach = attach;
    in.caster = caster;
    in.pos = pos;
    in.serial = ++serial_;
    // a lane per ribbon (none left: that ribbon is not drawn).
    for (int r = 0; r < kFxMaxRibbons && r < lit->second->view.ribbonCount; ++r) in.lanes[r] = static_cast<int16_t>(particleEngine().allocLane());
    active_.push_back(in);
    ++lit->second->users;
    lit->second->lastFrame = frame_;
    lastActiveMs_ = pp::logUptimeMs();
}

void SpellFxRenderer::removeAt(size_t i) {
    if (i >= active_.size()) return;
    const Instance& in = active_[i];
    auto lit = loaded_.find(in.effect);
    if (lit != loaded_.end() && lit->second->users > 0) {
        --lit->second->users;
        lit->second->lastFrame = frame_;  // its blob may be in this frame's display list
    }
    // upstream removeInstance: the instance's particles go with it (in the shared pool,
    // owner = kSpellOwnerBase + index; the owners above it move down with the erase).
    ParticleEngine& eng = particleEngine();
    eng.freeOwner(static_cast<uint8_t>(kSpellOwnerBase + i));
    eng.shiftOwnersDown(static_cast<uint8_t>(kSpellOwnerBase + i), kSpellOwnerEnd);
    for (int16_t l : in.lanes) eng.freeLane(l);
    active_.erase(active_.begin() + static_cast<std::ptrdiff_t>(i));
}

void SpellFxRenderer::prefetch(uint32_t visualId) {
    const SpellFxVisual* v = isOpen() ? findVisual(visualId) : nullptr;
    if (!v) return;
    for (uint16_t e : {v->impact, v->cast, v->precast})
        if (e != kFxNone && !loaded_.count(e) && std::find(prefetch_.begin(), prefetch_.end(), e) == prefetch_.end())
            prefetch_.push_back(e);
}

void SpellFxRenderer::cancelAllPrecast() {
    for (size_t i = active_.size(); i-- > 0;)
        if (active_[i].precast) removeAt(i);
}

void SpellFxRenderer::reset() {
    while (!active_.empty()) removeAt(active_.size() - 1);
}

void SpellFxRenderer::frame(const TerrainCamera& cam, float dtSec, SpellFxHost& host) {
    ++frame_;
    const uint32_t nowMs = pp::logUptimeMs();
    // Read-ahead: one effect per frame, only into the free first 3/4 of the cache.
    if (!prefetch_.empty()) {
        const uint16_t e = prefetch_.back();
        prefetch_.pop_back();
        const uint32_t size = e < effSize_.size() ? effSize_[e] : 0u;
        if (cacheBytes_ + size > cfg_.cacheBytes / 4u * 3u) {
            prefetch_.clear();
        } else if (!loaded_.count(e) && load(e, 0, "prefetch")) {
            ++prefetched_;
        }
    }
    closeIdle(nowMs);
    if (active_.empty()) {
        log(nowMs);
        return;
    }
    PROF_SCOPE("spellfx");
    const uint32_t t0 = sceKernelGetSystemTimeLow();
    const float dt = std::clamp(dtSec, 0.0f, 0.25f);
    // Lifetimes (upstream SpellVisualSystem::update).
    for (size_t i = active_.size(); i-- > 0;) {
        active_[i].elapsed += dt;
        if (active_[i].elapsed >= active_[i].duration) removeAt(i);
    }
    if (active_.empty()) {
        log(nowMs);
        return;
    }
    // Followed bones (the caster's hands / chest / head this frame).
    for (auto& in : active_)
        if (in.attach && in.caster) host.attachmentWorld(in.caster, in.attach, in.pos);

    // the shared engine - emission (the share split evenly between the effects and
    // their emitters, as before), motion, ribbons, then the GE pass (meshes, sprites, ribbons).
    const int n = std::min(static_cast<int>(active_.size()), static_cast<int>(kSpellOwnerEnd - kSpellOwnerBase));
    FxInstance list[kSpellOwnerEnd - kSpellOwnerBase];
    const int shareEffect = cfg_.maxSprites / std::max(1, n);
    for (int i = 0; i < n; ++i) {
        Instance& in = active_[static_cast<size_t>(i)];
        Loaded* L = loaded_[in.effect].get();
        L->lastFrame = frame_;
        FxInstance& f = list[i];
        f = FxInstance{};
        f.fx = &L->view;
        f.pos = in.pos;  // spell effects: no rotation or scale (upstream: the instance at a point)
        f.timeMs = in.elapsed * 1000.0f;
        f.acc = in.acc;
        for (int r = 0; r < kFxMaxRibbons; ++r) f.lanes[r] = in.lanes[r];
        f.owner = static_cast<uint8_t>(kSpellOwnerBase + i);
        // upstream: a particle-dominant effect draws no mesh (the converter baked none).
        f.mesh = true;
        f.rate = 1.0f;
        f.cap = L->h->emitterCount ? std::max(1, shareEffect / L->h->emitterCount) : 1;
        if (shareEffect <= 0) f.cap = 0;
        f.premultiply = cfg_.premultiply;  // Straight colour
        f.tex = &SpellFxRenderer::texFn;
        f.texUser = this;
    }
    FxFrameStats st;
    ParticleEngine& eng = particleEngine();
    eng.step(list, n, kSpellOwnerBase, kSpellOwnerEnd, eng.spellShare(), dt, st);
    PROF_GE_PASS("spellfx");
    eng.draw(list, n, kSpellOwnerBase, kSpellOwnerEnd, cam, cfg_.maxPx, st);
    PROF_GE_PASS("char");  // what follows (the target portrait) is a character render again
    spritesLast_ = st.live;
    spritesPeak_ = std::max(spritesPeak_, st.live);
    draws_ = st.draws;
    const uint32_t us = sceKernelGetSystemTimeLow() - t0;
    usSum_ += us;
    usMax_ = std::max(usMax_, us);
    ++usFrames_;
    lastActiveMs_ = nowMs;
    log(nowMs);
}

const pp::Texture* SpellFxRenderer::texFn(void* user, uint16_t id) {
    return static_cast<SpellFxRenderer*>(user)->texture(id);
}

void SpellFxRenderer::log(uint32_t nowMs) {
    if (nowMs < nextLogMs_) return;
    // Only while effects run (and once more after the last one ended).
    if (active_.empty() && nowMs - lastActiveMs_ > 10000u) return;
    nextLogMs_ = nowMs + 10000u;
    const uint32_t avg = usFrames_ ? usSum_ / usFrames_ : 0u;
    PSP_LOGI(pp::tag::Gfx,
             "spellfx_frame active=%u sprites=%u peak=%u kb=%u effects=%u textures=%u loads=%u evict=%u evict_cache=%u "
             "full=%u draws=%u us_avg=%u us_max=%u frames=%u file=%d prefetched=%u",
             static_cast<unsigned>(active_.size()), static_cast<unsigned>(spritesLast_),
             static_cast<unsigned>(spritesPeak_), static_cast<unsigned>(cacheBytes_ / 1024u),
             static_cast<unsigned>(loaded_.size()), static_cast<unsigned>(textures_.size()),
             static_cast<unsigned>(loads_), static_cast<unsigned>(evictFx_), static_cast<unsigned>(evictCache_),
             static_cast<unsigned>(full_), static_cast<unsigned>(draws_), static_cast<unsigned>(avg),
             static_cast<unsigned>(usMax_), static_cast<unsigned>(usFrames_), fd_ >= 0 ? 1 : 0,
             static_cast<unsigned>(prefetched_));
    usSum_ = usMax_ = usFrames_ = 0;
    spritesPeak_ = 0;
}


// CharacterModel (psp_character.hpp): the point's bone on this frame's pose, as
// CharacterAttachments::draw places an item (bone * T(position)), without the model matrix.
bool CharacterModel::attachmentPosition(uint16_t id, float out[3]) const {
    for (const AttachPoint& p : attachPoints_) {
        if (p.id != id) continue;
        const float* m = (skinned_ && p.bone != kNoAttachBone && p.bone < pose_.size()) ? pose_[p.bone].m : p.stand;
        for (int r = 0; r < 3; ++r)
            out[r] = m[9 + r] + m[r] * p.position[0] + m[3 + r] * p.position[1] + m[6 + r] * p.position[2];
        return true;
    }
    return false;
}

} // namespace wowee::rendering::psp
