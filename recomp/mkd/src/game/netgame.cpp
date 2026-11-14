// Deception's side of the shared netplay core (recomp/common/src/mk3d/netgame.h): where its controllers, counters and
// match state are (research/mkd/notes.md rounds 4 to 7).
#include "mk3d/netgame.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "game/aio.h"
#include "game/events.h"
#include "game/frame.h"
#include "game/netgame.h"
#include "game/agreed_width.h"
#include "game/rng.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "mk3d/netplay.h"

X86_EXTERN_FUNC(__imp__sub_0029B8AC);

namespace mkd {
namespace {

constexpr uint32_t kNeutralSticks = 0x807F807F;   // both sticks centred, as the pad update packs them
constexpr uint32_t kRoundWins[2] = {kP1Record + 0x40, kP2Record + 0x40};
constexpr uint32_t kFrameCounter = 0x61C3F8;      // main loop iterations, the engine's timers count on it
constexpr uint32_t kGameFrameCounter = 0x61C418;  // the same while the game is not paused
constexpr uint32_t kSeedRandom = 0x14BDB0;        // the engine's random generator: seed (main calls it once at boot)
constexpr uint32_t kRandomWrapped = 0x46F970;     // set by its draw 0x14BFB0 when the table index wraps
constexpr uint32_t kChessGame = 0x46CEDC, kPuzzleGame = 0x46CE5C;

uint32_t g_match_mode = 0;  // the game mode of the match in sync

float GuestFloat(uint32_t va) {
    uint32_t v = kernel::R32(va);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

uint32_t Fnv(uint32_t h, uint32_t v) {
    for (int i = 0; i < 4; i++) h = (h ^ ((v >> (i * 8)) & 0xFF)) * 16777619u;
    return h;
}

bool Pointer(uint32_t v) { return (v >= 0x10000 && v < 0x4000000) || v >= 0x80000000u; }

// a block of guest memory, pointers left out (the two games' heaps may lie differently); nothing when it is not there
uint32_t HashBlock(uint32_t h, uint32_t va, uint32_t size) {
    if (!va || !mem::IsCommitted(va) || !mem::IsCommitted(va + size - 1)) return h;
    for (uint32_t o = 0; o + 4 <= size; o += 4) {
        uint32_t v = kernel::R32(va + o);
        if (Pointer(v)) continue;
        h = (h ^ v) * 16777619u;
    }
    return h;
}

// The two fighter objects are 0x710 apart (0x470C78, 0x471388); past P2's lies the select screen's state (the cursors
// 0x471B18 / 0x471B1C and what each cursor did), which differs between the two games when a player picks there
// with their own controller while the other game's autopilot makes the same pick another way.
constexpr uint32_t kFighterBytes = 0x710;

// a fighter object's values (player record + 0x58), pointers left out
uint32_t FighterHash(uint32_t record) {
    uint32_t obj = kernel::R32(record + 0x58);
    return obj ? HashBlock(2166136261u, obj, kFighterBytes) : 0;
}

// what a side mode match is made of: Chess Kombat's game (0x46CEDC: 0x1BC bytes, its board +0x38 0x1454 bytes, each
// side's 17 pieces 0x90 bytes each through +0x3C / +0x40, from +8), Puzzle Kombat's (0x46CE5C: 0x148 bytes, the two
// wells +0x4C / +0x50 0x8C0 bytes each, the two players +0x54 / +0x58 0x554 bytes each, their next pieces +0x5C to
// +0x68 0x28 bytes each; all made by its setup sub_000F2BD0). Not the static range from 0x5FDEAC its players point
// to (+0x4C8): that is P1's profile (0x5FDF60, 0x5E0 bytes; P2's is 0x5FD980), loaded resources' handles and the
// engine's random generator, each PC's own (a profile from that PC's save), not the match's. Hashing it there made
// two games of the same match part at frame 0.
void SideModeBlocks(std::vector<std::pair<uint32_t, uint32_t>>& out) {
    uint32_t mode = GameMode();
    if (mode == kModeChess) {
        uint32_t game = kernel::R32(kChessGame);
        if (!game || !mem::IsCommitted(game)) return;
        out.push_back({game, 0x1BC});
        out.push_back({kernel::R32(game + 0x38), 0x1454});
        for (uint32_t side : {0x3Cu, 0x40u}) {
            uint32_t pieces = kernel::R32(game + side);
            if (!pieces || !mem::IsCommitted(pieces)) continue;
            for (uint32_t i = 0; i < 17; i++) out.push_back({kernel::R32(pieces + 8 + 4 * i), 0x90});
        }
    } else if (mode == kModePuzzle) {
        uint32_t game = kernel::R32(kPuzzleGame);
        if (!game || !mem::IsCommitted(game)) return;
        out.push_back({game, 0x148});
        out.push_back({kernel::R32(game + 0x4C), 0x8C0});
        out.push_back({kernel::R32(game + 0x50), 0x8C0});
        out.push_back({kernel::R32(game + 0x54), 0x554});
        out.push_back({kernel::R32(game + 0x58), 0x554});
        for (uint32_t next = 0x5C; next <= 0x68; next += 4) out.push_back({kernel::R32(game + next), 0x28});
    }
}

uint32_t SideModeHash() {
    std::vector<std::pair<uint32_t, uint32_t>> blocks;
    SideModeBlocks(blocks);
    uint32_t h = 2166136261u;
    for (auto [va, size] : blocks) h = HashBlock(h, va, size);
    return h;
}

// whether a game mode belongs to the match that started in another: Chess Kombat goes from its board (9) to battles
// (10) and back
bool SameMatch(uint32_t start, uint32_t now) {
    if (start == kModeChess || start == kModeChessBattle) return now == kModeChess || now == kModeChessBattle;
    return now == start;
}

// READY words (ReadyWords / Agree)
enum ReadyWord {
    kWordMode, kWordP1, kWordP2, kWordFrames, kWordGameFrames, kWordArena, kWordTeams,
    kWordOptions = kWordTeams + 10,  // the options a launcher match sets (linkgame.cpp): round time, blood, death traps
    kWordCostumes = kWordOptions + 3,  // each player's costume flags (record + 0x14): both games must have each's own
    kWordWidth = kWordCostumes + 2,    // this PC's viewport width (agreed_width.cpp)
    kWordCount = kWordWidth + 1
};
constexpr uint32_t kOptions[3] = {0x5FC9AC, 0x5FC9B4, 0x5FC9B8};

// Puzzle Kombat's last variant: sub_000F52A0 draws one of three and draws again while it equals this, and nothing but
// that function ever writes it, so a game that has played Puzzle before could take another variant than a fresh one
// with the same seed. Each match in sync starts from the value it has at boot.
constexpr uint32_t kPuzzleLastVariant = 0x3EE904, kPuzzleLastVariantAtBoot = 0x2710;

}  // namespace
}  // namespace mkd

namespace mk3d::net::game {

using namespace mkd;

uint32_t NeutralSticks() { return kNeutralSticks; }

// The logical bits the pad update (sub_001C52F0) sets for START and BACK: it maps 16 physical inputs (0x453A48:
// {analog, XInput button mask or analog index}) through the port's table of logical bits (default 0x3E95A0, 12 bytes
// an entry). They stay out of a match in sync: the pause menu would stop the other player's game as well.
uint32_t PauseBits() {
    static const uint32_t bits = [] {
        uint32_t out = 0;
        for (uint32_t i = 0; i < 16; i++) {
            uint32_t analog = kernel::R32(0x453A48 + i * 8), mask = kernel::R32(0x453A4C + i * 8);
            if (!analog && (mask == 0x10 || mask == 0x20)) out |= kernel::R32(0x3E95A0 + i * 12);
        }
        LOGI("netplay: START and BACK are logical bits %08X, kept out of matches", out);
        return out;
    }();
    return bits;
}

void ReadLocal(uint32_t& buttons, uint32_t& sticks) {
    buttons = kernel::R32(kPortBlock + kPortCurrent) & ~PauseBits();  // the physical controller of this PC is port 0
    sticks = kernel::R32(kPortBlock + kPortSticks);
}

// BACK's logical bits alone (the physical map's digital 0x20)
bool LocalBack() {
    static const uint32_t bits = [] {
        uint32_t out = 0;
        for (uint32_t i = 0; i < 16; i++)
            if (!kernel::R32(0x453A48 + i * 8) && kernel::R32(0x453A4C + i * 8) == 0x20) out |= kernel::R32(0x3E95A0 + i * 12);
        return out;
    }();
    return (kernel::R32(kPortBlock + kPortCurrent) & bits) != 0;
}

void WritePort(int port, uint32_t buttons, uint32_t sticks, uint32_t prev) {
    uint32_t b = kPortBlock + uint32_t(port) * kPortStride;
    kernel::W32(b + kPortFlags, kernel::R32(b + kPortFlags) | 2);
    kernel::W32(b + kPortPrevious, prev);
    kernel::W32(b + kPortCurrent, buttons);
    kernel::W32(b + kPortPressed, (buttons ^ prev) & buttons);
    kernel::W32(b + kPortSticks, sticks);
}

// the pad update's sub_0006AC90 ORs the left stick's directions into the current buttons from the block's sticks,
// which the pad update only reads afresh after it
void RestoreLocalSticks(uint32_t sticks) { kernel::W32(kPortBlock + kPortSticks, sticks); }

uint32_t PortBlock(int port, uint32_t& size) {
    size = kPortStride;
    return kPortBlock + uint32_t(port) * kPortStride;
}

// a fight setup (sub_00083570) or a side mode's match task (Chess Kombat 0x11DD20, Puzzle Kombat 0xF2850)
void OnMatchSetup(void (*hook)(X86Context& ctx)) { BeforeFightSetup(hook); }

void ReadyWords(std::vector<uint32_t>& out) {
    out.assign(kWordCount, 0);
    out[kWordMode] = GameMode();
    out[kWordP1] = kernel::R32(kP1Character);
    out[kWordP2] = kernel::R32(kP2Character);
    out[kWordFrames] = kernel::R32(kFrameCounter);
    out[kWordGameFrames] = kernel::R32(kGameFrameCounter);
    out[kWordArena] = kernel::R32(kArena);
    for (uint32_t i = 0; i < 5; i++) {
        out[kWordTeams + i] = kernel::R32(kChessTeams + 4 * i);
        out[kWordTeams + 5 + i] = kernel::R32(kChessTeams + 0x1C + 4 * i);
    }
    for (uint32_t i = 0; i < 3; i++) out[kWordOptions + i] = kernel::R32(kOptions[i]);
    out[kWordCostumes] = kernel::R32(kP1Record + 0x14);
    out[kWordCostumes + 1] = kernel::R32(kP2Record + 0x14);
    out[kWordWidth] = width::ViewportWidth();
}

std::string Describe() {
    char buf[96];
    std::snprintf(buf, sizeof buf, "mode %u, %u vs %u", GameMode(), kernel::R32(kP1Character), kernel::R32(kP2Character));
    return buf;
}

// A match's frame counters start kCounterLead past the larger of the two players' own, so that no game's counters go
// back at the meeting point, a spectator's included (its game agrees from the two players' words alone, netplay.cpp
// WatchMeet). The engine marks work done in a frame with the frame counter: sub_0006EF40 waits a frame while an
// object's +0x24 is the counter, sub_000FB310 counts its calls in a frame by +0x24 and clears its sums when +0x68 is
// not the counter, and 26 places compare with it. A game whose counters went back meets this frame's marks again a
// frame or more later and does that frame's work wrong. In Deadly Alliance a King of the Hill spectator one frame ahead
// of both players went out of step at frame 30 (the last match's loser, whose game left that match together with the
// king's); this game agrees the same way. 4096 keeps the counters' phase in any power of two up to 4096 as it was.
// Games before this one start from the larger counters alone: a match's two games and its spectators must agree in
// the same way.
constexpr uint32_t kCounterLead = 4096;

// Both games reach the meeting point after their own time in the menus: the main loop's frame counters differ (the
// engine's timers count on them and some code reads them as they are), so both take the larger values and
// kCounterLead more. A side mode match's select screens ran apart: its arena is the left player's, and in Chess Kombat
// each team is the one its own player's game picked (the board builds its pieces from 0x60166C / 0x601688 once the
// match task runs).
void Agree(X86Context&, int side, const std::vector<uint32_t>& mine, const std::vector<uint32_t>& theirs_in, FILE* log) {
    std::vector<uint32_t> theirs = theirs_in;
    theirs.resize(kWordCount, 0);
    if (theirs[kWordMode] != mine[kWordMode] || theirs[kWordP1] != mine[kWordP1] || theirs[kWordP2] != mine[kWordP2])
        LOGE("netplay: match setups differ: theirs mode %u %u vs %u, ours mode %u %u vs %u", theirs[kWordMode],
             theirs[kWordP1], theirs[kWordP2], mine[kWordMode], mine[kWordP1], mine[kWordP2]);
    const uint32_t frames = std::max(mine[kWordFrames], theirs[kWordFrames]) + kCounterLead;
    const uint32_t game_frames = std::max(mine[kWordGameFrames], theirs[kWordGameFrames]) + kCounterLead;
    LOGI("netplay: frame counters %u and %u (ours were %u and %u)", frames, game_frames, mine[kWordFrames],
         mine[kWordGameFrames]);
    // only a spectator's game more than kCounterLead ahead of both players' still goes back: said, as a desync it brings
    // shows later, at the frame its marks come round again
    const uint32_t now = kernel::R32(kFrameCounter), now_game = kernel::R32(kGameFrameCounter);
    if (now > frames || now_game > game_frames)
        LOGR("netplay: this game's frame counters go back from %u and %u to the match's %u and %u", now, now_game,
             frames, game_frames);
    kernel::W32(kFrameCounter, frames);
    kernel::W32(kGameFrameCounter, game_frames);
    LOGR("netplay: round time %u s, blood %u, death traps %s", mine[kWordOptions], mine[kWordOptions + 1],
         mine[kWordOptions + 2] ? "on" : "off");
    for (uint32_t i = 0; i < 3; i++)
        if (theirs[kWordOptions + i] != mine[kWordOptions + i])
            LOGE("netplay: the games' options differ (round time, blood, death traps: theirs %u %u %u)",
                 theirs[kWordOptions], theirs[kWordOptions + 1], theirs[kWordOptions + 2]);
    uint32_t mode = GameMode();
    if (mode != kModeChess && mode != kModePuzzle && theirs[kWordArena] != mine[kWordArena])
        LOGE("netplay: the arenas differ (theirs %u, ours %u): the fight's arena loaded already, the games will part",
             theirs[kWordArena], mine[kWordArena]);
    else if (mode != kModeChess && mode != kModePuzzle)
        LOGR("netplay: arena %u", mine[kWordArena]);
    if (theirs[kWordCostumes] != mine[kWordCostumes] || theirs[kWordCostumes + 1] != mine[kWordCostumes + 1])
        LOGE("netplay: the costumes differ (theirs %X and %X, ours %X and %X): the fighters loaded already",
             theirs[kWordCostumes], theirs[kWordCostumes + 1], mine[kWordCostumes], mine[kWordCostumes + 1]);
    else
        LOGR("netplay: costume flags %X and %X", mine[kWordCostumes], mine[kWordCostumes + 1]);
    if (mode == kModeChess || mode == kModePuzzle) {
        uint32_t arena = side == 0 ? mine[kWordArena] : theirs[kWordArena];
        if (arena != kernel::R32(kArena)) LOGI("netplay: arena %u (ours was %u)", arena, kernel::R32(kArena));
        kernel::W32(kArena, arena);
    }
    if (mode == kModePuzzle) {
        kernel::W32(kPuzzleLastVariant, kPuzzleLastVariantAtBoot);
    }
    if (mode == kModePuzzle || mode == kModeChess) width::AgreeWidth(mine[kWordWidth], theirs[kWordWidth]);
    if (mode == kModeChess) {
        for (uint32_t team = 0; team < 2; team++) {
            if (int(team) == side) continue;  // P1's from side 0, P2's from side 1
            for (uint32_t i = 0; i < 5; i++) {
                uint32_t va = kChessTeams + team * 0x1C + 4 * i, value = theirs[kWordTeams + team * 5 + i];
                if (kernel::R32(va) != value) {
                    LOGR("netplay: P%u's chess piece %u is %u (ours was %u)", team + 1, i, value, kernel::R32(va));
                    if (log) std::fprintf(log, "# P%u's chess piece %u is %u (ours was %u)\n", team + 1, i, value,
                                          kernel::R32(va));
                }
                kernel::W32(va, value);
            }
        }
    }
    // which port drives which fighter (player record + 4, port block + 8): the netplay writes P1's input to port 0
    // and P2's to port 1
    const uint32_t p1_port = kernel::R32(kP1Record + 4), p2_port = kernel::R32(kP2Record + 4);
    const uint32_t port0 = kernel::R32(kPortBlock + 8), port1 = kernel::R32(kPortBlock + kPortStride + 8);
    LOGR("netplay: P1 plays from port %u, P2 from port %u (ports 0 and 1 drive %08X and %08X)", p1_port, p2_port,
         port0, port1);
    if (log) std::fprintf(log, "# P1 plays from port %u, P2 from port %u (ports 0 and 1 drive %08X and %08X)\n",
                          p1_port, p2_port, port0, port1);
}

// the engine's random generator (a Mersenne Twister variant, state 0x5FE9F4 + 0x5FEA00) drew differently in the two
// games' menus: both seed it again; its draws from here on come from the per-site streams (rng.h)
void Reseed(X86Context& ctx, uint32_t seed) {
    kernel::CallGuest(ctx, kSeedRandom, {seed});
    kernel::W32(kRandomWrapped, 0);
    rng::BeginFrame(seed, 0xFFFFFFFFu);  // the setup's own draws
}

void MatchBegins() { g_match_mode = GameMode(); }

// the match in sync is over, as the top of this frame shows: it left its modes (Chess Kombat's game over), or the
// game is in the front end again (a fight's or Puzzle Kombat's match returns to its select screen). The next fight
// of a match, and a chess battle, stay in sync.
bool MatchOver() { return !SameMatch(g_match_mode, GameMode()) || InFrontEnd(); }

// the fighters' health, round wins and characters, the game mode, both fighter objects, a side mode match's board
uint32_t StateHash() {
    uint32_t h = 2166136261u;
    h = Fnv(h, kernel::R32(kP1Health));
    h = Fnv(h, kernel::R32(kP2Health));
    h = Fnv(h, kernel::R32(kRoundWins[0]));
    h = Fnv(h, kernel::R32(kRoundWins[1]));
    h = Fnv(h, kernel::R32(kP1Character));
    h = Fnv(h, kernel::R32(kP2Character));
    h = Fnv(h, GameMode());
    h = Fnv(h, FighterHash(kP1Record));
    h = Fnv(h, FighterHash(kP2Record));
    h = Fnv(h, SideModeHash());
    return h;
}

void Settle(Settled& s) {
    s.v[kSettledWins1] = kernel::R32(kRoundWins[0]);
    s.v[kSettledWins2] = kernel::R32(kRoundWins[1]);
    s.v[kSettledMode] = GameMode();
    s.v[kSettledChessWinner] = kernel::R32(kChessWinner);
    s.v[kSettledPuzzleMatches] = kernel::R32(kPuzzleMatches);
    s.v[kSettledPuzzleWinner] = kernel::R32(kPuzzleWinner);
}

uint32_t StateSwitches() { return mkd::StateSwitches(); }

bool LoaderSettle(X86Context& ctx, std::chrono::milliseconds timeout) { return aio::Settle(ctx, timeout); }

void RngFrame(uint32_t seed, uint32_t frame) { rng::BeginFrame(seed, frame); }
void RngEnd() { rng::End(); }

void LogState(FILE* f) {
    std::fprintf(f,
                 "health %g %g counters %u %u fighters %08X %08X mt %u %08X wins %u %u task %08X side %08X puzzle %u %u "
                 "%u %u",
                 GuestFloat(kP1Health), GuestFloat(kP2Health), kernel::R32(kFrameCounter), kernel::R32(kGameFrameCounter),
                 FighterHash(kP1Record), FighterHash(kP2Record), kernel::R32(0x5FE9F4),
                 kernel::R32(0x5FEA00 + (kernel::R32(0x5FE9F4) % 624) * 4), kernel::R32(kRoundWins[0]),
                 kernel::R32(kRoundWins[1]), kernel::R32(kMainTask), SideModeHash(), kernel::R32(kPuzzleMatches),
                 kernel::R32(kPuzzleWinner), kernel::R32(kPuzzleStreak[0]), kernel::R32(kPuzzleStreak[1]));
}

// the two fighter objects, the game state block 0x620000..0x622000 and a side mode match's board
void CheckBlocks(std::vector<std::pair<uint32_t, uint32_t>>& out) {
    out.push_back({kernel::R32(kP1Record + 0x58), kFighterBytes});
    out.push_back({kernel::R32(kP2Record + 0x58), kFighterBytes});
    out.push_back({0x620000, 0x2000});
    SideModeBlocks(out);
}

// the .data section (0x3E3580..0x64007C)
void DataRange(uint32_t& lo, uint32_t& hi) {
    lo = 0x3E3000;
    hi = 0x641000;
}

void SideBlocks(std::vector<std::pair<uint32_t, uint32_t>>& out) { SideModeBlocks(out); }

// each fighter's character, its state (+0x214: 0 stance, 0x1200 an attack, 0x60x a hit reaction...), its attack's
// phase (+0x20C: 1 startup, 2 hit window, 3 recovery) and the routine its task runs (the task +0x20, its function +0x30)
void DescribeFighters(char* out, size_t n) {
    if (!n) return;
    out[0] = 0;
    size_t at = 0;
    for (int p = 0; p < 2 && at + 1 < n; p++) {
        const uint32_t record = p ? kP2Record : kP1Record, obj = kernel::R32(record + 0x58);
        int w;
        if (!obj || !mem::IsCommitted(obj) || !mem::IsCommitted(obj + kFighterBytes - 1)) {
            w = std::snprintf(out + at, n - at, "%sP%d none", p ? ", " : "", p + 1);
        } else {
            const uint32_t task = kernel::R32(obj + 0x20);
            const uint32_t routine = task && mem::IsCommitted(task) && mem::IsCommitted(task + 0x33) &&
                                             kernel::R32(task + 4) == kernel::R32(obj + 0x24)
                                         ? kernel::R32(task + 0x30)
                                         : 0;
            w = std::snprintf(out + at, n - at, "%sP%d %u state %04X phase %u routine %08X", p ? ", " : "", p + 1,
                              kernel::R32(p ? kP2Character : kP1Character), kernel::R32(obj + 0x214),
                              kernel::R32(obj + 0x20C), routine);
        }
        if (w < 0) break;
        at = std::min(n - 1, at + size_t(w));
    }
}

}  // namespace mk3d::net::game

// The C runtime's rand. In a fight in sync a draw is keyed by the agreed seed, the frame, the call site and its count
// in the frame, as the engine's generator is (rng.cpp), so a stage object that starts drawing before the two games
// meet in one and after it in the other moves nothing else (as Deadly Alliance's arena props did); those draws are not
// counted for the checks. Outside a fight the runtime's own rand, counted.
X86_FUNC(sub_0029B8AC) {
    uint32_t v;
    if (mkd::rng::CrtDraw(kernel::R32(ctx.esp), v)) {
        ctx.eax = v & 0x7FFF;  // rand's range
        ctx.esp += 4;          // ret
        return;
    }
    __imp__sub_0029B8AC(ctx, base);
    mk3d::net::CountRand(ctx.eax);
}
