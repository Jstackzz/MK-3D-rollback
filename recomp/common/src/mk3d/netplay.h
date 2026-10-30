#pragma once
// Two-player netplay over UDP, shared by the games (the game's side: mk3d/netgame.h). Each game runs on its own until
// a match is set up; both meet at a barrier inside that frame, and from then on both players' logical inputs are
// written into the game's per-port input blocks right after its pad update, so both games simulate the same match.
//
// Version 2, rollback (the default): every frame runs at once with the other player's input predicted (their last
// one, repeated); when their real input for a frame arrives and differs, the game is put back to the top of that
// frame (rollback.h) and runs again up to the present frame within one displayed frame, unseen and unpaced. Local
// input waits a frame or two (the delay) before it is used, so most of the other player's inputs arrive before they
// are needed. While the game loads (a state switch, the match's start), frames wait for the real input instead.
// Version 1, lockstep, waits for the other input every frame (<prefix>_NET_MODE=lockstep).
//
// Spectators: a player's game also writes what a spectator needs to the launcher's feed (Settings.feed, TCP on this
// PC): its player's pick, its words at the meeting point, its own inputs and its checks; the launchers pass both
// players' feeds through the server to the lobby's spectators. A spectator's game (Settings.watch) reads both players'
// from its launcher's feed and plays the match from them: it meets the two at their meeting point, never predicts, so
// never runs a frame again, and runs ahead unseen while it is far behind them.
//
// Configuration, from the environment for tests (the launcher link configures it otherwise), <prefix> the game's
// (mk3d/game.h EnvPrefix: MKD, MKA, MKDA):
//   <prefix>_NET_BIND=<udp port>         this game's port
//   <prefix>_NET_PEER=<ip>:<udp port>    the other game
//   <prefix>_NET_SIDE=0|1                which player this one is (0 left/P1, 1 right/P2)
//   <prefix>_NET_DELAY=<frames>          input delay, default 1 with rollback, 2 with lockstep
//   <prefix>_NET_MODE=rollback|lockstep
//   <prefix>_NET_LOG=<path>              per frame inputs, state and checks, for desync hunting (tools/py/rbcmp.py)
//   <prefix>_NET_FEED=<ip>:<tcp port>    the spectator feed (a player's game writes to it, a spectator's reads)
//   <prefix>_NET_WATCH=1                 a spectator (with NET_FEED; no NET_BIND or NET_PEER)
//   <prefix>_NET_LAG=<ms>, _JITTER=<ms>, _LOSS=<percent>   simulated network on what this game sends
//   <prefix>_NET_DUMP, _MEMDUMP, _SIDEDUMP=<frame>, _DATA=1  diagnostics (netplay.cpp)
#include <cstdint>
#include <string>

namespace mk3d::net {

struct Settings {
    uint16_t bind = 0;           // local UDP port
    std::string peer;            // ip:port of the other game
    int side = 0;                // 0 left (P1), 1 right (P2)
    int delay = -1;              // input delay in frames, -1: the mode's default
    bool rollback = true;        // else lockstep
    bool loopback_only = false;  // bind 127.0.0.1 (local tests)
    std::string log;             // per-frame log path, empty for none
    int lag_ms = 0, jitter_ms = 0, loss_percent = 0;  // simulated network (tests)
    std::string feed;            // ip:port of the launcher's spectator feed (TCP), empty for none
    bool watch = false;          // a spectator: both players come from the feed, no peer, nothing sent
};

// Lockstep and Rollback: a match in sync with the other game
enum class Status { Off, Local, Barrier, Lockstep, Desynced, Disconnected, Rollback };

bool Enabled();
// start or stop netplay; applied by the main thread at the top of the next frame (any thread may call these)
void Configure(const Settings& st);
void Disable();
Status GetStatus();
uint32_t DesyncFrame();
uint32_t LockstepFrame();  // the frame in sync now (either mode)
// true while the main thread waits on the other game (its inputs, the meeting point) or a spectator's feed: a frame
// that stands still for that keeps sending and ends by itself
bool Waiting();

// What the last frame both games agree on shows (with rollback the present frame may still rest on a prediction),
// kept after the match in sync ends until the next one starts. The values are the game's (net::game::Settle); the
// launcher link reads them back through the game (link::game::Outcome). False before the first match in sync.
struct Settled {
    uint32_t v[16] = {};
};
bool GetSettled(Settled& out);

// the C runtime's rand, counted for the state checks: the game's rand hook calls this with each result
void CountRand(uint32_t value);

// The players' own picks on the game's select screen, for a match whose launcher left a side to its player: this
// game's player's pick goes out with every HELLO until the match starts in sync, the other player's comes back the
// same way. The words are the game's (the character, then anything else both games must have alike, such as a
// costume). Both belong to the match set up next and are cleared when it starts, so a rematch picks again.
void SetLocalPick(const uint32_t* words, int n);
// false until the other player's pick for the next match arrived; n: room in words, then the count
bool PeerPick(uint32_t* words, int& n);
// a spectator's game: a side's pick (0 left, 1 right) for the next match, as PeerPick gives a player the other's
bool WatchPick(int side, uint32_t* words, int& n);

struct Stats {
    uint32_t frames = 0, rollbacks = 0, replayed = 0, max_depth = 0, stalls = 0;
    double replay_ms = 0;  // time spent running frames again, total
    int advantage = 0;     // how many frames this game runs ahead of the other one
};
Stats GetStats();

}  // namespace mk3d::net
