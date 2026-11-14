#pragma once
// The costumes' other colours (MK3D Online 1.0.7), as in Deadly Alliance (recomp/mkda/src/game/colours.h): holding LT
// (L1) while picking gives the costume its other colours (the "ap" texture sections), LT and A the first costume's,
// LT and START the second's. Until the select screen ends the choice is the select block's colours flag (+0x28 P1,
// +0x5C P2), which the game's own mirror rule (sub_00207B90) sets; the screen's end ORs the flags into the records'
// costume bits (bit 1). Online the pick words carry it (autopilot.cpp: bit 0 the second costume, bit 1 the colours).
#include <cstdint>

namespace mka::colours {

// the select screen's end (sub_002056E0, linkgame.cpp), offline: P2 the other colours when both have the same fighter
// in the same costume and colours (online the agreed picks settle it there, by the same rule)
void SelectEnds();

}  // namespace mka::colours
