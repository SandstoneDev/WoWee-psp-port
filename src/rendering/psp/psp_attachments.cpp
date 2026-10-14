// Item models on character attachment points. See
// include/rendering/psp/psp_attachments.hpp.
#include "rendering/psp/psp_attachments.hpp"

#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_display.hpp"  // displayListFree
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_texture.hpp"

#include <pspgu.h>
#include <pspgum.h>
#include <pspkernel.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace wowee::rendering::psp {

namespace pp = wowee::platform::psp;
namespace prof = wowee::platform::psp::prof;

namespace {

constexpr int kVertexType = GU_INDEX_16BIT | GU_TEXTURE_16BIT | GU_COLOR_5650 | GU_VERTEX_16BIT | GU_TRANSFORM_3D;
constexpr int kStripType = GU_TEXTURE_16BIT | GU_COLOR_5650 | GU_VERTEX_16BIT | GU_TRANSFORM_3D;

int g_enabled = -1;         // -1: not read from wowee.cfg yet
uint32_t g_budgetBytes = 0;
uint32_t g_capBytes = 0;    // wowee.cfg attachcap
uint32_t g_govCapBytes = 0; // the memory governor's lower cap (0 = none)
// attachcap, or the governor's when it is lower.
uint32_t effectiveCap() { return g_govCapBytes && g_govCapBytes < g_capBytes ? g_govCapBytes : g_capBytes; }
uint32_t g_capSkips = 0;
bool g_attachFx = true;     // wowee.cfg attachfx

void readConfig() {
    if (g_enabled >= 0) return;
    g_enabled = pp::configGetInt("attachments", 1) != 0 ? 1 : 0;
    // the additive / modulate batches of an attached item (glow cards of weapons,
    // shoulder shine layers - 17 % of the item models). The client draws them (1, as before); upstream
    // CharacterRenderer drops them on every attachment ("detached flat quads for some swords") - attachfx=0 = upstream.
    g_attachFx = pp::configGetInt("attachfx", 1) != 0;
    int kb = pp::configGetInt("attachmax", 768);
    if (kb < 64) kb = 64;
    g_budgetBytes = static_cast<uint32_t>(kb) * 1024u;
    // every player is dressed from the DBCs now - a crowd in different gear holds
    // ~0.2 MB of item models and skins a player. Models of other characters are not loaded while what is in use
    // passes this (the own player's always are).
    int cap = pp::configGetInt("attachcap", 1536);
    if (cap < kb) cap = kb;
    g_capBytes = static_cast<uint32_t>(cap) * 1024u;
}

// ---- shared cache (every character) ----------------------------------------
struct CachedModel {
    void* blob = nullptr;
    uint32_t bytes = 0;
    int refs = 0;
    uint32_t lastUse = 0;
};
struct CachedTexture {
    pp::Texture tex;
    uint32_t bytes = 0;
    int refs = 0;
    uint32_t lastUse = 0;
};
struct Cache {
    // Node-based maps: element addresses stay valid while other entries come and go.
    std::unordered_map<uint16_t, CachedModel> models;
    std::unordered_map<uint16_t, CachedTexture> textures;
    std::unordered_set<uint16_t> missingModels, missingTextures;
    ItemBank* bank = nullptr;
    uint32_t bytes = 0;
    uint32_t tick = 0;
};
// Never destroyed: characters released at exit must not outlive it.
Cache& cache() {
    static Cache* c = new Cache;
    return *c;
}
int g_logged = 0;

// Least recently used unreferenced entries go until `need` more bytes fit.
void makeRoom(uint32_t need) {
    while (cache().bytes + need > g_budgetBytes) {
        CachedModel* om = nullptr;
        uint16_t omId = 0;
        CachedTexture* ot = nullptr;
        uint16_t otId = 0;
        for (auto& [id, m] : cache().models)
            if (m.refs == 0 && (!om || m.lastUse < om->lastUse)) {
                om = &m;
                omId = id;
            }
        for (auto& [id, t] : cache().textures)
            if (t.refs == 0 && (!ot || t.lastUse < ot->lastUse)) {
                ot = &t;
                otId = id;
            }
        if (!om && !ot) return;  // everything in use: over budget until released
        if (om && (!ot || om->lastUse <= ot->lastUse)) {
            cache().bytes -= std::min(cache().bytes, om->bytes);
            pp::pakFree(om->blob);
            cache().models.erase(omId);
        } else {
            cache().bytes -= std::min(cache().bytes, ot->bytes);
            pp::textureRelease(ot->tex);
            cache().textures.erase(otId);
        }
    }
}

void dropUnreferenced();

// A different ItemBank (items.pak reopened) means different ids: forget
// whatever nobody holds.
void bindBank(ItemBank& items) {
    if (cache().bank == &items) return;
    dropUnreferenced();
    cache().bank = &items;
}

// Every model and texture nobody holds goes (bindBank, attachmentCacheTrim).
void dropUnreferenced() {
    for (auto it = cache().models.begin(); it != cache().models.end();) {
        if (it->second.refs) {
            ++it;
            continue;
        }
        cache().bytes -= std::min(cache().bytes, it->second.bytes);
        pp::pakFree(it->second.blob);
        it = cache().models.erase(it);
    }
    for (auto it = cache().textures.begin(); it != cache().textures.end();) {
        if (it->second.refs) {
            ++it;
            continue;
        }
        cache().bytes -= std::min(cache().bytes, it->second.bytes);
        pp::textureRelease(it->second.tex);
        it = cache().textures.erase(it);
    }
    cache().missingModels.clear();
    cache().missingTextures.clear();
}

CachedModel* acquireModel(ItemBank& items, uint16_t id, bool& loaded) {
    loaded = false;
    if (id == kNoItemModel || cache().missingModels.count(id)) return nullptr;
    auto it = cache().models.find(id);
    if (it == cache().models.end()) {
        uint32_t size = 0;
        void* blob = items.loadModel(id, size);
        if (!blob || !itemModelValid(blob, size)) {
            if (blob) pp::pakFree(blob);
            cache().missingModels.insert(id);
            if (++g_logged <= 60) PSP_LOGW(pp::tag::Gfx, "attach_model_bad model=a_%u", static_cast<unsigned>(id));
            return nullptr;
        }
        makeRoom(size);
        CachedModel m;
        m.blob = blob;
        m.bytes = size;
        it = cache().models.emplace(id, m).first;
        cache().bytes += size;
        loaded = true;
    }
    ++it->second.refs;
    it->second.lastUse = ++cache().tick;
    return &it->second;
}

CachedTexture* acquireTexture(ItemBank& items, uint16_t id) {
    if (id == kNoItemLayer || cache().missingTextures.count(id)) return nullptr;
    auto it = cache().textures.find(id);
    if (it == cache().textures.end()) {
        uint32_t size = 0;
        void* blob = items.loadTexture(id, size);
        CachedTexture t;
        if (!blob || !pp::textureFromBlob(blob, size, t.tex, true)) {
            if (blob) pp::pakFree(blob);
            cache().missingTextures.insert(id);
            return nullptr;
        }
        t.bytes = pp::textureBytes(t.tex);
        makeRoom(t.bytes);
        it = cache().textures.emplace(id, t).first;
        cache().bytes += t.bytes;
    }
    ++it->second.refs;
    it->second.lastUse = ++cache().tick;
    return &it->second;
}

// bytes of what some character holds now (the unreferenced rest makeRoom can drop).
uint32_t inUseBytes() {
    uint32_t b = 0;
    for (const auto& [id, m] : cache().models) b += m.refs ? m.bytes : 0u;
    for (const auto& [id, t] : cache().textures) b += t.refs ? t.bytes : 0u;
    return b;
}

void releaseModel(const void* blob) {
    for (auto& [id, m] : cache().models)
        if (m.blob == blob) {
            if (m.refs > 0) --m.refs;
            return;
        }
}

void releaseTexture(const pp::Texture* tex) {
    for (auto& [id, t] : cache().textures)
        if (&t.tex == tex) {
            if (t.refs > 0) --t.refs;
            return;
        }
}

// attachcap's crowd policy (psp_attachments.hpp AttachDeferScope). May a character
// that is not essential get item model `model` now? Not while a defer scope lives (its models come later, or it gives
// way to a nearer player); a model already in the cache, yes (shared); a new one only under attachcap.
int g_deferDepth = 0;
std::unordered_map<const CharacterAttachments*, uint32_t> g_capSkipped;  // picks the last set() left off
bool capAllows(uint16_t model) {
    if (g_deferDepth > 0) return false;
    if (cache().models.count(model)) return true;
    return inUseBytes() < effectiveCap();  // the governor may lower it
}

// ---- matrices (BoneMatrix layout: 3x4 column-major) ------------------------
void mul(const BoneMatrix& a, const BoneMatrix& b, BoneMatrix& out) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 3; ++r) {
            float v = a.m[r] * b.m[c * 3] + a.m[3 + r] * b.m[c * 3 + 1] + a.m[6 + r] * b.m[c * 3 + 2];
            if (c == 3) v += a.m[9 + r];
            out.m[c * 3 + r] = v;
        }
}

} // namespace

// the frame number the weapons with a glow are stamped with.
namespace {
uint32_t g_attachFrame = 1;
}

uint32_t attachmentFrame() { return g_attachFrame; }
void attachmentNextFrame() { ++g_attachFrame; }

bool attachmentsEnabled() {
    readConfig();
    return g_enabled != 0;
}

void setAttachmentsEnabled(bool on) {
    readConfig();
    g_enabled = on ? 1 : 0;
}

uint32_t attachmentCacheBytes() { return cache().bytes; }

// cloaks through the shared cache (psp_attachments.hpp).
const pp::Texture* acquireItemTexture(ItemBank& items, uint16_t id) {
    readConfig();
    if (!items.isOpen() || id == kNoItemLayer) return nullptr;
    bindBank(items);
    CachedTexture* t = acquireTexture(items, id);
    return t ? &t->tex : nullptr;
}

void releaseItemTexture(const pp::Texture* tex) {
    if (tex) releaseTexture(tex);
}

void attachmentCacheTrim() {
    const uint32_t before = cache().bytes;
    dropUnreferenced();
    if (before) PSP_LOGI(pp::tag::Gfx, "attach_trim kb=%u->%u", static_cast<unsigned>(before / 1024u),
                         static_cast<unsigned>(cache().bytes / 1024u));
}

void CharacterAttachments::release(std::vector<Entry>& entries) {
    for (Entry& e : entries) {
        if (e.model) releaseModel(e.model);
        for (const void* t : e.held) releaseTexture(static_cast<const pp::Texture*>(t));
    }
    entries.clear();
}

void CharacterAttachments::clear() {
    release(entries_);
    g_capSkipped.erase(this);
}

// the crowd policy's handles (psp_attachments.hpp).
AttachDeferScope::AttachDeferScope() { ++g_deferDepth; }
AttachDeferScope::~AttachDeferScope() { --g_deferDepth; }

uint32_t attachCapSkipped(const CharacterAttachments& a) {
    auto it = g_capSkipped.find(&a);
    return it != g_capSkipped.end() ? it->second : 0u;
}

uint32_t attachmentInUseBytes() { return inUseBytes(); }

uint32_t attachmentCapBytes() {
    readConfig();
    return effectiveCap();  // attachcap, or the memory governor's lower one
}

// ---- the memory governor's hold on attachcap (psp_attachments.hpp) ----
void setAttachmentGovernorCap(uint32_t bytes) {
    readConfig();
    g_govCapBytes = bytes;
}

uint32_t attachmentGovernorCap() { return g_govCapBytes; }

void CharacterAttachments::giveWay(const std::vector<AttachPoint>& points, ItemBank& items) {
    const std::vector<AttachPick> picks = lastPicks_;  // set() stores its argument again
    AttachDeferScope defer;
    set(picks, points, items, false);
}
// ---- (end of the memory governor's hold) ----

void CharacterAttachments::set(const std::vector<AttachPick>& picks, const std::vector<AttachPoint>& points,
                               ItemBank& items, bool essential) {
    PROF_SCOPE("char.attach.load");
    readConfig();
    // The old set is released after the new one holds its references: a
    // re-dress keeps what stays worn instead of evicting and re-reading it.
    std::vector<Entry> old;
    old.swap(entries_);
    g_capSkipped.erase(this);  // counted again below
    if (&picks != &lastPicks_) lastPicks_ = picks;  // giveWay() sets them again
    if (!items.isOpen()) {
        release(old);
        return;
    }
    bindBank(items);
    uint32_t capSkipped = 0;
    for (const AttachPick& p : picks) {
        const AttachPoint* point = nullptr;
        for (const AttachPoint& q : points)
            if (q.id == p.point) {
                point = &q;
                break;
            }
        if (!point) {
            // upstream attachWeapon: "no bone found for attachment" - nothing attached.
            if (++g_logged <= 60)
                PSP_LOGW(pp::tag::Gfx, "attach_missing display=%u slot=%u point=%u reason=nopoint",
                         static_cast<unsigned>(p.displayId), static_cast<unsigned>(p.slot),
                         static_cast<unsigned>(p.point));
            continue;
        }
        // another character's model not cached yet, while the models and skins in
        // use already pass wowee.cfg attachcap - left off (PSP: memory; upstream has no limit).
        // the policy is capAllows (+ the defer scope); what it leaves off is counted per holder.
        if (!essential && p.model != kNoItemModel && !capAllows(p.model)) {
            ++capSkipped;
            if (g_deferDepth > 0) continue;  // deferred / giving way: not the cap (no warning)
            ++g_capSkips;
            if (++g_logged <= 60)
                PSP_LOGW(pp::tag::Gfx, "attach_cap display=%u slot=%u model=a_%u in_use_kb=%u cap_kb=%u skips=%u",
                         static_cast<unsigned>(p.displayId), static_cast<unsigned>(p.slot),
                         static_cast<unsigned>(p.model), static_cast<unsigned>(inUseBytes() / 1024u),
                         static_cast<unsigned>(effectiveCap() / 1024u), static_cast<unsigned>(g_capSkips));
            continue;
        }
        const uint64_t t0 = sceKernelGetSystemTimeWide();
        bool loaded = false;
        CachedModel* cm = acquireModel(items, p.model, loaded);
        if (!cm) {
            if (++g_logged <= 60)
                PSP_LOGW(pp::tag::Gfx, "attach_missing display=%u slot=%u model=a_%u", static_cast<unsigned>(p.displayId),
                         static_cast<unsigned>(p.slot), static_cast<unsigned>(p.model));
            continue;
        }
        Entry e;
        e.pick = p;
        e.point = *point;
        e.model = cm->blob;
        // a glowing weapon's visual points (an older items.pak: none - all at the origin, as
        // upstream for a model without the attachment).
        if (p.itemVisual && !itemVisualPoints(cm->blob, cm->bytes, e.points)) e.points = ItemVisualPoints{};
        CachedTexture* skin = acquireTexture(items, p.texture);
        e.skin = skin ? &skin->tex : nullptr;
        if (skin) e.held.push_back(&skin->tex);
        // Batch textures: the display's skin, a fixed texture of the model, or white.
        ModelHeader h;
        std::memcpy(&h, cm->blob, sizeof(h));
        const uint32_t batches = h.batchCount[0];
        const auto* bt = reinterpret_cast<const ModelBatch*>(static_cast<const uint8_t*>(cm->blob) + h.batchOffset);
        e.batchTextures.assign(batches, nullptr);
        for (uint32_t i = 0; i < batches; ++i) {
            const uint16_t tid = bt[i].texture;
            if (tid == kTexItemSkin) {
                e.batchTextures[i] = e.skin;
            } else if (tid != kNoTexture && tid < kTexSlot) {
                CachedTexture* t = acquireTexture(items, tid);
                e.batchTextures[i] = t ? &t->tex : nullptr;
                if (t) e.held.push_back(&t->tex);
            }
        }
        if (loaded && ++g_logged <= 60)
            PSP_LOGI(pp::tag::Gfx, "attach_load display=%u slot=%u model=a_%u kb=%u ms=%u",
                     static_cast<unsigned>(p.displayId), static_cast<unsigned>(p.slot), static_cast<unsigned>(p.model),
                     static_cast<unsigned>(cm->bytes / 1024u),
                     static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u));
        entries_.push_back(std::move(e));
    }
    release(old);
    if (capSkipped) g_capSkipped[this] = capSkipped;
}

void CharacterAttachments::drawPass(const Entry& e, bool blendPass, bool fade, uint32_t fixSrc, uint32_t fixDst) {
    ModelHeader h;
    std::memcpy(&h, e.model, sizeof(h));
    const auto* blob = static_cast<const uint8_t*>(e.model);
    const auto* verts = reinterpret_cast<const ModelVertex*>(blob + h.vertexOffset);
    const auto* indices = reinterpret_cast<const uint16_t*>(blob + h.indexOffset);
    const auto* batches = reinterpret_cast<const ModelBatch*>(blob + h.batchOffset);
    const bool strips = (h.flags & kModelStrips) != 0;
    for (uint32_t i = 0; i < h.batchCount[0]; ++i) {
        const ModelBatch& b = batches[i];
        if ((b.blend >= kBlendAlpha) != blendPass) continue;
        if (!g_attachFx && (b.blend == kBlendAdd || b.blend == kBlendModulate)) continue;  // A/B
        pp::textureBind(static_cast<const pp::Texture*>(e.batchTextures[i]));
        sceGuTexWrap((b.flags & kBatchWrapU) ? GU_REPEAT : GU_CLAMP, (b.flags & kBatchWrapV) ? GU_REPEAT : GU_CLAMP);
        // Same blend rules as the character's batches (CharacterModel::setBatchState).
        switch (b.blend) {
            case kBlendOpaque:
                sceGuDisable(GU_ALPHA_TEST);
                if (fade) sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, fixSrc, fixDst);
                break;
            case kBlendAlphaKey:
                sceGuEnable(GU_ALPHA_TEST);
                sceGuAlphaFunc(GU_GREATER, 0x66, 0xFF);
                if (fade) sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, fixSrc, fixDst);
                break;
            case kBlendAlpha: sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0); break;
            case kBlendAdd: sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0xFFFFFFFFu); break;
            default: sceGuBlendFunc(GU_ADD, GU_DST_COLOR, GU_FIX, 0, 0); break;
        }
        const uint32_t tris = strips ? b.indexCount - 2u : b.indexCount / 3u;
        prof::count(prof::kDraws, 1);
        prof::count(prof::kVertices, b.indexCount);
        prof::count(prof::kTriangles, tris);
        if (strips)
            sceGumDrawArray(GU_TRIANGLE_STRIP, kStripType, static_cast<int>(b.indexCount), nullptr, verts + b.indexStart);
        else
            sceGumDrawArray(GU_TRIANGLES, kVertexType, static_cast<int>(b.indexCount), indices + b.indexStart, verts);
    }
}

void CharacterAttachments::draw(const BoneMatrix& charModel, const BoneMatrix* pose, uint32_t poseCount,
                                float alpha) {
    draw(charModel, pose, poseCount, alpha, nullptr);
}

// the weapons with a glow drawn this frame.
size_t CharacterAttachments::glows(WeaponGlow* out, size_t max) const {
    size_t n = 0;
    const uint32_t frame = attachmentFrame();
    for (const Entry& e : entries_) {
        if (n >= max) break;
        if (!e.pick.itemVisual || e.drawnFrame != frame) continue;
        WeaponGlow& g = out[n++];
        std::memcpy(g.world, e.world, sizeof(g.world));
        g.points = e.points;
        g.itemVisual = e.pick.itemVisual;
        g.slot = e.pick.slot;
    }
    return n;
}

void CharacterAttachments::draw(const BoneMatrix& charModel, const BoneMatrix* pose, uint32_t poseCount,
                                float alpha, const float camPos[3]) {
    if (entries_.empty() || !attachmentsEnabled()) return;
    // every player is dressed from the DBCs now (up to five item models each):
    // never into the end of the GE list (its reserves, psp_display.hpp) - counted with the NPC pass.
    if (pp::displayListFree() < pp::kListReserveNpc) {
        pp::displayListFull("npc");
        return;
    }
    PROF_SCOPE("char.attach");
    PROF_GE_PASS("char.attach");
    // upstream: weaponMat = charModelMat * boneMat * T(offset) * localTransform;
    // PSP: then T(center) * S(half) of the s16 item vertices.
    ScePspFMatrix4 mats[8];
    const size_t n = entries_.size() < 8 ? entries_.size() : 8;
    for (size_t k = 0; k < n; ++k) {
        const Entry& e = entries_[k];
        const BoneMatrix* bone = nullptr;
        BoneMatrix stand;
        if (pose && e.point.bone != kNoAttachBone && e.point.bone < poseCount) {
            bone = &pose[e.point.bone];
        } else {
            std::memcpy(stand.m, e.point.stand, sizeof(stand.m));
            bone = &stand;
        }
        BoneMatrix a, b;
        mul(charModel, *bone, a);
        for (int r = 0; r < 3; ++r)
            a.m[9 + r] += a.m[r] * e.point.position[0] + a.m[3 + r] * e.point.position[1] +
                          a.m[6 + r] * e.point.position[2];
        if (e.pick.hasLocal) {
            BoneMatrix l;
            std::memcpy(l.m, e.pick.local, sizeof(l.m));
            mul(a, l, b);
        } else {
            b = a;
        }
        // a glowing weapon keeps its world matrix of this frame (glows()).
        if (camPos && e.pick.itemVisual) {
            Entry& w = entries_[k];
            std::memcpy(w.world, b.m, sizeof(w.world));
            for (int r = 0; r < 3; ++r) w.world[9 + r] += camPos[r];
            w.drawnFrame = attachmentFrame();
        }
        ModelHeader h;
        std::memcpy(&h, e.model, sizeof(h));
        ScePspFMatrix4& m = mats[k];
        m.x = {b.m[0] * h.half[0], b.m[1] * h.half[0], b.m[2] * h.half[0], 0.0f};
        m.y = {b.m[3] * h.half[1], b.m[4] * h.half[1], b.m[5] * h.half[1], 0.0f};
        m.z = {b.m[6] * h.half[2], b.m[7] * h.half[2], b.m[8] * h.half[2], 0.0f};
        float t[3];
        for (int r = 0; r < 3; ++r)
            t[r] = b.m[9 + r] + b.m[r] * h.center[0] + b.m[3 + r] * h.center[1] + b.m[6 + r] * h.center[2];
        m.w = {t[0], t[1], t[2], 1.0f};
    }
    const bool fade = alpha < 0.999f;
    const uint32_t a8 = static_cast<uint32_t>(alpha * 255.0f + 0.5f);
    const uint32_t fixSrc = 0xFF000000u | (a8 << 16) | (a8 << 8) | a8;
    const uint32_t fixDst = 0xFF000000u | ((255 - a8) << 16) | ((255 - a8) << 8) | (255 - a8);
    auto each = [&](bool blendPass, bool fadeNow) {
        for (size_t k = 0; k < n; ++k) {
            ModelHeader h;
            std::memcpy(&h, entries_[k].model, sizeof(h));
            sceGumMatrixMode(GU_MODEL);
            sceGumLoadMatrix(&mats[k]);
            sceGuTexScale(h.uvScale[0], h.uvScale[1]);
            sceGuTexOffset(h.uvMin[0], h.uvMin[1]);
            drawPass(entries_[k], blendPass, fadeNow, fixSrc, fixDst);
        }
    };
    // The character's own pass order (CharacterModel::draw): opaque, with a
    // depth-only prepass while fading, then the blended batches.
    if (fade) {
        sceGuDisable(GU_BLEND);
        sceGuPixelMask(0xFFFFFFFFu);
        each(false, false);
        sceGuPixelMask(0);
        sceGuEnable(GU_BLEND);
        sceGuDepthMask(GU_TRUE);
        sceGuDepthFunc(GU_EQUAL);
        each(false, true);
        sceGuDepthFunc(GU_GEQUAL);
    } else {
        sceGuDisable(GU_BLEND);
        each(false, false);
    }
    sceGuDisable(GU_ALPHA_TEST);
    sceGuEnable(GU_BLEND);
    sceGuDepthMask(GU_TRUE);
    each(true, fade);
    sceGuDepthMask(GU_FALSE);
    sceGuDisable(GU_BLEND);
}

} // namespace wowee::rendering::psp
