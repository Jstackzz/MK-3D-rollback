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

// On the character select screen LT is the costumes' other colours as a fighter is picked (colours.cpp), which the
// screen itself would take (after LT its cursor took no moves): it is kept from the screen, and this says whether the
// port held it at its last read there
bool SelectLT(int port);

// holds buttons on a port for a number of main loop frames from now
void InjectPad(int port, uint16_t buttons, uint8_t analog, uint64_t frames);

}  // namespace mka
