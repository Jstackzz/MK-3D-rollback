// Armageddon's side of the shared netplay core (recomp/common/src/mk3d/netgame.h): where its controllers, counters
// and match state are (research/mka/notes.md, engine map for netplay).
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
#include "mk3d/game.h"
#include "mk3d/netplay.h"

X86_EXTERN_FUNC(__imp__sub_0007B650);

namespace mka {
namespace {

constexpr uint32_t kNeutralSticks = 0x807F807F;  // both sticks centred, as the pad update packs them
constexpr uint32_t kRoundWins[2] = {kP1RoundWins, kP2RoundWins};
constexpr uint32_t kSeedRandom = 0x1D6070;       // the engine's random generator: seed (main calls it once at boot)
constexpr uint32_t kRandomIndex = 0x5BB0C8, kRandomState = 0x5BA708;
constexpr uint32_t kRandomDrawn = 0x5BB0CC;      // draws since its last regeneration; the seed leaves it as it was
constexpr uint32_t kFighterBytes = 0xC80;        // a fighter object reaches +0xC80 at least (its size is not known)
constexpr uint32_t kRaceBytes = 0x107C;          // the Motor Kombat race object [0x5B8334]
constexpr uint32_t kKartBytes = 0x4E8;           // a kart: the task data sub_000C2C30 makes for it
constexpr uint32_t kKartSlots = 8;
constexpr uint32_t kArenaCycle = 0x831A64;       // the next arena's place on the list (events.h, linkgame.cpp)
constexpr uint32_t kPhysicalButtons = 0x59C828;  // the pad update's 16 inputs: analog flag, digital mask or index
constexpr uint32_t kStateBlock = 0x830900, kStateBlockBytes = 0x1400;  // records, port blocks, fight state

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

// a fighter object's values (player record + 0x30), pointers left out
uint32_t FighterHash(uint32_t record) {
    uint32_t obj = kernel::R32(record + kRecordFighter);
    return obj ? HashBlock(2166136261u, obj, kFighterBytes) : 0;
}

uint32_t Race() { return GameMode() == kModeKart ? kernel::R32(kRace) : 0; }

// a handle the engine keeps as (object, the object's serial at +4): the object while it is alive, else 0
uint32_t Handle(uint32_t at) {
    uint32_t obj = kernel::R32(at);
    return obj && mem::IsCommitted(obj) && kernel::R32(obj + 4) == kernel::R32(at + 4) ? obj : 0;
}

// the kart in a slot of the race (handles at race + 0x24C + slot * 8; the players' slots first), 0 if none
uint32_t Kart(uint32_t race, uint32_t slot) { return race ? Handle(race + kKarts + slot * 8) : 0; }

uint32_t KartHash(uint32_t race, uint32_t slot) {
    uint32_t kart = Kart(race, slot);
    return kart ? HashBlock(2166136261u, kart, kKartBytes) : 0;
}

// the race object and its karts
uint32_t RaceHash() {
    uint32_t race = Race();
    if (!race) return 0;
    uint32_t h = HashBlock(2166136261u, race, kRaceBytes);
    for (uint32_t slot = 0; slot < kKartSlots; slot++) h = Fnv(h, KartHash(race, slot));
    return h;
}

// Motor Kombat: a player's place (1..8) in the finish order the results wrote (race + 0x29C: kart slots, -1 empty; a
// kart's +0x20 is its slot = player number - 1), 0 while there is none
uint32_t KartPlace(uint32_t race, uint32_t player) {
    if (!race || !(kernel::R32(race) & 0x20000)) return 0;
    for (uint32_t place = 0; place < 8; place++) {
        int32_t slot = int32_t(kernel::R32(race + kKartOrder + place * 4));
        if (slot < 0 || slot >= 8) continue;
        uint32_t kart = Handle(race + kKarts + uint32_t(slot) * 8);
        if (kart && kernel::R32(kart + 0x20) == player) return place + 1;
    }
    return 0;
}

// whether a game mode belongs to the match that started in another
bool SameMatch(uint32_t start, uint32_t now) { return now == start; }

// READY words (ReadyWords / Agree)
enum ReadyWord {
    kWordMode, kWordP1, kWordP2, kWordFrames, kWordGameFrames, kWordArena, kWordKarts,
    kWordOptions = kWordKarts + 4,  // the options a launcher match sets (linkgame.cpp): round time, blood, death traps
    kWordCount = kWordOptions + 3
};
constexpr uint32_t kOptions[3] = {0x723BC4, 0x723BC8, 0x723BCC};

// with netplay set up from the environment (tests on one PC), both games start the arena list at its first entry:
// the boot draws the place from the generator, which main seeds from the time stamp counter (a launcher match sets it
// from the match's seed, linkgame.cpp)
void FirstFrame(uint64_t frame) {
    if (frame != 1 || !mk3d::Env("NET_PEER")) return;
    kernel::W32(kArenaCycle, 0);
    LOGI("netplay: the arena list starts at its first entry (netplay from the environment)");
}

const bool g_registered = [] {
    OnFrameStart(&FirstFrame);
    return true;
}();

}  // namespace

uint32_t PauseBits() {
    static const uint32_t bits = [] {
        uint32_t out = 0;
        for (uint32_t i = 0; i < 16; i++) {
            uint32_t analog = kernel::R32(kPhysicalButtons + i * 8), mask = kernel::R32(kPhysicalButtons + 4 + i * 8);
            if (!analog && (mask == 0x10 || mask == 0x20)) out |= kernel::R32(kDefaultButtons + i * 12);
        }
        LOGI("netplay: START and BACK are logical bits %08X, kept out of matches", out);
        return out;
    }();
    return bits;
}

}  // namespace mka

namespace mk3d::net::game {

using namespace mka;

uint32_t NeutralSticks() { return kNeutralSticks; }

void ReadLocal(uint32_t& buttons, uint32_t& sticks) {
    buttons = kernel::R32(kPortBlock + kPortCurrent) & ~PauseBits();  // the physical controller of this PC is port 0
    sticks = kernel::R32(kPortBlock + kPortSticks);
}

void WritePort(int port, uint32_t buttons, uint32_t sticks, uint32_t prev) {
    uint32_t b = kPortBlock + uint32_t(port) * kPortStride;
    kernel::W32(b + kPortFlags, kernel::R32(b + kPortFlags) | 2);
    kernel::W32(b + kPortPrevious, prev);
    kernel::W32(b + kPortCurrent, buttons);
    kernel::W32(b + kPortPressed, (buttons ^ prev) & buttons);
    kernel::W32(b + kPortSticks, sticks);
}

// the pad update's sub_0007B9F0 ORs the left stick's directions into the current buttons from the block's sticks,
// which the pad update only reads afresh after it
void RestoreLocalSticks(uint32_t sticks) { kernel::W32(kPortBlock + kPortSticks, sticks); }

uint32_t PortBlock(int port, uint32_t& size) {
    size = kPortStride;
    return kPortBlock + uint32_t(port) * kPortStride;
}

// a fight setup (sub_000983A0) or the start of a Motor Kombat race load (events.cpp, kRaceLoadMode)
void OnMatchSetup(void (*hook)(X86Context& ctx)) { BeforeFightSetup(hook); }

void ReadyWords(std::vector<uint32_t>& out) {
    out.assign(kWordCount, 0);
    out[kWordMode] = GameMode();
    out[kWordP1] = kernel::R32(kP1Character);
    out[kWordP2] = kernel::R32(kP2Character);
    out[kWordFrames] = kernel::R32(kFrameCounter);
    out[kWordGameFrames] = kernel::R32(kGameCounter);
    out[kWordArena] = kernel::R32(GameMode() == kModeKart ? kTrackChoice : kArena);
    for (uint32_t i = 0; i < 4; i++) out[kWordKarts + i] = kernel::R32(kKartChoice + i * 0xC);
    for (uint32_t i = 0; i < 3; i++) out[kWordOptions + i] = kernel::R32(kOptions[i]);
}

std::string Describe() {
    char buf[96];
    if (GameMode() == kModeKart)
        std::snprintf(buf, sizeof buf, "mode %u, karts %u vs %u, track %u", GameMode(), kernel::R32(kKartChoice),
                      kernel::R32(kKartChoice + 0xC), kernel::R32(kTrackChoice));
    else
        std::snprintf(buf, sizeof buf, "mode %u, %u vs %u, arena %u", GameMode(), kernel::R32(kP1Character),
                      kernel::R32(kP2Character), kernel::R32(kArena));
    return buf;
}

// Both games reach the meeting point after their own time in the menus: the main loop's frame counters differ (the
// engine's timers count on them: the round timer starts from 0x709E6C), so both take the larger values. A fight's
// arena and picks loaded already, a race's track and karts are chosen (it loads after this point): they must have
// been the same (the launcher's seed picks the arena or the track, linkgame.cpp).
// Every port's button table is the default one from here on (sub_0007B650 below), so a logical bit means the same
// in both games.
void Agree(X86Context&, int, const std::vector<uint32_t>& mine, const std::vector<uint32_t>& theirs_in, FILE* log) {
    std::vector<uint32_t> theirs = theirs_in;
    theirs.resize(kWordCount, 0);
    if (theirs[kWordMode] != mine[kWordMode] || theirs[kWordP1] != mine[kWordP1] || theirs[kWordP2] != mine[kWordP2])
        LOGE("netplay: match setups differ: theirs mode %u %u vs %u, ours mode %u %u vs %u", theirs[kWordMode],
             theirs[kWordP1], theirs[kWordP2], mine[kWordMode], mine[kWordP1], mine[kWordP2]);
    uint32_t frames = std::max(mine[kWordFrames], theirs[kWordFrames]);
    uint32_t game_frames = std::max(mine[kWordGameFrames], theirs[kWordGameFrames]);
    LOGI("netplay: frame counters %u and %u (ours were %u and %u)", frames, game_frames, mine[kWordFrames],
         mine[kWordGameFrames]);
    kernel::W32(kFrameCounter, frames);
    kernel::W32(kGameCounter, game_frames);
    if (mine[kWordMode] != kModeKart) {
        LOGR("netplay: round time %u s, blood %u, death traps %s", mine[kWordOptions], mine[kWordOptions + 1],
             mine[kWordOptions + 2] ? "on" : "off");
        for (uint32_t i = 0; i < 3; i++)
            if (theirs[kWordOptions + i] != mine[kWordOptions + i])
                LOGE("netplay: the games' options differ (round time, blood, death traps: theirs %u %u %u)",
                     theirs[kWordOptions], theirs[kWordOptions + 1], theirs[kWordOptions + 2]);
    }
    if (theirs[kWordArena] != mine[kWordArena]) {
        LOGE("netplay: the arenas or tracks differ (theirs %u, ours %u): the games will part", theirs[kWordArena],
             mine[kWordArena]);
        if (log) std::fprintf(log, "# arenas differ: theirs %u, ours %u\n", theirs[kWordArena], mine[kWordArena]);
    }
    if (mine[kWordMode] == kModeKart)
        for (uint32_t i = 0; i < 4; i++)
            if (theirs[kWordKarts + i] != mine[kWordKarts + i])
                LOGE("netplay: player %u's kart differs (theirs %u, ours %u)", i + 1, theirs[kWordKarts + i],
                     mine[kWordKarts + i]);
    for (uint32_t port = 0; port < 4; port++)
        if (kernel::R32(kPortBlock + port * kPortStride + kPortButtonMap))
            kernel::W32(kPortBlock + port * kPortStride + kPortButtonMap, kDefaultButtons);
}

// the engine's random generator drew differently in the two games' menus (and main seeds it from the time stamp
// counter): both seed it again; its draws from here on come from the per-site streams (rng.h)
void Reseed(X86Context& ctx, uint32_t seed) {
    kernel::CallGuest(ctx, kSeedRandom, {seed});
    kernel::W32(kRandomDrawn, 0);
    rng::BeginFrame(seed, 0xFFFFFFFFu);  // the setup's own draws
}

void MatchBegins() { g_match_mode = GameMode(); }

// the match in sync is over, as the top of this frame shows: it left its mode (Motor Kombat's results set the front
// end mode), or the game is in the front end again (a versus match returns to its select screen). The next round of a
// fight stays in sync.
bool MatchOver() { return !SameMatch(g_match_mode, GameMode()) || InFrontEnd(); }

// the fighters' health, round wins and characters, the game mode, both fighter objects, a race and its karts
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
    h = Fnv(h, RaceHash());
    return h;
}

void Settle(Settled& s) {
    s.v[kSettledWins1] = kernel::R32(kRoundWins[0]);
    s.v[kSettledWins2] = kernel::R32(kRoundWins[1]);
    s.v[kSettledMode] = GameMode();
    s.v[kSettledMatchWinner] = kernel::R32(kMatchWinner);
    uint32_t race = Race();
    s.v[kSettledRaceOver] = race && (kernel::R32(race) & 0x20000) ? 1 : 0;
    s.v[kSettledKartPlace1] = KartPlace(race, 0);
    s.v[kSettledKartPlace2] = KartPlace(race, 1);
}

uint32_t StateSwitches() { return mka::StateSwitches(); }

bool LoaderSettle(X86Context& ctx, std::chrono::milliseconds timeout) { return aio::Settle(ctx, timeout); }

void RngFrame(uint32_t seed, uint32_t frame) { rng::BeginFrame(seed, frame); }
void RngEnd() { rng::End(); }

void LogState(FILE* f) {
    uint32_t index = kernel::R32(kRandomIndex);
    std::fprintf(f, "health %g %g counters %u %u fighters %08X %08X mt %u %08X wins %u %u task %08X race %08X %u %u",
                 GuestFloat(kP1Health), GuestFloat(kP2Health), kernel::R32(kFrameCounter), kernel::R32(kGameCounter),
                 FighterHash(kP1Record), FighterHash(kP2Record), index, kernel::R32(kRandomState + (index % 624) * 4),
                 kernel::R32(kRoundWins[0]), kernel::R32(kRoundWins[1]), kernel::R32(kMainTask), RaceHash(),
                 KartPlace(Race(), 0), KartPlace(Race(), 1));
    if (uint32_t race = Race()) {
        std::fprintf(f, " karts");
        for (uint32_t slot = 0; slot < kKartSlots; slot++) std::fprintf(f, " %08X", KartHash(race, slot));
    }
}

// the two fighter objects, the fight state block and a race with its karts
void CheckBlocks(std::vector<std::pair<uint32_t, uint32_t>>& out) {
    out.push_back({kernel::R32(kP1Record + kRecordFighter), kFighterBytes});
    out.push_back({kernel::R32(kP2Record + kRecordFighter), kFighterBytes});
    out.push_back({kStateBlock, kStateBlockBytes});
    if (uint32_t race = Race()) {
        out.push_back({race, kRaceBytes});
        for (uint32_t slot = 0; slot < kKartSlots; slot++) out.push_back({Kart(race, slot), kKartBytes});
    }
}

// the .data section and its bss (0x547880..0x945104)
void DataRange(uint32_t& lo, uint32_t& hi) {
    lo = 0x547000;
    hi = 0x946000;
}

void SideBlocks(std::vector<std::pair<uint32_t, uint32_t>>& out) {
    if (uint32_t race = Race()) out.push_back({race, kRaceBytes});
}

}  // namespace mk3d::net::game

// a player's button table (sub_0007B650(record, table): the record's port's +4): the default one while a match is in
// sync, whatever the player's profile says, so both games read a logical bit the same way
X86_FUNC(sub_0007B650) {
    mk3d::net::Status st = mk3d::net::GetStatus();
    if (st == mk3d::net::Status::Barrier || st == mk3d::net::Status::Lockstep || st == mk3d::net::Status::Rollback)
        kernel::W32(ctx.esp + 8, mka::kDefaultButtons);
    __imp__sub_0007B650(ctx, base);
}
