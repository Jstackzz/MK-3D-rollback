#pragma once
#include <cstdint>
#include <string>

#include "core/input.h"

namespace mka {

// GameInfo::pad: every controller read the game makes passes through here
void PadHook(int port, input::Pad& pad);

// a later stage (netplay) that may replace what the game reads; called last
using PadFilter = void (*)(int port, input::Pad& pad);
void SetPadFilter(PadFilter f);

// "A+B+LEFT" style names (A B X Y BLACK WHITE LT RT UP DOWN LEFT RIGHT START BACK LS RS) to digital buttons and the
// analog buttons pressed fully (bits of Pad::analog); false on an unknown name
bool ParseButtons(const std::string& names, uint16_t& buttons, uint8_t& analog);

// holds buttons on a port for a number of main loop frames from now
void InjectPad(int port, uint16_t buttons, uint8_t analog, uint64_t frames);

}  // namespace mka
