// Spell effects on the PSP - upstream SpellVisualSystem
// (src/rendering/spell_visual_system.cpp) over data/spellfx.pak (psp_spellfx_format.hpp,
// tools/psp/converter/psp_spellfx_conv.cpp).
//
// Upstream logic kept: the visual -> precast / cast / impact effect chain and its fallbacks
// (no precast kit -> the cast kit; no cast kit -> the impact kit and back), the attachment from
// the effect's name (hand + a mirror copy on the left hand, chest, head; the caster's bone is
// followed every frame; an untracked caster gets the height offset +0.8 / +1.0 / +1.6), the
// lifetimes (precast: the cast time 0.5..30 s when >= 500 ms, else the model's sequence 0
// duration 0.5..5 s, 2 s without one; cast / impact: the duration 0.5..5 s, 2 s), the effect
// loops sequence 0 from time 0, cancelAllPrecastVisuals, and the particle rules of
// M2Renderer::emitParticles / updateParticles / renderM2Particles for a spell effect.
//
// PSP (all off unless wowee.cfg spellfx=1|2; nothing allocated at 0):
// - Budgets: at most `maxActive` effects at once (spellfxmax, 4) - a new one replaces another
//   caster's before an own one, the one nearest its end first; `maxSprites` live particles in
//   all (spellfxsprites, 96), shared evenly between the effects and their emitters (an
//   emitter's rate is lowered to its share / lifespan); sprites at most `maxPx` pixels on
//   screen (spellfxpx, 64); effect models + textures in a cache of `cacheBytes` (spellfxkb,
//   384 KB): loaded on the first cast that needs them (one read per effect + one per new
//   texture), least recently used first out, never one drawn in the last 2 frames.
// - The pak is read through its own descriptor, closed after 3 s without reads (the card's
//   open-file limit); only the visual table and the entry offsets stay in RAM (~13 KB).
// - Mesh rigid per bone, curves sampled (see psp_spellfx_format.hpp); no fog on the effects, no
//   glow-card sprites; sprites are GE rectangles (GU_SPRITES).
// Particles: the emission, motion and drawing (meshes, sprites, and now the
//   ribbons of a v2 blob) are the shared ParticleEngine's (psp_particles.cpp): one particle pool with
//   the world's emitters, this user's share = spellfxsprites; the display list reserve
//   (kListReserveFx) applies here too. What stays here: the visual table, the cache, the events,
//   the lifetimes, the followed bones.
// Profiler: zones "spellfx", "spellfx.load"; GE pass "spellfx".
// Log: spellfx on ..., spellfx_load ..., spellfx_missing visual=, spellfx_full ..., every 10 s
// while effects run: spellfx_frame active= sprites= peak= kb= loads= evict= us_avg= us_max=;
// spellfx_prefetch visuals= effects= (the action bar, once).
#pragma once

#include "rendering/psp/psp_particles.hpp"  // particles: the shared engine
#include "rendering/psp/psp_spellfx_format.hpp"
#include "rendering/psp/psp_terrain.hpp"  // TerrainCamera

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee::platform::psp {
struct Texture;
}

namespace wowee::rendering::psp {

struct SpellFxConfig {
    uint32_t cacheBytes = 384u * 1024u;
    int maxActive = 4;
    int maxSprites = 96;
    int maxPx = 64;
    bool premultiply = false;  // particles: wowee.cfg spellfxpremul (1 = upstream's colour x alpha; 0 = straight colour)
};

// What the effects ask the world about a caster (upstream CharacterRenderer::getAttachmentTransform).
class SpellFxHost {
public:
    // World position (server coordinates) of M2 attachment `id` (1, 2, 5, 11) of a tracked
    // caster on this frame's pose; false when the unit is gone or has no such point.
    virtual bool attachmentWorld(uint64_t guid, uint8_t id, glm::vec3& out) = 0;

protected:
    ~SpellFxHost() = default;
};

class SpellFxRenderer {
public:
    SpellFxRenderer();  // in the .cpp: Loaded / TexEntry are complete there
    ~SpellFxRenderer();
    SpellFxRenderer(const SpellFxRenderer&) = delete;
    SpellFxRenderer& operator=(const SpellFxRenderer&) = delete;

    // Reads the pak's visual table and entry offsets; false without data/spellfx.pak.
    bool open(const SpellFxConfig& cfg);
    bool isOpen() const { return !visuals_.empty(); }

    // upstream SpellVisualSystem::playSpellVisualPrecast. feet: the caster's position (server
    // coordinates); caster: the guid to follow (0 = an untracked caster: static, height offset).
    void playPrecast(uint32_t visualId, const glm::vec3& feet, uint32_t castMs, uint64_t caster, bool own,
                     SpellFxHost& host);
    // upstream SpellVisualSystem::playSpellVisual (impact = the ImpactKit path, never tracked).
    void play(uint32_t visualId, const glm::vec3& feet, bool impact, uint64_t caster, bool own, SpellFxHost& host);
    void cancelAllPrecast();
    void reset();
    // PSP: the effects of a visual read ahead (the action bar at world entry), one per frame while
    // the cache is under 3/4 full - the first cast of a bar spell then reads nothing.
    void prefetch(uint32_t visualId);

    bool hasVisual(uint32_t visualId) const { return findVisual(visualId) != nullptr; }
    size_t activeCount() const { return active_.size(); }

    // Per frame, after every other 3D draw: lifetimes, followed bones, particles, the GE pass.
    void frame(const TerrainCamera& cam, float dtSec, SpellFxHost& host);

private:
    struct Loaded;
    struct TexEntry;
    struct Instance {
        uint16_t effect = kFxNone;  // "e_<n>"
        uint32_t visual = 0;
        float elapsed = 0.0f, duration = 1.0f;  // s
        bool precast = false, own = false;
        uint8_t attach = 0;          // followed attachment (0 = static)
        uint64_t caster = 0;
        glm::vec3 pos{0.0f};         // instance origin, server coordinates
        float acc[kFxEmitterSlots] = {};  // particles: the engine's accumulators
        int16_t lanes[kFxMaxRibbons] = {-1, -1, -1, -1};  // particles: ribbon lanes
        uint32_t serial = 0;
    };

    const SpellFxVisual* findVisual(uint32_t visualId) const;
    Loaded* load(uint16_t effect, uint32_t visualId, const char* kind);
    const platform::psp::Texture* texture(uint16_t id);
    bool readTexture(uint16_t id, TexEntry& te);
    bool readRaw(uint32_t offset, void* dst, uint32_t size);
    void closeIdle(uint32_t nowMs);
    bool makeRoom(uint32_t bytes);
    void spawn(uint16_t effect, uint32_t visualId, const glm::vec3& pos, float duration, bool precast, bool own,
               uint8_t attach, uint64_t caster);
    void removeAt(size_t i);
    void log(uint32_t nowMs);
    static const platform::psp::Texture* texFn(void* user, uint16_t id);  // particles: the engine's lookup

    SpellFxConfig cfg_;
    std::string path_;
    int fd_ = -1;
    uint32_t lastReadMs_ = 0;
    std::vector<SpellFxVisual> visuals_;          // sorted by visual id
    std::vector<uint32_t> effOff_, effSize_;      // "e_<n>" in the file
    std::vector<uint32_t> texOff_, texSize_;      // "x_<n>"
    std::unordered_map<uint16_t, std::unique_ptr<Loaded>> loaded_;
    std::unordered_map<uint16_t, std::unique_ptr<TexEntry>> textures_;
    std::unordered_set<uint32_t> missingLogged_;
    std::vector<uint16_t> prefetch_;  // effects to read ahead (last first)
    uint32_t prefetched_ = 0;
    uint32_t cacheBytes_ = 0;
    std::vector<Instance> active_;
    bool engine_ = false;  // particles: holds the shared engine (particle pool) while open
    uint32_t frame_ = 0, serial_ = 0;
    // Stats (the 10 s log line).
    uint32_t loads_ = 0, evictFx_ = 0, evictCache_ = 0, full_ = 0, logs_ = 0;
    uint32_t usSum_ = 0, usMax_ = 0, usFrames_ = 0, spritesPeak_ = 0, spritesLast_ = 0, draws_ = 0;
    uint32_t nextLogMs_ = 0, lastActiveMs_ = 0, memLogs_ = 0;
};

} // namespace wowee::rendering::psp
