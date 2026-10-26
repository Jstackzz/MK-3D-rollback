// The engine's heap blocks come back zeroed on the main thread; the XAPI heap below keeps its free pages committed.
//
// Every allocation goes through sub_0029D060(heap, size, alignment, name, ...) (Deception's sub_001DD3B0, the same
// code), and a new block holds whatever the heap last kept there. What the engine leaves uninitialised in a block
// (padding, fields set later or never) then depends on everything this game did before, which differs between two
// games about to play the same match; netplay's state checks hash such blocks, and a stale value read by mistake
// would part the two games. Zeroed, a new block is the same in both. Other threads (sound, streaming) are left alone.
//
// Below the engine's heap, the XAPI heap (RtlFreeHeap 0x2A93AE and its slow path 0x2A95A2, the heap at [esp+4])
// decommits the whole pages inside a large free block (RtlpDeCommitFreeBlock 0x2A7CEB) once the block and the heap's
// free total pass the heap's thresholds (+0x28 DeCommitFreeBlockThreshold, +0x2C DeCommitTotalFreeThreshold, free
// total at +0x30). A rollback cannot put a decommitted page back (the shared core skips pages that are no longer
// committed): run again after a rollback across such a free, a frame would find the still allocated block's pages
// gone. Motor Kombat's races free blocks like that (pages committed while the race loaded, decommitted 20 s into the
// race: the sync test faulted on them). The thresholds are raised to the maximum, so a freed block stays committed on
// the heap's free lists; the runtime does not count commits against the console's 64 MB.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstring>

#include "cpu/x86_ops.h"
#include "game/frame.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

X86_EXTERN_FUNC(__imp__sub_0029D060);
X86_EXTERN_FUNC(__imp__sub_002A93AE);
X86_EXTERN_FUNC(__imp__sub_002A95A2);

namespace mka::heap {
namespace {

std::atomic<DWORD> g_main_thread{0};

void FrameStart(uint64_t) {
    if (!g_main_thread.load(std::memory_order_relaxed)) g_main_thread = GetCurrentThreadId();
}

const bool g_registered = [] {
    OnFrameStart(&FrameStart);
    return true;
}();

}  // namespace

bool OnMainThread() { return GetCurrentThreadId() == g_main_thread.load(std::memory_order_relaxed); }

// an XAPI heap about to free a block: it keeps its free pages committed
void KeepCommitted(uint32_t heap) {
    constexpr uint32_t kDeCommitFreeBlockThreshold = 0x28, kDeCommitTotalFreeThreshold = 0x2C;
    if (!heap || !mem::IsCommitted(heap) || !mem::IsCommitted(heap + kDeCommitTotalFreeThreshold + 3)) return;
    if (kernel::R32(heap + kDeCommitFreeBlockThreshold) != 0xFFFFFFFFu)
        kernel::W32(heap + kDeCommitFreeBlockThreshold, 0xFFFFFFFFu);
    if (kernel::R32(heap + kDeCommitTotalFreeThreshold) != 0xFFFFFFFFu)
        kernel::W32(heap + kDeCommitTotalFreeThreshold, 0xFFFFFFFFu);
}

}  // namespace mka::heap

X86_FUNC(sub_0029D060) {
    uint32_t size = kernel::R32(ctx.esp + 8);
    __imp__sub_0029D060(ctx, base);
    uint32_t block = ctx.eax;
    if (block && size && size < (64u << 20) && mka::heap::OnMainThread() && mem::IsCommitted(block) &&
        mem::IsCommitted(block + size - 1))
        std::memset(kernel::HostPtr(block), 0, size);
}

// RtlFreeHeap(heap, flags, block) and its slow path RtlFreeHeapSlowly
X86_FUNC(sub_002A93AE) {
    mka::heap::KeepCommitted(kernel::R32(ctx.esp + 4));
    __imp__sub_002A93AE(ctx, base);
}

X86_FUNC(sub_002A95A2) {
    mka::heap::KeepCommitted(kernel::R32(ctx.esp + 4));
    __imp__sub_002A95A2(ctx, base);
}
