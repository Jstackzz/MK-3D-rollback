// Autopilot: drives the front end to a chosen mode and characters by pressing buttons the way a player would,
// watching game state instead of trusting fixed timings (boot and load times vary from run to run).
//
//   MKA_AUTOPILOT=versus|arcade|practice|motor   (motor: only as far as Motor Kombat's own select screen), or
//                                konquest|kontent|kreate or a main menu action's number (0xE): those have no select
//                                screen, so the autopilot is done once the menu took it
//   MKA_P1, MKA_P2=<character>   who each side picks (default: what the cursor starts on): the game's name ("SUB-ZERO"),
//                                the launcher's roster id (launcher/data/mka.json: "SubZero", "JohnnyCage") or the
//                                index; all characters are unlocked when either is given, or with MKA_UNLOCK_ALL=1
//   MKA_AUTOPILOT_HOLD=1         stop at character select
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
// a profile.
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
constexpr uint32_t kUnlockMasks[] = {0x7363C8, 0x7363D0};
constexpr uint32_t kDefaultMask = 0x561968;
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

// players picking on the select screen get the characters only (an alternate costume one of them chose would have to
// come over too; the costume bits do, ExchangePicks, but only for what the game offers unlocked anyway)
bool PlayersPick() { return g_req.by_player[0] || g_req.by_player[1]; }

void Unlock() {
    for (uint32_t m : kUnlockMasks) {
        if (m == kUnlockMasks[1] && PlayersPick()) continue;  // alternates
        kernel::W32(m, 0xFFFFFFFF);
        kernel::W32(m + 4, 0xFFFFFFFF);
    }
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
// controller (the pads route it to that side's port, LocalPickPort), and the pick (character, costume bits) goes to
// the other game with the HELLOs (net::SetLocalPick); the other side waits for the other player's pick, then the
// autopilot makes it and sets the same costume bits. The screen goes back to the main menu after 45 s without a press
// (2700 frames), so a side still waiting moves its cursor now and then.
constexpr int kWaitPick = -2;            // characters[]: the other player has not picked yet
constexpr double kPlayerPickTime = 40;   // seconds this PC's player has (the screen's own 45), then the cursor's
bool g_local_pick_sent = false;
bool g_local_forced = false;             // the time ran out: the autopilot picks for this PC's player
bool g_costume_due = false;              // the other player's costume bits, set once the autopilot picked
uint32_t g_their_costume = 0;

void ResetPlayerPicks() {
    g_local_pick_sent = g_local_forced = g_costume_due = false;
    for (int i = 0; i < 2; i++)
        if (g_req.by_player[i]) g_req.characters[i] = i == g_req.local_side ? -1 : kWaitPick;
}

// this PC's player's pick, once made, to the other game; the other player's, once it came, to the autopilot
void ExchangePicks(double t) {
    const int me = g_req.local_side;
    if (me < 0) return;
    if (g_req.by_player[me] && !g_local_pick_sent && Picked(me)) {
        const uint32_t words[2] = {kernel::R32(kRecord[me] + kRecordCharacter), kernel::R32(kRecord[me] + kRecordCostume)};
        mk3d::net::SetLocalPick(words, 2);
        g_local_pick_sent = true;
        LOGR("autopilot: this player picked character %u, costume bits %X", words[0], words[1]);
        LogSelectState("picked");
    }
    if (g_req.by_player[me] && !g_local_forced && !g_local_pick_sent && t > kPlayerPickTime) {
        g_local_forced = true;
        LOGR("autopilot: no pick after %.0f s: this player gets the cursor's", kPlayerPickTime);
        LogSelectState("taking over");
    }
    const int other = me ^ 1;
    if (g_req.by_player[other] && g_req.characters[other] == kWaitPick) {
        uint32_t words[4] = {};
        int n = 4;
        if (mk3d::net::PeerPick(words, n) && n > 0 && SlotOf(int(words[0])) >= 0) {
            g_req.characters[other] = int(words[0]);
            g_their_costume = n > 1 ? words[1] : 0;
            g_costume_due = true;
            LOGR("autopilot: the other player picked character %u, costume bits %X", words[0], g_their_costume);
        }
    }
    if (g_costume_due && Picked(other)) {
        kernel::W32(kRecord[other] + kRecordCostume, g_their_costume);
        g_costume_due = false;
    }
}

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

int LocalPickPort() {
    std::lock_guard<std::mutex> lock(g_lock);
    const int me = g_req.local_side;
    if (g_phase != Phase::Select || me < 0 || !g_req.by_player[me] || g_local_forced || Since() < 1.5) return -1;
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
            if (g_hold_at_select) return;
            if (FightSetups() > g_base_fights || t > (PlayersPick() ? 240 : 90)) {
                Enter(Phase::Done);
                return;
            }
            if (port > 1) return;
            pad.connected = true;
            if (!SelectUp() || t < 1.5) return;  // the select screen fades in
            if (g_req.unlock_all) Unlock();
            if (port == 0) ExchangePicks(t);
            int side = SideOf(port);
            if (side < 0 || Picked(side)) return;
            if (side == g_req.local_side && g_req.by_player[side] && !g_local_forced) return;  // this PC's player picks
            if (g_req.characters[side] == kWaitPick) {  // the other player has not picked: keep the screen awake
                if (Pulse(t, 8.0, 0.08)) pad.buttons |= (long(t / 8) & 1) ? kMoveButton[0] : kMoveButton[1];
                return;
            }
            SelectPort& p = g_select[side];
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
