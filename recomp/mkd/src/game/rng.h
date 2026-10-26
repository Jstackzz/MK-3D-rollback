#pragma once
// The engine's random numbers in a fight in sync (netplay.h); see rng.cpp.
#include <cstdint>

namespace mkd::rng {

// the top of a frame of a fight in sync: from here on draws are keyed by (seed, frame, call site, call count)
void BeginFrame(uint32_t seed, uint32_t frame);
// the fight in sync is over: the engine's own generator again
void End();

}  // namespace mkd::rng
