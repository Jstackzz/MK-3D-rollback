// The engine's cooperative task system on host fibers (MK Armageddon, title 4D570034, XBE sha1 24074b8f...).
// A port of recomp/mkd/src/game/task_fibers.cpp: Armageddon's scheduler is the same code at other addresses.
//
// The engine runs every game object as a coroutine on the main guest thread. Three task classes exist, each a
// vtable (0x55DB5C C, 0x55DB84 A, 0x55DBAC and 0x55DBD4 B) whose +0x0C destroys the task, +0x10 runs it, +0x14
// yields and +0x20 replaces the task function (Deception's vtables had one more slot in front: +0x10 destroy,
// +0x14 run, +0x18 yield, +0x24 set function):
//   A: copy-stack tasks. The task function runs on the scheduler's own stack; on yield its stack frames are copied
//      out to a per-task buffer (+0x40 end, +0x44 saved sp) and copied back on resume.
//      Run 0x1C51C0, Yield 0x1C5340, SetFunction 0x1C5430.
//   B: dedicated-stack tasks. +0x40 is the top of the task's own stack, +0x44 its saved sp.
//      Run 0x1C5470, Yield 0x1C55A0, SetFunction 0x1C5650, plus 0x1C5610/0x1C5630 which hop onto the
//      scheduler stack and back for deep calls.
//   C: simple tasks that cannot yield (their Yield is the int3 at 0x1C5150). Run 0x1C50A0, SetFunction 0x1C5180
//      (the same code as A's; Deception shared one function).
// Any task may end itself through 0x1C5010 (exit: run the hook, destroy the task, return to the scheduler).
//
// Resuming a task means jumping into the middle of its function with a restored stack, which recompiled code
// cannot do: its `ret` is a C++ return, not a jump to whatever the guest stack says. So A and B tasks get a host
// fiber that keeps the lifted frames alive across yields, while the engine's own bookkeeping (saved registers,
// stack image copies, result, hooks, the 0x5B8Bxx scheduler globals) is reproduced exactly so every other piece
// of engine code sees the memory it would on the Xbox. C tasks run straight on the scheduler's host stack.
// The engine's non-local exits (SetFunction, exit) become C++ exceptions caught where the task was entered.
//
// Task layout (as in Deception): +0x04 alive, +0x0C result/wait (float; 0.0 reruns the function at once, below zero
// destroys the task through vtable+0x0C), +0x10..+0x1C saved ebp/ebx/esi/edi, +0x2C hook called on yield and after
// the function returns, +0x30 task function, +0x34 resume address, +0x40 stack buffer end (A) or stack top (B),
// +0x44 saved stack pointer (equal to +0x40 when nothing is saved). Globals: 0x7350C0 current task, 0x7350C4 wait
// time stored into +0x0C on yield, 0x7350B4 cleared before a task is destroyed, 0x5B8B40/50/60 the scheduler's
// return address, esp and ebp, 0x5B8B3C the last yield's return address, 0x5B8B54 the esp saved by 0x1C5610,
// 0x5B8B38 the time passed to SetFunction. Unlike Deception, Armageddon checks the current task pointer for NULL
// before every destroy.
//
// MKA_TASK_TRACE=1 logs the first few hundred task events.
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "mk3d/sfiber.h"
#include "game/task_fibers.h"
#include "kernel/kernel.h"

namespace {

constexpr uint32_t kCurrentTask = 0x7350C0;
constexpr uint32_t kWaitTime = 0x7350C4;
constexpr uint32_t kDestroyFlag = 0x7350B4;
constexpr uint32_t kSchedRet = 0x5B8B40;
constexpr uint32_t kSchedEsp = 0x5B8B50;
constexpr uint32_t kSchedEbp = 0x5B8B60;
constexpr uint32_t kYieldRet = 0x5B8B3C;
constexpr uint32_t kHopEsp = 0x5B8B54;
constexpr uint32_t kSetTime = 0x5B8B38;
constexpr uint32_t kDestroySlot = 0x0C;  // vtable offset of the destroy function
constexpr uint32_t kWalkResume = 0x1C4FEE;  // sub_001C4DD0 after a task's Run, a continuation (game.toml)

using kernel::R32;
using kernel::W32;

enum class Kind : char { A = 'A', B = 'B', C = 'C' };

// where one call of a task's function loop is executing (a fiber for A and B, the scheduler's stack for C)
struct Running {
    uint32_t task = 0;
    Kind kind = Kind::C;
    uint32_t run_esp = 0;  // guest esp each call of the task function starts at
    bool finished = false;
    bool exited = false;      // ended through 0x1C5010
    bool exit_alive = false;  // still alive after the exit hook: destroy it
};

struct TaskFiber {
    int fiber = -1;   // sfiber slot; this record is g_tf[fiber]
    int caller = -1;  // the fiber that ran or resumed the task (-1 the thread's own stack), to switch back to
    X86Context* ctx = nullptr;
    Running run;
    // a longjmp out of the task (the game jumps back into main's setjmp): the recompiler's resume exception, caught
    // at the fiber's root and thrown again on the scheduler's stack, where the frame holding the target catches it
    bool escaped = false;
    X86Resume escape{};
};

// SetFunction: end this call of the task function with this result
struct TaskReturn {
    float value;
};
// exit (0x1C5010): the task is over
struct TaskExit {
    bool alive;
};

// Every task's fiber record sits at a fixed place, the slot of its fiber: pointers to it on fiber stacks stay good
// when a rollback puts the stacks back (task_fibers.h).
TaskFiber g_tf[sfiber::kSlots];
std::unordered_map<uint32_t, int> g_fibers;  // task -> fiber slot
Running* g_running = nullptr;  // the task whose function is executing right now
TaskFiber* g_running_fiber = nullptr;
int g_outside_exits = 0;  // exits from outside a task's function, logged the first few times

bool Trace() {
    static const bool on = std::getenv("MKA_TASK_TRACE") != nullptr;
    return on;
}
int g_trace_lines = 0;
#define TTRACE(...)                           \
    do {                                      \
        if (Trace() && g_trace_lines < 600) { \
            g_trace_lines++;                  \
            LOGI(__VA_ARGS__);                \
        }                                     \
    } while (0)

float ReadF(uint32_t va) {
    uint32_t bits = R32(va);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

void WriteF(uint32_t va, float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    W32(va, bits);
}

// the task function loop (0x1C50B2 / 0x1C5288 / 0x1C54BA): call it until it returns something other than 0.0
void FunctionLoop(X86Context& ctx, Running& r) {
    for (;;) {
        float result;
        ctx.esp = r.run_esp;
        try {
            kernel::CallGuest(ctx, R32(r.task + 0x30), {});
            result = float(X87_ST(0));  // the function leaves its float result in st(0); the engine does fstp
            x87_pop(ctx);
        } catch (const TaskReturn& t) {
            result = t.value;  // the engine returns straight into this loop with st(0) = time
            ctx.ebp = R32(kSchedEbp);
        } catch (const TaskExit& e) {
            r.exited = true;
            r.exit_alive = e.alive;
            break;
        }
        WriteF(r.task + 0x0C, result);
        if (r.kind != Kind::C) W32(r.task + 0x44, R32(r.task + 0x40));  // nothing saved: the function ran to its end
        if (result != 0.0f) break;
    }
    r.finished = true;
}

void FiberEntry(void*) {
    TaskFiber& tf = g_tf[sfiber::Current()];
    try {
        FunctionLoop(*tf.ctx, tf.run);
    } catch (const X86Resume& r) {
        tf.escaped = true;
        tf.escape = r;
    }
    for (;;) sfiber::SwitchTo(tf.caller);  // never resumed after finishing: RunTask drops the fiber
}

// restores the running-task globals however RunTask is left (a longjmp leaves by exception)
struct RunningScope {
    Running* outer;
    struct TaskFiber* outer_fiber;
    RunningScope(Running* r, TaskFiber* f);
    ~RunningScope();
};

RunningScope::RunningScope(Running* r, TaskFiber* f) : outer(g_running), outer_fiber(g_running_fiber) {
    g_running = r;
    g_running_fiber = f;
}
RunningScope::~RunningScope() {
    g_running = outer;
    g_running_fiber = outer_fiber;
}

void DropFiber(uint32_t task) {
    auto it = g_fibers.find(task);
    if (it == g_fibers.end()) return;
    sfiber::Destroy(it->second);
    g_tf[it->second] = TaskFiber{};
    g_fibers.erase(it);
}

TaskFiber& NewFiber(uint32_t task, X86Context& ctx) {
    DropFiber(task);  // a fresh start never continues an old fiber left by a task destroyed while parked
    int slot = sfiber::Create(FiberEntry, nullptr);
    if (slot < 0) {
        LOGE("tasks: no fiber for task %08X", task);
        std::abort();
    }
    TaskFiber& tf = g_tf[slot];
    tf = TaskFiber{};
    tf.fiber = slot;
    tf.ctx = &ctx;
    tf.run.task = task;
    g_fibers.emplace(task, slot);
    static int most = 0;
    if (int(g_fibers.size()) > most) {
        most = int(g_fibers.size());
        if (most % 8 == 0) LOGI("tasks: %d task fibers alive", most);
    }
    return tf;
}

// the destroy at the end of a run or an exit: clear 0x7350B4, then destroy the current task (read again, as the
// engine does) through its vtable if there is one and it is still alive
void Destroy(X86Context& ctx, uint32_t sched_esp) {
    W32(kDestroyFlag, 0);
    uint32_t cur = R32(kCurrentTask);
    if (cur == 0 || R32(cur + 0x04) == 0) return;
    ctx.esp = sched_esp;
    kernel::CallGuest(ctx, R32(R32(cur) + kDestroySlot), {cur});
}

// after the function loop: the post-run hook and the destroy of 0x1C50DD / 0x1C52C5 / 0x1C54F6, or the end of
// an exit (0x1C5049), which already ran the hook
void Finish(X86Context& ctx, Running& r, uint32_t sched_esp) {
    uint32_t task = r.task;
    if (r.exited) {
        TTRACE("tasks: %08X exited%s", task, r.exit_alive ? "" : ", dead after hook");
        if (r.exit_alive) Destroy(ctx, sched_esp);
        return;
    }
    float result = ReadF(task + 0x0C);
    bool alive = true;
    if (uint32_t hook = R32(task + 0x2C)) {
        ctx.esp = r.kind == Kind::B ? R32(task + 0x40) : sched_esp;  // B calls it on the task's stack
        kernel::CallGuest(ctx, hook, {});
        alive = R32(task + 0x04) != 0;
    }
    TTRACE("tasks: %08X finished, result %g%s", task, double(result), alive ? "" : ", dead after hook");
    if (alive && result < 0.0f) Destroy(ctx, sched_esp);
}

// Run (vtable +0x10): called by the scheduler with the current task in 0x7350C0. Returns when the task yields
// or finishes, with the stack back at the scheduler's level, like the engine's `jmp [0x5B8B40]`.
void RunTask(X86Context& ctx, Kind kind) {
    uint8_t* base = kernel::g_base;
    uint32_t ret = R32(ctx.esp);
    uint32_t sched_esp = ctx.esp + 4;
    W32(kSchedRet, ret);
    W32(kSchedEsp, sched_esp);
    W32(kSchedEbp, ctx.ebp);
    uint32_t task = R32(kCurrentTask);

    if (kind == Kind::C) {
        // cannot yield: run on this stack, nested like the engine would
        Running r;
        r.task = task;
        r.kind = kind;
        r.run_esp = sched_esp;
        TTRACE("tasks: run %08X (C) fn %08X", task, R32(task + 0x30));
        {
            RunningScope scope(&r, nullptr);
            FunctionLoop(ctx, r);  // a longjmp out passes through as the recompiler's resume exception
        }
        Finish(ctx, r, sched_esp);
        ctx.esp = sched_esp;
        ctx.ebp = R32(kSchedEbp);
        return;
    }

    uint32_t buf_end = R32(task + 0x40), saved_sp = R32(task + 0x44);
    bool resume = saved_sp < buf_end;
    auto found = g_fibers.find(task);
    TaskFiber* tf = found != g_fibers.end() ? &g_tf[found->second] : nullptr;
    if (resume && !tf) {
        LOGE("tasks: task %08X (%c) has a saved stack but no fiber; starting it over", task, char(kind));
        W32(task + 0x44, buf_end);
        resume = false;
    }
    if (!resume) tf = &NewFiber(task, ctx);
    tf->caller = sfiber::Current();
    tf->run.kind = kind;
    tf->run.run_esp = kind == Kind::A ? sched_esp : buf_end;
    TTRACE("tasks: run %08X (%c) fn %08X %s sched esp %08X saved %08X..%08X", task, char(kind), R32(task + 0x30),
           resume ? "resume" : "start", sched_esp, saved_sp, buf_end);

    if (resume) {
        // put the saved stack image back (A) or switch to the task's own stack (B), restore registers
        uint32_t resume_esp;
        if (kind == Kind::A) {
            uint32_t len = buf_end - saved_sp;
            resume_esp = sched_esp - len;
            std::memmove(base + resume_esp, base + saved_sp, len);
        } else {
            resume_esp = saved_sp;
        }
        ctx.ebp = R32(task + 0x10);
        ctx.ebx = R32(task + 0x14);
        ctx.esi = R32(task + 0x18);
        ctx.edi = R32(task + 0x1C);
        ctx.eax = task;  // the engine jumps to the resume address with eax = the task
        ctx.esp = resume_esp;
    }

    {
        RunningScope scope(&tf->run, tf);
        sfiber::SwitchTo(tf->fiber);
    }

    if (tf->escaped) {
        X86Resume r = tf->escape;
        LOGI("tasks: %08X longjmps to %08X (esp %08X); task abandoned", task, r.target, r.esp);
        DropFiber(task);
        throw r;
    }
    if (tf->run.finished) {
        Running r = tf->run;
        DropFiber(task);
        Finish(ctx, r, sched_esp);
    }
    ctx.esp = sched_esp;
    ctx.ebp = R32(kSchedEbp);
}

// Yield (vtable +0x14): called from inside a task. Saves what the engine saves, parks the fiber, and when the
// scheduler resumes the task, returns into the lifted task function right after its call.
void YieldTask(X86Context& ctx, Kind kind) {
    uint8_t* base = kernel::g_base;
    uint32_t task = R32(kCurrentTask);
    W32(task + 0x10, ctx.ebp);
    W32(task + 0x14, ctx.ebx);
    W32(task + 0x18, ctx.esi);
    W32(task + 0x1C, ctx.edi);
    W32(task + 0x0C, R32(kWaitTime));
    uint32_t ret = R32(ctx.esp);
    ctx.esp += 4;
    W32(kYieldRet, ret);
    W32(task + 0x34, ret);
    uint32_t sched_esp = R32(kSchedEsp);
    if (kind == Kind::A) {
        uint32_t len = sched_esp - ctx.esp;
        uint32_t buf_start = R32(task + 0x40) - len;
        std::memmove(base + buf_start, base + ctx.esp, len);
        W32(task + 0x44, buf_start);
    } else {
        W32(task + 0x44, ctx.esp);
    }
    if (uint32_t hook = R32(task + 0x2C)) kernel::CallGuest(ctx, hook, {});

    TaskFiber* tf = g_running_fiber;
    TTRACE("tasks: yield %08X (%c) to %08X, wait %g", task, char(kind), ret, double(ReadF(task + 0x0C)));
    if (!tf || tf->run.task != task) {
        LOGE("tasks: yield of %08X from %08X outside its fiber (running %08X)", task, ret, tf ? tf->run.task : 0);
        return;
    }
    ctx.esp = sched_esp;
    ctx.ebp = R32(kSchedEbp);
    sfiber::SwitchTo(tf->caller);
    // resumed by RunTask, which restored esp and the registers; continue after the yield call
}

// SetFunction(fn, time): replace the task function and end the current call of it with `time` as its result.
void SetFunction(X86Context& ctx) {
    uint32_t task = R32(kCurrentTask);
    uint32_t fn = R32(ctx.esp + 4);
    uint32_t time_bits = R32(ctx.esp + 8);
    W32(task + 0x30, fn);
    W32(kSetTime, time_bits);
    float time;
    std::memcpy(&time, &time_bits, 4);
    TTRACE("tasks: set function %08X fn %08X time %g", task, fn, double(time));
    if (g_running && g_running->task == task) throw TaskReturn{time};
    LOGE("tasks: SetFunction on %08X outside its task", task);
    x87_push(ctx, double(time));
    ctx.esp += 4;
}

// Exit (0x1C5010): run the hook on the task's stack, then end the task; if it is still alive, destroy it.
// Outside the task's function the engine does the same and jumps back to the scheduler's return point, the walk's
// next task (sub_001C4DD0 after its Run): the walk catches that jump at its continuation (game.toml).
void ExitTask(X86Context& ctx) {
    uint32_t task = R32(kCurrentTask);
    bool alive = true;
    if (uint32_t hook = R32(task + 0x2C)) {
        kernel::CallGuest(ctx, hook, {});
        alive = R32(task + 0x04) != 0;
    }
    if (g_running && g_running->task == task) throw TaskExit{alive};
    const uint32_t ret = R32(kSchedRet);
    if (ret != kWalkResume) {
        LOGE("tasks: exit of %08X outside its function, the scheduler at %08X", task, ret);
        return;
    }
    // the first few in every log (the path that aborted the game before patch 0033: the walk resumed inside its handler)
    if (g_outside_exits++ < 4) LOGR("tasks: exit of %08X outside its function: back to the scheduler", task);
    const uint32_t sched_esp = R32(kSchedEsp);
    if (alive) Destroy(ctx, sched_esp);
    ctx.esp = sched_esp;
    ctx.ebp = R32(kSchedEbp);
    throw X86Resume{ret, sched_esp};
}

}  // namespace

namespace mk3d::tasks {

void Save(Snapshot& snap) {
    if (g_running || sfiber::Current() != -1) LOGE("tasks: snapshot while a task runs");
    sfiber::Save(snap.fibers);
    snap.tasks.clear();
    for (auto& [task, slot] : g_fibers) snap.tasks.push_back({task, slot});
    snap.records.resize(sizeof(TaskFiber) * sfiber::kSlots);
    std::memcpy(snap.records.data(), g_tf, snap.records.size());
}

void Load(const Snapshot& snap) {
    if (g_running || sfiber::Current() != -1) {
        LOGE("tasks: restore while a task runs");
        return;
    }
    sfiber::Load(snap.fibers);
    g_fibers.clear();
    for (auto& [task, slot] : snap.tasks) g_fibers.emplace(task, slot);
    std::memcpy(g_tf, snap.records.data(), snap.records.size());
}

int Alive() { return int(g_fibers.size()); }

}  // namespace mk3d::tasks

// ---- class A: copy-stack tasks ----
X86_FUNC(sub_001C51C0) { (void)base; RunTask(ctx, Kind::A); }
X86_FUNC(sub_001C5340) { (void)base; YieldTask(ctx, Kind::A); }
X86_FUNC(sub_001C5430) { (void)base; SetFunction(ctx); }

// ---- class B: dedicated-stack tasks ----
X86_FUNC(sub_001C5470) { (void)base; RunTask(ctx, Kind::B); }
X86_FUNC(sub_001C55A0) { (void)base; YieldTask(ctx, Kind::B); }
X86_FUNC(sub_001C5650) { (void)base; SetFunction(ctx); }

// ---- class C: simple tasks ----
X86_FUNC(sub_001C50A0) { (void)base; RunTask(ctx, Kind::C); }
X86_FUNC(sub_001C5180) { (void)base; SetFunction(ctx); }

// ---- any class ----
X86_FUNC(sub_001C5010) { (void)base; ExitTask(ctx); }

// 0x1C5610: continue the caller on the scheduler's stack, remembering ours; 0x1C5630: come back.
// With fibers the host frames do not care which guest stack is current, so these only move esp.
X86_FUNC(sub_001C5610) {
    (void)base;
    ctx.esp += 4;  // the return address
    W32(kHopEsp, ctx.esp);
    ctx.esp = R32(kSchedEsp);
}
X86_FUNC(sub_001C5630) {
    (void)base;
    ctx.esp = R32(kHopEsp);
    W32(kHopEsp, 0);
}
