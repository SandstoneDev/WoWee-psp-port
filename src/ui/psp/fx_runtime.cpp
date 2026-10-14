// FrameXML runtime (see include/ui/psp/fx_runtime.hpp).
//
// Layout rules, strata/level order and the script signatures follow upstream
// (src/ui/widget_tree.cpp, src/addons/lua_engine.cpp, framexml_emitter.cpp);
// the Lua API functions follow src/addons/lua_unit_api.cpp for the subset the
// unit frames call. PSP deviations are marked "PSP:".
#include "ui/psp/fx_runtime.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <utility>

namespace wowee::ui::fx {

namespace {

// Anchor point fractions, y up (upstream resolveAnchorPoint).
constexpr float kPtX[kPtCount] = {0.0f, 0.5f, 1.0f, 0.0f, 0.5f, 1.0f, 0.0f, 0.5f, 1.0f};
constexpr float kPtY[kPtCount] = {1.0f, 1.0f, 1.0f, 0.5f, 0.5f, 0.5f, 0.0f, 0.0f, 0.0f};
const char* const kPtNames[kPtCount] = {"TOPLEFT", "TOP", "TOPRIGHT", "LEFT", "CENTER", "RIGHT",
                                        "BOTTOMLEFT", "BOTTOM", "BOTTOMRIGHT"};
const char* const kStrataNames[kStrataCount] = {"WORLD", "BACKGROUND", "LOW", "MEDIUM", "HIGH", "DIALOG",
                                                "FULLSCREEN", "FULLSCREEN_DIALOG", "TOOLTIP"};
const char* const kLayerNames[kLayerCount] = {"BACKGROUND", "BORDER", "ARTWORK", "OVERLAY", "HIGHLIGHT"};
const char* const kTypeNames[kFxTypeCount] = {"Frame", "Button", "CheckButton", "StatusBar", "Slider",
                                              "EditBox", "ScrollFrame", "ScrollingMessageFrame", "Model",
                                              "Cooldown", "GameTooltip", "SimpleHTML", "Texture", "FontString"};
const char* const kScriptNames[kScrCount] = {
    "OnLoad", "OnEvent", "OnUpdate", "OnShow", "OnHide", "OnClick", "OnEnter", "OnLeave",
    "OnMouseDown", "OnMouseUp", "OnValueChanged", "OnSizeChanged", "OnDragStart", "OnDragStop",
    "OnReceiveDrag", "OnDoubleClick", "OnMouseWheel", "OnKeyDown", "OnKeyUp", "OnChar",
    "OnTextChanged", "OnEnterPressed", "OnEscapePressed", "OnEditFocusGained", "OnEditFocusLost",
    "OnTooltipSetUnit", "OnTooltipSetItem", "OnVerticalScroll", "OnScrollRangeChanged",
    "OnMinMaxChanged", "OnAttributeChanged", "OnHyperlinkClick", "OnFinished"};

// Registry key of the FxUi (light userdata) - one lookup per binding call.
char kSelfKey = 0;

bool isRegion(uint8_t t) { return t == kFxTexture || t == kFxFontString; }

int pointOf(const char* s) {
    if (!s) return kPtCenter;
    for (int i = 0; i < kPtCount; ++i)
        if (std::strcmp(s, kPtNames[i]) == 0) return i;
    char u[16];
    size_t n = 0;
    for (; s[n] && n < sizeof(u) - 1; ++n) u[n] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[n])));
    u[n] = 0;
    for (int i = 0; i < kPtCount; ++i)
        if (std::strcmp(u, kPtNames[i]) == 0) return i;
    return kPtCenter;
}

int typeOf(const char* s) {
    if (!s) return kFxFrame;
    for (int i = 0; i < kFxTypeCount; ++i)
        if (std::strcmp(s, kTypeNames[i]) == 0) return i;
    if (std::strcmp(s, "MessageFrame") == 0) return kFxMessageFrame;
    if (std::strcmp(s, "PlayerModel") == 0 || std::strcmp(s, "DressUpModel") == 0 ||
        std::strcmp(s, "TabardModel") == 0)
        return kFxModel;
    return kFxFrame;
}

uint32_t packColor(float r, float g, float b, float a) {
    auto q = [](float v) {
        v = v < 0 ? 0 : (v > 1 ? 1 : v);
        return static_cast<uint32_t>(v * 255.0f + 0.5f);
    };
    return q(r) | (q(g) << 8) | (q(b) << 16) | (q(a) << 24);
}

void unpackColor(uint32_t c, float out[4]) {
    for (int i = 0; i < 4; ++i) out[i] = static_cast<float>((c >> (8 * i)) & 0xFF) / 255.0f;
}

uint32_t mulAlpha(uint32_t c, float a) {
    const float ca = static_cast<float>(c >> 24) * a;
    const uint32_t na = static_cast<uint32_t>(ca < 0 ? 0 : (ca > 255 ? 255 : ca + 0.5f));
    return (c & 0x00FFFFFFu) | (na << 24);
}

FxUi* self(lua_State* L) {
    lua_pushlightuserdata(L, &kSelfKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* ui = static_cast<FxUi*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return ui;
}

// "$parentText" in "FooFrame" -> "FooFrameText" (upstream substituteParent).
std::string substituteParent(const char* raw, const std::string& owner, bool& unnamed) {
    unnamed = false;
    if (!raw || !*raw) return std::string();
    if (std::strlen(raw) >= 7) {
        bool match = true;
        const char* tok = "$parent";
        for (int i = 0; i < 7; ++i)
            if (std::tolower(static_cast<unsigned char>(raw[i])) != tok[i]) match = false;
        if (match) {
            if (owner.empty()) {
                unnamed = true;
                return std::string();
            }
            return owner + (raw + 7);
        }
    }
    return raw;
}

// |cAARRGGBB ... |r colour codes and |T..|t textures are dropped (PSP: no
// markup renderer yet; upstream: include/ui/text_markup.hpp).
void stripMarkup(std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '|' && i + 1 < s.size()) {
            const char c = s[i + 1];
            if (c == 'c' && i + 10 <= s.size()) {
                i += 9;
                continue;
            }
            if (c == 'r') {
                ++i;
                continue;
            }
            if (c == 'n') {
                o.push_back(' ');
                ++i;
                continue;
            }
            if (c == '|') {
                o.push_back('|');
                ++i;
                continue;
            }
        }
        o.push_back(s[i]);
    }
    s.swap(o);
}

// ---------------------------------------------------------------- allocator
void* fxAlloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    auto* a = static_cast<FxUi::Alloc*>(ud);
    if (nsize == 0) {
        std::free(ptr);
        a->live -= osize;
        return nullptr;
    }
    // PSP: a hard budget, so a runaway script fails its own pcall instead of
    // taking the world's heap (Lua 5.1 raises LUA_ERRMEM, no emergency GC).
    if (nsize > osize && a->live - osize + nsize > a->budget) {
        if (++a->fails <= 8 && a->owner)
            a->owner->logf("lua_alloc_fail size=%u live_kb=%u budget_kb=%u", static_cast<unsigned>(nsize),
                           static_cast<unsigned>(a->live / 1024), static_cast<unsigned>(a->budget / 1024));
        return nullptr;
    }
    void* p = std::realloc(ptr, nsize);
    if (!p) {
        if (++a->fails <= 8 && a->owner)
            a->owner->logf("lua_alloc_fail size=%u live_kb=%u heap=1", static_cast<unsigned>(nsize),
                           static_cast<unsigned>(a->live / 1024));
        return nullptr;
    }
    a->live = a->live - osize + nsize;
    if (a->live > a->peak) a->peak = a->live;
    ++a->count;
    return p;
}

int noop(lua_State*) { return 0; }

// Finite by the exponent bits, not by a compare (the Allegrex FPU traps on
// NaN compares when its exceptions are enabled; reuse_probe.cpp does the same).
bool finite(float v) {
    uint32_t u;
    std::memcpy(&u, &v, 4);
    return (u & 0x7F800000u) != 0x7F800000u;
}
bool finiteItem(const FxDrawItem& d) {
    return finite(d.x) && finite(d.y) && finite(d.w) && finite(d.h) && finite(d.u0) && finite(d.u1) &&
           finite(d.v0) && finite(d.v1) && finite(d.edge);
}

}  // namespace

// the bindings in fx_runtime_game.cpp reach the FxUi the same way.
FxUi* FxUi::fromLua(lua_State* L) { return self(L); }

// ======================================================================
// FxUi basics

FxUi::FxUi() = default;
FxUi::~FxUi() { shutdown(); }

void FxUi::logf(const char* fmt, ...) {
    if (!api_) return;
    char line[128];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    api_->log(line);
}

void FxUi::noteLuaError(const char* where) {
    ++luaErrors_;
    const char* msg = L_ ? lua_tostring(L_, -1) : nullptr;
    if (luaErrors_ <= 40) logf("lua_error at=%s msg=%.90s", where ? where : "?", msg ? msg : "?");
}

void FxUi::noteMissingApi(const char* name) {
    if (missingSeen_.emplace(name, true).second) {
        ++missingApi_;
        if (missingApi_ <= 60) logf("fx_missing_api name=%.80s", name);
    }
}

const FxFont* FxUi::font(int i) const {
    return (i >= 0 && static_cast<uint32_t>(i) < hdr_.fontCount) ? &fonts_[i] : nullptr;
}

const char* FxUi::string(uint32_t off) const {
    return (strings_ && off < hdr_.stringsSize) ? strings_ + off : "";
}

uint32_t FxUi::internName(const char* s) {
    if (!s || !*s) return 0;
    const uint32_t off = static_cast<uint32_t>(names_.size());
    names_.append(s);
    names_.push_back('\0');
    return off;
}

uint32_t FxUi::findByName(const char* name) const {
    if (!name || !*name) return kNone;
    auto it = byName_.find(name);
    return it == byName_.end() ? kNone : it->second;
}

int FxUi::textureByPath(const char* path) const {
    if (!path || !*path) return -1;
    std::string k;
    for (const char* p = path; *p; ++p) {
        char c = *p;
        if (c == '\\') {
            k.push_back('/');
            while (p[1] == '\\') ++p;
            continue;
        }
        k.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    const size_t dot = k.find_last_of('.');
    if (dot != std::string::npos && dot > k.find_last_of('/')) k.resize(dot);
    auto it = texByPath_.find(k);
    return it == texByPath_.end() ? -1 : it->second;
}

float FxUi::effScale(uint32_t i) const {
    float s = 1.0f;
    for (int d = 0; i != kNone && d < 64; ++d) {
        s *= objs_[i].scale;
        i = objs_[i].parent;
    }
    return s;
}

bool FxUi::isVisible(uint32_t i) const {
    for (int d = 0; i != kNone && d < 64; ++d) {
        if (!(objs_[i].flags & kShown)) return false;
        i = objs_[i].parent;
    }
    return true;
}

int FxUi::fontForHeight(float h) const {
    int best = -1;
    float bestD = 1e9f;
    for (uint32_t f = 0; f < hdr_.fontCount; ++f) {
        const float d = std::fabs(fonts_[f].height - h);
        if (d < bestD) {
            bestD = d;
            best = static_cast<int>(f);
        }
    }
    return best;
}

float FxUi::textWidth(uint32_t i) const {
    const Obj& o = objs_[i];
    if (o.text.empty() || !api_) return 0.0f;
    const FxFont* f = font(o.font);
    const float h = o.fontHeight > 0 ? o.fontHeight : (f ? f->height : 12.0f);
    return api_->textWidth(o.font, h, o.text.c_str()) / cfg_.uiScale;
}

void FxUi::setText(uint32_t i, const char* text) {
    Obj& o = objs_[i];
    std::string s = text ? text : "";
    stripMarkup(s);
    if (s == o.text) return;
    o.text.swap(s);
    if (!(o.flags & kHasSize) || o.w == 0) markLayout();
}

// ======================================================================
// Objects

uint32_t FxUi::createObject(uint8_t type, uint32_t parent, const char* name) {
    const uint32_t idx = static_cast<uint32_t>(objs_.size());
    objs_.emplace_back();
    Obj& o = objs_.back();
    o.type = type;
    o.parent = parent;
    if (type == kFxButton || type == kFxCheckButton) o.flags |= kMouse;
    if (type == kFxStatusBar || type == kFxSlider) o.maxV = 1.0f;
    if (parent != kNone) {
        Obj& p = objs_[parent];
        o.strata = p.strata;
        o.level = static_cast<int16_t>(isRegion(type) ? p.level : p.level + 1);
        if (p.lastChild == kNone) p.firstChild = idx;
        else objs_[p.lastChild].next = idx;
        p.lastChild = idx;
    }
    if (name && *name) {
        o.name = internName(name);
        byName_[name] = idx;
    }
    if (!isRegion(type)) markOrder();
    markLayout();
    return idx;
}

int FxUi::setPoint(uint32_t i, uint8_t point, uint32_t relObj, uint8_t relPoint, float x, float y, bool relParent) {
    Obj& o = objs_[i];
    o.flags &= ~kSetAll;
    Anchor a{};
    a.point = point;
    a.relPoint = relPoint;
    a.relParent = relParent ? 1 : 0;
    a.relObj = relObj;
    a.x = x;
    a.y = y;
    // Same point again replaces it (the client keeps one anchor per point).
    for (uint8_t k = 0; k < o.anchorCount && k < 2; ++k)
        if (o.anchors[k].point == point) {
            o.anchors[k] = a;
            markLayout();
            return k;
        }
    int k = 1;
    if (o.anchorCount < 2) {
        k = o.anchorCount++;
        o.anchors[k] = a;
    } else {
        // PSP: a third distinct point replaces the second (two decide a rect).
        o.anchors[1] = a;
    }
    markLayout();
    return k;
}

void FxUi::clearPoints(uint32_t i) {
    objs_[i].anchorCount = 0;
    objs_[i].flags &= ~kSetAll;
    markLayout();
}

void FxUi::setShown(uint32_t i, bool shown) {
    Obj& o = objs_[i];
    const bool was = (o.flags & kShown) != 0;
    if (was == shown) return;
    drawDirty_ = true;
    const bool visBefore = isVisible(i);
    if (shown) o.flags |= kShown;
    else o.flags &= ~kShown;
    markLayout();
    if (!isRegion(o.type) && L_) {
        const bool visAfter = isVisible(i);
        if (visBefore != visAfter) {
            // the frame and every shown descendant frame that became
            // visible / hidden with it (the client's OnShow / OnHide; a
            // CharacterFrame shown runs PaperDollFrame's OnShow).
            const int kind = visAfter ? kScrOnShow : kScrOnHide;
            std::vector<uint32_t> stack{i};
            while (!stack.empty()) {
                const uint32_t x = stack.back();
                stack.pop_back();
                if (x != i && !(objs_[x].flags & kShown)) continue;
                if (hasScript(x, kind)) callScript(x, kind, 0);
                for (uint32_t c = objs_[x].firstChild; c != kNone; c = objs_[c].next)
                    if (!isRegion(objs_[c].type)) stack.push_back(c);
            }
        }
    }
}

// Lua table of a widget: t[0] = index, metatable of its type; kept in the
// registry objs table and, for a named widget, as the global of its name.
void FxUi::pushObject(uint32_t i) {
    lua_State* L = L_;
    Obj& o = objs_[i];
    lua_rawgeti(L, LUA_REGISTRYINDEX, objsRef_);
    if (o.luaRef) {
        lua_rawgeti(L, -1, static_cast<int>(i) + 1);
        lua_remove(L, -2);
        return;
    }
    lua_createtable(L, 0, 1);
    lua_pushnumber(L, static_cast<lua_Number>(i));
    lua_rawseti(L, -2, 0);
    lua_rawgeti(L, LUA_REGISTRYINDEX, methodsRef_[o.type < kFxTypeCount ? o.type : 0]);
    lua_setmetatable(L, -2);
    lua_pushvalue(L, -1);
    lua_rawseti(L, -3, static_cast<int>(i) + 1);
    lua_remove(L, -2);
    o.luaRef = 1;
    if (o.name) {
        const char* n = nameOf(i);
        if (findByName(n) == i) {
            lua_pushvalue(L, -1);
            lua_setglobal(L, n);  // _G has no __newindex: a plain set
        }
    }
}

uint32_t FxUi::toObject(int idx) {
    if (!lua_istable(L_, idx)) return kNone;
    lua_rawgeti(L_, idx, 0);
    uint32_t r = kNone;
    if (lua_isnumber(L_, -1)) {
        const lua_Number n = lua_tonumber(L_, -1);
        if (n >= 0 && n < static_cast<lua_Number>(objs_.size())) r = static_cast<uint32_t>(n);
    }
    lua_pop(L_, 1);
    return r;
}

int FxUi::scriptRef(uint32_t i, int kind) const {
    const Obj& o = objs_[i];
    if (o.scripts < 0 || kind < 0 || kind >= kScrCount) return 0;
    return scriptSets_[static_cast<size_t>(o.scripts)].ref[kind];
}

void FxUi::setScript(uint32_t i, int kind, int stackIndex) {
    if (kind < 0 || kind >= kScrCount) return;
    Obj& o = objs_[i];
    if (o.scripts < 0) {
        if (!stackIndex) return;
        ScriptSet s{};
        scriptSets_.push_back(s);
        objs_[i].scripts = static_cast<int32_t>(scriptSets_.size() - 1);
    }
    int32_t& ref = scriptSets_[static_cast<size_t>(objs_[i].scripts)].ref[kind];
    if (ref) {
        luaL_unref(L_, LUA_REGISTRYINDEX, ref);
        ref = 0;
    }
    if (stackIndex && lua_isfunction(L_, stackIndex)) {
        lua_pushvalue(L_, stackIndex);
        ref = luaL_ref(L_, LUA_REGISTRYINDEX);
    }
    if (kind == kScrOnUpdate) {
        auto it = std::find(onUpdate_.begin(), onUpdate_.end(), i);
        if (ref && it == onUpdate_.end()) onUpdate_.push_back(i);
        if (!ref && it != onUpdate_.end()) onUpdate_.erase(it);
    }
}

// Stack: args (nargs). Calls handler(self, args...) protected; pops the args.
void FxUi::callScript(uint32_t i, int kind, int nargs) {
    const int ref = scriptRef(i, kind);
    if (!ref) {
        lua_pop(L_, nargs);
        return;
    }
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
    lua_insert(L_, -(nargs + 1));
    pushObject(i);
    lua_insert(L_, -(nargs + 1));
    ++handlerCalls_;
    // script time per top-level frame (outermost call only: a
    // handler that clicks another frame counts once, to its own frame).
    const bool outer = scriptDepth_++ == 0;
    const uint64_t t0 = outer ? api_->nowUs() : 0;
    if (lua_pcall(L_, nargs + 1, 0, 0) != 0) {
        char where[64];
        std::snprintf(where, sizeof(where), "%s.%s", objs_[i].name ? nameOf(i) : "?", kScriptNames[kind]);
        noteLuaError(where);
        lua_pop(L_, 1);
    }
    --scriptDepth_;
    if (outer) {
        FxFrameTime& ft = frameTimes_[topLevelOf(i)];
        ft.us += api_->nowUs() - t0;
        ++ft.calls;
    }
}

int FxUi::eventId(const char* ev, bool create) {
    auto it = eventIds_.find(ev);
    if (it != eventIds_.end()) return it->second;
    if (!create) return -1;
    const int id = static_cast<int>(eventFrames_.size());
    eventIds_.emplace(ev, id);
    eventFrames_.emplace_back();
    lua_pushstring(L_, ev);
    eventNameRefs_.push_back(luaL_ref(L_, LUA_REGISTRYINDEX));
    return id;
}

void FxUi::registerEvent(uint32_t i, const char* ev, bool on) {
    const int id = eventId(ev, on);
    if (id < 0) return;
    auto& v = eventFrames_[static_cast<size_t>(id)];
    auto it = std::find(v.begin(), v.end(), i);
    if (on && it == v.end()) v.push_back(i);
    if (!on && it != v.end()) v.erase(it);
}

void FxUi::unregisterAll(uint32_t i) {
    for (auto& v : eventFrames_) v.erase(std::remove(v.begin(), v.end(), i), v.end());
}

// PSP: the event name is a pre-interned string (registry ref) - no
// lua_pushstring per dispatch (docs/PSP_LUA_NOTES.md).
void FxUi::fireEvent(const char* ev, const char* arg1) {
    FxEventArg a;
    if (arg1) {
        a.kind = 2;
        std::snprintf(a.str, sizeof(a.str), "%s", arg1);
    }
    fireEventArgs(ev, &a, arg1 ? 1 : 0);
}

// up to 5 fixed arguments (numbers / strings) pushed per handler.
void FxUi::fireEventArgs(const char* ev, const FxEventArg* args, int argc) {
    const int id = eventId(ev, false);
    if (id < 0 || eventFrames_[static_cast<size_t>(id)].empty()) return;
    ++eventsFired_;
    const std::vector<uint32_t> frames = eventFrames_[static_cast<size_t>(id)];
    for (uint32_t f : frames) {
        if (!hasScript(f, kScrOnEvent)) continue;
        lua_rawgeti(L_, LUA_REGISTRYINDEX, eventNameRefs_[static_cast<size_t>(id)]);
        for (int k = 0; k < argc; ++k) {
            if (args[k].kind == 1) lua_pushnumber(L_, static_cast<lua_Number>(args[k].num));
            else if (args[k].kind == 2) lua_pushstring(L_, args[k].str);
            else lua_pushnil(L_);
        }
        callScript(f, kScrOnEvent, argc + 1);
    }
}

// ======================================================================
// Instantiation (the load program, CreateFrame with templates)

uint32_t FxUi::instantiate(uint32_t ni, uint32_t parent, uint32_t into, const std::string& owner, bool runOnLoad) {
    const FxNode& n = nodes_[ni];
    uint32_t i = into;
    if (i == kNone) {
        bool unnamed = false;
        const std::string name = substituteParent(string(n.name), owner, unnamed);
        i = createObject(n.type, parent, name.empty() ? nullptr : name.c_str());
    }
    {
        Obj& o = objs_[i];
        o.node = &n;
        if (n.flags & kNodeHidden) o.flags &= ~kShown;
        if (n.flags & kNodeMouse) o.flags |= kMouse;
        if (n.flags & kNodeAlphaAdd) o.flags |= kAdd;
        if (n.flags & kNodeTexColor) o.flags |= kTexColor;
        if (n.flags & kNodeWrap) o.flags |= kWrap;
        if (n.flags & kNodeTopLevel) o.flags |= kTopLevel;
        if (n.flags & kNodeHasSize) {
            o.flags |= kHasSize;
            o.w = n.w;
            o.h = n.h;
        }
        o.alpha = n.alpha;
        o.scale = n.scale > 0 ? n.scale : 1.0f;
        if (n.strata != kStrataInherit) o.strata = n.strata;
        if (n.frameLevel >= 0) o.level = n.frameLevel;
        if (n.id) o.id = n.id;
        if (isRegion(n.type)) {
            o.layer = n.layer;
            o.subLevel = n.subLevel;
            o.role = n.role;
        }
        if (n.texture >= 0) o.texture = n.texture;
        std::memcpy(o.tc, n.tc, sizeof(o.tc));
        if (n.color != 0xFFFFFFFFu || isRegion(n.type)) o.color = n.color;
        if (n.font >= 0) o.font = n.font;
        o.jh = n.justifyH;
        o.jv = n.justifyV;
        if (n.type == kFxStatusBar || n.type == kFxSlider) {
            o.minV = n.minValue;
            o.maxV = n.maxValue > n.minValue ? n.maxValue : n.minValue + 1.0f;
            o.value = n.value;
        }
        if (n.backdrop) o.backdrop = n.backdrop;
        if (n.flags & kNodeSetAllPoints) o.flags |= kSetAll;
    }
    // text="GLOBAL_STRING": the global's value if it is a string (upstream emitTextAttr).
    if (n.text) {
        const char* t = string(n.text);
        if (n.flags & kNodeHasText) {
            lua_pushstring(L_, t);  // rawget: no __index fallback for a missing string
            lua_rawget(L_, LUA_GLOBALSINDEX);
            if (lua_type(L_, -1) == LUA_TSTRING) t = lua_tostring(L_, -1);
            if (n.type == kFxFontString) setText(i, t);
            else objs_[i].text = t;  // a Button's label: its ButtonText takes it below
            lua_pop(L_, 1);
        } else {
            setText(i, t);
        }
    }
    // Anchors (baked names are resolved; a template's still carry $parent).
    for (uint32_t k = 0; k < n.anchorCount; ++k) {
        const FxAnchor& a = anchors_[n.anchorFirst + k];
        uint32_t rel = kNone;
        bool relParent = a.relParent != 0;
        if (!relParent) {
            bool unnamed = false;
            const std::string rn = substituteParent(string(a.relTo), owner, unnamed);
            rel = findByName(rn.c_str());
            if (rel == kNone) {
                // Not created yet: resolved by name at layout time.
                const int k = setPoint(i, a.point, kNone, a.relPoint, a.x, a.y, false);
                objs_[i].anchors[k].relName = internName(rn.c_str());
                continue;
            }
        }
        setPoint(i, a.point, rel, a.relPoint, a.x, a.y, relParent);
    }
    // parentKey: parent.key = this (both get Lua tables).
    if (n.parentKey && parent != kNone) {
        pushObject(parent);
        pushObject(i);
        lua_setfield(L_, -2, string(n.parentKey));
        lua_pop(L_, 1);
    }
    // Children in creation order: regions, then frames.
    const std::string myName = objs_[i].name ? std::string(nameOf(i)) : std::string();
    const std::string childOwner = isRegion(n.type) ? owner : myName;
    for (uint32_t c = ni + 1; c < n.subtreeEnd; c = nodes_[c].subtreeEnd) {
        const uint32_t ci = instantiate(c, i, kNone, childOwner, runOnLoad);
        // A Button's label text lands on its ButtonText.
        if (objs_[ci].role == kRoleButtonText && !objs_[i].text.empty()) setText(ci, objs_[i].text.c_str());
    }
    // Scripts: the frame's handlers (XML bodies from their file's table).
    for (uint32_t s = 0; s < n.scriptCount; ++s) {
        const FxScript& sc = scripts_[n.scriptFirst + s];
        if (sc.isGlobal) {
            lua_pushstring(L_, string(sc.chunk));
            lua_rawget(L_, LUA_GLOBALSINDEX);
        } else {
            if (sc.chunk >= hdr_.chunkCount || !loadHandlers(sc.chunk)) continue;
            lua_rawgeti(L_, LUA_REGISTRYINDEX, handlerTables_[sc.chunk]);
            lua_rawgeti(L_, -1, sc.fn + 1);
            lua_remove(L_, -2);
        }
        setScript(i, sc.kind, lua_isfunction(L_, -1) ? lua_gettop(L_) : 0);
        lua_pop(L_, 1);
    }
    if (runOnLoad && into == kNone && hasScript(i, kScrOnLoad)) callScript(i, kScrOnLoad, 0);
    objs_[i].flags |= kLoaded;
    return i;
}

bool FxUi::applyTemplate(uint32_t obj, const char* name) {
    for (uint32_t t = 0; t < hdr_.templateCount; ++t) {
        if (std::strcmp(string(templates_[t].name), name) != 0) continue;
        const uint32_t root = templates_[t].node;
        const uint8_t type = objs_[obj].type;
        const std::string owner = objs_[obj].name ? std::string(nameOf(obj)) : std::string();
        instantiate(root, objs_[obj].parent, obj, owner, true);
        objs_[obj].type = type;  // the frame keeps the type it was created with
        if (hasScript(obj, kScrOnLoad)) callScript(obj, kScrOnLoad, 0);  // CreateFrame runs the template's OnLoad
        return true;
    }
    return false;
}

bool FxUi::loadHandlers(uint32_t chunk) {
    if (handlerTables_[chunk]) return handlerTables_[chunk] > 0;  // -1: failed before
    std::vector<uint8_t> bytes;
    if (!reader_ || !reader_(readerCtx_, chunk, bytes)) return false;
    char name[64];
    std::snprintf(name, sizeof(name), "=%s", string(chunks_[chunk].file));
    if (luaL_loadbuffer(L_, reinterpret_cast<const char*>(bytes.data()), bytes.size(), name) != 0 ||
        lua_pcall(L_, 0, 1, 0) != 0) {
        noteLuaError(string(chunks_[chunk].file));
        lua_pop(L_, 1);
        handlerTables_[chunk] = -1;
        return false;
    }
    if (!lua_istable(L_, -1)) {
        lua_pop(L_, 1);
        handlerTables_[chunk] = -1;
        return false;
    }
    handlerTables_[chunk] = luaL_ref(L_, LUA_REGISTRYINDEX);
    return true;
}

uint32_t FxUi::luaNumberSize() { return static_cast<uint32_t>(sizeof(lua_Number)); }

void FxUi::preloadHandlers() {
    if (L_) protect([](FxUi& u, void*) { u.preloadHandlersImpl(); }, nullptr, "preload");
}

void FxUi::preloadHandlersImpl() {
    for (uint32_t c = 0; c < hdr_.chunkCount; ++c)
        if (chunks_[c].fnCount > 0) loadHandlers(c);
}

bool FxUi::runChunk(uint32_t index, bool keepResult) {
    std::vector<uint8_t> bytes;
    if (!reader_ || !reader_(readerCtx_, index, bytes)) {
        logf("fx_chunk_missing index=%u", static_cast<unsigned>(index));
        return false;
    }
    char name[80];
    std::snprintf(name, sizeof(name), "=%s", string(chunks_[index].file));
    if (luaL_loadbuffer(L_, reinterpret_cast<const char*>(bytes.data()), bytes.size(), name) != 0 ||
        lua_pcall(L_, 0, keepResult ? 1 : 0, 0) != 0) {
        noteLuaError(string(chunks_[index].file));
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

void FxUi::setupStateImpl() {
    // base, table, string, math (upstream LuaEngine::initialize: no io/os/debug/package)
    for (lua_CFunction open : {luaopen_base, luaopen_table, luaopen_string, luaopen_math}) {
        lua_pushcfunction(L_, open);
        lua_call(L_, 0, 0);
    }
    for (const char* g : {"dofile", "loadfile", "load", "collectgarbage"}) {
        lua_pushnil(L_);
        lua_setglobal(L_, g);
    }
    lua_newtable(L_);
    objsRef_ = luaL_ref(L_, LUA_REGISTRYINDEX);
    installApi();
}

void FxUi::loadProgramImpl() {
    runLoadProgram();
    fireEvent("VARIABLES_LOADED");
    fireEvent("PLAYER_LOGIN");
    fireEvent("PLAYER_ENTERING_WORLD");
}

void FxUi::runLoadProgram() {
    for (uint32_t o = 0; o < hdr_.opCount; ++o) {
        const FxOp& op = ops_[o];
        if (op.op == kOpRunChunk) {
            if (op.a < hdr_.chunkCount) runChunk(op.a, false);
        } else if (op.op == kOpCreateFrames) {
            for (uint32_t n = op.a; n < op.b && n < hdr_.nodeCount; n = nodes_[n].subtreeEnd) {
                const FxNode& node = nodes_[n];
                if (node.flags & kNodeVirtual) continue;
                uint32_t parent = uiParent_;
                if (node.parentName) {
                    const uint32_t p = findByName(string(node.parentName));
                    if (p != kNone) parent = p;
                }
                instantiate(n, parent, kNone, std::string(), true);
            }
        }
    }
}

// ======================================================================
// Layout: upstream WidgetTree anchor rules. Rects in UI units, y up.

void FxUi::resolve(uint32_t i, int depth) {
    Obj& o = objs_[i];
    if (o.gen == layoutGen_ || depth > 32) return;
    o.gen = layoutGen_;  // set first: a cycle resolves against the stale rect
    if (i == uiParent_) {
        o.l = 0;
        o.b = 0;
        o.r = uiW_;
        o.t = uiH_;
        return;
    }
    const uint32_t parent = o.parent == kNone ? uiParent_ : o.parent;
    auto relRect = [&](uint32_t rel, float& l, float& b, float& r, float& t) {
        if (rel == kNone) rel = parent;
        if (rel != kNone && rel != i) resolve(rel, depth + 1);
        const Obj& ro = objs_[rel == kNone ? uiParent_ : rel];
        l = ro.l;
        b = ro.b;
        r = ro.r;
        t = ro.t;
    };
    const float es = effScale(i);
    float w = o.w * es, h = o.h * es;
    if (!(o.flags & kHasSize) || (o.w == 0 && o.h == 0)) {
        if (o.type == kFxFontString) {
            const FxFont* f = font(o.font);
            const float fh = o.fontHeight > 0 ? o.fontHeight : (f ? f->height : 12.0f);
            if (w == 0) w = textWidth(i) * es;
            if (h == 0) h = fh * es;
        }
    } else if (o.type == kFxFontString && o.h == 0) {
        const FxFont* f = font(o.font);
        h = (o.fontHeight > 0 ? o.fontHeight : (f ? f->height : 12.0f)) * es;
    }
    if (o.flags & kSetAll) {
        float l, b, r, t;
        relRect(kNone, l, b, r, t);
        o.l = l;
        o.b = b;
        o.r = r;
        o.t = t;
        return;
    }
    if (o.anchorCount == 0) {
        // upstream: an unanchored region/frame falls back to CENTER of its parent.
        if (isRegion(o.type) && !(o.flags & kHasSize) && o.type == kFxTexture) {
            float l, b, r, t;
            relRect(kNone, l, b, r, t);
            o.l = l;
            o.b = b;
            o.r = r;
            o.t = t;
            return;
        }
        float l, b, r, t;
        relRect(kNone, l, b, r, t);
        const float cx = (l + r) * 0.5f, cy = (b + t) * 0.5f;
        o.l = cx - w * 0.5f;
        o.r = cx + w * 0.5f;
        o.b = cy - h * 0.5f;
        o.t = cy + h * 0.5f;
        return;
    }
    // Constraints per axis: x = position of fraction fx of my rect.
    bool hasL = false, hasR = false, hasCx = false, hasB = false, hasT = false, hasCy = false;
    float L = 0, R = 0, Cx = 0, B = 0, T = 0, Cy = 0;
    for (uint8_t k = 0; k < o.anchorCount && k < 2; ++k) {
        Anchor& a = o.anchors[k];
        if (!a.relParent && a.relObj == kNone && a.relName) {
            a.relObj = findByName(names_.data() + a.relName);
        }
        float l, b, r, t;
        relRect(a.relParent ? kNone : a.relObj, l, b, r, t);
        const float px = l + (r - l) * kPtX[a.relPoint] + a.x * es;
        const float py = b + (t - b) * kPtY[a.relPoint] + a.y * es;
        const float fx = kPtX[a.point], fy = kPtY[a.point];
        if (fx == 0.0f) hasL = true, L = px;
        else if (fx == 1.0f) hasR = true, R = px;
        else hasCx = true, Cx = px;
        if (fy == 0.0f) hasB = true, B = py;
        else if (fy == 1.0f) hasT = true, T = py;
        else hasCy = true, Cy = py;
    }
    if (hasL && hasR) {
        o.l = L;
        o.r = R;
    } else if (hasL) {
        o.l = L;
        o.r = L + w;
    } else if (hasR) {
        o.r = R;
        o.l = R - w;
    } else {
        o.l = Cx - w * 0.5f;
        o.r = Cx + w * 0.5f;
    }
    if (hasB && hasT) {
        o.b = B;
        o.t = T;
    } else if (hasB) {
        o.b = B;
        o.t = B + h;
    } else if (hasT) {
        o.t = T;
        o.b = T - h;
    } else {
        o.b = Cy - h * 0.5f;
        o.t = Cy + h * 0.5f;
    }
    (void)hasCx;
    (void)hasCy;
}

// ======================================================================
// Drawing

void FxUi::rebuildOrder() {
    order_.clear();
    for (uint32_t i = 0; i < objs_.size(); ++i)
        if (!isRegion(objs_[i].type) && i != uiParent_) order_.push_back(i);
    std::stable_sort(order_.begin(), order_.end(), [&](uint32_t a, uint32_t b) {
        const Obj& A = objs_[a];
        const Obj& B = objs_[b];
        if (A.strata != B.strata) return A.strata < B.strata;
        return A.level < B.level;
    });
    orderDirty_ = false;
}

void FxUi::emitRegion(uint32_t ri, uint32_t owner, int clip) {
    Obj& r = objs_[ri];
    if (!(r.flags & kShown)) return;
    const Obj& f = objs_[owner];
    // Button state art (upstream ButtonArt): one of normal/pushed/disabled.
    if (r.role == kRoleNormal || r.role == kRolePushed || r.role == kRoleDisabled) {
        const bool disabled = !(f.flags & kEnabled);
        const bool pushed = (f.flags & kPushed) != 0;
        uint8_t want = disabled ? kRoleDisabled : (pushed ? kRolePushed : kRoleNormal);
        if (r.role != want) {
            // fall back to Normal when the state has no art
            bool haveWant = false;
            for (uint32_t c = f.firstChild; c != kNone; c = objs_[c].next)
                if (objs_[c].role == want && objs_[c].texture >= 0) haveWant = true;
            if (haveWant || r.role != kRoleNormal) return;
        }
    }
    if (r.role == kRoleHighlight || r.layer == kLayerHighlight) {
        if (!(f.flags & (kHovered | kHighlightLocked))) return;
    }
    if ((r.role == kRoleChecked || r.role == kRoleDisabledChecked) && !(f.flags & kChecked)) return;
    resolve(ri);
    const float s = cfg_.uiScale;
    float l = r.l, rr = r.r;
    float u1 = r.tc[1];
    if (r.role == kRoleBar) {
        // StatusBar fill: value fraction of the width, texture cropped (not squeezed).
        const float range = f.maxV - f.minV;
        float frac = range > 0 ? (f.value - f.minV) / range : 0.0f;
        frac = frac < 0 ? 0 : (frac > 1 ? 1 : frac);
        rr = l + (rr - l) * frac;
        u1 = r.tc[0] + (r.tc[1] - r.tc[0]) * frac;
        if (frac <= 0.0f) return;
    }
    float alpha = r.alpha;
    for (uint32_t p = owner; p != kNone; p = objs_[p].parent) alpha *= objs_[p].alpha;
    FxDrawItem d;
    d.strata = f.strata;
    d.clip = static_cast<int16_t>(clip);
    d.x = l * s;
    d.y = (uiH_ - r.t) * s;
    d.w = (rr - l) * s;
    d.h = (r.t - r.b) * s;
    if (r.type == kFxTexture) {
        uint32_t color = r.color;
        if (r.role == kRoleBar && f.color != 0xFFFFFFFFu) color = f.color;
        if (r.flags & kTexColor) {
            d.kind = FxDrawItem::Fill;
        } else {
            if (r.texture < 0) return;
            d.kind = FxDrawItem::Image;
            d.texture = r.texture;
            d.u0 = r.tc[0];
            d.u1 = u1;
            d.v0 = r.tc[2];
            d.v1 = r.tc[3];
        }
        d.add = (r.flags & kAdd) ? 1 : 0;
        d.color = mulAlpha(color, alpha);
        if (!finiteItem(d) || d.w <= 0.0f || d.h <= 0.0f) return;
    } else {
        if (r.text.empty()) return;
        d.kind = FxDrawItem::Text;
        d.text = r.text.c_str();
        d.font = r.font;
        const FxFont* ff = font(r.font);
        d.edge = r.fontHeight > 0 ? r.fontHeight : (ff ? ff->height : 12.0f);
        d.justify = r.jh;
        d.color = mulAlpha(r.color, alpha);
        // Vertical justify inside the box (single line).
        const float lineH = d.edge * s;
        if (r.jv == 1) d.y += (d.h - lineH) * 0.5f;
        else if (r.jv == 2) d.y += d.h - lineH;
        d.h = lineH;
        if (!finiteItem(d)) return;
    }
    draw_.push_back(d);
}

void FxUi::emitFrame(uint32_t fi, int clip) {
    Obj& f = objs_[fi];
    resolve(fi);
    const float s = cfg_.uiScale;
    if (f.backdrop && f.backdrop <= hdr_.backdropCount) {
        const FxBackdrop& bd = backdrops_[f.backdrop - 1];
        float alpha = 1.0f;
        for (uint32_t p = fi; p != kNone; p = objs_[p].parent) alpha *= objs_[p].alpha;
        FxDrawItem d;
        d.strata = f.strata;
        d.clip = static_cast<int16_t>(clip);
        if (bd.bgTexture >= 0) {
            d.kind = bd.tile ? FxDrawItem::Tile : FxDrawItem::Image;
            d.texture = bd.bgTexture;
            d.x = (f.l + bd.insets[0]) * s;
            d.y = (uiH_ - f.t + bd.insets[2]) * s;
            d.w = (f.r - f.l - bd.insets[0] - bd.insets[1]) * s;
            d.h = (f.t - f.b - bd.insets[2] - bd.insets[3]) * s;
            d.edge = bd.tileSize * s;
            d.color = mulAlpha(bd.color, alpha);
            if (finiteItem(d)) draw_.push_back(d);
        }
        if (bd.edgeTexture >= 0) {
            d.kind = FxDrawItem::NineSlice;
            d.texture = bd.edgeTexture;
            d.x = f.l * s;
            d.y = (uiH_ - f.t) * s;
            d.w = (f.r - f.l) * s;
            d.h = (f.t - f.b) * s;
            d.edge = bd.edgeSize * s;  // border thickness in px; the renderer maps it to texels
            d.color = mulAlpha(bd.borderColor, alpha);
            if (finiteItem(d)) draw_.push_back(d);
        }
    }
    // Regions by layer, then sublevel, then creation (upstream draw order).
    uint32_t regs[96];
    int n = 0;
    for (uint32_t c = f.firstChild; c != kNone && n < 96; c = objs_[c].next)
        if (isRegion(objs_[c].type)) regs[n++] = c;
    std::stable_sort(regs, regs + n, [&](uint32_t a, uint32_t b) {
        const Obj& A = objs_[a];
        const Obj& B = objs_[b];
        if (A.layer != B.layer) return A.layer < B.layer;
        return A.subLevel < B.subLevel;
    });
    for (int k = 0; k < n; ++k) emitRegion(regs[k], fi, clip);
    // the Cooldown model's sweep (SetCooldown(start, duration):
    // minV = start, maxV = duration), the part still to run.
    if (f.type == kFxCooldown && f.maxV > 0.0f) {
        const float left = f.minV + f.maxV - static_cast<float>(api_->timeSec());
        if (left > 0.0f && left <= f.maxV) {
            float alpha = 1.0f;
            for (uint32_t p = fi; p != kNone; p = objs_[p].parent) alpha *= objs_[p].alpha;
            FxDrawItem d;
            d.kind = FxDrawItem::Cooldown;
            d.strata = f.strata;
            d.clip = static_cast<int16_t>(clip);
            d.x = f.l * s;
            d.y = (uiH_ - f.t) * s;
            d.w = (f.r - f.l) * s;
            d.h = (f.t - f.b) * s;
            d.edge = left / f.maxV;
            d.color = mulAlpha(0xA0000000u, alpha);
            if (finiteItem(d) && d.w > 0.0f && d.h > 0.0f) draw_.push_back(d);
            animating_ = true;
        } else if (left <= 0.0f) {
            f.maxV = 0.0f;  // finished: nothing to draw until the next SetCooldown
        }
    }
}

void FxUi::buildDrawList() {
    if (!L_) return;
    if (!drawDirty_ && !orderDirty_) {
        layoutMsLast_ = 0;
        return;  // nothing changed since the last list
    }
    draw_.clear();
    clips_.clear();
    animating_ = false;
    const uint64_t t0 = api_->nowUs();
    if (orderDirty_) rebuildOrder();
    for (uint32_t fi : order_) {
        if (!isVisible(fi)) continue;
        // Clip: the nearest ScrollFrame ancestor.
        int clip = -1;
        for (uint32_t p = objs_[fi].parent; p != kNone; p = objs_[p].parent) {
            if (objs_[p].type == kFxScrollFrame) {
                resolve(p);
                const float s = cfg_.uiScale;
                clips_.push_back(FxClip{objs_[p].l * s, (uiH_ - objs_[p].t) * s, objs_[p].r * s,
                                        (uiH_ - objs_[p].b) * s});
                clip = static_cast<int>(clips_.size() - 1);
                break;
            }
        }
        emitFrame(fi, clip);
    }
    layoutDirty_ = false;
    drawDirty_ = animating_;  // a running cooldown sweep moves every frame
    layoutMsLast_ = static_cast<float>(api_->nowUs() - t0) / 1000.0f;
}

// ======================================================================
// Input

uint32_t FxUi::hitTest(float ux, float uy) const {
    for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
        const uint32_t f = *it;
        const Obj& o = objs_[f];
        if (!(o.flags & kMouse) || !isVisible(f) || o.gen != layoutGen_) continue;
        if (ux >= o.l && ux < o.r && uy >= o.b && uy < o.t) return f;
    }
    return kNone;
}

namespace {
struct Job {
    void (*fn)(FxUi&, void*);
    FxUi* ui;
    void* arg;
};
int jobTrampoline(lua_State* L) {
    Job* j = static_cast<Job*>(lua_touserdata(L, 1));
    j->fn(*j->ui, j->arg);
    return 0;
}
// Last resort (an error with no protected call around it): log, then Lua exits.
int panicLog(lua_State* L) {
    FxUi* ui = self(L);
    const char* m = lua_tostring(L, -1);
    if (ui) ui->logf("lua_panic msg=%.100s", m ? m : "?");
    return 0;
}
}  // namespace

bool FxUi::protect(void (*fn)(FxUi&, void*), void* arg, const char* where) {
    if (!L_) return false;
    Job j{fn, this, arg};
    if (lua_cpcall(L_, jobTrampoline, &j) != 0) {
        noteLuaError(where);
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

bool FxUi::pointerMove(float sx, float sy) {
    struct A {
        float x, y;
        bool r;
    } a{sx, sy, false};
    protect([](FxUi& u, void* p) {
        A* q = static_cast<A*>(p);
        q->r = u.pointerMoveImpl(q->x, q->y);
    }, &a, "pointer");
    return a.r;
}

bool FxUi::pointerMoveImpl(float sx, float sy) {
    if (!L_) return false;
    const float ux = sx / cfg_.uiScale, uy = uiH_ - sy / cfg_.uiScale;
    const uint32_t h = hitTest(ux, uy);
    if (h != hovered_) {
        drawDirty_ = true;
        if (hovered_ != kNone) {
            objs_[hovered_].flags &= ~(kHovered | kPushed);
            if (hasScript(hovered_, kScrOnLeave)) {
                lua_pushboolean(L_, 1);
                callScript(hovered_, kScrOnLeave, 1);
            }
        }
        hovered_ = h;
        if (h != kNone) {
            objs_[h].flags |= kHovered;
            if (hasScript(h, kScrOnEnter)) {
                lua_pushboolean(L_, 1);
                callScript(h, kScrOnEnter, 1);
            }
        }
    }
    return h != kNone;
}

bool FxUi::clickObject(uint32_t f, bool right) {
    struct A {
        uint32_t f;
        bool right, r;
    } a{f, right, false};
    protect([](FxUi& u, void* p) {
        A* q = static_cast<A*>(p);
        q->r = u.clickObjectImpl(q->f, q->right);
    }, &a, "click");
    return a.r;
}

bool FxUi::clickObjectImpl(uint32_t f, bool right) {
    if (f >= objs_.size() || !(objs_[f].flags & kEnabled)) return false;
    const char* btn = right ? "RightButton" : "LeftButton";
    drawDirty_ = true;
    if (objs_[f].type == kFxCheckButton) objs_[f].flags ^= kChecked;
    if (hasScript(f, kScrOnClick)) {
        lua_pushstring(L_, btn);
        lua_pushboolean(L_, 0);
        callScript(f, kScrOnClick, 2);
        return true;
    }
    return objs_[f].type == kFxButton || objs_[f].type == kFxCheckButton;
}

bool FxUi::pointerButton(bool down, bool right) {
    struct A {
        bool down, right, r;
    } a{down, right, false};
    protect([](FxUi& u, void* p) {
        A* q = static_cast<A*>(p);
        q->r = u.pointerButtonImpl(q->down, q->right);
    }, &a, "button");
    return a.r;
}

bool FxUi::pointerButtonImpl(bool down, bool right) {
    if (!L_ || hovered_ == kNone) return false;
    const uint32_t f = hovered_;
    drawDirty_ = true;
    const char* btn = right ? "RightButton" : "LeftButton";
    if (down) {
        pressed_ = f;
        objs_[f].flags |= kPushed;
        if (hasScript(f, kScrOnMouseDown)) {
            lua_pushstring(L_, btn);
            callScript(f, kScrOnMouseDown, 1);
        }
    } else {
        objs_[f].flags &= ~kPushed;
        if (hasScript(f, kScrOnMouseUp)) {
            lua_pushstring(L_, btn);
            callScript(f, kScrOnMouseUp, 1);
        }
        if (pressed_ == f) clickObject(f, right);
        pressed_ = kNone;
    }
    return true;
}

void FxUi::clickables(std::vector<Clickable>& out, size_t max) const {
    out.clear();
    const float s = cfg_.uiScale;
    for (auto it = order_.rbegin(); it != order_.rend() && out.size() < max; ++it) {
        const Obj& o = objs_[*it];
        if (!(o.flags & kMouse) || !isVisible(*it) || o.gen != layoutGen_) continue;
        if (!finite(o.l) || !finite(o.r) || !finite(o.t) || !finite(o.b)) continue;
        if (o.r - o.l < 1 || o.t - o.b < 1) continue;
        out.push_back(Clickable{*it, o.l * s, (uiH_ - o.t) * s, (o.r - o.l) * s, (o.t - o.b) * s});
    }
}

// ======================================================================
// Per frame

void FxUi::pollGame() {
    FxUnit p, t;
    api_->unit("player", p);
    api_->unit("target", t);
    if (!polledOnce_) {
        lastPlayer_ = p;
        lastTarget_ = t;
        polledOnce_ = true;
        return;
    }
    static const char* const kPowerEvents[8] = {"UNIT_MANA", "UNIT_RAGE", "UNIT_FOCUS", "UNIT_ENERGY",
                                                "UNIT_HAPPINESS", "UNIT_RUNIC_POWER", "UNIT_RUNIC_POWER",
                                                "UNIT_MANA"};
    static const char* const kMaxPowerEvents[8] = {"UNIT_MAXMANA", "UNIT_MAXRAGE", "UNIT_MAXFOCUS",
                                                   "UNIT_MAXENERGY", "UNIT_MAXHAPPINESS",
                                                   "UNIT_MAXRUNIC_POWER", "UNIT_MAXRUNIC_POWER", "UNIT_MAXMANA"};
    auto diff = [&](const char* unit, const FxUnit& now, const FxUnit& was) {
        if (!now.exists) return;
        if (now.health != was.health) fireEvent("UNIT_HEALTH", unit);
        if (now.maxHealth != was.maxHealth) fireEvent("UNIT_MAXHEALTH", unit);
        if (now.powerType != was.powerType) fireEvent("UNIT_DISPLAYPOWER", unit);
        const uint8_t pt = now.powerType & 7;
        if (now.power[pt] != was.power[pt]) fireEvent(kPowerEvents[pt], unit);
        if (now.maxPower[pt] != was.maxPower[pt]) fireEvent(kMaxPowerEvents[pt], unit);
        if (now.level != was.level) fireEvent("UNIT_LEVEL", unit);
        if (std::strcmp(now.name, was.name) != 0) fireEvent("UNIT_NAME_UPDATE", unit);
        if (now.dead != was.dead) fireEvent("UNIT_DYNAMIC_FLAGS", unit);
    };
    diff("player", p, lastPlayer_);
    const bool targetChanged = t.guid != lastTarget_.guid || t.exists != lastTarget_.exists;
    if (targetChanged) {
        fireEvent("PLAYER_TARGET_CHANGED");
    } else {
        diff("target", t, lastTarget_);
    }
    if (p.inCombat != lastPlayer_.inCombat) fireEvent(p.inCombat ? "PLAYER_REGEN_DISABLED" : "PLAYER_REGEN_ENABLED");
    if (p.dead && !lastPlayer_.dead) fireEvent("PLAYER_DEAD");
    if (!p.dead && lastPlayer_.dead) fireEvent(p.ghost ? "PLAYER_ALIVE" : "PLAYER_UNGHOST");
    if (p.level > lastPlayer_.level && lastPlayer_.level) {
        char lv[12];
        std::snprintf(lv, sizeof(lv), "%u", static_cast<unsigned>(p.level));
        fireEvent("PLAYER_LEVEL_UP", lv);
    }
    lastPlayer_ = p;
    lastTarget_ = t;
    // Unit watches (RegisterUnitWatch): on a target change and every 0.25 s.
    const double now = api_->timeSec();
    if (targetChanged || now - lastWatchSec_ >= 0.25) {
        lastWatchSec_ = now;
        lua_getglobal(L_, "__fx_unitwatch");
        if (lua_isfunction(L_, -1)) {
            if (lua_pcall(L_, 0, 0, 0) != 0) {
                noteLuaError("unitwatch");
                lua_pop(L_, 1);
            }
        } else {
            lua_pop(L_, 1);
        }
    }
    pollGameEvents();  // action bar / cast / auras / bags events
}

void FxUi::updateLua(float dtSec) {
    if (!L_) return;
    const uint64_t t0 = api_->nowUs();
    protect([](FxUi& u, void* p) { u.updateLuaImpl(*static_cast<float*>(p)); }, &dtSec, "update");
    luaMsLast_ = static_cast<float>(api_->nowUs() - t0) / 1000.0f;
}

void FxUi::updateLuaImpl(float dtSec) {
    pollGame();
    if (!onUpdate_.empty()) {
        const std::vector<uint32_t> list = onUpdate_;
        for (uint32_t f : list) {
            if (!isVisible(f)) continue;
            lua_pushnumber(L_, static_cast<lua_Number>(dtSec));
            callScript(f, kScrOnUpdate, 1);
        }
    }
}

// PSP: the collector is stopped and stepped here, in a time budget, outside
// the GE's read of the display list (docs/PSP_LUA_NOTES.md: pause 110 as the
// start threshold, pressure gate at 85 % of the budget doubles the time).
void FxUi::stepGc() {
    if (!L_) return;
    const size_t live = alloc_.live;
    if (!gcRunning_) {
        if (gcBaseline_ == 0) gcBaseline_ = live;
        if (live < gcBaseline_ + gcBaseline_ / 10 && live < alloc_.budget * 85 / 100) {
            gcMsLast_ = 0;
            return;
        }
        gcRunning_ = true;
    }
    const uint64_t t0 = api_->nowUs();
    const bool pressure = live > alloc_.budget * 85 / 100;
    const uint64_t budgetUs = static_cast<uint64_t>(cfg_.gcBudgetMs * (pressure ? 2000.0f : 1000.0f));
    int steps = 0;
    while (true) {
        ++steps;
        if (lua_gc(L_, LUA_GCSTEP, 1)) {
            gcRunning_ = false;
            gcBaseline_ = alloc_.live;
            ++gcCycles_;
            break;
        }
        if (api_->nowUs() - t0 >= budgetUs || steps > 4096) break;
    }
    lua_gc(L_, LUA_GCSTOP, 0);  // a finished step re-arms the automatic collector
    gcMsLast_ = static_cast<float>(api_->nowUs() - t0) / 1000.0f;
}

FxStats FxUi::stats() const {
    FxStats s;
    for (const Obj& o : objs_) {
        if (o.type == kFxTexture) ++s.textures;
        else if (o.type == kFxFontString) ++s.fontStrings;
        else ++s.frames;
        if (o.luaRef) ++s.luaObjects;
        if (o.scripts >= 0) ++s.scripted;
    }
    size_t bytes = objs_.capacity() * sizeof(Obj) + names_.capacity() + scriptSets_.capacity() * sizeof(ScriptSet) +
                   extraAnchors_.capacity() * sizeof(Anchor) + byName_.size() * (sizeof(void*) * 2 + 24 + 16) +
                   tree_.capacity();
    for (const Obj& o : objs_)
        if (o.text.capacity() > 15) bytes += o.text.capacity() + 1;
    s.nativeBytes = static_cast<uint32_t>(bytes);
    s.templates = hdr_.templateCount;
    s.luaKb = static_cast<uint32_t>(alloc_.live / 1024);
    s.luaBaseKb = static_cast<uint32_t>(luaBase_ / 1024);
    s.luaPeakKb = static_cast<uint32_t>(alloc_.peak / 1024);
    s.luaFails = alloc_.fails;
    s.loadMs = loadMs_;
    s.luaErrors = luaErrors_;
    s.missingApi = missingApi_;
    s.onUpdateFrames = static_cast<uint32_t>(onUpdate_.size());
    s.eventsFired = eventsFired_;
    s.handlerCalls = handlerCalls_;
    s.drawItems = static_cast<uint32_t>(draw_.size());
    s.gcCycles = gcCycles_;
    s.gcMsLast = gcMsLast_;
    s.luaMsLast = luaMsLast_;
    s.layoutMsLast = layoutMsLast_;
    s.dynTextures = static_cast<uint32_t>(dyn_.size());
    return s;
}

bool FxUi::runString(const char* code, const char* name) {
    if (!L_) return false;
    if (luaL_loadbuffer(L_, code, std::strlen(code), name) != 0 || lua_pcall(L_, 0, 0, 0) != 0) {
        noteLuaError(name);
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

void FxUi::shutdown() {
    if (L_) lua_close(L_);
    L_ = nullptr;
    objs_.clear();
    byName_.clear();
    names_.clear();
    scriptSets_.clear();
    eventIds_.clear();
    eventFrames_.clear();
    eventNameRefs_.clear();
    onUpdate_.clear();
    order_.clear();
    draw_.clear();
    tree_.clear();
    handlerTables_.clear();
    dyn_.clear();
    dynIndex_.clear();
    frameTimes_.clear();
    bookTabs_.clear();
    bookSpells_.clear();
    bookVersion_ = ~0u;
    cursor_ = Cursor{};
    scriptDepth_ = 0;
}

// ======================================================================
// Lua bindings: widget methods

namespace {

struct W {
    FxUi* ui;
    uint32_t i;
};

W widget(lua_State* L) {
    FxUi* ui = self(L);
    const uint32_t i = ui->toObject(1);
    if (i == FxUi::kNone) luaL_error(L, "not a widget");
    ui->markDraw();  // conservative: getters too (one flag store)
    return W{ui, i};
}

int m_Show(lua_State* L) {
    W w = widget(L);
    w.ui->setShown(w.i, true);
    return 0;
}
int m_Hide(lua_State* L) {
    W w = widget(L);
    w.ui->setShown(w.i, false);
    return 0;
}
int m_IsShown(lua_State* L) {
    W w = widget(L);
    lua_pushboolean(L, (w.ui->obj(w.i).flags & FxUi::kShown) != 0);
    return 1;
}
int m_IsVisible(lua_State* L) {
    W w = widget(L);
    lua_pushboolean(L, w.ui->isVisible(w.i));
    return 1;
}
int m_GetName(lua_State* L) {
    W w = widget(L);
    if (w.ui->obj(w.i).name) lua_pushstring(L, w.ui->nameOf(w.i));
    else lua_pushnil(L);
    return 1;
}
int m_GetParent(lua_State* L) {
    W w = widget(L);
    const uint32_t p = w.ui->obj(w.i).parent;
    if (p == FxUi::kNone) lua_pushnil(L);
    else w.ui->pushObject(p);
    return 1;
}
int m_SetParent(lua_State* L) {
    W w = widget(L);
    uint32_t p = w.ui->toObject(2);
    if (p == FxUi::kNone && lua_isstring(L, 2)) p = w.ui->findByName(lua_tostring(L, 2));
    FxUi::Obj& o = w.ui->obj(w.i);
    const uint32_t old = o.parent;
    if (p == old) return 0;
    if (old != FxUi::kNone) {  // unlink
        FxUi::Obj& op = w.ui->obj(old);
        uint32_t prev = FxUi::kNone;
        for (uint32_t c = op.firstChild; c != FxUi::kNone; prev = c, c = w.ui->obj(c).next) {
            if (c != w.i) continue;
            if (prev == FxUi::kNone) op.firstChild = w.ui->obj(c).next;
            else w.ui->obj(prev).next = w.ui->obj(c).next;
            if (op.lastChild == c) op.lastChild = prev;
            break;
        }
    }
    o.parent = p;
    o.next = FxUi::kNone;
    if (p != FxUi::kNone) {
        FxUi::Obj& np = w.ui->obj(p);
        if (np.lastChild == FxUi::kNone) np.firstChild = w.i;
        else w.ui->obj(np.lastChild).next = w.i;
        np.lastChild = w.i;
        w.ui->obj(w.i).level = static_cast<int16_t>(np.level + 1);
        w.ui->obj(w.i).strata = np.strata;
    }
    w.ui->markOrder();
    w.ui->markLayout();
    return 0;
}
int m_GetObjectType(lua_State* L) {
    W w = widget(L);
    lua_pushstring(L, kTypeNames[w.ui->obj(w.i).type % kFxTypeCount]);
    return 1;
}
int m_IsObjectType(lua_State* L) {
    W w = widget(L);
    const char* t = luaL_checkstring(L, 2);
    const uint8_t ty = w.ui->obj(w.i).type;
    bool r = std::strcmp(t, kTypeNames[ty % kFxTypeCount]) == 0;
    if (!r && std::strcmp(t, "Region") == 0) r = true;
    if (!r && std::strcmp(t, "Frame") == 0) r = ty != kFxTexture && ty != kFxFontString;
    if (!r && std::strcmp(t, "Button") == 0) r = ty == kFxCheckButton;
    if (!r && std::strcmp(t, "LayeredRegion") == 0) r = ty == kFxTexture || ty == kFxFontString;
    lua_pushboolean(L, r);
    return 1;
}
int m_GetID(lua_State* L) {
    W w = widget(L);
    lua_pushnumber(L, w.ui->obj(w.i).id);
    return 1;
}
int m_SetID(lua_State* L) {
    W w = widget(L);
    w.ui->obj(w.i).id = static_cast<uint16_t>(luaL_checknumber(L, 2));
    return 0;
}
int m_SetPoint(lua_State* L) {
    W w = widget(L);
    const int point = pointOf(luaL_checkstring(L, 2));
    uint32_t rel = FxUi::kNone;
    bool relParent = true;
    int relPoint = point;
    float x = 0, y = 0;
    int argi = 3;
    if (lua_istable(L, 3) || (lua_isstring(L, 3) && !lua_isnumber(L, 3))) {
        rel = lua_istable(L, 3) ? w.ui->toObject(3) : w.ui->findByName(lua_tostring(L, 3));
        relParent = rel == FxUi::kNone;
        argi = 4;
        if (lua_isstring(L, 4) && !lua_isnumber(L, 4)) {
            relPoint = pointOf(lua_tostring(L, 4));
            argi = 5;
        }
    } else if (lua_isnil(L, 3) && lua_gettop(L) >= 4) {
        argi = 4;
        if (lua_isstring(L, 4) && !lua_isnumber(L, 4)) {
            relPoint = pointOf(lua_tostring(L, 4));
            argi = 5;
        }
    }
    x = static_cast<float>(luaL_optnumber(L, argi, 0));
    y = static_cast<float>(luaL_optnumber(L, argi + 1, 0));
    w.ui->setPoint(w.i, static_cast<uint8_t>(point), rel, static_cast<uint8_t>(relPoint), x, y, relParent);
    return 0;
}
int m_ClearAllPoints(lua_State* L) {
    W w = widget(L);
    w.ui->clearPoints(w.i);
    return 0;
}
int m_SetAllPoints(lua_State* L) {
    W w = widget(L);
    const uint32_t rel = lua_gettop(L) >= 2 ? w.ui->toObject(2) : FxUi::kNone;
    w.ui->clearPoints(w.i);
    if (rel == FxUi::kNone || rel == w.ui->obj(w.i).parent) {
        w.ui->obj(w.i).flags |= FxUi::kSetAll;
    } else {
        w.ui->setPoint(w.i, kPtTopLeft, rel, kPtTopLeft, 0, 0, false);
        w.ui->setPoint(w.i, kPtBottomRight, rel, kPtBottomRight, 0, 0, false);
    }
    w.ui->markLayout();
    return 0;
}
int m_GetNumPoints(lua_State* L) {
    W w = widget(L);
    lua_pushnumber(L, w.ui->obj(w.i).anchorCount);
    return 1;
}
int m_GetPoint(lua_State* L) {
    W w = widget(L);
    const int k = static_cast<int>(luaL_optnumber(L, 2, 1)) - 1;
    const FxUi::Obj& o = w.ui->obj(w.i);
    if (k < 0 || k >= o.anchorCount || k > 1) return 0;
    const FxUi::Anchor& a = o.anchors[k];
    lua_pushstring(L, kPtNames[a.point]);
    const uint32_t rel = a.relParent ? o.parent : a.relObj;
    if (rel == FxUi::kNone) lua_pushnil(L);
    else w.ui->pushObject(rel);
    lua_pushstring(L, kPtNames[a.relPoint]);
    lua_pushnumber(L, a.x);
    lua_pushnumber(L, a.y);
    return 5;
}
int m_SetWidth(lua_State* L) {
    W w = widget(L);
    FxUi::Obj& o = w.ui->obj(w.i);
    o.w = static_cast<float>(luaL_checknumber(L, 2));
    o.flags |= FxUi::kHasSize;
    w.ui->markLayout();
    return 0;
}
int m_SetHeight(lua_State* L) {
    W w = widget(L);
    FxUi::Obj& o = w.ui->obj(w.i);
    o.h = static_cast<float>(luaL_checknumber(L, 2));
    o.flags |= FxUi::kHasSize;
    w.ui->markLayout();
    return 0;
}
int m_SetSize(lua_State* L) {
    W w = widget(L);
    FxUi::Obj& o = w.ui->obj(w.i);
    o.w = static_cast<float>(luaL_checknumber(L, 2));
    o.h = static_cast<float>(luaL_optnumber(L, 3, o.w));
    o.flags |= FxUi::kHasSize;
    w.ui->markLayout();
    return 0;
}
int m_GetWidth(lua_State* L) {
    W w = widget(L);
    w.ui->resolve(w.i);
    const FxUi::Obj& o = w.ui->obj(w.i);
    const float es = w.ui->effScale(w.i);
    lua_pushnumber(L, (o.r - o.l) / (es > 0 ? es : 1));
    return 1;
}
int m_GetHeight(lua_State* L) {
    W w = widget(L);
    w.ui->resolve(w.i);
    const FxUi::Obj& o = w.ui->obj(w.i);
    const float es = w.ui->effScale(w.i);
    lua_pushnumber(L, (o.t - o.b) / (es > 0 ? es : 1));
    return 1;
}
int m_GetSize(lua_State* L) {
    m_GetWidth(L);
    m_GetHeight(L);
    return 2;
}
int m_GetLeft(lua_State* L) {
    W w = widget(L);
    w.ui->resolve(w.i);
    lua_pushnumber(L, w.ui->obj(w.i).l);
    return 1;
}
int m_GetRight(lua_State* L) {
    W w = widget(L);
    w.ui->resolve(w.i);
    lua_pushnumber(L, w.ui->obj(w.i).r);
    return 1;
}
int m_GetTop(lua_State* L) {
    W w = widget(L);
    w.ui->resolve(w.i);
    lua_pushnumber(L, w.ui->obj(w.i).t);
    return 1;
}
int m_GetBottom(lua_State* L) {
    W w = widget(L);
    w.ui->resolve(w.i);
    lua_pushnumber(L, w.ui->obj(w.i).b);
    return 1;
}
int m_GetCenter(lua_State* L) {
    W w = widget(L);
    w.ui->resolve(w.i);
    const FxUi::Obj& o = w.ui->obj(w.i);
    lua_pushnumber(L, (o.l + o.r) * 0.5f);
    lua_pushnumber(L, (o.b + o.t) * 0.5f);
    return 2;
}
int m_SetAlpha(lua_State* L) {
    W w = widget(L);
    float a = static_cast<float>(luaL_checknumber(L, 2));
    w.ui->obj(w.i).alpha = a < 0 ? 0 : (a > 1 ? 1 : a);
    return 0;
}
int m_GetAlpha(lua_State* L) {
    W w = widget(L);
    lua_pushnumber(L, w.ui->obj(w.i).alpha);
    return 1;
}
int m_SetScale(lua_State* L) {
    W w = widget(L);
    const float s = static_cast<float>(luaL_checknumber(L, 2));
    w.ui->obj(w.i).scale = s > 0.01f ? s : 0.01f;
    w.ui->markLayout();
    return 0;
}
int m_GetScale(lua_State* L) {
    W w = widget(L);
    lua_pushnumber(L, w.ui->obj(w.i).scale);
    return 1;
}
int m_GetEffectiveScale(lua_State* L) {
    W w = widget(L);
    lua_pushnumber(L, w.ui->effScale(w.i));
    return 1;
}
int m_SetFrameLevel(lua_State* L) {
    W w = widget(L);
    w.ui->obj(w.i).level = static_cast<int16_t>(luaL_checknumber(L, 2));
    w.ui->markOrder();
    return 0;
}
int m_GetFrameLevel(lua_State* L) {
    W w = widget(L);
    lua_pushnumber(L, w.ui->obj(w.i).level);
    return 1;
}
int m_SetFrameStrata(lua_State* L) {
    W w = widget(L);
    const char* s = luaL_checkstring(L, 2);
    for (int k = 0; k < kStrataCount; ++k)
        if (std::strcmp(s, kStrataNames[k]) == 0) {
            // children follow (upstream: strata is inherited unless set)
            std::vector<uint32_t> stack{w.i};
            while (!stack.empty()) {
                const uint32_t x = stack.back();
                stack.pop_back();
                w.ui->obj(x).strata = static_cast<uint8_t>(k);
                for (uint32_t c = w.ui->obj(x).firstChild; c != FxUi::kNone; c = w.ui->obj(c).next)
                    stack.push_back(c);
            }
            w.ui->markOrder();
            break;
        }
    return 0;
}
int m_GetFrameStrata(lua_State* L) {
    W w = widget(L);
    lua_pushstring(L, kStrataNames[w.ui->obj(w.i).strata % kStrataCount]);
    return 1;
}
int m_Raise(lua_State* L) {
    W w = widget(L);
    int16_t top = 0;
    for (auto& o : w.ui->objs())
        if (o.strata == w.ui->obj(w.i).strata && o.level > top) top = o.level;
    w.ui->obj(w.i).level = static_cast<int16_t>(top + 1);
    w.ui->markOrder();
    return 0;
}
int m_EnableMouse(lua_State* L) {
    W w = widget(L);
    if (lua_isnone(L, 2) || lua_toboolean(L, 2)) w.ui->obj(w.i).flags |= FxUi::kMouse;
    else w.ui->obj(w.i).flags &= ~FxUi::kMouse;
    return 0;
}
int m_IsMouseEnabled(lua_State* L) {
    W w = widget(L);
    lua_pushboolean(L, (w.ui->obj(w.i).flags & FxUi::kMouse) != 0);
    return 1;
}
int m_IsMouseOver(lua_State* L) {
    W w = widget(L);
    lua_pushboolean(L, (w.ui->obj(w.i).flags & FxUi::kHovered) != 0);
    return 1;
}
int m_RegisterEvent(lua_State* L) {
    W w = widget(L);
    w.ui->registerEvent(w.i, luaL_checkstring(L, 2), true);
    return 0;
}
int m_UnregisterEvent(lua_State* L) {
    W w = widget(L);
    w.ui->registerEvent(w.i, luaL_checkstring(L, 2), false);
    return 0;
}
int m_UnregisterAllEvents(lua_State* L) {
    W w = widget(L);
    w.ui->unregisterAll(w.i);
    return 0;
}
int m_SetScript(lua_State* L) {
    W w = widget(L);
    const char* name = luaL_checkstring(L, 2);
    for (int k = 0; k < kScrCount; ++k)
        if (std::strcmp(name, kScriptNames[k]) == 0) {
            w.ui->setScript(w.i, k, lua_isfunction(L, 3) ? 3 : 0);
            return 0;
        }
    return 0;
}
int m_GetScript(lua_State* L) {
    W w = widget(L);
    const char* name = luaL_checkstring(L, 2);
    for (int k = 0; k < kScrCount; ++k)
        if (std::strcmp(name, kScriptNames[k]) == 0) {
            const int ref = w.ui->scriptRef(w.i, k);
            if (ref) lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
            else lua_pushnil(L);
            return 1;
        }
    lua_pushnil(L);
    return 1;
}
int m_HasScript(lua_State* L) {
    lua_pushboolean(L, 1);
    return 1;
}
int m_HookScript(lua_State* L) {
    // Old handler then the hook (upstream HookScript): compose in Lua.
    W w = widget(L);
    const char* name = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    int kind = -1;
    for (int k = 0; k < kScrCount; ++k)
        if (std::strcmp(name, kScriptNames[k]) == 0) kind = k;
    if (kind < 0) return 0;
    const int ref = w.ui->scriptRef(w.i, kind);
    if (!ref) {
        w.ui->setScript(w.i, kind, 3);
        return 0;
    }
    lua_getglobal(L, "__fx_compose");
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    lua_pushvalue(L, 3);
    if (lua_pcall(L, 2, 1, 0) == 0) w.ui->setScript(w.i, kind, lua_gettop(L));
    lua_pop(L, 1);
    return 0;
}
int createRegion(lua_State* L, uint8_t type);  // below, with the global API
int m_CreateTexture(lua_State* L) { return createRegion(L, kFxTexture); }
int m_CreateFontString(lua_State* L) { return createRegion(L, kFxFontString); }
int m_GetChildren(lua_State* L) {
    W w = widget(L);
    int n = 0;
    for (uint32_t c = w.ui->obj(w.i).firstChild; c != FxUi::kNone; c = w.ui->obj(c).next) {
        const uint8_t t = w.ui->obj(c).type;
        if (t == kFxTexture || t == kFxFontString) continue;
        w.ui->pushObject(c);
        ++n;
    }
    return n;
}
int m_GetRegions(lua_State* L) {
    W w = widget(L);
    int n = 0;
    for (uint32_t c = w.ui->obj(w.i).firstChild; c != FxUi::kNone; c = w.ui->obj(c).next) {
        const uint8_t t = w.ui->obj(c).type;
        if (t != kFxTexture && t != kFxFontString) continue;
        w.ui->pushObject(c);
        ++n;
    }
    return n;
}
// Buttons
int m_Enable(lua_State* L) {
    W w = widget(L);
    w.ui->obj(w.i).flags |= FxUi::kEnabled;
    return 0;
}
int m_Disable(lua_State* L) {
    W w = widget(L);
    w.ui->obj(w.i).flags &= ~FxUi::kEnabled;
    return 0;
}
int m_IsEnabled(lua_State* L) {
    W w = widget(L);
    lua_pushboolean(L, (w.ui->obj(w.i).flags & FxUi::kEnabled) != 0);
    return 1;
}
int m_Click(lua_State* L) {
    W w = widget(L);
    w.ui->clickObject(w.i, false);
    return 0;
}
uint32_t childByRole(FxUi* ui, uint32_t f, uint8_t role) {
    for (uint32_t c = ui->obj(f).firstChild; c != FxUi::kNone; c = ui->obj(c).next)
        if (ui->obj(c).role == role) return c;
    return FxUi::kNone;
}
uint32_t ensureRole(FxUi* ui, uint32_t f, uint8_t role, uint8_t type, uint8_t layer) {
    uint32_t c = childByRole(ui, f, role);
    if (c != FxUi::kNone) return c;
    c = ui->createObject(type, f, nullptr);
    ui->obj(c).role = role;
    ui->obj(c).layer = layer;
    ui->obj(c).flags |= FxUi::kSetAll;
    if (role == kRoleHighlight) ui->obj(c).flags |= FxUi::kAdd;
    return c;
}
int m_SetText(lua_State* L) {
    W w = widget(L);
    const char* t = lua_isnil(L, 2) || lua_isnone(L, 2) ? "" : luaL_checkstring(L, 2);
    const uint8_t ty = w.ui->obj(w.i).type;
    if (ty == kFxFontString || ty == kFxEditBox || ty == kFxMessageFrame) {
        w.ui->setText(w.i, t);
    } else {
        w.ui->obj(w.i).text = t;
        const uint32_t c = ensureRole(w.ui, w.i, kRoleButtonText, kFxFontString, kLayerOverlay);
        w.ui->obj(c).jh = 1;
        w.ui->setText(c, t);
    }
    return 0;
}
int m_GetText(lua_State* L) {
    W w = widget(L);
    const uint8_t ty = w.ui->obj(w.i).type;
    uint32_t src = w.i;
    if (ty != kFxFontString && ty != kFxEditBox) {
        const uint32_t c = childByRole(w.ui, w.i, kRoleButtonText);
        if (c != FxUi::kNone) src = c;
    }
    if (w.ui->obj(src).text.empty()) lua_pushnil(L);
    else lua_pushstring(L, w.ui->obj(src).text.c_str());
    return 1;
}
int m_SetFormattedText(lua_State* L) {
    lua_getglobal(L, "string");
    lua_getfield(L, -1, "format");
    lua_remove(L, -2);
    const int n = lua_gettop(L) - 2;  // format + args after self, minus the function itself
    for (int k = 2; k <= n + 1; ++k) lua_pushvalue(L, k);
    lua_call(L, n, 1);
    lua_replace(L, 2);
    lua_settop(L, 2);
    return m_SetText(L);
}
int m_GetFontString(lua_State* L) {
    W w = widget(L);
    const uint32_t c = childByRole(w.ui, w.i, kRoleButtonText);
    if (c == FxUi::kNone) lua_pushnil(L);
    else w.ui->pushObject(c);
    return 1;
}
int setRoleTexture(lua_State* L, uint8_t role) {
    W w = widget(L);
    const uint8_t layer = role == kRoleHighlight ? kLayerHighlight : kLayerArtwork;
    uint32_t c;
    if (lua_istable(L, 2)) {
        c = w.ui->toObject(2);
        if (c == FxUi::kNone) return 0;
        w.ui->obj(c).role = role;
        return 0;
    }
    c = ensureRole(w.ui, w.i, role, kFxTexture, layer);
    if (lua_isstring(L, 2)) w.ui->obj(c).texture = w.ui->resolveTexture(lua_tostring(L, 2));
    else w.ui->obj(c).texture = -1;
    return 0;
}
int getRoleTexture(lua_State* L, uint8_t role) {
    W w = widget(L);
    const uint32_t c = childByRole(w.ui, w.i, role);
    if (c == FxUi::kNone) lua_pushnil(L);
    else w.ui->pushObject(c);
    return 1;
}
int m_SetNormalTexture(lua_State* L) { return setRoleTexture(L, kRoleNormal); }
int m_SetPushedTexture(lua_State* L) { return setRoleTexture(L, kRolePushed); }
int m_SetHighlightTexture(lua_State* L) { return setRoleTexture(L, kRoleHighlight); }
int m_SetDisabledTexture(lua_State* L) { return setRoleTexture(L, kRoleDisabled); }
int m_SetCheckedTexture(lua_State* L) { return setRoleTexture(L, kRoleChecked); }
int m_GetNormalTexture(lua_State* L) { return getRoleTexture(L, kRoleNormal); }
int m_GetPushedTexture(lua_State* L) { return getRoleTexture(L, kRolePushed); }
int m_GetHighlightTexture(lua_State* L) { return getRoleTexture(L, kRoleHighlight); }
int m_GetDisabledTexture(lua_State* L) { return getRoleTexture(L, kRoleDisabled); }
int m_GetCheckedTexture(lua_State* L) { return getRoleTexture(L, kRoleChecked); }
int m_GetStatusBarTexture(lua_State* L) { return getRoleTexture(L, kRoleBar); }
int m_SetStatusBarTexture(lua_State* L) {
    W w = widget(L);
    const uint32_t c = ensureRole(w.ui, w.i, kRoleBar, kFxTexture, kLayerArtwork);
    if (lua_isstring(L, 2)) w.ui->obj(c).texture = w.ui->resolveTexture(lua_tostring(L, 2));
    return 0;
}
int m_LockHighlight(lua_State* L) {
    W w = widget(L);
    w.ui->obj(w.i).flags |= FxUi::kHighlightLocked;
    return 0;
}
int m_UnlockHighlight(lua_State* L) {
    W w = widget(L);
    w.ui->obj(w.i).flags &= ~FxUi::kHighlightLocked;
    return 0;
}
int m_SetButtonState(lua_State* L) {
    W w = widget(L);
    const char* s = luaL_checkstring(L, 2);
    if (std::strcmp(s, "PUSHED") == 0) w.ui->obj(w.i).flags |= FxUi::kPushed;
    else w.ui->obj(w.i).flags &= ~FxUi::kPushed;
    return 0;
}
int m_GetButtonState(lua_State* L) {
    W w = widget(L);
    const uint32_t f = w.ui->obj(w.i).flags;
    lua_pushstring(L, !(f & FxUi::kEnabled) ? "DISABLED" : ((f & FxUi::kPushed) ? "PUSHED" : "NORMAL"));
    return 1;
}
int m_SetChecked(lua_State* L) {
    W w = widget(L);
    if (lua_toboolean(L, 2)) w.ui->obj(w.i).flags |= FxUi::kChecked;
    else w.ui->obj(w.i).flags &= ~FxUi::kChecked;
    return 0;
}
int m_GetChecked(lua_State* L) {
    W w = widget(L);
    if (w.ui->obj(w.i).flags & FxUi::kChecked) lua_pushnumber(L, 1);  // 3.3.5 answers 1 / nil
    else lua_pushnil(L);
    return 1;
}
// Status bars / sliders
int m_SetMinMaxValues(lua_State* L) {
    W w = widget(L);
    FxUi::Obj& o = w.ui->obj(w.i);
    o.minV = static_cast<float>(luaL_checknumber(L, 2));
    o.maxV = static_cast<float>(luaL_checknumber(L, 3));
    if (o.value < o.minV) o.value = o.minV;
    if (o.value > o.maxV) o.value = o.maxV;
    return 0;
}
int m_GetMinMaxValues(lua_State* L) {
    W w = widget(L);
    lua_pushnumber(L, w.ui->obj(w.i).minV);
    lua_pushnumber(L, w.ui->obj(w.i).maxV);
    return 2;
}
int m_SetValue(lua_State* L) {
    W w = widget(L);
    FxUi::Obj& o = w.ui->obj(w.i);
    float v = static_cast<float>(luaL_checknumber(L, 2));
    v = v < o.minV ? o.minV : (v > o.maxV ? o.maxV : v);
    if (v == o.value) return 0;
    o.value = v;
    if (w.ui->hasScript(w.i, kScrOnValueChanged)) {
        lua_pushnumber(L, v);
        w.ui->callScript(w.i, kScrOnValueChanged, 1);
    }
    return 0;
}
int m_GetValue(lua_State* L) {
    W w = widget(L);
    lua_pushnumber(L, w.ui->obj(w.i).value);
    return 1;
}
int m_SetStatusBarColor(lua_State* L) {
    W w = widget(L);
    w.ui->obj(w.i).color = packColor(static_cast<float>(luaL_checknumber(L, 2)),
                                     static_cast<float>(luaL_checknumber(L, 3)),
                                     static_cast<float>(luaL_checknumber(L, 4)),
                                     static_cast<float>(luaL_optnumber(L, 5, 1)));
    return 0;
}
int m_GetStatusBarColor(lua_State* L) {
    W w = widget(L);
    float c[4];
    unpackColor(w.ui->obj(w.i).color, c);
    for (float v : c) lua_pushnumber(L, v);
    return 4;
}
// Textures
int m_SetTexture(lua_State* L) {
    W w = widget(L);
    FxUi::Obj& o = w.ui->obj(w.i);
    if (lua_isnumber(L, 2)) {  // SetTexture(r, g, b[, a]): a solid colour
        o.color = packColor(static_cast<float>(lua_tonumber(L, 2)), static_cast<float>(luaL_optnumber(L, 3, 0)),
                            static_cast<float>(luaL_optnumber(L, 4, 0)), static_cast<float>(luaL_optnumber(L, 5, 1)));
        o.flags |= FxUi::kTexColor;
        o.texture = -1;
        lua_pushboolean(L, 1);
        return 1;
    }
    o.flags &= ~FxUi::kTexColor;
    o.texture = lua_isstring(L, 2) ? w.ui->resolveTexture(lua_tostring(L, 2)) : -1;  // + game icons
    if (lua_isstring(L, 2) && o.texture < 0) {
        static int logged = 0;
        if (++logged <= 30) w.ui->logf("fx_texture_missing path=%.90s", lua_tostring(L, 2));
    }
    lua_pushboolean(L, o.texture >= 0);
    return 1;
}
int m_GetTexture(lua_State* L) {
    W w = widget(L);
    const int t = w.ui->obj(w.i).texture;
    uint8_t dk = 0;
    uint32_t did = 0;
    if (w.ui->dynTexInfo(t, dk, did)) {  // the path it was set from
        lua_pushfstring(L, "Interface\\Icons\\#%s:%d", dk == kFxDynSpell ? "spell" : "item", static_cast<int>(did));
        return 1;
    }
    if (t < 0 || static_cast<uint32_t>(t) >= w.ui->header().textureCount) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, "texture");
    return 1;
}
int m_SetTexCoord(lua_State* L) {
    W w = widget(L);
    FxUi::Obj& o = w.ui->obj(w.i);
    if (lua_gettop(L) >= 9) {
        // 8-number form: UL, LL, UR, LR. PSP: axis-aligned only (sprites).
        const float ulx = static_cast<float>(lua_tonumber(L, 2)), uly = static_cast<float>(lua_tonumber(L, 3));
        const float lrx = static_cast<float>(lua_tonumber(L, 8)), lry = static_cast<float>(lua_tonumber(L, 9));
        o.tc[0] = ulx;
        o.tc[1] = lrx;
        o.tc[2] = uly;
        o.tc[3] = lry;
    } else {
        o.tc[0] = static_cast<float>(luaL_optnumber(L, 2, 0));
        o.tc[1] = static_cast<float>(luaL_optnumber(L, 3, 1));
        o.tc[2] = static_cast<float>(luaL_optnumber(L, 4, 0));
        o.tc[3] = static_cast<float>(luaL_optnumber(L, 5, 1));
    }
    return 0;
}
int m_GetTexCoord(lua_State* L) {
    W w = widget(L);
    const FxUi::Obj& o = w.ui->obj(w.i);
    // UL, LL, UR, LR
    lua_pushnumber(L, o.tc[0]);
    lua_pushnumber(L, o.tc[2]);
    lua_pushnumber(L, o.tc[0]);
    lua_pushnumber(L, o.tc[3]);
    lua_pushnumber(L, o.tc[1]);
    lua_pushnumber(L, o.tc[2]);
    lua_pushnumber(L, o.tc[1]);
    lua_pushnumber(L, o.tc[3]);
    return 8;
}
int m_SetVertexColor(lua_State* L) {
    W w = widget(L);
    FxUi::Obj& o = w.ui->obj(w.i);
    o.color = packColor(static_cast<float>(luaL_checknumber(L, 2)), static_cast<float>(luaL_checknumber(L, 3)),
                        static_cast<float>(luaL_checknumber(L, 4)), static_cast<float>(luaL_optnumber(L, 5, 1)));
    return 0;
}
int m_GetVertexColor(lua_State* L) {
    W w = widget(L);
    float c[4];
    unpackColor(w.ui->obj(w.i).color, c);
    for (float v : c) lua_pushnumber(L, v);
    return 4;
}
int m_SetDesaturated(lua_State* L) {
    W w = widget(L);
    if (lua_toboolean(L, 2)) w.ui->obj(w.i).flags |= FxUi::kDesaturated;
    else w.ui->obj(w.i).flags &= ~FxUi::kDesaturated;
    lua_pushboolean(L, 1);
    return 1;
}
int m_SetBlendMode(lua_State* L) {
    W w = widget(L);
    const char* m = luaL_checkstring(L, 2);
    if (std::strcmp(m, "ADD") == 0) w.ui->obj(w.i).flags |= FxUi::kAdd;
    else w.ui->obj(w.i).flags &= ~FxUi::kAdd;
    return 0;
}
int m_SetDrawLayer(lua_State* L) {
    W w = widget(L);
    const char* s = luaL_checkstring(L, 2);
    for (int k = 0; k < kLayerCount; ++k)
        if (std::strcmp(s, kLayerNames[k]) == 0) w.ui->obj(w.i).layer = static_cast<uint8_t>(k);
    return 0;
}
int m_GetDrawLayer(lua_State* L) {
    W w = widget(L);
    lua_pushstring(L, kLayerNames[w.ui->obj(w.i).layer % kLayerCount]);
    return 1;
}
// Font strings
int m_SetTextColor(lua_State* L) {
    W w = widget(L);
    uint32_t target = w.i;
    const uint8_t ty = w.ui->obj(w.i).type;
    if (ty != kFxFontString) {
        const uint32_t c = childByRole(w.ui, w.i, kRoleButtonText);
        if (c != FxUi::kNone) target = c;
    }
    w.ui->obj(target).color =
        packColor(static_cast<float>(luaL_checknumber(L, 2)), static_cast<float>(luaL_checknumber(L, 3)),
                  static_cast<float>(luaL_checknumber(L, 4)), static_cast<float>(luaL_optnumber(L, 5, 1)));
    return 0;
}
int m_GetTextColor(lua_State* L) { return m_GetVertexColor(L); }
int fontArg(FxUi* ui, lua_State* L, int idx) {
    if (lua_istable(L, idx)) {
        lua_rawgeti(L, idx, 0);
        const int f = lua_isnumber(L, -1) ? static_cast<int>(lua_tonumber(L, -1)) : 0;
        lua_pop(L, 1);
        return f < 0 ? -f - 1 : -1;
    }
    if (lua_isstring(L, idx)) {
        const char* n = lua_tostring(L, idx);
        for (uint32_t k = 0; k < ui->header().fontCount; ++k)
            if (std::strcmp(ui->string(ui->font(static_cast<int>(k))->name), n) == 0) return static_cast<int>(k);
    }
    return -1;
}
int m_SetFontObject(lua_State* L) {
    W w = widget(L);
    const int f = fontArg(w.ui, L, 2);
    if (f < 0) return 0;
    FxUi::Obj& o = w.ui->obj(w.i);
    o.font = static_cast<int16_t>(f);
    o.color = w.ui->font(f)->color;
    w.ui->markLayout();
    return 0;
}
int m_GetFontObject(lua_State* L) {
    W w = widget(L);
    const int f = w.ui->obj(w.i).font;
    if (f < 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_getglobal(L, w.ui->string(w.ui->font(f)->name));
    return 1;
}
int m_SetNormalFontObject(lua_State* L) {
    W w = widget(L);
    const int f = fontArg(w.ui, L, 2);
    if (f < 0) return 0;
    const uint32_t c = ensureRole(w.ui, w.i, kRoleButtonText, kFxFontString, kLayerOverlay);
    w.ui->obj(c).font = static_cast<int16_t>(f);
    w.ui->obj(c).color = w.ui->font(f)->color;
    return 0;
}
int m_SetFont(lua_State* L) {
    W w = widget(L);
    const float h = static_cast<float>(luaL_optnumber(L, 3, 12));
    FxUi::Obj& o = w.ui->obj(w.i);
    o.fontHeight = h;
    if (o.font < 0) o.font = static_cast<int16_t>(w.ui->fontForHeight(h));
    w.ui->markLayout();
    lua_pushboolean(L, 1);
    return 1;
}
int m_GetFont(lua_State* L) {
    W w = widget(L);
    const FxUi::Obj& o = w.ui->obj(w.i);
    const FxFont* f = w.ui->font(o.font);
    lua_pushstring(L, f ? w.ui->string(f->file) : "Fonts\\FRIZQT__.TTF");
    lua_pushnumber(L, o.fontHeight > 0 ? o.fontHeight : (f ? f->height : 12));
    lua_pushstring(L, "");
    return 3;
}
int m_SetJustifyH(lua_State* L) {
    W w = widget(L);
    const char* j = luaL_checkstring(L, 2);
    w.ui->obj(w.i).jh = static_cast<uint8_t>(std::strcmp(j, "LEFT") == 0 ? 0 : (std::strcmp(j, "RIGHT") == 0 ? 2 : 1));
    return 0;
}
int m_SetJustifyV(lua_State* L) {
    W w = widget(L);
    const char* j = luaL_checkstring(L, 2);
    w.ui->obj(w.i).jv = static_cast<uint8_t>(std::strcmp(j, "TOP") == 0 ? 0 : (std::strcmp(j, "BOTTOM") == 0 ? 2 : 1));
    return 0;
}
int m_GetStringWidth(lua_State* L) {
    W w = widget(L);
    lua_pushnumber(L, w.ui->textWidth(w.i));
    return 1;
}
int m_GetStringHeight(lua_State* L) {
    W w = widget(L);
    const FxUi::Obj& o = w.ui->obj(w.i);
    const FxFont* f = w.ui->font(o.font);
    lua_pushnumber(L, o.fontHeight > 0 ? o.fontHeight : (f ? f->height : 12));
    return 1;
}
int m_SetTextHeight(lua_State* L) {
    W w = widget(L);
    w.ui->obj(w.i).fontHeight = static_cast<float>(luaL_checknumber(L, 2));
    w.ui->markLayout();
    return 0;
}
// Font objects (GameFontNormal & co. as globals): t[0] = -(index + 1).
int f_GetFont(lua_State* L) {
    FxUi* ui = self(L);
    const int f = fontArg(ui, L, 1);
    const FxFont* ff = ui->font(f);
    lua_pushstring(L, ff ? ui->string(ff->file) : "Fonts\\FRIZQT__.TTF");
    lua_pushnumber(L, ff ? ff->height : 12);
    lua_pushstring(L, ff && ff->outline ? "OUTLINE" : "");
    return 3;
}
int f_GetTextColor(lua_State* L) {
    FxUi* ui = self(L);
    const FxFont* ff = ui->font(fontArg(ui, L, 1));
    float c[4];
    unpackColor(ff ? ff->color : 0xFFFFFFFFu, c);
    for (float v : c) lua_pushnumber(L, v);
    return 4;
}
int f_GetName(lua_State* L) {
    FxUi* ui = self(L);
    const FxFont* ff = ui->font(fontArg(ui, L, 1));
    if (ff) lua_pushstring(L, ui->string(ff->name));
    else lua_pushnil(L);
    return 1;
}

const luaL_Reg kRegionMethods[] = {
    {"Show", m_Show}, {"Hide", m_Hide}, {"IsShown", m_IsShown}, {"IsVisible", m_IsVisible},
    {"GetName", m_GetName}, {"GetParent", m_GetParent}, {"SetParent", m_SetParent},
    {"GetObjectType", m_GetObjectType}, {"IsObjectType", m_IsObjectType}, {"SetPoint", m_SetPoint},
    {"ClearAllPoints", m_ClearAllPoints}, {"SetAllPoints", m_SetAllPoints}, {"GetPoint", m_GetPoint},
    {"GetNumPoints", m_GetNumPoints}, {"SetWidth", m_SetWidth}, {"SetHeight", m_SetHeight},
    {"SetSize", m_SetSize}, {"GetWidth", m_GetWidth}, {"GetHeight", m_GetHeight}, {"GetSize", m_GetSize},
    {"GetLeft", m_GetLeft}, {"GetRight", m_GetRight}, {"GetTop", m_GetTop}, {"GetBottom", m_GetBottom},
    {"GetCenter", m_GetCenter}, {"SetAlpha", m_SetAlpha}, {"GetAlpha", m_GetAlpha},
    {"SetDrawLayer", m_SetDrawLayer}, {"GetDrawLayer", m_GetDrawLayer}, {"SetVertexColor", m_SetVertexColor},
    {"GetVertexColor", m_GetVertexColor}, {"GetEffectiveScale", m_GetEffectiveScale}, {nullptr, nullptr}};
const luaL_Reg kTextureMethods[] = {
    {"SetTexture", m_SetTexture}, {"GetTexture", m_GetTexture}, {"SetTexCoord", m_SetTexCoord},
    {"GetTexCoord", m_GetTexCoord}, {"SetDesaturated", m_SetDesaturated}, {"SetBlendMode", m_SetBlendMode},
    {nullptr, nullptr}};
const luaL_Reg kFontStringMethods[] = {
    {"SetText", m_SetText}, {"GetText", m_GetText}, {"SetFormattedText", m_SetFormattedText},
    {"SetTextColor", m_SetTextColor}, {"GetTextColor", m_GetTextColor}, {"SetFontObject", m_SetFontObject},
    {"GetFontObject", m_GetFontObject}, {"SetFont", m_SetFont}, {"GetFont", m_GetFont},
    {"SetJustifyH", m_SetJustifyH}, {"SetJustifyV", m_SetJustifyV}, {"GetStringWidth", m_GetStringWidth},
    {"GetStringHeight", m_GetStringHeight}, {"SetTextHeight", m_SetTextHeight}, {nullptr, nullptr}};
const luaL_Reg kFrameMethods[] = {
    {"GetID", m_GetID}, {"SetID", m_SetID}, {"SetScale", m_SetScale}, {"GetScale", m_GetScale},
    {"SetFrameLevel", m_SetFrameLevel}, {"GetFrameLevel", m_GetFrameLevel}, {"SetFrameStrata", m_SetFrameStrata},
    {"GetFrameStrata", m_GetFrameStrata}, {"Raise", m_Raise}, {"EnableMouse", m_EnableMouse},
    {"IsMouseEnabled", m_IsMouseEnabled}, {"IsMouseOver", m_IsMouseOver}, {"RegisterEvent", m_RegisterEvent},
    {"UnregisterEvent", m_UnregisterEvent}, {"UnregisterAllEvents", m_UnregisterAllEvents},
    {"SetScript", m_SetScript}, {"GetScript", m_GetScript}, {"HasScript", m_HasScript},
    {"HookScript", m_HookScript}, {"CreateTexture", m_CreateTexture}, {"CreateFontString", m_CreateFontString},
    {"GetChildren", m_GetChildren}, {"GetRegions", m_GetRegions}, {nullptr, nullptr}};
const luaL_Reg kButtonMethods[] = {
    {"Enable", m_Enable}, {"Disable", m_Disable}, {"IsEnabled", m_IsEnabled}, {"Click", m_Click},
    {"SetText", m_SetText}, {"GetText", m_GetText}, {"SetFormattedText", m_SetFormattedText},
    {"GetFontString", m_GetFontString}, {"SetTextColor", m_SetTextColor},
    {"SetNormalTexture", m_SetNormalTexture}, {"GetNormalTexture", m_GetNormalTexture},
    {"SetPushedTexture", m_SetPushedTexture}, {"GetPushedTexture", m_GetPushedTexture},
    {"SetHighlightTexture", m_SetHighlightTexture}, {"GetHighlightTexture", m_GetHighlightTexture},
    {"SetDisabledTexture", m_SetDisabledTexture}, {"GetDisabledTexture", m_GetDisabledTexture},
    {"LockHighlight", m_LockHighlight}, {"UnlockHighlight", m_UnlockHighlight},
    {"SetButtonState", m_SetButtonState}, {"GetButtonState", m_GetButtonState},
    {"SetNormalFontObject", m_SetNormalFontObject}, {"SetChecked", m_SetChecked}, {"GetChecked", m_GetChecked},
    {"SetCheckedTexture", m_SetCheckedTexture}, {"GetCheckedTexture", m_GetCheckedTexture}, {nullptr, nullptr}};
const luaL_Reg kBarMethods[] = {
    {"SetMinMaxValues", m_SetMinMaxValues}, {"GetMinMaxValues", m_GetMinMaxValues}, {"SetValue", m_SetValue},
    {"GetValue", m_GetValue}, {"SetStatusBarColor", m_SetStatusBarColor},
    {"GetStatusBarColor", m_GetStatusBarColor}, {"SetStatusBarTexture", m_SetStatusBarTexture},
    {"GetStatusBarTexture", m_GetStatusBarTexture}, {nullptr, nullptr}};
const luaL_Reg kFontObjectMethods[] = {
    {"GetFont", f_GetFont}, {"GetTextColor", f_GetTextColor}, {"GetName", f_GetName}, {nullptr, nullptr}};

void addMethods(lua_State* L, const luaL_Reg* r) {
    for (; r->name; ++r) {
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }
}

// Unknown method: a no-op that notes the name once (upstream fallback idea
// applied to widget methods). Only Capitalized keys - a script's own fields
// (self.unit, self.healthbar) must read nil when unset.
int missingMethod(lua_State* L) {
    const char* k = lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2) : nullptr;
    if (!k || !std::isupper(static_cast<unsigned char>(k[0]))) return 0;
    char n[96];
    std::snprintf(n, sizeof(n), "method:%s", k);
    self(L)->noteMissingApi(n);
    lua_pushcfunction(L, noop);
    return 1;
}

}  // namespace

// Regions/frames created by scripts.
namespace {
int createRegion(lua_State* L, uint8_t type) {
    W w = widget(L);
    const char* rawName = lua_isstring(L, 2) ? lua_tostring(L, 2) : nullptr;
    const std::string owner = w.ui->obj(w.i).name ? std::string(w.ui->nameOf(w.i)) : std::string();
    bool unnamed = false;
    const std::string name = substituteParent(rawName, owner, unnamed);
    const uint32_t r = w.ui->createObject(type, w.i, name.empty() ? nullptr : name.c_str());
    if (lua_isstring(L, 3)) {
        const char* layer = lua_tostring(L, 3);
        for (int k = 0; k < kLayerCount; ++k)
            if (std::strcmp(layer, kLayerNames[k]) == 0) w.ui->obj(r).layer = static_cast<uint8_t>(k);
    }
    if (lua_isstring(L, 4)) {
        const char* inh = lua_tostring(L, 4);
        const int f = type == kFxFontString ? fontArg(w.ui, L, 4) : -1;
        if (f >= 0) {
            w.ui->obj(r).font = static_cast<int16_t>(f);
            w.ui->obj(r).color = w.ui->font(f)->color;
        } else {
            w.ui->applyTemplate(r, inh);
        }
    }
    if (type == kFxTexture) w.ui->obj(r).color = 0xFFFFFFFFu;
    w.ui->pushObject(r);
    return 1;
}
}  // namespace


// ======================================================================
// Global API (option (b): the adapter over FxGameApi)

namespace {

bool unitArg(lua_State* L, int idx, FxUnit& u) {
    const char* tok = luaL_optstring(L, idx, "player");
    char low[32];
    size_t n = 0;
    for (; tok[n] && n < sizeof(low) - 1; ++n) low[n] = static_cast<char>(std::tolower(static_cast<unsigned char>(tok[n])));
    low[n] = 0;
    return self(L)->api()->unit(low, u) && u.exists;
}

int g_CreateFrame(lua_State* L) {
    FxUi* ui = self(L);
    const int type = typeOf(luaL_checkstring(L, 1));
    const char* name = lua_isstring(L, 2) ? lua_tostring(L, 2) : nullptr;
    uint32_t parent = ui->toObject(3);
    if (parent == FxUi::kNone) parent = ui->findByName("UIParent");
    // "$parent..." names take the parent's name (as CreateFrame in the client).
    bool unnamed = false;
    const std::string owner = ui->obj(parent).name ? std::string(ui->nameOf(parent)) : std::string();
    const std::string full = substituteParent(name, owner, unnamed);
    if (name) name = full.empty() ? nullptr : full.c_str();
    const uint32_t f = ui->createObject(static_cast<uint8_t>(type), parent, name);
    if (lua_isstring(L, 4)) {
        std::string list = lua_tostring(L, 4);
        size_t p = 0;
        while (p <= list.size()) {
            size_t c = list.find(',', p);
            std::string t = list.substr(p, c == std::string::npos ? std::string::npos : c - p);
            t.erase(0, t.find_first_not_of(" \t"));
            t.erase(t.find_last_not_of(" \t") + 1);
            if (!t.empty() && !ui->applyTemplate(f, t.c_str())) {
                char n[96];
                std::snprintf(n, sizeof(n), "template:%s", t.c_str());
                ui->noteMissingApi(n);
            }
            if (c == std::string::npos) break;
            p = c + 1;
        }
    }
    ui->pushObject(f);
    return 1;
}
int g_GetTime(lua_State* L) {
    lua_pushnumber(L, static_cast<lua_Number>(self(L)->api()->timeSec()));
    return 1;
}
int g_UnitExists(lua_State* L) {
    FxUnit u;
    if (unitArg(L, 1, u)) lua_pushnumber(L, 1);  // 3.3.5: 1 / nil
    else lua_pushnil(L);
    return 1;
}
int g_UnitName(lua_State* L) {
    FxUnit u;
    if (!unitArg(L, 1, u)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, u.name[0] ? u.name : "Unknown");
    lua_pushnil(L);  // realm
    return 2;
}
int g_UnitGUID(lua_State* L) {
    FxUnit u;
    if (!unitArg(L, 1, u)) {
        lua_pushnil(L);
        return 1;
    }
    char b[24];
    std::snprintf(b, sizeof(b), "0x%016llX", static_cast<unsigned long long>(u.guid));
    lua_pushstring(L, b);
    return 1;
}
int g_UnitHealth(lua_State* L) {
    FxUnit u;
    lua_pushnumber(L, unitArg(L, 1, u) ? static_cast<lua_Number>(u.health) : 0);
    return 1;
}
int g_UnitHealthMax(lua_State* L) {
    FxUnit u;
    lua_pushnumber(L, unitArg(L, 1, u) ? static_cast<lua_Number>(u.maxHealth) : 0);
    return 1;
}
int g_UnitPower(lua_State* L) {
    FxUnit u;
    const bool ok = unitArg(L, 1, u);
    const int t = lua_isnumber(L, 2) ? static_cast<int>(lua_tonumber(L, 2)) & 7 : u.powerType & 7;
    lua_pushnumber(L, ok ? static_cast<lua_Number>(u.power[t]) : 0);
    return 1;
}
int g_UnitPowerMax(lua_State* L) {
    FxUnit u;
    const bool ok = unitArg(L, 1, u);
    const int t = lua_isnumber(L, 2) ? static_cast<int>(lua_tonumber(L, 2)) & 7 : u.powerType & 7;
    lua_pushnumber(L, ok ? static_cast<lua_Number>(u.maxPower[t]) : 0);
    return 1;
}
int g_UnitPowerType(lua_State* L) {
    FxUnit u;
    const bool ok = unitArg(L, 1, u);
    static const char* const kTokens[8] = {"MANA", "RAGE", "FOCUS", "ENERGY", "HAPPINESS", "RUNES", "RUNIC_POWER",
                                           "MANA"};
    const int t = ok ? (u.powerType & 7) : 0;
    lua_pushnumber(L, t);
    lua_pushstring(L, kTokens[t]);
    return 2;
}
int g_UnitLevel(lua_State* L) {
    FxUnit u;
    lua_pushnumber(L, unitArg(L, 1, u) ? static_cast<lua_Number>(u.level) : 0);
    return 1;
}
int g_UnitIsDead(lua_State* L) {
    FxUnit u;
    lua_pushboolean(L, unitArg(L, 1, u) && u.dead && !u.ghost);
    return 1;
}
int g_UnitIsGhost(lua_State* L) {
    FxUnit u;
    lua_pushboolean(L, unitArg(L, 1, u) && u.ghost);
    return 1;
}
int g_UnitIsDeadOrGhost(lua_State* L) {
    FxUnit u;
    lua_pushboolean(L, unitArg(L, 1, u) && (u.dead || u.ghost));
    return 1;
}
int g_UnitIsPlayer(lua_State* L) {
    FxUnit u;
    lua_pushboolean(L, unitArg(L, 1, u) && u.isPlayer);
    return 1;
}
int g_UnitPlayerControlled(lua_State* L) { return g_UnitIsPlayer(L); }
int g_UnitIsConnected(lua_State* L) {
    FxUnit u;
    lua_pushboolean(L, unitArg(L, 1, u) && u.connected);
    return 1;
}
int g_UnitAffectingCombat(lua_State* L) {
    FxUnit u;
    lua_pushboolean(L, unitArg(L, 1, u) && u.inCombat);
    return 1;
}
int g_UnitCanAttack(lua_State* L) {
    FxUnit u;
    lua_pushboolean(L, unitArg(L, 2, u) && u.canAttack);
    return 1;
}
int g_UnitIsFriend(lua_State* L) {
    FxUnit u;
    lua_pushboolean(L, unitArg(L, 2, u) && u.isFriend);
    return 1;
}
int g_UnitIsEnemy(lua_State* L) {
    FxUnit u;
    lua_pushboolean(L, unitArg(L, 2, u) && !u.isFriend);
    return 1;
}
int g_UnitReaction(lua_State* L) {
    FxUnit u;
    if (!unitArg(L, 2, u)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, u.canAttack ? 2 : (u.isFriend ? 5 : 4));
    return 1;
}
int g_UnitIsUnit(lua_State* L) {
    FxUnit a, b;
    lua_pushboolean(L, unitArg(L, 1, a) && unitArg(L, 2, b) && a.guid == b.guid);
    return 1;
}
int g_UnitSex(lua_State* L) {
    FxUnit u;
    lua_pushnumber(L, unitArg(L, 1, u) ? u.sex + 2 : 1);
    return 1;
}
int g_UnitRace(lua_State* L) {
    static const char* const kRaces[12] = {"", "Human", "Orc", "Dwarf", "NightElf", "Scourge", "Tauren", "Gnome",
                                           "Troll", "Goblin", "BloodElf", "Draenei"};
    FxUnit u;
    if (!unitArg(L, 1, u) || !u.isPlayer || u.race >= 12) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, kRaces[u.race]);
    lua_pushstring(L, kRaces[u.race]);
    return 2;
}
int g_UnitClass(lua_State* L) {
    static const char* const kClasses[12] = {"", "WARRIOR", "PALADIN", "HUNTER", "ROGUE", "PRIEST", "DEATHKNIGHT",
                                             "SHAMAN", "MAGE", "WARLOCK", "", "DRUID"};
    FxUnit u;
    if (!unitArg(L, 1, u) || u.classId >= 12 || !kClasses[u.classId][0]) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, kClasses[u.classId]);
    lua_pushstring(L, kClasses[u.classId]);
    lua_pushnumber(L, u.classId);
    return 3;
}
int g_UnitClassification(lua_State* L) {
    FxUnit u;
    if (!unitArg(L, 1, u)) {
        lua_pushnil(L);
        return 1;
    }
    // CreatureTemplate rank -> the token (upstream lua_unit_api UnitClassification).
    static const char* const kRanks[5] = {"normal", "elite", "rareelite", "worldboss", "rare"};
    lua_pushstring(L, u.rank < 5 ? kRanks[u.rank] : "normal");
    return 1;
}
int g_UnitIsTapped(lua_State* L) {
    lua_pushboolean(L, 0);
    return 1;
}
int g_falseFn(lua_State* L) {
    lua_pushboolean(L, 0);
    return 1;
}
int g_nilFn(lua_State* L) {
    lua_pushnil(L);
    return 1;
}
int g_zeroFn(lua_State* L) {
    lua_pushnumber(L, 0);
    return 1;
}
int g_IsResting(lua_State* L) {
    lua_pushboolean(L, self(L)->api()->isResting());
    return 1;
}
int g_GetMoney(lua_State* L) {
    lua_pushnumber(L, static_cast<lua_Number>(self(L)->api()->money()));
    return 1;
}
int g_TargetUnit(lua_State* L) {
    const char* t = luaL_optstring(L, 1, "player");
    self(L)->api()->targetUnit(t);
    return 0;
}
int g_ClearTarget(lua_State* L) {
    self(L)->api()->clearTarget();
    return 0;
}
int g_GetCVar(lua_State* L) {
    FxUi* ui = self(L);
    auto it = ui->cvars_.find(luaL_checkstring(L, 1));
    if (it == ui->cvars_.end()) lua_pushnil(L);
    else lua_pushstring(L, it->second.c_str());
    return 1;
}
int g_GetCVarBool(lua_State* L) {
    FxUi* ui = self(L);
    auto it = ui->cvars_.find(luaL_checkstring(L, 1));
    if (it != ui->cvars_.end() && it->second != "0" && !it->second.empty()) lua_pushnumber(L, 1);
    else lua_pushnil(L);
    return 1;
}
int g_SetCVar(lua_State* L) {
    FxUi* ui = self(L);
    const char* k = luaL_checkstring(L, 1);
    const char* v = lua_isnoneornil(L, 2) ? "" : luaL_checkstring(L, 2);
    ui->cvars_[k] = v;
    return 0;
}
int g_RegisterCVar(lua_State* L) {
    FxUi* ui = self(L);
    const char* k = luaL_checkstring(L, 1);
    if (!ui->cvars_.count(k)) ui->cvars_[k] = lua_isstring(L, 2) ? lua_tostring(L, 2) : "";
    return 0;
}
int g_GetLocale(lua_State* L) {
    lua_pushstring(L, self(L)->cfg().locale.c_str());  // was "ruRU" (the one language there was)
    return 1;
}
int g_GetBuildInfo(lua_State* L) {
    lua_pushstring(L, "3.3.5");
    lua_pushstring(L, "12340");
    lua_pushstring(L, "Jun 24 2010");
    lua_pushnumber(L, 30300);
    return 4;
}
int g_GetScreenWidth(lua_State* L) {
    FxUi* ui = self(L);
    lua_pushnumber(L, ui->cfg().screenW / ui->cfg().uiScale);
    return 1;
}
int g_GetScreenHeight(lua_State* L) {
    FxUi* ui = self(L);
    lua_pushnumber(L, ui->cfg().screenH / ui->cfg().uiScale);
    return 1;
}
int g_SetPortraitTexture(lua_State* L) {
    // PSP: no 3D portrait render; a dark disc stands in (the art frames it).
    FxUi* ui = self(L);
    const uint32_t t = ui->toObject(1);
    if (t != FxUi::kNone) {
        ui->obj(t).texture = -1;
        ui->obj(t).flags |= FxUi::kTexColor;
        ui->obj(t).color = 0xFF202020u;
    }
    return 0;
}
int g_bit_band(lua_State* L) {
    uint32_t r = 0xFFFFFFFFu;
    for (int k = 1; k <= lua_gettop(L); ++k) r &= static_cast<uint32_t>(static_cast<int64_t>(luaL_checknumber(L, k)));
    lua_pushnumber(L, static_cast<lua_Number>(r));
    return 1;
}
int g_bit_bor(lua_State* L) {
    uint32_t r = 0;
    for (int k = 1; k <= lua_gettop(L); ++k) r |= static_cast<uint32_t>(static_cast<int64_t>(luaL_checknumber(L, k)));
    lua_pushnumber(L, static_cast<lua_Number>(r));
    return 1;
}
int g_bit_bxor(lua_State* L) {
    uint32_t r = 0;
    for (int k = 1; k <= lua_gettop(L); ++k) r ^= static_cast<uint32_t>(static_cast<int64_t>(luaL_checknumber(L, k)));
    lua_pushnumber(L, static_cast<lua_Number>(r));
    return 1;
}
int g_bit_lshift(lua_State* L) {
    const uint32_t a = static_cast<uint32_t>(static_cast<int64_t>(luaL_checknumber(L, 1)));
    lua_pushnumber(L, static_cast<lua_Number>(a << (static_cast<int>(luaL_checknumber(L, 2)) & 31)));
    return 1;
}
int g_bit_rshift(lua_State* L) {
    const uint32_t a = static_cast<uint32_t>(static_cast<int64_t>(luaL_checknumber(L, 1)));
    lua_pushnumber(L, static_cast<lua_Number>(a >> (static_cast<int>(luaL_checknumber(L, 2)) & 31)));
    return 1;
}
// strsplit(delimiters, str[, pieces]) (upstream lua_system_api lua_strsplit).
int g_strsplit(lua_State* L) {
    const char* delim = luaL_checkstring(L, 1);
    size_t len = 0;
    const char* s = luaL_checklstring(L, 2, &len);
    const int limit = static_cast<int>(luaL_optnumber(L, 3, 0));
    int n = 0;
    size_t start = 0;
    for (size_t i = 0; i <= len; ++i) {
        const bool end = i == len;
        if (!end && !std::strchr(delim, s[i])) continue;
        if (!end && limit > 0 && n == limit - 1) continue;
        lua_pushlstring(L, s + start, i - start);
        ++n;
        start = i + 1;
        if (!lua_checkstack(L, 2)) break;
    }
    return n;
}
int g_strtrim(lua_State* L) {
    size_t len = 0;
    const char* s = luaL_checklstring(L, 1, &len);
    const char* chars = luaL_optstring(L, 2, " \t\r\n");
    size_t a = 0, b = len;
    while (a < b && std::strchr(chars, s[a])) ++a;
    while (b > a && std::strchr(chars, s[b - 1])) --b;
    lua_pushlstring(L, s + a, b - a);
    return 1;
}
int g_wipe(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_pushnil(L);
    while (lua_next(L, 1)) {
        lua_pop(L, 1);
        lua_pushvalue(L, -1);
        lua_pushnil(L);
        lua_rawset(L, 1);
    }
    lua_settop(L, 1);
    return 1;
}
int g_print(lua_State* L) {
    const char* s = lua_tostring(L, 1);
    self(L)->logf("lua_print %.100s", s ? s : "nil");
    return 0;
}

// _G.__index: named widgets appear as globals on first use; unknown API names
// answer a no-op (upstream installMissingApiFallback rules: CONSTANTS, names
// with digits and Blizzard_ namespaces stay nil).
int globalIndex(lua_State* L) {
    FxUi* ui = self(L);
    if (!lua_isstring(L, 2)) return 0;
    const char* k = lua_tostring(L, 2);
    const uint32_t o = ui->findByName(k);
    if (o != FxUi::kNone) {
        ui->pushObject(o);
        return 1;
    }
    if (!ui->cfg().apiFallback) return 0;
    bool upper = true, hasLower = false, digit = false;
    for (const char* p = k; *p; ++p) {
        if (std::islower(static_cast<unsigned char>(*p))) hasLower = true, upper = false;
        if (std::isdigit(static_cast<unsigned char>(*p))) digit = true;
    }
    if ((upper && !hasLower) || digit || std::strncmp(k, "Blizzard_", 9) == 0 || k[0] == '_') return 0;
    ui->noteMissingApi(k);
    lua_pushcfunction(L, noop);
    return 1;
}

// PSP-owned Lua prelude: the client's Lua-library aliases FrameXML expects
// (upstream registers the same names in C in lua_system_api.cpp).
const char kPrelude[] = R"LUA(
local string, table, math = string, table, math
format = string.format; strlen = string.len; strsub = string.sub; strfind = string.find
strlower = string.lower; strupper = string.upper; strrep = string.rep; strbyte = string.byte
strchar = string.char; gsub = string.gsub; strmatch = string.match; gmatch = string.gmatch
strgfind = string.gmatch
tinsert = table.insert; tremove = table.remove; sort = table.sort; getn = table.getn or function(t) return #t end
floor = math.floor; ceil = math.ceil; abs = math.abs; min = math.min; max = math.max; sqrt = math.sqrt
mod = math.fmod; random = math.random; exp = math.exp; log = math.log; PI = math.pi
function tContains(t, v) for _, x in pairs(t) do if x == v then return 1 end end return nil end
function getglobal(n) return _G[n] end
function setglobal(n, v) _G[n] = v end
function strjoin(d, ...) return table.concat({...}, d) end
function hooksecurefunc(t, name, fn)
  if type(t) ~= "table" then t, name, fn = _G, t, name end
  local old = t[name]
  if type(old) ~= "function" then t[name] = fn return end
  t[name] = function(...) local a, b, c, d, e = old(...) fn(...) return a, b, c, d, e end
end
function __fx_compose(a, b) return function(...) a(...) return b(...) end end
function issecure() return 1 end
function issecurevariable() return 1, nil end
function securecall(f, ...) if type(f) == "string" then f = _G[f] end if f then return f(...) end end
function InCombatLockdown() return nil end
function geterrorhandler() return function(e) print(e) end end
function seterrorhandler() end
function debugstack() return "" end
function IsShiftKeyDown() return nil end
IsControlKeyDown = IsShiftKeyDown; IsAltKeyDown = IsShiftKeyDown; IsModifiedClick = IsShiftKeyDown
function GetNumPartyMembers() return 0 end
function GetNumRaidMembers() return 0 end
function IsInInstance() return nil, "none" end
function GetRaidTargetIndex() return nil end
function GetComboPoints() return 0 end
function UnitInParty() return nil end
function UnitInRaid() return nil end
function UnitIsPartyLeader() return nil end
function UnitIsPVP() return nil end
function UnitIsPVPFreeForAll() return nil end
function UnitHasVehicleUI() return nil end
function UnitFactionGroup(u) return nil end
-- RegisterUnitWatch (SecureStateDriver.lua): shown while the unit exists.
-- PSP: checked when the target changes and every 0.25 s (FxUi::updateLua),
-- not by an OnUpdate frame; a baked SecureStateDriver.lua replaces these.
__fx_watch = {}
local function watchStep(f)
  local u = f:GetAttribute("unit") or rawget(f, "unit")
  if u and UnitExists(u) then f:Show() else f:Hide() end
end
function RegisterUnitWatch(f) __fx_watch[f] = true watchStep(f) end
function UnregisterUnitWatch(f) __fx_watch[f] = nil end
function __fx_unitwatch() for f in pairs(__fx_watch) do watchStep(f) end end
local F = __fx_methods
for _, t in pairs(F) do
  function t:SetAttribute(k, v)
    local a = rawget(self, "__attrs") if not a then a = {} rawset(self, "__attrs", a) end
    a[k] = v
    local f = self:GetScript("OnAttributeChanged") if f then f(self, k, v) end
  end
  function t:GetAttribute(k) local a = rawget(self, "__attrs") return a and a[k] end
  function t:RegisterForClicks() end
  function t:RegisterForDrag() end
  function t:SetMovable() end
  function t:SetToplevel() end
  function t:SetClampedToScreen() end
  function t:SetHitRectInsets() end
  function t:EnableKeyboard() end
  function t:EnableMouseWheel() end
  function t:IsProtected() return nil end
  function t:SetBackdropColor() end
  function t:SetBackdropBorderColor() end
end
)LUA";

}  // namespace

void FxUi::installApi() {
    lua_State* L = L_;
    // Method tables per type, composed from groups; unknown methods -> noop.
    lua_newtable(L);  // __fx_methods (by type name)
    for (int t = 0; t < kFxTypeCount; ++t) {
        lua_newtable(L);  // methods
        addMethods(L, kRegionMethods);
        if (t == kFxTexture) {
            addMethods(L, kTextureMethods);
        } else if (t == kFxFontString) {
            addMethods(L, kFontStringMethods);
        } else {
            addMethods(L, kFrameMethods);
            if (t == kFxButton || t == kFxCheckButton) addMethods(L, kButtonMethods);
            if (t == kFxStatusBar || t == kFxSlider) addMethods(L, kBarMethods);
            if (t == kFxEditBox || t == kFxMessageFrame) addMethods(L, kFontStringMethods);
        }
        lua_newtable(L);  // methods' metatable: missing -> noop
        lua_pushcfunction(L, missingMethod);
        lua_setfield(L, -2, "__index");
        lua_setmetatable(L, -2);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, kTypeNames[t]);
        lua_newtable(L);  // the widget metatable
        lua_insert(L, -2);
        lua_setfield(L, -2, "__index");
        methodsRef_[t] = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    lua_setglobal(L, "__fx_methods");

    static const luaL_Reg kGlobals[] = {
        {"CreateFrame", g_CreateFrame}, {"GetTime", g_GetTime}, {"UnitExists", g_UnitExists},
        {"UnitName", g_UnitName}, {"UnitGUID", g_UnitGUID}, {"UnitHealth", g_UnitHealth},
        {"UnitHealthMax", g_UnitHealthMax}, {"UnitPower", g_UnitPower}, {"UnitPowerMax", g_UnitPowerMax},
        {"UnitMana", g_UnitPower}, {"UnitManaMax", g_UnitPowerMax}, {"UnitPowerType", g_UnitPowerType},
        {"UnitLevel", g_UnitLevel}, {"UnitIsDead", g_UnitIsDead}, {"UnitIsGhost", g_UnitIsGhost},
        {"UnitIsDeadOrGhost", g_UnitIsDeadOrGhost}, {"UnitIsPlayer", g_UnitIsPlayer},
        {"UnitPlayerControlled", g_UnitPlayerControlled}, {"UnitIsConnected", g_UnitIsConnected},
        {"UnitAffectingCombat", g_UnitAffectingCombat}, {"UnitCanAttack", g_UnitCanAttack},
        {"UnitIsFriend", g_UnitIsFriend}, {"UnitIsEnemy", g_UnitIsEnemy}, {"UnitReaction", g_UnitReaction},
        {"UnitIsUnit", g_UnitIsUnit}, {"UnitSex", g_UnitSex}, {"UnitRace", g_UnitRace}, {"UnitClass", g_UnitClass},
        {"UnitClassification", g_UnitClassification}, {"UnitIsTapped", g_UnitIsTapped},
        {"UnitIsTappedByPlayer", g_falseFn}, {"UnitIsTrivial", g_falseFn}, {"UnitIsCorpse", g_falseFn},
        {"UnitCreatureType", g_nilFn}, {"IsResting", g_IsResting}, {"GetMoney", g_GetMoney},
        {"TargetUnit", g_TargetUnit}, {"ClearTarget", g_ClearTarget}, {"GetCVar", g_GetCVar},
        {"GetCVarBool", g_GetCVarBool}, {"SetCVar", g_SetCVar}, {"RegisterCVar", g_RegisterCVar},
        {"GetCVarDefault", g_GetCVar}, {"GetLocale", g_GetLocale}, {"GetBuildInfo", g_GetBuildInfo},
        {"GetScreenWidth", g_GetScreenWidth}, {"GetScreenHeight", g_GetScreenHeight},
        {"SetPortraitTexture", g_SetPortraitTexture}, {"GetNumShapeshiftForms", g_zeroFn},
        {"strsplit", g_strsplit}, {"strtrim", g_strtrim}, {"wipe", g_wipe}, {"table_wipe", g_wipe},
        {"print", g_print}, {nullptr, nullptr}};
    for (const luaL_Reg* r = kGlobals; r->name; ++r) {
        lua_pushcfunction(L, r->func);
        lua_setglobal(L, r->name);
    }
    lua_newtable(L);
    lua_pushcfunction(L, g_bit_band);
    lua_setfield(L, -2, "band");
    lua_pushcfunction(L, g_bit_bor);
    lua_setfield(L, -2, "bor");
    lua_pushcfunction(L, g_bit_bxor);
    lua_setfield(L, -2, "bxor");
    lua_pushcfunction(L, g_bit_lshift);
    lua_setfield(L, -2, "lshift");
    lua_pushcfunction(L, g_bit_rshift);
    lua_setfield(L, -2, "rshift");
    lua_setglobal(L, "bit");

    if (luaL_loadbuffer(L, kPrelude, sizeof(kPrelude) - 1, "=fx_prelude") != 0 || lua_pcall(L, 0, 0, 0) != 0) {
        noteLuaError("prelude");
        lua_pop(L, 1);
    }
    installGameApi();  // action bar, spells, auras, cast, bags, PlaySound (fx_runtime_game.cpp)
    // Font objects as globals (GameFontNormal:GetFont() ...).
    lua_newtable(L);  // font object metatable
    lua_newtable(L);
    addMethods(L, kFontObjectMethods);
    lua_setfield(L, -2, "__index");
    for (uint32_t f = 0; f < hdr_.fontCount; ++f) {
        lua_createtable(L, 0, 1);
        lua_pushnumber(L, -static_cast<lua_Number>(f) - 1);
        lua_rawseti(L, -2, 0);
        lua_pushvalue(L, -2);
        lua_setmetatable(L, -2);
        lua_setglobal(L, string(fonts_[f].name));
    }
    lua_pop(L, 1);
    // _G metatable last, so the prelude's globals are plain sets.
    lua_pushvalue(L, LUA_GLOBALSINDEX);
    lua_newtable(L);
    lua_pushcfunction(L, globalIndex);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_pop(L, 1);
}

// ======================================================================
// load

bool FxUi::load(const void* tree, uint32_t size, ChunkReader reader, void* readerCtx, FxGameApi* api,
                const FxConfig& cfg) {
    if (!tree) return false;
    std::vector<uint8_t> copy(static_cast<const uint8_t*>(tree), static_cast<const uint8_t*>(tree) + size);
    return load(std::move(copy), reader, readerCtx, api, cfg);
}

bool FxUi::load(std::vector<uint8_t>&& tree, ChunkReader reader, void* readerCtx, FxGameApi* api,
                const FxConfig& cfg) {
    shutdown();
    api_ = api;
    cfg_ = cfg;
    if (cfg_.uiScale <= 0.05f) cfg_.uiScale = 0.6f;
    uiW_ = cfg_.screenW / cfg_.uiScale;
    uiH_ = cfg_.screenH / cfg_.uiScale;
    const uint64_t t0 = api_->nowUs();
    const uint32_t size = static_cast<uint32_t>(tree.size());
    if (size < sizeof(FxHeader)) return false;
    tree_.swap(tree);
    std::memcpy(&hdr_, tree_.data(), sizeof(hdr_));
    if (std::memcmp(hdr_.magic, kFxMagic, 4) != 0 || hdr_.version != kFxVersion) {
        logf("framexml_bad_tree version=%u", static_cast<unsigned>(hdr_.version));
        return false;
    }
    if (hdr_.luaNumberSize != sizeof(lua_Number)) {
        logf("framexml_lua_number_mismatch pak=%u exe=%u", static_cast<unsigned>(hdr_.luaNumberSize),
             static_cast<unsigned>(sizeof(lua_Number)));
        return false;
    }
    auto sect = [&](uint32_t off, uint64_t bytes) -> const uint8_t* {
        return (off + bytes <= size) ? tree_.data() + off : nullptr;
    };
    nodes_ = reinterpret_cast<const FxNode*>(sect(hdr_.nodeOffset, uint64_t(hdr_.nodeCount) * sizeof(FxNode)));
    anchors_ = reinterpret_cast<const FxAnchor*>(sect(hdr_.anchorOffset, uint64_t(hdr_.anchorCount) * sizeof(FxAnchor)));
    scripts_ = reinterpret_cast<const FxScript*>(sect(hdr_.scriptOffset, uint64_t(hdr_.scriptCount) * sizeof(FxScript)));
    fonts_ = reinterpret_cast<const FxFont*>(sect(hdr_.fontOffset, uint64_t(hdr_.fontCount) * sizeof(FxFont)));
    textures_ = reinterpret_cast<const FxTexture*>(sect(hdr_.textureOffset, uint64_t(hdr_.textureCount) * sizeof(FxTexture)));
    ops_ = reinterpret_cast<const FxOp*>(sect(hdr_.opOffset, uint64_t(hdr_.opCount) * sizeof(FxOp)));
    chunks_ = reinterpret_cast<const FxChunk*>(sect(hdr_.chunkOffset, uint64_t(hdr_.chunkCount) * sizeof(FxChunk)));
    templates_ = reinterpret_cast<const FxTemplate*>(sect(hdr_.templateOffset, uint64_t(hdr_.templateCount) * sizeof(FxTemplate)));
    backdrops_ = reinterpret_cast<const FxBackdrop*>(sect(hdr_.backdropOffset, uint64_t(hdr_.backdropCount) * sizeof(FxBackdrop)));
    strings_ = reinterpret_cast<const char*>(sect(hdr_.stringsOffset, hdr_.stringsSize));
    if (!nodes_ || !anchors_ || !scripts_ || !fonts_ || !textures_ || !ops_ || !chunks_ || !templates_ ||
        !backdrops_ || !strings_) {
        logf("framexml_bad_tree sections=1");
        return false;
    }
    reader_ = reader;
    readerCtx_ = readerCtx;
    handlerTables_.assign(hdr_.chunkCount, 0);
    for (uint32_t t = 0; t < hdr_.textureCount; ++t) texByPath_[string(textures_[t].path)] = static_cast<int>(t);
    names_.assign(1, '\0');
    objs_.reserve(hdr_.nodeCount + 64);

    alloc_ = Alloc{};
    alloc_.budget = static_cast<size_t>(cfg_.luaBudgetKb) * 1024u;
    alloc_.owner = this;
    L_ = lua_newstate(fxAlloc, &alloc_);
    if (!L_) {
        logf("lua_state_fail budget_kb=%u", static_cast<unsigned>(cfg_.luaBudgetKb));
        return false;
    }
    lua_gc(L_, LUA_GCSTOP, 0);  // stepped per frame (stepGc)
    lua_atpanic(L_, panicLog);
    lua_pushlightuserdata(L_, &kSelfKey);  // before anything can fail: the panic handler logs through it
    lua_pushlightuserdata(L_, this);
    lua_rawset(L_, LUA_REGISTRYINDEX);
    // CVars the unit frames read (PSP: numbers on the bars always, like the
    // built-in HUD; no target-of-target unit yet).
    cvars_["statusText"] = "1";
    cvars_["statusTextPercentage"] = "0";
    cvars_["showTargetOfTarget"] = "0";
    cvars_["showPartyBackground"] = "0";
    cvars_["uiScale"] = "1";
    // The state's setup, protected too: a budget below the base fails the
    // load (logged) instead of reaching Lua's panic.
    if (!protect([](FxUi& u, void*) { u.setupStateImpl(); }, nullptr, "setup")) {
        logf("framexml_lua_setup_fail budget_kb=%u", static_cast<unsigned>(cfg_.luaBudgetKb));
        shutdown();
        return false;
    }
    luaBase_ = alloc_.live;
    // UIParent / WorldFrame (upstream WidgetTree: the screen and UIParent).
    uiParent_ = createObject(kFxFrame, kNone, "UIParent");
    objs_[uiParent_].level = 0;
    const uint32_t world = createObject(kFxFrame, kNone, "WorldFrame");
    objs_[world].flags |= kSetAll;
    const uint64_t tl = api_->nowUs();
    protect([](FxUi& u, void*) { u.loadProgramImpl(); }, nullptr, "load");
    const uint64_t tr = api_->nowUs();
    polledOnce_ = false;
    lua_gc(L_, LUA_GCCOLLECT, 0);  // once, at load (not in a frame)
    lua_gc(L_, LUA_GCSTOP, 0);
    gcBaseline_ = alloc_.live;
    loadMs_ = static_cast<uint32_t>((api_->nowUs() - t0) / 1000u);
    const FxStats s = stats();
    logf("framexml_load frames=%u regions=%u kb=%u lua_kb=%u ms=%u run_ms=%u errors=%u",
         static_cast<unsigned>(s.frames), static_cast<unsigned>(s.textures + s.fontStrings),
         static_cast<unsigned>(s.nativeBytes / 1024), static_cast<unsigned>(s.luaKb), static_cast<unsigned>(loadMs_),
         static_cast<unsigned>((tr - tl) / 1000u), static_cast<unsigned>(luaErrors_));
    return true;
}

// ======================================================================
// Micro benchmark (PSP: logged as lua_bench at world entry)

std::vector<FxUi::BenchResult> FxUi::bench(FxGameApi* api, uint32_t scale) {
    std::vector<BenchResult> out;
    Alloc a;
    a.budget = 4u << 20;
    lua_State* L = lua_newstate(fxAlloc, &a);
    if (!L) return out;
    // Automatic GC here (pause/stepmul of luaconf.h): the loops make garbage.
    for (lua_CFunction open : {luaopen_base, luaopen_table, luaopen_string, luaopen_math}) {
        lua_pushcfunction(L, open);
        lua_call(L, 0, 0);
    }
    // A stand-in FxUi so UnitHealth & co. reach the api.
    FxUi dummy;
    dummy.api_ = api;
    lua_pushlightuserdata(L, &kSelfKey);
    lua_pushlightuserdata(L, &dummy);
    lua_rawset(L, LUA_REGISTRYINDEX);
    lua_pushcfunction(L, g_UnitHealth);
    lua_setglobal(L, "UnitHealth");
    lua_pushcfunction(L, noop);
    lua_setglobal(L, "noop");
    const uint32_t n = scale ? scale : 20000;
    struct Case {
        const char* name;
        const char* code;
        uint32_t opsPerIter;
    };
    // Each body runs n iterations; ops per iteration ~ VM instructions of interest.
    const Case cases[] = {
        {"forloop", "local n=... for i=1,n do end", 1},
        {"arith", "local n=... local x=0.5 for i=1,n do x=x*1.0001+0.25 end return x", 1},
        {"table_rw", "local n=... local t={a=1} for i=1,n do t.a=t.a+1 end", 1},
        {"lua_call", "local n=... local function f(a) return a end for i=1,n do f(i) end", 1},
        {"c_call", "local n=... local f=noop for i=1,n do f(i) end", 1},
        {"api_call", "local n=... local f=UnitHealth for i=1,n do f('player') end", 1},
        {"closure", "local n=... for i=1,n do local f=function() return i end end", 1},
        {"concat", "local n=... local s for i=1,n do s='hp '..i end", 1},
        {"format", "local n=... local f=string.format for i=1,n do f('%d / %d', i, n) end", 1},
        {"event_tbl", "local n=... local h={} for i=1,10 do h[i]=function(self,ev,a1) return a1 end end "
                      "for i=1,n/10 do for k=1,10 do h[k](h,'UNIT_HEALTH','player') end end", 1},
    };
    for (const Case& c : cases) {
        if (luaL_loadstring(L, c.code) != 0) {
            lua_pop(L, 1);
            continue;
        }
        lua_pushnumber(L, static_cast<lua_Number>(n));
        const uint64_t t0 = api->nowUs();
        const int rc = lua_pcall(L, 1, 0, 0);
        const uint64_t us = api->nowUs() - t0;
        if (rc != 0) lua_pop(L, 1);
        out.push_back(BenchResult{c.name, rc == 0 ? static_cast<float>(us) / static_cast<float>(n * c.opsPerIter)
                                                  : -1.0f, n});
        lua_gc(L, LUA_GCCOLLECT, 0);
    }
    // One GC step on a heap with garbage: us per step (the per-frame unit).
    {
        const char* garbage = "local t={} for i=1,2000 do t[i]={i, tostring(i)} end t=nil";
        lua_gc(L, LUA_GCSTOP, 0);
        luaL_loadstring(L, garbage);
        lua_pcall(L, 0, 0, 0);
        const uint64_t t0 = api->nowUs();
        int steps = 0;
        while (!lua_gc(L, LUA_GCSTEP, 1) && steps < 10000) ++steps;
        const uint64_t us = api->nowUs() - t0;
        out.push_back(BenchResult{"gc_step", steps ? static_cast<float>(us) / static_cast<float>(steps) : 0.0f,
                                  static_cast<uint32_t>(steps)});
    }
    out.push_back(BenchResult{"state_kb", static_cast<float>(a.peak) / 1024.0f, 0});
    lua_close(L);
    dummy.api_ = nullptr;
    return out;
}

}  // namespace wowee::ui::fx
