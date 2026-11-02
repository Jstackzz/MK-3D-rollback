// Autopilot: drives the front end to a chosen mode and characters by pressing buttons the way a player would,
// watching game state instead of trusting fixed timings (boot and load times vary from run to run).
//
//   MKA_AUTOPILOT=versus|arcade|practice|motor   (motor: only as far as Motor Kombat's own select screen), or
//                                konquest|kontent|kreate or a main menu action's number (0xE): those have no select
//                                screen, so the autopilot is done once the menu took it
//   MKA_P1, MKA_P2=<character>   who each side picks (default: what the cursor starts on): the game's name ("SUB-ZERO"),
//                                the launcher's roster id (launcher/data/mka.json: "SubZero", "JohnnyCage") or the
//                                index; every character, costume and arena is unlocked when either is given, or with
//                                MKA_UNLOCK_ALL=1 (a launcher match always)
//   MKA_AUTOPILOT_HOLD=1         stop at character select
//   MK3D_TRAINING=1              to practice's select screen, every character, costume and arena unlocked (the created
//                                fighter as the profile has it); the player picks there (the launcher's practice: its
//                                own button, or while a search waits)
//   MKA_TRACK=<n>                Motor Kombat's track as its select numbers them (43 the first, the default)
//
//   1. START every 0.7 s until the main menu task starts (skips the movies and the title; its first start opens the
//      profile screen)
//   2. after the menu settles (the hold point a launcher would wait at), START and A in turn until the menu confirms
//      an item (the profile screen, then ARCADE, the first item); versus, practice and motor redirect that
//      confirmation to their action with both players taking part (events.cpp, sub_001D9530), then nothing is pressed
//      until the select screen is up
//   3. character select: each port steers its cursor to its character's slot, deciding one press at a time from the
//      slot it reads back, and confirms with A; in practice port 0 picks both (P1, then the dummy). The fight follows
//      by itself.
//
// Character select (task 0x2057E0, research/mka/notes.md): the cursor slots are in the select screen's block
// (events.h SelectBlock), 64 slots: 0x561B08 + slot * 0x10 gives the character, 0x561F08 + slot * 0x10 the slots
// LEFT, RIGHT, UP and DOWN go to (a move goes on past characters that cannot be picked). A player record's state +8
// is 2 once its pick is confirmed (3 for practice's dummy). A character can be picked when its bit is set in the
// 64-bit masks 0x7363C8 (0x7363D0 alternates) or the defaults 0x561968 (0x5BD988); the created fighter (0x3E) needs
// a profile. The masks are rebuilt from the defaults and the profiles' own bits; only the profiles' bits are saved.
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

namespace mka::autopilot {
namespace {

using Clock = std::chrono::steady_clock;

enum class Phase { Off, ToMenu, Settle, Hold, Choose, Select, Kart, Done };

constexpr uint32_t kRecord[2] = {kP1Record, kP2Record};
constexpr uint32_t kGrid = 0x561B08, kMoves = 0x561F08;  // 0x10 bytes a slot
constexpr int kSlots = 64;
// 64-bit unlock masks: characters, alternate costumes, arenas; the built-in ones in the same order (the rebuild
// 0x1FC5xx copies them in, then ORs in each profile's)
constexpr uint32_t kUnlockMasks[] = {0x7363C8, 0x7363D0, 0x7363D8};
constexpr uint32_t kDefaultMask = 0x561968;
constexpr uint32_t kBuiltIn[] = {kDefaultMask, 0x5BD988, 0x548A40};
constexpr int kCreatedFighter = 0x3E;
constexpr uint16_t kMoveButton[4] = {0x04, 0x08, 0x01, 0x02};  // LEFT, RIGHT, UP, DOWN: the moves table's order
constexpr uint16_t kStart = 0x10;

std::mutex g_lock;
Phase g_phase = Phase::Off;
Clock::time_point g_since;
uint32_t g_base_actions = 0, g_base_fights = 0, g_base_menus = 0, g_base_matches = 0;
bool g_confirmed = false;  // Choose: the menu confirmed an item, waiting for the select screen
Clock::time_point g_confirmed_at;
// one port's select: decided once a press cycle (what the cursor did is known by then), held for the cycle's press
struct SelectPort {
    long cycle = -1;
    uint16_t buttons = 0;
    uint8_t a = 0;
    int last_cursor = -1;
};
SelectPort g_select[2];
Request g_req;
bool g_have_request = false;
bool g_wait_for_request = false;
// MK3D_TRAINING=1, the launcher's practice while a search waits: to practice's select screen, where the player picks
// both fighters with their own controller as in the game's own practice
const bool g_training = std::getenv("MK3D_TRAINING") != nullptr;
const bool g_hold_at_select = std::getenv("MKA_AUTOPILOT_HOLD") != nullptr;
uint32_t g_track_seed = 0;  // Motor Kombat: the launcher's seed and stage for the track
std::string g_track_stage;

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

// MKA_TEST_ONE_HIT=1 (tests): in arcade the opponent, P2, is kept at 1% health and P1 at half or more, so P2 goes
// down from the first hit (or loses on time) and a scripted run gets through the whole ladder to its bosses and ending
const bool g_one_hit = [] {
    if (!std::getenv("MKA_TEST_ONE_HIT")) return false;
    OnFrameStart([](uint64_t) {
        if (GameMode() != 0) return;  // arcade
        ClampHealth(kP1Health, 0.5f, 1.0f);
        ClampHealth(kP2Health, 0.0f, 0.01f);
    });
    return true;
}();

// Motor Kombat's select (task 0x1052B0, screen "common/kart/kart_main"): first the track (0x831A58: 43 for the first,
// RIGHT adds one), A; then a quadrant per player: P1's is open, P2 joins with START (record state 0 -> 1); RIGHT
// moves a player's kart on (0x82ECA0 + player * 0xC), A readies it (state 2). The race starts once nobody is still
// choosing, so P1 readies only after P2 has joined.
constexpr uint32_t kFirstTrack = 43, kTracks = 5;  // the select numbers the tracks from 43 (kTrackChoice)
enum class KartStep { Track, Karts };
KartStep g_kart_step = KartStep::Track;
uint32_t g_track_target = kFirstTrack, g_track_confirmed = 0;
int g_track_presses = 0;
Clock::time_point g_kart_since;
int g_kart_target[2] = {0, 1};
struct KartPort {
    long cycle = -1;
    uint16_t buttons = 0;
    uint8_t a = 0;
};
KartPort g_kart[2];

void EnterKart();

void Enter(Phase p) {
    if (p == Phase::Select) g_select[0] = g_select[1] = SelectPort{};
    if (p == Phase::Kart) EnterKart();
    if (p == Phase::Choose) g_confirmed = false;
    g_phase = p;
    g_since = Clock::now();
    static const char* names[] = {"off", "to menu", "settle", "hold", "choose", "select", "kart select", "done"};
    LOGR("autopilot: %s", names[int(p)]);  // a few lines a match, in a player's log too: how a match got started
}

std::string Norm(const std::string& s) {
    std::string o;
    for (char c : s)
        if (std::isalnum(static_cast<unsigned char>(c))) o += char(std::toupper(static_cast<unsigned char>(c)));
    return o;
}

int ActionFor(const std::string& mode) {
    struct {
        const char* name;
        int action;
    } modes[] = {{"versus", kActionVersus},     {"kombat", kActionVersus}, {"arcade", kActionArcade},
                 {"practice", kActionPractice}, {"motor", kActionKart},    {"konquest", kActionKonquest},
                 {"kontent", 0x15},             {"kreate", 0x16}};
    for (auto& m : modes)
        if (mode == m.name) return m.action;
    if (!mode.empty() && std::isdigit(static_cast<unsigned char>(mode[0]))) return int(std::strtol(mode.c_str(), nullptr, 0));
    return -1;
}

// the actions that go on to a select screen (sub_0019F3D0)
bool Selects(int action) {
    return action == kActionArcade || action == kActionVersus || action == kActionPractice || action == kActionKart;
}

const bool g_loaded = [] {
    const char* s = std::getenv("MKA_AUTOPILOT");
    if (!s && g_training) s = "practice";
    if (!s) return false;
    g_req.action = ActionFor(s);
    if (g_req.action < 0) {
        LOGE("autopilot: unknown mode %s (versus, arcade, practice, motor, konquest, kontent, kreate or an action)", s);
        return false;
    }
    if (const char* p = std::getenv("MKA_P1")) g_req.names[0] = p;
    if (const char* p = std::getenv("MKA_P2")) g_req.names[1] = p;
    g_req.unlock_all = std::getenv("MKA_UNLOCK_ALL") || !g_req.names[0].empty() || !g_req.names[1].empty();
    if (const char* t = std::getenv("MKA_TRACK")) g_track_stage = t;  // Motor Kombat's track (43 the first)
    g_have_request = true;
    g_since = Clock::now();
    g_phase = Phase::ToMenu;
    return true;
}();

// a short press every cycle seconds, the first press seconds of each
bool Pulse(double t, double cycle, double press) { return t - int(t / cycle) * cycle < press; }

bool PlayersPick() { return g_req.by_player[0] || g_req.by_player[1]; }

bool Online() { return g_req.local_side >= 0 || g_req.watch; }

// Every character, alternate costume and arena, for as long as the game runs (online matches and practice): the
// masks the checks read and the built-in ones the checks OR in and every rebuild starts from, so a profile's saved
// unlocks (its own bits) stay its own. The created fighter lives in a profile: it stays as the profile has it, and
// locked online (the other game has no such fighter). A costume a player picks on the select screen goes to the
// other game with the pick (ExchangePicks).
void Unlock() {
    const uint32_t created = 1u << (kCreatedFighter - 32);
    for (const uint32_t* set : {kUnlockMasks, kBuiltIn})
        for (int i = 0; i < 3; i++) {
            kernel::W32(set[i], 0xFFFFFFFF);
            const uint32_t own = Online() ? 0 : kernel::R32(set[i] + 4) & created;
            kernel::W32(set[i] + 4, i == 0 ? ~created | own : 0xFFFFFFFF);
        }
}

void UnlockForPractice() {
    Unlock();
    LOGR("autopilot: practice offers every character, costume and arena");
}

int CharacterAt(int slot) { return int(kernel::R32(kGrid + uint32_t(slot) * 0x10)); }

// as the select screen sees it for P1's side (sub_00208D90 with its second argument 0)
bool Pickable(int character) {
    if (character < 0 || character >= 64 || character == kCreatedFighter) return false;
    uint32_t half = character < 32 ? 0 : 4;
    uint32_t mask = kernel::R32(kUnlockMasks[0] + half) | kernel::R32(kDefaultMask + half);
    return (mask >> (character & 31)) & 1;
}

// where a move from a slot ends: on in the same direction past what cannot be picked (ten steps), then the next
// pickable slot in order (sub_00207480)
int Move(int slot, int dir) {
    auto next = [&](int s) { return int(kernel::R32(kMoves + uint32_t(s) * 0x10 + uint32_t(dir) * 4)) & (kSlots - 1); };
    int s = next(slot);
    for (int i = 0; i < 10 && !Pickable(CharacterAt(s)); i++) s = next(s);
    for (int i = 0; i < kSlots && !Pickable(CharacterAt(s)); i++) s = (s + 1) % kSlots;
    return s;
}

// the first move of a shortest way from one slot to another, -1 if there is none
int FirstMove(int from, int to) {
    if (from < 0 || from >= kSlots || to < 0 || to >= kSlots || from == to) return -1;
    int first[kSlots], queue[kSlots], head = 0, tail = 0;
    for (int& f : first) f = -2;
    first[from] = -1;
    queue[tail++] = from;
    while (head < tail) {
        int s = queue[head++];
        for (int d = 0; d < 4; d++) {
            int n = Move(s, d);
            if (first[n] != -2) continue;
            first[n] = s == from ? d : first[s];
            if (n == to) return first[n];
            queue[tail++] = n;
        }
    }
    return -1;
}

// the slot holding a character, -1 when it is not on the grid
int SlotOf(int character) {
    for (int s = 0; s < kSlots; s++)
        if (CharacterAt(s) == character) return s;
    return -1;
}

int Cursor(int side) {
    uint32_t block = SelectBlock();
    return block ? int(kernel::R32(block + kSelectCursor[side])) : -1;
}

bool SelectUp() { return kernel::R32(kMainTask) == kSelectTask && SelectBlock() != 0; }

bool Picked(int side) {
    uint32_t state = kernel::R32(kRecord[side] + kRecordState);
    return state == 2 || state == 3;
}

// who drives what on the select screen: each port's record (port block + 8; a press on a free open port binds one)
// and each record's port and state, for the log
void LogSelectState(const char* when) {
    LOGR("autopilot: %s: port 0 -> %08X, port 1 -> %08X; P1 port %d state %u cursor %d, P2 port %d state %u cursor %d",
         when, kernel::R32(kPortBlock + 8), kernel::R32(kPortBlock + kPortStride + 8),
         int(kernel::R32(kRecord[0] + kRecordPort)), kernel::R32(kRecord[0] + kRecordState), Cursor(0),
         int(kernel::R32(kRecord[1] + kRecordPort)), kernel::R32(kRecord[1] + kRecordState), Cursor(1));
}

// the press one side's cursor needs now: a move toward its character, or A on it
void Decide(int side, SelectPort& p) {
    p.buttons = 0;
    p.a = 0;
    int cur = Cursor(side);
    if (cur < 0 || cur >= kSlots) return;
    int target = g_req.characters[side] >= 0 ? SlotOf(g_req.characters[side]) : -1;
    if (cur != p.last_cursor) {
        LOGI("autopilot: P%d cursor on slot %d (%s), target slot %d", side + 1, cur,
             static_cast<const char*>(kernel::HostPtr(kernel::R32(kCharacterNames +
                                                                  uint32_t(CharacterAt(cur) & 63) * 0x14))),
             target);
        p.last_cursor = cur;
    }
    if (target < 0 || target == cur) {
        p.a = 0xFF;
        return;
    }
    int d = FirstMove(cur, target);
    p.buttons = d >= 0 ? kMoveButton[d] : kMoveButton[1];  // no known way: RIGHT and look again
}

// the side a port picks for now: its own, except in practice, where port 0 picks P1 and then the dummy (P2); -1 for
// none (a side that takes no part: in arcade a press on port 1 would bring P2 in)
int SideOf(int port) {
    int side = port;
    if (GameMode() == kModePractice) side = port != 0 ? -1 : Picked(0) ? 1 : 0;
    if (side < 0 || side > 1 || kernel::R32(kRecord[side] + kRecordState) == 0) return -1;
    return side;
}

// An online match whose players pick on the select screen: this PC's player picks for their side with their own
// controller (the pads route it to that side's port, LocalPickPort), and where their cursor is, then their pick
// (character, alternate costume), goes to the other game with the HELLOs (net::SetLocalPick); there the autopilot
// steers that side's cursor after it, then makes the pick and sets the same costume. A spectator's game follows both
// players' cursors and picks that way. Nobody's time runs out: the screen's own 45 s without a press (2700 frames,
// then the main menu) is held off (HoldIdle).
//
// The pick is the character under the cursor (block +0x1C P1, +0x50 P2: the cursor setter sub_00207170 writes it with
// the move). The record's character (+0x2C) is only a copy that the screen's preview handler sub_00205470 makes, one
// player at a time and after loading the previous preview, and the screen's end (sub_002056E0) copies the characters
// under the cursors over the records once more: a player who confirms right after a move has the one before in the
// record for a moment, which 0.1.17 sent (24 of the 36 Armageddon desync reports since: the other game fought the
// character next to the pick).
//
// A pick's costume is a flag in the select block until the screen ends, which ORs the flags into the records'
// costume bits (cleared when it began): the alternate costume (+0x24 P1, +0x58 P2; sub_00207930 sets it when the
// pick is confirmed with logical button 0xB held and the alternate unlocked; the preview handler moves it into the
// record's bit 0 a moment later) and the other colours (+0x28, +0x5C; sub_00207B90 gives them to the second of two
// players picking the same character in the same costume). The picks come in a different order on each PC, so online
// both games give the other colours to P2.
constexpr uint32_t kSelectAlternate[2] = {0x24, 0x58}, kSelectColours[2] = {0x28, 0x5C};
constexpr int kWaitPick = -2;            // characters[]: the other player has not picked yet
// A cursor in the pick words: before the pick, a player's game sends where its player's cursor is as one word, the
// slot tagged so that no character id (0 to 0x3F) can be taken for it
constexpr uint32_t kCursorWord = 0xC5000000u, kCursorTagMask = 0xFF000000u;
bool g_local_pick_sent = false;
int g_sent_cursor = -1;                  // this PC's player's cursor as last sent
int g_their_cursor[2] = {-1, -1};        // the other player's (a spectator's game: both players') as it came
bool g_costume_due[2] = {};              // a player's costume from their game, set once the autopilot picked
uint32_t g_their_costume[2] = {};
int g_agreed[2] = {-1, -1};              // the picks both games fight with: this player's as sent, the other's as it came
uint32_t g_agreed_costume[2] = {};
// The arena panel (BLACK over the grid; LEFT / RIGHT through the arenas, A takes one, B puts the choice back): the
// choice is kTrackChoice, an arena id below kArenaChoices (-1 none), this PC's player's alone. It goes to the other
// game as the pick's third word (0xFFFFFFFF none), and both games fight on the arena linkgame.cpp agrees from the two.
constexpr uint32_t kSelectArenaPanel = 0x78;  // select block: 1 while the arena panel is open
constexpr uint32_t kArenaChoices = 0x38;      // the fight load takes a choice below this (sub_000982D0)
constexpr uint32_t kNoArena = 0xFFFFFFFFu;
int g_chosen_arena[2] = {-1, -1};          // each side's choice as sent or as it came

void ResetPlayerPicks() {
    g_local_pick_sent = false;
    g_sent_cursor = -1;
    for (int i = 0; i < 2; i++) {
        g_costume_due[i] = false;
        g_their_cursor[i] = g_agreed[i] = g_chosen_arena[i] = -1;
        g_agreed_costume[i] = 0;
        if (g_req.by_player[i]) g_req.characters[i] = i == g_req.local_side ? -1 : kWaitPick;
    }
}

// the press toward the other player's cursor as it is on their screen (none before it came, or once there)
void Follow(int side, SelectPort& p) {
    p.buttons = 0;
    p.a = 0;
    const int cur = Cursor(side), to = g_their_cursor[side];
    if (cur < 0 || cur >= kSlots || to < 0 || to == cur) return;
    const int d = FirstMove(cur, to);
    if (d >= 0) p.buttons = kMoveButton[d];
}

// this PC's player's cursor while they choose, then their pick, to the other game; the other player's (a spectator's
// game: both players'), as they come, to the autopilot
void ExchangePicks() {
    const int me = g_req.local_side;
    if (me < 0 && !g_req.watch) return;
    const uint32_t block = SelectBlock();
    if (me >= 0 && g_req.by_player[me] && !g_local_pick_sent) {
        const uint32_t under = kernel::R32(block + kSelectUnder[me]);
        if (Picked(me) && under < 0x3F) {  // 0x3F while a random pick's roulette runs
            const int at = Cursor(me);
            if (int(under) != CharacterAt(at))
                LOGR("autopilot: this player's cursor is on slot %d (character %d), the screen has %u under it", at,
                     CharacterAt(at), under);
            const bool alternate = kernel::R32(block + kSelectAlternate[me]) ||
                                   (kernel::R32(kRecord[me] + kRecordCostume) & 1);
            const uint32_t chosen = kernel::R32(kTrackChoice);
            const uint32_t words[3] = {under, alternate ? 1u : 0u, chosen < kArenaChoices ? chosen : kNoArena};
            mk3d::net::SetLocalPick(words, 3);
            g_local_pick_sent = true;
            g_agreed[me] = int(words[0]);
            g_agreed_costume[me] = words[1];
            g_chosen_arena[me] = words[2] != kNoArena ? int(words[2]) : -1;
            LOGR("autopilot: this player picked character %u, costume %u%s", words[0], words[1],
                 words[2] != kNoArena ? (", arena " + std::to_string(words[2])).c_str() : "");
            LogSelectState("picked");
        } else if (!Picked(me)) {
            const int at = Cursor(me);
            if (at >= 0 && at < kSlots && at != g_sent_cursor) {
                const uint32_t word = kCursorWord | uint32_t(at);
                mk3d::net::SetLocalPick(&word, 1);
                g_sent_cursor = at;
            }
        }
    }
    for (int other = 0; other < 2; other++) {
        if (other == me) continue;
        if (g_req.by_player[other] && g_req.characters[other] == kWaitPick) {
            uint32_t words[4] = {};
            int n = 4;
            const bool in = g_req.watch ? mk3d::net::WatchPick(other, words, n) : mk3d::net::PeerPick(words, n);
            if (in && n > 0 && (words[0] & kCursorTagMask) == kCursorWord) {
                g_their_cursor[other] = int(words[0] & uint32_t(kSlots - 1));
            } else if (in && n > 0 && SlotOf(int(words[0])) >= 0) {
                g_req.characters[other] = int(words[0]);
                g_their_costume[other] = n > 1 && words[1] ? 1 : 0;
                g_costume_due[other] = true;
                g_agreed[other] = int(words[0]);
                g_agreed_costume[other] = g_their_costume[other];
                g_chosen_arena[other] = n > 2 && words[2] < kArenaChoices ? int(words[2]) : -1;
                LOGR("autopilot: P%d picked character %u, costume %u%s", other + 1, words[0], g_their_costume[other],
                     g_chosen_arena[other] >= 0 ? (", arena " + std::to_string(words[2])).c_str() : "");
            }
        }
        if (g_costume_due[other] && Picked(other)) {
            kernel::W32(block + kSelectAlternate[other], g_their_costume[other]);
            g_costume_due[other] = false;
        }
    }
    if (Picked(0) && Picked(1) && !g_costume_due[0] && !g_costume_due[1]) {
        const auto alternate = [&](int s) {
            return kernel::R32(block + kSelectAlternate[s]) != 0 || (kernel::R32(kRecord[s] + kRecordCostume) & 1);
        };
        const bool mirror = kernel::R32(block + kSelectUnder[0]) == kernel::R32(block + kSelectUnder[1]) &&
                            alternate(0) == alternate(1);
        kernel::W32(block + kSelectColours[0], 0);
        kernel::W32(block + kSelectColours[1], mirror ? 1 : 0);
    }
}

// The select screen goes back to the main menu after 2700 frames without a press: the select task restarts its count
// whenever the main loop frame of the last input (0x5B7E64, which the pad code's sub_000A1590 sets) changes. Online
// and watching, a player takes as long as they like: it reads as a press every frame.
constexpr uint32_t kLastInputFrame = 0x5B7E64;
void HoldIdle() { kernel::W32(kLastInputFrame, kernel::R32(kFrameCounter)); }

// One press a cycle for the side a player picks on their own PC: toward their cursor while they choose, then to their
// pick and A on it. Quicker than the launcher's picks (Decide's 0.25 s), so the cursor keeps up with theirs.
constexpr double kFollowCycle = 0.1, kFollowPress = 0.05;

// a kart's index (0x560DA0 + kart * 0x34: +4 "motload_<name>.sec") by name, the character's roster id or index;
// -1 if unknown
int KartId(const std::string& s) {
    if (s.empty() || s == "-") return -1;
    if (std::isdigit(static_cast<unsigned char>(s[0]))) return std::atoi(s.c_str()) % 10;
    std::string want = Norm(s);
    if (want == "JOHNNYCAGE") want = "CAGE";
    for (int k = 0; k < 10; k++) {
        uint32_t p = kernel::R32(kKartTable + uint32_t(k) * 0x34 + 4);
        if (!p) continue;
        std::string file(static_cast<const char*>(kernel::HostPtr(p)));  // "motload_subzero.sec"
        size_t at = file.find('_'), dot = file.rfind('.');
        if (at == std::string::npos || dot == std::string::npos || dot <= at) continue;
        if (Norm(file.substr(at + 1, dot - at - 1)) == want) return k;
    }
    return -1;
}

void EnterKart() {
    g_kart_step = KartStep::Track;
    g_track_presses = 0;
    g_track_target = g_track_stage.empty() ? kFirstTrack + g_track_seed % kTracks
                                           : uint32_t(std::strtoul(g_track_stage.c_str(), nullptr, 10));
    for (int i = 0; i < 2; i++) {
        g_kart[i] = KartPort{};
        g_kart_target[i] = KartId(g_req.names[i]);
        if (g_kart_target[i] < 0) g_kart_target[i] = i;  // the quadrants' own: Baraka, Bo' Rai Cho
    }
    if (g_kart_target[1] == g_kart_target[0]) g_kart_target[1] = (g_kart_target[0] + 1) % 10;  // one kart each
    LOGI("autopilot: Motor Kombat on track %u, karts %d and %d", g_track_target, g_kart_target[0], g_kart_target[1]);
}

// the press a port needs now on Motor Kombat's select
void KartDecide(int port, KartPort& k) {
    k.buttons = 0;
    k.a = 0;
    uint32_t track = kernel::R32(kTrackChoice);
    if (g_kart_step == KartStep::Track) {
        if (port != 0) return;
        if (track != g_track_target && g_track_presses < 12) {
            k.buttons = kMoveButton[1];  // RIGHT
            g_track_presses++;
            return;
        }
        k.a = 0xFF;  // this track
        g_track_confirmed = track;
        g_kart_step = KartStep::Karts;
        g_kart_since = Clock::now();
        LOGI("autopilot: track %u", track);
        return;
    }
    if (std::chrono::duration<double>(Clock::now() - g_kart_since).count() < 1.0) return;  // the quadrants open
    if (port == 0 && track != g_track_confirmed) {  // still on the track screen: the A was not taken
        g_kart_step = KartStep::Track;
        return;
    }
    uint32_t state = kernel::R32(kRecord[port] + kRecordState);
    if (state == 2) return;  // ready
    if (port == 1 && state == 0) {
        k.buttons = kStart;  // P2 joins
        return;
    }
    int kart = int(kernel::R32(kKartChoice + uint32_t(port) * 0xC));
    if (kart != g_kart_target[port]) {
        k.buttons = kMoveButton[1];  // RIGHT: the next kart
        return;
    }
    if (port == 0 && kernel::R32(kRecord[1] + kRecordState) == 0) return;  // not before P2 has joined
    k.a = 0xFF;
}

}  // namespace

// the game's names, the launcher's roster ids (launcher/data/mka.json: "SubZero", "BoRaiCho", "JohnnyCage") and other
// spellings match on letters and digits alone, any case; the two roster ids longer than the game's names are aliases
int CharacterId(const std::string& s) {
    if (s.empty() || s == "-") return -1;
    if (std::isdigit(static_cast<unsigned char>(s[0]))) return std::atoi(s.c_str());
    std::string want = Norm(s);
    static const struct {
        const char* from;
        const char* to;
    } kAliases[] = {{"JOHNNYCAGE", "CAGE"}, {"NOOBSAIBOT", "NOOB"}, {"SONYABLADE", "SONYA"}, {"DRAGONKING", "ONAGA"}};
    for (const auto& a : kAliases)
        if (want == a.from) want = a.to;
    for (int i = 0; i < 64; i++) {
        uint32_t p = kernel::R32(kCharacterNames + uint32_t(i) * 0x14);
        if (!p) continue;
        std::string name(static_cast<const char*>(kernel::HostPtr(p)));
        if (Norm(name) == want) return i;
    }
    return -1;
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

void SetTrack(uint32_t seed, const std::string& stage) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_track_seed = seed;
    g_track_stage = stage;
}

void Rematch() {
    std::lock_guard<std::mutex> lock(g_lock);
    g_base_fights = FightSetups();
    g_base_matches = MatchStarts();
    ResetPlayerPicks();  // players who pick on the select screen pick again
    Enter(g_req.action == kActionKart ? Phase::Kart : Phase::Select);
}

bool AgreedPick(int side, int& character, uint32_t& costume) {
    std::lock_guard<std::mutex> lock(g_lock);
    if (side < 0 || side > 1 || g_agreed[side] < 0) return false;
    character = g_agreed[side];
    costume = g_agreed_costume[side];
    return true;
}

int ChosenArena(int side) {
    std::lock_guard<std::mutex> lock(g_lock);
    return side == 0 || side == 1 ? g_chosen_arena[side] : -1;
}

bool ArenaPanelOpen() {
    const uint32_t block = SelectBlock();
    return block && kernel::R32(kMainTask) == kSelectTask && kernel::R32(block + kSelectArenaPanel) == 1;
}

int LocalPickPort() {
    std::lock_guard<std::mutex> lock(g_lock);
    const int me = g_req.local_side;
    if (g_phase != Phase::Select || me < 0 || !g_req.by_player[me] || Since() < 1.5) return -1;
    if (!SelectUp() || Picked(me)) return -1;  // not up, or picked
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
            if (g_base_menus == 0 && port == 0 && Pulse(t, 0.7, 0.1)) pad.buttons |= kStart;  // boot only
            return;
        case Phase::Settle:
            if (t > 1.5) {
                // asked here, not at start: the other file's MKA_ACTION default may initialise after this one
                RequestAction(g_req.action == kActionArcade ? -1 : g_req.action);
                for (int i = 0; i < 2; i++) {
                    g_req.characters[i] = CharacterId(g_req.names[i]);
                    if (!g_req.names[i].empty() && g_req.characters[i] < 0)
                        LOGE("autopilot: unknown character %s for P%d", g_req.names[i].c_str(), i + 1);
                }
                ResetPlayerPicks();
                if (g_req.unlock_all) Unlock();
                if (g_training) UnlockForPractice();
                g_base_actions = MenuActions();
                Enter(Phase::Choose);
            }
            return;
        case Phase::Choose:
            if (g_req.action == kActionKart && kernel::R32(kMainTask) == kKartSelectTask) {
                g_base_matches = MatchStarts();
                Enter(Phase::Kart);
                return;
            }
            if (SelectUp()) {
                g_base_fights = FightSetups();
                Enter(Phase::Select);
                LogSelectState("select screen");
                return;
            }
            if (!g_confirmed && MenuActions() > g_base_actions) {
                g_confirmed = true;  // no more presses: the next screen must not get them
                g_confirmed_at = Clock::now();
                LOGR("autopilot: menu action %u confirmed", kernel::R32(kMenuAction));
                if (!Selects(g_req.action) && kernel::R32(kMenuAction) == uint32_t(g_req.action)) {
                    Enter(Phase::Done);  // a mode without a select screen
                    return;
                }
            }
            if (g_confirmed) {
                if (std::chrono::duration<double>(Clock::now() - g_confirmed_at).count() < 8) return;
                g_confirmed = false;  // something else was confirmed (no select screen came): go on
                g_base_actions = MenuActions();
            }
            // START passes the profile screen, A confirms menu items: in turn
            if (port == 0 && Pulse(t, 0.7, 0.1)) {
                if (int(t / 0.7) & 1) pad.analog[0] = 0xFF;  // A
                else pad.buttons |= kStart;
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
            if (FightSetups() > g_base_fights || (!PlayersPick() && t > 90)) {
                Enter(Phase::Done);
                return;
            }
            if (port > 1) return;
            pad.connected = true;
            if (!SelectUp() || t < 1.5) return;  // the select screen fades in
            if (g_req.unlock_all) Unlock();
            if (port == 0) {
                ExchangePicks();
                if (Online()) HoldIdle();
            }
            int side = SideOf(port);
            if (side < 0 || Picked(side)) return;
            if (side == g_req.local_side && g_req.by_player[side]) return;  // this PC's player picks
            SelectPort& p = g_select[side];
            if (g_req.by_player[side]) {  // a player on their own PC: after their cursor, then to their pick
                long cycle = long(t / kFollowCycle);
                if (cycle != p.cycle) {
                    p.cycle = cycle;
                    if (g_req.characters[side] == kWaitPick) Follow(side, p);
                    else Decide(side, p);
                }
                if (Pulse(t, kFollowCycle, kFollowPress)) {
                    pad.buttons |= p.buttons;
                    if (p.a) pad.analog[0] = p.a;
                }
                return;
            }
            long cycle = long(t / 0.25);
            if (cycle != p.cycle) {
                p.cycle = cycle;
                Decide(side, p);
            }
            if (Pulse(t, 0.25, 0.08)) {
                pad.buttons |= p.buttons;
                if (p.a) pad.analog[0] = p.a;
            }
            return;
        }
        case Phase::Kart: {
            if (MatchStarts() > g_base_matches || t > 120) {
                Enter(Phase::Done);
                return;
            }
            if (port > 1) return;
            pad.connected = true;
            if (kernel::R32(kMainTask) != kKartSelectTask || t < 1.5) return;  // the screen fades in
            KartPort& k = g_kart[port];
            long cycle = long(t / 0.25);
            if (cycle != k.cycle) {
                k.cycle = cycle;
                KartDecide(port, k);
            }
            if (Pulse(t, 0.25, 0.08)) {
                pad.buttons |= k.buttons;
                if (k.a) pad.analog[0] = k.a;
            }
            return;
        }
        default:
            return;
    }
}

}  // namespace mka::autopilot
