// Autopilot: drives the front end to a chosen mode and characters by pressing buttons the way a player would,
// watching game state instead of trusting fixed timings (boot and load times vary from run to run).
//
//   MKD_AUTOPILOT=versus|chess|puzzle|arcade|practice   or konquest, or a main menu action's number (0x10): those
//                                            have no select screen, so the autopilot is done once the menu took it
//   MKD_P1, MKD_P2=<character name or id>   who each side picks (default: what the cursor starts on); every
//                                            character, costume and arena is unlocked when either is given, or with
//                                            MKD_UNLOCK_ALL=1 (a launcher match always)
//   MKD_AUTOPILOT_HOLD=1                     stop at character select (for the debug monitor)
//   MK3D_TRAINING=1                          to practice's select screen, every character, costume and arena
//                                            unlocked; the player picks there (the launcher's practice: its own
//                                            button, or while a search waits)
//
//   1. START every 0.7 s until the main menu task starts (skips movies, the title and the profile screen)
//   2. after the menu settles (the hold point the launcher waits at), START and A in turn until the menu confirms an
//      item (the profile screen, KOMBAT, then ARCADE); the confirmation is redirected to the chosen mode with both
//      players taking part (events.cpp, sub_0014EF20)
//   3. character select: each port steers its cursor to its character's slot, reading the slot back after every
//      press, and confirms with A; then A on port 0 through whatever follows until the fight is set up
//
// Character select (research/mkd/notes.md, round 4): cursor slots 0x471B1C (P1) and 0x471B18 (P2) in an 8 x 3 grid
// (6 x 2 in puzzle kombat, 0x471B24 == 2; chess kombat's board game select sets 1); slot to character: 0x3F9210 +
// slot * 0x28 (puzzle 0x3F95D0); a player record's
// state +8 is 2 once its pick is confirmed; a character is selectable when its bit is set in the 64-bit masks at
// 0x601E60 (0x601E68 alternates, 0x601E90 puzzle) or the built-in defaults at 0x3F8FF0, an arena (sub_000406F0, which
// the versus arena cycle sub_00040640 skips locked ones by) in 0x601E70 or 0x3E4AF0. The masks are rebuilt from the
// defaults and the profiles' own bits (sub_001726E0); only the profiles' bits are saved.
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

namespace mkd::autopilot {
namespace {

using Clock = std::chrono::steady_clock;

enum class Phase { Off, ToMenu, Settle, Hold, Choose, Select, Done };

constexpr uint32_t kCursor[2] = {0x471B1C, 0x471B18};
constexpr uint32_t kRecord[2] = {kP1Record, kP2Record};
constexpr uint32_t kGrid = 0x3F9210, kChessGrid = 0x3F95D0, kGridKind = 0x471B24;
// 64-bit unlock masks: characters, alternate costumes, arenas (sub_000406F0), the side modes' characters and arenas;
// the built-in ones in the same order
constexpr uint32_t kUnlockMasks[] = {0x601E60, 0x601E68, 0x601E70, 0x601E90, 0x601E98};
constexpr uint32_t kBuiltIn[] = {0x3F8FF0, 0x471B28, 0x3E4AF0, 0x3F8FF8, 0x3E4AF8};
constexpr uint32_t kNames = 0x3F89E0;  // character names, 16 bytes per character, name pointer first
constexpr uint32_t kMenuAction = 0x46CDB8;  // the main menu task's action (0x18 none)
constexpr uint32_t kSelectTask = 0x179A60;  // character select (arcade, versus, practice)

std::mutex g_lock;
Phase g_phase = Phase::Off;
Clock::time_point g_since;
uint32_t g_base_actions = 0, g_base_fights = 0, g_base_menus = 0, g_base_matches = 0;
// Chess Kombat's team select, one port: decided once a press cycle (what the cursor did is known by then), held for
// the cycle's press
struct ChessPort {
    bool leader = false;  // the leader was picked
    int probe = 0;        // the grid slot tried next
    long cycle = -1;
    uint32_t buttons = 0;
    uint8_t a = 0;
};
ChessPort g_chess[2];
Request g_req;
bool g_have_request = false;
bool g_wait_for_request = false;
// MK3D_TRAINING=1, the launcher's practice while a search waits: to practice's select screen, where the player picks
// both fighters with their own controller as in the game's own practice
const bool g_training = std::getenv("MK3D_TRAINING") != nullptr;
const bool g_hold_at_select = std::getenv("MKD_AUTOPILOT_HOLD") != nullptr;

double Since() { return std::chrono::duration<double>(Clock::now() - g_since).count(); }

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

// MKD_TEST_ONE_HIT=1 (tests): in arcade the opponent, P2, is kept at 1% health and P1 at half or more, so P2 goes
// down from the first hit (or loses on time) and a scripted run gets through the whole ladder to its bosses and ending
const bool g_one_hit = [] {
    if (!std::getenv("MKD_TEST_ONE_HIT")) return false;
    OnFrameStart([](uint64_t) {
        if (GameMode() != 0) return;  // arcade
        ClampHealth(kP1Health, 0.5f, 1.0f);
        ClampHealth(kP2Health, 0.0f, 0.01f);
    });
    return true;
}();

void Enter(Phase p) {
    if (p == Phase::Select) g_chess[0] = g_chess[1] = ChessPort{};
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

// a character name (any case and punctuation: "SubZero", "Sub-Zero", "BO' RAI CHO") or index, -1 if unknown
int CharacterId(const std::string& s) {
    if (s.empty() || s == "-") return -1;
    if (std::isdigit(static_cast<unsigned char>(s[0]))) return std::atoi(s.c_str());
    std::string want = Norm(s);
    for (int i = 0; i < 40; i++) {
        uint32_t p = kernel::R32(kNames + uint32_t(i) * 16);
        if (!p) continue;
        std::string name(static_cast<const char*>(kernel::HostPtr(p)));
        if (Norm(name) == want) return i;
    }
    return -1;
}

int ActionFor(const std::string& mode) {
    struct {
        const char* name;
        int action;
    } modes[] = {{"versus", 7}, {"kombat", 7}, {"chess", 8}, {"puzzle", 9}, {"arcade", 6}, {"practice", 0xB},
                 {"konquest", 2}};
    for (auto& m : modes)
        if (mode == m.name) return m.action;
    if (!mode.empty() && std::isdigit(static_cast<unsigned char>(mode[0]))) return int(std::strtol(mode.c_str(), nullptr, 0));
    return -1;
}

// the actions that go on to a select screen (sub_000E3770)
bool Selects(int action) { return action == 6 || action == 7 || action == 8 || action == 9 || action == 0xB; }

const bool g_loaded = [] {
    const char* s = std::getenv("MKD_AUTOPILOT");
    if (!s && g_training) s = "practice";
    if (!s) return false;
    g_req.action = ActionFor(s);
    if (g_req.action < 0) {
        LOGE("autopilot: unknown mode %s (versus, chess, puzzle, arcade, practice, konquest or an action)", s);
        return false;
    }
    if (const char* p = std::getenv("MKD_P1")) g_req.names[0] = p;
    if (const char* p = std::getenv("MKD_P2")) g_req.names[1] = p;
    g_req.unlock_all = std::getenv("MKD_UNLOCK_ALL") || !g_req.names[0].empty() || !g_req.names[1].empty();
    g_have_request = true;
    g_since = Clock::now();
    g_phase = Phase::ToMenu;
    return true;
}();

// a short press every cycle seconds, the first press seconds of each
bool Pulse(double t, double cycle, double press) { return t - int(t / cycle) * cycle < press; }

int SlotOf(int character);

bool PlayersPick() { return g_req.by_player[0] || g_req.by_player[1]; }

// Every character, alternate costume and arena, for as long as the game runs (online matches and practice): the
// masks the checks read and the built-in ones the checks OR in and every rebuild of the masks starts from
// (sub_001726E0), so a profile's saved unlocks (its own bits) stay its own. A costume a player picks on the select
// screen goes to the other game with the pick (ExchangePicks).
void Unlock() {
    static bool told = false;  // what was open before, once: which characters, costumes and arenas this opens
    if (!told) {
        told = true;
        LOGR("autopilot: built-in characters %08X%08X, costumes %08X%08X, arenas %08X%08X open before the unlock",
             kernel::R32(kBuiltIn[0] + 4), kernel::R32(kBuiltIn[0]), kernel::R32(kBuiltIn[1] + 4),
             kernel::R32(kBuiltIn[1]), kernel::R32(kBuiltIn[2] + 4), kernel::R32(kBuiltIn[2]));
    }
    for (uint32_t m : kUnlockMasks) {
        kernel::W32(m, 0xFFFFFFFF);
        kernel::W32(m + 4, 0xFFFFFFFF);
    }
    for (uint32_t m : kBuiltIn) {
        kernel::W32(m, 0xFFFFFFFF);
        kernel::W32(m + 4, 0xFFFFFFFF);
    }
}

void UnlockForPractice() {
    Unlock();
    LOGR("autopilot: practice offers every character, costume and arena");
}

// An online match whose players pick on the select screen: this PC's player picks for their side with their own
// controller (the pads route it to that side's port, LocalPickPort), and the pick (character, costume flags) goes to
// the other game with the HELLOs (net::SetLocalPick); the other side waits for the other player's pick, then the
// autopilot makes it and sets the same costume flags. A spectator's game waits for both players' picks that way.
constexpr int kWaitPick = -2;            // characters[]: the other player has not picked yet
constexpr double kPlayerPickTime = 75;   // seconds this PC's player has, then the autopilot takes the cursor's
constexpr uint32_t kRecordCharacter = 0x54, kRecordCostume = 0x14;
bool g_local_pick_sent = false;
bool g_local_forced = false;             // the time ran out: the autopilot picks for this PC's player
bool g_costume_due[2] = {};              // a player's costume flags from their game, set once the autopilot picked
uint32_t g_their_costume[2] = {};

void ResetPlayerPicks() {
    g_local_pick_sent = g_local_forced = false;
    g_costume_due[0] = g_costume_due[1] = false;
    for (int i = 0; i < 2; i++)
        if (g_req.by_player[i]) g_req.characters[i] = i == g_req.local_side ? -1 : kWaitPick;
}

bool Picked(int side) { return kernel::R32(kRecord[side] + 8) == 2; }

// this PC's player's pick, once made, to the other game; the other player's (a spectator's game: both players'),
// once it came, to the autopilot
void ExchangePicks(double t) {
    const int me = g_req.local_side;
    if (me < 0 && !g_req.watch) return;
    if (me >= 0 && g_req.by_player[me] && !g_local_pick_sent && Picked(me)) {
        const uint32_t words[2] = {kernel::R32(kRecord[me] + kRecordCharacter), kernel::R32(kRecord[me] + kRecordCostume)};
        mk3d::net::SetLocalPick(words, 2);
        g_local_pick_sent = true;
        LOGR("autopilot: this player picked character %u, costume flags %X", words[0], words[1]);
    }
    if (me >= 0 && g_req.by_player[me] && !g_local_forced && !g_local_pick_sent && t > kPlayerPickTime) {
        g_local_forced = true;
        LOGI("autopilot: no pick after %.0f s: this player gets the cursor's", kPlayerPickTime);
    }
    for (int other = 0; other < 2; other++) {
        if (other == me) continue;
        if (g_req.by_player[other] && g_req.characters[other] == kWaitPick) {
            uint32_t words[4] = {};
            int n = 4;
            const bool in = g_req.watch ? mk3d::net::WatchPick(other, words, n) : mk3d::net::PeerPick(words, n);
            if (in && n > 0 && SlotOf(int(words[0])) >= 0) {
                g_req.characters[other] = int(words[0]);
                g_their_costume[other] = n > 1 ? words[1] : 0;
                g_costume_due[other] = true;
                LOGR("autopilot: P%d picked character %u, costume flags %X", other + 1, words[0],
                     g_their_costume[other]);
            }
        }
        if (g_costume_due[other] && Picked(other)) {
            kernel::W32(kRecord[other] + kRecordCostume, g_their_costume[other]);
            g_costume_due[other] = false;
        }
    }
}

// slot of a character in the current grid, -1 when it is not there
int SlotOf(int character) {
    bool chess = kernel::R32(kGridKind) == 2;
    uint32_t grid = chess ? kChessGrid : kGrid;
    int slots = chess ? 12 : 24;
    for (int s = 0; s < slots; s++)
        if (int32_t(kernel::R32(grid + uint32_t(s) * 0x28)) == character) return s;
    return -1;
}

// the d-pad press that takes a cursor one step from slot cur toward slot target in a grid width slots wide
uint32_t Steer(int cur, int target, int width) {
    int r = cur / width, c = cur % width, tr = target / width, tc = target % width;
    if (r != tr) return tr > r ? 0x02 : 0x01;  // DOWN / UP
    return tc > c ? 0x08 : 0x04;               // RIGHT / LEFT
}

// whether a port's chess team has all five fighters
bool ChessTeamFull(int port) {
    uint32_t block = ChessTeams();
    if (!block) return false;
    for (uint32_t i = 0; i < 5; i++)
        if (kernel::R32(block + (port ? 0x2C : 0xC) + 4 * i) == kChessEmpty) return false;
    return true;
}

// Chess Kombat's team (leader, champion, sorcerer, shifter, grunt; the select state stays 1 throughout): the leader
// as asked, then every slot of the grid in turn, wrapping: steer to it and A (each team place takes only some
// fighters, shown as silhouettes otherwise, and A on those does nothing). A full team asks "save team to profile?"
// with NO chosen: A. Nothing once the port's state leaves 1 (it is 2 when done).
void ChessDecide(int port, ChessPort& c) {
    c.buttons = 0;
    c.a = 0;
    if (kernel::R32(kRecord[port] + 8) != 1) return;
    if (ChessTeamFull(port)) {
        c.a = 0xFF;
        return;
    }
    int cur = int(kernel::R32(kCursor[port]));
    if (!c.leader) {
        int target = g_req.characters[port] >= 0 ? SlotOf(g_req.characters[port]) : -1;
        if (target >= 0 && cur != target) {
            c.buttons = Steer(cur, target, 8);
            return;
        }
        c.a = 0xFF;
        c.leader = true;
        return;
    }
    if (cur == c.probe) {
        c.a = 0xFF;
        c.probe = (c.probe + 1) % 24;
        return;
    }
    c.buttons = Steer(cur, c.probe, 8);
}

// one port's character select: steer to the slot, then confirm. In practice (game mode 4) port 0 picks both, its own
// fighter and then the dummy with the second cursor
void Select(int port, input::Pad& pad, double t) {
    if (port > 1) return;
    pad.connected = true;
    int side = port;
    if (GameMode() == 4) {
        if (port != 0) return;
        side = kernel::R32(kRecord[0] + 8) == 2 ? 1 : 0;
    }
    if (kernel::R32(kRecord[side] + 8) == 2) return;  // picked
    if (side == g_req.local_side && g_req.by_player[side] && !g_local_forced) return;  // this PC's player picks
    if (g_req.characters[side] == kWaitPick) {  // the other player has not picked: keep the screen awake
        if (Pulse(t, 8.0, 0.08)) pad.buttons |= (long(t / 8) & 1) ? 0x04 : 0x08;  // LEFT / RIGHT
        return;
    }
    if (!Pulse(t, 0.25, 0.08)) return;
    int target = g_req.characters[side] >= 0 ? SlotOf(g_req.characters[side]) : -1;
    int cur = int(kernel::R32(kCursor[side]));
    int width = kernel::R32(kGridKind) == 2 ? 6 : 8;
    if (target < 0 || target == cur) {
        pad.analog[0] = 0xFF;  // A
        return;
    }
    pad.buttons |= Steer(cur, target, width);
}

}  // namespace

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

int ActionForMode(const std::string& mode) { return ActionFor(mode); }

void Rematch() {
    std::lock_guard<std::mutex> lock(g_lock);
    g_base_fights = FightSetups();
    g_base_matches = MatchStarts();
    ResetPlayerPicks();  // players who pick on the select screen pick again
    Enter(Phase::Select);
}

int LocalPickPort() {
    std::lock_guard<std::mutex> lock(g_lock);
    const int me = g_req.local_side;
    if (g_phase != Phase::Select || me < 0 || !g_req.by_player[me] || g_local_forced || Since() < 1.5) return -1;
    if (kernel::R32(kMainTask) != kSelectTask || Picked(me)) return -1;  // not up, or picked
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
    if (g_phase == Phase::Off || g_phase == Phase::Done || g_phase == Phase::Hold) return;
    double t = Since();
    switch (g_phase) {
        case Phase::ToMenu:
            if (MainMenus() > g_base_menus) {
                Enter(g_wait_for_request && !g_have_request ? Phase::Hold : Phase::Settle);
                if (g_phase == Phase::Hold) LOGI("autopilot: at the hold point");
                return;
            }
            if (g_base_menus == 0 && port == 0 && Pulse(t, 0.7, 0.1)) pad.buttons |= 0x10;  // START (boot only)
            return;
        case Phase::Settle:
            if (t > 1.5) {
                // asked here, not at start: the other file's MKD_ACTION default may initialise after this one
                RequestAction(g_req.action == 6 ? -1 : g_req.action);
                for (int i = 0; i < 2; i++) g_req.characters[i] = CharacterId(g_req.names[i]);
                ResetPlayerPicks();
                if (g_req.unlock_all) Unlock();
                if (g_training) UnlockForPractice();
                g_base_actions = MenuActions();
                Enter(Phase::Choose);
            }
            return;
        case Phase::Choose:
            if (MenuActions() > g_base_actions) {
                if (!Selects(g_req.action)) {
                    LOGI("autopilot: in menu action %u", kernel::R32(kMenuAction));
                    Enter(Phase::Done);
                    return;
                }
                g_base_fights = FightSetups();
                g_base_matches = MatchStarts();
                Enter(Phase::Select);
                return;
            }
            // START passes the profile screen the menu task opens with, A confirms menu items: in turn
            if (port == 0 && Pulse(t, 0.7, 0.1)) {
                if (int(t / 0.7) & 1) pad.analog[0] = 0xFF;  // A
                else pad.buttons |= 0x10;                    // START
            }
            return;
        case Phase::Select: {
            if (g_training) {  // the player's own picks from here
                LOGR("autopilot: practice's select screen, the player picks");
                Enter(Phase::Done);
                return;
            }
            if (g_hold_at_select) return;
            // players picking on the select screen may take their time (75 s each, after that their cursor's)
            if (FightSetups() > g_base_fights || MatchStarts() > g_base_matches || t > (PlayersPick() ? 240 : 90)) {
                Enter(Phase::Done);
                return;
            }
            if (t < 1.5) return;  // the select screen fades in
            if (g_req.unlock_all) Unlock();
            if (port == 0) ExchangePicks(t);
            if (kernel::R32(kGridKind) == 1 && port <= 1) {
                ChessPort& c = g_chess[port];
                pad.connected = true;
                long cycle = long(t / 0.25);
                if (cycle != c.cycle) {
                    c.cycle = cycle;
                    ChessDecide(port, c);
                }
                if (Pulse(t, 0.25, 0.08)) {
                    pad.buttons |= c.buttons;
                    if (c.a) pad.analog[0] = c.a;
                }
                return;
            }
            bool picked = kernel::R32(kRecord[0] + 8) == 2 && kernel::R32(kRecord[1] + 8) == 2;
            if (!picked) {
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

}  // namespace mkd::autopilot
