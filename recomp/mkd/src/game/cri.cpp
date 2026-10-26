// CRI ADX/Sofdec middleware (movies and streamed sound) on a multi-core PC.
//
// CRI's server manager (SVM) lock assumes the Xbox's single CPU: SVM_Lock (0x2427D0) calls a platform callback
// (0x2405E0) that raises the calling thread to priority 15 so nothing can preempt it, keeping one global nesting
// count (0x4A0A80) and one saved priority (0x63FA64); SVM_Unlock (0x242A90) calls 0x240630, which restores that
// priority when the count reaches zero. Teardown code force-unlocks "while my priority is 15" (0x24BB20).
// On several cores two threads interleave inside those callbacks, one is left at priority 15 for good, and the
// force-unlock loop never ends (the main thread hung there after the first movie); movie playback also raced.
// Here the lock becomes real: a host recursive mutex is taken before the original lock code and released after
// the original unlock code, so CRI's own counters and priorities behave exactly as on the console.
//
// Two wrappers over that lock keep their own nesting count in a global and only call the lock when the count is 0:
// the stream joint ring buffers' (SJ) critical section, lock 0x2483C0 / unlock 0x2483E0 with count 0x4A12A0, and the
// server's, lock 0x248470 / unlock 0x248490 with count 0x4A12AC. The count is read before the lock is taken, so on
// several cores a second thread sees "already held", skips the lock and runs inside the section with the first; two
// threads then moved a ring buffer's chunks at once, the buffer's consistency check failed and called its default
// error handler 0x240ED0 (a jump to itself) with the lock held: every other CRI thread and the movie player on the
// main thread waited on it for good (an intermittent black screen when a movie started as START was pressed). The
// wrappers now take the same mutex before they look at their count.
//
// The ADX server pass 0x243350 (run by the server thread 0x240BE0 and by 0x243572) checks its busy flag 0x4A0D68
// under the lock, unlocks, and only then sets the flag: two threads could both pass the check and decode the same
// streams at once, which broke the ring buffers (the error handler above). On one CPU nothing ran in between. The
// whole pass now holds the mutex; decoding does not wait on other threads. Sofdec's per-handle pass
// mwPlyExecSvrHndl 0x224BD0 is the same story (reached from the handle loop 0x224C50 and through a pointer at
// 0x45DFA8; its busy flags 0x2249B0 / 0x2249D0 and the request flag at handle+0x44 are tested and set without a
// lock), so it holds the mutex too.
//
// The ADX idle thread (0x2406F0) spins on its stop flag (0x4A0AA0); on the Xbox it is the lowest priority thread
// and only runs when nothing else can. Here it would burn a core, so it sleeps between looks.
#include <cstdint>
#include <mutex>
#include <thread>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

X86_EXTERN_FUNC(__imp__sub_002427D0);
X86_EXTERN_FUNC(__imp__sub_00242A90);
X86_EXTERN_FUNC(__imp__sub_002483C0);
X86_EXTERN_FUNC(__imp__sub_002483E0);
X86_EXTERN_FUNC(__imp__sub_00248470);
X86_EXTERN_FUNC(__imp__sub_00248490);
X86_EXTERN_FUNC(__imp__sub_00243350);
X86_EXTERN_FUNC(__imp__sub_00224BD0);

namespace {

constexpr uint32_t kIdleStop = 0x4A0AA0, kIdleStopped = 0x4A0AA4;
constexpr uint32_t kExitThread = 0x1ED067;  // XAPI ExitThread

std::recursive_mutex g_svm;
std::atomic<DWORD> g_owner{0};
thread_local int t_depth = 0;  // how many times this thread holds g_svm

void Lock() {
    g_svm.lock();
    t_depth++;
    g_owner = GetCurrentThreadId();
}

bool Unlock() {
    if (t_depth == 0) {
        LOG_FIRST(4, "cri: SVM unlock on a thread that does not hold the lock");
        return false;
    }
    if (--t_depth == 0) g_owner = 0;
    g_svm.unlock();
    return true;
}

}  // namespace

// SVM_Lock(type)
X86_FUNC(sub_002427D0) {
    Lock();
    __imp__sub_002427D0(ctx, base);
}

// SVM_Unlock(type): the original ends in a tail jump to the platform callback, which returns here
X86_FUNC(sub_00242A90) {
    __imp__sub_00242A90(ctx, base);
    Unlock();
}

// SJ critical section: lock, then the original (count, and the SVM lock on the first entry)
X86_FUNC(sub_002483C0) {
    Lock();
    __imp__sub_002483C0(ctx, base);
}
X86_FUNC(sub_002483E0) {
    __imp__sub_002483E0(ctx, base);
    Unlock();
}
// the server's critical section, the same shape
X86_FUNC(sub_00248470) {
    Lock();
    __imp__sub_00248470(ctx, base);
}
X86_FUNC(sub_00248490) {
    __imp__sub_00248490(ctx, base);
    Unlock();
}

// the ADX server pass: its busy check and set as one step
X86_FUNC(sub_00243350) {
    Lock();
    __imp__sub_00243350(ctx, base);
    Unlock();
}

// Sofdec mwPlyExecSvrHndl(handle): one decoding pass per handle at a time
X86_FUNC(sub_00224BD0) {
    Lock();
    __imp__sub_00224BD0(ctx, base);
    Unlock();
}

// "is the SVM locked by me": the console checks for priority 15; that only means "I hold it" when one thread at a
// time can be in there, which the mutex above now guarantees, so ask the mutex directly
X86_FUNC(sub_0024BB10) {
    (void)base;
    ctx.eax = (t_depth > 0) ? 1u : 0u;
    ctx.esp += 4;
}

// the ADX idle thread
X86_FUNC(sub_002406F0) {
    (void)base;
    while (kernel::R32(kIdleStop) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    kernel::W32(kIdleStopped, 1);
    kernel::CallGuest(ctx, kExitThread, {0xF0000001u});
}
