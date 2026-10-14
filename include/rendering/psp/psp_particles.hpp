// Particles: M2 particle systems on the PSP - one engine for the spell effects
// (psp_spellfx.cpp) and the world's emitters (doodads, game objects), plus the weather
// (rain and snow).
//
// Upstream logic (src/rendering/m2_renderer_particles.cpp, m2_renderer_render.cpp, weather.cpp):
//  - emission / motion: M2Renderer::emitParticles + updateParticles - rate (x upstream's particle
//    density), the flame floor (15 live), the emission point bone * position through the model
//    matrix, the direction "up + spread" through mat3(model * bone) x speed, the drift when the speed is
//    ~0 (fireflies: their own), random / animated texture tiles, the accumulator dropped past 2,
//    gravity on world z (0 -> 4 or 1.5; none for fireflies);
//  - the look: renderM2Particles - FBlock colour / alpha / scale over the life, the per-model rules
//    (spell effect scale floor, generic whitening, flame floors), blend 3/4 additive, 1 alpha key;
//    the fragment's colour x alpha (premultiplied) under SRC_ALPHA blending;
//  - upstream's procedural smoke of isSmoke models (M2Renderer::update, renderSmokeParticles);
//  - ribbons: updateRibbons + renderM2Ribbons;
//  - weather: Weather::update / updateParticle (rain, snow, storm).
// PSP (budgets, the GE; every one a wowee.cfg key):
//  - one particle pool for both users, allocated once by the first (no allocation in the frame):
//    spell effects get `spellfxsprites` of it, the world's emitters `particlesprites`; each user
//    caps an emitter's live particles (its share) and lowers its rate to share / lifespan;
//  - sprites are GE rectangles (GU_SPRITES, 2 vertices) camera-relative, upstream's point size
//    (aSize x 500 / distance px at 1080 lines = 0.47 yd x scale here) capped at `maxPx`;
//  - ribbons keep 64 edges (upstream 128) in a fixed set of lanes;
//  - every draw keeps kListReserveFx bytes of the display list free (the portraits and the
//    interface come later); a draw that does not fit is dropped and counted (ge_list ... fx=);
//  - no fog on particles (upstream fades them with the fog: at <= 60 yd there is none).
// The engine itself (pool, owners, lanes, the level-3 rules) is psp_particles_l3.hpp (clean-room); this
// file's older rules (upstream's and the port's first ones) run on its pool through FxLegacyRules (psp_particles.cpp).
#pragma once

#include "rendering/psp/psp_particle_format.hpp"
#include "rendering/psp/psp_particle_sim.hpp"
#include "rendering/psp/psp_particles_l3.hpp"  // the engine (level-3 rules, portable)
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

// Bytes of the display list the particle passes leave free (the target portrait, the paper doll and
// the interface draw after them; psp_display.hpp: M2 < 160 KB, NPC < 96 KB, UI < 8 KB).
constexpr uint32_t kListReserveFx = 64 * 1024;

// Pool owners: spell effect instances 0..15, the world's emitter instances 64..127.
constexpr uint8_t kSpellOwnerBase = 0, kSpellOwnerEnd = 16;
constexpr uint8_t kWorldOwnerBase = 64, kWorldOwnerEnd = 128;
// weapons: the weapons' glow effects (psp_weapon_fx.hpp) 16..63, their share of the pool
// wowee.cfg enchantsprites (96; enchantfx=0: none).
constexpr uint8_t kWeaponOwnerBase = 16, kWeaponOwnerEnd = 64;
constexpr int kFxSmokeEmitter = kFxMaxEmitters;      // upstream's procedural smoke (kPtcSmoke)
constexpr int kFxSparkEmitter = kFxMaxEmitters + 1;  // its sparks
constexpr int kFxEmitterSlots = kFxMaxEmitters + 2;
constexpr int kRibbonEdges = ParticleEngine::kRingEdges;  // PSP: upstream keeps 128

// A texture of the user's cache by its "x_<n>" id (nullptr: draw white / skip).
using FxTextureFn = FxTexLookup;

// The engine of the three users (spell effects, the world's emitters, the weapons' glows): acquireShared() /
// releaseShared() (reference-counted; the first reads wowee.cfg spellfx / spellfxsprites, particles /
// particlesprites, enchantfx / enchantsprites, particleribbons and ptclevel3 - alias ptcclient - and allocates
// the pool once), then step() / draw() per user and frame (psp_particles_l3.hpp).
ParticleEngine& particleEngine();

// The soft dot (upstream's procedural point sprites: smoke, sparks, weather), 32 x 32 T8 in BSS
// (2 KB, made once, never freed - the weather draws it without the pool).
const platform::psp::Texture* fxDotTexture();

// ---------------------------------------------------------------------------------------------
// The world's emitters: doodads (data/doodads_<map>.pak) and game objects (data/gameobjects.pak),
// both from their pak's "pidx" entry (psp_particle_format.hpp), read through one descriptor of its
// own, opened when needed and closed after 3 s without reads (the card's open-file limit).

struct WorldFxConfig {
    int maxInstances = 24;       // particleinst: emitter models simulated at once (nearest first)
    float distance = 50.0f;      // particledist: yd from the camera to the model (+ its reach)
    float lodDistance = 20.0f;   // particlelod: full density inside, 1/4 at `distance`
    int emitterCap = 32;         // particleemit: live particles per emitter (x LOD)
    int modelCap = 64;           // particlemodel: live particles per model, split over its emitters (x LOD)
    uint32_t cacheBytes = 192u * 1024u;  // particlekb: effects + textures
    int maxPx = 32;              // particlepx: upstream caps a point at 128 px of 1080 lines
    float density = 1.0f;        // particledensity / 100: upstream's particle density setting
    bool ribbons = true;         // ribbons
};

// A game object with a particle effect, as WorldScreen hands them over (every 0.5 s).
struct WorldFxObject {
    uint32_t uniqueId = 0;
    uint32_t displayId = 0;
    float position[3] = {0, 0, 0};
    float axes[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
};

class WorldParticles {
public:
    WorldParticles();
    ~WorldParticles();
    WorldParticles(const WorldParticles&) = delete;
    WorldParticles& operator=(const WorldParticles&) = delete;

    // The paks' directories ("pidx"); false when neither has one (then nothing is kept).
    bool open(const WorldFxConfig& cfg, const char* doodadsPak, const char* gameObjectsPak);
    bool isOpen() const { return files_[0].ok || files_[1].ok; }
    void close();
    // The game objects in sight (displays without an effect are skipped here).
    void setObjects(const std::vector<WorldFxObject>& objects);
    // Per frame, after the other 3D draws: the tiles around the camera, the nearest emitter models,
    // their particles / ribbons / meshes, the GE pass.
    void frame(const TerrainCamera& cam, float dtSec);
    uint32_t indexBytes() const;

private:
    struct File {
        bool ok = false;
        std::string path;
        uint32_t entryOffset = 0, entrySize = 0;
        std::vector<uint32_t> dir;  // the directory (4-aligned copy)
        const ParticleIndexHeader* h = nullptr;
        const ParticleTileRec* tiles = nullptr;
        const ParticleDisplayRec* displays = nullptr;
        const ParticleEffectRec* effects = nullptr;
        const ParticleTextureRec* textures = nullptr;
    };
    struct Candidate {
        uint64_t key;      // file << 32 | uniqueId
        uint16_t effect;
        uint8_t file;
        float pos[3];
        float axes[9];
        float scale;
        float reach;       // world
    };
    struct Loaded;
    struct Tex;
    struct Slot;
    bool openFile(File& f, const char* path);
    bool read(int file, uint32_t offset, void* dst, uint32_t size);
    void closeIdle(uint32_t nowMs);
    void updateTiles(const glm::vec3& camPos);
    void rebuildCandidates();
    Loaded* effect(int file, uint16_t id, bool allowLoad);
    const platform::psp::Texture* texture(int file, uint16_t id);
    static const platform::psp::Texture* texFn(void* user, uint16_t id);
    bool makeRoom(uint32_t bytes);
    void releaseSlot(Slot& s);
    void log(uint32_t nowMs);

    WorldFxConfig cfg_;
    File files_[2];  // 0 doodads, 1 game objects
    int fd_ = -1, fdFile_ = -1;
    uint32_t lastReadMs_ = 0;
    struct TileData {
        int16_t x, y;
        std::vector<ParticlePlacement> list;
    };
    std::vector<TileData> tiles_;
    int tileX_ = -10000, tileY_ = -10000;
    std::vector<Candidate> candidates_;  // doodads of the loaded tiles, then the game objects
    size_t objectStart_ = 0;
    std::vector<WorldFxObject> objects_;
    bool candidatesDirty_ = true;
    std::unordered_map<uint32_t, std::unique_ptr<Loaded>> loaded_;  // file << 16 | effect
    std::unordered_map<uint32_t, std::unique_ptr<Tex>> textures_;   // file << 16 | texture
    std::unordered_set<uint32_t> broken_;                           // effects that failed (not read again)
    uint32_t cacheBytes_ = 0;
    std::unique_ptr<Slot[]> slots_;
    std::vector<FxInstance> frameList_;   // reserved at open: no allocation in the frame
    std::vector<int> frameSlot_;
    struct Pick {
        float d2;
        uint32_t c;
    };
    std::vector<Pick> picks_;
    uint32_t frame_ = 0;
    float clockMs_ = 0.0f;
    // Stats (the 10 s line).
    uint32_t nextLogMs_ = 0, loads_ = 0, loadFails_ = 0, evicts_ = 0, tileLoads_ = 0, logs_ = 0;
    uint32_t usSum_ = 0, usMax_ = 0, usFrames_ = 0, peakLive_ = 0;
    FxFrameStats last_;
    uint32_t lastActive_ = 0, lastCandidates_ = 0;
};

// ---------------------------------------------------------------------------------------------
// Weather (upstream rendering::Weather over the server's SMSG_WEATHER).

enum class WeatherKind : uint8_t { None, Rain, Snow, Storm, Sand };

struct WeatherConfig {
    int maxDrops = 300;      // weathermax (upstream MAX_PARTICLES 2000 in a 200 yd box)
    float range = 25.0f;     // weatherrange: the box around the camera (upstream 100)
};

class WeatherFx {
public:
    ~WeatherFx();
    void configure(const WeatherConfig& cfg);
    // upstream setWeatherType / setIntensity (0..1) / setEnabled; `dark`: black rain / snow (PSP colour).
    void set(WeatherKind kind, float intensity, bool dark);
    void setEnabled(bool on) { enabled_ = on; }
    WeatherKind kind() const { return kind_; }
    float intensity() const { return intensity_; }
    int drops() const { return count_; }
    uint32_t memoryBytes() const { return static_cast<uint32_t>(capacity_) * sizeof(Drop); }
    // upstream Weather::update + render, per frame after the other 3D draws.
    void frame(const TerrainCamera& cam, float dtSec, uint32_t& usOut);

private:
    struct Drop {
        glm::vec3 pos, vel;
        float life, maxLife;
    };
    void spawn(Drop& d, const glm::vec3& cam, bool anywhere);
    float rnd01();
    WeatherConfig cfg_;
    WeatherKind kind_ = WeatherKind::None;
    float intensity_ = 0.0f;
    bool dark_ = false, enabled_ = true;
    Drop* drops_ = nullptr;
    int capacity_ = 0, count_ = 0;
    uint32_t rng_ = 0x9E3779B9u;
};

} // namespace wowee::rendering::psp
