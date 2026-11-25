#pragma once
// Costumes: some fighters' looks changed or new, all picked as the game's own costumes are (A, START, LT with either
// for the other colours, colours.h):
//   RAIDEN       his first costume (A) in a deeper blue; its other colours (LT and A) red instead of purple
//   SCORPION     his first costume's other colours (LT and A) black with his first costume's own gold; when both
//                players ask for them, the one the mirror rule puts back in his own colours gets black and orange
//   BO' RAI CHO  his second costume's other colours (LT and START): the second costume with black pants
//   JAX          his second costume's other colours (LT and START): the game's red jacket with a white belt and a
//                black undershirt and hood, his own black gloves; and a costume 3 (L1 and START) that look with red
//                gloves (his own gloves, on his own model: the same collision)
//   DRAHMIN      his second costume's other colours (LT and START) Bronze: charcoal wraps, his green mask, bronze
//                metal; his costume 3 (L1 and START) Crimson: maroon wraps, an ivory mask with red markings, gold glyphs
//                on his banner
//   SHANG TSUNG  a costume 3 on his first costume (L1 and A) black and gold, his gloves black too
//   FROST        a costume 3 on her first costume (L1 and A) Deep Blue: a deep sapphire gi without its marks, a bronze
//                medallion, her gloves' and boots' lines in a darker blue, the belt around her leg and its tie blue,
//                the rest jet black; SQUARE (the pad's X) picks her in All Black (Jet Black): the same with her gloves,
//                boots, belt and tie all black
// Costume 3 is WHITE (L1 on a PlayStation pad) held as the costume it is made on is picked (A for the first, START for
// the second). The game's mirror rule stays: two players with one fighter in the same costume and colours, P2 the other
// colours. A player's own look of a costume (costume 3, a Square look) goes in either row of that costume, the one the
// other player of that fighter leaves (as if it were the other colours): that player keeps the row it asked for (the
// game's own rule at a pick would give the second of them the other colours whatever the costumes); both players in
// the same look: P2 that costume in its own colours; in two looks: a row each.
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
// deeper blue, alt_jax.ssf with Jax's alternate); a look made from another section is a copy of it at the end of a copy
// of the file (x_rayden.ssf, x_scorpion.ssf, x_drunkenfighter2.ssf, x_jax.ssf, x_drahmin2.ssf, x_shangtsung.ssf,
// x_subzerette.ssf) under the section id of the row that shows it, with an entry of its own and a copy of the file's
// list naming that file, in guest memory taken at the first frame (the same address on both games of a match). Before
// every lookup (sub_00103150) the rows hold the looks' entries (a player's own look for that player, a look of the
// kind having it under both rows' section ids); a look with its own copy of the fighter's shared section (row +0x80:
// Drahmin's mask) has the row's shared entry point at it for the fighter whose lookup lands on that look (the loader
// reads it right after). A texture changed in one look of a costume is renamed (the game knows textures by name, and
// both sections of a costume have the same names). Raiden's picture on the select screen (pselart.ssf) is made in the
// deeper blue too.
// MKDA_ART_DIR=<folder> (development) gives the game an artxbox file from that folder instead of its own or the one
// made here; MKDA_TEST_PICK_LT / _START (autopilot.cpp) pick with LT held or with START in tests.
#include <cstdint>

namespace mkda::costumes {

// the costume word's bits beside the game's own (bits 0 to 15): the pick held LT (asked for the other colours,
// colours.cpp), a fighter's mirror look (Scorpion's black and orange), costume 3, a Square look (Frost's All Black; for
// both bit 0 the costume it is made on, bit 1 the row it takes); all agreed online with the pick
constexpr uint32_t kAltPick = 0x01000000, kMirrorLook = 0x02000000, kThird = 0x04000000, kFourth = 0x08000000;

// a player's pick has just been taken (colours.cpp, after the other colours): Square gives a fighter's Square look,
// WHITE held as the costume a fighter's costume 3 is made on is picked gives costume 3, any other pick neither; with the
// other player in that fighter, the looks and the costume they are made on share the rows (above)
void Picked(uint32_t player);
// the select screen's end (colours.cpp), before the mirror rule: costume 3 and Square looks only where a fighter has
// them, both players in one look settled, the looks and the costume they are made on sharing the rows; the side the
// mirror rule turns, the one in a look when only one is, else P2
int Settle(uint32_t word[2]);

// the select screen's end (colours.cpp), once the agreed costumes are in the words and the mirror rule settled them:
// kMirrorLook for the player who gets a mirror look, none for anybody else
void SelectEnds(uint32_t word[2]);

}  // namespace mkda::costumes
