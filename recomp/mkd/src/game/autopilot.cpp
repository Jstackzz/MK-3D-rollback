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
uint32_t Steer(int cur, int target, int width);

bool PlayersPick() { return g_req.by_player[0] || g_req.by_player[1]; }

// Every character, alternate costume and arena, for as long as the game runs (online matches and practice): the
// masks the checks read and the built-in ones the checks OR in and every rebuild of the masks starts from
// (sub_001726E0), so a profile's saved unlocks (its own bits) stay its own. A costume a player picks on the select
// screen goes to the other game with the pick (ExchangePicks). Shujinko's moves too (g_moves_open, the move gate).
bool g_moves_open = false;

void Unlock() {
    g_moves_open = true;
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
// controller (the pads route it to that side's port, LocalPickPort), and where their cursor is, then their pick
// (character, costume flags, the arena they chose), goes to the other game with the HELLOs (net::SetLocalPick); there
// the autopilot steers that side's cursor after it, then makes the pick and sets the same costume flags. A spectator's
// game follows both players' cursors and picks that way. Puzzle Kombat's fighter goes the same way. Nobody's time runs
// out: the select screen never leaves by itself (2 minutes without a press tried), so nobody's pick is made for them
// and the waiting side's cursor stays where its player's is (0.1.17 took the cursor's character after 75 s and moved
// the waiting side's cursor every 8 s). The character in the player record (+0x54) is the one under the cursor at
// every move (the cursor setter sub_0017ACA0 writes both), so the pick never lags the cursor as Armageddon's did.
// Chess Kombat's pick is a whole team (leader, champion, sorcerer, shifter, grunt), sent once complete: the other
// side's team is filled only then, from its leader on (each game keeps its own side's team, and the meeting point
// gives each game the other's: netgame Agree); its cursor is not followed.
//
// The arena: the versus select screen keeps an arena choice of its own (0x62031C, linkgame.cpp), which the screen
// offers only in an Xbox Live match (its chooser, sub_0017BF70, opens from the screen's events 0x92828 / 0x92829,
// which no button raises in a launcher match); whatever it holds when a player picks goes with the pick, and both
// games fight on the arena the two choices give (linkgame.cpp AgreedArena).
constexpr int kWaitPick = -2;            // characters[]: the other player has not picked yet
constexpr uint32_t kRecordCharacter = 0x54, kRecordCostume = 0x14;
constexpr uint32_t kArenaChoice = 0x62031C;  // -1 none; the fight's arena setup takes 0 to 0x22 (sub_000834C0)
constexpr int kArenaChoices = 0x23;
// A cursor in the pick words: before the pick, a player's game sends where its player's cursor is as one word, the
// slot tagged so that no character id can be taken for it (as Armageddon's)
constexpr uint32_t kCursorWord = 0xC5000000u, kCursorTagMask = 0xFF000000u, kNoArena = 0xFFFFFFFFu;
// One press a cycle for the side a player picks on their own PC: toward their cursor while they choose, then to their
// pick and A on it. Quicker than the launcher's picks (0.25 s), so the cursor keeps up with theirs.
constexpr double kFollowCycle = 0.1, kFollowPress = 0.05;
bool g_local_pick_sent = false;
int g_sent_cursor = -1;                  // this PC's player's cursor as last sent
int g_their_cursor[2] = {-1, -1};        // the other player's (a spectator's game: both players') as it came
bool g_costume_due[2] = {};              // a side's costume flags to set once the autopilot picked it: a player's from
uint32_t g_their_costume[2] = {};        // their game, or the launcher's (a fighter a ranked set's winner keeps)
uint32_t g_their_team[2][5] = {};        // a player's Chess Kombat team from their game, in the order they picked it
bool g_their_team_in[2] = {};
int g_agreed[2] = {-1, -1};              // the picks both games fight with: this player's as sent, the other's as it came
uint32_t g_agreed_costume[2] = {};
int g_agreed_arena[2] = {-1, -1};        // the arena each player chose with their pick (-1 none)
struct Follow {
    long cycle = -1;
    uint32_t buttons = 0;
    uint8_t a = 0;
};
Follow g_follow[2];

void ResetPlayerPicks() {
    g_local_pick_sent = false;
    g_sent_cursor = -1;
    for (int i = 0; i < 2; i++) {
        g_costume_due[i] = g_their_team_in[i] = false;
        g_their_cursor[i] = g_agreed[i] = g_agreed_arena[i] = -1;
        g_agreed_costume[i] = 0;
        g_follow[i] = Follow{};
        if (g_req.by_player[i]) g_req.characters[i] = i == g_req.local_side ? -1 : kWaitPick;
        // a side the launcher picked with its costume flags (the fighter a ranked set's winner keeps, as they won with
        // it): both games pick it and give it those flags, and fight with both whatever the select screen did with them
        // (a mirror pick made in another order in each game)
        if (!g_req.by_player[i] && g_req.costumes[i] >= 0 && g_req.characters[i] >= 0) {
            g_their_costume[i] = g_agreed_costume[i] = uint32_t(g_req.costumes[i]);
            g_costume_due[i] = true;
            g_agreed[i] = g_req.characters[i];
            LOGR("autopilot: P%d keeps character %d, costume flags %X", i + 1, g_agreed[i], g_agreed_costume[i]);
        }
    }
}

int ArenaWord(uint32_t word) { return int32_t(word) >= 0 && int32_t(word) < kArenaChoices ? int(word) : -1; }

bool Picked(int side) { return kernel::R32(kRecord[side] + 8) == 2; }

// Chess Kombat's team select (the grid kind; Puzzle Kombat's is 2)
bool ChessSelect() { return kernel::R32(kGridKind) == 1; }

bool ChessTeamFull(int port);

// what this PC's player picked, for the other game: a fighter, its costume flags and the arena they chose (none:
// kNoArena), or a Chess Kombat team
int LocalPickWords(int me, uint32_t* words) {
    if (ChessSelect()) {
        const uint32_t block = ChessTeams();
        for (uint32_t i = 0; i < 5; i++) words[i] = block ? kernel::R32(block + (me ? 0x2C : 0xC) + 4 * i) : kChessEmpty;
        return 5;
    }
    words[0] = kernel::R32(kRecord[me] + kRecordCharacter);
    words[1] = kernel::R32(kRecord[me] + kRecordCostume);
    const int arena = ArenaWord(kernel::R32(kArenaChoice));
    words[2] = arena >= 0 ? uint32_t(arena) : kNoArena;
    return 3;
}

// the press toward the other player's cursor as it is on their screen (none before it came, or once there)
void FollowCursor(int side, Follow& f) {
    f.buttons = 0;
    f.a = 0;
    const int cur = int(kernel::R32(kCursor[side])), to = g_their_cursor[side];
    const int width = kernel::R32(kGridKind) == 2 ? 6 : 8, slots = width == 6 ? 12 : 24;
    if (cur < 0 || cur >= slots || to < 0 || to >= slots || to == cur) return;
    f.buttons = Steer(cur, to, width);
}

// this PC's player's cursor while they choose, then their pick, to the other game; the other player's (a spectator's
// game: both players'), as they come, to the autopilot
void ExchangePicks() {
    const int me = g_req.local_side;
    if (me < 0 && !g_req.watch) return;
    const bool chess = ChessSelect();
    if (me >= 0 && g_req.by_player[me] && !g_local_pick_sent && (Picked(me) || (chess && ChessTeamFull(me)))) {
        uint32_t words[5] = {};
        const int n = LocalPickWords(me, words);
        mk3d::net::SetLocalPick(words, n);
        g_local_pick_sent = true;
        if (chess) {
            LOGR("autopilot: this player's team %u %u %u %u %u", words[0], words[1], words[2], words[3], words[4]);
        } else {
            g_agreed[me] = int(words[0]);
            g_agreed_costume[me] = words[1];
            g_agreed_arena[me] = ArenaWord(words[2]);
            LOGR("autopilot: this player picked character %u, costume flags %X, arena %d", words[0], words[1],
                 g_agreed_arena[me]);
        }
    } else if (me >= 0 && g_req.by_player[me] && !g_local_pick_sent && !chess && !Picked(me)) {
        const int at = int(kernel::R32(kCursor[me]));
        if (at >= 0 && at < 24 && at != g_sent_cursor) {
            const uint32_t word = kCursorWord | uint32_t(at);
            mk3d::net::SetLocalPick(&word, 1);
            g_sent_cursor = at;
        }
    }
    for (int other = 0; other < 2; other++) {
        if (other == me) continue;
        if (g_req.by_player[other] && g_req.characters[other] == kWaitPick) {
            uint32_t words[8] = {};
            int n = 8;
            const bool in = g_req.watch ? mk3d::net::WatchPick(other, words, n) : mk3d::net::PeerPick(words, n);
            if (in && n > 0 && (words[0] & kCursorTagMask) == kCursorWord) {
                if (!chess) g_their_cursor[other] = int(words[0] & 0xFFu);
            } else if (in && n >= 5 && chess) {
                // picked again here place by place as that player did: the select screen ends as theirs did (the
                // player record keeps the last pick, which the state checks read)
                for (int i = 0; i < 5; i++) g_their_team[other][i] = words[i];
                g_their_team_in[other] = true;
                g_req.characters[other] = int(words[0]);
                LOGR("autopilot: P%d's team %u %u %u %u %u", other + 1, words[0], words[1], words[2], words[3],
                     words[4]);
            } else if (in && n > 0 && SlotOf(int(words[0])) >= 0) {
                g_req.characters[other] = int(words[0]);
                g_their_costume[other] = n > 1 ? words[1] : 0;
                g_costume_due[other] = true;
                g_agreed[other] = int(words[0]);
                g_agreed_costume[other] = g_their_costume[other];
                g_agreed_arena[other] = n > 2 ? ArenaWord(words[2]) : -1;
                LOGR("autopilot: P%d picked character %u, costume flags %X, arena %d", other + 1, words[0],
                     g_their_costume[other], g_agreed_arena[other]);
            }
        }
    }
    for (int side = 0; side < 2; side++) {
        if (g_costume_due[side] && Picked(side)) {
            kernel::W32(kRecord[side] + kRecordCostume, g_their_costume[side]);
            g_costume_due[side] = false;
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
    if (g_their_team_in[port]) {  // the other player's team: its first place still empty, as they picked it
        const uint32_t block = ChessTeams();
        int place = 0;
        while (place < 5 && block && kernel::R32(block + (port ? 0x2C : 0xC) + 4 * uint32_t(place)) != kChessEmpty)
            place++;
        const int target = place < 5 ? SlotOf(int(g_their_team[port][place])) : -1;
        if (target >= 0 && cur != target) c.buttons = Steer(cur, target, 8);
        else c.a = 0xFF;
        return;
    }
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
    if (side == g_req.local_side && g_req.by_player[side]) return;  // this PC's player picks
    int cur = int(kernel::R32(kCursor[side]));
    int width = kernel::R32(kGridKind) == 2 ? 6 : 8;
    if (g_req.by_player[side]) {  // a player on their own PC: after their cursor, then to their pick and A on it
        Follow& f = g_follow[side];
        const long cycle = long(t / kFollowCycle);
        if (cycle != f.cycle) {
            f.cycle = cycle;
            if (g_req.characters[side] == kWaitPick) {
                FollowCursor(side, f);
            } else {
                const int target = SlotOf(g_req.characters[side]);
                f.buttons = target >= 0 && target != cur ? Steer(cur, target, width) : 0;
                f.a = target < 0 || target == cur ? 0xFF : 0;
            }
        }
        if (Pulse(t, kFollowCycle, kFollowPress)) {
            pad.buttons |= f.buttons;
            if (f.a) pad.analog[0] = f.a;
        }
        return;
    }
    if (!Pulse(t, 0.25, 0.08)) return;
    int target = g_req.characters[side] >= 0 ? SlotOf(g_req.characters[side]) : -1;
    if (target < 0 || target == cur) {
        pad.analog[0] = 0xFF;  // A
        return;
    }
    pad.buttons |= Steer(cur, target, width);
}

}  // namespace

bool MovesOpen() { return g_moves_open; }

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

bool AgreedPick(int side, int& character, uint32_t& costume, int& arena) {
    std::lock_guard<std::mutex> lock(g_lock);
    if (side < 0 || side > 1 || g_agreed[side] < 0) return false;
    character = g_agreed[side];
    costume = g_agreed_costume[side];
    arena = g_agreed_arena[side];
    return true;
}

int LocalPickPort() {
    std::lock_guard<std::mutex> lock(g_lock);
    const int me = g_req.local_side;
    if (g_phase != Phase::Select || me < 0 || !g_req.by_player[me] || Since() < 1.5) return -1;
    // Kombat's select screen, Puzzle Kombat's, Chess Kombat's team select
    const uint32_t task = kernel::R32(kMainTask);
    if ((task != kSelectTask && task != kPuzzleSelectTask && task != kChessSelectTask) || Picked(me)) return -1;
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
            // the launcher's picks take seconds; players take as long as they like (the launcher ends a match that
            // does not start)
            if (FightSetups() > g_base_fights || MatchStarts() > g_base_matches || (!PlayersPick() && t > 90)) {
                Enter(Phase::Done);
                return;
            }
            if (t < 1.5) return;  // the select screen fades in
            if (g_req.unlock_all) Unlock();
            if (port == 0) ExchangePicks();
            if (ChessSelect() && port <= 1) {
                ChessPort& c = g_chess[port];
                pad.connected = true;
                // this PC's player builds their own team; the other player's side is filled once theirs is complete
                if (port == g_req.local_side && g_req.by_player[port]) return;
                if (g_req.characters[port] == kWaitPick) return;
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
