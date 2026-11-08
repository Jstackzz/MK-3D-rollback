#pragma once
// Each player's rank badge in an online fight, and in one watched: beside the timer, standing on their health bar
// (P1's on the timer's left, P2's on its right). The launcher's start line names each side's ranked title (p1_rank,
// p2_rank: lower case, dashes for spaces, "chosen-one"; "-" none), which the link hands over for an online or a
// watched match. Its picture (ranks/<title>.png: the package's ranks folder, two up from the game, or one beside the
// game) goes into the game's own frame just before the HUD draws its bars, so the bars, the timer and the rest of the
// HUD go over it where they meet, and it slides off and fades with the HUD (runtime patch 0043, d3d::DrawPicture). Each
// game says where its bars and timer are; the players' names above the bars (names.h) keep clear of the badges.
// <prefix>_TEST_RANKS=god|beginner shows badges in any fight (screenshots of an offline one);
// <prefix>_TEST_RANK_SIZE=<lines> tries another height.
#include <cstdint>
#include <string>

namespace mk3d::ranks {

// where the badges go this frame, in the HUD's own units across (the frame is hud_width of them wide: 640, or 720 in
// Deception's and Armageddon's 16:9 frame) and its 480 lines down: P1's ends at inner[0] (its right edge, on the
// timer's side), P2's starts at inner[1] (its left edge); both stand on the line `bottom` (the bars' top edge) and fit
// in a box `height` lines tall and 2.4 times as wide as it looks on screen (a badge wider than that is shorter)
struct Place {
    float hud_width = 640;
    float inner[2] = {};
    float bottom = 0;
    float height = 0;
    // the textures (D3D texture headers, as a draw's textures[0]) before whose first draw this frame the badges go:
    // the HUD's first draws at the bars; 0 unused
    uint32_t anchor[4] = {};
};

struct Adapter {
    // where the badges go this frame; false while the fight's HUD is not up. Main thread, at the top of a frame.
    bool (*place)(Place& out);
};

// the game's adapter, once at start
void Install(const Adapter& adapter);

// a match's two ranks, as the start line names them; empty or "-": none (an offline match, the match over)
void SetPlayers(const std::string& p1, const std::string& p2);

// the room the players' names have above the bars (names.cpp), in frame pixels: P1's ends before P1's badge, P2's
// starts after P2's; unchanged for a side without one. Any thread.
void KeepClear(float& p1_end, float& p2_start);

}  // namespace mk3d::ranks
