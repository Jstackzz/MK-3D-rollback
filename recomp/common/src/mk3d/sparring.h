#pragma once
// Online Practice ("sparring", MK3D Online 1.0.7): two friends in a lobby found by its code fight as long as they like.
// Each game plays the fight with its own round time option set to no time limit (linkgame.cpp), and these rules keep
// both fighters up: health back to full once a fighter took no damage for a while, and at once below a floor, so
// nobody is knocked out. A player's BACK goes back to the select screen (netplay.cpp: the match in sync ends, the
// launcher reports it and the lobby's next match brings both players to the select screen again).
//
// The rules run at the top of every frame of the match in sync, the same on both games and a spectator's, and what
// they remember lives in guest memory, which a rollback puts back with the rest of the game.
#include <cstdint>

namespace mk3d::sparring {

// the match the launcher started now is Online Practice (link.cpp, at each start)
void Set(bool on);
bool On();

// From a game's frame hook, at the top of a frame: the fighters' health (floats, 1.0 full; P1's, P2's), kept up while
// the match is Online Practice and in sync. `fighting`: a round is under way (the game's own test), so nothing is
// touched while a fight loads or after it.
void KeepHealth(const uint32_t health[2], bool fighting);

}  // namespace mk3d::sparring
