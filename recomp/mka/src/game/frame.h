#pragma once
// Armageddon's frame loop as hooks: the top of every main loop iteration (0x19C8AC) and the moment the game has read
// its pads (0x26A9C0). The functions are the shared core's (mk3d/game.h), implemented in frame.cpp.
#include <cstdint>

#include "mk3d/game.h"

namespace mka {

using mk3d::Frame;
using mk3d::FrameListener;
using mk3d::MainContext;
using mk3d::OnFrameStart;
using mk3d::OnPadsRead;

// the main loop's own frame counters, counted at its top: always, and while not paused (bit 13 of kFlags and
// 0x7350B0 clear); the engine's timers count on them
constexpr uint32_t kFrameCounter = 0x709E6C, kGameCounter = 0x829304;
constexpr uint32_t kFlags = 0x831A40;

// the per-port input blocks the pad update writes, 0x20 bytes each (ports 0-3; the engine also knows a port 4)
constexpr uint32_t kPortBlock = 0x831C0C, kPortStride = 0x20;
constexpr uint32_t kPortFlags = 0x0;      // bit 1: the port's controller is open
constexpr uint32_t kPortButtonMap = 0x4;  // the port's button table: the logical bits of each pad button, 0xC apart
constexpr uint32_t kPortRecord = 0x8;     // the player record the port drives (0 none: a press takes a free one)
constexpr uint32_t kPortPrevious = 0xC;   // logical buttons last frame
constexpr uint32_t kPortCurrent = 0x10;   // logical buttons now (after the player's button configuration)
constexpr uint32_t kPortPressed = 0x14;   // new presses: (current ^ previous) & current
constexpr uint32_t kPortSticks = 0x18;    // both sticks, a byte per axis (centred 0x807F807F)

}  // namespace mka
