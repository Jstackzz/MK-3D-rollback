// The game's file manager, finished within the frame while a rollback records.
//
// Deadly Alliance reads its files (the arena and fighters while a fight loads, the sound library's music and speech
// streams during it) through one file manager on the main thread, with no I/O thread: file commands wait on a queue
// ([0x31CCCC]: head, tail, count at +8; a command's type at +0x10, its state at +0x14: 0 queued, 1 running, 2 done, 3
// called back), and the pump sub_001665A0 first lets the read completions run (SleepEx(0, TRUE), sub_0015BAE7), then
// starts the head command: an open (sub_0016FAB4, CreateFile overlapped and unbuffered) or a read (sub_0016FD07,
// ReadFileEx in 32 KB pieces through a bounce buffer, sub_0016FF07). The completion routine sub_001702CA runs as an
// APC: it starts the next piece or ends the command and calls its callback. The main loop pumps twice a frame (the
// sound library's tick sub_0016B6DB at 0xCAE1C, sub_00022F90 at 0xCAE21), and a command queued with priority pumps
// at once.
//
// This runtime completes a read before NtReadFile returns and queues the APC for the main thread's next alertable
// wait, which the next pump makes, maybe in the next frame. The queued APC is host state a rollback does not put
// back: going back across it loses the completion (the command waits for ever) or runs it twice. While a rollback
// records (kernel/host_rollback.h), every frame therefore ends with the file manager drained (after the main loop's
// last pump): the completions run and the pump starts the next command until nothing is queued or completing, so a
// command started in a frame is back in that frame and no APC crosses a frame's top.
#include "game/aio.h"

#include <chrono>
#include <cstdlib>
#include <thread>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "game/frame.h"
#include "kernel/host_rollback.h"
#include "kernel/kernel.h"

X86_EXTERN_FUNC(__imp__sub_00022F90);

namespace mkda::aio {
namespace {

constexpr uint32_t kQueue = 0x31CCCC;  // the file command queue: its head command first
constexpr uint32_t kPump = 0x1665A0;
constexpr int kMaxPasses = 4096;

const bool g_log = std::getenv("MKDA_AIO_LOG") != nullptr;
uint64_t g_drains = 0;

uint32_t Head() {
    uint32_t q = kernel::R32(kQueue);
    return q ? kernel::R32(q) : 0;
}

bool ApcsWaiting() {
    kernel::Thread* self = kernel::Current();
    if (!self) return false;
    std::lock_guard<std::mutex> lock(self->apc_lock);
    return !self->apcs.empty();
}

// the completions, then the pump, until nothing is queued or completing; false when it did not get there
bool Drain(X86Context& ctx) {
    kernel::Thread* self = kernel::Current();
    for (int pass = 0; pass < kMaxPasses; pass++) {
        bool apcs = self && kernel::RunApcs(*self);
        uint32_t head = Head();
        if (!apcs && !head) return true;
        if (g_log && g_drains < 200)
            LOGI("aio: frame %llu pass %d: %s%s", (unsigned long long)Frame(), pass, apcs ? "completions ran " : "",
                 head ? "a command waits" : "");
        if (head) kernel::CallGuest(ctx, kPump, {});
    }
    return false;
}

}  // namespace

bool Idle() { return !Head() && !ApcsWaiting(); }

bool Settle(X86Context& ctx, std::chrono::milliseconds timeout) {
    auto t0 = std::chrono::steady_clock::now();
    while (!Idle()) {
        if (std::chrono::steady_clock::now() - t0 > timeout) return false;
        if (!Drain(ctx)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

}  // namespace mkda::aio

// the main loop's last pump of the frame (0xCAE21): while recording, the file manager is drained after it
X86_FUNC(sub_00022F90) {
    __imp__sub_00022F90(ctx, base);
    if (!host_rollback::Recording()) return;
    if (!mkda::aio::Drain(ctx) && mkda::aio::g_drains < 8)
        LOGW("aio: frame %llu: the file manager is still busy after %d passes", (unsigned long long)mkda::Frame(),
             mkda::aio::kMaxPasses);
    mkda::aio::g_drains++;
}
