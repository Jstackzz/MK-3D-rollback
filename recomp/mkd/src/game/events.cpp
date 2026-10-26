// Game events the netplay layer keys on, found while mapping the fight (research/mkd/notes.md, round 4):
//   sub_001BC170(mode)   the only writer of the game mode 0x61C414
//   sub_00083570()       fight setup: fighters' HUD, health bars and per-fight state, then the fight scene starts
//   sub_0029B89F(seed)   the C runtime's srand (per-thread state at _getptd()+0x14); rand is sub_0029B8AC
//   sub_0014EF20(port, action)  the front end menu confirming an item: action code to 0x46CDB8 for the menu task
//                        sub_000E3770 (6 arcade/mode 0, 7 versus/mode 1, 8 chess kombat/mode 9, 9 puzzle kombat/mode
//                        6, 0xB practice/mode 4, 2 konquest/mode 7), and the players taking part
//                        (sub_0016FBC0(player record, 1)). MKD_ACTION=<code> turns the next arcade, versus or practice
//                        confirmation into that action with both players taking part: how a scripted or netplay
//                        session reaches a two player mode without a second signed in controller (the menu greys
//                        VERSUS out then).
// Each is logged (MKD_EVENT_LOG=1) and counted; the counters are what netplay waits on.
#include "game/events.h"

#include <vector>
#include <atomic>
#include <cstdlib>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "game/frame.h"
#include "kernel/kernel.h"

X86_EXTERN_FUNC(__imp__sub_001BC170);
X86_EXTERN_FUNC(__imp__sub_00083570);
X86_EXTERN_FUNC(__imp__sub_0029B89F);
X86_EXTERN_FUNC(__imp__sub_0014EF20);
X86_EXTERN_FUNC(__imp__sub_000E3770);

namespace mkd {
namespace {

std::atomic<uint32_t> g_fights{0};
std::atomic<uint32_t> g_menus{0};
std::atomic<uint32_t> g_actions{0};
std::atomic<uint64_t> g_fight_frame{0};

bool Logging() {
    static const bool on = std::getenv("MKD_EVENT_LOG") != nullptr;
    return on;
}

}  // namespace

int g_action_override = [] {
    const char* s = std::getenv("MKD_ACTION");
    return s ? int(std::strtol(s, nullptr, 0)) : -1;
}();

void RequestAction(int action) { g_action_override = action; }

uint32_t FightSetups() { return g_fights.load(); }
uint32_t MainMenus() { return g_menus.load(); }
uint32_t MenuActions() { return g_actions.load(); }
uint64_t LastFightSetupFrame() { return g_fight_frame.load(); }
uint32_t GameMode() { return kernel::R32(kGameMode); }

std::atomic<uint32_t> g_match_starts{0};
uint32_t MatchStarts() { return g_match_starts.load(); }

std::atomic<uint32_t> g_chess_teams{0};
uint32_t ChessTeams() { return g_chess_teams.load(); }

std::atomic<uint32_t> g_state_switches{0};
uint32_t StateSwitches() { return g_state_switches.load(); }

bool InFrontEnd() {
    uint32_t task = kernel::R32(kMainTask);
    return task == kMainMenuTask || task == kVersusSelectTask || task == kPuzzleSelectTask || task == kChessSelectTask;
}

std::vector<FightSetupHook>& SetupHooks() {
    static std::vector<FightSetupHook> v;
    return v;
}
void BeforeFightSetup(FightSetupHook hook) { SetupHooks().push_back(hook); }

}  // namespace mkd

X86_FUNC(sub_001BC170) {
    uint32_t mode = kernel::R32(ctx.esp + 4);
    if (mkd::Logging()) LOGI("event: frame %llu game mode %u -> %u", (unsigned long long)mkd::Frame(), mkd::GameMode(), mode);
    __imp__sub_001BC170(ctx, base);
}

// the side modes' match tasks, started once both players have picked: Chess Kombat's board game (0x11DD20, from
// the select task 0x17A500) and Puzzle Kombat's game (from 0x17A270: 0xF2850 for two players, 0xDCE60 against the
// computer). They meet the other game the way a fight setup does (BeforeFightSetup); a chess battle's own fight
// setup then runs in sync already.
X86_EXTERN_FUNC(__imp__sub_0011DD20);
X86_EXTERN_FUNC(__imp__sub_000DCE60);
X86_EXTERN_FUNC(__imp__sub_000F2850);
X86_FUNC(sub_0011DD20) {
    mkd::g_match_starts++;
    if (mkd::Logging()) LOGI("event: frame %llu chess kombat match starts", (unsigned long long)mkd::Frame());
    for (mkd::FightSetupHook hook : mkd::SetupHooks()) hook(ctx);
    __imp__sub_0011DD20(ctx, base);
}
X86_FUNC(sub_000DCE60) {
    mkd::g_match_starts++;
    if (mkd::Logging()) LOGI("event: frame %llu puzzle kombat match starts", (unsigned long long)mkd::Frame());
    for (mkd::FightSetupHook hook : mkd::SetupHooks()) hook(ctx);
    __imp__sub_000DCE60(ctx, base);
}
X86_FUNC(sub_000F2850) {
    mkd::g_match_starts++;
    if (mkd::Logging()) LOGI("event: frame %llu puzzle kombat match starts (two players)", (unsigned long long)mkd::Frame());
    for (mkd::FightSetupHook hook : mkd::SetupHooks()) hook(ctx);
    __imp__sub_000F2850(ctx, base);
}

// a front end screen is built (name, ..., screen data, 0); Chess Kombat's select screen gets the select task's team
// block, which is where its picks go
X86_EXTERN_FUNC(__imp__sub_0015A930);
X86_FUNC(sub_0015A930) {
    if (kernel::R32(ctx.esp + 4) == mkd::kChessSelectScreen) mkd::g_chess_teams = kernel::R32(ctx.esp + 12);
    __imp__sub_0015A930(ctx, base);
}

X86_FUNC(sub_00083570) {
    for (mkd::FightSetupHook hook : mkd::SetupHooks()) hook(ctx);
    __imp__sub_00083570(ctx, base);
    mkd::g_fight_frame = mkd::Frame();
    uint32_t n = ++mkd::g_fights;
    if (mkd::Logging())
        LOGI("event: frame %llu fight setup #%u, mode %u, characters %u vs %u", (unsigned long long)mkd::Frame(), n,
             mkd::GameMode(), kernel::R32(mkd::kP1Character), kernel::R32(mkd::kP2Character));
}

X86_FUNC(sub_0029B89F) {
    if (mkd::Logging())
        LOGI("event: frame %llu srand(%08X) from %08X", (unsigned long long)mkd::Frame(), kernel::R32(ctx.esp + 4),
             kernel::R32(ctx.esp));
    __imp__sub_0029B89F(ctx, base);
}

X86_FUNC(sub_0014EF20) {
    uint32_t action = kernel::R32(ctx.esp + 8);
    int to = mkd::g_action_override;
    bool redirect = to >= 0 && (action == 6 || action == 7 || action == 8 || action == 9 || action == 0xB);
    if (redirect) kernel::W32(ctx.esp + 8, uint32_t(to));
    if (mkd::Logging())
        LOGI("event: frame %llu menu action %u from port %u%s", (unsigned long long)mkd::Frame(), action,
             kernel::R32(ctx.esp + 4), redirect ? " (redirected)" : "");
    __imp__sub_0014EF20(ctx, base);
    mkd::g_actions++;
    if (redirect) {
        // both players take part (the original only does this for versus)
        kernel::CallGuest(ctx, mkd::kSetPlayerActive, {mkd::kP1Record, 1});
        kernel::CallGuest(ctx, mkd::kSetPlayerActive, {mkd::kP2Record, 1});
        LOGI("event: menu action %u redirected to %d, both players in", action, to);
    }
}

// the state switch: counted first, since it leaves the frame through the engine's longjmp to the main loop
X86_EXTERN_FUNC(__imp__sub_000E0190);
X86_FUNC(sub_000E0190) {
    mkd::g_state_switches++;
    if (mkd::Logging())
        LOGI("event: frame %llu state switch to %08X (priority %u)", (unsigned long long)mkd::Frame(),
             kernel::R32(ctx.esp + 8), kernel::R32(ctx.esp + 4));
    __imp__sub_000E0190(ctx, base);
}

// the front end's main menu task (started through a task pointer, runs until the player leaves the menu)
X86_FUNC(sub_000E3770) {
    uint32_t n = ++mkd::g_menus;
    if (mkd::Logging()) LOGI("event: frame %llu main menu #%u", (unsigned long long)mkd::Frame(), n);
    __imp__sub_000E3770(ctx, base);
}
