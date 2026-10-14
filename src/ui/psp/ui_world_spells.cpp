// Spells on the world screen: the main action bar, the player's and the
// target's cast bars, buffs / debuffs, the spell book window and the cast
// animations. The logic is upstream's (WorldSession mirrors SpellHandler, see
// psp_world_session_spells.cpp); the art is the client's own (ui.pak "ab_*",
// "cast_*", spell icons from data/spells.pak).
//
//   - Action bar: upstream ActionButton / game_screen.cpp keys 1..12 ->
//     castSpell(slot.id, target). PSP controls: L + Square / Triangle /
//     Circle / Cross press the 4 buttons of the current page, L + D-pad down
//     turns the page (3 pages = the 12 buttons of the main bar; L + up talks
//     to the target). The
//     hotkey text of a button shows its PSP key. Cursor mode (SELECT): the
//     buttons are clickable, and the spell book (its window bar button) places
//     spells on them (CMSG_SET_ACTION_BUTTON). Attack (6603) is the auto-attack
//     toggle, as upstream. Cooldowns: the dark clockwise sweep of the
//     Cooldown model (drawn as spans); GCD on every spell button.
//   - Cast bar: CastingBarFrame.lua (yellow while casting, green channel /
//     success + flash + fade, red INTERRUPTED held 1 s), 0.6 scale like the
//     unit frames. The target's cast bar under the target frame.
//   - Auras: BuffFrame (player, top right: buffs, then debuffs with their
//     dispel colour, time left under the icon; in cursor mode Cross on a buff
//     cancels it) and TargetFrame's rows under the target frame.
//   - Cast animations: upstream Application's spellCastAnimCallback
//     (precast / cast loop / finalize by DIRECTED / OMNI / AREA) through
//     CharacterAnimator::startSpellCast for us, a loop on other players and
//     NPCs; instant melee abilities swing (meleeSwingCallback).
#include "ui/psp/ui_world_screen.hpp"

#include "game/spell_classification.hpp"
#include "game/update_field_table.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"
#include "rendering/animation/animation_ids.hpp"

#include <pspctrl.h>
#include <pspkernel.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
namespace anim = wowee::rendering::anim;
using game::ActionBarSlot;
using CastType = game::SpellCastType;

namespace {

constexpr uint32_t kIdBarBook = 0x5C01;
constexpr uint32_t kIdSlot0 = 0x5C10;      // + 0..11 action buttons
constexpr uint32_t kIdAura0 = 0x5C40;      // + 0..31 player auras (cancel)
constexpr uint32_t kIdBook0 = 0x5C80;      // + 0..11 SpellButton1..12 (ui_world_spellbook.cpp)
constexpr uint32_t kIdBookPrev = 0x5CA0, kIdBookNext = 0x5CA1, kIdBookClose = 0x5CA3;  // 0x5CA2 was the PSP "clear button"
constexpr int kBookPer = 12;  // SPELLS_PER_PAGE
constexpr uint32_t kSpellAttack = 6603;

// Face buttons in bar order (left to right) and their hotkey labels.
constexpr uint32_t kFaceKeys[4] = {PSP_CTRL_SQUARE, PSP_CTRL_TRIANGLE, PSP_CTRL_CIRCLE, PSP_CTRL_CROSS};
const char* const kFaceLabel[4] = {"[]", "/\\", "O", "X"};
constexpr uint32_t kFaceColor[4] = {0xFFD080FFu, 0xFF80E080u, 0xFF6060FFu, 0xFFFFB080u};
constexpr uint32_t kFaceMask = PSP_CTRL_SQUARE | PSP_CTRL_TRIANGLE | PSP_CTRL_CIRCLE | PSP_CTRL_CROSS;

// Layout (480x272): 12 buttons bottom centre - in MainMenuBar's slots at 0.5
// (WorldScreen::mmbButton, ui_world_mainmenubar.cpp; was 20 px, 23 apart, y 248).
// ActionButton_UpdateUsable vertex colours (0xAABBGGRR).
constexpr uint32_t kTintNoMana = 0xFFFF8080u;    // (0.5, 0.5, 1.0): the icon and the NormalTexture
constexpr uint32_t kTintUnusable = 0xFF666666u;  // (0.4, 0.4, 0.4): the icon only
// Cast bar (CastingBarFrame 195x13, border 256x64) at 0.6.
constexpr float kCastScale = 0.6f;
constexpr float kCastW = 195.0f * kCastScale, kCastH = 13.0f * kCastScale;
constexpr float kCastX = 240.0f - kCastW * 0.5f, kCastY = 200.0f;

uint32_t abgr(float r, float g, float b, float a) {
    auto c = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return (c(a) << 24) | (c(b) << 16) | (c(g) << 8) | c(r);
}
uint32_t withAlpha(uint32_t col, float a) {
    const uint32_t al = static_cast<uint32_t>(std::clamp(a, 0.0f, 1.0f) * ((col >> 24) & 0xFF) + 0.5f);
    return (col & 0x00FFFFFFu) | (al << 24);
}

bool loadArt(pp::Pak& pak, const char* name, pp::Texture& out, uint32_t& bytes) {
    const pp::PakEntry* e = pak.find(name);
    void* blob = e ? pak.load(e) : nullptr;
    if (!blob) return false;
    if (!pp::textureFromBlob(blob, e->size, out, true)) {
        pp::pakFree(blob);
        return false;
    }
    out.name = "spell";
    bytes += e->size;
    return true;
}

// DebuffTypeColor (FrameXML BuffFrame.lua): none, Magic, Curse, Disease, Poison.
uint32_t dispelColor(uint8_t dispel) {
    switch (dispel) {
        case 1: return abgr(0.20f, 0.60f, 1.00f, 1);
        case 2: return abgr(0.60f, 0.00f, 1.00f, 1);
        case 3: return abgr(0.60f, 0.40f, 0.00f, 1);
        case 4: return abgr(0.00f, 0.60f, 0.00f, 1);
        default: return abgr(0.80f, 0.00f, 0.00f, 1);
    }
}

// BuffFrame's AuraButton_UpdateDuration -> SecondsToTimeAbbrev (UIParent.lua) with
// DAY / HOUR / MINUTE / SECOND_ONELETTER_ABBR: days, hours and minutes rounded up (ceil), seconds down
// (%d of a fraction). Was the minutes and hours rounded to the nearest.
void timeAbbrev(char* out, size_t n, int32_t ms) {
    if (ms < 0) ms = 0;
    std::string t;
    if (ms >= 86400000) t = pp::csf(pp::CS::DAY_ONELETTER_ABBR, static_cast<int>((ms + 86399999LL) / 86400000LL));
    else if (ms >= 3600000) t = pp::csf(pp::CS::HOUR_ONELETTER_ABBR, static_cast<int>((ms + 3599999) / 3600000));
    else if (ms >= 60000) t = pp::csf(pp::CS::MINUTE_ONELETTER_ABBR, static_cast<int>((ms + 59999) / 60000));
    else t = pp::csf(pp::CS::SECOND_ONELETTER_ABBR, static_cast<int>(ms / 1000));
    std::snprintf(out, n, "%s", t.c_str());
}

// BuffFrame.lua: BUFF_WARNING_TIME 31 (flash below), BUFF_DURATION_WARNING_TIME 60 (white below),
// BUFF_FLASH_TIME_ON / OFF 0.75, BUFF_MIN_ALPHA 0.3.
constexpr int32_t kBuffWarningMs = 31000, kBuffDurationWarningMs = 60000;

// BuffFrame_OnUpdate's BuffAlphaValue: one clock for every button - 0.75 s fading out, 0.75 s back in
// (the first update flips the state to 0, so the cycle starts fading), scaled into BUFF_MIN_ALPHA..1.
float buffFlashAlpha(uint32_t nowMs) {
    const float t = static_cast<float>(nowMs % 1500u) / 1000.0f;
    const float v = t < 0.75f ? (0.75f - t) / 0.75f : (t - 0.75f) / 0.75f;
    return v * (1.0f - 0.3f) + 0.3f;
}

}  // namespace

WorldScreen::SpellArt::~SpellArt() {
    for (pp::Texture* t : {&slot, &empty, &hilite, &red, &castBorder, &castBorderSmall, &castSpark, &castFlash, &debuff})
        pp::textureRelease(*t);
}

void WorldScreen::initSpells() {
    spellsInit_ = true;
    spellDb_.open();
    session_.setSpellDb(&spellDb_);
    spellDbLink_.session = &session_;
    // SMSG_INITIAL_SPELLS / SMSG_ACTION_BUTTONS come right after the world
    // login, often before the screen's first update: read their buckets now.
    std::vector<uint32_t> ids(session_.knownSpells().begin(), session_.knownSpells().end());
    for (const auto& s : session_.actionBar())
        if (s.type == ActionBarSlot::SPELL && s.id) ids.push_back(s.id);
    if (!ids.empty()) spellDb_.prefetch(ids);
    char path[256];
    pp::fsPath(path, sizeof(path), "data/ui.pak");
    pp::Pak pak;
    int ok = 0, missing = 0;
    uint32_t bytes = 0;
    if (pak.open(path)) {
        struct { const char* name; pp::Texture* tex; } art[] = {
            {"ab_slot", &spellArt_.slot},          {"ab_empty", &spellArt_.empty},
            {"ab_hl", &spellArt_.hilite},          {"ab_red", &spellArt_.red},
            {"cast_border", &spellArt_.castBorder}, {"cast_border_s", &spellArt_.castBorderSmall},
            {"cast_spark", &spellArt_.castSpark},   {"cast_flash", &spellArt_.castFlash},
            {"debuff_border", &spellArt_.debuff},
        };
        for (auto& a : art) (loadArt(pak, a.name, *a.tex, bytes) ? ok : missing)++;
        uint32_t mmbBytes = 0;
        loadMainMenuBarArt(pak, mmbBytes);  // "mmb_art" (logs mainmenubar_art kb=)
        pak.close();
    } else {
        missing = 9;
    }
    PSP_LOGI(pp::tag::Gfx, "spell_art ok=%d missing=%d kb=%u", ok, missing, static_cast<unsigned>(bytes / 1024u));
}

// The action slots the main bar shows: page 1 (0..11), or the bonus bar
// of the current stance / form (ActionBarFrame.lua: GetBonusBarOffset() > 0
// -> page NUM_ACTIONBAR_PAGES + offset, e.g. Battle Stance = slots 72..83).
int WorldScreen::mainBarBase() const {
    // GetBonusBarOffset as the client reports it (the override bar = 5, actions 121..132).
    if (const int cb = classBarBonusBase(); cb >= 0) return cb;
    // 3.3.5a: the form is UNIT_FIELD_BYTES_2 byte 3 (UNIT_BYTES_2_OFFSET_SHAPESHIFT_FORM);
    // PSP: the field is not in upstream's UF table - 3.3.5a UpdateFields.h
    // OBJECT_END + 0x74 (= UNIT_FIELD_STAT0 84 + 38, see update_fields.json).
    constexpr uint16_t kUnitFieldBytes2 = 122;
    const uint32_t b2 = session_.selfField(kUnitFieldBytes2);
    const uint8_t bonus = spellDb_.bonusBar(static_cast<uint8_t>((b2 >> 24) & 0xFF));
    // ActionButton.lua: page = NUM_ACTIONBAR_PAGES (6) + offset, action = (page - 1) * 12 + button:
    // offset 1 (Battle Stance) = slots 72..83 (the server's 72:Attack 73:Heroic Strike).
    return bonus > 0 && bonus <= 4 ? (5 + bonus) * 12 : 0;
}

// --- Input ------------------------------------------------------------------------

// The pad movement / sitting see: face buttons pressed together with L are
// action buttons, not steps, until they are released (a button already held
// for walking when L went down keeps walking).
const pp::PadState& WorldScreen::spellPad(const pp::PadState& pad) {
    spellPad_ = pad;
    spellPad_.held &= ~lCastMask_;
    spellPad_.pressed &= ~lCastMask_;
    spellPad_.released &= ~lCastMask_;
    return spellPad_;
}

void WorldScreen::castFromSlot(int slot) {
    if (slot < 0 || slot >= pp::WorldSession::kActionBarSlots) return;
    if (classBarCastSlot(slot)) return;  // the override bar's actions (a pet action / a spell)
    const ActionBarSlot& s = session_.actionBar()[slot];
    const int shown = slot - mainBarBase();
    if (shown >= 0 && shown < 12) slotFlash_[shown] = 0.2f;
    if (s.type == ActionBarSlot::EMPTY || s.id == 0) return;
    if (s.type == ActionBarSlot::ITEM) {  // UseAction on an item button (ui_world_item_use.cpp)
        useItemAction(s.id);
        return;
    }
    if (s.type != ActionBarSlot::SPELL) {
        combatError_ = OWN_STR(MACROS_UNSUPPORTED);
        combatErrorUntilMs_ = pp::logUptimeMs() + 2000u;
        return;
    }
    if (s.id == kSpellAttack) {  // upstream: Attack toggles auto-attack
        toggleAutoAttack();
        return;
    }
    // upstream UseAction - a spell that is an item's use spell (upstream's default Hearthstone 8690
    // on button 12) goes as that item's CMSG_USE_ITEM (a CMSG_CAST_SPELL of it the realm ignores).
    if (session_.itemUseSpellCarried(s.id)) {
        if (sitting_) setSitting(false, false);
        session_.useItemForSpell(s.id);
        return;
    }
    if (sitting_) setSitting(false, false);  // the original client stands up to cast
    // PSP (as toggleAutoAttack, wowee.cfg autoface): turn to a unit target
    // for a spell that reaches others - no mouse to aim with.
    const pp::SpellInfo* info = spellDb_.get(s.id);
    const uint64_t t = session_.target();
    const bool selfOnly = info && game::spellclass::isSelfCastRange(info->rec.maxRange);
    glm::vec3 tp;
    float h = 0.0f;
    if (autoFace_ && !selfOnly && t != 0 && t != session_.playerGuid() && unitPosition(t, tp, h)) {
        const float dx = tp.x - playerX_, dy = tp.y - playerY_;
        if (std::fabs(dx) > 0.01f || std::fabs(dy) > 0.01f) {
            facing_ = std::atan2(dy, dx);
            sendMove(game::Opcode::MSG_MOVE_SET_FACING, moving_);
        }
    }
    session_.castSpell(s.id, 0, wantMove_ || moving_);
}

void WorldScreen::updateSpells(const pp::PadState& pad, float dtSec) {
    if (!spellsInit_) initSpells();
    PROF_SCOPE("spells");
    ++spellFrame_;
    session_.updateSpells(dtSec);
    spellDb_.tick(pp::logUptimeMs());
    for (float& f : slotFlash_) f = std::max(0.0f, f - dtSec);

    // L + face / L + D-pad up/down (the game pad: idle in cursor mode).
    lCastMask_ &= pad.held;
    const bool lHeld = (pad.held & PSP_CTRL_LTRIGGER) != 0;
    if (lHeld && session_.life() != pp::WorldSession::Life::Dead) {
        for (int i = 0; i < 4; ++i) {
            if (!(pad.pressed & kFaceKeys[i])) continue;
            lCastMask_ |= kFaceKeys[i];
            castFromSlot(mainBarBase() + barPage_ * 4 + i);
        }
        // L + up is the NPC code's "talk to the target" (ui_world_npc.cpp).
        if (pad.pressed & PSP_CTRL_DOWN) barPage_ = (barPage_ + 1) % 3;
    }

    // Session queues: animations, combat text, errors, cast bar.
    pp::WorldSession::CastAnim a;
    while (session_.takeCastAnim(a)) playCastAnim(a);
    spellFxEvents();  // SPELL_START / GO / PLAY_SPELL_VISUAL -> effects (ui_world_spellfx.cpp)
    pp::WorldSession::SpellText st;
    while (session_.takeSpellText(st)) addCombatText(st.type, st.amount, st.outgoing, st.dst);
    std::string err;
    while (session_.takeSpellError(err)) {
        combatError_ = err;
        combatErrorUntilMs_ = pp::logUptimeMs() + 2000u;
    }

    // CastingBarFrame_OnEvent / OnUpdate.
    uint32_t endSpell = 0;
    const uint8_t ended = session_.takeCastEnd(endSpell);
    fxForwardCastEnd(ended, endSpell);  // framexml - UNIT_SPELLCAST_SUCCEEDED / INTERRUPTED
    if (session_.isCasting()) {
        const bool channel = session_.isChanneling();
        castBarMode_ = channel ? 2 : 1;
        castBarSpell_ = session_.currentCastSpellId();
        castBarValue_ = channel ? (session_.castTimeTotal() > 0.0f ? session_.castTimeRemaining() / session_.castTimeTotal() : 0.0f)
                                : session_.castProgress();
        castBarColor_ = channel ? abgr(0, 1, 0, 1) : abgr(1.0f, 0.7f, 0.0f, 1);
        castBarAlpha_ = 1.0f;
        castBarFlash_ = 0.0f;
    } else if (ended == 1 && (castBarMode_ == 1 || castBarMode_ == 2)) {
        castBarValue_ = castBarMode_ == 2 ? 0.0f : 1.0f;
        castBarMode_ = 3;  // success: full (a channel: drained), green, flash, fade out
        castBarColor_ = abgr(0, 1, 0, 1);
        castBarFlash_ = 0.01f;
    } else if (ended == 2 && (castBarMode_ == 1 || castBarMode_ == 2)) {
        castBarMode_ = 4;  // interrupted: red, INTERRUPTED, held 1 s
        castBarColor_ = abgr(1, 0, 0, 1);
        castBarHold_ = 1.0f;
        castBarValue_ = 1.0f;
    } else if (castBarMode_ == 1 || castBarMode_ == 2) {
        castBarMode_ = 3;  // ended without an event (channel ran out)
        castBarColor_ = abgr(0, 1, 0, 1);
        castBarFlash_ = 0.01f;
    }
    if (castBarMode_ == 3) {
        if (castBarFlash_ > 0.0f && castBarFlash_ < 1.0f) castBarFlash_ = std::min(1.0f, castBarFlash_ + dtSec * 6.0f);
        else castBarAlpha_ -= dtSec * 3.0f;  // CASTING_BAR_ALPHA_STEP 0.05 / frame at 60 fps
        if (castBarAlpha_ <= 0.0f) castBarMode_ = 0;
    } else if (castBarMode_ == 4) {
        if (castBarHold_ > 0.0f) castBarHold_ -= dtSec;
        else castBarAlpha_ -= dtSec * 3.0f;
        if (castBarAlpha_ <= 0.0f) castBarMode_ = 0;
    }
    const uint32_t now = pp::logUptimeMs();
    static uint32_t nextLog = 0;
    if (now >= nextLog && session_.knownSpells().size() > 0) {
        nextLog = now + 30000;
        PSP_LOGI(pp::tag::Net, "spells known=%u db_cached=%u buckets=%u icons=%u page=%d casting=%d",
                 static_cast<unsigned>(session_.knownSpells().size()), static_cast<unsigned>(spellDb_.spellsCached()),
                 static_cast<unsigned>(spellDb_.bucketsRead()), static_cast<unsigned>(spellDb_.iconsLoaded()), barPage_,
                 session_.isCasting() ? 1 : 0);
    }
}

// Upstream Application::spellCastAnimCallback (animation_callback_handler.cpp).
void WorldScreen::playCastAnim(const pp::WorldSession::CastAnim& a) {
    const bool self = a.guid == session_.playerGuid();
    if (self && a.melee) {
        triggerMeleeSwing();
        return;
    }
    const bool directed = a.castType == CastType::DIRECTED;
    const bool area = a.castType == CastType::AREA;
    auto has = [&](uint32_t id) -> bool {
        if (self) return player_.hasAnimation(id);
        auto r = remotes_.find(a.guid);
        if (r != remotes_.end()) return r->second.model && r->second.model->hasAnimation(id);
        return creatures_.hasAnimation(a.guid, id);
    };
    auto pickFirst = [&](std::initializer_list<uint32_t> ids) -> uint32_t {
        for (uint32_t id : ids)
            if (has(id)) return id;
        return 0;
    };
    if (++spellLogs_ <= 200)
        PSP_LOGI(pp::tag::Gfx, "cast_anim guid=%08X start=%d chan=%d type=%u spell=%u",
                 static_cast<unsigned>(a.guid & 0xFFFFFFFFu), a.start ? 1 : 0, a.isChannel ? 1 : 0,
                 static_cast<unsigned>(a.castType), static_cast<unsigned>(a.spellId));
    if (a.start) {
        const uint32_t precast = a.isChannel ? 0 : pickFirst({anim::SPELL_PRECAST});
        uint32_t cast = 0;
        if (a.isChannel) {
            cast = directed ? pickFirst({anim::CHANNEL_CAST_DIRECTED, anim::CHANNEL_CAST_OMNI, anim::READY_SPELL_DIRECTED, anim::SPELL})
                            : pickFirst({anim::CHANNEL_CAST_OMNI, anim::CHANNEL_CAST_DIRECTED, anim::READY_SPELL_OMNI, anim::SPELL});
        } else {
            cast = directed ? pickFirst({anim::READY_SPELL_DIRECTED, anim::READY_SPELL_OMNI, anim::SPELL_CAST_DIRECTED,
                                         anim::SPELL_CAST, anim::SPELL})
                            : pickFirst({anim::READY_SPELL_OMNI, anim::READY_SPELL_DIRECTED, anim::SPELL_CAST_OMNI,
                                         anim::SPELL_CAST, anim::SPELL});
        }
        if (cast == 0) cast = anim::STAND;
        uint32_t finalize = 0;
        if (self && !a.isChannel) {
            if (area) finalize = pickFirst({anim::SPELL_CAST_AREA, anim::SPELL_CAST_OMNI, anim::SPELL_CAST, anim::SPELL});
            else if (directed) finalize = pickFirst({anim::SPELL_CAST_DIRECTED, anim::SPELL_CAST_OMNI, anim::SPELL_CAST, anim::SPELL});
            else finalize = pickFirst({anim::SPELL_CAST_OMNI, anim::SPELL_CAST_AREA, anim::SPELL_CAST, anim::SPELL});
        }
        if (self) {
            if (animator_.getActivity().isEmoteActive()) animator_.cancelEmote();
            animator_.startSpellCast(precast, cast, true, finalize);
            return;
        }
        auto r = remotes_.find(a.guid);
        if (r != remotes_.end()) {
            if (r->second.model && cast != anim::STAND) {
                r->second.model->playAnimation(cast, true);
                r->second.anim = cast;
                remoteCastAnim_[a.guid] = cast;
            }
        } else if (cast != anim::STAND) {
            creatures_.playEmote(a.guid, cast, true);  // PSP: the looping state slot of the NPC animator
            remoteCastAnim_[a.guid] = cast;
        }
        return;
    }
    // Cast / channel ended.
    if (self) {
        animator_.stopSpellCast();
        return;
    }
    auto held = remoteCastAnim_.find(a.guid);
    if (held == remoteCastAnim_.end()) return;
    remoteCastAnim_.erase(held);
    auto r = remotes_.find(a.guid);
    if (r != remotes_.end()) {
        // PSP: upstream leaves a finished non-channel pose to the next
        // movement change; a standing player goes back to Stand here.
        if (r->second.model && !r->second.wasMoving) {
            r->second.model->playAnimation(anim::STAND, true);
            r->second.anim = anim::STAND;
        }
    } else {
        creatures_.playEmote(a.guid, 0, true);
    }
}

// --- Drawing ----------------------------------------------------------------------

void WorldScreen::drawSpells(UiRenderer& ui) {
    if (!spellsInit_ || settings_.isOpen()) return;
    PROF_SCOPE("hud.spells");
    drawModeHud(ui);  // controls: the mode line
    // framexml - FrameXML's BuffFrame / CastingBarFrame / action bar replace these (fxhide*)
    if (!fxHides(kFxHideAuras)) drawAuras(ui);
    if (!fxHides(kFxHideCast)) drawTargetCastBar(ui);
    if (!fxHides(kFxHideCast)) drawCastBar(ui);
    // VehicleMenuBar / the override bar (BonusActionBarFrame) replace the main bar's buttons
    if (!fxHides(kFxHideBar) && !classBarHidesMainBar()) drawActionBar(ui);
    drawPetActionBar(ui);  // PetActionBarFrame over MainMenuBar (ui_world_pets.cpp)
    drawClassBars(ui);  // Shapeshift / Possess / MultiCast / Bonus / VehicleMenuBar (ui_world_classbars.cpp)
    if (sbArt_.tried && !windowOpen(kWinSpellbook)) sbArt_.release();  // ~56 KB while the book is shut
}

// The Cooldown model's sweep: dark, clockwise from 12 o'clock over the part
// still to run. Drawn as horizontal spans (UiRenderer has sprites only); the
// per-pixel angles of a button size are computed once (atan2 per pixel per
// frame would cost ~5 ms on a GCD over the whole bar).
void WorldScreen::drawCooldown(UiRenderer& ui, const Rect& r, float remaining, float total) {
    if (remaining <= 0.0f || total <= 0.0f) return;
    const float frac = std::clamp(remaining / total, 0.0f, 1.0f);
    const uint32_t col = 0xA0000000u;
    if (frac >= 0.999f) {
        ui.fill(r, col);
        return;
    }
    const int w = static_cast<int>(r.w), h = static_cast<int>(r.h);
    static std::unordered_map<int, std::vector<float>> tables;
    std::vector<float>& ang = tables[w * 256 + h];
    if (ang.empty()) {
        ang.resize(static_cast<size_t>(w * h));
        for (int i = 0; i < h; ++i)
            for (int j = 0; j < w; ++j) {
                float a = std::atan2((j + 0.5f) - w * 0.5f, h * 0.5f - (i + 0.5f));  // 0 = up, clockwise
                if (a < 0.0f) a += 6.2831853f;
                ang[static_cast<size_t>(i * w + j)] = a;
            }
    }
    const float end = 6.2831853f * (1.0f - frac);  // the elapsed wedge stays clear
    for (int i = 0; i < h; ++i) {
        const float* row = &ang[static_cast<size_t>(i * w)];
        int runStart = -1;
        for (int j = 0; j <= w; ++j) {
            const bool dark = j < w && row[j] >= end;
            if (dark && runStart < 0) runStart = j;
            if (!dark && runStart >= 0) {
                ui.fill(Rect{r.x + runStart, r.y + i, static_cast<float>(j - runStart), 1.0f}, col);
                runStart = -1;
            }
        }
    }
}

void WorldScreen::drawActionBar(UiRenderer& ui) {
    const auto& bar = session_.actionBar();
    const int base = mainBarBase();
    const float gcd = session_.gcdRemaining(), gcdTotal = session_.gcdTotal();
    const uint64_t target = session_.target();
    glm::vec3 tp;
    float th = 0.0f;
    const bool haveTarget = target != 0 && target != session_.playerGuid() && unitPosition(target, tp, th);
    const float dist = haveTarget ? std::sqrt((tp.x - playerX_) * (tp.x - playerX_) + (tp.y - playerY_) * (tp.y - playerY_) +
                                              (tp.z - playerZ_) * (tp.z - playerZ_))
                                  : 0.0f;
    const Font& small = kit_.font(kFontSmall);
    const bool clickable = uiMode_ && !settings_.isOpen();
    tipFrame(clickable);  // the button under the cursor is set again below
    updateUseCtx();  // IsUsableAction's player / target state
    for (int i = 0; i < 12; ++i) {
        const Rect r = mmbButton(i);
        const float fs = r.w * 66.0f / 36.0f;  // ActionButton: NormalTexture 66 around a 36 icon
        const Rect frame{r.x + (r.w - fs) * 0.5f, r.y + (r.h - fs) * 0.5f, fs, fs};
        const ActionBarSlot& s = bar[base + i];
        const bool focused = clickable && winFocus_.addStop(kIdSlot0 + i, r);
        if (focused && !s.isEmpty()) tipActionSlot_ = static_cast<uint32_t>(i) + 1;  // ActionButton_SetTooltip
        if (s.isEmpty()) {
            // an empty main bar button is hidden over MainMenuBar's own slot art, shown
            // (UI-Quickslot) while a spell is on the cursor (ACTIONBAR_SHOWGRID); without the art as before.
            if (!mmbArt_.tex.pixels || bookPick_ != 0) {
                if (spellArt_.empty.pixels) ui.image(spellArt_.empty, frame, 0xB0FFFFFFu);
                else ui.fill(r, 0x60000000u);
            }
        } else {
            // ActionButton_UpdateUsable - blue without the power, dark grey not usable.
            const uint8_t use = actionUsable(s);
            const uint32_t iconTint = use == 1 ? kTintNoMana : use == 2 ? kTintUnusable : 0xFFFFFFFFu;
            const pp::Texture* icon = s.type == ActionBarSlot::SPELL ? spellDb_.icon(s.id, spellFrame_)
                                    : s.type == ActionBarSlot::ITEM  ? itemActionIcon(s.id)
                                                                     : nullptr;
            if (icon) ui.image(*icon, r, iconTint);
            else ui.fill(r, 0xFF303030u);
            if (s.type == ActionBarSlot::SPELL) {
                float rem = s.cooldownRemaining, tot = s.cooldownTotal;
                const float cd = session_.spellCooldown(s.id);
                if (cd > rem) {
                    rem = cd;
                    tot = session_.spellCooldownTotal(s.id);
                }
                if (gcd > rem && s.id != kSpellAttack) {
                    rem = gcd;
                    tot = gcdTotal;
                }
                drawCooldown(ui, r, rem, tot);
            } else if (s.type == ActionBarSlot::ITEM) {
                drawItemActionCooldown(ui, r, s.id);  // GetActionCooldown of an item
            }
            if (spellArt_.slot.pixels) ui.image(spellArt_.slot, frame, use == 1 ? kTintNoMana : 0xFFFFFFFFu);
            if (s.type == ActionBarSlot::ITEM) drawItemActionOverlay(ui, r, s.id);  // count, worn border
            // Auto-attack on: the Attack button flashes red (ActionButton_UpdateFlash).
            if (s.id == kSpellAttack && session_.autoAttacking() && spellArt_.red.pixels &&
                ((pp::logUptimeMs() / 400u) & 1u) == 0)
                ui.image(spellArt_.red, frame, 0xFFFFFFFFu, Blend::Add);
            // The spell being cast is "checked".
            if (s.type == ActionBarSlot::SPELL && session_.isCasting() && session_.currentCastSpellId() == s.id &&
                spellArt_.hilite.pixels)
                ui.image(spellArt_.hilite, r, 0xFFFFFFFFu, Blend::Add);
        }
        if ((slotFlash_[i] > 0.0f || focused) && spellArt_.hilite.pixels)
            ui.image(spellArt_.hilite, r, focused ? 0xFFFFFFFFu : 0xC0FFFFFFu, Blend::Add);
        // Hotkey text: the PSP key on the current page; red out of range
        // (ActionButton_OnUpdate: IsActionInRange == 0).
        const int page = i / 4, key = i % 4;
        // Which PSP keys press this button now: controls=2 by the mode (combat:
        // 1-4 face, 5-8 L + face, 9-12 R + face; travel: 1-4 L + face), else
        // L + face on the current page.
        int modifier = -1;  // -1 no glyph, 0 face alone, 1 L + face, 2 R + face
        if (controlsScheme_ >= 2) modifier = combatMode_ ? page : (page == 0 ? 1 : -1);
        else if (page == barPage_) modifier = 1;
        if (modifier >= 0 && !clickable) {
            uint32_t col = kFaceColor[key];
            if (haveTarget && s.type == ActionBarSlot::SPELL && s.id != kSpellAttack) {
                if (const pp::SpellInfo* info = spellDb_.get(s.id)) {
                    const float mx = info->rec.maxRange;
                    if (mx > 0.0f && !game::spellclass::withinSpellRange(dist, info->rec.minRange, mx, 1.5f, 1.5f))
                        col = kColRed;
                }
            }
            // Kenney glyph (ui.pak pad_*), tinted red out of range; text fallback.
            constexpr PadGlyph kFaceGlyph[4] = {kPadSquare, kPadTriangle, kPadCircle, kPadCross};
            const Rect g{r.x + r.w - 7.0f, r.y - 4.0f, 10.0f, 10.0f};
            if (!kit_.glyph(ui, kFaceGlyph[key], g, col == kColRed ? 0xFF6060FFu : 0xFFFFFFFFu))
                ui.textShadow(small, r.x + r.w, r.y - 3, kFaceLabel[key], col, Align::Right);
            if (modifier > 0) kit_.glyph(ui, modifier == 1 ? kPadL : kPadR, Rect{g.x - 7.0f, g.y + 1.0f, 8.0f, 8.0f});
        }
    }
    // Page number (MainMenuBar's ActionBarPageNumber).
    if (controlsScheme_ < 2) {  // controls=2 has no pages: all 12 buttons have keys
        char pg[16];
        std::snprintf(pg, sizeof(pg), "%d", barPage_ + 1);
        const Rect b0 = mmbButton(0);
        ui.textShadow(small, b0.x - 8, b0.y + 3, pg, kColGold, Align::Center);
    }
    // No hint line - the spell on the cursor shows it (drawSpellCursor), as the original.
    drawMainMenuBarOverlay(ui);  // ExhaustionTick, the XP text, the XP bar's cursor stops
}

void WorldScreen::drawCastBar(UiRenderer& ui) {
    if (castBarMode_ == 0) return;
    const float a = std::clamp(castBarAlpha_, 0.0f, 1.0f);
    const Rect bar{kCastX, kCastY, kCastW, kCastH};
    ui.fill(bar, withAlpha(0xB0000000u, a));
    const float v = std::clamp(castBarValue_, 0.0f, 1.0f);
    const Rect fill{bar.x, bar.y, bar.w * v, bar.h};
    if (ufArt_.bar.pixels) ui.image(ufArt_.bar, fill, withAlpha(castBarColor_, a));
    else ui.fill(fill, withAlpha(castBarColor_, a));
    const Rect border{240.0f - 128.0f * kCastScale, bar.y - 28.0f * kCastScale + 1.0f, 256.0f * kCastScale, 64.0f * kCastScale};
    if (spellArt_.castBorder.pixels) ui.image(spellArt_.castBorder, border, withAlpha(0xFFFFFFFFu, a));
    if ((castBarMode_ == 1 || castBarMode_ == 2) && spellArt_.castSpark.pixels) {
        const float sx = bar.x + bar.w * v;
        ui.image(spellArt_.castSpark, Rect{sx - 16.0f * kCastScale, bar.y + bar.h * 0.5f - 16.0f * kCastScale,
                                           32.0f * kCastScale, 32.0f * kCastScale},
                 0xFFFFFFFFu, Blend::Add);
    }
    if (castBarMode_ == 3 && castBarFlash_ > 0.0f && spellArt_.castFlash.pixels)
        ui.image(spellArt_.castFlash, border, withAlpha(0xFFFFFFFFu, castBarFlash_ * a), Blend::Add);
    const char* text = castBarMode_ == 4 ? CLIENT_STR(INTERRUPTED) : nullptr;
    const std::string& name = spellDb_.name(castBarSpell_);
    if (!text) text = name.c_str();
    ui.textMarquee(kit_.font(kFontSmall), Rect{bar.x + 2, bar.y - 3, bar.w - 4, bar.h + 4}, text,
                   withAlpha(kColWhite, a));
}

// TargetFrameSpellBar under the target frame.
void WorldScreen::drawTargetCastBar(UiRenderer& ui) {
    const uint64_t t = session_.target();
    if (t == 0 || t == session_.playerGuid() || !unitFramesOn_) return;
    const game::UnitCastState* c = session_.unitCast(t);
    if (!c || c->timeTotal <= 0.0f) return;
    const float x = 2.0f + 139.0f + 4.0f + 24.0f, y = 100.0f, w = 90.0f, h = 7.0f;
    const float v = c->isChannel ? c->timeRemaining / c->timeTotal : 1.0f - c->timeRemaining / c->timeTotal;
    const Rect bar{x, y, w, h};
    ui.fill(bar, 0xB0000000u);
    const uint32_t col = c->isChannel ? abgr(0, 1, 0, 1) : abgr(1.0f, 0.7f, 0.0f, 1);
    const Rect fill{x, y, w * std::clamp(v, 0.0f, 1.0f), h};
    if (ufArt_.bar.pixels) ui.image(ufArt_.bar, fill, col);
    else ui.fill(fill, col);
    if (spellArt_.castBorderSmall.pixels)
        ui.image(spellArt_.castBorderSmall, Rect{x + w * 0.5f - 64.0f, y - 12.0f, 128.0f, 32.0f});
    ui.textMarquee(kit_.font(kFontSmall), Rect{x, y + h, w, 11}, spellDb_.name(c->spellId).c_str(), kColWhite);
}

void WorldScreen::drawAuras(UiRenderer& ui) {
    const uint64_t now = pp::WorldSession::auraNowMs();
    const Font& small = kit_.font(kFontSmall);
    const bool clickable = uiMode_ && !settings_.isOpen();
    // Player: BuffFrame, top right, right to left; buffs then debuffs.
    const auto& mine = session_.playerAuras();
    constexpr float kSz = 17.0f, kGap = 3.0f;
    // AuraButton_OnUpdate - a button with less than BUFF_WARNING_TIME left
    // flashes (SetAlpha(BuffAlphaValue): icon, border, count and timer); the timer is
    // SecondsToTimeAbbrev, HIGHLIGHT_FONT_COLOR (white) under BUFF_DURATION_WARNING_TIME, else
    // NORMAL_FONT_COLOR (gold). PSP: the timer in font_tiny (FRIZQT 8 px) - "45 m" (MINUTE_ONELETTER_ABBR) in the 10 px font is
    // wider than the 20 px button pitch and ran into the next timer.
    const float flash = buffFlashAlpha(pp::logUptimeMs());
    const Font& timerFont = kit_.font(kFontTiny);
    for (int pass = 0; pass < 2; ++pass) {
        float x = pp::kScreenW - 4.0f - kSz - minimapReserveW(), y = 40.0f + pass * (kSz + 14.0f);  // map (left of the minimap)
        int n = 0;
        for (size_t i = 0; i < mine.size() && n < 12; ++i) {
            const game::AuraSlot& au = mine[i];
            if (au.isEmpty()) continue;
            const bool debuff = (au.flags & 0x80) != 0;
            if (debuff != (pass == 1)) continue;
            const Rect r{x, y, kSz, kSz};
            const int32_t rem = au.getRemainingMs(now);
            // AuraButton_Update: "duration > 0 and expirationTime" - else no OnUpdate (no timer, no flash).
            const bool timed = rem >= 0 && (au.maxDurationMs > 0 || au.durationMs > 0);
            const float a = timed && rem < kBuffWarningMs ? flash : 1.0f;
            const uint32_t white = withAlpha(0xFFFFFFFFu, a);
            if (const pp::Texture* ic = spellDb_.icon(au.spellId, spellFrame_)) ui.image(*ic, r, white);
            else if (const pp::Texture* q = itemIcons().fixed(pp::ItemIconDb::kQuestionMark, spellFrame_))
                ui.image(*q, r, white);  // the client's INV_Misc_QuestionMark, not a grey square
            else ui.fill(r, withAlpha(0xFF404040u, a));
            if (debuff && spellArt_.debuff.pixels) {
                const pp::SpellInfo* info = spellDb_.get(au.spellId);
                ui.image(spellArt_.debuff, Rect{x - 1, y - 1, kSz + 2, kSz + 2},
                         withAlpha(dispelColor(info ? info->rec.dispel : 0), a));
            }
            if (clickable && !debuff && i < 32 && winFocus_.addStop(kIdAura0 + static_cast<uint32_t>(i), r) &&
                spellArt_.hilite.pixels)
                ui.image(spellArt_.hilite, r, 0xFFFFFFFFu, Blend::Add);
            if (au.charges > 1) {
                char c[8];
                std::snprintf(c, sizeof(c), "%u", static_cast<unsigned>(au.charges));
                ui.textShadow(small, x + kSz, y + kSz - 10, c, withAlpha(kColWhite, a), Align::Right);
            }
            if (timed) {
                char tbuf[16];
                timeAbbrev(tbuf, sizeof(tbuf), rem);
                ui.textShadow(timerFont, x + kSz * 0.5f, y + kSz + 1, tbuf,
                              withAlpha(rem < kBuffDurationWarningMs ? kColWhite : kColGold, a), Align::Center);
            }
            x -= kSz + kGap;
            ++n;
        }
    }
    // Target: TargetFrame buff / debuff rows under the frame.
    const uint64_t t = session_.target();
    if (t == 0 || !unitFramesOn_) return;
    const std::vector<game::AuraSlot>* list = session_.unitAuras(t);
    if (!list) return;
    constexpr float kT = 13.0f;
    const float x0 = 2.0f + 139.0f + 4.0f + 6.0f;
    float y = session_.unitCast(t) ? 122.0f : 99.0f;
    for (int pass = 0; pass < 2; ++pass) {
        float x = x0;
        int n = 0;
        for (const auto& au : *list) {
            if (au.isEmpty()) continue;
            const bool debuff = (au.flags & 0x80) != 0;
            if (debuff != (pass == 1) || n >= 9) continue;
            const Rect r{x, y, kT, kT};
            if (const pp::Texture* ic = spellDb_.icon(au.spellId, spellFrame_)) ui.image(*ic, r);
            else if (const pp::Texture* q = itemIcons().fixed(pp::ItemIconDb::kQuestionMark, spellFrame_))
                ui.image(*q, r);  // the client's INV_Misc_QuestionMark, not a grey square
            else ui.fill(r, 0xFF404040u);
            if (debuff && spellArt_.debuff.pixels) {
                const pp::SpellInfo* info = spellDb_.get(au.spellId);
                ui.image(spellArt_.debuff, Rect{x - 1, y - 1, kT + 2, kT + 2}, dispelColor(info ? info->rec.dispel : 0));
            }
            x += kT + 2.0f;
            ++n;
        }
        if (n) y += kT + 3.0f;
    }
}

// --- Spell book (window stack) ------------------------------------------------------

// Known spells the book lists (upstream SpellBookFrame via getSpellBookTabs):
// not passive, not hidden, highest known rank per name; by skill line, name.
void WorldScreen::rebuildSpellbook() {
    bookVersion_ = session_.knownSpellsVersion();
    bookSpells_.clear();
    std::unordered_map<std::string, std::pair<uint32_t, int>> best;  // name -> spell, rank
    // spell book tabs - the shown tab's spells (upstream getSpellBookTabs).
    const auto& tabs = session_.getSpellBookTabs();
    if (bookTab_ >= static_cast<int>(tabs.size())) bookTab_ = 0;
    std::unordered_set<uint32_t> tabSpells;
    if (!tabs.empty()) tabSpells.insert(tabs[bookTab_].spellIds.begin(), tabs[bookTab_].spellIds.end());
    for (uint32_t id : session_.knownSpells()) {
        if (!tabs.empty() && tabSpells.count(id) == 0) continue;
        const pp::SpellInfo* s = spellDb_.get(id);
        if (!s || s->name.empty() || s->passive() || s->hiddenClientSide()) continue;
        const int rank = game::spellclass::rankValue(s->rank);
        auto it = best.find(s->name);
        if (it == best.end() || rank > it->second.second) best[s->name] = {id, rank};
    }
    for (const auto& [name, v] : best) bookSpells_.push_back(v.first);
    std::sort(bookSpells_.begin(), bookSpells_.end(), [&](uint32_t a, uint32_t b) {
        const pp::SpellInfo* sa = spellDb_.get(a);
        const pp::SpellInfo* sb = spellDb_.get(b);
        if (sa->rec.skillLine != sb->rec.skillLine) return sa->rec.skillLine < sb->rec.skillLine;
        return sa->name < sb->name;
    });
    bookPage_ = std::min(bookPage_, std::max(0, (static_cast<int>(bookSpells_.size()) - 1) / kBookPer));
    PSP_LOGI(pp::tag::Gfx, "spellbook n=%u known=%u", static_cast<unsigned>(bookSpells_.size()),
             static_cast<unsigned>(session_.knownSpells().size()));
}

bool WorldScreen::spellWindowAction(uint32_t act) {
    if (act == kIdBarBook) {
        if (windowOpen(kWinSpellbook)) closeWindow(kWinSpellbook);
        else openWindow(kWinSpellbook);
        return true;
    }
    if (act >= kIdSlot0 && act < kIdSlot0 + 12) {
        const int slot = mainBarBase() + static_cast<int>(act - kIdSlot0);
        if (placeHeldItemOnAction(slot)) return true;  // an item held with Square -> PlaceAction
        if (bookPick_ != 0) spellSlotPickPlace(slot, false);  // PlaceAction (the spell on the cursor)
        else castFromSlot(slot);  // a click = the key (upstream ActionButton OnClick)
        return true;
    }
    if (act == kIdXpBar || act == kIdXpTick) return true;  // MainMenuExpBar has no OnClick
    if (act >= kIdAura0 && act < kIdAura0 + 32) {  // upstream: right-click cancels a buff
        const auto& mine = session_.playerAuras();
        const size_t i = act - kIdAura0;
        if (i < mine.size() && !mine[i].isEmpty() && !(mine[i].flags & 0x80)) session_.cancelAura(mine[i].spellId);
        return true;
    }
    if (act >= kIdBook0 && act < kIdBook0 + kBookPer) {
        // SpellButton_OnClick - CastSpell (Square picks it up: spellWindowPad).
        const int idx = bookPage_ * kBookPer + static_cast<int>(act - kIdBook0);
        if (idx < static_cast<int>(bookSpells_.size())) {
            const uint32_t id = bookSpells_[idx];
            if (id == kSpellAttack) toggleAutoAttack();
            else if (!session_.useItemForSpell(id)) session_.castSpell(id, 0, wantMove_ || moving_);  // 8690
        }
        return true;
    }
    if (act == kIdBookPrev) {
        bookPage_ = std::max(0, bookPage_ - 1);
        return true;
    }
    if (act == kIdBookNext) {
        ++bookPage_;
        return true;
    }
    if (act == kIdBookClose) {
        closeWindow(kWinSpellbook);
        return true;
    }
    return false;
}

// An action button with a spell on the cursor - PlaceAction (the button's spell, if any, comes
// onto the cursor: the original swaps); without one and `pick` (Square) - PickupAction (the button empties,
// its spell on the cursor). PSP: only spells go on the cursor; another action type on the button is
// replaced by the placed spell.
void WorldScreen::spellSlotPickPlace(int slot, bool pick) {
    const auto& bar = session_.actionBar();
    if (slot < 0 || slot >= static_cast<int>(bar.size())) return;
    const ActionBarSlot old = bar[static_cast<size_t>(slot)];
    // an item button goes on the cursor too (bookPickItem_: bookPick_ is the item entry).
    const bool oldItem = old.type == ActionBarSlot::ITEM;
    const uint32_t oldAction = old.type == ActionBarSlot::SPELL || oldItem ? old.id : 0;
    if (bookPick_ != 0) {
        session_.setActionBarSlot(slot, bookPickItem_ ? ActionBarSlot::ITEM : ActionBarSlot::SPELL, bookPick_);
        bookPick_ = oldAction;
        bookPickItem_ = oldItem;
    } else if (pick && oldAction != 0) {
        session_.setActionBarSlot(slot, ActionBarSlot::EMPTY, 0);
        bookPick_ = oldAction;
        bookPickItem_ = oldItem;
    }
}

}  // namespace wowee::ui::psp
