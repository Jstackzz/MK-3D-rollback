#pragma once
// Deception's frame loop as hooks: the top of every main loop iteration (0xE0007) and the moment the game has read
// its pads (0x1C52F0). The functions are the shared core's (mk3d/game.h), implemented in frame.cpp.
#include <cstdint>

#include "mk3d/game.h"

namespace mkd {

using mk3d::Frame;
using mk3d::FrameListener;
using mk3d::MainContext;
using mk3d::OnFrameStart;
using mk3d::OnPadsRead;

// the per-port input blocks the pad update writes, 0x1C bytes each
constexpr uint32_t kPortBlock = 0x62109C, kPortStride = 0x1C;
constexpr uint32_t kPortFlags = 0x0;      // bit 1: the port's controller is open
constexpr uint32_t kPortPrevious = 0xC;   // logical buttons last frame
constexpr uint32_t kPortCurrent = 0x10;   // logical buttons now (after the player's button configuration)
constexpr uint32_t kPortPressed = 0x14;   // new presses: (current ^ previous) & current
constexpr uint32_t kPortSticks = 0x18;    // both sticks, a byte per axis

}  // namespace mkd
