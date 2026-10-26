// Game events the netplay layer keys on (research/mka/notes.md, engine map for netplay):
//   sub_00258FF0(mode)   sets the game mode 0x8292F8 (a few places also write it directly: logged from the frame hook)
//   sub_000983A0()       fight setup, once a fight after it loaded: round wins zeroed, no winner yet, round 1, the
//                        round timer started (sub_00098A40 takes the frame counter), the HUD; then the fight task
//                        (sub_00096FC0) runs the rounds
//   sub_0039C6AB(seed)   the C runtime's srand (per-thread state at _getptd()+0x14); rand is sub_0039C6B8
//   sub_001D9530(player, action)  the front end confirming a menu item: action code to 0x5B8960 for the main menu task
//                        sub_0019F3D0 (5 arcade/mode 0, 6 versus/mode 1, 9 practice/mode 4, 7 Motor Kombat/mode 0xF,
//                        1 mode 8), and the players taking part (sub_001F8C00(player record, 1)). The main menu task
//                        calls it again with the stored action before it switches. MKA_ACTION=<code> turns the next
//                        arcade, versus or practice confirmation into that action with both players taking part: how
//                        a scripted or netplay session reaches a two player mode without a second signed in
//                        controller (the menu greys VERSUS out then).
//   sub_0019CA30(priority, task)  the state switch: ends the current task, starts the new one, longjmps to the main loop
//   sub_0019F3D0()       the main menu task
//   sub_001DDDA0(name, id, block, flag)  a front end screen is built: character select's block holds its cursors
//   sub_00258FF0(7) from the race load task 0xE47B0 (returning to 0xE47E8): a Motor Kombat race starts loading (met
//                        like a fight setup: the load draws the karts' random driving values)
//   sub_000E9F60()       Motor Kombat's race setup, once a race after it loaded
// Each is logged (MKA_EVENT_LOG=1) and counted; the counters are what netplay and the autopilot wait on. The log also
// follows a fight (health, round wins, round, winner) and names Motor Kombat's finish order.
#include "game/events.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "game/frame.h"
#include "kernel/kernel.h"

X86_EXTERN_FUNC(__imp__sub_00258FF0);
X86_EXTERN_FUNC(__imp__sub_000983A0);
X86_EXTERN_FUNC(__imp__sub_0039C6AB);
X86_EXTERN_FUNC(__imp__sub_001D9530);
X86_EXTERN_FUNC(__imp__sub_0019CA30);
X86_EXTERN_FUNC(__imp__sub_0019F3D0);

namespace mka {
namespace {

std::atomic<uint32_t> g_fights{0};
std::atomic<uint32_t> g_menus{0};
std::atomic<uint32_t> g_actions{0};
std::atomic<uint32_t> g_state_switches{0};
std::atomic<uint64_t> g_fight_frame{0};
uint32_t g_seen_mode = 0xFFFFFFFF;  // the mode as the setter or the last frame left it (main thread)

bool Logging() {
    static const bool on = std::getenv("MKA_EVENT_LOG") != nullptr;
    return on;
}

// the mode written without the setter (0x8A9CC, 0x113EA3, 0x182AB3...): seen at the next frame's top
void WatchMode(uint64_t frame) {
    uint32_t mode = kernel::R32(kGameMode);
    if (mode == g_seen_mode) return;
    if (Logging() && g_seen_mode != 0xFFFFFFFF)
        LOGI("event: frame %llu game mode %u -> %u (written directly)", (unsigned long long)frame, g_seen_mode, mode);
    g_seen_mode = mode;
}

// a fight's progress for the log: both healths, round wins, the round and the winner, whenever one changes
void WatchFight(uint64_t frame) {
    static uint32_t seen[7] = {};
    if (!Logging() || !g_fights.load()) return;
    uint32_t now[7] = {kernel::R32(kP1Health), kernel::R32(kP2Health), kernel::R32(kP1RoundWins),
                       kernel::R32(kP2RoundWins), kernel::R32(kRound), kernel::R32(kMatchWinner),
                       kernel::R32(kRoundTimer)};
    bool changed = false;
    for (int i = 0; i < 6; i++) changed |= now[i] != seen[i];
    if (!changed) return;
    std::memcpy(seen, now, sizeof(seen));
    float h1, h2;
    std::memcpy(&h1, &now[0], 4);
    std::memcpy(&h2, &now[1], 4);
    LOGI("event: frame %llu health %.3f / %.3f, round wins %u-%u, round %u, winner %d, timer %u",
         (unsigned long long)frame, h1, h2, now[2], now[3], now[4], int32_t(now[5]), now[6]);
}

const bool g_registered = [] {
    OnFrameStart(&WatchMode);
    OnFrameStart(&WatchFight);
    return true;
}();

// a handle the engine keeps as (object, the object's serial at +4): the object while it is alive, else 0
uint32_t Handle(uint32_t at) {
    uint32_t obj = kernel::R32(at);
    return obj && kernel::R32(obj + 4) == kernel::R32(at + 4) ? obj : 0;
}

// Motor Kombat's finish order as its results screen (sub_000F1330) leaves it, right before it sets the front end mode:
// places at race + 0x29C (a kart index each, -1 empty), karts at race + 0x24C + kart * 8 (handles); a kart's +0x20 is
// its player number - 1, +0x1C its kart (0x560DA0 table: 0 Baraka ... 9 Sub-Zero), +0x1BC the port driving it
void LogRaceResult() {
    uint32_t race = kernel::R32(kRace);
    if (!race) return;
    for (uint32_t place = 0; place < 8; place++) {
        int32_t kart = int32_t(kernel::R32(race + kKartOrder + place * 4));
        if (kart < 0 || kart >= 8) continue;
        uint32_t obj = Handle(race + kKarts + uint32_t(kart) * 8);
        if (!obj) continue;
        LOGI("event: race place %u: kart slot %d, player %u, kart %u, port %d", place + 1, kart,
             kernel::R32(obj + 0x20) + 1, kernel::R32(obj + 0x1C), int32_t(kernel::R32(obj + 0x1BC)));
    }
}

bool Redirectable(uint32_t action) {
    return action == uint32_t(kActionArcade) || action == uint32_t(kActionVersus) ||
           action == uint32_t(kActionPractice);
}

}  // namespace

int g_action_override = [] {
    const char* s = std::getenv("MKA_ACTION");
    return s ? int(std::strtol(s, nullptr, 0)) : -1;
}();

void RequestAction(int action) { g_action_override = action; }

uint32_t FightSetups() { return g_fights.load(); }
uint64_t LastFightSetupFrame() { return g_fight_frame.load(); }
uint32_t MainMenus() { return g_menus.load(); }
uint32_t MenuActions() { return g_actions.load(); }
uint32_t StateSwitches() { return g_state_switches.load(); }
uint32_t GameMode() { return kernel::R32(kGameMode); }

std::atomic<uint32_t> g_select_block{0};
uint32_t SelectBlock() { return g_select_block.load(); }

std::atomic<uint32_t> g_match_starts{0};
uint32_t MatchStarts() { return g_match_starts.load(); }

bool InFrontEnd() {
    uint32_t task = kernel::R32(kMainTask);
    return task == kMainMenuTask || task == kSelectTask || task == kKartSelectTask;
}

std::vector<FightSetupHook>& SetupHooks() {
    static std::vector<FightSetupHook> v;
    return v;
}
void BeforeFightSetup(FightSetupHook hook) { SetupHooks().push_back(hook); }

// Motor Kombat's race load task has set its mode: a match start, met like a fight setup before the load makes the
// race object and the karts (sub_000C44D0 draws each kart's driving values from the random generator: the computer's
// karts would drive differently on the two games if the generator still had its menu state) and loads the track
void RaceLoadStarts(X86Context& ctx) {
    uint32_t n = ++g_match_starts;
    if (Logging())
        LOGI("event: frame %llu motor kombat race #%u loads, track %u, karts %u and %u", (unsigned long long)Frame(), n,
             kernel::R32(kTrackChoice), kernel::R32(kKartChoice), kernel::R32(kKartChoice + 0xC));
    for (FightSetupHook hook : SetupHooks()) hook(ctx);
}

}  // namespace mka

X86_FUNC(sub_00258FF0) {
    uint32_t mode = kernel::R32(ctx.esp + 4), from = kernel::R32(ctx.esp);
    if (mka::Logging()) {
        LOGI("event: frame %llu game mode %u -> %u (from %08X)", (unsigned long long)mka::Frame(), mka::GameMode(),
             mode, from);
        if (from == mka::kKartResultsDone) mka::LogRaceResult();
    }
    __imp__sub_00258FF0(ctx, base);
    mka::g_seen_mode = mode;
    if (from == mka::kRaceLoadMode) mka::RaceLoadStarts(ctx);
}

// Motor Kombat's race setup (task function 0xE9F60, run once the race load task 0xE47B0 has loaded the track and the
// karts): the karts for the players, the round setup sub_00098530 (timers from the frame counter), the countdown
X86_EXTERN_FUNC(__imp__sub_000E9F60);
X86_FUNC(sub_000E9F60) {
    uint32_t race = kernel::R32(mka::kRace);
    if (mka::Logging())
        LOGI("event: frame %llu motor kombat race #%u set up, track %u, %u players", (unsigned long long)mka::Frame(),
             mka::MatchStarts(), kernel::R32(mka::kArena), race ? kernel::R32(race + 0x6C) : 0);  // +0x6C: players
    __imp__sub_000E9F60(ctx, base);
}

X86_FUNC(sub_000983A0) {
    for (mka::FightSetupHook hook : mka::SetupHooks()) hook(ctx);
    __imp__sub_000983A0(ctx, base);
    mka::g_fight_frame = mka::Frame();
    uint32_t n = ++mka::g_fights;
    if (mka::Logging())
        LOGR("event: frame %llu fight setup #%u, mode %u, characters %u vs %u, arena %u", (unsigned long long)mka::Frame(),
             n, mka::GameMode(), kernel::R32(mka::kP1Character), kernel::R32(mka::kP2Character),
             kernel::R32(mka::kArena));
}

X86_FUNC(sub_0039C6AB) {
    if (mka::Logging())
        LOGI("event: frame %llu srand(%08X) from %08X", (unsigned long long)mka::Frame(), kernel::R32(ctx.esp + 4),
             kernel::R32(ctx.esp));
    __imp__sub_0039C6AB(ctx, base);
}

X86_FUNC(sub_001D9530) {
    uint32_t action = kernel::R32(ctx.esp + 8);
    int to = mka::g_action_override;
    // armed for every arcade, versus or practice confirmation, also the main menu task's own second call with the
    // action already redirected (which would take the second player out again for arcade or practice)
    bool armed = to >= 0 && mka::Redirectable(action);
    bool redirect = armed && action != uint32_t(to);
    if (redirect) kernel::W32(ctx.esp + 8, uint32_t(to));
    if (mka::Logging())
        LOGR("event: frame %llu menu action %u from player %u%s", (unsigned long long)mka::Frame(), action,
             kernel::R32(ctx.esp + 4), redirect ? " (redirected)" : "");
    __imp__sub_001D9530(ctx, base);
    mka::g_actions++;
    if (armed) {
        // both players take part (the original does this only for versus, or when both have joined)
        kernel::CallGuest(ctx, mka::kSetPlayerActive, {mka::kP1Record, 1});
        kernel::CallGuest(ctx, mka::kSetPlayerActive, {mka::kP2Record, 1});
        if (redirect) LOGI("event: menu action %u redirected to %d, both players in", action, to);
    }
}

// a front end screen is built (name, id, screen data, 0): character select's data block is where its cursors are
X86_EXTERN_FUNC(__imp__sub_001DDDA0);
X86_FUNC(sub_001DDDA0) {
    if (kernel::R32(ctx.esp + 4) == mka::kSelectScreen) {
        mka::g_select_block = kernel::R32(ctx.esp + 12);
        if (mka::Logging())
            LOGR("event: frame %llu character select screen, block %08X", (unsigned long long)mka::Frame(),
                 mka::SelectBlock());
    }
    __imp__sub_001DDDA0(ctx, base);
}

// the state switch: counted first, since it leaves the frame through the engine's longjmp to the main loop
X86_FUNC(sub_0019CA30) {
    mka::g_state_switches++;
    mka::g_select_block = 0;  // the screen goes with the task that built it
    if (mka::Logging())
        LOGR("event: frame %llu state switch to %08X (priority %u) from %08X", (unsigned long long)mka::Frame(),
             kernel::R32(ctx.esp + 8), kernel::R32(ctx.esp + 4), kernel::R32(ctx.esp));
    __imp__sub_0019CA30(ctx, base);
}

// the front end's main menu task (started through a task pointer, runs until the player leaves the menu; its first
// start switches to the profile screen sub_001FA0C0, which comes back here)
X86_FUNC(sub_0019F3D0) {
    uint32_t n = ++mka::g_menus;
    if (mka::Logging()) LOGR("event: frame %llu main menu #%u", (unsigned long long)mka::Frame(), n);
    __imp__sub_0019F3D0(ctx, base);
}
