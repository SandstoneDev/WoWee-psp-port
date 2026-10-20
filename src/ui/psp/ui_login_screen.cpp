#include "ui/psp/ui_login_screen.hpp"
#include "auth/auth_opcodes.hpp"  // PSP: getAuthResultString - a refused login forgets the saved one

#include "platform/psp/psp_audio.hpp"  // sfxStopAll before a movie
#include "platform/psp/psp_bigheap.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_locale.hpp"  // the client's languages
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_login_store.hpp"  // PSP: the saved login outside the game folder
#include "platform/psp/psp_osk.hpp"
#include "platform/psp/psp_net.hpp"
#include "platform/psp/psp_netconf.hpp"  // PSP: the system connection dialog
#include "platform/psp/psp_pak.hpp"  // pakSetReadTick
#include "platform/psp/psp_public_safe.hpp"  // PSP: the logon address decides the debug aids
#include "platform/psp/psp_system.hpp"  // logMemory
#include "ui/psp/ui_character_screen.hpp"
#include "ui/psp/ui_world_screen.hpp"

#include <pspctrl.h>
#include <pspthreadman.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <malloc.h>  // mallinfo (the transfer's memory line)

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

enum : uint32_t {
    kIdLogin = 1,
    kIdExit = 2,
    kIdRetry = 3,
    kIdRealmOk = 4,      // RealmListOkButton
    kIdRealmCancel = 5,  // RealmListCancelButton
    kIdAccountBox = 6,   // PSP: AccountLoginAccountEdit - the system keyboard
    kIdPasswordBox = 7,  // AccountLoginPasswordEdit
    kIdRealm0 = 100,
};

// Overwrites a string's bytes before it goes (the typed password).
void wipe(std::string& s) {
    volatile char* p = s.empty() ? nullptr : &s[0];
    for (size_t i = 0; i < s.size(); ++i) p[i] = 0;
    s.clear();
}

uint64_t nowUs() { return sceKernelGetSystemTimeWide(); }

// RealmList.xml on 480 x 272 - the frame (596 x 511 units with the header) baked at
// 0.765 x 0.524 (tools/psp/build_glue_pak.py RL_SX / RL_SY), drawn at (12, 2).
constexpr float kRlX = 12.0f, kRlY = 2.0f, kRlSX = 0.765f, kRlSY = 0.524f, kRlTop = 12.0f;
float RX(float u) { return kRlX + u * kRlSX; }
constexpr float kRowsY = 46.0f, kRowH = 14.0f;  // PSP rows (REALM_BUTTON_HEIGHT 16 + 4 at 0.524 is too small to read)
constexpr int kRealmRows = 13;

// Realm flags (AzerothCore RealmFlags; upstream RealmScreen): 0x01 version mismatch, 0x02 offline,
// 0x20 recommended, 0x40 new, 0x80 full.
struct RealmLoad {
    const char* text;
    uint32_t col;
    bool down;
};
RealmLoad realmLoad(const auth::Realm& r) {
    // RealmListUpdate: realmDown, locked, load -3 recommended, -2 new, 2 full, > 0 high, < 0 low, else medium;
    // the load from the population as upstream's getPopulationName (< 0.5 low, < 1.5 medium, else high).
    if (r.flags & 0x02) return {GLUE_STR(REALM_DOWN), kGlueDisable, true};
    if (r.lock) return {GLUE_STR(REALM_LOCKED), kGlueRed, false};
    if (r.flags & 0x20) return {GLUE_STR(LOAD_RECOMMENDED), kGlueBlue, false};
    if (r.flags & 0x40) return {GLUE_STR(LOAD_NEW), kGlueGreen, false};
    if ((r.flags & 0x80) || r.population >= 2.5f) return {GLUE_STR(LOAD_FULL), kGlueRed, false};
    if (r.population >= 1.5f) return {GLUE_STR(LOAD_HIGH), kGlueRed, false};
    if (r.population < 0.5f) return {GLUE_STR(LOAD_LOW), kGlueGreen, false};
    return {GLUE_STR(LOAD_MEDIUM), kGlueGold, false};
}

// pvpText: RP-PvP gold, RP green, PvP red, else GAMETYPE_NORMAL gold (realm icon: 1 PvP, 6 RP, 8 RP-PvP).
void realmType(const auth::Realm& r, const char*& text, uint32_t& col) {
    switch (r.icon) {
        case 1: text = GLUE_STR(PVP_PARENTHESES); col = kGlueRed; break;
        case 6: text = GLUE_STR(RP_PARENTHESES); col = kGlueGreen; break;
        case 8: text = GLUE_STR(RPPVP_PARENTHESES); col = kGlueGold; break;
        default: text = GLUE_STR(GAMETYPE_NORMAL); col = kGlueGold; break;
    }
}

} // namespace

LoginScreen::LoginScreen(const UiKit& kit, LoginSettings settings) : kit_(kit), settings_(std::move(settings)) {}

LoginScreen::~LoginScreen() {
    loadingSetHook(nullptr, nullptr);
    pp::pakSetReadTick(nullptr, nullptr);
    if (auth_) auth_->disconnect();
}

void LoginScreen::enterStage(Stage s) {
    stage_ = s;
    stageStartUs_ = nowUs();
    PSP_LOGI(pp::tag::Net, "login_stage=%s", stageText());
    // Stage changes are rare events, not per-frame work: commit so the card
    // holds the login trace even if the session dies right after.
    pp::logFlush();
}

void LoginScreen::fail(const std::string& reason) {
    failure_ = reason;
    PSP_LOGE(pp::tag::Net, "login_failed stage=%s reason=%s", stageText(), reason.c_str());
    // May run inside an AuthHandler callback: the handler is torn down on the
    // next update(), never from within its own packet processing.
    enterStage(Stage::Failed);
    // The original's dialog ("OKAY" type) with the client's text. `reason` is display text
    // (the client's or the port's own, in the running language); the auth link's English reasons go through
    // loginError() first (setOnFailure).
    dialog_.show(reason, GLUE_STR(OKAY));
    realmsPending_ = false;
}

void LoginScreen::start() {
    realms_.clear();
    failure_.clear();
    dialog_.hide();
    worstUpdateUs_ = 0;
    realmsPending_ = false;
    if (settings_.server.empty()) {
        fail(OWN_STR(CFG_NO_SERVER));
        return;
    }
    // PSP: AccountLogin_Login asks for what is missing - here the keyboard opens on that box.
    if (settings_.account.empty()) {
        openField(1);
        return;
    }
    if (settings_.password.empty() && settings_.authHash.size() != 20) {
        openField(2);
        return;
    }
    if (pp::netState() == pp::NetState::Connected) {
        enterStage(Stage::ConnectPending);
        return;
    }
    // PSP: already on an access point (apctl GOT_IP) - no dialog, no new connection.
    if (pp::netApConnected()) {
        pp::netAdoptConnection("apctl");
        enterStage(Stage::ConnectPending);
        return;
    }
    // PSP: the firmware's connection dialog lets the player pick the network (wowee.cfg netdialog=1, the
    // default); the saved-profile connection below is netdialog=0 and the automated runs.
    if (settings_.netDialog) {
        if (!pp::netPrepare()) {
            fail(pp::netState() == pp::NetState::NoSwitch ? OWN_STR(WIFI_SWITCH_OFF)
                                                          : std::string("Wi-Fi: ") + pp::netStatus());
            return;
        }
        if (pp::netconfBegin()) {
            enterStage(Stage::NetDialog);
            return;
        }
        PSP_LOGW(pp::tag::Net, "netconf_fallback profile=%d ssid=%s", settings_.netProfile, settings_.netSsid.c_str());
    }
    enterStage(Stage::Wifi);
    if (!pp::netStart(settings_.netProfile, settings_.netSsid.c_str())) {
        switch (pp::netState()) {
            case pp::NetState::NoSwitch: fail(OWN_STR(WIFI_SWITCH_OFF)); break;
            case pp::NetState::NoProfile: fail(OWN_STR(WIFI_NO_PROFILE)); break;
            default: fail(std::string("Wi-Fi: ") + pp::netStatus()); break;
        }
    }
}

// A hint for the codes seen in practice (names as in the PSP network library's error header) - the port's own strings.
std::string LoginScreen::wifiFailureText() const {
    const std::string ssid = pp::netSsid();
    switch (pp::netLastError()) {
        case 0x80410106:
        case 0x80410107: return OWN_FMT(WIFI_NOT_FOUND, ssid);
        case 0x80410A08: return OWN_FMT(WIFI_REJECTED, ssid);
        case 0x80410A07: return OWN_FMT(WIFI_SIGNAL_LOST, ssid);
        case 0x80410502: return OWN_FMT(WIFI_NO_IP, ssid);
        case 0x80410A06: return OWN_STR(WIFI_SWITCH_OFF);
        default: return std::string("Wi-Fi: ") + pp::netStatus();
    }
}

const char* LoginScreen::stageText() const {
    switch (stage_) {
        case Stage::Idle: return "idle";
        case Stage::NetDialog: return "netconf";
        case Stage::Wifi: return "wifi";
        case Stage::ConnectPending:
        case Stage::Connecting: return "connecting";
        case Stage::Auth: return "auth";
        case Stage::Realms: return "realms";
        case Stage::WorldPending: return "world_pending";
        case Stage::World: return "world";
        case Stage::Failed: return "failed";
    }
    return "?";
}

std::string LoginScreen::realmTitle(const auth::Realm& r) const {
    // CharSelectRealmName: name + " " + the type in parentheses (PvP / RP / RPPvP; nothing for PvE).
    switch (r.icon) {
        case 1: return r.name + " " + GLUE_STR(PVP_PARENTHESES);
        case 6: return r.name + " " + GLUE_STR(RP_PARENTHESES);
        case 8: return r.name + " " + GLUE_STR(RPPVP_PARENTHESES);
        default: return r.name;
    }
}

// The world connection is gone (server kick - the same account logged in
// elsewhere, AzerothCore WorldSessionMgr::AddSession_ - or the network):
// the original goes back to the login screen and shows DISCONNECTED
// (GlueParent.lua DISCONNECTED_FROM_SERVER). The world, the character list and
// the session are torn down here, in update(): frameEnd() has already waited
// for the GE, so nothing still reads the world's textures.
void LoginScreen::leaveWorld() {
    const bool inWorld = worldView_ != nullptr;
    worldView_.reset();
    chars_.reset();
    world_.reset();
    loading_.end();
    transferUnloaded_ = false;  // a transfer the disconnect cut short
    transferPendingShown_ = false;
    if (logSyncUntilMs_) {
        logSyncUntilMs_ = 0;
        pp::logSetSync(false);
    }
    pp::logMemory(inWorld ? "world_left" : "chars_left");
}

// CharacterSelect_Exit - DisconnectFromServer, SetGlueScreen("login").
void LoginScreen::backToLogin() {
    PSP_LOGI(pp::tag::Net, "glue_back_to_login");
    leaveWorld();
    if (auth_) {
        auth_->disconnect();
        auth_.reset();
    }
    realms_.clear();
    prevRealm_ = -1;
    realmsPending_ = false;
    dialog_.hide();
    enterStage(Stage::Idle);
    focus_.setFocus(kIdLogin);
}

// CharacterSelect_ChangeRealm - RequestRealmList. The world connection goes now (PSP: one
// session at a time); the auth session asks for the list again when it is still open, else a new login.
void LoginScreen::changeRealm() {
    prevRealm_ = static_cast<int>(pickedRealm_);
    leaveWorld();
    dialog_.hide();
    const bool authOpen = auth_ && auth_->isConnected() &&
                          (auth_->getState() == auth::AuthState::AUTHENTICATED ||
                           auth_->getState() == auth::AuthState::REALM_LIST_RECEIVED);
    PSP_LOGI(pp::tag::Net, "glue_change_realm auth_open=%d", authOpen ? 1 : 0);
    if (authOpen) {
        realmsPending_ = true;
        enterStage(Stage::Realms);
        auth_->requestRealmList();
    } else {
        if (auth_) {
            auth_->disconnect();
            auth_.reset();
        }
        start();
    }
}

void LoginScreen::onRealmList(const std::vector<auth::Realm>& realms) {
    realms_ = realms;
    realmsPending_ = false;
    PSP_LOGI(pp::tag::Net, "realms=%u worst_update_us=%u", static_cast<unsigned>(realms.size()),
             static_cast<unsigned>(worstUpdateUs_));
    for (const auto& r : realms) {
        PSP_LOGI(pp::tag::Net, "realm id=%u name=%s addr=%s chars=%u flags=0x%02X icon=%u pop=%.2f", r.id,
                 r.name.c_str(), r.address.c_str(), r.characters, static_cast<unsigned>(r.flags),
                 static_cast<unsigned>(r.icon), static_cast<double>(r.population));
    }
    enterStage(Stage::Realms);
    // RealmList.currentRealm: the realm we came from, else the first one up.
    realmSel_ = -1;
    if (prevRealm_ >= 0 && prevRealm_ < static_cast<int>(realms_.size())) realmSel_ = prevRealm_;
    for (int i = 0; realmSel_ < 0 && i < static_cast<int>(realms_.size()); ++i)
        if (!realmLoad(realms_[i]).down) realmSel_ = i;
    realmScroll_ = std::max(0, realmSel_ - kRealmRows + 1);
    if (realmSel_ >= 0) focus_.setFocus(kIdRealm0 + static_cast<uint32_t>(realmSel_));
    else focus_.setFocus(kIdRealmCancel);
    if (autoRealm_ && !realms_.empty()) {  // emulator smoke test: go on to the characters
        autoRealm_ = false;
        pickedRealm_ = 0;
        enterStage(Stage::WorldPending);
    }
}

// RealmList_OnOk: ChangeRealm(category, currentRealm).
void LoginScreen::realmOk() {
    if (realmSel_ < 0 || realmSel_ >= static_cast<int>(realms_.size()) || realmLoad(realms_[realmSel_]).down) return;
    pickedRealm_ = static_cast<size_t>(realmSel_);
    const auth::Realm& r = realms_[pickedRealm_];
    PSP_LOGI(pp::tag::Net, "realm_selected id=%u addr=%s", r.id, r.address.c_str());
    enterStage(Stage::WorldPending);
}

// RealmList_OnCancel: hidden; back to the realm CharacterSelect was on, or (from the login) to the login.
void LoginScreen::realmCancel() {
    if (prevRealm_ >= 0 && prevRealm_ < static_cast<int>(realms_.size()) && auth_) {
        pickedRealm_ = static_cast<size_t>(prevRealm_);
        enterStage(Stage::WorldPending);
    } else {
        backToLogin();
    }
}

// Every 16th pak read of the world load (the long steps: on a busy emulator host the doodads alone took
// 9 minutes of real time, 0.3 s of its clock) sends a keep-alive too. Any thread: keepAlive() only sends.
void LoginScreen::loadingReadTick(void* self) {
    auto* s = static_cast<LoginScreen*>(self);
    if ((++s->loadReads_ & 15u) == 0 && s->world_ && s->world_->keepAlive()) ++s->loadKeepAlives_;
}

void LoginScreen::loadingHook(void* self, float frac) {
    auto* s = static_cast<LoginScreen*>(self);
    s->loading_.setProgress(frac);
    if (s->world_ && s->world_->keepAlive()) ++s->loadKeepAlives_;  // the server hears us while we load
    // A frame of the loading screen between the world's load stages (the GE is idle here: update()
    // runs between frames). At most ~15 a second: each one waits for a vblank.
    const uint32_t now = pp::logUptimeMs();
    if (!s->ui_ || (frac < 1.0f && now - s->lastLoadingDrawMs_ < 66u)) return;
    s->lastLoadingDrawMs_ = now;
    pp::frameBegin(0xFF000000u);
    s->ui_->beginFrame();
    s->loading_.draw(*s->ui_);
    s->ui_->endFrame();
    pp::frameEnd(nullptr, nullptr);
}

// The world is ready on the server: build the world screen under the loading screen.
void LoginScreen::enterWorldScreen() {
    // World entry and its first 20 s write every log line through (the
    // buffered log lost the tail of a hardware crash).
    pp::logSetSync(true);
    PSP_LOGI(pp::tag::Loop, "log_sync on (world entry, 20 s)");
    // The loading screen of the map the server put us on (normally the one shown since
    // ENTER_WORLD); the glue screens' art and model go first (heap).
    chars_->releaseForWorld();
    art_.release();
    loading_.begin(world_->worldEntry().mapId);
    loading_.setProgress(0.0f);
    lastLoadingDrawMs_ = 0;
    loadKeepAlives_ = 0;
    loadReads_ = 0;
    loadingSetHook(&LoginScreen::loadingHook, this);
    pp::pakSetReadTick(&LoginScreen::loadingReadTick, this);
    const uint64_t t0 = nowUs();
    worldView_ = std::make_unique<WorldScreen>(kit_, *world_, chars_->selectedName());
    loadingStep(1.0f);
    pp::pakSetReadTick(nullptr, nullptr);
    loadingSetHook(nullptr, nullptr);
    // No black screen between the loading screen and the world: the loading screen stays over
    // the world's first frames (terrain, NPC and texture uploads: 95 ms frames on hardware 99) - until the
    // frames settle (update()), not ended here.
    loadingHoldStartMs_ = pp::logUptimeMs();
    loadingHoldFrames_ = 0;
    PSP_LOGI(pp::tag::Loop, "screen=world load_ms=%u keepalive=%u", static_cast<unsigned>((nowUs() - t0) / 1000u),
             static_cast<unsigned>(loadKeepAlives_));
    pp::logMemory("loading_end");
    logSyncUntilMs_ = pp::logUptimeMs() + 20000u;
}

// --- map transfer ----------------------------------------------------------------------------------------------

namespace {
uint32_t heapUsedKb() { return pp::heapUsedKb(); }
}  // namespace

// SMSG_TRANSFER_PENDING (the original client's loading screen comes up with it): that map's loading screen over the
// world, which runs on without input until SMSG_NEW_WORLD (beginTransfer) or SMSG_TRANSFER_ABORTED / a timeout /
// a same-map move (the stage goes back to None: the screen goes, the world is there again).
void LoginScreen::updateTransferPending() {
    const auto st = world_->transferStage();
    if (st == pp::WorldSession::TransferStage::Pending && !transferPendingShown_) {
        transferPendingShown_ = true;
        loading_.begin(world_->transferMapId());
        loading_.setProgress(0.0f);
        PSP_LOGI(pp::tag::Loop, "transfer_pending_screen map=%u", static_cast<unsigned>(world_->transferMapId()));
    } else if (st == pp::WorldSession::TransferStage::None && transferPendingShown_) {
        transferPendingShown_ = false;
        loading_.end();
        PSP_LOGI(pp::tag::Loop, "transfer_pending_end (aborted or same map)");
    }
}

// SMSG_NEW_WORLD to another map, acknowledged: the world goes - the WorldScreen and everything it holds (terrain,
// doodads, WMO groups, their textures and collision, creatures, players, the character, sounds, the streamer's and
// the minimap's threads; the eDRAM copies are reset by the next world's models). Upstream WorldLoader clears its
// renderers the same way before it loads the next map. Here, in update(): frameEnd has already waited for the GE.
void LoginScreen::beginTransfer() {
    transferFrom_ = world_->worldEntry().mapId;
    transferTo_ = world_->transferMapId();
    transferStartMs_ = pp::logUptimeMs();
    pp::logSetSync(true);  // as at world entry: the transfer's lines reach the card even if it dies in it
    logSyncUntilMs_ = 0;
    PSP_LOGI(pp::tag::Loop, "transfer_begin from=%u to=%u transport=%d", static_cast<unsigned>(transferFrom_),
             static_cast<unsigned>(transferTo_), world_->transferOnTransport() ? 1 : 0);
    transferBeforeKb_ = heapUsedKb();
    pp::logMemory("transfer_begin");
    transferPendingShown_ = false;
    loading_.begin(transferTo_);  // nothing new when the pending screen shows this map already
    loading_.setProgress(0.0f);
    lastLoadingDrawMs_ = 0;
    loadingHook(this, 0.0f);  // its first frame now: the unload takes a moment
    const uint32_t heapBefore = heapUsedKb();
    const pp::WorldArenaInfo a0 = pp::worldArenaInfo();
    const uint32_t t0 = pp::logUptimeMs();
    worldView_.reset();
    transferUnloadedKb_ = heapUsedKb();
    pp::logMemory("transfer_unloaded");
    transferUnloaded_ = true;
    // world_flush: the world went with its screen - the terrain pool and the world
    // arena must be empty now (their owners all freed); checked, reset, and logged as the teleport's flush.
    uint32_t pt = 0, pl = 0, pr = 0;
    rendering::psp::TerrainRenderer::poolUsage(pt, pl, pr);
    const uint32_t poolLost = rendering::psp::TerrainRenderer::poolReset();
    char leak[160];
    const uint32_t live = pp::worldArenaReset(leak, sizeof(leak));
    const pp::WorldArenaInfo a1 = pp::worldArenaInfo();
    PSP_LOGI(pp::tag::Mem, "world_flush why=map dist=0 ms=%u freed_kb=%u arena_used_before=%u arena_used_after=%u",
             static_cast<unsigned>(pp::logUptimeMs() - t0),
             static_cast<unsigned>(heapBefore > transferUnloadedKb_ ? heapBefore - transferUnloadedKb_ : 0u),
             static_cast<unsigned>(a0.usedKb), static_cast<unsigned>(a1.usedKb));
    PSP_LOGI(pp::tag::Mem, "world_flush_heap heap_used_before=%u heap_used_after=%u largest_after=%u free_kb=%u pool_lost=%u",
             static_cast<unsigned>(heapBefore), static_cast<unsigned>(transferUnloadedKb_),
             static_cast<unsigned>(pp::heapLargestKb()), static_cast<unsigned>(pp::heapFreeKb()),
             static_cast<unsigned>(poolLost));
    if (poolLost)
        PSP_LOGW(pp::tag::Mem, "world_flush_leak pool tex=%u tiles=%u runs=%u (slots reset)", static_cast<unsigned>(pt),
                 static_cast<unsigned>(pl), static_cast<unsigned>(pr));
    if (live)
        PSP_LOGW(pp::tag::Mem, "world_flush_leak arena_blocks=%u used_kb=%u at=%s", static_cast<unsigned>(live),
                 static_cast<unsigned>(a1.usedKb), leak);
}

// The session is serviced (packets, ping) with no world; then the next map's world is built as at world entry.
void LoginScreen::updateTransfer(float dtSec) {
    world_->update();
    world_->updateEntities(dtSec);
    if (world_->state() != pp::WorldSession::State::InWorld) return;  // a lost link: the world_lost path next frame
    const uint32_t now = pp::logUptimeMs();
    // A ship / zeppelin: SMSG_NEW_WORLD's place was transport-local - our own create block has the world one.
    if (world_->transferAwaitingSelf() && now - transferStartMs_ < 3000u) return;
    world_->commitTransfer();
    loading_.begin(world_->worldEntry().mapId);
    loading_.setProgress(0.0f);
    lastLoadingDrawMs_ = 0;
    loadKeepAlives_ = 0;  // keep-alives as at world entry
    loadReads_ = 0;
    loadingSetHook(&LoginScreen::loadingHook, this);
    pp::pakSetReadTick(&LoginScreen::loadingReadTick, this);
    const uint64_t t0 = nowUs();
    worldView_ = std::make_unique<WorldScreen>(kit_, *world_, chars_ ? chars_->selectedName() : std::string());
    loadingStep(1.0f);
    pp::pakSetReadTick(nullptr, nullptr);
    loadingSetHook(nullptr, nullptr);
    loadingHoldStartMs_ = pp::logUptimeMs();  // the warm-up hold, as after world entry
    loadingHoldFrames_ = 0;
    transferUnloaded_ = false;
    const uint32_t afterKb = heapUsedKb();
    pp::logMemory("transfer_end");
    PSP_LOGI(pp::tag::Mem,
             "transfer map=%u->%u ms=%u build_ms=%u heap_kb begin=%u unloaded=%u end=%u freed=%d keepalive=%u",
             static_cast<unsigned>(transferFrom_), static_cast<unsigned>(world_->worldEntry().mapId),
             static_cast<unsigned>(pp::logUptimeMs() - transferStartMs_),
             static_cast<unsigned>((nowUs() - t0) / 1000u), static_cast<unsigned>(transferBeforeKb_),
             static_cast<unsigned>(transferUnloadedKb_), static_cast<unsigned>(afterKb),
             static_cast<int>(transferBeforeKb_) - static_cast<int>(transferUnloadedKb_),
             static_cast<unsigned>(loadKeepAlives_));
    logSyncUntilMs_ = pp::logUptimeMs() + 20000u;
}

void LoginScreen::update(const pp::PadState& pad, float dtSec) {
    nowMs_ = pp::logUptimeMs();
    // with the CVar "movie" set (wowee.cfg movie, 1 until the intro played) the client shows the
    // "movie" glue screen first - MovieFrame with the client's expansion level, the Wrath intro - then the login
    // screen (MovieFrame_OnHide). Once the first frame was drawn (the movie draws through ui_); not for autologin.
    if (!introChecked_ && ui_ && stage_ == Stage::Idle) {
        introChecked_ = true;
        if (!settings_.autoLogin && MovieFrame::introWanted()) {
            if (MovieFrame::glueAvailable(kClientExpansionLevel)) {
                movies_.playGlue(*ui_, kClientExpansionLevel);
                MovieFrame::introPlayed();
            } else {
                PSP_LOGI(pp::tag::Loop, "movie_intro skipped (no movie of MovieList[%d] in data/movies)",
                         kClientExpansionLevel);
            }
            return;
        }
    }
    if (stage_ == Stage::World && world_ && world_->state() == pp::WorldSession::State::Failed) {
        // CHAR_LOGIN_FAILED when the server refused the character, DISCONNECTED otherwise
        // (the session's own text goes to the log).
        const bool entering = !worldView_ && chars_ && chars_->enteringWorld();
        const std::string why = world_->failure();
        PSP_LOGW(pp::tag::Net, "world_lost in_world=%d entering=%d reason=%s", worldView_ ? 1 : 0, entering ? 1 : 0,
                 why.c_str());
        leaveWorld();
        fail(entering ? GLUE_STR(CHAR_LOGIN_FAILED) : (why.empty() ? GLUE_STR(DISCONNECTED) : why));
        return;
    }
    if (stage_ == Stage::World && worldView_) {
        // wowee.cfg logsync=1 keeps every line written through (a hardware hang keeps its tail).
        if (logSyncUntilMs_ && nowMs_ > logSyncUntilMs_ && !pp::configGetInt("logsync", 0)) {  // back to the buffered log
            logSyncUntilMs_ = 0;
            PSP_LOGI(pp::tag::Loop, "log_sync off");
            pp::logSetSync(false);
        }
        updateTransferPending();  // SMSG_TRANSFER_PENDING - the next map's loading screen at once
        if (loading_.active() && !transferPendingShown_) {
            // The world warms up under the loading screen (no input), at least 1 s, at most 8 s.
            // (On hardware it held 6.3 s - vsync makes frames 33 or 50 ms, "20 in a row under 45 ms"
            // kept breaking at ~28 fps): until the last 16 frames average under 40 ms (25 fps).
            loadingHoldDt_[loadingHoldFrames_ % kLoadingHoldWindow] = dtSec;
            ++loadingHoldFrames_;
            float avg = 1.0f;
            if (loadingHoldFrames_ >= kLoadingHoldWindow) {
                avg = 0.0f;
                for (float d : loadingHoldDt_) avg += d;
                avg /= static_cast<float>(kLoadingHoldWindow);
            }
            const bool settled = avg < 0.040f;
            const uint32_t held = nowMs_ - loadingHoldStartMs_;
            if ((held >= 1000u && settled) || held >= 8000u) {
                loading_.end();
                pp::inputScriptSignal("world");  // the autopad test script (psp_input.hpp)
                PSP_LOGI(pp::tag::Loop, "loading_hold ms=%u frames=%u settled=%d avg_ms=%.1f", static_cast<unsigned>(held),
                         static_cast<unsigned>(loadingHoldFrames_), settled ? 1 : 0, static_cast<double>(avg * 1000.0f));
            }
            worldView_->update(pp::PadState{}, dtSec);
            if (world_->transferStage() == pp::WorldSession::TransferStage::Ready) beginTransfer();
            return;
        }
        // no input under a pending transfer's loading screen (the world goes on).
        worldView_->update(transferPendingShown_ ? pp::PadState{} : pad, dtSec);
        updateGameMovies();  // PLAY_MOVIE / CINEMATIC_START the session took this frame
        if (world_->transferStage() == pp::WorldSession::TransferStage::Ready) beginTransfer();
        return;
    }
    // the old world is gone - the session is serviced until the next map's world is built.
    if (stage_ == Stage::World && transferUnloaded_ && world_) {
        updateTransfer(dtSec);
        return;
    }
    if (stage_ == Stage::World && chars_ && world_ && world_->state() == pp::WorldSession::State::InWorld) {
        // sent to another map before the first world was built (e.g. out of a reset instance
        // right after the login) - that map is the one to build.
        if (world_->transferStage() == pp::WorldSession::TransferStage::Ready) world_->commitTransfer();
        enterWorldScreen();
        return;
    }
    if (stage_ == Stage::World && chars_) {
        chars_->update(chars_->enteringWorld() ? pp::PadState{} : pad, dtSec);
        if (chars_->enteringWorld() && !loading_.active()) {
            // ENTER_WORLD: the loading screen from now on (the map from the character list).
            chars_->releaseForWorld();
            art_.release();
            loading_.begin(chars_->enteringMapId());
        }
        if (chars_->wantsExit()) wantsExit_ = true;
        if (chars_->wantsBack()) {
            backToLogin();
            return;
        }
        if (chars_->wantsRealmList()) {
            changeRealm();
            return;
        }
        if (auth_) auth_->update(dtSec);  // keep the auth link serviced; it closes on its own
        return;
    }
    // PSP: the account / password keyboard owns the pad while it is open.
    if (oskField_) {
        if (pp::oskActive()) return;
        const int field = oskField_;
        oskField_ = 0;
        bool accepted = false;
        std::string typed;
        if (pp::oskFinished(accepted, typed)) fieldTyped(field, accepted, typed);
        return;
    }
    // PSP: the system connection dialog owns the pad while it is open.
    if (stage_ == Stage::NetDialog) {
        int result = 0;
        if (pp::netconfActive() || !pp::netconfFinished(result)) return;
        const bool connected = pp::netApConnected();
        PSP_LOGI(pp::tag::Net, "netconf_done result=%d connected=%d", result, connected ? 1 : 0);
        if (connected) {
            pp::netAdoptConnection("netconf");
            PSP_LOGI(pp::tag::Net, "netconf_connected ip=%s ssid=%s", pp::netIp(), pp::netSsid());
            enterStage(Stage::ConnectPending);
        } else if (result == 1 || result == 2) {  // the player backed out (cancelled / aborted)
            fail(OWN_STR(WIFI_NOT_CHOSEN));
        } else {
            char code[16];
            std::snprintf(code, sizeof(code), "%d", result);
            if (result < 0) std::snprintf(code, sizeof(code), "%08X", static_cast<unsigned>(result));
            fail(OWN_FMT(WIFI_DIALOG_FAILED, std::string(code)));
        }
        return;
    }
    // On the realm list UP / DOWN walk the rows (consumed here, before the focus ring).
    focus_.update(stage_ == Stage::Realms && !dialog_.shown() ? updateRealmList(pad) : pad, dtSec);

    if (settings_.autoLogin && stage_ == Stage::Idle) {
        settings_.autoLogin = false;
        autoRealm_ = true;
        autoCharCreate_ = true;
        PSP_LOGI(pp::tag::Net, "autologin=1");
        start();
    }

    if (stage_ == Stage::Failed && auth_) {
        auth_->disconnect();
        auth_.reset();
    }

    switch (stage_) {
        case Stage::Wifi:
            pp::netPoll();
            if (pp::netState() == pp::NetState::Connected) enterStage(Stage::ConnectPending);
            else if (pp::netState() == pp::NetState::Failed) fail(wifiFailureText());
            break;

        case Stage::ConnectPending:
            // One frame shows "connecting" before the blocking connect (up to 5 s).
            enterStage(Stage::Connecting);
            break;

        case Stage::Connecting: {
            auth_ = std::make_unique<auth::AuthHandler>();
            auth::ClientInfo info;  // 3.3.5a build 12340, protocol 8
            info.locale = settings_.locale;
            auth_->setClientInfo(info);
            auth_->setOnSuccess([this](const std::vector<uint8_t>& key) {
                PSP_LOGI(pp::tag::Net, "auth_ok session_key_bytes=%u ms=%u", static_cast<unsigned>(key.size()),
                         static_cast<unsigned>((nowUs() - stageStartUs_) / 1000u));
                saveLogin();
                auth_->requestRealmList();
            });
            auth_->setOnFailure([this](const std::string& reason) {
                if (stage_ == Stage::World) {  // the world session goes on without the auth link
                    PSP_LOGW(pp::tag::Net, "auth_lost_in_world reason=%s", reason.c_str());
                    return;
                }
                // PSP: the server refused the account or the password - the saved login goes.
                if (reason.find(auth::getAuthResultString(auth::AuthResult::PASSWORD_INVALID)) != std::string::npos ||
                    reason.find(auth::getAuthResultString(auth::AuthResult::ACCOUNT_INVALID)) != std::string::npos)
                    forgetLogin();
                fail(loginError(reason));  // upstream's English reason -> LOGIN_*
            });
            auth_->setOnRealmList([this](const std::vector<auth::Realm>& realms) { onRealmList(realms); });
            uint64_t t0 = nowUs();
            bool ok = auth_->connect(settings_.server, settings_.port);
            PSP_LOGI(pp::tag::Net, "tcp_connect host=%s port=%u ok=%d ms=%u", settings_.server.c_str(),
                     static_cast<unsigned>(settings_.port), ok ? 1 : 0,
                     static_cast<unsigned>((nowUs() - t0) / 1000u));
            if (!ok) {
                fail(GLUE_STR(LOGIN_SERVER_DOWN));
                break;
            }
            // PSP (public safe): the logon server's address (the one connected to) - not a dev server: the
            // debug / automation aids are off for this run; autologin's automatic realm, character and world steps
            // too (autologin itself only pressed the login button with the saved account).
            pp::publicSafeOnConnect("logon", settings_.server.c_str());
            if (!pp::devToolsAllowed() && (autoRealm_ || autoCharCreate_)) {
                PSP_LOGW(pp::tag::Net, "autologin_steps_refused public=1");
                autoRealm_ = false;
                autoCharCreate_ = false;
            }
            enterStage(Stage::Auth);
            // PSP: a typed (or wowee.cfg) password, else the saved credential hash.
            if (!settings_.password.empty()) auth_->authenticate(settings_.account, settings_.password);
            else auth_->authenticateWithHash(settings_.account, settings_.authHash);
            break;
        }

        case Stage::Auth:
        case Stage::Realms:
            if (auth_) {
                uint64_t t0 = nowUs();
                auth_->update(dtSec);
                uint32_t us = static_cast<uint32_t>(nowUs() - t0);
                if (us > worstUpdateUs_) worstUpdateUs_ = us;
            }
            if (stage_ == Stage::Auth && nowUs() - stageStartUs_ > 20ull * 1000 * 1000) fail(OWN_STR(AUTH_TIMEOUT));
            // The list asked from CharacterSelect did not come (the auth server closed the link):
            // log in again.
            if (stage_ == Stage::Realms && realmsPending_ && nowUs() - stageStartUs_ > 5ull * 1000 * 1000) {
                PSP_LOGW(pp::tag::Net, "realm_list_timeout relogin");
                if (auth_) {
                    auth_->disconnect();
                    auth_.reset();
                }
                start();
            }
            break;

        case Stage::WorldPending: {
            // One frame shows "connecting" before the blocking world connect.
            const auth::Realm& r = realms_[pickedRealm_];
            std::string host = r.address;
            uint16_t port = 8085;
            if (size_t colon = host.rfind(':'); colon != std::string::npos) {
                port = static_cast<uint16_t>(std::atoi(host.c_str() + colon + 1));
                host.resize(colon);
            }
            std::vector<uint8_t> key = auth_ ? auth_->getSessionKey() : std::vector<uint8_t>{};
            chars_.reset();
            dialog_.hide();
            world_ = std::make_unique<pp::WorldSession>();
            if (!world_->begin(host, port, settings_.account, key, r.id)) {
                fail(world_->failure());
                break;
            }
            prevRealm_ = static_cast<int>(pickedRealm_);
            // PSP: WorldSession::begin judged the realm's address - automatic character steps only on a dev one.
            if (autoCharCreate_ && !pp::devToolsAllowed()) autoCharCreate_ = false;
            chars_ = std::make_unique<CharacterScreen>(kit_, *world_, realmTitle(r), autoCharCreate_, settings_.autoOsk);
            enterStage(Stage::World);
            break;
        }
        default:
            break;
    }
    if (stage_ == Stage::World) return;

    const uint32_t act = focus_.activated();
    // CinematicsFrame is modal (DIALOG strata, it takes the keys): a button plays that expansion's
    // movies (Cinematics_PlayMovie -> MovieFrame, then SetGlueScreen("login")), any other key closes it
    // (CinematicsFrame_OnKeyDown) - PSP: Circle or START.
    if (cinematics_.shown()) {
        if (const int version = cinematics_.pressed(act)) {
            cinematics_.hide();
            PSP_LOGI(pp::tag::Loop, "ui_activate id=0x%X (cinematics) version=%d", static_cast<unsigned>(act), version);
            if (ui_) movies_.playGlue(*ui_, version);
            focus_.setFocus(kIdCinematicsButton);
        } else if (pad.pressed & (PSP_CTRL_CIRCLE | PSP_CTRL_START)) {
            cinematics_.hide();
            focus_.setFocus(kIdCinematicsButton);
        }
        return;
    }
    // LanguageFrame is modal the same way - a language button picks it, Circle or START closes.
    if (language_.shown()) {
        if (const int k = language_.pressed(act)) {
            language_.hide();
            focus_.setFocus(kIdLanguageButton);
            PSP_LOGI(pp::tag::Loop, "ui_activate id=0x%X (language)", static_cast<unsigned>(act));
            pickLanguage(pp::localeInstalled()[static_cast<size_t>(k - 1)]);
        } else if (pad.pressed & (PSP_CTRL_CIRCLE | PSP_CTRL_START)) {
            language_.hide();
            focus_.setFocus(kIdLanguageButton);
        }
        return;
    }
    // A dialog is modal - only its buttons act.
    if (dialog_.shown()) {
        const uint32_t b = dialog_.pressed(act);
        if (!b && !(pad.pressed & PSP_CTRL_START)) return;
        PSP_LOGI(pp::tag::Loop, "ui_activate id=%u (dialog)", static_cast<unsigned>(b));
        dialog_.hide();
        if (localePrompt_) {  // RESTART ends the run (main() starts the EBOOT again), OKAY / START = later
            localePrompt_ = false;
            focus_.setFocus(kIdLanguageButton);
            if (b == kGlueDlgButton1) {
                pp::localeRequestRestart();
                wantsExit_ = true;
            }
            return;
        }
        if (stage_ == Stage::Failed) {  // OKAY on an error: back to the login
            enterStage(Stage::Idle);
            focus_.setFocus(kIdLogin);
        } else if (stage_ == Stage::Realms && realmsPending_) {  // CANCEL on REALM_LIST_IN_PROGRESS
            realmsPending_ = false;
            realmCancel();
        } else if (stage_ != Stage::Idle && stage_ != Stage::Realms) {  // CANCEL on the status: disconnect
            PSP_LOGI(pp::tag::Net, "login_cancel stage=%s", stageText());
            backToLogin();
        }
        return;
    }
    if (stage_ == Stage::Realms) {
        if ((pad.pressed & PSP_CTRL_START)) {  // RealmList_OnKeyDown ESCAPE
            realmCancel();
            return;
        }
        if (!act) return;
        PSP_LOGI(pp::tag::Loop, "ui_activate id=%u", static_cast<unsigned>(act));
        if (act == kIdRealmOk) {
            realmOk();
        } else if (act == kIdRealmCancel) {
            realmCancel();
        } else if (act >= kIdRealm0 && act < kIdRealm0 + realms_.size()) {
            const int i = static_cast<int>(act - kIdRealm0);
            if (realmLoad(realms_[i]).down) return;
            if (i == realmSel_) realmOk();  // RealmSelectButton_OnDoubleClick
            else realmSel_ = i;             // RealmSelectButton_OnClick
        }
        return;
    }
    if (!act) return;
    PSP_LOGI(pp::tag::Loop, "ui_activate id=%u", static_cast<unsigned>(act));
    if (act == kIdExit) {
        wantsExit_ = true;
    } else if (act == kIdLogin || act == kIdRetry) {
        start();
    } else if (act == kIdAccountBox || act == kIdPasswordBox) {
        openField(act == kIdAccountBox ? 1 : 2);
    } else if (act == kIdCinematicsButton) {
        // AccountLogin_Cinematics - CinematicsFrame (numMovies = GetClientExpansionLevel() = 3 > 1).
        cinematics_.show(focus_);
    } else if (act == kIdLanguageButton && languageChoiceOffered()) {
        language_.show(focus_);
    }
}

// PSP: the account / password boxes - the system keyboard (the PSP has no other text input). The
// password is never shown back: its keyboard opens empty.
void LoginScreen::openField(int field) {
    const bool account = field == 1;
    if (!pp::oskBegin(account ? GLUE_STR(ACCOUNT_NAME) : GLUE_STR(PASSWORD), account ? settings_.account : std::string(),
                      account ? 32 : 16)) {
        dialog_.show(OWN_STR(PSP_KEYBOARD_FAILED), GLUE_STR(OKAY));
        return;
    }
    oskField_ = field;
    PSP_LOGI(pp::tag::Loop, "login_field_osk field=%s", account ? "account" : "password");
}

void LoginScreen::fieldTyped(int field, bool accepted, std::string& typed) {
    if (!accepted || typed.empty()) {
        wipe(typed);
        return;
    }
    if (field == 1) {
        if (typed != settings_.account) {  // another account: its password is typed next
            settings_.account = typed;
            wipe(settings_.password);
            settings_.authHash.clear();
            loginSaved_ = false;
        }
        PSP_LOGI(pp::tag::Loop, "login_account_typed len=%u", static_cast<unsigned>(typed.size()));
        if (settings_.password.empty() && settings_.authHash.size() != 20) {
            openField(2);
        } else {
            focus_.setFocus(kIdLogin);
        }
    } else {
        wipe(settings_.password);
        settings_.password = typed;
        settings_.authHash.clear();
        loginSaved_ = false;
        PSP_LOGI(pp::tag::Loop, "login_password_typed");
        focus_.setFocus(kIdLogin);
    }
    wipe(typed);
}

// The logon proof was accepted: the account goes to ms0:/WOWEE/account.dat (outside the game folder) as the SRP
// credential hash, the typed password is wiped, and wowee.cfg's account / password lines are erased (and from its
// .bak) - a shared game folder carries no account.
void LoginScreen::saveLogin() {
    if (loginSaved_) return;
    loginSaved_ = true;
    if (!settings_.password.empty()) {
        settings_.authHash = pp::loginAuthHash(settings_.account, settings_.password);
        wipe(settings_.password);
    }
    pp::StoredLogin login{settings_.account, settings_.authHash};
    const bool ok = pp::loginStorePut(settings_.server, login);
    if (ok && settings_.credentialsInCfg) {
        pp::configErase("account");
        pp::configErase("password");
        int written = 0;
        const bool cfgOk = pp::configSave(&written);
        settings_.credentialsInCfg = !cfgOk;
        PSP_LOGI(pp::tag::Fs, "login_cfg_moved ok=%d keys=%d", cfgOk ? 1 : 0, written);
    }
}

// The server refused the account or the password: a saved login that was used goes (a mistyped password leaves the
// saved one alone - typing it already set the hash aside for this run).
void LoginScreen::forgetLogin() {
    if (settings_.password.empty() && settings_.authHash.size() == 20) {
        pp::loginStoreForget(settings_.server);
        settings_.authHash.clear();
    }
    wipe(settings_.password);
    loginSaved_ = false;
}

// The original's LocaleDropDown SetValue - SetCVar("locale") (wowee.cfg locale=, written now: the
// same writer as the settings menu, every other line kept), then CLIENT_RESTART_ALERT's place: the notice with
// RESTART (now) / OKAY (at the next start). The running language picked again: only the file changes back.
void LoginScreen::pickLanguage(const std::string& code) {
    const bool restart = pp::localePick(code);
    int written = 0;
    const bool ok = pp::configSave(&written);
    PSP_LOGI(pp::tag::Fs, "language_save code=%s ok=%d keys=%d restart=%d", code.c_str(), ok ? 1 : 0, written,
             restart ? 1 : 0);
    if (!restart) return;
    const Font& fn = kit_.font(kFontNormal);
    localePrompt_ = true;
    dialog_.show(languageRestartNotice(fn, code), languageText(fn, pp::locale::Str::Restart, code),
                 languageText(fn, pp::locale::Str::Okay, code));
    dialog_.setOpaque(true);  // the list's look (nothing of the screen through the dialog)
}

// the world's PLAY_MOVIE (FrameXML MovieFrame: WorldFrame and UIParent hidden, the movie, then
// GameMovieFinished whatever the end) and CINEMATIC_START (no flyby on the PSP yet: the camera "began" and was
// stopped at once - the original when ESCAPE skips it).
void LoginScreen::updateGameMovies() {
    if (!world_ || !ui_ || !worldView_) return;
    uint32_t id = 0;
    if (world_->takeCinematic(id)) {
        PSP_LOGI(pp::tag::Loop, "cinematic_skipped sequence=%u (no flyby on the PSP yet)", static_cast<unsigned>(id));
        world_->cinematicCameraStarted();
        world_->cinematicFinished();
    }
    if (!world_->takeMovie(id)) return;
    pp::sfxStopAll();  // the world is frozen under the movie: no sound of it goes on
    moviePumpUs_ = movieKeepAliveUs_ = nowUs();
    movieKeepAlives_ = 0;
    const pp::MovieEnd end = movies_.playGame(*ui_, id, &LoginScreen::moviePump, this);
    if (world_) world_->movieFinished();
    PSP_LOGI(pp::tag::Loop, "movie_game id=%u end=%s keepalive=%u", static_cast<unsigned>(id), pp::movieEndName(end),
             static_cast<unsigned>(movieKeepAlives_));
}

// Every pass of the movie: the world session goes on (packets, time sync, entities) and the server hears us
// (AzerothCore drops a session silent for 60 s; CMSG_PING does not count) - a keep-alive every 10 s.
bool LoginScreen::moviePump(void* self) {
    auto* s = static_cast<LoginScreen*>(self);
    if (!s->world_) return false;
    const uint64_t now = nowUs();
    const float dt = std::min(0.1f, static_cast<float>(now - s->moviePumpUs_) / 1e6f);
    s->moviePumpUs_ = now;
    s->world_->update();
    s->world_->updateEntities(dt);
    if (now - s->movieKeepAliveUs_ >= 10ull * 1000 * 1000) {
        s->movieKeepAliveUs_ = now;
        if (s->world_->keepAlive()) ++s->movieKeepAlives_;
    }
    return s->world_->state() != pp::WorldSession::State::Failed;
}

// UP / DOWN walk the realm rows and scroll them (the ScrollFrame), like CharacterSelect;
// returns the pad without the press it used (off the list's ends the focus ring moves as usual).
pp::PadState LoginScreen::updateRealmList(const pp::PadState& pad) {
    pp::PadState out = pad;
    const uint32_t f = focus_.focused();
    const int n = static_cast<int>(realms_.size());
    if (f < kIdRealm0 || f >= kIdRealm0 + static_cast<uint32_t>(n)) return out;
    int i = static_cast<int>(f - kIdRealm0);
    int next = i;
    for (int j = i + 1; (pad.pressed & PSP_CTRL_DOWN) && j < n; ++j)  // the next row that can take the focus
        if (!realmLoad(realms_[j]).down) {
            next = j;
            break;
        }
    for (int j = i - 1; (pad.pressed & PSP_CTRL_UP) && j >= 0; --j)
        if (!realmLoad(realms_[j]).down) {
            next = j;
            break;
        }
    if (next == i) return out;
    out.pressed &= ~static_cast<uint32_t>(PSP_CTRL_DOWN | PSP_CTRL_UP);
    if (next < realmScroll_) realmScroll_ = next;
    if (next >= realmScroll_ + kRealmRows) realmScroll_ = next - kRealmRows + 1;
    focus_.setFocus(kIdRealm0 + static_cast<uint32_t>(next));
    return out;
}

void LoginScreen::drawRealmList(UiRenderer& ui, FocusRing& focus) {
    const Font& fs = kit_.font(kFontSmall);
    // RealmList: the screen dimmed (Color a 0.75), RealmListBackground (HelpFrame + header).
    ui.fill(Rect{0, 0, static_cast<float>(pp::kScreenW), static_cast<float>(pp::kScreenH)}, 0xBF000000u);
    const Rect frame{kRlX, kRlY, 456, 268};
    if (art_.rlFrame.pixels) ui.image(art_.rlFrame, frame);
    else kit_.panel(ui, Rect{frame.x, frame.y + 8, frame.w, frame.h - 8});
    // SERVER_SELECTION on the header (GlueFontNormalSmall, TOP 0,-14 of the header).
    ui.textShadow(fs, RX(308), kRlY + 6, GLUE_STR(SERVER_SELECTION), kGlueGold, Align::Center);
    // The sort buttons (column headers): REALM_NAME 223 / REALM_TYPE 80 / REALM_CHARACTERS 100 / REALM_LOAD 138 units.
    const float hy = kRlY + 30;
    const float cName = RX(21), cType = RX(244), cChars = RX(324), cLoad = RX(424), cEnd = RX(562);
    const float colX[5] = {cName, cType, cChars, cLoad, cEnd};
    const char* colText[4] = {GLUE_STR(REALM_NAME), GLUE_STR(REALM_TYPE), GLUE_STR(REALM_CHARACTERS), GLUE_STR(REALM_LOAD)};
    for (int k = 0; k < 4; ++k) {
        const Rect c{colX[k], hy, colX[k + 1] - colX[k] - 1, 12};
        if (art_.colTab.pixels) ui.image(art_.colTab, c, 0xFFFFFFFFu);
        else ui.fill(c, 0x60000000u);
        ui.textShadow(fs, c.x + 5, c.y, colText[k], kGlueGold);
    }
    if (realmsPending_) return;  // REALM_LIST_IN_PROGRESS dialog over it
    // RealmListRealmButton1..18 (PSP: 13 rows) + RealmListScrollFrame.
    const int n = static_cast<int>(realms_.size());
    if (realmScroll_ > std::max(0, n - kRealmRows)) realmScroll_ = std::max(0, n - kRealmRows);
    char buf[48];
    for (int k = 0; k < kRealmRows && realmScroll_ + k < n; ++k) {
        const int i = realmScroll_ + k;
        const auth::Realm& r = realms_[i];
        const RealmLoad load = realmLoad(r);
        const Rect row{cName, kRowsY + k * kRowH, cEnd - cName, kRowH - 1};
        const bool focused = !load.down && focus.addStop(kIdRealm0 + static_cast<uint32_t>(i), row);
        const bool sel = i == realmSel_ && !load.down;
        const bool invalid = (r.flags & 0x01) != 0;
        if (sel) {  // RealmListHighlight (UI-QuestLogTitleHighlight, ADD) tinted as RealmListUpdate does
            const uint32_t tint = invalid ? 0xFF1A1AFFu : (r.characters > 0 ? 0xFF1AFF1Au : 0xFF00C7FFu);
            if (art_.rlHl.pixels) ui.image(art_.rlHl, row, tint, Blend::Add);
            else ui.fill(row, (tint & 0x00FFFFFFu) | 0x40000000u);
        } else if (focused) {
            if (art_.rlHl.pixels) ui.image(art_.rlHl, row, 0x80FFFFFFu, Blend::Add);
            else ui.fill(row, 0x30FFFFFFu);
        }
        // Name: RealmDownNormal grey, RealmInvalidNormal red, RealmCharactersNormal green,
        // RealmNoCharactersNormal gold; GlueFontHighlightLeft (white) when selected / focused.
        uint32_t nameCol = load.down ? kGlueRealmDown : invalid ? kGlueRed : r.characters > 0 ? kGlueGreen : kGlueGold;
        if ((sel || focused) && !load.down) nameCol = invalid ? 0xFF8080FFu : kColWhite;
        ui.pushClip(Rect{cName, row.y, cType - cName - 2, kRowH});
        ui.textShadow(fs, cName + 4, row.y, r.name.c_str(), nameCol);
        ui.popClip();
        const char* type = nullptr;
        uint32_t typeCol = 0;
        realmType(r, type, typeCol);
        ui.textShadow(fs, cType + 4, row.y, type, sel ? kColWhite : typeCol);
        if (r.characters > 0) {
            std::snprintf(buf, sizeof(buf), "(%u)", static_cast<unsigned>(r.characters));
            ui.textShadow(fs, cChars + 4, row.y, buf, kColWhite);
        }
        ui.textShadow(fs, cLoad + 4, row.y, load.text, sel ? kColWhite : load.col);
    }
    if (n > kRealmRows) {
        const float ax = RX(565);
        const pp::Texture& up = realmScroll_ > 0 ? art_.scrUp : art_.scrUpDis;
        const pp::Texture& dn = realmScroll_ + kRealmRows < n ? art_.scrDn : art_.scrDnDis;
        if (up.pixels) ui.image(up, Rect{ax, kRowsY, 14, 14});
        if (dn.pixels) ui.image(dn, Rect{ax, kRowsY + kRealmRows * kRowH - 14, 14, 14});
    }
    // RealmListOkButton / RealmListCancelButton (GlueDialogButtonTemplate, BOTTOMRIGHT -46,13, 125 x 35).
    const float by = kRlY + frame.h - 30;
    const float bw = 125.0f * kRlSX;
    const float cx = RX(596 - 46) - bw;
    const bool canOk = realmSel_ >= 0 && realmSel_ < n && !realmLoad(realms_[realmSel_]).down;
    glueButton(ui, kit_, focus, kIdRealmOk, Rect{cx - bw + 6, by, bw, 20}, GLUE_STR(OKAY), canOk);
    glueButton(ui, kit_, focus, kIdRealmCancel, Rect{cx, by, bw, 20}, GLUE_STR(CANCEL));
}

void LoginScreen::drawLogin(UiRenderer& ui, const FrameStats& stats, FocusRing& focus) {
    const Font& fn = kit_.font(kFontNormal);
    const Font& fs = kit_.font(kFontSmall);
    // AccountLogin (1024 x 768 -> 480 x 272): the logo TOPLEFT, the account (BOTTOM 0,345) and password
    // (BOTTOM 0,275) boxes with their labels, LOGIN (BOTTOM 0,170), QUIT (BOTTOMRIGHT -5,29), the version
    // (BOTTOMLEFT). PSP: the boxes take the system keyboard; the password shows as stars (8 for a
    // saved login).
    // PSP: no glueLogo - the background art (bg_login) carries the logo already.
    const bool busy = stage_ == Stage::NetDialog || stage_ == Stage::Wifi || stage_ == Stage::ConnectPending ||
                      stage_ == Stage::Connecting || stage_ == Stage::Auth;
    const float cx = pp::kScreenW * 0.5f;
    auto box = [&](float y, const char* label, const std::string& text, uint32_t id) {
        ui.textShadow(fn, cx, y, label, kGlueGold, Align::Center);
        const Rect b{cx - 90, y + 17, 180, 22};
        const bool focused = !busy && focus.addStop(id, b);  // PSP: X opens the keyboard
        if (kit_.has(kTexTipBg)) ui.tile(kit_.tex(kTexTipBg), Rect{b.x + 3, b.y + 3, b.w - 6, b.h - 6}, 0xD8171717u);
        else ui.fill(b, 0xD8171717u);
        if (kit_.has(kTexTipBorder)) ui.nineSlice(kit_.tex(kTexTipBorder), b, 1.0f, focused ? kColWhite : 0xFFCCCCCCu);
        if (focused && kit_.has(kTexBtnHl)) ui.nineSlice(kit_.tex(kTexBtnHl), b, 1.0f, 0x80FFFFFFu, Blend::Add);
        ui.pushClip(Rect{b.x + 4, b.y, b.w - 8, b.h});
        ui.text(fn, cx, b.y + (b.h - fn.lineHeight()) * 0.5f, text.c_str(), kColWhite, Align::Center);
        ui.popClip();
    };
    // The picker must not overlap the login texts: under LanguageFrame and the restart notice the
    // AccountLogin panel is hidden - nothing of it shows through the dialog.
    const bool languageUp = language_.shown() || localePrompt_;
    if (!languageUp) {
        const size_t stars = !settings_.password.empty() ? std::min<size_t>(settings_.password.size(), 16)
                             : settings_.authHash.size() == 20 ? 8 : 0;
        box(96, GLUE_STR(ACCOUNT_NAME), settings_.account, kIdAccountBox);
        box(142, GLUE_STR(PASSWORD), std::string(stars, '*'), kIdPasswordBox);
    }
    if (!languageUp) glueButton(ui, kit_, focus, kIdLogin, Rect{cx - 70, 196, 140, 28}, GLUE_STR(LOGIN), !busy);
    glueButton(ui, kit_, focus, kIdExit, Rect{pp::kScreenW - 84.0f, 244, 80, 22}, GLUE_STR(QUIT), true, kFontSmall);
    // AccountLoginCinematicsButton (GlueButtonSmallTemplateBlue) - the original's right-hand column
    // from the bottom: QUIT, the realm name, TOS, CREDITS, CINEMATICS, OPTIONS; the PSP has no TOS / credits /
    // options, so it stands right over the realm name.
    glueButton(ui, kit_, focus, kIdCinematicsButton, Rect{pp::kScreenW - 84.0f, 202, 80, 22}, GLUE_STR(CINEMATICS),
               !busy, kFontSmall);
    // "Language" on top of the column (the original's OPTIONS place) when the card has more than one
    // language - always in English (found by anyone, whatever language is running).
    if (languageChoiceOffered())
        glueButton(ui, kit_, focus, kIdLanguageButton, Rect{pp::kScreenW - 84.0f, 176, 80, 22},
                   pp::locale::text(pp::locale::Str::Language, "enUS"), !busy, kFontSmall);
    // AccountLoginRealmName (GlueFontDisableSmall, above the right-hand buttons): the server of wowee.cfg.
    ui.textShadow(fs, pp::kScreenW - 6.0f, 228, settings_.server.c_str(), kGlueDisable, Align::Right);
    glueVersionText(ui, kit_, stats.build);
}

void LoginScreen::draw(UiRenderer& ui, const FrameStats& stats) {
    ui_ = &ui;
    if (stage_ == Stage::World && worldView_) {
        worldView_->draw(ui, stats);
        if (loading_.active()) {  // over the warming world
            ui.beginFrame();
            loading_.draw(ui);
            ui.endFrame();
        }
        return;
    }
    if (stage_ == Stage::World && loading_.active()) {  // ENTER_WORLD .. the world
        ui.beginFrame();
        loading_.draw(ui);
        ui.endFrame();
        return;
    }
    if (stage_ == Stage::World && chars_) {
        chars_->draw(ui, stats);
        return;
    }
    ui.beginFrame();
    kit_.background(ui, false);
    art_.load();
    if (pp::oskActive() || stage_ == Stage::NetDialog) {
        // PSP: the system keyboard / connection dialog draws over this frame - a plain dimmed backdrop under
        // it (as the character screen does), not the login boxes.
        ui.fill(Rect{0, 0, static_cast<float>(pp::kScreenW), static_cast<float>(pp::kScreenH)}, 0x80000000u);
        ui.endFrame();
        return;
    }

    // Status ("CANCEL") dialogs while connecting: LOGIN_STATE_*; Wi-Fi is the PSP's own step.
    char line[160];
    const char* status = nullptr;
    switch (stage_) {
        case Stage::Wifi:
            std::snprintf(line, sizeof(line), "Wi-Fi %s: %s", pp::netSsid(), pp::netStatus());
            status = line;
            break;
        case Stage::ConnectPending:
        case Stage::Connecting: status = GLUE_STR(LOGIN_STATE_CONNECTING); break;
        case Stage::Auth: status = GLUE_STR(LOGIN_STATE_AUTHENTICATING); break;
        case Stage::WorldPending: status = GLUE_STR(GAME_SERVER_LOGIN); break;
        case Stage::Realms: status = realmsPending_ ? GLUE_STR(REALM_LIST_IN_PROGRESS) : nullptr; break;
        default: break;
    }
    if (status && (!dialog_.shown() || dialog_.text() != status)) dialog_.show(status, GLUE_STR(CANCEL));
    if (!status && stage_ != Stage::Failed && dialog_.shown() && !localePrompt_) dialog_.hide();  // + notice

    inert_.setFocus(~0u);  // nothing in it is ever focused
    inert_.update(pp::PadState{}, 0.0f);
    // + the frame, + LanguageFrame
    FocusRing& under = dialog_.shown() || cinematics_.shown() || language_.shown() ? inert_ : focus_;
    if (stage_ == Stage::Realms) drawRealmList(ui, under);
    else drawLogin(ui, stats, under);
    dialog_.draw(ui, kit_, focus_, &art_);
    cinematics_.draw(ui, kit_, focus_);  // CinematicsFrame (modal)
    language_.draw(ui, kit_, focus_, true);  // LanguageFrame (modal)

    kit_.cursor(ui, focus_);
    ui.endFrame();
}

} // namespace wowee::ui::psp
