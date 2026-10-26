// Autopilot: drives the front end to a chosen mode and characters by pressing buttons the way a player would,
// watching game state instead of trusting fixed timings (boot and load times vary from run to run).
//
//   MKDA_AUTOPILOT=versus|arcade|practice
//   MKDA_P1, MKDA_P2=<character>             who each side picks (default: what the cursor starts on): the game's
//                                            name, the launcher's roster id (SubZero, BoRaiCho) or the index; all
//                                            characters are unlocked when either is given, or with MKDA_UNLOCK_ALL=1
//   MKDA_AUTOPILOT_HOLD=1                    stop at character select (for the debug monitor)
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
#include "kernel/kernel.h"

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
    LOGI("autopilot: %s", names[int(p)]);
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

int ItemFor(int mode) {
    switch (mode) {
        case int(kModeArcade): return 0;
        case int(kModeVersus): return 1;
        case int(kModePractice): return 2;
        default: return -1;
    }
}

const bool g_loaded = [] {
    const char* s = std::getenv("MKDA_AUTOPILOT");
    if (!s) return false;
    g_req.mode = ModeByName(s);
    if (ItemFor(g_req.mode) < 0) {
        LOGE("autopilot: unknown mode %s (versus, arcade, practice)", s);
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

// a short press every cycle seconds, the first press seconds of each
bool Pulse(double t, double cycle, double press) { return t - int(t / cycle) * cycle < press; }

void Unlock() {
    for (uint32_t m : kUnlockDefaults) kernel::W32(m, 0xFFFFFFFF);
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
                }
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
                else p.a = 0xFF;
            }
            if (Pulse(t, 0.25, 0.08)) {
                pad.buttons |= p.buttons;
                if (p.a) pad.analog[0] = p.a;
            }
            return;
        }
        case Phase::Select: {
            if (g_hold_at_select) return;
            if (FightSetups() > g_base_fights || t > 120) {
                if (FightSetups() <= g_base_fights) LOGE("autopilot: no fight after 120 s at character select");
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
