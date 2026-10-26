#pragma once
// Armageddon's side of the shared netplay core is netgame.cpp; this is what its launcher side (linkgame.cpp) reads
// back from what both games agreed on (mk3d::net::Settled::v).
#include <cstdint>

namespace mka {

enum SettledValue {
    kSettledWins1,        // round wins, P1 (player record + 0x1C)
    kSettledWins2,        // round wins, P2
    kSettledMode,         // the game mode
    kSettledMatchWinner,  // 0x830934: 1 P1, 2 P2, -1 while undecided
    kSettledRaceOver,     // Motor Kombat: the race object's bit 17 (a kart finished, the results follow)
    kSettledKartPlace1,   // Motor Kombat: player 1's place in the finish order (1..8), 0 while unknown
    kSettledKartPlace2,   // player 2's
};

// The logical bits of the pad update's START and BACK (0x900 here, as in Deception): kept out of what a player sends
// during a match in sync, so one player cannot pause both games
uint32_t PauseBits();

// the default button table (0x54CCA0): every port uses it while a match is in sync, so a logical bit means the same
// in both games
constexpr uint32_t kDefaultButtons = 0x54CCA0;

}  // namespace mka
