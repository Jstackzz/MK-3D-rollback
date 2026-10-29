// Snapshot-able fibers; see sfiber.h.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "mk3d/sfiber.h"

#include <cstring>
#include <exception>

#include "core/log.h"

// sf_switch(void** save_sp, void* new_sp): parks the current context on its stack, writes the stack pointer to
// *save_sp and resumes the context parked at new_sp. Frame (from the stack pointer up): TEB DeallocationStack,
// StackLimit, StackBase, x87 control word and MXCSR (8 bytes), xmm6-15 (160), r15 r14 r13 r12 rsi rdi rbx rbp,
// return address.
__asm__(
    ".intel_syntax noprefix\n"
    ".text\n"
    ".globl sf_switch\n"
    ".def sf_switch; .scl 2; .type 32; .endef\n"
    "sf_switch:\n"
    "  push rbp\n"
    "  push rbx\n"
    "  push rdi\n"
    "  push rsi\n"
    "  push r12\n"
    "  push r13\n"
    "  push r14\n"
    "  push r15\n"
    "  sub rsp, 168\n"
    "  movdqu [rsp + 8], xmm6\n"
    "  movdqu [rsp + 24], xmm7\n"
    "  movdqu [rsp + 40], xmm8\n"
    "  movdqu [rsp + 56], xmm9\n"
    "  movdqu [rsp + 72], xmm10\n"
    "  movdqu [rsp + 88], xmm11\n"
    "  movdqu [rsp + 104], xmm12\n"
    "  movdqu [rsp + 120], xmm13\n"
    "  movdqu [rsp + 136], xmm14\n"
    "  movdqu [rsp + 152], xmm15\n"
    "  stmxcsr [rsp]\n"
    "  fnstcw [rsp + 4]\n"
    "  push qword ptr gs:[0x08]\n"      // StackBase
    "  push qword ptr gs:[0x10]\n"      // StackLimit
    "  push qword ptr gs:[0x1478]\n"    // DeallocationStack
    "  mov [rcx], rsp\n"
    "  mov rsp, rdx\n"
    "  pop qword ptr gs:[0x1478]\n"
    "  pop qword ptr gs:[0x10]\n"
    "  pop qword ptr gs:[0x08]\n"
    "  ldmxcsr [rsp]\n"
    "  fldcw [rsp + 4]\n"
    "  movdqu xmm6, [rsp + 8]\n"
    "  movdqu xmm7, [rsp + 24]\n"
    "  movdqu xmm8, [rsp + 40]\n"
    "  movdqu xmm9, [rsp + 56]\n"
    "  movdqu xmm10, [rsp + 72]\n"
    "  movdqu xmm11, [rsp + 88]\n"
    "  movdqu xmm12, [rsp + 104]\n"
    "  movdqu xmm13, [rsp + 120]\n"
    "  movdqu xmm14, [rsp + 136]\n"
    "  movdqu xmm15, [rsp + 152]\n"
    "  add rsp, 168\n"
    "  pop r15\n"
    "  pop r14\n"
    "  pop r13\n"
    "  pop r12\n"
    "  pop rsi\n"
    "  pop rdi\n"
    "  pop rbx\n"
    "  pop rbp\n"
    "  ret\n"
    // a new fiber's first resume returns here: r12 = param, r13 = entry, rsp 16-byte aligned
    ".globl sf_start\n"
    ".def sf_start; .scl 2; .type 32; .endef\n"
    "sf_start:\n"
    "  mov rcx, r13\n"                  // sf_main(entry, param)
    "  mov rdx, r12\n"
    "  sub rsp, 32\n"                   // shadow space; rsp stays 16-aligned, so the callee starts at 8 mod 16
    "  call sf_main\n"
    "  int3\n"                          // an entry never returns
    ".att_syntax prefix\n");

extern "C" void sf_switch(void** save_sp, void* new_sp);
extern "C" void sf_start();

namespace {

// Windows calls the unhandled exception filter (the runtime's crash line and dump) from the bottom of a thread's own
// stack, which an exception raised on a fiber never reaches: the search for a handler ends where the fiber's stack
// does and the game was gone without a word. The filter is called from here instead, and the game ends at once, with
// the exception's code, as it would have on the thread's own stack.
LONG FiberCrash(EXCEPTION_POINTERS* e) {
    if (IsDebuggerPresent()) return EXCEPTION_CONTINUE_SEARCH;
    UnhandledExceptionFilter(e);
    TerminateProcess(GetCurrentProcess(), e->ExceptionRecord->ExceptionCode);
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

// the bottom of every fiber's stack
extern "C" void sf_main(sfiber::Entry entry, void* param) {
    __try {
        entry(param);
    } __except (FiberCrash(GetExceptionInformation())) {
    }
}

namespace sfiber {
namespace {

struct Slot {
    bool alive = false;
    void* sp = nullptr;  // parked stack pointer
};

uint8_t* g_region = nullptr;  // kSlots stacks of kStackSize, a guard page below each
constexpr size_t kGuard = 0x1000;
constexpr size_t kStride = kStackSize + kGuard;
Slot g_slots[kSlots];
void* g_main_sp = nullptr;  // the thread's own stack while a fiber runs
int g_current = -1;

uint8_t* StackLow(int i) { return g_region + size_t(i) * kStride + kGuard; }
uint8_t* StackTop(int i) { return StackLow(i) + kStackSize; }

bool Reserve() {
    if (g_region) return true;
    g_region = static_cast<uint8_t*>(VirtualAlloc(nullptr, kStride * kSlots, MEM_RESERVE, PAGE_NOACCESS));
    if (!g_region) {
        LOGE("sfiber: cannot reserve %zu MB for fiber stacks", kStride * kSlots >> 20);
        return false;
    }
    return true;
}

}  // namespace

int Create(Entry entry, void* param) {
    if (!Reserve()) return -1;
    int i = 0;
    while (i < kSlots && g_slots[i].alive) i++;
    if (i == kSlots) {
        LOGE("sfiber: all %d fiber slots in use", kSlots);
        return -1;
    }
    static bool committed[kSlots] = {};
    if (!committed[i]) {
        if (!VirtualAlloc(StackLow(i), kStackSize, MEM_COMMIT, PAGE_READWRITE)) {
            LOGE("sfiber: cannot commit fiber stack %d", i);
            return -1;
        }
        committed[i] = true;
    }
    // the frame sf_switch pops: TEB fields, MXCSR + x87 CW, xmm6-15, 8 registers, return address sf_start
    uint64_t* top = reinterpret_cast<uint64_t*>(StackTop(i));
    uint64_t* sp = top - 4;  // 32 bytes of headroom, keeps sf_start's stack aligned
    *--sp = reinterpret_cast<uint64_t>(&sf_start);
    uint64_t regs[8] = {0, 0, 0, 0, 0, 0, 0, 0};  // pop order r15 r14 r13 r12 rsi rdi rbx rbp
    regs[2] = reinterpret_cast<uint64_t>(entry);    // r13
    regs[3] = reinterpret_cast<uint64_t>(param);    // r12
    for (int r = 7; r >= 0; r--) *--sp = regs[r];
    sp -= 21;  // 168 bytes: MXCSR, x87 CW, xmm6-15
    std::memset(sp, 0, 168);
    reinterpret_cast<uint32_t*>(sp)[0] = 0x1F80;  // MXCSR default
    reinterpret_cast<uint16_t*>(sp)[2] = 0x027F;  // x87 CW default (64-bit precision, all masked)
    // popped DeallocationStack first (lowest), then StackLimit, then StackBase
    *--sp = reinterpret_cast<uint64_t>(StackTop(i));           // StackBase
    *--sp = reinterpret_cast<uint64_t>(StackLow(i));           // StackLimit
    *--sp = reinterpret_cast<uint64_t>(StackLow(i) - kGuard);  // DeallocationStack
    g_slots[i].sp = sp;
    g_slots[i].alive = true;
    return i;
}

void Destroy(int fiber) {
    if (fiber < 0 || fiber >= kSlots) return;
    if (fiber == g_current) {
        LOGE("sfiber: a fiber cannot destroy itself");
        return;
    }
    g_slots[fiber].alive = false;
    g_slots[fiber].sp = nullptr;
}

void SwitchTo(int fiber) {
    if (fiber == g_current) return;
    // the C++ runtime keeps its exception state (the open catch blocks) per thread, which every fiber shares: a switch
    // inside a catch block leaves it open under another fiber's frames, and ending it later aborts (C0000409 in
    // vcruntime _FindAndUnlinkFrame). The recompiled code runs a resume only after its handler; this says if anything
    // switches inside one again.
    static bool warned = false;
    if (!warned && std::current_exception()) {
        warned = true;
        LOGE("sfiber: switch to fiber %d inside a catch block", fiber);
    }
    void** save = g_current < 0 ? &g_main_sp : &g_slots[g_current].sp;
    void* to = fiber < 0 ? g_main_sp : g_slots[fiber].sp;
    if (fiber >= 0 && !g_slots[fiber].alive) {
        LOGE("sfiber: switch to dead fiber %d", fiber);
        return;
    }
    g_current = fiber;
    sf_switch(save, to);
    // back here once something switched to the context this call parked
}

int Current() { return g_current; }
bool Alive(int fiber) { return fiber >= 0 && fiber < kSlots && g_slots[fiber].alive; }

int LiveCount() {
    int n = 0;
    for (const Slot& s : g_slots) n += s.alive;
    return n;
}

size_t UsedStackBytes() {
    size_t n = 0;
    for (int i = 0; i < kSlots; i++)
        if (g_slots[i].alive && g_slots[i].sp) n += size_t(StackTop(i) - static_cast<uint8_t*>(g_slots[i].sp));
    return n;
}

// layout: u32 count, then per live slot: u32 index, u64 sp offset from the stack top, the bytes [sp, top)
void Save(Snapshot& s) {
    s.data.clear();
    auto put = [&](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        s.data.insert(s.data.end(), b, b + n);
    };
    uint32_t count = 0;
    for (const Slot& sl : g_slots) count += sl.alive;
    put(&count, 4);
    for (int i = 0; i < kSlots; i++) {
        if (!g_slots[i].alive) continue;
        uint32_t idx = uint32_t(i);
        uint64_t used = uint64_t(StackTop(i) - static_cast<uint8_t*>(g_slots[i].sp));
        put(&idx, 4);
        put(&used, 8);
        put(g_slots[i].sp, size_t(used));
    }
}

void Load(const Snapshot& s) {
    if (g_current != -1) {
        LOGE("sfiber: load while fiber %d runs", g_current);
        return;
    }
    for (Slot& sl : g_slots) sl = Slot{};
    const uint8_t* p = s.data.data();
    if (s.data.size() < 4) return;
    uint32_t count;
    std::memcpy(&count, p, 4);
    p += 4;
    for (uint32_t k = 0; k < count; k++) {
        uint32_t idx;
        uint64_t used;
        std::memcpy(&idx, p, 4);
        std::memcpy(&used, p + 4, 8);
        p += 12;
        uint8_t* sp = StackTop(int(idx)) - used;
        std::memcpy(sp, p, size_t(used));
        p += used;
        g_slots[idx].alive = true;
        g_slots[idx].sp = sp;
    }
}

}  // namespace sfiber
