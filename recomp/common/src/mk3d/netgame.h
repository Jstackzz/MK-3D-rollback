#pragma once
// What the shared netplay core (netplay.cpp) needs from a game, implemented by each game's layer (its netgame.cpp).
// The core owns the connection, the meeting point, prediction and rollback, time sync, checks and the per-frame log;
// the game says where its controllers, counters and match state are.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

struct X86Context;

namespace mk3d::net {

struct Settled;

namespace game {

// --- controllers: the game's per-port input blocks, as its pad update left them
uint32_t NeutralSticks();  // both sticks centred, packed as the blocks hold them
// this PC's controller as the game read it this frame (the physical pad is port 0)
void ReadLocal(uint32_t& buttons, uint32_t& sticks);
// what a port's block gets: these buttons and sticks, with prev_buttons as the last frame's
void WritePort(int port, uint32_t buttons, uint32_t sticks, uint32_t prev_buttons);
// a port's whole block (the sync test copies it as the pad update left it and writes it back on the run again)
uint32_t PortBlock(int port, uint32_t& size);

// --- the meeting point
// the core's barrier runs from the game's match setup events, inside the frame a fight or a side mode match is set
// up, before anything of it loads or starts a timer
void OnMatchSetup(void (*hook)(X86Context& ctx));
// what this game tells the other one at the meeting point (its mode, picks, frame counters, a side mode's setup)
void ReadyWords(std::vector<uint32_t>& out);
std::string Describe();  // the match about to start, for the log ("mode 1, 3 vs 7")
// both games' words are here: make this game's state what both agree on (the larger frame counters, the left player's
// arena, each player's own team...). side: this player's (0 left, 1 right)
void Agree(X86Context& ctx, int side, const std::vector<uint32_t>& mine, const std::vector<uint32_t>& theirs, FILE* log);
// the engine's random generator from the agreed seed (the setup's own draws included)
void Reseed(X86Context& ctx, uint32_t seed);

// --- the match in sync
void MatchBegins();  // at its first frame: what it starts with (its mode)
bool MatchOver();    // at the top of a frame both games agree on: the match is done (left its modes, front end again)
uint32_t StateHash();  // what both games must agree on at a check
void Settle(Settled& s);  // what the launcher may report (game-defined values), from a frame both agree on
uint32_t StateSwitches();  // state task switches so far: the new task loads from the next frame on
// what the game's loader has in flight is finished and taken back (rollback runs the loader on the main thread)
bool LoaderSettle(X86Context& ctx, std::chrono::milliseconds timeout);
void RngFrame(uint32_t seed, uint32_t frame);  // per-site random streams from a frame's top
void RngEnd();

// --- diagnostics
void LogState(FILE* f);  // the per-frame log line's state fields (health, counters, hashes...)
void CheckBlocks(std::vector<std::pair<uint32_t, uint32_t>>& out);  // guest blocks a desync dump writes (va, size)
void DataRange(uint32_t& lo, uint32_t& hi);  // the image's data and bss (<prefix>_NET_MEMDUMP, <prefix>_NET_DATA)
void SideBlocks(std::vector<std::pair<uint32_t, uint32_t>>& out);  // a side mode's board (<prefix>_NET_SIDEDUMP)

}  // namespace game
}  // namespace mk3d::net
