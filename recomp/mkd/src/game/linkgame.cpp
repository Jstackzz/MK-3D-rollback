// Deception's side of the shared launcher link (recomp/common/src/mk3d/linkgame.h): the autopilot drives its front
// end, and a match's result comes from what both games agreed on (netgame.h).
#include "mk3d/linkgame.h"

#include <cstdlib>
#include <string>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "game/autopilot.h"
#include "game/events.h"
#include "game/netgame.h"
#include "kernel/kernel.h"
#include "mk3d/netgame.h"
#include "mk3d/netplay.h"

namespace mkd {
namespace {

constexpr uint32_t kStateSwitch = 0xE0190;  // sub_000E0190(priority, task)

autopilot::Request g_request;
uint32_t g_puzzle_base = 0;  // Puzzle Kombat matches finished before this one

// The arena of a versus or puzzle match comes from sub_00040640, which takes the next open one (sub_000406F0) of a list
// (0x3E4D30, -1 at the end) from a cycle index (0x620F44) that moves on every match: two players who played a
// different number of matches since they started their games would load different arenas. Both set the index from
// the match's seed instead (or to the stage the launcher names, an arena id on the list); every arena is open in a
// launcher match (the autopilot's Unlock), so that is the arena.
constexpr uint32_t kArenaList = 0x3E4D30, kArenaCycle = 0x620F44;

// A fight's round end (sub_00083960) ends the match once the round's winner has as many round wins as 0x5FC9A4 (the
// options' rounds, 2 by default) when no profile is in use (0x620F24 bit 0, as in a launcher match; a profile makes
// it 2): the launcher's best of N.
constexpr uint32_t kRoundsToWin = 0x5FC9A4;

// The game options' other settings a fight plays with (sub_001B4640 sets the defaults, sub_001B4430 sets one by its
// menu item): the round time (item 0x3331, seconds, 60 by default), the blood level (0x3333, 0 to 3, 3) and the death
// traps (0x3332, on). Each PC keeps its own options, so a launcher match sets them: the defaults for the round time
// and the blood (two players whose options differ would play two different fights), the launcher's death traps. The
// front end sets the options again on its way to the match: choosing a mode makes the profile's options the game's
// (sub_001B3E30 copies its 0x78-byte block over 0x5FC980 with sub_001B3DC0: the death traps were back on by the
// select screen), so the match's go back whenever the block or an option is set (the hooks below). sub_001BF070,
// which the arenas' scripts ask (their natives table 0x6597E8) and the fight code, gives no death traps when the option
// is off or the blood is; in a launcher match it gives the match's.
constexpr uint32_t kOptionsBlock = 0x5FC980;
constexpr uint32_t kRoundTime = 0x5FC9AC, kBloodLevel = 0x5FC9B4, kDeathTraps = 0x5FC9B8;

struct MatchOptions {
    bool set = false;  // a launcher match was started: these hold from then on
    int wins_needed = 2;
    bool death_traps = true;
};
MatchOptions g_options;

void ApplyOptions() {
    if (!g_options.set) return;
    if (g_options.wins_needed > 0) kernel::W32(kRoundsToWin, uint32_t(g_options.wins_needed));
    kernel::W32(kRoundTime, 60);
    kernel::W32(kBloodLevel, 3);
    kernel::W32(kDeathTraps, g_options.death_traps ? 1 : 0);
}

void SetOptions(const mk3d::link::game::Start& start) {
    g_options.set = true;
    g_options.wins_needed = start.wins_needed;
    g_options.death_traps = start.death_traps;
    ApplyOptions();
    LOGR("link: round time 60 s, blood 3, death traps %s", start.death_traps ? "on" : "off");
}

void PickArena(uint32_t seed, const std::string& stage) {
    uint32_t n = 0;
    while (n < 64 && int32_t(kernel::R32(kArenaList + 4 * n)) != -1) n++;
    if (!n) return;
    uint32_t index = seed % n;
    if (!stage.empty()) {
        uint32_t id = uint32_t(std::strtoul(stage.c_str(), nullptr, 10));
        for (uint32_t i = 0; i < n; i++)
            if (kernel::R32(kArenaList + 4 * i) == id) index = i;
    }
    kernel::W32(kArenaCycle, index);
    LOGR("link: arena %u (place %u of %u on the list)", kernel::R32(kArenaList + 4 * index), index, n);
}

}  // namespace
}  // namespace mkd

namespace mk3d::link::game {

using namespace mkd;

void WaitForRequest() { autopilot::WaitForRequest(); }
bool AtHoldPoint() { return autopilot::AtHoldPoint(); }
bool ModeKnown(const std::string& mode) { return autopilot::ActionForMode(mode) >= 0; }

bool Begin(const Start& start) {
    autopilot::Request req;
    req.action = autopilot::ActionForMode(start.mode);
    req.names[0] = start.picks[0];
    req.names[1] = start.picks[1];
    req.unlock_all = true;
    // online, a side the launcher left without a character: that player picks on the select screen (a spectator's
    // game takes both players' picks from their games)
    req.local_side = start.online && !start.watch ? start.side : -1;
    req.watch = start.watch;
    for (int i = 0; i < 2; i++) req.by_player[i] = start.online && start.mode == "kombat" && start.picks[i].empty();
    g_request = req;
    PickArena(start.seed, start.stage);
    SetOptions(start);
    return autopilot::Begin(req);
}

// Versus and Puzzle Kombat return to their select screen after a match, a chess game to the main menu
void Rematch(const Start& start) {
    PickArena(start.seed, start.stage);
    SetOptions(start);
    if (start.mode == "chess") autopilot::Restart(g_request);
    else autopilot::Rematch();
}

void ExpectMenu() { autopilot::ExpectMenu(); }

// the game's own state switch to the main menu task; it leaves through the engine's longjmp to the main loop
void Park(X86Context& ctx) { kernel::CallGuest(ctx, kStateSwitch, {6, kMainMenuTask}); }

bool AutopilotActive() { return autopilot::Active(); }

uint64_t MatchesStarted() { return uint64_t(FightSetups()) + MatchStarts(); }

void MatchBegins(const std::string&) { g_puzzle_base = kernel::R32(kPuzzleMatches); }

void SettleNow(net::Settled& s) { net::game::Settle(s); }

// Fights: a player has won the rounds the match needs (player record + 0x40, zeroed by the fight setup). Chess
// Kombat's game over screen (mode 12) and Puzzle Kombat's match end (its matches counter moved) name their winner,
// reported as 1-0.
bool Outcome(const std::string& mode, const net::Settled& s, int wins_needed, int& winner, uint32_t& r1, uint32_t& r2) {
    winner = 0;
    if (mode == "chess") {
        if (s.v[kSettledMode] == kModeChessOver) winner = s.v[kSettledChessWinner] == 0 ? 1 : 2;
    } else if (mode == "puzzle") {
        uint32_t w = s.v[kSettledPuzzleWinner];
        if (s.v[kSettledPuzzleMatches] != g_puzzle_base && (w == 1 || w == 2)) winner = int(w);
    } else {
        r1 = s.v[kSettledWins1];
        r2 = s.v[kSettledWins2];
        if (int(r1) >= wins_needed || int(r2) >= wins_needed) winner = int(r1) >= wins_needed ? 1 : 2;
        return winner != 0;
    }
    r1 = winner == 1 ? 1 : 0;
    r2 = winner == 2 ? 1 : 0;
    return winner != 0;
}

// a fight's two characters (the side modes' teams and wells are picked again)
bool Fighters(const std::string& mode, int out[2]) {
    if (mode == "chess" || mode == "puzzle") return false;
    out[0] = int(kernel::R32(kP1Character));
    out[1] = int(kernel::R32(kP2Character));
    return out[0] >= 0 && out[0] < 40 && out[1] >= 0 && out[1] < 40;
}

}  // namespace mk3d::link::game

X86_EXTERN_FUNC(__imp__sub_001B4640);
X86_EXTERN_FUNC(__imp__sub_001B4430);
X86_EXTERN_FUNC(__imp__sub_001B3DC0);
X86_EXTERN_FUNC(__imp__sub_001BF070);

// the front end sets the game options (their defaults, or one menu item): a launcher match's go back over them
X86_FUNC(sub_001B4640) {
    __imp__sub_001B4640(ctx, base);
    mkd::ApplyOptions();
}
X86_FUNC(sub_001B4430) {
    __imp__sub_001B4430(ctx, base);
    mkd::ApplyOptions();
}
// sub_001B3DC0(dst, src): an options block copied whole (a profile's made the game's, the options screen's backup)
X86_FUNC(sub_001B3DC0) {
    const uint32_t dst = kernel::R32(ctx.esp + 4);
    __imp__sub_001B3DC0(ctx, base);
    if (dst == mkd::kOptionsBlock) mkd::ApplyOptions();
}

// whether the arenas' death traps work (fastcall, no arguments on the stack): in a launcher match, the match's
X86_FUNC(sub_001BF070) {
    if (!mkd::g_options.set) return __imp__sub_001BF070(ctx, base);
    ctx.eax = mkd::g_options.death_traps ? 1u : 0u;
    ctx.esp += 4;  // ret
}
