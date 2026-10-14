// A test kit: give yourself anything on the server, debug teleports / summons / modes in the
// client, so everything can be tested on the emulator itself. Test aids, never on in a release build's cfg:
//
//   wowee.cfg gmtools=1  - "GM" in the Debug window's title row opens the GM window: four pages of the server's
//                          GM chat commands (.tele / .additem / .learn / .gm / .cheat / .revive ...), sent as SAY. The
//                          account needs a GM level on the server (tools/psp/dev/gm.py set-gm).
//   wowee.cfg devcmd=1   - the PC drives the client: every 0.5 s the client reads devcmd.txt next to the EBOOT
//                          (the emulator's memstick is a PC folder: tools/psp/dev/emutest.py writes it), runs its
//                          lines and deletes it. Lines:
//                            say <text>         a chat line (GM commands too)
//                            pad <tokens>       buttons as autopad (psp_input.hpp: x o s t u d l r L R start select,
//                                               w<ms>, +b/-b, n<x>,<y>:<ms>)
//                            mark <name>        a log line "devcmd_mark name=<name>" (the PC waits for it)
//                            ui 0|1             the interface mode
//                            open|close <win>   a window by name (bags char spells talents quests map menu debug gm
//                                               friends raid lfd achievements calendar) or number (0x51);
//                                               "open settings" = Menu -> Graphics
//                            target next|clear  Tab target / clear
//                          Each line logs "devcmd line=... ok=0|1".
//   (also the on-device autopilot autotest.txt - platform/psp/psp_autotest.hpp):
//                            pos                "devcmd_pos x= y= z= map=" (the autopilot's "moved" compares two)
//                            open who|guild     FriendsFrame on its Who / Guild tab (out of a guild: the Friends tab)
//                            open delete [item] DELETE_ITEM for that item id (else the backpack's first item);
//                            close delete       its No button - never Yes
//                            open dressup [item]  DressUpFrame with that item (else the first wearable one carried)
//                            open language      Menu -> Graphics and its language list (only looked at);
//                            close language|settings  the list / the menu, nothing saved to wowee.cfg
//                            close all          every window, the settings menu, the cursor mode off
//
// PSP ("public safe"): all of this works only against a dev server (platform/psp/psp_public_safe.hpp:
// wowee.cfg devhosts=, private addresses only). Elsewhere the GM button and window are gone, nothing is sent from them,
// devcmd.txt is not read (nor deleted), the autopilot's verbs are refused - and no cfg key changes that.
#include "ui/psp/ui_world_screen.hpp"

#include "game/inventory_slots.hpp"         // devDressUp walks the backpack and the bags
#include "platform/psp/psp_autotest.hpp"    // the on-device autopilot runs these verbs
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_input.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_public_safe.hpp"  // PSP: only against a dev server
#include "platform/psp/psp_strings.hpp"
#include "ui/psp/ui_world_social.hpp"  // "open who|guild" (FriendsFrame's tabs)

#include <pspiofilemgr.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

constexpr uint32_t kIdGmTab0 = 0x9300, kIdGmBtn0 = 0x9310, kIdGmOpen = 0x93FE, kIdGmClose = 0x93FF;  // the developer windows' widget id range

using pp::PS;

// The labels are the port's own strings (ps_b8_port.inc GM_*), shown in the running language.
struct GmCmd {
    PS label;
    const char* cmds;  // one or more GM chat commands, '|' between
};

// Page 0: teleports (game_tele names of AzerothCore - `.tele <name>`).
constexpr GmCmd kTele[] = {
    {PS::GM_TELE_GOLDSHIRE, ".tele Goldshire"},
    {PS::GM_TELE_NORTHSHIRE, ".tele NorthshireValley"},
    {PS::GM_TELE_STORMWIND, ".tele Stormwind"},
    {PS::GM_TELE_STORMWIND_HARBOR, ".tele StormwindHarbor"},
    {PS::GM_TELE_IRONFORGE, ".tele Ironforge"},
    {PS::GM_TELE_DARNASSUS, ".tele Darnassus"},
    {PS::GM_TELE_ORGRIMMAR, ".tele Orgrimmar"},
    {PS::GM_TELE_UNDERCITY, ".tele Undercity"},
    {PS::GM_TELE_THUNDER_BLUFF, ".tele ThunderBluff"},
    {PS::GM_TELE_SILVERMOON, ".tele SilvermoonCity"},
    {PS::GM_TELE_SHATTRATH, ".tele Shattrath"},
    {PS::GM_TELE_DALARAN, ".tele Dalaran"},
    {PS::GM_TELE_MENETHIL, ".tele MenethilHarbor"},
    {PS::GM_TELE_THERAMORE, ".tele Theramore"},
    {PS::GM_TELE_BOOTY_BAY, ".tele BootyBay"},
    {PS::GM_TELE_RATCHET, ".tele Ratchet"},
    {PS::GM_TELE_WARSONG_HOLD, ".tele WarsongHold"},
    {PS::GM_TELE_VALIANCE_KEEP, ".tele ValianceKeep"},
    {PS::GM_TELE_DEADMINES, ".tele Deadmines"},
    {PS::GM_TELE_RAGEFIRE, ".tele RagefireChasm"},
    {PS::GM_TELE_ULDUAR, ".tele UlduarRaid"},
    {PS::GM_TELE_ICC, ".tele IcecrownCitadelRaid"},
    {PS::GM_TELE_NAXXRAMAS, ".tele NaxxramasOutside"},
    {PS::GM_TELE_KARAZHAN, ".tele Karazhan"},
};
// Page 1: give.
constexpr GmCmd kGive[] = {
    {PS::GM_GIVE_BAGS, ".additem 41599 4"},
    {PS::GM_GIVE_GOLD, ".modify money 10000000"},
    {PS::GM_GIVE_LEVELS, ".levelup 10"},
    {PS::GM_GIVE_LEVEL80, ".levelup 79"},
    {PS::GM_GIVE_RIDING, ".learn 33388|.learn 33391|.learn 34090|.learn 34091|.learn 54197"},
    {PS::GM_GIVE_MOUNTS, ".additem 18776|.additem 25470"},
    {PS::GM_GIVE_SPELLS, ".learn all my class"},
    {PS::GM_GIVE_RESET_TALENTS, ".reset talents"},
    {PS::GM_GIVE_POTIONS, ".additem 33447 20|.additem 33445 20"},
    {PS::GM_GIVE_HEARTHSTONE, ".additem 6948"},
    {PS::GM_GIVE_FISHING, ".additem 6256|.additem 6529 20|.learn 7620"},
    {PS::GM_GIVE_GATHERING, ".learn 2366|.learn 2575|.additem 2901"},
};
// Page 2: modes.
constexpr GmCmd kModes[] = {
    {PS::GM_MODE_GM_ON, ".gm on"},
    {PS::GM_MODE_GM_OFF, ".gm off"},
    {PS::GM_MODE_FLY_ON, ".gm fly on"},
    {PS::GM_MODE_FLY_OFF, ".gm fly off"},
    {PS::GM_MODE_SPEED3, ".modify speed 3"},
    {PS::GM_MODE_SPEED1, ".modify speed 1"},
    {PS::GM_MODE_GOD_ON, ".cheat god on"},
    {PS::GM_MODE_GOD_OFF, ".cheat god off"},
    {PS::GM_MODE_NO_COOLDOWN, ".cheat cooldown on"},
    {PS::GM_MODE_NO_CAST, ".cheat casttime on"},
    {PS::GM_MODE_INVIS_ON, ".gm visible off"},
    {PS::GM_MODE_INVIS_OFF, ".gm visible on"},
    {PS::GM_MODE_RAIN, ".wchange 1 0.8"},
    {PS::GM_MODE_CLEAR, ".wchange 0 0"},
};
// Page 3: the character and the target.
constexpr GmCmd kChar[] = {
    {PS::GM_CHAR_REVIVE, ".revive"},
    {PS::GM_CHAR_COOLDOWNS, ".cooldown"},
    {PS::GM_CHAR_SUMMON, ".summon"},
    {PS::GM_CHAR_APPEAR, ".appear"},
    {PS::GM_CHAR_KILL, ".die"},
    {PS::GM_CHAR_RESPAWN, ".respawn"},
    {PS::GM_CHAR_FULL_HP, ".modify hp 999999"},
    {PS::GM_CHAR_GPS, ".gps"},
    {PS::GM_CHAR_SAVE, ".save"},
};
struct GmPage {
    PS title;
    const GmCmd* cmds;
    int count;
};
constexpr GmPage kPages[] = {
    {PS::GM_PAGE_TELEPORT, kTele, static_cast<int>(sizeof(kTele) / sizeof(kTele[0]))},
    {PS::GM_PAGE_GIVE, kGive, static_cast<int>(sizeof(kGive) / sizeof(kGive[0]))},
    {PS::GM_PAGE_MODES, kModes, static_cast<int>(sizeof(kModes) / sizeof(kModes[0]))},
    {PS::GM_PAGE_CHARACTER, kChar, static_cast<int>(sizeof(kChar) / sizeof(kChar[0]))},
};
constexpr int kPageCount = static_cast<int>(sizeof(kPages) / sizeof(kPages[0]));
constexpr int kRows = 8;  // buttons per column; two columns, a second screen of the page with GM_MORE

}  // namespace

bool WorldScreen::gmToolsOn() const { return dev_.gmtools && pp::devToolsAllowed(); }

// "GM" in the Debug window's title row (gmtools=1, a dev server).
void WorldScreen::drawGmDebugButton(UiRenderer& ui, float x, float y) {
    if (gmToolsOn()) kit_.button(ui, winFocus_, kIdGmOpen, Rect{x, y, 56, 20}, "GM");
}

void WorldScreen::devtoolsInit() {
    if (dev_.inited) return;
    dev_.inited = true;
    dev_.gmtools = pp::configGetInt("gmtools", 0) != 0;
    dev_.devcmd = pp::configGetInt("devcmd", 0) != 0;
    if (dev_.gmtools || dev_.devcmd)
        PSP_LOGI(pp::tag::Ctrl, "devtools gmtools=%d devcmd=%d", dev_.gmtools ? 1 : 0, dev_.devcmd ? 1 : 0);
}

// The GM commands of one button (several with '|'), as SAY lines. PSP: never to a server that is not a dev one.
void WorldScreen::gmSend(const char* cmds) {
    if (!pp::devToolsAllowed()) {
        PSP_LOGW(pp::tag::Ctrl, "gm_cmd_refused public=1");
        return;
    }
    std::string all(cmds);
    size_t i = 0;
    while (i <= all.size()) {
        size_t j = all.find('|', i);
        if (j == std::string::npos) j = all.size();
        std::string c = all.substr(i, j - i);
        if (!c.empty()) {
            session_.sendChatMessage(game::ChatType::SAY, c);
            PSP_LOGI(pp::tag::Ctrl, "gm_cmd %s", c.c_str());
        }
        i = j + 1;
    }
}

bool WorldScreen::drawGmWindow(UiRenderer& ui, uint8_t w) {
    if (w != kWinGmTools) return false;
    if (!pp::devToolsAllowed()) return true;  // PSP: nothing drawn; devtoolsUpdate closes it (not while drawing)
    const Rect r{8, 4, 340, 30 + kRows * 23 + 30};
    kit_.panel(ui, r);
    ui.textShadow(kit_.font(kFontNormal), r.x + r.w * 0.5f, r.y + 8, OWN_STR(GM_TITLE), kColGold, Align::Center);
    const float tw = (r.w - 16) / kPageCount;
    for (int p = 0; p < kPageCount; ++p)
        kit_.button(ui, winFocus_, kIdGmTab0 + static_cast<uint32_t>(p),
                    Rect{r.x + 8 + p * tw, r.y + 24, tw - 4, 20}, pp::ps(kPages[p].title), p != dev_.page);
    const GmPage& pg = kPages[dev_.page];
    const int perScreen = kRows * 2;
    const int first = dev_.screen * perScreen;
    const float bw = (r.w - 24) * 0.5f;
    for (int i = 0; i < perScreen && first + i < pg.count; ++i) {
        const int col = i / kRows, row = i % kRows;
        kit_.button(ui, winFocus_, kIdGmBtn0 + static_cast<uint32_t>(i),
                    Rect{r.x + 8 + col * (bw + 8), r.y + 50 + row * 23, bw, 21}, pp::ps(pg.cmds[first + i].label));
    }
    if (pg.count > perScreen)
        kit_.button(ui, winFocus_, kIdGmBtn0 + 0xE, Rect{r.x + 8, r.y + r.h - 26, 80, 20}, OWN_STR(GM_MORE));
    kit_.button(ui, winFocus_, kIdGmClose, Rect{r.x + r.w - 88, r.y + r.h - 26, 80, 20}, CLIENT_STR(CLOSE));
    return true;
}

bool WorldScreen::gmWindowAction(uint32_t act) {
    if (act < kIdGmTab0 || act > kIdGmClose || (act > kIdGmBtn0 + 0xE && act != kIdGmClose && act != kIdGmOpen))
        return false;
    if (!pp::devToolsAllowed()) {  // PSP: the ids are consumed, nothing is sent
        if (windowOpen(kWinGmTools)) closeWindow(kWinGmTools);
        return true;
    }
    if (act == kIdGmOpen) {  // "GM" in the Debug window's title row
        if (windowOpen(kWinGmTools)) closeWindow(kWinGmTools);
        else openWindow(kWinGmTools);
        return true;
    }
    if (act == kIdGmClose) {
        closeWindow(kWinGmTools);
        return true;
    }
    if (act >= kIdGmTab0 && act < kIdGmTab0 + kPageCount) {
        dev_.page = static_cast<int>(act - kIdGmTab0);
        dev_.screen = 0;
        return true;
    }
    const GmPage& pg = kPages[dev_.page];
    if (act == kIdGmBtn0 + 0xE) {
        const int screens = (pg.count + kRows * 2 - 1) / (kRows * 2);
        dev_.screen = (dev_.screen + 1) % (screens > 0 ? screens : 1);
        return true;
    }
    const int i = dev_.screen * kRows * 2 + static_cast<int>(act - kIdGmBtn0);
    if (i >= 0 && i < pg.count) gmSend(pg.cmds[i].cmds);
    return true;
}

// --- The PC's command file --------------------------------------------------------------------------------------

namespace {
uint8_t windowByName(const std::string& n) {
    struct W {
        const char* name;
        uint8_t id;
    };
    static const W kW[] = {{"bags", 0x51}, {"char", 0x50}, {"spells", 0x43}, {"spellbook", 0x43}, {"talents", 0x91},
                           {"quests", 0x5D},
                           {"map", 0x98},  {"friends", 0x1A}, {"raid", 0xE4}, {"lfd", 0xEC}, {"achievements", 0xF2},
                           {"calendar", 0xF3}, {"gm", 0x3E}};
    for (const W& w : kW)
        if (n == w.name) return w.id;
    if (n.size() > 2 && n[0] == '0' && (n[1] == 'x' || n[1] == 'X')) return static_cast<uint8_t>(std::strtoul(n.c_str(), nullptr, 16));
    return 0;
}
}  // namespace

// "open dressup [item]" - DressUpItemLink with that item, else the first dressable one carried.
bool WorldScreen::devDressUp(uint32_t itemId) {
    namespace sl = game::slots;
    if (itemId) {
        dressUpItemLink(itemId);
        return windowOpen(kWinDressUp);
    }
    for (int i = 0; i < sl::kBackpackCount && !windowOpen(kWinDressUp); ++i)
        if (const auto* it = itemAtWire(sl::kNoContainer, static_cast<uint8_t>(sl::kBackpackFirst + i)))
            dressUpItemLink(it->itemId);
    const auto& inv = session_.getInventory();
    for (int b = 0; b < sl::kWornBagCount && !windowOpen(kWinDressUp); ++b)
        for (int i = 0; i < inv.getBagSize(b) && !windowOpen(kWinDressUp); ++i)
            if (const auto* it = itemAtWire(static_cast<uint8_t>(sl::kWornBagFirst + b), static_cast<uint8_t>(i)))
                dressUpItemLink(it->itemId);
    for (int s = 0; s < game::Inventory::NUM_EQUIP_SLOTS && !windowOpen(kWinDressUp); ++s)  // what is worn
        if (const auto* it = itemAtWire(sl::kNoContainer, static_cast<uint8_t>(s))) dressUpItemLink(it->itemId);
    return windowOpen(kWinDressUp);
}

bool WorldScreen::autotestExec(void* self, const std::string& line) {
    return static_cast<WorldScreen*>(self)->devCommand(line);
}

bool WorldScreen::devCommand(const std::string& line) {
    // PSP: devcmd.txt and the autopilot drive the client only against a dev server.
    if (!pp::devToolsAllowed()) return false;
    const size_t sp = line.find(' ');
    const std::string verb = line.substr(0, sp);
    const std::string arg = sp == std::string::npos ? std::string() : line.substr(sp + 1);
    // The autopilot's verbs (also for devcmd.txt).
    if (verb == "pos") {
        PSP_LOGI(pp::tag::Ctrl, "devcmd_pos x=%.1f y=%.1f z=%.1f map=%u", static_cast<double>(playerX_),
                 static_cast<double>(playerY_), static_cast<double>(playerZ_),
                 static_cast<unsigned>(session_.worldEntry().mapId));
        return true;
    }
    if (verb == "open" || verb == "close") {
        const size_t s2 = arg.find(' ');
        const std::string what = arg.substr(0, s2);
        const uint32_t item = s2 == std::string::npos ? 0u : static_cast<uint32_t>(std::strtoul(arg.c_str() + s2 + 1, nullptr, 10));
        if (what == "delete") return devDeleteItem(verb == "open", item);
        if (what == "dressup") {
            if (verb == "open") {
                if (!uiMode_) setUiMode(true);
                return devDressUp(item);
            }
            if (windowOpen(kWinDressUp)) closeWindow(kWinDressUp);
            return true;
        }
        if (what == "who" || what == "guild") {
            if (verb == "close") {
                if (windowOpen(kWinFriends)) closeWindow(kWinFriends);
                return true;
            }
            if (!uiMode_) setUiMode(true);
            if (!windowOpen(kWinFriends)) openWindow(kWinFriends);
            socialUi().tab = what == "who" ? 2 : 3;  // updateSocial: the Guild tab falls back to Friends out of a guild
            return true;
        }
        if (what == "language") {
            if (verb == "close") {
                if (settings_.isOpen()) settings_.dismiss();
                return true;
            }
            if (!uiMode_) setUiMode(true);
            if (!settings_.isOpen()) settings_.open(liveSettings());
            return settings_.openLanguagePicker();
        }
        if (verb == "close" && what == "settings") {
            if (settings_.isOpen()) settings_.dismiss();
            return true;
        }
        if (verb == "close" && what == "all") {
            if (windowOpen(kWinDeleteItem)) devDeleteItem(false, 0);  // the No path, never Yes
            if (settings_.isOpen()) settings_.dismiss();
            const std::vector<uint8_t> open = windows_;
            for (auto it = open.rbegin(); it != open.rend(); ++it)
                if (windowOpen(*it)) closeWindow(*it);
            setUiMode(false);
            return true;
        }
    }
    if (verb == "say" && !arg.empty()) {
        gmSend(arg.c_str());
        return true;
    }
    if (verb == "pad" && !arg.empty()) {
        pp::inputScriptRun(arg);
        return true;
    }
    if (verb == "mark") {
        PSP_LOGI(pp::tag::Ctrl, "devcmd_mark name=%s", arg.c_str());
        return true;
    }
    if (verb == "ui") {
        setUiMode(arg == "1");
        return true;
    }
    if (verb == "open" && arg == "settings") {  // Menu -> Graphics (the settings menu, not a window)
        if (!uiMode_) setUiMode(true);
        if (!settings_.isOpen()) settings_.open(liveSettings());
        return true;
    }
    if (verb == "open" || verb == "close") {
        uint8_t w = 0;
        if (arg == "menu") w = kWinMenu;
        else if (arg == "debug") w = kWinDebug;
        else w = windowByName(arg);
        if (!w) return false;
        if (verb == "open") {
            if (!uiMode_) setUiMode(true);
            if (!windowOpen(w)) openWindow(w);
        } else if (windowOpen(w)) {
            closeWindow(w);
        }
        return true;
    }
    if (verb == "target") {
        if (arg == "clear") clearTarget();
        else tabTarget(1, 1);
        return true;
    }
    return false;
}

void WorldScreen::devtoolsUpdate() {
    devtoolsInit();
    pp::autotestServiceWorld(&WorldScreen::autotestExec, this);  // the autopilot's verbs (none: no-op)
    if (!pp::devToolsAllowed() && windowOpen(kWinGmTools)) closeWindow(kWinGmTools);  // PSP
    if (!dev_.devcmd) return;
    if (!pp::devToolsAllowed()) {  // PSP: devcmd.txt is left alone (not read, not deleted)
        if (pp::publicSafeLocked()) {
            PSP_LOGW(pp::tag::Ctrl, "devcmd_refused public=1");
            dev_.devcmd = false;  // said once; the lock is for the whole run
        }
        return;
    }
    const uint32_t now = pp::logUptimeMs();
    if (now - dev_.lastPollMs < 500u) return;
    dev_.lastPollMs = now;
    char path[256];
    pp::fsPath(path, sizeof(path), "devcmd.txt");
    const SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return;
    char buf[2048];
    const int n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    sceIoRemove(path);
    if (n <= 0) return;
    buf[n] = 0;
    std::string all(buf);
    size_t i = 0;
    while (i < all.size()) {
        size_t j = all.find('\n', i);
        if (j == std::string::npos) j = all.size();
        std::string line = all.substr(i, j - i);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty() && line[0] != '#') {
            const bool ok = devCommand(line);
            PSP_LOGI(pp::tag::Ctrl, "devcmd line=%s ok=%d", line.c_str(), ok ? 1 : 0);
        }
        i = j + 1;
    }
}

}  // namespace wowee::ui::psp
