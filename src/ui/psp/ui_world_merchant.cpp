// The merchant window of the original - MerchantFrame.xml / .lua
// (the merchant like the original, with item icons).
//
//   - the frame composed by tools/psp/build_ui_pak.py (merch_frame: the
//     merchant tab, merch_buyback: the buyback tab) at 0.6 x 0.55 of its
//     frame units, so every position below is the FrameXML one through FR();
//   - the merchant tab: 10 items a page (MERCHANT_ITEMS_PER_PAGE) in two
//     columns - ItemButtonTemplate (icon, count, stock "(N)"), the name in its
//     quality colour, the price (SmallMoneyFrame); red when the item is not
//     usable (required level), grey when sold out; the last sold item in the
//     buyback slot; MERCHANT_PAGE_NUMBER with PREV / NEXT when there are more
//     than 10; REPAIR_ITEMS with REPAIR_ALL_ITEMS for a repairer; the player's money;
//   - the buyback tab: the 12 slots of the server's ring (BUYBACK_ITEMS_PER_PAGE);
//   - the tabs MERCHANT / BUYBACK; the backpack opens with the window
//     (MerchantFrame_OnShow: OpenBackpack) and Cross on a bag item sells it
//     (the original's right click, ui_world_inventory.cpp);
//   - Cross on an item buys it (BuyMerchantItem: one purchase), on a buyback
//     item buys it back; the focused item shows its tooltip (GameTooltip).
// PSP: no "repair one item" button (it needs the item cursor), no
// ItemExtendedCost.dbc - token prices show TOKENS; errors on the red line
// (ERR_* of GlobalStrings ruRU, psp_world_session_npc.cpp).
#include "ui/psp/ui_world_screen.hpp"

#include "game/update_field_table.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"
#include "platform/psp/psp_texture.hpp"
#include "ui/psp/ui_world_sounds.hpp"

#include <algorithm>
#include <cstdio>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

// Widget ids: 0x6A00-0x6AFF.
enum : uint32_t {
    kIdItem0 = 0x6A00,   // 0x6A00..0x6A0B: the slots of the page
    kIdBuybackSlot = 0x6A10,
    kIdPrev,
    kIdNext,
    kIdRepairAll,
    kIdTab1,
    kIdTab2,
    kIdClose,
};

// MerchantFrame units -> px (tools/psp/build_ui_pak.py MERCH_*): x from -4, 0.6 x 0.55.
constexpr float kSX = 0.6f, kSY = 0.55f, kX0 = -4.0f;
constexpr float kOx = 4.0f, kOy = 2.0f;                     // TOPLEFT 0,-104 -> the left, at the top
constexpr float kFrameW = 212.0f, kFrameH = 265.0f;
float PX(float x) { return kOx + (x - kX0) * kSX; }
float PY(float y) { return kOy + y * kSY; }
constexpr float kIcon = 21.0f;                              // ItemButton 37 units, square on the PSP
constexpr int kPerPage = 10, kBuybackSlots = 12;

uint32_t qualityCol(uint32_t q) {  // ITEM_QUALITY_COLORS as 0xAABBGGRR
    switch (q) {
        case 0: return 0xFF9D9D9Du;
        case 2: return 0xFF00FF1Eu;
        case 3: return 0xFFDD7000u;
        case 4: return 0xFFEE35A3u;
        case 5: return 0xFF0080FFu;
        case 6:
        case 7: return 0xFF80CCE6u;
        default: return 0xFFFFFFFFu;
    }
}

// MerchantItem<i> TOPLEFT: two columns 165 apart, rows 52 (merchant) / 59 (buyback) apart.
void itemOrigin(int i, bool buyback, float& x, float& y) {
    x = 24.0f + static_cast<float>(i % 2) * 165.0f;
    y = 80.0f + static_cast<float>(i / 2) * (buyback ? 59.0f : 52.0f);
}

}  // namespace

WorldScreen::MerchantUi::~MerchantUi() { release(); }

void WorldScreen::MerchantUi::release() {
    for (pp::Texture* t : {&frame, &prev, &prevDis, &next, &nextDis, &repair, &bbIcon})
        if (t->pixels) pp::textureRelease(*t);
    frameTab = 0;
    smallTried = false;
}

void WorldScreen::loadMerchantArt() {
    // What failed (no pak, no memory: ui_art_fail) is loaded again at the next window
    // open - was: the plain merchant for the rest of the session (seen on hardware).
    if (merch_.artRetry.due()) {
        merch_.smallTried = false;
        if (!merch_.frame.pixels) merch_.frameTab = 0;
    }
    if (merch_.frameTab == merch_.tab && merch_.smallTried) return;
    UiArtPak art("merchant");
    if (!art.isOpen()) {
        merch_.frameTab = merch_.tab;
        merch_.smallTried = true;
        merch_.artRetry.fail();
        return;
    }
    if (merch_.frameTab != merch_.tab) {  // one tab's frame at a time (128 KB)
        if (merch_.frame.pixels) pp::textureRelease(merch_.frame);
        art.load(merch_.tab == 1 ? "merch_frame" : "merch_buyback", merch_.frame);
        merch_.frameTab = merch_.tab;
    }
    if (!merch_.smallTried) {
        merch_.smallTried = true;
        art.load("merch_prev", merch_.prev);
        art.load("merch_prev_dis", merch_.prevDis);
        art.load("merch_next", merch_.next);
        art.load("merch_next_dis", merch_.nextDis);
        art.load("merch_repair", merch_.repair);
        art.load("merch_bbicon", merch_.bbIcon);
    }
    if (art.failed()) merch_.artRetry.fail();
    art.close();
    PSP_LOGI(pp::tag::Gfx, "merchant_art tab=%d frame=%d buttons=%d retry=%d", merch_.tab, merch_.frame.pixels ? 1 : 0,
             merch_.prev.pixels ? 1 : 0, art.failed() ? 1 : 0);
}

// Per frame (updateNpc): the backpack with the window, the error line, the art.
void WorldScreen::updateMerchant() {
    if (!windowOpen(kWinQuestLog) && qlog_.tried) {  // the quest log's art only while it is open
        qlog_.release();  // (its abandon popup closes itself - drawQuestAbandon)
    }
    if (!windowOpen(kWinBags) && bagArt_.tried) bagArt_.release();       // the frames' art
    if (!windowOpen(kWinCharacter) && pdArt_.tried) pdArt_.release();  // only while open
    if (!windowOpen(kWinCharacter)) charTabsClosed();  // the Reputation / Skills tabs' art and texts
    const bool open = windowOpen(kWinVendor);
    if (open && !merch_.shown) {
        merch_.shown = true;
        merch_.tab = 1;
        merch_.page = 0;
        uint32_t ms = 0;
        session_.lastInventoryError(&ms);
        merch_.errSeenMs = ms;
        // MerchantFrame_OnShow: BACKPACK_WAS_OPEN = OpenBackpack().
        merch_.openedBags = !windowOpen(kWinBags);
        if (merch_.openedBags) {
            openBags(false);  // OpenBackpack
            openWindow(kWinVendor);  // the merchant stays on top (it has the focus)
        }
        winFocus_.setFocus(kIdItem0);
        PSP_LOGI(pp::tag::Ctrl, "merchant_show items=%u bags=%d",
                 static_cast<unsigned>(session_.getVendorItems().items.size()), merch_.openedBags ? 1 : 0);
    } else if (!open && merch_.shown) {
        merch_.shown = false;
        if (merch_.openedBags && windowOpen(kWinBags)) closeWindow(kWinBags);  // if (not BACKPACK_WAS_OPEN) CloseBackpack()
        merch_.openedBags = false;
        merch_.release();
    }
    if (!open) return;
    // UI_ERROR_MESSAGE (buy / sell failures) on the red line under the frames.
    uint32_t ms = 0;
    const std::string& err = session_.lastInventoryError(&ms);
    if (ms != merch_.errSeenMs) {
        merch_.errSeenMs = ms;
        if (!err.empty()) {
            combatError_ = err;
            combatErrorUntilMs_ = pp::logUptimeMs() + 2000u;
        }
    }
}

float WorldScreen::drawMoneyFrame(UiRenderer& ui, float x, float y, uint32_t copper, bool right) {
    loadNpcArt(false);  // the coins (money_icons) live with the NPC windows' small art
    const Font& fs = kit_.font(kFontSmall);
    const uint32_t parts[3] = {copper / 10000u, (copper / 100u) % 100u, copper % 100u};
    char txt[3][12] = {};
    float w = 0.0f;
    for (int p = 0; p < 3; ++p) {
        if (parts[p] == 0 && !(p == 2 && parts[0] == 0 && parts[1] == 0)) continue;
        std::snprintf(txt[p], sizeof(txt[p]), "%u", static_cast<unsigned>(parts[p]));
        w += static_cast<float>(fs.measure(txt[p])) + 1.0f + 12.0f;
    }
    float cx = right ? x - w : x;
    for (int p = 0; p < 3; ++p) {
        if (!txt[p][0]) continue;
        ui.textShadow(fs, cx, y, txt[p], kColWhite);
        cx += static_cast<float>(fs.measure(txt[p])) + 1.0f;
        if (npcArt_.money.pixels) {
            const float uw = npcArt_.money.uw * 0.25f;
            ui.image(npcArt_.money, Rect{cx, y + 1, 10, 10},
                     SrcRect{uw * static_cast<float>(p), 0, uw, static_cast<float>(npcArt_.money.uh)});
        }
        cx += 12.0f;
    }
    return w;
}

void WorldScreen::drawItemButton(UiRenderer& ui, const Rect& icon, uint32_t displayId, uint32_t count, int stock,
                                 uint32_t tint) {
    const Font& fs = kit_.font(kFontSmall);
    if (const pp::Texture* t = itemIcons().byDisplay(displayId, spellFrame_)) ui.image(*t, icon, tint);
    else ui.fill(icon, 0xFF202020u);
    if (spellArt_.slot.pixels) {  // NormalTexture UI-Quickslot2 64 x 64 around the 37 px button
        const float c = icon.w * 64.0f / 37.0f;
        ui.image(spellArt_.slot, Rect{icon.x + icon.w * 0.5f - c * 0.5f, icon.y + icon.h * 0.5f - c * 0.5f, c, c},
                 tint);
    }
    char b[16];
    if (count > 1) {
        std::snprintf(b, sizeof(b), "%u", static_cast<unsigned>(count));
        ui.textShadow(fs, icon.x + icon.w + 1, icon.y + icon.h - fs.lineHeight() + 2, b, kColWhite, Align::Right);
    }
    if (stock >= 0) {
        std::snprintf(b, sizeof(b), "(%d)", stock);  // MERCHANT_STOCK
        ui.textShadow(fs, icon.x - 1, icon.y - 3, b, 0xFF00FFFFu);
    }
}

bool WorldScreen::drawMerchantWindow(UiRenderer& ui, uint8_t w) {
    if (w != kWinVendor) return false;
    loadMerchantArt();
    if (!merch_.frame.pixels) return false;  // no art baked: the plain window of ui_world_npc.cpp
    PROF_SCOPE("npc.merchant");
    loadNpcArt(false);
    const auto& v = session_.getVendorItems();
    // Every open window takes the focus (the covered parts not: FocusRing occluders).
    const bool top = uiMode_ && !settings_.isOpen() && !windowOpen(kWinBindConfirm);
    const Font& fs = kit_.font(kFontSmall);
    const Font& fn = kit_.font(kFontNormal);
    const float lh = static_cast<float>(fs.lineHeight());
    const uint32_t playerLevel = session_.selfField(game::fieldIndex(game::UF::UNIT_FIELD_LEVEL));
    auto pressSeen = [&](uint32_t id) {
        if (winFocus_.isPressed(id)) npc_.guard = false;
    };

    // Portrait (60 x 60 at 7,-6) under the frame: the NPC, the buyback icon on its tab.
    const Rect pr{PX(7), PY(6), 60 * kSX, 60 * kSY};
    // Round (SetPortraitTexture / SetPortraitToTexture are circular; a square stuck out).
    if (merch_.tab == 2 && merch_.bbIcon.pixels) ui.imageCircle(merch_.bbIcon, pr);
    else if (const pp::Texture* face = portraitOf(v.vendorGuid)) ui.imageCircle(*face, pr);
    else ui.fillCircle(Rect{pr.x + 2, pr.y + 2, pr.w - 4, pr.h - 4}, 0xFF101010u);
    ui.image(merch_.frame, Rect{kOx, kOy, kFrameW, kFrameH});

    // MerchantNameText (TOP 0,-17, GameFontNormal): the NPC, MERCHANT_BUYBACK on the buyback tab.
    {
        UnitView u;
        const std::string name = merch_.tab == 2 ? std::string(CLIENT_STR(MERCHANT_BUYBACK))
                                                 : unitView(v.vendorGuid, u) && !u.name.empty() ? u.name
                                                                                                : std::string();
        const Rect nr{PX(76), PY(13), (318 - 76) * kSX, 16};
        if (static_cast<float>(fn.measure(name.c_str())) <= nr.w)
            ui.textShadow(fn, nr.x + nr.w * 0.5f, nr.y, name.c_str(), kColGold, Align::Center);
        else
            ui.textMarquee(fs, nr, name.c_str(), kColGold);
    }
    // MerchantFrameCloseButton (UIPanelCloseButton, TOPRIGHT -30,-8).
    {
        const Rect cr{PX(326) - 1, PY(12) - 1, 18, 18};
        const bool f = top && winFocus_.addStop(kIdClose, cr);
        if (f) pressSeen(kIdClose);
        const bool p = f && winFocus_.isPressed(kIdClose);
        const pp::Texture& t = p && npcArt_.closeDown.pixels ? npcArt_.closeDown : npcArt_.close;
        if (t.pixels) ui.image(t, cr);
        if (f && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, cr, 0xFFFFFFFFu, Blend::Add);
    }

    // One item slot: the button, the name, the price, the focus; its tooltip comes after.
    uint32_t tipItem = 0, tipCount = 1;
    Rect tipRect{};
    auto slot = [&](uint32_t id, float ux, float uy, uint32_t itemId, uint32_t displayId, uint32_t count, int stock,
                    uint32_t price, bool tokens, bool soldOut, bool nameplate64) {
        const Rect icon{PX(ux), PY(uy), kIcon, kIcon};
        const auto* info = itemId ? session_.getItemInfo(itemId) : nullptr;
        if (itemId && (!info || !info->valid)) session_.ensureItemInfo(itemId);
        if (info && info->valid && displayId == 0) displayId = info->displayInfoId;
        const bool usable = !info || !info->valid || info->requiredLevel <= playerLevel;
        // MerchantFrame_UpdateMerchantInfo: grey when sold out, red when not usable.
        const uint32_t tint = soldOut ? 0xFF808080u : usable ? 0xFFFFFFFFu : 0xFF0000E6u;
        const Rect hit{icon.x - 1, icon.y - 1, 153 * kSX, 40 * kSY};
        const bool f = itemId && top && winFocus_.addStop(id, hit);
        if (f) pressSeen(id);
        if (itemId) drawItemButton(ui, icon, displayId, count, stock, tint);
        if (f) {
            ui.fill(icon, 0x50FFFFFFu);  // ButtonHilight-Square
            tipItem = itemId;
            tipCount = count;
            tipRect = Rect{icon.x + ui.offsetX(), icon.y + ui.offsetY(), icon.w, icon.h};
        }
        if (!itemId) return;
        // Name: GameFontNormalSmall in the quality colour, 1 line (PSP font), a marquee when focused.
        const std::string name = info && info->valid ? info->name : std::string();
        const uint32_t col = !usable ? kColRed : qualityCol(info && info->valid ? info->quality : 1);
        const Rect nr{PX(ux + 46), PY(uy + (nameplate64 ? 9.0f : 12.0f)) - lh * 0.5f, (nameplate64 ? 120 : 118) * kSX,
                      lh};
        if (f && static_cast<float>(fs.measure(name.c_str())) > nr.w) {
            ui.textMarquee(fs, nr, name.c_str(), col);
        } else {
            ui.pushClip(nr);
            ui.textShadow(fs, nr.x, nr.y, name.c_str(), col);
            ui.popClip();
        }
        const float my = PY(uy + (nameplate64 ? 36.0f : 45.0f)) - 11.0f;  // SmallMoneyFrame, BOTTOMLEFT
        if (price > 0) drawMoneyFrame(ui, PX(ux + 44), my, price, false);
        else if (tokens) ui.textShadow(fs, PX(ux + 44), my, CLIENT_STR(TOKENS), kColGrey);
    };

    if (merch_.tab == 1) {
        const int total = static_cast<int>(v.items.size());
        const int pages = std::max(1, (total + kPerPage - 1) / kPerPage);
        merch_.page = std::clamp(merch_.page, 0, pages - 1);
        for (int i = 0; i < kPerPage; ++i) {
            const int idx = merch_.page * kPerPage + i;
            float ux, uy;
            itemOrigin(i, false, ux, uy);
            if (idx >= total) continue;
            const auto& it = v.items[static_cast<size_t>(idx)];
            slot(kIdItem0 + static_cast<uint32_t>(i), ux, uy, it.itemId, it.displayInfoId, it.stackCount,
                 it.maxCount, it.buyPrice, it.extendedCost != 0, it.maxCount == 0, false);
        }
        // Pages (MerchantPageText BOTTOM -14,150; the buttons CENTER at BOTTOMLEFT 37,156 / 324,156).
        if (total > kPerPage) {
            const std::string b = CLIENT_FMT(MERCHANT_PAGE_NUMBER, merch_.page + 1, pages);
            ui.textShadow(fs, PX(178), PY(362) - lh, b.c_str(), kColGold, Align::Center);
            auto pageBtn = [&](uint32_t id, float cxu, bool enabled, const pp::Texture& up, const pp::Texture& dis) {
                const Rect r{PX(cxu) - 9, PY(356) - 9, 18, 18};
                const bool f = enabled && top && winFocus_.addStop(id, r);
                if (f) pressSeen(id);
                const pp::Texture& t = enabled ? up : dis;
                if (t.pixels) ui.image(t, r);
                if (f) ui.fill(r, 0x40FFFFFFu);
            };
            pageBtn(kIdPrev, 37, merch_.page > 0, merch_.prev, merch_.prevDis);
            pageBtn(kIdNext, 324, merch_.page + 1 < pages, merch_.next, merch_.nextDis);
            ui.textShadow(fs, PX(53) + 2, PY(356) - lh * 0.5f, CLIENT_STR(PREV), merch_.page > 0 ? kColGold : kColGrey);
            ui.textShadow(fs, PX(305), PY(356) - lh * 0.5f, CLIENT_STR(NEXT), merch_.page + 1 < pages ? kColGold : kColGrey,
                          Align::Right);
        }
        // MerchantBuyBackItem: the last item sold (GetBuybackItemInfo(GetNumBuybackItems())).
        const auto rows = session_.buybackRows();
        const pp::WorldSession::BuybackRow* last = nullptr;
        for (const auto& r : rows)
            if (!last || r.time >= last->time) last = &r;
        if (last) slot(kIdBuybackSlot, 189, 385, last->itemId, 0, last->count, -1, last->price, false, false, true);
        // Repair (MerchantRepairText BOTTOMLEFT 26,103; RepairAll 36 x 36 at BOTTOMLEFT 136,91).
        if (v.canRepair) {
            ui.textShadow(fs, PX(26), PY(409) - lh, CLIENT_STR(REPAIR_ITEMS), kColWhite);
            const Rect r{PX(154) - 10, PY(403) - 10, 20, 20};
            const bool f = top && winFocus_.addStop(kIdRepairAll, r);
            if (f) pressSeen(kIdRepairAll);
            if (merch_.repair.pixels) {
                const float hw = merch_.repair.uw * 0.5f;
                ui.image(merch_.repair, r, SrcRect{hw, 0, hw, static_cast<float>(merch_.repair.uh)});
            } else {
                ui.fill(r, 0xFF303030u);
            }
            if (f) {
                ui.fill(r, 0x40FFFFFFu);
                kit_.tooltip(ui, r, CLIENT_STR(REPAIR_ALL_ITEMS), "");
            }
        }
    } else {
        const auto rows = session_.buybackRows();
        for (int i = 0; i < kBuybackSlots && i < static_cast<int>(rows.size()); ++i) {
            float ux, uy;
            itemOrigin(i, true, ux, uy);
            const auto& r = rows[static_cast<size_t>(i)];
            slot(kIdItem0 + static_cast<uint32_t>(i), ux, uy, r.itemId, 0, r.count, -1, r.price, false, false, false);
        }
    }
    // MerchantMoneyFrame (BOTTOMRIGHT -40,67): the player's money.
    drawMoneyFrame(ui, PX(344), PY(445) - 11.0f, static_cast<uint32_t>(session_.getMoneyCopper()), true);

    // Tabs (CENTER at BOTTOMLEFT 60,46 and its RIGHT -16): GameFontHighlightSmall on the selected one.
    auto tabBtn = [&](uint32_t id, float cxu, const char* label, bool selected) {
        const float cy = PY(466 - (selected ? 5 : 0));
        const Rect r{PX(cxu) - 64 * kSX + 6, cy - 7, 128 * kSX - 12, 14};
        const bool f = top && winFocus_.addStop(id, r);
        if (f) pressSeen(id);
        ui.textShadow(fs, PX(cxu), cy - lh * 0.5f + 1, label, selected ? kColWhite : kColGold, Align::Center);
        if (f) ui.fill(r, 0x30FFFFFFu);
    };
    tabBtn(kIdTab1, 60, CLIENT_STR(MERCHANT), merch_.tab == 1);
    tabBtn(kIdTab2, 172, CLIENT_STR(BUYBACK), merch_.tab == 2);

    // MerchantItemButton_OnEnter: the item's tooltip (drawn after all the windows, drawWindows).
    if (tipItem) {
        tipProtoItem_ = tipItem;
        tipProtoCount_ = tipCount;
        tipProtoRect_ = tipRect;
    }
    return true;
}

bool WorldScreen::merchantWindowAction(uint32_t act) {
    if (act < 0x6A00 || act > 0x6AFF) return false;
    if (npc_.guard) return true;  // a Cross held since before the window came up
    const auto& v = session_.getVendorItems();
    switch (act) {
        case kIdClose: closeWindow(kWinVendor); return true;  // syncNpcWindows closes the session's
        case kIdTab1:
        case kIdTab2:
            merch_.tab = act == kIdTab1 ? 1 : 2;
            merch_.page = 0;
            playUiSound(UiSound::ButtonClick);
            return true;
        case kIdPrev:
            if (merch_.page > 0) --merch_.page;
            playUiSound(UiSound::ButtonClick);
            return true;
        case kIdNext:
            ++merch_.page;  // clamped when drawn
            playUiSound(UiSound::ButtonClick);
            return true;
        case kIdRepairAll: session_.repairAll(v.vendorGuid, false); return true;
        case kIdBuybackSlot: {
            const auto rows = session_.buybackRows();
            const pp::WorldSession::BuybackRow* last = nullptr;
            for (const auto& r : rows)
                if (!last || r.time >= last->time) last = &r;
            if (last) session_.buybackItem(last->wireSlot);
            return true;
        }
        default: break;
    }
    if (act >= kIdItem0 && act < kIdItem0 + kBuybackSlots) {
        const int i = static_cast<int>(act - kIdItem0);
        if (merch_.tab == 1) {
            const size_t idx = static_cast<size_t>(merch_.page * kPerPage + i);
            if (i < kPerPage && idx < v.items.size()) {
                const auto& it = v.items[idx];
                session_.buyItem(v.vendorGuid, it.itemId, it.slot, 1);  // BuyMerchantItem(index): one purchase
            }
        } else {
            const auto rows = session_.buybackRows();
            if (static_cast<size_t>(i) < rows.size()) session_.buybackItem(rows[static_cast<size_t>(i)].wireSlot);
        }
    }
    return true;
}

}  // namespace wowee::ui::psp
