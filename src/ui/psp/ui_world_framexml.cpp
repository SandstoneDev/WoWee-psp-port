// FrameXML in the world - option (b) of the verdict: the Lua API
// (fx_runtime) answers from the PSP's WorldSession through this adapter, not
// from upstream's GameHandler. Field names follow GameHandler / game::Unit
// (the port's own unit frames' UnitView is the same read of the update fields), so what the
// data models of tasks C/D/E add (getBagItem, getGossipOptions, getLootItems,
// getMoney...) becomes one more adapter method each.
//
// wowee.cfg framexml=1 turns it on (default 0: the current HUD stays until the
// hardware says yes); data/framexml.pak missing -> logged, the HUD stays.
// luabench=1 runs the Lua micro benchmark once at world entry either way: hardware numbers for the verdict
//. Default 0 - the numbers are in the port's architecture notes, and the
// bench cost every world entry a Lua state (~180 KB) and ~220 ms right when the heap is tightest.
#include "ui/psp/ui_world_framexml_impl.hpp"  // the adapter struct, shared with ui_world_framexml_game.cpp

#include "game/update_field_table.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_prof.hpp"
#include "ui/psp/ui_framexml.hpp"

#include <pspthreadman.h>

#include <cstdio>
#include <cstring>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

// One read of the session per frame; UnitHealth & co. copy from here.
void WorldScreen::FrameXml::fill(uint64_t guid, fx::FxUnit& u) {
    u = fx::FxUnit{};
    UnitView v;
    if (!guid || !ws.unitView(guid, v)) return;
    u.exists = true;
    u.guid = guid;
    std::snprintf(u.name, sizeof(u.name), "%s", v.name.c_str());
    u.level = v.level;
    u.health = v.hp;
    u.maxHealth = v.maxHp;
    u.powerType = static_cast<uint8_t>(v.powerType & 7);
    u.power[u.powerType] = v.power;
    u.maxPower[u.powerType] = v.maxPower;
    u.dead = v.dead;
    u.isPlayer = v.player;
    u.race = v.race;
    u.sex = v.sex;
    u.rank = v.rank;
    const uint16_t fBytes0 = game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0);
    const bool self = guid == ws.session_.playerGuid();
    uint32_t b0 = 0;
    if (self) {
        b0 = ws.session_.selfField(fBytes0);
    } else if (auto e = ws.session_.entities().getEntity(guid)) {
        b0 = e->getField(fBytes0);
    }
    u.classId = static_cast<uint8_t>((b0 >> 8) & 0xFF);
    if (self) {
        u.inCombat = ws.session_.inCombat();
        u.ghost = ws.session_.life() == pp::WorldSession::Life::Ghost;
        if (u.ghost) u.dead = true;
    } else {
        u.inCombat = ws.session_.isHostileAttacker(guid);
    }
    // reaction from the baked FactionTemplate (data/dbc.pak,
    // upstream hostility rules); without it NPCs attackable, players friendly.
    if (!self && ws.session_.factionsLoaded()) {
        u.canAttack = ws.session_.isHostileUnit(guid);
        u.isFriend = ws.session_.unitReactionToPlayer(guid) >= 5;
    } else {
        u.canAttack = !v.player && !self;
        u.isFriend = v.player || self;
    }
}

void WorldScreen::FrameXml::refresh() {
    fill(ws.session_.playerGuid(), player);
    fill(ws.session_.target(), target);
}

bool WorldScreen::FrameXml::unit(const char* token, fx::FxUnit& out) {
    if (!std::strcmp(token, "player")) {
        out = player;
        return out.exists;
    }
    if (!std::strcmp(token, "target")) {
        out = target;
        return out.exists;
    }
    out = fx::FxUnit{};
    return false;  // party/raid/pet/focus: not on the PSP yet
}

void WorldScreen::FrameXml::log(const char* line) { PSP_LOGI(pp::tag::Gfx, "%s", line); }

void WorldScreen::FrameXml::targetUnit(const char* token) {
    // SecureUnitButton / unit frame click: select that unit (CMSG_SET_SELECTION).
    if (!std::strcmp(token, "player")) ws.session_.setTarget(ws.session_.playerGuid());
}

void WorldScreen::FrameXml::clearTarget() { ws.session_.setTarget(0); }

void WorldScreen::destroyFrameXml(FrameXml* p) { delete p; }

void WorldScreen::updateFrameXml(float dtSec) {
    if (!fxInit_) {
        fxInit_ = true;
        const bool want = pp::configGetInt("framexml", 0) != 0;
        const bool bench = pp::configGetInt("luabench", 0) != 0;  // off by default (was 1)
        if (want || bench) {
            fx_ = std::unique_ptr<FrameXml, void (*)(FrameXml*)>(new FrameXml(*this), &destroyFrameXml);
            fx_->refresh();
            if (bench) FrameXmlUi::runBench(fx_.get());
            if (want && fx_->ui.load(fx_.get())) {
                fx_->ui.registerLayers(fx_->layers);
                fx_->initGame();  // hide bits (fxhide*), spell icon resolver
                // PSP: FrameXML's PlayerFrame/TargetFrame replace the port's own frames
                // (wowee.cfg fxhideframes=0 keeps both, for comparing).
                if (pp::configGetInt("fxhideframes", 1) != 0) unitFramesOn_ = false;
            } else {
                if (want) PSP_LOGW(pp::tag::Gfx, "framexml_fallback hud=builtin");
                fx_.reset();
            }
        }
    }
    if (!fx_) return;
    fx_->refresh();
    fx_->pollGame();  // cast / aura / action bar / cooldown events
    if (uiMode_ && !settings_.isOpen()) {
        if (winFocus_.cursorVisible()) fx_->ui.pointer(winFocus_.cursorX(), winFocus_.cursorY());
        if (const uint32_t act = winFocus_.activated()) fx_->ui.activate(act);
    }
    fx_->ui.update(dtSec);
}

void WorldScreen::drawFrameXml(UiRenderer& ui) {
    if (!fx_) return;
    {
        PROF_GE_PASS("ui.frames");
        fx_->ui.buildDrawList();
        ui.beginFrame();
        fx_->layers.draw(ui);
        ui.endFrame();
    }
    if (uiMode_ && !settings_.isOpen()) fx_->ui.declareStops(winFocus_);
    // GC after the list is built: the GE reads vertices, never Lua memory.
    fx_->ui.stepGc();
}

}  // namespace wowee::ui::psp
