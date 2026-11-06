// The spell book of the original - SpellBookFrame.xml / .lua
// ("a normal spell book window; the general tab and the clear-button
// button run together - why are they there at all").
//
//   - the frame composed by tools/psp/build_ui_pak.py (sb_frame: the book
//     icon, the UI-SpellbookPanel quarters, the twelve SpellButton
//     backgrounds, the selected SPELLBOOK tab) at 0.6 x 0.55 of its units;
//   - SpellBookTitleText SPELLBOOK, the close button;
//   - SpellButton1..12: two columns of six (ids 1..6 left, 7..12 right),
//     the icon in UI-Quickslot2, the name (GameFontNormal) and the rank
//     (SubSpellFont, brown) to its right, the cooldown, the spell being cast
//     checked (CheckButtonHilight);
//   - SpellBookSkillLineTab1..8 down the right edge (SpellBook-SkillLineTab
//     with the line's icon, the shown one checked, its name as the tooltip);
//   - SpellBookPrevPageButton / NextPageButton with PREV / NEXT and
//     SpellBookPageText PAGE_NUMBER; each tab keeps its page
//     (SPELLBOOK_PAGENUMBERS).
// Buttons (PSP): Cross / Square opens the controller slot picker; Circle casts.
// Legacy cursor path:
// Square = SpellButton_OnDrag (PickupSpell: the spell goes on the cursor; Cross
// or Square on an action button puts it there - PlaceAction, the old one
// comes onto the cursor; START drops it). Square on an action button picks
// its spell up (PickupAction) - this replaces the PSP "clear button".
// PSP: the list stays upstream's (no passives, the highest rank of each
// spell), so ShowAllSpellRanksCheckBox is not drawn; no pet book tab yet.
#include "ui/psp/ui_world_screen.hpp"

#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"
#include "platform/psp/psp_texture.hpp"

#include <pspctrl.h>

#include <algorithm>
#include <cstdio>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

// ui_world_spells.cpp ids (the actions stay there).
constexpr uint32_t kIdSlot0 = 0x5C10;
constexpr uint32_t kIdAssign0 = 0x6F80; // private overlay focus ring: twelve destinations
constexpr uint32_t kIdBook0 = 0x5C80;  // + 0..11 SpellButton1..12 in display order
constexpr uint32_t kIdBookPrev = 0x5CA0, kIdBookNext = 0x5CA1, kIdBookClose = 0x5CA3;
constexpr uint32_t kIdBookTab0 = 0x6760;  // ui_world_trainer.cpp: + 0..7 skill line tabs
constexpr int kSpellsPerPage = 12, kMaxTabs = 8;

// 0.7 wide (the other framed windows 0.6): the names' column, 103 units, holds 72 px of Cyrillic.
constexpr float kSX = 0.7f, kSY = 0.55f;  // tools/psp/build_ui_pak.py SB_SX / SB_SY
constexpr float kOx = 4.0f, kOy = 2.0f;   // TOPLEFT 0,-104 -> the left, at the top
constexpr float kFrameW = 246.0f, kFrameH = 259.0f;
float PX(float x) { return kOx + x * kSX; }
float PY(float y) { return kOy + y * kSY; }
constexpr uint32_t kColSubSpell = 0xFF003359u;  // SubSpellFont (0.35, 0.2, 0)

}  // namespace

WorldScreen::SpellBookArt::~SpellBookArt() { release(); }

void WorldScreen::SpellBookArt::release() {
    for (pp::Texture* t : {&frame, &tab, &checked, &prev, &prevDis, &next, &nextDis})
        if (t->pixels) pp::textureRelease(*t);
    tried = false;
}

void WorldScreen::loadSpellBookArt() {
    if (sbArt_.tried && sbArtRetry_.due()) sbArt_.tried = false;  // failed - again at the next window open
    if (sbArt_.tried) return;
    sbArt_.tried = true;
    UiArtPak art("spellbook");  // ui_art_fail when the pak / a texture finds no memory
    auto load = [&](const char* name, pp::Texture& t) { art.load(name, t); };
    load("sb_frame", sbArt_.frame);
    load("sb_tab", sbArt_.tab);
    load("sb_checked", sbArt_.checked);
    load("sb_prev", sbArt_.prev);
    load("sb_prev_dis", sbArt_.prevDis);
    load("sb_next", sbArt_.next);
    load("sb_next_dis", sbArt_.nextDis);
    if (art.failed()) sbArtRetry_.fail();
    art.close();
    PSP_LOGI(pp::tag::Gfx, "spellbook_art frame=%d tab=%d arrows=%d retry=%d", sbArt_.frame.pixels ? 1 : 0,
             sbArt_.tab.pixels ? 1 : 0, sbArt_.prev.pixels ? 1 : 0, art.failed() ? 1 : 0);
}

bool WorldScreen::drawSpellWindow(UiRenderer& ui, uint8_t w) {
    if (w != kWinSpellbook) return false;
    if (bookVersion_ != session_.knownSpellsVersion()) rebuildSpellbook();
    loadSpellBookArt();
    loadNpcArt(false);  // the close button
    PROF_SCOPE("spellbook");
    const bool focusable = uiMode_ && !settings_.isOpen() && !windowOpen(kWinBindConfirm);
    const Font& fs = kit_.font(kFontSmall);
    const Font& fn = kit_.font(kFontNormal);
    const Font& ft = kit_.font(kFontTiny);
    const float lhS = static_cast<float>(fs.lineHeight()), lhT = static_cast<float>(ft.lineHeight());

    if (sbArt_.frame.pixels) ui.image(sbArt_.frame, Rect{kOx, kOy, kFrameW, kFrameH});
    else kit_.panel(ui, Rect{kOx + 10, kOy + 6, kFrameW - 20, kFrameH - 30});
    // SpellBookTitleText (CENTER 6,230 = 198,26).
    ui.textShadow(fn, PX(198), PY(26) - static_cast<float>(fn.lineHeight()) * 0.5f, CLIENT_STR(SPELLBOOK), kColGold,
                  Align::Center);
    // SpellBookCloseButton (CENTER at TOPRIGHT -44,-25).
    {
        const Rect cr{PX(340) - 10, PY(25) - 10, 20, 20};
        const bool f = focusable && winFocus_.addStop(kIdBookClose, cr);
        const pp::Texture& t = f && winFocus_.isPressed(kIdBookClose) && npcArt_.closeDown.pixels ? npcArt_.closeDown
                                                                                                  : npcArt_.close;
        if (t.pixels) ui.image(t, cr);
        if (f) ui.fill(cr, 0x40FFFFFFu);
    }
    // SpellBookFrameTabButton1 (selected: GameFontHighlightSmall, CENTER 0,3 of the tab at BOTTOMLEFT 79,61).
    ui.textShadow(fs, PX(79), PY(451 - 3) - lhS * 0.5f, CLIENT_STR(SPELLBOOK), kColWhite, Align::Center);

    // SpellButton1..12.
    const int start = bookPage_ * kSpellsPerPage;
    const float side = std::min(37.0f * kSX, 37.0f * kSY);
    for (int k = 0; k < kSpellsPerPage; ++k) {
        const float ux = 34.0f + static_cast<float>(k / 6) * 157.0f, uy = 85.0f + static_cast<float>(k % 6) * 51.0f;
        const Rect icon{PX(ux) + (37.0f * kSX - side) * 0.5f, PY(uy) + (37.0f * kSY - side) * 0.5f, side, side};
        const int idx = start + k;
        if (idx >= static_cast<int>(bookSpells_.size())) continue;  // disabled: only the background (baked)
        const uint32_t id = bookSpells_[static_cast<size_t>(idx)];
        const pp::SpellInfo* s = spellDb_.get(id);
        // The button and its text are one click target (the CheckButton + its FontStrings' hit area).
        const Rect hit{icon.x, icon.y, PX(ux + 37 + 4 + 103) - icon.x, icon.h};
        const bool f = focusable && winFocus_.addStop(kIdBook0 + static_cast<uint32_t>(k), hit);
        if (const pp::Texture* t = spellDb_.icon(id, spellFrame_)) ui.image(*t, icon);
        else ui.fill(icon, 0xFF303030u);
        drawCooldown(ui, icon, session_.spellCooldown(id), session_.spellCooldownTotal(id));
        if (spellArt_.slot.pixels) {  // NormalTexture UI-Quickslot2 64 x 64 around the 37-unit button
            const float c = side * 64.0f / 37.0f;
            ui.image(spellArt_.slot, Rect{icon.x + side * 0.5f - c * 0.5f, icon.y + side * 0.5f - c * 0.5f, c, c});
        }
        if (session_.isCasting() && session_.currentCastSpellId() == id && sbArt_.checked.pixels)
            ui.image(sbArt_.checked, icon, 0xFFFFFFFFu, Blend::Add);  // IsSelectedSpell: checked
        if (f && spellArt_.hilite.pixels) ui.image(spellArt_.hilite, icon, 0xFFFFFFFFu, Blend::Add);
        if (f) {  // SpellButton_OnEnter - GameTooltip:SetSpell, ANCHOR_RIGHT (drawSpellTooltips)
            tipSpellId_ = id;
            tipSpellRect_ = Rect{hit.x + ui.offsetX(), hit.y + ui.offsetY(), hit.w, hit.h};
        }
        if (bookPick_ == id && !bookPickItem_) ui.fill(icon, 0x60000000u);  // on the cursor (a spell)
        // $parentSpellName (LEFT of the button's RIGHT +4, 103 wide; 4 up with a rank) and $parentSubSpellName.
        const bool rank = s && !s->rank.empty();
        const float tx = icon.x + icon.w + 4.0f * kSX + 1.0f, tw = PX(ux + 37 + 4 + 103) - tx;
        // maxLines 3: the name wraps (PSP: in the small font while two lines and no rank fit the 28 px row,
        // else in the 8 px one), the rank under it.
        const std::string name = s ? s->name : std::string("?");
        const Font* nf = &fs;
        std::vector<std::string> lines = wrapQuestText(fs, name, tw);
        if (lines.size() > (rank ? 1u : 2u)) {
            nf = &ft;
            lines = wrapQuestText(ft, name, tw);
            if (lines.size() > (rank ? 2u : 3u)) lines.resize(rank ? 2u : 3u);
        }
        const float lhN = static_cast<float>(nf->lineHeight()) - 1.0f;
        const float blockH = lhN * static_cast<float>(lines.size()) + (rank ? lhT - 1.0f : 0.0f);
        float ny = icon.y + (icon.h - blockH) * 0.5f;
        ui.pushClip(Rect{tx, icon.y - 6.0f, tw, icon.h + 12.0f});
        for (const std::string& l : lines) {
            ui.textShadow(*nf, tx, ny, l.c_str(), kColGold);
            ny += lhN;
        }
        if (rank) ui.text(ft, tx, ny, s->rank.c_str(), kColSubSpell);
        ui.popClip();
        // PSP: no GameTooltip:SetSpell with only the name and the rank. The whole
        // GameTooltip (cost, range, cast, description) - wowee.cfg spelltooltip=0 turns it off.
    }

    // SpellBookPrevPageButton (CENTER 50,105 from BOTTOMLEFT) PREV, NextPageButton (314,105) NEXT,
    // SpellBookPageText (BOTTOM -14,96, GameFontNormal).
    const int pages = std::max(1, (static_cast<int>(bookSpells_.size()) + kSpellsPerPage - 1) / kSpellsPerPage);
    {
        const float bs = 32.0f * kSY, cy = PY(512 - 105);
        const Rect pr{PX(50) - bs * 0.5f, cy - bs * 0.5f, bs, bs}, nr{PX(314) - bs * 0.5f, cy - bs * 0.5f, bs, bs};
        const bool canPrev = bookPage_ > 0, canNext = bookPage_ + 1 < pages;
        auto arrow = [&](uint32_t id, const Rect& r, bool on, const pp::Texture& up, const pp::Texture& dis) {
            const bool f = focusable && on && winFocus_.addStop(id, r);
            const pp::Texture& t = on || !dis.pixels ? up : dis;
            if (t.pixels) ui.image(t, r, on ? 0xFFFFFFFFu : 0xFF808080u);
            else ui.fill(r, 0xFF202020u);
            if (f && spellArt_.hilite.pixels) ui.image(spellArt_.hilite, r, 0xFFFFFFFFu, Blend::Add);
        };
        arrow(kIdBookPrev, pr, canPrev, sbArt_.prev, sbArt_.prevDis);
        arrow(kIdBookNext, nr, canNext, sbArt_.next, sbArt_.nextDis);
        const float ty = cy - lhS * 0.5f;
        ui.textShadow(fs, pr.x + pr.w + 1, ty, CLIENT_STR(PREV), canPrev ? kColGold : kColGrey);
        ui.textShadow(fs, nr.x - 1, ty, CLIENT_STR(NEXT), canNext ? kColGold : kColGrey, Align::Right);
        const std::string pg = CLIENT_FMT(PAGE_NUMBER, bookPage_ + 1);
        ui.textShadow(fn, PX(178), PY(512 - 96) - static_cast<float>(fn.lineHeight()), pg.c_str(), kColGold, Align::Center);
    }

    // SpellBookSkillLineTab1..8 (TOPLEFT at TOPRIGHT -32,-65, 32 x 32, 17 apart; the tab art 64 x 64 at -3,11).
    const auto& tabs = session_.getSpellBookTabs();
    const int n = std::min<int>(static_cast<int>(tabs.size()), kMaxTabs);
    for (int i = 0; i < n; ++i) {
        const float uy = 65.0f + static_cast<float>(i) * 49.0f;
        const float ts = 32.0f * kSY;
        const Rect tr{PX(352), PY(uy), ts, ts};
        if (sbArt_.tab.pixels) ui.image(sbArt_.tab, Rect{tr.x - 3.0f * kSY, tr.y - 11.0f * kSY, 64.0f * kSY, 64.0f * kSY});
        const bool f = focusable && winFocus_.addStop(kIdBookTab0 + static_cast<uint32_t>(i), tr);
        if (const pp::Texture* t = spellDb_.iconById(tabs[static_cast<size_t>(i)].iconId, spellFrame_)) ui.image(*t, tr);
        else ui.fill(tr, 0xFF404040u);
        if (i == bookTab_ && sbArt_.checked.pixels) ui.image(sbArt_.checked, tr, 0xFFFFFFFFu, Blend::Add);
        if (f) {
            if (spellArt_.hilite.pixels) ui.image(spellArt_.hilite, tr, 0xFFFFFFFFu, Blend::Add);
            tipBookTab_ = i + 1;  // GameTooltip:SetText(self.tooltip) - the line's name, one line
            tipSpellRect_ = Rect{tr.x + tr.w + 2.0f + ui.offsetX(), tr.y + ui.offsetY(), tr.w, tr.h};  // ANCHOR_RIGHT
        }
    }
    return true;
}

// Square on a book spell / an action button (inventoryWindowPad's twin).
void WorldScreen::spellWindowPad(const pp::PadState& pad) {
    if (!(pad.pressed & PSP_CTRL_SQUARE) || !uiMode_ || settings_.isOpen()) return;
    const uint32_t id = winFocus_.focused();
    if (id >= kIdBook0 && id < kIdBook0 + kSpellsPerPage && windowOpen(kWinSpellbook)) {
        const int idx = bookPage_ * kSpellsPerPage + static_cast<int>(id - kIdBook0);
        if (idx < static_cast<int>(bookSpells_.size())) {
            if (settings_.values().barassignment) beginSpellAssignment(bookSpells_[static_cast<size_t>(idx)], id);
            else { bookPick_=bookSpells_[static_cast<size_t>(idx)]; bookPickItem_=false; }
        }
    } else if (id >= kIdSlot0 && id < kIdSlot0 + 12) {
        // an item held with Square goes on the button (PickupContainerItem + PlaceAction)
        if (!placeHeldItemOnAction(mainBarBase() + static_cast<int>(id - kIdSlot0)))
            spellSlotPickPlace(mainBarBase() + static_cast<int>(id - kIdSlot0), true);
    }
}

// The spell on the cursor and the book's deferred tooltips, after all the windows (drawWindows).
void WorldScreen::drawSpellCursor(UiRenderer& ui) {
    if (tipBookTab_ > 0) {
        // Opposite the tab, at its right, centred on it (UiKit::tooltip: 22 px high with one
        // line, 44 px above its anchor's y).
        const auto& tabs = session_.getSpellBookTabs();
        if (tipBookTab_ <= static_cast<int>(tabs.size())) {
            Rect a = tipSpellRect_;
            a.y = tipSpellRect_.y + (tipSpellRect_.h - 22.0f) * 0.5f + 44.0f;
            kit_.tooltip(ui, a, tabs[static_cast<size_t>(tipBookTab_ - 1)].name.c_str(), "");
        }
    }
    if (!barAssign_.active() && bookPick_ != 0 && uiMode_ && !settings_.isOpen()) {
        // an item button on the cursor shows the item's icon
        if (const pp::Texture* t = bookPickItem_ ? itemActionIcon(bookPick_) : spellDb_.icon(bookPick_, spellFrame_))
            ui.image(*t, Rect{winFocus_.cursorX() + 6.0f, winFocus_.cursorY() + 6.0f, 18, 18}, 0xD0FFFFFFu);
    }
}

void WorldScreen::beginSpellAssignment(uint32_t spell, uint32_t origin) {
    if (!spell) return;
    if (bookPick_) { uiInfo(OWN_STR(ASSIGN_CLEAR_CURSOR)); return; }
    if (classBarHidesMainBar()) { uiInfo(OWN_STR(ASSIGN_BAR_UNAVAILABLE)); return; }
    const auto& bar = session_.actionBar();
    const int base = mainBarBase();
    if (base < 0 || base + 12 > static_cast<int>(bar.size())) return;
    int selected = -1;
    for (int i=0;i<12;++i)
        if (bar[base+i].type == game::ActionBarSlot::SPELL && bar[base+i].id == spell) { selected=i; break; }
    if (selected < 0) for (int i=0;i<12;++i) if (bar[base+i].isEmpty()) { selected=i; break; }
    if (selected < 0) selected=0;
    barAssign_.begin(spell, base, selected, origin);
    barAssignFocus_.setFocus(kIdAssign0+static_cast<uint32_t>(selected));
}

void WorldScreen::cancelSpellAssignment() {
    const uint32_t origin = barAssign_.returnFocus;
    barAssign_.cancel();
    if (windowOpen(kWinSpellbook)) { winFocus_.focusGroup(kWinSpellbook); winFocus_.setFocus(origin); }
}

void WorldScreen::updateSpellAssignment(const pp::PadState& pad, float dtSec) {
    if (!barAssign_.active()) return;
    if ((pad.pressed & (PSP_CTRL_CIRCLE | PSP_CTRL_START)) ||
        mainBarBase()!=barAssign_.base || classBarHidesMainBar() || !windowOpen(kWinSpellbook)) {
        cancelSpellAssignment(); return;
    }
    if (pad.pressed & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER)) {
        actionBarPage_ = (actionBarPage_ + ((pad.pressed & PSP_CTRL_RTRIGGER) ? 1 : 5)) % 6;
        barAssign_.base = mainBarBase();
        barAssign_.confirming = false;
    }
    barAssignFocus_.update(pad,dtSec);
    const uint32_t focus=barAssignFocus_.focused();
    if (focus>=kIdAssign0 && focus<kIdAssign0+12) barAssign_.select(static_cast<int>(focus-kIdAssign0));
    const uint32_t act=barAssignFocus_.activated();
    if (act<kIdAssign0 || act>=kIdAssign0+12) return;
    const auto& bar=session_.actionBar();
    const int slot=barAssign_.base+barAssign_.slot;
    if (slot<0 || slot>=static_cast<int>(bar.size())) { cancelSpellAssignment(); return; }
    const auto& old=bar[slot];
    const bool same=old.type==game::ActionBarSlot::SPELL && old.id==barAssign_.spell;
    if (barAssign_.activate(!old.isEmpty() && !same)==ActionAssignment::ConfirmReplacement) return;
    if (!same) session_.setActionBarSlot(slot,game::ActionBarSlot::SPELL,barAssign_.spell);
    slotFlash_[barAssign_.slot]=.5f;
    cancelSpellAssignment();
}

void WorldScreen::drawSpellAssignment(UiRenderer& ui) {
    if (!barAssign_.active()) return;
    ui.fill(Rect{0,0,480,272},0xA0000000u);
    loadPopupArt();
    // Use the same client metal border and overlapping title tab as Settings.
    auto panel=[&](const Rect& r,uint32_t background) {
        ui.fill(Rect{r.x+4,r.y+4,r.w-8,r.h-8},background);
        if(popupArt_.border.pixels)ui.nineSlice(popupArt_.border,r,.45f);
        else if(kit_.has(kTexDlgBorder))ui.nineSlice(kit_.tex(kTexDlgBorder),r,.7f);
    };
    panel(Rect{16,38,448,206},0xB0000000u);
    const auto& normal=kit_.font(kFontNormal);
    const auto& font=kit_.font(kFontSmall);
    const char* caption=OWN_STR(ASSIGN_HEADER);
    const float headerWidth=std::clamp(static_cast<float>(normal.measure(caption))+28.f,116.f,300.f);
    const Rect header{240.f-headerWidth*.5f,26,headerWidth,32};
    if(popupArt_.header.pixels)ui.image(popupArt_.header,header);
    else panel(header,0xFF171717u);
    ui.textShadow(normal,240,header.y+(header.h-normal.lineHeight())*.5f,caption,kColGold,Align::Center);
    ui.pushClip(Rect{26,59,428,14});
    ui.textShadow(font,240,61,OWN_STR(ASSIGN_TITLE),kColGold,Align::Center);
    ui.popClip();
    panel(Rect{26,73,428,35},0x80000000u);
    panel(Rect{24,110,432,74},0x40000000u);
    panel(Rect{26,185,428,19},0x80000000u);
    const auto* pending=spellDb_.get(barAssign_.spell);
    if(const auto* icon=spellDb_.icon(barAssign_.spell,spellFrame_))ui.image(*icon,Rect{34,76,30,30});
    ui.pushClip(Rect{74,76,310,30});
    ui.textShadow(font,74,85,pending ? pending->name.c_str() : "?",kColWhite);
    ui.popClip();
    char pageLabel[16];std::snprintf(pageLabel,sizeof(pageLabel),"%d/6",actionBarPage_+1);
    ui.textShadow(font,438,85,pageLabel,kColGold,Align::Right);
    const auto& bar=session_.actionBar();
    const char* rowNames[]={OWN_STR(ASSIGN_ROW_MAIN),OWN_STR(ASSIGN_ROW_L),OWN_STR(ASSIGN_ROW_R)};
    constexpr PadGlyph keys[]={kPadSquare,kPadTriangle,kPadCircle,kPadCross};
    for(int group=0;group<3;++group)kit_.controlHint(ui,font,94.f+group*144.f,112,rowNames[group],kColGold,Align::Center);
    for(int i=0;i<12;++i) {
        const int slot=barAssign_.base+i;
        const Rect button{26.f+i*36.f,128,32,32};
        const bool focused=barAssignFocus_.addStop(kIdAssign0+i,button);
        const float frameSide=26.f*66.f/36.f;
        const Rect slotFrame{button.x+(button.w-frameSide)*.5f,button.y+(button.h-frameSide)*.5f,frameSide,frameSide};
        bool occupied=false;
        if(slot>=0 && slot<static_cast<int>(bar.size())) {
            const auto& action=bar[slot];
            occupied=!action.isEmpty();
            const pp::Texture* icon=action.type==game::ActionBarSlot::SPELL ? spellDb_.icon(action.id,spellFrame_) :
                action.type==game::ActionBarSlot::ITEM ? itemActionIcon(action.id) : nullptr;
            if(icon)ui.image(*icon,Rect{button.x+3,button.y+3,26,26});
        }
        if(occupied && spellArt_.slot.pixels)ui.image(spellArt_.slot,slotFrame);
        else if(!occupied && spellArt_.empty.pixels)ui.image(spellArt_.empty,slotFrame,0xC0FFFFFFu);
        else if(!occupied)ui.fill(button,0x80000000u);
        if(focused && spellArt_.hilite.pixels)ui.image(spellArt_.hilite,button,0x80FFFFFFu,Blend::Add);
        char number[8];std::snprintf(number,sizeof(number),"%d",i+1);
        ui.textShadow(font,button.x+2,button.y+1,number,kColWhite);
        kit_.glyph(ui,keys[i%4],Rect{button.x+9,166,14,14});
        if(focused) {
            ui.fill(Rect{button.x,161,button.w,2},kColGold);
            kit_.glyph(ui,kPadCross,Rect{button.x+24,button.y+24,10,10});
        }
    }
    const int destination=barAssign_.base+barAssign_.slot;
    std::string existing=OWN_STR(ASSIGN_EMPTY);
    if (destination>=0 && destination<static_cast<int>(bar.size()) && !bar[destination].isEmpty()) {
        const auto& action=bar[destination];
        const auto* info=action.type==game::ActionBarSlot::SPELL ? spellDb_.get(action.id) : nullptr;
        existing=info ? info->name : OWN_STR(ASSIGN_EXISTING);
    }
    const std::string destinationText=OWN_FMT(ASSIGN_DESTINATION,barAssign_.slot+1,existing.c_str());
    ui.pushClip(Rect{28,186,424,16});
    ui.textShadow(font,240,188,destinationText.c_str(),kColGold,Align::Center);
    ui.popClip();
    kit_.controlHint(ui,font,240,209,barAssign_.confirming ? OWN_STR(ASSIGN_REPLACE) : OWN_STR(ASSIGN_HINT),kColWhite,Align::Center);
    kit_.controlHint(ui,font,240,228,OWN_STR(ASSIGN_PAGES),kColGold,Align::Center);
    kit_.cursor(ui,barAssignFocus_);
}

}  // namespace wowee::ui::psp
