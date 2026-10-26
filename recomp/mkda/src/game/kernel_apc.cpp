// Kernel export 232, NtUserIoApcDispatcher, which the SDK runtime does not implement (boot warns "kernel import
// 232 has no implementation" and the engine's I/O worker thread traps on it 0.7 s after start).
//
// XAPI's ReadFileEx/WriteFileEx issue NtReadFile/NtWriteFile with this kernel routine as the APC and the caller's
// completion routine as the APC context. When the APC runs, the kernel unpacks the I/O status block and calls
// the completion routine the Win32 way:
//     void CALLBACK routine(DWORD error, DWORD transferred, LPOVERLAPPED overlapped)
// with the OVERLAPPED being the structure whose first two members are the status block (so its address is the
// status block's). Same shape as Cxbx-Reloaded's implementation (EmuKrnlNt.cpp, export 232).
//
// Deadly Alliance's file manager reads every file through ReadFileEx on the main thread (aio.cpp), so without it
// nothing loads. Generic enough to upstream into the SDK runtime.
#include <cstdint>

#include "kernel/exports.h"

namespace {

// NTSTATUS to Win32 error, the cases the runtime's own RtlNtStatusToDosError covers (that function is file-local).
uint32_t DosError(uint32_t status) {
    switch (status) {
        case 0x00000000: return 0;      // success
        case 0x00000103: return 997;    // pending
        case 0x00000102: return 1460;   // timeout
        case 0xC000000F:                // no such file
        case 0xC0000034: return 2;      // object name not found
        case 0xC000003A: return 3;      // object path not found
        case 0xC0000022:                // access denied
        case 0xC0000121:                // cannot delete
        case 0xC00000BA: return 5;      // file is a directory
        case 0xC0000008: return 6;      // invalid handle
        case 0xC0000017: return 8;      // no memory
        case 0x80000006: return 18;     // no more files
        case 0xC0000011: return 38;     // end of file
        case 0xC00000BB: return 50;     // not supported
        case 0xC000000D: return 87;     // invalid parameter
        case 0xC0000002: return 120;    // not implemented
        case 0xC0000023: return 122;    // buffer too small
        case 0xC0000033: return 123;    // object name invalid
        case 0xC0000035: return 183;    // object name collision
        case 0xC0000103: return 267;    // not a directory
        case 0xC000007F: return 112;    // disk full
        case 0xC000009D: return 1167;   // device not connected
        case 0xC0000120: return 995;    // cancelled
        default: return (status & 0x80000000u) ? 31 : 0;  // general failure for other errors, success for warnings
    }
}

}  // namespace

// stdcall, 3 arguments: ApcContext (the completion routine), IoStatusBlock, Reserved
KERNEL_EXPORT(232, NtUserIoApcDispatcher, 3) {
    uint32_t routine = a[0];
    uint32_t iosb = a[1];
    if (!routine || !iosb) return 0;
    uint32_t status = kernel::R32(iosb);
    uint32_t information = kernel::R32(iosb + 4);
    bool success = (status & 0x80000000u) == 0;  // NT_SUCCESS: informational and warning statuses count as success
    uint32_t error = success ? 0 : DosError(status);
    uint32_t transferred = success ? information : 0;
    kernel::CallGuest(ctx, routine, {error, transferred, iosb});
    return 0;
}
