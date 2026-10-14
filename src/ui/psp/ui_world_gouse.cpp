// The game objects one can use, on the world screen.
//
// The PSP has no mouse: the "hover" is the nearest usable object (goClickable) within its reach (+3 yd: the tooltip
// shows while walking up) in front of the character, the own fishing bobber first. Then:
//   - its model is drawn once more additively (ModelRenderer::drawHighlight);
//   - its GameTooltip at the default anchor (GameTooltip_SetDefaultAnchor: bottom right over the bars): the name
//     (gold), LOCKED (red when nothing opens it, green when a key or a skill-less spell does, else the skill's
//     colour), the lock line (LOCKED_WITH_ITEM / _SPELL / _SPELL_KNOWN for the case the session reports), the quests
//     it serves (the title, " - " + the objective); the line order is kept pending observation;
//   - the up glyph over it while it is in reach (as over a corpse with loot): up uses it (the session's
//     useGameObject), unless a talking NPC target is nearer (ui_world_npc.cpp interactTarget); L + up picks the next
//     object in reach.
// Also here: the session's object refusals on the red line (with their |3-N declension), the stand state a chair /
// the barber chair sets (SMSG_STANDSTATE_UPDATE -> the sit-in-chair loops, ActivityFSM), the fishing bobber's
// bite (its highlight pulses, the display's Custom0 sound from ui.pak "go_snd"), the sparkle of a quest object
// (GO_DYNFLAG_LO_SPARKLE), ItemTextFrame (window 0x12) over the session's ItemTextState.
#include "ui/psp/ui_world_screen.hpp"

#include "game/update_field_table.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_strings.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_texture.hpp"
#include "rendering/psp/psp_gameobject_format.hpp"
#include "ui/psp/ui_world_sounds.hpp"

#include <pspctrl.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
namespace rp = wowee::rendering::psp;
using GoInfo = pp::WorldSession::GameObjectInfo;

namespace {

// Widgets (0x8100..0x81FF).
enum : uint32_t {
    kIdTextClose = 0x8100, kIdTextPrev, kIdTextNext, kIdTextBody,
    // BarberShopFrame: BarberShopFrameSelector1..3 Prev / Next, Okay / Cancel / Reset.
    kIdBarberPrev1 = 0x8110, kIdBarberNext1, kIdBarberPrev2, kIdBarberNext2, kIdBarberPrev3, kIdBarberNext3,
    kIdBarberOkay = 0x8118, kIdBarberCancel, kIdBarberReset,
};

// BarberShopFrame (265 x 319 units at RIGHT -18, -54 of UIParent) and BarberShopBannerFrame (381 x 210 at TOP 2, 5)
// at 0.6 x 0.55 (tools/psp/build_ui_pak.py BARBER_*).
constexpr float kBarberW = 265.0f * 0.6f, kBarberH = 319.0f * 0.55f;
constexpr float kBarberX = 480.0f - 18.0f * 0.6f - kBarberW, kBarberY = 136.0f + 54.0f * 0.55f - kBarberH * 0.5f;
float BX(float u) { return kBarberX + u * 0.6f; }
float BY(float u) { return kBarberY + u * 0.55f; }

constexpr float kHoverExtra = 3.0f;      // yd past the reach: the hover (tooltip, highlight) while walking up
constexpr float kHoverCone = 1.31f;      // rad (75 deg) either side of the facing
constexpr uint32_t kScanMs = 100;
constexpr uint32_t kHighlightAdd = 0x50505050u;  // src x 0.31 + dst: the mouseover brightening
constexpr uint32_t kBiteShowMs = 1500;

// GameTooltip colours (ABGR): FrameXML constants.lua NORMAL_FONT_COLOR / HIGHLIGHT / RED_FONT_COLOR_CODE and the
// trade skill difficulty colours of the trade skill add-on (TradeSkillTypeColor); the layout of ui_world_tooltip.cpp.
constexpr uint32_t kTipGold = 0xFF00D2FFu;      // ffd200: the object's name, a quest's title
constexpr uint32_t kTipWhite = 0xFFFFFFFFu;     // ffffff
constexpr uint32_t kTipRed = 0xFF2020FFu;       // ff2020 (impossible)
constexpr uint32_t kTipGreen = 0xFF40C040u;     // 40c040 (easy)
constexpr uint32_t kTipYellow = 0xFF00FFFFu;    // ffff00 (medium)
constexpr uint32_t kTipOrange = 0xFF4080FFu;    // ff8040 (optimal)
constexpr uint32_t kTipGrey = 0xFF808080u;      // 808080 (trivial)
constexpr uint32_t kTipBackground = 0xBB1B0D0Du;
constexpr float kTipPad = 6.0f, kTipGap = 1.0f, kTipWrap = 200.0f;

// The colour of a skill against a requirement: the bands AzerothCore uses for a gathering skill's gain chance
// (PlayerUpdates.cpp UpdateGatherSkill -> SkillGainChance(skill, req + 100, req + 50, req + 25)), red below it.
uint32_t skillColor(int32_t skill, int32_t required) {
    if (skill < required) return kTipRed;
    if (skill < required + 25) return kTipOrange;
    if (skill < required + 50) return kTipYellow;
    if (skill < required + 100) return kTipGreen;
    return kTipGrey;
}

// ItemTextFrame (FrameXML units -> px: 0.6 x 0.55 as the mail frame; the panels' place at the left).
constexpr float kSX = 0.6f, kSY = 0.55f, kOx = 4.0f, kOy = 2.0f;
constexpr float kTextFrameW = 354.0f, kTextFrameH = 442.0f;  // 384 x 512 minus HitRectInsets right 30, bottom 70
float TX(float u) { return kOx + u * kSX; }
float TY(float u) { return kOy + u * kSY; }

// MATERIAL_TEXT_COLOR_TABLE (Constants.lua) by PageTextMaterial.dbc id; ItemTextFrame draws no art for Parchment.
const char* materialName(uint8_t id) {
    switch (id) {
        case 2: return "stone";
        case 3: return "marble";
        case 4: return "silver";
        case 5: return "bronze";
        case 6: return "valentine";
        case 7: return "illidan";
        default: return nullptr;  // 1 Parchment (0: none -> Parchment)
    }
}
uint32_t materialTextColor(uint8_t id) {
    switch (id) {
        case 2: return 0xFFFFFFFFu;  // Stone 1, 1, 1
        case 3: return 0xFF000000u;  // Marble 0, 0, 0
        case 4: return 0xFF1F1F1Fu;  // Silver .12
        default: return 0xFF0F1F2Eu;  // Default / Parchment / Bronze .18, .12, .06
    }
}

void loadTex(pp::Pak& pak, const char* name, pp::Texture& t) {
    if (t.pixels) return;
    const pp::PakEntry* e = pak.find(name);
    void* blob = e ? pak.load(e) : nullptr;
    if (blob && !pp::textureFromBlob(blob, e->size, t, true)) pp::pakFree(blob);
}

// SimpleHTML of a page (<HTML><BODY><H1>..</H1><P>..</P></BODY></HTML>): the text, a paragraph per line.
std::string pageText(const std::string& raw) {
    if (raw.size() < 6 || raw.compare(0, 6, "<HTML>") != 0) return raw;
    std::string out;
    for (size_t i = 0; i < raw.size();) {
        if (raw[i] != '<') {
            out += raw[i++];
            continue;
        }
        const size_t end = raw.find('>', i);
        if (end == std::string::npos) break;
        std::string tag = raw.substr(i + 1, end - i - 1);
        for (char& c : tag) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (tag == "/P" || tag == "BR/" || tag == "BR" || tag == "/H1" || tag == "/H2" || tag == "/H3") out += '\n';
        i = end + 1;
    }
    return out;
}

}  // namespace

struct WorldScreen::GoUseUi {
    // The hover.
    uint64_t hover = 0, chosen = 0;
    bool hoverInReach = false;
    glm::vec3 hoverTop{0.0f};
    uint32_t scanMs = 0;
    std::vector<std::pair<float, uint64_t>> cands;  // (score, guid), in reach first
    // The GameTooltip.
    struct TipLine {
        std::string text;
        uint32_t color = kTipWhite;
        bool header = false;
    };
    std::vector<TipLine> tip;
    uint64_t tipKey = ~0ull;
    uint32_t tipBuiltMs = 0;
    float tipW = 0.0f, tipH = 0.0f;
    pp::Texture border;
    bool borderTried = false;
    // ItemTextFrame.
    pp::Texture frame, mat, scrollArt, prev, next;
    uint8_t matLoaded = 0;
    bool frameTried = false, textShown = false, raisedCursor = false;
    uint32_t textVersion = ~0u;
    std::vector<std::string> lines;
    int scroll = 0;
    // GameObjectDisplayInfo sounds (ui.pak "go_snd").
    std::vector<rp::GoSoundRec> sounds;
    bool soundsTried = false;
    uint32_t biteSeen = 0;
    // The stand state the server set.
    uint8_t chairState = 0;
    // The animated objects (creatures.pak "goanim", sorted display ids) and their animation state.
    std::vector<uint32_t> animDisplays;
    bool animTried = false;
    struct AnimGo {
        uint32_t frame = 0, seq = 0;
        uint8_t animState = 0;
        uint32_t loopAnim = 0, pendingShot = 0, pendingTries = 0;
    };
    std::unordered_map<uint64_t, AnimGo> animGos;
    uint32_t animFrame = 0, baseMaxDrawn = 0, lastMaxDrawn = 0;
    // The own look the model was loaded with (PLAYER_BYTES, PLAYER_BYTES_2 byte 0): a change (a haircut) reloads it.
    uint32_t lookBytes = 0, lookFacial = 0;
    bool lookKnown = false;
    // BarberShopFrame.
    struct Barber {
        bool shown = false, raised = false;
        std::vector<uint8_t> blob;  // ui.pak "go_barber" while the frame is open
        const rp::GoBarberHeader* h = nullptr;
        std::vector<const rp::GoBarberStyle*> hair, facial;  // the player's race / sex, by Data
        const float* costs = nullptr;
        const char* pool = nullptr;
        std::string label[3];       // HAIR_<x>_STYLE, HAIR_<x>_COLOR, FACIAL_HAIR_<x>
        int colors = 0;
        int cur[3] = {}, sel[3] = {};  // hair (index into hair), colour, facial (index into facial)
        std::string banner;         // BarberShopBannerFrameCaption
        pp::Texture frame, bannerTex, money, prev, next;
        bool artTried = false;
        void release() {
            for (pp::Texture* t : {&frame, &bannerTex, &money, &prev, &next})
                if (t->pixels) pp::textureRelease(*t);
            artTried = false;
            blob.clear();
            blob.shrink_to_fit();
            h = nullptr;
            hair.clear();
            facial.clear();
            costs = nullptr;
            pool = nullptr;
        }
    } barber;
    uint32_t logs = 0, tipLogs = 0;
    void releaseText() {
        for (pp::Texture* t : {&frame, &mat, &scrollArt, &prev, &next})
            if (t->pixels) pp::textureRelease(*t);
        frameTried = false;
        matLoaded = 0;
        lines.clear();
        textVersion = ~0u;
    }
    ~GoUseUi() {
        releaseText();
        barber.release();
        if (border.pixels) pp::textureRelease(border);
    }
};

void WorldScreen::destroyGoUseUi(GoUseUi* p) { delete p; }

// The realm's stand state (the item-use code's handler) - the chair's loop (SitChairLow / Med / High).
void WorldScreen::goNoteStandState(uint8_t st) {
    goUseUi().chairState = st;
    PSP_LOGI(pp::tag::Gfx, "stand_state_set state=%u", static_cast<unsigned>(st));
}

WorldScreen::GoUseUi& WorldScreen::goUseUi() {
    if (!gou_) gou_ = std::unique_ptr<GoUseUi, void (*)(GoUseUi*)>(new GoUseUi(), &destroyGoUseUi);
    return *gou_;
}

// The model's bounding sphere in the world (gameobjects.pak "gidx", as updateGameObjects places it); a display the
// pak lacks (a WMO, an unknown one): a 1.5 yd sphere 1 yd over the object's origin.
bool WorldScreen::goSphere(const GoInfo& g, glm::vec3& center, float& radius) const {
    auto rec = std::lower_bound(goIndex_.begin(), goIndex_.end(), g.displayId,
                                [](const rp::GameObjectDisplayRec& a, uint32_t id) { return a.displayId < id; });
    const float k = g.scale;
    if (rec == goIndex_.end() || rec->displayId != g.displayId) {
        center = g.pos + glm::vec3(0.0f, 0.0f, 1.0f * k);
        radius = 1.5f * k;
        return false;
    }
    const float c = std::cos(g.orientation), s = std::sin(g.orientation);
    const float cx = rec->centerCm[0] * 0.01f, cy = rec->centerCm[1] * 0.01f, cz = rec->centerCm[2] * 0.01f;
    center = g.pos + glm::vec3((c * cx - s * cy) * k, (s * cx + c * cy) * k, cz * k);
    radius = std::max(0.25f, rec->radiusCm * 0.01f * k);
    return true;
}

void WorldScreen::updateGameObjectUse(float dtSec) {
    GoUseUi& s = goUseUi();
    const glm::vec3 me(playerX_, playerY_, playerZ_);
    session_.gameObjectUseUpdate(dtSec, me);
    loadGoAnimList();  // once creatures.pak is open: the animated objects' list
    // The refusals (ERR_USE_*...) on the red line, their |3-N(word) declined.
    {
        std::string e;
        while (session_.takeGoError(e)) uiError(questText(e));
    }
    // The stand state the server set (a chair: SIT_LOW/MEDIUM/HIGH_CHAIR; 0 stands up): taken by
    // the item-use code's applyServerStandState, which notes the chair here (goNoteStandState) for the sit-in-chair loops.
    // The animations of objects (custom / despawn): the fishing bobber's bite plays its display's Custom0 sound.
    {
        pp::WorldSession::GoAnimEvent ev;
        while (session_.takeGoAnimEvent(ev)) {
            if (ev.kind != 1) continue;
            const GoInfo* g = session_.findGameObject(ev.guid);
            if (!g) continue;
            if (goAnimatedDisplay(g->displayId)) {  // Custom0..3 / Despawn once (after the next draw)
                auto it = s.animGos.find(ev.guid);
                if (it != s.animGos.end()) {
                    it->second.pendingShot = ev.kind == 2 ? 157u : (ev.kind == 1 ? 153u + std::min<uint32_t>(ev.arg, 3u) : 0u);
                    it->second.pendingTries = 0;
                }
            }
            if (!s.soundsTried) {
                s.soundsTried = true;
                char path[256];
                pp::fsPath(path, sizeof(path), "data/ui.pak");
                pp::Pak pak;
                const pp::PakEntry* e = pak.open(path) ? pak.find("go_snd") : nullptr;
                if (void* blob = e ? pak.load(e) : nullptr) {
                    const auto* h = static_cast<const rp::GoSoundHeader*>(blob);
                    if (e->size >= sizeof(*h) && std::memcmp(h->magic, "WGSN", 4) == 0 &&
                        sizeof(*h) + h->count * sizeof(rp::GoSoundRec) <= e->size) {
                        const auto* r = reinterpret_cast<const rp::GoSoundRec*>(h + 1);
                        s.sounds.assign(r, r + h->count);
                    }
                    pp::pakFree(blob);
                }
                PSP_LOGI(pp::tag::Gfx, "go_snd rows=%u kb=%u", static_cast<unsigned>(s.sounds.size()),
                         static_cast<unsigned>(s.sounds.size() * sizeof(rp::GoSoundRec) / 1024u));
            }
            auto rec = std::lower_bound(s.sounds.begin(), s.sounds.end(), g->displayId,
                                        [](const rp::GoSoundRec& r, uint32_t id) { return r.display < id; });
            const uint32_t slot = 6u + std::min<uint32_t>(ev.arg, 3u);  // Sound[6..9] = Custom0..3
            bool played = false;
            if (rec != s.sounds.end() && rec->display == g->displayId && rec->sound[slot] != 0)
                played = playSound3D(rec->sound[slot], g->pos);
            if (!played && ev.guid == session_.fishingBobber()) playUiSound(UiSound::FishingBite);  // upstream's
        }
    }
    // ItemTextFrame follows the session (ITEM_TEXT_READY shows it, ITEM_TEXT_CLOSED hides it, its OnHide closes it).
    {
        const auto& it = session_.itemText();
        const bool open = windowOpen(kWinItemText);
        if (it.open && it.ready && !open && !s.textShown) {
            s.textShown = true;
            openWindow(kWinItemText);
            playSoundByName("igMainMenuOpen");  // OnShow
            if (!uiMode_) {
                setUiMode(true);
                s.raisedCursor = true;
            }
            winFocus_.setFocus(kIdTextBody);
        } else if (s.textShown && (!it.open || !open)) {
            s.textShown = false;
            if (open) closeWindow(kWinItemText);
            if (it.open) session_.closeItemText();  // OnHide: CloseItemText()
            playSoundByName("igMainMenuClose");
            if (s.raisedCursor && uiMode_ && windows_.empty()) setUiMode(false);
            s.raisedCursor = false;
            s.releaseText();
            PSP_LOGI(pp::tag::Gfx, "itemtext_art released");
        }
    }
    barberSync();  // BarberShopFrame follows the session (BARBER_SHOP_OPEN / _CLOSE / _SUCCESS)
    // The own look from our fields: a change (a haircut) reloads the model - not while the barber shop previews.
    if (session_.selfKnown() && !s.barber.shown) {
        const uint32_t bytes = session_.ownField(game::fieldIndex(game::UF::PLAYER_BYTES));
        const uint32_t facial = session_.ownField(game::fieldIndex(game::UF::PLAYER_BYTES_2)) & 0xFFu;
        if (!s.lookKnown) {
            s.lookKnown = true;
            s.lookBytes = bytes;
            s.lookFacial = facial;
        } else if (bytes != s.lookBytes || facial != s.lookFacial) {
            PSP_LOGI(pp::tag::Gfx, "own_look_changed bytes=%08X facial=%u", static_cast<unsigned>(bytes),
                     static_cast<unsigned>(facial));
            loadCharacter();  // (ownLookLoaded records the new look)
        }
    }
    // The hover: the nearest usable object in reach (+ kHoverExtra) in front, the own bobber first.
    const uint32_t now = pp::logUptimeMs();
    if (now - s.scanMs < kScanMs) return;
    s.scanMs = now;
    PROF_SCOPE("gouse.scan");
    s.cands.clear();
    const uint64_t bobber = session_.fishingBobber();
    const bool alive = session_.life() == pp::WorldSession::Life::Alive;
    const float fx = std::cos(facing_), fy = std::sin(facing_);
    for (const GoInfo& g : session_.gameObjects()) {
        if (!alive || !session_.goClickable(g)) continue;
        const auto* t = session_.goTemplate(g.entry);
        if (!t || !t->known) continue;  // an object is known by its cache entry
        glm::vec3 c;
        float r = 0.0f;
        goSphere(g, c, r);
        const float reach = pp::WorldSession::goUseDistance(g.type);
        // Every type, chairs too, measured to the object (AzerothCore GameObject::IsAtInteractDistance).
        const float d = std::max(0.0f, glm::length(c - me) - r);
        const bool inReach = d <= reach;
        if (g.guid == bobber) {
            s.cands.emplace_back(-1.0f, g.guid);  // the own bobber first (the 100 yd reach)
            continue;
        }
        if (d > reach + kHoverExtra) continue;
        const glm::vec2 to(c.x - me.x, c.y - me.y);
        const float len = glm::length(to);
        if (len > 1.0f && d > 0.5f) {  // in front (a hand's reach: any side)
            const float cosA = (to.x * fx + to.y * fy) / len;
            if (cosA < std::cos(kHoverCone)) continue;
        }
        s.cands.emplace_back(inReach ? d : 100.0f + d, g.guid);
    }
    std::sort(s.cands.begin(), s.cands.end());
    uint64_t hover = s.cands.empty() ? 0 : s.cands.front().second;
    if (s.chosen != 0) {
        const bool still = std::any_of(s.cands.begin(), s.cands.end(), [&](const auto& c) { return c.second == s.chosen; });
        if (still) hover = s.chosen;
        else s.chosen = 0;
    }
    if (hover != s.hover) {
        s.hover = hover;
        s.tipKey = ~0ull;
        if (hover != 0 && ++s.logs <= 40) {
            const GoInfo* g = session_.findGameObject(hover);
            const auto* t = g ? session_.goTemplate(g->entry) : nullptr;
            PSP_LOGI(pp::tag::Gfx, "go_hover guid=%08X entry=%u type=%u name=%s cands=%u",
                     static_cast<unsigned>(hover & 0xFFFFFFFFu), g ? static_cast<unsigned>(g->entry) : 0u,
                     g ? static_cast<unsigned>(g->type) : 0u, t ? t->name.c_str() : "?",
                     static_cast<unsigned>(s.cands.size()));
        }
    }
    s.hoverInReach = false;
    if (const GoInfo* g = hover ? session_.findGameObject(hover) : nullptr) {
        glm::vec3 c;
        float r = 0.0f;
        goSphere(*g, c, r);
        s.hoverTop = c + glm::vec3(0.0f, 0.0f, std::min(r, 2.5f) + 0.3f);
        for (const auto& cd : s.cands)
            if (cd.second == hover) s.hoverInReach = cd.first < 100.0f;
    }
}

// L + ↑: the next object in reach becomes the hover.
void WorldScreen::cycleGameObjectHover() {
    GoUseUi& s = goUseUi();
    if (s.cands.size() < 2) return;
    size_t i = 0;
    while (i < s.cands.size() && s.cands[i].second != s.hover) ++i;
    i = (i + 1) % s.cands.size();
    s.chosen = s.hover = s.cands[i].second;
    s.tipKey = ~0ull;
    s.hoverInReach = s.cands[i].first < 100.0f;
    PSP_LOGI(pp::tag::Ctrl, "go_hover_next guid=%08X", static_cast<unsigned>(s.hover & 0xFFFFFFFFu));
}

// ↑ (interactTarget): the hovered object when it is nearer than the NPC the press would talk to.
bool WorldScreen::tryUseGameObject(float npcDist) {
    if (!gou_) return false;
    GoUseUi& s = *gou_;
    if (s.hover == 0) return false;
    const GoInfo* g = session_.findGameObject(s.hover);
    if (!g) return false;
    glm::vec3 c;
    float r = 0.0f;
    goSphere(*g, c, r);
    const float d = std::max(0.0f, glm::length(c - glm::vec3(playerX_, playerY_, playerZ_)) - r);
    if (d >= npcDist && s.hover != session_.fishingBobber()) return false;
    const bool moving = moving_ || wantMove_;
    session_.useGameObject(s.hover, s.hoverInReach, moving);
    s.tipKey = ~0ull;  // its state may change
    return true;
}

// draw(): the hovered object's model once more, additively (the PSP's highlight); a biting bobber pulses.
void WorldScreen::drawGameObjectHighlight(const rendering::psp::TerrainCamera& cam) {
    if (!gou_ || gou_->hover == 0 || goState_ <= 0 || !modelsOn_) return;
    const GoInfo* g = session_.findGameObject(gou_->hover);
    if (!g || goAnimatedDisplay(g->displayId)) return;  // (the creature renderer's rim: drawGameObjectPost)
    uint32_t add = kHighlightAdd;
    const uint32_t bite = session_.bobberBiteMs();
    if (bite != 0 && g->guid == session_.fishingBobber() && pp::logUptimeMs() - bite < kBiteShowMs) {
        const float t = static_cast<float>((pp::logUptimeMs() - bite) % 400u) / 400.0f;
        const uint32_t k = static_cast<uint32_t>(0x50 + 0x60 * (0.5f + 0.5f * std::sin(t * 6.2831853f)));
        add = (k << 24) | (k << 16) | (k << 8) | k;
    }
    const uint32_t uid = static_cast<uint32_t>(g->guid & 0xFFFFFFFFu) ^ static_cast<uint32_t>(g->guid >> 32);
    PROF_SCOPE("draw.gouse");
    goModels_.drawHighlight(uid, cam, 0xFFE0B080u, add);
}

// drawHudLayers: the ↑ glyph over the hovered object in reach, the sparkle of quest objects (GO_DYNFLAG_LO_SPARKLE).
void WorldScreen::drawGameObjectHud(UiRenderer& ui) {
    if (!gou_) return;
    GoUseUi& s = *gou_;
    PROF_SCOPE("gouse.hud");
    // BarberShopBannerFrame (TOP 2, 5 of UIParent; parent UIParent, not the frame): the art and its caption
    // (GameFontNormalLarge at TOP -94).
    if (s.barber.shown) {
        const float bw = 381.0f * 0.6f, bh = 210.0f * 0.55f, bx = 240.0f + 1.0f - bw * 0.5f, by = -5.0f * 0.55f;
        if (s.barber.bannerTex.pixels) ui.image(s.barber.bannerTex, Rect{bx, by, bw, bh});
        const Font& fn = kit_.font(kFontNormal);
        ui.textShadow(fn, 241.0f, by + 94.0f * 0.55f, s.barber.banner.c_str(), kColGold, Align::Center);
    }
    const float t = static_cast<float>(pp::logUptimeMs() % 1400u) / 1400.0f;
    const float pulse = 0.5f + 0.5f * std::sin(t * 6.2831853f);
    const uint32_t alpha = static_cast<uint32_t>(120.0f + 135.0f * pulse);
    const uint32_t gold = (alpha << 24) | 0x0040D8FFu, white = (alpha << 24) | 0x00C0F0FFu;
    int sparkles = 0;
    for (const GoInfo& g : session_.gameObjects()) {
        if (!(g.dynamic & rp::kGoDynSparkle) || sparkles >= 8) continue;
        const float dx = g.pos.x - playerX_, dy = g.pos.y - playerY_;
        if (dx * dx + dy * dy > 40.0f * 40.0f) continue;
        glm::vec3 c;
        float r = 0.0f, sx = 0.0f, sy = 0.0f;
        goSphere(g, c, r);
        if (!projectHud(c, sx, sy)) continue;
        ++sparkles;
        const float arm = 6.0f * (0.7f + 0.3f * pulse);
        ui.fill(Rect{sx - arm, sy - 1.0f, arm * 2.0f, 2.0f}, gold);
        ui.fill(Rect{sx - 1.0f, sy - arm, 2.0f, arm * 2.0f}, gold);
        ui.fill(Rect{sx - 1.5f, sy - 1.5f, 3.0f, 3.0f}, white);
    }
    if (s.hover == 0 || !s.hoverInReach || uiMode_) return;
    float sx = 0.0f, sy = 0.0f;
    if (projectHud(s.hoverTop, sx, sy)) kit_.glyph(ui, kPadUp, Rect{sx - 8, sy - 8, 16, 16});
}

// The hovered object's GameTooltip, at the default anchor.
void WorldScreen::drawGameObjectTip(UiRenderer& ui) {
    if (!gou_ || gou_->hover == 0) return;
    GoUseUi& s = *gou_;
    if (uiMode_ && !windows_.empty()) return;  // the windows' own tooltips then
    const GoInfo* g = session_.findGameObject(s.hover);
    const auto* t = g ? session_.goTemplate(g->entry) : nullptr;
    if (!g || !t || !t->known) return;
    const uint32_t now = pp::logUptimeMs();
    const uint64_t key = s.hover ^ (static_cast<uint64_t>(session_.goTemplateVersion()) << 40) ^
                         (static_cast<uint64_t>(session_.knownSpellsVersion()) << 32) ^
                         (static_cast<uint64_t>(session_.inventoryVersion()) << 20) ^
                         (static_cast<uint64_t>(g->flags) << 8) ^ g->state;
    if (key != s.tipKey || now - s.tipBuiltMs > 1000u) {
        s.tipKey = key;
        s.tipBuiltMs = now;
        s.tip.clear();
        s.tip.push_back({t->name, kTipGold, true});  // GetName, the header font
        pp::WorldSession::GoLockState lock;
        const bool hasLock = session_.goLock(*g, lock);
        if (g->flags & rp::kGoFlagLocked) {  // LOCKED (own colours: red / green / the skill's colour)
            uint32_t col = kTipRed;
            if (hasLock && lock.openable)
                col = (lock.kind != 2 || lock.skill < 0) ? kTipGreen : skillColor(lock.skill, lock.required);
            s.tip.push_back({CLIENT_STR(LOCKED), col, false});
        }
        // The lock line for the case the session reports (a case that applies now).
        const bool caseLine = hasLock && (lock.applies || (lock.openable && lock.kind != 0));
        if (caseLine && lock.kind == 1) {  // LOCKED_WITH_ITEM: the key's name (asked for when not known)
            if (const auto* it = session_.getItemInfo(lock.index)) {
                if (!it->name.empty()) s.tip.push_back({CLIENT_FMT(LOCKED_WITH_ITEM, it->name), kTipWhite, false});
            } else {
                session_.ensureItemInfo(lock.index);
            }
        } else if (caseLine && lock.kind == 3) {  // LOCKED_WITH_SPELL
            const std::string nm = spellDb_.name(lock.index);
            if (!nm.empty()) s.tip.push_back({CLIENT_FMT(LOCKED_WITH_SPELL, nm), kTipWhite, false});
        } else if (caseLine && lock.kind == 2) {  // LOCKED_WITH_SPELL_KNOWN: the lock type, red without a known spell
            const std::string type = lock.typeName.empty() ? std::string("UNKNOWN") : lock.typeName;
            const uint32_t col = lock.spell == 0 ? kTipRed : lock.skill < 0 ? kTipWhite : skillColor(lock.skill, lock.required);
            s.tip.push_back({CLIENT_FMT(LOCKED_WITH_SPELL_KNOWN, type), col, false});
        }
        // showQuestTrackingTooltips: the quests in the log the object serves - an objective of using it, an item it holds.
        for (const auto& q : session_.getQuestLog()) {
            if (q.complete) continue;
            std::vector<std::string> objLines;
            for (const auto& ko : q.killObjectives) {
                if (ko.npcOrGoId >= 0 || static_cast<uint32_t>(-ko.npcOrGoId) != g->entry || ko.required == 0) continue;
                auto kc = q.killCounts.find(g->entry);
                const uint32_t have = kc != q.killCounts.end() ? kc->second.first : 0;
                char b[160];
                std::snprintf(b, sizeof(b), " - %s: %u/%u", t->name.c_str(), static_cast<unsigned>(have),
                              static_cast<unsigned>(ko.required));  // QUEST_OBJECTS_FOUND
                objLines.emplace_back(b);
            }
            for (uint32_t qi : t->questItems) {
                if (qi == 0) continue;
                for (const auto& io : q.itemObjectives) {
                    if (io.itemId != qi || io.required == 0) continue;
                    auto ic = q.itemCounts.find(qi);
                    const uint32_t have = ic != q.itemCounts.end() ? ic->second : 0;
                    if (have >= io.required) continue;
                    const auto* info = session_.getItemInfo(qi);
                    if (!info) session_.ensureItemInfo(qi);
                    char b[160];
                    std::snprintf(b, sizeof(b), " - %s: %u/%u", info && !info->name.empty() ? info->name.c_str() : "?",
                                  static_cast<unsigned>(have), static_cast<unsigned>(io.required));  // QUEST_ITEMS_NEEDED
                    objLines.emplace_back(b);
                }
            }
            if (objLines.empty()) continue;
            s.tip.push_back({questText(q.title), kTipGold, false});
            for (auto& l : objLines) s.tip.push_back({std::move(l), kTipWhite, false});
        }
        // Layout (ui_world_tooltip.cpp: the header 13 px, the body 10 px, wrapped at 200 px).
        float w = 0.0f, h = 0.0f;
        std::vector<GoUseUi::TipLine> wrapped;
        for (const auto& l : s.tip) {
            const Font& f = kit_.font(l.header ? kFontNormal : kFontSmall);
            for (const std::string& part : wrapQuestText(f, l.text, kTipWrap)) {
                wrapped.push_back({part, l.color, l.header});
                w = std::max(w, static_cast<float>(f.measure(part.c_str())));
                h += static_cast<float>(f.lineHeight()) + kTipGap;
            }
        }
        s.tip.swap(wrapped);
        s.tipW = std::ceil(w + 2.0f * kTipPad);
        s.tipH = std::ceil(h - kTipGap + 2.0f * kTipPad);
        if (++s.tipLogs <= 20)
            PSP_LOGI(pp::tag::Gfx, "go_tip entry=%u lines=%u lock=%u kind=%u open=%d", static_cast<unsigned>(g->entry),
                     static_cast<unsigned>(s.tip.size()), static_cast<unsigned>(session_.goData(*g, rp::kPropLock)),
                     static_cast<unsigned>(lock.kind), lock.openable ? 1 : 0);
    }
    if (!s.borderTried) {  // ui.pak "gt_border" (UI-Tooltip-Border, 2 KB), kept with the state
        s.borderTried = true;
        char path[256];
        pp::fsPath(path, sizeof(path), "data/ui.pak");
        pp::Pak pak;
        if (pak.open(path)) loadTex(pak, "gt_border", s.border);
    }
    PROF_GE_PASS("gouse.tip");
    ui.beginFrame();
    // GameTooltip_SetDefaultAnchor: BOTTOMRIGHT over the bars (ui_world_tooltip.cpp: x 476, y 230).
    const float x = std::clamp(476.0f - s.tipW, 2.0f, std::max(2.0f, pp::kScreenW - 2.0f - s.tipW));
    const float y = std::clamp(230.0f - s.tipH, 2.0f, std::max(2.0f, pp::kScreenH - 2.0f - s.tipH));
    const Rect r{x, y, s.tipW, s.tipH};
    ui.fill(Rect{r.x + 2, r.y + 2, r.w - 4, r.h - 4}, kTipBackground);
    if (s.border.pixels) ui.nineSlice(s.border, r);
    else if (kit_.has(kTexTipBorder)) ui.nineSlice(kit_.tex(kTexTipBorder), r);
    float ly = y + kTipPad;
    for (const auto& l : s.tip) {
        const Font& f = kit_.font(l.header ? kFontNormal : kFontSmall);
        ui.textShadow(f, x + kTipPad, ly, l.text.c_str(), l.color);
        ly += static_cast<float>(f.lineHeight()) + kTipGap;
    }
    ui.endFrame();
}

// ---- The animations of objects (the state rules of psp_gameobject_format.hpp) ----
// An object whose M2 has the object animations is a unit of the creature renderer (creatures.pak "d_<kGoAnimDisplay-
// Base + display>", the list "goanim"); the rest stay static models (gameobjects.pak). Its loop is the state's anim
// (Closed / Opened / Destroyed: the unit's emoteAnim - the renderer plays Stand when the model lacks it); a change of
// GAMEOBJECT_BYTES_1's state plays the transition (Open / Close / Destroy / Rebuild) once, then the new loop; the
// server's custom anims (SMSG_GAMEOBJECT_CUSTOM_ANIM: Custom0..3) and despawn (Despawn) once.

bool WorldScreen::goAnimatedDisplay(uint32_t displayId) const {
    if (!gou_ || gou_->animDisplays.empty()) return false;
    return std::binary_search(gou_->animDisplays.begin(), gou_->animDisplays.end(), displayId);
}

void WorldScreen::loadGoAnimList() {
    GoUseUi& s = goUseUi();
    if (s.animTried || !creatures_.isOpen()) return;
    s.animTried = true;
    if (pp::configGetInt("goanim", 1) == 0) return;  // wowee.cfg goanim=0: every object static (A/B)
    std::vector<uint8_t> blob;
    if (creatures_.readRecord("goanim", blob) && blob.size() >= sizeof(rp::GoAnimHeader) &&
        std::memcmp(blob.data(), "WGOA", 4) == 0) {
        rp::GoAnimHeader h;
        std::memcpy(&h, blob.data(), sizeof(h));
        if (sizeof(h) + uint64_t{h.count} * 4u <= blob.size()) {
            s.animDisplays.resize(h.count);
            if (h.count) std::memcpy(s.animDisplays.data(), blob.data() + sizeof(h), h.count * 4u);
        }
    }
    if (!s.animDisplays.empty()) goSeenVersion_ = ~0u;  // the static set again, without them
    PSP_LOGI(pp::tag::Gfx, "goanim displays=%u kb=%u%s", static_cast<unsigned>(s.animDisplays.size()),
             static_cast<unsigned>(s.animDisplays.size() * 4u / 1024u),
             s.animDisplays.empty() ? " (creatures.pak without \"goanim\": objects static)" : "");
}

void WorldScreen::appendGameObjectUnits(std::vector<rendering::psp::CreatureUnit>& out) {
    if (!gou_ || gou_->animDisplays.empty() || goState_ < 0) return;
    GoUseUi& s = *gou_;
    PROF_SCOPE("gouse.units");
    const float reach = creatures_.viewDistance + 10.0f;
    const uint32_t frame = ++s.animFrame;
    size_t added = 0;
    for (const GoInfo& g : session_.gameObjects()) {
        if (g.displayId == 0 || !goAnimatedDisplay(g.displayId)) continue;
        const float dx = g.pos.x - playerX_, dy = g.pos.y - playerY_;
        if (dx * dx + dy * dy > reach * reach) continue;
        GoUseUi::AnimGo& a = s.animGos[g.guid];
        if (a.frame == 0) {  // first sight: the state as it is (goAnimStateOnChange with old == new)
            a.seq = g.stateSeq;
            a.animState = rp::goAnimStateOnChange(g.state, g.state, false);
            a.loopAnim = rp::goAnimLoops(a.animState) ? rp::goAnimIdOf(a.animState) : rp::goAnimIdOf(rp::goAnimStateAfter(a.animState));
        } else if (a.seq != g.stateSeq) {  // the state changed: the transition, then its loop
            a.seq = g.stateSeq;
            const bool synced = (g.dynamic >> 16) != 0xFFFFu && (g.dynamic >> 16) != 0u;
            a.animState = rp::goAnimStateOnChange(g.prevState == 0xFF ? g.state : g.prevState, g.state, synced);
            const uint8_t after = rp::goAnimStateAfter(a.animState);
            a.loopAnim = rp::goAnimIdOf(after == 0xFF ? a.animState : after);
            if (!rp::goAnimLoops(a.animState)) {
                a.pendingShot = rp::goAnimIdOf(a.animState);  // played after this frame's draw (drawGameObjectPost)
                a.pendingTries = 0;
            }
            if (++s.logs <= 60)
                PSP_LOGI(pp::tag::Gfx, "goanim_state guid=%08X state=%u->%u anim=%u loop=%u",
                         static_cast<unsigned>(g.guid & 0xFFFFFFFFu), static_cast<unsigned>(g.prevState),
                         static_cast<unsigned>(g.state), static_cast<unsigned>(rp::goAnimIdOf(a.animState)),
                         static_cast<unsigned>(a.loopAnim));
        }
        a.frame = frame;
        rendering::psp::CreatureUnit u;
        u.guid = g.guid;
        u.displayId = rp::kGoAnimDisplayBase + g.displayId;
        u.x = g.pos.x;
        u.y = g.pos.y;
        u.z = g.pos.z;
        u.orientation = g.orientation;
        u.scale = g.scale;
        u.emoteAnim = a.loopAnim;
        out.push_back(u);
        ++added;
    }
    // Objects out of sight forget their state (the next sight is a create again).
    for (auto it = s.animGos.begin(); it != s.animGos.end();) {
        if (it->second.frame != frame) it = s.animGos.erase(it);
        else ++it;
    }
    // The renderer keeps its nearest maxDrawn units: the objects come on top of the NPCs' (wowee.cfg npcmax).
    if (creatures_.maxDrawn != s.lastMaxDrawn) s.baseMaxDrawn = creatures_.maxDrawn;
    creatures_.maxDrawn = s.baseMaxDrawn + static_cast<uint32_t>(added);
    s.lastMaxDrawn = creatures_.maxDrawn;
}

// draw(): after the creatures. The one-shots wait for this point: the renderer took the new loop in its draw (a new
// emoteAnim restarts the loop), so the transition plays now and returns to it. The hovered object drawn by the creature
// renderer gets the target rim (its static counterpart the additive highlight, drawGameObjectHighlight).
void WorldScreen::drawGameObjectPost(const rendering::psp::TerrainCamera& cam) {
    if (!gou_) return;
    GoUseUi& s = *gou_;
    for (auto& [guid, a] : s.animGos) {
        if (!a.pendingShot) continue;
        if (creatures_.playOneShot(guid, a.pendingShot)) a.pendingShot = 0;
        else if (++a.pendingTries >= 30) a.pendingShot = 0;  // not drawn / the model lacks it: the loop only
    }
    if (s.hover == 0 || !creaturesOn_) return;
    const GoInfo* g = session_.findGameObject(s.hover);
    if (!g || !goAnimatedDisplay(g->displayId)) return;
    creatures_.drawOutline(g->guid, 0xB0A0E0F0u, cam, 0xFFE0B080u);
}

// ---- The own look, BarberShopFrame (the barber shop add-on's .xml / .lua) ----

void WorldScreen::ownLookOverride(rendering::psp::CharacterLook& look) {
    if (session_.selfKnown()) {  // PLAYER_BYTES: skin, face, hair style, hair colour; PLAYER_BYTES_2 byte 0: facial
        const uint32_t b = session_.ownField(game::fieldIndex(game::UF::PLAYER_BYTES));
        if (b != 0) {
            look.skin = static_cast<uint8_t>(b & 0xFFu);
            look.face = static_cast<uint8_t>((b >> 8) & 0xFFu);
            look.hairStyle = static_cast<uint8_t>((b >> 16) & 0xFFu);
            look.hairColor = static_cast<uint8_t>((b >> 24) & 0xFFu);
            look.facial = static_cast<uint8_t>(session_.ownField(game::fieldIndex(game::UF::PLAYER_BYTES_2)) & 0xFFu);
        }
    }
    if (!gou_ || !gou_->barber.shown) return;
    const GoUseUi::Barber& br = gou_->barber;  // SetNextBarberShopStyle's preview on the model
    if (!br.hair.empty()) look.hairStyle = br.hair[static_cast<size_t>(br.sel[0])]->data;
    look.hairColor = static_cast<uint8_t>(br.sel[1]);
    if (!br.facial.empty()) look.facial = br.facial[static_cast<size_t>(br.sel[2])]->data;
}

void WorldScreen::ownLookLoaded() {
    if (!gou_) return;
    GoUseUi& s = *gou_;
    if (s.chairState >= 4 && s.chairState <= 6 && sitting_) animator_.setStandState(s.chairState);  // a chair's loop
    if (session_.selfKnown() && !s.barber.shown) {
        s.lookKnown = true;
        s.lookBytes = session_.ownField(game::fieldIndex(game::UF::PLAYER_BYTES));
        s.lookFacial = session_.ownField(game::fieldIndex(game::UF::PLAYER_BYTES_2)) & 0xFFu;
    }
}

// BarberShopFrame follows the session: BARBER_SHOP_OPEN (SMSG_ENABLE_BARBER_SHOP) shows it with the current look's
// styles (OnShow: CloseAllBags, BarberShop_ResetLabelColors, BarberShop_UpdateCost, the banner BARBERSHOP, the sound
// BarberShop_Sit), the player standing up (BARBER_SHOP_CLOSE) hides it; closing it by hand is CancelBarberShop.
void WorldScreen::barberSync() {
    GoUseUi& s = goUseUi();
    GoUseUi::Barber& b = s.barber;
    const bool want = session_.barberShopOpen();
    if (want && !b.shown) {
        const game::Character* me = session_.activeCharacter();
        if (!me) return;
        b.shown = true;
        char path[256];
        pp::fsPath(path, sizeof(path), "data/ui.pak");
        pp::Pak pak;
        if (pak.open(path)) {
            if (const pp::PakEntry* e = pak.find("go_barber"))
                if (void* raw = pak.load(e)) {
                    b.blob.assign(static_cast<const uint8_t*>(raw), static_cast<const uint8_t*>(raw) + e->size);
                    pp::pakFree(raw);
                }
            loadTex(pak, "barber_frame", b.frame);
            loadTex(pak, "barber_banner", b.bannerTex);
            loadTex(pak, "barber_money", b.money);
            loadTex(pak, "sb_prev", b.prev);
            loadTex(pak, "sb_next", b.next);
            pak.close();
        }
        b.artTried = true;
        const uint8_t race = static_cast<uint8_t>(me->race);
        const uint8_t sex = me->gender == game::Gender::FEMALE ? 1 : 0;
        if (b.blob.size() >= sizeof(rp::GoBarberHeader) && std::memcmp(b.blob.data(), "WGBS", 4) == 0) {
            b.h = reinterpret_cast<const rp::GoBarberHeader*>(b.blob.data());
            const size_t stylesAt = sizeof(rp::GoBarberHeader);
            const size_t costsAt = stylesAt + b.h->styles * sizeof(rp::GoBarberStyle);
            const size_t racesAt = costsAt + b.h->levels * sizeof(float);
            const size_t poolAt = racesAt + b.h->races * sizeof(rp::GoBarberRace);
            if (poolAt + b.h->poolSize <= b.blob.size()) {
                const auto* st = reinterpret_cast<const rp::GoBarberStyle*>(b.blob.data() + stylesAt);
                b.costs = reinterpret_cast<const float*>(b.blob.data() + costsAt);
                b.pool = reinterpret_cast<const char*>(b.blob.data() + poolAt);
                for (uint32_t i = 0; i < b.h->styles; ++i) {
                    if (st[i].race != race || st[i].sex != sex) continue;
                    if (st[i].type == 0) b.hair.push_back(&st[i]);
                    else if (st[i].type == 2) b.facial.push_back(&st[i]);
                }
                auto byData = [](const rp::GoBarberStyle* x, const rp::GoBarberStyle* y) { return x->data < y->data; };
                std::sort(b.hair.begin(), b.hair.end(), byData);
                std::sort(b.facial.begin(), b.facial.end(), byData);
                if (race < b.h->races) {
                    const rp::GoBarberRace& rr = reinterpret_cast<const rp::GoBarberRace*>(b.blob.data() + racesAt)[race];
                    b.label[0] = b.pool + rr.hairStyle;
                    b.label[1] = b.pool + rr.hairColor;
                    b.label[2] = b.pool + rr.facial[sex];
                    b.colors = rr.hairColors[sex];
                }
            } else {
                b.h = nullptr;
            }
        }
        // The current look -> the selectors (GetBarberShopStyleInfo's isCurrent).
        const uint32_t bytes = session_.ownField(game::fieldIndex(game::UF::PLAYER_BYTES));
        const uint32_t facial = session_.ownField(game::fieldIndex(game::UF::PLAYER_BYTES_2)) & 0xFFu;
        auto indexOf = [](const std::vector<const rp::GoBarberStyle*>& v, uint32_t data) {
            for (size_t i = 0; i < v.size(); ++i)
                if (v[i]->data == data) return static_cast<int>(i);
            return 0;
        };
        b.cur[0] = indexOf(b.hair, (bytes >> 16) & 0xFFu);
        b.cur[1] = static_cast<int>((bytes >> 24) & 0xFFu);
        b.cur[2] = indexOf(b.facial, facial);
        std::copy(b.cur, b.cur + 3, b.sel);
        b.banner = CLIENT_STR(BARBERSHOP);
        for (bool& o : bagOpen_) o = false;  // CloseAllBags
        openWindow(kWinBarber);
        playSoundByName("BarberShop_Sit");
        if (!uiMode_) {
            setUiMode(true);
            b.raised = true;
        }
        winFocus_.setFocus(kIdBarberNext1);
        PSP_LOGI(pp::tag::Gfx, "barber_frame hair=%u facial=%u colors=%d cur=%d/%d/%d kb=%u",
                 static_cast<unsigned>(b.hair.size()), static_cast<unsigned>(b.facial.size()), b.colors, b.cur[0],
                 b.cur[1], b.cur[2],
                 static_cast<unsigned>((b.blob.size() + pp::textureBytes(b.frame) + pp::textureBytes(b.bannerTex) +
                                        pp::textureBytes(b.money) + pp::textureBytes(b.prev) + pp::textureBytes(b.next)) /
                                       1024u));
    } else if (b.shown && (!want || !windowOpen(kWinBarber))) {
        if (want) session_.cancelBarberShop();  // the frame closed by hand: CancelBarberShop
        if (windowOpen(kWinBarber)) closeWindow(kWinBarber);
        const bool previewed = b.sel[0] != b.cur[0] || b.sel[1] != b.cur[1] || b.sel[2] != b.cur[2];
        b.shown = false;
        b.release();
        if (b.raised && uiMode_ && windows_.empty()) setUiMode(false);
        b.raised = false;
        if (previewed) loadCharacter();  // the preview goes: the look of the fields again
        PSP_LOGI(pp::tag::Gfx, "barber_frame closed");
    }
    uint32_t result = 0;
    while (session_.takeBarberResult(result))
        if (result == 0) {  // BARBER_SHOP_SUCCESS: PlaySound("Barbershop_Haircut"); the new look is the current one
            playSoundByName("Barbershop_Haircut");
            std::copy(b.sel, b.sel + 3, b.cur);
        }
}

bool WorldScreen::drawBarberFrame(UiRenderer& ui) {
    GoUseUi& s = goUseUi();
    GoUseUi::Barber& b = s.barber;
    PROF_SCOPE("gouse.barber");
    loadNpcArt(false);  // UIPanelButtonTemplate, the coins
    const bool focusable = uiMode_ && !settings_.isOpen() && !windowOpen(kWinBindConfirm);
    const Font& fn = kit_.font(kFontNormal);
    const Rect frame{kBarberX, kBarberY, kBarberW, kBarberH};
    if (b.frame.pixels) ui.image(b.frame, frame);
    else kit_.panel(ui, frame);
    // BarberShopFrameSelector1..3 (TOP 2,-74, then 1 under each other; 175 x 32): the category label (GameFontNormal,
    // 90 x 14 centred; white = the current style, gold = changed), Prev / Next 5 outside it.
    bool changed = false;
    for (int i = 0; i < 3; ++i) {
        const float top = 74.0f + static_cast<float>(i) * 33.0f, cy = BY(top + 16.0f);
        const bool current = b.sel[i] == b.cur[i];
        changed = changed || !current;
        ui.textShadow(fn, BX(134.5f), cy - fn.lineHeight() * 0.5f, b.label[i].c_str(), current ? kColWhite : kColGold,
                      Align::Center);
        const uint32_t prevId = kIdBarberPrev1 + static_cast<uint32_t>(i) * 2u, nextId = prevId + 1u;
        const Rect pr{BX(68.5f) - 9, cy - 9, 18, 18}, nr{BX(200.5f) - 9, cy - 9, 18, 18};
        const bool fp = focusable && winFocus_.addStop(prevId, pr);
        const bool fnx = focusable && winFocus_.addStop(nextId, nr);
        if (b.prev.pixels) ui.image(b.prev, pr);
        if (b.next.pixels) ui.image(b.next, nr);
        if (fp && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, pr, 0xFFFFFFFFu, Blend::Add);
        if (fnx && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, nr, 0xFFFFFFFFu, Blend::Add);
    }
    // BarberShopFrameMoneyFrame (TOP at Selector3's BOTTOM 7,-10) over UI-MoneyFrame-Border: GetBarberShopTotalCost -
    // gtBarberShopCostBase of the level x the new hair style's CostModifier, the colour alone x 0.5, the facial style's
    // CostModifier (AzerothCore Player::GetBarberShopCost reckons the same).
    uint32_t cost = 0;
    if (b.costs && b.h && b.h->levels > 0) {
        const uint32_t level = std::clamp<uint32_t>(session_.selfField(game::fieldIndex(game::UF::UNIT_FIELD_LEVEL)), 1u,
                                                    b.h->levels);
        const float base = b.costs[level - 1];
        float c = 0.0f;
        if (b.sel[0] != b.cur[0] && !b.hair.empty()) c += base * b.hair[static_cast<size_t>(b.sel[0])]->costModifier;
        if (b.sel[1] != b.cur[1] && b.sel[0] == b.cur[0]) c += base * 0.5f;
        if (b.sel[2] != b.cur[2] && !b.facial.empty()) c += base * b.facial[static_cast<size_t>(b.sel[2])]->costModifier;
        cost = static_cast<uint32_t>(c);
    }
    if (b.money.pixels) ui.image(b.money, Rect{BX(133.5f - 64.0f), BY(194.5f - 16.0f), 128 * 0.6f, 32 * 0.55f});
    drawMoneyFrame(ui, BX(141.5f + 36.0f), BY(182.0f), cost, true);
    // Okay (RIGHT at Selector3's BOTTOM -2,-48), Cancel (5 right of it), Reset (BOTTOM 2,50): disabled while nothing
    // changed (BarberShop_UpdateCost).
    drawPanelButton(ui, kIdBarberOkay, Rect{BX(52.5f), BY(208), 80 * 0.6f, 24 * 0.55f}, CLIENT_STR(OKAY), changed, focusable);
    drawPanelButton(ui, kIdBarberCancel, Rect{BX(137.5f), BY(208), 80 * 0.6f, 24 * 0.55f}, CLIENT_STR(COINPICKUP_CANCEL), true,
                    focusable);
    drawPanelButton(ui, kIdBarberReset, Rect{BX(94.5f), BY(245), 80 * 0.6f, 24 * 0.55f}, CLIENT_STR(RESET), changed,
                    focusable);
    return true;
}

// ---- ItemTextFrame (ItemTextFrame.xml / .lua) ----

bool WorldScreen::drawGameObjectWindow(UiRenderer& ui, uint8_t w) {
    if (w == kWinBarber) return drawBarberFrame(ui);
    if (w != kWinItemText) return false;
    GoUseUi& s = goUseUi();
    const auto& it = session_.itemText();
    PROF_SCOPE("gouse.itemtext");
    // The art: the frame, the material's (none for Parchment), the scroll bar; the close / page buttons (NPC art).
    loadNpcArt(false);
    const uint8_t mat = it.material ? it.material : 1;
    if (!s.frameTried || s.matLoaded != mat) {
        char path[256];
        pp::fsPath(path, sizeof(path), "data/ui.pak");
        pp::Pak pak;
        if (pak.open(path)) {
            if (!s.frameTried) {
                loadTex(pak, "itext_frame", s.frame);
                loadTex(pak, "mail_scroll", s.scrollArt);  // UI-Character-ScrollBar (the bank / mail art's)
                loadTex(pak, "sb_prev", s.prev);  // UI-SpellbookIcon-PrevPage / -NextPage (the spell book's)
                loadTex(pak, "sb_next", s.next);
            }
            if (s.mat.pixels) pp::textureRelease(s.mat);
            if (const char* m = materialName(mat)) {
                char name[32];
                std::snprintf(name, sizeof(name), "itext_mat_%s", m);
                loadTex(pak, name, s.mat);
            }
            pak.close();
        }
        s.frameTried = true;
        s.matLoaded = mat;
        PSP_LOGI(pp::tag::Gfx, "itemtext_art frame=%d material=%u mat=%d kb=%u", s.frame.pixels ? 1 : 0,
                 static_cast<unsigned>(mat), s.mat.pixels ? 1 : 0,
                 static_cast<unsigned>((pp::textureBytes(s.frame) + pp::textureBytes(s.mat) + pp::textureBytes(s.scrollArt) +
                                        pp::textureBytes(s.prev) + pp::textureBytes(s.next)) / 1024u));
    }
    const bool focusable = uiMode_ && !settings_.isOpen() && !windowOpen(kWinBindConfirm);
    const Font& fs = kit_.font(kFontSmall);
    const Font& fn = kit_.font(kFontNormal);
    const float lh = static_cast<float>(fs.lineHeight());
    const Rect frame{kOx, kOy, kTextFrameW * kSX, kTextFrameH * kSY};
    if (s.frame.pixels) ui.image(s.frame, frame);
    else kit_.panel(ui, frame);
    if (s.mat.pixels) ui.image(s.mat, Rect{TX(21), TY(75), 320 * kSX, 384 * kSY});
    // ItemTextTitleText (CENTER 6, 230: the frame's top band), GameFontNormal.
    {
        const Rect tr{TX(198 - 112), TY(26) - fn.lineHeight() * 0.5f, 224 * kSX, static_cast<float>(fn.lineHeight())};
        ui.pushClip(tr);
        ui.textShadow(fn, TX(198), tr.y, it.title.c_str(), kColGold, Align::Center);
        ui.popClip();
    }
    // ItemTextCloseButton (CENTER at TOPRIGHT -45, -26).
    {
        const Rect cr{TX(339) - 9, TY(26) - 9, 18, 18};
        const bool f = focusable && winFocus_.addStop(kIdTextClose, cr);
        const pp::Texture& t = f && winFocus_.isPressed(kIdTextClose) && npcArt_.closeDown.pixels ? npcArt_.closeDown : npcArt_.close;
        if (t.pixels) ui.image(t, cr);
        if (f && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, cr, 0xFFFFFFFFu, Blend::Add);
    }
    // ITEM_TEXT_READY: the page number and the buttons only with more than one page.
    if (it.ready && (it.page > 1 || it.hasNext)) {
        char pg[16];
        std::snprintf(pg, sizeof(pg), "%u", static_cast<unsigned>(it.page));  // ItemTextCurrentPage:SetText(page)
        ui.textShadow(fn, TX(202), TY(50) - fn.lineHeight() * 0.5f, pg, kColGold, Align::Center);
        if (it.page > 1) {  // ItemTextPrevPageButton (CENTER at TOPLEFT 90, -56) + PREV right of it
            const Rect br{TX(90) - 9, TY(56) - 9, 18, 18};
            const bool f = focusable && winFocus_.addStop(kIdTextPrev, br);
            if (s.prev.pixels) ui.image(s.prev, br);
            if (f && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, br, 0xFFFFFFFFu, Blend::Add);
            ui.textShadow(fs, br.x + br.w + 1, TY(56) - lh * 0.5f, CLIENT_STR(PREV), kColGold);
        }
        if (it.hasNext) {  // ItemTextNextPageButton (CENTER at TOPRIGHT -55, -56) + NEXT left of it
            const Rect br{TX(329) - 9, TY(56) - 9, 18, 18};
            const bool f = focusable && winFocus_.addStop(kIdTextNext, br);
            if (s.next.pixels) ui.image(s.next, br);
            if (f && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, br, 0xFFFFFFFFu, Blend::Add);
            ui.textShadow(fs, br.x - 1, TY(56) - lh * 0.5f, CLIENT_STR(NEXT), kColGold, Align::Right);
        }
    }
    // ItemTextScrollFrame (TOPRIGHT -66, -76, 280 x 355): the bar's art right of it; ItemTextPageText 270 wide at -15.
    const Rect box{TX(38), TY(76), 280 * kSX, 355 * kSY};
    if (s.scrollArt.pixels) ui.image(s.scrollArt, Rect{TX(318), TY(71), 19, 199});
    if (it.ready) {
        if (s.textVersion != it.version) {
            s.textVersion = it.version;
            s.scroll = 0;
            // ITEM_TEXT_READY: "\n" .. ItemTextGetText() .. "\n\n" (no creator for an object).
            s.lines = wrapQuestText(fs, "\n" + pageText(it.text), 270 * kSX);
        }
        const int perBox = std::max(1, static_cast<int>((box.h - 15 * kSY) / lh));
        const int total = static_cast<int>(s.lines.size());
        s.scroll = std::clamp(s.scroll, 0, std::max(0, total - 1));
        const bool f = focusable && winFocus_.addStop(kIdTextBody, box);
        (void)f;
        ui.pushClip(box);
        float y = box.y + 15 * kSY;
        const uint32_t col = materialTextColor(mat);
        for (int i = s.scroll; i < total && i < s.scroll + perBox; ++i) {
            ui.text(fs, box.x, y, s.lines[static_cast<size_t>(i)].c_str(), col);
            y += lh;
        }
        ui.popClip();
        // PSP: more below / above - the scroll bar's arrows lit (Cross on the text scrolls a box).
        if (total > perBox) {
            char more[24];
            std::snprintf(more, sizeof(more), "%d/%d", s.scroll / perBox + 1, (total + perBox - 1) / perBox);
            ui.textShadow(kit_.font(kFontTiny), TX(332), TY(440) - 8.0f, more, kColGold, Align::Center);
        }
    }
    return true;
}

bool WorldScreen::gameObjectWindowAction(uint32_t act) {
    if (act < 0x8100 || act > 0x81FF) return false;
    GoUseUi& s = goUseUi();
    switch (act) {
        case kIdTextClose:
            closeWindow(kWinItemText);  // HideParentPanel -> OnHide: CloseItemText (updateGameObjectUse)
            return true;
        case kIdTextPrev:
            playSoundByName("igMainMenuOptionCheckBoxOn");
            session_.itemTextPrevPage();
            return true;
        case kIdTextNext:
            playSoundByName("igMainMenuOptionCheckBoxOn");
            session_.itemTextNextPage();
            return true;
        case kIdBarberPrev1: case kIdBarberNext1: case kIdBarberPrev2: case kIdBarberNext2: case kIdBarberPrev3:
        case kIdBarberNext3: {  // SetNextBarberShopStyle(id, reverse) + BarberShop_UpdateCost / _UpdateSelector
            GoUseUi::Barber& b = s.barber;
            const int cat = static_cast<int>((act - kIdBarberPrev1) / 2u);
            const bool reverse = ((act - kIdBarberPrev1) % 2u) == 0;
            const int n = cat == 0 ? static_cast<int>(b.hair.size()) : cat == 1 ? b.colors : static_cast<int>(b.facial.size());
            if (n <= 0) return true;
            b.sel[cat] = (b.sel[cat] + (reverse ? n - 1 : 1)) % n;
            playSoundByName("UChatScrollButton");
            if (cat == 0) b.banner = b.pool + b.hair[static_cast<size_t>(b.sel[0])]->name;
            else if (cat == 2) b.banner = b.pool + b.facial[static_cast<size_t>(b.sel[2])]->name;
            PSP_LOGI(pp::tag::Gfx, "barber_style cat=%d sel=%d", cat + 1, b.sel[cat]);
            loadCharacter();  // the preview (the model changes at once)
            return true;
        }
        case kIdBarberOkay: {  // ApplyBarberShopStyle: CMSG_ALTER_APPEARANCE
            GoUseUi::Barber& b = s.barber;
            if (b.hair.empty()) return true;
            session_.alterAppearance(b.hair[static_cast<size_t>(b.sel[0])]->id, static_cast<uint32_t>(b.sel[1]),
                                     b.facial.empty() ? 0u : b.facial[static_cast<size_t>(b.sel[2])]->id);
            return true;
        }
        case kIdBarberCancel:  // CancelBarberShop(); PlaySound("igCharacterInfoClose")
            playSoundByName("igCharacterInfoClose");
            session_.cancelBarberShop();
            return true;
        case kIdBarberReset: {  // BarberShopReset(); BarberShop_UpdateBanner(GetBarberShopStyleInfo(1))
            GoUseUi::Barber& b = s.barber;
            std::copy(b.cur, b.cur + 3, b.sel);
            if (!b.hair.empty()) b.banner = b.pool + b.hair[static_cast<size_t>(b.sel[0])]->name;
            playSoundByName("igCharacterInfoClose");
            loadCharacter();
            return true;
        }
        case kIdTextBody: {  // PSP: a box further down; past the end back to the top
            const Font& fs = kit_.font(kFontSmall);
            const int perBox = std::max(1, static_cast<int>((355 * kSY - 15 * kSY) / static_cast<float>(fs.lineHeight())));
            s.scroll += perBox;
            if (s.scroll >= static_cast<int>(s.lines.size())) s.scroll = 0;
            return true;
        }
        default: return true;
    }
}

bool WorldScreen::gameObjectAltAction(uint32_t alt) {
    if (alt < 0x8100 || alt > 0x81FF) return false;
    GoUseUi& s = goUseUi();
    if (alt == kIdTextBody) {  // PSP: Circle on the text - a box back up
        const Font& fs = kit_.font(kFontSmall);
        const int perBox = std::max(1, static_cast<int>((355 * kSY - 15 * kSY) / static_cast<float>(fs.lineHeight())));
        s.scroll = std::max(0, s.scroll - perBox);
    }
    return true;
}

}  // namespace wowee::ui::psp
