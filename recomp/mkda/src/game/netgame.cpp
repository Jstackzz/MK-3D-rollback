// Deadly Alliance's side of the shared netplay core (recomp/common/src/mk3d/netgame.h): where its controllers,
// counters and match state are (research/mkda-rev1/notes.md, "Engine map for netplay").
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
#include "game/rng.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "mk3d/netplay.h"

X86_EXTERN_FUNC(__imp__sub_001C60C4);

namespace mkda {
namespace {

constexpr uint32_t kNeutralSticks = 0x807F807F;  // both sticks centred, as the pad update packs them
constexpr uint32_t kSeedRandom = 0xEC810;        // the engine's random generator: seed (main calls it once at boot)
constexpr uint32_t kRandomWrapped = 0x2E8404;    // set by its draw 0xECA20 when the table index wraps
constexpr uint32_t kRandomIndex = 0x320A28, kRandomTable = 0x320A40;
// the per-player button configuration (17 actions of 12 bytes: the logical bits that make the action, its handler,
// a value the action's press records; sub_00142DD5 fills both from the defaults 0x266B50)
constexpr uint32_t kConfig[2] = {0x42C640, 0x42C560};
constexpr uint32_t kConfigActions = 17, kConfigStride = 12, kConfigWords = kConfigStride / 4;
// the C runtime's per-thread data (_getptd, sub_001CA1EE): rand's state at +0x14
constexpr uint32_t kGetPtd = 0x1CA1EE, kRandState = 0x14;
// the two fighter objects come from a pool of two at 0x2E88D8 (sub_00100A80), 0xE2C bytes each
constexpr uint32_t kFighterBytes = 0xE2C;
constexpr uint32_t kMatchOver = 0x42C748;
// the button map the pad update reads (sub_00143030): 17 entries {analog, XInput button mask or analog index}, and the
// logical bit each one sets (0x266B50, 12 bytes an entry)
constexpr uint32_t kButtonMap = 0x281438, kLogicalBits = 0x266B50;

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

uint32_t FighterHash(int player) {
    uint32_t obj = kernel::R32(kFighter[player]);
    return obj ? HashBlock(2166136261u, obj, kFighterBytes) : 0;
}

// the player the physical controller of this PC (port 0) plays as: its block, else the block of pads on no player
uint32_t LocalButtons() {
    for (int p = 0; p < 2; p++)
        if (kernel::R32(kPlayerPort[p]) == 0) return kPlayerBlock + uint32_t(p) * kPlayerStride + kPlayerCurrent;
    return kScratchCurrent;
}
uint32_t LocalSticks() {
    for (int p = 0; p < 2; p++)
        if (kernel::R32(kPlayerPort[p]) == 0) return kPlayerSticks + uint32_t(p) * 4;
    return kScratchSticks;
}
// the configuration of the player on port 0 (this PC's player's own, as its options set it)
uint32_t LocalConfig() {
    return kernel::R32(kPlayerPort[1]) == 0 && kernel::R32(kPlayerPort[0]) != 0 ? kConfig[1] : kConfig[0];
}

// READY words (ReadyWords / Agree)
enum ReadyWord {
    kWordMode,
    kWordP1,
    kWordP2,
    kWordFrames,
    kWordGameFrames,
    kWordArena,
    kWordRounds,
    kWordRoundTime,
    kWordFatalities,
    kWordBonusGames,
    kWordDifficulty,
    kWordConfig,  // this PC's player's configuration: 17 actions of 3 words
    kWordCount = kWordConfig + kConfigWords * kConfigActions
};
static_assert(kWordCount <= 64, "the core sends at most 64 READY words");

}  // namespace
}  // namespace mkda

namespace mk3d::net::game {

using namespace mkda;

uint32_t NeutralSticks() { return kNeutralSticks; }

// The logical bits the pad update sets for START and BACK (the button map's digital 0x10 and 0x20). They stay out of
// a match in sync: the pause menu would stop the other player's game as well.
uint32_t PauseBits() {
    static const uint32_t bits = [] {
        uint32_t out = 0;
        for (uint32_t i = 0; i < kConfigActions; i++) {
            uint32_t analog = kernel::R32(kButtonMap + i * 8), mask = kernel::R32(kButtonMap + 4 + i * 8);
            if (!analog && (mask == 0x10 || mask == 0x20)) out |= kernel::R32(kLogicalBits + i * 12);
        }
        LOGI("netplay: START and BACK are logical bits %08X, kept out of matches", out);
        return out;
    }();
    return bits;
}

void ReadLocal(uint32_t& buttons, uint32_t& sticks) {
    buttons = kernel::R32(LocalButtons()) & ~PauseBits();
    sticks = kernel::R32(LocalSticks());
}

// a port is a player here: the pad update keeps the logical buttons per player
void WritePort(int port, uint32_t buttons, uint32_t sticks, uint32_t prev) {
    uint32_t b = kPlayerBlock + uint32_t(port) * kPlayerStride;
    kernel::W32(b + kPlayerPrevious, prev);
    kernel::W32(b + kPlayerCurrent, buttons);
    kernel::W32(b + kPlayerPressed, (buttons ^ prev) & buttons);
    kernel::W32(kPlayerSticks + uint32_t(port) * 4, sticks);
}

// both players' blocks and sticks in one range (the four slots' buttons, then their sticks)
uint32_t PortBlock(int, uint32_t& size) {
    size = kPlayerSticks + 0x10 - kPlayerBlock;
    return kPlayerBlock;
}

// a fight setup (sub_0006F680): the fight task's last loading frame, before the round loop and its timers. The
// attract sequence's demo fights (mode 8, both players on recorded input) are no match: two games do not meet there.
void OnMatchSetup(void (*hook)(X86Context& ctx)) {
    static std::vector<void (*)(X86Context&)> hooks;
    if (hooks.empty())
        BeforeFightSetup([](X86Context& ctx) {
            if (GameMode() == kModeFrontEnd) return;
            for (auto h : hooks) h(ctx);
        });
    hooks.push_back(hook);
}

void ReadyWords(std::vector<uint32_t>& out) {
    out.assign(kWordCount, 0);
    out[kWordMode] = GameMode();
    out[kWordP1] = kernel::R32(kCharacter[0]);
    out[kWordP2] = kernel::R32(kCharacter[1]);
    out[kWordFrames] = kernel::R32(kFrameCounter);
    out[kWordGameFrames] = kernel::R32(kGameCounter);
    out[kWordArena] = kernel::R32(kArena);
    out[kWordRounds] = kernel::R32(kOptRounds);
    out[kWordRoundTime] = kernel::R32(kOptRoundTime);
    out[kWordFatalities] = kernel::R32(kOptFatalities);
    out[kWordBonusGames] = kernel::R32(kOptBonusGames);
    out[kWordDifficulty] = kernel::R32(kOptDifficulty);
    uint32_t config = LocalConfig();
    for (uint32_t i = 0; i < kConfigWords * kConfigActions; i++) out[kWordConfig + i] = kernel::R32(config + i * 4);
}

std::string Describe() {
    char buf[96];
    std::snprintf(buf, sizeof buf, "mode %u, %u vs %u, arena %u", GameMode(), kernel::R32(kCharacter[0]),
                  kernel::R32(kCharacter[1]), kernel::R32(kArena));
    return buf;
}

// Both games reach the meeting point after their own time in the menus: the main loop's frame counters differ (the
// engine's timers count on them), so both take the larger values. The match's options are the left player's (the
// round time is set inside the fight setup, after this; the difficulty goes into the match record sub_0001B700
// keeps), and each player's button configuration is the one their own game has, so each plays with their own
// buttons on both games. The arena loaded already: both games got it from the launcher's seed (linkgame.cpp), here it
// is only checked.
void Agree(X86Context&, int side, const std::vector<uint32_t>& mine, const std::vector<uint32_t>& theirs_in, FILE* log) {
    std::vector<uint32_t> theirs = theirs_in;
    theirs.resize(kWordCount, 0);
    if (theirs[kWordMode] != mine[kWordMode] || theirs[kWordP1] != mine[kWordP1] || theirs[kWordP2] != mine[kWordP2])
        LOGE("netplay: match setups differ: theirs mode %u %u vs %u, ours mode %u %u vs %u", theirs[kWordMode],
             theirs[kWordP1], theirs[kWordP2], mine[kWordMode], mine[kWordP1], mine[kWordP2]);
    if (theirs[kWordArena] != mine[kWordArena])
        LOGE("netplay: the arenas differ (theirs %u, ours %u): the fight's arena loaded already, the games will part",
             theirs[kWordArena], mine[kWordArena]);
    uint32_t frames = std::max(mine[kWordFrames], theirs[kWordFrames]);
    uint32_t game_frames = std::max(mine[kWordGameFrames], theirs[kWordGameFrames]);
    LOGI("netplay: frame counters %u and %u (ours were %u and %u)", frames, game_frames, mine[kWordFrames],
         mine[kWordGameFrames]);
    kernel::W32(kFrameCounter, frames);
    kernel::W32(kGameCounter, game_frames);
    const std::vector<uint32_t>& left = side == 0 ? mine : theirs;
    const struct {
        uint32_t va;
        ReadyWord word;
        const char* name;
    } options[] = {{kOptRounds, kWordRounds, "rounds"},
                   {kOptRoundTime, kWordRoundTime, "round time"},
                   {kOptFatalities, kWordFatalities, "fatalities"},
                   {kOptBonusGames, kWordBonusGames, "bonus games"},
                   {kOptDifficulty, kWordDifficulty, "difficulty"}};
    for (const auto& o : options) {
        if (kernel::R32(o.va) != left[o.word]) {
            LOGI("netplay: %s %u (ours was %u)", o.name, left[o.word], kernel::R32(o.va));
            if (log) std::fprintf(log, "# %s %u (ours was %u)\n", o.name, left[o.word], kernel::R32(o.va));
        }
        kernel::W32(o.va, left[o.word]);
    }
    for (int player = 0; player < 2; player++) {
        const std::vector<uint32_t>& own = player == side ? mine : theirs;
        for (uint32_t i = 0; i < kConfigWords * kConfigActions; i++)
            kernel::W32(kConfig[player] + i * 4, own[kWordConfig + i]);
    }
}

// the engine's random generator (a Mersenne Twister variant) drew differently in the two games' menus: both seed it
// again; its draws from here on come from the per-site streams (rng.h). The C runtime's rand (never seeded, so its
// state is how often this game called it) starts from the seed too.
void Reseed(X86Context& ctx, uint32_t seed) {
    kernel::CallGuest(ctx, kSeedRandom, {seed});
    kernel::W32(kRandomWrapped, 0);
    if (uint32_t ptd = kernel::CallGuest(ctx, kGetPtd, {})) kernel::W32(ptd + kRandState, seed);
    rng::BeginFrame(seed, 0xFFFFFFFFu);  // the setup's own draws
}

void MatchBegins() { g_match_mode = GameMode(); }

// the match in sync is over, as the top of this frame shows: the game left its mode, or it is in the front end again
// (a versus match returns to its select screen), or a round nobody touched sent it to the attract sequence
bool MatchOver() {
    return GameMode() != g_match_mode || InFrontEnd() || kernel::R32(kMainTask) == kAttractTask;
}

// the fighters' health, round wins and characters, the game mode, the round and its time, both fighter objects
uint32_t StateHash() {
    uint32_t h = 2166136261u;
    for (int p = 0; p < 2; p++) {
        h = Fnv(h, kernel::R32(kHealth[p]));
        h = Fnv(h, kernel::R32(kRoundWins[p]));
        h = Fnv(h, kernel::R32(kCharacter[p]));
        h = Fnv(h, FighterHash(p));
    }
    h = Fnv(h, GameMode());
    h = Fnv(h, kernel::R32(kRound));
    h = Fnv(h, kernel::R32(kRoundTime));
    h = Fnv(h, kernel::R32(kMatchWinner));
    return h;
}

void Settle(Settled& s) {
    s.v[kSettledWins1] = kernel::R32(kRoundWins[0]);
    s.v[kSettledWins2] = kernel::R32(kRoundWins[1]);
    s.v[kSettledMode] = GameMode();
    s.v[kSettledWinner] = kernel::R32(kMatchWinner);
    s.v[kSettledTask] = kernel::R32(kMainTask);
}

uint32_t StateSwitches() { return mkda::StateSwitches(); }

bool LoaderSettle(X86Context& ctx, std::chrono::milliseconds timeout) { return aio::Settle(ctx, timeout); }

void RngFrame(uint32_t seed, uint32_t frame) { rng::BeginFrame(seed, frame); }
void RngEnd() { rng::End(); }

void LogState(FILE* f) {
    uint32_t index = kernel::R32(kRandomIndex);
    std::fprintf(f, "health %g %g counters %u %u fighters %08X %08X mt %u %08X wins %u %u round %u time %u task %08X over %u",
                 GuestFloat(kHealth[0]), GuestFloat(kHealth[1]), kernel::R32(kFrameCounter), kernel::R32(kGameCounter),
                 FighterHash(0), FighterHash(1), index, kernel::R32(kRandomTable + (index % 624) * 4),
                 kernel::R32(kRoundWins[0]), kernel::R32(kRoundWins[1]), kernel::R32(kRound), kernel::R32(kRoundTime),
                 kernel::R32(kMainTask), kernel::R32(kMatchOver));
}

// the two fighter objects, the fight's globals (health, wins, round, timer: 0x42C700..0x42C800) and the players'
// select and fight state (0x2EA530..0x2EA560)
void CheckBlocks(std::vector<std::pair<uint32_t, uint32_t>>& out) {
    out.push_back({kernel::R32(kFighter[0]), kFighterBytes});
    out.push_back({kernel::R32(kFighter[1]), kFighterBytes});
    out.push_back({0x42C700, 0x100});
    out.push_back({0x2EA530, 0x30});
}

// the .data section with its zero filled tail (0x24AFA0..0x431C90)
void DataRange(uint32_t& lo, uint32_t& hi) {
    lo = 0x24A000;
    hi = 0x432000;
}

void SideBlocks(std::vector<std::pair<uint32_t, uint32_t>>&) {}  // no side modes

}  // namespace mk3d::net::game

// the C runtime's rand (no srand is linked): counted for the netplay state checks
X86_FUNC(sub_001C60C4) {
    __imp__sub_001C60C4(ctx, base);
    mk3d::net::CountRand(ctx.eax);
}
