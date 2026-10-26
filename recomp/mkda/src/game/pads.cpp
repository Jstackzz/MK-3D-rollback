// Controller input seen by the game: the physical pads, plus scripted presses for tests, plus the autopilot, plus
// (later) netplay.
//
// MKDA_PADS=2           report a second controller even when only one is plugged in (idle unless scripted); the
//                       main menu offers VERSUS only with two controllers open
// MKDA_INPUT=<script>   scripted presses, comma separated entries  [p<port>@]<seconds>:<buttons>[:<hold seconds>]
//                       buttons joined with '+': A B X Y BLACK WHITE LT RT UP DOWN LEFT RIGHT START BACK LS RS
//                       e.g. MKDA_INPUT=8:START,10:A,12:DOWN,12.5:A,p2@14:A
//                       times count from the first controller read; holds default to 0.15 s
//                       (with MKDA_AUTOPILOT the times count from the first fight setup instead, so a script can
//                       attack once the autopilot has reached a fight)
#include "game/pads.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "core/log.h"
#include "game/autopilot.h"
#include "game/events.h"
#include "game/frame.h"
#include "kernel/kernel.h"
#include "mk3d/link.h"
#include "mk3d/netplay.h"

namespace mkda {
namespace {

struct Press {
    int port = 0;
    double at = 0, until = 0;
    uint16_t buttons = 0;
    uint8_t analog_mask = 0;  // bits of Pad::analog to press fully
};

std::once_flag g_once;
std::vector<Press> g_script;
int g_forced_pads = 0;
std::chrono::steady_clock::time_point g_t0;
PadFilter g_filter = nullptr;

// with the autopilot on, a script's times count from the first fight setup (set once it ran)
bool g_script_after_fight = false;
std::atomic<bool> g_fight_clock{false};
std::chrono::steady_clock::time_point g_fight_t0;

struct Injected {
    uint16_t buttons = 0;
    uint8_t analog = 0;
    uint64_t until = 0;  // main loop frame
};
std::mutex g_inject_lock;
Injected g_injected[4];

// While a match is in sync (netplay) no controller comes or goes for the game: each port's connection is latched at
// the first read in sync (the players' ports 0 and 1 connected), a pad pulled out meanwhile reads as connected and
// idle, one plugged in stays unseen. The pad update's insertion path (sub_00076B20 with 0: every action reads 0 for
// ten frames, 0x2CE764) and its removal path (sub_00076E90: the pause) would otherwise run on one game only.
std::mutex g_latch_lock;
bool g_latched[4] = {}, g_latch_connected[4] = {};

bool InSync() {
    mk3d::net::Status st = mk3d::net::GetStatus();
    return st == mk3d::net::Status::Barrier || st == mk3d::net::Status::Lockstep || st == mk3d::net::Status::Rollback;
}

void LatchConnection(int port, input::Pad& pad) {
    std::lock_guard<std::mutex> lock(g_latch_lock);
    int p = port & 3;
    if (!InSync()) {
        g_latched[p] = false;
        return;
    }
    if (!g_latched[p]) {
        g_latched[p] = true;
        g_latch_connected[p] = p < 2 || pad.connected;
    }
    if (!g_latch_connected[p]) pad = input::Pad{};
    else if (!pad.connected) {
        pad = input::Pad{};
        pad.connected = true;
    }
}

bool ParseButton(const std::string& name, uint16_t& buttons, uint8_t& analog) {
    static const struct {
        const char* name;
        uint16_t button;
        uint8_t analog;
    } kNames[] = {{"UP", 0x01, 0},     {"DOWN", 0x02, 0},  {"LEFT", 0x04, 0},   {"RIGHT", 0x08, 0},  {"START", 0x10, 0},
                  {"BACK", 0x20, 0},   {"LS", 0x40, 0},    {"RS", 0x80, 0},     {"A", 0, 0x01},      {"B", 0, 0x02},
                  {"X", 0, 0x04},      {"Y", 0, 0x08},     {"BLACK", 0, 0x10},  {"WHITE", 0, 0x20},  {"LT", 0, 0x40},
                  {"RT", 0, 0x80}};
    for (const auto& n : kNames)
        if (name == n.name) {
            buttons |= n.button;
            analog |= n.analog;
            return true;
        }
    return false;
}

void Load() {
    if (const char* n = std::getenv("MKDA_PADS")) g_forced_pads = std::atoi(n);
    const char* s = std::getenv("MKDA_INPUT");
    if (!s) return;
    std::string all(s);
    size_t pos = 0;
    while (pos < all.size()) {
        size_t end = all.find(',', pos);
        std::string entry = all.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? all.size() : end + 1;
        if (entry.empty()) continue;
        Press p;
        if (entry.size() > 2 && entry[0] == 'p' && entry[2] == '@') {
            p.port = entry[1] - '1';
            entry = entry.substr(3);
        }
        size_t c1 = entry.find(':');
        if (c1 == std::string::npos) continue;
        size_t c2 = entry.find(':', c1 + 1);
        p.at = std::atof(entry.substr(0, c1).c_str());
        std::string names = entry.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
        double hold = c2 == std::string::npos ? 0.15 : std::atof(entry.substr(c2 + 1).c_str());
        p.until = p.at + hold;
        size_t b = 0;
        bool ok = true;
        while (b <= names.size()) {
            size_t plus = names.find('+', b);
            std::string name = names.substr(b, plus == std::string::npos ? std::string::npos : plus - b);
            if (!name.empty() && !ParseButton(name, p.buttons, p.analog_mask)) {
                LOGE("pads: unknown button %s in MKDA_INPUT", name.c_str());
                ok = false;
            }
            if (plus == std::string::npos) break;
            b = plus + 1;
        }
        if (ok) g_script.push_back(p);
    }
    g_script_after_fight = autopilot::Active() && !g_script.empty();
    LOGI("pads: %zu scripted presses%s, %d forced pads", g_script.size(),
         g_script_after_fight ? " (timed from the first fight setup)" : "", g_forced_pads);
}

// seconds on the script's clock, or -1 while it has not started
double ScriptTime() {
    auto now = std::chrono::steady_clock::now();
    if (!g_script_after_fight) return std::chrono::duration<double>(now - g_t0).count();
    if (!g_fight_clock.load()) {
        if (FightSetups() == 0) return -1;
        static std::once_flag once;
        std::call_once(once, [now] {
            g_fight_t0 = now;
            g_fight_clock = true;
            LOGI("pads: the script starts now (first fight setup)");
        });
    }
    return std::chrono::duration<double>(now - g_fight_t0).count();
}

}  // namespace

void SetPadFilter(PadFilter f) { g_filter = f; }

bool ParseButtons(const std::string& names, uint16_t& buttons, uint8_t& analog) {
    size_t b = 0;
    bool ok = true;
    while (b <= names.size()) {
        size_t plus = names.find('+', b);
        std::string name = names.substr(b, plus == std::string::npos ? std::string::npos : plus - b);
        if (!name.empty() && !ParseButton(name, buttons, analog)) ok = false;
        if (plus == std::string::npos) break;
        b = plus + 1;
    }
    return ok;
}

void InjectPad(int port, uint16_t buttons, uint8_t analog, uint64_t frames) {
    if (port < 0 || port > 3) return;
    std::lock_guard<std::mutex> lock(g_inject_lock);
    g_injected[port] = {buttons, analog, Frame() + frames};
}

void PadHook(int port, input::Pad& pad) {
    std::call_once(g_once, [] {
        g_t0 = std::chrono::steady_clock::now();
        Load();
    });
    if (port < g_forced_pads) pad.connected = true;
    // USB enumeration delay: on the console, controllers that are already plugged in appear a moment after
    // XInitDevices. Reported at once, the game consumes the insertion before its input system initialises, which
    // then resets the port flags, and the pads are never opened (no button works). Hide them for a short while.
    static const double enumerate_after = [] {
        const char* s = std::getenv("MKDA_PAD_DELAY");
        return s ? std::atof(s) : 1.5;
    }();
    if (std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count() < enumerate_after) {
        pad = input::Pad{};
        return;
    }
    static std::atomic<uint32_t> reads{0};
    static const bool log_reads = std::getenv("MKDA_PAD_LOG") != nullptr;
    uint32_t n = ++reads;
    if (log_reads && (n <= 4 || n % 600 == 0 || (port == 0 && pad.buttons))) {
        double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count();
        // the game's own input (sub_00143030, Deadly Alliance Rev 1): the ports P1 and P2 play on (0x266B24 /
        // 0x266B28, -1 none), each player's logical buttons (P1 current 0x2CE774, new presses 0x2CE778; P2
        // 0x2CE780 / 0x2CE784), and XPP's gamepad device type record 0x22990C: current, changed, previous masks
        LOGI("pads: read %u, port %d, t %.1f, raw buttons %04X | P1 port %d cur %08X new %08X, P2 port %d cur %08X "
             "new %08X | XPP %X %X %X",
             n, port, t, pad.buttons, int32_t(kernel::R32(0x266B24)), kernel::R32(0x2CE774), kernel::R32(0x2CE778),
             int32_t(kernel::R32(0x266B28)), kernel::R32(0x2CE780), kernel::R32(0x2CE784), kernel::R32(0x22990C),
             kernel::R32(0x229910), kernel::R32(0x229914));
    }
    if (!g_script.empty()) {
        double t = ScriptTime();
        for (const Press& p : g_script) {
            if (t < 0 || p.port != port || t < p.at || t >= p.until) continue;
            pad.connected = true;
            pad.buttons |= p.buttons;
            for (int i = 0; i < 8; i++)
                if (p.analog_mask & (1u << i)) pad.analog[i] = 0xFF;
        }
    }
    // the launcher's autopilot drives the menus: the player's own controller reaches the game in the match only
    if (mk3d::link::SuppressPhysicalPads()) {
        pad.buttons = 0;
        for (auto& a : pad.analog) a = 0;
    }
    autopilot::Apply(port, pad);
    {
        std::lock_guard<std::mutex> lock(g_inject_lock);
        const Injected& in = g_injected[port & 3];
        if (Frame() < in.until) {
            pad.connected = true;
            pad.buttons |= in.buttons;
            for (int i = 0; i < 8; i++)
                if (in.analog & (1u << i)) pad.analog[i] = 0xFF;
        }
    }
    LatchConnection(port, pad);
    if (g_filter) g_filter(port, pad);
}

}  // namespace mkda

// the shared core's controller helpers (mk3d/game.h)
namespace mk3d {
bool ParseButtons(const std::string& names, uint16_t& buttons, uint8_t& analog) {
    return mkda::ParseButtons(names, buttons, analog);
}
void InjectPad(int port, uint16_t buttons, uint8_t analog, uint64_t frames) {
    mkda::InjectPad(port, buttons, analog, frames);
}
}  // namespace mk3d
