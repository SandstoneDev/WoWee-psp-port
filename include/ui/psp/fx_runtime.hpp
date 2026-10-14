// FrameXML runtime for the PSP - the baked frame tree
// (fx_format.hpp), the Lua 5.1 VM that runs its scripts, the client's widget API
// the scripts call, anchor layout, draw order and pointer input.
//
// Platform-free on purpose (no PSP headers): the same code runs in the host
// test (tools/psp/host_tests/framexml_host_test.cpp, 32-bit, float Lua) that
// measures memory per frame/region and the load time, and on the PSP behind
// ui_framexml.cpp (textures, fonts, GE drawing, WorldSession adapter).
//
// Upstream: this is the job of src/ui/widget_tree.cpp (anchors, strata,
// levels, hit testing) + src/addons/lua_engine.cpp (CreateFrame, widget
// methods, events) + src/addons/lua_unit_api.cpp (UnitHealth...), cut to the
// subset the unit frames need and reshaped for 64 MB / 333 MHz: one compact
// record per widget (upstream ui::Widget is 1184 bytes on the PSP), Lua tables
// only for widgets a script touches, events as pre-interned strings, GC
// stepped per frame under a time budget (docs/PSP_LUA_NOTES.md).
#pragma once

#include "ui/psp/fx_format.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct lua_State;

namespace wowee::ui::fx {

// What the Lua API asks the game (option (b): an adapter over the PSP's
// WorldSession, not upstream's GameHandler). Field names follow GameHandler /
// game::Unit so the adapter stays thin.
struct FxUnit {
    bool exists = false;
    uint64_t guid = 0;
    char name[48] = {};  // UTF-8; a fixed buffer: UnitHealth & co. copy the struct, no allocation
    uint32_t level = 0, health = 0, maxHealth = 0;
    uint8_t powerType = 0;
    uint32_t power[8] = {}, maxPower[8] = {};
    bool dead = false, ghost = false, isPlayer = false, inCombat = false, connected = true;
    bool canAttack = false, isFriend = true;  // no faction data yet (FactionTemplate comes later)
    uint8_t race = 0, classId = 0, sex = 0;  // sex: 0 male, 1 female (UnitSex answers +2)
    uint32_t rank = 0;                        // creature rank (elite 1, rare elite 2, boss 3, rare 4)
};

// What the action bar, cast bar, auras, spell book and bag frames
// ask (fx_runtime_game.cpp binds them to upstream's Lua names; the PSP
// adapter answers from WorldSession::actionBar()/playerAuras()/unitCast()/
// getInventory() and SpellDb). Fixed buffers: no allocation per call.
struct FxAction {
    uint8_t type = 0;  // 0 empty, 1 spell, 2 item, 3 macro (game::ActionBarSlot::Type order)
    uint32_t id = 0;
};
struct FxSpellInfo {
    char name[64] = {};
    char rank[32] = {};
    bool passive = false;
    uint32_t castMs = 0, powerCost = 0;
    uint8_t powerType = 0, dispel = 0;  // dispel: 1 Magic, 2 Curse, 3 Disease, 4 Poison
    float minRange = 0, maxRange = 0;
};
struct FxAura {
    uint32_t spellId = 0;
    uint8_t flags = 0;     // 0x80 = harmful (upstream AuraSlot::flags)
    uint8_t charges = 0;
    uint64_t caster = 0;
    int32_t durationMs = 0;   // max duration, <= 0 = none
    int32_t remainingMs = 0;  // < 0 = permanent
};
struct FxCast {
    bool casting = false, channel = false, interruptible = true;
    uint32_t spellId = 0;
    float total = 0, remaining = 0;  // seconds
};
struct FxItem {
    uint32_t entry = 0, count = 0;
    uint8_t quality = 1, invType = 0;
    uint32_t itemLevel = 0, reqLevel = 0, maxStack = 1, sellPrice = 0, bagSlots = 0;
    uint32_t durability = 0, maxDurability = 0;
    char name[64] = {};
};
struct FxSpellTab {
    char name[32] = {};
    uint32_t iconSpell = 0;  // the tab's picture: that spell's icon
    uint32_t offset = 0, count = 0;
};
// One game event for Lua (the session queues, diffed or forwarded by the
// adapter). PSP: fixed arguments pushed straight onto the stack - no table
// per event (docs/PSP_LUA_NOTES.md).
struct FxEventArg {
    uint8_t kind = 0;  // 0 nil, 1 number, 2 string
    float num = 0;
    char str[48] = {};
};
struct FxEvent {
    const char* name = "";  // a string literal
    uint8_t argc = 0;
    FxEventArg args[5];
    FxEvent& n(float v) {
        if (argc < 5) args[argc].kind = 1, args[argc++].num = v;
        return *this;
    }
    FxEvent& s(const char* v);
};

class FxGameApi {
public:
    virtual ~FxGameApi() = default;
    // unit token already lower case: player, target, targettarget, pet, focus, mouseover, npc.
    // The adapter answers from a snapshot it refreshes once per frame (cheap).
    virtual bool unit(const char* token, FxUnit& out) = 0;
    virtual double timeSec() = 0;       // GetTime()
    virtual uint64_t nowUs() = 0;       // for budgets and logs
    virtual void log(const char* line) = 0;  // one key=value line (<= 120 chars)
    virtual uint32_t money() { return 0; }
    virtual void targetUnit(const char* token) { (void)token; }  // TargetUnit / secure unit button
    virtual void clearTarget() {}
    // Pixel width of a UTF-8 line in the font a font object maps to.
    virtual float textWidth(int fontObject, float heightUnits, const char* utf8) = 0;
    virtual bool isResting() { return false; }
    // Action bar / spells / auras / cast / bags / sound. Defaults
    // answer "nothing" (the host test and a missing model stay safe).
    virtual bool takeEvent(FxEvent& out) { (void)out; return false; }  // drained once per frame
    virtual bool action(int slot, FxAction& out) { (void)slot; (void)out; return false; }  // 0-based, 144
    virtual void useAction(int slot, const char* unit) { (void)slot; (void)unit; }
    virtual int bonusBarOffset() { return 0; }
    virtual bool spellInfo(uint32_t id, FxSpellInfo& out) { (void)id; (void)out; return false; }
    virtual bool spellKnown(uint32_t id) { (void)id; return false; }
    virtual bool spellCooldown(uint32_t id, float& remaining, float& total) { (void)id; remaining = total = 0; return false; }
    virtual bool gcd(float& remaining, float& total) { remaining = total = 0; return false; }
    virtual bool isCurrentSpell(uint32_t id) { (void)id; return false; }  // being cast / auto attack on
    virtual float unitDistance(const char* unit) { (void)unit; return -1.0f; }  // yd, < 0 unknown
    virtual void castSpell(uint32_t id, const char* unit) { (void)id; (void)unit; }
    virtual uint32_t spellBookVersion() { return 0; }
    virtual void spellBook(std::vector<FxSpellTab>& tabs, std::vector<uint32_t>& spells) { tabs.clear(); spells.clear(); }
    virtual size_t auras(const char* unit, FxAura* out, size_t max) { (void)unit; (void)out; (void)max; return 0; }
    virtual void cancelAura(uint32_t spellId) { (void)spellId; }
    virtual bool unitCast(const char* unit, FxCast& out) { (void)unit; out = FxCast{}; return false; }
    // Bags in the Lua numbering: bag 0 backpack, 1..4 worn bags; invSlot
    // 1..23 the client's inventory slots (head 1 .. tabard 19, bags 20..23).
    virtual int containerSlots(int bag) { (void)bag; return 0; }
    virtual bool containerItem(int bag, int slot, FxItem& out) { (void)bag; (void)slot; (void)out; return false; }
    virtual bool inventoryItem(int invSlot, FxItem& out) { (void)invSlot; (void)out; return false; }
    virtual bool itemInfo(uint32_t entry, FxItem& out) { (void)entry; (void)out; return false; }
    virtual uint32_t itemCount(uint32_t entry) { (void)entry; return 0; }
    virtual void useContainerItem(int bag, int slot) { (void)bag; (void)slot; }
    // Moves what is at (srcBag, srcSlot) - bag -1 = inventory slot - to
    // (dstBag, dstSlot); dstSlot 0 = the first free slot of dstBag.
    virtual void moveItem(int srcBag, int srcSlot, int dstBag, int dstSlot) {
        (void)srcBag; (void)srcSlot; (void)dstBag; (void)dstSlot;
    }
    virtual int32_t unitField(int field) { (void)field; return 0; }  // FxField below
    virtual bool playSound(const char* name) { (void)name; return false; }
    virtual bool playSoundId(uint32_t soundEntryId) { (void)soundEntryId; return false; }
};

// FxGameApi::unitField ids (the player's own update fields).
enum FxField : int {
    kFxStat0 = 0,          // 0..4 Strength, Agility, Stamina, Intellect, Spirit
    kFxResist0 = 5,        // 5..11 armor, holy, fire, nature, frost, shadow, arcane
    kFxAttackPower = 12,
    kFxRangedAttackPower = 13,
    kFxMinDamage = 14,     // float bits
    kFxMaxDamage = 15,     // float bits
};

struct FxConfig {
    float uiScale = 0.6f;        // screen px per UI unit (wowee.cfg fxscale)
    float screenW = 480, screenH = 272;
    uint32_t luaBudgetKb = 3072; // allocator hard limit (wowee.cfg luamem)
    float gcBudgetMs = 1.0f;     // GC steps per frame (wowee.cfg luagcms)
    bool apiFallback = true;     // unknown globals answer a no-op (upstream WOWEE_LUA_API_FALLBACK)
    std::string locale = "ruRU"; // GetLocale() - the running language (framexml.pak is its own)
};

// One thing to draw, in screen pixels (y down), already in draw order.
struct FxDrawItem {
    enum Kind : uint8_t { Image, Fill, Text, NineSlice, Tile, Cooldown };  // Cooldown: edge = part left
    uint8_t kind = Image;
    uint8_t add = 0;       // additive blend
    uint8_t justify = 0;   // text: 0 left, 1 center, 2 right
    uint8_t strata = kStrataMedium;
    int16_t font = -1;     // text: font object
    int16_t clip = -1;     // index into clips(), -1 none
    int32_t texture = -1;  // FxTexture index
    float x = 0, y = 0, w = 0, h = 0;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;  // normalized
    float edge = 1.0f;     // NineSlice: border scale; Text: height in UI units
    uint32_t color = 0xFFFFFFFFu;
    const char* text = nullptr;  // valid until the next buildDrawList
};

// A texture index at or above this is not baked in framexml.pak
// but named by the game (a spell icon from spells.pak): FxUi::dynTexture()
// says which; the renderer asks its resolver (ui_framexml.cpp).
inline constexpr int32_t kFxDynTexBase = 0x40000000;
enum FxDynKind : uint8_t { kFxDynSpell = 1, kFxDynItem = 2 };

struct FxClip {
    float x0, y0, x1, y1;  // screen px
};

struct FxStats {
    uint32_t frames = 0, textures = 0, fontStrings = 0, templates = 0;
    uint32_t luaObjects = 0;       // widgets with a Lua table
    uint32_t scripted = 0;         // widgets with at least one handler
    uint32_t nativeBytes = 0;      // records + names + texts + script refs
    uint32_t luaKb = 0, luaPeakKb = 0, luaFails = 0;
    uint32_t luaBaseKb = 0;        // after the libraries + API, before the load program
    uint32_t loadMs = 0, luaErrors = 0, missingApi = 0;
    uint32_t onUpdateFrames = 0, eventsFired = 0, handlerCalls = 0;
    uint32_t drawItems = 0, visible = 0;
    uint32_t gcCycles = 0;
    float gcMsLast = 0, luaMsLast = 0, layoutMsLast = 0;
    uint32_t dynTextures = 0;      // Game textures named (spell icons)
};

// Script time per top-level frame (child of UIParent) - the
// fx_frame name= us= lines.
struct FxFrameTime {
    uint32_t obj = 0;
    uint64_t us = 0;
    uint32_t calls = 0;
};

class FxUi {
public:
    // Reads a baked chunk "l_<index>" into `out` (the PSP: from the already
    // read framexml.pak blobs; the host test: from the pak file).
    using ChunkReader = bool (*)(void* ctx, uint32_t index, std::vector<uint8_t>& out);

    FxUi();
    ~FxUi();
    FxUi(const FxUi&) = delete;
    FxUi& operator=(const FxUi&) = delete;

    // Takes a copy of the tree blob. Runs the load program (TOC order), then
    // VARIABLES_LOADED, PLAYER_LOGIN, PLAYER_ENTERING_WORLD.
    bool load(const void* tree, uint32_t size, ChunkReader reader, void* readerCtx, FxGameApi* api,
              const FxConfig& cfg);
    // Same, taking the blob (the PSP reads it from the pak: no second copy).
    bool load(std::vector<uint8_t>&& tree, ChunkReader reader, void* readerCtx, FxGameApi* api,
              const FxConfig& cfg);
    void shutdown();
    bool loaded() const { return L_ != nullptr; }
    // Loads every XML handler table now (templates used later by CreateFrame
    // included), so the caller can free the chunk bytes.
    void preloadHandlers();

    // Per frame: game state -> events, OnUpdate handlers (ui.lua); then
    // layout of what is shown and the draw list (ui.frames); GC last, with the
    // remaining budget.
    void updateLua(float dtSec);
    void buildDrawList();
    void stepGc();

    const std::vector<FxDrawItem>& drawList() const { return draw_; }
    const std::vector<FxClip>& clips() const { return clips_; }
    const FxHeader& header() const { return hdr_; }
    const FxFont* font(int i) const;
    const char* string(uint32_t off) const;

    // Pointer in screen px (the cursor mode). Returns true when a mouse
    // enabled frame is under it.
    bool pointerMove(float sx, float sy);
    bool pointerButton(bool down, bool right);
    // Visible clickable frames (focus ring stops), screen px.
    struct Clickable {
        uint32_t obj;
        float x, y, w, h;
    };
    void clickables(std::vector<Clickable>& out, size_t max) const;
    bool clickObject(uint32_t obj, bool right);

    FxStats stats() const;
    lua_State* lua() const { return L_; }
    static uint32_t luaNumberSize();
    // Runs a string (tests, debug). Returns false on error (logged).
    bool runString(const char* code, const char* name);
    // Runs fn inside lua_cpcall: a Lua error or an allocation refused by the
    // budget outside a script's own pcall (creating a widget's table, pushing
    // an event name) fails this call instead of reaching Lua's panic (exit).
    bool protect(void (*fn)(FxUi&, void*), void* arg, const char* where);

    // Micro benchmark on a fresh state (no FrameXML needed): us per op.
    struct BenchResult {
        const char* name;
        float usPerOp;
        uint32_t ops;
    };
    static std::vector<BenchResult> bench(FxGameApi* api, uint32_t scale);

    // ------------------------------------------------------------ internals
    // (public for the C bindings in fx_runtime.cpp)
    struct Anchor {
        uint8_t point, relPoint;
        uint8_t relParent;     // 1: the parent
        uint8_t pad;
        uint32_t relObj;       // resolved object (kNone = unresolved)
        uint32_t relName;      // string offset in names_ (0 = none)
        float x, y;
    };
    static constexpr uint32_t kNone = 0xFFFFFFFFu;
    enum ObjFlags : uint32_t {
        kShown = 1u << 0,
        kMouse = 1u << 1,
        kEnabled = 1u << 2,
        kChecked = 1u << 3,
        kAdd = 1u << 4,
        kSetAll = 1u << 5,
        kTexColor = 1u << 6,
        kHasSize = 1u << 7,
        kHovered = 1u << 8,
        kPushed = 1u << 9,
        kHighlightLocked = 1u << 10,
        kVisibleCache = 1u << 11,  // computed in layout
        kWrap = 1u << 12,
        kLoaded = 1u << 13,        // OnLoad ran
        kTopLevel = 1u << 14,
        kDesaturated = 1u << 15,
    };
    struct Obj {
        const FxNode* node = nullptr;  // baked record (nullptr: made by Lua)
        uint32_t parent = kNone;
        uint32_t firstChild = kNone, lastChild = kNone, next = kNone;
        uint32_t flags = kShown | kEnabled;
        uint32_t name = 0;        // offset in names_ (0 = none)
        uint8_t type = kFxFrame;
        uint8_t strata = kStrataMedium;
        uint8_t layer = kLayerArtwork;
        uint8_t role = kRoleNone;
        int8_t subLevel = 0;
        uint8_t anchorCount = 0;
        uint8_t jh = 1, jv = 1;
        int16_t level = 0;
        int16_t font = -1;
        uint16_t id = 0;
        uint16_t backdrop = 0;
        uint32_t anchorsExtra = kNone;  // index into extraAnchors_ when > 2
        Anchor anchors[2] = {};
        float w = 0, h = 0, alpha = 1, scale = 1;
        float fontHeight = 0;
        int32_t texture = -1;
        float tc[4] = {0, 1, 0, 1};
        uint32_t color = 0xFFFFFFFFu;
        float minV = 0, maxV = 1, value = 0;
        int32_t scripts = -1;     // index into scriptSets_
        int32_t luaRef = 0;       // index in the registry objs table (0 = none)
        float l = 0, b = 0, r = 0, t = 0;  // resolved rect, UI units, y up
        uint32_t gen = 0;         // layout generation of l/b/r/t
        std::string text;
    };
    struct ScriptSet {
        int32_t ref[kScrCount];
    };

    std::vector<Obj>& objs() { return objs_; }
    const Obj& objAt(uint32_t i) const { return objs_[i]; }
    Obj& obj(uint32_t i) { return objs_[i]; }
    FxGameApi* api() { return api_; }
    const FxConfig& cfg() const { return cfg_; }

    uint32_t createObject(uint8_t type, uint32_t parent, const char* name);
    uint32_t instantiate(uint32_t node, uint32_t parent, uint32_t into, const std::string& ownerName,
                         bool runOnLoad);
    bool applyTemplate(uint32_t obj, const char* templateName);
    uint32_t findByName(const char* name) const;
    void pushObject(uint32_t i);           // its Lua table (created on first use)
    uint32_t toObject(int stackIndex);     // kNone when not a widget
    void setShown(uint32_t i, bool shown);
    bool isVisible(uint32_t i) const;
    int setPoint(uint32_t i, uint8_t point, uint32_t relObj, uint8_t relPoint, float x, float y,
                 bool relParent);  // returns the anchor slot
    void clearPoints(uint32_t i);
    void setScript(uint32_t i, int kind, int stackIndex);  // stackIndex 0 = clear
    int scriptRef(uint32_t i, int kind) const;
    void callScript(uint32_t i, int kind, int nargs);      // args on the stack
    bool hasScript(uint32_t i, int kind) const { return scriptRef(i, kind) != 0; }
    void registerEvent(uint32_t i, const char* ev, bool on);
    void unregisterAll(uint32_t i);
    void fireEvent(const char* ev, const char* arg1 = nullptr);
    int eventId(const char* ev, bool create);
    void markLayout() { ++layoutGen_; layoutDirty_ = true; drawDirty_ = true; }
    // Anything a widget method touched: the draw list is rebuilt next frame
    // (an idle interface costs nothing - PSP_LUA_NOTES: skip clean work).
    void markDraw() { drawDirty_ = true; }
    void markOrder() { orderDirty_ = true; }
    void resolve(uint32_t i, int depth = 0);
    float effScale(uint32_t i) const;
    void noteLuaError(const char* where);
    void noteMissingApi(const char* name);
    uint32_t internName(const char* s);    // names_ offset
    const char* nameOf(uint32_t i) const { return names_.data() + objs_[i].name; }
    int textureByPath(const char* path) const;
    void setText(uint32_t i, const char* text);
    float textWidth(uint32_t i) const;
    int fontForHeight(float h) const;

    std::vector<FxDrawItem> draw_;
    std::vector<FxClip> clips_;

private:
    void updateLuaImpl(float dtSec);
    bool pointerMoveImpl(float sx, float sy);
    bool pointerButtonImpl(bool down, bool right);
    bool clickObjectImpl(uint32_t obj, bool right);
    void preloadHandlersImpl();
    void loadProgramImpl();
    void setupStateImpl();
    bool runChunk(uint32_t index, bool keepResult);
    bool loadHandlers(uint32_t chunk);
    void runLoadProgram();
    void pollGame();
    void emitFrame(uint32_t f, int clip);
    void emitRegion(uint32_t r, uint32_t owner, int clip);
    void rebuildOrder();
    uint32_t hitTest(float ux, float uy) const;
    void installApi();

    lua_State* L_ = nullptr;
    FxGameApi* api_ = nullptr;
    FxConfig cfg_{};
    std::vector<uint8_t> tree_;
    FxHeader hdr_{};
    const FxNode* nodes_ = nullptr;
    const FxAnchor* anchors_ = nullptr;
    const FxScript* scripts_ = nullptr;
    const FxFont* fonts_ = nullptr;
    const FxTexture* textures_ = nullptr;
    const FxOp* ops_ = nullptr;
    const FxChunk* chunks_ = nullptr;
    const FxTemplate* templates_ = nullptr;
    const FxBackdrop* backdrops_ = nullptr;
    const char* strings_ = nullptr;
    ChunkReader reader_ = nullptr;
    void* readerCtx_ = nullptr;
    std::vector<int32_t> handlerTables_;  // registry ref per chunk (0 = not loaded)

    std::vector<Obj> objs_;
    std::vector<Anchor> extraAnchors_;
    std::vector<ScriptSet> scriptSets_;
    std::string names_;
    std::unordered_map<std::string, uint32_t> byName_;
    std::unordered_map<std::string, int> texByPath_;
    std::unordered_map<std::string, int> eventIds_;
    std::vector<std::vector<uint32_t>> eventFrames_;
    std::vector<int32_t> eventNameRefs_;  // pre-interned event strings (registry refs)
    std::vector<uint32_t> onUpdate_;      // frames with OnUpdate
    std::vector<uint32_t> order_;         // frames in draw order
    bool orderDirty_ = true, layoutDirty_ = true, drawDirty_ = true;
    uint32_t layoutGen_ = 1;
    uint32_t uiParent_ = kNone;
    uint32_t hovered_ = kNone, pressed_ = kNone;
    float uiW_ = 800, uiH_ = 453;
    int objsRef_ = 0;                     // registry: table obj index -> Lua table
    // game state seen last frame (events)
    FxUnit lastPlayer_{}, lastTarget_{};
    bool polledOnce_ = false;
    double lastWatchSec_ = 0;
    // stats
    uint32_t luaErrors_ = 0, missingApi_ = 0, handlerCalls_ = 0, eventsFired_ = 0, gcCycles_ = 0;
    uint32_t loadMs_ = 0;
    float gcMsLast_ = 0, luaMsLast_ = 0, layoutMsLast_ = 0;
    size_t gcBaseline_ = 0;
    size_t luaBase_ = 0;
    bool gcRunning_ = false;
    uint32_t logLines_ = 0;

public:
    // Game API (fx_runtime_game.cpp) - bindings, dynamic textures,
    // the item cursor, script time per top-level frame.
    static FxUi* fromLua(lua_State* L);
    void installGameApi();
    void pollGameEvents();
    void fireEventArgs(const char* ev, const FxEventArg* args, int argc);
    int32_t resolveTexture(const char* path);  // baked, else a game texture, else -1
    int32_t dynTexture(uint8_t kind, uint32_t id);
    bool dynTexInfo(int32_t tex, uint8_t& kind, uint32_t& id) const;
    void takeFrameTimes(std::vector<FxFrameTime>& out);  // sorted by time, then reset
    uint32_t topLevelOf(uint32_t i) const;
    struct Cursor {
        int bag = -2, slot = 0;  // -2 empty; bag -1 = inventory slot
    };
    Cursor cursor_;
    std::vector<FxSpellTab> bookTabs_;
    std::vector<uint32_t> bookSpells_;
    uint32_t bookVersion_ = ~0u;
    void refreshSpellBook();
    uint32_t actionLogs_ = 0, castLogs_ = 0, soundLogs_ = 0;
    std::vector<std::pair<uint8_t, uint32_t>> dyn_;
    std::unordered_map<uint64_t, int32_t> dynIndex_;
    std::unordered_map<uint32_t, FxFrameTime> frameTimes_;
    int scriptDepth_ = 0;
    bool animating_ = false;  // a running Cooldown: the list is rebuilt every frame

    struct Alloc {
        size_t live = 0, peak = 0, budget = 0;
        uint32_t fails = 0, count = 0;
        FxUi* owner = nullptr;
    };
    Alloc alloc_;
    int methodsRef_[kFxTypeCount] = {};
    std::unordered_map<std::string, bool> missingSeen_;
    std::unordered_map<std::string, std::string> cvars_;
    void logf(const char* fmt, ...);
};

}  // namespace wowee::ui::fx
