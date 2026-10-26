#pragma once
// Deadly Alliance's side of the shared netplay core is netgame.cpp; this is what its launcher side (linkgame.cpp)
// reads back from what both games agreed on (mk3d::net::Settled::v).

namespace mkda {

enum SettledValue {
    kSettledWins1,   // round wins, P1 (0x42C76C, zeroed by the fight setup)
    kSettledWins2,   // round wins, P2 (0x42C768)
    kSettledMode,    // the game mode
    kSettledWinner,  // 0x42C758: 0 P1, 1 P2, -1 while nobody has won the match
    kSettledTask,    // the state task (0x2D4BF8)
};

}  // namespace mkda
