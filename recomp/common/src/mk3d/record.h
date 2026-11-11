#pragma once
// Record and playback for the practice dummy (player 2), as modern fighting games have it in their training modes:
// F9 records what the player's controller does while it drives the dummy (up to 30 seconds into one of 8 slots),
// F10 plays the slot back for it, once or again and again (F12: once, repeat, random slot), F11 picks the slot.
// Directions are kept relative to the way the dummy faces, so a sequence recorded on one side plays right from the
// other. The slots are saved beside the game, per dummy fighter, and come back the next time it is the dummy.
//
// Only in a practice fight on this PC alone (framedata.h: never in a game the launcher drives or with netplay
// configured), so the games of an online match run exactly as they would without it. The game's own pad update runs
// as always; right after it, each frame, the game's side moves the player's logical buttons over to the dummy
// (recording) or writes the slot's buttons for it (playback), and makes the dummy a player the game reads controls for
// (each game's Adapter says how, and puts it back as it was). While the pause menu is up nothing is driven, so its
// controls work and the dummy is the game's own again before a state switch (PLAYER SELECT, MAIN MENU).
#include <cstdint>

namespace mk3d::record {

// the dummy's controls for one game frame: the logical buttons as the game's input block holds them, and the sticks
// word the game keeps beside them
struct Input {
    uint32_t buttons = 0;
    uint32_t sticks = 0;
};

enum class Drive { Off, Record, Play };

struct Adapter {
    // the game's frame counter, which stands still while the game is paused
    uint32_t (*time)();
    // the pause menu is up
    bool (*paused)();
    // the dummy faces left: left on the pad walks it towards player 1 (the game's own rule for its walk, by the side
    // player 1 is on); the last answer when there is no fight to read
    bool (*dummy_faces_left)();
    // the dummy stands turned away from player 1 (one jumped over it and it has had no input since): the game turns it
    // round at its first input, a direction without a step, so that input is no move
    bool (*dummy_turned_away)();
    // the dummy's fighting style (the game's own number, which its style button cycles), -1 when not known; and the
    // logical bits of that button in the dummy's controls
    int (*dummy_style)();
    uint32_t (*style_bits)();
    // the dummy's input for the frame about to run, right after the game's pad update (main thread):
    //   Off     the game's own practice: the dummy on no controls (undo whatever Record or Play set up; cheap when
    //           nothing is set up)
    //   Record  player 1's controller drives the dummy and player 1 keeps only its pause buttons; out = what the
    //           dummy got, as it will be played back
    //   Play    the dummy gets `in`; player 1 keeps its own controller
    void (*drive)(Drive mode, const Input& in, Input& out);
    // the logical bits of left and right in the dummy's controls (mirrored when the dummy faces the other way), and
    // the sticks word with nothing pushed
    uint32_t left_bits, right_bits, neutral_sticks;
};

// the game's adapter, once at start (before the first frame)
void Install(const Adapter& adapter);

// the dummy's controls come from a recording or the player's controller this frame: the practice dummy's own block
// (framedata.h, F8) stands aside
bool DrivesDummy();

// app::GameInfo::ui and ::message (F9 to F12); the overlay uses the frame data's font
void Ui(double fps);
bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);

}  // namespace mk3d::record
