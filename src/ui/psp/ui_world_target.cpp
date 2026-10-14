// Target selection and the unit frames (player + target).
//
// Target: D-pad right = the next unit/player (upstream CombatHandler::tabTarget:
// 40 yd, nearest first, a 4 s pause restarts the cycle from the nearest, a
// target picked some other way resumes the cycle from its place, the
// server hears CMSG_SET_SELECTION); L + D-pad left clears it.
// candidates as upstream isValidTabTarget - hostile by faction
// (FactionTemplate.dbc/Faction.dbc baked in data/dbc.pak, upstream
// buildFactionHostilityMap) or attacking us. The settings row "Targets"
// (wowee.cfg targetall=1) or a missing dbc.pak = every living unit.
// PSP deviations (wowee.cfg):
//   - units flagged UNIT_FLAG_NOT_SELECTABLE are never candidates;
//   - units in front of the camera (targetcone, half-angle in degrees) come
//     before the rest, nearest first in each group - the PSP has no mouse to
//     click the unit it looks at; targetcone=0 = upstream's all-around order;
//   - targetrange (yd) = upstream kTabRange (40).
// Frames: the client's TargetFrame / PlayerFrame layout (FrameXML TargetFrame.xml /
// PlayerFrame.xml: 232x100, health 119x12 at 41, mana at 52, portrait 64x64)
// scaled 0.6, on the ui.pak art "uf_target" / "uf_player" / "uf_bar" /
// "uf_port_<race>_<sex>" (tools/psp/build_ui_pak.py); without the art, plain
// fills. Health/power/level from the update fields (UNIT_FIELD_HEALTH,
// MAXHEALTH, POWER1+type, MAXPOWER1+type, LEVEL, power type = BYTES_0 byte 3).
// As TargetFrame.lua / PlayerFrame.lua - round portraits (a disc,
// UiRenderer::imageCircle), the elite / rare / rare-elite dragon frames, the
// name plate in UnitSelectionColor (grey when tapped by another), the level in
// GetQuestDifficultyColor or the "??" skull, DeadText, the status text in the
// bars' size, PlayerFrame_UpdateStatus (rest / attack icon, pulsing glow).
#include "ui/psp/ui_world_screen.hpp"

#include "core/coordinates.hpp"
#include "game/protocol_constants.hpp"
#include "game/update_field_table.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_strings.hpp"
#include "platform/psp/psp_prof.hpp"

#include <pspthreadman.h>

#include <pspge.h>
#include <pspgu.h>
#include <pspgum.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

constexpr float kFrameScale = 0.6f;  // 232x100 -> 139x60 on the 480x272 screen

// PowerBarColor (FrameXML UnitFrame.lua), 0xAABBGGRR.
uint32_t powerColor(uint8_t type) {
    switch (type) {
        case 0: return 0xFFFF0000u;  // mana (0, 0, 1)
        case 1: return 0xFF0000FFu;  // rage (1, 0, 0)
        case 2: return 0xFF4080FFu;  // focus (1, .5, .25)
        case 3: return 0xFF00FFFFu;  // energy (1, 1, 0)
        case 6: return 0xFFFFD100u;  // runic power (0, .82, 1)
        default: return 0xFFFF0000u;
    }
}

// GetQuestDifficultyColor (UIParent.lua, QuestDifficultyColors) for a level; the green
// range ends at the client's grey level (GetQuestGreenRange = the server's XP grey level, as the
// earlier quest log).
uint32_t levelDifficultyColor(uint32_t level, uint32_t playerLevel) {
    const int pl = static_cast<int>(playerLevel), l = static_cast<int>(level);
    const int diff = l - pl;
    const int gray = pl <= 5 ? 0 : pl <= 39 ? pl - 5 - pl / 10 : pl <= 59 ? pl - 1 - pl / 5 : pl - 9;
    if (diff >= 5) return 0xFF1A1AFFu;   // impossible (1, .1, .1)
    if (diff >= 3) return 0xFF4080FFu;   // verydifficult (1, .5, .25)
    if (diff >= -2) return 0xFF00D1FFu;  // difficult (1, .82, 0)
    if (l > gray) return 0xFF40BF40u;    // standard (.25, .75, .25)
    return 0xFF808080u;                  // trivial (.5, .5, .5)
}

bool loadArt(pp::Pak& pak, const char* name, pp::Texture& out, uint32_t& bytes) {
    const pp::PakEntry* e = pak.find(name);
    void* blob = e ? pak.load(e) : nullptr;
    if (!blob) return false;
    if (!pp::textureFromBlob(blob, e->size, out, true)) {
        pp::pakFree(blob);
        return false;
    }
    out.name = "uf";
    bytes += e->size;
    return true;
}

}  // namespace

WorldScreen::UnitFrameArt::~UnitFrameArt() {
    pp::textureRelease(target);
    pp::textureRelease(player);
    pp::textureRelease(bar);
    pp::textureRelease(stateCombat);
    for (pp::Texture* t : {&targetElite, &targetRare, &targetRareElite, &nameBg, &skull, &stateRest, &glowRest,
                           &glowCombat, &status, &attackBg})
        pp::textureRelease(*t);
    for (auto& r : portrait)
        for (auto& t : r) pp::textureRelease(t);
}

// wowee.cfg keys + the unit frame art (once, on the first update).
void WorldScreen::initCombat() {
    combatInit_ = true;
    targetRange_ = static_cast<float>(std::clamp(pp::configGetInt("targetrange", 40), 5, 100));
    targetCone_ = static_cast<float>(std::clamp(pp::configGetInt("targetcone", 60), 0, 180));
    combatTextOn_ = pp::configGetInt("combattext", 1) != 0;
    npcNamesOn_ = pp::configGetInt("npcnames", 1) != 0;
    npcNameDist_ = static_cast<float>(std::clamp(pp::configGetInt("npcnamedist", 30), 5, 150));
    unitFramesOn_ = pp::configGetInt("unitframes", 1) != 0;
    autoFace_ = pp::configGetInt("autoface", 1) != 0;
    char path[256];
    pp::fsPath(path, sizeof(path), "data/ui.pak");
    pp::Pak pak;
    int ok = 0, missing = 0, portraits = 0, extras = 0;
    uint32_t bytes = 0;
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    if (pak.open(path)) {
        (loadArt(pak, "uf_target", ufArt_.target, bytes) ? ok : missing)++;
        (loadArt(pak, "uf_player", ufArt_.player, bytes) ? ok : missing)++;
        (loadArt(pak, "uf_bar", ufArt_.bar, bytes) ? ok : missing)++;
        loadArt(pak, "uf_state_combat", ufArt_.stateCombat, bytes);  // controls: optional
        // TargetFrame / PlayerFrame details, all optional (an old ui.pak = the older look).
        const struct {
            const char* name;
            pp::Texture* tex;
        } extra[] = {{"uf_target_elite", &ufArt_.targetElite}, {"uf_target_rare", &ufArt_.targetRare},
                     {"uf_target_rareelite", &ufArt_.targetRareElite}, {"uf_namebg", &ufArt_.nameBg},
                     {"uf_skull", &ufArt_.skull}, {"uf_state_rest", &ufArt_.stateRest},
                     {"uf_glow_rest", &ufArt_.glowRest}, {"uf_glow_combat", &ufArt_.glowCombat},
                     {"uf_status", &ufArt_.status}, {"uf_attackbg", &ufArt_.attackBg}};
        for (const auto& x : extra)
            if (loadArt(pak, x.name, *x.tex, bytes)) ++extras;
        for (int race = 1; race < 12; ++race)
            for (int sex = 0; sex < 2; ++sex) {
                char name[32];
                std::snprintf(name, sizeof(name), "uf_port_%d_%d", race, sex);
                if (loadArt(pak, name, ufArt_.portrait[race][sex], bytes)) ++portraits;
            }
        pak.close();
    } else {
        missing = 3;
    }
    ufArt_.loaded = true;
    PSP_LOGI(pp::tag::Gfx, "uf_art ok=%d missing=%d portraits=%d extra=%d kb=%u ms=%u", ok, missing, portraits, extras,
             static_cast<unsigned>(bytes / 1024u),
             static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u));
    PSP_LOGI(pp::tag::Gfx, "combat_cfg targetrange=%.0f targetcone=%.0f combattext=%d npcnames=%d frames=%d autoface=%d",
             targetRange_, targetCone_, combatTextOn_ ? 1 : 0, npcNamesOn_ ? 1 : 0, unitFramesOn_ ? 1 : 0,
             autoFace_ ? 1 : 0);
    // factions (data/dbc.pak, read once, pak closed) + the keys of
    // this batch's fixes.
    factionsOk_ = session_.loadFactions();
    camM2_ = pp::configGetInt("camm2", 1) != 0;
    camM2Min_ = static_cast<float>(std::clamp(pp::configGetInt("camm2min", 2), 0, 20));
    hudLines_ = pp::configGetInt("hudlines", 0) != 0;  // off by default, Debug -> "Numbers"
    nameColors_ = std::clamp(pp::configGetInt("namecolors", 0), 0, 1);
    sheathAuto_ = pp::configGetInt("sheathauto", 1) != 0;
    sheathDelay_ = static_cast<float>(std::clamp(pp::configGetInt("sheathdelay", 5), 0, 600));
    sheathRemote_ = pp::configGetInt("sheathremote", 1) != 0;
    PSP_LOGI(pp::tag::Gfx, "b3_cfg factions=%d targetall=%d camm2=%d camm2min=%.0f hudlines=%d namecolors=%d sheath=%d/%.0f/%d",
             factionsOk_ ? 1 : 0, settings_.values().targetall, camM2_ ? 1 : 0, camM2Min_, hudLines_ ? 1 : 0,
             nameColors_, sheathAuto_ ? 1 : 0, sheathDelay_, sheathRemote_ ? 1 : 0);
}

// What a frame / name plate shows about a unit (the own player from its
// own update fields, the rest from the entity's).
bool WorldScreen::unitView(uint64_t guid, UnitView& u) const {
    using game::UF;
    const uint16_t fHealth = game::fieldIndex(UF::UNIT_FIELD_HEALTH);
    const uint16_t fMaxHealth = game::fieldIndex(UF::UNIT_FIELD_MAXHEALTH);
    const uint16_t fPower = game::fieldIndex(UF::UNIT_FIELD_POWER1);
    const uint16_t fMaxPower = game::fieldIndex(UF::UNIT_FIELD_MAXPOWER1);
    const uint16_t fLevel = game::fieldIndex(UF::UNIT_FIELD_LEVEL);
    const uint16_t fBytes0 = game::fieldIndex(UF::UNIT_FIELD_BYTES_0);
    if (guid == 0) return false;
    if (guid == session_.playerGuid()) {
        u.player = true;
        u.name = name_;
        const uint32_t b0 = session_.selfField(fBytes0);
        u.race = static_cast<uint8_t>(b0 & 0xFF);
        u.sex = static_cast<uint8_t>((b0 >> 16) & 0xFF);
        u.powerType = static_cast<uint8_t>((b0 >> 24) & 0xFF);
        u.level = session_.selfField(fLevel);
        u.hp = session_.selfField(fHealth);
        u.maxHp = session_.selfField(fMaxHealth);
        if (u.powerType < 7) {
            u.power = session_.selfField(static_cast<uint16_t>(fPower + u.powerType));
            u.maxPower = session_.selfField(static_cast<uint16_t>(fMaxPower + u.powerType));
        }
        if (!session_.selfKnown()) {  // before the first update: SMSG_CHAR_ENUM
            for (const auto& c : session_.characters())
                if (c.name == name_) {
                    u.level = c.level;
                    u.race = static_cast<uint8_t>(c.race);
                    u.sex = c.gender == game::Gender::FEMALE ? 1 : 0;
                }
        }
        u.dead = session_.life() == pp::WorldSession::Life::Dead;
        return true;
    }
    auto e = session_.entities().getEntity(guid);
    if (!e) return false;
    const auto type = e->getType();
    if (type != game::ObjectType::UNIT && type != game::ObjectType::PLAYER) return false;
    u.player = type == game::ObjectType::PLAYER;
    const uint32_t b0 = e->getField(fBytes0);
    u.race = static_cast<uint8_t>(b0 & 0xFF);
    u.sex = static_cast<uint8_t>((b0 >> 16) & 0xFF);
    u.powerType = static_cast<uint8_t>((b0 >> 24) & 0xFF);
    u.level = e->getField(fLevel);
    u.hp = e->getField(fHealth);
    u.maxHp = e->getField(fMaxHealth);
    if (u.powerType < 7) {
        u.power = e->getField(static_cast<uint16_t>(fPower + u.powerType));
        u.maxPower = e->getField(static_cast<uint16_t>(fMaxPower + u.powerType));
    }
    u.dead = game::isUnitCorpseState(u.hp, u.maxHp,
                                     e->getField(game::fieldIndex(UF::UNIT_DYNAMIC_FLAGS)) & game::UNIT_DYNFLAG_DEAD);
    if (u.player) {
        const std::string* n = session_.nameOf(guid);
        u.name = n ? *n : std::string("...");
    } else {
        const auto* info = session_.creatureInfo(e->getField(game::fieldIndex(UF::OBJECT_FIELD_ENTRY)));
        u.name = info ? info->name : std::string("...");
        u.rank = info ? info->rank : 0;
    }
    return true;
}

// Where a unit stands as drawn (server coordinates, feet) and roughly how
// tall it is: the own player and other players from their models, NPCs from
// the entity (the creature renderer keeps no per-unit bounds; 2.2 yd x scale).
bool WorldScreen::unitPosition(uint64_t guid, glm::vec3& pos, float& height) const {
    if (guid == session_.playerGuid()) {
        pos = glm::vec3(playerX_, playerY_, shownZ_);
        height = player_.height() > 0.5f ? player_.height() : 2.0f;
        return true;
    }
    auto r = remotes_.find(guid);
    if (r != remotes_.end() && r->second.model && r->second.hasCache) {
        pos = r->second.pos;
        height = r->second.model->height() > 0.5f ? r->second.model->height() : 2.0f;
        return true;
    }
    auto e = session_.entities().getEntity(guid);
    if (!e) return false;
    pos = core::coords::canonicalToServer(glm::vec3(e->getX(), e->getY(), e->getZ()));
    float scale = 1.0f;
    const uint32_t bits = e->getField(game::fieldIndex(game::UF::OBJECT_FIELD_SCALE_X));
    if (bits) std::memcpy(&scale, &bits, sizeof(scale));
    if (!(scale > 0.01f && scale < 100.0f)) scale = 1.0f;
    height = 2.2f * scale;
    return true;
}

// Same projection as the player name plates (viewProj_ / camPosForHud_ of
// this frame's camera).
bool WorldScreen::projectHud(const glm::vec3& p, float& sx, float& sy) const {
    const glm::vec4 clip = viewProj_ * glm::vec4(p - camPosForHud_, 1.0f);
    if (clip.w < 0.5f) return false;
    sx = (clip.x / clip.w * 0.5f + 0.5f) * pp::kScreenW;
    sy = (1.0f - (clip.y / clip.w * 0.5f + 0.5f)) * pp::kScreenH;
    return sx >= -40.0f && sx <= pp::kScreenW + 40.0f && sy >= -20.0f && sy <= pp::kScreenH + 20.0f;
}

// upstream CombatHandler::tabTarget (see the file comment for the PSP rules).
// step: +1 next / -1 previous; filter: -1 = the menu's "Targets" (targetall),
// 0 = enemies only, 1 = every living unit (controls=2: combat / travel mode).
void WorldScreen::tabTarget(int step, int filter) {
    PROF_SCOPE("target");
    constexpr uint32_t kCycleResetMs = 4000;
    const uint32_t nowMs = pp::logUptimeMs();
    if (nowMs - lastTabMs_ > kCycleResetMs) tabStale_ = true;
    lastTabMs_ = nowMs;
    // upstream: the list goes stale when entities come or go.
    if (session_.entityVersion() != tabVersion_) tabStale_ = true;
    // factions - upstream isValidTabTarget: hostile by faction
    // (FactionTemplate via dbc.pak) or attacking us (isAggressiveTowardPlayer).
    // Menu "Targets" = targetall: 1 = every living unit (the PSP rule before);
    // without dbc.pak the same (log factions_missing).
    const int targetAll = filter >= 0 ? filter : settings_.values().targetall;
    if (targetAll != lastTargetAll_) {
        lastTargetAll_ = targetAll;
        tabStale_ = true;
    }
    const bool hostileOnly = targetAll == 0 && factionsOk_;
    const uint16_t fFlags = game::fieldIndex(game::UF::UNIT_FIELD_FLAGS);
    auto valid = [&](uint64_t guid) {
        UnitView u;
        if (!unitView(guid, u) || u.dead) return false;  // no loot on the PSP: corpses never cycle
        auto e = session_.entities().getEntity(guid);
        if (!e || (e->getField(fFlags) & game::UNIT_FLAG_NOT_SELECTABLE)) return false;
        return !hostileOnly || session_.isHostileUnit(guid) || session_.isHostileAttacker(guid);
    };
    if (tabStale_) {
        tabList_.clear();
        tabIndex_ = -1;
        tabVersion_ = session_.entityVersion();
        struct Cand {
            uint64_t guid;
            float distSq;
            bool inCone;
        };
        std::vector<Cand> cands;
        const float coneCos = std::cos(targetCone_ * 3.14159265f / 180.0f);
        const glm::vec2 fwd(std::cos(camYaw_), std::sin(camYaw_));
        for (const auto& [guid, e] : session_.entities().getEntities()) {
            const auto t = e->getType();
            if (t != game::ObjectType::UNIT && t != game::ObjectType::PLAYER) continue;
            if (guid == session_.playerGuid() || !valid(guid)) continue;
            const glm::vec3 p = core::coords::canonicalToServer(glm::vec3(e->getX(), e->getY(), e->getZ()));
            const glm::vec3 d(p.x - playerX_, p.y - playerY_, p.z - playerZ_);
            const float distSq = glm::dot(d, d);
            if (distSq > targetRange_ * targetRange_) continue;
            const float planar = std::sqrt(d.x * d.x + d.y * d.y);
            const bool inCone = targetCone_ <= 0.0f || targetCone_ >= 180.0f || planar < 1.0f ||
                                (d.x * fwd.x + d.y * fwd.y) / planar >= coneCos;
            cands.push_back({guid, distSq, inCone});
        }
        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
            if (a.inCone != b.inCone) return a.inCone;
            return a.distSq < b.distSq;
        });
        for (const auto& c : cands) tabList_.push_back(c.guid);
        tabStale_ = false;
        if (++targetFilterLogs_ <= 60)  // factions
            PSP_LOGI(pp::tag::Gfx, "target_filter hostile=%d all=%d factions=%d n=%u", hostileOnly ? 1 : 0, targetAll,
                     factionsOk_ ? 1 : 0, static_cast<unsigned>(tabList_.size()));
    }
    if (tabList_.empty()) {
        clearTarget();
        return;
    }
    const uint64_t cur = session_.target();
    if (cur != 0)
        for (size_t i = 0; i < tabList_.size(); ++i)
            if (tabList_[i] == cur) tabIndex_ = static_cast<int>(i);
    int tries = static_cast<int>(tabList_.size());
    while (tries-- > 0) {
        const int n = static_cast<int>(tabList_.size());
        tabIndex_ = tabIndex_ < 0 && step < 0 ? n - 1 : ((tabIndex_ + step) % n + n) % n;
        const uint64_t guid = tabList_[static_cast<size_t>(tabIndex_)];
        if (valid(guid)) {
            session_.setTarget(guid);
            return;
        }
    }
    tabStale_ = true;
    clearTarget();
}

// upstream CombatHandler::clearTarget.
void WorldScreen::clearTarget() {
    session_.setTarget(0);
    tabIndex_ = -1;
    tabStale_ = true;
}

// --- Frames ---------------------------------------------------------------------

void WorldScreen::drawUnitFrame(UiRenderer& ui, const UnitView& u, float x, float y, bool mirrored, bool isSelf) {
    using game::UF;
    const float s = kFrameScale;
    const float fw = 232.0f * s;
    // A rectangle of the 232x100 TargetFrame layout, mirrored for the player
    // frame (PlayerFrame is the same art flipped).
    auto R = [&](float lx, float ly, float lw, float lh) {
        const float rx = mirrored ? 232.0f - lx - lw : lx;
        return Rect{x + rx * s, y + ly * s, lw * s, lh * s};
    };
    auto C = [&](float lx, float ly, float& cx, float& cy) {
        cx = x + (mirrored ? 232.0f - lx : lx) * s;
        cy = y + ly * s;
    };
    // PlayerFrame.xml's own textures, in its own (unmirrored) units.
    auto P = [&](float lx, float ly, float lw, float lh) { return Rect{x + lx * s, y + ly * s, lw * s, lh * s}; };
    const bool targetFrame = !mirrored;
    const uint64_t guid = isSelf ? session_.playerGuid() : session_.target();
    const auto ent = isSelf ? nullptr : session_.entities().getEntity(guid);

    // TargetFrame_CheckFaction. A unit tapped by someone else (not player-controlled,
    // UnitIsTapped and not UnitIsTappedByPlayer / ByAllThreatList): name plate and portrait grey.
    constexpr uint32_t kDynTappedByAllThreat = 0x80u;  // UNIT_DYNFLAG_TAPPED_BY_ALL_THREAT_LIST
    bool tappedByOther = false;
    if (ent && !u.player) {
        const uint32_t dyn = ent->getField(game::fieldIndex(UF::UNIT_DYNAMIC_FLAGS));
        tappedByOther = (dyn & game::UNIT_DYNFLAG_TAPPED) && !(dyn & game::UNIT_DYNFLAG_TAPPED_BY_PLAYER) &&
                        !(dyn & kDynTappedByAllThreat);
    }
    // UnitCanAttack / UnitSelectionColor from the factions (dbc.pak; without it every NPC
    // is neutral). PSP: no PvP flags - another player is attackable when his faction is hostile.
    const bool hostile = !isSelf && (session_.isHostileUnit(guid) || session_.isHostileAttacker(guid));
    const int reaction = isSelf ? 5 : u.player ? (hostile ? 2 : 5) : session_.unitReactionToPlayer(guid);
    const bool canAttack = !isSelf && (hostile || (!u.player && reaction <= 4));
    uint32_t selColor;  // UnitSelectionColor, 0xAABBGGRR
    if (u.dead) selColor = 0xFF808080u;                                    // a corpse: grey
    else if (u.player) selColor = hostile ? 0xFF0000FFu : 0xFFFF0000u;      // enemy red, friend blue (0, 0, 1)
    else if (hostile || reaction <= 2) selColor = 0xFF0000FFu;             // hostile (1, 0, 0)
    else if (reaction == 3) selColor = 0xFF0080FFu;                        // unfriendly (1, .5, 0)
    else if (reaction == 4) selColor = 0xFF00FFFFu;                        // neutral (1, 1, 0)
    else selColor = 0xFF00FF00u;                                           // friendly (0, 1, 0)

    // TargetFrame_CheckClassification: worldboss / elite = UI-TargetingFrame-Elite, rareelite =
    // -Rare-Elite, rare = -Rare (creature rank 1/3, 2, 4); the frame art with the dragon.
    const pp::Texture* artp = mirrored ? &ufArt_.player : &ufArt_.target;
    if (targetFrame && !isSelf && !u.player) {
        const pp::Texture* cls = (u.rank == 1 || u.rank == 3) ? &ufArt_.targetElite
                                 : u.rank == 2                ? &ufArt_.targetRareElite
                                 : u.rank == 4                ? &ufArt_.targetRare
                                                              : nullptr;
        if (cls && cls->pixels) artp = cls;
    }
    const pp::Texture& art = *artp;
    const bool haveArt = art.pixels != nullptr;
    const Font& small = kit_.font(kFontSmall);
    const Font& tiny = kit_.font(kFontTiny);  // TextStatusBarText (10 units = 6 px at 0.6)
    // Background behind name and bars (TargetFrameBackground: black, 0.5, 119x41 at 7,22).
    ui.fill(R(7, 22, 119, 41), 0x80000000u);
    // TargetFrameNameBackground (UI-TargetingFrame-LevelBackground 119x19 at 7,22) in
    // UnitSelectionColor, grey when tapped by another; the player frame has none.
    if (targetFrame) {
        const uint32_t nb = tappedByOther ? 0xFF808080u : selColor;
        if (ufArt_.nameBg.pixels) ui.image(ufArt_.nameBg, R(7, 22, 119, 19), nb);
        else ui.fill(R(7, 22, 119, 19), (nb & 0x00FFFFFFu) | 0x90000000u);
    }
    // Portrait - round (a disc, SetPortraitTexture is circular; the square stuck out of
    // the ring of the art).
    const Rect port = R(126, 12, 64, 64);
    const uint32_t portTint = tappedByOther ? 0xFF808080u : kColWhite;
    const pp::Texture* face = (u.player && u.race < 12 && u.sex < 2) ? &ufArt_.portrait[u.race][u.sex] : nullptr;
    if (const pp::Texture* live = !u.player && !isSelf ? portraitOf(guid) : nullptr) {
        // The NPC's own model (SetPortraitTexture), under the round hole of the art.
        const float in = port.w * 0.08f;
        ui.imageCircle(*live, Rect{port.x + in, port.y + in, port.w - 2 * in, port.h - 2 * in}, portTint);
    } else if (face && face->pixels) {
        ui.imageCircle(*face, port, portTint);
    } else {
        const float in = port.w * 0.1f;  // inside the round hole
        ui.fillCircle(Rect{port.x + in, port.y + in, port.w - 2 * in, port.h - 2 * in}, 0xC0101010u);
    }
    // Bars.
    auto bar = [&](const Rect& r, uint32_t cur, uint32_t max, uint32_t color) {
        ui.fill(r, 0xA0000000u);
        if (max == 0) return;
        const float f = std::clamp(static_cast<float>(cur) / static_cast<float>(max), 0.0f, 1.0f);
        if (f <= 0.0f) return;
        const Rect filled{r.x, r.y, r.w * f, r.h};
        if (ufArt_.bar.pixels) {
            ui.image(ufArt_.bar, filled, SrcRect{0, 0, ufArt_.bar.uw * f, static_cast<float>(ufArt_.bar.uh)}, color);
        } else {
            ui.fill(filled, color);
        }
    };
    const Rect hpBar = R(7, 41, 119, 12);
    const Rect mpBar = R(7, 52, 119, 12);
    bar(hpBar, u.dead ? 0 : u.hp, u.maxHp, 0xFF00FF00u);
    if (u.maxPower > 0) bar(mpBar, u.power, u.maxPower, powerColor(u.powerType));
    // Frame art (or a plain outline).
    if (haveArt) {
        ui.image(art, Rect{x, y, fw, 100.0f * s});
    } else {
        const Rect b = R(5, 20, 123, 46);
        const uint32_t edge = 0xFF808080u;
        ui.fill(Rect{b.x, b.y, b.w, 1}, edge);
        ui.fill(Rect{b.x, b.y + b.h - 1, b.w, 1}, edge);
        ui.fill(Rect{b.x, b.y, 1, b.h}, edge);
        ui.fill(Rect{b.x + b.w - 1, b.y, 1, b.h}, edge);
    }
    // PlayerFrame_UpdateStatus. Resting: UI-Player-Status yellow (1, .88, .25) + rest
    // icon + its glow; auto-attack (PlayerFrame.inCombat): red + crossed swords + glow + attack
    // background; on the hate list: the swords alone. Status and glows pulse (PlayerFrame_OnUpdate:
    // alpha 55..255 over 0.5 s and back), drawn ADD. PSP: the combat mode of controls=2 shows the
    // swords (the control scheme) and wins over resting.
    enum class St : uint8_t { None, Rest, Attack, Swords };
    St st = St::None;
    if (mirrored && isSelf) {
        const bool resting = (session_.selfField(game::fieldIndex(UF::PLAYER_FLAGS)) & 0x20u) != 0;  // PLAYER_FLAGS_RESTING
        const bool modeSwords = controlsScheme_ >= 2 && combatMode_;
        if (resting && !modeSwords) st = St::Rest;
        else if (session_.autoAttacking()) st = St::Attack;
        else if (modeSwords || session_.inCombat()) st = St::Swords;
    }
    float pulse = 1.0f;
    if (st == St::Rest || st == St::Attack) {
        const float t = std::fmod(static_cast<float>(sceKernelGetSystemTimeWide() % 3600000000ull) / 1e6f, 1.0f);
        pulse = t < 0.5f ? (55.0f + t * 400.0f) / 255.0f : (255.0f - (t - 0.5f) * 400.0f) / 255.0f;
        const uint32_t a = static_cast<uint32_t>(std::clamp(pulse, 0.0f, 1.0f) * 255.0f) << 24;
        if (ufArt_.status.pixels)
            ui.image(ufArt_.status, P(35, 8, 190, 66), (st == St::Rest ? 0x0040E0FFu : 0x000000FFu) | a, Blend::Add);
        if (st == St::Attack && ufArt_.attackBg.pixels) ui.image(ufArt_.attackBg, P(37, 50, 32, 32));
    }
    bool stateIcon = false;
    if (st == St::Rest && ufArt_.stateRest.pixels) {
        ui.image(ufArt_.stateRest, P(37, 49, 31, 33));
        if (ufArt_.glowRest.pixels)
            ui.image(ufArt_.glowRest, P(37, 49, 32, 32), 0x00FFFFFFu | (static_cast<uint32_t>(pulse * 255.0f) << 24),
                     Blend::Add);
        stateIcon = true;
    } else if ((st == St::Attack || st == St::Swords) && ufArt_.stateCombat.pixels) {
        ui.image(ufArt_.stateCombat, P(38, 50, 32, 32));
        if (st == St::Attack && ufArt_.glowCombat.pixels)
            ui.image(ufArt_.glowCombat, P(38, 50, 32, 32), 0x00FFFFFFu | (static_cast<uint32_t>(pulse * 255.0f) << 24),
                     Blend::Add);
        stateIcon = true;
    }
    // Texts: name (GameFontNormalSmall, gold), level, the numbers on the bars (the client's status text).
    float cx = 0, cy = 0;
    // Name area of the frame art (TargetFrame's name text box): a long name
    // scrolls inside it instead of running over the frame.
    ui.textMarquee(small, R(16, 25, 100, 12), u.name.c_str(), kColGold);
    char line[48];
    C(179, 66, cx, cy);
    // TargetFrame_CheckLevel - UnitLevel -1 (a world boss, or an enemy 10+ levels above
    // us) = the skull UI-TargetingFrame-Skull; an attackable unit's level in GetQuestDifficultyColor,
    // otherwise gold. The earlier rank "+" is gone (the dragon art shows it).
    const uint32_t playerLevel = session_.selfField(game::fieldIndex(UF::UNIT_FIELD_LEVEL));
    const bool skull = !isSelf && targetFrame &&
                       ((!u.player && u.rank == 3) || (canAttack && playerLevel > 0 && u.level >= playerLevel + 10));
    if (skull) {
        const float sz = 16.0f * s;
        if (ufArt_.skull.pixels) ui.image(ufArt_.skull, Rect{cx - sz * 0.5f, cy - sz * 0.5f, sz, sz});
        else ui.textShadow(small, cx, cy - small.lineHeight() * 0.5f, "??", 0xFF1A1AFFu, Align::Center);
    } else if (u.level > 0 && !stateIcon) {
        std::snprintf(line, sizeof(line), "%u", static_cast<unsigned>(u.level));
        uint32_t col = canAttack ? levelDifficultyColor(u.level, playerLevel) : kColGold;
        // Without the state art (old ui.pak): red while fighting, as before.
        if (isSelf && mirrored && st != St::None && st != St::Rest) col = kColRed;
        ui.textShadow(small, cx, cy - small.lineHeight() * 0.5f, line, col, Align::Center);
    }
    // The bar numbers in TextStatusBarText size (font_tiny) - the small font's 13 px lines
    // overlapped on bars 7 px apart ("60 / 60" over "0 / 1000", audit). A dead target: DeadText
    // (GlobalStrings DEAD) over the health bar instead of the numbers (TargetFrame_CheckDead).
    const bool ghost = isSelf && session_.life() == pp::WorldSession::Life::Ghost;
    // emulator check of 98: the digits (cap height ~0.72 of the pixel size) centred in the 7 px bar
    // by the font's ascent - centring the 8 px font's whole line box still ran "60 / 60" into "0 / 1000".
    const float capH = std::round(static_cast<float>(tiny.pixelSize()) * 0.72f);
    auto numY = [&](const Rect& b) {
        return std::floor(b.y + (b.h - capH) * 0.5f + capH - static_cast<float>(tiny.ascent()) + 0.5f);
    };
    if (u.dead && !isSelf) {
        ui.textShadow(small, hpBar.x + hpBar.w * 0.5f, hpBar.y + (hpBar.h - small.lineHeight()) * 0.5f, CLIENT_STR(DEAD),
                      kColGold, Align::Center);
        return;
    }
    if (u.dead || ghost) {
        std::snprintf(line, sizeof(line), "%s", ghost ? OWN_STR(UNIT_GHOST) : OWN_STR(UNIT_DEAD));
    } else if (u.maxHp > 0) {
        std::snprintf(line, sizeof(line), "%u / %u", static_cast<unsigned>(u.hp), static_cast<unsigned>(u.maxHp));
    } else {
        line[0] = '\0';
    }
    if (line[0])
        ui.textShadow(tiny, hpBar.x + hpBar.w * 0.5f, numY(hpBar), line, kColWhite, Align::Center);
    if (u.maxPower > 0 && !u.dead) {
        // rage and runic power in tenths on the wire - UnitPower shows them / 10 ("0 / 100").
        const uint32_t div = (u.powerType == 1 || u.powerType == 6) ? 10u : 1u;
        std::snprintf(line, sizeof(line), "%u / %u", static_cast<unsigned>(u.power / div),
                      static_cast<unsigned>(u.maxPower / div));
        ui.textShadow(tiny, mpBar.x + mpBar.w * 0.5f, numY(mpBar), line, kColWhite, Align::Center);
    }
}

// Player frame top left, target frame right of it (the client's PlayerFrame /
// TargetFrame placement; layers - the top row, the debug lines go
// under them). Own GE pass "frames".
void WorldScreen::drawUnitFrames(UiRenderer& ui) {
    if (!unitFramesOn_ || settings_.isOpen()) return;
    PROF_GE_PASS("frames");
    PROF_SCOPE("hud.frames");
    ui.beginFrame();
    const float y = 2.0f;  // layers (was 38, under the debug lines)
    UnitView self;
    if (unitView(session_.playerGuid(), self)) drawUnitFrame(ui, self, 2.0f, y, true, true);
    drawPartyFrameExtras(ui, 2.0f, y, true);  // PlayerLeaderIcon / PlayerMasterIcon, the menu stop
    drawRaidFrameExtras(ui, 2.0f, y, true);   // PlayerFrameReadyCheck
    drawPetFrame(ui);  // PetFrame at PlayerFrame TOPLEFT 80,-60 (ui_world_pets.cpp)
    drawTotemFrame(ui);  // TotemFrame under PlayerFrame (ui_world_classbars.cpp)
    UnitView tgt;
    const uint64_t t = session_.target();
    if (t != 0 && t != session_.playerGuid() && unitView(t, tgt))
        drawUnitFrame(ui, tgt, 2.0f + 232.0f * kFrameScale + 4.0f, y, false, false);
    else if (t != 0 && t == session_.playerGuid())
        drawUnitFrame(ui, self, 2.0f + 232.0f * kFrameScale + 4.0f, y, false, true);
    drawPartyFrameExtras(ui, 2.0f + 232.0f * kFrameScale + 4.0f, y, false);  // leader icon, menu stop
    drawRaidFrameExtras(ui, 2.0f + 232.0f * kFrameScale + 4.0f, y, false);   // the target's raid mark
    drawPartyFrames(ui);  // PartyMemberFrame1..4 under the player frame
    drawRaidBgHud(ui);    // the marks over the units, WorldStateAlwaysUpFrame, WorldStateCaptureBar
    ui.endFrame();
}

// --- Selection circle ------------------------------------------------
//
// The client draws a ring on the ground under the selected unit or player -
// upstream GameScreen: radius = the model's bounds x 1.1 clamped 0.8..8 yd,
// dead = grey, nothing under objects. Colour: the reaction (red hostile,
// orange unfriendly, yellow neutral, green friendly) as the client tints it.
// PSP: the model bounds are not at hand here - UNIT_FIELD_BOUNDINGRADIUS x
// 2.2 (the server's collision radius is about half the model's); a flat quad
// drawn right after the terrain with no depth test, so the models drawn after
// it cover it and a slope does not cut it.
void WorldScreen::drawSelectionCircle(const rendering::psp::TerrainCamera& cam) {
    const uint64_t t = session_.target();
    if (t == 0 || t == session_.playerGuid()) return;
    auto e = session_.entities().getEntity(t);
    if (!e || (e->getType() != game::ObjectType::UNIT && e->getType() != game::ObjectType::PLAYER)) return;
    if (!selCircleTried_) {
        selCircleTried_ = true;
        char path[256];
        pp::fsPath(path, sizeof(path), "data/ui.pak");
        pp::Pak pak;
        uint32_t bytes = 0;
        if (pak.open(path)) {
            loadArt(pak, "sel_circle", selCircle_, bytes);
            pak.close();
        }
        PSP_LOGI(pp::tag::Gfx, "sel_circle ok=%d", selCircle_.pixels ? 1 : 0);
    }
    if (!selCircle_.pixels) return;
    glm::vec3 p;
    float h = 0.0f;
    if (!unitPosition(t, p, h)) return;
    if (t == session_.target() && e->getType() == game::ObjectType::UNIT) {
        // Feet on the drawn ground, not the server's z (a creature may hover a little).
        const float fl = floorAt(p.x, p.y, p.z + 1.0f);
        if (fl > -1e29f && std::fabs(fl - p.z) < 2.0f) p.z = fl;
    }
    float bound = 0.0f;
    const uint32_t bits = e->getField(game::fieldIndex(game::UF::UNIT_FIELD_BOUNDINGRADIUS));
    if (bits) std::memcpy(&bound, &bits, sizeof(bound));
    if (!(bound > 0.0f && bound < 20.0f)) bound = 0.5f;
    const float r = std::clamp(bound * 1.4f, 0.6f, 6.0f);  // was too big at 2.2
    const uint32_t col = selectionColor(t);
    struct V {
        float u, v;
        uint32_t c;
        float x, y, z;
    };
    const float cx = p.x - cam.pos.x, cy = p.y - cam.pos.y, cz = p.z - cam.pos.z + 0.05f;
    // 3D draws take normalised UVs (x sceGuTexScale): 0..1 over the content (were texels -
    // clamped to the transparent edge, the circle never showed).
    const float tu = static_cast<float>(selCircle_.uw) / selCircle_.w, tv = static_cast<float>(selCircle_.uh) / selCircle_.h;
    // a 4 x 4 grid, not one quad - a ring up to 12 yd across under a close target crossed the near
    // plane and the GE dropped the whole quad (clipped corners past its 4096-px window, GE Users Manual §10); cells
    // of <= 3 yd are safe at every cfg fov (psp_ge_limits.hpp: 3.22 x near at the narrowest). 4 strips x 10 verts.
    constexpr int kCells = 4;
    V* strips[kCells];
    for (int j = 0; j < kCells; ++j) {
        V* v = strips[j] = static_cast<V*>(sceGuGetMemory((kCells + 1) * 2 * sizeof(V)));
        for (int i = 0; i <= kCells; ++i) {
            const float fu = static_cast<float>(i) / kCells;
            for (int k = 0; k < 2; ++k) {
                const float fv = static_cast<float>(j + k) / kCells;
                v[i * 2 + k] = {tu * fu, tv * fv, col, cx - r + 2.0f * r * fu, cy - r + 2.0f * r * fv, cz};
            }
        }
    }
    const int depth = sceGuGetStatus(GU_DEPTH_TEST), light = sceGuGetStatus(GU_LIGHTING), blend = sceGuGetStatus(GU_BLEND);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDepthMask(GU_TRUE);  // no depth writes
    sceGuDisable(GU_LIGHTING);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    pp::textureBind(&selCircle_);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    for (V* v : strips)
        sceGumDrawArray(GU_TRIANGLE_STRIP, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
                        (kCells + 1) * 2, nullptr, v);
    sceGuDepthMask(GU_FALSE);
    if (depth) sceGuEnable(GU_DEPTH_TEST);
    if (light) sceGuEnable(GU_LIGHTING);
    if (!blend) sceGuDisable(GU_BLEND);
}

// --- Portraits ---------------------------------------------------------
namespace {
constexpr uint32_t kGlowBytes = 128u * 72u * 2u;  // psp_lighting_ge.cpp kGlowStride x kGlowRows, 16 bpp
constexpr uint32_t kPortraitBytes = 64u * 64u * 2u;
constexpr uint32_t kPdModelBytes = 128u * 128u * 2u;  // CharacterModelFrame
constexpr uint32_t kInspModelBytes = 128u * 128u * 2u;  // InspectModelFrame (between the glow and it)
constexpr uint32_t kPetPortraitBytes = 64u * 64u * 2u;  // PetPortrait (below the inspect model's)
}  // namespace

void WorldScreen::initPortrait() {
    if (portraitVram_ || pp::configGetInt("portraits", 1) == 0) return;
    // One block at the top of eDRAM: the glow at its start (glowReserve asks
    // for less and gets the same start), the portrait in its last 8 KB.
    // + the character window's model below it (pdModelVram_).
    // + PetPortrait's 8 KB (the block's start stays the glow's: glowReserve runs after this).
    if (pp::vramReserveTop(kGlowBytes + kPetPortraitBytes + kInspModelBytes + kPdModelBytes + kPortraitBytes) != 0) {  // + inspect
        portraitVram_ = sceGeEdramGetSize() - kPortraitBytes;
        pdModelVram_ = portraitVram_ - kPdModelBytes;
        pdModelTex_ = pp::Texture{};
        pdModelTex_.pixels = static_cast<const uint8_t*>(sceGeEdramGetAddr()) + pdModelVram_;
        pdModelTex_.mip[0] = pdModelTex_.pixels;
        pdModelTex_.w = pdModelTex_.h = pdModelTex_.uw = pdModelTex_.uh = 128;
        pdModelTex_.psm = GU_PSM_5650;
        pdModelTex_.flags = pp::kTexLinear;
        pdModelTex_.name = "pd_model";
        portraitTex_ = pp::Texture{};
        portraitTex_.pixels = static_cast<const uint8_t*>(sceGeEdramGetAddr()) + portraitVram_;
        portraitTex_.mip[0] = portraitTex_.pixels;
        portraitTex_.w = portraitTex_.h = portraitTex_.uw = portraitTex_.uh = 64;
        portraitTex_.psm = GU_PSM_5650;
        portraitTex_.flags = pp::kTexLinear;
        portraitTex_.name = "portrait";
        // InspectFrame's model below the character window's (the glow keeps the block's start).
        inspModelVram_ = pdModelVram_ - kInspModelBytes;
        inspModelTex_ = pdModelTex_;
        inspModelTex_.pixels = static_cast<const uint8_t*>(sceGeEdramGetAddr()) + inspModelVram_;
        inspModelTex_.mip[0] = inspModelTex_.pixels;
        inspModelTex_.name = "insp_model";
        // PetPortrait's 64 x 64 below the inspect model's.
        petPortraitVram_ = inspModelVram_ - kPetPortraitBytes;
        petPortraitTex_ = portraitTex_;
        petPortraitTex_.pixels = static_cast<const uint8_t*>(sceGeEdramGetAddr()) + petPortraitVram_;
        petPortraitTex_.mip[0] = petPortraitTex_.pixels;
        petPortraitTex_.name = "pet_portrait";
    }
    PSP_LOGI(pp::tag::Gfx, "portrait_vram off=%u pd_model=%u", static_cast<unsigned>(portraitVram_),
             static_cast<unsigned>(pdModelVram_));
}

void WorldScreen::renderPortrait(const rendering::psp::TerrainCamera& cam) {
    if (!portraitVram_) return;
    // The target, or the NPC of an open conversation (usually the same unit).
    uint64_t g = session_.target();
    if (windowOpen(kWinGossip)) g = session_.getCurrentGossip().npcGuid;
    else if (windowOpen(kWinQuestDetails)) g = session_.getQuestDetails().npcGuid;
    else if (windowOpen(kWinQuestProgress)) g = session_.getQuestRequestItems().npcGuid;
    else if (windowOpen(kWinQuestReward)) g = session_.getQuestOfferReward().npcGuid;
    auto e = g ? session_.entities().getEntity(g) : nullptr;
    if (!e || e->getType() != game::ObjectType::UNIT || !creaturesOn_) {
        creatures_.setPortraitUnit(0);
        portraitOk_ = false;
        return;
    }
    creatures_.setPortraitUnit(g);  // next frame it stays animated even when culled
    if (creatures_.drawPortrait(g, portraitVram_, cam)) {
        portraitGuid_ = g;
        portraitOk_ = true;
    } else if (g != portraitGuid_) {
        portraitOk_ = false;  // a new unit not drawn yet: no stale face
    }
}

// The selection circle's and the outline's colour - the reaction
// (red hostile, orange unfriendly, yellow neutral, green friendly), a corpse grey.
uint32_t WorldScreen::selectionColor(uint64_t t) const {
    auto e = session_.entities().getEntity(t);
    UnitView u;
    if (unitView(t, u) && u.dead) return 0xC0808080u;
    const bool hostile = session_.isHostileUnit(t) || session_.isHostileAttacker(t);
    const int reaction = e && e->getType() == game::ObjectType::PLAYER ? (hostile ? 1 : 5) : session_.unitReactionToPlayer(t);
    if (hostile || reaction <= 2) return 0xFF2020FFu;  // red
    if (reaction == 3) return 0xFF0080FFu;             // orange
    if (reaction == 4) return 0xFF00FFFFu;             // yellow
    return 0xFF30FF30u;                                // green
}

void WorldScreen::drawTargetOutline(const rendering::psp::TerrainCamera& cam) {
    if (!creaturesOn_ || pp::configGetInt("outline", 1) == 0) return;
    const uint64_t t = session_.target();
    auto e = t ? session_.entities().getEntity(t) : nullptr;
    if (!e || e->getType() != game::ObjectType::UNIT) return;
    const uint32_t col = (selectionColor(t) & 0x00FFFFFFu) | 0xB0000000u;  // a translucent rim
    creatures_.drawOutline(t, col, cam, 0xFFE0B080u);  // the fog colour of the world pass (kSkyABGR)
}

}  // namespace wowee::ui::psp
