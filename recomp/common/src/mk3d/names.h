#pragma once
// The players' names over an online fight, and over one watched: each player's account name sits inside their health
// bar, in the half beside the timer (Deception and Armageddon letter the fighter's own name in the outer half), P1's
// against the timer's left and P2's against its right, and only while the fight's HUD is up: they fade in once its bars
// have grown in from the timer, slide away with it and fade with the game's screen fades. Each game says where its bars
// are and how far a fade has gone; the launcher's start line carries the names (p1_name, p2_name), which the link hands
// over for an online or a watched match. <prefix>_TEST_NAMES=Kitana|Sindel shows names in any fight (screenshots of an
// offline one).
#include <cstdint>
#include <string>

namespace mk3d::names {

// where the names go this frame, in the frame's 640x480 pixels (y down): P1's name ends at p1_end and P2's starts at
// p2_start, beside the timer, their capitals centred on the line y (the bars' middle); a name too long for its room (P1's
// from p1_start, P2's up to p2_end) is drawn smaller to fit. shade: how bright the game draws its HUD this frame, 1 at
// full and down to 0 as the screen fades to black, which the names follow.
struct Place {
    float p1_start = 0, p1_end = 0, p2_start = 0, p2_end = 0, y = 0;
    float shade = 1;
};

struct Adapter {
    // the fight's HUD is on screen and where the names go; false otherwise. Main thread, at the top of a frame.
    bool (*place)(Place& out);
};

// the game's adapter, once at start
void Install(const Adapter& adapter);

// For a game whose health bars are quads in its HUD's own space (Deception, Armageddon): y up from the bottom of 480
// lines, as wide as the word at hud_width says, each bar's back an object (its id 4 bytes after the pointer, its quad's
// four corners at +0x34, 0x14 apart, moved by the object's offset at +0x14 / +0x18). The names take each bar's inner
// half, from a little past its middle to clear either side of the HUD's centre, the timer's room. False while either
// bar's object is gone.
struct BarQuads {
    uint32_t back[2];  // P1's, P2's: where the object pointer is kept
    uint32_t hud_width;
    float clear;
};
bool PlaceInBars(const BarQuads& bars, Place& out);

// The games' screen fades: a box sprite blown up over the whole screen (at -50, -50, scaled 50 x 40) whose alpha
// darkens what is drawn under it, the HUD too. lists: where the object lists start that hold the game's sprites (0: none;
// a node holds the object, the next node and at +0x10 the object's id when it was listed, a stale node is passed over);
// scale: the box's scale in the object; corners: where the pointer to its four corners is (0x14 apart, alpha the byte
// in each). The shade under the most opaque box: 1 without any, 0 under a black one.
struct FadeBoxes {
    uint32_t lists[2];
    uint32_t scale, corners, alpha;
};
float ShadeUnder(const FadeBoxes& boxes);

// a match's two names, as the launcher's start line gives them; empty: none (an offline match, the match over)
void SetPlayers(const std::string& p1, const std::string& p2);

// app::GameInfo::window (the font, before the renderer builds the atlas) and ::ui
void Window(void* hwnd);
void Ui(double fps);

}  // namespace mk3d::names
