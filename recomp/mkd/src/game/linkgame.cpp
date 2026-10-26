// Deception's side of the shared launcher link (recomp/common/src/mk3d/linkgame.h): the autopilot drives its front
// end, and a match's result comes from what both games agreed on (netgame.h).
#include "mk3d/linkgame.h"

#include <cstdlib>
#include <string>

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

// The arena of a versus or puzzle match comes from sub_00040640, which takes the next one of a list (0x3E4D30, -1 at
// the end) that is still locked or not, from a cycle index (0x620F44) that moves on every match: two players who
// played a different number of matches since they started their games would load different arenas. Both set the
// index from the match's seed instead (or to the stage the launcher names, an arena id on the list).
constexpr uint32_t kArenaList = 0x3E4D30, kArenaCycle = 0x620F44;

// A fight's round end (sub_00083960) ends the match once the round's winner has as many round wins as 0x5FC9A4 (the
// options' rounds, 2 by default) when no profile is in use (0x620F24 bit 0, as in a launcher match; a profile makes
// it 2): the launcher's best of N.
constexpr uint32_t kRoundsToWin = 0x5FC9A4;

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
    // online, a side the launcher left without a character: that player picks on the select screen
    req.local_side = start.online ? start.side : -1;
    for (int i = 0; i < 2; i++) req.by_player[i] = start.online && start.mode == "kombat" && start.picks[i].empty();
    g_request = req;
    PickArena(start.seed, start.stage);
    if (start.wins_needed > 0) kernel::W32(kRoundsToWin, uint32_t(start.wins_needed));
    return autopilot::Begin(req);
}

// Versus and Puzzle Kombat return to their select screen after a match, a chess game to the main menu
void Rematch(const Start& start) {
    PickArena(start.seed, start.stage);
    if (start.wins_needed > 0) kernel::W32(kRoundsToWin, uint32_t(start.wins_needed));
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

}  // namespace mk3d::link::game
