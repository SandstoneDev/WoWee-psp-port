// Loot on the world screen.
//
// The data model is the session's (psp_world_session_loot.cpp, upstream
// InventoryHandler's names); this is the temporary PSP view of it until the
// Lua LootFrame (task A) reads the same calls:
//   - tryLootTarget(guid): upstream GameScreen's right click on a dead unit
//     (lootTarget), gated like the server gates CMSG_LOOT (alive player, dead
//     unit, AzerothCore INTERACTION_DISTANCE 5.5 yd plus both combat reaches);
//   - the loot window in the window stack (ui_world_windows.cpp): corpse name,
//     the coin row, the items in their quality colours with counts; Cross on a
//     row takes it (LootSlot), take all, close / Circle = CloseLoot
//     (CMSG_LOOT_RELEASE); it goes away when the session closes the loot
//     (SMSG_LOOT_RELEASE_RESPONSE, an emptied corpse) or the player walks off;
//   - the sparkle over lootable corpses: upstream LootSparkles puts
//     Particles\LootFX.m2 on every dead unit marked UNIT_DYNFLAG_LOOTABLE;
//     PSP: a pulsing gold star drawn in the HUD pass (no particle system);
//   - the loot button in the Debug window: tryLootTarget on the target, else the
//     nearest lootable corpse (until the orchestrator wires task D's L + up).
#include "ui/psp/ui_world_screen.hpp"

#include "core/coordinates.hpp"
#include "game/item_text.hpp"
#include "game/update_field_table.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_texture.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"

#include <pspctrl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

// Widget ids (E: 0x6200-0x62FF).
constexpr uint32_t kIdLootSlot = 0x6200;  // + server slot index (0..63)
constexpr uint32_t kIdLootMoney = 0x6240;
constexpr uint32_t kIdLootAll = 0x6241;
constexpr uint32_t kIdLootClose = 0x6242;
constexpr uint32_t kIdLootDebug = 0x6250;
constexpr float kOwnReach = 1.5f;    // player combat reach (UNIT_FIELD_COMBATREACH default)
constexpr size_t kMaxSparkles = 8;

unsigned lo32(uint64_t guid) { return static_cast<unsigned>(guid & 0xFFFFFFFFu); }

// upstream ui::getQualityColor (ui_colors.hpp), as 0xAABBGGRR.
uint32_t qualityColour(uint8_t q) {
    switch (q) {
        case 0: return 0xFF9E9E9Eu;  // poor      0.62 0.62 0.62
        case 1: return 0xFFFFFFFFu;  // common
        case 2: return 0xFF00FF1Fu;  // uncommon  0.12 1.00 0.00
        case 3: return 0xFFDE7000u;  // rare      0.00 0.44 0.87
        case 4: return 0xFFED36A3u;  // epic      0.64 0.21 0.93
        case 5: return 0xFF0080FFu;  // legendary 1.00 0.50 0.00
        case 6:
        case 7: return 0xFF80CCE6u;  // artifact / heirloom 0.90 0.80 0.50
        default: return 0xFFFFFFFFu;
    }
}

float cfgFloat(const char* key, float fallback) {
    const std::string v = pp::configGet(key, "");
    if (v.empty()) return fallback;
    char* end = nullptr;
    const float f = std::strtof(v.c_str(), &end);
    return end != v.c_str() && f >= 0.0f && f < 1000.0f ? f : fallback;
}

}  // namespace

void WorldScreen::initLoot() {
    lootInit_ = true;
    // upstream SettingsPanel pendingAutoLoot (off by default).
    session_.setAutoLoot(pp::configGetInt("autoloot", 0) != 0);
    lootGate_ = pp::configGetInt("lootgate", 1) != 0;       // 0: any dead unit (upstream GameScreen)
    lootRange_ = cfgFloat("lootrange", 5.5f);               // AzerothCore INTERACTION_DISTANCE
    lootCloseDist_ = cfgFloat("lootclosedist", 15.0f);      // 0: never (upstream)
    lootDespawn_ = pp::configGetInt("lootdespawn", 1) != 0;  // upstream despawnCreatureLocally
    lootMarker_ = pp::configGetInt("lootmarker", 1) != 0;
    lootMarkDist_ = cfgFloat("lootmarkdist", 40.0f);
    PSP_LOGI(pp::tag::Gfx, "loot_cfg autoloot=%d gate=%d range=%.1f closedist=%.0f despawn=%d marker=%d markdist=%.0f",
             session_.isAutoLoot() ? 1 : 0, lootGate_ ? 1 : 0, lootRange_, lootCloseDist_, lootDespawn_ ? 1 : 0,
             lootMarker_ ? 1 : 0, lootMarkDist_);
}

// upstream GameScreen: target dead (health 0) -> gameHandler.lootTarget.
bool WorldScreen::tryLootTarget(uint64_t guid) {
    PROF_SCOPE("loot");
    if (!lootInit_) initLoot();
    if (guid == 0 || guid == session_.playerGuid()) return false;
    auto e = session_.entities().getEntity(guid);
    if (!e || e->getType() != game::ObjectType::UNIT) return false;
    const uint32_t hp = e->getField(game::fieldIndex(game::UF::UNIT_FIELD_HEALTH));
    const uint32_t maxHp = e->getField(game::fieldIndex(game::UF::UNIT_FIELD_MAXHEALTH));
    const uint32_t dyn = e->getField(game::fieldIndex(game::UF::UNIT_DYNAMIC_FLAGS));
    if (!game::isUnitCorpseState(hp, maxHp, dyn)) return false;
    // PSP: only corpses the server marked lootable for us (wowee.cfg lootgate;
    // upstream asks for any dead unit and lets the server refuse - its gate
    // was dropped while the flag was read from the wrong field).
    if (lootGate_ && !(dyn & game::UNIT_DYNFLAG_LOOTABLE)) return false;
    if (session_.life() != pp::WorldSession::Life::Alive) return false;  // server: dead players do not loot
    // Server-side reach (AzerothCore Player::SendLoot: IsWithinDistInMap(
    // INTERACTION_DISTANCE) = centre distance minus both object sizes).
    float reach = 0.0f;
    const uint32_t reachBits = e->getField(game::fieldIndex(game::UF::UNIT_FIELD_COMBATREACH));
    if (reachBits) std::memcpy(&reach, &reachBits, sizeof(reach));
    if (!(reach > 0.0f && reach < 20.0f)) reach = 0.388f;  // DEFAULT_WORLD_OBJECT_SIZE
    const glm::vec3 tp = core::coords::canonicalToServer(glm::vec3(e->getLatestX(), e->getLatestY(), e->getLatestZ()));
    const glm::vec3 d = tp - glm::vec3(playerX_, playerY_, playerZ_);
    const float dist = std::sqrt(glm::dot(d, d));
    const float maxDist = lootRange_ + reach + kOwnReach;
    if (dist > maxDist) {
        combatError_ = CLIENT_STR(ERR_OUT_OF_RANGE);  // the error line (UIErrorsFrame)
        combatErrorUntilMs_ = pp::logUptimeMs() + 2000u;
        if (++lootUiLogs_ <= 60)
            PSP_LOGI(pp::tag::Net, "loot_far guid=%08X dist=%.1f max=%.1f", lo32(guid), dist, maxDist);
        return true;
    }
    if (sitting_) setSitting(false, false);  // PSP: stand up to loot, as the original client does
    session_.lootTarget(guid);
    return true;
}

void WorldScreen::updateLoot(float dtSec) {
    PROF_SCOPE("loot");
    if (!lootInit_) initLoot();
    session_.updateLoot(dtSec, lootDespawn_);
    if (itemIconsTried_) itemIcons_.tick(pp::logUptimeMs());  // close the pak when idle
    const bool open = session_.isLootWindowOpen();
    const uint64_t guid = open ? session_.getLootGuid() : 0;
    // A new loot: its window on top, the cursor up (like the release window).
    // PSP: auto-loot opens no window (upstream fires LOOT_OPENED with
    // autoLoot = 1 and the Lua frame stays hidden).
    if (open && guid != lootShownGuid_ && !session_.isAutoLoot()) {
        lootShownGuid_ = guid;
        lootPressSeen_ = false;
        lootPage_ = 1;  // LootFrame_OnEvent LOOT_OPENED: page 1
        if (session_.getLootItems().empty() && !session_.getCurrentLoot().goldLeft())
            playSoundByName("LOOTWINDOWOPENEMPTY");  // LootFrame_OnShow
        if (!windowOpen(kWinLoot)) lootRaisedUi_ = !uiMode_;  // (a second corpse keeps the first's)
        openWindow(kWinLoot);
        if (!uiMode_) setUiMode(true);
        suppress_ = ~0u;  // whatever is held now stays dead until released (masked by held next frame)
        winFocus_.setFocus(kIdLootSlot + (session_.getLootItems().empty() ? 0 : session_.getLootItems()[0].slotIndex));
        if (session_.getLootItems().empty()) winFocus_.setFocus(kIdLootClose);
    } else if (open && lootShownGuid_ == guid && guid != 0 && !windowOpen(kWinLoot)) {
        // Circle took the window down: CloseLoot (LootFrame_OnHide).
        session_.closeLoot();
    }
    // Walked off (PSP, wowee.cfg lootclosedist): upstream closes the vendor /
    // gossip / taxi / trainer windows past NPC_INTERACT_MAX_DISTANCE (15 yd)
    // and leaves the loot to the server; the PSP closes the loot the same way.
    if (session_.isLootWindowOpen() && lootCloseDist_ > 0.0f) {
        if (auto e = session_.entities().getEntity(session_.getLootGuid())) {
            const glm::vec3 p = core::coords::canonicalToServer(glm::vec3(e->getX(), e->getY(), e->getZ()));
            const float dx = p.x - playerX_, dy = p.y - playerY_;
            const float dist = std::sqrt(dx * dx + dy * dy);
            if (dist > lootCloseDist_) {
                PSP_LOGI(pp::tag::Net, "loot_close reason=range dist=%.1f", dist);
                session_.closeLoot();
            }
        }
    }
    if (!session_.isLootWindowOpen() && lootShownGuid_ != 0) {
        lootShownGuid_ = 0;
        closeWindow(kWinLoot);
        if (lootRaisedUi_ && uiMode_ && windows_.empty() && !settings_.isOpen()) {
            setUiMode(false);
            suppress_ = ~0u;  // the Cross / Circle that closed it must not walk
        }
        lootRaisedUi_ = false;
    }
}

// --- HUD: the lootable-corpse sparkle ---------------------------------------------

void WorldScreen::drawLootHud(UiRenderer& ui) {
    if (!lootMarker_ || !lootInit_) return;
    PROF_SCOPE("loot");
    struct Near {
        uint64_t guid;
        float distSq;
    };
    Near nearest[kMaxSparkles];
    size_t count = 0;
    const float maxSq = lootMarkDist_ * lootMarkDist_;
    for (uint64_t guid : session_.lootableCorpses()) {
        auto e = session_.entities().getEntity(guid);
        if (!e) continue;
        const glm::vec3 p = core::coords::canonicalToServer(glm::vec3(e->getX(), e->getY(), e->getZ()));
        const glm::vec3 d(p.x - playerX_, p.y - playerY_, p.z - playerZ_);
        const float distSq = glm::dot(d, d);
        if (distSq > maxSq) continue;
        if (count < kMaxSparkles) {
            nearest[count++] = {guid, distSq};
        } else {
            size_t far = 0;
            for (size_t i = 1; i < count; ++i)
                if (nearest[i].distSq > nearest[far].distSq) far = i;
            if (distSq < nearest[far].distSq) nearest[far] = {guid, distSq};
        }
    }
    if (count == 0) return;
    // The nearest corpse within Up's reach (8 yd, interactTarget)
    // shows the PSP button that loots it.
    {
        size_t best = count;
        for (size_t i = 0; i < count; ++i)
            if (nearest[i].distSq <= 64.0f && (best == count || nearest[i].distSq < nearest[best].distSq)) best = i;
        glm::vec3 pos;
        float h = 0.0f, sx = 0.0f, sy = 0.0f;
        if (best < count && !session_.isLootWindowOpen() && unitPosition(nearest[best].guid, pos, h) &&
            projectHud(pos + glm::vec3(0.0f, 0.0f, 1.2f), sx, sy))
            kit_.glyph(ui, kPadUp, Rect{sx - 8, sy - 8, 16, 16});
    }
    // One pulse for all (1.4 s), float only (no double on the Allegrex).
    const float t = static_cast<float>(pp::logUptimeMs() % 1400u) / 1400.0f;
    const float pulse = 0.5f + 0.5f * std::sin(t * 6.2831853f);
    const uint32_t alpha = static_cast<uint32_t>(120.0f + 135.0f * pulse);
    const uint32_t gold = (alpha << 24) | 0x0040D8FFu;   // warm gold
    const uint32_t white = (alpha << 24) | 0x00C0F0FFu;  // pale core
    for (size_t i = 0; i < count; ++i) {
        glm::vec3 pos;
        float h = 0.0f, sx = 0.0f, sy = 0.0f;
        if (!unitPosition(nearest[i].guid, pos, h)) continue;
        // On the body (it lies on the ground), not over the head.
        if (!projectHud(pos + glm::vec3(0.0f, 0.0f, 0.6f), sx, sy)) continue;
        // Nearer = bigger (2..5 px arms), plus the pulse.
        const float dist = std::sqrt(nearest[i].distSq);
        const float arm = std::clamp(9.0f - dist * 0.15f, 3.0f, 8.0f) * (0.7f + 0.3f * pulse);
        ui.fill(Rect{sx - arm, sy - 1.0f, arm * 2.0f, 2.0f}, gold);
        ui.fill(Rect{sx - 1.0f, sy - arm, 2.0f, arm * 2.0f}, gold);
        ui.fill(Rect{sx - 1.5f, sy - 1.5f, 3.0f, 3.0f}, white);
        // Two small twinkles circling the star.
        const float a = t * 6.2831853f;
        const float r = arm + 3.0f;
        ui.fill(Rect{sx + std::cos(a) * r - 1.0f, sy + std::sin(a) * r * 0.5f - 1.0f, 2.0f, 2.0f}, white);
        ui.fill(Rect{sx - std::cos(a) * r - 1.0f, sy - std::sin(a) * r * 0.5f - 1.0f, 2.0f, 2.0f}, gold);
    }
}

// --- The loot window (window stack, ui_world_windows.cpp) -------------------------
//
// The loot window like the original: the
// original LootFrame.xml / LootFrame.lua, drawn at 0.72 of its 256 x 256 units:
//   - UI-LootPanel, the TargetDead skull in its portrait circle, ITEMS
//     at CENTER -12,+102, the close button at TOPRIGHT -81,-26;
//   - LootButton1..4 (ItemButtonTemplate 37 x 37 at 24,-80, 4 apart): the item
//     icon in UI-Quickslot2, the count in the icon's corner, the grey
//     UI-QuestItemNameFrame, the name in its quality colour (2 lines);
//   - the slots are the server's: the coin first (LootSlotIsCoin), taken
//     slots leave a gap, pages of 4 (3 + the arrows when there are more),
//     PREV / NEXT with the scroll arrows (LootFrame_Update);
//   - the coin: GetCoinIcon and the amounts in words (GOLD/SILVER/COPPER_AMOUNT).
// PSP: Cross on a button takes it (LootButton_OnClick), Triangle takes all
// (auto-loot's packets - the client's shift-click; nothing on screen).
namespace {

constexpr float kLootScale = 0.72f;
constexpr uint32_t kIdLootUp = 0x6243, kIdLootDown = 0x6244;

// The coin slot's text: GOLD_AMOUNT / SILVER_AMOUNT / COPPER_AMOUNT (their |4 plural by the count), one per line.
std::string coinWords(uint32_t amount) {
    const game::CoinAmount c = game::splitCopper(amount);
    std::string out;
    auto add = [&](uint32_t v, pp::CS fmt) {
        if (!out.empty()) out += '\n';
        out += pp::text::plural(pp::csf(fmt, static_cast<unsigned>(v)));
    };
    if (c.gold) add(c.gold, pp::CS::GOLD_AMOUNT);
    if (c.silver) add(c.silver, pp::CS::SILVER_AMOUNT);
    if (c.copper || out.empty()) add(c.copper, pp::CS::COPPER_AMOUNT);
    return out;
}

// Up to two lines of `text` (hard breaks kept, words wrapped) inside `box`,
// centred vertically.
void drawNameBox(UiRenderer& ui, const Font& f, const Rect& box, const std::string& text, uint32_t col) {
    std::string lines[2];
    int n = 0;
    std::string word, cur;
    auto push = [&]() {
        if (n < 2) lines[n++] = cur;
        cur.clear();
    };
    for (size_t i = 0; i <= text.size() && n < 2; ++i) {
        const char ch = i < text.size() ? text[i] : '\n';
        if (ch != ' ' && ch != '\n') {
            word += ch;
            continue;
        }
        if (!word.empty()) {
            const std::string t = cur.empty() ? word : cur + " " + word;
            if (!cur.empty() && static_cast<float>(f.measure(t.c_str())) > box.w) {
                push();
                cur = word;
            } else {
                cur = t;
            }
            word.clear();
        }
        if (ch == '\n' && !cur.empty()) push();
    }
    if (n < 2 && !cur.empty()) push();
    const float lh = static_cast<float>(f.lineHeight());
    float y = box.y + (box.h - lh * n) * 0.5f;
    ui.pushClip(box);
    for (int i = 0; i < n; ++i, y += lh) ui.textShadow(f, box.x, y, lines[i].c_str(), col);
    ui.popClip();
}

}  // namespace

WorldScreen::LootArt::~LootArt() {
    for (pp::Texture* t : {&panel, &dead, &nameFrame, &close, &closeDown, &up, &down})
        if (t->pixels) pp::textureRelease(*t);
}

void WorldScreen::loadLootArt() {
    if (lootArt_.tried && lootArt_.artRetry.due()) lootArt_.tried = false;  // failed - again at the next open
    if (lootArt_.tried) return;
    lootArt_.tried = true;
    UiArtPak pak("loot");  // ui_art_fail when the pak / a texture finds no memory
    int ok = 0;
    if (pak.isOpen()) {
        struct { const char* name; pp::Texture* tex; } art[] = {
            {"loot_panel", &lootArt_.panel}, {"loot_dead", &lootArt_.dead},   {"loot_namefr", &lootArt_.nameFrame},
            {"loot_close", &lootArt_.close}, {"loot_close_dn", &lootArt_.closeDown},
            {"loot_up", &lootArt_.up},       {"loot_down", &lootArt_.down},
        };
        for (auto& a : art)
            if (pak.load(a.name, *a.tex)) ++ok;
    }
    if (pak.failed()) lootArt_.artRetry.fail();
    pak.close();
    PSP_LOGI(pp::tag::Gfx, "loot_art ok=%d of 7 retry=%d", ok, pak.failed() ? 1 : 0);
}

pp::ItemIconDb& WorldScreen::itemIcons() {
    // itemicons.pak there but not opened (no memory for its TOC, no descriptor) - asked again at
    // the next window open (was: "missing", items without icons for the session; seen on hardware).
    if (itemIconsTried_ && itemIconsRetry_.due()) itemIconsTried_ = false;
    if (!itemIconsTried_) {
        itemIconsTried_ = true;
        if (!itemIcons_.open() && itemIcons_.openFailedTransient()) {
            itemIconsRetry_.fail();
            UiArtPak::logFail("itemicons", "itemicons.pak");
        }
    }
    return itemIcons_;
}

bool WorldScreen::drawLootWindow(UiRenderer& ui, uint8_t w) {
    if (w != kWinLoot) return false;
    if (!session_.isLootWindowOpen()) return true;  // closing this frame
    loadLootArt();
    const auto& loot = session_.getCurrentLoot();
    constexpr float s = kLootScale;
    const float fx = 4.0f, fy = 56.0f;  // LootFrame: TOPLEFT 0,-104 - under the player frame
    auto R = [&](float x, float y, float w2, float h2) { return Rect{fx + x * s, fy + y * s, w2 * s, h2 * s}; };
    const Font& fn = kit_.font(kFontNormal);
    const Font& fs = kit_.font(kFontSmall);

    // BORDER: the skull; ARTWORK: the parchment over it (its circle is open).
    if (lootArt_.dead.pixels) ui.image(lootArt_.dead, R(10, 8, 58, 58));
    if (lootArt_.panel.pixels) ui.image(lootArt_.panel, R(0, 0, 256, 256));
    else kit_.panel(ui, R(0, 0, 186, 256));
    ui.textShadow(fn, fx + 116 * s, fy + 26 * s - fn.lineHeight() * 0.5f, CLIENT_STR(ITEMS), kColGold, Align::Center);
    {
        const Rect cr = R(159, 10, 32, 32);
        const bool f = winFocus_.addStop(kIdLootClose, cr);
        if (f && winFocus_.isPressed(kIdLootClose)) lootPressSeen_ = true;
        const pp::Texture& t = f && winFocus_.isPressed(kIdLootClose) && lootArt_.closeDown.pixels ? lootArt_.closeDown : lootArt_.close;
        if (t.pixels) ui.image(t, cr);
        else ui.textShadow(fn, cr.x + cr.w * 0.5f, cr.y + 4, "x", kColGold, Align::Center);
        if (f && spellArt_.hilite.pixels) ui.image(spellArt_.hilite, cr, 0xFFFFFFFFu, Blend::Add);
    }

    // The slots: [coin] + the items in the server's order, taken ones stay as gaps.
    struct Slot {
        bool coin;
        const game::LootItem* item;
    };
    Slot slots[65];
    int numLoot = 0;
    if (loot.gold > 0) slots[numLoot++] = Slot{true, nullptr};
    for (const auto& it : loot.items)
        if (numLoot < 65) slots[numLoot++] = Slot{false, &it};
    const int perPage = numLoot > 4 ? 3 : 4;
    const int pages = std::max(1, (numLoot + perPage - 1) / perPage);
    lootPage_ = std::clamp(lootPage_, 1, pages);
    const uint32_t frame = spellFrame_;
    for (int index = 0; index < perPage; ++index) {
        const int k = perPage * (lootPage_ - 1) + index;
        if (k >= numLoot) break;
        const Slot& sl = slots[k];
        if (sl.coin ? !loot.goldLeft() : sl.item->looted) continue;  // LootSlotIsItem / IsCoin: false
        const float by = 80.0f + index * 41.0f;
        const Rect icon = R(24, by, 37, 37);
        const uint32_t id = sl.coin ? kIdLootMoney : kIdLootSlot + sl.item->slotIndex;
        const bool focused = winFocus_.addStop(id, Rect{icon.x, icon.y, (54 + 130 - 24) * s, icon.h});
        if (focused && winFocus_.isPressed(id)) lootPressSeen_ = true;
        if (focused && !sl.coin) {  // LootFrameItem_OnEnter - GameTooltip:SetLootItem (drawn after the windows)
            tipProtoItem_ = sl.item->itemId;
            tipProtoCount_ = std::max<uint32_t>(1u, sl.item->count);
            tipProtoRect_ = Rect{icon.x + ui.offsetX(), icon.y + ui.offsetY(), icon.w, icon.h};
        }
        // Name plate (vertex colour 0.5 unless locked), then the icon and its frame.
        if (lootArt_.nameFrame.pixels) ui.image(lootArt_.nameFrame, R(54, by + 18.5f - 31, 130, 62), 0xFF808080u);
        const pp::Texture* tex = nullptr;
        std::string name;
        uint32_t col = kColGold;
        if (sl.coin) {
            tex = itemIcons().coin(loot.gold, frame);
            name = coinWords(loot.gold);
            col = 0xFFFFFFFFu;
        } else {
            const auto* info = session_.lootItemInfo(sl.item->itemId);
            tex = itemIcons().byDisplay(sl.item->displayInfoId, frame);
            name = info && !info->name.empty() ? info->name : std::string();
            col = qualityColour(info ? info->quality : 1);
        }
        if (tex) ui.image(*tex, icon);
        else ui.fill(icon, 0xFF202020u);
        if (spellArt_.slot.pixels) {
            const float c = 64.0f * s;
            ui.image(spellArt_.slot, Rect{icon.x + icon.w * 0.5f - c * 0.5f, icon.y + icon.h * 0.5f - c * 0.5f, c, c});
        }
        if (focused && spellArt_.hilite.pixels) ui.image(spellArt_.hilite, icon, 0xFFFFFFFFu, Blend::Add);
        if (!sl.coin && sl.item->count > 1) {
            char cnt[12];
            std::snprintf(cnt, sizeof(cnt), "%u", static_cast<unsigned>(sl.item->count));
            ui.textShadow(fs, icon.x + icon.w - 2, icon.y + icon.h - fs.lineHeight() - 1, cnt, 0xFFFFFFFFu, Align::Right);
        }
        drawNameBox(ui, fs, R(69, by + 18.5f - 19, 93, 38), name, col);
    }
    // The PSP buttons of this window under the frame (how to loot).
    {
        float hx = fx + 12;
        const float hy = fy + 256 * s + 1;
        auto hint = [&](PadGlyph g, const char* text) {
            const float gw = kit_.glyphWidth(g, 12.0f);  // START is a word
            if (kit_.glyph(ui, g, Rect{hx, hy, gw, 12})) hx += gw + 2;
            ui.textShadow(fs, hx, hy, text, 0xFFFFFFFFu);
            hx += static_cast<float>(fs.measure(text)) + 8;
        };
        hint(kPadCross, OWN_STR(LOOT_HINT_TAKE));
        hint(kPadUp, OWN_STR(LOOT_HINT_ALL));
        hint(kPadStart, OWN_STR(LOOT_HINT_CLOSE));  // START = Esc (Circle is the right click now)
    }
    // Pages (LootFrame_Update): the arrows and PREV / NEXT.
    if (lootPage_ > 1) {
        const Rect ur = R(25, 208, 32, 32);
        const bool f = winFocus_.addStop(kIdLootUp, ur);
        if (lootArt_.up.pixels) ui.image(lootArt_.up, ur);
        if (f && spellArt_.hilite.pixels) ui.image(spellArt_.hilite, ur, 0xFFFFFFFFu, Blend::Add);
        ui.textShadow(fn, fx + 57 * s, fy + 229 * s - fn.lineHeight(), CLIENT_STR(PREV), kColGold);
    }
    if (lootPage_ < pages) {
        const Rect dr = R(143, 208, 32, 32);
        const bool f = winFocus_.addStop(kIdLootDown, dr);
        if (lootArt_.down.pixels) ui.image(lootArt_.down, dr);
        if (f && spellArt_.hilite.pixels) ui.image(spellArt_.hilite, dr, 0xFFFFFFFFu, Blend::Add);
        ui.textShadow(fn, fx + 142 * s, fy + 229 * s - fn.lineHeight(), CLIENT_STR(NEXT), kColGold, Align::Right);
    }
    return true;
}

// PSP: Triangle on the open loot window takes everything (LootFrame has no
// button for it; the client's shift-click / auto-loot).
// (Looting failed: Up, the key that opened the window, moved
// the focus onto the close button above the items and Cross closed it): Up -
// the same key again - and Triangle take everything; Up never moves the focus
// here.
void WorldScreen::lootWindowPad(pp::PadState& pad) {
    if (!windowOpen(kWinLoot) || !session_.isLootWindowOpen()) return;
    const bool take = (pad.pressed & (PSP_CTRL_TRIANGLE | PSP_CTRL_UP)) != 0;
    pad.held &= ~PSP_CTRL_UP;
    pad.pressed &= ~PSP_CTRL_UP;
    pad.released &= ~PSP_CTRL_UP;
    if (!take) return;
    lootPressSeen_ = true;
    lootWindowAction(kIdLootAll);
}

bool WorldScreen::lootWindowAction(uint32_t act) {
    if (act == kIdLootDebug) {
        // Debug: the target, else the nearest lootable corpse.
        uint64_t guid = session_.target();
        const bool handled = tryLootTarget(guid);
        if (!handled) {
            float best = 1e30f;
            guid = 0;
            for (uint64_t g : session_.lootableCorpses()) {
                auto e = session_.entities().getEntity(g);
                if (!e) continue;
                const glm::vec3 p = core::coords::canonicalToServer(glm::vec3(e->getX(), e->getY(), e->getZ()));
                const glm::vec3 d(p.x - playerX_, p.y - playerY_, p.z - playerZ_);
                const float dsq = glm::dot(d, d);
                if (dsq < best) {
                    best = dsq;
                    guid = g;
                }
            }
            if (guid == 0 || !tryLootTarget(guid)) {
                combatError_ = OWN_STR(LOOT_NOTHING_NEAR);
                combatErrorUntilMs_ = pp::logUptimeMs() + 2000u;
            }
        }
        PSP_LOGI(pp::tag::Ctrl, "debug_set loot_target=%08X", lo32(guid));
        return true;
    }
    if (act == kIdLootUp || act == kIdLootDown) {  // LootFrame_PageUp / PageDown
        lootPage_ += act == kIdLootDown ? 1 : -1;
        return true;
    }
    const bool mine = act == kIdLootMoney || act == kIdLootAll || act == kIdLootClose ||
                      (act >= kIdLootSlot && act < kIdLootSlot + 64);
    if (!mine) return false;
    // FocusRing activates on release: only a press seen on the window counts
    // (not a Cross released that was held when the window came up).
    if (!lootPressSeen_) return true;
    lootPressSeen_ = false;
    if (!session_.isLootWindowOpen()) return true;
    const auto& loot = session_.getCurrentLoot();
    if (act == kIdLootClose) {
        session_.closeLoot();
    } else if (act == kIdLootMoney) {
        session_.lootMoney();  // upstream LootSlot(1) on the coin
        playSoundByName("LOOTWINDOWCOINSOUND");
    } else if (act == kIdLootAll) {
        // PSP: Triangle - the packets of upstream's auto-loot path (the
        // client's shift-click / auto-loot), no button on the frame.
        if (loot.goldLeft()) session_.lootMoney();
        std::vector<uint8_t> slots;
        for (const auto& it : loot.items)
            if (!it.looted) slots.push_back(it.slotIndex);
        for (uint8_t s : slots) session_.lootItem(s, true);
    } else {
        // PSP: the bind-on-pickup confirmation (LOOT_BIND) is still answered
        // yes here (the Lua interface asks through hasPendingLootConfirm).
        session_.lootItem(static_cast<uint8_t>(act - kIdLootSlot), true);
    }
    return true;
}

// The loot button in the Debug window title row (debug check of tryLootTarget).
void WorldScreen::drawLootDebugRow(UiRenderer& ui, const Rect& dbg) {
    // Debug sits on the right above the window bar, so the
    // button lives in its title row, left (the light button of task G is on the right).
    const std::string line = OWN_FMT(LOOT_DEBUG_BUTTON, static_cast<unsigned>(session_.lootableCorpses().size()));
    kit_.button(ui, winFocus_, kIdLootDebug, Rect{dbg.x + 6, dbg.y + 4, 58, 20}, line.c_str());
}

}  // namespace wowee::ui::psp
