#pragma once
// The deterministic guest clock for netplay and sync tests; see gclock.cpp.
#include <cstdint>

namespace mk3d::gclock {

// main thread: from now on the main thread's clock is base_ns (0: the real time now) + frames since `frame` / 60 s
void Start(uint32_t frame, uint64_t base_ns = 0);
// at the top of every frame, also after a rollback: the frame about to run
void SetFrame(uint32_t frame);
void Stop();
bool On();

}  // namespace mk3d::gclock
