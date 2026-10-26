// The game's asynchronous loader, run by the main thread while a rollback records.
//
// The engine streams sound banks and music (the announcer, the stage music) through an I/O worker thread (entry
// 0x28D230, loop 0x291500). The main thread submits a request to a scheduler (0x2918D0: onto the scheduler's queue,
// and the I/O manager, the object at [0x5F0E90], is told there is work); the worker runs every busy scheduler (0x2911A0, scheduler
// vtable +8 = 0x2925C0), which steps each request (request vtable +0x1C) through its reads (ReadFileEx: the
// completion routine runs as an APC in the worker's next alertable wait), and pushes finished requests onto the
// completion queue 0x5F0E34; every frame the main thread takes them back (0x2917B0, from the main loop through
// 0x28A750) and runs their handlers.
//
// When a request finishes then depends on the disc and the other thread, and both threads write the requests and
// the queues, so a rollback can neither put that state back nor leave it alone. While a rollback records
// (kernel/host_rollback.h) the worker skips its passes and the main thread runs the loader itself at a fixed point
// of every frame, the start of the completion drain: the busy schedulers' passes, then the read completions (APCs),
// until nothing is busy. Reads complete as they are made in this runtime, so a request submitted in a frame is
// back in that frame, every loader write is the main thread's, and a frame run again after a rollback loads the
// same way.
#include <chrono>
#include <cstdlib>
#include <thread>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "game/frame.h"
#include "game/rollback.h"
#include "kernel/host_rollback.h"
#include "kernel/kernel.h"

X86_EXTERN_FUNC(__imp__sub_002911A0);
X86_EXTERN_FUNC(__imp__sub_002917B0);

namespace mkd::aio {
namespace {

constexpr uint32_t kManagerPtr = 0x5F0E90;   // the I/O manager: +4/+8 its schedulers, +0x3A "there is work"
constexpr uint32_t kRunSchedulers = 0x2911A0;
constexpr uint32_t kCompletions = 0x5F0E34;  // the completion queue: count at +0x3C
constexpr uint32_t kDeferred = 0x5F0F28;     // deferred calls for the main thread: count at +0x3C
constexpr uint32_t kSchedulerBusy = 0x24;
constexpr int kMaxPasses = 16;

uint64_t g_passes = 0, g_busy_left = 0, g_skipped = 0;
uint32_t g_shared_in = 0;  // the recording the worker's fields were made volatile for

// What the worker still writes while it idles: the critical section its loop takes (0x5F0E18) and the manager's
// condition (+0x10 event and waiters) and flags (+0x38 quit, +0x39 running, +0x3A work)
void ShareWorkerFields() {
    if (g_shared_in == host_rollback::Recordings()) return;
    g_shared_in = host_rollback::Recordings();
    rb::AddVolatile(0x5F0E18, 0x1C);
    if (uint32_t m = kernel::R32(kManagerPtr)) {
        rb::AddVolatile(m + 0x10, 8);
        rb::AddVolatile(m + 0x38, 4);
    }
}
const bool g_log = std::getenv("MKD_AIO_LOG") != nullptr;

bool AnyBusy() {
    uint32_t m = kernel::R32(kManagerPtr);
    if (!m) return false;
    uint32_t begin = kernel::R32(m + 4), end = kernel::R32(m + 8);
    if (!begin || end < begin || end - begin > 0x100) return false;
    for (uint32_t at = begin; at < end; at += 4) {
        uint32_t s = kernel::R32(at);
        if (s && kernel::G<uint8_t>(s + kSchedulerBusy)) return true;
    }
    return false;
}

// the worker's passes, on this thread: schedulers, then the read completions, while any is busy
void RunLoader(X86Context& ctx) {
    ShareWorkerFields();
    kernel::Thread* self = kernel::Current();
    for (int pass = 0; pass < kMaxPasses; pass++) {
        bool busy = AnyBusy();
        bool apcs = self && kernel::RunApcs(*self);
        if (!busy && !apcs) return;
        if (g_log && (busy || apcs))
            LOGI("aio: frame %llu pass %d: %s%s", (unsigned long long)Frame(), pass, busy ? "schedulers busy " : "",
                 apcs ? "completions ran" : "");
        if (busy) {
            uint32_t ecx = ctx.ecx;
            ctx.ecx = kernel::R32(kManagerPtr);
            kernel::CallGuest(ctx, kRunSchedulers, {});
            ctx.ecx = ecx;
            g_passes++;
        }
    }
    if (AnyBusy() && g_busy_left++ < 8)
        LOGW("aio: frame %llu: the loader is still busy after %d passes", (unsigned long long)Frame(), kMaxPasses);
}

}  // namespace

uint64_t Skipped() { return ++g_skipped; }

// nothing in flight: no busy scheduler, nothing waiting in the completion and deferred queues (a good moment to
// start recording)
bool Idle() {
    return !AnyBusy() && kernel::R32(kCompletions + 0x3C) == 0 && kernel::R32(kDeferred + 0x3C) == 0;
}

bool Settle(X86Context& ctx, std::chrono::milliseconds timeout) {
    constexpr uint32_t kPump = 0x28A750;  // the main loop's: the completion drain, then the manager's callback
    auto t0 = std::chrono::steady_clock::now();
    while (!Idle()) {
        if (std::chrono::steady_clock::now() - t0 > timeout) return false;
        if (AnyBusy()) {  // the worker is at it
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        kernel::CallGuest(ctx, kPump, {});
    }
    return true;
}

}  // namespace mkd::aio

// the worker's pass over the busy schedulers: skipped on the worker while the main thread runs them
X86_FUNC(sub_002911A0) {
    if (host_rollback::Active() && !host_rollback::Recording()) {
        if (mkd::aio::Skipped() % 500 == 1) LOGI("aio: the worker's pass skipped (%llu times)", (unsigned long long)mkd::aio::Skipped());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ctx.esp += 4;  // ret
        return;
    }
    __imp__sub_002911A0(ctx, base);
}

// the main thread's completion drain: the loader runs first
X86_FUNC(sub_002917B0) {
    if (host_rollback::Recording()) mkd::aio::RunLoader(ctx);
    __imp__sub_002917B0(ctx, base);
}
