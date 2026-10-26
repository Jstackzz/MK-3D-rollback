// Armageddon's side of the shared launcher link (recomp/common/src/mk3d/linkgame.h): the autopilot drives its front
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

namespace mka {
namespace {

constexpr uint32_t kStateSwitch = 0x19CA30;  // sub_0019CA30(priority, task)

autopilot::Request g_request;

// The arena of a versus fight comes from sub_000393A0, which takes the next one of a list (0x548F88, -1 at the end)
// that is not locked (64-bit masks 0x7363D8, defaults 0x548A40, sub_00039440), from a place on the list (0x831A64)
// that moves on every fight and that the boot draws from the random generator, which main seeds from the time stamp
// counter: two games would load different arenas. Both set the place from the match's seed instead (or to the stage
// the launcher names, an arena id on the list), with every arena unlocked.
constexpr uint32_t kArenaList = 0x548F88, kArenaCycle = 0x831A64, kArenaUnlocks = 0x7363D8;

// A round end (sub_00098B50) ends the match once the round's winner has as many round wins as 0x723BC0 (the options'
// rounds to win, 2 by default; 2 in an Xbox Live match): the launcher's best of N.
constexpr uint32_t kRoundsToWin = 0x723BC0;

void PickArena(uint32_t seed, const std::string& stage) {
    uint32_t n = 0;
    while (n < 64 && int32_t(kernel::R32(kArenaList + 4 * n)) != -1) n++;
    if (!n) return;
    kernel::W32(kArenaUnlocks, 0xFFFFFFFF);
    kernel::W32(kArenaUnlocks + 4, 0xFFFFFFFF);
    uint32_t index = seed % n;
    if (!stage.empty()) {
        uint32_t id = uint32_t(std::strtoul(stage.c_str(), nullptr, 10));
        for (uint32_t i = 0; i < n; i++)
            if (kernel::R32(kArenaList + 4 * i) == id) index = i;
    }
    kernel::W32(kArenaCycle, index);
    LOGI("link: arena %u (place %u of %u on the list)", kernel::R32(kArenaList + 4 * index), index, n);
}

void Prepare(const mk3d::link::game::Start& start) {
    if (start.mode == "motor") {
        autopilot::SetTrack(start.seed, start.stage);
    } else {
        PickArena(start.seed, start.stage);
        if (start.wins_needed > 0) kernel::W32(kRoundsToWin, uint32_t(start.wins_needed));
    }
}

}  // namespace
}  // namespace mka

namespace mk3d::link::game {

using namespace mka;

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
    Prepare(start);
    return autopilot::Begin(req);
}

// a versus fight returns to its select screen, a Motor Kombat race to its kart select
void Rematch(const Start& start) {
    Prepare(start);
    autopilot::Rematch();
}

void ExpectMenu() { autopilot::ExpectMenu(); }

// the game's own state switch to the main menu task (priority 3, as the game's own way back); it leaves through the
// engine's longjmp to the main loop
void Park(X86Context& ctx) { kernel::CallGuest(ctx, kStateSwitch, {3, kMainMenuTask}); }

bool AutopilotActive() { return autopilot::Active(); }

uint64_t MatchesStarted() { return uint64_t(FightSetups()) + MatchStarts(); }

void MatchBegins(const std::string&) {}

void SettleNow(net::Settled& s) { net::game::Settle(s); }

// Fights: a player has won the rounds the match needs (player record + 0x1C, zeroed by the fight setup). Motor
// Kombat: the race is over and the finish order names both players' places; the better placed player wins, reported
// as 1-0.
bool Outcome(const std::string& mode, const net::Settled& s, int wins_needed, int& winner, uint32_t& r1, uint32_t& r2) {
    winner = 0;
    if (mode == "motor") {
        uint32_t p1 = s.v[kSettledKartPlace1], p2 = s.v[kSettledKartPlace2];
        if (s.v[kSettledRaceOver] && p1 && p2 && p1 != p2) winner = p1 < p2 ? 1 : 2;
        r1 = winner == 1 ? 1 : 0;
        r2 = winner == 2 ? 1 : 0;
        return winner != 0;
    }
    r1 = s.v[kSettledWins1];
    r2 = s.v[kSettledWins2];
    if (int(r1) >= wins_needed || int(r2) >= wins_needed) winner = int(r1) >= wins_needed ? 1 : 2;
    return winner != 0;
}

// a fight's two characters (Motor Kombat's karts are picked again; 0x40 none)
bool Fighters(const std::string& mode, int out[2]) {
    if (mode == "motor") return false;
    out[0] = int(kernel::R32(kP1Character));
    out[1] = int(kernel::R32(kP2Character));
    return out[0] >= 0 && out[0] < 0x40 && out[1] >= 0 && out[1] < 0x40;
}

}  // namespace mk3d::link::game
