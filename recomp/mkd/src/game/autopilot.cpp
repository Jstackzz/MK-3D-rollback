// Autopilot: drives the front end to a chosen mode and characters by pressing buttons the way a player would,
// watching game state instead of trusting fixed timings (boot and load times vary from run to run).
//
//   MKD_AUTOPILOT=versus|chess|puzzle|arcade|practice
//   MKD_P1, MKD_P2=<character name or id>   who each side picks (default: what the cursor starts on); all
//                                            characters are unlocked when either is given, or with MKD_UNLOCK_ALL=1
//   MKD_AUTOPILOT_HOLD=1                     stop at character select (for the debug monitor)
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
// 0x601E60 (0x601E68 alternates, 0x601E90 puzzle) or the built-in defaults at 0x3F8FF0.
#include "game/autopilot.h"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "core/log.h"
#include "game/events.h"
#include "kernel/kernel.h"

namespace mkd::autopilot {
namespace {

using Clock = std::chrono::steady_clock;

enum class Phase { Off, ToMenu, Settle, Hold, Choose, Select, Done };

constexpr uint32_t kCursor[2] = {0x471B1C, 0x471B18};
constexpr uint32_t kRecord[2] = {kP1Record, kP2Record};
constexpr uint32_t kGrid = 0x3F9210, kChessGrid = 0x3F95D0, kGridKind = 0x471B24;
constexpr uint32_t kUnlockMasks[] = {0x601E60, 0x601E68, 0x601E90};
constexpr uint32_t kNames = 0x3F89E0;  // character names, 16 bytes per character, name pointer first

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
const bool g_hold_at_select = std::getenv("MKD_AUTOPILOT_HOLD") != nullptr;

double Since() { return std::chrono::duration<double>(Clock::now() - g_since).count(); }

void Enter(Phase p) {
    if (p == Phase::Select) g_chess[0] = g_chess[1] = ChessPort{};
    g_phase = p;
    g_since = Clock::now();
    static const char* names[] = {"off", "to menu", "settle", "hold", "choose", "select", "done"};
    LOGI("autopilot: %s", names[int(p)]);
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
    } modes[] = {{"versus", 7}, {"kombat", 7}, {"chess", 8}, {"puzzle", 9}, {"arcade", 6}, {"practice", 0xB}};
    for (auto& m : modes)
        if (mode == m.name) return m.action;
    return -1;
}

const bool g_loaded = [] {
    const char* s = std::getenv("MKD_AUTOPILOT");
    if (!s) return false;
    g_req.action = ActionFor(s);
    if (g_req.action < 0) {
        LOGE("autopilot: unknown mode %s (versus, chess, puzzle, arcade, practice)", s);
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

void Unlock() {
    for (uint32_t m : kUnlockMasks) {
        kernel::W32(m, 0xFFFFFFFF);
        kernel::W32(m + 4, 0xFFFFFFFF);
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

// one port's character select: steer to the slot, then confirm
void Select(int port, input::Pad& pad, double t) {
    if (port > 1) return;
    pad.connected = true;
    if (kernel::R32(kRecord[port] + 8) == 2) return;  // picked
    if (!Pulse(t, 0.25, 0.08)) return;
    int target = g_req.characters[port] >= 0 ? SlotOf(g_req.characters[port]) : -1;
    int cur = int(kernel::R32(kCursor[port]));
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
    Enter(Phase::Select);
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
                if (g_req.unlock_all) Unlock();
                g_base_actions = MenuActions();
                Enter(Phase::Choose);
            }
            return;
        case Phase::Choose:
            if (MenuActions() > g_base_actions) {
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
            if (g_hold_at_select) return;
            if (FightSetups() > g_base_fights || MatchStarts() > g_base_matches || t > 90) {
                Enter(Phase::Done);
                return;
            }
            if (t < 1.5) return;  // the select screen fades in
            if (g_req.unlock_all) Unlock();
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
