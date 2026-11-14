// Armageddon's side of the shared launcher link (recomp/common/src/mk3d/linkgame.h): the autopilot drives its front
// end, and a match's result comes from what both games agreed on (netgame.h).
#include "mk3d/linkgame.h"

#include <cstdlib>
#include <string>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "game/autopilot.h"
#include "game/colours.h"
#include "game/events.h"
#include "game/netgame.h"
#include "kernel/kernel.h"
#include "mk3d/game.h"
#include "mk3d/netgame.h"
#include "mk3d/netplay.h"
#include "mk3d/sparring.h"
#include "mk3d/volume.h"

namespace mka {
namespace {

// The launcher's sound levels (mk3d/volume.h) over the game's own sound categories: music, environment, announcer,
// effects, fighter voices, the options block's first five floats (0.75 each by default, 0x5684B0; sub_00235BD0 puts them back after a profile's)
const bool g_volume_installed = (mk3d::volume::Install(0x723BA0, 0.75f), true);

constexpr uint32_t kStateSwitch = 0x19CA30;  // sub_0019CA30(priority, task)

autopilot::Request g_request;

// The arena of a versus fight comes from sub_000393A0, which takes the next one of a list (0x548F88, -1 at the end)
// that is not locked (64-bit masks 0x7363D8, defaults 0x548A40, sub_00039440), from a place on the list (0x831A64)
// that moves on every fight and that the boot draws from the random generator, which main seeds from the time stamp
// counter: two games would load different arenas. Both set the place from the match's seed instead (or to the stage
// the launcher names, an arena id on the list), with every arena unlocked.
constexpr uint32_t kArenaList = 0x548F88, kArenaCycle = 0x831A64, kArenaUnlocks = 0x7363D8;

// A round end (sub_00098B50) ends the match once the round's winner has as many round wins as 0x723BC0 (the options'
// rounds to win, 2 by default; 2 in an Xbox Live match): the launcher's best of N.
constexpr uint32_t kRoundsToWin = 0x723BC0;

// The game options' other settings a fight plays with (sub_00236320 sets the defaults, sub_00236150 sets one by its
// menu item, as in Deception): the round time (item 0x3331, seconds, 90 by default), the blood level (0x3333, 0 to 3,
// 3) and the death traps (0x3332, on). Each PC keeps its own options, so a launcher match sets them: the defaults for
// the round time and the blood, the launcher's death traps. The front end sets the options again on its way to the
// match (as in Deception, a profile's options become the game's: sub_00235B60 copies its block over 0x723BA0 with
// sub_00235AF0), so the match's go back whenever the block or an option is set (the hooks below). sub_0025D2D0, which
// the arenas' scripts ask (natives table 0x965270) and the fight code, gives no death traps when the option is off or
// the blood is; in a launcher match it gives the match's.
constexpr uint32_t kOptionsBlock = 0x723BA0;
constexpr uint32_t kRoundTime = 0x723BC4, kBloodLevel = 0x723BC8, kDeathTraps = 0x723BCC;

struct MatchOptions {
    bool set = false;  // a launcher fight was started: these hold from then on
    int wins_needed = 2;
    bool death_traps = true;
    bool sparring = false;  // Online Practice: no time limit (the round time 95, which the round's timer setup
                            // sub_00098A40 turns into the no time limit flag 0x54DFBC, as the practice mode's)
};
MatchOptions g_options;

// The launcher match now is a fight online, or watched: the select screen's own arena choice and the picks there are
// one game's alone, so both games set up the fight from what they agreed on (the hooks below)
bool g_online_fight = false;

// The select screen's arena panel (opened over the grid; sub_002086F0 moves through the arenas of table 0x561970)
// keeps its choice in kTrackChoice (0x831A58, -1 for none; Motor Kombat's track select uses it too), and the fight
// load's sub_000982D0 puts it over the arena the select task took from the list: online only the game of the player
// who used the panel had it, the two games loaded different arenas and parted (8 of the 36 Armageddon desync reports
// since 0.1.17). Online each player's choice goes to the other game with their pick (autopilot ChosenArena), and
// both games (a spectator's too) put the same one there: AgreedArena.
constexpr uint32_t kArenaChoiceLimit = 0x38;  // sub_000982D0 takes a choice below this
uint32_t g_match_seed = 0;

// The arena of an online fight whose players chose one on the select screen: both chose, the match's seed picks one
// of the two (bit 7 set, P2's); one chose, that one; neither, -1 (the match's own, from the seed's place on the list)
// A choice counts only when it is an arena a player can choose: on the versus list or in the arena panel's table
// (0x561970: id, name, a group; the names in the image's read-only data). Both games take the same choices the same
// way, so an arena word that names no arena (only another program could send one, and its load would end both games)
// leaves the match's own.
constexpr uint32_t kPanelTable = 0x561970;
bool Choosable(int id) {
    if (id < 0 || uint32_t(id) >= kArenaChoiceLimit) return false;
    for (uint32_t i = 0; i < 64; i++) {
        const int32_t listed = int32_t(kernel::R32(kArenaList + 4 * i));
        if (listed < 0) break;
        if (listed == id) return true;
    }
    for (uint32_t i = 0; i < 64; i++) {
        const uint32_t entry = kPanelTable + 12 * i, name = kernel::R32(entry + 4);
        if (name < 0x400000 || name >= 0x700000) break;
        if (int32_t(kernel::R32(entry)) == id) return true;
    }
    return false;
}

int AgreedArena() {
    int p1 = autopilot::ChosenArena(0), p2 = autopilot::ChosenArena(1);
    if (!Choosable(p1)) p1 = -1;
    if (!Choosable(p2)) p2 = -1;
    if (p1 >= 0 && p2 >= 0) return (g_match_seed >> 7) & 1 ? p2 : p1;
    return p1 >= 0 ? p1 : p2;
}

// A fight's two characters as it was set up: a result settled late (the versus fight already back on its select
// screen, which sets the records to none, 0x40, and then to the cursors') named no characters, so the result names
// the ones the fight set up with, and their costumes (0 or 1, the alternate: the records' bit 0)
int g_fighters[2] = {-1, -1};
int64_t g_fight_costumes[2] = {-1, -1};
const bool g_fighters_hooked = [] {
    BeforeFightSetup([](X86Context&) {
        g_fighters[0] = int(kernel::R32(kP1Character));
        g_fighters[1] = int(kernel::R32(kP2Character));
        g_fight_costumes[0] = kernel::R32(kP1Record + kRecordCostume) & 3;  // the costume and its colours
        g_fight_costumes[1] = kernel::R32(kP2Record + kRecordCostume) & 3;
    });
    return true;
}();

void ApplyOptions() {
    if (!g_options.set) return;
    if (g_options.wins_needed > 0) kernel::W32(kRoundsToWin, uint32_t(g_options.wins_needed));
    kernel::W32(kRoundTime, g_options.sparring ? 95 : 90);
    kernel::W32(kBloodLevel, 3);
    kernel::W32(kDeathTraps, g_options.death_traps ? 1 : 0);
}

// Online Practice's fight: the fighters' health kept up (sparring.h) while a round can be under way
const bool g_sparring_hook = (mk3d::OnFrameStart([](uint64_t) {
                                  if (!mk3d::sparring::On()) return;
                                  const uint32_t health[2] = {kP1Health, kP2Health};
                                  mk3d::sparring::KeepHealth(health, !InFrontEnd());
                              }),
                              true);

void PickArena(uint32_t seed, const std::string& stage) {
    uint32_t n = 0;
    while (n < 64 && int32_t(kernel::R32(kArenaList + 4 * n)) != -1) n++;
    if (!n) return;
    kernel::W32(kArenaUnlocks, 0xFFFFFFFF);
    kernel::W32(kArenaUnlocks + 4, 0xFFFFFFFF);
    uint32_t index = seed % n;
    if (!stage.empty()) {
        uint32_t id = uint32_t(std::strtoul(stage.c_str(), nullptr, 10));
        for (uint32_t i = 0; i < n; i++)
            if (kernel::R32(kArenaList + 4 * i) == id) index = i;
    }
    kernel::W32(kArenaCycle, index);
    LOGR("link: arena %u (place %u of %u on the list)", kernel::R32(kArenaList + 4 * index), index, n);
}

void Prepare(const mk3d::link::game::Start& start) {
    g_online_fight = (start.online || start.watch) && start.mode != "motor";
    g_match_seed = start.seed;
    if (start.mode == "motor") {
        autopilot::SetTrack(start.seed, start.stage);
    } else {
        PickArena(start.seed, start.stage);
        g_options.set = true;
        g_options.wins_needed = start.wins_needed;
        g_options.death_traps = start.death_traps;
        g_options.sparring = start.mode == "sparring";
        ApplyOptions();
        LOGR("link: round time %s, blood 3, death traps %s", g_options.sparring ? "none" : "90 s",
             start.death_traps ? "on" : "off");
    }
}

}  // namespace
}  // namespace mka

namespace mk3d::link::game {

using namespace mka;

void WaitForRequest() { autopilot::WaitForRequest(); }
bool AtHoldPoint() { return autopilot::AtHoldPoint(); }
bool ModeKnown(const std::string& mode) { return autopilot::ActionForMode(mode) >= 0; }

bool Begin(const Start& start) {
    autopilot::Request req;
    req.action = autopilot::ActionForMode(start.mode);
    req.names[0] = start.picks[0];
    req.names[1] = start.picks[1];
    req.unlock_all = true;
    // online, a side the launcher left without a character: that player picks on the select screen (a spectator's
    // game takes both players' picks from their games)
    req.local_side = start.online && !start.watch ? start.side : -1;
    req.watch = start.watch;
    const bool fight = start.mode == "kombat" || start.mode == "sparring";  // Online Practice picks as Kombat does
    for (int i = 0; i < 2; i++) {
        req.by_player[i] = start.online && fight && start.picks[i].empty();
        req.costumes[i] = start.online && fight ? start.costumes[i] : -1;
    }
    g_request = req;
    Prepare(start);
    return autopilot::Begin(req);
}

// a versus fight returns to its select screen, a Motor Kombat race to its kart select
void Rematch(const Start& start) {
    Prepare(start);
    autopilot::Rematch();
}

void ExpectMenu() { autopilot::ExpectMenu(); }

// the game's own state switch to the main menu task (priority 3, as the game's own way back); it leaves through the
// engine's longjmp to the main loop
void Park(X86Context& ctx) { kernel::CallGuest(ctx, kStateSwitch, {3, kMainMenuTask}); }

bool AutopilotActive() { return autopilot::Active(); }

uint64_t MatchesStarted() { return uint64_t(FightSetups()) + MatchStarts(); }

void MatchBegins(const std::string&) {}

void SettleNow(net::Settled& s) { net::game::Settle(s); }

// Fights: a player has won the rounds the match needs (player record + 0x1C, zeroed by the fight setup). Motor
// Kombat: the race is over and the finish order names both players' places; the better placed player wins, reported
// as 1-0.
bool Outcome(const std::string& mode, const net::Settled& s, int wins_needed, int& winner, uint32_t& r1, uint32_t& r2) {
    winner = 0;
    if (mode == "motor") {
        uint32_t p1 = s.v[kSettledKartPlace1], p2 = s.v[kSettledKartPlace2];
        if (s.v[kSettledRaceOver] && p1 && p2 && p1 != p2) winner = p1 < p2 ? 1 : 2;
        r1 = winner == 1 ? 1 : 0;
        r2 = winner == 2 ? 1 : 0;
        return winner != 0;
    }
    r1 = s.v[kSettledWins1];
    r2 = s.v[kSettledWins2];
    if (int(r1) >= wins_needed || int(r2) >= wins_needed) winner = int(r1) >= wins_needed ? 1 : 2;
    return winner != 0;
}

// a fight's two characters and their costumes, as it was set up (Motor Kombat's karts are picked again; 0x40 none)
bool Fighters(const std::string& mode, int out[2], int64_t costumes[2]) {
    if (mode == "motor") return false;
    out[0] = g_fighters[0];
    out[1] = g_fighters[1];
    costumes[0] = g_fight_costumes[0];
    costumes[1] = g_fight_costumes[1];
    return out[0] >= 0 && out[0] < 0x40 && out[1] >= 0 && out[1] < 0x40;
}

}  // namespace mk3d::link::game

X86_EXTERN_FUNC(__imp__sub_00236320);
X86_EXTERN_FUNC(__imp__sub_00236150);
X86_EXTERN_FUNC(__imp__sub_00235AF0);
X86_EXTERN_FUNC(__imp__sub_0025D2D0);
X86_EXTERN_FUNC(__imp__sub_000982D0);
X86_EXTERN_FUNC(__imp__sub_002056E0);

// the front end sets the game options (their defaults, or one menu item): a launcher match's go back over them
X86_FUNC(sub_00236320) {
    __imp__sub_00236320(ctx, base);
    mka::ApplyOptions();
}
X86_FUNC(sub_00236150) {
    __imp__sub_00236150(ctx, base);
    mka::ApplyOptions();
}
// sub_00235AF0(dst, src): an options block copied whole (a profile's made the game's, the options screen's backup)
X86_FUNC(sub_00235AF0) {
    const uint32_t dst = kernel::R32(ctx.esp + 4);
    __imp__sub_00235AF0(ctx, base);
    if (dst == mka::kOptionsBlock) mka::ApplyOptions();
}

// whether the arenas' death traps work (fastcall, no arguments on the stack): in a launcher match, the match's
X86_FUNC(sub_0025D2D0) {
    if (!mka::g_options.set) return __imp__sub_0025D2D0(ctx, base);
    ctx.eax = mka::g_options.death_traps ? 1u : 0u;
    ctx.esp += 4;  // ret
}

// the fight load's arena (no arguments): online and watching, the arena both games agreed from the players' choices
// on the select screen (AgreedArena), else the match's own: what this game's panel holds is one player's choice alone
X86_FUNC(sub_000982D0) {
    if (mka::g_online_fight) {
        const int32_t choice = int32_t(kernel::R32(mka::kTrackChoice));
        const int agreed = mka::AgreedArena();
        if (agreed >= 0) {
            LOGR("link: arena %d, as the players chose on the select screen (P1 %d, P2 %d)", agreed,
                 mka::autopilot::ChosenArena(0), mka::autopilot::ChosenArena(1));
            kernel::W32(mka::kTrackChoice, uint32_t(agreed));
        } else if (choice >= 0 && uint32_t(choice) < mka::kArenaChoiceLimit) {
            LOGR("link: the select screen's arena choice %d left out, the match's arena is %u", choice,
                 kernel::R32(mka::kArena));
            kernel::W32(mka::kTrackChoice, 0xFFFFFFFFu);
        }
    }
    __imp__sub_000982D0(ctx, base);
}

// The select screen's end (no arguments: the records' characters from the characters under the cursors; the select
// task then ORs the costume flags into the records and switches to the fight load). Online and watching, the records
// get the picks both games fight with whatever this screen ended on (the autopilot sends the character under the
// cursor, so they should never differ; a game set up with another character than the other's parts from it at once),
// and P2 the other colours when both picked the same character in the same costume.
X86_FUNC(sub_002056E0) {
    __imp__sub_002056E0(ctx, base);
    if (!mka::g_online_fight) {
        mka::colours::SelectEnds();  // offline: P2 the other colours in a mirror of costume and colours
        return;
    }
    using namespace mka;
    constexpr uint32_t kRecords[2] = {kP1Record, kP2Record};
    constexpr uint32_t kAlternate[2] = {0x24, 0x58}, kColours[2] = {0x28, 0x5C};  // select block flags (autopilot.cpp)
    const uint32_t block = SelectBlock();
    int character[2] = {-1, -1};
    uint32_t costume[2] = {};
    for (int s = 0; s < 2; s++) {
        if (!autopilot::AgreedPick(s, character[s], costume[s])) continue;
        const uint32_t was = kernel::R32(kRecords[s] + kRecordCharacter);
        if (was != uint32_t(character[s]))
            LOGR("link: P%d ends the select screen on character %u, the match's pick is %d: that one", s + 1, was,
                 character[s]);
        kernel::W32(kRecords[s] + kRecordCharacter, uint32_t(character[s]));
        costume[s] &= 3u;  // bit 0 the second costume, bit 1 its other colours (colours.h)
    }
    // P2 the other colours when both have the same fighter in the same costume and colours
    if (character[0] >= 0 && character[0] == character[1] && costume[0] == costume[1]) costume[1] ^= 2u;
    for (int s = 0; s < 2; s++) {
        if (character[s] < 0) continue;
        kernel::W32(kRecords[s] + kRecordCostume, (kernel::R32(kRecords[s] + kRecordCostume) & ~3u) | costume[s]);
        if (block) {
            kernel::W32(block + kAlternate[s], costume[s] & 1u);
            kernel::W32(block + kColours[s], (costume[s] >> 1) & 1u);
        }
    }
}
