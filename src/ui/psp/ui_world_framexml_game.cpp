// FrameXML's next frames on the PSP - the adapter half of the
// game API (fx_runtime_game.cpp binds the Lua names): ActionBar
// (MainMenuBar + ActionButton), CastingBarFrame, BuffFrame / TargetFrame
// auras, SpellBookFrame, ContainerFrame / CharacterFrame. The data is the
// session's (spells: actionBar(), knownSpells(), castSpell,
// playerAuras(), unitAuras(), unitCast(); inventory: getInventory(),
// swapContainerItems, autoEquip...) and SpellDb's - nothing is copied or
// kept twice, spell icons are the SpellDb cache (a small resolver).
//
// Events: once per frame the session state is compared with what Lua was
// last told and the differences queue upstream's addon events (upstream
// fires them from SpellHandler/InventoryHandler as the packets arrive; PSP:
// the session's queues are drained by the built-in UI, so the inventory
// events and the cast end are forwarded by those drains - one line each -
// and the rest is diffed here). Arguments are fixed values pushed straight
// onto the Lua stack (fx_runtime fireEventArgs) - no table per event.
//
// A/B (wowee.cfg, with framexml=1 and the frame present in framexml.pak):
//   fxhidebar=1   MainMenuBar / ActionButton1 replaces the built-in action bar
//   fxhidecast=1  CastingBarFrame replaces the built-in cast bars
//   fxhideauras=1 BuffFrame replaces the built-in buff/debuff rows
//   fxhidebook=1  SpellBookFrame replaces the "Book" window (the button toggles it)
//   fxhidebags=1  ContainerFrame1 replaces the bags window (Menu → Bags toggles it)
//   fxhidechar=1  CharacterFrame replaces the CHARACTER window
// 0 keeps the built-in one too (both drawn / the built-in window opens).
// Log: fx_hide mask= (once), fx_action ..., fx_cast ..., fx_event_drop n= (queue full).
#include "ui/psp/ui_world_framexml_impl.hpp"

#include "game/inventory_slots.hpp"
#include "game/spell_classification.hpp"
#include "game/update_field_table.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_strings.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_spell_db.hpp"

#include <pspctrl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
using game::ActionBarSlot;

namespace {
constexpr size_t kMaxQueued = 96;
constexpr uint32_t kSpellAttack = 6603;

uint32_t fnv(uint32_t h, uint32_t v) {
    for (int k = 0; k < 4; ++k) h = (h ^ ((v >> (8 * k)) & 0xFF)) * 16777619u;
    return h;
}
uint32_t auraSig(const std::vector<game::AuraSlot>* list) {
    uint32_t h = 2166136261u;
    if (!list) return h;
    for (const auto& a : *list) {
        if (a.isEmpty()) continue;
        h = fnv(h, a.spellId);
        h = fnv(h, a.flags | (static_cast<uint32_t>(a.charges) << 8));
        h = fnv(h, static_cast<uint32_t>(a.durationMs));
        h = fnv(h, static_cast<uint32_t>(a.receivedAtMs));
    }
    return h;
}
void fillItem(const game::ItemDef& d, fx::FxItem& o) {
    o = fx::FxItem{};
    o.entry = d.itemId;
    o.count = d.stackCount;
    o.quality = static_cast<uint8_t>(d.quality);
    o.invType = d.inventoryType;
    o.itemLevel = d.itemLevel;
    o.reqLevel = d.requiredLevel;
    o.maxStack = d.maxStack;
    o.sellPrice = d.sellPrice;
    o.bagSlots = d.bagSlots;
    o.durability = d.curDurability;
    o.maxDurability = d.maxDurability;
    std::snprintf(o.name, sizeof(o.name), "%s", d.name.c_str());
}
}  // namespace

// ------------------------------------------------------------ world hooks

bool WorldScreen::fxHides(uint8_t bit) const { return fx_ && (fx_->hide & bit) != 0; }

void WorldScreen::fxForwardInventoryEvent(const pp::WorldSession::InventoryEvent& ev) {
    if (fx_) fx_->forwardInventoryEvent(ev);
}

void WorldScreen::fxForwardCastEnd(uint8_t ended, uint32_t spellId) {
    if (fx_ && ended) fx_->forwardCastEnd(ended, spellId);
}

bool WorldScreen::fxOpenWindow(uint8_t w) { return fx_ && fx_->openWindow(w); }

// Cursor mode, a FrameXML frame focused: Square = its right click (FrameXML
// uses it: ContainerFrame's UseContainerItem, BuffButton's cancel) - the
// window-local Square of the bags window, no new game combination.
// wowee.cfg fxrightclick=0 turns it off.
void WorldScreen::fxWindowPad(const pp::PadState& pad) {
    if (!fx_ || !(pad.pressed & PSP_CTRL_SQUARE)) return;
    static const bool on = pp::configGetInt("fxrightclick", 1) != 0;
    if (on) fx_->ui.activate(winFocus_.focused(), true);
}

// ------------------------------------------------------------ setup

void WorldScreen::FrameXml::initGame() {
    fx::FxUi& f = ui.fx();
    struct Take {
        uint8_t bit;
        const char* key;
        const char* frames[2];
    };
    static const Take kTakes[] = {
        {WorldScreen::kFxHideBar, "fxhidebar", {"MainMenuBar", "ActionButton1"}},
        {WorldScreen::kFxHideCast, "fxhidecast", {"CastingBarFrame", nullptr}},
        {WorldScreen::kFxHideAuras, "fxhideauras", {"BuffFrame", nullptr}},
        {WorldScreen::kFxHideBook, "fxhidebook", {"SpellBookFrame", nullptr}},
        {WorldScreen::kFxHideBags, "fxhidebags", {"ContainerFrame1", nullptr}},
        {WorldScreen::kFxHideChar, "fxhidechar", {"CharacterFrame", "PaperDollFrame"}},
    };
    hide = 0;
    for (const Take& t : kTakes) {
        bool present = false;
        for (const char* n : t.frames)
            if (n && f.findByName(n) != fx::FxUi::kNone) present = true;
        if (present && pp::configGetInt(t.key, 1) != 0) hide |= t.bit;
    }
    ui.setDynTextures(&FrameXml::dynTexture, this);
    PSP_LOGI(pp::tag::Gfx, "fx_hide mask=%u bar=%d cast=%d auras=%d book=%d bags=%d char=%d", static_cast<unsigned>(hide),
             (hide & WorldScreen::kFxHideBar) ? 1 : 0, (hide & WorldScreen::kFxHideCast) ? 1 : 0, (hide & WorldScreen::kFxHideAuras) ? 1 : 0,
             (hide & WorldScreen::kFxHideBook) ? 1 : 0, (hide & WorldScreen::kFxHideBags) ? 1 : 0, (hide & WorldScreen::kFxHideChar) ? 1 : 0);
}

// Spell icons straight from the SpellDb cache (the built-in bar's; its
// eviction keeps what was drawn in the last two frames).
const pp::Texture* WorldScreen::FrameXml::dynTexture(void* ctx, uint8_t kind, uint32_t id) {
    auto* self = static_cast<FrameXml*>(ctx);
    if (kind == fx::kFxDynSpell) return self->ws.spellDb_.icon(id, self->ws.spellFrame_);
    return nullptr;  // PSP: no item icons on the card yet (the question mark stands in)
}

// The Menu / window bar asked for a built-in window FrameXML replaces:
// toggle the FrameXML panel instead (upstream's ToggleSpellBook /
// ToggleBackpack / ToggleCharacter when baked, else the frame itself).
bool WorldScreen::FrameXml::openWindow(uint8_t w) {
    const char* code = nullptr;
    if (w == WorldScreen::kWinSpellbook && (hide & WorldScreen::kFxHideBook))
        code = "__fx_toggle('ToggleSpellBook', BOOKTYPE_SPELL, 'SpellBookFrame')";
    else if (w == WorldScreen::kWinBags && (hide & WorldScreen::kFxHideBags))
        code = "__fx_toggle('ToggleBackpack', nil, 'ContainerFrame1')";
    else if (w == WorldScreen::kWinCharacter && (hide & WorldScreen::kFxHideChar))
        code = "__fx_toggle('ToggleCharacter', 'PaperDollFrame', 'CharacterFrame')";
    if (!code) return false;
    ui.runLua(code, "=fx_window");
    PSP_LOGI(pp::tag::Ctrl, "fx_window id=%u", static_cast<unsigned>(w));
    return true;
}

// ------------------------------------------------------------ events

void WorldScreen::FrameXml::push(const fx::FxEvent& ev) {
    if (events.size() >= kMaxQueued) {
        events.pop_front();
        if (++eventDrops <= 10 || eventDrops % 100 == 0)
            PSP_LOGW(pp::tag::Gfx, "fx_event_drop n=%u", static_cast<unsigned>(eventDrops));
    }
    events.push_back(ev);
}

bool WorldScreen::FrameXml::takeEvent(fx::FxEvent& out) {
    if (events.empty()) return false;
    out = events.front();
    events.pop_front();
    return true;
}

// Upstream spellcastArgs: unit, name, rank, castID, spellID (PSP: the last
// two as numbers - CastingBarFrame compares select(4, ...) with the castID
// UnitCastingInfo answers, a number).
void WorldScreen::FrameXml::castEvent(const char* name, const char* unit, uint32_t spellId) {
    fx::FxEvent ev;
    ev.name = name;
    const pp::SpellInfo* s = ws.spellDb_.get(spellId);
    ev.s(unit).s(s ? s->name.c_str() : "").s(s ? s->rank.c_str() : "");
    ev.n(static_cast<float>(spellId)).n(static_cast<float>(spellId));
    push(ev);
}

void WorldScreen::FrameXml::forwardCastEnd(uint8_t ended, uint32_t spellId) {
    castEnd = ended;
    castEndSpell = spellId;
}

// Upstream's addon events of InventoryHandler (the session queue, drained by
// updateInventory and forwarded here). PSP: bag / slot ids as numbers, like
// the original client (ContainerFrame compares GetID() == arg1).
void WorldScreen::FrameXml::forwardInventoryEvent(const pp::WorldSession::InventoryEvent& e) {
    fx::FxEvent ev;
    ev.name = e.name;  // string literals of the session
    const char* n = e.name;
    if (!std::strcmp(n, "BAG_UPDATE") || !std::strcmp(n, "BAG_CLOSED") || !std::strcmp(n, "PLAYERBANKSLOTS_CHANGED") ||
        !std::strcmp(n, "AUTOEQUIP_BIND_CONFIRM")) {
        ev.n(static_cast<float>(e.arg));
    } else if (!std::strcmp(n, "UNIT_INVENTORY_CHANGED")) {
        ev.s("player");
    } else if (!std::strcmp(n, "ITEM_PUSH")) {
        char icon[48];
        std::snprintf(icon, sizeof(icon), "Interface\\Icons\\#item:%u", static_cast<unsigned>(e.entry));
        ev.n(static_cast<float>(e.arg)).s(icon);
    } else if (!std::strcmp(n, "GET_ITEM_INFO_RECEIVED")) {
        ev.n(static_cast<float>(e.arg));
    } else if (!std::strcmp(n, "UI_ERROR_MESSAGE")) {
        ev.s(ws.session_.lastInventoryError().c_str());
    }
    push(ev);
}

void WorldScreen::FrameXml::pollGame() {
    PROF_SCOPE("ui.lua");
    auto& s = ws.session_;
    const float now = static_cast<float>(timeSec());
    auto simple = [this](const char* name) {
        fx::FxEvent ev;
        ev.name = name;
        push(ev);
    };

    // --- own cast (CastingBarFrame "player"): upstream SPELL_START / SPELL_GO /
    // CAST_FAILED / SPELL_FAILURE -> UNIT_SPELLCAST_*.
    CastSeen nowSelf;
    nowSelf.casting = s.isCasting();
    nowSelf.channel = s.isChanneling();
    nowSelf.spell = s.currentCastSpellId();
    nowSelf.endAt = now + s.castTimeRemaining();
    const bool ended = castSelf.casting && (!nowSelf.casting || nowSelf.spell != castSelf.spell);
    if (ended) {
        const uint32_t sp = castSelf.spell;
        if (castEnd == 2) castEvent("UNIT_SPELLCAST_INTERRUPTED", "player", sp);
        else castEvent("UNIT_SPELLCAST_SUCCEEDED", "player", sp);
        castEvent(castSelf.channel ? "UNIT_SPELLCAST_CHANNEL_STOP" : "UNIT_SPELLCAST_STOP", "player", sp);
        castEnd = 0;
    } else if (!castSelf.casting && !nowSelf.casting && castEnd) {
        // an instant (SPELL_GO without a start) or a refusal before any start
        if (castEnd == 1) castEvent("UNIT_SPELLCAST_SUCCEEDED", "player", castEndSpell);
        else {
            castEvent("UNIT_SPELLCAST_FAILED", "player", castEndSpell);
            castEvent("UNIT_SPELLCAST_STOP", "player", castEndSpell);
        }
        castEnd = 0;
    }
    if (nowSelf.casting && (ended || !castSelf.casting)) {
        castEvent(nowSelf.channel ? "UNIT_SPELLCAST_CHANNEL_START" : "UNIT_SPELLCAST_START", "player", nowSelf.spell);
        castEnd = 0;
    } else if (nowSelf.casting && castSelf.casting && std::fabs(nowSelf.endAt - castSelf.endAt) > 0.08f) {
        // SMSG_SPELL_DELAYED / MSG_CHANNEL_UPDATE moved the end
        castEvent(nowSelf.channel ? "UNIT_SPELLCAST_CHANNEL_UPDATE" : "UNIT_SPELLCAST_DELAYED", "player", nowSelf.spell);
    }
    castSelf = nowSelf;

    // --- the target's cast (TargetFrameSpellBar): a new target resyncs
    // silently (the bar reads UnitCastingInfo on PLAYER_TARGET_CHANGED).
    const uint64_t tg = s.target();
    CastSeen nowT;
    if (tg && tg != s.playerGuid()) {
        if (const game::UnitCastState* c = s.unitCast(tg)) {
            nowT.casting = c->casting;
            nowT.channel = c->isChannel;
            nowT.spell = c->spellId;
            nowT.endAt = now + c->timeRemaining;
        }
    }
    if (tg != castTargetGuid) {
        castTargetGuid = tg;
    } else {
        const bool tEnded = castTarget.casting && (!nowT.casting || nowT.spell != castTarget.spell);
        if (tEnded)
            castEvent(castTarget.channel ? "UNIT_SPELLCAST_CHANNEL_STOP" : "UNIT_SPELLCAST_STOP", "target",
                      castTarget.spell);
        if (nowT.casting && (tEnded || !castTarget.casting))
            castEvent(nowT.channel ? "UNIT_SPELLCAST_CHANNEL_START" : "UNIT_SPELLCAST_START", "target", nowT.spell);
    }
    castTarget = nowT;

    // --- auras (BuffFrame, TargetFrame): UNIT_AURA + PLAYER_AURAS_CHANGED.
    const uint32_t ap = auraSig(&s.playerAuras());
    if (ap != auraSigPlayer) {
        auraSigPlayer = ap;
        fx::FxEvent ev;
        ev.name = "UNIT_AURA";
        push(ev.s("player"));
        simple("PLAYER_AURAS_CHANGED");
    }
    const uint32_t at = auraSig(tg ? s.unitAuras(tg) : nullptr);
    if (tg != auraTargetGuid) {
        auraTargetGuid = tg;
        auraSigTarget = at;
    } else if (at != auraSigTarget) {
        auraSigTarget = at;
        fx::FxEvent ev;
        ev.name = "UNIT_AURA";
        push(ev.s("target"));
    }

    // --- action bar (SMSG_ACTION_BUTTONS / CMSG_SET_ACTION_BUTTON):
    // ACTIONBAR_SLOT_CHANGED per slot, 0 = all (upstream).
    const auto& bar = s.actionBar();
    int changed = 0;
    for (int i = 0; i < pp::WorldSession::kActionBarSlots; ++i) {
        const uint8_t type = static_cast<uint8_t>(bar[static_cast<size_t>(i)].type);
        const uint32_t id = bar[static_cast<size_t>(i)].id;
        fx::FxAction& seen = barSeen[static_cast<size_t>(i)];
        if (barInit && seen.type == type && seen.id == id) continue;
        seen.type = type;
        seen.id = id;
        ++changed;
        if (barInit && changed <= 12) {
            fx::FxEvent ev;
            ev.name = "ACTIONBAR_SLOT_CHANGED";
            push(ev.n(static_cast<float>(i + 1)));
        }
    }
    if (changed && (!barInit || changed > 12)) {
        fx::FxEvent ev;
        ev.name = "ACTIONBAR_SLOT_CHANGED";
        push(ev.n(0));
    }
    if (changed) {
        simple("ACTIONBAR_UPDATE_STATE");
        cdSeen.clear();  // the spell list below is rebuilt
        for (const auto& b : barSeen)
            if (b.type == ActionBarSlot::SPELL && b.id &&
                std::find_if(cdSeen.begin(), cdSeen.end(), [&](const auto& p) { return p.first == b.id; }) == cdSeen.end())
                cdSeen.emplace_back(b.id, -1.0f);
    }
    barInit = true;

    // --- stance / form: the bonus bar (UPDATE_BONUS_ACTIONBAR).
    const int bonus = bonusBarOffset();
    if (bonus != bonusSeen) {
        if (bonusSeen >= 0) {
            simple("UPDATE_BONUS_ACTIONBAR");
            simple("ACTIONBAR_PAGE_CHANGED");
        }
        bonusSeen = bonus;
    }

    // --- cooldowns: a cooldown or GCD that started (or restarted) ->
    // SPELL_UPDATE_COOLDOWN + ACTIONBAR_UPDATE_COOLDOWN (upstream SMSG_SPELL_COOLDOWN).
    bool cdChanged = false;
    for (auto& [id, start] : cdSeen) {
        const float rem = s.spellCooldown(id), total = s.spellCooldownTotal(id);
        const float st = rem > 0.0f ? now - (total - rem) : 0.0f;
        if (std::fabs(st - start) > 0.1f) {
            if (start >= 0.0f) cdChanged = true;
            start = st;
        }
    }
    const float gRem = s.gcdRemaining();
    const float gStart = gRem > 0.0f ? now - (s.gcdTotal() - gRem) : 0.0f;
    if (std::fabs(gStart - gcdStartSeen) > 0.1f) {
        cdChanged = true;
        gcdStartSeen = gStart;
    }
    if (cdChanged) {
        simple("SPELL_UPDATE_COOLDOWN");
        simple("ACTIONBAR_UPDATE_COOLDOWN");
    }

    // --- usable (mana): at most every 0.25 s when the own power changed.
    const uint32_t pw = player.power[player.powerType & 7];
    if (pw != powerSeen && now >= usableTimer) {
        powerSeen = pw;
        usableTimer = now + 0.25f;
        simple("ACTIONBAR_UPDATE_USABLE");
    }

    // --- known spells (SMSG_LEARNED_SPELL / SUPERCEDED / REMOVED).
    if (s.knownSpellsVersion() != knownSeen) {
        const bool first = knownSeen == ~0u;
        knownSeen = s.knownSpellsVersion();
        if (!first) simple("SPELLS_CHANGED");
    }
}

// ------------------------------------------------------------ actions

bool WorldScreen::FrameXml::action(int slot, fx::FxAction& out) {
    if (slot < 0 || slot >= pp::WorldSession::kActionBarSlots) return false;
    const ActionBarSlot& a = ws.session_.actionBar()[static_cast<size_t>(slot)];
    out.type = static_cast<uint8_t>(a.type);
    out.id = a.id;
    return a.type != ActionBarSlot::EMPTY;
}

// Upstream lua_UseAction: a spell at the unit (self-cast "player") or the
// target; the rest as the built-in bar's button (castFromSlot: Attack
// toggles auto-attack, stand up, turn to the target).
void WorldScreen::FrameXml::useAction(int slot, const char* unit) {
    if (slot < 0 || slot >= pp::WorldSession::kActionBarSlots) return;
    const ActionBarSlot& a = ws.session_.actionBar()[static_cast<size_t>(slot)];
    if (a.type == ActionBarSlot::SPELL && a.id != kSpellAttack && unit && *unit) {
        const uint64_t g = unitGuid(unit);
        if (g) {
            castSpellAt(a.id, g);
            return;
        }
    }
    ws.castFromSlot(slot);
}

// GetBonusBarOffset: the form's SpellShapeshiftForm.BonusActionBar (the
// built-in bar's mainBarBase rule).
int WorldScreen::FrameXml::bonusBarOffset() {
    constexpr uint16_t kUnitFieldBytes2 = 122;  // see WorldScreen::mainBarBase
    const uint32_t b2 = ws.session_.selfField(kUnitFieldBytes2);
    const uint8_t bonus = ws.spellDb_.bonusBar(static_cast<uint8_t>((b2 >> 24) & 0xFF));
    return bonus > 0 && bonus <= 4 ? bonus : 0;
}

uint64_t WorldScreen::FrameXml::unitGuid(const char* unit) const {
    if (!unit) return 0;
    if (!std::strcmp(unit, "player")) return ws.session_.playerGuid();
    if (!std::strcmp(unit, "target")) return ws.session_.target();
    return 0;
}

// The built-in bar's cast path for a spell id (WorldScreen::castFromSlot):
// Attack toggles, stand up, PSP autoface to a unit target.
void WorldScreen::FrameXml::castSpellAt(uint32_t spellId, uint64_t targetGuid) {
    if (spellId == kSpellAttack) {
        ws.toggleAutoAttack();
        return;
    }
    if (ws.sitting_) ws.setSitting(false, false);
    const pp::SpellInfo* info = ws.spellDb_.get(spellId);
    const uint64_t t = targetGuid ? targetGuid : ws.session_.target();
    const bool selfOnly = info && game::spellclass::isSelfCastRange(info->rec.maxRange);
    glm::vec3 tp;
    float h = 0.0f;
    if (ws.autoFace_ && !selfOnly && t != 0 && t != ws.session_.playerGuid() && ws.unitPosition(t, tp, h)) {
        const float dx = tp.x - ws.playerX_, dy = tp.y - ws.playerY_;
        if (std::fabs(dx) > 0.01f || std::fabs(dy) > 0.01f) {
            ws.facing_ = std::atan2(dy, dx);
            ws.sendMove(game::Opcode::MSG_MOVE_SET_FACING, ws.moving_);
        }
    }
    ws.session_.castSpell(spellId, targetGuid, ws.wantMove_ || ws.moving_);
}

void WorldScreen::FrameXml::castSpell(uint32_t id, const char* unit) {
    if (++castSpellLogs <= 60) PSP_LOGI(pp::tag::Gfx, "fx_action cast spell=%u unit=%s", static_cast<unsigned>(id), unit ? unit : "-");
    castSpellAt(id, unitGuid(unit));
}

// ------------------------------------------------------------ spells

bool WorldScreen::FrameXml::spellInfo(uint32_t id, fx::FxSpellInfo& out) {
    const pp::SpellInfo* s = ws.spellDb_.get(id);
    if (!s) return false;
    out = fx::FxSpellInfo{};
    std::snprintf(out.name, sizeof(out.name), "%s", s->name.c_str());
    std::snprintf(out.rank, sizeof(out.rank), "%s", s->rank.c_str());
    out.passive = s->passive();
    out.castMs = s->rec.castMs;
    out.powerCost = s->rec.manaCost;
    out.powerType = s->rec.powerType;
    out.dispel = s->rec.dispel;
    out.minRange = s->rec.minRange;
    out.maxRange = s->rec.maxRange;
    return true;
}

bool WorldScreen::FrameXml::spellKnown(uint32_t id) { return ws.session_.knownSpells().count(id) != 0; }

bool WorldScreen::FrameXml::spellCooldown(uint32_t id, float& remaining, float& total) {
    remaining = ws.session_.spellCooldown(id);
    total = ws.session_.spellCooldownTotal(id);
    return remaining > 0.0f;
}

bool WorldScreen::FrameXml::gcd(float& remaining, float& total) {
    remaining = ws.session_.gcdRemaining();
    total = ws.session_.gcdTotal();
    return remaining > 0.0f;
}

bool WorldScreen::FrameXml::isCurrentSpell(uint32_t id) {
    if (id == kSpellAttack) return ws.session_.autoAttacking() || ws.session_.autoAttackRequested();
    return ws.session_.isCasting() && ws.session_.currentCastSpellId() == id;
}

float WorldScreen::FrameXml::unitDistance(const char* unit) {
    const uint64_t g = unitGuid(unit);
    if (!g) return -1.0f;
    if (g == ws.session_.playerGuid()) return 0.0f;
    glm::vec3 p;
    float h = 0.0f;
    if (!ws.unitPosition(g, p, h)) return -1.0f;
    const float dx = p.x - ws.playerX_, dy = p.y - ws.playerY_, dz = p.z - ws.playerZ_;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

uint32_t WorldScreen::FrameXml::spellBookVersion() { return ws.session_.knownSpellsVersion(); }

// Upstream SpellHandler::getSpellBookTabs, cut to what spells.pak carries:
// highest rank per name, no passive / hidden spells (the built-in book's
// rules), a tab per SkillLine (0 = General first), names alphabetical.
// PSP: no SkillLine names / categories on the card - tab names are
// placeholders until task A's SkillLine records (merge: use its tabs).
void WorldScreen::FrameXml::spellBook(std::vector<fx::FxSpellTab>& tabs, std::vector<uint32_t>& spells) {
    tabs.clear();
    spells.clear();
    std::map<std::string, std::pair<uint32_t, int>> best;  // name -> spell, rank
    for (uint32_t id : ws.session_.knownSpells()) {
        const pp::SpellInfo* s = ws.spellDb_.get(id);
        if (!s || s->name.empty() || s->passive() || s->hiddenClientSide()) continue;
        const int rank = game::spellclass::rankValue(s->rank);
        auto it = best.find(s->name);
        if (it == best.end() || rank > it->second.second) best[s->name] = {id, rank};
    }
    std::map<uint16_t, std::vector<uint32_t>> byLine;  // std::map: General (0) first
    for (const auto& [name, v] : best) byLine[ws.spellDb_.get(v.first)->rec.skillLine].push_back(v.first);
    for (auto& [line, ids] : byLine) {
        fx::FxSpellTab t;
        // SkillLine names from spells.pak ("skills", task A) when baked.
        const std::string& lineName = line ? ws.session_.getSkillLineName(line) : std::string();
        if (line == 0) std::snprintf(t.name, sizeof(t.name), "%s", CLIENT_STR(GENERAL_SPELLS));
        else if (!lineName.empty()) std::snprintf(t.name, sizeof(t.name), "%s", lineName.c_str());
        else std::snprintf(t.name, sizeof(t.name), "%s", OWN_FMT(FX_SKILL_LINE, static_cast<unsigned>(line)).c_str());
        t.iconSpell = ids.empty() ? 0 : ids.front();
        t.offset = static_cast<uint32_t>(spells.size());
        t.count = static_cast<uint32_t>(ids.size());
        spells.insert(spells.end(), ids.begin(), ids.end());  // already by name (std::map order)
        tabs.push_back(t);
    }
    PSP_LOGI(pp::tag::Gfx, "fx_spellbook tabs=%u spells=%u", static_cast<unsigned>(tabs.size()),
             static_cast<unsigned>(spells.size()));
}

size_t WorldScreen::FrameXml::auras(const char* unit, fx::FxAura* out, size_t max) {
    const uint64_t g = unitGuid(unit);
    const std::vector<game::AuraSlot>* list = nullptr;
    if (g == ws.session_.playerGuid()) list = &ws.session_.playerAuras();
    else if (g) list = ws.session_.unitAuras(g);
    if (!list) return 0;
    const uint64_t nowMs = pp::WorldSession::auraNowMs();
    size_t n = 0;
    for (const auto& a : *list) {
        if (n >= max) break;
        if (a.isEmpty()) continue;
        fx::FxAura& o = out[n++];
        o.spellId = a.spellId;
        o.flags = a.flags;
        o.charges = a.charges;
        o.caster = a.casterGuid;
        o.durationMs = a.maxDurationMs;
        o.remainingMs = a.getRemainingMs(nowMs);
    }
    return n;
}

void WorldScreen::FrameXml::cancelAura(uint32_t spellId) { ws.session_.cancelAura(spellId); }

bool WorldScreen::FrameXml::unitCast(const char* unit, fx::FxCast& out) {
    out = fx::FxCast{};
    const uint64_t g = unitGuid(unit);
    if (!g) return false;
    auto& s = ws.session_;
    if (g == s.playerGuid()) {
        out.casting = s.isCasting();
        out.channel = s.isChanneling();
        out.spellId = s.currentCastSpellId();
        out.total = s.castTimeTotal();
        out.remaining = s.castTimeRemaining();
        return out.casting;
    }
    const game::UnitCastState* c = s.unitCast(g);
    if (!c) return false;
    out.casting = c->casting;
    out.channel = c->isChannel;
    out.spellId = c->spellId;
    out.total = c->timeTotal;
    out.remaining = c->timeRemaining;
    out.interruptible = c->interruptible;
    return out.casting;
}

// ------------------------------------------------------------ bags

int WorldScreen::FrameXml::containerSlots(int bag) {
    const auto& inv = ws.session_.getInventory();
    if (bag == 0) return inv.getBackpackSize();
    if (bag >= 1 && bag <= game::Inventory::NUM_BAG_SLOTS) return inv.getBagSize(bag - 1);
    return 0;  // PSP: bank / keyring containers not wired yet
}

bool WorldScreen::FrameXml::containerItem(int bag, int slot, fx::FxItem& out) {
    const auto& inv = ws.session_.getInventory();
    if (slot < 1 || slot > containerSlots(bag)) return false;
    const game::ItemSlot& is = bag == 0 ? inv.getBackpackSlot(slot - 1) : inv.getBagSlot(bag - 1, slot - 1);
    if (is.empty()) return false;
    fillItem(is.item, out);
    return true;
}

bool WorldScreen::FrameXml::inventoryItem(int invSlot, fx::FxItem& out) {
    if (invSlot < 1 || invSlot > game::Inventory::NUM_EQUIP_SLOTS) return false;
    const game::ItemSlot& is = ws.session_.getInventory().getEquipSlot(static_cast<game::EquipSlot>(invSlot - 1));
    if (is.empty()) return false;
    fillItem(is.item, out);
    return true;
}

bool WorldScreen::FrameXml::itemInfo(uint32_t entry, fx::FxItem& out) {
    const game::ItemQueryResponseData* q = ws.session_.getItemInfo(entry);
    if (!q || !q->valid) {
        if (!q) ws.session_.ensureItemInfo(entry);  // GET_ITEM_INFO_RECEIVED follows
        return false;
    }
    out = fx::FxItem{};
    out.entry = entry;
    out.count = 1;
    out.quality = static_cast<uint8_t>(q->quality);
    out.invType = static_cast<uint8_t>(q->inventoryType);
    out.itemLevel = q->itemLevel;
    out.reqLevel = q->requiredLevel;
    out.maxStack = q->maxStack > 0 ? static_cast<uint32_t>(q->maxStack) : 1u;
    out.sellPrice = q->sellPrice;
    out.bagSlots = q->containerSlots;
    std::snprintf(out.name, sizeof(out.name), "%s", q->name.c_str());
    return true;
}

uint32_t WorldScreen::FrameXml::itemCount(uint32_t entry) { return ws.session_.getInventory().countItem(entry); }

// Upstream lua_UseContainerItem after the window verbs: equippable ->
// autoEquip (the bind prompt is the built-in one, kWinBindConfirm); the rest
// is not on the PSP yet (upstream: quest / read / open / use).
void WorldScreen::FrameXml::useContainerItem(int bag, int slot) {
    fx::FxItem it;
    if (!containerItem(bag, slot, it)) return;
    const game::ItemQueryResponseData* q = ws.session_.getItemInfo(it.entry);
    if (!q) {
        ws.session_.ensureItemInfo(it.entry);
        return;
    }
    const bool equippable = q->valid && q->inventoryType != 0 && q->itemClass != game::ITEM_CLASS_QUEST;
    if (equippable) {
        if (bag == 0) ws.session_.autoEquipItemBySlot(slot - 1);
        else ws.session_.autoEquipItemInBag(bag - 1, slot - 1);
        return;
    }
    fx::FxEvent ev;
    ev.name = "UI_ERROR_MESSAGE";
    push(ev.s(OWN_STR(FX_ITEM_USE_UNSUPPORTED)));
}

// PickupContainerItem / PickupInventoryItem pairs, PutItemInBackpack /
// PutItemInBag - the built-in window's Square rules: a free slot of a
// bag, a bag into an empty bag slot, two worn bags change places, else
// CMSG_SWAP_ITEM (the server answers a misfit with an error).
void WorldScreen::FrameXml::moveItem(int srcBag, int srcSlot, int dstBag, int dstSlot) {
    auto& s = ws.session_;
    const auto& inv = s.getInventory();
    namespace sl = game::slots;
    auto wire = [&](int bag, int slot, uint8_t& wb, uint8_t& ws8) -> bool {
        if (bag == -1) {  // inventory slot 1..23 -> equip 0..22
            if (slot < 1 || slot > game::Inventory::NUM_EQUIP_SLOTS) return false;
            wb = sl::kNoContainer;
            ws8 = static_cast<uint8_t>(slot - 1);
        } else if (bag == 0) {
            if (slot < 1 || slot > inv.getBackpackSize()) return false;
            wb = sl::kNoContainer;
            ws8 = static_cast<uint8_t>(sl::backpackWireSlot(slot - 1));
        } else if (bag >= 1 && bag <= game::Inventory::NUM_BAG_SLOTS) {
            if (slot < 1 || slot > inv.getBagSize(bag - 1)) return false;
            wb = static_cast<uint8_t>(sl::wornBagContainer(bag - 1));
            ws8 = static_cast<uint8_t>(slot - 1);
        } else {
            return false;
        }
        return true;
    };
    uint8_t sb = 0, ss = 0;
    if (!wire(srcBag, srcSlot, sb, ss)) return;
    const bool srcWornBag = srcBag == -1 && srcSlot >= 20 && srcSlot <= 23;
    if (dstBag == -1 && dstSlot >= 20 && dstSlot <= 23) {  // onto a bag slot
        if (srcWornBag) {
            s.swapBagSlots(srcSlot - 20, dstSlot - 20);
            return;
        }
        if (inv.getBagSize(dstSlot - 20) > 0) {  // PutItemInBag into that bag
            moveItem(srcBag, srcSlot, dstSlot - 19, 0);
            return;
        }
    }
    if (dstSlot == 0) {  // the first free slot of dstBag
        if (dstBag == 0) {
            const int free = inv.findFreeBackpackSlot();
            if (free >= 0) s.swapContainerItems(sb, ss, sl::kNoContainer, static_cast<uint8_t>(sl::backpackWireSlot(free)));
            return;
        }
        if (dstBag >= 1 && dstBag <= game::Inventory::NUM_BAG_SLOTS) {
            const int size = inv.getBagSize(dstBag - 1);
            if (size == 0) {  // an empty bag slot: equip the held bag there
                s.swapContainerItems(sb, ss, sl::kNoContainer, static_cast<uint8_t>(sl::kWornBagFirst + dstBag - 1));
                return;
            }
            for (int i = 0; i < size; ++i)
                if (inv.getBagSlot(dstBag - 1, i).empty()) {
                    s.swapContainerItems(sb, ss, static_cast<uint8_t>(sl::wornBagContainer(dstBag - 1)), static_cast<uint8_t>(i));
                    return;
                }
        }
        return;
    }
    uint8_t db = 0, ds = 0;
    if (!wire(dstBag, dstSlot, db, ds)) return;
    s.swapContainerItems(sb, ss, db, ds);
}

// ------------------------------------------------------------ character, sound

int32_t WorldScreen::FrameXml::unitField(int field) {
    using game::UF;
    auto& s = ws.session_;
    auto at = [&](UF f, int add) { return static_cast<int32_t>(s.selfField(static_cast<uint16_t>(game::fieldIndex(f) + add))); };
    if (field >= fx::kFxStat0 && field < fx::kFxStat0 + 5) return at(UF::UNIT_FIELD_STAT0, field - fx::kFxStat0);
    if (field >= fx::kFxResist0 && field < fx::kFxResist0 + 7) return at(UF::UNIT_FIELD_RESISTANCES, field - fx::kFxResist0);
    if (field == fx::kFxAttackPower) return at(UF::UNIT_FIELD_ATTACK_POWER, 0);
    if (field == fx::kFxRangedAttackPower) return at(UF::UNIT_FIELD_RANGED_ATTACK_POWER, 0);
    if (field == fx::kFxMinDamage) return at(UF::UNIT_FIELD_MINDAMAGE, 0);
    if (field == fx::kFxMaxDamage) return at(UF::UNIT_FIELD_MAXDAMAGE, 0);
    return 0;
}

// FrameXML PlaySound -> the sound code's UiSoundManager::playByName (sounds.pak).
bool WorldScreen::FrameXml::playSound(const char* name) { return playSoundByName(name); }
bool WorldScreen::FrameXml::playSoundId(uint32_t soundEntryId) { return playSound2D(soundEntryId); }

}  // namespace wowee::ui::psp
