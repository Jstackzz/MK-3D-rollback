#pragma once
// The game's asynchronous loader under rollback; see aio.cpp.

#include <chrono>

struct X86Context;

namespace mka::aio {

// nothing in flight (no busy scheduler, nothing waiting for the main thread): a good moment to start recording
bool Idle();

// on the main thread at the top of a frame, before recording starts: let the I/O worker finish what it has, take
// the finished requests back (the main loop's drain, 0x380800), until nothing is in flight; false on timeout
bool Settle(X86Context& ctx, std::chrono::milliseconds timeout);

}  // namespace mka::aio
