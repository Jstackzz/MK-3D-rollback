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

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "app/app.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
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
    g_online = !Key(c, "remote").empty() && mode != "practice";
    const int side = Key(c, "side", "1") == "2" ? 1 : 0;
    if (g_online) {
        net::Settings st;
        st.bind = uint16_t(std::atoi(Key(c, "local_port", "7001").c_str()));
        st.peer = Key(c, "remote");
        st.side = side;
        st.delay = std::atoi(Key(c, "delay", "2").c_str());
        st.loopback_only = st.peer.rfind("127.", 0) == 0;
        if (const char* log = Env("NET_LOG")) {  // "{side}" becomes 0 or 1 (two games on one PC), "{match}" the id
            st.log = log;
            size_t at = st.log.find("{side}");
            if (at != std::string::npos) st.log.replace(at, 6, std::to_string(st.side));
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
    start.online = g_online;
    start.side = side;
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
    LOGI("link: start %s (%s vs %s), match %s, %s", mode.c_str(), picks[0].c_str(), picks[1].c_str(), g_match.c_str(),
         g_online ? "online" : "offline");
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
        WriteStatus("result winner=" + std::to_string(winner) + " p1_rounds=" + std::to_string(r1) +
                    " p2_rounds=" + std::to_string(r2) + " match=" + g_match);
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
    LOGI("link: back to the main menu");
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
    // the player's controller reaches the game only while the match runs (and on the select screen, for their own
    // side's cursor when they pick there: the game's pads route it, autopilot LocalPickPort)
    return !(g_in_match && g_fighting && !g_reported) || game::AutopilotActive();
}

void ApplyLauncherDisplay() {
    const char* mode = std::getenv("MK3D_DISPLAY");
    const char* res = std::getenv("MK3D_RESOLUTION");
    const char* vsync = std::getenv("MK3D_VSYNC");
    if (!mode && !res && !vsync) return;  // started by hand: the settings file alone
    app::Settings& s = app::Get();
    if (mode) {
        if (!std::strcmp(mode, "windowed")) s.display_mode = app::DisplayMode::Windowed;
        else if (!std::strcmp(mode, "borderless")) s.display_mode = app::DisplayMode::Borderless;
        else if (!std::strcmp(mode, "fullscreen")) s.display_mode = app::DisplayMode::Fullscreen;
        else LOGR("link: MK3D_DISPLAY=%s is no display mode", mode);
    }
    if (res && !std::strcmp(res, "desktop")) {
        s.width = s.height = 0;
    } else if (res) {
        char* end = nullptr;
        const unsigned long w = std::strtoul(res, &end, 10);
        const unsigned long h = (*end == 'x' || *end == 'X') ? std::strtoul(end + 1, &end, 10) : 0;
        if (!*end && w >= 320 && h >= 240 && w <= 16384 && h <= 16384) {
            s.width = uint32_t(w);
            s.height = uint32_t(h);
        } else {
            LOGR("link: MK3D_RESOLUTION=%s is no resolution", res);
        }
    }
    // a window the desktop's size would not fit on it with its frame
    if (s.display_mode == app::DisplayMode::Windowed && (!s.width || !s.height)) {
        s.width = 1280;
        s.height = 720;
    }
    if (vsync) s.vsync = std::atoi(vsync) != 0;
}

}  // namespace mk3d::link
