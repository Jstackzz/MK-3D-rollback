#pragma once
// The engine's random numbers in a fight in sync (netplay.h); see rng.cpp.
#include <cstdint>

namespace mkd::rng {

// the top of a frame of a fight in sync: from here on draws are keyed by (seed, frame, call site, call count)
void BeginFrame(uint32_t seed, uint32_t frame);
// the fight in sync is over: the engine's own generator again
void End();
// the C runtime's rand in a fight in sync, on the main thread: a draw keyed as the engine's are, in a stream of its
// own (`site`: the caller's return address); false otherwise (rand's own state then)
bool CrtDraw(uint32_t site, uint32_t& out);

}  // namespace mkd::rng
