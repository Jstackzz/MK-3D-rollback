// Controller input seen by the game: the physical pads, plus scripted presses for tests, the autopilot
// (autopilot.cpp), plus (later) netplay.
//
// MKA_PADS=2            report a second controller even when only one is plugged in (idle unless scripted)
// MKA_INPUT=<script>    scripted presses, comma separated entries  [p<port>@]<seconds>:<buttons>[:<hold seconds>]
//                       buttons joined with '+': A B X Y BLACK WHITE LT RT UP DOWN LEFT RIGHT START BACK LS RS
//                       e.g. MKA_INPUT=8:START,10:A,12:DOWN,12.5:A,p2@14:A
//                       times count from the first controller read; holds default to 0.15 s
//                       (with MKA_AUTOPILOT the times count from when it is done instead, the first fight setup or
//                       the mode it went to, so a script can attack once the autopilot has reached a fight)
#include "game/pads.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "core/log.h"
#include "game/autopilot.h"
#include "game/frame.h"
#include "game/launcher_link.h"
#include "kernel/kernel.h"

namespace mka {
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

// with the autopilot on, a script's times count from when it is done (set once it ran)
bool g_script_after_autopilot = false;
std::atomic<bool> g_script_clock{false};
std::chrono::steady_clock::time_point g_script_t0;

struct Injected {
    uint16_t buttons = 0;
    uint8_t analog = 0;
    uint64_t until = 0;  // main loop frame
};
std::mutex g_inject_lock;
Injected g_injected[4];

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
    if (const char* n = std::getenv("MKA_PADS")) g_forced_pads = std::atoi(n);
    // the launcher's matches need player 2's controller: the other player's online, the autopilot's on the select
    // screen; with none plugged in the game stops after the picks ("controller disconnected", port 2)
    if (link::Active()) g_forced_pads = std::max(g_forced_pads, 2);
    const char* s = std::getenv("MKA_INPUT");
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
                LOGE("pads: unknown button %s in MKA_INPUT", name.c_str());
                ok = false;
            }
            if (plus == std::string::npos) break;
            b = plus + 1;
        }
        if (ok) g_script.push_back(p);
    }
    g_script_after_autopilot = autopilot::Active() && !g_script.empty();
    LOGI("pads: %zu scripted presses%s, %d forced pads", g_script.size(),
         g_script_after_autopilot ? " (timed from when the autopilot is done)" : "", g_forced_pads);
}

// seconds on the script's clock, or -1 while it has not started
double ScriptTime() {
    auto now = std::chrono::steady_clock::now();
    if (!g_script_after_autopilot) return std::chrono::duration<double>(now - g_t0).count();
    if (!g_script_clock.load()) {
        // or when it hands this PC's player the cursor on the select screen (an online match whose players pick
        // there: the script is that player)
        if ((autopilot::Active() || autopilot::AtHoldPoint()) && autopilot::LocalPickPort() < 0) return -1;
        static std::once_flag once;
        std::call_once(once, [now] {
            g_script_t0 = now;
            g_script_clock = true;
            LOGI("pads: the script starts now (%s)", autopilot::Active() ? "this player picks" : "the autopilot is done");
        });
    }
    return std::chrono::duration<double>(now - g_script_t0).count();
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
        const char* s = std::getenv("MKA_PAD_DELAY");
        return s ? std::atof(s) : 1.5;
    }();
    if (std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count() < enumerate_after) {
        pad = input::Pad{};
        return;
    }
    static std::atomic<uint32_t> reads{0};
    static const bool log_reads = std::getenv("MKA_PAD_LOG") != nullptr;
    uint32_t n = ++reads;
    if (log_reads && (n <= 4 || n % 600 == 0 || (port == 0 && pad.buttons))) {
        double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count();
        // the game's own per-port input block (frame.h): flags, current logical buttons, new presses
        uint32_t b = kPortBlock + uint32_t(port) * kPortStride;
        LOGI("pads: read %u, port %d, t %.1f, raw buttons %04X | game flags %X cur %08X edge %08X | XPP %X %X %X", n,
             port, t, pad.buttons, kernel::R32(b + kPortFlags), kernel::R32(b + kPortCurrent),
             kernel::R32(b + kPortPressed), kernel::R32(0x4CE334), kernel::R32(0x4CE338), kernel::R32(0x4CE33C));
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
    // the launcher's autopilot drives the menus: the player's own controller reaches the game in the match only, and
    // on the select screen of an online match whose players pick there, where it steers their own side's cursor: the
    // controller on port 0 (the keyboard's too) goes to port 1 when they are P2
    const int pick_port = autopilot::LocalPickPort();
    if (pick_port >= 0) {
        static input::Pad player;
        if (port == 0) player = pad;
        const bool connected = pad.connected;
        pad = port == pick_port ? player : input::Pad{};
        pad.analog[1] = 0;      // B would leave the select screen (and the match with it)
        pad.buttons &= ~0x20u;  // as BACK would
        pad.connected = connected || port == pick_port;
    } else if (link::SuppressPhysicalPads()) {
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
    if (g_filter) g_filter(port, pad);
}

}  // namespace mka

// the shared core's controller helpers (mk3d/game.h)
namespace mk3d {
bool ParseButtons(const std::string& names, uint16_t& buttons, uint8_t& analog) {
    return mka::ParseButtons(names, buttons, analog);
}
void InjectPad(int port, uint16_t buttons, uint8_t analog, uint64_t frames) {
    mka::InjectPad(port, buttons, analog, frames);
}
}  // namespace mk3d
