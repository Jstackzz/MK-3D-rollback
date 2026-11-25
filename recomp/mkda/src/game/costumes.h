#pragma once
// Costumes: every fighter's own two costumes and their other colours as the game has them (A, START, LT with either for
// the other colours, colours.h), and new looks on two buttons of their own, SQUARE (the pad's X) and TRIANGLE (the
// pad's Y); a fighter with one new look has it on SQUARE:
//   RAIDEN       on his first costume: SQUARE a deeper blue, TRIANGLE red
//   SCORPION     on his first costume: SQUARE black with its own gold (his mask, trims and studs as they are),
//                TRIANGLE black and orange (his gear's gold rims orange too)
//   BO' RAI CHO  on his second costume: SQUARE black pants
//   JAX          on his second costume's other colours (the game's red jacket): SQUARE with a white belt and a black
//                undershirt and hood, his own black gloves; TRIANGLE that look with red gloves (his own gloves, on his
//                own model: the same collision)
//   DRAHMIN      on his second costume: SQUARE Bronze (made from its other colours): charcoal wraps, his green mask,
//                bronze metal, a red banner; TRIANGLE Crimson: maroon wraps with the logo on his chest gold, an ivory
//                mask with red markings, gold glyphs on his banner
//   SHANG TSUNG  on his first costume: SQUARE black and gold, his gloves black too
//   FROST        on her first costume: SQUARE All Black (Jet Black): a deep sapphire gi without its marks, a bronze
//                medallion, the rest jet black; TRIANGLE Deep Blue: the same with her gloves' and boots' lines in a
//                darker blue and the belt around her leg and its tie blue
// SQUARE or TRIANGLE stands for the press that picks the costume the look is made on (A for the first, START for the
// second). In 1.0.16 to 1.0.18 some looks took the place of the game's own costume or other colours and costume 3 was
// WHITE (L1) held with the pick; outside practice WHITE opens the select screen's HANDICAP, which takes no pick while it
// is open, so held before START or A it never picked online. The game's mirror rule stays: two players with one
// fighter in the same costume and colours, P2 the other colours. A player's own look of a costume is in their side's
// row of that costume from the pick on, P1 its own, P2 its other colours (each look is under both rows' section ids,
// the same look either way), and never moves: a look that moved after its player's game had shown it on the select
// screen parted the two games of a match (1.0.17 and 1.0.18 moved it to the row the other player of that fighter
// left). A plain costume of that fighter takes the row the look leaves, whatever it asked for; two looks, the same one
// too, are a row apart already.
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
// gives the game those (a change made in the file itself, the in_place kind, is used by no fighter since 1.0.19: the
// game's own costumes stay as they are); a look made from one of the costume's sections is a copy of it at the end of a
// copy of the file (x_rayden.ssf, x_scorpion.ssf, x_drunkenfighter2.ssf, x_jax.ssf, x_drahmin2.ssf, x_shangtsung.ssf,
// x_subzerette.ssf) under the section id of the row that shows it, with an entry of its own and a copy of the file's
// list naming that file, in guest memory taken at the first frame (the same address on both games of a match). Before
// every lookup (sub_00103150) the rows hold the looks' entries (a player's own look for that player, a look of the
// kind having it under both rows' section ids); a look with its own copy of the fighter's shared section (row +0x80:
// Drahmin's mask) has the row's shared entry point at it for the fighter whose lookup lands on that look (the loader
// reads it right after). A texture changed in one look of a costume is renamed (the game knows textures by name, and
// both sections of a costume have the same names).
// MKDA_ART_DIR=<folder> (development) gives the game an artxbox file from that folder instead of its own or the one
// made here; MKDA_TEST_PICK_LT / _START (autopilot.cpp) pick with LT held or with START in tests.
#include <cstdint>

namespace mkda::costumes {

// the costume word's bits beside the game's own (bits 0 to 15): the pick held LT (asked for the other colours,
// colours.cpp), a fighter's mirror look (none since 1.0.19), a Triangle look (kThird), a Square look (kFourth; for both
// bit 0 the costume it is made on, bit 1 the row it takes); all agreed online with the pick
constexpr uint32_t kAltPick = 0x01000000, kMirrorLook = 0x02000000, kThird = 0x04000000, kFourth = 0x08000000;

// a player's pick has just been taken (colours.cpp, after the other colours): Square gives a fighter's Square look,
// Triangle its Triangle look, any other pick neither; with the other player in that fighter, a plain costume takes the
// row a look leaves (above)
void Picked(uint32_t player);
// the select screen's end (colours.cpp), before the mirror rule: Triangle and Square looks only where a fighter has
// them, each in its side's row, a plain costume of that fighter in the row a look leaves; the side the mirror rule may
// turn, never one in a look
int Settle(uint32_t word[2]);

// the select screen's end (colours.cpp), once the agreed costumes are in the words and the mirror rule settled them:
// kMirrorLook for the player who gets a mirror look, none for anybody else
void SelectEnds(uint32_t word[2]);

}  // namespace mkda::costumes
