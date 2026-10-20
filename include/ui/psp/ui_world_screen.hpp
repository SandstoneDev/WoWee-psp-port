// Milestone 5: the character is in the world. Loads the baked terrain tiles
// and their doodads (M7) around the login position and orbits a camera around the character.
// No movement yet (that is gameplay, M10): the server knows where we stand.
#pragma once

#include "platform/psp/psp_item_icons.hpp"  // Item icons
#include "platform/psp/psp_input.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_world_session.hpp"
#include "rendering/animation/character_animator.hpp"
#include "rendering/psp/psp_audio_format.hpp"
#include "platform/psp/psp_spell_db.hpp"
#include "rendering/psp/psp_character.hpp"
#include "rendering/psp/psp_creatures.hpp"
#include "rendering/psp/psp_models.hpp"
#include "rendering/psp/psp_terrain.hpp"
#include "ui/psp/ui_art.hpp"  // Window art with a failure trail (ui_art_fail, retry at the next open)
#include "ui/psp/ui_focus.hpp"
#include "ui/psp/ui_kit.hpp"
#include "ui/psp/ui_renderer.hpp"
#include "ui/psp/ui_settings_menu.hpp"
#include "ui/psp/ui_world_chat.hpp"  // Chat
#include "ui/psp/ui_world_streaming.hpp"  // Streaming
#include "rendering/psp/psp_lighting.hpp"  // Lighting
#include "ui/psp/ui_world_sounds.hpp"     // Sounds
#include "rendering/psp/psp_attach_crowd.hpp"  // memory budget: attachcap's crowd queue

#include <cstdint>
#include <memory>
#include <array>
#include <unordered_map>
#include <string>

namespace wowee::ui::psp {

struct FrameStats;

class WorldScreen {
public:
    WorldScreen(const UiKit& kit, platform::psp::WorldSession& session, std::string characterName);

    void update(const platform::psp::PadState& pad, float dtSec);
    void draw(UiRenderer& ui, const FrameStats& stats);

private:
    void loadTerrain();
    void loadDoodads(const char* dir, int tileX, int tileY);
    void loadCharacter();
    void syncRemotePlayers();
    void drawRemotePlayers(const rendering::psp::TerrainCamera& cam);
    void loadCreatures(int tileX, int tileY);  // M10b, ui_world_creatures.cpp
    void syncCreatures();                      // live UNIT entities -> creatures_
    void updateMovement(const platform::psp::PadState& pad, float dtSec);
    void updateAnimation(float dtSec);
    float cameraDistance(const glm::vec3& pivot, glm::vec3& dir);
    float floorAt(float x, float y, float maxZ, bool* onTerrain = nullptr) const;
    void sendMove(game::Opcode opcode, bool moving);
    // Graphics settings menu (SELECT -> Menu -> Graphics): values -> renderers, and back.
    void applySettings();
    GraphicsSettings liveSettings() const;

    const UiKit& kit_;
    platform::psp::WorldSession& session_;
    std::string name_;
    platform::psp::Pak pak_;
    rendering::psp::TerrainRenderer terrain_;
    rendering::psp::ModelRenderer models_;
    // The game objects' models (ui_world_gameobjects.cpp, data/gameobjects.pak): a second renderer,
    // RAM only, fed the objects in sight within kGoRange as one pseudo-tile.
    rendering::psp::ModelRenderer goModels_;
    platform::psp::Pak goPak_;
    std::vector<rendering::psp::GameObjectDisplayRec> goIndex_;  // "gidx", by displayId
    int goState_ = 0;               // 0 not opened, 1 on, -1 off (no pak / wowee.cfg gameobjects=0)
    uint32_t goSeenVersion_ = ~0u, goNextMs_ = 0, goTile_ = 0, goMissing_ = 0, goLogs_ = 0;
    float goLastX_ = 0.0f, goLastY_ = 0.0f;
    std::vector<rendering::psp::DoodadPlacement> goPlaced_;
    void updateGameObjects();
    void drawGameObjects(const rendering::psp::TerrainCamera& cam, uint32_t fogABGR);
    rendering::psp::CharacterModel player_;
    // M10a: other players (models built from their update fields).
    struct RemotePlayer {
        std::unique_ptr<rendering::psp::CharacterModel> model;
        rendering::psp::CharacterLook look;
        rendering::psp::Equipment equip;  // M10c: what the model wears
        uint32_t equipChangeMs = 0;       // first seen different, queries pending (0 = none)
        glm::vec3 pos{0.0f};  // server coords, drawn this frame
        float yaw = 0.0f;
        bool visible = false;
        uint32_t anim = ~0u;  // M9: STAND / RUN from the entity's movement
        // Upstream Application player sync + CharacterRenderer::moveInstanceTo:
        // the drawn position chases the entity's in short eased moves.
        glm::vec3 cache{0.0f};  // last synced entity position (render pos cache)
        bool hasCache = false;
        glm::vec3 moveStart{0.0f}, moveEnd{0.0f};
        float moveDuration = 0.0f, moveElapsed = 0.0f;
        bool moveActive = false;
        bool wasMoving = false, wasWalking = false;
        bool wasBackward = false;  // PSP: npc-walk - backpedal (WalkBackwards)
        bool airborne = false;  // between MSG_MOVE_JUMP and landing (FALLING cleared)
        uint8_t standState = 0; // UNIT_FIELD_BYTES_1 byte 0 (1 = sitting)
        bool sittingDown = false;
        uint32_t emoteState = 0; // UNIT_NPC_EMOTESTATE seen (a looping state emote, e.g. dance)
        // Mounts - UNIT_FIELD_MOUNTDISPLAYID and the mount drawn under the rider.
        uint32_t mountDisplay = 0;
        bool mountReady = false;   // the mount's display is resident: the rider sits on it
        float riderHeight = 1.8f;  // upstream remote rider height (p_ table)
        bool movingNow = false, walkingNow = false;  // this frame (the mount's Stand / Walk / Run)
        // other players' equipment: what the model was last dressed from - the raw update fields of every look
        // (the visible items' entries and enchantments, the helm / cloak flags): any change re-dresses at once,
        // nearest first; the crowd's item models (attachcap) by distance.
        uint32_t seenEntry[19] = {}, seenEnchant[19] = {};
        uint32_t seenFlags = 0;          // PLAYER_FLAGS & (HIDE_HELM | HIDE_CLOAK)
        rendering::psp::CharacterLook wantLook;  // what the last scan read (applied when its turn comes)
        rendering::psp::Equipment wantEquip;
        int wantUnresolved = 0;
        bool dressWanted = false;        // a (re-)dress is due: the look, an item, an enchantment or a flag changed
        bool attachWanted = false;       // dressed without item models yet: they come in a later frame
        uint32_t unresolvedSinceMs = 0;  // entries the client's Item.dbc lacks: their queries pending since
        float dist = 0.0f;               // yd from the player (the scan, then the drawn position)
        bool modelFailed = false;        // characters.pak has no model for wantLook: not tried again for it
        bool flySwim = false;            // flying: playing the swim set (swimming / flying)
        bool inCap = true;               // playermax: among the nearest drawn (remoteCapTurn)
        uint32_t outSinceMs = 0;         // left the nearest N then (its model goes 3 s later)
    };
    // playermax (settings "Players drawn"): only the nearest N other players are drawn and keep a model.
    void remoteCapTurn(uint32_t nowMs);
    uint32_t remoteMax_ = 10;
    uint32_t remoteCapMem_ = 10;  // the cap the heap allows now (<= remoteMax_): down under pressure, back up with room
    uint32_t remoteMemWaits_ = 0;  // models not read for want of memory (the remote_cap log)
    uint32_t remoteCapNextMs_ = 0, remoteCapLogMs_ = 0, remoteCapUnloads_ = 0;
    std::vector<std::pair<float, uint64_t>> remoteCapRank_;
    // other players' equipment: the dress queue and attachcap's crowd turn (ui_world_screen.cpp).
    void dressRemoteQueue(uint32_t nowMs);
    void remoteCrowdTurn(uint32_t nowMs);
    float remoteDistance(uint64_t guid, const RemotePlayer& r) const;
    std::vector<std::pair<float, uint64_t>> dressJobs_;  // this frame's queue, nearest first
    uint32_t crowdNextMs_ = 0, crowdLogMs_ = 0, crowdYields_ = 0, crowdTurns_ = 0, dressMaxMs_ = 0, dressQueueMax_ = 0;
    uint32_t dressLogs_ = 0, dressCount_ = 0;
    std::unordered_map<uint64_t, RemotePlayer> remotes_;
    platform::psp::Pak charPak_;  // kept open: remote players load on demand
    rendering::psp::ItemBank items_;  // M10c: data/items.pak, kept open (equipment)
    uint32_t seenEntityVersion_ = ~0u;
    bool equipWaiting_ = false;       // a remote's item queries are still out
    uint32_t equipCheckMs_ = 0;
    glm::mat4 viewProj_{1.0f};    // for name plates
    glm::vec3 camPosForHud_{0.0f};
    // M9: upstream LocomotionFSM picks the player's animation (as
    // AnimationController::updateCharacterAnimation does on the PC).
    rendering::CharacterAnimator animator_;  // upstream: locomotion + activity (sit) + combat FSMs
    rendering::AnimCapabilitySet animCaps_;
    uint32_t lastAnimRequest_ = UINT32_MAX;
    bool lastAnimLoop_ = false;
    bool charAnimated_ = true;  // wowee.cfg charanim=0 or L + D-pad left: static Stand model (A/B)
    bool wantMove_ = false;     // movement input held this frame (upstream isMoving)
    rendering::psp::CreatureRenderer creatures_;
    std::vector<rendering::psp::CreatureUnit> liveUnits_;
    // Mounts (ui_world_mount.cpp, docs/pending/mounts_spellfx.md):
    // upstream EntitySpawner::processPendingMount / processPendingRemotePlayerMounts
    // + AnimationController::setMounted / updateMountedAnimation /
    // applyMountPositioning; the mount is a unit of creatures_.
    void updateMount(float dtSec);             // own mount field -> set up / clear (before movement)
    void setOwnMount(uint32_t displayId);
    void clearOwnMount();
    bool updateMountedAnimation(float dtSec);  // from updateAnimation: true = mounted (MountFSM drove it)
    void appendMountUnits(std::vector<rendering::psp::CreatureUnit>& out);  // from syncCreatures
    glm::vec3 ownRiderPos();                   // after creatures_.draw: the rider on this frame's seat
    void updateRemotePlayers();                // positions + animations (was the first half of drawRemotePlayers)
    void updateRemoteMount(uint64_t guid, RemotePlayer& r, const game::Entity& e);
    bool mountsOn_ = true;                     // wowee.cfg mounts=0: never draw mounts (A/B)
    uint32_t ownMountDisplay_ = 0;             // what the session's field asked for
    bool mountActive_ = false;                 // the model is resident and the MountFSM configured
    float mountHeightOffset_ = 0.0f;           // upstream mountHeightOffset_ (camera pivot, no-seat fallback)
    float mountBob_ = 0.0f;
    bool mountJumpKey_ = false, mountJumpGo_ = false;
    bool mountSeatInit_ = false;
    glm::vec3 mountSeatSmoothed_{0.0f}, mountSeatLast_{0.0f};
    bool mountSeatFound_ = false;
    uint32_t mountLogs_ = 0, mountFrameLogMs_ = 0, mountSetups_ = 0;
    bool remoteMountLoadedThisFrame_ = false;
    bool liveCreatures_ = false;  // server units seen: static DB spawns dropped  // M10b: M10a hands it live units (setUnits)
    std::string loadStatus_;
    uint32_t loadMs_ = 0;

    float playerX_ = 0, playerY_ = 0, playerZ_ = 0;
    float facing_ = 0;   // character, radians, server orientation (0 = north/+x, CCW)
    float camYaw_ = 0;   // camera orbit, same convention; independent of facing_
    // Start view: 10 deg down, the public CVar default cameraSavedPitch 10 (public console-variable list);
    // was 20 deg. The distances (start 5.55 yd, max 15) await observation (round 2: ui_camera).
    float pitch_ = 0.1745f;
    float distance_ = 5.55f;      // the player's zoom (upstream userTargetDistance), 0.5..camDistMax_
    float currentDist_ = 5.55f;   // eased toward distance_ (ZOOM_SMOOTH_SPEED)
    float camFovY_ = 54.0f;       // cfg fov (default 90) x 0.6; awaits observation (camera_fov)
    float camDistMax_ = 15.0f;    // cfg camdistmax
    static constexpr float kZoomSpeed = 8.33f;  // yd/s: public CVar default cameraDistanceMoveSpeed 8.33 (was 12)
    static constexpr float kCamMaxPitch = 1.5359f;  // 88 deg (upstream CameraController MAX_PITCH)
    float zoomDebt_ = 0.0f;       // zoom taken by collisions, given back later
    float clearSeconds_ = 0.0f;
    float shownDist_ = 5.55f;     // camera distance actually used this frame
    // Camera collision state (upstream CameraController): smoothed limits
    // and the sideways deflection around pillars / door frames.
    float camLimit_ = -1.0f;
    float camWallLimit_ = -1.0f;  // buildings only: what the zoom debt follows
    float camTerrainLimit_ = -1.0f;
    float camDeflectDeg_ = 0.0f;
    float lastDt_ = 1.0f / 60.0f;
    float yawVel_ = 0.0f, pitchVel_ = 0.0f;  // eased stick turn rates (rad/s)
    glm::vec3 smoothedCam_{0.0f};             // upstream smoothedCamPos
    float shownZ_ = 0.0f;                     // drawn feet height, eased over steps
    bool camInit_ = false;
    bool rotating_ = false;
    uint32_t nextStatsMs_ = 0;
    bool moving_ = false;
    uint32_t moveFlags_ = 0;   // MovementFlags sent while moving (R: back / strafe)
    bool animBackward_ = false;
    float animStrafe_ = 0.0f;  // <0 left, >0 right (R held, no forward/back)
    uint32_t animLogs_ = 0;
    bool remotePredict_ = true;
    bool perfHud_ = false;  // settings "FPS and ping" (perfhud)
    // Sitting (upstream CameraController X key -> CMSG_STANDSTATECHANGE) and
    // the owner's AFK sit: no input for afkSec_ (wowee.cfg afksit, 0 = off).
    bool sitting_ = false;
    bool sitStandNoJump_ = false;  // PSP: this frame's jump key stood the character up (upstream: no jump while sitting)
    bool afk_ = false;
    float idleSec_ = 0.0f;
    float afkSec_ = 300.0f;
    void setSitting(bool sit, bool afk);
    // M12 zone music (ui_world_audio.cpp): data/audio.pak = upstream
    // ZoneManager's tables, the logic of AudioCoordinator::updateZoneAudio.
    struct ZoneMusic {
        platform::psp::Pak pak;
        void* blob = nullptr;
        const rendering::psp::AudioZonesHeader* h = nullptr;
        const char* names = nullptr;
        const uint32_t* nameOffsets = nullptr;
        const rendering::psp::AudioZone* zones = nullptr;
        const uint16_t* zoneTracks = nullptr;
        std::unordered_map<uint32_t, uint32_t> areaParents, tileToZone, zoneIndex;
        std::unordered_map<uint32_t, std::array<uint16_t, 256>> grids;  // tileX * 100 + tileY
        const char* lastPlayed = nullptr;
        // world map levels: the area grid of the map we are on - data/minimap_<map>.pak "agrid"
        // (rendering::psp::AreaGridView, validated at load; ~60 KB for a continent). audio.pak's "a_<x>_<y>"
        // grids above are map 0's (psp_audio_conv --map <map 0>): read only on map 0 and only without it.
        void* gridBlob = nullptr;
        uint32_t gridBytes = 0;
        bool load(uint32_t mapId);
        void releaseGrids();
        ~ZoneMusic();  // the grids and the "zones" blob (a world left for the login screen kept them)
        // (end world map levels)
        uint32_t resolveAreaZoneId(uint32_t areaId) const;
        uint32_t zoneOfTile(int tileX, int tileY) const;
        uint32_t areaAt(int tileX, int tileY, int chunk);
        const char* randomMusic(uint32_t zoneId);
    };
    ZoneMusic zoneMusic_;
    bool musicOk_ = false;
    uint32_t zoneId_ = 0, lastResolvedZone_ = 0;
    float musicCooldown_ = 0.0f;
    char pendingTrack_[256] = {};
    float pendingTrackSec_ = 0.0f;
    void initAudio();
    void updateAudio(float dtSec);
    void playZoneMusic(const char* track);
    // Windows + cursor mode (ui_world_windows.cpp): SELECT toggles, like
    // upstream GamepadControls' pointer mode.
    enum : uint8_t { kWinMenu = 1, kWinDebug = 2 };
    bool uiMode_ = false;
    std::vector<uint8_t> windows_;   // open windows, last = top
    FocusRing winFocus_;
    // PSP (owner): movable windows - R + stick drags the window under the
    // cursor; the place of each window id is kept in wowee.cfg (winxy_<id>).
    struct WinPos {
        float dx = 0.0f, dy = 0.0f;
    };
    // A dropped window keeps its ABSOLUTE top-left (winxy_<id>), not an
    // offset from its default place: one id can draw from different bases (every
    // tutorial is 0xA0, anchored left or right), and an offset saved on one base
    // threw the next tutorial off the screen (hardware: -150 px).
    std::unordered_map<uint8_t, WinPos> winPos_;  // saved top-left (dx = x, dy = y)
    std::unordered_map<uint8_t, Rect> winBase_;   // last frame's bounds without the offset
    std::unordered_map<uint8_t, Rect> winRect_;  // last frame's drawn bounds (offset included)
    std::vector<uint8_t> winDrawn_;               // last frame's draw order, last = top
    uint8_t dragWin_ = 0;
    float dragX_ = 0.0f, dragY_ = 0.0f;
    bool winPosLoaded_ = false;
    WinPos windowPos(uint8_t w);
    // PSP (owner: "I cannot keep two windows open - START closes the first"): START opens the game menu over
    // the open windows (it closes only the menu, a dropdown or a popup); a panel opened while others are open is
    // placed beside them (autoPos_, this session only - a dragged place in winxy_ wins); Circle on a window's title
    // band closes it (its close button is there as well).
    std::unordered_map<uint8_t, WinPos> autoPos_;  // top-left beside the open panels (not saved)
    std::vector<uint8_t> placeNext_;               // panels opened beside others, placed once their size is known
    bool isPopupWindow(uint8_t w) const;           // StaticPopup-like: START closes it, never placed beside
    void placeBeside(uint8_t w, const Rect& base);
    bool closeTitleUnderCursor();                  // Circle on a window's title band
    void drawWindowColumn(UiRenderer& ui);  // Book / Chat / Menu / Debug, under the windows
    void dragWindows(platform::psp::PadState& p, const platform::psp::PadState& pad);
    bool creaturesOn_ = true;
    // Buttons held when the mode changed stay dead until released (steam-psp's
    // "armed" rule: the SELECT/Circle hand must not click or walk).
    uint32_t suppress_ = 0;
    void setUiMode(bool on);
    bool windowOpen(uint8_t w) const;
    void openWindow(uint8_t w);
    void closeWindow(uint8_t w);
    void updateWindows(const platform::psp::PadState& pad, float dtSec);
    void drawWindows(UiRenderer& ui);
    // M9c emotes: Emotes.dbc from characters.pak ("emotes"): id -> anim | state << 16.
    std::unordered_map<uint32_t, uint32_t> emoteTable_;
    void loadEmoteTable();
    void processEmotes();
    void playRemoteEmote(RemotePlayer& r, uint32_t emoteId, bool fromField);
    void playOwnEmote(uint32_t anim, bool loop);
    uint32_t ownEmoteState_ = 0;
    void updateSitting(const platform::psp::PadState& pad, float dtSec);    // wowee.cfg remotepredict (PSP remote-player prediction)
    bool airborne_ = false;
    float vz_ = 0.0f;
    float jumpVelocity_ = 0.0f;
    uint32_t fallStartMs_ = 0;
    uint32_t dropLogs_ = 0;
    float sentFacing_ = 0;
    uint32_t nextHeartbeatMs_ = 0;
    uint32_t moveStarts_ = 0;
    bool modelsOn_ = true;
    bool fillTest_ = false;     // D-pad right: 1x1 scissor for the 3D pass (fill vs vertex bound)      // D-pad left: A/B test of the doodad cost on hardware
    uint32_t modelsCpuUs_ = 0;
    SettingsMenu settings_;
    float remoteView_ = 120.0f;  // yd, other players drawn (wowee.cfg playerview)
    uint32_t terrainCpuUs_ = 0;  // CPU time of TerrainRenderer::draw  // CPU time of ModelRenderer::draw (culling + list building)
    WorldChat chat_{kit_, session_};  // Chat (ui_world_chat.cpp)

    // Combat - target, unit frames, melee, death
    // (ui_world_target.cpp, ui_world_combat.cpp).
    static constexpr uint8_t kWinRelease = 0x42;  // StaticPopup DEATH "Release Spirit" (window stack id)
    struct UnitView {
        std::string name;
        uint32_t level = 0, hp = 0, maxHp = 0, power = 0, maxPower = 0, rank = 0;
        uint8_t powerType = 0, race = 0, sex = 0;
        bool player = false, dead = false;
    };
    struct CombatText {  // upstream CombatTextEntry (floating combat text)
        uint8_t type = 0;
        bool outgoing = false;
        int32_t amount = 0;
        uint64_t dst = 0;
        float age = 0.0f;
        float xSeed = 0.0f;
    };
    struct UnitFrameArt {
        platform::psp::Texture target, player, bar;
        platform::psp::Texture stateCombat;  // controls: PlayerAttackIcon (UI-StateIcon crossed swords)
        platform::psp::Texture portrait[12][2];  // [race][sex], optional
        // Optional: TargetFrame_CheckClassification borders (elite / rare / rare-elite),
        // the name plate (UI-TargetingFrame-LevelBackground, tinted UnitSelectionColor), the "??"
        // skull, PlayerFrame_UpdateStatus art (rest icon, ADD glows, UI-Player-Status, attack background).
        platform::psp::Texture targetElite, targetRare, targetRareElite;
        platform::psp::Texture nameBg, skull;
        platform::psp::Texture stateRest, glowRest, glowCombat, status, attackBg;
        bool loaded = false;
        UnitFrameArt() = default;
        UnitFrameArt(const UnitFrameArt&) = delete;
        UnitFrameArt& operator=(const UnitFrameArt&) = delete;
        ~UnitFrameArt();
    };
    void initCombat();
    void applyCombatLoadout();
    void updateCombat(const platform::psp::PadState& pad, float dtSec);
    const platform::psp::PadState& combatPad(const platform::psp::PadState& pad);
    bool combatAnimOverride();
    void combatFrameInput(rendering::CharacterAnimator::FrameInput& in, float dtSec);
    void tabTarget(int step = 1, int filter = -1);
    void clearTarget();
    void toggleAutoAttack();
    void triggerMeleeSwing();
    void processMeleeHits();
    void addCombatText(uint8_t type, int32_t amount, bool outgoing, uint64_t dst);
    bool unitView(uint64_t guid, UnitView& out) const;
    bool unitPosition(uint64_t guid, glm::vec3& pos, float& height) const;
    bool projectHud(const glm::vec3& worldPos, float& sx, float& sy) const;
    void drawCombatUnder(UiRenderer& ui);
    void drawUnitFrames(UiRenderer& ui);
    void drawUnitFrame(UiRenderer& ui, const UnitView& u, float x, float y, bool mirrored, bool isSelf);
    bool drawCombatWindow(UiRenderer& ui, uint8_t w);
    bool combatWindowAction(uint32_t act);
    // Death -> ghost -> corpse run -> resurrection (ui_world_death.cpp): the
    // original StaticPopup dialogs DEATH (kWinRelease), RECOVER_CORPSE, RESURRECT(_NO_SICKNESS /
    // _NO_TIMER) and XP_LOSS(_*) with their ruRU GlobalStrings, the teleport to the graveyard
    // (and to a resurrecting caster) and the ghost's look. One small draw function per dialog
    // (kit_.panel + kit_.button) until the shared StaticPopup component replaces them.
    static constexpr uint8_t kWinRecoverCorpse = 0x44, kWinResurrect = 0x45, kWinXpLoss = 0x46;
    struct DeathUi {
        float releaseLeft = 360.0f;  // GetReleaseTimeRemaining (s); < 0: unknown -> DEATH_RELEASE_NOTIMER
        bool sawAlive = false;       // alive seen this session (dead at login: no timer known)
        float reshowSec = 0.0f;      // PSP: DEATH again when a release went unanswered
        bool corpseInRange = false;  // CORPSE_IN_RANGE announced (upstream corpseInRangeAnnounced_)
        uint32_t offerSeq = 0;       // the RESURRECT / XP_LOSS offer on screen
        float offerLeft = 0.0f;      // RESURRECT: its timeout (GetCorpseRecoveryDelay + 60)
        float offerDelay = 0.0f;     // RESURRECT: its StartDelay (Accept disabled)
        bool xpAgain = false;        // XP_LOSS: the second confirmation (CONFIRM_XP_LOSS_AGAIN*) is up
        uint64_t healer = 0;         // XP_LOSS: the spirit healer (CheckSpiritHealerDist)
        bool shownRes = false, shownXp = false;  // as last synced (START closes them = cancel)
        bool raisedUi = false;       // the cursor came up for a death popup
        bool pressSeen = false;      // Cross went down on a popup button (not a release left over)
        float nextY = 0.0f;          // popups stack downwards, as StaticPopup1..4
    };
    DeathUi death_;
    void updateDeath(const platform::psp::PadState& pad, float dtSec);
    void applyTeleport(const glm::vec3& to, float orientation);
    // PSP (a hardware test: ".tele" to a capital city landed UNDER a city district): the landing hold
    // (ui_teleport_hold.hpp) - after teleport_applied and at world entry the position is held (no gravity, no input, no
    // heartbeat) until the floor under the player is loaded, or wowee.cfg tphold ms (8000; 0 = off).
    // Log: teleport_hold ms= why=floor|timeout terrain= doodads= wmo= col=resident/wanted floor=.
    struct TpHold {
        bool active = false, entryDone = false;
        uint32_t startMs = 0, holdMs = 8000, holds = 0;
        const char* what = "";
    };
    TpHold tpHold_;
    void startTeleportHold(const char* what);
    bool teleportHold();  // true: held this frame (updateMovement returns)
    bool teleportHolding() const { return tpHold_.active; }
    void showDeathPopup(uint8_t w, uint32_t focusId);
    void closeDeathPopup(uint8_t w);
    bool deathPopupKeepsOnEscape(uint8_t w) const;  // not hideOnEscape: START passes it by
    bool drawDeathWindow(UiRenderer& ui, uint8_t w);
    bool deathWindowAction(uint32_t act);
    Rect deathPopupBox(UiRenderer& ui, const char* text, float& buttonY);
    void drawDeathPopup(UiRenderer& ui);     // StaticPopupDialogs["DEATH"]
    void drawCorpsePopup(UiRenderer& ui);    // StaticPopupDialogs["RECOVER_CORPSE"]
    void drawResurrectPopup(UiRenderer& ui); // StaticPopupDialogs["RESURRECT*"]
    void drawXpLossPopup(UiRenderer& ui);    // StaticPopupDialogs["XP_LOSS*"]
    UnitFrameArt ufArt_;
    std::vector<CombatText> combatText_;
    std::vector<uint64_t> tabList_;
    int tabIndex_ = -1;
    bool tabStale_ = true;
    uint32_t tabVersion_ = ~0u, lastTabMs_ = 0;
    uint64_t shownTarget_ = 0;
    float targetRange_ = 40.0f, targetCone_ = 60.0f, npcNameDist_ = 30.0f;
    bool combatTextOn_ = true, npcNamesOn_ = true, unitFramesOn_ = true, autoFace_ = true;
    bool combatInit_ = false;
    float meleeSwingTimer_ = 0.0f, meleeSwingCooldown_ = 0.0f;
    bool meleeDurPending_ = false;
    uint8_t lastLife_ = 0;  // WorldSession::Life
    std::string combatError_;
    uint32_t combatErrorUntilMs_ = 0;
    // UI errors: UIErrorsFrame (ui_world_errors.cpp) - up to 3 rows, the newest on
    // top, 5 s + a 3 s fade each; combatError_ above stays the input (taken into the stack once a frame).
    struct UiErrorRow {
        std::string text;
        uint32_t color = 0;   // 0x00BBGGRR, the alpha comes from the fade
        uint32_t bornMs = 0;
    };
    std::vector<UiErrorRow> uiErrors_;  // [0] = the top row
    void uiErrorAdd(const std::string& text, uint32_t rgb);  // MessageFrame:AddMessage (insertMode TOP)
    void uiError(const std::string& text);  // UI_ERROR_MESSAGE (red)
    void uiInfo(const std::string& text);   // UI_INFO_MESSAGE (yellow)
    void uiErrorsTick();                     // the inputs -> the stack, faded rows out
    void drawUiErrors(UiRenderer& ui);
    float uiErrorsBottom() const;            // y under the last row (the lines drawn under the stack)
    bool uiErrorsOverWindows() const;        // trainer / talents open: drawn after the windows
    uint32_t combatLogs_ = 0, meleeLogs_ = 0;
    platform::psp::PadState deadPad_{};
    bool repopSent_ = false;
    uint32_t nextCombatLogMs_ = 0;
    std::vector<uint64_t> deadRemotes_;  // other players shown dead (Death held)
    void drawNpcNames(UiRenderer& ui);
    void syncRemoteDeaths();
    void playCreatureEmote(uint64_t guid, uint32_t emoteId);  // Creature anim (ui_world_creatures.cpp)

    // Spells (ui_world_spells.cpp): action bar, cast bars, auras, the spell
    // book window and the cast animations (upstream spellCastAnimCallback).
    // Controls: L + Square / Triangle / Circle / Cross = the 4 buttons of the
    // bar's current page, L + D-pad down = next page (3 pages of 4 = the
    // 12 buttons of the main bar; L + up = talk); in cursor mode
    // the buttons are clickable.
    static constexpr uint8_t kWinSpellbook = 0x43;
    struct SpellArt {
        platform::psp::Texture slot, empty, hilite, red, castBorder, castBorderSmall, castSpark, castFlash, debuff;
        SpellArt() = default;
        SpellArt(const SpellArt&) = delete;
        SpellArt& operator=(const SpellArt&) = delete;
        ~SpellArt();
    };
    void initSpells();
    void updateSpells(const platform::psp::PadState& pad, float dtSec);
    const platform::psp::PadState& spellPad(const platform::psp::PadState& pad);
    void castFromSlot(int slot);
    int mainBarBase() const;
    void playCastAnim(const platform::psp::WorldSession::CastAnim& a);
    void drawSpells(UiRenderer& ui);
    void drawActionBar(UiRenderer& ui);
    void drawCastBar(UiRenderer& ui);
    void drawTargetCastBar(UiRenderer& ui);
    void drawAuras(UiRenderer& ui);
    void drawCooldown(UiRenderer& ui, const Rect& r, float remaining, float total);
    bool drawSpellWindow(UiRenderer& ui, uint8_t w);
    bool spellWindowAction(uint32_t act);
    void rebuildSpellbook();
    platform::psp::SpellDb spellDb_;
    struct SpellDbLink {  // the session outlives the screen: unhook the db first
        platform::psp::WorldSession* session = nullptr;
        ~SpellDbLink() { if (session) session->setSpellDb(nullptr); }
    } spellDbLink_;
    SpellArt spellArt_;
    bool spellsInit_ = false;
    int barPage_ = 0;             // 0..2: which 4 of the 12 main-bar buttons L + face buttons press
    uint32_t lCastMask_ = 0;      // face buttons pressed with L: not movement until released
    platform::psp::PadState spellPad_{};
    uint32_t spellFrame_ = 0;     // icon cache clock
    float slotFlash_[12] = {};    // pressed highlight (s)
    // CastingBarFrame.lua state: 0 hidden, 1 casting, 2 channeling, 3 finished (flash + fade), 4 failed (hold + fade)
    uint8_t castBarMode_ = 0;
    uint32_t castBarSpell_ = 0;
    float castBarValue_ = 0.0f, castBarAlpha_ = 0.0f, castBarFlash_ = 0.0f, castBarHold_ = 0.0f;
    uint32_t castBarColor_ = 0;
    std::vector<uint32_t> bookSpells_;
    uint32_t bookVersion_ = ~0u;
    int bookPage_ = 0;
    uint32_t bookPick_ = 0;       // the spell on the cursor (PickupSpell / PickupAction), 0 = none
    // SpellBookFrame (ui_world_spellbook.cpp).
    struct SpellBookArt {
        platform::psp::Texture frame, tab, checked, prev, prevDis, next, nextDis;
        bool tried = false;
        void release();
        SpellBookArt() = default;
        SpellBookArt(const SpellBookArt&) = delete;
        SpellBookArt& operator=(const SpellBookArt&) = delete;
        ~SpellBookArt();
    };
    SpellBookArt sbArt_;
    UiArtRetry sbArtRetry_;  // A failed load is tried again at the next window open
    int bookPages_[8] = {};       // SPELLBOOK_PAGENUMBERS: each skill line tab keeps its page
    uint32_t tipSpellId_ = 0;     // the book's tooltip (spell / tab), drawn after the windows
    int tipBookTab_ = 0;
    Rect tipSpellRect_{};
    void loadSpellBookArt();
    void spellWindowPad(const platform::psp::PadState& pad);
    void spellSlotPickPlace(int slot, bool pick);
    void drawSpellCursor(UiRenderer& ui);
    uint32_t spellLogs_ = 0;
    std::unordered_map<uint64_t, uint32_t> remoteCastAnim_;  // other players / NPCs in a cast pose
    // Spell effects (ui_world_spellfx.cpp, src/rendering/psp/psp_spellfx.cpp).
    // wowee.cfg spellfx=0 (default): never allocated; 1 own spells; 2 every caster.
    struct SpellFxUi;
    static void destroySpellFxUi(SpellFxUi* p);
    std::unique_ptr<SpellFxUi, void (*)(SpellFxUi*)> spellFx_{nullptr, nullptr};
    int spellFxMode_ = -1;                                       // -1: wowee.cfg not read yet
    void spellFxEvents();                                        // updateSpells, after the cast animations
    void drawSpellFx(const rendering::psp::TerrainCamera& cam);  // draw, after the last 3D draw
    // spell effects (end)
    // particles: the world's particle emitters (doodads, game objects: torches, braziers, campfires,
    // smoke, splashes, magic lights), their ribbons and the weather (ui_world_particles.cpp, ui_world_weather.cpp,
    // src/rendering/psp/psp_particles.cpp). wowee.cfg particles=0 + weather=0: never made.
    struct FxWorldUi;
    static void destroyFxWorldUi(FxWorldUi* p);
    std::unique_ptr<FxWorldUi, void (*)(FxWorldUi*)> fxWorld_{nullptr, nullptr};
    int fxWorldState_ = 0;                                         // 0 not made yet, 1 on, -1 off
    void updateParticles(float dtSec);                             // update(): the game objects, the weather state
    void updateWeather(float dtSec);                               // from updateParticles (ui_world_weather.cpp)
    void drawParticles(const rendering::psp::TerrainCamera& cam);  // draw(): before the spell effects
    void drawWeather(const rendering::psp::TerrainCamera& cam);    // draw(): after the spell effects
    bool weatherRaining() const;                                   // the light's rain profile (upstream isRaining)
    // particles (end)
    // action bar: MainMenuBar (ui_world_mainmenubar.cpp): the bar art and the
    // gryphon end caps (ui.pak "mmb_art", loaded with the spell art and kept - always on screen), MainMenuExpBar
    // with ExhaustionTick (our XP / rest fields), ActionButton_UpdateUsable (blue: no power, grey: not usable).
    static constexpr uint32_t kIdXpBar = 0x5CB0, kIdXpTick = 0x5CB1;
    struct MmbArt {
        platform::psp::Texture tex;
        bool off = false;  // wowee.cfg mainmenubar=0: no art (the earlier look, A/B)
        MmbArt() = default;
        MmbArt(const MmbArt&) = delete;
        MmbArt& operator=(const MmbArt&) = delete;
        ~MmbArt();
    };
    MmbArt mmbArt_;
    struct XpView {  // MainMenuExpBar_Update + ExhaustionTick_OnEvent, once per frame
        bool shown = false, maxLevelBar = false, tick = false;
        uint32_t cur = 0, max = 1, rested = 0;
        uint8_t state = 2;   // GetRestState id
        float tickX = 0.0f;  // ExhaustionTick: px right of the bar's left edge
    };
    XpView xpView_;
    uint32_t xpColor_ = 0xFF8C0094u;  // MainMenuExpBar BarColor (0.58, 0, 0.55); SetStatusBarColor by the rest state
    uint32_t xpHoverMs_ = 0;          // ExhaustionTick.timer: the bar under the cursor since (ms), 0 = not
    uint8_t xpTip_ = 0;               // ExhaustionToolTipText this frame (drawn after the windows)
    bool xpTextAlways_ = false;       // wowee.cfg xpbartext=1 (CVar xpBarText)
    struct UseCtx {  // what IsUsableAction reads of the player / the target, once per frame
        uint32_t power[7] = {}, maxPower[7] = {};
        uint32_t health = 0, baseHealth = 0, baseMana = 0, auraState = 0, targetAuraState = 0;
        int32_t costMod[7] = {};
        float costMul[7] = {};
        uint8_t form = 0;
        bool haveTarget = false, ignoreAuraState = false;
    } useCtx_;
    uint32_t useAuraScanMs_ = 0;
    std::unordered_map<uint32_t, uint32_t> reagentCount_;  // item -> count in the bags, per inventoryVersion
    uint32_t reagentInvVersion_ = ~0u;
    void loadMainMenuBarArt(platform::psp::Pak& pak, uint32_t& bytes);
    Rect mmbButton(int i) const;  // ActionButton1..12 (MainMenuBar at 0.5)
    void updateXpView();
    void updateUseCtx();
    uint8_t actionUsable(const game::ActionBarSlot& s);  // 0 usable, 1 not enough power, 2 not usable
    uint32_t reagentCount(uint32_t item);
    void drawMainMenuBarArt(UiRenderer& ui);      // "hud" pass: the XP bar, the bar art, the end caps
    void drawMainMenuBarOverlay(UiRenderer& ui);  // after the buttons: ExhaustionTick, the XP text, focus stops
    void drawXpTooltip(UiRenderer& ui);           // after the windows: ExhaustionToolTipText

    // Controls with modes (ui_world_controls.cpp, docs/PSP_CONTROLS.md):
    // wowee.cfg controls=2 (nub moves, Travel / Combat modes) or 1 (the old scheme).
    void initControls();
    const platform::psp::PadState& updateControls(const platform::psp::PadState& pad, float dtSec);
    void setCombatMode(bool on, const char* src);
    void castMount();
    void drawModeHud(UiRenderer& ui);
    bool controlsInit_ = false, modeTextOn_ = true, camFollow_ = true;
    int controlsScheme_ = 2;
    bool combatMode_ = false;      // Combat mode: weapons drawn; else Travel mode
    bool modeManual_ = false;      // drawn by hand: stays until the next calm after a fight
    float modeCalm_ = 0.0f, modeTextSec_ = 0.0f;
    uint32_t ctlMask_ = 0;         // buttons held across a mode switch: dead until released
    uint32_t lastRawHeld_ = 0;
    platform::psp::PadState ctlPad_{};  // what the older handlers see
    bool stickMove_ = false, stickWalk_ = false, jumpReq_ = false, autorun_ = false;
    float moveX_ = 0.0f, moveY_ = 0.0f;  // stick, dead zone removed (y down = forward)
    // Loot (ui_world_loot.cpp) - loot window (window stack), the
    // lootable-corpse sparkle, "Loot target" in the Debug window.
public:
    // A dead unit with loot for us (UNIT_DYNFLAG_LOOTABLE): CMSG_LOOT when in
    // reach, an error line when too far - both true (handled); anything else
    // false, so the caller's own interaction (task D, L + up) goes on.
    bool tryLootTarget(uint64_t guid);

private:
    static constexpr uint8_t kWinLoot = 0x60;  // window stack id (E: 0x60-0x67)
    void initLoot();
    void updateLoot(float dtSec);
    void drawLootHud(UiRenderer& ui);
    bool drawLootWindow(UiRenderer& ui, uint8_t w);
    bool lootWindowAction(uint32_t act);
    void lootWindowPad(platform::psp::PadState& pad);  // Triangle / Up = take all (Up kept off the focus)
    void drawLootDebugRow(UiRenderer& ui, const Rect& debugWindow);
    bool lootInit_ = false, lootGate_ = true, lootDespawn_ = true, lootMarker_ = true;
    bool lootRaisedUi_ = false, lootPressSeen_ = false;
    float lootRange_ = 5.5f, lootCloseDist_ = 15.0f, lootMarkDist_ = 40.0f;
    uint64_t lootShownGuid_ = 0;
    uint32_t lootUiLogs_ = 0;
    // The original LootFrame - its art (ui.pak loot_*), the item
    // icons (itemicons.pak, shared with the other item windows later), pages.
    struct LootArt {
        platform::psp::Texture panel, dead, nameFrame, close, closeDown, up, down;
        bool tried = false;
        UiArtRetry artRetry;
        ~LootArt();
    };
    LootArt lootArt_;
    platform::psp::ItemIconDb itemIcons_;
    bool itemIconsTried_ = false;
    UiArtRetry itemIconsRetry_;  // itemicons.pak there but not opened (memory / descriptor)
    int lootPage_ = 1;
    void loadLootArt();
    platform::psp::ItemIconDb& itemIcons();
    // Loot (end)
    // Lighting (ui_world_lighting.cpp) - upstream LightingManager
    // over data/light.pak (Light.dbc bands by hour and zone), GE fog / tint /
    // sky gradient / glow, the "Light" window (Debug -> Light).
    static constexpr uint8_t kWinLight = 0x70;
    struct LightingUi {
        rendering::psp::Lighting light;
        // wowee.cfg: lighting sky fogbands lighttint glow timeofday lightgain
        // fogstrength lightzero lightdefault timeadvance light2x
        int on = 1, sky = 1, fogBands = 1, tint = 1, glow = 0, timeOfDay = -1, gain = 100, fogStrength = 40;
        int zero = 1, mapDefault = 1, advance = 1, twoX = 0;
        int moon = 60;  // Moonlight, % of the night diffuse (was 100 = |z|)
        float hours = 12.0f, tintLevel = 1.0f;
        uint32_t nextLogMs = 0, zoneLogs = 0, lastVol0 = ~0u, lastVol1 = ~0u;
        uint32_t seenTimeMs = 0;  // The realm time as last seen (a new one snaps the light)
        // sky speed: wowee.cfg skyfast (1: the sky replaces the frame clear, cached
        // grid; 0: the plain clear) and skybench (0 off; 1..3: 4 extra whole-screen passes, GE "sky.bench").
        // skyFastFrame: skyfast as latched for this frame in updateLighting (the clear is skipped then).
        int skyFast = 1, skyBench = 0;
        bool skyFastFrame = false;
        uint32_t skyNextLogMs = 0;
    };
    // PSP (owner: the world showed unlit, then the light switched on):
    // until the realm's time arrives (SMSG_LOGIN_SETTIMESPEED, a moment after
    // the first frame) the world is held black, then fades in lit.
    uint32_t worldEnterMs_ = 0, worldLitMs_ = 0;
    float worldFade();
    LightingUi light_;
    void initLighting();
    void updateLighting(float dtSec);
    bool lightingSkyOn() const;
    void drawLightingSky(const rendering::psp::TerrainCamera& cam, uint32_t flatSkyABGR);  // sky speed: + flat colour
    void drawLightingPost();
    void drawLightingDebugButton(UiRenderer& ui, float x, float y);
    bool lightingWindowAction(uint32_t act);
    bool drawLightingWindow(UiRenderer& ui, uint8_t w);
    // Sounds (ui_world_sounds.cpp): effects of the world - UI,
    // footsteps, combat, activity (upstream's sound managers).
    WorldSounds sounds_;
    StepSurface sfxSurface_ = StepSurface::Stone;
    glm::vec3 sfxSurfacePos_{0.0f};
    float sfxSurfaceTimer_ = 999.0f;
    void initSounds();
    void updateSounds(float dtSec);
    StepSurface soundSurface(float dtSec);
    void onMeleeHitSound(bool mine, bool atMe, uint32_t hitInfo, uint32_t victimState);
    bool soundDebugAction(uint32_t act);  // Debug window row: test sound, footsteps mode
    void drawSoundDebugRow(UiRenderer& ui, float c0, float c1, float bw, float bh, float y);
    uint32_t sfxTestIndex_ = 0;
    // Streaming (ui_world_streaming.cpp) - tiles load/evict around
    // the player as it walks; declared last so it stops its loader thread
    // before the renderers it feeds are destroyed.
    void startStreaming();
    void updateStreaming(float dtSec);
    TileStreamer stream_;
    // NPC - L + D-pad up talks to the target (interactTarget), the
    // gossip / vendor / quest windows in the window stack, the quest log
    // (Menu) and the ! / ? marks over questgivers (ui_world_npc.cpp). Plain
    // data, never touched by the streamer thread, so it may follow stream_.
    static constexpr uint8_t kWinGossip = 0x58, kWinVendor = 0x59, kWinQuestDetails = 0x5A,
                             kWinQuestProgress = 0x5B, kWinQuestReward = 0x5C, kWinQuestLog = 0x5D;
    static constexpr uint32_t kIdNpcQuestLog = 0x6100;  // "Quest Log" in the Menu window
    void interactTarget();
    void updateNpc(const platform::psp::PadState& pad, float dtSec);
    void syncNpcWindows();
    void drawQuestMarkers(UiRenderer& ui);
    bool drawNpcWindow(UiRenderer& ui, uint8_t w);
    bool npcWindowAction(uint32_t act);
    struct NpcUi {
        bool init = false;
        bool shown[5] = {};          // session windows (gossip..reward) as last synced
        bool raisedCursor = false;   // the cursor came up for an NPC window
        bool guard = false;          // a window opened by itself: ignore a click until a press is seen on it
        uint32_t holdCursorUntilMs = 0;
        uint32_t gossipSig = 0;
        int page = 0, scroll = 0, logSel = 0, logPage = 0, rewardChoice = -1;
        bool abandonArmed = false;
        float range = 6.0f, markDist = 40.0f;
        int hello = 1;               // wowee.cfg npchello: 1 by NPC flags (original client), 0 upstream
        bool marks = true;
        std::string error;
        uint32_t errorUntilMs = 0, logs = 0;
        std::string wrapSrc[6];
        std::vector<std::string> wrapLines[6];
    };
    NpcUi npc_;
    // The original GossipFrame / QuestFrame (ui_world_npc_frame.cpp).
    enum : uint8_t {
        kNpcIconGossip = 0, kNpcIconAvailable = 11, kNpcIconActive, kNpcIconIncomplete, kNpcIconDaily,
        kNpcIconDailyActive, kNpcIconCount
    };
    struct NpcArt {
        platform::psp::Texture frame;  // 132 KB: loaded with a conversation window, released after
        platform::psp::Texture icons[kNpcIconCount];
        platform::psp::Texture titleHl, btnUp, btnDown, btnDis, btnHl, scrUp, scrDown, scrUpDis, scrDownDis, money,
            close, closeDown;
        bool frameTried = false, smallTried = false;
        UiArtRetry artRetry;
        void release(bool all);
        ~NpcArt();
    };
    NpcArt npcArt_;
    void loadNpcArt(bool frame = true);  // False = only the small art (buttons, coins, close)
    bool drawNpcFrame(UiRenderer& ui, uint8_t w);
    void drawPanelButton(UiRenderer& ui, uint32_t id, const Rect& r, const char* label, bool enabled, bool top);
    // MerchantFrame (ui_world_merchant.cpp) - the merchant / buyback tabs, pages, repair,
    // selling from the bags (ui_world_inventory.cpp), the backpack opened with it as the original.
    struct MerchantUi {
        platform::psp::Texture frame, prev, prevDis, next, nextDis, repair, bbIcon;
        int frameTab = 0;            // which tab's frame is loaded (merch_frame / merch_buyback)
        bool smallTried = false;
        UiArtRetry artRetry;         // ui_art_fail - loaded again at the next window open
        int tab = 1, page = 0;
        bool shown = false, openedBags = false;
        uint32_t errSeenMs = 0;
        void release();
        ~MerchantUi();
    };
    MerchantUi merch_;
    void loadMerchantArt();
    void updateMerchant();
    bool drawMerchantWindow(UiRenderer& ui, uint8_t w);
    bool merchantWindowAction(uint32_t act);
    // SmallMoneyFrame: gold / silver / copper with their coins; right-aligned at x when `right`. Width.
    float drawMoneyFrame(UiRenderer& ui, float x, float y, uint32_t copper, bool right);
    // ItemButtonTemplate: the icon in the UI-Quickslot2 frame, the count (bottom right) and the
    // stock "(N)" (top left, MERCHANT_STOCK); tint = SetItemButtonTextureVertexColor.
    void drawItemButton(UiRenderer& ui, const Rect& icon, uint32_t displayId, uint32_t count, int stock,
                        uint32_t tint);
    // QuestLogFrame (ui_world_questlog.cpp) - quests grouped by zone (ZoneOrSort, text.pak
    // "zones"), the details on the parchment as QuestInfo, abandon with ABANDON_QUEST_CONFIRM.
    struct QuestLogUi {
        platform::psp::Texture frame, empty, plus, minus, titleHl, check;
        bool tried = false;          // The abandon confirm is the kWinQuestAbandon popup
        UiArtRetry artRetry;
        int sel = -1;                // quest log index of the selected quest
        int listScroll = 0, detailScroll = 0, detailMax = 0;
        std::vector<int64_t> visibleRows;  // per visible row: header (key << 8 | 1) or quest (index << 8)
        std::vector<int32_t> collapsed;          // ZoneOrSort keys of collapsed headers
        std::unordered_map<int32_t, std::string> zoneNames;
        bool zonesLoaded = false;
        void release();
        ~QuestLogUi();
    };
    QuestLogUi qlog_;
    // WatchFrame (ui_world_watch.cpp) - the tracked quests under the minimap, collapsible.
    bool watchInit_ = false, watchCollapsed_ = false;
    std::vector<uint32_t> watchOff_;  // quests taken off the tracker (QuestLogFrameTrackButton)
    platform::psp::Texture watchBtn_;
    struct WatchRow {  // The tracker's lines, rebuilt twice a second
        std::string text;
        bool header, dash;
        float y;
    };
    std::vector<WatchRow> watchRows_;
    uint32_t watchRebuildMs_ = 0;
    bool questWatched(uint32_t questId) const;
    void setQuestWatched(uint32_t questId, bool on);
    void drawWatchFrame(UiRenderer& ui);
    bool watchAction(uint32_t act);
    void loadQuestLogArt();
    const std::string& questZoneName(int32_t zoneOrSort);
    bool drawQuestLogWindow(UiRenderer& ui, uint8_t w);
    bool questLogWindowAction(uint32_t act);
    // The quest texts as the NPC windows show them (ui_world_npc_frame.cpp): $-tokens and the
    // ruRU declension (text.pak), and their word wrap.
    std::string questText(const std::string& s);
    static std::vector<std::string> wrapQuestText(const Font& f, const std::string& text, float maxW);
    // NPC (end)

    // HUD fixes from hardware tests (ui_world_hud.cpp,
    // ui_world_sheath.cpp, ui_world_target.cpp, ui_world_combat.cpp). Plain
    // data only (nothing the tile loader thread touches), so it may follow
    // stream_.
    void drawHudLayers(UiRenderer& ui, const FrameStats& stats);  // one fixed HUD/window order (until task A)
    void drawPlayerPlates(UiRenderer& ui);
    void drawHudLines(UiRenderer& ui, const FrameStats& stats);
    void drawPerfHud(UiRenderer& ui, const FrameStats& stats);  // perfhud: fps + ping left of the minimap
    uint32_t reactionColor(uint64_t guid) const;
    // PSP (owner: "you cannot tell whom you selected"): the client's
    // selection circle on the ground under the target, in its reaction colour.
    void drawSelectionCircle(const rendering::psp::TerrainCamera& cam);
    void drawTargetOutline(const rendering::psp::TerrainCamera& cam);
    uint32_t selectionColor(uint64_t guid) const;                       // the circle's / rim's colour
    // The target's 3D portrait (CreatureRenderer::drawPortrait) in
    // the last 8 KB of a VRAM block reserved at the top with the glow's.
    uint32_t portraitVram_ = 0;
    platform::psp::Texture portraitTex_;
    uint64_t portraitGuid_ = 0;
    bool portraitOk_ = false;
    void initPortrait();
    void renderPortrait(const rendering::psp::TerrainCamera& cam);
    // The CharacterModelFrame - our model in a 128 x 128 VRAM target below the portrait's.
    uint32_t pdModelVram_ = 0;
    platform::psp::Texture pdModelTex_;
    bool pdModelOk_ = false;
    void renderPaperDollModel(const rendering::psp::TerrainCamera& cam);
    const platform::psp::Texture* portraitOf(uint64_t guid) const {
        return portraitOk_ && guid != 0 && guid == portraitGuid_ ? &portraitTex_ : nullptr;
    }
    platform::psp::Texture selCircle_;
    bool selCircleTried_ = false;  // name colour by faction reaction (ABGR)
    void updateSheath(float dtSec);
    void ownWeaponTypes(uint8_t& mainInv, uint8_t& offInv, uint8_t& rangedInv) const;
    bool ownOffHandShield() const;
    void drawOwnWeapons(bool draw);  // controls=2: the mode's sheath / unsheath
    bool camM2_ = true;        // wowee.cfg camm2: the camera stops at props (trees) too
    float camM2Min_ = 2.0f;    // wowee.cfg camm2min: only props at least this tall (yd)
    // Breadcrumbs of the first frames; members so a
    // second world entry (after a disconnect) logs its memory steps again.
    int firstUpdates_ = 0;
    float memWatchSec_ = 0.0f;  // The 10 s memory watch (pp::memWatchTick)
    int firstDraws_ = 0;
    bool hudLines_ = false;    // wowee.cfg hudlines (default 0): the debug lines (fps...); Debug -> "Numbers"
    int nameColors_ = 0;       // wowee.cfg namecolors: 0 upstream nameplates, 1 original client (green friendly)
    bool factionsOk_ = false;  // data/dbc.pak read: target filter + reaction colours
    int lastTargetAll_ = -1;
    uint32_t targetFilterLogs_ = 0;
    bool sheathAuto_ = true;   // wowee.cfg sheathauto: sheathed out of combat, drawn to attack
    float sheathDelay_ = 5.0f; // wowee.cfg sheathdelay: s out of combat before sheathing
    bool sheathRemote_ = true; // wowee.cfg sheathremote: other players by UNIT_FIELD_BYTES_2
    bool sheathInit_ = false;
    float calmSec_ = 0.0f;     // seconds without combat / attack intent
    struct RemoteSheath {
        const void* model = nullptr;  // the model it was applied to (a rebuilt one starts drawn)
        bool sheathed = false;
        uint8_t state = 0xFF;         // weapons: UNIT_FIELD_BYTES_2 byte 0 applied (0 / 1 / 2)
    };
    std::unordered_map<uint64_t, RemoteSheath> remoteSheath_;
    uint32_t sheathLogs_ = 0;
    // weapons: weapons, shields and their glows as the 3.3.5a client shows them
    // (ui_world_weapons.cpp, ui_world_sheath.cpp). Plain data, after stream_.
    struct WeaponFxUi;
    static void destroyWeaponFxUi(WeaponFxUi* p);
    std::unique_ptr<WeaponFxUi, void (*)(WeaponFxUi*)> weaponFx_{nullptr, nullptr};
    int weaponFxState_ = 0;  // 0 not opened yet, 1 on, -1 off (wowee.cfg enchantfx=0, an items.pak without "ivis")
    bool weaponFxEnsure();   // opens the glows once items.pak is (their table names the enchantments' visuals)
    // The weapon slots' Item.dbc sheath types and enchantment visuals from PLAYER_VISIBLE_ITEM_n (entry, enchantment).
    void fillWeaponVisuals(rendering::psp::Equipment& eq, const uint32_t entry[3], const uint32_t enchant[3]);
    void fillOwnWeaponVisuals(rendering::psp::Equipment& eq);                      // the own player's fields
    void fillRemoteWeaponVisuals(const game::Entity& e, rendering::psp::Equipment& eq);  // another player's
    void initRemoteSheath(rendering::psp::CharacterModel& m, const game::Entity& e);    // its state before load()
    void drawWeaponFx(const rendering::psp::TerrainCamera& cam);  // after the characters were drawn
    void updateOwnSheathState();  // the own model's state (0 / 1 / 2) and CMSG_SET_SHEATHED (end of updateSheath)
    bool ownSheathAway_ = true;   // the mode / automatic sheath's wish: weapons put away
    uint8_t ownSheathShown_ = 0xFF, ownSheathSent_ = 0xFF;
    bool sheathSend_ = true;      // wowee.cfg sheathsend: CMSG_SET_SHEATHED when the own state changes
    uint32_t ownRangedSeen_ = 0, ownRangedUntilMs_ = 0;  // own ranged shots: the ranged weapon in hand a while
    uint32_t ownMountSeen_ = 0;   // the own mount display seen last (mounting puts the weapons away)
    // weapons (end)
    // HUD fixes (end)

    // Inventory (ui_world_inventory.cpp) - temporary debug windows
    // "Character" / "Bags" over WorldSession's upstream inventory model, and
    // our model re-dressed when our equipment changes (upstream GameScreen:
    // consumeOnlineEquipmentDirty -> updateCharacterGeosets/Textures +
    // loadEquippedWeapons + setEquippedWeaponType). Members after stream_:
    // its loader thread never touches them.
    static constexpr uint8_t kWinCharacter = 0x50, kWinBags = 0x51, kWinBindConfirm = 0x52;
    struct InvHeld {  // Square pick-up (upstream cursor item), wire numbering
        bool active = false;
        uint8_t bag = 0xFF, slot = 0;
    };
    void updateInventory(float dtSec);
    void inventoryWindowPad(const platform::psp::PadState& pad);
    bool inventoryWindowAction(uint32_t act);
    bool drawInventoryWindow(UiRenderer& ui, uint8_t w);
    // "Character" and "Bags" as two GameMenuFrame buttons (was one row of two).
    void drawInventoryMenuRow(UiRenderer& ui, const Rect& character, const Rect& bags, bool focusable);
    void syncOwnEquipment();
    void ownEquipmentOverride(rendering::psp::Equipment& equip);
    bool resolveOwnEquipment(rendering::psp::Equipment& want, int& unresolved);
    void applyOwnLoadout();
    bool inventoryActivateSlot(uint32_t id);
    bool inventoryAltAction(uint32_t id);  // Circle = the right click
    void inventoryPickPut(uint32_t id);    // Cross / Square = the left click
    bool inventoryIdToWire(uint32_t id, uint8_t& bag, uint8_t& slot) const;
    const game::ItemDef* inventoryItemAt(uint32_t id) const;
    void drawItemSlot(UiRenderer& ui, uint32_t id, const Rect& r, const game::ItemDef* item, const char* emptyLabel,
                      bool focusable);
    void drawItemTooltip(UiRenderer& ui, const game::ItemDef& item, const Rect& anchor);
    bool invInit_ = false;
    int ownEquipMode_ = 1;          // wowee.cfg ownequip: 0 CHAR_ENUM only, 1 inventory, 2 visible fields
    int invBag_ = 0;                // Bags window: 0 backpack, 1..4 worn bags
    InvHeld invHeld_;
    uint32_t invTipId_ = 0;         // focused slot with an item this frame (tooltip)
    Rect invTipRect_{};             // screen coordinates
    uint32_t tipProtoItem_ = 0, tipProtoCount_ = 1;  // A merchant item's tooltip, after the windows
    Rect tipProtoRect_{};
    uint32_t ownEquipCheckMs_ = 0, ownEquipChangeMs_ = 0;
    std::string invStatus_;         // window status line (last action / error)
    uint32_t invStatusMs_ = 0, invSeenErrorMs_ = 0;
    uint32_t invEventLogs_ = 0, equipLogs_ = 0;
    bool invLoadoutDirty_ = false;  // weapon loadout from the inventory, next sync
    struct InvTipLine {
        std::string text;
        uint32_t color;
    };
    std::vector<InvTipLine> invTipLines_;  // tooltip cache (rebuilt when the item or inventory changes)
    uint64_t invTipKey_ = ~0ull;
    float invTipW_ = 0.0f;
    // ContainerFrame (ui_world_bags.cpp) - the backpack and the worn bags as the original's
    // container frames, stacked from the bottom right (updateContainerFrameAnchors), each with its
    // close button; the bag bar of MainMenuBar (MainMenuBarBackpackButton, CharacterBag0..3Slot) under
    // them. Slot ids kIdBagSlot0 + container * 36 + slot (container 0 = backpack).
    static constexpr uint32_t kIdBagSlot0 = 0x6C00, kIdBagClose0 = 0x6CF0;
    struct BagArt {
        platform::psp::Texture backpack, comps, backpackBtn, emptyBag;
        bool tried = false;
        UiArtRetry artRetry;
        void release();
        ~BagArt();
    };
    BagArt bagArt_;
    bool bagOpen_[5] = {};          // ContainerFrame shown: backpack, worn bags 1..4
    void openBags(bool all);        // OpenAllBags / OpenBackpack
    void loadBagArt();
    void drawContainerFrames(UiRenderer& ui, bool focusable);
    static bool bagSlotId(uint32_t id, int& container, int& slot);
    // CharacterFrame + PaperDollFrame (ui_world_paperdoll.cpp): the frame, the 19 slots with the
    // empty-slot art, the model (rendered into VRAM like the NPC portraits), the level line, the two stat
    // blocks (PLAYERSTAT_* categories, Cross cycles them as the dropdowns), resistances, the tabs.
    struct PaperDollArt {
        platform::psp::Texture frame, rotLeft, rotRight;
        platform::psp::Texture slot[21];  // PD_SLOTS order 0..18, 19 bag, 20 ammo (ui.pak pd_slot_*)
        bool tried = false;
        UiArtRetry artRetry;
        void release();
        ~PaperDollArt();
    };
    PaperDollArt pdArt_;
    int pdStatLeft_ = 0, pdStatRight_ = -1;  // PLAYERSTAT_DROPDOWN_OPTIONS index; -1 = by class
    float pdModelYaw_ = 0.0f;
    void loadPaperDollArt();
    bool drawPaperDoll(UiRenderer& ui, bool focusable);
    bool paperDollAction(uint32_t act);
    // The tab of a framed window (CharacterFrameTabButtonTemplate, ui.pak tab_*): three slices, the width of
    // its label; the selected one raised 5 units. Returns its width.
    float drawFrameTab(UiRenderer& ui, uint32_t id, float x, float y, float h, const char* label, bool selected,
                       bool focusable);
    platform::psp::Texture tabArt_[2];
    bool tabArtTried_ = false;
    // character tabs (ui_world_char_tabs.cpp): CharacterFrame's Reputation and Skills tabs - ReputationFrame
    // (factions under their headers, standing bars, ReputationDetailFrame: description, at war, inactive, watch)
    // and SkillFrame (skills under the SkillLineCategory headings, the bars, the details, UNLEARN_SKILL). Their art
    // and texts (ui.pak rep_* / skill_*, text.pak faction_text / skill_text) only while their tab is shown.
    enum : uint8_t { kCharTabPaperDoll = 0, kCharTabRep = 1, kCharTabSkills = 2 };
    static constexpr uint8_t kWinUnlearnSkill = 0x53;  // StaticPopup UNLEARN_SKILL (over the skills tab)
    uint8_t charTab_ = kCharTabPaperDoll;
    struct CharTabs;
    static void destroyCharTabs(CharTabs* p);
    std::unique_ptr<CharTabs, void (*)(CharTabs*)> charTabs_{nullptr, nullptr};
    CharTabs& charTabs();
    void drawCharacterFrameTabs(UiRenderer& ui, bool focusable);  // CharacterFrameTab1/3/4 (ui_world_paperdoll.cpp)
    void selectCharTab(uint8_t tab);                               // CharacterFrameTab_OnClick
    bool drawCharTab(UiRenderer& ui, bool focusable);              // the Reputation / Skills tab of the window
    bool charTabAction(uint32_t act);
    bool drawUnlearnSkillPopup(UiRenderer& ui, uint8_t w);
    void drawCharTabsOverlay(UiRenderer& ui);  // after all windows: the tabs' GameTooltip
    void charTabsClosed();                     // the window is closed: PaperDollFrame next time, art / texts freed
    // character tabs (end)
    // dressing room (src/ui/psp/ui_world_dressup.cpp): DressUpFrame.xml / .lua
    // of the original - DressUpItemLink (the original's Ctrl+click on an item; PSP: L + Triangle on the item under
    // the cursor - bags, the character window, the merchant, loot, quest rewards, chat links; the tooltip says
    // so), DressUpModel (a view of player_ sharing its model: CharacterModel::loadView), TryOn, "Reset" (Dress),
    // the rotate buttons (Model_OnLoad / Model_RotateLeft / Right / Model_OnUpdate). Its art (ui.pak
    // du_frame_<race>) and model only while it is open. The character window (ui_world_paperdoll.cpp): the same
    // rotation, the model in its Stand pose, "Show Helm" / "Show Cloak" (ShowHelm / ShowCloak).
    static constexpr uint8_t kWinDressUp = 0xFC;  // window stack id
    struct DressUpUi;
    static void destroyDressUpUi(DressUpUi* p);
    std::unique_ptr<DressUpUi, void (*)(DressUpUi*)> du_{nullptr, nullptr};
    DressUpUi& dressUpUi();
    struct ModelSpin {  // a model frame's self.rotation (UIParent.lua Model_* functions)
        float rotation = 0.61f;  // Model_OnLoad; radians, + = the face towards the viewer's left (left button held)
        uint32_t lastMs = 0;     // Model_OnUpdate's elapsed
        uint32_t pushed = 0;     // the rotate button pushed last frame (OnClick on LeftButtonDown)
    };
    ModelSpin pdSpin_;                 // CharacterModelFrame (pdModelYaw_ is no longer read)
    uint32_t duHoverItem_ = 0;         // the item under the cursor last frame (L + Triangle tries it on)
    void dressUpItemLink(uint32_t itemId);  // DressUpItemLink
    // The item's ItemDisplayInfo and InventoryType for TryOn: 1 dressable (IsDressableItem: a weapon or armour
    // the model can wear), 0 not, -1 not known yet (asked for when `ask`).
    int dressUpResolve(uint32_t itemId, uint32_t& displayId, uint8_t& inventoryType, bool ask);
    bool dressableItem(uint32_t itemId);  // the tooltip's PSP line "L + Triangle: Dressing Room"
    float dressUpKeys(UiRenderer* ui, float x, float y, float h);  // draws "L + [Triangle]" (null: width only)
    void dressUpPad(platform::psp::PadState& pad);  // updateWindows: L + Triangle (before the loot window's Triangle)
    void dressUpTick();                              // drawWindows, every frame: what a closed window held goes
    void dressUpHover();                             // drawWindows, after the windows: duHoverItem_, a chat link's tooltip
    bool drawDressUpWindow(UiRenderer& ui, uint8_t w);
    bool dressUpAction(uint32_t act);                // Cross on the ids 0x6300..0x63BF
    void dressUpBaseChanged();                       // loadCharacter: the view lets go of player_ before it reloads
    void drawDressUpOverlay(UiRenderer& ui);         // after the windows: the check boxes' GameTooltip
    void drawShowChecks(UiRenderer& ui, const Rect& modelSquare, bool focusable);  // the character window's
    void modelSpinTick(ModelSpin& s, uint32_t leftId, uint32_t rightId);           // Model_OnUpdate
    void modelSpinClick(ModelSpin& s, bool left);                                  // Model_RotateLeft / Right
    // dressing room (end)
    // Inventory (end)

    // FrameXML + Lua (ui_world_framexml.cpp, wowee.cfg framexml=0/1).
    // Declared after stream_ on purpose: destroyed first, it owns no renderer.
    struct FrameXml;
    static void destroyFrameXml(FrameXml* p);
    std::unique_ptr<FrameXml, void (*)(FrameXml*)> fx_{nullptr, nullptr};
    bool fxInit_ = false;
    void updateFrameXml(float dtSec);
    void drawFrameXml(UiRenderer& ui);

    // Trainers + talents (ui_world_trainer.cpp) - the class trainer
    // window, the talent tree, the talent-reset confirm and the spell book's
    // skill line tabs over WorldSession's upstream models. Plain data after
    // stream_ (the loader thread never touches it).
    static constexpr uint8_t kWinTrainer = 0x90, kWinTalents = 0x91, kWinTalentWipe = 0x92;
    static constexpr uint32_t kIdMenuTalents = 0x6700;  // "Talents" in the Menu window
    void updateTrainer(float dtSec);
    bool trainerWindowAction(uint32_t act);
    bool drawTrainerWindow(UiRenderer& ui, uint8_t w);
    void drawTrainerList(UiRenderer& ui, bool top);
    void drawTalents(UiRenderer& ui, bool top);
    void drawTalentWipe(UiRenderer& ui, bool top);
    void openTalentWindow();
    void loadTalentArt();
    struct TrainerUi {
        bool init = false;
        bool shown[2] = {};         // session windows (trainer, talent wipe) as last synced
        bool raisedCursor = false, guard = false;
        bool showKnown = false;     // wowee.cfg trainerknown: list known services too
        bool talentClick = false;   // wowee.cfg talentclick: Cross on the selected talent learns
        bool talentArt = true;      // wowee.cfg talentart: tree backgrounds from ui.pak
        int tab = 0, page = 0, talTab = 0, talScroll = 0;
        uint32_t sel = 0, talSel = 0;
        bool talSelLearnable = false;
        std::vector<uint32_t> rowIds, talIds;  // what the last frame showed (actions)
        bool bgLoaded = false;
        platform::psp::Texture bg[3], rankBorder;
        // ClassTrainerFrame / PlayerTalentFrame of the original (ui_world_trainer.cpp).
        bool filter[3] = {true, true, false};  // TRAINER_FILTER_AVAILABLE / _UNAVAILABLE / _USED
        bool filterOpen = false;               // the filter dropdown's list is shown
        int listScroll = 0;                    // FauxScrollFrame offset (rows)
        uint32_t listVersion = ~0u;            // trainerVersion of the selection (TRAINER_UPDATE)
        std::vector<std::string> collapsed;    // collapsed skill line headers (by name)
        std::vector<int64_t> visibleRows;      // the rows the last frame showed (spell id, or header index | 1<<40)
        float talScrollU = 0.0f;               // PlayerTalentFrameScrollFrame offset (frame units)
        uint32_t talFocus = 0;                 // the talent under the focus last frame (scroll follows it)
        TrainerUi() = default;
        TrainerUi(const TrainerUi&) = delete;
        TrainerUi& operator=(const TrainerUi&) = delete;
        ~TrainerUi();
    };
    TrainerUi trainer_;
    // The composed frames and small art (ui.pak ct_* / tal_*), loaded while a window is open.
    struct TrainerArt {
        platform::psp::Texture ctFrame, hl, plusHl, slot, plus, minus, check;
        platform::psp::Texture talFrame, talSlot, branches, arrows;
        bool ctTried = false, talTried = false;
        void releaseTrainer();
        void releaseTalents();
        TrainerArt() = default;
        TrainerArt(const TrainerArt&) = delete;
        TrainerArt& operator=(const TrainerArt&) = delete;
        ~TrainerArt();
    };
    TrainerArt trArt_;
    struct TrainerTip {  // a GameTooltip drawn after all the windows (talents)
        std::vector<std::pair<std::string, uint32_t>> lines;  // the first in the large font
        Rect anchor{};
    } trTip_;
    void loadTrainerFrameArt();
    void loadTalentFrameArt();
    bool trainerAltAction(uint32_t alt);         // Circle on a trainer row: learn it (PSP)
    void drawTrainerOverlay(UiRenderer& ui);     // after the windows: the talent tooltip, UIErrorsFrame
    void trainerError(const std::string& text);  // UI_ERROR_MESSAGE on the red line
    std::string trainerReqText(const game::TrainerSpell& s);  // why a service is unavailable (the red line)
    int bookTab_ = 0;  // spell book: the skill line tab shown
    // Trainers + talents (end)
    // trade skills (ui_world_tradeskill.cpp): TradeSkillFrame (the TradeSkillUI addon's .xml / .lua)
    // over the session's crafting window (WorldSession::isCraftingWindowOpen / getCraftingRecipes /
    // startCraftQueue): the recipes under the crafted items' subclass headings (+ / -, "All"), coloured by
    // TradeSkillTypeColor, "[N]" makeable, "Have Materials", the subclass / slot dropdowns, the details (icon,
    // made count, cooldown, description, reagents "have /need"), "Create All" / count / "Create" (or the
    // line's AlternateVerb), "Exit", TradeSkillRankFrame. Its art (ui.pak ts_*) only while it is open.
    static constexpr uint8_t kWinTradeSkill = 0xB8;  // window stack id (trade skills)
    struct TradeSkillUi;
    static void destroyTradeSkillUi(TradeSkillUi* p);
    std::unique_ptr<TradeSkillUi, void (*)(TradeSkillUi*)> ts_{nullptr, nullptr};
    TradeSkillUi& tradeSkillUi();
    void updateTradeSkill(float dtSec);          // the session's window -> the stack (WorldScreen::update)
    bool drawTradeSkillWindow(UiRenderer& ui, uint8_t w);
    bool tradeSkillAction(uint32_t act);         // Cross (the left click)
    bool tradeSkillAltAction(uint32_t alt);      // Circle on a recipe: select + Create (PSP)
    bool tradeSkillCloseDropDown();              // START closes an open dropdown list first (CloseDropDownMenus)
    void drawTradeSkillOverlay(UiRenderer& ui);  // after the windows: UIErrorsFrame over the frame, the tooltip
    // trade skills (end)
    // Tutorials (ui_world_tutorial.cpp) - the client's TutorialFrame
    // (queue, alert button, Prev / Next) over WorldSession's tutorial flags and
    // data/tutorial.pak; "Menu -> Tips". Plain data, after stream_.
    struct TutorialUi;
    static void destroyTutorialUi(TutorialUi* p);
    std::unique_ptr<TutorialUi, void (*)(TutorialUi*)> tut_{nullptr, nullptr};
    static constexpr uint8_t kWinTutorial = 0xA0, kWinTutorialMenu = 0xA1;
    void updateTutorial(float dtSec);
    bool tutorialWindowAction(uint32_t act);
    bool drawTutorialWindow(UiRenderer& ui, uint8_t w);
    void drawTutorialAlert(UiRenderer& ui);
    void drawTutorialMenuButton(UiRenderer& ui, const Rect& button, bool focusable);  // A menu button
    void tutorialNew(uint32_t id);
    void tutorialShow(uint32_t id);
    void tutorialAlertClick();
    void tutorialHidden(bool showNext);
    // Map - minimap (HUD, own GE pass "minimap") and the world map window
    // (Menu -> Map) over data/minimap_<map>.pak (ui_world_minimap.cpp, ui_world_map.cpp,
    // include/ui/psp/ui_world_map.hpp). Last member: its tile reader thread stops first.
    static constexpr uint8_t kWinWorldMap = 0x98;
    static constexpr uint32_t kIdMenuMap = 0x6800;  // "Map" in the Menu window
public:
    struct MapUi;  // public: named by the helpers of ui_world_minimap.cpp / ui_world_map.cpp
private:
    static void destroyMapUi(MapUi* p);
    std::unique_ptr<MapUi, void (*)(MapUi*)> map_{nullptr, nullptr};
    void initMap();
    void drawMinimap(UiRenderer& ui);    // HUD layer order (ui_world_hud.cpp); updates too
    float minimapReserveW() const;       // px the minimap cluster takes at the top right
    bool drawMapWindow(UiRenderer& ui, uint8_t w);
    bool mapWindowAction(uint32_t act);
    void openWorldMap();
    bool mapMini() const;  // The world map in its mini mode (a movable window, no blackout)
    // The D-pad on the continent view walks the zone spots (updateWindows, before the focus ring).
    void mapZonePad(platform::psp::PadState& p);
    // The full-screen world map is up - the HUD is hidden (the original hides UIParent).
    bool fullMapOpen() const { return windowOpen(kWinWorldMap) && !mapMini(); }
    // world map: WorldMapFrame - Circle on the map = the right click (zoom out), START
    // closes an open dropdown list first (CloseDropDownMenus).
    bool mapWindowAltAction(uint32_t alt);
    bool mapCloseDropDown();
    // Map (end)
    // FrameXML - the next frames (ui_world_framexml_game.cpp): which
    // built-in UI FrameXML replaces (FrameXml::HideBits), the session queue
    // items the built-in drains forward, a built-in window FrameXML opens.
    enum : uint8_t { kFxHideBar = 1, kFxHideCast = 2, kFxHideAuras = 4, kFxHideBook = 8, kFxHideBags = 16, kFxHideChar = 32 };
    bool fxHides(uint8_t bit) const;
    void fxForwardInventoryEvent(const platform::psp::WorldSession::InventoryEvent& ev);
    void fxForwardCastEnd(uint8_t ended, uint32_t spellId);
    bool fxOpenWindow(uint8_t w);
    void fxWindowPad(const platform::psp::PadState& pad);
    // StaticPopup.xml / .lua and GameMenuFrame.xml of the original (ui_world_popup.cpp; the
    // API is described in that file's header). One scale for the dialog boxes: the popup grows with its
    // text (StaticPopup_Resize) and the PSP font is ~1.8x the 0.6 frames' FontStrings.
    static constexpr float kDialogScale = 0.8f;  // tools/psp/build_ui_pak.py POPUP_S
    static constexpr uint8_t kWinQuestAbandon = 0x5E;  // StaticPopup ABANDON_QUEST (over the quest log)
    struct StaticPopup {
        const char* text = "";           // formatted (SetFormattedText), wrapped to 290 units
        const char* button1 = nullptr;   // null: no button 1
        uint32_t button1Id = 0;
        bool button1Enabled = true;      // StartDelay: button1:Disable()
        const char* button2 = nullptr;   // null: button 1 alone, centred
        uint32_t button2Id = 0;
        bool button2Enabled = true;
        bool hasMoneyFrame = false;      // SmallMoneyFrame under the text
        uint32_t money = 0;              // copper
        bool focusable = false;          // the window's focus rule (buttons declare stops)
        const char* editText = nullptr;  // hasEditBox (StaticPopupEditBoxTemplate) - its text; null = none
        uint32_t editId = 0;             // its focus stop (Cross: the keyboard, the caller's action)
        const platform::psp::Texture* alert = nullptr;  // auction: showAlert (420 wide, the icon at LEFT 24,0)
    };
    Rect drawStaticPopup(UiRenderer& ui, const StaticPopup& p);
    // UI-DialogBox-Background + -Border (the Backdrop of StaticPopupTemplate / GameMenuFrame) and the
    // UI-DialogBox-Header with its GameFontNormal title (GameMenuFrame).
    void drawDialogBox(UiRenderer& ui, const Rect& r);
    void drawDialogHeader(UiRenderer& ui, const Rect& frame, const char* title);
    // GameMenuButtonTemplate: UIPanelButtonTemplate art, GameFontHighlight (white) text.
    void drawGameMenuButton(UiRenderer& ui, uint32_t id, const Rect& r, const char* label, bool focusable,
                            bool enabled = true);
    void drawGameMenu(UiRenderer& ui);    // ui_world_windows.cpp (its ids)
    bool drawQuestAbandon(UiRenderer& ui, uint8_t w);  // ui_world_questlog.cpp
    struct PopupArt {
        platform::psp::Texture border, header, btnUp, btnDown, btnDis, btnHl;
        bool tried = false;
        void release();
        ~PopupArt();
    };
    PopupArt popupArt_;
    float popupStackY_ = 0.0f;  // StaticPopup2..4 anchor TOP to the previous one's BOTTOM (per frame)
    void loadPopupArt();
    void beginPopups();         // drawWindows: the stack restarts; the art goes when no dialog is open
    // minimap: MinimapCluster of the original around the disc (ui_world_minimap.cpp; state in
    // MapUi::Cluster, ui_world_map.hpp): MinimapBorderTop + MinimapZoneText in the zone's PvP colour,
    // MinimapNorthTag / MinimapCompassTexture, MinimapZoomIn / Out, MiniMapTracking and its dropdown (a window),
    // MiniMapWorldMapButton, MiniMapMailFrame, GameTimeFrame, TimeManagerClockButton; their GameTooltip is
    // drawn after the windows.
    static constexpr uint8_t kWinMinimapTracking = 0x9A;  // MiniMapTrackingDropDown (window stack id)
    bool minimapAction(uint32_t act);                    // ahead of mapWindowAction (the zoom buttons too)
    bool drawMinimapWindow(UiRenderer& ui, uint8_t w);   // the tracking dropdown
    void drawMinimapTooltip(UiRenderer& ui);             // after the windows
    void drawMinimapCluster(UiRenderer& ui);             // drawMinimap: the cluster's art and buttons
    void refreshMinimapBlips();                          // drawMinimap: the blip list (every 8 frames)
    void refreshMinimapCluster();                        // drawMinimap: zone text / PvP type, tracking, clock
    // minimap (end)

    // tooltips (ui_world_tooltip.cpp): GameTooltip as the ruRU 3.3.5a client builds it (the
    // order and colours of the client's item / spell tooltip code, GlobalStrings ruRU), ShoppingTooltip1/2
    // ("Currently Equipped": hold L = IsModifiedClick("COMPAREITEMS"); merchant items always, as
    // MerchantItemButton_OnEnter), the spell tooltips of the book, the action bar and the trainer (spells.pak
    // "stip": costs, ranges, cast / cooldown, reagents, the description with its $-tokens resolved).
    struct GameTip;  // the lines of one tooltip (ui_world_tooltip.cpp)
    struct TipState;
    static void destroyTipState(TipState* p);
    std::unique_ptr<TipState, void (*)(TipState*)> tipState_{nullptr, nullptr};
    TipState& tipState();
    bool tipCompareHeld_ = false;  // L held in the interface (updateWindows): Shift of the original
    uint32_t tipActionSlot_ = 0;   // the action button under the cursor + 1 this frame (ActionButton_SetTooltip)
    void buildItemTip(GameTip& t, const game::ItemDef& item, bool shopping, bool atMerchant);
    void buildSpellTip(GameTip& t, uint32_t spellId);
    void layoutTip(GameTip& t);
    void drawTip(UiRenderer& ui, const GameTip& t, float x, float y);
    void drawSpellTooltips(UiRenderer& ui);  // drawWindows, after the windows: book / trainer / action bar
    void tipFrame(bool interfaceMode);       // drawActionBar, every frame: resets tipActionSlot_, art out of the UI
    // ClassTrainerSkillDescription (GetTrainerServiceDescription) in its box, as many lines as fit.
    void drawTrainerDescription(UiRenderer& ui, uint32_t spellId, const Rect& box);
    std::string spellDescription(uint32_t spellId);  // the description, $-tokens resolved (GetSpellDescription)
    // tooltips (end)
    // party (ui_world_party.cpp): PartyMemberFrame1..4 (PartyFrameTemplates.xml /
    // PartyMemberFrame.lua) under the player frame, the unit menus of PlayerFrame / TargetFrame / the party
    // frames (UnitPopup.lua SELF / PARTY / PLAYER as a UIDropDownMenu: Circle = the right click, Cross on the
    // target frame too), StaticPopup PARTY_INVITE, the leader / master looter icons of PlayerFrame /
    // TargetFrame. Its art (ui.pak pf_*) only while in a group or a menu is open. State behind a pointer
    // (made on first use: the WorldScreen constructor is not touched).
    static constexpr uint8_t kWinUnitPopup = 0xC8, kWinPartyInvite = 0xC9;
    // GroupLootFrame1..4 (LootFrame.xml / .lua: the rolls of the session) and StaticPopup CONFIRM_LOOT_ROLL.
    static constexpr uint8_t kWinGroupLoot = 0xCA, kWinLootRollConfirm = 0xCB;
    struct PartyUi;
    static void destroyPartyUi(PartyUi* p);
    std::unique_ptr<PartyUi, void (*)(PartyUi*)> party_{nullptr, nullptr};
    PartyUi& partyUi();
    void updateParty();                               // drawWindows (every frame): session -> popup, menu, art
    void drawPartyFrames(UiRenderer& ui);             // drawUnitFrames ("frames" pass)
    void drawPartyFrameExtras(UiRenderer& ui, float x, float y, bool self);  // after a Player/TargetFrame
    bool partyWindowAction(uint32_t act);             // Cross (the left click)
    bool partyAltAction(uint32_t alt);                // Circle (the right click)
    void partyWindowPad(const platform::psp::PadState& pad);  // a click outside the menu closes it
    bool drawPartyWindow(UiRenderer& ui, uint8_t w);
    void openUnitPopup(uint64_t guid, uint8_t menu, float x, float y);
    void drawUnitMenu(UiRenderer& ui);                // the UIDropDownMenu lists (level 1, the nested one)
    void drawGroupLoot(UiRenderer& ui);               // GroupLootFrame1..4
    // party (end)
    // taxi: TaxiFrame.xml / .lua of the original and the taxi flight (ui_world_taxi.cpp):
    // the window follows the session's taxi window (SMSG_SHOWTAXINODES), the flight moves the
    // player (WorldSession::taxiDrivesPlayer), the landing clamp after it. Its state and art in TaxiUi (only while
    // the window is open; the art is released after it closes).
public:
    struct TaxiUi;

private:
    static void destroyTaxiUi(TaxiUi* p);
    std::unique_ptr<TaxiUi, void (*)(TaxiUi*)> taxi_{nullptr, nullptr};
    TaxiUi& taxiUi();
    static constexpr uint8_t kWinTaxi = 0xC4;       // window stack id
    void updateTaxi(float dtSec);                   // the window <-> the session, the flight's bookkeeping
    bool taxiMovement(float dtSec);                 // from updateMovement: true = the flight / landing moves us
    bool taxiLocksControls() const;                 // ui_world_controls: no stick, jump, sit while flying
    bool taxiGossipAutoSelect();                    // syncNpcWindows: GossipFrame_OnEvent's lone taxi option
    bool drawTaxiWindow(UiRenderer& ui, uint8_t w);
    bool taxiWindowAction(uint32_t act);
    void drawTaxiTooltip(UiRenderer& ui);           // after all windows: TaxiNodeOnButtonEnter's GameTooltip
    // taxi (end)
    // bank and mail: BankFrame (ui_world_bank.cpp) and MailFrame / OpenMailFrame (ui_world_mail.cpp)
    // of the original over the session's bank / mail half (psp_world_session_mail.cpp); the bank's bags in the
    // ContainerFrame stack (ui_world_bags.cpp). State and art in BankMailUi (src/ui/psp/ui_world_bank_mail.hpp),
    // made when one of them opens and freed when they are all closed.
    static constexpr uint8_t kWinBank = 0xB0, kWinBankBuy = 0xB1, kWinMail = 0xB4, kWinOpenMail = 0xB5,
                             kWinMailPopup = 0xB6;
    static constexpr uint32_t kIdBankBagSlot0 = 0x7500;  // + bank bag * 36 + slot (the bank bags' ContainerFrames)
    struct BankMailUi;
    static void destroyBankMailUi(BankMailUi* p);
    std::unique_ptr<BankMailUi, void (*)(BankMailUi*)> bm_{nullptr, nullptr};
    BankMailUi& bankMail();
    void updateBankMail(float dtSec);                     // ui_world_npc.cpp updateNpc: events, windows, keyboard
    bool drawBankMailWindow(UiRenderer& ui, uint8_t w);   // drawWindows
    bool bankMailAction(uint32_t act);                    // Cross (the left click) on one of their ids
    bool bankMailAltAction(uint32_t alt);                 // Circle (the right click): also a bag item at the bank /
                                                          // with the send tab shown (UseContainerItem)
    void bankMailWindowPad(platform::psp::PadState& pad); // Square on their slots (pick up / put down)
    void drawBankMailOverlay(UiRenderer& ui);             // after the windows: their tooltips
    bool tryUseMailbox(float npcDist);                    // L + up: the nearest mailbox, nearer than any NPC
    bool bankMailItemLocked(uint8_t bag, uint8_t slot) const;  // a bag item on the letter (drawn locked)
    // ContainerFrame for the bank's bags (ToggleBag(5..11)), continuing updateContainerFrameAnchors' stack.
    void drawBankBagFrames(UiRenderer& ui, bool focusable, float& colRight, float& bottom, int& shown);
    void syncBank();
    bool drawBankFrame(UiRenderer& ui);
    bool drawBankBuyPopup(UiRenderer& ui);
    bool bankFrameAction(uint32_t act, bool alt);
    void bankPickPut(uint32_t id);
    bool bankIdToWire(uint32_t id, uint8_t& bag, uint8_t& slot) const;
    const game::ItemDef* bankItemAt(uint32_t id) const;
    void syncMail();
    void mailOskTick();
    bool drawMailFrame(UiRenderer& ui);
    bool drawOpenMailFrame(UiRenderer& ui);
    bool drawMailPopup(UiRenderer& ui);
    bool mailFrameAction(uint32_t act, bool alt);
    void mailShowPopup(uint8_t kind);
    void mailResetSend();
    // bank and mail (end)
    // auction (src/ui/psp/ui_world_auction.cpp): AuctionFrame of the original
    // (the AuctionUI addon, ruRU: the tabs Browse / Bids / Auctions, the filters, the sort columns, the sell slot,
    // AuctionProgressFrame; StaticPopup BUYOUT_AUCTION / BID_AUCTION / CANCEL_AUCTION / AUCTION_HOUSE_DISABLED) over
    // the session's auction half (psp_world_session_auction.cpp). Windows 0x16-0x19, widgets 0x8200-0x83FF; the art
    // (ui.pak auc_*) only while the frame is open; the state behind a pointer, made when it opens, freed when closed.
    static constexpr uint8_t kWinAuction = 0x16, kWinAuctionPopup = 0x17, kWinAuctionDropDown = 0x18,
                             kWinAuctionDisabled = 0x19;
    struct AuctionUi;
    static void destroyAuctionUi(AuctionUi* p);
    std::unique_ptr<AuctionUi, void (*)(AuctionUi*)> auc_{nullptr, nullptr};
    AuctionUi& auctionUi();
    // What the Lua frame keeps between two visits (it is not destroyed when hidden): the browse filters, the price
    // and duration dropdowns, LAST_ITEM_AUCTIONED / _COUNT / _START_BID / _BUYOUT, CVar auctionDisplayOnCharacter.
    struct AuctionKeep {
        std::string name, minLevel, maxLevel;
        bool usable = false, showOnPlayer = false;
        int quality = -1, classIndex = 0, subIndex = 0, invTypeIndex = 0;
        int priceType = 2, duration = 2;  // PRICE_TYPE_STACK, 24 h
        std::string lastItem;
        uint32_t lastCount = 0, lastStartBid = 0, lastBuyout = 0;
    };
    AuctionKeep aucKeep_;
    uint32_t aucTipBid_ = 0, aucTipBuyout_ = 0;     // AuctionFrameItem_OnEnter's per-unit lines (ui_world_tooltip.cpp)
    bool aucTipRow_ = false;                         // the item tooltip is an auction row's (compared always)
    void updateAuction();                            // ui_world_npc.cpp updateNpc: the session's events -> the frame
    bool drawAuctionWindow(UiRenderer& ui, uint8_t w);
    bool auctionAction(uint32_t act);                // Cross (the left click) on its widgets
    bool auctionAltAction(uint32_t alt);             // Circle (the right click): a bag item -> the sell slot
    void auctionWindowPad(platform::psp::PadState& pad);  // Square on the sell slot (the drag)
    void drawAuctionOverlay(UiRenderer& ui);         // after the windows: its one-line tooltips
    bool auctionItemLocked(uint8_t bag, uint8_t slot) const;  // the bags draw the sell slot's item locked
    bool auctionKeepsPopupArt() const;               // beginPopups: a dialog of ours is open
    void auctionRefresh(int list);                   // the selected row's bid box and buttons (*_Update)
    // OpenMailInvoiceFrame on an open letter (ui_world_mail.cpp): false when the letter is no auction invoice.
    bool drawAuctionInvoice(UiRenderer& ui, const game::MailMessage& m, float x0, float y0, float sx, float sy);
    // auction (end)
    // map transfer: the world is destroyed and built again on every
    // transfer between maps (LoginScreen), so what the implicit destruction left behind is freed here; a map with a
    // global WMO (a dungeon: terrain_<dir>.pak holds only "wmo_only") stands on the WMO's floors - no terrain;
    // a map whose paks are missing says so (red line + chat) instead of a void without a word.
public:
    ~WorldScreen();

private:
    bool wmoOnly_ = false;        // no terrain: horizontal moves need a floor, not a terrain tile
    glm::vec3 wmoSafePos_{0.0f};  // the last place stood on (a fall through a hole of the WMO's collision)
    bool wmoSafeValid_ = false;
    uint32_t wmoVoidFalls_ = 0;
    void mapDataMissing(uint32_t mapId, const char* what);
    // INSTANCE_BOOT / INSTANCE_LOCK (StaticPopup, ui_world_instance.cpp): window ids 0xD0 / 0xD1, buttons 0x7D00..
    static constexpr uint8_t kWinInstanceBoot = 0xD0, kWinInstanceLock = 0xD1;
    void updateInstancePopups();                      // the session's timers -> the window stack
    bool drawInstancePopup(UiRenderer& ui, uint8_t w);  // drawWindows
    bool instancePopupAction(uint32_t act);           // Cross on its buttons
    bool instanceRaisedUi_ = false;                   // the cursor came up for INSTANCE_LOCK
    // Test aids (ui_world_devtools.cpp): wowee.cfg gmtools=1 (the GM window: the server's GM chat
    // commands) and devcmd=1 (the PC drives the client through devcmd.txt next to the EBOOT). Window 0x3E, ids 0x93xx.
    static constexpr uint8_t kWinGmTools = 0x3E;
    struct DevState {
        bool inited = false, gmtools = false, devcmd = false;
        int page = 0, screen = 0;
        uint32_t lastPollMs = 0;
    };
    DevState dev_;
    bool gmToolsOn() const;
    void devtoolsInit();
    void devtoolsUpdate();                     // every frame: the PC's command file
    void gmSend(const char* cmds);             // SAY lines, '|' between commands
    void drawGmDebugButton(UiRenderer& ui, float x, float y);
    bool drawGmWindow(UiRenderer& ui, uint8_t w);
    bool gmWindowAction(uint32_t act);
    bool devCommand(const std::string& line);
    // The on-device autopilot runs the same verbs (platform/psp/psp_autotest.hpp).
    static bool autotestExec(void* self, const std::string& line);
    bool devDeleteItem(bool open, uint32_t itemId);  // ui_world_delete_item.cpp: "open|close delete [item]"
    bool devDressUp(uint32_t itemId);                // ui_world_dressup.cpp: "open dressup [item]"
    // Destroying an item - StaticPopup DELETE_ITEM / DELETE_GOOD_ITEM (ui_world_delete_item.cpp); window
    // 0x30, buttons from 0x8C00.
    static constexpr uint8_t kWinDeleteItem = 0x30;
    struct DeleteItemState {
        uint8_t bag = 0xFF, slot = 0;  // wire position of the item the popup names
        uint32_t itemId = 0;
        std::string name;
        std::string typed;             // DELETE_GOOD_ITEM's edit box
        bool good = false;             // quality >= 3: the confirm string is asked for
        bool osk = false;              // the keyboard is up for the edit box
        bool open = false;
        platform::psp::Texture alert;  // showAlert - UI-Dialog-Icon-AlertNew (ui.pak "mail_alert"), while open
        bool alertTried = false;
    };
    DeleteItemState del_;
    const game::ItemDef* itemAtWire(uint8_t bag, uint8_t slot) const;
    bool deleteItemPad(const platform::psp::PadState& pad);  // the held item put down on the world
    bool drawDeleteItemPopup(UiRenderer& ui, uint8_t w);
    bool deleteItemAction(uint32_t act);
    void deleteItemTick();                                    // updateInventory: closed by START = ClearCursor
    bool mountCfgRead_ = false;  // wowee.cfg mounts, read once per world (was a function static: once per run)
    // map transfer (end)
    // raid and battlegrounds: the raid and the battlegrounds of the original over the
    // session's raid / battleground half (psp_world_session_raid.cpp, _battleground.cpp):
    //   ui_world_raid.cpp - FriendsFrame's Raid tab (RaidFrame + the RaidUI addon: RaidGroup1..8, RaidGroupButton,
    //     RaidClassButton, RaidPullout frames), ReadyCheckFrame and the ready check icons, the raid target marks
    //     (UnitPopup RAID_TARGET_ICON, TargetFrame's icon, the icons over the units), "Social" in the game menu;
    //   ui_world_battleground.cpp - BattlefieldFrame, StaticPopup CONFIRM_BATTLEFIELD_ENTRY (one per queue),
    //     MiniMapBattlefieldFrame + MiniMapBattlefieldDropDown, WorldStateScoreFrame, the group-joined lines;
    //   ui_world_worldstate.cpp - WorldStateAlwaysUpFrame and WorldStateCaptureBar (WorldStateUI.dbc, ui.pak ws_ui).
    // State and art in RaidBgUi (src/ui/psp/ui_world_raid_bg.hpp), made on first use; each frame's art only while
    // it is shown.
    // kWinDropDown: MiniMapBattlefieldDropDown or a pullout's RaidPulloutDropDown (one UIDropDownMenu at a time);
    // kWinPullouts: the RaidPullout frames (PSP: side by side in one window - R + stick moves them together).
    static constexpr uint8_t kWinRaid = 0xE4, kWinReadyCheck = 0xE5, kWinBattlefield = 0xE6, kWinBgConfirm = 0xE7,
                             kWinScore = 0xE8, kWinDropDown = 0xE9, kWinPullouts = 0xEA, kWinBgConfirm2 = 0xEB;

public:
    struct RaidBgUi;

private:
    static void destroyRaidBgUi(RaidBgUi* p);
    std::unique_ptr<RaidBgUi, void (*)(RaidBgUi*)> rbg_{nullptr, nullptr};
    RaidBgUi& raidBg();
    void updateRaidBg();                               // drawWindows, every frame: session -> windows / lines / art
    bool drawRaidBgWindow(UiRenderer& ui, uint8_t w);  // drawWindows
    bool raidBgAction(uint32_t act);                   // Cross (the left click)
    bool raidBgAltAction(uint32_t alt);                // Circle (the right click)
    void drawRaidBgOverlay(UiRenderer& ui);            // after the windows: their GameTooltip
    void drawRaidBgHud(UiRenderer& ui);                // drawUnitFrames: marks over the units, pullouts, world states
    void drawRaidFrameExtras(UiRenderer& ui, float x, float y, bool self);  // PlayerFrame ready check, target's mark
    void drawPartyReadyCheck(UiRenderer& ui, uint64_t guid, const Rect& portrait);  // PartyMemberFrameN
    void drawBattlefieldMinimapButton(UiRenderer& ui, const platform::psp::Texture* cluster, bool focusable);
    void drawRaidMenuButton(UiRenderer& ui, const Rect& r, bool focusable);  // GameMenuFrame: "Social"
    const platform::psp::Texture* raidTargetIcons();   // UI-RaidTargetingIcons (also the unit menu's rows)
    void raidBgWindowPad(const platform::psp::PadState& pad);  // Square: pick up / drop a raid member, pullouts
    // ui_world_party.cpp: the unit menu of a RaidGroupButton / pullout (UnitPopup RAID) from the raid code.
    void openRaidUnitPopup(uint64_t guid, uint8_t menu, float x, float y, uint32_t opener);
    // ui_world_raid.cpp
    void updateRaid();
    bool drawRaidFrame(UiRenderer& ui);
    bool drawReadyCheckFrame(UiRenderer& ui);
    bool drawPulloutMenu(UiRenderer& ui);
    bool raidFrameAction(uint32_t act, bool alt);
    void drawRaidPullouts(UiRenderer& ui);
    void drawRaidMarks(UiRenderer& ui);
    void drawReadyIcon(UiRenderer& ui, uint64_t guid, const Rect& r);
    void openRaidPullout(uint8_t kind, uint32_t id, const std::string& name);
    // ui_world_battleground.cpp
    void updateBattleground();
    bool drawBattlefieldFrame(UiRenderer& ui);
    bool drawBattlefieldConfirm(UiRenderer& ui, int slot);
    bool drawBattlefieldMenu(UiRenderer& ui);
    bool drawScoreFrame(UiRenderer& ui);
    bool battlegroundAction(uint32_t act, bool alt);
    std::string battlefieldName(int slot);
    // ui_world_worldstate.cpp
    void loadRaidBgData();
    void drawWorldStates(UiRenderer& ui);
    // raid and battlegrounds (end)
    // expansion windows: the 3.3.5a windows of the original ruRU client -
    // the Dungeon Finder (LFDFrame.xml / LFGFrame.xml / Minimap.xml MiniMapLFGFrame / AlertFrames.xml: LFDParentFrame,
    // LFDDungeonReadyPopup, LFDRoleCheckPopup, StaticPopup VOTE_BOOT_PLAYER / LFG_OFFER_CONTINUE, LFDSearchStatus,
    // DungeonCompletionAlertFrame - ui_world_lfd.cpp), the achievements (the AchievementUI addon: AchievementFrame,
    // AchievementAlertFrame - ui_world_achievements.cpp) and the calendar (the Calendar addon: CalendarFrame and its
    // event / holiday / raid views - ui_world_calendar.cpp). State and art behind pointers, made on first use, the art
    // (data/wotlk.pak, never kept open) freed when the windows close; the constructor is not touched.
    static constexpr uint8_t kWinLfd = 0xEC, kWinLfdReady = 0xED, kWinLfdRoleCheck = 0xEE, kWinLfgBoot = 0xEF,
                             kWinLfgOffer = 0xF0, kWinWotlkDropDown = 0xF1, kWinAchievements = 0xF2,
                             kWinCalendar = 0xF3, kWinCalendarEvent = 0xF4, kWinWotlkPopup = 0xF5;
    static constexpr uint32_t kIdMenuAchievements = 0x79F0, kIdMenuLfd = 0x79F1;  // the game menu's buttons
    struct LfdUi;
    struct AchUi;
    struct CalUi;
    static void destroyLfdUi(LfdUi* p);
    static void destroyAchUi(AchUi* p);
    static void destroyCalUi(CalUi* p);
    std::unique_ptr<LfdUi, void (*)(LfdUi*)> lfd_{nullptr, nullptr};
    std::unique_ptr<AchUi, void (*)(AchUi*)> ach_{nullptr, nullptr};
    std::unique_ptr<CalUi, void (*)(CalUi*)> cal_{nullptr, nullptr};
    LfdUi& lfdUi();
    AchUi& achUi();
    CalUi& calUi();
    uint32_t wotlkLastMs_ = 0;                 // updateWotlk's clock (drawWindows has no dt)
    // The hooks (ui_world_windows.cpp, ui_world_minimap.cpp, ui_world_popup.cpp).
    void updateWotlk();                        // drawWindows, every frame: the session's events -> windows, popups, alerts
    void drawWotlkAlerts(UiRenderer& ui);      // drawWindows, under the windows: the alert frames (UIParent)
    bool drawWotlkWindow(UiRenderer& ui, uint8_t w);
    void drawWotlkOverlay(UiRenderer& ui);     // after the windows: their GameTooltip, LFDSearchStatus
    bool wotlkWindowAction(uint32_t act);      // Cross; the minimap's calendar button: ToggleCalendar (then the minimap's)
    bool wotlkAltAction(uint32_t alt);         // Circle (the right click)
    bool wotlkCloseDropDown();                 // START: an open dropdown list closes first (CloseDropDownMenus)
    bool wotlkKeepsPopupArt() const;           // beginPopups: a dialog of ours is open (the StaticPopup art stays)
    void drawWotlkMenuButtons(UiRenderer& ui, const Rect& achievements, const Rect& lfd, bool focusable);  // GameMenu
    void drawMinimapLfg(UiRenderer& ui, bool focusable);  // MiniMapLFGFrame (drawMinimapCluster)
    // UIPanelButtonTemplate(2) in three slices with the small font (the panels' 22-unit buttons are 12 px high).
    void wotlkButton(UiRenderer& ui, uint32_t id, const Rect& r, const char* label, bool enabled, bool focusable);
    // GameTooltip lines drawn after all the windows (one per frame: the focused widget's). The first line in the
    // header font; `money` > 0 adds a SmallMoneyFrame (SetTooltipMoney); `right` = AddDoubleLine's right text.
    struct WotlkTipLine {
        std::string text;
        uint32_t color = 0xFFFFFFFFu;
        uint32_t money = 0;
        std::string right;
        uint32_t rightColor = 0xFFFFFFFFu;
        WotlkTipLine() = default;
        WotlkTipLine(std::string t, uint32_t c = 0xFFFFFFFFu) : text(std::move(t)), color(c) {}
    };
    enum : uint8_t { kTipRight = 0, kTipLeft, kTipTop, kTipBottom };  // ANCHOR_RIGHT / _LEFT / _TOP / _BOTTOM
    void wotlkTip(const Rect& anchorOnScreen, std::vector<WotlkTipLine> lines, uint8_t anchor = kTipRight);
    // UIDropDownMenu list (DropDownList1: the tooltip backdrop, a row per item, UI-CheckBox-Check on the checked one,
    // titles gold, disabled grey) at x, y (window units, inside the window's draw); item k is focus stop idBase + k.
    struct WotlkDdItem {
        std::string text;
        bool checked = false, disabled = false, isTitle = false;
        std::string tipTitle, tipText;  // tooltipTitle / tooltipText (tooltipWhileDisabled)
    };
    void wotlkDropDownList(UiRenderer& ui, float x, float y, const std::vector<WotlkDdItem>& items, uint32_t idBase,
                           bool focusable);
    // Dungeon Finder (ui_world_lfd.cpp)
    uint8_t lfgMode(uint8_t* sub = nullptr);  // GetLFGMode (UIParent.lua): 0 none, then the kMode* of the file
    bool lfdEmpowered() const;                // LFD_IsEmpowered
    bool lfgPartyLfg() const;                 // IsPartyLFG
    void lfdUpdate(float dtSec);
    void lfdToggle();                         // ToggleLFDParentFrame
    void lfdRebuildList();                    // LFDQueueFrame_Update
    void lfdSetType(uint32_t type);           // LFDQueueFrame_SetType
    void lfdJoin();                           // LFDQueueFrame_Join
    std::string lfdDeclinedMessage(uint32_t dungeonId);  // LFDConstructDeclinedMessage
    void lfdShowPopup(uint8_t w, uint32_t focusId);      // StaticPopupSpecial_Show / StaticPopup_Show of ours
    bool drawLfdFrame(UiRenderer& ui);
    bool drawLfdReadyPopup(UiRenderer& ui);
    bool drawLfdRoleCheck(UiRenderer& ui);
    bool drawLfgVotePopup(UiRenderer& ui, uint8_t w);    // VOTE_BOOT_PLAYER / LFG_OFFER_CONTINUE
    bool drawWotlkDropDown(UiRenderer& ui);               // MiniMapLFGFrameDropDown (a window)
    void drawLfdSearchStatus(UiRenderer& ui);
    void drawLfgCompletionAlert(UiRenderer& ui, float& stackTop);
    bool lfdAction(uint32_t act);
    // Achievements (ui_world_achievements.cpp)
    void achUpdate(float dtSec);
    void achToggle();                         // ToggleAchievementFrame
    bool achAvailable() const;                // CanShowAchievementUI: the data is in
    bool drawAchievementFrame(UiRenderer& ui);
    void drawAchievementAlerts(UiRenderer& ui, float& stackTop);
    bool achAction(uint32_t act);
    bool achAltAction(uint32_t alt);
    bool achCloseDropDown();
    // Calendar (ui_world_calendar.cpp)
    void calUpdate(float dtSec);
    void calToggle();                         // ToggleCalendar
    bool drawCalendarFrame(UiRenderer& ui);
    bool drawCalendarEventFrame(UiRenderer& ui);
    bool drawCalendarPopup(UiRenderer& ui);
    bool calAction(uint32_t act);
    bool calAltAction(uint32_t alt);
    bool calCloseDropDown();
    // expansion windows (end)
    // inspect: InspectFrame of the original (the InspectUI addon:
    // InspectFrame / InspectPaperDollFrame / InspectPVPFrame / InspectTalentFrame) over the session's inspect half
    // (psp_world_session_inspect.cpp), opened by the unit menu's "Inspect" (UnitPopup INSPECT -> InspectUnit). The
    // inspected player's model is his CharacterModel of the world (remotes_) drawn into its own 128 x 128 VRAM target
    // after the 3D pass; the art (ui.pak insp_*, pd_slot_*, pd_rot_*) only while the window is open. Window 0xFA,
    // widgets 0x6F00..0x6FFF. State behind a pointer, made on first use (the constructor is not touched).
    static constexpr uint8_t kWinInspect = 0xFA;
    struct InspectUi;
    static void destroyInspectUi(InspectUi* p);
    std::unique_ptr<InspectUi, void (*)(InspectUi*)> insp_{nullptr, nullptr};
    InspectUi& inspectUi();
    uint32_t inspectHoverItem() const;  // The inspected slot's item under the cursor (L + Triangle)
    uint32_t inspModelVram_ = 0;            // the model's target (initPortrait: below the character window's)
    platform::psp::Texture inspModelTex_;
    bool inspModelOk_ = false;
    void openInspect(uint64_t guid, bool fromTarget);  // InspectUnit -> InspectFrame_Show (CanInspect, showError)
    bool canInspect(uint64_t guid, bool showError);     // CanInspect
    bool inspectMenuEnabled(uint64_t guid) const;      // UnitPopup INSPECT: alive, CheckInteractDistance(unit, 1)
    void updateInspect();                              // drawWindows, every frame: the unit, OnHide, the art
    void renderInspectModel(const rendering::psp::TerrainCamera& cam);  // after the 3D pass (with the paperdoll's)
    bool drawInspectWindow(UiRenderer& ui, uint8_t w);
    bool inspectAction(uint32_t act);                  // Cross (the left click)
    void drawInspectOverlay(UiRenderer& ui);           // after the windows: its GameTooltip
    void drawInspectTab(UiRenderer& ui, uint8_t tab, bool focusable);  // InspectPVPFrame / InspectTalentFrame
    void inspectTabShown(uint8_t tab);                 // the subframe's OnShow (RequestInspectHonorData...)
    void drawInspectTip(UiRenderer& ui, const Rect& anchorOnScreen, const char* text, uint32_t color);
    // PLAYER_LEVEL for another character (ui_world_npc_frame.cpp: text.pak's names and declension).
    std::string unitLevelText(uint8_t race, uint8_t cls, bool female, const char* level);
    // inspect (end)
    // big WMOs (capitals and every WMO over the doodad
    // bake's budget) from the doodads pak's "wbig" record (ui_world_bigwmo.cpp, rendering::psp::BigWmo). State behind
    // a pointer, made when the map has any; declared last: its loader thread stops before the renderers it feeds.
    struct BigWmoState;
    static void destroyBigWmoState(BigWmoState* p);
    std::unique_ptr<BigWmoState, void (*)(BigWmoState*)> bigWmo_{nullptr, nullptr};
    void loadBigWmoIndex(platform::psp::Pak& doodads);  // loadDoodads: "wbig"
    void primeBigWmos();                                // world entry, after startStreaming: near chunks now
    void updateBigWmos(float dtSec);                    // update(), after the streamer
    void drawBigWmos(const rendering::psp::TerrainCamera& cam, uint32_t fogABGR);  // before the M2 pass
    bool bigWmoFloor(float x, float y, float probeZ, float& outZ) const;
    // The landing hold: (x, y, z) inside a big WMO's box? ready: that WMO is open with its collision around
    // the player resident (col: resident / wanted chunks). While the hold waits, the WMO reads past the low-memory gate.
    void bigWmoLanding(float x, float y, float z, bool& inside, bool& ready, uint32_t& resident, uint32_t& wanted);
    void bigWmoWalls(glm::vec3& to, float feetZ) const;
    float bigWmoRay(const glm::vec3& origin, const glm::vec3& dir, float maxDist) const;
    // big WMOs (end)
    // transports (ui_world_transports.cpp): the WMO game objects
    // (data/transports.pak "gw_<display>": ships, zeppelins, the icebreaker, WMO lifts and doors) and the moving
    // transports (GAMEOBJECT_TYPE_TRANSPORT 11 and _MO_TRANSPORT 15, WMO or M2) moved by the original client's rules
    // (psp_transport_path.cpp), drawn by two more RAM-only model renderers (transports.pak / gameobjects.pak), their
    // collision a floor and walls; riding one (MOVEMENTFLAG_ONTRANSPORT, the place in its frame), the riders drawn
    // where their transport carries them. State behind a pointer, made on first use (the constructor is not touched).
public:
    struct TransportsUi;  // (named by ui_world_transports.cpp's helpers)
private:
    static void destroyTransportsUi(TransportsUi* p);
    std::unique_ptr<TransportsUi, void (*)(TransportsUi*)> tr_{nullptr, nullptr};
    TransportsUi& transportsUi();
    void updateTransports(float dtSec);  // update(), before the movement: the objects, their places, the riders
    void drawTransports(const rendering::psp::TerrainCamera& cam, uint32_t fogABGR);  // with the game objects
    bool transportCarry(float dtSec);    // updateMovement's start: carried along; true = hold still (map change)
    void transportAfterMove();           // updateMovement's end: boarding / leaving, the place in the frame
    bool transportFloor(float x, float y, float maxZ, float& z) const;   // floorAt: a deck, a lift's floor
    void transportWalls(glm::vec3& to, float feetZ) const;               // the hulls' walls
    float transportCameraRay(const glm::vec3& origin, const glm::vec3& dir, float maxDist) const;
    bool transportOwns(uint64_t guid, uint32_t displayId);               // updateGameObjects leaves it to us
    bool transportRiding() const;
    bool transportRemotePos(uint64_t guid, glm::vec3& pos, bool& moving) const;  // updateRemotePlayers: no easing
    void transportMoveInfo(game::MovementInfo& mi);                      // sendMove: ONTRANSPORT + the place
    // transports (end)
    // flying (ui_world_flying.cpp): flight (a flying mount, the flight form, any server CAN_FLY) and swimming in 3D
    // after upstream CameraController / MovementHandler (psp_flight_rules.hpp): the jump key takes off with CAN_FLY and
    // climbs while held, the sit key descends, the camera tilt pitches the flight / the swim, MSG_MOVE_START_ASCEND /
    // _START_DESCEND / _STOP_ASCEND / _START_SWIM / _STOP_SWIM with the pitch in every packet; the landing on the
    // ground, the fall when CAN_FLY goes in the air; the swimmer's swim set, the flier's Fly / Hover, other players'
    // fliers and swimmers.
    bool flightMovement(const platform::psp::PadState& pad, float dtSec);  // updateMovement's start: true = moved here
    void flightMoveInfo(game::MovementInfo& mi);                          // sendMove: the flags and the pitch
    bool flightMountAnim();                                               // updateMountedAnimation: in the water
    bool flightOwnAnim();                                                 // updateAnimation: flying on foot
    bool ownSwimming() const { return (flyFlags_ & 0x200000u) != 0; }     // SWIMMING: the swim set
    bool ownFlying() const { return (flyFlags_ & 0x2000000u) != 0; }      // FLYING: Fly / Hover, MountFSM's flight
    bool ownSwimOrFly() const { return (flyFlags_ & 0x2200000u) != 0; }   // no sitting, the orbit may go under
    float flightCamMinPitch() const;                                      // update(): the orbit may go under the player
    bool flightCamFree() const { return (flyFlags_ & 0x2200000u) != 0; }  // draw(): no feet clamp, no step easing
    void flightControls(const platform::psp::PadState& pad, bool combatMode);  // updateControls: climb keys held
    bool flightTakesCircle() const { return (flyFlags_ & 0x2200000u) != 0; }  // updateControls: ○ descends, no sit
    bool flightWaterFloor(float x, float y, float maxZ, float& z) const;  // floorAt: water walking
    bool flightRemoteAnim(uint64_t guid, RemotePlayer& r, bool moving);   // updateRemotePlayers: swim set / Fly
    bool flightRemoteKeepsZ(uint64_t guid) const;                         // updateRemotePlayers: no floor snap
    bool flightRemoteSwims(uint64_t guid) const;                          // appendMountUnits: the mount's swim set
    bool flightRemoteFlies(uint64_t guid) const;                          // appendMountUnits: the mount's Fly / Hover
    void flightSend(game::Opcode op);                                     // a movement packet of the flight / swim
    uint32_t flyFlags_ = 0;  // our FLYING | SWIMMING | ASCENDING | DESCENDING (the granted ones: the session)
    float flyPitch_ = 0.0f, sentPitch_ = 0.0f;
    uint32_t nextPitchMs_ = 0, flySeenToggle_ = 0xFFFFFFFFu, flyLogs_ = 0, flyGranted_ = 0;
    uint32_t flyFrameLogMs_ = 0;
    bool flyAscKey_ = false, flyDescKey_ = false, flyInit_ = false, flyMountDriven_ = false;
    uint32_t flyMountAnim_ = 0;
    // flying (end)
    // game object use (ui_world_gouse.cpp): the game objects one can
    // use - the PSP has no mouse, so the object the client's cursor would be over is the nearest usable one in reach
    // in front of the character (the "hover": its model highlighted as the client's mouseover, its GameTooltip as
    // the client builds it, the ↑ glyph over it); ↑ uses it (OnRightClick, the session's useGameObject), L + ↑
    // picks the next one in reach. ItemTextFrame (window 0x12: a text object's pages) and BarberShopFrame (0x13),
    // a chair's / the barber chair's stand state from the server, the fishing bobber's bite, quest objects' sparkle.
    // Widgets 0x8100..0x81FF. State behind a pointer, made on first use; the art only while it is needed.
    static constexpr uint8_t kWinItemText = 0x12, kWinBarber = 0x13;
    struct GoUseUi;
    static void destroyGoUseUi(GoUseUi* p);
    std::unique_ptr<GoUseUi, void (*)(GoUseUi*)> gou_{nullptr, nullptr};
    GoUseUi& goUseUi();
    void updateGameObjectUse(float dtSec);  // update(): the session's events, the hover, the windows
    bool tryUseGameObject(float npcDist);   // interactTarget (↑): the hovered object when nearer than npcDist
    void cycleGameObjectHover();            // L + ↑ (controls=2): the next object in reach
    void drawGameObjectHighlight(const rendering::psp::TerrainCamera& cam);  // draw(): after the objects' models
    void drawGameObjectHud(UiRenderer& ui);   // drawHudLayers: the ↑ glyph, the quest sparkles
    void drawGameObjectTip(UiRenderer& ui);   // drawHudLayers (own pass): the hovered object's GameTooltip
    bool drawGameObjectWindow(UiRenderer& ui, uint8_t w);  // drawWindows: ItemTextFrame, BarberShopFrame
    bool gameObjectWindowAction(uint32_t act);  // Cross
    bool gameObjectAltAction(uint32_t alt);     // Circle
    bool goSphere(const platform::psp::WorldSession::GameObjectInfo& g, glm::vec3& center, float& radius) const;
    // loadCharacter: the own look from our PLAYER_BYTES / PLAYER_BYTES_2 (a haircut changes them; CHAR_ENUM's are the
    // login's), the barber shop's preview while BarberShopFrame is open; after the load: the chair's stand state again.
    void ownLookOverride(rendering::psp::CharacterLook& look);
    void ownLookLoaded();
    void barberSync();
    bool drawBarberFrame(UiRenderer& ui);
    // The animated objects (creatures.pak "goanim"): units of creatures_ with the client's state animations.
    bool goAnimatedDisplay(uint32_t displayId) const;
    void loadGoAnimList();
    void appendGameObjectUnits(std::vector<rendering::psp::CreatureUnit>& out);  // syncCreatures
    void drawGameObjectPost(const rendering::psp::TerrainCamera& cam);          // draw(), after the creatures
    void goNoteStandState(uint8_t st);  // applyServerStandState -> the chair's loop
    // game object use (end)
    // item use (ui_world_item_use.cpp): using items - ○ on a bag item
    // (UseContainerItem), ○ on a worn item with a use spell (UseInventoryItem), the targeting of item spells
    // (SpellIsTargeting / SpellTargetItem), StaticPopup USE_BIND, the item buttons of the action bar (icon, count,
    // grey without the item, the worn border, the cooldown, PlaceAction of a held item), the bag cooldown sweep, the
    // tooltip's cooldown line, the stand state the realm sets (food and drink). Window 0x10, widgets 0x8000..0x80FF.
    static constexpr uint8_t kWinUseBind = 0x10;  // StaticPopup USE_BIND (window stack id)
    struct ItemUseArt {
        platform::psp::Texture border;  // ui.pak "ab_border" (UI-ActionButton-Border), loaded with the first worn item button
        ~ItemUseArt();
    };
    ItemUseArt itemUseArt_;
    bool itemUseInit_ = false, itemUseArtTried_ = false, itemUseRaisedUi_ = false;
    bool bookPickItem_ = false;  // bookPick_ is an item entry (PickupAction of an item button), not a spell
    void updateItemUse();                                   // drawWindows, every frame
    void applyServerStandState(uint8_t st);                 // SMSG_STANDSTATE_UPDATE
    void itemUseFeedback(platform::psp::WorldSession::ItemUse r, const game::ItemDef* item);
    bool itemUseContainerClick(int bag, int slot, const game::ItemDef& item);  // ○ on a bag item
    bool itemUseWornClick(int equipSlot, const game::ItemDef& item);           // ○ on a worn item (false: none)
    bool itemUseTargetClick(uint32_t id);                  // SpellTargetItem (true = taken)
    bool itemUseEscape();                                   // START: SpellStopTargeting (true = taken)
    void drawItemCooldownOverlay(UiRenderer& ui, const Rect& r, uint32_t entry);  // ContainerFrame_UpdateCooldown
    std::string itemCooldownText(uint32_t entry) const;     // GameTooltip ITEM_COOLDOWN_TIME / COOLDOWN_ON_LEAVE_COMBAT
    uint32_t itemCooldownTipKey(uint32_t entry) const;      // the tooltip's rebuild key part
    void useItemAction(uint32_t entry);                     // UseAction on an item button
    const platform::psp::Texture* itemActionIcon(uint32_t entry);
    uint8_t itemActionUsable(uint32_t entry);               // actionUsable for an item button
    void drawItemActionCooldown(UiRenderer& ui, const Rect& r, uint32_t entry);
    void drawItemActionOverlay(UiRenderer& ui, const Rect& r, uint32_t entry);  // count + worn border
    bool placeHeldItemOnAction(int slot);                   // PickupContainerItem + PlaceAction
    bool drawItemUseWindow(UiRenderer& ui, uint8_t w);      // StaticPopup USE_BIND
    bool itemUseWindowAction(uint32_t act);
    void drawItemUseOverlay(UiRenderer& ui);                // the targeting cursor
    // item use (end)
    // social: FriendsFrame of the original - the Friends tab
    // (FriendsTabHeader Friends / Ignore, FriendsFrameStatusDropDown), WhoFrame (Who), GuildFrame (Guild)
    // with its side frames (GuildMemberDetailFrame, GuildInfoFrame, GuildControlPopupFrame, GuildEventLogFrame) -
    // ui_world_social.cpp / ui_world_guild.cpp; the Raid tab stays the raid window (kWinRaid): a tab switch
    // swaps the two windows in place. StaticPopup GUILD_INVITE and the confirmations (the edit-box popups: the PSP
    // keyboard), the FRIEND unit menus (ui_world_party.cpp), the chat's /friend /ignore /who /g* commands, the guild
    // tabard emblem of the player and the others (Equipment::emblem). State and art in SocialUi
    // (src/ui/psp/ui_world_social.hpp), made on first use; the art only while the frame is open.
    // Windows 0x1A-0x21, widgets 0x8400-0x86FF (tasks_6.md).
    static constexpr uint8_t kWinFriends = 0x1A, kWinGuildInvite = 0x1B, kWinSocialConfirm = 0x1C,
                             kWinSocialDropDown = 0x1D;
    struct SocialUi;
    static void destroySocialUi(SocialUi* p);
    std::unique_ptr<SocialUi, void (*)(SocialUi*)> soc_{nullptr, nullptr};
    SocialUi& socialUi();
    void updateSocial();                                  // drawWindows, every frame: session -> windows, popups, art
    bool drawSocialWindow(UiRenderer& ui, uint8_t w);     // drawWindows
    bool socialAction(uint32_t act);                      // Cross (the left click)
    bool socialAltAction(uint32_t alt);                   // Circle (the right click)
    void socialWindowPad(const platform::psp::PadState& pad);  // a click outside a dropdown closes it
    void drawSocialOverlay(UiRenderer& ui);               // after the windows: their GameTooltip
    bool socialKeepsPopupArt() const;                     // beginPopups: GUILD_INVITE / a confirmation is open
    bool socialCloseDropDown();                           // START: an open dropdown list closes first
    void socialOskTick();                                 // the PSP keyboard's text for the edit box it was opened for
    void toggleFriendsFrame(int tab);                     // ToggleFriendsFrame(tab): 0 the last tab (the game menu)
    void drawFriendsTabs(UiRenderer& ui, int selected, bool focusable);  // FriendsFrameTab1..5 (the raid tab too)
    void socialSwapFrame(uint8_t from, uint8_t to);       // the Raid tab <-> the others: the window in place
    void socialUnitMenuAction(uint8_t btn, uint64_t unit, const std::string& name);  // the unit menus' social rows
    void socialChatRequest(int code, const std::string& arg);  // /who, /friends, /ignore, /groster, /gdisband
    void socialEditPopup(int field, const std::string& arg);   // a StaticPopup with an edit box: the keyboard
    void socialConfirm(int kind, const std::string& arg);      // StaticPopup CONFIRM_GUILD_* / REMOVE_GUILDMEMBER
    void fillGuildEmblem(uint32_t guildId, const rendering::psp::Equipment& base, rendering::psp::Equipment& equip);
    // ui_world_social.cpp
    bool drawFriendsFrame(UiRenderer& ui);
    void drawFriendsList(UiRenderer& ui, bool focusable);
    void drawIgnoreList(UiRenderer& ui, bool focusable);
    void drawWhoFrame(UiRenderer& ui, bool focusable);
    bool drawGuildInvitePopup(UiRenderer& ui);
    bool drawSocialConfirm(UiRenderer& ui);
    bool drawSocialDropDown(UiRenderer& ui);
    void loadSocialSmallArt();
    void loadSocialFrameArt(int kind);
    void socialFriendsFrameUpdate();                      // FriendsFrame_Update (the tab's content, ShowFriends)
    std::string socialZoneName(uint32_t area);            // the AreaTable name (text.pak "zones"), "" while read
    std::string socialClassName(uint8_t cls, bool female);  // ChrClasses / ChrRaces ruRU (text.pak "names")
    std::string socialRaceName(uint8_t race, bool female);
    std::string socialDecline(const std::string& text);   // |3-N(word) through text.pak "decl"
    void socialWhoChat();                                 // the /who lines (WHO_LIST_FORMAT, WHO_NUM_RESULTS)
    void socialSetWhoLookup();                            // the who tags' names for the session's SendWho
    void socialTip(UiRenderer& ui, uint32_t id, const Rect& r, const std::string& title, const std::string& text,
                   uint32_t titleColor = 0xFFFFFFFFu);
    // ui_world_party.cpp: a unit menu for a name (FriendsFrame_ShowDropdown: the FRIEND menus of a friend / who /
    // guild row; `guid` 0 when only the name is known) at the cursor; the same opener again closes it.
    void openNamedUnitPopup(uint64_t guid, const std::string& name, uint8_t menu, float x, float y, uint32_t opener);
    // ui_world_guild.cpp
    void drawGuildFrame(UiRenderer& ui, bool focusable);
    void drawGuildSideFrame(UiRenderer& ui, bool focusable);
    void drawGuildMemberDetail(UiRenderer& ui, bool focusable);
    void drawGuildInfoFrame(UiRenderer& ui, bool focusable);
    void drawGuildEventLog(UiRenderer& ui, bool focusable);
    void drawGuildControlPopup(UiRenderer& ui, bool focusable);
    bool guildAction(uint32_t act, bool alt);
    void guildPopupShow(int popup);                       // GuildFramePopup_Show (the others hide)
    void guildControlLoadRank(int rank);                  // GuildControlSetRank + GuildControlPopupframe_Update
    std::string guildLastOnline(float days);              // GuildFrame_GetLastOnline (RecentTimeDate)
    // social (end)
    // trade and duels: TradeFrame and the StaticPopups of the trade request (TRADE), the
    // duels and the server's requests (DUEL_REQUESTED, DUEL_OUTOFBOUNDS, CONFIRM_SUMMON, QUEST_ACCEPT / _LOG_FULL,
    // TRADE_POTENTIAL_BIND_ENCHANT) over the session's half (psp_world_session_trade_duel.cpp): ui_world_trade_duel.cpp.
    // Windows 0x22..0x27 and 0x2C, widgets 0x8700..0x87FF; the frame's art (ui.pak trade_frame / trade_hl) only while
    // a trade is open. State behind a pointer, made on first use (the constructor is not touched).
    static constexpr uint8_t kWinTrade = 0x22, kWinDuelRequest = 0x23, kWinDuelBounds = 0x24, kWinSummon = 0x25,
                             kWinQuestAccept = 0x26, kWinTradeBind = 0x27, kWinTradeRequest = 0x2C;
    struct TradeDuelUi;
    static void destroyTradeDuelUi(TradeDuelUi* p);
    std::unique_ptr<TradeDuelUi, void (*)(TradeDuelUi*)> tradeDuel_{nullptr, nullptr};
    TradeDuelUi& tradeDuelUi();
    void updateTradeDuel();                     // drawWindows, every frame: the session's events -> the windows
    void tradeDuelOskTick();                    // the money boxes' keyboard
    bool drawTradeDuelWindow(UiRenderer& ui, uint8_t w);
    bool tradeDuelAction(uint32_t act);         // Cross (the left click)
    bool tradeDuelAltAction(uint32_t alt);      // Circle (the right click)
    void drawTradeDuelOverlay(UiRenderer& ui);  // after the windows: the trade items' GameTooltip
    bool tradeDuelKeepsPopupArt() const;        // beginPopups: one of these popups is open
    bool tradeTakesBagItem(uint32_t invId);     // UseContainerItem while trading (ui_world_inventory.cpp)
    uint8_t tradeDuelMenuMask(uint64_t guid) const;  // UnitPopup TRADE (bit 0) / DUEL (bit 1) enabled
    // trade and duels (end)
    // pets (ui_world_pets.cpp): PetFrame (PetFrame.xml / .lua: the portrait, health / mana /
    // focus, the happiness icon, the attack glow, debuffs), PetActionBarFrame (PetActionBarFrame.xml / .lua: the ten
    // PetActionButtons over MainMenuBar - commands, reactions, spells, autocast, cooldowns), PetStableFrame (PetStable.xml
    // / .lua), the StaticPopups RENAME_PET (PSP: the keyboard), PETRENAMECONFIRM, ABANDON_PET, CONFIRM_BUY_STABLE_SLOT,
    // TALENTS_INVOLUNTARILY_RESET_PET, the pet's tab of PlayerTalentFrame (PlayerSpecTab "petspec1") and of
    // CharacterFrame (PetPaperDollFrame). Windows 0x28..0x2B, widgets 0x8800..0x89FF. State and art behind a pointer
    // (made on first use; the art only while a pet / the stable / a popup needs it).
    static constexpr uint8_t kWinPetStable = 0x28, kWinPetRename = 0x29, kWinPetPopup = 0x2A;
    static constexpr uint8_t kCharTabPet = 3;  // CharacterFrameTab2 (PetPaperDollFrame)
    static constexpr uint32_t kIdCharTabPet = 0x8870;  // its tab button (ui_world_paperdoll.cpp draws it)
    struct PetUi;
    static void destroyPetUi(PetUi* p);
    std::unique_ptr<PetUi, void (*)(PetUi*)> pet_{nullptr, nullptr};
    PetUi& petUi();
    void updatePets();                                 // drawWindows, every frame: session -> windows, popups, art
    void drawPetFrame(UiRenderer& ui);                 // drawUnitFrames: PetFrame under the player frame
    void drawPetActionBar(UiRenderer& ui);             // drawSpells: PetActionBarFrame over the main bar
    bool petBarShown() const;                          // PetActionBarFrame:IsShown()
    float petChatLift() const;                         // UIParent_ManageFramePositions: ChatFrame1 "pet = 35"
    float petPartyShift() const;                       // PSP: the party frames go under PetFrame
    void petKeys(uint32_t live, bool lHeld, bool rHeld);  // updateControls (controls=2): L + D-pad = pet buttons 1..4
    bool petWindowAction(uint32_t act);                // Cross (the left click)
    bool petAltAction(uint32_t alt);                   // Circle (the right click): autocast, the pet frame's menu
    void petWindowPad(platform::psp::PadState& pad);   // Square = the drag (pet buttons, stable slots)
    bool drawPetWindow(UiRenderer& ui, uint8_t w);     // drawWindows
    bool isPetPopup(uint8_t w) const;                  // isPopupWindow: RENAME_PET / the confirmations
    bool petKeepsPopupArt() const;                     // beginPopups: a pet dialog is open
    void drawPetOverlay(UiRenderer& ui);               // after the windows: the pet tooltips, the cursor's icon
    void petMenuAction(uint8_t which);                 // UnitPopup PET: 0 PET_PAPERDOLL, 1 RENAME, 2 ABANDON, 3 DISMISS
    bool petMenuShown(uint8_t which) const;            // UnitPopup_HideButtons for those
    void openPetUnitMenu(float x, float y, uint32_t opener);  // ui_world_party.cpp: UnitPopup_ShowMenu(.., "PET", "pet")
    bool petClearCursor();                             // updateWindows' START: ClearCursor of a pet action / stable pet
    // PlayerTalentFrame: the pet's spec tab (ui_world_trainer.cpp asks these).
    bool talentPetView() const;                        // the "petspec1" tab is selected and has a tree
    const platform::psp::Texture* petTalentBg();       // its tree background (ui.pak tb_hunterpet*)
    void drawTalentSpecTabs(UiRenderer& ui, bool top, const Rect& specTab1, const Rect& petTab);
    // CharacterFrame's pet tab (PetPaperDollFrame, ui_world_pets.cpp).
    bool petTabShown() const;                          // CharacterFrameTab2: HasPetUI
    bool drawPetPaperDoll(UiRenderer& ui, bool focusable);
    // The pet's 3D portrait: a 64 x 64 VRAM target below the inspect model's (initPortrait), drawn when it changes.
    uint32_t petPortraitVram_ = 0;
    platform::psp::Texture petPortraitTex_;
    void renderPetPortrait(const rendering::psp::TerrainCamera& cam);  // after the 3D pass (with the target's)
    // pets (end)
    // class bars (ui_world_classbars.cpp): ShapeshiftBarFrame, PossessBarFrame and
    // BonusActionBarFrame (BonusActionBarFrame.xml / .lua: the stances / forms / presences / auras, the possession's
    // two buttons, the override bar of a possessed unit / a vehicle / a form's preset spells), MultiCastActionBarFrame
    // (the calls, the totem slots, the flyout, Totemic Recall), TotemFrame (the totem timers under PlayerFrame),
    // MainMenuBarVehicleLeaveButton, VehicleMenuBar (its skins Mechanical / Natural, the action buttons, the leave and
    // pitch buttons, the vehicle's health / power) and VehicleSeatIndicator; the player's model swap (a form, a morph:
    // UNIT_FIELD_DISPLAYID that is not the native display is drawn as that creature display), the vehicle seat (the
    // player on the seat's attachment, the seat's ride animation) and the vehicle we drive / the unit we possess (the
    // movement code moves it). Window 0x3A (the flyout), widgets 0x8D00..0x8DFF. State and art behind a pointer.
    static constexpr uint8_t kWinTotemFlyout = 0x3A;  // MultiCastFlyoutFrame (window stack id)
    struct ClassBarUi;
    static void destroyClassBarUi(ClassBarUi* p);
    std::unique_ptr<ClassBarUi, void (*)(ClassBarUi*)> cbar_{nullptr, nullptr};
    ClassBarUi& cbarUi();
    void updateClassBars(float dtSec);                 // update (after the spells): session, art, the swap, the vehicle
    void drawClassBars(UiRenderer& ui);                // drawSpells: the bars over / around MainMenuBar, VehicleMenuBar
    void drawTotemFrame(UiRenderer& ui);               // drawUnitFrames: TotemFrame under PlayerFrame
    void drawClassBarOverlay(UiRenderer& ui);          // after the windows: the bars' tooltips
    bool classBarWindowAction(uint32_t act);           // Cross (the left click) on a bar button
    bool classBarAltAction(uint32_t alt);              // Circle (the right click): DestroyTotem, a slot's flyout
    bool drawClassBarWindow(UiRenderer& ui, uint8_t w);  // drawWindows: MultiCastFlyoutFrame
    void classBarKeys(uint32_t live, bool lHeld, bool rHeld);  // updateControls (controls=2), see the doc
    bool classBarCastSlot(int slot);                   // castFromSlot: actions 121..132 of the override bar
    int classBarBonusBase() const;                     // mainBarBase: GetBonusBarOffset with the override (5)
    bool classBarHidesMainBar() const;                 // VehicleMenuBar replaces MainMenuBar (UnitHasVehicleUI)
    float classBarPetShift() const;                    // PETACTIONBAR_XPOS: the pet bar right of these bars
    float classBarChatLift() const;                    // UIParent "pet = 35" for these bars too
    bool classBarMovement(float dtSec);                // updateMovement: a passenger / no control - the seat carries us
    bool classBarSeatAnimation();                      // updateAnimation: the seat's ride animation
    bool classBarHidesPlayer() const;                  // the player's model is not drawn (a form, a hiding seat)
    glm::vec3 classBarRiderPos();                      // where the player's model is drawn (a seat, else ownRiderPos)
    float classBarRiderYaw() const;
    void appendClassBarUnits(std::vector<rendering::psp::CreatureUnit>& out);  // syncCreatures: swaps, the driven unit
    bool classBarRemoteHidden(uint64_t guid) const;    // drawRemotePlayers: a player drawn as a creature (a form)
    void classBarRemoteSeat(uint64_t guid, glm::vec3& at, float& yaw);  // drawRemotePlayers: a passenger's seat
    bool classBarInteract(uint64_t guid, uint32_t npcFlags);  // interactTarget: CMSG_SPELLCLICK / a player's vehicle
    // The model swap - shared with flying: the own model is drawn as the
    // creature display ownSwapDisplay() while ownModelSwapped(); it mirrors player_'s animation (whatever plays on the
    // character model plays on the swapped one, with the creature's fallbacks); ownSwapPlay forces one.
    bool ownModelSwapped() const;
    uint32_t ownSwapDisplay() const;
    void ownSwapPlay(uint32_t animId, bool loop);
    bool ownSwapHasAnimation(uint32_t animId) const;
    // class bars (end)
    // glyphs, guild bank, channels: GlyphFrame - the talents window's Glyphs
    // tab (the GlyphUI addon over PlayerTalentFrame, ui_world_glyphs.cpp), GuildBankFrame - the guild vault's frame
    // (the GuildBankUI addon, ui_world_guildbank.cpp), ChannelFrame - FriendsFrame's Channels tab (ChannelFrame.xml /
    // .lua, ui_world_channels.cpp), their StaticPopups and the chat's channel commands. Windows 0x31..0x37, widgets
    // 0x8E00..0x8FFF; state and art in GgbUi (src/ui/psp/ui_world_glyphs_guildbank.hpp), made on first use; each
    // frame's art only while it is shown.
    static constexpr uint8_t kWinGuildBank = 0x31, kWinGuildBankPopup = 0x32, kWinGlyphConfirm = 0x33,
                             kWinChannelDaughter = 0x34, kWinChannelDropDown = 0x35, kWinChannelPopup = 0x36,
                             kWinGuildBankTabPopup = 0x37;
    struct GgbUi;
    static void destroyGgbUi(GgbUi* p);
    std::unique_ptr<GgbUi, void (*)(GgbUi*)> ggb_{nullptr, nullptr};
    GgbUi& ggbUi();
    void updateGlyphsGuildBank();                      // drawWindows, every frame: session -> windows, popups, art
    bool drawGgbWindow(UiRenderer& ui, uint8_t w);     // drawWindows
    bool ggbAction(uint32_t act);                      // Cross (the left click)
    bool ggbAltAction(uint32_t alt);                   // Circle (the right click): also a bag item at the vault
    void ggbWindowPad(platform::psp::PadState& pad);   // Square: the vault's pick-up / put-down
    void drawGgbOverlay(UiRenderer& ui);               // after the windows: their GameTooltip, the cursor's item
    bool ggbCloseDropDown();                           // START: ChannelListDropDown first
    bool ggbEscape();                                  // START: the glyph's targeting, a held vault item
    bool ggbKeepsPopupArt() const;                     // beginPopups: a dialog of ours is open
    bool isGgbPopup(uint8_t w) const;                  // isPopupWindow
    // ui_world_glyphs.cpp - PlayerTalentFrame's glyph tab (ui_world_trainer.cpp asks these)
    bool glyphTabShown();                              // GLYPH_TALENT_TAB: SHOW_INSCRIPTION_LEVEL, not the pet's spec
    bool glyphTabSelected();
    void selectGlyphTab(bool on);                      // PlayerTalentTab_OnClick on it / a tree tab
    float drawGlyphTabButton(UiRenderer& ui, float x, float y, float w, float h, bool focusable);
    void drawGlyphFrame(UiRenderer& ui, bool focusable);
    void openGlyphFrame();                             // OpenGlyphFrame (USE_GLYPH)
    void glyphUpdate(float dtSec);
    bool drawGlyphConfirm(UiRenderer& ui);
    bool glyphAction(uint32_t act, bool alt);
    void buildGlyphTip(int socket, const Rect& anchorOnScreen);
    const platform::psp::Texture* glyphRune(uint32_t rune);
    void loadGlyphArt();
    // ui_world_guildbank.cpp
    void guildBankUpdate();
    bool drawGuildBankFrame(UiRenderer& ui);
    bool drawGuildBankPopup(UiRenderer& ui);
    bool drawGuildBankTabPopup(UiRenderer& ui);
    bool guildBankAction(uint32_t act, bool alt);
    bool guildBankBagClick(uint32_t id, bool alt);     // a bag slot clicked while the vault is open
    void guildBankOskTick();
    void loadGuildBankArt();
    void guildBankIconsFor();                          // the tab icons' display ids (text.pak "gb_icons")
    // ui_world_channels.cpp - FriendsFrame's Channels tab (ui_world_social.cpp draws it)
    void drawChannelFrame(UiRenderer& ui, bool focusable);
    void channelsUpdate();
    bool drawChannelDaughter(UiRenderer& ui);
    bool drawChannelDropDown(UiRenderer& ui);
    bool drawChannelPopup(UiRenderer& ui);
    bool channelAction(uint32_t act, bool alt);
    void channelOskTick();
    void loadChannelArt();
    void channelZoneTick();                            // the zone's name for the session's channel names
    void channelDropItems(std::vector<std::pair<std::string, int>>& out);  // the open list's items (text, action)
    // glyphs, guild bank, channels (end)
    // dungeons (ui_world_minimap.cpp): a dungeon's WMO minimap - the player's WMO group
    // (MapUi::dg, every frame of the minimap) and the indoor disc of WMO tiles (the minimap's bearing passed in).
    void updateDungeonGroup(MapUi& m);
    void drawIndoorMinimap(MapUi& m, float sinBearing, float cosBearing, float cx, float cy);
    void openDungeonMap(MapUi& m, const glm::vec3& pos);  // ui_world_map.cpp: SetMapToCurrentZone inside an instance
    // dungeons (end)
    // memory budget (ui_world_memory.cpp): the memory governor - a heap
    // probe twice a second; short of memory it cuts, in this order, the world's particles, other characters' item
    // models (attachcap halved), the big WMOs' doodads, the NPC view distance - each with its log line - and gives
    // them back when there is room (mem_governor); the descriptor budget's per-frame work (pakFdService, the rare
    // paks' idle release) and the tile streamer's top-up permission.
    struct MemGovState;
    static void destroyMemGovState(MemGovState* p);
    std::unique_ptr<MemGovState, void (*)(MemGovState*)> memGov_{nullptr, nullptr};
    void updateMemoryGovernor(float dtSec);     // update(), after the streamer and the big WMOs
    uint32_t memGovernorApply(uint8_t step, bool cut);  // ui_world_memory.cpp: the step's own action; KB freed now
    uint32_t particlesGovernor(bool cut);       // ui_world_particles.cpp: the world's emitters closed / reopened
    uint32_t bigWmoDoodadsGovernor(bool cut);   // ui_world_bigwmo.cpp: the big WMOs' doodads dropped / allowed
    bool particlesGovCut_ = false;              // updateParticles does not reopen the emitters while set
    // PSP (memory diet; an emulator run: after 16 teleports the last
    // place, a village, had no houses / trees - the world arena full of the old places' blobs): world_flush - on a
    // teleport whose new stream window shares no tile with the old one (wowee.cfg tpflush=1, 0 = off) everything
    // streamed for the old place is released (terrain, doodads, big WMOs, game objects' models, NPC models, the
    // world's emitters, parked read-ahead), the world arena and the terrain pool are checked empty and reset, and the
    // new place loads as at world entry. The player model, the interface, fonts, strings, sounds and the indexes stay.
    // Log: world_flush why= dist= ms= freed_kb= arena_used_before= arena_used_after= heap_used_before= heap_used_after=
    // largest_after= (+ world_flush_pool / world_flush_leak). Map changes destroy the whole WorldScreen
    // (LoginScreen::beginTransfer logs the same check as world_flush why=map).
    void worldFlush(const char* why, float distYd);
    uint32_t bigWmoFlush();        // ui_world_bigwmo.cpp: every big WMO closed, their loader stopped (KB freed)
    uint32_t particlesFlush();     // ui_world_particles.cpp: the world's emitters closed (reopened by the next update)
    // PSP (emulator emu_memdiet7): near_starve_flush - the doodads the player can see waited nearstarvems
    // (3000) with nothing in flight for them (a same-window teleport: no world_flush): the NPC models no unit drew for
    // ~3 s and the big WMOs that drew nothing for 5 s from outside are released, at most every 10 s. Log:
    // near_starve_flush freed_kb= npc_kb= wmo_kb= arena_kb=a->b free_kb= largest_kb=.
    void nearStarveFlush();
    uint32_t bigWmoCloseBlank(uint32_t retryMs);  // ui_world_bigwmo.cpp: KB freed
    uint32_t worldFlushes_ = 0;
    // attachcap's crowd queue with the NPCs (remoteCrowdTurn, psp_attach_crowd.hpp).
    std::vector<rendering::psp::CrowdHolder> crowdHolders_;
    std::vector<rendering::psp::CreatureRenderer::AttachHolder> crowdNpcs_;
    float crowdNpcBiasYd_ = -1.0f;              // wowee.cfg crowdnpcbias (read on the first turn)
    uint32_t crowdNpcTurns_ = 0, crowdNpcYields_ = 0, crowdSheds_ = 0;
    // memory budget (end)
};

// map transfer: Map.dbc's Directory of a map in lower case - the <dir> of terrain_<dir>.pak,
// doodads_<dir>.pak, minimap_<dir>.pak (WorldSession::mapInfo: glue.pak "mapdirs", the continents built in);
// empty when the map is unknown.
std::string worldMapDir(uint32_t mapId);

} // namespace wowee::ui::psp
