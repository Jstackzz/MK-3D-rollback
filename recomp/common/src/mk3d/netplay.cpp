// Netplay: rollback (version 2) or lockstep (version 1) over UDP, shared by the games; see netplay.h for the model and
// the configuration, netgame.h for what each game supplies.
//
// Packets (little endian): u32 magic "MKDR", u8 type, u8 side, u16 barrier sequence, then
//   1 HELLO   [u8 n, n x u32]             sent while running locally, so each side knows the other is there; with the
//             words of this game's player's pick on the select screen once there is one (SetLocalPick)
//   2 READY   u32 seed, u8 n, n x u32     "my match is about to be set up": the seed this game proposes and the game's
//             own words (its mode, picks, frame counters, a side mode's setup: netgame.h ReadyWords); the barrier ends
//             when both have one
//   3 INPUT   u32 ack, u32 now, i32 advantage, u32 first, u8 n, n x (u32 buttons, u32 sticks)
//             the sender's inputs for frames first .. first+n-1, the highest frame of ours it holds without gaps,
//             the frame it runs and how many frames it thinks it runs ahead of us
//   4 CHECK   u32 frame, u32 state, u32 rand, u8 loading
//             at the top of a frame both games agree on: the game's state hash, the C runtime's random numbers drawn
//             since the barrier reseeded them, and whether the match was still loading (Checks)
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

#include "app/app.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
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
constexpr int kWindow = 16;                      // inputs repeated in every INPUT packet
constexpr uint32_t kCheckEvery = 30;
constexpr uint32_t kMaxPredict = 12;             // frames past the last agreed one (rollback keeps 16)
constexpr uint32_t kMixFrames = 48000 / 60;      // the sound of one frame
constexpr int kMaxReadyWords = 64;

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
bool g_had_focus = true;           // the game's window was in front at the last frame (WatchFocus)
uint32_t g_their_now = 0;          // the frame the other game said it runs
int g_their_adv = 0;               // how far ahead of us it thinks it runs
Clock::time_point g_replay_t0, g_last_slowdown;
std::map<uint32_t, std::pair<uint32_t, uint32_t>> g_rand_at;  // the rand counters at each frame's top
// Loading: what a state switch's new task loads must never be run again by a rollback: the heaps, the graphics
// device and the files it touches outside guest memory do not go back with it (a Deception chess battle's load
// crashed in a corrupted heap). From a state switch (the switching frame itself ends there) and from the match's
// start, every frame waits for the other player's real input instead of predicting it, until kLoadQuiet frames
// passed without a file read. Reads alone (music streams through the main thread while recording) start nothing.
constexpr uint32_t kLoadQuiet = 60;
bool g_loading = false;
uint32_t g_quiet = 0;              // frames in a row without a file read while loading
uint64_t g_reads_seen = 0;
uint32_t g_switches_seen = 0;
bool g_frame_waited = false;       // the frame running now waited for the real input (for the log)

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

// the top of a frame both games agree on: what the launcher may report from it
void Settle() {
    Settled s;
    game::Settle(s);
    std::lock_guard<std::mutex> lock(g_settled_lock);
    g_settled_state = s;
    g_settled = true;
}

void Unsettle() {
    std::lock_guard<std::mutex> lock(g_settled_lock);
    g_settled = false;
}

bool InSync();
void FeedEnd(uint32_t frame, uint8_t how);

void CloseNow() {
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

Writer ReadyPacket(uint8_t type) {
    Writer w = Header(type);
    w.U32(g_my_seed);
    uint8_t n = uint8_t(std::min<size_t>(g_my_words.size(), kMaxReadyWords));
    w.U8(n);
    for (uint8_t i = 0; i < n; i++) w.U32(g_my_words[i]);
    return w;
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
    uint32_t first = last + 1 >= uint32_t(kWindow) ? last + 1 - kWindow : 0;
    first = std::max(first, g_their_ack > uint32_t(kWindow) ? g_their_ack - 1 : 0u);
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

Input ReadLocal() {
    Input in;
    game::ReadLocal(in.buttons, in.sticks);
    g_local_sticks = in.sticks;
    g_local_sticks_in = true;
    return in;
}

void WritePort(int port, const Input& in, uint32_t prev) { game::WritePort(port, in.buttons, in.sticks, prev); }

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

// what a check covered, kept a while: written out when the other game disagrees
struct CheckCopy {
    uint32_t frame = 0;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> blocks;  // va, bytes
};
std::deque<CheckCopy> g_check_copies;

void KeepCheckCopy(uint32_t frame) {
    CheckCopy c;
    c.frame = frame;
    std::vector<std::pair<uint32_t, uint32_t>> blocks;
    game::CheckBlocks(blocks);
    for (auto [va, size] : blocks) {
        std::vector<uint8_t> bytes;
        if (va && size && mem::IsCommitted(va) && mem::IsCommitted(va + size - 1)) {
            auto* src = static_cast<const uint8_t*>(kernel::HostPtr(va));
            bytes.assign(src, src + size);
        }
        c.blocks.push_back({va, std::move(bytes)});
    }
    g_check_copies.push_back(std::move(c));
    while (g_check_copies.size() > 16) g_check_copies.pop_front();
}

void DumpCheckCopy(uint32_t frame) {
    if (!g_cfg.log) return;
    for (const CheckCopy& c : g_check_copies) {
        if (c.frame != frame) continue;
        for (size_t i = 0; i < c.blocks.size(); i++) {
            const auto& [va, bytes] = c.blocks[i];
            std::fprintf(g_cfg.log, "# dump %u block %zu at %08X\n", frame, i, va);
            for (size_t o = 0; o + 16 <= bytes.size(); o += 16) {
                uint32_t v[4];
                std::memcpy(v, bytes.data() + o, 16);
                std::fprintf(g_cfg.log, "# %zu %04zX %08X %08X %08X %08X\n", i, o, v[0], v[1], v[2], v[3]);
            }
        }
        std::fflush(g_cfg.log);
    }
}

void Leave(const char* why, Status st = Status::Local) {
    LOGR("netplay: leaving the match in sync at frame %u: %s", g_frame, why);
    FeedEnd(g_frame, st == Status::Local ? 0 : 1);
    g_left_seq = g_seq;
    StopRollback();
    g_phase = Phase::Local;
    SetStatus(st);
}

// checks for frames both games agree on: send ours, compare theirs
void Checks(uint32_t frame, bool agreed) {
    if (agreed && frame % kCheckEvery == 0 && !g_my_checks.count(frame)) {
        const Check c{StateHash(), RandHash(), g_loading};
        g_my_checks[frame] = c;
        KeepCheckCopy(frame);
        SendCheck(frame, c);
    }
    for (auto it = g_their_checks.begin(); it != g_their_checks.end();) {
        auto mine_it = g_my_checks.find(it->first);
        if (mine_it == g_my_checks.end()) {
            ++it;
            continue;
        }
        const Check& mine = mine_it->second;
        const Check& theirs = it->second;
        const bool rand_counts = !mine.loading && !theirs.loading;
        if ((mine.state != theirs.state || (rand_counts && mine.rand != theirs.rand)) && !g_desynced) {
            LOGE("netplay: DESYNC at frame %u (ours %08X/%08X, theirs %08X/%08X%s)", it->first, mine.state, mine.rand,
                 theirs.state, theirs.rand, rand_counts ? "" : ", loading");
            DumpCheckCopy(it->first);
            g_desynced = true;
            g_desync_frame = it->first;
            SetStatus(Status::Desynced);
        }
        it = g_their_checks.erase(it);
    }
}

// --- the match in sync starts

bool WaitForFeed(uint32_t f);

void StartSync(X86Context& ctx) {
    ClearPicks();  // the match set up: the next one picks again
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
    g_prev_buttons[0] = g_prev_buttons[1] = 0;
    g_local_sticks_in = false;  // the blocks hold this PC's own sticks until the first frame writes them
    g_fed = 0;
    g_my_checks.clear();
    if (!g_cfg.watch) g_their_checks.clear();  // a spectator's came with the feed, maybe before this
    g_desynced = false;  // the random numbers' counters run from the barrier's reseed (MeetBeforeSetup)
    g_confirmed = 0;
    g_replaying = false;
    g_had_focus = true;  // a match that starts without it says so at its first frame
    game::MatchBegins();
    Unsettle();
    g_loading = true;  // the match's own start loads
    g_quiet = 0;
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
    g_phase = Phase::Barrier;
    SetStatus(Status::Barrier);
    g_peer_ready = false;
    LOGI("netplay: match %u about to be set up (%s), waiting for the other game", g_seq + 1, game::Describe().c_str());
    auto t0 = Clock::now(), last = Clock::time_point{};
    while (!g_peer_ready) {
        if (Clock::now() - last > std::chrono::milliseconds(100)) {
            SendReady();
            last = Clock::now();
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

// Time sync before running frame `next`: never more than kMaxPredict frames past what both agree on (a rollback
// must stay within the frames kept), and a game running ahead of the other slows down a little. False when the other
// game is gone.
bool Throttle(uint32_t next) {
    auto t0 = Clock::now(), last_send = t0;
    bool waited = false, warned = false;
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
    // time and not more often than every quarter second
    int diff = LocalAdvantage() - g_their_adv;
    if (diff >= 2 && Clock::now() - g_last_slowdown > std::chrono::milliseconds(250)) {
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

// waits until frame `next` can run on real inputs only: the other player's input for it is here and every frame
// before rests on real inputs; returns early when a frame before turns out mispredicted (the caller runs it again
// first, with the real inputs). False when the other game is gone.
bool WaitForRealInput(uint32_t next) {
    auto t0 = Clock::now(), last_send = t0;
    bool waited = false;
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
        uint32_t bad = FirstMispredicted(next);
        if (bad < next) {
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
            g_replay_t0 = Clock::now();
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

// after the pad update in a match with rollback
void RollbackPads() {
    if (!g_replaying) {
        g_mine[g_frame + uint32_t(g_cfg.delay)] = ReadLocal();
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
    Checks(g_frame, agreed);
    if (agreed) Settle();
    Diagnostics();
    LogFrame(mine, theirs, !g_theirs.count(g_frame));
    if (!g_replaying) {
        std::lock_guard<std::mutex> lock(g_stats_lock);
        g_stats.frames = g_frame;
        g_stats.advantage = LocalAdvantage() - g_their_adv;
        if (g_frame % 600 == 0 && g_frame) {
            char line[256];
            std::snprintf(line, sizeof line,
                          "frame %u: %u rollbacks, %u frames run again (deepest %u, %.2f ms a rollback), %u waits, "
                          "running %d frames ahead, agreed up to %u",
                          g_frame, g_stats.rollbacks, g_stats.replayed, g_stats.max_depth,
                          g_stats.rollbacks ? g_stats.replay_ms / g_stats.rollbacks : 0.0, g_stats.stalls,
                          g_stats.advantage, g_confirmed);
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
    g_lockstep_frame = g_confirmed;
}

// --- lockstep (version 1)

void LockstepPads() {
    // this frame's local input is used delay frames from now
    g_mine[g_frame + uint32_t(g_cfg.delay)] = ReadLocal();
    SendInputs();
    FeedOwn(false);
    WatchFocus();
    auto t0 = Clock::now(), last = t0;
    bool warned = false;
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
    WritePort(g_cfg.side, mine, g_prev_buttons[g_cfg.side]);
    g_prev_buttons[g_cfg.side] = mine.buttons;
    WritePort(1 - g_cfg.side, theirs, g_prev_buttons[1 - g_cfg.side]);
    g_prev_buttons[1 - g_cfg.side] = theirs.buttons;
    g_confirmed = g_frame;
    Checks(g_frame, true);
    Settle();
    Diagnostics();
    LogFrame(mine, theirs, false);
    // forget what both sides are done with
    if (g_frame > 64) {
        g_mine.erase(g_mine.begin(), g_mine.lower_bound(std::min(g_frame - 64, g_their_ack > 64 ? g_their_ack - 64 : 0)));
        g_theirs.erase(g_theirs.begin(), g_theirs.lower_bound(g_frame - 64));
    }
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

// far behind the players (a spectator who came late, or whose game set the match up slowly): three frames of four
// run unseen, as frames run again do, until it is close to them again
bool FastForward(uint32_t next) {
    const uint32_t have = WatchHave(), behind = have > next ? have - next : 0;
    if (!g_fast && behind > kFastFrom) {
        g_fast = true;
        LOGI("netplay: %u frames behind the players: catching up", behind);
    } else if (g_fast && behind < kFastUntil) {
        g_fast = false;
        LOGI("netplay: caught up with the players (%u frames behind)", behind);
    }
    return g_fast && next % 4 != 0;
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
    g_lockstep_frame = g_frame;
}

// --- frame hooks

void FrameStart(uint64_t) {
    ApplyPending();
    if (!g_cfg.on) return;
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
