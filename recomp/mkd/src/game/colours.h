#pragma once
// The costumes' other colours (MK3D Online 1.0.7), as in Deadly Alliance (recomp/mkda/src/game/colours.h): holding LT
// (L1) while picking gives the costume its other colours (the "ap" texture sections), LT and A the first costume's,
// LT and START the second's. The player record's costume flags (+0x14) keep it as bit 1, as the game's own mirror rule
// (sub_0017B440) sets it. The fight's arena setup settles both players' flags the same way on every game of a match.
#include <cstdint>

namespace mkd::colours {

// the versus fight task's first step (sub_000834C0, linkgame.cpp), after the agreed picks went over the records
// online: P2 the other colours when both have the same fighter in the same costume and colours
void SelectEnds();

}  // namespace mkd::colours
