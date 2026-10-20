// Milestone 3: log in to a server with upstream's AuthHandler (SRP6a, realm
// list) over PSP Wi-Fi, drawn with the client-styled UI kit.
//
// Flow: Wi-Fi (saved profile) -> TCP connect -> LOGON_CHALLENGE/PROOF ->
// REALM_LIST -> realm buttons. Server, account and password come from
// wowee.cfg until the on-screen keyboard exists.
//
// The glue screens of the original (audit "Login, realms, characters"): AccountLogin's layout
// (the expansion logo, the account and password boxes, LOGIN, QUIT, the version bottom left - no
// debug line), the progress and the errors as GlueDialog with the client's own strings (the
// "CANCEL" status dialog while connecting, LOGIN_* / DISCONNECTED / CHAR_LOGIN_FAILED with OKAY),
// RealmList.xml (HelpFrame art, columns REALM_NAME / REALM_TYPE / REALM_CHARACTERS / REALM_LOAD in the original's
// colours, selection highlight, scrolling, OKAY / CANCEL), CharacterSelect's BACK (back to the
// login screen, disconnected) and CHANGE_REALM (the realm list again, on the same auth session when it
// is still open), and the loading screen (LoadingScreens.dbc + LoadingBar) from ENTER_WORLD
// until the world's first frame, the bar filled by the world's load stages.
#pragma once

#include "auth/auth_handler.hpp"
#include "auth/auth_packets.hpp"
#include "platform/psp/psp_input.hpp"
#include "platform/psp/psp_world_session.hpp"
#include "ui/psp/ui_focus.hpp"
#include "ui/psp/ui_glue.hpp"
#include "ui/psp/ui_kit.hpp"
#include "ui/psp/ui_language.hpp"  // LanguageFrame
#include "ui/psp/ui_loading_screen.hpp"
#include "ui/psp/ui_movie.hpp"  // cinematics: MovieFrame, CinematicsFrame
#include "ui/psp/ui_renderer.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wowee::ui::psp {

struct FrameStats {
    float fps = 0.0f;
    uint32_t frameUs = 0;
    uint32_t geWaitUs = 0;
    int build = 0;
};

struct LoginSettings {
    std::string server;
    uint16_t port = 3724;
    std::string account;
    std::string password;
    // PSP (owner 14.10): the saved login (ms0:/WOWEE/account.dat, psp_login_store.hpp) - the SRP credential hash, used
    // when no password was typed; credentialsInCfg: wowee.cfg still has account / password lines (moved out by the
    // first login that succeeds).
    std::vector<uint8_t> authHash;
    bool credentialsInCfg = false;
    std::string locale = "ruRU";
    int netProfile = 0;
    std::string netSsid;     // preferred saved Wi-Fi profile, by SSID
    // PSP: the system connection dialog picks the network (wowee.cfg netdialog, default 1; off for automated runs).
    bool netDialog = true;
    bool autoLogin = false;  // start on the first frame (emulator smoke tests)
    bool autoOsk = false;    // open the name keyboard on the character screen (smoke test)
};

class LoginScreen {
public:
    LoginScreen(const UiKit& kit, LoginSettings settings);
    LoginScreen(const LoginScreen&) = delete;
    LoginScreen& operator=(const LoginScreen&) = delete;
    ~LoginScreen();

    void update(const platform::psp::PadState& pad, float dtSec);
    void draw(UiRenderer& ui, const FrameStats& stats);
    bool wantsExit() const { return wantsExit_; }

private:
    enum class Stage { Idle, NetDialog, Wifi, ConnectPending, Connecting, Auth, Realms, WorldPending, World, Failed };

    void start();
    void fail(const std::string& reason);
    void enterStage(Stage s);
    void leaveWorld();
    const char* stageText() const;
    std::string wifiFailureText() const;
    // Realm list.
    void onRealmList(const std::vector<auth::Realm>& realms);
    void backToLogin();
    void changeRealm();
    void realmCancel();
    void realmOk();
    void enterWorldScreen();
    platform::psp::PadState updateRealmList(const platform::psp::PadState& pad);
    void drawLogin(UiRenderer& ui, const FrameStats& stats, FocusRing& focus);
    void drawRealmList(UiRenderer& ui, FocusRing& focus);
    static void loadingHook(void* self, float frac);
    static void loadingReadTick(void* self);
    std::string realmTitle(const auth::Realm& r) const;

    const UiKit& kit_;
    LoginSettings settings_;
    FocusRing focus_;
    FocusRing inert_;  // Widgets under a dialog declare their stops here
    Stage stage_ = Stage::Idle;
    std::unique_ptr<auth::AuthHandler> auth_;
    std::vector<auth::Realm> realms_;
    // Milestone 4: world session + character screen, after a realm is picked.
    // One per world connection: a fresh session on every realm
    // pick, so nothing of a dropped one (entities, caches, chat) survives.
    std::unique_ptr<platform::psp::WorldSession> world_;
    std::unique_ptr<class CharacterScreen> chars_;
    std::unique_ptr<class WorldScreen> worldView_;
    size_t pickedRealm_ = 0;
    // PSP (owner 14.10): the account / password boxes take the system keyboard (0 none, 1 account, 2 password);
    // the login that succeeded is saved outside the game folder once per run (psp_login_store.hpp).
    int oskField_ = 0;
    bool loginSaved_ = false;
    void openField(int field);
    void fieldTyped(int field, bool accepted, std::string& typed);
    void saveLogin();
    void forgetLogin();
    bool autoRealm_ = false;
    bool autoCharCreate_ = false;
    std::string failure_;
    uint32_t nowMs_ = 0;
    uint32_t logSyncUntilMs_ = 0;  // world entry: every log line reaches the card (HW crash hunt)
    uint64_t stageStartUs_ = 0;
    uint32_t worstUpdateUs_ = 0;
    bool wantsExit_ = false;

    // Glue screens.
    GlueDialog dialog_;
    GlueArt art_;
    LoadingScreen loading_;
    UiRenderer* ui_ = nullptr;          // the frame's renderer, for the loading frames drawn during the load
    uint32_t lastLoadingDrawMs_ = 0;
    uint32_t loadKeepAlives_ = 0;  // CMSG_KEEP_ALIVE sent during this load
    uint32_t loadReads_ = 0;       // Pak reads of this load (loadingReadTick)
    uint32_t loadingHoldStartMs_ = 0, loadingHoldFrames_ = 0;  // The warm-up hold
    static constexpr uint32_t kLoadingHoldWindow = 16;         // Frames averaged
    float loadingHoldDt_[kLoadingHoldWindow] = {};
    int realmSel_ = -1;                 // RealmList.currentRealm (0-based)
    int realmScroll_ = 0;               // RealmList.offset
    int prevRealm_ = -1;                // the realm CharacterSelect came from (CANCEL goes back to it)
    bool realmsPending_ = false;        // REALM_LIST_IN_PROGRESS (asked from CharacterSelect)

    // map transfer: a transfer to another map rebuilds the world -
    // SMSG_TRANSFER_PENDING puts that map's loading screen over the world at once, SMSG_NEW_WORLD (acknowledged by the
    // session) destroys the WorldScreen with everything it holds, the session is serviced meanwhile, then the next
    // map's WorldScreen is built under the loading screen as at world entry. Memory: "mem at=transfer_begin /
    // transfer_unloaded / transfer_end" and one "transfer" line.
    void updateTransferPending();      // the pending transfer's loading screen over the world (or its end)
    void beginTransfer();              // the old world goes
    void updateTransfer(float dtSec);  // until the next world is built
    bool transferUnloaded_ = false;      // between beginTransfer and the new WorldScreen
    bool transferPendingShown_ = false;  // the loading screen is up for a pending transfer
    uint32_t transferFrom_ = 0, transferTo_ = 0, transferStartMs_ = 0;
    uint32_t transferBeforeKb_ = 0, transferUnloadedKb_ = 0;
    // cinematics (ui_movie.cpp): the intro on the first start (the client's CVar "movie"), the
    // CinematicsFrame behind CINEMATICS on the login screen, PLAY_MOVIE in the world (SMSG_TRIGGER_MOVIE: the
    // movie over the frozen world, the session pumped and kept alive meanwhile) and CINEMATIC_START (no flyby yet:
    // skipped as ESCAPE would).
    MovieFrame movies_{kit_};
    CinematicsFrame cinematics_;
    bool introChecked_ = false;
    uint64_t moviePumpUs_ = 0, movieKeepAliveUs_ = 0;
    uint32_t movieKeepAlives_ = 0;
    void updateGameMovies();
    static bool moviePump(void* self);
    // cinematics (end)

    // "Language" on the login screen - LanguageFrame, then OPTION_RESTART_REQUIREMENT (GlueDialog,
    // RESTART / OKAY) when the pick is not the running language. RESTART ends the run for main()'s relaunch.
    LanguageFrame language_;
    bool localePrompt_ = false;  // dialog_ holds the restart notice
    void pickLanguage(const std::string& code);
};

} // namespace wowee::ui::psp
