#pragma once
// Deception's side of the shared netplay core is netgame.cpp; this is what its launcher side (linkgame.cpp) reads
// back from what both games agreed on (mk3d::net::Settled::v).

namespace mkd {

enum SettledValue {
    kSettledWins1,          // round wins, P1 (player record + 0x40)
    kSettledWins2,          // round wins, P2
    kSettledMode,           // the game mode (12: Chess Kombat's game over)
    kSettledChessWinner,    // 0x600518: 0 P1, 1 P2
    kSettledPuzzleMatches,  // 0x620324: Puzzle Kombat matches finished
    kSettledPuzzleWinner,   // 0x620320: 1 P1, 2 P2
};

}  // namespace mkd
