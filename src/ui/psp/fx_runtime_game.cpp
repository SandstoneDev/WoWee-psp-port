// The FrameXML game API of the next frames - ActionBar
// (MainMenuBar + ActionButton), CastingBarFrame, BuffFrame / TargetFrame
// auras, SpellBookFrame, ContainerFrame / CharacterFrame - bound to
// upstream's Lua names over FxGameApi (the PSP adapter answers from
// WorldSession + SpellDb, ui_world_framexml_game.cpp).
//
// Semantics follow upstream's bindings, function by function:
//   src/addons/lua_action_api.cpp   HasAction, GetActionTexture, GetActionInfo,
//                                   GetActionCount, GetActionCooldown,
//                                   IsUsableAction, IsActionInRange, UseAction
//   src/addons/lua_spell_api.cpp    UnitBuff/UnitDebuff/UnitAura, UnitCastingInfo,
//                                   UnitChannelInfo, GetNumSpellTabs, GetSpellTabInfo,
//                                   GetSpellName, GetSpellTexture, CastSpell, ...
//   src/addons/lua_inventory_api.cpp GetContainerItemInfo, GetInventorySlotInfo, ...
//   src/addons/lua_system_api.cpp   PlaySound
// Platform-free like fx_runtime.cpp (the host test builds it). PSP
// differences are marked "PSP:".
#include "ui/psp/fx_runtime.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace wowee::ui::fx {

FxEvent& FxEvent::s(const char* v) {
    if (argc < 5) {
        args[argc].kind = 2;
        std::snprintf(args[argc].str, sizeof(args[argc].str), "%s", v ? v : "");
        ++argc;
    }
    return *this;
}

// ======================================================================
// Runtime side: game textures, frame times, events, the spell book

int32_t FxUi::dynTexture(uint8_t kind, uint32_t id) {
    const uint64_t key = (static_cast<uint64_t>(kind) << 32) | id;
    auto it = dynIndex_.find(key);
    if (it != dynIndex_.end()) return it->second;
    const int32_t t = kFxDynTexBase + static_cast<int32_t>(dyn_.size());
    dyn_.emplace_back(kind, id);
    dynIndex_.emplace(key, t);
    return t;
}

bool FxUi::dynTexInfo(int32_t tex, uint8_t& kind, uint32_t& id) const {
    if (tex < kFxDynTexBase) return false;
    const size_t i = static_cast<size_t>(tex - kFxDynTexBase);
    if (i >= dyn_.size()) return false;
    kind = dyn_[i].first;
    id = dyn_[i].second;
    return true;
}

// "Interface\Icons\#spell:133" (what GetActionTexture & co. answer): PSP:
// spells.pak keeps icons by slot, not by file name, so the game icon travels
// as this name and the renderer asks the adapter for it (no second copy of
// the icon data in framexml.pak).
int32_t FxUi::resolveTexture(const char* path) {
    const int t = textureByPath(path);
    if (t >= 0 || !path) return t;
    const char* h = std::strchr(path, '#');
    if (!h) return -1;
    if (std::strncmp(h, "#spell:", 7) == 0) return dynTexture(kFxDynSpell, static_cast<uint32_t>(std::strtoul(h + 7, nullptr, 10)));
    if (std::strncmp(h, "#item:", 6) == 0) return dynTexture(kFxDynItem, static_cast<uint32_t>(std::strtoul(h + 6, nullptr, 10)));
    return -1;
}

uint32_t FxUi::topLevelOf(uint32_t i) const {
    for (int d = 0; i != kNone && d < 64; ++d) {
        const uint32_t p = objs_[i].parent;
        if (p == kNone || p == uiParent_) return i;
        i = p;
    }
    return i;
}

void FxUi::takeFrameTimes(std::vector<FxFrameTime>& out) {
    out.clear();
    for (auto& [obj, t] : frameTimes_) {
        FxFrameTime f = t;
        f.obj = obj;
        out.push_back(f);
    }
    std::sort(out.begin(), out.end(), [](const FxFrameTime& a, const FxFrameTime& b) { return a.us > b.us; });
    frameTimes_.clear();
}

void FxUi::refreshSpellBook() {
    const uint32_t v = api_->spellBookVersion();
    if (v == bookVersion_) return;
    bookVersion_ = v;
    api_->spellBook(bookTabs_, bookSpells_);
}

// Once per frame after the unit diff: the adapter's queue (session events,
// diffed or forwarded). PSP: at most 48 per frame, the rest next frame.
void FxUi::pollGameEvents() {
    FxEvent ev;
    for (int n = 0; n < 48 && api_->takeEvent(ev); ++n) {
        if (std::strncmp(ev.name, "UNIT_SPELLCAST_", 15) == 0 && ++castLogs_ <= 60)
            logf("fx_cast ev=%s unit=%s spell=%u", ev.name + 15, ev.argc > 0 ? ev.args[0].str : "?",
                 static_cast<unsigned>(ev.argc > 4 ? ev.args[4].num : 0.0f));
        if (std::strcmp(ev.name, "SPELLS_CHANGED") == 0) bookVersion_ = ~0u;
        fireEventArgs(ev.name, ev.args, ev.argc);
        ev = FxEvent{};
    }
}

namespace {

FxUi* U(lua_State* L) { return FxUi::fromLua(L); }
FxGameApi* A(lua_State* L) { return U(L)->api(); }

int retNil(lua_State* L) {
    lua_pushnil(L);
    return 1;
}
int retFalse(lua_State* L) {
    lua_pushboolean(L, 0);
    return 1;
}
int retZero(lua_State* L) {
    lua_pushnumber(L, 0);
    return 1;
}

void lowerTok(const char* in, char* out, size_t n) {
    size_t k = 0;
    for (; in && in[k] && k < n - 1; ++k) out[k] = static_cast<char>(std::tolower(static_cast<unsigned char>(in[k])));
    out[k] = 0;
}

void pushSpellIcon(lua_State* L, uint32_t spellId) { lua_pushfstring(L, "Interface\\Icons\\#spell:%d", static_cast<int>(spellId)); }
// PSP: no item icons on the card yet (an open question) - the item's
// own name is kept so a later icon pak can answer it by entry.
void pushItemIcon(lua_State* L, uint32_t entry) { lua_pushfstring(L, "Interface\\Icons\\#item:%d", static_cast<int>(entry)); }

const char* dispelName(uint8_t d) {
    switch (d) {
        case 1: return "Magic";
        case 2: return "Curse";
        case 3: return "Disease";
        case 4: return "Poison";
        default: return nullptr;
    }
}

// Upstream ITEM_QUALITY_COLORS (poor .. heirloom).
const uint32_t kQualityRgb[8] = {0x9d9d9d, 0xffffff, 0x1eff00, 0x0070dd, 0xa335ee, 0xff8000, 0xe6cc80, 0xe6cc80};

void pushItemLink(lua_State* L, const FxItem& it) {
    char b[160];
    std::snprintf(b, sizeof(b), "|cff%06x|Hitem:%u:0:0:0:0:0:0:0:0|h[%s]|h|r",
                  static_cast<unsigned>(kQualityRgb[it.quality & 7]), static_cast<unsigned>(it.entry), it.name);
    lua_pushstring(L, b);
}

// ---------------------------------------------------------------- actions
// Lua slots are 1-based (upstream: slot - 1).
bool actionAt(lua_State* L, int idx, int& slot, FxAction& a) {
    slot = static_cast<int>(luaL_checknumber(L, idx)) - 1;
    return slot >= 0 && A(L)->action(slot, a) && a.type != 0;
}

int g_HasAction(lua_State* L) {
    int s;
    FxAction a;
    lua_pushboolean(L, actionAt(L, 1, s, a));
    return 1;
}
int g_GetActionTexture(lua_State* L) {
    int s;
    FxAction a;
    if (!actionAt(L, 1, s, a)) return retNil(L);
    if (a.type == 1) pushSpellIcon(L, a.id);
    else if (a.type == 2 && a.id) pushItemIcon(L, a.id);
    else lua_pushnil(L);  // PSP: macros have no icon here (no macro store)
    return 1;
}
int g_GetActionInfo(lua_State* L) {
    int s;
    FxAction a;
    if (!actionAt(L, 1, s, a)) return 0;
    static const char* const kType[4] = {"", "spell", "item", "macro"};
    lua_pushstring(L, kType[a.type & 3]);
    lua_pushnumber(L, static_cast<lua_Number>(a.id));
    lua_pushstring(L, kType[a.type & 3]);
    if (a.type == 1) {
        lua_pushnumber(L, static_cast<lua_Number>(a.id));
        return 4;
    }
    return 3;
}
int g_GetActionCount(lua_State* L) {
    int s;
    FxAction a;
    lua_pushnumber(L, actionAt(L, 1, s, a) && a.type == 2 ? static_cast<lua_Number>(A(L)->itemCount(a.id)) : 0);
    return 1;
}
// start, duration, enable - the spell's own cooldown, else the GCD on a spell.
int pushCooldown(lua_State* L, uint32_t spellId, bool isSpell) {
    FxGameApi* api = A(L);
    float rem = 0, total = 0;
    const float now = static_cast<float>(api->timeSec());
    if (spellId && api->spellCooldown(spellId, rem, total) && rem > 0.0f) {
        lua_pushnumber(L, now - (total - rem));
        lua_pushnumber(L, total);
    } else if (isSpell && api->gcd(rem, total) && rem > 0.0f) {
        lua_pushnumber(L, now - (total - rem));
        lua_pushnumber(L, total);
    } else {
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
    }
    lua_pushnumber(L, 1);
    return 3;
}
int g_GetActionCooldown(lua_State* L) {
    int s;
    FxAction a;
    if (!actionAt(L, 1, s, a)) {
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 1);
        return 3;
    }
    return pushCooldown(L, a.type == 1 ? a.id : 0, a.type == 1);
}
// usable, notEnoughMana: known + affordable (not the cooldown - upstream).
bool spellUsable(lua_State* L, uint32_t id, bool& noMana) {
    FxGameApi* api = A(L);
    noMana = false;
    if (!api->spellKnown(id)) return false;
    FxSpellInfo si;
    if (api->spellInfo(id, si) && si.powerCost > 0) {
        FxUnit p;
        if (api->unit("player", p) && p.exists && p.power[si.powerType & 7] < si.powerCost) {
            noMana = true;
            return false;
        }
    }
    return true;
}
int g_IsUsableAction(lua_State* L) {
    int s;
    FxAction a;
    bool usable = false, noMana = false;
    if (actionAt(L, 1, s, a)) usable = a.type == 1 ? spellUsable(L, a.id, noMana) : true;
    lua_pushboolean(L, usable);
    lua_pushboolean(L, noMana);
    return 2;
}
// PSP: upstream answers false (no tracking); the PSP knows the spell being
// cast and the auto-attack toggle, so the button is checked like the client's.
int g_IsCurrentAction(lua_State* L) {
    int s;
    FxAction a;
    lua_pushboolean(L, actionAt(L, 1, s, a) && a.type == 1 && A(L)->isCurrentSpell(a.id));
    return 1;
}
int g_IsAttackAction(lua_State* L) {
    int s;
    FxAction a;
    lua_pushboolean(L, actionAt(L, 1, s, a) && a.type == 1 && a.id == 6603);
    return 1;
}
bool spellHasRange(FxGameApi* api, uint32_t id, FxSpellInfo& si) {
    return api->spellInfo(id, si) && si.maxRange > 0.0f && !(si.minRange == 0.0f && si.maxRange <= 0.1f);
}
int g_ActionHasRange(lua_State* L) {
    int s;
    FxAction a;
    FxSpellInfo si;
    lua_pushboolean(L, actionAt(L, 1, s, a) && a.type == 1 && spellHasRange(A(L), a.id, si));
    return 1;
}
// 1 in range, 0 out, nil without a target / range (upstream pushSpellRangeAnswer).
int rangeAnswer(lua_State* L, uint32_t spellId, const char* unit) {
    FxGameApi* api = A(L);
    FxSpellInfo si;
    if (!spellHasRange(api, spellId, si)) return retNil(L);
    char u[32];
    lowerTok(unit ? unit : "target", u, sizeof(u));
    const float d = api->unitDistance(u);
    if (d < 0.0f) return retNil(L);
    lua_pushnumber(L, (d <= si.maxRange && d >= si.minRange) ? 1 : 0);
    return 1;
}
int g_IsActionInRange(lua_State* L) {
    int s;
    FxAction a;
    if (!actionAt(L, 1, s, a) || a.type != 1) return retNil(L);
    return rangeAnswer(L, a.id, luaL_optstring(L, 2, "target"));
}
int g_UseAction(lua_State* L) {
    FxUi* ui = U(L);
    int s;
    FxAction a;
    if (!actionAt(L, 1, s, a)) return 0;
    const char* unit = lua_isstring(L, 2) ? lua_tostring(L, 2) : nullptr;
    if (++ui->actionLogs_ <= 60)
        ui->logf("fx_action slot=%d type=%u id=%u unit=%s", s + 1, static_cast<unsigned>(a.type),
                 static_cast<unsigned>(a.id), unit ? unit : "-");
    char u[32];
    lowerTok(unit, u, sizeof(u));
    A(L)->useAction(s, unit ? u : nullptr);
    return 0;
}
int g_GetBonusBarOffset(lua_State* L) {
    lua_pushnumber(L, A(L)->bonusBarOffset());
    return 1;
}
int g_GetActionBarPage(lua_State* L) {
    lua_pushnumber(L, 1);  // PSP: one page (L + D-pad down pages the built-in bar only)
    return 1;
}

// ---------------------------------------------------------------- spells
// A spell argument: (slot, "spell"|"pet") = spell book slot; a number alone
// = spell id; a string = the highest known rank of that name.
uint32_t spellArg(lua_State* L, int idx) {
    FxUi* ui = U(L);
    ui->refreshSpellBook();
    if (lua_type(L, idx) == LUA_TNUMBER) {
        const int n = static_cast<int>(lua_tonumber(L, idx));
        if (lua_type(L, idx + 1) == LUA_TSTRING) {
            // PSP: no pet book yet - "pet" slots answer nothing.
            if (std::strcmp(lua_tostring(L, idx + 1), "pet") == 0) return 0;
            return n >= 1 && static_cast<size_t>(n) <= ui->bookSpells_.size() ? ui->bookSpells_[static_cast<size_t>(n - 1)] : 0;
        }
        return n > 0 ? static_cast<uint32_t>(n) : 0;
    }
    if (lua_type(L, idx) == LUA_TSTRING) {
        const char* want = lua_tostring(L, idx);
        FxSpellInfo si;
        for (uint32_t id : ui->bookSpells_)
            if (ui->api()->spellInfo(id, si) && std::strcmp(si.name, want) == 0) return id;
    }
    return 0;
}
int g_GetNumSpellTabs(lua_State* L) {
    FxUi* ui = U(L);
    ui->refreshSpellBook();
    lua_pushnumber(L, static_cast<lua_Number>(ui->bookTabs_.size()));
    return 1;
}
// name, texture, offset, numSpells, highestRankOffset, highestRankNumSpells -
// six values always (upstream: SpellBookFrame's OnLoad does arithmetic on them).
int g_GetSpellTabInfo(lua_State* L) {
    FxUi* ui = U(L);
    ui->refreshSpellBook();
    const int t = static_cast<int>(luaL_checknumber(L, 1));
    if (t < 1 || static_cast<size_t>(t) > ui->bookTabs_.size()) {
        lua_pushstring(L, "");
        lua_pushstring(L, "");
        for (int k = 0; k < 4; ++k) lua_pushnumber(L, 0);
        return 6;
    }
    const FxSpellTab& tab = ui->bookTabs_[static_cast<size_t>(t - 1)];
    lua_pushstring(L, tab.name);
    if (tab.iconSpell) pushSpellIcon(L, tab.iconSpell);
    else lua_pushstring(L, "Interface\\Icons\\INV_Misc_Book_09");
    lua_pushnumber(L, static_cast<lua_Number>(tab.offset));
    lua_pushnumber(L, static_cast<lua_Number>(tab.count));
    lua_pushnumber(L, static_cast<lua_Number>(tab.offset));
    lua_pushnumber(L, static_cast<lua_Number>(tab.count));
    return 6;
}
int g_GetSpellName(lua_State* L) {  // also GetSpellBookItemName
    const uint32_t id = spellArg(L, 1);
    FxSpellInfo si;
    if (!id || !A(L)->spellInfo(id, si)) return retNil(L);
    lua_pushstring(L, si.name[0] ? si.name : "Unknown");
    lua_pushstring(L, si.rank);  // PSP: the rank from spells.pak (upstream: "")
    return 2;
}
int g_GetSpellBookItemInfo(lua_State* L) {
    lua_pushstring(L, "SPELL");
    lua_pushnumber(L, static_cast<lua_Number>(spellArg(L, 1)));
    return 2;
}
int g_GetSpellTexture(lua_State* L) {
    const uint32_t id = spellArg(L, 1);
    if (!id) return retNil(L);
    pushSpellIcon(L, id);
    return 1;
}
int g_GetSpellCooldown(lua_State* L) {
    const uint32_t id = spellArg(L, 1);
    return pushCooldown(L, id, id != 0);
}
int g_CastSpell(lua_State* L) {  // (slot, book) - the book's click
    const uint32_t id = spellArg(L, 1);
    if (id) A(L)->castSpell(id, nullptr);
    return 0;
}
int g_CastSpellByName(lua_State* L) {
    const uint32_t id = spellArg(L, 1);
    const char* unit = lua_isstring(L, 2) ? lua_tostring(L, 2) : nullptr;
    char u[32];
    lowerTok(unit, u, sizeof(u));
    if (id) A(L)->castSpell(id, unit ? u : nullptr);
    return 0;
}
int g_IsPassiveSpell(lua_State* L) {
    const uint32_t id = spellArg(L, 1);
    FxSpellInfo si;
    lua_pushboolean(L, id && A(L)->spellInfo(id, si) && si.passive);
    return 1;
}
int g_IsSpellKnown(lua_State* L) {
    lua_pushboolean(L, A(L)->spellKnown(static_cast<uint32_t>(luaL_checknumber(L, 1))));
    return 1;
}
int g_IsCurrentSpell(lua_State* L) {
    const uint32_t id = spellArg(L, 1);
    lua_pushboolean(L, id && A(L)->isCurrentSpell(id));
    return 1;
}
int g_IsSpellInRange(lua_State* L) {
    const uint32_t id = spellArg(L, 1);
    if (!id) return retNil(L);
    const int unitIdx = lua_type(L, 2) == LUA_TSTRING && (std::strcmp(lua_tostring(L, 2), "spell") == 0 ||
                                                          std::strcmp(lua_tostring(L, 2), "pet") == 0) ? 3 : 2;
    return rangeAnswer(L, id, luaL_optstring(L, unitIdx, "target"));
}
// name, rank, icon, powerCost, isFunnel, powerType, castTime, minRange, maxRange
int g_GetSpellInfo(lua_State* L) {
    uint32_t id = lua_type(L, 1) == LUA_TNUMBER ? static_cast<uint32_t>(lua_tonumber(L, 1)) : 0;
    if (!id) id = spellArg(L, 1);
    FxSpellInfo si;
    if (!id || !A(L)->spellInfo(id, si)) return retNil(L);
    lua_pushstring(L, si.name);
    lua_pushstring(L, si.rank);
    pushSpellIcon(L, id);
    lua_pushnumber(L, static_cast<lua_Number>(si.powerCost));
    lua_pushboolean(L, 0);
    lua_pushnumber(L, si.powerType);
    lua_pushnumber(L, static_cast<lua_Number>(si.castMs));
    lua_pushnumber(L, si.minRange);
    lua_pushnumber(L, si.maxRange);
    return 9;
}
int g_GetSpellLink(lua_State* L) {
    const uint32_t id = spellArg(L, 1);
    FxSpellInfo si;
    if (!id || !A(L)->spellInfo(id, si)) return retNil(L);
    lua_pushfstring(L, "|cff71d5ff|Hspell:%d|h[%s]|h|r", static_cast<int>(id), si.name);
    return 1;
}

// ---------------------------------------------------------------- auras
constexpr size_t kMaxAuras = 64;
// Upstream lua_UnitAura: the index counts buffs or debuffs; 11 values.
int unitAura(lua_State* L, bool wantBuff) {
    FxGameApi* api = A(L);
    char u[32];
    lowerTok(luaL_optstring(L, 1, "player"), u, sizeof(u));
    FxAura list[kMaxAuras];
    const size_t n = api->auras(u, list, kMaxAuras);
    const bool byName = lua_type(L, 2) == LUA_TSTRING;
    const int index = byName ? 0 : static_cast<int>(luaL_optnumber(L, 2, 1));
    const char* want = byName ? lua_tostring(L, 2) : nullptr;
    if (!byName && index < 1) return retNil(L);
    int found = 0;
    FxSpellInfo si;
    for (size_t k = 0; k < n; ++k) {
        const FxAura& a = list[k];
        if (!a.spellId) continue;
        const bool debuff = (a.flags & 0x80) != 0;
        if (wantBuff == debuff) continue;
        const bool haveInfo = api->spellInfo(a.spellId, si);
        if (byName) {
            if (!haveInfo || std::strcmp(si.name, want) != 0) continue;
        } else if (++found != index) {
            continue;
        }
        lua_pushstring(L, haveInfo && si.name[0] ? si.name : "Unknown");
        lua_pushstring(L, haveInfo ? si.rank : "");
        pushSpellIcon(L, a.spellId);
        lua_pushnumber(L, a.charges);
        if (const char* d = haveInfo ? dispelName(si.dispel) : nullptr) lua_pushstring(L, d);
        else lua_pushnil(L);
        lua_pushnumber(L, a.durationMs > 0 ? static_cast<float>(a.durationMs) / 1000.0f : 0.0f);
        if (a.remainingMs >= 0 && a.durationMs > 0)
            lua_pushnumber(L, static_cast<float>(api->timeSec()) + static_cast<float>(a.remainingMs) / 1000.0f);
        else
            lua_pushnumber(L, 0);
        FxUnit p, t;
        if (!a.caster) lua_pushnil(L);
        else if (api->unit("player", p) && p.guid == a.caster) lua_pushstring(L, "player");
        else if (api->unit("target", t) && t.guid == a.caster) lua_pushstring(L, "target");
        else {
            char g[24];  // (lua_pushfstring has no %X)
            std::snprintf(g, sizeof(g), "0x%016llX", static_cast<unsigned long long>(a.caster));
            lua_pushstring(L, g);
        }
        lua_pushboolean(L, 0);
        lua_pushboolean(L, 0);
        lua_pushnumber(L, static_cast<lua_Number>(a.spellId));
        return 11;
    }
    return retNil(L);
}
int g_UnitBuff(lua_State* L) { return unitAura(L, true); }
int g_UnitDebuff(lua_State* L) { return unitAura(L, false); }
int g_UnitAura(lua_State* L) {
    const char* f = luaL_optstring(L, 3, "HELPFUL");
    char up[32];
    size_t k = 0;
    for (; f[k] && k < sizeof(up) - 1; ++k) up[k] = static_cast<char>(std::toupper(static_cast<unsigned char>(f[k])));
    up[k] = 0;
    return unitAura(L, std::strstr(up, "HARMFUL") == nullptr);
}
// CancelUnitBuff("player", index|name[, filter]) -> CMSG_CANCEL_AURA.
int g_CancelUnitBuff(lua_State* L) {
    char u[32];
    lowerTok(luaL_optstring(L, 1, "player"), u, sizeof(u));
    if (std::strcmp(u, "player") != 0) return 0;
    const int top = lua_gettop(L);
    lua_settop(L, 3);
    const int r = g_UnitAura(L);
    if (r == 11) {
        const uint32_t id = static_cast<uint32_t>(lua_tonumber(L, -1));
        if (id) A(L)->cancelAura(id);
    }
    lua_settop(L, top);
    return 0;
}

// ---------------------------------------------------------------- cast
// Upstream lua_UnitCastInfo: 9 values (casting) / 8 (channel), times in ms.
int unitCastInfo(lua_State* L, bool wantChannel) {
    FxGameApi* api = A(L);
    char u[32];
    lowerTok(luaL_optstring(L, 1, "player"), u, sizeof(u));
    FxCast c;
    if (!api->unitCast(u, c) || !c.casting || c.channel != wantChannel) return retNil(L);
    FxSpellInfo si;
    const bool haveInfo = api->spellInfo(c.spellId, si);
    const float now = static_cast<float>(api->timeSec());
    lua_pushstring(L, haveInfo && si.name[0] ? si.name : "Unknown");
    lua_pushstring(L, haveInfo ? si.rank : "");
    lua_pushstring(L, haveInfo && si.name[0] ? si.name : "Unknown");
    pushSpellIcon(L, c.spellId);
    lua_pushnumber(L, (now - (c.total - c.remaining)) * 1000.0f);
    lua_pushnumber(L, (now + c.remaining) * 1000.0f);
    lua_pushboolean(L, 0);  // PSP: no trade skill casts yet
    if (!wantChannel) lua_pushnumber(L, static_cast<lua_Number>(c.spellId));  // castID = spell id (upstream)
    lua_pushboolean(L, c.interruptible ? 0 : 1);
    return wantChannel ? 8 : 9;
}
int g_UnitCastingInfo(lua_State* L) { return unitCastInfo(L, false); }
int g_UnitChannelInfo(lua_State* L) { return unitCastInfo(L, true); }

// ---------------------------------------------------------------- bags
int g_GetContainerNumSlots(lua_State* L) {
    lua_pushnumber(L, A(L)->containerSlots(static_cast<int>(luaL_checknumber(L, 1))));
    return 1;
}
bool cursorAt(FxUi* ui, int bag, int slot) { return ui->cursor_.bag == bag && ui->cursor_.slot == slot; }
// texture, itemCount, locked, quality, readable, lootable, link (upstream order)
int g_GetContainerItemInfo(lua_State* L) {
    const int bag = static_cast<int>(luaL_checknumber(L, 1)), slot = static_cast<int>(luaL_checknumber(L, 2));
    FxItem it;
    if (!A(L)->containerItem(bag, slot, it) || !it.entry) return retNil(L);
    pushItemIcon(L, it.entry);
    lua_pushnumber(L, static_cast<lua_Number>(it.count ? it.count : 1));
    lua_pushboolean(L, cursorAt(U(L), bag, slot));
    lua_pushnumber(L, it.quality);
    lua_pushboolean(L, 0);
    lua_pushboolean(L, 0);
    pushItemLink(L, it);
    return 7;
}
int g_GetContainerItemLink(lua_State* L) {
    FxItem it;
    if (!A(L)->containerItem(static_cast<int>(luaL_checknumber(L, 1)), static_cast<int>(luaL_checknumber(L, 2)), it) ||
        !it.entry)
        return retNil(L);
    pushItemLink(L, it);
    return 1;
}
int g_GetContainerItemID(lua_State* L) {
    FxItem it;
    if (!A(L)->containerItem(static_cast<int>(luaL_checknumber(L, 1)), static_cast<int>(luaL_checknumber(L, 2)), it) ||
        !it.entry)
        return retNil(L);
    lua_pushnumber(L, static_cast<lua_Number>(it.entry));
    return 1;
}
int g_GetContainerNumFreeSlots(lua_State* L) {
    FxGameApi* api = A(L);
    const int bag = static_cast<int>(luaL_checknumber(L, 1));
    const int n = api->containerSlots(bag);
    int free = 0;
    FxItem it;
    for (int s = 1; s <= n; ++s)
        if (!api->containerItem(bag, s, it) || !it.entry) ++free;
    lua_pushnumber(L, free);
    lua_pushnumber(L, 0);  // bag family
    return 2;
}
int g_GetBagName(lua_State* L) {
    const int bag = static_cast<int>(luaL_checknumber(L, 1));
    if (bag == 0) {
        // PSP: the client's BACKPACK_TOOLTIP - the GlobalStrings global of the language's framexml.pak
        // (the C function of the original returns the same text); "Backpack" when the bake filtered it out.
        lua_getglobal(L, "BACKPACK_TOOLTIP");
        if (!lua_isstring(L, -1)) {
            lua_pop(L, 1);
            lua_pushstring(L, "Backpack");
        }
        return 1;
    }
    FxItem it;
    if (bag < 1 || bag > 4 || !A(L)->inventoryItem(19 + bag, it) || !it.entry) return retNil(L);
    lua_pushstring(L, it.name);
    return 1;
}
void fireLock(FxUi* ui, int bag, int slot) {
    FxEventArg a[2];
    a[0].kind = 1;
    a[0].num = static_cast<float>(bag);
    a[1].kind = slot ? 1 : 0;
    a[1].num = static_cast<float>(slot);
    if (bag >= 0) ui->fireEventArgs("ITEM_LOCK_CHANGED", a, 2);
    else {
        a[0].num = static_cast<float>(slot);  // an inventory slot: one argument
        ui->fireEventArgs("ITEM_LOCK_CHANGED", a, 1);
    }
    ui->fireEventArgs("CURSOR_UPDATE", nullptr, 0);
}
void clearCursor(FxUi* ui) {
    const FxUi::Cursor c = ui->cursor_;
    ui->cursor_ = FxUi::Cursor{};
    if (c.bag != -2) fireLock(ui, c.bag, c.slot);
}
// The item cursor (upstream s_cursor*): the first pickup takes the item,
// the second puts it there (CMSG_SWAP_ITEM / autoequip in the adapter).
void pickup(lua_State* L, int bag, int slot) {
    FxUi* ui = U(L);
    FxGameApi* api = ui->api();
    if (ui->cursor_.bag == -2) {
        FxItem it;
        const bool have = bag >= 0 ? api->containerItem(bag, slot, it) : api->inventoryItem(slot, it);
        if (!have || !it.entry) return;
        ui->cursor_.bag = bag;
        ui->cursor_.slot = slot;
        fireLock(ui, bag, slot);
        return;
    }
    const FxUi::Cursor c = ui->cursor_;
    clearCursor(ui);
    if (c.bag == bag && c.slot == slot) return;
    if (++ui->actionLogs_ <= 60)
        ui->logf("fx_action item_move src=%d:%d dst=%d:%d", c.bag, c.slot, bag, slot);
    api->moveItem(c.bag, c.slot, bag, slot);
}
int g_PickupContainerItem(lua_State* L) {
    pickup(L, static_cast<int>(luaL_checknumber(L, 1)), static_cast<int>(luaL_checknumber(L, 2)));
    return 0;
}
int g_PickupInventoryItem(lua_State* L) {
    pickup(L, -1, static_cast<int>(luaL_checknumber(L, 1)));
    return 0;
}
int g_UseContainerItem(lua_State* L) {
    FxUi* ui = U(L);
    const int bag = static_cast<int>(luaL_checknumber(L, 1)), slot = static_cast<int>(luaL_checknumber(L, 2));
    if (ui->cursor_.bag != -2) clearCursor(ui);
    if (++ui->actionLogs_ <= 60) ui->logf("fx_action use_item bag=%d slot=%d", bag, slot);
    ui->api()->useContainerItem(bag, slot);
    return 0;
}
int g_CursorHasItem(lua_State* L) {
    lua_pushboolean(L, U(L)->cursor_.bag != -2);
    return 1;
}
int g_ClearCursor(lua_State* L) {
    clearCursor(U(L));
    return 0;
}
int g_GetCursorInfo(lua_State* L) {
    FxUi* ui = U(L);
    if (ui->cursor_.bag == -2) return 0;
    FxItem it;
    const bool have = ui->cursor_.bag >= 0 ? ui->api()->containerItem(ui->cursor_.bag, ui->cursor_.slot, it)
                                           : ui->api()->inventoryItem(ui->cursor_.slot, it);
    if (!have) return 0;
    lua_pushstring(L, "item");
    lua_pushnumber(L, static_cast<lua_Number>(it.entry));
    pushItemLink(L, it);
    return 3;
}
int g_PutItemInBackpack(lua_State* L) {
    FxUi* ui = U(L);
    if (ui->cursor_.bag == -2) return 0;
    const FxUi::Cursor c = ui->cursor_;
    clearCursor(ui);
    ui->api()->moveItem(c.bag, c.slot, 0, 0);
    return 0;
}
int g_PutItemInBag(lua_State* L) {  // (inventory slot 20..23)
    FxUi* ui = U(L);
    const int inv = static_cast<int>(luaL_checknumber(L, 1));
    if (ui->cursor_.bag == -2 || inv < 20 || inv > 23) return 0;
    const FxUi::Cursor c = ui->cursor_;
    clearCursor(ui);
    ui->api()->moveItem(c.bag, c.slot, inv - 19, 0);
    return 0;
}
int g_ContainerIDToInventoryID(lua_State* L) {
    lua_pushnumber(L, 19 + static_cast<int>(luaL_checknumber(L, 1)));
    return 1;
}
// Upstream lua_GetInventorySlotInfo table (1-based client inventory slots).
int g_GetInventorySlotInfo(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    char up[40];
    size_t k = 0;
    for (; name[k] && k < sizeof(up) - 1; ++k) up[k] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[k])));
    up[k] = 0;
    if (k > 4 && std::strcmp(up + k - 4, "SLOT") == 0) up[k - 4] = 0;
    struct M {
        const char* n;
        int id;
        const char* tex;
    };
    static const M kMap[] = {
        {"HEAD", 1, "Head"},       {"NECK", 2, "Neck"},          {"SHOULDER", 3, "Shoulder"},
        {"SHIRT", 4, "Shirt"},     {"CHEST", 5, "Chest"},        {"WAIST", 6, "Waist"},
        {"LEGS", 7, "Legs"},       {"FEET", 8, "Feet"},          {"WRIST", 9, "Wrists"},
        {"HANDS", 10, "Hands"},    {"FINGER0", 11, "Finger"},    {"FINGER1", 12, "Finger"},
        {"TRINKET0", 13, "Trinket"}, {"TRINKET1", 14, "Trinket"}, {"BACK", 15, "Chest"},
        {"MAINHAND", 16, "MainHand"}, {"SECONDARYHAND", 17, "SecondaryHand"}, {"RANGED", 18, "Ranged"},
        {"TABARD", 19, "Tabard"},  {"BAG0", 20, "Bag"},          {"BAG1", 21, "Bag"},
        {"BAG2", 22, "Bag"},       {"BAG3", 23, "Bag"},          {"AMMO", 0, "Ammo"},
    };
    for (const M& m : kMap) {
        if (std::strcmp(up, m.n) != 0) continue;
        lua_pushnumber(L, m.id);
        lua_pushfstring(L, "Interface\\PaperDoll\\UI-PaperDoll-Slot-%s", m.tex);
        lua_pushboolean(L, m.id == 18);
        return 3;
    }
    return retNil(L);
}
bool invArg(lua_State* L, FxItem& it) {  // (unit, slot)
    char u[32];
    lowerTok(luaL_optstring(L, 1, "player"), u, sizeof(u));
    if (std::strcmp(u, "player") != 0) return false;  // PSP: no inspect yet
    return A(L)->inventoryItem(static_cast<int>(luaL_checknumber(L, 2)), it) && it.entry;
}
int g_GetInventoryItemTexture(lua_State* L) {
    FxItem it;
    if (!invArg(L, it)) return retNil(L);
    pushItemIcon(L, it.entry);
    return 1;
}
int g_GetInventoryItemLink(lua_State* L) {
    FxItem it;
    if (!invArg(L, it)) return retNil(L);
    pushItemLink(L, it);
    return 1;
}
int g_GetInventoryItemID(lua_State* L) {
    FxItem it;
    if (!invArg(L, it)) return retNil(L);
    lua_pushnumber(L, static_cast<lua_Number>(it.entry));
    return 1;
}
int g_GetInventoryItemCount(lua_State* L) {
    FxItem it;
    lua_pushnumber(L, invArg(L, it) ? static_cast<lua_Number>(it.count ? it.count : 1) : 0);
    return 1;
}
int g_GetInventoryItemQuality(lua_State* L) {
    FxItem it;
    if (!invArg(L, it)) return retNil(L);
    lua_pushnumber(L, it.quality);
    return 1;
}
int g_GetInventoryItemBroken(lua_State* L) {
    FxItem it;
    lua_pushboolean(L, invArg(L, it) && it.maxDurability > 0 && it.durability == 0);
    return 1;
}
int g_GetInventoryItemDurability(lua_State* L) {
    FxItem it;
    if (!A(L)->inventoryItem(static_cast<int>(luaL_checknumber(L, 1)), it) || !it.entry || !it.maxDurability) return retNil(L);
    lua_pushnumber(L, static_cast<lua_Number>(it.durability));
    lua_pushnumber(L, static_cast<lua_Number>(it.maxDurability));
    return 2;
}
bool itemArg(lua_State* L, int idx, FxItem& it) {
    uint32_t entry = 0;
    if (lua_type(L, idx) == LUA_TNUMBER) {
        entry = static_cast<uint32_t>(lua_tonumber(L, idx));
    } else if (lua_type(L, idx) == LUA_TSTRING) {  // "item:123..." inside a link
        const char* s = std::strstr(lua_tostring(L, idx), "item:");
        if (s) entry = static_cast<uint32_t>(std::strtoul(s + 5, nullptr, 10));
    }
    return entry && A(L)->itemInfo(entry, it) && it.entry;
}
// name, link, quality, iLevel, reqLevel, class, subclass, maxStack, equipLoc, texture, sellPrice
int g_GetItemInfo(lua_State* L) {
    FxItem it;
    if (!itemArg(L, 1, it)) return retNil(L);
    static const char* const kEquipLoc[29] = {
        "", "INVTYPE_HEAD", "INVTYPE_NECK", "INVTYPE_SHOULDER", "INVTYPE_BODY", "INVTYPE_CHEST", "INVTYPE_WAIST",
        "INVTYPE_LEGS", "INVTYPE_FEET", "INVTYPE_WRIST", "INVTYPE_HAND", "INVTYPE_FINGER", "INVTYPE_TRINKET",
        "INVTYPE_WEAPON", "INVTYPE_SHIELD", "INVTYPE_RANGED", "INVTYPE_CLOAK", "INVTYPE_2HWEAPON", "INVTYPE_BAG",
        "INVTYPE_TABARD", "INVTYPE_ROBE", "INVTYPE_WEAPONMAINHAND", "INVTYPE_WEAPONOFFHAND", "INVTYPE_HOLDABLE",
        "INVTYPE_AMMO", "INVTYPE_THROWN", "INVTYPE_RANGEDRIGHT", "INVTYPE_QUIVER", "INVTYPE_RELIC"};
    lua_pushstring(L, it.name);
    pushItemLink(L, it);
    lua_pushnumber(L, it.quality);
    lua_pushnumber(L, static_cast<lua_Number>(it.itemLevel));
    lua_pushnumber(L, static_cast<lua_Number>(it.reqLevel));
    lua_pushstring(L, "");  // PSP: class / subclass names need ItemClass.dbc (not baked)
    lua_pushstring(L, "");
    lua_pushnumber(L, static_cast<lua_Number>(it.maxStack));
    lua_pushstring(L, it.invType < 29 ? kEquipLoc[it.invType] : "");
    pushItemIcon(L, it.entry);
    lua_pushnumber(L, static_cast<lua_Number>(it.sellPrice));
    return 11;
}
int g_GetItemCount(lua_State* L) {
    FxItem it;
    lua_pushnumber(L, itemArg(L, 1, it) ? static_cast<lua_Number>(A(L)->itemCount(it.entry)) : 0);
    return 1;
}
int g_GetItemQualityColor(lua_State* L) {
    const int q = static_cast<int>(luaL_optnumber(L, 1, 1)) & 7;
    const uint32_t c = kQualityRgb[q];
    lua_pushnumber(L, static_cast<float>((c >> 16) & 0xFF) / 255.0f);
    lua_pushnumber(L, static_cast<float>((c >> 8) & 0xFF) / 255.0f);
    lua_pushnumber(L, static_cast<float>(c & 0xFF) / 255.0f);
    char hex[16];
    std::snprintf(hex, sizeof(hex), "|cff%06x", static_cast<unsigned>(c));
    lua_pushstring(L, hex);
    return 4;
}

// ---------------------------------------------------------------- character
// UnitStat(unit, i) -> base, stat, posBuff, negBuff (own fields only).
int g_UnitStat(lua_State* L) {
    const int i = static_cast<int>(luaL_checknumber(L, 2));
    const int32_t v = i >= 1 && i <= 5 ? A(L)->unitField(kFxStat0 + i - 1) : 0;
    lua_pushnumber(L, static_cast<lua_Number>(v));
    lua_pushnumber(L, static_cast<lua_Number>(v));
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 0);
    return 4;
}
// UnitArmor -> base, effective, armor, posBuff, negBuff
int g_UnitArmor(lua_State* L) {
    const int32_t v = A(L)->unitField(kFxResist0);
    for (int k = 0; k < 3; ++k) lua_pushnumber(L, static_cast<lua_Number>(v));
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 0);
    return 5;
}
// UnitResistance(unit, school) -> base, total, bonus, minus
int g_UnitResistance(lua_State* L) {
    const int sc = static_cast<int>(luaL_optnumber(L, 2, 0));
    const int32_t v = sc >= 0 && sc <= 6 ? A(L)->unitField(kFxResist0 + sc) : 0;
    lua_pushnumber(L, static_cast<lua_Number>(v));
    lua_pushnumber(L, static_cast<lua_Number>(v));
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 0);
    return 4;
}
int g_UnitAttackPower(lua_State* L) {  // base, posBuff, negBuff
    lua_pushnumber(L, static_cast<lua_Number>(A(L)->unitField(kFxAttackPower)));
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 0);
    return 3;
}
int g_UnitRangedAttackPower(lua_State* L) {
    lua_pushnumber(L, static_cast<lua_Number>(A(L)->unitField(kFxRangedAttackPower)));
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 0);
    return 3;
}
float fieldFloat(lua_State* L, int f) {
    const int32_t b = A(L)->unitField(f);
    float v;
    std::memcpy(&v, &b, 4);
    return ((b & 0x7F800000) == 0x7F800000) ? 0.0f : v;  // finite by the bits (Allegrex FPU)
}
// UnitDamage -> minDamage, maxDamage, minOff, maxOff, physPos, physNeg, percent
int g_UnitDamage(lua_State* L) {
    lua_pushnumber(L, fieldFloat(L, kFxMinDamage));
    lua_pushnumber(L, fieldFloat(L, kFxMaxDamage));
    for (int k = 0; k < 4; ++k) lua_pushnumber(L, 0);
    lua_pushnumber(L, 1);
    return 7;
}
int g_UnitAttackSpeed(lua_State* L) {  // PSP: not in the PSP's field table yet
    lua_pushnumber(L, 2);
    lua_pushnumber(L, 0);
    return 2;
}
int g_zeros4(lua_State* L) {  // stubbed stat getters that answer numbers (arithmetic-safe)
    for (int k = 0; k < 4; ++k) lua_pushnumber(L, 0);
    return 4;
}

// ---------------------------------------------------------------- sound
// Upstream lua_PlaySound: a SoundEntries name (a number = a row id).
int g_PlaySound(lua_State* L) {
    FxUi* ui = U(L);
    bool ok = false;
    const char* name = "?";
    if (lua_type(L, 1) == LUA_TNUMBER) {
        ok = ui->api()->playSoundId(static_cast<uint32_t>(lua_tonumber(L, 1)));
        name = "id";
    } else if (lua_type(L, 1) == LUA_TSTRING) {
        name = lua_tostring(L, 1);
        ok = ui->api()->playSound(name);
    }
    if (!ok && ++ui->soundLogs_ <= 30) ui->logf("fx_sound_missing name=%.60s", name);
    return 0;
}

// Cooldown model: SetCooldown(start, duration) (PSP: minV/maxV of the record).
int m_SetCooldown(lua_State* L) {
    FxUi* ui = U(L);
    const uint32_t i = ui->toObject(1);
    if (i == FxUi::kNone) return 0;
    ui->obj(i).minV = static_cast<float>(luaL_optnumber(L, 2, 0));
    ui->obj(i).maxV = static_cast<float>(luaL_optnumber(L, 3, 0));
    ui->markDraw();
    return 0;
}

// Fallbacks for what UIParent.lua / ItemButtonTemplate.lua / Constants.lua
// define: a baked file of FrameXML replaces them (it runs later).
const char kGamePrelude[] = R"LUA(
BOOKTYPE_SPELL = "spell"; BOOKTYPE_PET = "pet"
NUM_BAG_SLOTS = 4; BACKPACK_CONTAINER = 0; NUM_BAG_FRAMES = 4
function ShowUIPanel(f) if f then f:Show() end end
function HideUIPanel(f) if f then f:Hide() end end
function ToggleFrame(f) if f:IsShown() then HideUIPanel(f) else ShowUIPanel(f) end end
function CooldownFrame_SetTimer(self, start, duration, enable)
  if start > 0 and duration > 0 and enable > 0 then self:SetCooldown(start, duration) self:Show()
  else self:Hide() end
end
function SetItemButtonTexture(b, t) local i = _G[b:GetName().."IconTexture"] if i then i:SetTexture(t) end end
function SetItemButtonCount(b, n)
  local c = _G[b:GetName().."Count"] if not c then return end
  if n and n > 1 then c:SetText(n) c:Show() else c:Hide() end
end
function SetItemButtonDesaturated(b, d) end
function PlaySoundFile() end
-- The world's Menu / window bar opening a FrameXML panel (upstream's
-- toggle when baked, else the frame itself).
function __fx_toggle(fn, arg, frame)
  local f = rawget(_G, fn)
  if type(f) == "function" then f(arg) return end
  local fr = _G[frame]
  if fr then if fr:IsShown() then HideUIPanel(fr) else ShowUIPanel(fr) end end
end
)LUA";

}  // namespace

void FxUi::installGameApi() {
    lua_State* L = L_;
    static const luaL_Reg kFns[] = {
        {"HasAction", g_HasAction}, {"GetActionTexture", g_GetActionTexture}, {"GetActionInfo", g_GetActionInfo},
        {"GetActionCount", g_GetActionCount}, {"GetActionCooldown", g_GetActionCooldown},
        {"IsUsableAction", g_IsUsableAction}, {"IsCurrentAction", g_IsCurrentAction},
        {"IsAttackAction", g_IsAttackAction}, {"IsAutoRepeatAction", retFalse}, {"IsConsumableAction", retFalse},
        {"IsEquippedAction", retFalse}, {"IsStackableAction", retFalse}, {"ActionHasRange", g_ActionHasRange},
        {"IsActionInRange", g_IsActionInRange}, {"GetActionText", retNil}, {"UseAction", g_UseAction},
        {"GetBonusBarOffset", g_GetBonusBarOffset}, {"GetActionBarPage", g_GetActionBarPage},
        {"GetNumSpellTabs", g_GetNumSpellTabs}, {"GetSpellTabInfo", g_GetSpellTabInfo},
        {"GetSpellName", g_GetSpellName}, {"GetSpellBookItemName", g_GetSpellName},
        {"GetSpellBookItemInfo", g_GetSpellBookItemInfo}, {"GetSpellTexture", g_GetSpellTexture},
        {"GetSpellCooldown", g_GetSpellCooldown}, {"CastSpell", g_CastSpell}, {"CastSpellByName", g_CastSpellByName},
        {"CastSpellByID", g_CastSpellByName}, {"IsPassiveSpell", g_IsPassiveSpell}, {"IsSpellKnown", g_IsSpellKnown},
        {"IsCurrentSpell", g_IsCurrentSpell}, {"IsSelectedSpell", retFalse}, {"SpellIsTargeting", retNil},
        {"IsSpellInRange", g_IsSpellInRange}, {"GetSpellInfo", g_GetSpellInfo}, {"GetSpellLink", g_GetSpellLink},
        {"UnitBuff", g_UnitBuff}, {"UnitDebuff", g_UnitDebuff}, {"UnitAura", g_UnitAura},
        {"CancelUnitBuff", g_CancelUnitBuff}, {"UnitCastingInfo", g_UnitCastingInfo},
        {"UnitChannelInfo", g_UnitChannelInfo}, {"GetContainerNumSlots", g_GetContainerNumSlots},
        {"GetContainerItemInfo", g_GetContainerItemInfo}, {"GetContainerItemLink", g_GetContainerItemLink},
        {"GetContainerItemID", g_GetContainerItemID}, {"GetContainerNumFreeSlots", g_GetContainerNumFreeSlots},
        {"GetBagName", g_GetBagName}, {"PickupContainerItem", g_PickupContainerItem},
        {"PickupInventoryItem", g_PickupInventoryItem}, {"UseContainerItem", g_UseContainerItem},
        {"CursorHasItem", g_CursorHasItem}, {"ClearCursor", g_ClearCursor}, {"GetCursorInfo", g_GetCursorInfo},
        {"PutItemInBackpack", g_PutItemInBackpack}, {"PutItemInBag", g_PutItemInBag},
        {"ContainerIDToInventoryID", g_ContainerIDToInventoryID}, {"GetInventorySlotInfo", g_GetInventorySlotInfo},
        {"GetInventoryItemTexture", g_GetInventoryItemTexture}, {"GetInventoryItemLink", g_GetInventoryItemLink},
        {"GetInventoryItemID", g_GetInventoryItemID}, {"GetInventoryItemCount", g_GetInventoryItemCount},
        {"GetInventoryItemQuality", g_GetInventoryItemQuality}, {"GetInventoryItemBroken", g_GetInventoryItemBroken},
        {"GetInventoryItemDurability", g_GetInventoryItemDurability}, {"GetItemInfo", g_GetItemInfo},
        {"GetItemCount", g_GetItemCount}, {"GetItemQualityColor", g_GetItemQualityColor},
        {"UnitStat", g_UnitStat}, {"UnitArmor", g_UnitArmor}, {"UnitResistance", g_UnitResistance},
        {"UnitAttackPower", g_UnitAttackPower}, {"UnitRangedAttackPower", g_UnitRangedAttackPower},
        {"UnitDamage", g_UnitDamage}, {"UnitAttackSpeed", g_UnitAttackSpeed}, {"UnitRangedDamage", g_zeros4},
        {"UnitRangedAttack", g_zeros4}, {"UnitAttackBothHands", g_zeros4}, {"GetCritChance", retZero},
        {"GetRangedCritChance", retZero}, {"GetSpellCritChance", retZero}, {"GetDodgeChance", retZero},
        {"GetParryChance", retZero}, {"GetBlockChance", retZero}, {"GetSpellBonusDamage", retZero},
        {"GetSpellBonusHealing", retZero}, {"GetCombatRating", retZero}, {"GetCombatRatingBonus", retZero},
        {"GetManaRegen", g_zeros4}, {"GetExpertise", retZero}, {"GetArmorPenetration", retZero},
        {"PlaySound", g_PlaySound}, {nullptr, nullptr}};
    for (const luaL_Reg* r = kFns; r->name; ++r) {
        lua_pushcfunction(L, r->func);
        lua_setglobal(L, r->name);
    }
    // Cooldown:SetCooldown on the Cooldown type's method table.
    lua_rawgeti(L, LUA_REGISTRYINDEX, methodsRef_[kFxCooldown]);
    lua_getfield(L, -1, "__index");
    lua_pushcfunction(L, m_SetCooldown);
    lua_setfield(L, -2, "SetCooldown");
    lua_pop(L, 2);
    if (luaL_loadbuffer(L, kGamePrelude, sizeof(kGamePrelude) - 1, "=fx_game_prelude") != 0 ||
        lua_pcall(L, 0, 0, 0) != 0) {
        noteLuaError("game_prelude");
        lua_pop(L, 1);
    }
    // PSP: GameTooltip.xml is not baked by default - a stand-in whose every
    // method is a no-op keeps ActionButton / ContainerFrame OnEnter working
    // (a baked GameTooltip frame wins: the stand-in is not made then).
    bool haveTooltip = false;
    for (uint32_t n = 0; n < hdr_.nodeCount && !haveTooltip; ++n)
        haveTooltip = std::strcmp(string(nodes_[n].name), "GameTooltip") == 0;
    if (!haveTooltip)
        runString("GameTooltip = setmetatable({}, {__index = function() return function() end end})", "=fx_tooltip");
}

}  // namespace wowee::ui::fx
