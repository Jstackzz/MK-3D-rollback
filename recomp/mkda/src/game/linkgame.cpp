// Deadly Alliance's side of the shared launcher link (recomp/common/src/mk3d/linkgame.h): the autopilot drives its
// front end, and a match's result comes from what both games agreed on (netgame.h).
#include "mk3d/linkgame.h"

#include <cstdlib>
#include <string>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "game/autopilot.h"
#include "game/events.h"
#include "game/frame.h"
#include "game/netgame.h"
#include "kernel/kernel.h"
#include "mk3d/netgame.h"
#include "mk3d/netplay.h"

namespace mkda {
namespace {

// The arena of a fight comes from sub_0006F570 while the fight task loads, before the meeting point: in a versus
// fight the next arena of a list (0x24D800, -1 at the end) that is open (sub_00116400: a bit of 0x273584, or of the
// profile's 0x421538), from a cycle index (0x2C5DE8) that moves on every fight. Two players who fought a different
// number of fights since they started their games, or with different profiles, would load different arenas. Both
// take the arena from the match's seed instead, among the ones open without a profile (or the stage the launcher
// names, an arena id), through the engine's own chosen arena (0x42C754, events.h SetNextArena).
void PickArena(uint32_t seed, const std::string& stage) {
    uint32_t open = kernel::R32(kArenaDefaults), ids[64], all[64], n = 0, n_all = 0;
    for (uint32_t i = 0; i < 64; i++) {
        int32_t id = int32_t(kernel::R32(kArenaList + 4 * i));
        if (id < 0) break;
        if (id >= 0x19) continue;  // what the chosen arena takes: 0 to 24
        all[n_all++] = uint32_t(id);
        if (open >> id & 1) ids[n++] = uint32_t(id);
    }
    if (!n) {
        if (!n_all) return;
        for (uint32_t i = 0; i < n_all; i++) ids[i] = all[i];
        n = n_all;
    }
    uint32_t arena = ids[seed % n];
    if (!stage.empty()) {
        char* end = nullptr;
        unsigned long id = std::strtoul(stage.c_str(), &end, 10);
        if (end && !*end && id < 0x19) arena = uint32_t(id);
    }
    LOGI("link: arena %u (seed %u, %u open)", arena, seed, n);
    SetNextArena(int(arena));
}

// the match's options as the launcher sets them: the rounds a player must win (the round end sub_0006F9A0 ends the
// match at 0x31FD18) and no bonus game (Test Your Might or Sight) in place of a fight
void SetOptions(int wins_needed) {
    if (wins_needed > 0) kernel::W32(kOptRounds, uint32_t(wins_needed));
    kernel::W32(kOptBonusGames, kBonusGamesNever);
}

}  // namespace
}  // namespace mkda

namespace mk3d::link::game {

using namespace mkda;

void WaitForRequest() { autopilot::WaitForRequest(); }
bool AtHoldPoint() { return autopilot::AtHoldPoint(); }
bool ModeKnown(const std::string& mode) { return autopilot::ModeForName(mode) >= 0; }

bool Begin(const Start& start) {
    autopilot::Request req;
    req.mode = autopilot::ModeForName(start.mode);
    req.names[0] = start.picks[0];
    req.names[1] = start.picks[1];
    req.unlock_all = true;
    // online, a side the launcher left without a character: that player picks on the select screen
    req.local_side = start.online ? start.side : -1;
    for (int i = 0; i < 2; i++) req.by_player[i] = start.online && start.mode == "kombat" && start.picks[i].empty();
    PickArena(start.seed, start.stage);
    SetOptions(start.wins_needed);
    return autopilot::Begin(req);
}

// a versus match returns to its select screen, where the autopilot picks the same characters again (players who pick
// on the select screen pick again)
void Rematch(const Start& start) {
    PickArena(start.seed, start.stage);
    SetOptions(start.wins_needed);
    autopilot::Rematch();
}

void ExpectMenu() { autopilot::ExpectMenu(); }

// the game's own state switch to the main menu task (as backing out of character select does, priority 4); it leaves
// through the engine's longjmp to the main loop
void Park(X86Context& ctx) {
    kernel::W32(kPaused, 0);  // a pause menu open in a fight is gone with it (as its quit item does)
    kernel::CallGuest(ctx, kStateSwitch, {4, kMainMenuTask});
}

bool AutopilotActive() { return autopilot::Active(); }

uint64_t MatchesStarted() { return FightSetups(); }

void MatchBegins(const std::string&) {}

void SettleNow(net::Settled& s) { net::game::Settle(s); }

// a player has won the rounds the match needs (zeroed by the fight setup)
bool Outcome(const std::string&, const net::Settled& s, int wins_needed, int& winner, uint32_t& r1, uint32_t& r2) {
    r1 = s.v[kSettledWins1];
    r2 = s.v[kSettledWins2];
    winner = 0;
    if (int(r1) >= wins_needed || int(r2) >= wins_needed) winner = int(r1) >= wins_needed ? 1 : 2;
    return winner != 0;
}

}  // namespace mk3d::link::game
