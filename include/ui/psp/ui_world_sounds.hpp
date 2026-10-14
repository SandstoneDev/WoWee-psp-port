// Sounds in the world - upstream's UiSoundManager,
// FootstepManager (+ FootstepDriver), CombatSoundManager,
// ActivitySoundManager (+ SfxStateDriver) and MovementSoundManager over
// data/sounds.pak (platform/psp/psp_sound_bank.hpp), played by the effects
// mixer of platform/psp/psp_audio. Logic and numbers are upstream's
// (src/audio/*.cpp, src/rendering/animation/footstep_driver.cpp,
// sfx_state_driver.cpp); PSP differences are marked "PSP:" in the .cpp and
// listed in docs/PSP_PORT_ARCHITECTURE.md.
//
// wowee.cfg: sfx (1; 0 = no effects, sounds.pak not read), sfxvol (0..100,
// 100; the graphics menu row SOUND_VOLUME), footsteps (0 off, 1 own only =
// upstream, 2 = + other players and NPCs within 25 yd, default 2),
// sfxbudget (KB of PCM kept in RAM, 1024).
#pragma once

#include "platform/psp/psp_sound_bank.hpp"
#include "ui/psp/ui_world_sound_paths.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace wowee::ui::psp {

// --- API for the rest of the world code (orchestrator wires loot, vendor,
// quest and inventory events to these). No-ops while sounds are off. ---
// upstream UiSoundManager::play<Name>().
void playUiSound(UiSound s);
// upstream UiSoundManager::playByName (FrameXML PlaySound("igQuestFailed")):
// the first baked file of the SoundEntries row with that Name. False when
// the name or its files are not in sounds.pak (the caller may fall back).
bool playSoundByName(const char* name);
// A SoundEntries row by id at a world position (server coords), 3D like
// upstream AudioEngine::playSound3D (inverse distance from 1 yd, max 100 yd,
// panned to the camera), volume = the row's Volume; a random baked file of
// the row. playSound2D: the same without position.
bool playSound3D(uint32_t soundEntryId, const glm::vec3& serverPos);
bool playSound2D(uint32_t soundEntryId);

// Upstream audio::FootstepSurface (psp_sound_format.hpp SoundSurface order).
enum class StepSurface : uint8_t { Stone = 0, Dirt, Grass, Wood, Metal, Water, Snow };

class WorldSounds {
public:
    WorldSounds() = default;
    ~WorldSounds();
    WorldSounds(const WorldSounds&) = delete;
    WorldSounds& operator=(const WorldSounds&) = delete;

    // World entry: wowee.cfg keys, sounds.pak (grids of mapId), the banks
    // resolved for the own race/sex (ActivitySoundManager voice profile).
    bool init(uint32_t mapId, uint32_t race, bool male);
    void shutdown();
    bool active() const { return bank_.isOpen(); }
    int footstepMode() const { return footsteps_; }
    void setFootstepMode(int m) { footsteps_ = m < 0 ? 0 : (m > 2 ? 2 : m); }
    const platform::psp::SoundBankStats& bankStats() const { return bank_.stats(); }
    // Debug window "Sound: test": the n-th test sound (UI sounds, then a step,
    // a swing, a hit); returns its name.
    const char* playTest(uint32_t n);

    // Once per frame, before anything plays: loads that arrived, the category
    // volume (0..100), the camera for 3D sounds (server coords, orbit yaw).
    void beginFrame(float dtSec, int volumePercent, const glm::vec3& listener, float listenerYaw);

    // UiSoundManager.
    void playUi(UiSound s);
    bool playByName(const char* name);
    bool playEntry(uint32_t soundEntryId, const glm::vec3* serverPos);

    // CombatSoundManager + ActivitySoundManager vocals: upstream
    // CombatHandler::handleAttackerStateUpdate's sound block (player's
    // combat only). hitInfo / victimState as SMSG_ATTACKERSTATEUPDATE.
    void onMeleeHit(bool playerAttacker, bool playerTarget, uint32_t hitInfo, uint32_t victimState);

    // SfxStateDriver::update over the own movement state (upstream
    // CameraController isGrounded/isJumping/isFalling/isSwimming/isMoving).
    void updateActivity(float dtSec, bool grounded, float verticalVelocity, bool swimming, bool moving,
                        StepSurface surface);

    // FootstepDriver: the own character's clock (upstream
    // shouldTriggerFootstepEvent: $FSD times, else the biped 0.22/0.72
    // beats). footstepState = LocomotionFSM WALK or RUN.
    void ownFootsteps(bool canPlay, bool footstepState, uint32_t animId, float timeMs, float durationMs,
                      const uint32_t* events, uint32_t eventCount, bool sprinting, StepSurface surface);
    // PSP: others (players, NPCs) - authored $FSD events only, 3D.
    struct Walker {
        uint64_t guid;
        glm::vec3 pos;
        uint32_t animId;
        float timeMs, durationMs;
        const uint32_t* events;
        uint32_t eventCount;
        bool moving;
        bool player;
    };
    // `surfaceOf` resolves a walker's surface only when a step fires.
    template <class SurfaceFn>
    void otherFootsteps(const Walker* w, uint32_t n, SurfaceFn&& surfaceOf) {
        for (uint32_t i = 0; i < n; ++i)
            if (otherStepDue(w[i])) playOtherStep(w[i], surfaceOf(w[i]));
        sweepTrackers();
    }

    // 3D gain/pan of a world position (upstream inverse attenuation).
    bool spatial(const glm::vec3& serverPos, float maxDistance, float& gain, float& pan) const;
    // Terrain part of upstream resolveFootstepSurface at a position (server
    // coords): water (MH2O under the feet) or the dominant ground layer's
    // surface, from the baked grids. False when the tile has no grid.
    bool surfaceAt(float x, float y, float z, StepSurface& out) const;
    // flying: the terrain water (MH2O) at a position - the cell is covered and the surface is
    // its chunk's highest liquid level (the same baked grid). False: no water there / no grid for the tile.
    bool waterAt(float x, float y, float& level) const;
    float otherDistance = 25.0f;  // yd, footsteps of others

private:
    struct Tracker {  // upstream FootstepDriver's footstepLast* state
        uint32_t lastAnimationId = 0;
        float lastNormTime = 0.0f;
        bool initialized = false;
        uint32_t seenFrame = 0;
        bool trigger(uint32_t animationId, float timeMs, float durationMs, const uint32_t* ev, uint32_t n,
                     bool genericBeats);
    };
    bool otherStepDue(const Walker& w);
    void playOtherStep(const Walker& w, StepSurface surface);
    void sweepTrackers();
    int ui(UiSound s) const;
    std::vector<int> resolve(const std::vector<std::string>& paths) const;
    int first(const std::vector<std::string>& paths) const;  // upstream library[0]: the first path only
    bool play2D(int clip, float volume, float pitch = 1.0f);
    int pick(const std::vector<int>& clips);
    float rnd(float lo, float hi);
    bool elapsed(uint32_t& lastMs, float minSec, uint32_t now) const;
    void playRandomStep(StepSurface surface, float minInterval, float volumeMul);
    void playWaterFootstep(int size);  // MovementSoundManager, 0 small 1 medium 2 large/giant
    void playJump();
    void playLanding(StepSurface surface, bool hardLanding);
    void playMeleeSwing();
    void playAttackGrunt();
    void playWound(bool crit);

    platform::psp::SoundBank bank_;
    const void* entries_ = nullptr;
    uint32_t entriesSize_ = 0;
    std::unordered_map<uint32_t, uint32_t> entryById_, entryByName_;
    int footsteps_ = 2;
    int volume_ = -1;
    glm::vec3 listener_{0.0f};
    float listenerYaw_ = 0.0f;
    uint32_t rng_ = 0x9E3779B9u;
    uint32_t frame_ = 0;

    // UiSoundManager
    int uiClip_[static_cast<int>(UiSound::Count)] = {};
    // CombatSoundManager: [0] small, [1] medium, [2] large
    std::vector<int> swing_[3];
    int swingCrit_[3] = {-1, -1, -1};
    int miss1H_ = -1, miss2H_ = -1;
    std::vector<int> hitFlesh_, hitChain_, hitPlate_, hitShield_, hitMetalWeapon_, hitWood_, hitStone_;
    int critFlesh_ = -1, critChain_ = -1, critPlate_ = -1, critShield_ = -1;
    // upstream CombatSoundManager::ImpactType
    enum Impact : uint8_t { kFlesh, kChain, kPlate, kShield, kMetalWeapon, kWood, kStone };
    void playImpact(Impact type, bool crit);
    // FootstepManager (character bank; no mounts on the PSP yet)
    std::vector<int> steps_[7];
    uint32_t lastStepMs_ = 0;
    Tracker own_;
    std::unordered_map<uint64_t, Tracker> others_;
    uint32_t lastOtherStepMs_ = 0;
    // ActivitySoundManager
    std::vector<int> jump_, splash_, swimStroke_, hardLand_, meleeSwing_, attackGrunt_, wound_, woundCrit_;
    std::vector<int> landing_[7];
    uint32_t lastJumpMs_ = 0, lastLandMs_ = 0, lastMeleeSwingMs_ = 0, lastGruntMs_ = 0, lastWoundMs_ = 0;
    uint32_t lastSplashMs_ = 0, lastStrokeMs_ = 0;
    // SfxStateDriver
    bool actInit_ = false, prevGrounded_ = true, prevJumping_ = false, prevFalling_ = false, prevSwimming_ = false;
    float sinceWaterExit_ = 0.45f;
    // MovementSoundManager: [0] small, [1] medium, [2] huge
    std::vector<int> waterStep_[3];
    int enterWater_[3] = {-1, -1, -1};

    // Log: footstep n= every 10 s, sfx_cache every 10 s, sfx_play (first 60).
    uint32_t nextLogMs_ = 0;
    uint32_t stepsOwn_ = 0, stepsOther_ = 0, stepsNpc_ = 0;
    uint32_t playLogs_ = 0;
};

} // namespace wowee::ui::psp
