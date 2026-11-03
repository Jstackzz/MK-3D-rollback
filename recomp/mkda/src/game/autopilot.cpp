// Autopilot: drives the front end to a chosen mode and characters by pressing buttons the way a player would,
// watching game state instead of trusting fixed timings (boot and load times vary from run to run).
//
//   MKDA_AUTOPILOT=versus|arcade|practice    or konquest|krypt|profile|options|kontent, which have no character
//                                            select: the autopilot is done once the menu took the item; or menu:
//                                            done at the main menu (KONQUEST and THE KRYPT take a loaded profile)
//   MKDA_P1, MKDA_P2=<character>             who each side picks (default: what the cursor starts on): the game's
//                                            name, the launcher's roster id (SubZero, BoRaiCho) or the index; every
//                                            character, costume and arena is unlocked when either is given, or with
//                                            MKDA_UNLOCK_ALL=1 (a launcher match always)
//   MKDA_AUTOPILOT_HOLD=1                    stop at character select (for the debug monitor)
//   MK3D_TRAINING=1                          to practice's select screen, every character, costume and arena
//                                            unlocked; the player picks there (the launcher's practice: its own
//                                            button, or while a search waits)
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
// the profiles' mask 0x421530 or the built-in one 0x273580 (alternate costumes 0x421534 / 0x2EA844, arenas 0x421538 /
// 0x273584); the unlock sets the built-in ones, which nothing saves.
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
constexpr uint32_t kUnlockDefaults[3] = {0x273580, 0x2EA844, kArenaDefaults};  // characters, costumes, arenas
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
    bool x = false;
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

bool PlayersPick() { return g_req.by_player[0] || g_req.by_player[1]; }

// Every character, alternate costume and arena, for as long as the game runs (online matches and practice): the
// built-in masks, which the checks OR the profiles' into and nothing saves. A costume a player picks on the select
// screen goes to the other game with the pick (ExchangePicks).
void Unlock() {
    static bool told = false;  // what was open before, once: which characters, costumes and arenas this opens
    if (!told) {
        told = true;
        LOGR("autopilot: built-in characters %08X, costumes %08X, arenas %08X open before the unlock",
             kernel::R32(kUnlockDefaults[0]), kernel::R32(kUnlockDefaults[1]), kernel::R32(kUnlockDefaults[2]));
    }
    for (uint32_t m : kUnlockDefaults) kernel::W32(m, 0xFFFFFFFF);
}

void UnlockForPractice() {
    Unlock();
    LOGR("autopilot: practice offers every character, costume and arena");
}

// An online match whose players pick on the select screen: this PC's player picks for their side with their own
// controller (the pads route it to that side's port, LocalPickPort), and where their cursor is, then their pick
// (character, costume, the arena they chose), goes to the other game with the HELLOs (net::SetLocalPick); there the
// autopilot steers that side's cursor after it, then makes the pick and gives it the same costume. A spectator's game
// follows both players' cursors and picks that way. Each costume is taken once its own game's pick is done and set
// once the other game's pick is: a mirror match only ever turns the costume of the player picking, so both games end
// with each player's own. Nobody's time runs out: the screen's own 2700 frames without a press (linkgame.cpp) are
// held off, nobody's pick is made for them, and the waiting side's cursor stays where its player's is (0.1.17 took the
// cursor's character after 75 s and moved the waiting side's cursor every 8 s). The pick (sub_001112A0) takes the
// character under the cursor as it is confirmed, so it never lags the cursor as Armageddon's did.
//
// The arena: BLACK on the select screen, while a player still chooses, both players are in and no arena choice is
// open, opens theirs (sub_0010EC90: +0x800 in the word that holds their costume, events.h kCostume); LEFT / RIGHT go
// through the arenas (0x2EA658, of the table 0x274398), START takes the one shown (sub_00111F60, sub_001121F0:
// 0x42C754, the engine's chosen arena, which the screen's setup sets to -1) and closes it. Nobody picks while one is
// open (B and a second BLACK do not close it), and the autopilot presses nothing for the other side then; it has P2
// join at once (X, which moves nothing), as the choice needs both players in. What 0x42C754 holds when a player picks
// goes with the pick, and both games fight on the arena the two choices give (linkgame.cpp).
constexpr int kWaitPick = -2;            // characters[]: the other player has not picked yet
// A cursor in the pick words: before the pick, a player's game sends where its player's cursor is as one word, the
// slot tagged so that no character id can be taken for it (as Armageddon's)
constexpr uint32_t kCursorWord = 0xC5000000u, kCursorTagMask = 0xFF000000u, kNoArena = 0xFFFFFFFFu;
constexpr int kArenaChoices = 0x19;      // what the chosen arena takes: 0 to 24 (sub_0006F570)
constexpr uint32_t kArenaOpen = 0x800;
// One press a cycle for the side a player picks on their own PC: toward their cursor while they choose, then to their
// pick and A on it. Quicker than the launcher's picks (0.25 s), so the cursor keeps up with theirs.
constexpr double kFollowCycle = 0.1, kFollowPress = 0.05;
bool g_local_pick_sent = false;
int g_sent_cursor = -1;                  // this PC's player's cursor as last sent
int g_their_cursor[2] = {-1, -1};        // the other player's (a spectator's game: both players') as it came
bool g_costume_due[2] = {};              // a side's costume to set once the autopilot picked it: a player's from their
uint32_t g_their_costume[2] = {};        // game, or the launcher's (a fighter a ranked set's winner keeps)
int g_agreed[2] = {-1, -1};              // the picks both games fight with: this player's as sent, the other's as it came
uint32_t g_agreed_costume[2] = {};
int g_agreed_arena[2] = {-1, -1};        // the arena each player chose with their pick (-1 none)

int ArenaWord(uint32_t word) { return int32_t(word) >= 0 && int32_t(word) < kArenaChoices ? int(word) : -1; }

// a player's arena choice is open on this screen
bool ArenaOpen() {
    for (uint32_t va : kCostume) {
        const uint32_t word = kernel::R32(va);
        if (word && (kernel::R32(word) & kArenaOpen)) return true;
    }
    return false;
}
// MKDA_TEST_COSTUME=<n> (tests): this PC's player's costume once picked, as if picked with the alternate's button
const int g_test_costume = [] {
    const char* s = std::getenv("MKDA_TEST_COSTUME");
    return s ? std::atoi(s) : -1;
}();

uint32_t CostumeOf(int side) { return kernel::R32(kernel::R32(kCostume[side])); }

void ResetPlayerPicks() {
    g_local_pick_sent = false;
    g_sent_cursor = -1;
    for (int i = 0; i < 2; i++) {
        g_costume_due[i] = false;
        g_their_cursor[i] = g_agreed[i] = g_agreed_arena[i] = -1;
        g_agreed_costume[i] = 0;
        if (g_req.by_player[i]) g_req.characters[i] = i == g_req.local_side ? -1 : kWaitPick;
        // a side the launcher picked with its costume (the fighter a ranked set's winner keeps, as they won with it):
        // both games pick it and then give it that costume, whatever a mirror pick made in another order in each game
        // turned it to, and fight with both
        if (!g_req.by_player[i] && g_req.costumes[i] >= 0 && g_req.characters[i] >= 0) {
            g_their_costume[i] = g_agreed_costume[i] = uint32_t(g_req.costumes[i]);
            g_costume_due[i] = true;
            g_agreed[i] = g_req.characters[i];
            LOGR("autopilot: P%d keeps %s (%d), costume %u", i + 1, Name(g_agreed[i]).c_str(), g_agreed[i],
                 g_agreed_costume[i]);
        }
    }
}

// this PC's player's cursor while they choose, then their pick, to the other game; the other player's (a spectator's
// game: both players'), as they come, to the autopilot
void ExchangePicks() {
    const int me = g_req.local_side;
    if (me < 0 && !g_req.watch) return;
    if (me >= 0 && g_req.by_player[me] && !g_local_pick_sent && kernel::R32(kPlayerState[me]) == 3) {
        if (g_test_costume >= 0) kernel::W32(kernel::R32(kCostume[me]), uint32_t(g_test_costume));
        const int arena = ArenaWord(kernel::R32(kNextArena));
        const uint32_t words[3] = {kernel::R32(kCharacter[me]), CostumeOf(me), arena >= 0 ? uint32_t(arena) : kNoArena};
        mk3d::net::SetLocalPick(words, 3);
        g_local_pick_sent = true;
        g_agreed[me] = int(words[0]);
        g_agreed_costume[me] = words[1];
        g_agreed_arena[me] = arena;
        LOGR("autopilot: this player picked %s (%u), costume %u, arena %d", Name(int(words[0])).c_str(), words[0],
             words[1], arena);
    } else if (me >= 0 && g_req.by_player[me] && !g_local_pick_sent && kernel::R32(kPlayerState[me]) == 2) {
        const int at = int(kernel::R32(kCursor[me]));
        if (at >= 0 && at < kGridMax && at != g_sent_cursor) {
            const uint32_t word = kCursorWord | uint32_t(at);
            mk3d::net::SetLocalPick(&word, 1);
            g_sent_cursor = at;
        }
    }
    for (int other = 0; other < 2; other++) {
        if (other == me || !g_req.by_player[other]) continue;
        if (g_req.characters[other] == kWaitPick) {
            uint32_t words[4] = {};
            int n = 4;
            const bool in = g_req.watch ? mk3d::net::WatchPick(other, words, n) : mk3d::net::PeerPick(words, n);
            if (in && n > 0 && (words[0] & kCursorTagMask) == kCursorWord) {
                g_their_cursor[other] = int(words[0] & 0xFFu);
            } else if (in && n > 0 && words[0] < uint32_t(kCharacters)) {
                g_req.characters[other] = int(words[0]);
                g_their_costume[other] = n > 1 ? words[1] : 0;
                g_costume_due[other] = true;
                g_agreed[other] = int(words[0]);
                g_agreed_costume[other] = g_their_costume[other];
                g_agreed_arena[other] = n > 2 ? ArenaWord(words[2]) : -1;
                LOGR("autopilot: P%d picked %s (%u), costume %u, arena %d", other + 1, Name(int(words[0])).c_str(),
                     words[0], g_their_costume[other], g_agreed_arena[other]);
            }
        }
    }
    for (int side = 0; side < 2; side++) {
        if (g_costume_due[side] && kernel::R32(kPlayerState[side]) == 3) {
            kernel::W32(kernel::R32(kCostume[side]), g_their_costume[side]);
            g_costume_due[side] = false;
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

// the press toward the other player's cursor as it is on their screen (none before it came, or once there)
Press FollowCursor(int side) {
    Press p;
    const int cur = int(kernel::R32(kCursor[side])), to = g_their_cursor[side];
    if (cur >= 0 && cur < kGridMax && to >= 0 && to < kGridMax && to != cur) p.buttons = Steer(cur, to);
    return p;
}

// one port's character select; port 0 is P1, port 1 P2 (practice: port 0 picks both, the opponent second)
void Select(int port, input::Pad& pad, double t) {
    if (g_req.by_player[port] && port != g_req.local_side && GameMode() != kModePractice) {
        // a player on their own PC: after their cursor, then to their pick and A on it; P2 joins at once
        Press& p = g_press[port];
        const long cycle = long(t / kFollowCycle);
        if (cycle != p.cycle) {
            p = Press{};
            p.cycle = cycle;
            if (kernel::R32(kPlayerState[port]) != 2 || ArenaOpen()) {
            } else if (port == 1 && kernel::R32(kP2Port) != 1) {
                p.x = true;
            } else if (g_req.characters[port] == kWaitPick) {
                p = FollowCursor(port);
                p.cycle = cycle;
            } else {
                p = Decide(g_req.characters[port], port);
                p.cycle = cycle;
            }
        }
        if (Pulse(t, kFollowCycle, kFollowPress)) {
            pad.buttons |= p.buttons;
            if (p.a) pad.analog[0] = p.a;
            if (p.x) pad.analog[2] = 0xFF;
        }
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
        } else if (port == g_req.local_side && g_req.by_player[port]) {
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
    if (g_phase != Phase::Select || me < 0 || !g_req.by_player[me] || Since() < 1.5) return -1;
    if (kernel::R32(kMainTask) != kSelectTask || kernel::R32(kPlayerState[me]) == 3) return -1;  // not up, or picked
    return me;
}

bool AgreedPick(int side, int& character, uint32_t& costume, int& arena) {
    std::lock_guard<std::mutex> lock(g_lock);
    if (!g_have_request || side < 0 || side > 1 || g_agreed[side] < 0) return false;
    character = g_agreed[side];
    costume = g_agreed_costume[side];
    arena = g_agreed_arena[side];
    return true;
}

bool HoldsIdle() {
    std::lock_guard<std::mutex> lock(g_lock);
    return g_have_request && (g_req.local_side >= 0 || g_req.watch);
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

// The main menu falls back into the attract sequence (its state task 0x238E0: the title, the movies, the demo
// fights) after 2700 frames without a press, as on the console: a launcher's game waiting at its hold point gets there
// (a spectator's between two matches, a lobby whose players take their time), and its next start found no menu to
// take it ("the main menu did not take item 1"). START brings the menu back, as at boot; a start waits for it.
constexpr uint32_t kAttractTask = 0x238E0;
bool BackFromAttract(int port, input::Pad& pad) {
    static bool told = false;
    if (kernel::R32(kMainTask) != kAttractTask) {
        told = false;
        return false;
    }
    if (!told) {
        told = true;
        LOGR("autopilot: the main menu went over to its attract sequence: START, back to the menu");
    }
    if (port == 0 && Pulse(std::chrono::duration<double>(Clock::now().time_since_epoch()).count(), 0.7, 0.1))
        pad.buttons |= kStart;
    return true;
}

void Apply(int port, input::Pad& pad) {
    std::lock_guard<std::mutex> lock(g_lock);
    if (port < 32 && (g_connected >> port & 1)) pad.connected = true;
    if (g_phase == Phase::Hold) {
        BackFromAttract(port, pad);
        return;
    }
    if (g_phase == Phase::Settle && BackFromAttract(port, pad)) {
        g_since = Clock::now();  // the settle's wait starts once the menu is back
        return;
    }
    if (g_phase == Phase::Off || g_phase == Phase::Done) return;
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
                if (g_training) UnlockForPractice();
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
            // players picking on the select screen take the time they want
            if (FightSetups() > g_base_fights || (!PlayersPick() && t > 120)) {
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
            if (port == 0) ExchangePicks();
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
