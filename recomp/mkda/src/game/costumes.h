#pragma once
// Costumes: some fighters' looks changed or new, all picked as the game's own costumes are (A, START, LT with either
// for the other colours, colours.h):
//   RAIDEN       his first costume (A) in a deeper blue; its other colours (LT and A) red instead of purple
//   SCORPION     his first costume's other colours (LT and A) black and yellow; when both players ask for them, the one
//                the mirror rule puts back in his own colours gets black and orange instead
//   BO' RAI CHO  his second costume's other colours (LT and START): the second costume with black pants
//   JAX          his second costume's other colours (LT and START): the game's red jacket with red gloves (his own
//                gloves, on his own model: the same collision)
// The game's mirror rule stays: two players with one fighter in the same costume and colours, P2 the other colours.
//
// Each fighter's costumes are a row of the game's table (0x243B40 + character * 0x110): +0x00 the first costume's
// section entry (rayden.sec), model ids at +4 and +8, +0x30 the second costume's (alt_rayden.sec, in its own file
// alt_rayden.ssf), +0x60 and +0x70 their other colours (ap_rayden.sec, ap_alt_rayden.sec, in the same files).
// sub_00103150(character, colours, costume) picks one when a fighter loads (the select screen's model, the fight); the
// section goes into that player's own slots, where the models find it by its id. An entry {name, the file's list,
// type} names its file through that list: every section of the file with its offset and size (rayden.ssf's at
// 0x238C20).
//
// Nothing ships with the game's data: at the first frame the looks are made from the player's own files (palette
// rules and maps of some textures' texels, costumes.cpp and costumes_maps.h) into ExeDir()\costumes, and an open hook
// gives the game those. A change of a section that keeps its size is made in the file itself (rayden.ssf in the
// deeper blue, alt_jax.ssf with the red gloves); a look made from another section is a copy of it at the end of a copy
// of the file (x_rayden.ssf, x_scorpion.ssf, x_drunkenfighter2.ssf) under the section id of the row that shows it,
// with an entry of its own and a copy of the file's list naming that file, in guest memory taken at the first frame
// (the same address on both games of a match). Before every lookup (sub_00103150) the rows hold the looks' entries. A
// texture changed in one look of a costume is renamed (the game knows textures by name, and both sections of a costume
// have the same names). Raiden's picture on the select screen (pselart.ssf) is made in the deeper blue too.
// MKDA_ART_DIR=<folder> (development) gives the game an artxbox file from that folder instead of its own or the one
// made here; MKDA_TEST_PICK_LT / _START (autopilot.cpp) pick with LT held or with START in tests.
#include <cstdint>

namespace mkda::costumes {

// the costume word's bits beside the game's own (bits 0 to 15): the pick held LT (asked for the other colours,
// colours.cpp), and a fighter's mirror look (Scorpion's black and orange); both agreed online with the pick
constexpr uint32_t kAltPick = 0x01000000, kMirrorLook = 0x02000000;

// the select screen's end (colours.cpp), once the agreed costumes are in the words and the mirror rule settled them:
// kMirrorLook for the player who gets a mirror look, none for anybody else
void SelectEnds(uint32_t word[2]);

}  // namespace mkda::costumes
