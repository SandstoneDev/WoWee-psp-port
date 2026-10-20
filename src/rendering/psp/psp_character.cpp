#include "rendering/psp/psp_character.hpp"
#include "platform/psp/psp_prof.hpp"

#include "core/geoset_rules.hpp"
#include "platform/psp/psp_log.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/melee_anim_chains.hpp"
#include "rendering/psp/psp_lighting_ge.hpp"  // Lighting
#include "platform/psp/psp_display.hpp"       // displayListFree

#include <pspgu.h>
#include <pspgum.h>
#include <pspthreadman.h>  // sceKernelGetSystemTimeWide
#include <psputils.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <unordered_set>
#include <malloc.h>

namespace wowee::rendering::psp {

namespace pp = wowee::platform::psp;
namespace prof = wowee::platform::psp::prof;

namespace {

constexpr int kVertexType = GU_INDEX_16BIT | GU_TEXTURE_16BIT | GU_COLOR_5650 | GU_VERTEX_16BIT | GU_TRANSFORM_3D;
constexpr int kStripType = GU_TEXTURE_16BIT | GU_COLOR_5650 | GU_VERTEX_16BIT | GU_TRANSFORM_3D;
// M9 skinned sub-batch: u8 weights[n] first (GE component order), then as above.
constexpr int skinnedType(int weights) {
    return GU_WEIGHTS(weights) | GU_WEIGHT_8BIT | GU_TEXTURE_16BIT | GU_COLOR_5650 | GU_VERTEX_16BIT |
           GU_TRANSFORM_3D;
}

template <typename T>
T rd(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

// the part of the canvas a blit stays in.
struct Clip {
    int x0, y0, x1, y1;
};
constexpr Clip kFullClip{0, 0, kBodyTexSize, kBodyTexSize};

// section s of the body atlas on the 128 canvas (the 256 regions halved).
Clip sectionClip(int s) {
    const uint16_t* r = kItemRegionRect256[s];
    return Clip{r[0] / 2, r[1] / 2, (r[0] + r[2]) / 2, (r[1] + r[3]) / 2};
}

// an underwear layer of CharSections by where psp_char_conv placed it (model_bake.hpp
// regionFor): 1 = torso (TorsoUpper), 2 = pelvis (LegUpper), 0 = anything else.
uint8_t underwearKind(const uint8_t* blob, uint32_t size) {
    if (size < sizeof(LayerHeader) || std::memcmp(blob, "WLAY", 4) != 0) return 0;
    LayerHeader lh;
    std::memcpy(&lh, blob, sizeof(lh));
    const Clip tu = sectionClip(body::kTorsoUpper), lu = sectionClip(body::kLegUpper);
    if (lh.x == tu.x0 && lh.y == tu.y0) return 1;
    if (lh.x == lu.x0 && lh.y == lu.y0) return 2;
    return 0;
}

// Alpha-over of one WLAY onto the ABGR canvas (upstream blitOverlay; the
// base skin is copied, overlays blend by their alpha).
// within `clip` (a section painted again).
void blitLayer(uint32_t* canvas, const uint8_t* blob, uint32_t size, bool base, const Clip& clip = kFullClip) {
    if (size < sizeof(LayerHeader) + 1024 || std::memcmp(blob, "WLAY", 4) != 0) return;
    const auto* lh = reinterpret_cast<const LayerHeader*>(blob);
    if (size < sizeof(LayerHeader) + 1024u + static_cast<uint32_t>(lh->w) * lh->h) return;
    const uint8_t* clutBytes = blob + sizeof(LayerHeader);
    const uint8_t* px = clutBytes + 1024;
    for (int y = 0; y < lh->h; ++y) {
        const int cy = lh->y + y;
        if (cy < clip.y0) continue;
        if (cy >= clip.y1) break;
        for (int x = 0; x < lh->w; ++x) {
            const int cx = lh->x + x;
            if (cx < clip.x0) continue;
            if (cx >= clip.x1) break;
            const uint32_t s = rd<uint32_t>(clutBytes + px[y * lh->w + x] * 4u);
            uint32_t& d = canvas[cy * kBodyTexSize + cx];
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

// PSP: the race / sex model blobs (n_R_S, c_R_S of characters.pak) shared by every model of that race and sex.
// A blob is read-only after the load (SkinView / ModelHeader views, the GE draws from it), so one copy serves them
// all: in a crowd every remote player read its own 0.7-1.3 MB copy and the heap ran out (pak_alloc_fail
// n_R_S with ~25 players, 10-15). Main thread only (the world screen, the character screens). Refcounted; the
// last release frees it.
struct SharedBlob {
    const pp::Pak* pak = nullptr;
    char name[16] = {};
    void* blob = nullptr;
    uint32_t size = 0;
    uint32_t refs = 0;
};
std::vector<SharedBlob> g_sharedBlobs;
uint32_t g_sharedLogs = 0;

// -> the blob (refs + 1) or nullptr; `fresh` = read now (the first holder).
void* sharedAcquire(pp::Pak& pak, const pp::PakEntry* e, const char* name, bool& fresh) {
    fresh = false;
    for (SharedBlob& s : g_sharedBlobs)
        if (s.pak == &pak && std::strcmp(s.name, name) == 0) {
            ++s.refs;
            return s.blob;
        }
    void* blob = pak.load(e);
    if (!blob) return nullptr;
    SharedBlob s;
    s.pak = &pak;
    std::snprintf(s.name, sizeof(s.name), "%s", name);
    s.blob = blob;
    s.size = e->size;
    s.refs = 1;
    g_sharedBlobs.push_back(s);
    fresh = true;
    if (++g_sharedLogs <= 40)
        PSP_LOGI(pp::tag::Gfx, "char_blob_load name=%s kb=%u shared=%u", name, static_cast<unsigned>(e->size / 1024u),
                 static_cast<unsigned>(g_sharedBlobs.size()));
    return blob;
}

void sharedRelease(void* blob) {
    if (!blob) return;
    for (size_t i = 0; i < g_sharedBlobs.size(); ++i) {
        if (g_sharedBlobs[i].blob != blob) continue;
        if (--g_sharedBlobs[i].refs == 0) {
            pp::pakFree(blob);
            g_sharedBlobs.erase(g_sharedBlobs.begin() + static_cast<std::ptrdiff_t>(i));
        }
        return;
    }
    pp::pakFree(blob);  // not a shared one
}

const SharedBlob* sharedFind(const pp::Pak& pak, const char* name) {
    for (const SharedBlob& s : g_sharedBlobs)
        if (s.pak == &pak && std::strcmp(s.name, name) == 0) return &s;
    return nullptr;
}

// GE fast-mode swizzle: blocks of 16 bytes x 8 rows.
void swizzle(const uint8_t* in, uint8_t* out, int widthBytes, int height) {
    for (int by = 0; by < height / 8; ++by)
        for (int bx = 0; bx < widthBytes / 16; ++bx)
            for (int y = 0; y < 8; ++y) {
                std::memcpy(out, in + (by * 8 + y) * widthBytes + bx * 16, 16);
                out += 16;
            }
}

// swizzle's inverse - the body texture back into rows, to paint some sections again.
void unswizzle(const uint8_t* in, uint8_t* out, int widthBytes, int height) {
    for (int by = 0; by < height / 8; ++by)
        for (int bx = 0; bx < widthBytes / 16; ++bx)
            for (int y = 0; y < 8; ++y) {
                std::memcpy(out + (by * 8 + y) * widthBytes + bx * 16, in, 16);
                in += 16;
            }
}

} // namespace

CharacterModel::~CharacterModel() { clear(); }

uint32_t CharacterModel::raceBlobBytes(pp::Pak& pak, const CharacterLook& look, bool animated) {
    char name[32];
    if (animated) {
        std::snprintf(name, sizeof(name), "n_%u_%u", look.race, look.sex);
        if (const pp::PakEntry* e = pak.find(name)) return e->size;
    }
    std::snprintf(name, sizeof(name), "c_%u_%u", look.race, look.sex);
    const pp::PakEntry* e = pak.find(name);
    return e ? e->size : 0u;
}

bool CharacterModel::raceBlobResident(const pp::Pak& pak, const CharacterLook& look, bool animated) {
    char name[32];
    std::snprintf(name, sizeof(name), "%s_%u_%u", animated ? "n" : "c", look.race, look.sex);
    return sharedFind(pak, name) != nullptr;
}

uint32_t CharacterModel::sharedBlobCount() { return static_cast<uint32_t>(g_sharedBlobs.size()); }

uint32_t CharacterModel::sharedBlobKb() {
    uint32_t b = 0;
    for (const SharedBlob& s : g_sharedBlobs) b += s.size;
    return b / 1024u;
}

void CharacterModel::clear() {
    for (auto& t : textures_)
        if (t.block) pp::textureRelease(t);
    textures_.clear();
    if (modelBlob_ && !view_) sharedRelease(modelBlob_);  // a view's blob is its base model's (no reference)
    modelBlob_ = nullptr;
    view_ = false;
    h_ = nullptr;
    std::free(bodyPixels_);
    bodyPixels_ = nullptr;
    body_ = pp::Texture{};
    hairTexture_ = kNoTexture;
    releaseItemTexture(cape_);  // the shared cache's reference
    cape_ = nullptr;
    bodyPasteValid_ = false;
    for (auto& u : baseUnderwear_) u = 0;
    geo_ = nullptr;
    equipment_ = Equipment{};
    texFlush_ = false;
    active_.clear();
    memoryBytes_ = 0;
    batches_ = nullptr;
    batchCount_ = 0;
    skinned_ = false;
    skin_ = SkinView{};
    if (extraBlob_) pp::pakFree(extraBlob_);
    extraBlob_ = nullptr;
    extra_ = SkinView{};
    extraActive_ = false;
    blendFromSeq_ = -1;
    sequence_ = -1;
    animId_ = 0;
    animTimeMs_ = 0.0f;
    poseDirty_ = true;
    pose_.clear();
    std::free(geBones_);
    geBones_ = nullptr;
    attach_.clear();
    attachPoints_.clear();
    attachItems_ = nullptr;
}

const pp::Texture* CharacterModel::texture(uint16_t id) const {
    if (id == kTexBody) return body_.pixels ? &body_ : nullptr;
    if (id == kTexHair) id = hairTexture_;
    if (id >= textures_.size() || !textures_[id].pixels) return nullptr;
    return &textures_[id];
}

bool CharacterModel::load(pp::Pak& pak, const CharacterLook& look, bool animated, const Equipment* equipment,
                          ItemBank* items) {
    clear();
    char name[32];
    // M9: the skinned model with its animations, when the pak has it.
    if (animated) {
        std::snprintf(name, sizeof(name), "n_%u_%u", look.race, look.sex);
        const pp::PakEntry* ne = pak.find(name);
        bool fresh = false;
        auto* nb = ne ? static_cast<uint8_t*>(sharedAcquire(pak, ne, name, fresh)) : nullptr;
        if (nb && skin_.init(nb, ne->size)) {
            skinned_ = true;
            modelBlob_ = nb;
            batches_ = skin_.batches;
            batchCount_ = skin_.h->batchCount;
            if (fresh) memoryBytes_ += ne->size;  // counted once, by the model that read it
            const uint32_t bones = skin_.h->boneCount;
            pose_.resize(bones);
            geBones_ = static_cast<ScePspFMatrix4*>(memalign(16, bones * sizeof(ScePspFMatrix4)));
            if (!geBones_) {
                PSP_LOGE(pp::tag::Gfx, "char_skin_nomem bones=%u", static_cast<unsigned>(bones));
                clear();
                return false;
            }
            memoryBytes_ += bones * (sizeof(ScePspFMatrix4) + sizeof(BoneMatrix));
            playAnimation(anim::STAND, true);
        } else if (nb) {
            PSP_LOGE(pp::tag::Gfx, "char_skin_bad name=%s size=%u", name, static_cast<unsigned>(ne->size));
            sharedRelease(nb);
            skin_ = SkinView{};
        } else if (ne) {
            // PSP: no memory for the animated model - no static stand-in (a white, frozen body in a crowd); the
            // caller tries again when there is room.
            return false;
        }
    }
    const uint16_t* geo = nullptr;
    if (skinned_) {
        geo = skin_.geosets;
    } else {
        std::snprintf(name, sizeof(name), "c_%u_%u", look.race, look.sex);
        const pp::PakEntry* e = pak.find(name);
        bool fresh = false;
        auto* blob = e ? static_cast<uint8_t*>(sharedAcquire(pak, e, name, fresh)) : nullptr;
        if (!blob) return false;
        const auto* h = reinterpret_cast<const ModelHeader*>(blob);
        const uint32_t batches = h->batchCount[0];
        if (e->size < sizeof(ModelHeader) || std::memcmp(h->magic, "WMDL", 4) != 0 ||
            h->version != kModelFormatVersion || h->vertexOffset % 16 || h->geosetOffset == 0 ||
            h->vertexOffset + h->vertexCount * sizeof(ModelVertex) > e->size ||
            h->indexOffset + h->indexCount * 2u > e->size || h->batchOffset + batches * sizeof(ModelBatch) > e->size ||
            h->geosetOffset + batches * 2u > e->size) {
            PSP_LOGE(pp::tag::Gfx, "char_bad name=%s size=%u", name, static_cast<unsigned>(e->size));
            sharedRelease(blob);
            return false;
        }
        modelBlob_ = blob;
        h_ = h;
        verts_ = reinterpret_cast<const ModelVertex*>(blob + h->vertexOffset);
        indices_ = reinterpret_cast<const uint16_t*>(blob + h->indexOffset);
        batches_ = reinterpret_cast<const ModelBatch*>(blob + h->batchOffset);
        batchCount_ = batches;
        geo = reinterpret_cast<const uint16_t*>(blob + h->geosetOffset);
        if (fresh) memoryBytes_ += e->size;  // counted once, by the model that read it
    }
    look_ = look;
    geo_ = geo;
    equipment_ = equipment ? *equipment : Equipment{};

    // Appearance tables.
    hairScalp_ = 1;
    facial_[0] = facial_[1] = facial_[2] = 1;
    for (auto& l : bodyLayers_) l = kNoLayer;
    std::snprintf(name, sizeof(name), "a_%u_%u", look.race, look.sex);
    const pp::PakEntry* ae = pak.find(name);
    auto* app = ae ? static_cast<uint8_t*>(pak.load(ae)) : nullptr;
    if (app && ae->size >= sizeof(AppearanceHeader) && std::memcmp(app, "WAPP", 4) == 0) {
        const auto* ah = reinterpret_cast<const AppearanceHeader*>(app);
        auto clampTo = [](uint8_t v, uint8_t n) { return static_cast<uint32_t>(n ? (v < n ? v : n - 1) : 0); };
        const uint32_t skin = clampTo(look.skin, ah->skins), face = clampTo(look.face, ah->faces);
        const uint32_t style = clampTo(look.hairStyle, ah->hairStyles), color = clampTo(look.hairColor, ah->hairColors);
        const uint32_t facial = clampTo(look.facial, ah->facials);
        const uint32_t bodyAt = ah->bodyOffset + ((skin * ah->faces + face) * kBodyLayers) * 2u;
        if (bodyAt + kBodyLayers * 2u <= ae->size)
            for (int i = 0; i < kBodyLayers; ++i) bodyLayers_[i] = rd<uint16_t>(app + bodyAt + i * 2);
        const uint32_t hairAt = ah->hairOffset + (style * ah->hairColors + color) * 2u;
        if (hairAt + 2 <= ae->size) hairTexture_ = rd<uint16_t>(app + hairAt);
        const uint32_t scalpAt = ah->hairGeosetOffset + style * 2u;
        if (scalpAt + 2 <= ae->size) {
            const uint16_t g = rd<uint16_t>(app + scalpAt);
            if (g) hairScalp_ = g;
        }
        const uint32_t facialAt = ah->facialOffset + facial * 6u;
        if (facialAt + 6 <= ae->size)
            for (int k = 0; k < 3; ++k) facial_[k] = rd<uint16_t>(app + facialAt + k * 2);
    } else {
        PSP_LOGW(pp::tag::Gfx, "char_no_appearance %s", name);
    }
    if (app) pp::pakFree(app);

    dress(pak, items);
    // The attachment points (older paks have none: nothing attached).
    std::snprintf(name, sizeof(name), "p_%u_%u", look.race, look.sex);
    if (const pp::PakEntry* pe = pak.find(name)) {
        auto* pb = static_cast<uint8_t*>(pak.load(pe));
        AttachTable at{};
        if (pb && pe->size >= sizeof(at)) std::memcpy(&at, pb, sizeof(at));
        if (pb && std::memcmp(at.magic, "WATP", 4) == 0 &&
            pe->size >= sizeof(at) + static_cast<uint32_t>(at.count) * sizeof(AttachPoint)) {
            attachPoints_.resize(at.count);
            std::memcpy(attachPoints_.data(), pb + sizeof(at), at.count * sizeof(AttachPoint));
        } else {
            PSP_LOGW(pp::tag::Gfx, "attach_points_bad name=%s", name);
        }
        if (pb) pp::pakFree(pb);
    }
    attachEquipment(items);
    PSP_LOGI(pp::tag::Gfx, "char race=%u sex=%u skin=%u face=%u hair=%u/%u facial=%u tris=%u kb=%u", look.race,
             look.sex, look.skin, look.face, look.hairStyle, look.hairColor, look.facial,
             static_cast<unsigned>(triangles_), static_cast<unsigned>(memoryBytes_ / 1024u));
    if (skinned_)
        PSP_LOGI(pp::tag::Gfx, "char_anim bones=%u seqs=%u subbatches=%u", static_cast<unsigned>(skin_.h->boneCount),
                 static_cast<unsigned>(skin_.h->sequenceCount), static_cast<unsigned>(skin_.h->subBatchCount));
    return true;
}

// Geosets, the textures the shown batches use and the body composite, for
// look_ and equipment_. Upstream: the bare set (core/geoset_rules.hpp), then
// EntitySpawner::setOnlinePlayerEquipment's geoset rules and
// CharacterRenderer::compositeWithRegions' layer order.
// the equipment groups, the helm's hidden parts, the body layers and the cloak by
// the body rules (psp_body_rules.hpp, psp_equipment_dress.cpp), for what is shown (shownEquipment: Show Helm /
// Show Cloak).
void CharacterModel::dress(pp::Pak& pak, ItemBank* items) {
    PROF_SCOPE("char.dress");
    const uint64_t dressT0 = sceKernelGetSystemTimeWide();  // char_equip ms= (records/layers read on demand)
    const uint32_t reads0 = pp::pakReadCount();             // reads= (card reads of this dress)
    const uint32_t batches = batchCount_;
    const bool dressed = items && items->isOpen() && !equipment_.empty();
    // the outfit (outfitOf: records read in one pass).
    body::Outfit outfit;
    uint32_t helmHide[kHelmetHideKinds] = {};
    if (dressed) outfitOf(equipment_, look_.race, look_.sex, *items, outfit, helmHide);
    body::Dress bodyDress;
    body::buildDress(outfit, bodyDress);
    // Geosets: upstream's bare set (core/geoset_rules.hpp) for the character's own groups (hair, facial hair,
    // ears, eye glow); the helm's hidden parts and every equipment group by the body rules - with nothing worn too
    // (each group's variant 01: no kneepads on bare legs; no cloak = 1501, the collar panel that closes the back of
    // the neck - the hardware showed a hole there without it).
    auto geosets = core::bareCharacterGeosets(hairScalp_, facial_[0], facial_[1], facial_[2], look_.race);
    body::helmetGeosets(geosets, helmHide, look_.race);
    body::outfitGeosets(geosets, outfit, bodyDress);
    active_.assign(batches, 0);
    triangles_ = 0;
    for (uint32_t i = 0; i < batches; ++i) {
        if (!geosets.count(geo_[i])) continue;
        active_[i] = 1;
        if (skinned_) {
            for (uint32_t k = 0; k < batches_[i].indexCount; ++k)
                triangles_ += skin_.subBatches[batches_[i].indexStart + k].vertexCount - 2u;
        } else {
            triangles_ += (h_->flags & kModelStrips) ? batches_[i].indexCount - 2 : batches_[i].indexCount / 3;
        }
    }

    // Textures the shown batches use (kept once loaded).
    auto loadTex = [&](uint16_t id) {
        if (id >= 0xFFF0) return;
        if (id >= textures_.size()) textures_.resize(id + 1u);
        if (textures_[id].pixels) return;
        char tn[16];
        std::snprintf(tn, sizeof(tn), "x_%u", static_cast<unsigned>(id));
        const pp::PakEntry* te = pak.find(tn);
        void* tb = te ? pak.load(te) : nullptr;
        if (!tb) return;
        if (!pp::textureFromBlob(tb, te->size, textures_[id], true)) {
            pp::pakFree(tb);
            return;
        }
        memoryBytes_ += te->size;
    };
    loadTex(hairTexture_);
    for (uint32_t i = 0; i < batches; ++i)
        if (active_[i]) loadTex(batches_[i].texture);
    // The cloak's texture (upstream "Cape texture" + group 15 override).
    // from the shared item texture cache (one copy for every character in that
    // cloak, counted in its KB, not in this model's); the old one is given back after the new one is held, so a
    // re-dress in the same cloak reads nothing.
    // capeTexture - the shown back slot (none while Show Cloak is off).
    const pp::Texture* oldCape = cape_;
    cape_ = nullptr;
    if (dressed) {
        const uint16_t capeId = capeTexture(equipment_, look_.sex, *items);
        if (capeId != kNoItemLayer) cape_ = acquireItemTexture(*items, capeId);
    }
    releaseItemTexture(oldCape);

    // Body: skin, face, underwear ("l_" from characters.pak), then the equipment region layers ("q_" from
    // items.pak); swizzled for the GE. The body rules' sections and layers, only what changed.
    const uint32_t painted = composeBody(pak, dressed ? items : nullptr, bodyDress);
    // a guild tabard with the guild's emblem (psp_emblem_format.hpp) - what was pasted.
    if (painted && bodyDress.part[body::kTorsoUpper][emblem::kLayerEmblem] == emblem::kEmblem) {
        static uint32_t emblemLogs = 0;
        const emblem::Emblem& e = outfit.emblem;
        if (++emblemLogs <= 40)
            PSP_LOGI(pp::tag::Gfx, "char_emblem style=%d color=%d border=%d border_color=%d bg=%d dirty=0x%02X",
                     static_cast<int>(e.style), static_cast<int>(e.color), static_cast<int>(e.borderStyle),
                     static_cast<int>(e.borderColor), static_cast<int>(e.background), static_cast<unsigned>(painted));
    }
    int worn = 0;
    for (uint32_t d : equipment_.displayId) worn += d ? 1 : 0;
    if (worn || painted) {
        uint32_t itemLayers = 0;
        for (const body::Paste& p : bodyPaste_) itemLayers += p.count;
        // + dirty= (sections painted, 0xFF = all), reads= (card reads of the whole dress: records,
        // layers, skin, cloak), hide= helm/cloak switches.
        PSP_LOGI(pp::tag::Gfx, "char_equip worn=%d item_layers=%u baked=%d ms=%u dirty=0x%02X reads=%u hide=%d%d", worn,
                 static_cast<unsigned>(itemLayers), dressed ? 1 : 0,
                 static_cast<unsigned>((sceKernelGetSystemTimeWide() - dressT0) / 1000u),
                 static_cast<unsigned>(painted), static_cast<unsigned>(pp::pakReadCount() - reads0),
                 equipment_.hideHelm ? 1 : 0, equipment_.hideCloak ? 1 : 0);
    }
}

// the body texture by the body rules' sections (psp_body_rules.hpp pasteOf). A section
// is painted again only when what it shows changed: the current texture is un-swizzled into the canvas, the skin
// put back over those sections and their underwear / layers pasted on it - the rest of the texture stays as it is.
uint32_t CharacterModel::composeBody(pp::Pak& pak, ItemBank* items, const body::Dress& dress) {
    body::Paste want[body::kSections];
    for (int s = 0; s < body::kSections; ++s) want[s] = body::pasteOf(dress, s);
    bool full = !bodyPasteValid_ || !bodyPixels_;
    uint32_t dirty = 0;
    for (int s = 0; s < body::kSections; ++s)
        if (full || !(want[s] == bodyPaste_[s])) dirty |= 1u << s;
    if (!dirty) return 0;  // nothing of the body changed: no read, no blit
    auto loadBase = [&](int i, uint32_t& size) -> uint8_t* {
        char name[16];
        std::snprintf(name, sizeof(name), "l_%u", static_cast<unsigned>(bodyLayers_[i]));
        const pp::PakEntry* le = pak.find(name);
        size = le ? le->size : 0u;
        return le ? static_cast<uint8_t*>(pak.load(le)) : nullptr;
    };
    // Painting some sections again needs the skin under them; without it the whole texture is built anew.
    uint32_t skinSize = 0;
    uint8_t* skin = nullptr;
    if (!full) {
        skin = bodyLayers_[0] != kNoLayer ? loadBase(0, skinSize) : nullptr;
        full = skin == nullptr;
    }
    if (full) dirty = 0xFFu;
    // The layers to paste, located in one pass (TOC order), then read one by one as they are blitted.
    if (items) {
        uint16_t need[body::kSections * body::kLayers];
        uint32_t n = 0;
        for (int s = 0; s < body::kSections; ++s)
            if (dirty & (1u << s))
                for (int i = 0; i < want[s].count; ++i)
                    if (want[s].part[i] == emblem::kItem) need[n++] = want[s].id[i];  // items.pak only
        items->prefetchLayers(need, n);
    }
    const size_t bytes = kBodyTexSize * kBodyTexSize * 4;
    auto* canvas = static_cast<uint32_t*>(std::malloc(bytes));
    const bool fresh = bodyPixels_ == nullptr;
    if (fresh) bodyPixels_ = memalign(16, bytes);
    if (!canvas || !bodyPixels_) {
        std::free(canvas);
        if (skin) pp::pakFree(skin);
        if (fresh) {
            std::free(bodyPixels_);
            bodyPixels_ = nullptr;
        }
        bodyPasteValid_ = false;
        return 0;
    }
    auto underwearShown = [&](int i) {
        return (baseUnderwear_[i] != 1 || want[body::kTorsoUpper].underwear) &&
               (baseUnderwear_[i] != 2 || want[body::kLegUpper].underwear);
    };
    if (full) {
        for (int i = 0; i < kBodyTexSize * kBodyTexSize; ++i) canvas[i] = 0xFF808080u;
        for (int i = 0; i < kBodyLayers; ++i) {
            baseUnderwear_[i] = 0;
            if (bodyLayers_[i] == kNoLayer) continue;
            uint32_t size = 0;
            uint8_t* lb = loadBase(i, size);
            if (!lb) continue;
            if (i >= 3) baseUnderwear_[i] = underwearKind(lb, size);  // the underwear rows of CharSections
            if (underwearShown(i)) blitLayer(canvas, lb, size, i == 0);
            pp::pakFree(lb);
        }
    } else {
        unswizzle(static_cast<const uint8_t*>(bodyPixels_), reinterpret_cast<uint8_t*>(canvas), kBodyTexSize * 4,
                  kBodyTexSize);
        // The skin under the sections painted again, then their underwear where the outfit leaves it bare.
        for (int s = 0; s < body::kSections; ++s)
            if (dirty & (1u << s)) blitLayer(canvas, skin, skinSize, true, sectionClip(s));
        pp::pakFree(skin);
        uint32_t size = 0;
        for (int i = 3; i < kBodyLayers; ++i) {
            const int s = baseUnderwear_[i] == 1 ? body::kTorsoUpper : baseUnderwear_[i] == 2 ? body::kLegUpper : -1;
            if (s < 0 || bodyLayers_[i] == kNoLayer || !(dirty & (1u << s)) || !want[s].underwear) continue;
            if (uint8_t* lb = loadBase(i, size)) {
                blitLayer(canvas, lb, size, false, sectionClip(s));
                pp::pakFree(lb);
            }
        }
    }
    if (items) {
        for (int s = 0; s < body::kSections; ++s) {
            if (!(dirty & (1u << s))) continue;
            for (int i = 0; i < want[s].count; ++i) {
                uint32_t size = 0;
                // a guild emblem part (data/emblems.pak) or an item layer (items.pak)
                auto* lb = static_cast<uint8_t*>(want[s].part[i] != emblem::kItem
                                                     ? items->loadEmblemLayer(want[s].part[i], s, want[s].id[i], size)
                                                     : items->loadLayer(want[s].id[i], size));
                if (!lb) continue;
                blitLayer(canvas, lb, size, false, sectionClip(s));
                pp::pakFree(lb);
            }
        }
    }
    swizzle(reinterpret_cast<const uint8_t*>(canvas), static_cast<uint8_t*>(bodyPixels_), kBodyTexSize * 4,
            kBodyTexSize);
    sceKernelDcacheWritebackRange(bodyPixels_, bytes);
    std::free(canvas);
    body_.pixels = bodyPixels_;
    body_.mip[0] = bodyPixels_;
    body_.w = body_.h = body_.uw = body_.uh = kBodyTexSize;
    body_.psm = GU_PSM_8888;
    body_.swizzled = 1;
    body_.flags = pp::kTexLinear;
    body_.name = "body";
    if (fresh) memoryBytes_ += bytes;
    else texFlush_ = true;
    for (int s = 0; s < body::kSections; ++s) bodyPaste_[s] = want[s];
    bodyPasteValid_ = true;
    return dirty;
}

void CharacterModel::setEquipment(pp::Pak& pak, const Equipment& equipment, ItemBank* items) {
    if (!loaded() || !geo_) return;
    equipment_ = equipment;
    dress(pak, items);
    attachEquipment(items);
}

// a view of `base` (psp_character.hpp) - load() without reading the model: the
// base's blob, appearance tables and loaded textures are taken over, the per-instance state is made here.
bool CharacterModel::loadView(const CharacterModel& base, pp::Pak& pak, const Equipment& equipment, ItemBank* items) {
    clear();
    if (&base == this || !base.loaded() || !base.geo_ || base.view_) return false;
    view_ = true;
    modelBlob_ = base.modelBlob_;
    h_ = base.h_;
    verts_ = base.verts_;
    indices_ = base.indices_;
    batches_ = base.batches_;
    batchCount_ = base.batchCount_;
    skinned_ = base.skinned_;
    skin_ = base.skin_;
    geo_ = base.geo_;
    look_ = base.look_;
    std::memcpy(bodyLayers_, base.bodyLayers_, sizeof(bodyLayers_));
    hairTexture_ = base.hairTexture_;
    hairScalp_ = base.hairScalp_;
    std::memcpy(facial_, base.facial_, sizeof(facial_));
    // The base's textures: the same pixels, no owning block (clear() releases only what this view loaded).
    textures_ = base.textures_;
    for (auto& t : textures_) t.block = nullptr;
    attachPoints_ = base.attachPoints_;
    if (skinned_) {
        const uint32_t bones = skin_.h->boneCount;
        pose_.resize(bones);
        geBones_ = static_cast<ScePspFMatrix4*>(memalign(16, bones * sizeof(ScePspFMatrix4)));
        if (!geBones_) {
            PSP_LOGE(pp::tag::Gfx, "char_view_nomem bones=%u", static_cast<unsigned>(bones));
            clear();
            return false;
        }
        memoryBytes_ += bones * (sizeof(ScePspFMatrix4) + sizeof(BoneMatrix));
        playAnimation(anim::STAND, true);
    }
    equipment_ = equipment;
    dress(pak, items);
    attachEquipment(items);
    PSP_LOGI(pp::tag::Gfx, "char_view race=%u sex=%u skinned=%d tris=%u kb=%u", look_.race, look_.sex,
             skinned_ ? 1 : 0, static_cast<unsigned>(triangles_), static_cast<unsigned>(memoryBytes_ / 1024u));
    return true;
}

// the Stand sequence (variation 0) at standMs for one draw; the sequence in play, its time and its
// blend come back after it. The GE has the pose already: the bone matrices go into the display list by value
// (sceGuBoneMatrix), the attachments' matrices too (sceGumLoadMatrix).
void CharacterModel::drawStand(const TerrainCamera& cam, const glm::vec3& pos, float yaw, uint32_t fogColorABGR,
                               float standMs) {
    const int seq = skinned_ ? skin_.findSequence(anim::STAND) : -1;
    if (seq < 0) {
        draw(cam, pos, yaw, fogColorABGR, 1.0f);
        return;
    }
    const int savedSeq = sequence_, savedBlend = blendFromSeq_;
    const bool savedExtra = extraActive_;
    const float savedTime = animTimeMs_;
    const float dur = static_cast<float>(skin_.sequences[seq].durationMs);
    sequence_ = seq;
    extraActive_ = false;
    blendFromSeq_ = -1;
    animTimeMs_ = dur > 0.0f ? std::fmod(std::max(0.0f, standMs), dur) : 0.0f;
    poseDirty_ = true;
    draw(cam, pos, yaw, fogColorABGR, 1.0f);
    sequence_ = savedSeq;
    extraActive_ = savedExtra;
    blendFromSeq_ = savedBlend;
    animTimeMs_ = savedTime;
    // The pose in play again (attachmentPosition reads "the pose last evaluated" - spell effects at the hands).
    if (sequence_ >= 0) evaluatePose();
    else poseDirty_ = true;
}

// Item models on the attachment points for equipment_ (upstream
// setOnlinePlayerEquipment / AppearanceComposer::loadEquippedWeapons).
void CharacterModel::attachEquipment(ItemBank* items) {
    attachItems_ = items;
    if (!items || !items->isOpen() || attachPoints_.empty() || equipment_.empty()) {
        attach_.clear();
        return;
    }
    // Sheath - PSP: another player whose UNIT_FIELD_BYTES_2 says
    // sheathed takes the own player's sheath points (upstream has no sheathed
    // path for others: always in hand); drawn = upstream's online rules.
    // with sheathClient, one rule for every player by the sheath state
    // (equipmentAttachmentsBySheath: Item.dbc sheath types, the ranged state, the glows).
    const bool localRules = localPlayer_ || sheathed_;
    // what is shown - no helm / cloak model while "Show Helm" / "Show Cloak" is off (the item
    // stays worn).
    const Equipment shown = shownEquipment(equipment_);
    const auto picks = sheathClient
                           ? equipmentAttachmentsBySheath(shown, look_.race, look_.sex, sheathState_, *items,
                                                          SheathAttachOptions{rangedOnBack, false})
                           : equipmentAttachments(shown, look_.race, look_.sex,
                                                  localRules ? AttachRules::Local : AttachRules::Online, sheathed_,
                                                  *items);
    attach_.set(picks, attachPoints_, *items, localPlayer_);  // others within attachcap
    // What is worn on a slot with a model upstream but found nothing to draw.
    static int missingLogged = 0;
    for (int slot : {0, 2, 15, 16}) {
        const uint32_t d = equipment_.displayId[slot];
        if (!d || items->models(d)) continue;
        if (++missingLogged <= 60)
            PSP_LOGW(pp::tag::Gfx, "attach_missing display=%u slot=%d reason=%s", static_cast<unsigned>(d), slot,
                     items->get(d) ? "nomodel" : "notbaked");
    }
    // + bank_kb = items.pak's index and caches (ItemBank::ramBytes).
    PSP_LOGI(pp::tag::Gfx, "char_attach n=%u picks=%u points=%u local=%d sheathed=%d cache_kb=%u bank_kb=%u",
             static_cast<unsigned>(attach_.count()), static_cast<unsigned>(picks.size()),
             static_cast<unsigned>(attachPoints_.size()), localPlayer_ ? 1 : 0, sheathed_ ? 1 : 0,
             static_cast<unsigned>(attachmentCacheBytes() / 1024u), static_cast<unsigned>(items->ramBytes() / 1024u));
    // where the weapons went (the first 60 dresses / sheath changes).
    static int weaponLogs = 0;
    if (sheathClient && ++weaponLogs <= 60) {
        char line[160];
        int at = 0;
        for (const AttachPick& p : picks)
            if (p.slot >= 15 && at < static_cast<int>(sizeof(line)) - 32)
                at += std::snprintf(line + at, sizeof(line) - static_cast<size_t>(at), " %u@%u%s",
                                    static_cast<unsigned>(p.slot), static_cast<unsigned>(p.point),
                                    p.itemVisual ? "*" : "");
        line[at] = 0;
        PSP_LOGI(pp::tag::Gfx, "char_weapons state=%u sheath=%u/%u/%u visual=%u/%u picks=%s",
                 static_cast<unsigned>(sheathState_), static_cast<unsigned>(equipment_.sheath[0]),
                 static_cast<unsigned>(equipment_.sheath[1]), static_cast<unsigned>(equipment_.sheath[2]),
                 static_cast<unsigned>(equipment_.enchantVisual[0]), static_cast<unsigned>(equipment_.enchantVisual[1]),
                 at ? line : " none");
    }
}

void CharacterModel::setWeaponsSheathed(bool sheathed) {
    if (sheathed == sheathed_ && sheathState_ == (sheathed ? kSheathStateUnarmed : kSheathStateMelee)) return;
    sheathed_ = sheathed;
    sheathState_ = sheathed ? kSheathStateUnarmed : kSheathStateMelee;
    if (loaded()) attachEquipment(attachItems_);  // Sheath - others too
}

// the unit's sheath state (0 / 1 / 2) - the attachments again when it changes.
void CharacterModel::setSheathState(uint8_t state) {
    if (state > kSheathStateRanged) state = kSheathStateMelee;
    if (state == sheathState_ && sheathed_ == (state != kSheathStateMelee)) return;
    sheathState_ = state;
    sheathed_ = state != kSheathStateMelee;
    if (loaded()) attachEquipment(attachItems_);
}

bool CharacterModel::sheathClient = true;
bool CharacterModel::rangedOnBack = false;

// upstream CharacterRenderer::playAnimation: variation 0 preferred, else
// the first sequence with the id, else the model's first sequence.
void CharacterModel::playAnimation(uint32_t animId, bool loop) {
    if (!skinned_ || skin_.h->sequenceCount == 0) return;
    int seq = skin_.findSequence(animId);
    bool extra = false;
    if (seq < 0 && loadExtra(animId)) {
        seq = extra_.findSequence(animId);
        extra = seq >= 0;
    }
    if (seq < 0) {
        seq = 0;
        animId = skin_.sequences[0].animId;
    }
    startBlend(seq, extra);
    extraActive_ = extra;
    sequence_ = seq;
    animId_ = animId;
    animTimeMs_ = 0.0f;
    animLoop_ = loop;
    poseDirty_ = true;
    repeatsLeft_ = 0;
}

// M9c: the emote sequence "o_<race>_<sex>_<anim>" (WSKA, same skeleton).
bool CharacterModel::loadExtra(uint32_t animId) {
    if (extraBlob_ && extra_.findSequence(animId) >= 0) return true;
    if (!emotePak_ || !emotePak_->isOpen()) return false;
    char name[32];
    std::snprintf(name, sizeof(name), "o_%u_%u_%u", static_cast<unsigned>(look_.race), static_cast<unsigned>(look_.sex),
                  static_cast<unsigned>(animId));
    const pp::PakEntry* e = emotePak_->find(name);
    if (!e) return false;
    void* blob = emotePak_->load(e);
    SkinView v;
    if (!blob || !v.init(blob, e->size, true, skin_.h->boneCount)) {
        if (blob) pp::pakFree(blob);
        PSP_LOGW(pp::tag::Gfx, "emote_anim_bad %s", name);
        return false;
    }
    if (blendFromExtra_) blendFromSeq_ = -1;  // the fade source lives in the blob being replaced
    if (extraBlob_) pp::pakFree(extraBlob_);
    extraBlob_ = blob;
    extra_ = v;
    return true;
}

// The sequence playing now becomes the blend source of `newSeq`; the fade
// lasts the new sequence's M2 blendTime (Stand 500 ms, most 150 ms; 150 when
// the pak predates the field). No blend from an emote blob that the new
// sequence replaces (it is freed) or into the same sequence.
void CharacterModel::startBlend(int newSeq, bool newExtra) {
    blendFromSeq_ = -1;
    blendMs_ = 0.0f;
    if (sequence_ < 0 || (sequence_ == newSeq && extraActive_ == newExtra)) return;
    if (extraActive_ && newExtra) return;  // the old emote's blob was just replaced
    blendFromSeq_ = sequence_;
    blendFromExtra_ = extraActive_;
    blendFromTimeMs_ = animTimeMs_;
    blendElapsedMs_ = 0.0f;
    const SkinView& to = newExtra ? extra_ : skin_;
    const SkinView& from = extraActive_ ? extra_ : skin_;
    // The shorter of the new sequence's blend-in and the old one's (Stand
    // carries 500 ms: Run -> Stand took half a second of running legs on a
    // standing character - "sliding" on the hardware). Into locomotion at
    // most 100 ms: the body is already moving at full speed.
    float ms = to.sequences[newSeq].blendTimeMs ? to.sequences[newSeq].blendTimeMs : 150.0f;
    if (from.sequences[sequence_].blendTimeMs) ms = std::min(ms, static_cast<float>(from.sequences[sequence_].blendTimeMs));
    switch (to.sequences[newSeq].animId) {
        case anim::WALK: case anim::RUN: case anim::WALK_BACKWARDS: case anim::SHUFFLE_LEFT:
        case anim::SHUFFLE_RIGHT: case anim::SWIM: case anim::SPRINT:
            ms = std::min(ms, 100.0f);
            break;
        default: break;
    }
    blendMs_ = ms * static_cast<float>(blendPercent) / 100.0f;
    if (blendMs_ < 1.0f) blendFromSeq_ = -1;
}

int CharacterModel::blendPercent = 100;

bool CharacterModel::canPlay(uint32_t animId) {
    if (hasAnimation(animId)) return true;
    if (!skinned_ || !emotePak_ || !emotePak_->isOpen()) return false;
    char name[32];
    std::snprintf(name, sizeof(name), "o_%u_%u_%u", static_cast<unsigned>(look_.race), static_cast<unsigned>(look_.sex),
                  static_cast<unsigned>(animId));
    return emotePak_->find(name) != nullptr;
}

// A loop of an id with variations ended: the original client plays the current
// variation again while it has repeats left (replay min..max), else picks
// the next by frequency (probability x 0x7FFF, variations of Stand = the
// idle fidgets). Upstream CharacterRenderer always plays variation 0.
void CharacterModel::nextVariation() {
    if (repeatsLeft_ > 0) {
        --repeatsLeft_;
        return;
    }
    const SkinView& view = animView();
    const auto* seqs = view.sequences;
    int count = 0;
    for (int i = 0; i < view.h->sequenceCount; ++i)
        if (seqs[i].animId == animId_) ++count;
    if (count < 2) return;
    rng_ = rng_ * 1664525u + 1013904223u;
    int r = static_cast<int>((rng_ >> 8) % 0x7FFFu);
    int pick = -1;
    for (int i = 0; i < view.h->sequenceCount; ++i) {
        if (seqs[i].animId != animId_ || seqs[i].frequency <= 0) continue;
        if (r < seqs[i].frequency) {
            pick = i;
            break;
        }
        r -= seqs[i].frequency;
    }
    if (pick < 0) pick = view.findSequence(animId_);  // frequencies short of 0x7FFF: variation 0
    if (pick != sequence_) {
        startBlend(pick, extraActive_);
        sequence_ = pick;
        poseDirty_ = true;
    }
    const auto& v = seqs[pick];
    if (v.replayMax > v.replayMin) {
        rng_ = rng_ * 1664525u + 1013904223u;
        repeatsLeft_ = static_cast<uint16_t>(v.replayMin + (rng_ >> 8) % (v.replayMax - v.replayMin + 1u));
    } else {
        repeatsLeft_ = v.replayMin;
    }
}

bool CharacterModel::getAnimationState(uint32_t& animId, float& timeMs, float& durationMs) const {
    if (!skinned_ || sequence_ < 0) return false;
    animId = animId_;
    timeMs = animTimeMs_;
    durationMs = static_cast<float>(animView().sequences[sequence_].durationMs);
    return true;
}

// upstream CharacterRenderer::update: advance, wrap a loop by subtraction,
// a finished one-shot plays Stand (looping), Death holds its last frame.
void CharacterModel::update(float dtSec) {
    if (!skinned_ || sequence_ < 0) return;
    const float duration = static_cast<float>(animView().sequences[sequence_].durationMs);
    if (blendFromSeq_ >= 0) {
        blendElapsedMs_ += dtSec * 1000.0f;
        if (blendElapsedMs_ >= blendMs_) {
            blendFromSeq_ = -1;
        } else {
            // The old animation keeps playing while it fades out (wraps if it loops).
            const SkinView& fv = blendFromExtra_ ? extra_ : skin_;
            const float fd = static_cast<float>(fv.sequences[blendFromSeq_].durationMs);
            blendFromTimeMs_ += dtSec * 1000.0f;
            if (fd > 0.0f && blendFromTimeMs_ >= fd) blendFromTimeMs_ = std::fmod(blendFromTimeMs_, fd);
        }
    }
    animTimeMs_ += dtSec * 1000.0f;
    poseDirty_ = true;
    if (duration > 0.0f && animTimeMs_ >= duration) {
        if (animLoop_) {
            animTimeMs_ -= duration;
            while (animTimeMs_ >= duration) animTimeMs_ -= duration;
            nextVariation();
        } else if (animId_ != anim::DEATH) {
            playAnimation(anim::STAND, true);
        } else {
            animTimeMs_ = duration;
        }
    }
}

// The locomotion part of upstream AnimCapabilityProbe::probe, with "has" =
// the pak baked that sequence.
// Upstream AnimCapabilityProbe::probe, body copied as is (it reads the
// desktop CharacterRenderer); "has" = the pak baked that sequence.
rendering::AnimCapabilitySet CharacterModel::capabilities() const {
    namespace anim = rendering::anim;
    rendering::AnimCapabilitySet caps;
    auto has = [&](uint32_t id) -> bool { return hasAnimation(id); };
    auto pick = [&](const uint32_t* candidates, size_t count) -> uint32_t {
        for (size_t i = 0; i < count; ++i)
            if (hasAnimation(candidates[i])) return candidates[i];
        return 0;
    };
    auto pickFrom = [&](anim::MeleeChain kind) -> uint32_t {
        const auto chain = anim::meleeAnimChain(kind);
        return pick(chain.data(), chain.size());
    };
    caps.resolvedStand = anim::STAND;
    caps.hasStand = has(anim::STAND);

    {
        static const uint32_t walkCands[] = {anim::WALK, anim::RUN};
        caps.resolvedWalk = pick(walkCands, 2);
        caps.hasWalk = (caps.resolvedWalk != 0);
    }
    {
        static const uint32_t runCands[] = {anim::RUN, anim::WALK};
        caps.resolvedRun = pick(runCands, 2);
        caps.hasRun = (caps.resolvedRun != 0);
    }
    {
        static const uint32_t sprintCands[] = {anim::SPRINT, anim::RUN, anim::WALK};
        caps.resolvedSprint = pick(sprintCands, 3);
        caps.hasSprint = (caps.resolvedSprint != 0);
    }
    {
        static const uint32_t walkBackCands[] = {anim::WALK_BACKWARDS, anim::WALK};
        caps.resolvedWalkBackwards = pick(walkBackCands, 2);
        caps.hasWalkBackwards = (caps.resolvedWalkBackwards != 0);
    }
    // Note: no resolvedStrafeLeft/Right or resolvedRunLeft/Right here --
    // strafing (pure or diagonal) reuses plain Walk/Run and the renderer's
    // torso-bone rotation (CharacterRenderer::setInstanceTorsoYaw) for the
    // visual angle. See LocomotionFSM::resolve().

    // ── Jump ────────────────────────────────────────────────────────────
    caps.resolvedJumpStart = has(anim::JUMP_START) ? anim::JUMP_START : 0;
    caps.resolvedJump = has(anim::JUMP) ? anim::JUMP : 0;
    caps.resolvedJumpEnd = has(anim::JUMP_END) ? anim::JUMP_END : 0;
    caps.hasJump = (caps.resolvedJumpStart != 0);

    // ── Swim ────────────────────────────────────────────────────────────
    caps.resolvedSwimIdle = has(anim::SWIM_IDLE) ? anim::SWIM_IDLE : 0;
    caps.resolvedSwim = has(anim::SWIM) ? anim::SWIM : 0;
    caps.hasSwim = (caps.resolvedSwimIdle != 0 || caps.resolvedSwim != 0);
    {
        static const uint32_t swimBackCands[] = {anim::SWIM_BACKWARDS, anim::SWIM};
        caps.resolvedSwimBackwards = pick(swimBackCands, 2);
    }
    {
        static const uint32_t swimLeftCands[] = {anim::SWIM_LEFT, anim::SWIM};
        caps.resolvedSwimLeft = pick(swimLeftCands, 2);
    }
    {
        static const uint32_t swimRightCands[] = {anim::SWIM_RIGHT, anim::SWIM};
        caps.resolvedSwimRight = pick(swimRightCands, 2);
    }

    // ── Melee combat ────────────────────────────────────────────────────
    // The chains are in melee_anim_chains.hpp, which the controller that
    // plays them reads too. They were written out in both files, matching,
    // under a comment here saying so.
    caps.resolvedMelee1H = pickFrom(anim::MeleeChain::OneHand);
    caps.resolvedMelee2H = pickFrom(anim::MeleeChain::TwoHand);
    caps.resolvedMelee2HLoose = pickFrom(anim::MeleeChain::TwoHandLoose);
    caps.resolvedMeleeUnarmed = pickFrom(anim::MeleeChain::Unarmed);
    caps.resolvedMeleeFist = pickFrom(anim::MeleeChain::Fist);
    caps.resolvedMeleePierce = pickFrom(anim::MeleeChain::Dagger);
    caps.resolvedMeleeOffHand = pickFrom(anim::MeleeChain::OffHand);
    caps.resolvedMeleeOffHandFist = pickFrom(anim::MeleeChain::OffHandFist);
    caps.resolvedMeleeOffHandPierce = pickFrom(anim::MeleeChain::OffHandPierce);
    caps.resolvedMeleeOffHandUnarmed = pickFrom(anim::MeleeChain::OffHandUnarmed);
    caps.hasMelee = (caps.resolvedMelee1H != 0 || caps.resolvedMeleeUnarmed != 0);

    // ── Ready stances ───────────────────────────────────────────────────
    {
        static const uint32_t ready1HCands[] = {
            anim::READY_1H, anim::READY_2H, anim::READY_UNARMED};
        caps.resolvedReady1H = pick(ready1HCands, 3);
    }
    {
        static const uint32_t ready2HCands[] = {
            anim::READY_2H, anim::READY_2H_LOOSE, anim::READY_1H, anim::READY_UNARMED};
        caps.resolvedReady2H = pick(ready2HCands, 4);
    }
    {
        static const uint32_t ready2HLooseCands[] = {
            anim::READY_2H_LOOSE, anim::READY_2H, anim::READY_1H, anim::READY_UNARMED};
        caps.resolvedReady2HLoose = pick(ready2HLooseCands, 4);
    }
    {
        static const uint32_t readyUnarmedCands[] = {
            anim::READY_UNARMED, anim::READY_1H};
        caps.resolvedReadyUnarmed = pick(readyUnarmedCands, 2);
    }
    {
        // A fist weapon fights unarmed in 3.3.5, which has no fist animations.
        static const uint32_t readyFistCands[] = {
            anim::READY_UNARMED, anim::READY_1H};
        caps.resolvedReadyFist = pick(readyFistCands, 2);
    }
    {
        static const uint32_t readyBowCands[] = {
            anim::READY_BOW, anim::READY_1H, anim::READY_UNARMED};
        caps.resolvedReadyBow = pick(readyBowCands, 3);
    }
    {
        static const uint32_t readyRifleCands[] = {
            anim::READY_RIFLE, anim::READY_BOW, anim::READY_1H, anim::READY_UNARMED};
        caps.resolvedReadyRifle = pick(readyRifleCands, 4);
    }
    {
        // No crossbow stance of its own in 3.3.5: the bow's.
        static const uint32_t readyCrossbowCands[] = {
            anim::READY_BOW, anim::READY_1H, anim::READY_UNARMED};
        caps.resolvedReadyCrossbow = pick(readyCrossbowCands, 3);
    }
    {
        static const uint32_t readyThrownCands[] = {
            anim::READY_THROWN, anim::READY_1H, anim::READY_UNARMED};
        caps.resolvedReadyThrown = pick(readyThrownCands, 3);
    }
    {
        // A wand takes the directed spell stance, not the rifle's.
        static const uint32_t readyWandCands[] = {
            anim::READY_SPELL_DIRECTED, anim::READY_1H, anim::READY_UNARMED};
        caps.resolvedReadyWand = pick(readyWandCands, 3);
    }

    // ── Ranged attacks ──────────────────────────────────────────────────
    {
        static const uint32_t fireBowCands[] = {anim::FIRE_BOW, anim::ATTACK_BOW};
        caps.resolvedFireBow = pick(fireBowCands, 2);
    }
    caps.resolvedAttackRifle = has(anim::ATTACK_RIFLE) ? anim::ATTACK_RIFLE : 0;
    {
        static const uint32_t attackCrossbowCands[] = {anim::ATTACK_BOW};
        caps.resolvedAttackCrossbow = pick(attackCrossbowCands, 1);
    }
    caps.resolvedAttackThrown = has(anim::ATTACK_THROWN) ? anim::ATTACK_THROWN : 0;
    {
        // The shot, matching the stance above.
        static const uint32_t attackWandCands[] = {
            anim::SPELL_CAST_DIRECTED, anim::ATTACK_RIFLE};
        caps.resolvedAttackWand = pick(attackWandCands, 2);
    }
    caps.resolvedLoadBow = has(anim::LOAD_BOW) ? anim::LOAD_BOW : 0;
    caps.resolvedLoadRifle = has(anim::LOAD_RIFLE) ? anim::LOAD_RIFLE : 0;

    // ── Special attacks ─────────────────────────────────────────────────
    caps.resolvedSpecial1H = has(anim::SPECIAL_1H) ? anim::SPECIAL_1H : 0;
    caps.resolvedSpecial2H = has(anim::SPECIAL_2H) ? anim::SPECIAL_2H : 0;
    caps.resolvedSpecialUnarmed = has(anim::SPECIAL_UNARMED) ? anim::SPECIAL_UNARMED : 0;
    caps.resolvedShieldBash = has(anim::SHIELD_BASH) ? anim::SHIELD_BASH : 0;

    // ── Combat idle ─────────────────────────────────────────────────────
    // Base combat idle - weapon-specific stances are resolved per-frame
    // using ready stance fields above
    caps.resolvedCombatIdle = has(anim::READY_1H) ? anim::READY_1H
                            : (has(anim::READY_UNARMED) ? anim::READY_UNARMED : 0);

    // ── Activity animations ─────────────────────────────────────────────
    caps.resolvedStandWound = has(anim::STAND_WOUND) ? anim::STAND_WOUND : 0;
    caps.resolvedDeath = has(anim::DEATH) ? anim::DEATH : 0;
    caps.hasDeath = (caps.resolvedDeath != 0);
    caps.resolvedLoot = has(anim::LOOT) ? anim::LOOT : 0;
    caps.resolvedSitDown = has(anim::SIT_GROUND_DOWN) ? anim::SIT_GROUND_DOWN : 0;
    caps.resolvedSitLoop = has(anim::SITTING) ? anim::SITTING : 0;
    caps.resolvedSitUp = has(anim::SIT_GROUND_UP) ? anim::SIT_GROUND_UP : 0;
    caps.resolvedKneel = has(anim::KNEEL_LOOP) ? anim::KNEEL_LOOP : 0;

    // ── Stealth ─────────────────────────────────────────────────────────
    caps.resolvedStealthIdle = has(anim::STEALTH_STAND) ? anim::STEALTH_STAND : 0;
    caps.resolvedStealthWalk = has(anim::STEALTH_WALK) ? anim::STEALTH_WALK : 0;
    caps.resolvedStealthRun = has(anim::STEALTH_RUN) ? anim::STEALTH_RUN : 0;
    caps.hasStealth = (caps.resolvedStealthIdle != 0);

    // ── Misc ────────────────────────────────────────────────────────────
    caps.resolvedMount = has(anim::MOUNT) ? anim::MOUNT : 0;
    caps.hasMount = (caps.resolvedMount != 0);
    // The same reach as sheathing: 3.3.5 has no animation of its own for
    // drawing a weapon.
    caps.resolvedUnsheathe = has(anim::SHEATHE) ? anim::SHEATHE
                           : has(anim::HIP_SHEATHE) ? anim::HIP_SHEATHE : 0;
    {
        static const uint32_t sheatheCands[] = {anim::SHEATHE, anim::HIP_SHEATHE};
        caps.resolvedSheathe = pick(sheatheCands, 2);
    }
    caps.resolvedStun = has(anim::STUN) ? anim::STUN : 0;
    caps.resolvedCombatWound = has(anim::COMBAT_WOUND) ? anim::COMBAT_WOUND : 0;

    return caps;
}

// Bone matrices for this frame (psp_skeleton.hpp), with the vertex
// dequantisation folded in, as sceGuBoneMatrix takes them.
void CharacterModel::evaluatePose() {
    PROF_SCOPE("char.anim");
    if (blendFromSeq_ >= 0 && blendMs_ > 0.0f) {
        rendering::psp::evaluatePose(animView(), sequence_, animTimeMs_, pose_.data(),
                                     blendFromExtra_ ? &extra_ : &skin_, blendFromSeq_, blendFromTimeMs_,
                                     blendElapsedMs_ / blendMs_);
    } else {
        rendering::psp::evaluatePose(animView(), sequence_, animTimeMs_, pose_.data());
    }
    for (uint32_t i = 0; i < pose_.size(); ++i) {
        float g[12];
        geBoneMatrix(pose_[i], *skin_.h, g);
        ScePspFMatrix4& m = geBones_[i];
        m.x = {g[0], g[1], g[2], 0.0f};
        m.y = {g[3], g[4], g[5], 0.0f};
        m.z = {g[6], g[7], g[8], 0.0f};
        m.w = {g[9], g[10], g[11], 1.0f};
    }
    poseDirty_ = false;
}

void CharacterModel::setBatchState(const ModelBatch& b, const pp::Texture* tex, bool fade, uint32_t fixSrc,
                                   uint32_t fixDst) {
    pp::textureBind(tex);
    sceGuTexWrap((b.flags & kBatchWrapU) ? GU_REPEAT : GU_CLAMP, (b.flags & kBatchWrapV) ? GU_REPEAT : GU_CLAMP);
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
}

void CharacterModel::drawPass(bool blendPass, float alpha) {
    const bool fade = alpha < 0.999f;
    const uint32_t a = static_cast<uint32_t>(alpha * 255.0f + 0.5f);
    const uint32_t fixSrc = 0xFF000000u | (a << 16) | (a << 8) | a;
    const uint32_t fixDst = 0xFF000000u | ((255 - a) << 16) | ((255 - a) << 8) | (255 - a);
    for (uint32_t i = 0; i < batchCount_; ++i) {
        if (!active_[i]) continue;
        const ModelBatch& b = batches_[i];
        if ((b.blend >= kBlendAlpha) != blendPass) continue;
        const bool cape = cape_ && cape_->pixels && geo_ && geo_[i] / 100 == 15;
        setBatchState(b, cape ? cape_ : texture(b.texture), fade, fixSrc, fixDst);
        if (skinned_) {
            // One strip per sub-batch; a GE bone slot is re-sent only when it
            // holds another bone (the matrices do not change within a frame).
            for (uint32_t k = 0; k < b.indexCount; ++k) {
                const SkinSubBatch& sb = skin_.subBatches[b.indexStart + k];
                for (int j = 0; j < sb.weights; ++j) {
                    if (slotBone_[j] == sb.palette[j]) continue;
                    sceGuBoneMatrix(static_cast<unsigned>(j), &geBones_[sb.palette[j]]);
                    slotBone_[j] = sb.palette[j];
                    ++boneUploads_;
                }
                prof::count(prof::kDraws, 1);
                prof::count(prof::kVertices, sb.vertexCount);
                prof::count(prof::kTriangles, sb.vertexCount - 2u);
                sceGumDrawArray(GU_TRIANGLE_STRIP, skinnedType(sb.weights), sb.vertexCount, nullptr,
                                skin_.vertices(sb));
            }
        } else {
            prof::count(prof::kDraws, 1);
            prof::count(prof::kVertices, b.indexCount);
            prof::count(prof::kTriangles, (h_->flags & kModelStrips) ? b.indexCount - 2 : b.indexCount / 3);
            if (h_->flags & kModelStrips)
                sceGumDrawArray(GU_TRIANGLE_STRIP, kStripType, static_cast<int>(b.indexCount), nullptr,
                                verts_ + b.indexStart);
            else
                sceGumDrawArray(GU_TRIANGLES, kVertexType, static_cast<int>(b.indexCount), indices_ + b.indexStart,
                                verts_);
        }
        ++batchesDrawn_;
    }
}

void CharacterModel::draw(const TerrainCamera& cam, const glm::vec3& pos, float yaw, uint32_t fogColorABGR,
                          float alpha) {
    PROF_SCOPE("char");
    PROF_GE_PASS("char");
    batchesDrawn_ = 0;
    boneUploads_ = 0;
    if (!loaded()) return;
    // other players are dressed from the DBCs now (body, cloak, up to five item
    // models each) - in a crowd they stop before the end of the GE list like the NPC pass (the list's reserves,
    // psp_display.hpp); the own player and the select screen's model are always drawn.
    if (!localPlayer_ && pp::displayListFree() < pp::kListReserveNpc) {
        pp::displayListFull("npc");
        return;
    }
    const float c = std::cos(yaw), s = std::sin(yaw);
    const glm::vec3 ax(c, s, 0.0f), ay(-s, c, 0.0f), az(0.0f, 0.0f, 1.0f);
    ScePspFMatrix4 m;
    float uvScale[2], uvMin[2];
    if (skinned_) {
        // The bone matrices carry the dequantisation: the model matrix only
        // places the feet and turns the model.
        if (poseDirty_) evaluatePose();
        for (int& b : slotBone_) b = -1;
        const glm::vec3 t = pos - cam.pos;
        m.x = {ax.x, ax.y, 0.0f, 0.0f};
        m.y = {ay.x, ay.y, 0.0f, 0.0f};
        m.z = {0.0f, 0.0f, 1.0f, 0.0f};
        m.w = {t.x, t.y, t.z, 1.0f};
        for (int k = 0; k < 2; ++k) {
            uvScale[k] = skin_.h->uvScale[k];
            uvMin[k] = skin_.h->uvMin[k];
        }
    } else {
        const ModelHeader& h = *h_;
        const glm::vec3 t = pos + ax * h.center[0] + ay * h.center[1] + az * h.center[2] - cam.pos;
        m.x = {ax.x * h.half[0], ax.y * h.half[0], 0.0f, 0.0f};
        m.y = {ay.x * h.half[1], ay.y * h.half[1], 0.0f, 0.0f};
        m.z = {0.0f, 0.0f, h.half[2], 0.0f};
        m.w = {t.x, t.y, t.z, 1.0f};
        for (int k = 0; k < 2; ++k) {
            uvScale[k] = h.uvScale[k];
            uvMin[k] = h.uvMin[k];
        }
    }
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadMatrix(&m);

    pp::textureResetBinding();
    if (texFlush_) {  // setEquipment rewrote the body texture at the same address
        sceGuTexFlush();
        texFlush_ = false;
    }
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuShadeModel(GU_SMOOTH);
    lightBegin(cam, fogColorABGR);  // Lighting (fog from the bands, light tint)
    sceGuTexScale(uvScale[0], uvScale[1]);
    sceGuTexOffset(uvMin[0], uvMin[1]);
    // Opaque pass. While fading: first depth only (colour writes masked), then
    // the colour blended with fixed factors against that depth, so only the
    // front surface shows - without the prepass the inside and the back of
    // the (two-sided) model showed through.
    if (alpha < 0.999f) {
        sceGuDisable(GU_BLEND);
        sceGuPixelMask(0xFFFFFFFFu);
        drawPass(false, 1.0f);
        sceGuPixelMask(0);
        sceGuEnable(GU_BLEND);
        sceGuDepthMask(GU_TRUE);
        sceGuDepthFunc(GU_EQUAL);
        drawPass(false, alpha);
        sceGuDepthFunc(GU_GEQUAL);
    } else {
        sceGuDisable(GU_BLEND);
        drawPass(false, alpha);
    }
    sceGuDisable(GU_ALPHA_TEST);
    sceGuEnable(GU_BLEND);
    sceGuDepthMask(GU_TRUE);
    drawPass(true, alpha);
    sceGuDepthMask(GU_FALSE);
    sceGuDisable(GU_BLEND);
    // Attached item models on this frame's pose (the static model:
    // the points' Stand matrices). Character model matrix without the vertex
    // dequantisation: turn by yaw, feet at pos (relative to the camera).
    if (attach_.count()) {
        BoneMatrix cm;
        const glm::vec3 t = pos - cam.pos;
        const float mm[12] = {ax.x, ax.y, 0.0f, ay.x, ay.y, 0.0f, 0.0f, 0.0f, 1.0f, t.x, t.y, t.z};
        std::memcpy(cm.m, mm, sizeof(mm));
        const float camPos[3] = {cam.pos.x, cam.pos.y, cam.pos.z};  // the glows' world matrices
        attach_.draw(cm, skinned_ ? pose_.data() : nullptr, skinned_ ? static_cast<uint32_t>(pose_.size()) : 0u, alpha,
                     camPos);
    }
    lightEnd();  // Lighting
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    pp::textureResetBinding();
}


// attachcap's crowd policy (psp_character.hpp) - what is worn attached again, with
// the policy of this moment (WorldScreen::syncRemotePlayers decides when), and what the last attach left off.
void CharacterModel::refreshAttachments() {
    if (loaded()) attachEquipment(attachItems_);
}

uint32_t CharacterModel::attachmentsCapped() const { return attachCapSkipped(attach_); }

} // namespace wowee::rendering::psp
