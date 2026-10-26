// Debug monitor (shared by the games): a line protocol on 127.0.0.1:<<prefix>_MONITOR> to look at and steer the
// running game (MKD_MONITOR, MKA_MONITOR, MKDA_MONITOR), for reverse
// engineering and netplay tests (tools/py/mkdbg.py is the client). One client at a time; every reply is one line.
//
//   frame                    main loop frame
//   r <addr> <len>           bytes as hex (len up to 64 KB)
//   r32 <addr> [n]           n dwords, rf <addr> [n] n floats
//   w <addr> <hex>           write bytes, w32 <addr> <value>, wf <addr> <float>
//   dump <path> [lo hi]      every committed page (default: user space, physical memory, system pool) to a file:
//                            "MKDUMP1" and a zero byte, then records of u32 va, u32 size, bytes
//   pause | resume           park the main loop at the top of its next frame / let it run
//   step <n>                 while paused: run n frames, park again, reply with the frame
//   wait <n>                 reply once n more frames have started
//   pad <port> <names> [n]   hold buttons ("A+LEFT") on a port (0-3) for n frames (default 6)
//   call <addr> [args...]    call a game function on the main thread at the top of the next frame (also while
//                            paused), cdecl/stdcall arguments; replies with eax
//   quit                     close this connection
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "core/log.h"
#include "mk3d/game.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace mk3d {
namespace {

std::mutex g_lock;
std::condition_variable g_cv;
struct Call {
    uint32_t fn = 0;
    std::vector<uint32_t> args;
    bool done = false;
    uint32_t result = 0;
};
std::vector<Call*> g_calls;  // waiting for the main thread
bool g_paused = false;
bool g_parked = false;
int64_t g_steps = 0;
uint64_t g_parked_frame = 0;

// runs the queued calls on the main thread; g_lock held
void RunCalls(std::unique_lock<std::mutex>& lock) {
    X86Context* ctx = MainContext();
    while (ctx && !g_calls.empty()) {
        Call* c = g_calls.front();
        g_calls.erase(g_calls.begin());
        lock.unlock();
        uint32_t r = kernel::CallGuest(*ctx, c->fn, c->args.data(), uint32_t(c->args.size()));
        lock.lock();
        c->result = r;
        c->done = true;
        g_cv.notify_all();
    }
}

void FrameStart(uint64_t frame) {
    std::unique_lock<std::mutex> lock(g_lock);
    RunCalls(lock);
    if (!g_paused) return;
    g_parked = true;
    g_parked_frame = frame;
    g_cv.notify_all();
    for (;;) {
        g_cv.wait(lock, [] { return !g_paused || g_steps > 0 || !g_calls.empty(); });
        RunCalls(lock);
        if (!g_paused || g_steps > 0) break;
    }
    if (g_paused && g_steps > 0) g_steps--;
    g_parked = false;
    g_cv.notify_all();
}

bool Readable(uint32_t va, uint32_t len) {
    for (uint64_t p = va & ~0xFFFull; p < uint64_t(va) + len; p += 0x1000)
        if (!mem::IsCommitted(uint32_t(p))) return false;
    return true;
}

uint32_t Num(const std::string& s) { return uint32_t(std::strtoul(s.c_str(), nullptr, 0)); }

std::string Hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; i++) {
        s[i * 2] = d[p[i] >> 4];
        s[i * 2 + 1] = d[p[i] & 15];
    }
    return s;
}

std::string Dump(const std::string& path, uint32_t lo, uint32_t hi) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return "error cannot open " + path;
    const char magic[8] = {'M', 'K', 'D', 'U', 'M', 'P', '1', 0};
    std::fwrite(magic, 1, 8, f);
    struct Range {
        uint64_t lo, hi;
    };
    std::vector<Range> ranges;
    if (lo || hi) {
        ranges.push_back({lo, hi});
    } else {
        ranges = {{mem::kUserLow, mem::kUserHigh},
                  {mem::kContiguousBase, uint64_t(mem::kContiguousBase) + mem::kPhysicalSize},
                  {mem::kSystemBase, uint64_t(mem::kSystemBase) + mem::kSystemSize}};
    }
    uint64_t total = 0, records = 0;
    for (const Range& r : ranges) {
        uint64_t p = r.lo & ~0xFFFull;
        while (p < r.hi) {
            if (!mem::IsCommitted(uint32_t(p))) {
                p += 0x1000;
                continue;
            }
            uint64_t q = p;
            while (q < r.hi && mem::IsCommitted(uint32_t(q))) q += 0x1000;
            uint32_t va = uint32_t(p), size = uint32_t(q - p);
            std::fwrite(&va, 4, 1, f);
            std::fwrite(&size, 4, 1, f);
            std::fwrite(mem::Base() + va, 1, size, f);
            total += size;
            records++;
            p = q;
        }
    }
    std::fclose(f);
    return "ok " + std::to_string(records) + " ranges " + std::to_string(total) + " bytes";
}

std::string Handle(const std::string& line, bool& quit) {
    std::istringstream in(line);
    std::vector<std::string> a;
    for (std::string w; in >> w;) a.push_back(w);
    if (a.empty()) return "error empty";
    const std::string& c = a[0];
    char buf[256];
    if (!mem::Base()) return "error the guest is not up yet";
    if (c == "frame") {
        std::snprintf(buf, sizeof buf, "frame %llu", (unsigned long long)Frame());
        return buf;
    }
    if (c == "r" && a.size() >= 3) {
        uint32_t va = Num(a[1]), n = std::min<uint32_t>(Num(a[2]), 65536);
        if (!Readable(va, n)) return "error not committed";
        return "ok " + Hex(mem::Base() + va, n);
    }
    if ((c == "r32" || c == "rf") && a.size() >= 2) {
        uint32_t va = Num(a[1]), n = a.size() > 2 ? std::min<uint32_t>(Num(a[2]), 4096) : 1;
        if (!Readable(va, n * 4)) return "error not committed";
        std::string s = "ok";
        for (uint32_t i = 0; i < n; i++) {
            uint32_t v = kernel::R32(va + i * 4);
            if (c == "r32") {
                std::snprintf(buf, sizeof buf, " %08X", v);
            } else {
                float fl;
                std::memcpy(&fl, &v, 4);
                std::snprintf(buf, sizeof buf, " %g", fl);
            }
            s += buf;
        }
        return s;
    }
    if (c == "w" && a.size() >= 3) {
        uint32_t va = Num(a[1]);
        const std::string& h = a[2];
        if (h.size() % 2 || !Readable(va, uint32_t(h.size() / 2))) return "error bad write";
        for (size_t i = 0; i < h.size() / 2; i++)
            mem::Base()[va + i] = uint8_t(std::strtoul(h.substr(i * 2, 2).c_str(), nullptr, 16));
        return "ok";
    }
    if ((c == "w32" || c == "wf") && a.size() >= 3) {
        uint32_t va = Num(a[1]);
        if (!Readable(va, 4)) return "error not committed";
        uint32_t v;
        if (c == "w32") {
            v = Num(a[2]);
        } else {
            float fl = std::strtof(a[2].c_str(), nullptr);
            std::memcpy(&v, &fl, 4);
        }
        kernel::W32(va, v);
        return "ok";
    }
    if (c == "dump" && a.size() >= 2) return Dump(a[1], a.size() > 3 ? Num(a[2]) : 0, a.size() > 3 ? Num(a[3]) : 0);
    if (c == "pause") {
        std::unique_lock<std::mutex> lock(g_lock);
        g_paused = true;
        g_steps = 0;
        if (!g_cv.wait_for(lock, std::chrono::seconds(5), [] { return g_parked; })) return "error main loop did not park";
        return "ok parked at " + std::to_string(g_parked_frame);
    }
    if (c == "resume") {
        std::lock_guard<std::mutex> lock(g_lock);
        g_paused = false;
        g_cv.notify_all();
        return "ok";
    }
    if (c == "step" && a.size() >= 2) {
        std::unique_lock<std::mutex> lock(g_lock);
        if (!g_paused) return "error not paused";
        uint64_t target = g_parked_frame + Num(a[1]);
        g_steps = Num(a[1]);
        g_cv.notify_all();
        if (!g_cv.wait_for(lock, std::chrono::seconds(30), [&] { return g_parked && g_parked_frame >= target; }))
            return "error step timed out";
        return "ok parked at " + std::to_string(g_parked_frame);
    }
    if (c == "wait" && a.size() >= 2) {
        uint64_t target = Frame() + Num(a[1]);
        auto t0 = std::chrono::steady_clock::now();
        while (Frame() < target) {
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(60)) return "error wait timed out";
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return "ok frame " + std::to_string(Frame());
    }
    if (c == "pad" && a.size() >= 3) {
        uint16_t buttons = 0;
        uint8_t analog = 0;
        if (!ParseButtons(a[2], buttons, analog)) return "error unknown button in " + a[2];
        InjectPad(int(Num(a[1])), buttons, analog, a.size() > 3 ? Num(a[3]) : 6);
        return "ok";
    }
    if (c == "call" && a.size() >= 2) {
        Call call;
        call.fn = Num(a[1]);
        for (size_t i = 2; i < a.size(); i++) call.args.push_back(Num(a[i]));
        std::unique_lock<std::mutex> lock(g_lock);
        g_calls.push_back(&call);
        g_cv.notify_all();
        if (!g_cv.wait_for(lock, std::chrono::seconds(30), [&] { return call.done; })) {
            g_calls.erase(std::remove(g_calls.begin(), g_calls.end(), &call), g_calls.end());
            return "error the main loop did not run the call";
        }
        std::snprintf(buf, sizeof buf, "ok %08X", call.result);
        return buf;
    }
    if (c == "quit") {
        quit = true;
        return "ok bye";
    }
    return "error unknown command " + c;
}

void Serve(uint16_t port) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (ls == INVALID_SOCKET || bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof addr) || listen(ls, 1)) {
        LOGE("monitor: cannot listen on 127.0.0.1:%u", port);
        return;
    }
    LOGI("monitor: listening on 127.0.0.1:%u", port);
    for (;;) {
        SOCKET s = accept(ls, nullptr, nullptr);
        if (s == INVALID_SOCKET) continue;
        std::string pending;
        char chunk[4096];
        bool quit = false;
        while (!quit) {
            int n = recv(s, chunk, sizeof chunk, 0);
            if (n <= 0) break;
            pending.append(chunk, size_t(n));
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                std::string reply = Handle(line, quit) + "\n";
                send(s, reply.data(), int(reply.size()), 0);
                if (quit) break;
            }
        }
        closesocket(s);
    }
}

const bool g_registered = [] {
    const char* p = Env("MONITOR");
    if (!p) return false;
    uint16_t port = uint16_t(std::atoi(p));
    if (!port) return false;
    OnFrameStart(&FrameStart);
    std::thread(Serve, port).detach();
    return true;
}();

}  // namespace
}  // namespace mk3d
