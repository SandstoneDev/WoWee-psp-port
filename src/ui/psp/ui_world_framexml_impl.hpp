// WorldScreen::FrameXml - the FrameXML adapter over WorldSession
// (option (b) of the FrameXML design), shared by
// ui_world_framexml.cpp (units, load, draw) and
// ui_world_framexml_game.cpp (action bar, cast bar, auras, spell book,
// bags, PlaySound). Private to those two files.
#pragma once

#include "ui/psp/ui_world_screen.hpp"

#include "platform/psp/psp_log.hpp"
#include "ui/psp/ui_framexml.hpp"

#include <pspthreadman.h>

#include <array>
#include <deque>

namespace wowee::ui::psp {

struct WorldScreen::FrameXml : fx::FxGameApi {
    WorldScreen& ws;
    FrameXmlUi ui;
    UiLayers layers;
    fx::FxUnit player{}, target{};
    uint64_t startUs = sceKernelGetSystemTimeWide();

    explicit FrameXml(WorldScreen& w) : ws(w), ui(w.kit_) {}

    // units (ui_world_framexml.cpp)
    void fill(uint64_t guid, fx::FxUnit& u);
    void refresh();
    bool unit(const char* token, fx::FxUnit& out) override;
    // GetTime() starts at 1000 s, not 0 - the client's is the machine's
    // uptime and FrameXML tests `start > 0` for a running cooldown (PSP: not
    // the real uptime, a float Lua keeps ms precision in cast times this way).
    double timeSec() override { return 1000.0 + static_cast<double>(sceKernelGetSystemTimeWide() - startUs) / 1e6; }
    uint64_t nowUs() override { return sceKernelGetSystemTimeWide(); }
    void log(const char* line) override;
    float textWidth(int fontObject, float heightUnits, const char* utf8) override {
        return static_cast<float>(ui.fontFor(fontObject, heightUnits).measure(utf8));
    }
    void targetUnit(const char* token) override;
    void clearTarget() override;

    // game API (ui_world_framexml_game.cpp)
    uint8_t hide = 0;  // WorldScreen::kFxHide* bits  // which built-in UI FrameXML replaces (wowee.cfg fxhide*)
    void initGame();   // after a successful load: hide bits, icon resolver
    void pollGame();   // per frame, before the runtime drains takeEvent
    bool openWindow(uint8_t w);  // the Menu / window bar asked for a built-in window
    void forwardInventoryEvent(const platform::psp::WorldSession::InventoryEvent& ev);
    void forwardCastEnd(uint8_t ended, uint32_t spellId);
    void push(const fx::FxEvent& ev);
    void castEvent(const char* name, const char* unit, uint32_t spellId);
    static const platform::psp::Texture* dynTexture(void* ctx, uint8_t kind, uint32_t id);
    uint64_t unitGuid(const char* unit) const;
    void castSpellAt(uint32_t spellId, uint64_t targetGuid);

    bool takeEvent(fx::FxEvent& out) override;
    bool action(int slot, fx::FxAction& out) override;
    void useAction(int slot, const char* unit) override;
    int bonusBarOffset() override;
    bool spellInfo(uint32_t id, fx::FxSpellInfo& out) override;
    bool spellKnown(uint32_t id) override;
    bool spellCooldown(uint32_t id, float& remaining, float& total) override;
    bool gcd(float& remaining, float& total) override;
    bool isCurrentSpell(uint32_t id) override;
    float unitDistance(const char* unit) override;
    void castSpell(uint32_t id, const char* unit) override;
    uint32_t spellBookVersion() override;
    void spellBook(std::vector<fx::FxSpellTab>& tabs, std::vector<uint32_t>& spells) override;
    size_t auras(const char* unit, fx::FxAura* out, size_t max) override;
    void cancelAura(uint32_t spellId) override;
    bool unitCast(const char* unit, fx::FxCast& out) override;
    int containerSlots(int bag) override;
    bool containerItem(int bag, int slot, fx::FxItem& out) override;
    bool inventoryItem(int invSlot, fx::FxItem& out) override;
    bool itemInfo(uint32_t entry, fx::FxItem& out) override;
    uint32_t itemCount(uint32_t entry) override;
    void useContainerItem(int bag, int slot) override;
    void moveItem(int srcBag, int srcSlot, int dstBag, int dstSlot) override;
    int32_t unitField(int field) override;
    bool playSound(const char* name) override;
    bool playSoundId(uint32_t soundEntryId) override;

    // diff state (what Lua was last told)
    std::deque<fx::FxEvent> events;
    struct CastSeen {
        bool casting = false, channel = false;
        uint32_t spell = 0;
        float endAt = 0;
    } castSelf, castTarget;
    uint64_t castTargetGuid = 0;
    uint8_t castEnd = 0;  // forwarded takeCastEnd (1 success, 2 failed / interrupted)
    uint32_t castEndSpell = 0;
    uint32_t auraSigPlayer = 0, auraSigTarget = 0;
    uint64_t auraTargetGuid = 0;
    std::array<fx::FxAction, platform::psp::WorldSession::kActionBarSlots> barSeen{};
    bool barInit = false;
    std::vector<std::pair<uint32_t, float>> cdSeen;  // bar spell -> cooldown start (GetTime s), 0 = none
    float gcdStartSeen = 0;
    int bonusSeen = -1;
    uint32_t knownSeen = ~0u;
    uint32_t powerSeen = ~0u;
    float usableTimer = 0;  // GetTime() of the next ACTIONBAR_UPDATE_USABLE allowed
    uint32_t eventDrops = 0, castSpellLogs = 0;
};

}  // namespace wowee::ui::psp
