// Rollback state machinery; see rollback.h.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "mk3d/rollback.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <vector>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "mk3d/tasks.h"
#include "kernel/host_rollback.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

#include "mk3d/game.h"

namespace mk3d::rb {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kPage = 0x1000;
constexpr uint32_t kUserLow = 0x10000, kUserHigh = 0x80000000;
constexpr uint32_t kPhys = 0x80000000, kPhysSize = 64u << 20, kWc = 0xF0000000;
constexpr size_t kCtxBytes = offsetof(X86Context, thread_handle);  // guest state; the runtime's tail stays

enum : uint8_t { kUntracked = 0, kClean = 1, kDirty = 2, kForeign = 3 };

std::vector<uint8_t> g_page(0x100000, kUntracked);  // per 4 KB guest page
std::mutex g_lock;
bool g_armed = false;
bool g_restoring = false;
DWORD g_main_tid = 0;
uint8_t* g_base = nullptr;
std::vector<uint32_t> g_dirty;  // pages the main thread made writable in the open frame

// Byte ranges another thread owns on pages that otherwise hold simulation state ("volatile"). Such a page stays
// tracked whichever thread writes it first, and a restore leaves those bytes as they are now; hashes skip them. Some
// are known (a game names them once with AddFixedVolatile, marked at every Arm), the rest are added as they show
// up: what another thread's file read fills (BeforeOsWrite), queues a game's loader thread shares with the main
// thread (through AddVolatile).
struct Range {
    uint32_t va, size;
};
std::vector<Range>& FixedVolatile() {
    static std::vector<Range> v;
    return v;
}
std::vector<uint8_t> g_shared(0x100000, 0);  // per page: holds volatile bytes
std::vector<uint8_t> g_main_ever(0x100000, 0);  // per page: the main thread wrote it since arming
std::vector<uint32_t> g_os_written(0x100000, 0);  // per page: the CloseFrame count when an OS write last made it writable
uint32_t g_closes = 0;
std::vector<uint32_t> g_shared_pages;
struct Mask {
    uint64_t bits[kPage / 64] = {};  // a bit per byte
};
std::unordered_map<uint32_t, Mask> g_masks;  // the shared pages' volatile bytes

// each run of volatile bytes on a page: fn(offset in page, size)
template <class F>
void ForVolatile(uint32_t page, F&& fn) {
    auto it = g_masks.find(page);
    if (it == g_masks.end()) return;
    const uint64_t* bits = it->second.bits;
    uint32_t b = 0;
    while (b < kPage) {
        uint64_t w = bits[b >> 6] >> (b & 63);
        if (!w) {
            b = (b | 63) + 1;
            continue;
        }
        if (!(w & 1)) {
            b++;
            continue;
        }
        uint32_t start = b;
        while (b < kPage && (bits[b >> 6] >> (b & 63) & 1)) b++;
        fn(start, b - start);
    }
}

void Protect(uint32_t page, bool read_only);

// g_lock held: bytes another thread owns from now on; a page that went foreign for them is tracked again. True if
// any of them was not volatile yet.
bool MarkVolatile(uint32_t va, uint32_t size) {
    bool added = false;
    for (uint64_t at = va; at < uint64_t(va) + size;) {
        uint32_t page = uint32_t(at) & ~(kPage - 1), off = uint32_t(at) - page;
        uint32_t n = uint32_t(std::min<uint64_t>(uint64_t(va) + size - at, kPage - off));
        at += n;
        if (g_page[page >> 12] == kUntracked) continue;
        Mask& m = g_masks[page];
        for (uint32_t b = off; b < off + n; b++) {
            uint64_t bit = 1ull << (b & 63);
            if (!(m.bits[b >> 6] & bit)) added = true;
            m.bits[b >> 6] |= bit;
        }
        if (g_shared[page >> 12]) continue;
        g_shared[page >> 12] = 1;
        g_shared_pages.push_back(page);
        if (g_page[page >> 12] == kForeign) {
            g_page[page >> 12] = kClean;
            Protect(page, true);
        }
    }
    return added;
}

// a page's contents with its volatile bytes zeroed (hashes, sync test details)
const uint8_t* Masked(uint32_t page, uint8_t* tmp) {
    if (!g_shared[page >> 12]) return g_base + page;
    std::memcpy(tmp, g_base + page, kPage);
    ForVolatile(page, [&](uint32_t off, uint32_t n) { std::memset(tmp + off, 0, n); });
    return tmp;
}

// a page taken while no frame is kept (between a restore to the oldest frame and OpenFrame): the next frame's log
std::vector<uint32_t> g_pending_pages;
std::vector<uint8_t> g_pending_data;
PVOID g_veh = nullptr;

struct FrameState {
    uint32_t frame = 0;
    std::vector<uint32_t> pages;  // undo log: pages first written in this frame
    std::vector<uint8_t> data;    // their contents at the top of the frame
    tasks::Snapshot tasks;
    alignas(64) uint8_t ctx[kCtxBytes];
    bool closed = false;
    uint64_t hash = 0;
};
std::deque<FrameState> g_frames;  // oldest first; the last one is open between OpenFrame and CloseFrame
Stats g_stats;
uint64_t g_last_hash = 0;
uint32_t g_last_pages = 0;
bool g_detail = false;
std::vector<uint32_t> g_detail_pages;  // the last closed frame's pages and their contents then
std::vector<uint8_t> g_detail_data;
std::vector<uint8_t> g_detail_top;  // and their contents at the top of the frame

uint32_t Canon(uint32_t va) { return va - kWc < kPhysSize ? va - kWc + kPhys : va; }
bool InPhys(uint32_t va) { return va - kPhys < kPhysSize; }

void Protect(uint32_t page, bool read_only) {
    DWORD old;
    DWORD prot = read_only ? PAGE_READONLY : PAGE_READWRITE;
    VirtualProtect(g_base + page, kPage, prot, &old);
    if (InPhys(page)) VirtualProtect(g_base + (page - kPhys + kWc), kPage, prot, &old);
}

// a run of pages at once (arming)
void ProtectRun(uint32_t first, uint32_t count, bool read_only) {
    DWORD old;
    DWORD prot = read_only ? PAGE_READONLY : PAGE_READWRITE;
    VirtualProtect(g_base + first, size_t(count) * kPage, prot, &old);
    if (InPhys(first)) VirtualProtect(g_base + (first - kPhys + kWc), size_t(count) * kPage, prot, &old);
}

// the first write of a page in a frame: log it (main thread) or give it up (another thread); lock held. A main
// thread write between CloseFrame and OpenFrame goes into the newest log, closed or not: the page did not change
// during that frame (or it would not be protected), so its contents now are its contents at the frame's top.
struct ForeignEvent {
    uint32_t va;
    DWORD tid;
    uintptr_t rip;
};
std::vector<ForeignEvent> g_foreign_new;  // pages that went foreign since the last CloseFrame, for the log
std::vector<ForeignEvent> g_mixed_new;    // bytes another thread wrote on pages the main thread uses
int g_foreign_logged = 0, g_mixed_logged = 0;
bool g_after_restore = false;  // Restore started the frame for the runtime (host_rollback::Undo)
std::vector<uint8_t> g_restore_mark(0x100000, 0);  // Restore: pages made writable already

void Take(uint32_t page, uint32_t va = 0, uintptr_t rip = 0) {
    g_stats.faults++;
    bool main = GetCurrentThreadId() == g_main_tid;
    if (main || g_shared[page >> 12]) {
        if (main) g_main_ever[page >> 12] = 1;
        std::vector<uint32_t>& pages = g_frames.empty() ? g_pending_pages : g_frames.back().pages;
        std::vector<uint8_t>& data = g_frames.empty() ? g_pending_data : g_frames.back().data;
        pages.push_back(page);
        size_t at = data.size();
        data.resize(at + kPage);
        std::memcpy(data.data() + at, g_base + page, kPage);
        g_stats.logged_bytes += kPage;
        g_page[page >> 12] = kDirty;
        g_dirty.push_back(page);
    } else {
        g_page[page >> 12] = kForeign;
        g_stats.foreign_pages++;
        if (g_foreign_new.size() < 32) g_foreign_new.push_back({va ? va : page, GetCurrentThreadId(), rip});
    }
    Protect(page, false);
}

LONG CALLBACK OnFault(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2 || er->ExceptionInformation[0] != 1)
        return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t a = er->ExceptionInformation[1];
    if (!g_base || a < uintptr_t(g_base) || a - uintptr_t(g_base) >= (uint64_t(1) << 32)) return EXCEPTION_CONTINUE_SEARCH;
    uint32_t page = Canon(uint32_t(a - uintptr_t(g_base))) & ~(kPage - 1);
    std::lock_guard<std::mutex> lock(g_lock);
    uint8_t st = g_page[page >> 12];
    if (st == kUntracked) {
        // a real fault, unless the page was tracked when the write faulted and is writable now (Disarm let it go
        // while this thread waited for the lock): the write goes through on a retry
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)))
            return EXCEPTION_CONTINUE_EXECUTION;
        return EXCEPTION_CONTINUE_SEARCH;
    }
    uint32_t va = Canon(uint32_t(a - uintptr_t(g_base)));
    bool other = GetCurrentThreadId() != g_main_tid;
    // Another thread writing a page the main thread uses or may use (an object both keep state in, like the loader's
    // requests, which the main thread destroys after the loader thread is done): the bytes it writes are its own from
    // now on, the rest of the page stays the main thread's and is put back by a restore. Only its first write after
    // the page was protected is seen; the probe (MKD_RB_PROBE) sees more. Only physical memory the main thread has
    // not written (sound buffers other threads fill) goes foreign: never put back.
    if (other && st != kForeign && (va < kUserHigh || g_shared[page >> 12] || g_main_ever[page >> 12])) {
        if (MarkVolatile(va & ~3u, 8) && g_mixed_new.size() < 32)
            g_mixed_new.push_back({va, GetCurrentThreadId(), uintptr_t(ep->ContextRecord->Rip)});
    }
    if (st == kClean) {
        Take(page, va, uintptr_t(ep->ContextRecord->Rip));
    } else {
        Protect(page, false);  // another thread took it a moment ago, or the probe protected a page this thread wrote
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

// the host OS is about to write guest memory itself (a file read): no fault would tell us
void BeforeOsWrite(uint32_t va, uint32_t len) {
    if (!g_armed || !len) return;
    std::lock_guard<std::mutex> lock(g_lock);
    // another thread's read (a loader thread streaming sound): the bytes it fills are its own, the rest of their
    // pages (other objects) stays the simulation's
    if (GetCurrentThreadId() != g_main_tid) MarkVolatile(Canon(va), len);
    // every page must be writable now, whatever its state: the OS fails a read into a protected page (no fault to
    // catch), and the game retries such a read for ever
    for (uint64_t p = Canon(va) & ~(kPage - 1); p < uint64_t(Canon(va)) + len; p += kPage) {
        uint8_t st = g_page[size_t(p >> 12)];
        if (st == kClean) Take(uint32_t(p));
        else if (st == kDirty) Protect(uint32_t(p), false);
        g_os_written[size_t(p >> 12)] = g_closes;
    }
}

void OnCommit(uint32_t va, uint32_t size, bool committed) {
    if (!g_armed || va < kUserLow || va >= kUserHigh) return;
    std::lock_guard<std::mutex> lock(g_lock);
    for (uint64_t p = va; p < uint64_t(va) + size; p += kPage) {
        uint8_t& st = g_page[size_t(p >> 12)];
        if (committed && st == kUntracked) {
            st = kClean;
            Protect(uint32_t(p), true);
            g_stats.tracked_pages++;
        } else if (committed && st == kClean) {
            Protect(uint32_t(p), true);  // committing a committed page made it writable: its next write must be seen
        } else if (!committed) {
            st = kUntracked;
        }
    }
}

uint32_t g_hash_lo = 0, g_hash_hi = 0xFFFFFFFF;  // only pages in [lo, hi) count in the frame hash
bool g_hashing = false;                           // frame hashes (sync tests); off they cost a pass over every page

// g_lock held: every page written since the last call protected again, runs of neighbours in one call
void ProtectDirty() {
    std::sort(g_dirty.begin(), g_dirty.end());
    uint32_t run = 0, count = 0;
    for (uint32_t p : g_dirty) {
        if (g_page[p >> 12] != kDirty) continue;
        g_page[p >> 12] = kClean;
        if (count && p == run + count * kPage) {
            count++;
            continue;
        }
        if (count) ProtectRun(run, count, true);
        run = p;
        count = 1;
    }
    if (count) ProtectRun(run, count, true);
    g_dirty.clear();
}

// the pages a frame wrote, and the shared pages always: whether another thread wrote one in a frame is timing
std::vector<uint32_t> HashedPages(const std::vector<uint32_t>& pages) {
    std::vector<uint32_t> sorted;
    sorted.reserve(pages.size() + g_shared_pages.size());
    for (uint32_t p : pages)
        if (!g_shared[p >> 12]) sorted.push_back(p);
    sorted.insert(sorted.end(), g_shared_pages.begin(), g_shared_pages.end());
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    return sorted;
}

uint64_t HashPages(const std::vector<uint32_t>& pages) {
    std::vector<uint32_t> sorted = HashedPages(pages);
    uint64_t h = 0x9E3779B97F4A7C15ull;
    for (uint32_t p : sorted) {
        if (g_page[p >> 12] == kForeign || p < g_hash_lo || p >= g_hash_hi || !mem::IsCommitted(p)) continue;
        h = (h ^ p) * 0x100000001B3ull;
        alignas(8) uint8_t tmp[kPage];
        const uint64_t* w = reinterpret_cast<const uint64_t*>(Masked(p, tmp));
        for (size_t i = 0; i < kPage / 8; i++) h = (h ^ w[i]) * 0x9E3779B97F4A7C15ull + (h >> 29);
    }
    return h;
}

// MKD_RB_PROBE=<ms>: every <ms> ms the pages the main thread wrote in the open frame are protected again, so
// another thread's write to one of them faults as well (without it such a write goes unseen: the page stays writable
// for the rest of the frame once the main thread wrote it). A diagnostic, it costs the main thread faults.
const int g_probe_ms = [] {
    const char* s = std::getenv("MKD_RB_PROBE");
    int ms = s ? std::atoi(s) : 0;
    if (ms > 0)
        std::thread([ms] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                std::lock_guard<std::mutex> lock(g_lock);
                if (!g_armed) continue;
                for (uint32_t p : g_dirty)  // not under an OS write of this frame (BeforeOsWrite)
                    if (g_page[p >> 12] == kDirty && g_os_written[p >> 12] != g_closes) Protect(p, true);
            }
        }).detach();
    return ms;
}();

}  // namespace

bool Arm(X86Context& ctx, uint32_t frame) {
    if (g_armed) return true;
    g_base = mem::Base();
    if (!g_base) return false;
    g_main_tid = GetCurrentThreadId();
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, OnFault);
    kernel::SetGuestWriteHook(&BeforeOsWrite);
    mem::SetCommitHook(&OnCommit);
    auto t0 = Clock::now();
    {
    std::lock_guard<std::mutex> lock(g_lock);
    std::fill(g_page.begin(), g_page.end(), kUntracked);
    std::fill(g_shared.begin(), g_shared.end(), uint8_t(0));
    g_shared_pages.clear();
    g_masks.clear();
    std::fill(g_main_ever.begin(), g_main_ever.end(), uint8_t(0));
    g_after_restore = false;
    g_pending_pages.clear();
    g_pending_data.clear();
    g_stats = Stats{};
    // committed user pages, physical memory, and the main thread's guest stack, protected run by run
    auto track = [&](uint32_t lo, uint32_t hi, bool need_commit) {
        uint32_t run = 0, count = 0;
        for (uint64_t p = lo; p <= hi; p += kPage) {
            bool take = p < hi && (!need_commit || mem::IsCommitted(uint32_t(p)));
            if (take) {
                if (!count) run = uint32_t(p);
                count++;
                g_page[size_t(p >> 12)] = kClean;
                g_stats.tracked_pages++;
            } else if (count) {
                ProtectRun(run, count, true);
                count = 0;
            }
        }
    };
    track(kUserLow, kUserHigh, true);
    track(kPhys, kPhys + kPhysSize, false);
    for (const Range& v : FixedVolatile()) MarkVolatile(v.va, v.size);
    // the main thread's guest stack: the committed run around esp (a guard page sits below it)
    uint32_t lo = ctx.esp & ~(kPage - 1), hi = lo;
    while (lo > kPage && mem::IsCommitted(lo - kPage)) lo -= kPage;
    while (hi < 0xFFFFE000u && mem::IsCommitted(hi + kPage)) hi += kPage;
    if (mem::IsCommitted(ctx.esp)) track(lo, hi + kPage, true);
    LOGI("rollback: main stack %08X..%08X", lo, hi + kPage);
    g_frames.clear();
    g_dirty.clear();
    g_armed = true;
    LOGI("rollback: armed at frame %u, %llu pages tracked (%.1f MB), %.1f ms", frame,
         (unsigned long long)g_stats.tracked_pages, double(g_stats.tracked_pages) * 4 / 1024,
         std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    // the first frame's snapshot
    g_frames.emplace_back();
    g_frames.back().frame = frame;
    tasks::Save(g_frames.back().tasks);
    std::memcpy(g_frames.back().ctx, &ctx, kCtxBytes);
    }
    // The runtime's journals take their own locks: never with g_lock held. A thread holding one of those (the
    // mixer's, the pools') can fault on a tracked page and wait for g_lock in OnFault.
    host_rollback::Begin(frame);
    return true;
}

void Disarm() {
    if (!g_armed) return;
    host_rollback::End();  // before g_lock (see Arm)
    std::lock_guard<std::mutex> lock(g_lock);
    uint32_t run = 0, count = 0;
    for (uint64_t p = 0; p <= 0xFFFFFFFFull; p += kPage) {
        bool take = p < 0xFFFFF000ull && (g_page[size_t(p >> 12)] == kClean || g_page[size_t(p >> 12)] == kDirty);
        if (take) {
            if (!count) run = uint32_t(p);
            count++;
        } else if (count) {
            ProtectRun(run, count, false);
            count = 0;
        }
        g_page[size_t(p >> 12)] = kUntracked;
        if (p >= 0xFFFFF000ull) break;
    }
    g_frames.clear();
    g_dirty.clear();
    g_armed = false;
    kernel::SetGuestWriteHook(nullptr);
    mem::SetCommitHook(nullptr);
    LOGI("rollback: disarmed (%llu faults, %llu foreign pages)", (unsigned long long)g_stats.faults,
         (unsigned long long)g_stats.foreign_pages);
}

bool Armed() { return g_armed; }

void CloseFrame() {
    if (!g_armed || g_frames.empty() || g_frames.back().closed) return;
    auto t0 = Clock::now();
    uint32_t confirm = UINT32_MAX;
    {
    std::lock_guard<std::mutex> lock(g_lock);
    FrameState& f = g_frames.back();
    if (g_hashing) {
        f.hash = HashPages(f.pages);
        g_last_hash = f.hash;
    }
    g_last_pages = uint32_t(f.pages.size());
    if (g_detail) {
        g_detail_pages.clear();
        g_detail_data.clear();
        g_detail_top.clear();
        for (size_t i = 0; i < f.pages.size(); i++) {
            uint32_t p = f.pages[i];
            if (g_page[p >> 12] == kForeign || p < g_hash_lo || p >= g_hash_hi || !mem::IsCommitted(p)) continue;
            g_detail_pages.push_back(p);
            g_detail_data.insert(g_detail_data.end(), g_base + p, g_base + p + kPage);
            g_detail_top.insert(g_detail_top.end(), f.data.begin() + ptrdiff_t(i * kPage), f.data.begin() + ptrdiff_t((i + 1) * kPage));
        }
    }
    f.closed = true;
    g_closes++;
    ProtectDirty();
    if (g_frames.size() > size_t(kKeepFrames)) {
        while (g_frames.size() > size_t(kKeepFrames)) g_frames.pop_front();
        confirm = g_frames.front().frame;
    }
    // who made pages foreign (another thread's write; rip 0: the runtime writing for the OS, a file read)
    for (const ForeignEvent& e : g_foreign_new) {
        if (g_foreign_logged++ >= 64) break;
        char where[256] = "the runtime (a file read)";
        if (e.rip) mk3d::DescribeCode(e.rip, where, sizeof(where));
        LOGI("rollback: page %08X is foreign from frame %u: %08X written by t%04lx in %s", e.va & ~(kPage - 1), f.frame,
             e.va, e.tid & 0xFFFF, where);
    }
    g_foreign_new.clear();
    for (const ForeignEvent& e : g_mixed_new) {
        if (g_mixed_logged++ >= 64) break;
        char where[256] = "?";
        if (e.rip) mk3d::DescribeCode(e.rip, where, sizeof(where));
        LOGI("rollback: %08X belongs to another thread too from frame %u: written by t%04lx in %s", e.va, f.frame,
             e.tid & 0xFFFF, where);
    }
    g_mixed_new.clear();
    }
    if (confirm != UINT32_MAX) host_rollback::Confirm(confirm);  // after g_lock (see Arm)
    g_stats.close_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

bool Restore(uint32_t frame, X86Context& ctx) {
    if (!g_armed) return false;
    auto t0 = Clock::now();
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (std::none_of(g_frames.begin(), g_frames.end(), [&](const FrameState& f) { return f.frame == frame; })) {
            LOGE("rollback: frame %u is no longer kept (oldest %u)", frame, g_frames.empty() ? 0 : g_frames.front().frame);
            return false;
        }
    }
    // the runtime's side first, before g_lock (see Arm): what it writes to guest memory doing so, the pages put back
    host_rollback::Undo(frame);
    std::lock_guard<std::mutex> lock(g_lock);
    struct Flag {
        Flag() { g_restoring = true; }
        ~Flag() { g_restoring = false; }
    } restoring;
    auto it = std::find_if(g_frames.begin(), g_frames.end(), [&](const FrameState& f) { return f.frame == frame; });
    if (it == g_frames.end()) {
        LOGE("rollback: frame %u is no longer kept (oldest %u)", frame, g_frames.empty() ? 0 : g_frames.front().frame);
        return false;
    }
    // newest frame first, and newest entry first within a frame: the oldest copy of a page is put back last
    std::vector<uint32_t> touched;
    for (auto f = g_frames.rbegin(); f != g_frames.rend(); ++f) {
        for (size_t i = f->pages.size(); i-- > 0;) {
            uint32_t p = f->pages[i];
            if (g_page[p >> 12] == kForeign || !mem::IsCommitted(p)) continue;
            if (!g_restore_mark[p >> 12]) {  // whatever its state: the probe may have protected a dirty page
                g_restore_mark[p >> 12] = 1;
                Protect(p, false);
                touched.push_back(p);
            }
            if (g_shared[p >> 12]) {
                // the other thread's bytes keep their values now
                alignas(8) uint8_t now[kPage];
                std::memcpy(now, g_base + p, kPage);
                std::memcpy(g_base + p, f->data.data() + i * kPage, kPage);
                ForVolatile(p, [&](uint32_t off, uint32_t n) { std::memcpy(g_base + p + off, now + off, n); });
            } else {
                std::memcpy(g_base + p, f->data.data() + i * kPage, kPage);
            }
        }
        if (f->frame == frame) break;
    }
    for (uint32_t p : touched) {
        g_restore_mark[p >> 12] = 0;
        g_page[p >> 12] = kClean;
        Protect(p, true);
    }
    tasks::Load(it->tasks);
    uint32_t esp_now = ctx.esp;
    std::memcpy(&ctx, it->ctx, kCtxBytes);
    if (ctx.esp != esp_now) LOGW("rollback: guest esp %08X at frame %u, %08X now", ctx.esp, frame, esp_now);
    g_frames.erase(it, g_frames.end());
    g_after_restore = true;
    g_stats.restore_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    return true;
}

void OpenFrame(uint32_t frame, X86Context& ctx) {
    if (!g_armed) return;
    // before g_lock (see Arm); after Restore, host_rollback::Undo started this frame already
    if (!g_after_restore && (g_frames.empty() || g_frames.back().closed)) host_rollback::Frame(frame);
    g_after_restore = false;
    std::lock_guard<std::mutex> lock(g_lock);
    if (!g_frames.empty() && !g_frames.back().closed) {
        // Arm's snapshot of this same frame is the open one
        if (g_frames.back().frame == frame) return;
        LOGW("rollback: frame %u opened while %u is open", frame, g_frames.back().frame);
    }
    // pages written since CloseFrame (logged then, into the frame before) must log their state at this frame's top
    ProtectDirty();
    g_frames.emplace_back();
    FrameState& f = g_frames.back();
    f.frame = frame;
    f.pages.swap(g_pending_pages);
    f.data.swap(g_pending_data);
    g_pending_pages.clear();
    g_pending_data.clear();
    tasks::Save(f.tasks);
    std::memcpy(f.ctx, &ctx, kCtxBytes);
}

uint32_t OldestFrame() { return g_frames.empty() ? 0 : g_frames.front().frame; }
uint64_t LastFrameHash() { return g_last_hash; }
uint32_t LastFramePages() { return g_last_pages; }
Stats GetStats() { return g_stats; }

bool Restoring() { return g_restoring; }

void MaskVolatile(uint32_t page, uint8_t* data) {
    std::lock_guard<std::mutex> lock(g_lock);
    ForVolatile(page, [&](uint32_t off, uint32_t n) { std::memset(data + off, 0, n); });
}

void AddFixedVolatile(uint32_t va, uint32_t size) { FixedVolatile().push_back({va, size}); }

void AddVolatile(uint32_t va, uint32_t size) {
    if (!g_armed || !size) return;
    std::lock_guard<std::mutex> lock(g_lock);
    MarkVolatile(Canon(va), size);
}
void SetDetail(bool on) {
    g_detail = on;
    g_hashing = on;
}
void SetHashRange(uint32_t lo, uint32_t hi) {
    g_hash_lo = lo;
    g_hash_hi = hi;
}

void LastFrameDetail(std::vector<uint32_t>& pages, std::vector<uint8_t>& data, std::vector<uint8_t>* top) {
    pages = g_detail_pages;
    data = g_detail_data;
    if (top) *top = g_detail_top;
}

}  // namespace mk3d::rb
