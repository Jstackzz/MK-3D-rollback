#pragma once
// The game's file manager under rollback; see aio.cpp.
#include <chrono>

struct X86Context;

namespace mkda::aio {

// nothing in flight: no file command queued or running, no read completion waiting for the main thread
bool Idle();

// on the main thread at the top of a frame, before recording starts: run the file manager until nothing is in
// flight; false on timeout
bool Settle(X86Context& ctx, std::chrono::milliseconds timeout);

}  // namespace mkda::aio
