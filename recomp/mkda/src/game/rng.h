#pragma once
// The engine's random numbers in a match in sync (mk3d/netplay.h); see rng.cpp.
#include <cstdint>

namespace mkda::rng {

// the top of a frame of a match in sync: from here on draws are keyed by (seed, frame, call site, call count)
void BeginFrame(uint32_t seed, uint32_t frame);
// the match in sync is over: the engine's own generator again
void End();

}  // namespace mkda::rng
