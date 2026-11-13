#pragma once
// What the shared launcher link (link.cpp) needs from a game, implemented by each game's layer (its linkgame.cpp,
// mostly on its autopilot): driving the front end to a match and back, and reading a match's result.
#include <cstdint>
#include <string>

struct X86Context;

namespace mk3d::net {
struct Settled;
}

namespace mk3d::link::game {

// boot: drive the game to its main menu and hold there (the hold point) until a start
void WaitForRequest();
bool AtHoldPoint();
// whether the game has this mode ("kombat", "chess", "puzzle", "motor", "practice"...)
bool ModeKnown(const std::string& mode);
// The match's settings both games got from the launcher: the seed is the same on both (what the game derives from it,
// such as the arena when no stage is named, then agrees too), stage is empty unless the launcher names one.
struct Start {
    std::string mode;
    std::string picks[2];  // roster ids, empty for the cursor's own; online in a fight mode: that side's player picks
                           // on the select screen (on their own PC; the other game takes it over netplay, SetLocalPick)
    // a pick's costume, -1 for the one the pick gives: online, a launcher's "<character>.<costume>" (the fighter a
    // ranked set's winner keeps, as they won with it), which both games give that side once it is picked
    int64_t costumes[2] = {-1, -1};
    uint32_t seed = 0;
    std::string stage;
    int wins_needed = 2;   // rounds a player must win (from the launcher's best of N)
    bool death_traps = true;  // the arenas' death traps (Deception, Armageddon: the options' DEATH TRAPS)
    bool ranked = false;   // a ranked match (no game rule hangs on it since 1.0.5: Deadly Alliance's Acid Bath is back)
    bool online = false;
    int side = 0;          // online: this PC's player, 0 (P1) or 1 (P2)
    bool watch = false;    // a spectator: no player on this PC, both picks come from the players' games
};
// from the hold point: the autopilot drives the front end to the mode with these picks; false when it cannot start
bool Begin(const Start& start);
// the same picks again (a new seed): from the select screen the mode returns to, or from the main menu
void Rematch(const Start& start);
// the game is being sent back to the main menu: hold there once it is up
void ExpectMenu();
// on the main thread at the top of a frame: switch the game to its main menu (through the engine's own state switch;
// it may leave the frame and not return)
void Park(X86Context& ctx);
// still pressing buttons
bool AutopilotActive();

// fights set up and side mode matches started so far (an offline match has started once it moves)
uint64_t MatchesStarted();
// a match starts now (a start or a rematch): what its result is measured against
void MatchBegins(const std::string& mode);
// what an agreed frame shows, read now (offline: no netplay to agree with)
void SettleNow(net::Settled& s);
// the match's result from what both games agree on: true with the winner (1 or 2) and the rounds each won (what a
// mode counts as rounds) once it is decided
bool Outcome(const std::string& mode, const net::Settled& s, int wins_needed, int& winner, uint32_t& p1_rounds,
             uint32_t& p2_rounds);
// the fighters of a fight mode's match (the game's character indices, which a start's picks take as numbers, so a
// rematch can have the same two) and their costumes as the fight was set up (-1 not known, the game's own costume
// words, which a start's picks take after the character); false for a mode without them
bool Fighters(const std::string& mode, int out[2], int64_t costumes[2]);

}  // namespace mk3d::link::game
