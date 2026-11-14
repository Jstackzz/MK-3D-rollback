// Netplay: rollback (version 2) or lockstep (version 1) over UDP, shared by the games; see netplay.h for the model and
// the configuration, netgame.h for what each game supplies.
//
// Packets (little endian): u32 magic "MKDR", u8 type, u8 side, u16 barrier sequence, then
//   1 HELLO   [u8 n, n x u32]             sent while running locally, so each side knows the other is there; with the
//             words of this game's player's pick on the select screen once there is one (SetLocalPick)
//   2 READY   u32 seed, u8 n, n x u32, u8 delay
//             "my match is about to be set up": the seed this game proposes and the game's own words (its mode, picks,
//             frame counters, a side mode's setup: netgame.h ReadyWords), then the input delay it came with (both play
//             the larger; a game before 0.1.18 sends none); the barrier ends when both have one
//   3 INPUT   u32 ack, u32 now, i32 advantage, u32 first, u8 n, n x (u32 buttons, u32 sticks)
//             the sender's inputs for frames first .. first+n-1, the highest frame of ours it holds without gaps,
//             the frame it runs and how many frames it thinks it runs ahead of us
//   4 CHECK   u32 frame, u32 state, u32 rand, u8 loading
//             at the top of a frame both games agree on: the game's state hash, the C runtime's random numbers drawn
//             since the barrier reseeded them, and whether the match was still loading (Checks)
//  10 DETAIL  u32 check, u8 kind, then by kind: after a failed check, what the other game needs to tell where the two
//             parted (an older game sends none and passes them by)
//             1 frames: u32 first, u8 n, n x (u32 state, u32 rand, u8 loading)   each frame's top from the check before
//                       the failed one, as this game's run of it that counts had it
//             0 pieces: u32 frame, u8 blocks, u8 runs, runs x (u8 block, u32 size, u16 first, u8 n, n x u16)   the
//                       16-bit hash of each 64-byte piece of the check blocks at that frame's top (the first one the
//                       two games' states differ at)
//
// The spectator feed (TCP to the launcher, each packet after its u16 length; the header's side is the sender's):
//   5 PICK    [u8 n, n x u32]             the sender's player's pick on the select screen (as in HELLO)
//   6 MEET    u32 seed, u8 n, n x u32     the sender's READY at the meeting point: its seed and words
//   7 OWN     u32 first, u8 n, n x (u32 buttons, u32 sticks)   the sender's own inputs, frames first .. first+n-1
//   8 CHECK   as above, the sender's
//   9 END     u32 frame, u8 how           the sender left the match in sync at that frame (0 over, 1 not)
//
// Rollback, per frame (the frame hooks of mk3d/game.h):
//   top of frame    the frame that ran is closed (rollback.h); what arrived is read; if the other player's real
//                   input for a frame differs from what that frame used, the game goes back to that frame's top
//                   and runs again up to the present (unseen: no present, no pacing, sound mixed but not heard);
//                   time sync: a game too far past the frames both agree on waits, one running ahead slows down;
//                   while the game loads, the frame waits for the other player's real input instead
//   after the pads  this frame's local input is taken for frame + delay and sent; both ports' blocks get the inputs
//                   for this frame (the other player's real one, or the last one repeated)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include "mk3d/netplay.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <xmmintrin.h>

#include "app/app.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "gpu/backend.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "mk3d/game.h"
#include "mk3d/gclock.h"
#include "mk3d/netgame.h"
#include "mk3d/rollback.h"
#include "xdk/d3d8.h"
#include "xdk/dsound.h"

namespace mk3d::net {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kMagic = 0x52444B4D;  // "MKDR"
constexpr uint8_t kHello = 1, kReady = 2, kInput = 3, kCheck = 4;
constexpr uint8_t kFeedPick = 5, kFeedMeet = 6, kFeedOwn = 7, kFeedCheck = 8, kFeedEnd = 9;
constexpr uint8_t kDetail = 10, kDetailPieces = 0, kDetailFrames = 1;
constexpr uint32_t kMaxResend = 48;              // inputs an INPUT packet holds at most: 409 bytes, the relay takes 512
constexpr uint32_t kCheckEvery = 30;
constexpr uint32_t kMaxPredict = 12;             // frames past the last agreed one (rollback keeps 16)
constexpr uint32_t kMixFrames = 48000 / 60;      // the sound of one frame
constexpr int kMaxReadyWords = 64;
// Online Practice (Settings.sparring): a player's BACK goes in their input as this bit, which never reaches the game's
// blocks (no game's logical buttons reach it); once it is in an input both games have, the match in sync ends
constexpr uint32_t kBackBit = 0x80000000u;
constexpr uint32_t kNoFrame = 0xFFFFFFFFu;

struct Input {
    uint32_t buttons = 0;
    uint32_t sticks = game::NeutralSticks();
    bool operator==(const Input& o) const { return buttons == o.buttons && sticks == o.sticks; }
    bool operator!=(const Input& o) const { return !(*this == o); }
};

enum class Phase { Off, Local, Barrier, Lockstep, Rollback };

struct Config {
    bool on = false;
    uint16_t bind = 0;
    sockaddr_in peer{};
    int side = 0;
    int delay = 1;
    bool rollback = true;
    FILE* log = nullptr;
    int lag = 0, jitter = 0, loss = 0;
    bool feed_on = false;  // the launcher's spectator feed
    sockaddr_in feed{};
    bool watch = false;    // a spectator
    bool sparring = false;  // Online Practice
};

Config g_cfg;
SOCKET g_sock = INVALID_SOCKET;
Phase g_phase = Phase::Off;
uint16_t g_seq = 0;              // barriers passed
bool g_peer_ready = false;       // READY for g_seq received
uint16_t g_left_seq = 0xFFFF;    // the match in sync we left on our own (the other game may still need our inputs)
Clock::time_point g_last_resend;
uint32_t g_their_seed = 0;
std::vector<uint32_t> g_their_words;  // the other game's READY words
uint32_t g_my_seed = 0;          // the seed this game proposes at a barrier (side 0's is taken)
uint32_t g_seed = 0;             // the seed both games agreed on
bool g_start_pending = false;    // met at a match setup: the match in sync starts at the next frame's top
uint32_t g_frame = 0;            // the frame in sync running now
std::map<uint32_t, Input> g_mine, g_theirs;
uint32_t g_their_ack = 0;        // frames of ours the peer holds (exclusive)
struct Check {
    uint32_t state = 0, rand = 0;  // StateHash, RandHash
    bool loading = false;          // the match was still loading at that frame
};
std::map<uint32_t, Check> g_my_checks, g_their_checks;
bool g_desynced = false;
Clock::time_point g_last_hello;
bool g_peer_seen = false;

// lockstep only
uint32_t g_prev_buttons[2] = {};

// rollback only
std::map<uint32_t, Input> g_used;  // the other player's input each frame's last run used
uint32_t g_target = 0;             // running again up to this frame (exclusive)
bool g_replaying = false;
uint32_t g_confirmed = 0;          // every frame below ran with the other player's real input
uint32_t g_back_frame = kNoFrame;  // Online Practice: the first frame both agree a player pressed BACK at
uint32_t g_back_scan = 0;          // the next frame to look at for it
bool g_had_focus = true;           // the game's window was in front at the last frame (WatchFocus)
uint32_t g_their_now = 0;          // the frame the other game said it runs
int g_their_adv = 0;               // how far ahead of us it thinks it runs
Clock::time_point g_replay_t0, g_last_slowdown;
int g_lead_frames = 0;             // new frames in a row this game has led the other by 2 or more
uint32_t g_lead_at = 0;            // the frame last counted for it
std::map<uint32_t, std::pair<uint32_t, uint32_t>> g_rand_at;  // the rand counters at each frame's top
// Loading: what a state switch's new task loads must never be run again by a rollback: the heaps, the graphics
// device and the files it touches outside guest memory do not go back with it (a Deception chess battle's load
// crashed in a corrupted heap). From a state switch (the switching frame itself ends there) and from the match's
// start, every frame waits for the other player's real input instead of predicting it, until kLoadQuiet frames
// passed without a file read. Reads alone (music streams through the main thread while recording) start nothing.
constexpr uint32_t kLoadQuiet = 60;
// A match's opening two seconds: its load (then kLoadQuiet frames without a read; a late read, the announcer's say,
// starts them again) and the first round's intro, where nobody can move yet. Both games play them with no input on either side, known before they run, so neither waits on
// the other's real input there: the load's wait for it cost a frame per one-way trip, about a second of slow motion
// per 100 ms of ping at every match start (twice that at one frame of delay). <prefix>_NET_NO_INTRO=1 (tests, both
// games) waits as before.
constexpr uint32_t kIntroFrames = 120;
uint32_t g_intro = kIntroFrames;
Clock::time_point g_sync_t0, g_second_t0;  // the match's first frame, the last whole second's first (first runs)
uint32_t g_second_stalls = 0, g_second_rollbacks = 0;
bool g_loading = false;
uint32_t g_quiet = 0;              // frames in a row without a file read while loading
uint64_t g_reads_seen = 0;
uint32_t g_switches_seen = 0;
bool g_frame_waited = false;       // the frame running now waited for the real input (for the log)
// A frame shown late: one that took this long from the end of its waits for the other game to the next frame's top,
// the rollback it began with included, where 60 frames a second give each 16.7 ms
constexpr auto kLateFrame = std::chrono::milliseconds(25);
uint32_t g_timed = 0;              // the last frame timed so, at its first run
uint32_t g_rolled = 0;             // the frame whose top last began a rollback
Clock::time_point g_timed_t0;      // when that run began, after its waits

std::atomic<uint32_t> g_rand_calls{0};
std::atomic<uint32_t> g_rand_last{0};

// what other threads see, and what they ask for: applied by the main thread at the top of a frame
std::atomic<int> g_status{int(Status::Off)};
std::atomic<uint32_t> g_desync_frame{0};
std::atomic<uint32_t> g_lockstep_frame{0};
std::mutex g_settled_lock;
Settled g_settled_state;
bool g_settled = false;
std::mutex g_stats_lock;
Stats g_stats;
std::mutex g_pending_lock;
std::optional<Settings> g_pending;
bool g_pending_close = false;
// the launcher ended the match (Disable while it runs: the other player left it, the server says): what waits for the
// other game gives up at once instead of after its timeout
std::atomic<bool> g_abort{false};
// the main thread waits on the other game or the feed now (Waiting)
std::atomic<bool> g_waiting{false};
struct WaitScope {
    WaitScope() { g_waiting = true; }
    ~WaitScope() { g_waiting = false; }
};

void SetStatus(Status st) { g_status = int(st); }

bool ParsePeer(const char* s, sockaddr_in& out) {
    std::string a(s);
    size_t c = a.rfind(':');
    if (c == std::string::npos) return false;
    out.sin_family = AF_INET;
    out.sin_port = htons(uint16_t(std::atoi(a.c_str() + c + 1)));
    return inet_pton(AF_INET, a.substr(0, c).c_str(), &out.sin_addr) == 1;
}

// --- the spectator feed: a TCP connection to the launcher on this PC, each packet after its u16 length. A player's
// game writes to it (FeedSend), a spectator's game reads both players' packets from it (HandleFeed); the launcher
// sends a spectator everything again when it connects again, so duplicates are taken as they come.
SOCKET g_feed = INVALID_SOCKET;
bool g_feed_connected = false;
Clock::time_point g_feed_retry;
std::string g_feed_out, g_feed_in;
constexpr size_t kFeedOutMax = size_t(4) << 20;  // what may wait for a launcher that does not read
bool g_feed_overflow = false;

void HandleFeed(const uint8_t* b, int n);

void FeedClose(const char* why) {
    if (g_feed != INVALID_SOCKET) {
        if (why) LOGW("netplay: the spectator feed %s", why);
        closesocket(g_feed);
    }
    g_feed = INVALID_SOCKET;
    g_feed_connected = false;
    g_feed_in.clear();
}

void FeedConnect() {
    if (g_feed != INVALID_SOCKET || Clock::now() < g_feed_retry) return;
    g_feed_retry = Clock::now() + std::chrono::milliseconds(500);
    g_feed = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_feed == INVALID_SOCKET) return;
    u_long nb = 1;
    ioctlsocket(g_feed, FIONBIO, &nb);
    BOOL nodelay = TRUE;
    setsockopt(g_feed, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof nodelay);
    if (connect(g_feed, reinterpret_cast<const sockaddr*>(&g_cfg.feed), sizeof g_cfg.feed) == 0)
        g_feed_connected = true;
    else if (WSAGetLastError() != WSAEWOULDBLOCK)
        FeedClose(nullptr);
}

// main thread: connects (again, every half second while the launcher does not listen), writes what waits, and for a
// spectator reads and handles what came
void FeedPump() {
    if (!g_cfg.feed_on) return;
    FeedConnect();
    if (g_feed == INVALID_SOCKET) return;
    if (!g_feed_connected) {
        fd_set w, e;
        FD_ZERO(&w);
        FD_ZERO(&e);
        FD_SET(g_feed, &w);
        FD_SET(g_feed, &e);
        timeval tv{0, 0};
        if (select(0, nullptr, &w, &e, &tv) <= 0) return;
        if (FD_ISSET(g_feed, &e) || !FD_ISSET(g_feed, &w)) {
            FeedClose(nullptr);
            return;
        }
        g_feed_connected = true;
        LOGI("netplay: spectator feed connected");
    }
    while (!g_feed_out.empty()) {
        const int n = send(g_feed, g_feed_out.data(), int(std::min<size_t>(g_feed_out.size(), 1 << 16)), 0);
        if (n > 0) {
            g_feed_out.erase(0, size_t(n));
            continue;
        }
        if (WSAGetLastError() != WSAEWOULDBLOCK) FeedClose("could not be written");
        break;
    }
    if (!g_cfg.watch || g_feed == INVALID_SOCKET) return;
    char buf[1 << 14];
    for (;;) {
        const int n = recv(g_feed, buf, sizeof buf, 0);
        if (n > 0) {
            g_feed_in.append(buf, size_t(n));
            continue;
        }
        if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK) {
            FeedClose("was closed by the launcher");
            return;
        }
        break;
    }
    size_t at = 0;
    while (g_feed_in.size() - at >= 2) {
        uint16_t len;
        std::memcpy(&len, g_feed_in.data() + at, 2);
        if (g_feed_in.size() - at - 2 < len) break;
        HandleFeed(reinterpret_cast<const uint8_t*>(g_feed_in.data()) + at + 2, len);
        at += 2 + size_t(len);
    }
    g_feed_in.erase(0, at);
}

// the last packets (a match's end) go out before the connection closes, for a moment at most
void FeedFlushAndClose() {
    auto t0 = Clock::now();
    while (g_feed != INVALID_SOCKET && g_feed_connected && !g_feed_out.empty() &&
           Clock::now() - t0 < std::chrono::milliseconds(200)) {
        const int n = send(g_feed, g_feed_out.data(), int(std::min<size_t>(g_feed_out.size(), 1 << 16)), 0);
        if (n > 0) g_feed_out.erase(0, size_t(n));
        else if (WSAGetLastError() == WSAEWOULDBLOCK) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        else break;
    }
    FeedClose(nullptr);
    g_feed_out.clear();
    g_feed_overflow = false;
}

// the rollback machinery off, the game on its own again
void StopRollback() {
    game::RngEnd();
    if (rb::Armed()) rb::Disarm();
    dsound::SetFrameClock(false);
    gclock::Stop();
    d3d::SetSkipPresent(false);
    g_replaying = false;
}

// the top of a frame both games agree on: what the launcher may report from it (nothing once a check failed: the
// match ends as a desync, FinishDetail)
void Settle() {
    if (g_desynced) return;
    Settled s;
    game::Settle(s);
    std::lock_guard<std::mutex> lock(g_settled_lock);
    g_settled_state = s;
    g_settled = true;
}

// rollback: the top of each frame as its last run had it, until every input before it is real (g_confirmed)
std::map<uint32_t, Settled> g_frame_settled;

void Unsettle() {
    g_frame_settled.clear();
    std::lock_guard<std::mutex> lock(g_settled_lock);
    g_settled = false;
}

// Rollback: this frame's top as this run has it, kept; the newest kept one whose inputs before it are all real is
// what both games agree on (a misprediction runs the frame again and keeps that run's). Settled only at a present
// frame resting on real inputs alone, which at a high ping it seldom is, the result went out late: at 120 ms one
// game's came 11 to 13 s after the other's, and the rematch waited for it.
void SettleConfirmed(uint32_t frame) {
    if (g_desynced) return;
    Settled s;
    game::Settle(s);
    g_frame_settled[frame] = s;
    auto it = g_frame_settled.upper_bound(g_confirmed);
    if (it == g_frame_settled.begin()) return;
    --it;
    {
        std::lock_guard<std::mutex> lock(g_settled_lock);
        g_settled_state = it->second;
        g_settled = true;
    }
    g_frame_settled.erase(g_frame_settled.begin(), it);
}

bool InSync();
void FeedEnd(uint32_t frame, uint8_t how);
void FinishDetail(bool complete);

void CloseNow() {
    FinishDetail(false);  // a failed check's detail still under way: written out with what came of it
    if (InSync()) {  // the launcher ended the match while it ran (the other player left it): spectators stop there too
        LOGR("netplay: the match in sync was ended at frame %u", g_frame);
        FeedEnd(g_frame, 1);
    }
    StopRollback();
    g_abort = false;
    FeedFlushAndClose();
    if (g_sock != INVALID_SOCKET) closesocket(g_sock);
    g_sock = INVALID_SOCKET;
    if (g_cfg.log) std::fclose(g_cfg.log);
    g_cfg = Config{};
    g_phase = Phase::Off;
    SetStatus(Status::Off);
}

void WatchReset();

// a spectator: no peer and no UDP, only the launcher's feed
bool OpenWatch(const Settings& st) {
    if (!ParsePeer(st.feed.c_str(), g_cfg.feed)) {
        LOGE("netplay: the spectator feed must be ip:port, not %s", st.feed.c_str());
        return false;
    }
    g_cfg.feed_on = true;
    g_cfg.watch = true;
    g_cfg.sparring = st.sparring;
    g_cfg.side = 0;  // P1's inputs go to port 0, P2's to port 1, as in the players' games
    g_cfg.rollback = true;
    g_cfg.delay = 0;
    if (!st.log.empty()) g_cfg.log = std::fopen(st.log.c_str(), "w");
    g_cfg.on = true;
    g_phase = Phase::Local;
    g_seq = 0;
    g_left_seq = 0xFFFF;
    g_desync_frame = 0;
    WatchReset();
    SetStatus(Status::Local);
    LOGR("netplay: watching, both players' inputs from the feed at %s", st.feed.c_str());
    return true;
}

// main thread only
bool OpenNow(const Settings& st) {
    CloseNow();
    static const bool wsa_up = [] {
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    (void)wsa_up;
    if (st.watch) return OpenWatch(st);
    sockaddr_in peer{};
    if (!ParsePeer(st.peer.c_str(), peer)) {
        LOGE("netplay: the peer must be ip:port, not %s", st.peer.c_str());
        return false;
    }
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(st.bind);
    addr.sin_addr.s_addr = htonl(st.loopback_only ? INADDR_LOOPBACK : INADDR_ANY);
    if (g_sock == INVALID_SOCKET || bind(g_sock, reinterpret_cast<sockaddr*>(&addr), sizeof addr)) {
        LOGE("netplay: cannot bind UDP port %u", st.bind);
        CloseNow();
        return false;
    }
    u_long nb = 1;
    ioctlsocket(g_sock, FIONBIO, &nb);
    g_cfg.bind = st.bind;
    g_cfg.peer = peer;
    g_cfg.side = st.side ? 1 : 0;
    g_cfg.rollback = st.rollback;
    g_cfg.sparring = st.sparring;
    int delay = st.delay >= 0 ? st.delay : (st.rollback ? 1 : 2);
    g_cfg.delay = std::max(0, std::min(8, delay));
    g_cfg.lag = std::max(0, st.lag_ms);
    g_cfg.jitter = std::max(0, st.jitter_ms);
    g_cfg.loss = std::max(0, std::min(90, st.loss_percent));
    if (!st.log.empty()) g_cfg.log = std::fopen(st.log.c_str(), "w");
    if (!st.feed.empty()) {
        if (ParsePeer(st.feed.c_str(), g_cfg.feed)) g_cfg.feed_on = true;
        else LOGW("netplay: the spectator feed must be ip:port, not %s", st.feed.c_str());
    }
    g_cfg.on = true;
    g_phase = Phase::Local;
    g_seq = 0;
    g_left_seq = 0xFFFF;
    g_desync_frame = 0;
    SetStatus(Status::Local);
    // no address in the log (the other player's, or the server's relay): players pass their logs around
    LOGR("netplay: UDP %u, side %d, %s with %d frames of delay%s%s", st.bind, g_cfg.side,
         g_cfg.rollback ? "rollback" : "lockstep", g_cfg.delay, g_cfg.feed_on ? ", a feed for spectators" : "",
         g_cfg.lag || g_cfg.jitter || g_cfg.loss ? "," : "");
    if (g_cfg.lag || g_cfg.jitter || g_cfg.loss)
        LOGR("netplay: simulated network on what this game sends: %d ms + up to %d ms, %d%% lost", g_cfg.lag,
             g_cfg.jitter, g_cfg.loss);
    return true;
}

void ApplyPending() {
    std::lock_guard<std::mutex> lock(g_pending_lock);
    if (g_pending_close) {
        CloseNow();
        g_pending_close = false;
    }
    if (g_pending) {
        OpenNow(*g_pending);
        g_pending.reset();
    }
}

void Load() {
    const char* bind_port = Env("NET_BIND");
    const char* peer = Env("NET_PEER");
    const char* feed = Env("NET_FEED");
    const bool watch = Env("NET_WATCH") != nullptr;
    if (watch ? !feed : !bind_port || !peer) return;
    Settings st;
    if (bind_port) st.bind = uint16_t(std::atoi(bind_port));
    if (peer) st.peer = peer;
    if (feed) st.feed = feed;
    st.watch = watch;
    if (const char* v = Env("NET_SIDE")) st.side = std::atoi(v) ? 1 : 0;
    if (const char* v = Env("NET_DELAY")) st.delay = std::atoi(v);
    if (const char* v = Env("NET_MODE")) st.rollback = std::strcmp(v, "lockstep") != 0;
    if (const char* v = Env("NET_LOG")) st.log = v;
    if (const char* v = Env("NET_LAG")) st.lag_ms = std::atoi(v);
    if (const char* v = Env("NET_JITTER")) st.jitter_ms = std::atoi(v);
    if (const char* v = Env("NET_LOSS")) st.loss_percent = std::atoi(v);
    std::lock_guard<std::mutex> lock(g_pending_lock);
    g_pending = st;
}

// --- packets

struct Writer {
    uint8_t b[1024];
    int n = 0;
    void U8(uint8_t v) { b[n++] = v; }
    void U16(uint16_t v) { std::memcpy(b + n, &v, 2), n += 2; }
    void U32(uint32_t v) { std::memcpy(b + n, &v, 4), n += 4; }
};

Writer Header(uint8_t type) {
    Writer w;
    w.U32(kMagic);
    w.U8(type);
    w.U8(uint8_t(g_cfg.side));
    w.U16(g_seq);
    return w;
}

// the simulated network: what this game sends waits, and some of it is lost
struct Delayed {
    Clock::time_point at;
    std::vector<uint8_t> bytes;
};
std::deque<Delayed> g_outbox;
std::mt19937 g_rng{0x4D4B44u};

void SendRaw(const uint8_t* b, int n) {
    if (g_sock == INVALID_SOCKET) return;  // a spectator has no peer
    sendto(g_sock, reinterpret_cast<const char*>(b), n, 0, reinterpret_cast<const sockaddr*>(&g_cfg.peer),
           sizeof g_cfg.peer);
}

// a player's packet for the spectators, out with the next FeedPump (every frame's Receive)
void FeedSend(const Writer& w) {
    if (!g_cfg.feed_on || g_cfg.watch) return;
    if (g_feed_out.size() + size_t(w.n) + 2 > kFeedOutMax) {
        if (!g_feed_overflow) LOGW("netplay: the launcher does not read the spectator feed: dropping what it carries");
        g_feed_overflow = true;
        return;
    }
    const uint16_t len = uint16_t(w.n);
    g_feed_out.append(reinterpret_cast<const char*>(&len), 2);
    g_feed_out.append(reinterpret_cast<const char*>(w.b), size_t(w.n));
}

void Pump() {
    if (g_outbox.empty()) return;
    auto now = Clock::now();
    for (auto it = g_outbox.begin(); it != g_outbox.end();) {
        if (it->at <= now) {
            SendRaw(it->bytes.data(), int(it->bytes.size()));
            it = g_outbox.erase(it);
        } else {
            ++it;
        }
    }
}

void Send(const Writer& w) {
    if (g_cfg.loss && int(g_rng() % 100) < g_cfg.loss) return;
    if (!g_cfg.lag && !g_cfg.jitter) return SendRaw(w.b, w.n);
    int extra = g_cfg.jitter ? int(g_rng() % uint32_t(g_cfg.jitter + 1)) : 0;
    g_outbox.push_back({Clock::now() + std::chrono::milliseconds(g_cfg.lag + extra), std::vector<uint8_t>(w.b, w.b + w.n)});
}

// How far past this game's frame a packet's inputs and checks may name frames: a player's game waits for the other's
// inputs long before it gets that far ahead, a spectator's feed holds at most a whole match. A hostile sender cannot
// fill the maps with frames that are never reached (and never pruned).
constexpr uint32_t kMaxAhead = 600, kMaxFeedAhead = 60 * 3600;

// the players' picks on the select screen for the next match (SetLocalPick / PeerPick)
constexpr int kMaxPickWords = 8;
std::mutex g_pick_lock;
std::vector<uint32_t> g_my_pick, g_their_pick;
bool g_their_pick_in = false;

// a spectator: what the feed brought from one player's game
struct WatchSide {
    std::vector<uint32_t> pick;        // for the match set up next (g_pick_lock)
    bool meet_in = false;              // its READY at the meeting point: its seed and words
    uint32_t seed = 0;
    std::vector<uint32_t> words;
    std::map<uint32_t, Input> inputs;  // its player's own inputs in the match in sync (or about to be)
    uint32_t contiguous = 0;           // frames held without a gap (exclusive)
    bool ended = false;                // its game left the match in sync at end_frame (how: 0 over, 1 not)
    uint32_t end_frame = 0;
    uint8_t end_how = 0;
};
WatchSide g_watch[2];

void ClearPicks() {
    std::lock_guard<std::mutex> lock(g_pick_lock);
    g_my_pick.clear();
    g_their_pick.clear();
    g_their_pick_in = false;
    for (WatchSide& w : g_watch) w.pick.clear();
}

void SendHello() {
    Writer w = Header(kHello);
    std::lock_guard<std::mutex> lock(g_pick_lock);
    if (!g_my_pick.empty()) {
        w.U8(uint8_t(g_my_pick.size()));
        for (uint32_t v : g_my_pick) w.U32(v);
        Writer f = w;  // the same words for the spectators
        f.b[4] = kFeedPick;
        FeedSend(f);
    }
    Send(w);
}

std::vector<uint32_t> g_my_words;  // this game's READY words at the barrier now
int g_their_delay = -1;            // the input delay the other game came with (READY's last byte; -1 an older game's)

// after the words, the input delay this game came with (its launcher's: the player's, or more for the ping, cfg
// autoDelay): both play the larger (AgreeDelay); a game before 0.1.18 reads past it
Writer ReadyPacket(uint8_t type) {
    Writer w = Header(type);
    w.U32(g_my_seed);
    uint8_t n = uint8_t(std::min<size_t>(g_my_words.size(), kMaxReadyWords));
    w.U8(n);
    for (uint8_t i = 0; i < n; i++) w.U32(g_my_words[i]);
    w.U8(uint8_t(g_cfg.delay));
    return w;
}

// both games play the larger of their two delays, so the one with the worse ping sets it for both. A READY without one
// is an older game's (a player whose launcher updated but whose game did not: 0.1.17's arena desyncs were all one such
// player), or the other game's READY was lost and its inputs came instead.
void AgreeDelay() {
    if (g_their_delay < 0)
        LOGW("netplay: the other game sent no delay with its READY: a game older than 0.1.18, or its READY was lost");
    if (g_their_delay <= g_cfg.delay) return;
    LOGR("netplay: %d frames of delay, as the other game came with (ours %d)", std::min(8, g_their_delay),
         g_cfg.delay);
    g_cfg.delay = std::min(8, g_their_delay);
}

void SendReady() { Send(ReadyPacket(kReady)); }

uint32_t g_theirs_contiguous = 0;  // the other player's inputs we hold without gaps (exclusive)

void UpdateContiguous() {
    while (g_theirs.count(g_theirs_contiguous)) g_theirs_contiguous++;
}

int LocalAdvantage() { return int(g_frame) - int(g_their_now); }

void SendInputs() {
    if (g_mine.empty()) return;
    uint32_t last = g_mine.rbegin()->first;
    // from the first input the other game lacks, however old. Only the newest 16 went out before: a third of a second of
    // packets lost one way let this game run on past inputs the other never got, nothing sent them again, and both games
    // waited for each other until the timeout, a frozen match that ended as a disconnect on both sides
    uint32_t first = g_their_ack ? g_their_ack - 1 : 0;
    if (first <= last && last - first >= kMaxResend) first = last - kMaxResend + 1;
    first = std::max(first, g_mine.begin()->first);
    if (first > last) first = last;
    Writer w = Header(kInput);
    w.U32(g_theirs_contiguous);
    w.U32(g_frame);
    w.U32(uint32_t(LocalAdvantage()));
    w.U32(first);
    uint8_t n = uint8_t(last - first + 1);
    w.U8(n);
    for (uint32_t f = first; f <= last; f++) {
        auto it = g_mine.find(f);
        Input in = it != g_mine.end() ? it->second : Input{};
        w.U32(in.buttons);
        w.U32(in.sticks);
    }
    Send(w);
}

void SendCheck(uint32_t frame, const Check& c) {
    Writer w = Header(kCheck);
    w.U32(frame);
    w.U32(c.state);
    w.U32(c.rand);
    w.U8(c.loading ? 1 : 0);
    Send(w);
    w.b[4] = kFeedCheck;  // the spectators compare theirs with it
    FeedSend(w);
}

// --- what a player's game writes to the spectator feed besides its pick and its checks

uint32_t g_fed = 0;  // this player's own inputs from this frame on are not in the feed yet

// this player's own inputs (final the moment the pad is read), every 4 frames or all of them now, the last 4 again
void FeedOwn(bool all) {
    if (!g_cfg.feed_on || g_cfg.watch || g_mine.empty()) return;
    const uint32_t last = g_mine.rbegin()->first;
    if (last < g_fed || (!all && last < g_fed + 3)) return;
    uint32_t first = std::max(g_fed > 4 ? g_fed - 4 : 0u, g_mine.begin()->first);
    while (first <= last) {
        const uint32_t n = std::min<uint32_t>(last - first + 1, 100);
        Writer w = Header(kFeedOwn);
        w.U32(first);
        w.U8(uint8_t(n));
        for (uint32_t f = first; f < first + n; f++) {
            auto it = g_mine.find(f);
            const Input in = it != g_mine.end() ? it->second : Input{};
            w.U32(in.buttons);
            w.U32(in.sticks);
        }
        FeedSend(w);
        first += n;
    }
    g_fed = last + 1;
}

// this game left the match in sync at frame `frame`: how 0 at its end, 1 otherwise (the other game gone, a desync)
void FeedEnd(uint32_t frame, uint8_t how) {
    if (!g_cfg.feed_on || g_cfg.watch) return;
    FeedOwn(true);
    Writer w = Header(kFeedEnd);
    w.U32(frame);
    w.U8(how);
    FeedSend(w);
}

bool InSync() { return g_phase == Phase::Lockstep || g_phase == Phase::Rollback; }

void OnDetail(const uint8_t* b, int n);

void Receive() {
    Pump();
    FeedPump();
    if (g_sock == INVALID_SOCKET) return;  // a spectator: the feed only
    uint8_t b[1500];
    for (;;) {
        sockaddr_in from{};
        int fl = sizeof from;
        int n = recvfrom(g_sock, reinterpret_cast<char*>(b), sizeof b, 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n < 8) return;
        // only the endpoint this match was set up with (the other player's game, or the server's relay) is heard:
        // anyone else who learned the port could otherwise send this game inputs or end its match
        if (from.sin_family != AF_INET || from.sin_addr.s_addr != g_cfg.peer.sin_addr.s_addr ||
            from.sin_port != g_cfg.peer.sin_port)
            continue;
        uint32_t magic;
        std::memcpy(&magic, b, 4);
        if (magic != kMagic) continue;
        uint8_t type = b[4];
        uint16_t seq;
        std::memcpy(&seq, b + 6, 2);
        g_peer_seen = true;
        auto u32 = [&](int o) {
            uint32_t v;
            std::memcpy(&v, b + o, 4);
            return v;
        };
        if (type == kReady && n >= 13) {
            if (seq == g_seq && g_phase == Phase::Barrier) {
                g_peer_ready = true;
                g_their_seed = u32(8);
                uint8_t count = b[12];
                g_their_words.clear();
                for (int i = 0; i < count && 13 + i * 4 + 4 <= n; i++) g_their_words.push_back(u32(13 + i * 4));
                const int at = 13 + count * 4;
                g_their_delay = at < n ? b[at] : -1;
            }
            // a peer still at the barrier we already left lost our READY: tell it again
            if (seq + 1 == g_seq && InSync()) SendInputs();
        } else if (type == kInput && n >= 25) {
            if (g_phase == Phase::Barrier && seq == g_seq + 1) {
                // the peer left the barrier we are still in: its READY got lost, its inputs say the same
                g_peer_ready = true;
            }
            if (!InSync() && g_phase != Phase::Barrier && seq == g_seq && g_left_seq == g_seq) {
                // the other game is still in the match we left: it ends at the same frame once it has our last
                // inputs, which a lost packet may have kept from it
                g_their_ack = std::max(g_their_ack, u32(8));
                if (Clock::now() - g_last_resend > std::chrono::milliseconds(20)) {
                    SendInputs();
                    g_last_resend = Clock::now();
                }
                continue;
            }
            uint16_t want = InSync() ? g_seq : uint16_t(g_seq + 1);
            if (seq != want) continue;
            g_their_ack = std::max(g_their_ack, u32(8));
            uint32_t now = u32(12);
            if (now >= g_their_now) {
                g_their_now = now;
                g_their_adv = int(u32(16));
            }
            uint32_t first = u32(20);
            uint8_t cnt = b[24];
            for (int i = 0; i < cnt && 25 + i * 8 + 8 <= n; i++) {
                if (first + uint32_t(i) > g_frame + kMaxAhead) break;
                Input in;
                in.buttons = u32(25 + i * 8);
                in.sticks = u32(29 + i * 8);
                g_theirs.emplace(first + uint32_t(i), in);
            }
            UpdateContiguous();
        } else if (type == kCheck && n >= 21 && seq == g_seq && u32(8) <= g_frame + kMaxAhead) {
            g_their_checks[u32(8)] = Check{u32(12), u32(16), b[20] != 0};
        } else if (type == kDetail && seq == g_seq) {
            OnDetail(b, n);  // a failed check's, in the match or out of it
        } else if (type == kHello && n >= 9 && seq == g_seq && !InSync()) {
            // the other player's pick for the match set up next
            int count = std::min<int>(b[8], kMaxPickWords);
            std::lock_guard<std::mutex> lock(g_pick_lock);
            g_their_pick.clear();
            for (int i = 0; i < count && 9 + i * 4 + 4 <= n; i++) g_their_pick.push_back(u32(9 + i * 4));
            if (!g_their_pick_in && !g_their_pick.empty())
                LOGI("netplay: the other player picked %08X (match %u)", g_their_pick[0], unsigned(seq));
            g_their_pick_in = !g_their_pick.empty();
        }
    }
}

// --- a spectator: both players' packets from the feed

bool g_rebuffer = true;   // the inputs ran out: wait until kWatchLead frames are ahead before going on
bool g_fast = false;      // far behind the players: three frames of four run unseen (FastForward)
constexpr uint32_t kWatchLead = 20;                    // the feed comes in bursts (the launchers send every 0.1 s)
constexpr uint32_t kFastFrom = 180, kFastUntil = 30;   // frames behind both players' newest input

void WatchReset() {
    for (WatchSide& w : g_watch) {
        std::lock_guard<std::mutex> lock(g_pick_lock);
        w = WatchSide{};
    }
    g_their_checks.clear();
    g_rebuffer = true;
    g_fast = false;
}

// A packet from one player's game. Its pick and READY count for the match set up next, its inputs, checks and end
// for the match in sync, or the one about to be (a spectator runs behind the players: their match may be under way
// while this game still sets it up).
void HandleFeed(const uint8_t* b, int n) {
    if (n < 8 || !g_cfg.watch) return;
    uint32_t magic;
    std::memcpy(&magic, b, 4);
    if (magic != kMagic) return;
    const uint8_t type = b[4];
    const int side = b[5] ? 1 : 0;
    uint16_t seq;
    std::memcpy(&seq, b + 6, 2);
    auto u32 = [&](int o) {
        uint32_t v;
        std::memcpy(&v, b + o, 4);
        return v;
    };
    WatchSide& w = g_watch[side];
    const bool next_match = !InSync() && seq == g_seq;
    const uint16_t sync_seq = InSync() ? g_seq : uint16_t(g_seq + 1);
    if (type == kFeedPick && n >= 9 && next_match) {
        const int count = std::min<int>(b[8], kMaxPickWords);
        std::lock_guard<std::mutex> lock(g_pick_lock);
        const bool had = !w.pick.empty();
        w.pick.clear();
        for (int i = 0; i < count && 9 + i * 4 + 4 <= n; i++) w.pick.push_back(u32(9 + i * 4));
        if (!had && !w.pick.empty()) LOGI("netplay: P%d picked %08X (match %u)", side + 1, w.pick[0], unsigned(seq));
    } else if (type == kFeedMeet && n >= 13 && next_match) {
        if (!w.meet_in) LOGI("netplay: P%d's game is at the meeting point", side + 1);
        w.meet_in = true;
        w.seed = u32(8);
        w.words.clear();
        for (int i = 0; i < b[12] && 13 + i * 4 + 4 <= n; i++) w.words.push_back(u32(13 + i * 4));
    } else if (type == kFeedOwn && n >= 13 && seq == sync_seq) {
        const uint32_t first = u32(8);
        for (int i = 0; i < b[12] && 13 + i * 8 + 8 <= n; i++) {
            if (first + uint32_t(i) > g_frame + kMaxFeedAhead) break;
            Input in;
            in.buttons = u32(13 + i * 8);
            in.sticks = u32(17 + i * 8);
            w.inputs.emplace(first + uint32_t(i), in);
        }
        while (w.inputs.count(w.contiguous)) w.contiguous++;
    } else if (type == kFeedCheck && n >= 21 && seq == sync_seq && u32(8) <= g_frame + kMaxFeedAhead) {
        g_their_checks[u32(8)] = Check{u32(12), u32(16), b[20] != 0};
    } else if (type == kFeedEnd && n >= 13 && seq == sync_seq && !w.ended) {
        w.ended = true;
        w.end_frame = u32(8);
        w.end_how = b[12];
        LOGI("netplay: P%d's game left the match at frame %u%s", side + 1, w.end_frame,
             w.end_how ? " before its end" : "");
    }
}

// --- the game's per-port blocks

// this PC's sticks as the last pad update read them, for the next one (BeforePads)
uint32_t g_local_sticks = 0;
bool g_local_sticks_in = false;

// An analog stick at rest is never quite still: its bytes wander by a point or two from frame to frame, and every
// change was an input the other game had mispredicted, so it went back and ran frames again over nothing (70% of the
// rollbacks in a player's matches, against players whose pads wander; players on a d-pad or a keyboard send none). All
// three games read a stick byte closer than this to 0x7F as centred (their deadzones, never changed: Deadly Alliance
// 0x266B34, Deception 0x3EBB24, Armageddon 0x54E5EC), so such a byte goes out centred: no game code can tell, and the
// input stands still until the stick means something. Past the deadzone the bytes go out as they are.
constexpr int kStickDeadzone = 48;

uint32_t SteadySticks(uint32_t sticks) {
    const uint32_t neutral = game::NeutralSticks();
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        const int v = int((sticks >> shift) & 0xFF);
        out |= (std::abs(v - 0x7F) < kStickDeadzone ? (neutral >> shift) & 0xFF : uint32_t(v)) << shift;
    }
    return out;
}

// <prefix>_NET_TEST_STICK_NOISE=1 (tests): this PC's sticks wander as a worn pad's do, around a point off centre
// <prefix>_NET_RAW_STICKS=1 (tests): they go out as read, to compare
const bool g_test_stick_noise = Env("NET_TEST_STICK_NOISE") != nullptr;
const bool g_raw_sticks = Env("NET_RAW_STICKS") != nullptr;

uint32_t WanderingSticks() {
    static std::minstd_rand rng(std::random_device{}());
    static uint8_t now[4] = {0x80, 0x7E, 0x81, 0x8B};  // a player's resting pad, from a match's log
    constexpr int kRest[4] = {0x80, 0x7E, 0x81, 0x8B};
    const int axis = int(rng() % 4);  // one axis a frame moves or stays: that pad's input changed in 2 of 3 frames
    now[axis] = uint8_t(kRest[axis] + int(rng() % 3) - 1);
    return uint32_t(now[0]) | uint32_t(now[1]) << 8 | uint32_t(now[2]) << 16 | uint32_t(now[3]) << 24;
}

Input ReadLocal() {
    Input in;
    game::ReadLocal(in.buttons, in.sticks);
    if (g_cfg.sparring && game::LocalBack()) in.buttons |= kBackBit;
    g_local_sticks = in.sticks;
    g_local_sticks_in = true;
    if (g_test_stick_noise) in.sticks = WanderingSticks();
    if (!g_raw_sticks) in.sticks = SteadySticks(in.sticks);
    return in;
}

void WritePort(int port, const Input& in, uint32_t prev) {
    game::WritePort(port, in.buttons & ~kBackBit, in.sticks, prev & ~kBackBit);
}

uint32_t Fnv(uint32_t h, uint32_t v) {
    for (int i = 0; i < 4; i++) h = (h ^ ((v >> (i * 8)) & 0xFF)) * 16777619u;
    return h;
}

// <prefix>_NET_DATA=1: a much wider net than the state checks for finding where two games part, in the log lines
// only: the image's data section in 64 KB blocks, pointers and bytes other threads own left out
constexpr uint32_t kDataBlock = 0x10000;
constexpr int kMaxDataBlocks = 64;
const bool g_data_hash = Env("NET_DATA") != nullptr;

int DataHashes(uint32_t (&out)[kMaxDataBlocks]) {
    uint32_t lo = 0, hi = 0;
    game::DataRange(lo, hi);
    lo &= ~0xFFFu;
    int blocks = std::min(kMaxDataBlocks, int((hi - lo + kDataBlock - 1) / kDataBlock));
    static uint8_t page[0x1000];
    for (int b = 0; b < blocks; b++) out[b] = 2166136261u;
    for (uint32_t va = lo; va < hi && int((va - lo) / kDataBlock) < blocks; va += 0x1000) {
        if (!mem::IsCommitted(va)) continue;
        std::memcpy(page, kernel::HostPtr(va), sizeof page);
        if (rb::Armed()) rb::MaskVolatile(va, page);
        uint32_t& h = out[(va - lo) / kDataBlock];
        for (uint32_t o = 0; o < sizeof page; o += 4) {
            uint32_t v;
            std::memcpy(&v, page + o, 4);
            if ((v >= 0x10000 && v < 0x4000000) || v >= 0x80000000u) continue;
            h = (h ^ v) * 16777619u;
        }
    }
    return blocks;
}

// what both games must agree on: the game's state (netgame.h StateHash)
uint32_t StateHash() { return Fnv(2166136261u, game::StateHash()); }

// and the C runtime's random numbers drawn since the barrier reseeded them (how many, the last one). An arena can draw
// them as its files arrive, which the two games' loaders do a few frames apart (Deadly Alliance: 210 draws before one
// game's first frame, at its frames 1 and 8 in the other's), so they are compared once neither game is loading.
uint32_t RandHash() { return Fnv(Fnv(2166136261u, g_rand_calls.load()), g_rand_last.load()); }

// --- each frame's top as the run of it that counts had it. Rollback runs a frame again after a misprediction; once
// every input before a frame is real, its latest run is the one both games must agree on. Its record holds the check's
// hashes and what a failed check needs to tell where and how two games parted.

constexpr size_t kPiece = 64;          // the check blocks in pieces of 64 bytes, 16 bits of hash each
constexpr uint32_t kFramesKept = 160;  // frame records kept below the frames agreed on

struct Block {
    uint32_t va = 0;
    std::vector<uint8_t> bytes;  // empty when its memory was not there
};
struct FrameRecord {
    Check check;
    uint32_t mxcsr = 0;          // the main thread's MXCSR: the game's SSE math rounds and flushes by the host's
    char about[160] = {};        // what each fighter does (netgame.h DescribeFighters)
    std::vector<Block> blocks;   // the check blocks (netgame.h CheckBlocks)
};
std::map<uint32_t, FrameRecord> g_frames;
uint32_t g_next_check = 0;   // the next check frame to send
uint32_t g_match_mxcsr = 0;  // the main thread's MXCSR as the match in sync began

uint32_t Mxcsr() { return _mm_getcsr(); }

// this run of a frame's top: what the frame's record holds from now on
void RecordFrame(uint32_t frame) {
    FrameRecord& r = g_frames[frame];
    r.check = Check{StateHash(), RandHash(), g_loading};
    r.mxcsr = Mxcsr();
    r.about[0] = 0;
    game::DescribeFighters(r.about, sizeof r.about);
    std::vector<std::pair<uint32_t, uint32_t>> blocks;
    game::CheckBlocks(blocks);
    r.blocks.resize(blocks.size());
    for (size_t i = 0; i < blocks.size(); i++) {
        const auto [va, size] = blocks[i];
        Block& b = r.blocks[i];
        b.va = va;
        if (va && size && mem::IsCommitted(va) && mem::IsCommitted(va + size - 1)) {
            auto* src = static_cast<const uint8_t*>(kernel::HostPtr(va));
            b.bytes.assign(src, src + size);
        } else {
            b.bytes.clear();
        }
    }
}

// a 64-byte piece's hash over the words the checks take (pointers left out, as the state checks leave them: the two
// games' heaps may lie differently), 16 bits of FNV; 0 for a piece of nothing but pointers
uint16_t PieceHash(const std::vector<uint8_t>& bytes, size_t piece) {
    uint32_t h = 2166136261u;
    bool any = false;
    for (size_t o = piece * kPiece; o + 4 <= bytes.size() && o < (piece + 1) * kPiece; o += 4) {
        uint32_t v;
        std::memcpy(&v, bytes.data() + o, 4);
        if ((v >= 0x10000 && v < 0x4000000) || v >= 0x80000000u) continue;
        h = (h ^ v) * 16777619u;
        any = true;
    }
    return any ? uint16_t((h ^ (h >> 16)) & 0xFFFF) : 0;
}

size_t Pieces(const Block& b) { return (b.bytes.size() + kPiece - 1) / kPiece; }

// A frame's blocks as short hashes, into the game's log (so into a desync report): each block's 64-byte pieces, 16
// bits a piece. What a report carried before the two games told each other their pieces, and still does when the
// other game is an older one: the pieces that differ between the two players' reports are where the games parted.
void LogPieceHashes(uint32_t frame, const FrameRecord& r) {
    constexpr size_t kPerLine = 30;
    for (size_t i = 0; i < r.blocks.size(); i++) {
        const Block& b = r.blocks[i];
        const size_t pieces = Pieces(b);
        for (size_t first = 0; first < pieces; first += kPerLine) {
            char hex[kPerLine * 4 + 1] = {};
            size_t n = 0;
            for (size_t p = first; p < pieces && p < first + kPerLine; p++, n++)
                std::snprintf(hex + n * 4, 5, "%04X", PieceHash(b.bytes, p));
            LOGR("netplay: frame %u block %zu at %08X, pieces %zu-%zu: %s", frame, i, b.va, first, first + n - 1, hex);
        }
    }
}

// a frame's blocks word by word, into this match's net log (the player's own file)
void DumpBlocks(uint32_t frame, const FrameRecord& r) {
    if (!g_cfg.log) return;
    for (size_t i = 0; i < r.blocks.size(); i++) {
        const Block& b = r.blocks[i];
        std::fprintf(g_cfg.log, "# dump %u block %zu at %08X\n", frame, i, b.va);
        for (size_t o = 0; o < b.bytes.size(); o += 16) {
            uint32_t v[4] = {};
            std::memcpy(v, b.bytes.data() + o, std::min<size_t>(16, b.bytes.size() - o) & ~size_t(3));
            std::fprintf(g_cfg.log, "# %zu %04zX %08X %08X %08X %08X\n", i, o, v[0], v[1], v[2], v[3]);
        }
    }
    std::fflush(g_cfg.log);
}

// --- a failed check. The two games tell each other what they had (DETAIL), so that each game's log, and so each
// player's desync report, says where they parted and what differed: the first frame whose top differs (each frame's
// state since the check before, compared), the inputs that led to it, what each fighter did then, this PC's floating
// point control and display, and this game's words of every 64-byte piece the other game's hashes disagree with (the
// server's log then has both players' words side by side). The desync is reported (Status::Desynced, which ends the
// match for the launcher) once that is written, or after kDetailWait without the other game's pieces (an older game
// sends none): then the piece hashes, as before. Until then the match plays on, settling nothing (SettleConfirmed),
// so no result goes out of it.
constexpr auto kDetailWait = std::chrono::milliseconds(2000);
constexpr auto kDetailResend = std::chrono::milliseconds(250);
constexpr int kMaxWordLines = 12;      // pieces written out word by word, at most
constexpr int kMaxDetailPacket = 440;  // bytes a DETAIL packet holds at most
constexpr uint8_t kNoRecord = 2;       // a frames entry's loading byte: this game kept no record of that frame
const bool g_test_no_detail = Env("NET_TEST_NO_DETAIL") != nullptr;

struct Detail {
    bool active = false;  // a check failed: its detail is under way
    bool done = false;    // written out, the desync reported
    uint32_t check = 0;   // the failed check's frame
    uint32_t first = 0;   // this game's records from the frame after the check before, up to it
    std::map<uint32_t, FrameRecord> frames;
    std::map<uint32_t, std::pair<Input, Input>> inputs;  // P1's and P2's input of each of those frames, and 4 before
    Clock::time_point t0, last_send;
    uint32_t at = UINT32_MAX;  // the frame whose pieces both compare: the first whose state differs, else the check
    // and the frame before it, where the two states still agreed: a piece that differs there already is told apart
    // from the ones that changed at the parting (some words outside the state hash differ between two games all along)
    uint32_t before = UINT32_MAX;
    bool found = false;        // the frames differed somewhere (else the check's own hashes did)
    bool by_rand = false;      // only the C runtime's random numbers did
};
Detail g_detail;

struct TheirBlock {
    uint32_t size = 0;
    std::vector<uint16_t> hashes;
    std::vector<uint8_t> have;
    size_t got = 0;
};
// the other game's pieces of one frame
struct TheirFrame {
    uint32_t blocks = 0;
    std::map<uint32_t, TheirBlock> pieces;
};
// the other game's detail; it may come before this game saw the check fail
struct TheirDetail {
    uint32_t check = UINT32_MAX;
    bool frames_in = false;
    uint32_t first = 0;
    std::vector<Check> frames;
    std::vector<uint8_t> known;
    std::map<uint32_t, TheirFrame> pieces;  // by frame: the parting one and the one before it
};
TheirDetail g_their_detail;

Input Mine(uint32_t f);
Input Theirs(uint32_t f);

// the other player's input a frame's run that counts used (rollback), else the one that came for it
Input TheirsRan(uint32_t f) {
    auto it = g_used.find(f);
    return it != g_used.end() ? it->second : Theirs(f);
}

std::string InputText(const Input& in) {
    char buf[32];
    if (in.sticks == game::NeutralSticks()) std::snprintf(buf, sizeof buf, "%X", in.buttons);
    else std::snprintf(buf, sizeof buf, "%X/%08X", in.buttons, in.sticks);
    return buf;
}

// this PC's side of a desync: the floating point control the game's math ran with, the display it ran on
std::string PcFacts(uint32_t frame, uint32_t mxcsr) {
    const app::Settings& s = app::Get();
    const app::Mode out = app::Output();
    uint32_t hz = 0;
    if (HWND w = static_cast<HWND>(app::Window())) {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof mi;
        DEVMODEW dm{};
        dm.dmSize = sizeof dm;
        if (GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTOPRIMARY), &mi) &&
            EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
            hz = dm.dmDisplayFrequency;
    }
    const char* mode = s.display_mode == app::DisplayMode::Fullscreen ? "fullscreen"
                       : s.display_mode == app::DisplayMode::Windowed ? "windowed"
                                                                      : "borderless";
    // MXCSR's control (masks, rounding, flush to zero 8000, denormals as zero 0040) apart from its sticky flags, which
    // only say what the math met so far (1F80 is Windows' own control)
    char buf[180];
    std::snprintf(buf, sizeof buf, "MXCSR control %04X flags %02X at frame %u (control %04X as the match began), %s "
                  "%ux%u at %u Hz, vsync %s, cap %u, render %u%%, %s", mxcsr & 0xFFC0, mxcsr & 0x3F, frame,
                  g_match_mxcsr & 0xFFC0, mode, out.width, out.height, hz, s.vsync ? "on" : "off", s.fps_cap,
                  s.render_percent, app::Widescreen() ? "16:9" : "4:3");
    return buf;
}

// what the other game needs from this one: each frame's state from the check before up to the failed one, then the
// pieces of the frame both compare and of the one before it, once this game knows which
void SendPieces(uint32_t frame);
void SendDetail() {
    Detail& d = g_detail;
    d.last_send = Clock::now();
    Writer w = Header(kDetail);
    w.U32(d.check);
    w.U8(kDetailFrames);
    w.U32(d.first);
    const uint32_t n = std::min<uint32_t>(d.check - d.first + 1, 40);
    w.U8(uint8_t(n));
    for (uint32_t f = d.first; f < d.first + n; f++) {
        auto it = d.frames.find(f);
        const Check c = it != d.frames.end() ? it->second.check : Check{};
        w.U32(c.state);
        w.U32(c.rand);
        w.U8(it == d.frames.end() ? kNoRecord : c.loading ? 1 : 0);
    }
    Send(w);
    if (d.at != UINT32_MAX) SendPieces(d.at);
    if (d.before != UINT32_MAX) SendPieces(d.before);
}

void SendPieces(uint32_t frame) {
    const Detail& d = g_detail;
    auto rec = d.frames.find(frame);
    if (rec == d.frames.end()) return;
    const FrameRecord& r = rec->second;
    const size_t blocks = std::min<size_t>(r.blocks.size(), 255);
    Writer w;
    int runs_at = -1;
    auto open = [&] {
        w = Header(kDetail);
        w.U32(d.check);
        w.U8(kDetailPieces);
        w.U32(frame);
        w.U8(uint8_t(blocks));
        runs_at = w.n;
        w.U8(0);
    };
    auto flush = [&] {
        if (runs_at >= 0 && w.b[runs_at]) Send(w);
        runs_at = -1;
    };
    open();
    for (size_t i = 0; i < blocks; i++) {
        const Block& b = r.blocks[i];
        const size_t pieces = std::min<size_t>(Pieces(b), 0xFFFF);
        size_t p = 0;
        do {
            if (w.n + 10 > kMaxDetailPacket || w.b[runs_at] == 255) {
                flush();
                open();
            }
            const size_t room = size_t(kMaxDetailPacket - w.n - 8) / 2;
            const size_t n = std::min({pieces - p, room, size_t(255)});
            w.U8(uint8_t(i));
            w.U32(uint32_t(b.bytes.size()));
            w.U16(uint16_t(p));
            w.U8(uint8_t(n));
            for (size_t k = 0; k < n; k++) w.U16(PieceHash(b.bytes, p + k));
            w.b[runs_at]++;
            p += n;
        } while (p < pieces);
    }
    flush();
}

bool TheirPiecesIn(const TheirDetail& t, uint32_t frame) {
    auto f = t.pieces.find(frame);
    if (f == t.pieces.end() || !f->second.blocks || f->second.pieces.size() < f->second.blocks) return false;
    for (const auto& [i, b] : f->second.pieces)
        if (b.got < b.hashes.size()) return false;
    return true;
}

// a piece of this game's block at a frame differs from the other game's (whose pieces of that frame are all here)
bool PieceDiffers(const TheirDetail& t, uint32_t frame, const FrameRecord& r, size_t block, size_t piece) {
    auto f = t.pieces.find(frame);
    if (f == t.pieces.end() || block >= r.blocks.size()) return false;
    auto b = f->second.pieces.find(uint32_t(block));
    if (b == f->second.pieces.end() || b->second.size != r.blocks[block].bytes.size() || piece >= b->second.hashes.size() ||
        !b->second.have[piece])
        return false;
    return b->second.hashes[piece] != PieceHash(r.blocks[block].bytes, piece);
}

// This game's words of every piece the other game's hashes disagree with at the parting frame, a line a piece: first
// the pieces that changed there, then those that differed at the frame before already, where the states still agreed
// (with the other game's pieces of that frame in), 12 lines in all at most.
void LogDifferingWords(uint32_t frame, const FrameRecord& r, const FrameRecord* before, uint32_t before_frame,
                       const TheirDetail& t) {
    const TheirFrame& tf = t.pieces.at(frame);
    std::string legend;
    for (size_t i = 0; i < r.blocks.size() && legend.size() < 140; i++) {
        char one[40];
        std::snprintf(one, sizeof one, "%s%zu %08X+%zX", i ? ", " : "", i, r.blocks[i].va, r.blocks[i].bytes.size());
        legend += one;
    }
    LOGR("netplay: desync detail: this game's blocks at frame %u: %s%s", frame, legend.c_str(),
         tf.blocks == r.blocks.size() ? "" : " (the other game checked another count of them)");
    struct Piece {
        size_t block, piece;
        bool already;  // differed at the frame before as well
    };
    std::vector<Piece> differ;
    for (size_t i = 0; i < r.blocks.size(); i++) {
        auto it = tf.pieces.find(uint32_t(i));
        if (it != tf.pieces.end() && it->second.size != r.blocks[i].bytes.size())
            LOGR("netplay: desync detail: block %zu holds %zX bytes here, %X in the other game", i,
                 r.blocks[i].bytes.size(), it->second.size);
        for (size_t p = 0; p < Pieces(r.blocks[i]); p++) {
            if (!PieceDiffers(t, frame, r, i, p)) continue;
            const bool already = before && TheirPiecesIn(t, before_frame) && PieceDiffers(t, before_frame, *before, i, p);
            differ.push_back({i, p, already});
        }
    }
    std::stable_partition(differ.begin(), differ.end(), [](const Piece& p) { return !p.already; });
    int lines = 0, fresh = 0;
    for (const Piece& d : differ) {
        fresh += d.already ? 0 : 1;
        if (lines >= kMaxWordLines) continue;
        const Block& b = r.blocks[d.block];
        char words[16 * 9 + 1] = {};
        size_t n = 0;
        for (size_t o = d.piece * kPiece; o + 4 <= b.bytes.size() && o < (d.piece + 1) * kPiece; o += 4) {
            uint32_t v;
            std::memcpy(&v, b.bytes.data() + o, 4);
            n += size_t(std::snprintf(words + n, sizeof words - n, "%s%08X", n ? " " : "", v));
        }
        // short, as a report line holds 200 characters with the log's time in front: "from" the frame before, for a
        // piece that differed there already
        LOGR("netplay: differs %s %u, block %zu +%04zX: %s", d.already ? "from" : "at", d.already ? before_frame : frame,
             d.block, d.piece * kPiece, words);
        lines++;
    }
    if (differ.empty())
        LOGR("netplay: desync detail: every piece agrees at frame %u: what differs is outside the check blocks", frame);
    else
        LOGR("netplay: desync detail: %zu pieces differ at frame %u, %d of them first there%s", differ.size(), frame,
             fresh, differ.size() > size_t(lines) ? " (the rest not written out)" : "");
}

// writes the detail out and reports the desync: with the other game's pieces once they are all here (complete), else
// with what came of them (the words when its pieces of the parting frame came, else this game's piece hashes)
void FinishDetail(bool complete) {
    (void)complete;
    Detail& d = g_detail;
    if (!d.active || d.done) return;
    d.done = true;
    const TheirDetail& t = g_their_detail;
    const bool frames_in = t.check == d.check && t.frames_in && d.at != UINT32_MAX;
    if (frames_in) {
        if (d.found && !d.by_rand)
            LOGR("netplay: desync detail: the two games' states first differ at the top of frame %u (alike from %u)",
                 d.at, d.first);
        else if (d.by_rand)
            LOGR("netplay: desync detail: the states agree from %u to %u, the C runtime's random numbers first differ "
                 "at %u", d.first, d.check, d.at);
        else
            LOGR("netplay: desync detail: frames %u-%u agree in both games' records, the check's hashes do not",
                 d.first, d.check);
    } else {
        d.at = d.check;
        LOGR("netplay: desync detail: %s", g_cfg.watch ? "a spectator's game: the players' games have their own"
                                                       : "the other game told this one nothing of it (an older game?)");
    }
    std::string in;
    for (uint32_t f = d.at >= 4 ? d.at - 4 : 0; f < d.at; f++) {
        auto it = d.inputs.find(f);
        if (it == d.inputs.end()) continue;
        in += (in.empty() ? "" : ", ") + std::to_string(f) + " " + InputText(it->second.first) + " " +
              InputText(it->second.second);
    }
    if (!in.empty()) LOGR("netplay: desync detail: P1's and P2's inputs before frame %u: %s", d.at, in.c_str());
    auto rec = d.frames.find(d.at);
    if (rec != d.frames.end()) {
        if (rec->second.about[0]) LOGR("netplay: desync detail: frame %u: %s", d.at, rec->second.about);
        LOGR("netplay: desync detail: this PC: %s", PcFacts(d.at, rec->second.mxcsr).c_str());
        auto before = d.before != UINT32_MAX ? d.frames.find(d.before) : d.frames.end();
        if (frames_in && TheirPiecesIn(t, d.at))
            LogDifferingWords(d.at, rec->second, before != d.frames.end() ? &before->second : nullptr, d.before, t);
        else
            LogPieceHashes(d.at, rec->second);
        DumpBlocks(d.at, rec->second);
    }
    if (d.at != d.check)
        if (auto c = d.frames.find(d.check); c != d.frames.end()) DumpBlocks(d.check, c->second);
    SetStatus(Status::Desynced);
}

// the other game's detail, as it comes (Receive)
void DetailProgress();
void OnDetail(const uint8_t* b, int n) {
    auto u32 = [&](int o) {
        uint32_t v;
        std::memcpy(&v, b + o, 4);
        return v;
    };
    auto u16 = [&](int o) {
        uint16_t v;
        std::memcpy(&v, b + o, 2);
        return v;
    };
    if (n < 13 || g_test_no_detail) return;
    const uint32_t check = u32(8);
    TheirDetail& t = g_their_detail;
    if (t.check != check) t = TheirDetail{check};
    if (b[12] == kDetailFrames && n >= 18) {
        const uint32_t first = u32(13);
        const int count = b[17];
        if (t.frames_in || count < 1 || 18 + count * 9 > n) return;
        t.first = first;
        t.frames.resize(size_t(count));
        t.known.resize(size_t(count));
        for (int i = 0; i < count; i++) {
            const int o = 18 + i * 9;
            t.frames[size_t(i)] = Check{u32(o), u32(o + 4), b[o + 8] == 1};
            t.known[size_t(i)] = b[o + 8] != kNoRecord;
        }
        t.frames_in = true;
    } else if (b[12] == kDetailPieces && n >= 19) {
        const uint32_t frame = u32(13);
        if (!t.pieces.count(frame) && t.pieces.size() >= 4) return;  // two frames are told, a few at most are kept
        TheirFrame& tf = t.pieces[frame];
        tf.blocks = b[17];
        int o = 19;
        for (int run = 0; run < b[18] && o + 8 <= n; run++) {
            const uint32_t block = b[o], size = u32(o + 1), first = u16(o + 5), count = b[o + 7];
            if (o + 8 + int(count) * 2 > n || size > (1u << 20)) break;
            TheirBlock& tb = tf.pieces[block];
            if (tb.size != size || tb.hashes.empty()) {
                tb = TheirBlock{};
                tb.size = size;
                tb.hashes.resize((size + kPiece - 1) / kPiece);
                tb.have.resize(tb.hashes.size());
            }
            for (uint32_t k = 0; k < count && first + k < tb.hashes.size(); k++) {
                tb.hashes[first + k] = u16(o + 8 + int(k) * 2);
                if (!tb.have[first + k]) tb.got++;
                tb.have[first + k] = 1;
            }
            o += 8 + int(count) * 2;
        }
    }
    DetailProgress();
}

// once the other game's frames are here: the frame both compare; once its pieces of that frame are: done
void DetailProgress() {
    Detail& d = g_detail;
    const TheirDetail& t = g_their_detail;
    if (!d.active || d.done || t.check != d.check) return;
    if (d.at == UINT32_MAX && t.frames_in) {
        uint32_t state_at = UINT32_MAX, rand_at = UINT32_MAX;
        for (uint32_t f = std::max(d.first, t.first); f <= d.check && f - t.first < t.frames.size(); f++) {
            auto mine = d.frames.find(f);
            if (mine == d.frames.end() || !t.known[f - t.first]) continue;
            const Check& a = mine->second.check;
            const Check& c = t.frames[f - t.first];
            if (a.state != c.state) {
                state_at = f;
                break;
            }
            if (rand_at == UINT32_MAX && !a.loading && !c.loading && a.rand != c.rand) rand_at = f;
        }
        d.found = state_at != UINT32_MAX || rand_at != UINT32_MAX;
        d.by_rand = state_at == UINT32_MAX && rand_at != UINT32_MAX;
        d.at = state_at != UINT32_MAX ? state_at : rand_at != UINT32_MAX ? rand_at : d.check;
        if (d.at > 0 && d.frames.count(d.at - 1)) d.before = d.at - 1;
        SendPieces(d.at);
        if (d.before != UINT32_MAX) SendPieces(d.before);
    }
    if (d.at != UINT32_MAX && TheirPiecesIn(t, d.at) && (d.before == UINT32_MAX || TheirPiecesIn(t, d.before)))
        FinishDetail(true);
}

// a check failed at frame `check`: this game's records of it kept, its detail out to the other game
void BeginDetail(uint32_t check) {
    g_detail = Detail{};
    Detail& d = g_detail;
    d.active = true;
    d.check = check;
    d.first = check >= kCheckEvery ? check - kCheckEvery + 1 : 0;
    d.t0 = Clock::now();
    // from the check before (both states agreed there), the frame before the first one compared
    for (auto it = g_frames.lower_bound(d.first ? d.first - 1 : 0); it != g_frames.end() && it->first <= check; ++it)
        d.frames.insert(*it);
    if (!g_cfg.watch) {
        for (uint32_t f = d.first >= 4 ? d.first - 4 : 0; f <= check; f++) {
            const Input mine = Mine(f), theirs = TheirsRan(f);
            d.inputs[f] = g_cfg.side == 0 ? std::make_pair(mine, theirs) : std::make_pair(theirs, mine);
        }
    }
    // a spectator has no other game to ask; <prefix>_NET_TEST_NO_DETAIL=1 (tests) plays an older game's part: no detail
    // sent or heard, the desync reported at once with the piece hashes
    if (g_cfg.watch || g_sock == INVALID_SOCKET || g_test_no_detail) return FinishDetail(false);
    SendDetail();
    DetailProgress();  // the other game's may be here already
}

// every frame, in the match or out of it: the detail sent again for a while (a packet may be lost), and written out
// without the other game's once kDetailWait is over
void DetailPump() {
    Detail& d = g_detail;
    if (!d.active || g_test_no_detail) return;
    const auto now = Clock::now();
    if (now - d.t0 > kDetailWait) {
        FinishDetail(false);
        return;
    }
    if (now - d.last_send > kDetailResend) SendDetail();
}

void Leave(const char* why, Status st = Status::Local) {
    LOGR("netplay: leaving the match in sync at frame %u: %s", g_frame, why);
    FeedEnd(g_frame, st == Status::Local && !g_desynced ? 0 : 1);
    g_left_seq = g_seq;
    StopRollback();
    g_phase = Phase::Local;
    // a failed check's detail still under way: the desync is what this match ends with, once it is written (the other
    // game's may still come, out of the match too), at once when the other game is gone
    if (g_detail.active && !g_detail.done) {
        if (st != Status::Local) FinishDetail(false);
        return;
    }
    SetStatus(st);
}

// Checks. Every frame's top is recorded as it runs; a check frame (every kCheckEvery) goes out once every input before
// it is real, with the state its latest run had. Only a check frame run at a moment it was agreed went out before: a
// right prediction is never run again, so at a high ping most were never checked, and two games that had parted
// played on unchecked for seconds (598 frames in a test). Theirs are compared with ours as both come.
void Checks(uint32_t frame, bool agreed) {
    RecordFrame(frame);
    const uint32_t upto = agreed ? frame : std::min(g_confirmed, frame);
    for (; g_next_check <= upto; g_next_check += kCheckEvery) {
        auto rec = g_frames.find(g_next_check);
        if (rec == g_frames.end()) continue;
        g_my_checks[g_next_check] = rec->second.check;
        SendCheck(g_next_check, rec->second.check);
    }
    for (auto it = g_their_checks.begin(); it != g_their_checks.end();) {
        auto mine_it = g_my_checks.find(it->first);
        if (mine_it == g_my_checks.end()) {
            // ours went by unrecorded or was forgotten (it came very late): nothing left to compare it with
            if (it->first < g_next_check) it = g_their_checks.erase(it);
            else ++it;
            continue;
        }
        const Check& mine = mine_it->second;
        const Check& theirs = it->second;
        const bool rand_counts = !mine.loading && !theirs.loading;
        if ((mine.state != theirs.state || (rand_counts && mine.rand != theirs.rand)) && !g_desynced) {
            LOGE("netplay: DESYNC at frame %u (ours %08X/%08X, theirs %08X/%08X%s)", it->first, mine.state, mine.rand,
                 theirs.state, theirs.rand, rand_counts ? "" : ", loading");
            g_desynced = true;
            g_desync_frame = it->first;
            BeginDetail(it->first);
        }
        it = g_their_checks.erase(it);
    }
}

// --- the match in sync starts

bool WaitForFeed(uint32_t f);

void StartSync(X86Context& ctx) {
    ClearPicks();  // the match set up: the next one picks again
    FinishDetail(false);  // the last match's, if it is still under way
    g_detail = Detail{};
    g_their_detail = TheirDetail{};
    g_frames.clear();
    g_next_check = 0;
    g_match_mxcsr = Mxcsr();
    g_seq++;
    g_frame = 0;
    game::RngFrame(g_seed, 0);
    g_mine.clear();
    g_theirs.clear();
    g_used.clear();
    g_rand_at.clear();
    g_their_ack = 0;
    g_theirs_contiguous = 0;
    g_their_now = 0;
    g_their_adv = 0;
    g_lead_frames = 0;
    g_lead_at = 0;
    g_prev_buttons[0] = g_prev_buttons[1] = 0;
    g_local_sticks_in = false;  // the blocks hold this PC's own sticks until the first frame writes them
    g_fed = 0;
    g_my_checks.clear();
    if (!g_cfg.watch) g_their_checks.clear();  // a spectator's came with the feed, maybe before this
    g_desynced = false;  // the random numbers' counters run from the barrier's reseed (MeetBeforeSetup)
    g_confirmed = 0;
    g_back_frame = kNoFrame;
    g_back_scan = 0;
    g_replaying = false;
    g_had_focus = true;  // a match that starts without it says so at its first frame
    game::MatchBegins();
    Unsettle();
    g_loading = true;  // the match's own start loads
    g_quiet = 0;
    // the opening's inputs are known on both sides (no input): the other player's are here before they run
    g_intro = Env("NET_NO_INTRO") ? 0 : kIntroFrames;
    for (uint32_t f = 0; f < g_intro; f++) g_theirs[f] = Input{};
    g_sync_t0 = g_second_t0 = Clock::now();
    g_second_stalls = g_second_rollbacks = 0;
    g_timed = g_rolled = 0;
    g_reads_seen = kernel::RecordedFileReads();
    g_switches_seen = game::StateSwitches();
    {
        std::lock_guard<std::mutex> lock(g_stats_lock);
        g_stats = Stats{};
    }
    for (int f = 0; f < g_cfg.delay; f++) g_mine[uint32_t(f)] = Input{};
    if (g_cfg.rollback) {
        // what the loader still has in flight is finished and taken back first, so recording starts with nothing
        // in flight (rollback runs the loader on this thread from then on)
        if (!game::LoaderSettle(ctx, std::chrono::seconds(10))) LOGW("netplay: the loader is still busy");
        gclock::Start(0);
        dsound::SetFrameClock(true);  // before arming: the output thread no longer touches the voices
        if (!rb::Arm(ctx, 0)) {
            LOGE("netplay: rollback cannot start, playing in lockstep");
            dsound::SetFrameClock(false);
            gclock::Stop();
            g_cfg.rollback = false;
            g_cfg.delay = std::max(g_cfg.delay, 2);
            for (int f = 0; f < g_cfg.delay; f++) g_mine[uint32_t(f)] = Input{};
        }
    }
    if (g_cfg.rollback) {
        d3d::SetSkipPresent(false);
        dsound::MixFrames(kMixFrames, true);  // frame 0's sound
        g_phase = Phase::Rollback;
        SetStatus(Status::Rollback);
        g_last_slowdown = Clock::now();
        if (g_cfg.watch) LOGR("netplay: watching match %u", g_seq);
        else LOGR("netplay: match %u in sync with rollback, %d frame%s of delay", g_seq, g_cfg.delay,
                  g_cfg.delay == 1 ? "" : "s");
    } else {
        g_phase = Phase::Lockstep;
        SetStatus(Status::Lockstep);
        LOGR("netplay: match %u in lockstep, %d frames of delay", g_seq, g_cfg.delay);
    }
    if (g_cfg.watch) {
        for (WatchSide& w : g_watch) w.meet_in = false;
        WaitForFeed(0);  // frame 0's top: its pads take both players' inputs for it
    }
}

// a spectator at the match's setup: both players' words from their meeting point, then everything as they made it
void WatchMeet(X86Context& ctx) {
    g_phase = Phase::Barrier;
    SetStatus(Status::Barrier);
    LOGI("netplay: match %u about to be set up (%s), waiting for the players' meeting point", g_seq + 1,
         game::Describe().c_str());
    auto t0 = Clock::now();
    WaitScope waiting;
    while (!g_watch[0].meet_in || !g_watch[1].meet_in) {
        Receive();
        if (g_abort || Clock::now() - t0 > std::chrono::seconds(120)) {
            LOGE("netplay: the players never reached the match");
            g_phase = Phase::Local;
            SetStatus(Status::Disconnected);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // each player's game keeps some values of its own side (Chess Kombat's team): a spectator's has neither, so it
    // agrees as each of the two in turn
    game::Agree(ctx, 0, g_watch[0].words, g_watch[1].words, g_cfg.log);
    game::Agree(ctx, 1, g_watch[1].words, g_watch[0].words, g_cfg.log);
    g_seed = g_watch[0].seed;
    game::Reseed(ctx, g_seed);
    g_rand_calls = 0;
    g_rand_last = 0;
    LOGI("netplay: random generator seeded with %08X", g_seed);
    WritePort(0, Input{}, 0);
    WritePort(1, Input{}, 0);
    g_start_pending = true;
}

// A match is about to be set up (netgame.h OnMatchSetup, inside the frame): meet the other game here. Both games reach
// this point after their own time in the menus, so what they did apart differs (the frame counters the engine's
// timers count on, the random generator's state, the picks of a side mode); the game makes both agree now, before
// the match's timers start or anything of it loads. The rest of this frame runs with neutral pads on both, and the
// match in sync starts at the next frame's top (FrameStart).
void MeetBeforeSetup(X86Context& ctx) {
    if (!g_cfg.on || g_phase != Phase::Local) return;
    if (g_cfg.watch) return WatchMeet(ctx);
    g_my_seed = uint32_t(std::random_device{}()) | 1;
    g_my_words.clear();
    game::ReadyWords(g_my_words);
    FeedSend(ReadyPacket(kFeedMeet));
    g_peer_ready = false;
    g_their_delay = -1;
    g_phase = Phase::Barrier;
    SetStatus(Status::Barrier);
    // in a player's log too: a match that never started shows which game got here and which did not
    LOGR("netplay: match %u about to be set up (%s), waiting for the other game", g_seq + 1, game::Describe().c_str());
    auto t0 = Clock::now(), last = Clock::time_point{};
    int noted = 0;  // seconds of waiting said so far
    WaitScope waiting;
    while (!g_peer_ready) {
        if (Clock::now() - last > std::chrono::milliseconds(100)) {
            SendReady();
            last = Clock::now();
        }
        if (Clock::now() - t0 > std::chrono::seconds(noted + 10)) {
            noted += 10;
            LOGR("netplay: %d s at the meeting point, the other game not there yet", noted);
        }
        Receive();
        if (g_abort || Clock::now() - t0 > std::chrono::seconds(60)) {
            LOGE("netplay: the other game never reached the match, playing on alone");
            g_phase = Phase::Local;
            SetStatus(Status::Disconnected);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    game::Agree(ctx, g_cfg.side, g_my_words, g_their_words, g_cfg.log);
    AgreeDelay();
    // the engine's random generator drew differently in the two games' menus: both seed it again with the left
    // player's seed
    uint32_t seed = g_cfg.side == 0 ? g_my_seed : g_their_seed;
    g_seed = seed;
    game::Reseed(ctx, seed);
    // the C runtime's random numbers are counted from here, where both games' draws start alike: the rest of this
    // frame (a loader finishing an arena's props) draws in one game and not yet in the other
    g_rand_calls = 0;
    g_rand_last = 0;
    LOGI("netplay: random generator seeded with %08X", seed);
    WritePort(0, Input{}, 0);
    WritePort(1, Input{}, 0);
    for (int i = 0; i < 3; i++) SendReady();
    g_start_pending = true;
}

// --- rollback

Input Mine(uint32_t f) {
    auto it = g_mine.find(f);
    return it != g_mine.end() ? it->second : Input{};
}

// the other player's input for a frame: theirs if it came, else the last one that came before it, repeated
Input Theirs(uint32_t f) {
    auto it = g_theirs.upper_bound(f);
    if (it == g_theirs.begin()) return Input{};
    return std::prev(it)->second;
}

Input Used(uint32_t f) {
    auto it = g_used.find(f);
    return it != g_used.end() ? it->second : Input{};
}

// the first frame below `end` whose run used another input than the other player's real one (end: none)
uint32_t FirstMispredicted(uint32_t end) {
    for (uint32_t f = g_confirmed; f < end; f++) {
        auto t = g_theirs.find(f);
        if (t == g_theirs.end()) continue;  // not here yet: the prediction stands for now
        auto u = g_used.find(f);
        if (u == g_used.end() || u->second != t->second) return f;
    }
    return end;
}

// frames below `end` that ran with the real input are agreed
void AdvanceConfirmed(uint32_t end) {
    while (g_confirmed < end) {
        auto t = g_theirs.find(g_confirmed);
        auto u = g_used.find(g_confirmed);
        if (t == g_theirs.end() || u == g_used.end() || t->second != u->second) break;
        g_confirmed++;
    }
}

// <prefix>_NET_OLD_SYNC=1 (tests): a frame given up every quarter second at most, however far ahead, to compare
const bool g_old_sync = Env("NET_OLD_SYNC") != nullptr;

// a game gives up frames once it has led this many frames in a row; <prefix>_NET_TEST_LEAD_FRAMES (tests) sets it, 0 as before
const int g_lead_needed = [] {
    const char* v = Env("NET_TEST_LEAD_FRAMES");
    return v ? std::atoi(v) : 45;
}();

// Time sync before running frame `next`: never more than kMaxPredict frames past what both agree on (a rollback
// must stay within the frames kept), and a game running ahead of the other slows down a little. False when the other
// game is gone.
bool Throttle(uint32_t next) {
    auto t0 = Clock::now(), last_send = t0;
    bool waited = false, warned = false;
    WaitScope waiting;
    while (next >= g_confirmed + kMaxPredict && FirstMispredicted(next) == next) {
        waited = true;
        Receive();
        AdvanceConfirmed(next);
        auto now = Clock::now();
        if (now - last_send > std::chrono::milliseconds(30)) {
            SendInputs();
            last_send = now;
        }
        if (!warned && now - t0 > std::chrono::seconds(1)) {
            LOGW("netplay: frame %u waits for the other game's inputs from frame %u", next, g_confirmed);
            warned = true;
        }
        if (g_abort || now - t0 > std::chrono::seconds(15)) {
            Leave(g_abort ? "the match was ended" : "the other game stopped sending", Status::Disconnected);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    if (waited) {
        // the frames from here keep their pace instead of catching up the time waited
        app::PaceShift(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
        std::lock_guard<std::mutex> lock(g_stats_lock);
        g_stats.stalls++;
    }
    // both games estimate how far ahead they run; the one further ahead gives up half the difference, a frame at a
    // time: every quarter second at first, sooner the further ahead it is. A game far ahead of a slower one otherwise
    // sat at kMaxPredict, and each input the other changed ran the 12 frames again (Deadly Alliance against a Steam
    // Deck: 21 frames of difference, 62% of the frames rolled back). This only ever slows a game down. Only a lead that
    // lasts counts: a moment of the other's inputs arriving late looks the same as a lead, and giving up a frame for each
    // one showed as a hitch every second or so on a connection with spikes, the games in step all along
    int diff = LocalAdvantage() - g_their_adv;
    if (next != g_lead_at) {
        g_lead_at = next;
        g_lead_frames = diff >= 2 ? g_lead_frames + 1 : 0;
    }
    const auto every = g_old_sync || diff < 4 ? std::chrono::milliseconds(250)
                       : diff < 8             ? std::chrono::milliseconds(125)
                       : diff < 16            ? std::chrono::milliseconds(66)
                                              : std::chrono::milliseconds(33);
    if (diff >= 2 && g_lead_frames >= g_lead_needed && Clock::now() - g_last_slowdown > every) {
        app::PaceShift(16666667);  // this frame is shown a frame later
        g_last_slowdown = Clock::now();
        std::lock_guard<std::mutex> lock(g_stats_lock);
        g_stats.stalls++;
    }
    return true;
}

// what the frame that just ran did: a state switch starts loading, which lasts until kLoadQuiet frames run in a row
// without a file read (counted on frames run the first time; a frame run again only switches)
void NoteLoading(bool replay) {
    uint64_t reads = kernel::RecordedFileReads();
    uint32_t switches = game::StateSwitches();
    bool read = reads != g_reads_seen, switched = switches != g_switches_seen;
    g_reads_seen = reads;
    g_switches_seen = switches;
    if (switched) {
        g_loading = true;
        g_quiet = 0;
    } else if (g_loading && !replay) {
        g_quiet = read ? 0 : g_quiet + 1;
        if (g_quiet >= kLoadQuiet) g_loading = false;
    }
}

// how long the opening took, and any second of play that took much longer than one (a frame run for the first time:
// the log says where a match went slow, its waits and rollbacks then)
void NoteTiming(uint32_t next) {
    const auto now = Clock::now();
    if (next == g_intro && g_intro) {
        LOGR("netplay: the opening %u frames took %.1f s", g_intro,
             std::chrono::duration<double>(now - g_sync_t0).count());
    }
    if (next == 0 || next % 60 != 0) return;
    Stats s;
    {
        std::lock_guard<std::mutex> lock(g_stats_lock);
        s = g_stats;
    }
    const double took = std::chrono::duration<double>(now - g_second_t0).count();
    if (took > 1.5)
        LOGR("netplay: frames %u-%u took %.1f s (%u waits, %u rollbacks)", next - 60, next - 1, took,
             s.stalls - g_second_stalls, s.rollbacks - g_second_rollbacks);
    g_second_t0 = now;
    g_second_stalls = s.stalls;
    g_second_rollbacks = s.rollbacks;
}

// waits until frame `next` can run on real inputs only: the other player's input for it is here and every frame
// before rests on real inputs; returns early when a frame before turns out mispredicted (the caller runs it again
// first, with the real inputs). False when the other game is gone.
bool WaitForRealInput(uint32_t next) {
    auto t0 = Clock::now(), last_send = t0;
    bool waited = false;
    WaitScope waiting;
    for (;;) {
        Receive();
        if (FirstMispredicted(next) < next) break;
        AdvanceConfirmed(next);
        if (g_confirmed >= next && g_theirs.count(next)) break;
        waited = true;
        auto now = Clock::now();
        if (now - last_send > std::chrono::milliseconds(30)) {
            SendInputs();
            last_send = now;
        }
        if (g_abort || now - t0 > std::chrono::seconds(15)) {
            Leave(g_abort ? "the match was ended" : "the other game stopped sending", Status::Disconnected);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    if (waited) app::PaceShift(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
    return true;
}

void CaptureForTests(uint32_t frame);

// Online Practice: a player's BACK, in an input both games ran with, ends the match in sync at the next frame's top.
// Each game leaves at its own such frame (one may run ahead of the other): neither needs the other's inputs past the
// BACK's frame, which both have, so neither waits for inputs that never come.
bool BackPressed(uint32_t next) {
    for (; g_back_frame == kNoFrame && g_back_scan < g_confirmed && g_back_scan < next; g_back_scan++)
        if ((Mine(g_back_scan).buttons | Theirs(g_back_scan).buttons) & kBackBit) g_back_frame = g_back_scan;
    if (g_back_frame < next) LOGR("netplay: P%d pressed BACK at frame %u", (Mine(g_back_frame).buttons & kBackBit) ? g_cfg.side + 1 : 2 - g_cfg.side, g_back_frame);
    return g_back_frame < next;
}

// the top of a frame in a match with rollback
void RollbackFrameStart(X86Context& ctx) {
    rb::CloseFrame();
    NoteLoading(g_replaying);
    uint32_t next = g_frame + 1;
    if (g_replaying && next >= g_target) {  // caught up: `next` is a new frame
        g_replaying = false;
        double ms = std::chrono::duration<double, std::milli>(Clock::now() - g_replay_t0).count();
        std::lock_guard<std::mutex> lock(g_stats_lock);
        g_stats.replay_ms += ms;
    }
    if (!g_replaying) {
        NoteTiming(next);
        const bool first_run = next > g_timed;  // not the top again after the rollback it began with
        if (first_run && g_timed && !g_loading && Clock::now() - g_timed_t0 > kLateFrame) {
            std::lock_guard<std::mutex> lock(g_stats_lock);
            g_stats.late++;
            if (g_rolled && g_rolled + 2 >= g_timed) g_stats.late_rollback++;  // the frames still behind its drawing
        }
        if (g_cfg.sparring && BackPressed(next)) {
            Settle();
            Leave("a player went back to the select screen");
            return;
        }
        // the match in sync is over when the game says so at a frame both agree on (the next fight of a match, a
        // rematch, stays in sync: both games set it up in the same frame)
        if (g_confirmed + 1 >= next && game::MatchOver()) {
            Settle();
            Leave("the match is over");
            return;
        }
        Receive();
        if (!Throttle(next)) return;
        g_frame_waited = g_loading;
        if (g_frame_waited && !WaitForRealInput(next)) return;
        if (first_run) {
            g_timed = next;
            g_timed_t0 = Clock::now();
            CaptureForTests(next);
        }
        uint32_t bad = FirstMispredicted(next);
        if (bad < next) {
            const auto rollback_t0 = Clock::now();  // a rollback's time counts its restore, the renderer's wait in it
            if (bad < rb::OldestFrame() || !rb::Restore(bad, ctx)) {
                LOGE("netplay: frame %u can no longer be run again (oldest kept %u)", bad, rb::OldestFrame());
                g_desync_frame = bad;
                Leave("rollback out of range", Status::Desynced);
                return;
            }
            {
                std::lock_guard<std::mutex> lock(g_stats_lock);
                g_stats.rollbacks++;
                g_stats.replayed += next - bad;
                g_stats.max_depth = std::max(g_stats.max_depth, next - bad);
            }
            g_target = next;
            g_replaying = true;
            g_replay_t0 = rollback_t0;
            g_rolled = next;
            next = bad;
        }
    }
    // the rand counters (host state) as they were at this frame's top, if it ran before
    if (g_replaying) {
        auto it = g_rand_at.find(next);
        if (it != g_rand_at.end()) {
            g_rand_calls = it->second.first;
            g_rand_last = it->second.second;
        }
    }
    g_rand_at[next] = {g_rand_calls.load(), g_rand_last.load()};
    g_frame = next;
    game::RngFrame(g_seed, g_frame);
    gclock::SetFrame(g_frame);
    rb::OpenFrame(g_frame, ctx);
    d3d::SetSkipPresent(g_replaying);
    dsound::MixFrames(kMixFrames, !g_replaying);
}

uint32_t EnvFrame(const char* name) {
    const char* v = Env(name);
    return v ? uint32_t(std::atoi(v)) : 0xFFFFFFFFu;
}

// <prefix>_NET_CAPTURE_AT=<frame>,<frame>,... (tests): what is drawn after each of those frames is saved as F3 saves it
// (XBE_CAPTURE_DIR): the next frame, then the pictures of the 20 after it, so the frames shown after rollbacks can be
// looked at. Called at a frame's first run, before any rollback it begins with.
void CaptureForTests(uint32_t frame) {
    static const std::vector<uint32_t> at = [] {
        std::vector<uint32_t> v;
        for (const char* p = Env("NET_CAPTURE_AT"); p && *p;) {
            char* end = nullptr;
            v.push_back(uint32_t(std::strtoul(p, &end, 10)));
            if (end == p || *end != ',') break;
            p = end + 1;
        }
        return v;
    }();
    if (std::find(at.begin(), at.end(), frame) == at.end()) return;
    d3d::WaitForRenderer();  // the next present is this frame's, so the capture is of the frame after it
    gpu::g_capture = true;
    LOGR("netplay: capture asked at frame %u", frame);
    if (g_cfg.log) std::fprintf(g_cfg.log, "# capture asked at frame %u\n", frame);
}

// diagnostics at the top of frame g_frame (every run of it: the last one written is the final run)
void Diagnostics() {
    // <prefix>_NET_MEMDUMP=<frame>: the game's static data at the top of that frame, to <log>.mem
    static const uint32_t memdump_at = EnvFrame("NET_MEMDUMP");
    if (g_frame == memdump_at) {
        const char* log = Env("NET_LOG");
        std::string path = std::string(log ? log : "net") + ".mem";
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            uint32_t lo = 0, hi = 0;
            game::DataRange(lo, hi);
            static uint8_t page[0x1000];
            for (uint32_t va = lo & ~0xFFFu; va < hi; va += 0x1000) {
                if (mem::IsCommitted(va)) std::memcpy(page, kernel::HostPtr(va), sizeof page);
                else std::memset(page, 0, sizeof page);
                std::fwrite(page, 1, sizeof page, f);
            }
            std::fclose(f);
        }
    }
    if (!g_cfg.log) return;
    // <prefix>_NET_SIDEDUMP=<frame>: a side mode's board; <prefix>_NET_DUMP=<frame>: what a desync dump writes
    static const uint32_t sidedump_at = EnvFrame("NET_SIDEDUMP"), dump_at = EnvFrame("NET_DUMP");
    for (int which = 0; which < 2; which++) {
        if (g_frame != (which ? dump_at : sidedump_at)) continue;
        std::vector<std::pair<uint32_t, uint32_t>> blocks;
        if (which) game::CheckBlocks(blocks);
        else game::SideBlocks(blocks);
        for (size_t b = 0; b < blocks.size(); b++) {
            auto [va, size] = blocks[b];
            if (!va || !size || !mem::IsCommitted(va) || !mem::IsCommitted(va + size - 1)) continue;
            for (uint32_t o = 0; o + 4 <= size; o += 4)
                std::fprintf(g_cfg.log, "# %s %zu %04X %08X\n", which ? "dd" : "sd", b, o, kernel::R32(va + o));
        }
    }
    // <prefix>_NET_DATA=1: the data section's block hashes
    if (g_data_hash) {
        uint32_t blocks[kMaxDataBlocks];
        int n = DataHashes(blocks);
        uint32_t all = 2166136261u;
        for (int i = 0; i < n; i++) all = Fnv(all, blocks[i]);
        uint32_t lo = 0, hi = 0;
        game::DataRange(lo, hi);
        std::fprintf(g_cfg.log, "%u data %08X from %08X blocks", g_frame, all, lo & ~0xFFFu);
        for (int i = 0; i < n; i++) std::fprintf(g_cfg.log, " %08X", blocks[i]);
        std::fprintf(g_cfg.log, "\n");
    }
}

// The game's window giving the keyboard and the controllers away during a match (to the launcher, an overlay, another
// window: the controllers read nothing then): noted in the game's log, as it explains a player's input going missing
void WatchFocus() {
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    const bool has = pid == GetCurrentProcessId();
    if (has == g_had_focus) return;
    g_had_focus = has;
    const char* what = has ? "has the focus again" : "lost the focus to another window";
    LOGR("netplay: frame %u: the game window %s", g_frame, what);
    if (g_cfg.log) std::fprintf(g_cfg.log, "# frame %u: the game window %s\n", g_frame, what);
}

// the per-frame log line: frame, inputs, how it ran, then the game's state fields
void LogFrame(const Input& mine, const Input& theirs, bool predicted) {
    if (!g_cfg.log) return;
    std::fprintf(g_cfg.log, "%u%s %08X %08X %08X %08X%s%s confirmed %u rand %u ", g_frame, g_replaying ? " again" : "",
                 mine.buttons, mine.sticks, theirs.buttons, theirs.sticks, predicted ? " predicted" : "",
                 g_frame_waited && !g_replaying ? " loading" : "", g_confirmed, g_rand_calls.load());
    game::LogState(g_cfg.log);
    std::fprintf(g_cfg.log, "\n");
}

// <prefix>_NET_TEST_FRAME_COST=<microseconds> (tests): every frame a match runs, again or not, takes that much longer,
// as on a slower processor
const int g_test_frame_cost = [] {
    const char* v = Env("NET_TEST_FRAME_COST");
    return v ? std::atoi(v) : 0;
}();

// <prefix>_NET_TEST_POKE=<frame>:<hex address>:<hex value> (tests, in one game): that 32-bit word written at the top of
// every run of every frame from that one on, so this game's state parts from the other game's there, whatever rollback
// does: what a desync's report must then name, frame and word
struct TestPoke {
    uint32_t frame = UINT32_MAX, va = 0, value = 0;
};
const TestPoke g_test_poke = [] {
    TestPoke p;
    unsigned f = 0, va = 0, value = 0;
    if (const char* v = Env("NET_TEST_POKE"); v && std::sscanf(v, "%u:%x:%x", &f, &va, &value) == 3) p = {f, va, value};
    return p;
}();

void TestPokeFrame() {
    if (g_frame >= g_test_poke.frame && mem::IsCommitted(g_test_poke.va)) kernel::W32(g_test_poke.va, g_test_poke.value);
}

// after the pad update in a match with rollback
void RollbackPads() {
    if (g_test_frame_cost) {
        const auto until = Clock::now() + std::chrono::microseconds(g_test_frame_cost);
        while (Clock::now() < until) {
        }
    }
    if (!g_replaying) {
        const uint32_t f = g_frame + uint32_t(g_cfg.delay);
        const Input in = ReadLocal();
        g_mine[f] = f < g_intro ? Input{} : in;  // the opening: no input (StartSync)
        SendInputs();
        FeedOwn(false);
        WatchFocus();
    }
    Input mine = Mine(g_frame), theirs = Theirs(g_frame);
    g_used[g_frame] = theirs;
    uint32_t mine_prev = g_frame ? Mine(g_frame - 1).buttons : 0, theirs_prev = g_frame ? Used(g_frame - 1).buttons : 0;
    WritePort(g_cfg.side, mine, mine_prev);
    WritePort(1 - g_cfg.side, theirs, theirs_prev);
    AdvanceConfirmed(g_frame);
    bool agreed = g_confirmed >= g_frame;  // the top of this frame rests on real inputs only
    TestPokeFrame();
    Checks(g_frame, agreed);
    SettleConfirmed(g_frame);
    Diagnostics();
    LogFrame(mine, theirs, !g_theirs.count(g_frame));
    if (!g_replaying) {
        std::lock_guard<std::mutex> lock(g_stats_lock);
        g_stats.frames = g_frame;
        g_stats.advantage = LocalAdvantage() - g_their_adv;
        if (g_frame % 600 == 0 && g_frame) {
            char line[256];
            std::snprintf(line, sizeof line,
                          "frame %u: %u rollbacks, %u frames run again (deepest %u, %.2f ms a rollback), %u frames "
                          "late (%u after a rollback), %u waits, running %d frames ahead, agreed up to %u",
                          g_frame, g_stats.rollbacks, g_stats.replayed, g_stats.max_depth,
                          g_stats.rollbacks ? g_stats.replay_ms / g_stats.rollbacks : 0.0, g_stats.late,
                          g_stats.late_rollback, g_stats.stalls, g_stats.advantage, g_confirmed);
            LOGR("netplay: %s", line);
            if (g_cfg.log) std::fprintf(g_cfg.log, "# %s\n", line);
        }
    }
    // forget what both sides are done with
    if (g_confirmed > 64) {
        uint32_t keep = g_confirmed - 64;
        g_mine.erase(g_mine.begin(), g_mine.lower_bound(std::min(keep, g_their_ack > 64 ? g_their_ack - 64 : 0)));
        g_theirs.erase(g_theirs.begin(), g_theirs.lower_bound(keep));
        g_used.erase(g_used.begin(), g_used.lower_bound(keep));
        g_my_checks.erase(g_my_checks.begin(), g_my_checks.lower_bound(keep));
        g_rand_at.erase(g_rand_at.begin(), g_rand_at.lower_bound(keep));
    }
    if (g_confirmed > kFramesKept) g_frames.erase(g_frames.begin(), g_frames.lower_bound(g_confirmed - kFramesKept));
    g_lockstep_frame = g_confirmed;
}

// --- lockstep (version 1)

void LockstepPads() {
    // this frame's local input is used delay frames from now (the opening's: no input, StartSync)
    const uint32_t f = g_frame + uint32_t(g_cfg.delay);
    const Input in = ReadLocal();
    g_mine[f] = f < g_intro ? Input{} : in;
    SendInputs();
    FeedOwn(false);
    WatchFocus();
    auto t0 = Clock::now(), last = t0;
    bool warned = false;
    WaitScope waiting;
    while (!g_theirs.count(g_frame)) {
        Receive();
        if (g_theirs.count(g_frame)) break;
        auto now = Clock::now();
        if (now - last > std::chrono::milliseconds(30)) {
            SendInputs();
            last = now;
        }
        if (!warned && now - t0 > std::chrono::seconds(1)) {
            LOGW("netplay: waiting for the other game's input of frame %u", g_frame);
            warned = true;
        }
        if (g_abort || now - t0 > std::chrono::seconds(15)) {
            Leave(g_abort ? "the match was ended" : "the other game stopped sending", Status::Disconnected);
            return;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    const Input mine = g_mine[g_frame];
    const Input theirs = g_theirs[g_frame];
    if (g_cfg.sparring && g_back_frame == kNoFrame && ((mine.buttons | theirs.buttons) & kBackBit)) g_back_frame = g_frame;
    WritePort(g_cfg.side, mine, g_prev_buttons[g_cfg.side]);
    g_prev_buttons[g_cfg.side] = mine.buttons;
    WritePort(1 - g_cfg.side, theirs, g_prev_buttons[1 - g_cfg.side]);
    g_prev_buttons[1 - g_cfg.side] = theirs.buttons;
    g_confirmed = g_frame;
    TestPokeFrame();
    Checks(g_frame, true);
    Settle();
    Diagnostics();
    LogFrame(mine, theirs, false);
    // forget what both sides are done with
    if (g_frame > 64) {
        g_mine.erase(g_mine.begin(), g_mine.lower_bound(std::min(g_frame - 64, g_their_ack > 64 ? g_their_ack - 64 : 0)));
        g_theirs.erase(g_theirs.begin(), g_theirs.lower_bound(g_frame - 64));
    }
    if (g_frame > kFramesKept) g_frames.erase(g_frames.begin(), g_frames.lower_bound(g_frame - kFramesKept));
    g_frame++;
    game::RngFrame(g_seed, g_frame);
    g_lockstep_frame = g_frame;
    std::lock_guard<std::mutex> lock(g_stats_lock);
    g_stats.frames = g_frame;
}

// --- a spectator's match: both players' inputs from the feed, every frame run once

Input WatchInput(int side, uint32_t f) {
    auto it = g_watch[side].inputs.find(f);
    return it != g_watch[side].inputs.end() ? it->second : Input{};
}

uint32_t WatchHave() { return std::min(g_watch[0].contiguous, g_watch[1].contiguous); }

// Waits until both players' inputs for frame f are here; once they ran out, until kWatchLead more frames are (the
// feed comes in bursts, so a spectator stays a little behind instead of stopping at every burst). False when they
// stopped coming: the players' match ended before this frame, or nothing came for half a minute.
bool WaitForFeed(uint32_t f) {
    auto t0 = Clock::now();
    bool waited = false, warned = false;
    WaitScope waiting;
    for (;;) {
        Receive();
        const uint32_t have = WatchHave();
        const bool ended = (g_watch[0].ended && g_watch[1].ended) || (g_watch[0].ended && g_watch[0].end_how) ||
                           (g_watch[1].ended && g_watch[1].end_how);
        if (have > f && (!g_rebuffer || have >= f + kWatchLead || ended)) break;
        if (ended) {
            Leave("the players' match ended before this frame", Status::Disconnected);
            return false;
        }
        g_rebuffer = true;
        waited = true;
        const auto now = Clock::now();
        if (!warned && now - t0 > std::chrono::seconds(2)) {
            LOGW("netplay: frame %u waits for the players' inputs (P1's up to %u, P2's up to %u)", f,
                 g_watch[0].contiguous, g_watch[1].contiguous);
            warned = true;
        }
        if (g_abort || now - t0 > std::chrono::seconds(30)) {
            Leave(g_abort ? "the match was ended" : "the players' inputs stopped coming", Status::Disconnected);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    g_rebuffer = false;
    if (waited) {
        app::PaceShift(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
        std::lock_guard<std::mutex> lock(g_stats_lock);
        g_stats.stalls++;
    }
    return true;
}

// far behind the players (a spectator who came late, or whose game set the match up slowly): every frame runs unseen
// and unheard, as frames run again do (no present, no pacing: as fast as this PC runs them), until it is close to
// them again, so the match comes on where the players are instead of playing fast to there (the screen holds its
// last picture meanwhile)
bool FastForward(uint32_t next) {
    static Clock::time_point since;
    const uint32_t have = WatchHave(), behind = have > next ? have - next : 0;
    if (!g_fast && behind > kFastFrom) {
        g_fast = true;
        since = Clock::now();
        LOGR("netplay: %u frames behind the players: catching up unseen", behind);
    } else if (g_fast && behind < kFastUntil) {
        g_fast = false;
        LOGR("netplay: caught up with the players in %.1f s (%u frames behind)",
             std::chrono::duration<double>(Clock::now() - since).count(), behind);
    }
    return g_fast;
}

// the top of a frame a spectator runs
void WatchFrameStart(X86Context& ctx) {
    const bool armed = rb::Armed();
    if (armed) rb::CloseFrame();
    const uint32_t next = g_frame + 1;
    if (game::MatchOver()) {  // every frame a spectator runs rests on both players' real inputs
        Settle();
        Leave("the match is over");
        return;
    }
    if (g_back_frame < next) {
        Settle();
        Leave("a player went back to the select screen");
        return;
    }
    if (!WaitForFeed(next)) return;
    const bool unseen = FastForward(next);
    g_frame = next;
    game::RngFrame(g_seed, g_frame);
    if (armed) {
        gclock::SetFrame(g_frame);
        rb::OpenFrame(g_frame, ctx);
        d3d::SetSkipPresent(unseen);
        dsound::MixFrames(kMixFrames, !unseen);
    }
}

// after the pad update in a spectator's match: both ports get their player's input
void WatchPads() {
    const Input p1 = WatchInput(0, g_frame), p2 = WatchInput(1, g_frame);
    if (g_cfg.sparring && g_back_frame == kNoFrame && ((p1.buttons | p2.buttons) & kBackBit)) g_back_frame = g_frame;
    WritePort(0, p1, g_frame ? WatchInput(0, g_frame - 1).buttons : 0);
    WritePort(1, p2, g_frame ? WatchInput(1, g_frame - 1).buttons : 0);
    g_confirmed = g_frame;
    Checks(g_frame, true);  // against the players' own, from the feed
    Settle();
    Diagnostics();
    LogFrame(p1, p2, false);
    {
        std::lock_guard<std::mutex> lock(g_stats_lock);
        g_stats.frames = g_frame;
        if (g_frame % 600 == 0 && g_frame) {
            LOGR("netplay: watching frame %u, %u frames behind the players, %u waits", g_frame,
                 WatchHave() > g_frame ? WatchHave() - g_frame : 0, g_stats.stalls);
        }
    }
    if (g_frame > 64) {
        const uint32_t keep = g_frame - 64;
        for (WatchSide& w : g_watch) w.inputs.erase(w.inputs.begin(), w.inputs.lower_bound(keep));
        g_my_checks.erase(g_my_checks.begin(), g_my_checks.lower_bound(keep));
    }
    if (g_frame > kFramesKept) g_frames.erase(g_frames.begin(), g_frames.lower_bound(g_frame - kFramesKept));
    g_lockstep_frame = g_frame;
}

// --- frame hooks

void FrameStart(uint64_t) {
    ApplyPending();
    if (!g_cfg.on) return;
    DetailPump();
    X86Context* ctx = MainContext();
    if (g_cfg.watch && InSync()) {
        if (ctx) WatchFrameStart(*ctx);
        return;
    }
    if (g_phase == Phase::Rollback) {
        if (ctx) RollbackFrameStart(*ctx);
        return;
    }
    if (g_phase == Phase::Lockstep && game::MatchOver()) {  // every frame's top is agreed in lockstep
        Settle();
        Leave("the match is over");
        return;
    }
    if (g_phase == Phase::Lockstep && g_back_frame < g_frame) {
        Settle();
        Leave("a player went back to the select screen");
        return;
    }
    Receive();
    if (g_start_pending && ctx) {  // met the other game at the match setup in the frame that just ran
        g_start_pending = false;
        StartSync(*ctx);
        return;
    }
    if (g_phase == Phase::Local && !g_cfg.watch && Clock::now() - g_last_hello > std::chrono::milliseconds(500)) {
        SendHello();
        g_last_hello = Clock::now();
    }
}

// The frames in sync write both players' inputs into the blocks, port 0's sticks included: the pad update would turn
// those into this player's directions (the right player's buttons took the left player's stick). It reads this PC's
// sticks there instead, once more after a match (the last frame wrote them).
void BeforePads(uint64_t) {
    if (!g_cfg.on || !g_local_sticks_in) return;
    game::RestoreLocalSticks(g_local_sticks);
    if (!InSync()) g_local_sticks_in = false;
}

void PadsRead(uint64_t) {
    if (!g_cfg.on) return;
    if (g_cfg.watch) {
        if (InSync()) WatchPads();
        return;
    }
    if (g_phase == Phase::Rollback) RollbackPads();
    else if (g_phase == Phase::Lockstep) LockstepPads();
}

const bool g_registered = [] {
    Load();  // from the environment, for tests; the launcher link configures later otherwise
    game::OnMatchSetup(&MeetBeforeSetup);
    OnFrameStart(&FrameStart);
    OnBeforePads(&BeforePads);
    OnPadsRead(&PadsRead);
    return true;
}();

}  // namespace

bool Enabled() { return g_status.load() != int(Status::Off); }

void Configure(const Settings& st) {
    ClearPicks();
    g_abort = false;
    std::lock_guard<std::mutex> lock(g_pending_lock);
    g_pending_close = false;
    g_pending = st;
}

void SetLocalPick(const uint32_t* words, int n) {
    {
        std::lock_guard<std::mutex> lock(g_pick_lock);
        g_my_pick.assign(words, words + std::max(0, std::min(n, kMaxPickWords)));
    }
    g_last_hello = Clock::time_point{};  // out with the next frame's HELLO
    LOGI("netplay: this player picked %08X", n > 0 ? words[0] : 0);
}

bool PeerPick(uint32_t* words, int& n) {
    std::lock_guard<std::mutex> lock(g_pick_lock);
    if (!g_their_pick_in) return false;
    n = std::min(n, int(g_their_pick.size()));
    for (int i = 0; i < n; i++) words[i] = g_their_pick[i];
    return true;
}

bool WatchPick(int side, uint32_t* words, int& n) {
    std::lock_guard<std::mutex> lock(g_pick_lock);
    const std::vector<uint32_t>& pick = g_watch[side ? 1 : 0].pick;
    if (pick.empty()) return false;
    n = std::min(n, int(pick.size()));
    for (int i = 0; i < n; i++) words[i] = pick[i];
    return true;
}

void Disable() {
    g_abort = true;  // a frame waiting for the other game stops waiting
    std::lock_guard<std::mutex> lock(g_pending_lock);
    g_pending.reset();
    g_pending_close = true;
}

Status GetStatus() { return Status(g_status.load()); }
uint32_t DesyncFrame() { return g_desync_frame.load(); }
uint32_t LockstepFrame() { return g_lockstep_frame.load(); }
bool Waiting() { return g_waiting.load(); }

int LocalSide() { return g_cfg.on && !g_cfg.watch ? g_cfg.side : -1; }

bool GetSettled(Settled& out) {
    std::lock_guard<std::mutex> lock(g_settled_lock);
    if (!g_settled) return false;
    out = g_settled_state;
    return true;
}

void CountRand(uint32_t value) {
    g_rand_calls++;
    g_rand_last = value;
}

Stats GetStats() {
    std::lock_guard<std::mutex> lock(g_stats_lock);
    return g_stats;
}

}  // namespace mk3d::net
