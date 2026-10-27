// Autopilot: drives the front end to a chosen mode and characters by pressing buttons the way a player would,
// watching game state instead of trusting fixed timings (boot and load times vary from run to run).
//
//   MKDA_AUTOPILOT=versus|arcade|practice    or konquest|krypt|profile|options|kontent, which have no character
//                                            select: the autopilot is done once the menu took the item; or menu:
//                                            done at the main menu (KONQUEST and THE KRYPT take a loaded profile)
//   MKDA_P1, MKDA_P2=<character>             who each side picks (default: what the cursor starts on): the game's
//                                            name, the launcher's roster id (SubZero, BoRaiCho) or the index; all
//                                            characters are unlocked when either is given, or with MKDA_UNLOCK_ALL=1
//   MKDA_AUTOPILOT_HOLD=1                    stop at character select (for the debug monitor)
//   MK3D_TRAINING=1                          to practice's select screen; the player picks there (the launcher's
//                                            practice while it searches for a match)
//
//   1. START every 0.7 s until the main menu's state task starts (skips the movies and the title)
//   2. after the menu settles (the hold point the launcher waits at), UP/DOWN on port 0 until the main menu's
//      selection (0x268D84) is the mode's item, then A until the menu confirms it. VERSUS needs two open controllers
//      (MKDA_PADS=2); without them the ARCADE item is confirmed and redirected to versus (events.cpp, sub_000E3CC0)
//   3. character select: each port steers its player's cursor to its character's slot, reading the slot back, and
//      confirms with A (port 1 joins as P2 with its first press); practice picks both fighters from port 0, in turn;
//      then A on port 0 through whatever follows until the fight is set up
//
// Character select (research/mkda-rev1/notes.md, "Game layer"): cursors 0x2EA660 (P1) and 0x2EA65C (P2) are slots of
// a 3 x 7 grid (UP is slot - 3, RIGHT slot + 1; the handlers skip locked slots), slot to character 0x273E38 + slot *
// 0x2C (0x18 ends it). A player's state (0x2EA550 / 0x2EA554) is 2 while choosing and 3 once picked; the pick
// (sub_001112A0) stores the character in 0x271F00 / 0x271F04. BLAZE and MOKAP have no square: DOWN held while
// picking on RAIDEN's (slot 18) or CYRAX's (slot 20) chooses them. A character is selectable when its bit is set in
// the profiles' mask 0x421530 or the built-in one 0x273580 (alternate costumes 0x421534 / 0x2EA844); the unlock
// sets the built-in ones, which nothing saves.
#include "game/autopilot.h"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "core/log.h"
#include "game/events.h"
#include "game/frame.h"
#include "kernel/kernel.h"
#include "mk3d/netplay.h"

namespace mkda::autopilot {
namespace {

using Clock = std::chrono::steady_clock;

enum class Phase { Off, ToMenu, Settle, Hold, Choose, Select, Done };

constexpr uint32_t kCursor[2] = {0x2EA660, 0x2EA65C};
constexpr uint32_t kGrid = 0x273E38, kGridStride = 0x2C, kGridEnd = 0x18;
constexpr int kGridWidth = 3, kGridMax = 32;
constexpr uint32_t kUnlockDefaults[2] = {0x273580, 0x2EA844};  // characters, alternate costumes
constexpr int kNoCharacter = 0x18;
constexpr int kBlaze = 22, kMokap = 23, kRaidenSlot = 18, kCyraxSlot = 20;
constexpr int kCharacters = 24;
constexpr uint32_t kP2Port = 0x266B28;

constexpr uint16_t kUp = 0x01, kDown = 0x02, kLeft = 0x04, kRight = 0x08, kStart = 0x10;

std::mutex g_lock;
Phase g_phase = Phase::Off;
Clock::time_point g_since;
// ports the autopilot plays on, reported as connected from then on (port 0 from boot, port 1 from a versus select
// screen): a pad that went away again would take the game to its controller screen
uint32_t g_connected = 0;
uint32_t g_base_actions = 0, g_base_fights = 0, g_base_menus = 0;
int g_item = -1;          // the main menu item to confirm
bool g_redirect = false;  // the confirmation is redirected (events.cpp RequestMode) by the autopilot
Request g_req;
bool g_have_request = false;
bool g_wait_for_request = false;
// MK3D_TRAINING=1, the launcher's practice while a search waits: to practice's select screen, where the player picks
// both fighters with their own controller as in the game's own practice
const bool g_training = std::getenv("MK3D_TRAINING") != nullptr;
const bool g_hold_at_select = std::getenv("MKDA_AUTOPILOT_HOLD") != nullptr;

// one port's press, decided once a press cycle (what the cursor did is known by then) and held for the cycle's press
struct Press {
    long cycle = -1;
    uint16_t buttons = 0;
    uint8_t a = 0;
};
Press g_press[2];

double Since() { return std::chrono::duration<double>(Clock::now() - g_since).count(); }

void Enter(Phase p) {
    g_press[0] = g_press[1] = Press{};
    g_phase = p;
    g_since = Clock::now();
    static const char* names[] = {"off", "to menu", "settle", "hold", "choose", "select", "done"};
    LOGR("autopilot: %s", names[int(p)]);  // a few lines a match, in a player's log too: how a match got started
}

std::string Norm(const std::string& s) {
    std::string o;
    for (char c : s)
        if (std::isalnum(static_cast<unsigned char>(c))) o += char(std::toupper(static_cast<unsigned char>(c)));
    return o;
}

std::string Name(int id) {
    if (id < 0 || id >= kCharacters) return "?";
    uint32_t p = kernel::R32(kCharacterNames + uint32_t(id) * kCharacterStride);
    return p ? kernel::ReadCString(p, 32) : "?";
}

// the main menu's items in order (research/mkda-rev1/notes.md, sub_000E3CC0)
int ItemFor(int mode) {
    switch (mode) {
        case int(kModeArcade): return 0;
        case int(kModeVersus): return 1;
        case int(kModePractice): return 2;
        case int(kModeKonquest): return 3;
        case int(kModeKrypt): return 4;
        case int(kModeProfile): return 5;
        case int(kModeOptions): return 6;
        case int(kModeKontent): return 7;
        default: return -1;
    }
}

// the modes that go on to character select
bool Fights(int mode) { return mode == int(kModeArcade) || mode == int(kModeVersus) || mode == int(kModePractice); }

// MKDA_AUTOPILOT=menu: done once the main menu is up, for a script to take it from there (KONQUEST and THE KRYPT
// need a profile loaded first: LT at the main menu, the profile, then its kode)
constexpr int kMenuOnly = -2;

const bool g_loaded = [] {
    const char* s = std::getenv("MKDA_AUTOPILOT");
    if (!s && g_training) s = "practice";
    if (!s) return false;
    g_req.mode = _stricmp(s, "menu") == 0 ? kMenuOnly : ModeByName(s);
    if (g_req.mode != kMenuOnly && ItemFor(g_req.mode) < 0) {
        LOGE("autopilot: unknown mode %s (versus, arcade, practice, konquest, krypt, profile, options, kontent, menu)", s);
        return false;
    }
    if (const char* p = std::getenv("MKDA_P1")) g_req.names[0] = p;
    if (const char* p = std::getenv("MKDA_P2")) g_req.names[1] = p;
    g_req.unlock_all = std::getenv("MKDA_UNLOCK_ALL") || !g_req.names[0].empty() || !g_req.names[1].empty();
    g_have_request = true;
    g_connected = 1;
    g_since = Clock::now();
    g_phase = Phase::ToMenu;
    return true;
}();

// a fighter's health (float, 1.0 full) held to [low, high] while it is above zero
void ClampHealth(uint32_t va, float low, float high) {
    uint32_t bits = kernel::R32(va);
    float h;
    std::memcpy(&h, &bits, 4);
    if (h <= 0.0f || h > 1.0f || (h >= low && h <= high)) return;
    h = h < low ? low : high;
    std::memcpy(&bits, &h, 4);
    kernel::W32(va, bits);
}

// MKDA_TEST_ONE_HIT=1 (tests): in arcade the opponent, P2, is kept at 1% health and P1 at half or more, so P2 goes
// down from the first hit (or loses on time) and a scripted run gets through the whole ladder to its bosses and ending
const bool g_one_hit = [] {
    if (!std::getenv("MKDA_TEST_ONE_HIT")) return false;
    OnFrameStart([](uint64_t) {
        if (GameMode() != kModeArcade || kernel::R32(kMainTask) != kFightTask) return;
        ClampHealth(kHealth[0], 0.5f, 1.0f);
        ClampHealth(kHealth[1], 0.0f, 0.01f);
    });
    return true;
}();

// a short press every cycle seconds, the first press seconds of each
bool Pulse(double t, double cycle, double press) { return t - int(t / cycle) * cycle < press; }

// players picking on the select screen get the characters only: an alternate costume one of them chose would differ
// on the other PC, whose autopilot picks the character as it comes over netplay
bool PlayersPick() { return g_req.by_player[0] || g_req.by_player[1]; }

void Unlock() {
    kernel::W32(kUnlockDefaults[0], 0xFFFFFFFF);
    if (!PlayersPick()) kernel::W32(kUnlockDefaults[1], 0xFFFFFFFF);
}

// An online match whose players pick on the select screen: this PC's player picks for their side with their own
// controller (the pads route it to that side's port, LocalPickPort), and the pick goes to the other game with the
// HELLOs (net::SetLocalPick); the other side waits for the other player's pick, then the autopilot makes it. A
// spectator's game waits for both players' picks that way.
constexpr int kWaitPick = -2;            // characters[]: the other player has not picked yet
constexpr double kPlayerPickTime = 75;   // seconds this PC's player has, then the autopilot takes the cursor's
bool g_local_pick_sent = false;
bool g_local_forced = false;             // the time ran out: the autopilot picks for this PC's player

void ResetPlayerPicks() {
    g_local_pick_sent = g_local_forced = false;
    for (int i = 0; i < 2; i++)
        if (g_req.by_player[i]) g_req.characters[i] = i == g_req.local_side ? -1 : kWaitPick;
}

// this PC's player's pick, once made, to the other game; the other player's (a spectator's game: both players'),
// once it came, to the autopilot
void ExchangePicks(double t) {
    const int me = g_req.local_side;
    if (me < 0 && !g_req.watch) return;
    if (me >= 0 && g_req.by_player[me] && !g_local_pick_sent && kernel::R32(kPlayerState[me]) == 3) {
        const uint32_t words[2] = {kernel::R32(kCharacter[me]), 0};
        mk3d::net::SetLocalPick(words, 2);
        g_local_pick_sent = true;
        LOGI("autopilot: this player picked %s (%u)", Name(int(words[0])).c_str(), words[0]);
    }
    if (me >= 0 && g_req.by_player[me] && !g_local_forced && !g_local_pick_sent && t > kPlayerPickTime) {
        g_local_forced = true;
        LOGI("autopilot: no pick after %.0f s: this player gets the cursor's", kPlayerPickTime);
    }
    for (int other = 0; other < 2; other++) {
        if (other == me || !g_req.by_player[other] || g_req.characters[other] != kWaitPick) continue;
        uint32_t words[4];
        int n = 4;
        const bool in = g_req.watch ? mk3d::net::WatchPick(other, words, n) : mk3d::net::PeerPick(words, n);
        if (in && n > 0 && int(words[0]) < kCharacters) {
            g_req.characters[other] = int(words[0]);
            LOGI("autopilot: P%d picked %s (%u)", other + 1, Name(int(words[0])).c_str(), words[0]);
        }
    }
}

// the grid slot that picks a character, -1 when none does; hidden ones need DOWN held while picking
int SlotOf(int character, bool& hold_down) {
    hold_down = false;
    if (character == kBlaze || character == kMokap) {
        hold_down = true;
        return character == kBlaze ? kRaidenSlot : kCyraxSlot;
    }
    for (int s = 0; s < kGridMax; s++) {
        int id = int(kernel::R32(kGrid + uint32_t(s) * kGridStride));
        if (id == int(kGridEnd)) break;
        if (id == character) return s;
    }
    return -1;
}

// the d-pad press that takes a cursor one step from slot cur toward slot target (UP is the row above, slot - 3)
uint16_t Steer(int cur, int target) {
    int r = cur / kGridWidth, c = cur % kGridWidth, tr = target / kGridWidth, tc = target % kGridWidth;
    if (r != tr) return tr > r ? kDown : kUp;
    return tc > c ? kRight : kLeft;
}

// what to press this cycle at character select to get a character with a cursor: the step toward its slot, or A there
Press Decide(int character, int cursor_player) {
    Press p;
    bool down = false;
    int target = character >= 0 ? SlotOf(character, down) : -1;
    int cur = int(kernel::R32(kCursor[cursor_player]));
    if (target < 0 || target == cur) {
        p.a = 0xFF;
        if (down) p.buttons |= kDown;
    } else {
        p.buttons = Steer(cur, target);
    }
    return p;
}

// one port's character select; port 0 is P1, port 1 P2 (practice: port 0 picks both, the opponent second)
void Select(int port, input::Pad& pad, double t) {
    if (g_req.characters[port] == kWaitPick) {  // the other player has not picked: keep the screen awake
        if (Pulse(t, 8.0, 0.08)) pad.buttons |= (long(t / 8) & 1) ? kLeft : kRight;
        return;
    }
    long cycle = long(t / 0.25);
    Press& p = g_press[port];
    if (cycle != p.cycle) {
        p = Press{};
        p.cycle = cycle;
        uint32_t mode = GameMode();
        if (mode == kModePractice) {
            // the first pick is P1's own fighter, the second (P1's state is set back to 2) the opponent
            if (port == 0 && kernel::R32(kPlayerState[0]) == 2) {
                int which = int(kernel::R32(kCharacter[0])) == kNoCharacter ? 0 : 1;
                p = Decide(g_req.characters[which], 0);
                p.cycle = cycle;
            }
        } else if (port == g_req.local_side && g_req.by_player[port] && !g_local_forced) {
            // this PC's player picks for this side with their own controller
        } else if (kernel::R32(kPlayerState[port]) == 2) {
            p = Decide(g_req.characters[port], port);
            p.cycle = cycle;
            // P2's pad is not assigned yet: its first press joins (and counts as a press of the player it joins)
            if (port == 1 && kernel::R32(kP2Port) != 1 && p.a) {
                p.a = 0;
                p.buttons = kRight;
            }
        }
    }
    if (Pulse(t, 0.25, 0.08)) {
        pad.buttons |= p.buttons;
        if (p.a) pad.analog[0] = p.a;
    }
}

bool AllPicked() {
    for (int i = 0; i < 2; i++) {
        uint32_t s = kernel::R32(kPlayerState[i]);
        if (s != 0 && s != 3) return false;
    }
    return true;
}

}  // namespace

// Letters and digits only, any case, against the game's names: the launcher's roster ids (launcher/data/mkda.json:
// BoRaiCho, JohnnyCage, SubZero, ..., Blaze, Mokap) and the game's own spellings ("BO' RAI CHO", "SUB-ZERO") both
// match exactly; otherwise one name that contains the text or is contained in it ("cage", "Sonya Blade")
int CharacterId(const std::string& s) {
    if (s.empty() || s == "-") return -1;
    if (std::isdigit(static_cast<unsigned char>(s[0]))) return std::atoi(s.c_str());
    if (!kernel::g_base) return -1;  // the names are read from the game image
    std::string want = Norm(s);
    if (want.empty()) return -1;
    int partial = -1, partials = 0;
    for (int i = 0; i < kCharacters; i++) {
        std::string name = Norm(Name(i));
        if (name.empty()) continue;
        if (name == want) return i;
        if (name.find(want) != std::string::npos || want.find(name) != std::string::npos) {
            partial = i;
            partials++;
        }
    }
    return partials == 1 ? partial : -1;
}

bool Active() {
    std::lock_guard<std::mutex> lock(g_lock);
    return g_phase != Phase::Off && g_phase != Phase::Done && g_phase != Phase::Hold;
}

bool AtHoldPoint() {
    std::lock_guard<std::mutex> lock(g_lock);
    return g_phase == Phase::Hold;
}

void WaitForRequest() {
    std::lock_guard<std::mutex> lock(g_lock);
    g_wait_for_request = true;
    g_have_request = false;
    g_connected |= 1;
    if (g_phase == Phase::Off) {
        g_since = Clock::now();
        g_phase = Phase::ToMenu;
    }
}

bool Begin(const Request& req) {
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_phase != Phase::Hold) return false;
    g_req = req;
    g_have_request = true;
    Enter(Phase::Settle);
    return true;
}

int ModeForName(const std::string& mode) {
    int m = ModeByName(mode.c_str());
    return ItemFor(m) >= 0 ? m : -1;
}

void Rematch() {
    std::lock_guard<std::mutex> lock(g_lock);
    g_base_fights = FightSetups();
    ResetPlayerPicks();  // players who pick on the select screen pick again
    Enter(Phase::Select);
}

int LocalPickPort() {
    std::lock_guard<std::mutex> lock(g_lock);
    const int me = g_req.local_side;
    if (g_phase != Phase::Select || me < 0 || !g_req.by_player[me] || g_local_forced || Since() < 1.5) return -1;
    if (kernel::R32(kMainTask) != kSelectTask || kernel::R32(kPlayerState[me]) == 3) return -1;  // not up, or picked
    return me;
}

void Restart(const Request& req) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_req = req;
    g_have_request = true;
    if (kernel::R32(kMainTask) == kMainMenuTask) {
        Enter(Phase::Settle);  // already there
    } else {
        g_base_menus = MainMenus();
        Enter(Phase::ToMenu);
    }
}

void ExpectMenu() {
    std::lock_guard<std::mutex> lock(g_lock);
    g_base_menus = MainMenus();
    g_have_request = false;
    g_wait_for_request = true;
    Enter(Phase::ToMenu);
}

void Apply(int port, input::Pad& pad) {
    std::lock_guard<std::mutex> lock(g_lock);
    if (port < 32 && (g_connected >> port & 1)) pad.connected = true;
    if (g_phase == Phase::Off || g_phase == Phase::Done || g_phase == Phase::Hold) return;
    double t = Since();
    switch (g_phase) {
        case Phase::ToMenu:
            if (MainMenus() > g_base_menus) {
                Enter(g_wait_for_request && !g_have_request ? Phase::Hold : Phase::Settle);
                if (g_phase == Phase::Hold) LOGI("autopilot: at the hold point");
                return;
            }
            if (g_base_menus == 0 && port == 0 && Pulse(t, 0.7, 0.1)) pad.buttons |= kStart;  // boot only
            return;
        case Phase::Settle:
            if (t > 1.5 && g_req.mode == kMenuOnly) {
                if (g_req.unlock_all) Unlock();
                LOGI("autopilot: at the main menu");
                Enter(Phase::Done);
                return;
            }
            if (t > 1.5) {
                for (int i = 0; i < 2; i++) {
                    int id = CharacterId(g_req.names[i]);
                    bool down = false;
                    if (id >= 0 && SlotOf(id, down) < 0) {
                        LOGE("autopilot: %s (%d) cannot be picked at character select; P%d takes the cursor's",
                             Name(id).c_str(), id, i + 1);
                        id = -1;
                    }
                    g_req.characters[i] = id;
                    if (!g_req.names[i].empty() && id < 0 && CharacterId(g_req.names[i]) < 0)
                        LOGE("autopilot: unknown character %s for P%d", g_req.names[i].c_str(), i + 1);
                    else if (id >= 0)
                        LOGI("autopilot: P%d plays %s (%d)", i + 1, Name(id).c_str(), id);
                    else if (g_req.by_player[i])
                        LOGI("autopilot: P%d picks on the select screen%s", i + 1,
                             i == g_req.local_side ? " (this PC's player)" : "");
                }
                ResetPlayerPicks();
                if (g_req.unlock_all) Unlock();
                g_item = ItemFor(g_req.mode);
                if (g_req.mode == int(kModeVersus) && OpenPads() < 2) {
                    // VERSUS is not offered with one controller: confirm ARCADE and have it redirected
                    g_item = ItemFor(int(kModeArcade));
                    RequestMode(int(kModeVersus));
                    g_redirect = true;
                    LOGI("autopilot: %d controller(s) open, VERSUS through the arcade item", OpenPads());
                }
                g_base_actions = MenuActions();
                Enter(Phase::Choose);
            }
            return;
        case Phase::Choose: {
            if (MenuActions() > g_base_actions || t > 60) {
                if (g_redirect) RequestMode(-1);  // used (or not needed any more)
                g_redirect = false;
                if (MenuActions() <= g_base_actions) {
                    LOGE("autopilot: the main menu did not take item %d (mode %d)", g_item, g_req.mode);
                    Enter(Phase::Done);
                    return;
                }
                if (!Fights(g_req.mode)) {
                    LOGI("autopilot: in mode %d", g_req.mode);
                    Enter(Phase::Done);
                    return;
                }
                g_base_fights = FightSetups();
                Enter(Phase::Select);
                return;
            }
            if (port != 0 || TopMenu() != int(kMainMenu)) return;
            long cycle = long(t / 0.25);
            Press& p = g_press[0];
            if (cycle != p.cycle) {
                p = Press{};
                p.cycle = cycle;
                int cur = int(kernel::R32(kMenuSelection + kMainMenu * kMenuStride));
                if (cur != g_item) p.buttons = g_item > cur ? kDown : kUp;
                else if (Fights(g_req.mode)) p.a = 0xFF;
                else p.buttons = kStart;  // the items that need a profile (sub_000CD9C0) take START, not A
            }
            if (Pulse(t, 0.25, 0.08)) {
                pad.buttons |= p.buttons;
                if (p.a) pad.analog[0] = p.a;
            }
            return;
        }
        case Phase::Select: {
            if (g_training) {  // the player's own picks from here
                LOGR("autopilot: practice's select screen, the player picks");
                Enter(Phase::Done);
                return;
            }
            if (g_hold_at_select) return;
            // players picking on the select screen may take their time (75 s each, after that their cursor's)
            const double limit = PlayersPick() ? 240 : 120;
            if (FightSetups() > g_base_fights || t > limit) {
                if (FightSetups() <= g_base_fights) LOGE("autopilot: no fight after %.0f s at character select", limit);
                Enter(Phase::Done);
                return;
            }
            if (port > 1) return;
            if (port == 1 && GameMode() == kModeVersus && !(g_connected & 2)) {
                g_connected |= 2;  // P2's pad, plugged in now if MKDA_PADS did not already
                pad.connected = true;
            }
            if (t < 1.5) return;  // the select screen fades in
            if (g_req.unlock_all) Unlock();
            if (port == 0) ExchangePicks(t);
            if (!AllPicked() && (kernel::R32(kMainTask) == kSelectTask)) {
                Select(port, pad, t);
            } else if (port == 0 && Pulse(t, 0.7, 0.1)) {
                pad.analog[0] = 0xFF;  // A through what follows the picks
            }
            return;
        }
        default:
            return;
    }
}

}  // namespace mkda::autopilot
