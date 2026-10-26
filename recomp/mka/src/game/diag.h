#pragma once
#include <cstdint>

namespace mka {

// every thread's stack into the log (MKA_HANG_DUMP=<seconds>,... schedules it)
void DumpAllThreads(const char* why);

// GameInfo::message: logs how the window is asked to close (a close request from another program arrives as a bare
// WM_CLOSE, the title bar button and Alt+F4 as WM_SYSCOMMAND SC_CLOSE first); never takes a message
bool WindowMessage(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);

}  // namespace mka
