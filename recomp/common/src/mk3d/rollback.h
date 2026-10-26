#pragma once
// The state machinery rollback netplay needs: put the whole simulation back to the top of an earlier frame (shared
// by the games; the engine's tasks come from the game's task fibers, mk3d/tasks.h).
//
// Guest memory is tracked copy-on-write: at the top of each frame every tracked page is write-protected; the first
// write to a page in a frame copies its old contents into that frame's undo log. Rolling back to frame r plays the
// undo logs of the frames since r backwards. Pages other threads write (CRI streaming, the I/O thread, the sound
// mixer, the vertical blank thread) are "foreign": never logged or restored, so those threads are not pulled back
// in time under their own feet. Tracked: committed user pages, the 64 MB of physical memory (both of its views) and
// the main thread's guest stack. The host-side state of the simulation is saved per frame too: the engine's task
// fibers (task_fibers.h) and the main thread's guest registers.
//
// All calls are made on the main thread at the top of the main loop (frame.h), where no task runs.
#include <cstdint>
#include <vector>

struct X86Context;

namespace mk3d::rb {

// start tracking from the top of this frame / stop and forget everything
bool Arm(X86Context& ctx, uint32_t frame);
void Disarm();
bool Armed();
// true while Restore copies pages back (diagnostics that watch guest writes skip those)
bool Restoring();

// at the top of each frame while armed, in this order:
//   CloseFrame()        the frame that just ran is complete: its undo log is closed
//   [Restore(r, ctx)]   optionally go back to the top of an earlier frame r (its log and snapshot must be kept)
//   OpenFrame(f, ctx)   the frame about to run: snapshot the host state, open its undo log
void CloseFrame();
bool Restore(uint32_t frame, X86Context& ctx);
void OpenFrame(uint32_t frame, X86Context& ctx);

// the oldest frame that can still be restored, and how many frames are kept
uint32_t OldestFrame();
constexpr int kKeepFrames = 16;

// hash of what the last closed frame wrote (the pages it dirtied, their contents now), for sync tests
uint64_t LastFrameHash();
uint32_t LastFramePages();

// bytes another thread owns, on pages that otherwise hold simulation state (a queue a loader thread fills): a restore
// leaves them as they are and hashes skip them; while armed
void AddVolatile(uint32_t va, uint32_t size);
// the same for bytes known in advance (a graphics device's vertical blank fields): named once, marked at every Arm
void AddFixedVolatile(uint32_t va, uint32_t size);

// keep the last closed frame's pages and their contents (sync test diagnostics)
void SetDetail(bool on);
// only pages in [lo, hi) count in frame hashes and details (default: all)
void SetHashRange(uint32_t lo, uint32_t hi);
// the pages the last closed frame wrote: their contents at its end, and at its top if asked; raw (MaskVolatile)
void LastFrameDetail(std::vector<uint32_t>& pages, std::vector<uint8_t>& data, std::vector<uint8_t>* top = nullptr);
// zero a page copy's volatile bytes (as they are known now)
void MaskVolatile(uint32_t page, uint8_t* data);

struct Stats {
    uint64_t faults = 0, foreign_pages = 0, tracked_pages = 0;
    uint64_t logged_bytes = 0;
    double close_ms = 0, restore_ms = 0;
};
Stats GetStats();

}  // namespace mk3d::rb
