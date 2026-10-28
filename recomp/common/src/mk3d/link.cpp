// The game's side of the launcher protocol (docs/PROTOCOL.md section 3), shared by the games: with MK3D_NET=1 the
// launcher owns this process. The game boots to the main menu and waits there (the hold point, "ready"), takes
// commands from cmd.txt (claimed by renaming it to cmd.txt.taken) and reports its state in status.txt (written to a
// .tmp file and moved over it).
//
//   start    the autopilot drives the front end to the mode and both players' picks (linkgame.h Begin); for an online
//            match netplay meets the other game at the match's first frame (netplay.cpp) and the status turns in_match
//   result   once the game names a winner from what both games agree on (linkgame.h Outcome)
//   park     back to the main menu through the game's own state switch, run on the main thread at the top of a frame
//   rematch  the next match with the same picks
//   quit     exit
//
// While the launcher drives the game the player's controller only reaches the game during the match itself, so it
// cannot wander off the hold point or disturb the autopilot.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "mk3d/link.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#include "app/app.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "mk3d/framedata.h"
#include "mk3d/game.h"
#include "mk3d/linkgame.h"
#include "mk3d/netplay.h"

namespace mk3d::link {
namespace {

std::wstring g_cmd, g_status;
bool g_on = false;

std::mutex g_lock;
std::string g_last_status;
std::string g_match = "none";
game::Start g_start;
bool g_in_match = false;      // a start was taken and not parked
bool g_online = false;
bool g_fighting = false;      // the match runs (in sync online, set up offline)
bool g_reported = false;      // result / desync / disconnected written for this match
uint64_t g_started_base = 0;  // matches started before this one (offline)
int g_wins_needed = 2;
std::atomic<bool> g_park{false};
bool g_park_after_end = false;  // asked to park while the match's end still plays in sync
std::chrono::steady_clock::time_point g_park_deadline;

std::wstring Widen(const char* s) {
    std::wstring w;
    for (; *s; s++) w += wchar_t(static_cast<unsigned char>(*s));
    return w;
}

void WriteStatus(const std::string& line) {
    if (line == g_last_status) return;
    g_last_status = line;
    std::wstring tmp = g_status + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return;
    std::string text = line + "\n";
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    for (int i = 0; i < 200; i++) {
        if (MoveFileExW(tmp.c_str(), g_status.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) break;
        Sleep(5);
    }
    LOGI("link: status %s", line.c_str());
}

bool ClaimCommand(std::string& out) {
    std::wstring taken = g_cmd + L".taken";
    if (!MoveFileExW(g_cmd.c_str(), taken.c_str(), MOVEFILE_REPLACE_EXISTING)) return false;
    FILE* f = _wfopen(taken.c_str(), L"rb");
    if (!f) return false;
    char buf[2048];
    size_t n = std::fread(buf, 1, sizeof buf - 1, f);
    std::fclose(f);
    buf[n] = 0;
    out = buf;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
    return true;
}

struct Command {
    std::string verb;
    std::map<std::string, std::string> keys;
};

Command Parse(const std::string& line) {
    Command c;
    std::istringstream in(line);
    in >> c.verb;
    for (std::string tok; in >> tok;) {
        size_t eq = tok.find('=');
        if (eq != std::string::npos) c.keys[tok.substr(0, eq)] = tok.substr(eq + 1);
    }
    return c;
}

std::string Key(const Command& c, const char* k, const char* def = "") {
    auto it = c.keys.find(k);
    return it == c.keys.end() ? def : it->second;
}

void Start(const Command& c) {
    std::string mode = Key(c, "mode", "kombat");
    if (!game::ModeKnown(mode)) {
        WriteStatus("error bad command mode " + mode);
        return;
    }
    if (!game::AtHoldPoint()) {
        WriteStatus("error not at the hold point");
        return;
    }
    std::string picks[2] = {Key(c, "p1"), Key(c, "p2")};
    for (std::string& p : picks)
        if (p == "-") p.clear();
    int rounds = std::atoi(Key(c, "rounds", "3").c_str());
    g_wins_needed = rounds > 1 ? rounds / 2 + 1 : 1;
    g_match = Key(c, "match", mode == "practice" ? "practice" : "none");
    // watch=1: a spectator, both players' inputs from the launcher's feed (no remote); a player's game writes to it
    const bool watch = Key(c, "watch") == "1" && !Key(c, "feed").empty();
    g_online = (!Key(c, "remote").empty() || watch) && mode != "practice";
    const int side = Key(c, "side", "1") == "2" ? 1 : 0;
    if (g_online) {
        net::Settings st;
        st.watch = watch;
        st.feed = Key(c, "feed");
        st.bind = uint16_t(std::atoi(Key(c, "local_port", "7001").c_str()));
        st.peer = Key(c, "remote");
        st.side = side;
        st.delay = std::atoi(Key(c, "delay", "2").c_str());
        st.loopback_only = st.peer.rfind("127.", 0) == 0;
        // tests: a simulated network on what this game sends, as for a game started by hand (netplay.cpp Load)
        if (const char* v = Env("NET_LAG")) st.lag_ms = std::atoi(v);
        if (const char* v = Env("NET_JITTER")) st.jitter_ms = std::atoi(v);
        if (const char* v = Env("NET_LOSS")) st.loss_percent = std::atoi(v);
        if (const char* log = Env("NET_LOG")) {  // "{side}" becomes 0, 1 or w (games on one PC), "{match}" the id
            st.log = log;
            size_t at = st.log.find("{side}");
            if (at != std::string::npos) st.log.replace(at, 6, watch ? std::string("w") : std::to_string(st.side));
            at = st.log.find("{match}");
            if (at != std::string::npos) st.log.replace(at, 7, Key(c, "match", "none"));
        }
        net::Configure(st);
    } else {
        net::Disable();
    }
    game::Start start;
    start.mode = mode;
    start.picks[0] = picks[0];
    start.picks[1] = picks[1];
    start.seed = uint32_t(std::strtoul(Key(c, "seed", "0").c_str(), nullptr, 10));
    start.stage = Key(c, "stage");
    start.wins_needed = g_wins_needed;
    start.death_traps = Key(c, "traps", "1") != "0";
    start.online = g_online;
    start.side = side;
    start.watch = watch;
    g_start = start;
    g_in_match = true;
    g_fighting = false;
    g_reported = false;
    g_started_base = game::MatchesStarted();
    game::MatchBegins(mode);
    if (!game::Begin(start)) {
        WriteStatus("error the autopilot did not take the start");
        g_in_match = false;
        return;
    }
    LOGR("link: start %s (%s vs %s), match %s, %s%s", mode.c_str(), picks[0].c_str(), picks[1].c_str(),
         g_match.c_str(), watch ? "watching" : g_online ? "online" : "offline",
         start.death_traps ? "" : ", no death traps");
    // off the hold point from now on: a launcher still reading "ready" until the fight began would send its next
    // start (a spectator's next match) into this one
    WriteStatus("starting match=" + g_match);
}

void Rematch(const Command& c) {
    if (!g_in_match) {
        WriteStatus("error no match to repeat");
        return;
    }
    std::string m = Key(c, "match");
    if (!m.empty()) g_match = m;
    std::string seed = Key(c, "seed");
    if (!seed.empty()) g_start.seed = uint32_t(std::strtoul(seed.c_str(), nullptr, 10));
    g_fighting = false;
    g_reported = false;
    g_started_base = game::MatchesStarted();
    game::MatchBegins(g_start.mode);
    game::Rematch(g_start);
    LOGI("link: rematch, match %s", g_match.c_str());
}

void Park() {
    g_park_after_end = false;
    g_in_match = false;
    g_fighting = false;
    net::Disable();
    game::ExpectMenu();
    g_park = true;  // the frame hook switches the game to the main menu
}

bool InSync() {
    const net::Status ns = net::GetStatus();
    return ns == net::Status::Lockstep || ns == net::Status::Rollback;
}

// The result goes out at the round that decides the match, and the launcher parks once both players reported it,
// while the game still plays the match's end: the finisher, a fatality, the winner's pose. Both games leave the match
// in sync together after that (netplay.cpp, MatchOver), so the park waits for it, and for 30 s at most.
void RequestPark() {
    if (g_in_match && g_online && g_reported && InSync()) {
        g_park_after_end = true;
        g_park_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        LOGI("link: park once the match's end has played");
        return;
    }
    Park();
}

// the match's state, written when it changes
void Update() {
    if (g_park_after_end) {
        if (!InSync() || std::chrono::steady_clock::now() > g_park_deadline) {
            LOGI("link: the match's end has played");
            Park();
        }
        return;
    }
    if (!g_in_match) {
        if (game::AtHoldPoint()) WriteStatus("ready");
        return;
    }
    net::Status ns = g_online ? net::GetStatus() : net::Status::Off;
    if (!g_reported && g_online && ns == net::Status::Desynced) {
        WriteStatus("desync frame=" + std::to_string(net::DesyncFrame()) + " match=" + g_match);
        g_reported = true;
        return;
    }
    if (!g_reported && g_online && ns == net::Status::Disconnected) {
        WriteStatus("disconnected match=" + g_match);
        g_reported = true;
        return;
    }
    if (!g_fighting) {
        bool started = g_online ? (ns == net::Status::Lockstep || ns == net::Status::Rollback)
                                : game::MatchesStarted() > g_started_base;
        if (started) {
            g_fighting = true;
            WriteStatus("in_match match=" + g_match);
        }
        return;
    }
    if (g_reported || g_match == "practice") return;
    // online, only what both games agree on: with rollback the present frame may rest on a predicted input
    net::Settled s;
    if (g_online) {
        if (!net::GetSettled(s)) return;
    } else {
        game::SettleNow(s);
    }
    int winner = 0;
    uint32_t r1 = 0, r2 = 0;
    if (game::Outcome(g_start.mode, s, g_wins_needed, winner, r1, r2) && (winner == 1 || winner == 2)) {
        std::string fighters;
        int chars[2] = {-1, -1};
        if (game::Fighters(g_start.mode, chars) && chars[0] >= 0 && chars[1] >= 0)
            fighters = " p1_char=" + std::to_string(chars[0]) + " p2_char=" + std::to_string(chars[1]);
        WriteStatus("result winner=" + std::to_string(winner) + " p1_rounds=" + std::to_string(r1) +
                    " p2_rounds=" + std::to_string(r2) + fighters + " match=" + g_match);
        g_reported = true;
    }
}

void Loop() {
    WriteStatus("booting");
    for (;;) {
        std::string line;
        if (ClaimCommand(line)) {
            std::lock_guard<std::mutex> lock(g_lock);
            LOGI("link: command %s", line.c_str());
            Command c = Parse(line);
            if (c.verb == "start") Start(c);
            else if (c.verb == "park") RequestPark();
            else if (c.verb == "rematch") Rematch(c);
            else if (c.verb == "quit") {
                WriteStatus("quitting");
                ExitProcess(0);
            } else WriteStatus("error bad command " + c.verb);
        }
        {
            std::lock_guard<std::mutex> lock(g_lock);
            Update();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void FrameStart(uint64_t) {
    if (!g_park.exchange(false)) return;
    X86Context* ctx = MainContext();
    if (!ctx) return;
    LOGR("link: back to the main menu");
    game::Park(*ctx);
}

const bool g_registered = [] {
    const char* net = std::getenv("MK3D_NET");
    const char* cmd = std::getenv("MK3D_CMD");
    const char* status = std::getenv("MK3D_STATUS");
    if (!net || std::strcmp(net, "1") || !cmd || !status) return false;
    g_cmd = Widen(cmd);
    g_status = Widen(status);
    g_on = true;
    game::WaitForRequest();
    OnFrameStart(&FrameStart);
    std::thread(Loop).detach();
    return true;
}();

}  // namespace

bool Active() { return g_on; }

bool SuppressPhysicalPads() {
    if (!g_on) return false;
    // the player's controller reaches the game only while the match runs, its end included: online the result goes
    // out at the round that decides it, and the finisher, a fatality and the winner's pose still play in sync after
    // (and on the select screen, for their own side's cursor when they pick there: the game's pads route it,
    // autopilot LocalPickPort); a spectator's never
    if (g_in_match && g_start.watch) return true;
    return !(g_in_match && g_fighting && (!g_reported || InSync())) || game::AutopilotActive();
}

namespace {

// The display as the launcher's Settings have it: from the environment at start, from its display file while the game
// runs. A field the launcher did not give stays empty.
struct Display {
    std::optional<app::DisplayMode> mode;
    std::optional<std::pair<uint32_t, uint32_t>> size;  // 0x0: the monitor's own
    std::optional<bool> vsync, adaptive_sync, stretch_4x3, frame_data;
    std::optional<uint32_t> brightness;  // percent, 100 as the game draws it
    bool Any() const { return mode || size || vsync || adaptive_sync || stretch_4x3 || frame_data || brightness; }
};

// one setting by its display file key (the launcher's names, as in its settings file)
void Take(Display& d, const std::string& key, const std::string& v) {
    if (key == "mode") {
        if (v == "windowed") d.mode = app::DisplayMode::Windowed;
        else if (v == "borderless") d.mode = app::DisplayMode::Borderless;
        else if (v == "fullscreen") d.mode = app::DisplayMode::Fullscreen;
        else LOGR("display: %s is no display mode", v.c_str());
    } else if (key == "resolution") {
        char* end = nullptr;
        const unsigned long w = std::strtoul(v.c_str(), &end, 10);
        const unsigned long h = (*end == 'x' || *end == 'X') ? std::strtoul(end + 1, &end, 10) : 0;
        if (v == "desktop") d.size = std::pair<uint32_t, uint32_t>(0, 0);
        else if (!*end && w >= 320 && h >= 240 && w <= 16384 && h <= 16384) d.size = std::pair(uint32_t(w), uint32_t(h));
        else LOGR("display: %s is no resolution", v.c_str());
    } else if (key == "vsync") {
        d.vsync = std::atoi(v.c_str()) != 0;
    } else if (key == "adaptive_sync") {
        d.adaptive_sync = std::atoi(v.c_str()) != 0;
    } else if (key == "stretch_4x3") {  // a 4:3 game (Deadly Alliance): fill the screen or not
        d.stretch_4x3 = std::atoi(v.c_str()) != 0;
    } else if (key == "brightness") {
        d.brightness = uint32_t(std::clamp(std::atoi(v.c_str()), 50, 200));
    } else if (key == "frame_data") {
        d.frame_data = std::atoi(v.c_str()) != 0;
    }
}

// key=value lines; the launcher writes the file whole and moves it over the last one, so it is never half written (and
// it is read sharing everything, so that move never fails because of this read: docs/PROTOCOL.md 3.2)
Display ReadDisplayFile(const std::wstring& path) {
    Display d;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return d;
    char buf[1024];
    DWORD n = 0;
    const bool read = ReadFile(f, buf, sizeof buf - 1, &n, nullptr);
    CloseHandle(f);
    if (!read) return d;
    std::istringstream text(std::string(buf, n));
    for (std::string line; std::getline(text, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const size_t eq = line.find('=');
        if (eq != std::string::npos) Take(d, line.substr(0, eq), line.substr(eq + 1));
    }
    return d;
}

// A window's size: as asked when it fits on its monitor's work area with its frame, else the largest of the same shape
// that does (the monitor's own size, 0x0, never fits: a window as large as fits on it then)
std::pair<uint32_t, uint32_t> FitWindow(uint32_t w, uint32_t h) {
    if (!w || !h) {
        const app::Mode desk = app::Desktop();
        w = desk.width;
        h = desk.height;
    }
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(static_cast<HWND>(app::Window()), MONITOR_DEFAULTTOPRIMARY), &mi))
        return {w, h};
    RECT frame{0, 0, 0, 0};
    AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);  // as the window gets it (runtime app/main.cpp ApplyNow)
    const long room_w = (mi.rcWork.right - mi.rcWork.left) - (frame.right - frame.left);
    const long room_h = (mi.rcWork.bottom - mi.rcWork.top) - (frame.bottom - frame.top);
    if (room_w < 320 || room_h < 240 || (long(w) <= room_w && long(h) <= room_h)) return {w, h};
    const double scale = std::min(double(room_w) / w, double(room_h) / h);
    return {std::max(320u, uint32_t(w * scale)), std::max(240u, uint32_t(h * scale))};
}

const char* ModeName(app::DisplayMode m) {
    return m == app::DisplayMode::Windowed ? "windowed" : m == app::DisplayMode::Fullscreen ? "fullscreen" : "borderless";
}

// The window's mode and size into the settings; the window applies them itself (at start, or app::ApplyDisplay)
void SetWindow(app::DisplayMode mode, std::pair<uint32_t, uint32_t> size) {
    app::Settings& s = app::Get();
    if (mode == app::DisplayMode::Windowed) size = FitWindow(size.first, size.second);
    s.display_mode = mode;
    s.width = size.first;
    s.height = size.second;
}

// The launcher's Settings while the game runs (MK3D_DISPLAY_FILE, written again at each change): vsync, brightness,
// the 4:3 stretch and frame data change at once; the window's mode and size the next time the game is in front
// (app::ApplyDisplayWhenActive), so a change made in the launcher never pulls the game over it, nor switches the
// display's mode behind it. `last`: what the game took at start.
void WatchDisplayFile(std::wstring path, Display last) {
    std::thread([path = std::move(path), last]() mutable {
        FILETIME seen{};
        for (;;) {
            WIN32_FILE_ATTRIBUTE_DATA a{};
            if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) &&
                CompareFileTime(&a.ftLastWriteTime, &seen) != 0) {
                seen = a.ftLastWriteTime;
                const Display d = ReadDisplayFile(path);
                app::Settings& s = app::Get();
                if (d.vsync && d.vsync != last.vsync) {
                    s.vsync = *d.vsync;
                    LOGR("display: vsync %s, from the launcher", s.vsync ? "on" : "off");
                }
                if (d.adaptive_sync && d.adaptive_sync != last.adaptive_sync) {
                    s.adaptive_sync = *d.adaptive_sync;
                    LOGR("display: G-SYNC or FreeSync %s, from the launcher", s.adaptive_sync ? "on" : "off");
                }
                if (d.brightness && d.brightness != last.brightness) s.brightness = *d.brightness;
                if (d.stretch_4x3 && d.stretch_4x3 != last.stretch_4x3) s.stretch_4x3 = *d.stretch_4x3;
                if (d.frame_data && d.frame_data != last.frame_data) framedata::SetShown(*d.frame_data);
                if ((d.mode && d.mode != last.mode) || (d.size && d.size != last.size)) {
                    SetWindow(d.mode.value_or(last.mode.value_or(s.display_mode)),
                              d.size.value_or(last.size.value_or(std::pair(s.width, s.height))));
                    LOGR("display: %s %ux%u, from the launcher", ModeName(s.display_mode), s.width, s.height);
                    app::ApplyDisplayWhenActive();
                }
                if (d.vsync) last.vsync = d.vsync;
                if (d.adaptive_sync) last.adaptive_sync = d.adaptive_sync;
                if (d.brightness) last.brightness = d.brightness;
                if (d.stretch_4x3) last.stretch_4x3 = d.stretch_4x3;
                if (d.frame_data) last.frame_data = d.frame_data;
                if (d.mode) last.mode = d.mode;
                if (d.size) last.size = d.size;
            }
            Sleep(500);
        }
    }).detach();
}

}  // namespace

void ApplyLauncherDisplay() {
    // a game the launcher started logs into its session folder (MK3D_LOG), beside the session's other files, not into
    // the logs folder that two games of one PC would write into at once; a relaunch into another image carries on
    if (const char* log = std::getenv("MK3D_LOG"); log && *log) {
        const std::filesystem::path p(log);
        logx::Init(p.parent_path().string(), p.stem().string(), std::wcsstr(GetCommandLineW(), L"--relaunched") != nullptr);
    }
    Display d;
    for (const auto& [key, var] : {std::pair("mode", "MK3D_DISPLAY"), std::pair("resolution", "MK3D_RESOLUTION"),
                                   std::pair("vsync", "MK3D_VSYNC"), std::pair("adaptive_sync", "MK3D_ADAPTIVE_SYNC"),
                                   std::pair("stretch_4x3", "MK3D_STRETCH_4X3"), std::pair("brightness", "MK3D_BRIGHTNESS")})
        if (const char* v = std::getenv(var)) Take(d, key, v);
    if (!d.Any()) return;  // started by hand: the settings file alone
    app::Settings& s = app::Get();
    SetWindow(d.mode.value_or(s.display_mode), d.size.value_or(std::pair(s.width, s.height)));
    if (d.vsync) s.vsync = *d.vsync;
    if (d.adaptive_sync) s.adaptive_sync = *d.adaptive_sync;
    if (d.stretch_4x3) s.stretch_4x3 = *d.stretch_4x3;
    if (d.brightness) s.brightness = *d.brightness;
    if (const char* v = std::getenv("MK3D_FRAMEDATA")) d.frame_data = std::atoi(v) != 0;  // framedata.cpp took it
    if (const wchar_t* file = _wgetenv(L"MK3D_DISPLAY_FILE"); file && *file) WatchDisplayFile(file, d);
}

}  // namespace mk3d::link
