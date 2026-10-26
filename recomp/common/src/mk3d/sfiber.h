#pragma once
// Snapshot-able fibers: cooperative contexts on stacks at fixed addresses in one reserved region, so the state of
// every parked fiber (its stack bytes and the registers saved on it) can be copied out and put back, which Windows
// fibers do not allow. The engine's task system runs on these (task_fibers.cpp) so rollback netplay can restore it.
//
// Windows x64: a switch saves the callee-saved registers, xmm6-15, MXCSR, the x87 control word and the thread's
// stack bounds (TEB StackBase, StackLimit, DeallocationStack, which the exception unwinder checks) on the old stack,
// stores the stack pointer in the old fiber's slot and loads the new one. C++ exceptions work inside a fiber as long
// as they are caught inside it.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sfiber {

constexpr int kSlots = 256;
constexpr size_t kStackSize = 1u << 20;  // committed per fiber

using Entry = void (*)(void* param);

// a fiber's slot index, -1 for none
int Create(Entry entry, void* param);
void Destroy(int fiber);
// run another fiber (-1: the thread's own stack, the "main fiber"); returns when someone switches back
void SwitchTo(int fiber);
int Current();  // -1 on the thread's own stack
bool Alive(int fiber);
int LiveCount();

// Every live fiber's parked state and the slot table. Only valid while no fiber runs (all parked, the thread on its
// own stack), which is the case at the top of the game's main loop.
struct Snapshot {
    std::vector<uint8_t> data;
};
void Save(Snapshot& s);
void Load(const Snapshot& s);
// bytes in use on every live fiber's stack right now (for sizing)
size_t UsedStackBytes();

}  // namespace sfiber
