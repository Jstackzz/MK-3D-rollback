#pragma once
// Puzzle and Chess Kombat online (agreed_width.cpp): the viewport width both games of a match use for their logic.
#include <cstdint>

namespace mkd::width {

uint32_t ViewportWidth();                // this PC's, told to the other game at the meeting point
void AgreeWidth(uint32_t a, uint32_t b);  // both players' are here

}  // namespace mkd::width
