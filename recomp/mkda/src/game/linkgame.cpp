// Deadly Alliance's side of the shared launcher link (recomp/common/src/mk3d/linkgame.h): the autopilot drives its
// front end, and a match's result comes from what both games agreed on (netgame.h).
#include "mk3d/linkgame.h"

#include <algorithm>
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
// take the arena from the match's seed instead, among all of the list (a launcher match has every arena open, the
// autopilot's Unlock; or the stage the launcher names, an arena id), through the engine's own chosen arena (0x42C754,
// events.h SetNextArena). A ranked match is never on Acid Bath (arena 0): both games know it from the launcher, so they
// leave it off the list alike.
constexpr int kAcidBath = 0;
bool g_ranked = false;  // this match is ranked (the launcher's start line)

void PickArena(uint32_t seed, const std::string& stage) {
    uint32_t ids[64], n = 0;
    for (uint32_t i = 0; i < 64; i++) {
        int32_t id = int32_t(kernel::R32(kArenaList + 4 * i));
        if (id < 0) break;
        if (id >= 0x19) continue;  // what the chosen arena takes: 0 to 24
        if (g_ranked && id == kAcidBath) continue;
        ids[n++] = uint32_t(id);
    }
    if (!n) return;
    uint32_t arena = ids[seed % n];
    // MKDA_TEST_ARENA=<id> (tests: both games of a one-PC run inherit it) stands in for a stage the launcher names
    const char* test = std::getenv("MKDA_TEST_ARENA");
    const std::string named = !stage.empty() ? stage : test ? std::string(test) : std::string();
    if (!named.empty()) {
        char* end = nullptr;
        unsigned long id = std::strtoul(named.c_str(), &end, 10);
        // only an arena of the list: another id below 25 is no fight's (21 crashed both games at their load)
        if (end && !*end && std::find(ids, ids + n, uint32_t(id)) != ids + n) arena = uint32_t(id);
        else LOGW("link: stage %s is not on the arena list, the seed's arena instead", named.c_str());
    }
    LOGR("link: arena %u (seed %u, %u on the list)", arena, seed, n);
    SetNextArena(int(arena));
}

// The select screen has an arena choice of its own (BLACK, then START on the arena; autopilot.cpp): it leaves the
// arena in 0x42C754, which the fight's arena pick takes. Online each game had only its own player's choice, and the
// match's arena (SetNextArena) went over it in both. Whatever 0x42C754 holds when a player picks goes with the pick,
// and both games fight on what the two choices give (AgreeOnTheFight).
uint32_t g_seed = 0;  // the match's seed (the launcher's start line)
int64_t g_fight_costumes[2] = {-1, -1};  // both players' costumes as the select screen ended (-1 not yet)

// The arena both games fight on from their players' choices on the select screen (-1 none): both chose, the match's
// seed picks between them (bit 7 set: P2's, else P1's); one chose, that one; neither, -1: the match's own (PickArena).
// Deception and Armageddon go by the same rule, with the same pick words.
// A choice counts only when it is an arena a player can choose: on the versus list or in the select screen's own
// table (0x274398: id, two string ids; increasing ids). Both games take the same choices the same way, so an arena word
// that names no arena (only another program could send one, and its load would end both games) leaves the match's own.
constexpr uint32_t kChoiceTable = 0x274398;
bool Choosable(int id) {
    if (id < 0 || id >= 0x19) return false;
    for (uint32_t i = 0; i < 64; i++) {
        const int32_t listed = int32_t(kernel::R32(kArenaList + 4 * i));
        if (listed < 0) break;
        if (listed == id) return true;
    }
    int32_t prev = -1;
    for (uint32_t i = 0; i < 64; i++) {
        const int32_t offered = int32_t(kernel::R32(kChoiceTable + 12 * i));
        if (offered <= prev || offered >= 0x19) break;
        if (offered == id) return true;
        prev = offered;
    }
    return false;
}

int AgreedArena(int p1, int p2, uint32_t seed) {
    if (!Choosable(p1) || (g_ranked && p1 == kAcidBath)) p1 = -1;
    if (!Choosable(p2) || (g_ranked && p2 == kAcidBath)) p2 = -1;
    if (p1 >= 0 && p2 >= 0) return (seed & 0x80) ? p2 : p1;
    return p1 >= 0 ? p1 : p2;
}

// Online and watched, as the select screen ends on its way to the fight (before anything loads): the characters both
// games agreed on over the screen's own (the autopilot sends what its player picked, and picks what came for the
// other side: a game set up otherwise would part from the other at once), and the arena the players' choices give
// over the match's own (a spectator's game: both players' as they came). The costumes are set as the picks come
// (autopilot.cpp); a word unlike the agreed one is only told.
void AgreeOnTheFight() {
    int character[2] = {-1, -1}, arena[2] = {-1, -1};
    uint32_t costume[2] = {};
    bool any = false;
    for (int s = 0; s < 2; s++) {
        const uint32_t word = kernel::R32(kCostume[s]);
        g_fight_costumes[s] = word ? int64_t(kernel::R32(word)) : -1;
    }
    for (int s = 0; s < 2; s++) {
        if (!autopilot::AgreedPick(s, character[s], costume[s], arena[s])) continue;
        any = true;
        const uint32_t was = kernel::R32(kCharacter[s]);
        if (was != uint32_t(character[s]))
            LOGR("link: P%d is character %u in this game, the match's pick is %d: that one", s + 1, was, character[s]);
        kernel::W32(kCharacter[s], uint32_t(character[s]));
        const uint32_t word = kernel::R32(kCostume[s]);
        if (word && kernel::R32(word) != costume[s])
            LOGR("link: P%d's costume word is %X in this game, %X in the pick", s + 1, kernel::R32(word), costume[s]);
    }
    if (!any) return;
    const int agreed = AgreedArena(arena[0], arena[1], g_seed);
    LOGR("link: arena choices P1 %d, P2 %d: %s %d", arena[0], arena[1],
         agreed >= 0 ? "the match is on arena" : "the match's own arena", agreed >= 0 ? agreed : -1);
    if (agreed >= 0) SetNextArena(agreed);
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
    // online, a side the launcher left without a character: that player picks on the select screen (a spectator's
    // game takes both players' picks from their games)
    req.local_side = start.online && !start.watch ? start.side : -1;
    req.watch = start.watch;
    for (int i = 0; i < 2; i++) {
        req.by_player[i] = start.online && start.mode == "kombat" && start.picks[i].empty();
        req.costumes[i] = start.online && start.mode == "kombat" ? start.costumes[i] : -1;
        g_fight_costumes[i] = -1;
    }
    g_seed = start.seed;
    g_ranked = start.ranked;
    PickArena(start.seed, start.stage);
    SetOptions(start.wins_needed);
    return autopilot::Begin(req);
}

// a versus match returns to its select screen, where the autopilot picks the same characters again (players who pick
// on the select screen pick again)
void Rematch(const Start& start) {
    g_seed = start.seed;
    g_ranked = start.ranked;
    g_fight_costumes[0] = g_fight_costumes[1] = -1;
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

// the fight's two characters (0x18 none) and their costumes as the select screen ended
bool Fighters(const std::string&, int out[2], int64_t costumes[2]) {
    out[0] = int(kernel::R32(kCharacter[0]));
    out[1] = int(kernel::R32(kCharacter[1]));
    costumes[0] = g_fight_costumes[0];
    costumes[1] = g_fight_costumes[1];
    return out[0] >= 0 && out[0] < 0x18 && out[1] >= 0 && out[1] < 0x18;
}

}  // namespace mk3d::link::game

// Every press on the select screen starts its idle count again (fastcall, no arguments on the stack): task 0x203E
// (sub_0010D710) sends the game back to the main menu after 2700 frames without one. Online and watched there is no
// count: nobody's time runs out on the select screen of a launcher match (autopilot.cpp).
X86_EXTERN_FUNC(__imp__sub_0010D250);
X86_FUNC(sub_0010D250) {
    if (!mkda::autopilot::HoldsIdle()) return __imp__sub_0010D250(ctx, base);
    ctx.esp += 4;  // ret
}

// The select screen's reset (no arguments): at its setup (sub_0010CB00), and at the select task's end on its way to
// the fight (0x10CAAE, 0x10CAC4, 0x10CAD8), where the agreed picks and arena go over the screen's
X86_EXTERN_FUNC(__imp__sub_0010D2B0);
X86_FUNC(sub_0010D2B0) {
    constexpr uint32_t kSelectSetup = 0x10CB00;  // follows the select task's code
    const uint32_t from = kernel::R32(ctx.esp);
    __imp__sub_0010D2B0(ctx, base);
    if (from > mkda::kSelectTask && from < kSelectSetup) mkda::AgreeOnTheFight();
}
