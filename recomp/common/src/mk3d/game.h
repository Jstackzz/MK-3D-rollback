#pragma once
// What every game layer provides to the shared code in recomp/common (netplay, rollback, the launcher link): its name
// for environment variables, its frame loop as hooks, code addresses described for diagnostics, and controller
// helpers. Each game implements these in its own src/game files (frame.cpp, diag.cpp, pads.cpp, game.cpp).
#include <cstddef>
#include <cstdint>
#include <string>

struct X86Context;

namespace mk3d {

// "MKD", "MKA", "MKDA": environment variables are <prefix>_NET_BIND, <prefix>_MONITOR and so on
const char* EnvPrefix();
// getenv of <prefix>_<name>, null when unset
const char* Env(const char* name);

// main loop iterations since boot
uint64_t Frame();
// the main thread's guest context while it runs a frame hook (null elsewhere); for calling game functions there
X86Context* MainContext();
using FrameListener = void (*)(uint64_t frame);
// at the top of every main loop iteration, before anything of that frame ran; on the main thread
void OnFrameStart(FrameListener f);
// right before the main loop's pad update, once everything else of the frame before it ran; on the main thread
void OnBeforePads(FrameListener f);
// right after the main loop's pad update wrote the game's per-port input blocks; on the main thread
void OnPadsRead(FrameListener f);

// a code address as module!symbol and the guest address when it is recompiled code (diag.cpp)
void DescribeCode(uintptr_t addr, char* out, size_t n);

// "A+B+LEFT" style names (A B X Y BLACK WHITE LT RT UP DOWN LEFT RIGHT START BACK LS RS) to digital buttons and the
// analog buttons pressed fully (bits of input::Pad::analog); false on an unknown name
bool ParseButtons(const std::string& names, uint16_t& buttons, uint8_t& analog);
// holds buttons on a port for a number of main loop frames from now (the debug monitor's "pad" command)
void InjectPad(int port, uint16_t buttons, uint8_t analog, uint64_t frames);

}  // namespace mk3d
