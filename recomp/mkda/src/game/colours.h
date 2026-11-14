#pragma once
// The costumes' other colours (MK3D Online 1.0.7). Every costume of the regular fighters has a second set of colours
// in the game's data (its "ap" texture sections: ap_scorpion.sec beside scorpion.sec), which the game itself gives
// only to the second player of a mirror match. Holding LT (L1 on a PlayStation pad) while picking gives them: LT and A
// the first costume's, LT and START the second costume's (START picks the second costume, as ever). The costume word
// keeps it as its bit 1 (bit 0 the second costume), as the game's own mirror rule sets it.
//
// The select screen's end settles both players' words the same way on every game of a match (a spectator's too): the
// costumes as both games agreed on them online (autopilot.cpp sends the whole word with the pick), and when both
// players have the same fighter in the same costume and colours, P2 the other colours.
#include <cstdint>

struct X86Context;

namespace mkda::colours {

// a player's pick has just been taken (sub_001112A0, events.cpp): LT held gives the costume's other colours
void Picked(X86Context& ctx, uint32_t player);
// the select screen ends on its way to the fight (sub_0010D2B0, linkgame.cpp), before the fight loads its fighters
void SelectEnds();

}  // namespace mkda::colours
