#pragma once
// Deadly Alliance's frame loop as hooks: the top of every main loop iteration and the moment the game has read its pads.
// The functions are the shared core's (mk3d/game.h), implemented in frame.cpp.
#include <cstdint>

#include "mk3d/game.h"

namespace mkda {

// main loop iterations since boot; the main thread's guest context while it runs a frame hook (null elsewhere);
// OnFrameStart: at the top of every main loop iteration (0xCAD95, right after the counters below), before anything
// else of that frame ran; OnPadsRead: right after the main loop's pad update (0x143030) wrote the per-player input
// blocks. Both on the main thread.
using mk3d::Frame;
using mk3d::FrameListener;
using mk3d::MainContext;
using mk3d::OnFrameStart;
using mk3d::OnPadsRead;

// the main loop's counters: +1 every iteration, and +1 while the game is not paused (kPaused, set by the pause menu)
constexpr uint32_t kFrameCounter = 0x42A888, kGameCounter = 0x42A8A0, kPaused = 0x321424;

// The pad update keeps logical buttons per player, not per port. The ports P1 and P2 play on (-1 none; a pad that
// presses a button while it is not assigned joins through sub_00076B20):
constexpr uint32_t kPlayerPort[2] = {0x266B24, 0x266B28};
// each player's block (P1, P2, then two spare slots; pads on no player write a scratch block at 0x2EFBFC): logical
// buttons last frame, now, and new presses ((now ^ last) & now), one bit per entry of the button map 0x281438
// (A 0x40, B 0x20, X 0x80, Y 0x10, BLACK 0x8, WHITE 0x1, LT 0x4, RT 0x2, BACK 0x100, LS 0x200, RS 0x400,
// START 0x800, UP 0x1000, RIGHT 0x2000, DOWN 0x4000, LEFT 0x8000); the game reads a player's actions through its
// button configuration (P1 0x42C640, P2 0x42C560, 12 bytes per action, the mask first)
constexpr uint32_t kPlayerBlock = 0x2CE770, kPlayerStride = 0xC;
constexpr uint32_t kPlayerPrevious = 0x0, kPlayerCurrent = 0x4, kPlayerPressed = 0x8;
// both sticks per player, a byte per axis (0x80 / 0x7F centred): kPlayerSticks + player * 4
constexpr uint32_t kPlayerSticks = 0x2CE7A0;
// the block pads on no player share (current, previous, sticks, new presses: 0x2EFC08, 0x2EFC04, 0x2EFC00, 0x2EFBFC)
constexpr uint32_t kScratchCurrent = 0x2EFC08, kScratchSticks = 0x2EFC00;

}  // namespace mkda
