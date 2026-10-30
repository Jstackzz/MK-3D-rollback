// Bring-up diagnostics: when a hardware exception hits game code, name the lifted function it happened in and the
// lifted callers above it, from the host stack (exact, unlike guessing return addresses on the guest stack).
// The code DLL exports every recompiled function as __imp__sub_XXXXXXXX, so the nearest export below the faulting
// address names the guest function; the offset can be fed to llvm-symbolizer with the debug build's PDB for the
// generated source line (tools/py/crash_locate.py does both).
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/log.h"
#include "game/frame.h"
#include "mk3d/game.h"
#include "game/rollback.h"
#include "kernel/memory.h"

namespace {

struct Export {
    uint32_t rva;
    const char* name;
};

struct Module {
    uintptr_t base = 0;
    size_t size = 0;
    char file[64] = {};
    std::vector<Export> exports;
};

std::mutex g_lock;
std::vector<Module> g_modules;
std::atomic<int> g_reports{0};
constexpr int kMaxReports = 6;

const Module* ModuleFor(uintptr_t addr) {
    for (const Module& m : g_modules)
        if (addr >= m.base && addr < m.base + m.size) return &m;
    HMODULE h = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(addr), &h))
        return nullptr;
    Module m;
    m.base = reinterpret_cast<uintptr_t>(h);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(m.base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(m.base + dos->e_lfanew);
    m.size = nt->OptionalHeader.SizeOfImage;
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(h, path, MAX_PATH);
    const wchar_t* slash = path;
    for (const wchar_t* p = path; *p; p++)
        if (*p == L'\\' || *p == L'/') slash = p + 1;
    std::snprintf(m.file, sizeof(m.file), "%ls", slash);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (dir.VirtualAddress) {
        auto* ed = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(m.base + dir.VirtualAddress);
        auto* names = reinterpret_cast<uint32_t*>(m.base + ed->AddressOfNames);
        auto* ords = reinterpret_cast<uint16_t*>(m.base + ed->AddressOfNameOrdinals);
        auto* funcs = reinterpret_cast<uint32_t*>(m.base + ed->AddressOfFunctions);
        m.exports.reserve(ed->NumberOfNames);
        for (DWORD i = 0; i < ed->NumberOfNames; i++)
            m.exports.push_back({funcs[ords[i]], reinterpret_cast<const char*>(m.base + names[i])});
        std::sort(m.exports.begin(), m.exports.end(), [](const Export& a, const Export& b) { return a.rva < b.rva; });
    }
    g_modules.push_back(std::move(m));
    return &g_modules.back();
}

// dbghelp names host frames from the PDBs; for generated code it also gives the source line, and the comment
// above that line names the guest instruction (every lifted instruction is preceded by "// ADDRESS mnemonic")
bool g_sym = false;
void SymStart() {
    if (g_sym) return;
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    // pdb-dll and pdb-exe: each module's copy of MKD.pdb (CMakeLists.txt keeps them; both link to that one name)
    char dir[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    if (char* slash = std::strrchr(dir, '\\')) *slash = 0;
    std::string search = std::string(dir) + "\\pdb-dll;" + dir + "\\pdb-exe;" + dir;
    g_sym = SymInitialize(GetCurrentProcess(), search.c_str(), TRUE) != FALSE;
}

struct SourceFile {
    std::string path;
    std::vector<std::string> lines;
};
std::vector<SourceFile> g_sources;

// the "// 00ABCDEF insn" comment at or above a generated source line
std::string GuestInstruction(const char* file, DWORD line) {
    SourceFile* sf = nullptr;
    for (SourceFile& s : g_sources)
        if (s.path == file) sf = &s;
    if (!sf) {
        FILE* f = std::fopen(file, "rb");
        if (!f) return {};
        g_sources.push_back({file, {}});
        sf = &g_sources.back();
        char buf[1024];
        while (std::fgets(buf, sizeof(buf), f)) sf->lines.emplace_back(buf);
        std::fclose(f);
    }
    for (DWORD i = line; i >= 1 && i + 400 > line; i--) {
        const std::string& l = sf->lines.size() >= i ? sf->lines[i - 1] : std::string();
        size_t c = l.find("// ");
        if (c != std::string::npos && l.size() > c + 11 && std::isxdigit(uint8_t(l[c + 3])) && l[c + 11] == ' ') {
            std::string insn = l.substr(c + 3);
            while (!insn.empty() && (insn.back() == '\n' || insn.back() == '\r')) insn.pop_back();
            return insn;
        }
    }
    return {};
}

void Describe(uintptr_t addr, char* out, size_t n) {
    const Module* m = ModuleFor(addr);
    if (!m) {
        std::snprintf(out, n, "%016llX", static_cast<unsigned long long>(addr));
        return;
    }
    uint32_t rva = uint32_t(addr - m->base);
    SymStart();
    if (g_sym) {
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
        auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (SymFromAddr(GetCurrentProcess(), addr, &disp, si)) {
            IMAGEHLP_LINE64 li{sizeof(li)};
            DWORD ldisp = 0;
            std::string guest;
            if (SymGetLineFromAddr64(GetCurrentProcess(), addr, &ldisp, &li) && std::strstr(li.FileName, "x86_recomp"))
                guest = GuestInstruction(li.FileName, li.LineNumber);
            if (!guest.empty())
                std::snprintf(out, n, "%s!%s  [guest %s]", m->file, si->Name, guest.c_str());
            else
                std::snprintf(out, n, "%s!%s+0x%llX", m->file, si->Name, static_cast<unsigned long long>(disp));
            return;
        }
    }
    auto it = std::upper_bound(m->exports.begin(), m->exports.end(), rva,
                               [](uint32_t v, const Export& e) { return v < e.rva; });
    if (it != m->exports.begin()) {
        --it;
        std::snprintf(out, n, "%s!%s+0x%X (rva %X)", m->file, it->name, rva - it->rva, rva);
    } else {
        std::snprintf(out, n, "%s+0x%X", m->file, rva);
    }
}

bool InGameCode(uintptr_t addr) {
    const Module* m = ModuleFor(addr);
    if (!m) return false;
    // MKD.exe holds the runtime and our overrides, MKD.dll the recompiled game
    return _strnicmp(m->file, "MKD", 3) == 0;
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t rip = ep->ContextRecord->Rip;
    std::lock_guard<std::mutex> lock(g_lock);
    if (!InGameCode(rip) || g_reports.load() >= kMaxReports) return EXCEPTION_CONTINUE_SEARCH;
    g_reports++;
    char where[256];
    Describe(rip, where, sizeof(where));
    LOGE("diag: exception %08lX in %s, thread %lu", code, where, GetCurrentThreadId());
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
        LOGE("diag:   %s %016llX", ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
             static_cast<unsigned long long>(ep->ExceptionRecord->ExceptionInformation[1]));
    CONTEXT c = *ep->ContextRecord;
    for (int depth = 0; depth < 40 && c.Rip; depth++) {
        DWORD64 image = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &image, nullptr);
        if (depth > 0) {
            Describe(uintptr_t(c.Rip), where, sizeof(where));
            LOGE("diag:   from %s", where);
        }
        if (!fe) {  // leaf: the return address is on top of the stack
            c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
            c.Rsp += 8;
            continue;
        }
        void* handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, fe, &c, &handler_data, &establisher, nullptr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// last in the chain: anything the runtime handles itself (it may use faults on purpose) never reaches here first
const bool g_installed = AddVectoredExceptionHandler(0, Handler) != nullptr;

// ---- all-thread stack dumps, for hangs ----

constexpr int kMaxThreads = 96, kMaxFrames = 40;
struct ThreadStack {
    DWORD tid = 0;
    int frames = 0;
    DWORD64 rip[kMaxFrames] = {};
};
ThreadStack g_stacks[kMaxThreads];

int Walk(CONTEXT c, DWORD64* out) {
    int n = 0;
    while (n < kMaxFrames && c.Rip) {
        out[n++] = c.Rip;
        DWORD64 image = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &image, nullptr);
        if (!fe) {
            c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
            c.Rsp += 8;
            continue;
        }
        void* handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, fe, &c, &handler_data, &establisher, nullptr);
    }
    return n;
}

}  // namespace

namespace mkd {

// names the lifted function (and guest instruction) a host code address is in
void DescribeCode(uintptr_t addr, char* out, size_t n) {
    std::lock_guard<std::mutex> lock(g_lock);
    Describe(addr, out, n);
}

}  // namespace mkd

namespace mk3d {
void DescribeCode(uintptr_t addr, char* out, size_t n) { mkd::DescribeCode(addr, out, n); }  // mk3d/game.h
}  // namespace mk3d

namespace mkd {

// every other thread is suspended only while its registers and return addresses are copied; naming and logging
// happen after all of them run again, so a thread holding the heap or log lock cannot deadlock the dump
void DumpAllThreads(const char* why) {
    static std::mutex dump_lock;
    std::lock_guard<std::mutex> guard(dump_lock);
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    std::vector<DWORD> tids;
    tids.reserve(kMaxThreads);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{sizeof(te)};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        if (te.th32OwnerProcessID == pid && te.th32ThreadID != self && tids.size() < kMaxThreads)
            tids.push_back(te.th32ThreadID);
    CloseHandle(snap);
    int count = 0;
    for (DWORD tid : tids) {
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
        if (!h) continue;
        if (SuspendThread(h) != DWORD(-1)) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_FULL;
            ThreadStack& s = g_stacks[count];
            s.tid = tid;
            s.frames = GetThreadContext(h, &c) ? Walk(c, s.rip) : 0;
            ResumeThread(h);
            count++;
        }
        CloseHandle(h);
    }
    std::lock_guard<std::mutex> lock(g_lock);
    LOGE("diag: thread dump (%s), %d threads", why, count);
    char where[256];
    for (int i = 0; i < count; i++) {
        const ThreadStack& s = g_stacks[i];
        // the frames worth reading: game code, plus the first frame wherever it is (what the thread waits in)
        LOGE("diag: thread t%04lx", s.tid & 0xFFFF);
        int shown = 0;
        for (int f = 0; f < s.frames && shown < 14; f++) {
            if (f > 0 && !InGameCode(uintptr_t(s.rip[f]))) continue;
            Describe(uintptr_t(s.rip[f]), where, sizeof(where));
            LOGE("diag:   %s", where);
            shown++;
        }
    }
}

namespace {
// MKD_HANG_DUMP=<seconds>[,<seconds>...]: dump every thread's stack at those times after start
const bool g_hang_dump = [] {
    const char* s = std::getenv("MKD_HANG_DUMP");
    if (!s) return false;
    std::vector<double> times;
    for (const char* p = s; *p;) {
        times.push_back(std::atof(p));
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
    }
    std::thread([times] {
        auto t0 = std::chrono::steady_clock::now();
        for (double t : times) {
            std::this_thread::sleep_until(t0 + std::chrono::duration<double>(t));
            char why[32];
            std::snprintf(why, sizeof(why), "at %.0f s", t);
            DumpAllThreads(why);
        }
    }).detach();
    return true;
}();
}  // namespace

}  // namespace mkd

namespace mk3d {
void DumpThreads(const char* why) { mkd::DumpAllThreads(why); }  // mk3d/game.h
}  // namespace mk3d

// ---- hardware write watchpoints, for finding what writes a guest address ----
namespace {

// MKD_WATCH=<va>[,<va>...] (up to 4 dword aligned addresses): a data breakpoint on the main thread for each. Every
// write to one logs the main loop frame, the new value and the lifted code that wrote it with its callers; only while
// a rollback is armed (sync tests) unless MKD_WATCH_ALL=1, at most MKD_WATCH_MAX (200) hits. Rollback's own restores
// are not logged.
uint32_t g_watch[4] = {};
int g_watch_n = 0, g_watch_max = 200;
bool g_watch_all = false;
std::atomic<int> g_watch_hits{0};

LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    DWORD64 which = c->Dr6 & 0xF;
    if (!which) return EXCEPTION_CONTINUE_SEARCH;
    c->Dr6 = 0;
    if (!(g_watch_all || mkd::rb::Armed()) || mkd::rb::Restoring() || g_watch_hits.load() >= g_watch_max)
        return EXCEPTION_CONTINUE_EXECUTION;
    g_watch_hits++;
    std::lock_guard<std::mutex> lock(g_lock);
    for (int i = 0; i < g_watch_n; i++) {
        if (!(which & (1ull << i))) continue;
        uint32_t v = 0;
        std::memcpy(&v, mem::Base() + g_watch[i], 4);
        char where[256];
        // a data breakpoint traps after the write: the instruction that wrote ends at rip
        Describe(uintptr_t(c->Rip - 1), where, sizeof(where));
        LOGI("watch: frame %llu: %08X = %08X by %s", (unsigned long long)mkd::Frame(), g_watch[i], v, where);
        DWORD64 pcs[kMaxFrames];
        int n = Walk(*c, pcs), shown = 0;
        for (int f = 1; f < n && shown < 6; f++) {
            if (!InGameCode(uintptr_t(pcs[f]))) continue;
            Describe(uintptr_t(pcs[f]), where, sizeof(where));
            LOGI("watch:   from %s", where);
            shown++;
        }
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

// on the main thread at its first frame: a helper thread sets the debug registers while the main thread waits
void ArmWatch(uint64_t) {
    static bool done = false;
    if (done) return;
    done = true;
    DWORD tid = GetCurrentThreadId();
    bool ok = false;
    std::thread([tid, &ok] {
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, tid);
        if (!h) return;
        if (SuspendThread(h) != DWORD(-1)) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(h, &c)) {
                DWORD64* dr[4] = {&c.Dr0, &c.Dr1, &c.Dr2, &c.Dr3};
                DWORD64 dr7 = 0;
                for (int i = 0; i < g_watch_n; i++) {
                    *dr[i] = DWORD64(uintptr_t(mem::Base() + g_watch[i]));
                    dr7 |= 1ull << (2 * i);       // local enable
                    dr7 |= 1ull << (16 + 4 * i);  // R/W 01: data writes
                    dr7 |= 3ull << (18 + 4 * i);  // LEN 11: 4 bytes
                }
                c.Dr7 = dr7;
                c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                ok = SetThreadContext(h, &c) != FALSE;
            }
            ResumeThread(h);
        }
        CloseHandle(h);
    }).join();
    LOGI("watch: %d write watchpoints on the main thread%s", g_watch_n, ok ? "" : " FAILED");
}

const bool g_watch_on = [] {
    const char* s = std::getenv("MKD_WATCH");
    if (!s || !*s) return false;
    for (const char* p = s; *p && g_watch_n < 4;) {
        g_watch[g_watch_n++] = uint32_t(std::strtoul(p, nullptr, 16)) & ~3u;
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
    }
    if (const char* m = std::getenv("MKD_WATCH_MAX")) g_watch_max = std::atoi(m);
    g_watch_all = std::getenv("MKD_WATCH_ALL") != nullptr;
    AddVectoredExceptionHandler(1, WatchHandler);
    mkd::OnFrameStart(&ArmWatch);
    return true;
}();

}  // namespace
