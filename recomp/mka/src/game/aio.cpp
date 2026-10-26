// The game's asynchronous loader, run by the main thread while a rollback records (Deception's aio.cpp, the same
// library at other addresses).
//
// The engine streams sound banks and music through an I/O worker thread (entry 0x385E10, loop 0x38D2F0 with ecx = the
// I/O manager [0x709450], the object at 0x7093B8). The manager keeps its schedulers at +4..+8, a critical section at
// +0x10, the worker's condition (event, waiters) at +0x2C, quit / running flags at +0x38 / +0x39 and the completion
// ring at +0x3C (count +0x78). Two schedulers (vtable +8 the pass, +0x10 "has work", +0x14 "busy"): the request
// scheduler 0x709460 (vtable 0x51C4C8) and the device scheduler 0x709538 (vtable 0x51C4B0); see SchedulerBusy. The
// worker waits until a scheduler has work, then runs the busy schedulers' passes (sub_0038CB20, ecx = the manager),
// which step each request through its reads (ReadFileEx 0x2A4E58: the completion routine runs as an APC in the
// worker's next alertable wait, sub_00385E50 = SleepEx(0, TRUE)) and push finished requests onto the completion
// ring; every frame the main thread takes them back (sub_0038D7C0, from the main loop through 0x19C950 -> 0x380800),
// after the deferred calls queued at 0x7094E8 (count +0x3C).
//
// When a request finishes then depends on the disc and the other thread, and both threads write the requests and
// the queues, so a rollback can neither put that state back nor leave it alone. While a rollback records
// (kernel/host_rollback.h) the worker skips its passes and the main thread runs the loader itself at a fixed point of
// every frame, the start of the completion drain: the busy schedulers' passes, then the read completions (APCs),
// until nothing is busy. Reads complete as they are made in this runtime, so a request submitted in a frame is back
// in that frame, every loader write is the main thread's, and a frame run again after a rollback loads the same way.
#include "game/aio.h"

#include <chrono>
#include <cstdlib>
#include <thread>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "game/frame.h"
#include "game/rollback.h"
#include "kernel/host_rollback.h"
#include "kernel/kernel.h"

X86_EXTERN_FUNC(__imp__sub_0038CB20);
X86_EXTERN_FUNC(__imp__sub_0038D7C0);

namespace mka::aio {
namespace {

constexpr uint32_t kManagerPtr = 0x709450;  // the I/O manager
constexpr uint32_t kRunSchedulers = 0x38CB20;
constexpr uint32_t kCompletionCount = 0x78;  // in the manager: requests waiting for the main thread
constexpr uint32_t kDeferred = 0x7094E8;     // deferred calls for the main thread: count at +0x3C
constexpr uint32_t kSchedulerBusy = 0x38CC40, kRequestSchedulerBusy = 0x38C4F0;
constexpr int kMaxPasses = 16;

uint64_t g_passes = 0, g_busy_left = 0, g_skipped = 0;
uint32_t g_shared_in = 0;  // the recording the worker's fields were made volatile for
const bool g_log = std::getenv("MKA_AIO_LOG") != nullptr;

// What the worker still writes while it idles: the manager's critical section (+0x10), its condition (+0x2C event
// and waiters) and flags (+0x38 quit, +0x39 running)
void ShareWorkerFields() {
    if (g_shared_in == host_rollback::Recordings()) return;
    g_shared_in = host_rollback::Recordings();
    if (uint32_t m = kernel::R32(kManagerPtr)) {
        rb::AddVolatile(m + 0x10, 0x1C);
        rb::AddVolatile(m + 0x2C, 8);
        rb::AddVolatile(m + 0x38, 4);
    }
}

// a scheduler's "busy" (its vtable +0x14) read without its lock. The two kinds there are: the device scheduler
// (0x709538, vtable 0x51C4B0, busy 0x38CC40): requests queued at +0x30, or the first running one (+0x24 to +0x2C)
// with work left (+0x10); the request scheduler (0x709460, vtable 0x51C4C8, pass 0x38F1F0, busy 0x38C4F0): a request
// in its list (+0x68..+0x6C) with work left, or requests waiting to be taken in (+0x60)
bool SchedulerBusy(uint32_t s) {
    uint32_t vtable = kernel::R32(s);
    uint32_t busy = vtable ? kernel::R32(vtable + 0x14) : 0;
    if (busy == kSchedulerBusy) {
        if (uint32_t q = kernel::R32(s + 0x30)) {
            uint32_t begin = kernel::R32(q + 8), end = kernel::R32(q + 0xC);
            if (begin && end > begin && (end - begin) / 0xC) return true;
        }
        for (uint32_t i = 0; i < 3; i++)
            if (uint32_t r = kernel::R32(s + 0x24 + 4 * i)) return int32_t(kernel::R32(r + 0x10)) > 0;
        return false;
    }
    if (busy == kRequestSchedulerBusy) {
        uint32_t begin = kernel::R32(s + 0x68), end = kernel::R32(s + 0x6C);
        for (uint32_t at = begin; at && at < end && at < begin + 0x400; at += 4)
            if (int32_t(kernel::R32(kernel::R32(at) + 0x10)) >= 1) return true;
        return kernel::R32(s + 0x60) != 0;
    }
    static bool told = false;
    if (!told) {
        told = true;
        LOGW("aio: scheduler %08X of an unknown kind (vtable %08X), taken as idle", s, vtable);
    }
    return false;
}

bool AnyBusy() {
    uint32_t m = kernel::R32(kManagerPtr);
    if (!m) return false;
    uint32_t begin = kernel::R32(m + 4), end = kernel::R32(m + 8);
    if (!begin || end < begin || end - begin > 0x100) return false;
    for (uint32_t at = begin; at < end; at += 4) {
        uint32_t s = kernel::R32(at);
        if (s && SchedulerBusy(s)) return true;
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
        if (g_log)
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

// MKA_AIO_LOG=1: the loader's state every 5 s (which scheduler is busy, the queues)
void LogState(uint64_t frame) {
    if (!g_log || frame % 300) return;
    uint32_t m = kernel::R32(kManagerPtr);
    if (!m) return;
    uint32_t begin = kernel::R32(m + 4), end = kernel::R32(m + 8);
    LOGI("aio: frame %llu manager %08X schedulers %u, completions %u, deferred %u", (unsigned long long)frame, m,
         (end - begin) / 4, kernel::R32(m + kCompletionCount), kernel::R32(kDeferred + 0x3C));
    for (uint32_t at = begin; at < end && at < begin + 0x40; at += 4) {
        uint32_t s = kernel::R32(at), vt = s ? kernel::R32(s) : 0;
        LOGI("aio:   scheduler %08X vtable %08X (+8 %08X +0x10 %08X +0x14 %08X) busy %d", s, vt,
             vt ? kernel::R32(vt + 8) : 0, vt ? kernel::R32(vt + 0x10) : 0, vt ? kernel::R32(vt + 0x14) : 0,
             s ? int(SchedulerBusy(s)) : -1);
    }
}

const bool g_registered = [] {
    OnFrameStart(&LogState);
    return true;
}();

}  // namespace

uint64_t Skipped() { return ++g_skipped; }

// nothing in flight: no busy scheduler, nothing waiting in the completion and deferred queues (a good moment to
// start recording)
bool Idle() {
    uint32_t m = kernel::R32(kManagerPtr);
    return !AnyBusy() && (!m || kernel::R32(m + kCompletionCount) == 0) && kernel::R32(kDeferred + 0x3C) == 0;
}

bool Settle(X86Context& ctx, std::chrono::milliseconds timeout) {
    constexpr uint32_t kPump = 0x380800;  // the main loop's: the deferred calls and the completion drain, then the
                                          // manager's callback
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

}  // namespace mka::aio

// the worker's pass over the busy schedulers: skipped on the worker while the main thread runs them
X86_FUNC(sub_0038CB20) {
    if (host_rollback::Active() && !host_rollback::Recording()) {
        if (mka::aio::Skipped() % 500 == 1)
            LOGI("aio: the worker's pass skipped (%llu times)", (unsigned long long)mka::aio::Skipped());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ctx.esp += 4;  // ret
        return;
    }
    __imp__sub_0038CB20(ctx, base);
}

// the main thread's completion drain: the loader runs first
X86_FUNC(sub_0038D7C0) {
    if (host_rollback::Recording()) mka::aio::RunLoader(ctx);
    __imp__sub_0038D7C0(ctx, base);
}
