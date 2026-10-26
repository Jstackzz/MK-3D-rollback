// Game events the netplay layer keys on (research/mkda-rev1/notes.md, "Game layer"):
//   sub_0012FDE0(mode)    the game mode setter (0x42A89C): 8 front end, then the main menu's choice
//   sub_000E3CC0()        the main menu confirming an item: its selection (0x268D68 + menu * 0x1C) becomes the mode
//                         (ARCADE 0, VERSUS 1 or, with fewer than two controllers open, nothing, PRACTICE 6,
//                         KONQUEST 2, THE KRYPT 3, PLAYER PROFILE 5, OPTIONS 4, KONTENT 7). Each item's confirm
//                         callback (table 0x2371C0) marks the confirming player as taking part (state 2), VERSUS's
//                         (sub_000CD980) both players. MKDA_MODE=<mode> turns the next arcade, versus or practice
//                         confirmation into that mode (versus: both players in), for a session without a second pad.
//   sub_0006F680()        fight setup: once per match, in the fight task's last loading frame (9 frames after both
//                         fighters are loaded): round wins, winner, round number, health bars; the round loop
//                         (sub_0006E290) starts the next frame
//   sub_000CB140(priority, task)  the state switch (longjmps to the main loop)
//   sub_000E24E0()        the main menu's state task
// Each is logged (MKDA_EVENT_LOG=1) and counted; the counters are what netplay and the autopilot wait on.
#include "game/events.h"

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "game/frame.h"
#include "kernel/kernel.h"

X86_EXTERN_FUNC(__imp__sub_0012FDE0);
X86_EXTERN_FUNC(__imp__sub_000E3CC0);
X86_EXTERN_FUNC(__imp__sub_0006F680);
X86_EXTERN_FUNC(__imp__sub_000CB140);
X86_EXTERN_FUNC(__imp__sub_000E24E0);
X86_EXTERN_FUNC(__imp__sub_000E25C0);
X86_EXTERN_FUNC(__imp__sub_000719C0);
X86_EXTERN_FUNC(__imp__sub_0006F7F0);
X86_EXTERN_FUNC(__imp__sub_0012FDF0);
X86_EXTERN_FUNC(__imp__sub_001112A0);
X86_EXTERN_FUNC(__imp__sub_000EC810);

namespace mkda {
namespace {

constexpr uint32_t kMenuStack = 0x2D4C98, kMenuDepth = 0x2D4CFC, kMenuActive = 0x2CE63C;
constexpr uint32_t kMenuPlayer = 0x2D4CF8;  // the player whose press confirmed the item
constexpr uint32_t kSelectCursor[2] = {0x2EA660, 0x2EA65C};
constexpr uint32_t kPadHandle = 0x2EFD9C, kPadCaps = 0x2EFD80, kPadStride = 0x78;

std::atomic<uint32_t> g_fights{0};
std::atomic<uint32_t> g_menus{0};
std::atomic<uint32_t> g_actions{0};
std::atomic<uint32_t> g_switches{0};
std::atomic<uint64_t> g_fight_frame{0};

bool Logging() {
    static const bool on = std::getenv("MKDA_EVENT_LOG") != nullptr;
    return on;
}

unsigned long long F() { return (unsigned long long)Frame(); }

std::vector<FightSetupHook>& SetupHooks() {
    static std::vector<FightSetupHook> v;
    return v;
}

float Health(int player) {
    uint32_t v = kernel::R32(kHealth[player]);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

// the fight as it goes, in the log: health and round wins whenever they change, and the pause flag with the counters
void WatchFight(uint64_t frame) {
    static float health[2] = {-1, -1};
    static uint32_t wins[2] = {~0u, ~0u};
    static uint32_t paused = 0;
    if (kernel::R32(kPaused) != paused) {
        paused = kernel::R32(kPaused);
        LOGI("event: frame %llu paused %u, counters %u %u", (unsigned long long)frame, paused,
             kernel::R32(kFrameCounter), kernel::R32(kGameCounter));
    }
    if (kernel::R32(kMainTask) != kFightTask) return;
    float h[2] = {Health(0), Health(1)};
    uint32_t w[2] = {kernel::R32(kRoundWins[0]), kernel::R32(kRoundWins[1])};
    if (h[0] == health[0] && h[1] == health[1] && w[0] == wins[0] && w[1] == wins[1]) return;
    LOGI("event: frame %llu health %.3f %.3f, round wins %u %u, round %u, time %u, winner %d", (unsigned long long)frame,
         h[0], h[1], w[0], w[1], kernel::R32(kRound), kernel::R32(kRoundTime), int32_t(kernel::R32(kMatchWinner)));
    for (int i = 0; i < 2; i++) {
        health[i] = h[i];
        wins[i] = w[i];
    }
}

const bool g_watch = [] {
    if (Logging()) OnFrameStart(&WatchFight);
    return true;
}();

}  // namespace

int TopMenu() {
    if (!kernel::R32(kMenuActive)) return -1;
    int depth = int(kernel::R32(kMenuDepth));
    if (depth < 1 || depth > 10) return -1;
    return int(kernel::R32(kMenuStack + uint32_t(depth - 1) * 4));
}

int OpenPads() {
    int n = 0;
    for (uint32_t port = 0; port < 4; port++) {
        if (!kernel::R32(kPadHandle + port * kPadStride)) continue;
        uint8_t subtype = *static_cast<const uint8_t*>(kernel::HostPtr(kPadCaps + port * kPadStride));
        if (subtype == 1 || subtype == 2 || subtype == 0x20 || subtype == 0x21) n++;
    }
    return n;
}

int ModeByName(const char* name) {
    if (!name || !*name) return -1;
    if (std::isdigit(static_cast<unsigned char>(name[0]))) return int(std::strtol(name, nullptr, 0));
    struct {
        const char* name;
        int mode;
    } modes[] = {{"arcade", int(kModeArcade)}, {"versus", int(kModeVersus)}, {"kombat", int(kModeVersus)},
                 {"practice", int(kModePractice)}};
    for (auto& m : modes)
        if (_stricmp(name, m.name) == 0) return m.mode;
    return -1;
}

std::atomic<int> g_mode_override{ModeByName(std::getenv("MKDA_MODE"))};

void RequestMode(int mode) { g_mode_override = mode; }

uint32_t FightSetups() { return g_fights.load(); }
uint64_t LastFightSetupFrame() { return g_fight_frame.load(); }
uint32_t MainMenus() { return g_menus.load(); }
uint32_t MenuActions() { return g_actions.load(); }
uint32_t StateSwitches() { return g_switches.load(); }
uint32_t GameMode() { return kernel::R32(kGameMode); }

bool InFrontEnd() {
    uint32_t task = kernel::R32(kMainTask);
    return task == kMainMenuTask || task == kSelectTask;
}

std::atomic<int> g_next_arena{-1};
void SetNextArena(int arena) { g_next_arena = arena; }

void BeforeFightSetup(FightSetupHook hook) { SetupHooks().push_back(hook); }

}  // namespace mkda

X86_FUNC(sub_0012FDE0) {
    uint32_t mode = kernel::R32(ctx.esp + 4);
    if (mkda::Logging())
        LOGI("event: frame %llu game mode %u -> %u (from %08X)", mkda::F(), mkda::GameMode(), mode, kernel::R32(ctx.esp));
    __imp__sub_0012FDE0(ctx, base);
}

// the main menu confirmed its selected item; the redirect replaces the mode it set
X86_FUNC(sub_000E3CC0) {
    int menu = mkda::TopMenu();
    int item = menu >= 0 && menu < 10 ? int(kernel::R32(mkda::kMenuSelection + uint32_t(menu) * mkda::kMenuStride)) : -1;
    int to = mkda::g_mode_override.load();
    // only between the modes of the first three items, ARCADE, VERSUS and PRACTICE (they share the select screen)
    bool target_ok = to == int(mkda::kModeArcade) || to == int(mkda::kModeVersus) || to == int(mkda::kModePractice);
    bool redirect = target_ok && menu == int(mkda::kMainMenu) && item >= 0 && item <= 2;
    __imp__sub_000E3CC0(ctx, base);
    uint32_t n = ++mkda::g_actions;
    if (redirect) {
        uint32_t player = kernel::R32(mkda::kMenuPlayer) ? 1 : 0;
        kernel::CallGuest(ctx, mkda::kSetGameMode, {uint32_t(to)});
        kernel::CallGuest(ctx, mkda::kSetPlayerState, {player, 2});
        // versus takes both players; the others start with the confirming one alone
        kernel::CallGuest(ctx, mkda::kSetPlayerState, {player ^ 1, uint32_t(to) == mkda::kModeVersus ? 2u : 0u});
        LOGI("event: main menu item %d redirected to mode %d (players %u %u)", item, to,
             kernel::R32(mkda::kPlayerState[0]), kernel::R32(mkda::kPlayerState[1]));
    }
    if (mkda::Logging())
        LOGI("event: frame %llu menu confirm #%u: menu %d item %d by player %u, mode %u, players %u %u", mkda::F(), n,
             menu, item, kernel::R32(mkda::kMenuPlayer), mkda::GameMode(), kernel::R32(mkda::kPlayerState[0]),
             kernel::R32(mkda::kPlayerState[1]));
}

X86_FUNC(sub_0006F680) {
    for (mkda::FightSetupHook hook : mkda::SetupHooks()) hook(ctx);
    __imp__sub_0006F680(ctx, base);
    mkda::g_fight_frame = mkda::Frame();
    uint32_t n = ++mkda::g_fights;
    if (mkda::Logging())
        LOGI("event: frame %llu fight setup #%u, mode %u, characters %u vs %u, counters %u %u", mkda::F(), n,
             mkda::GameMode(), kernel::R32(mkda::kCharacter[0]), kernel::R32(mkda::kCharacter[1]),
             kernel::R32(mkda::kFrameCounter), kernel::R32(mkda::kGameCounter));
}

// the state switch: counted first, since it leaves the frame through the engine's longjmp to the main loop
X86_FUNC(sub_000CB140) {
    mkda::g_switches++;
    if (mkda::Logging())
        LOGI("event: frame %llu state switch to %08X (priority %u) from %08X", mkda::F(), kernel::R32(ctx.esp + 8),
             kernel::R32(ctx.esp + 4), kernel::R32(mkda::kMainTask));
    __imp__sub_000CB140(ctx, base);
}

// the main menu's state task (title, a match or a select screen switch to it); it sets the menu up, then hands over
// to the menu page loop sub_000E25C0
X86_FUNC(sub_000E24E0) {
    uint32_t n = ++mkda::g_menus;
    if (mkda::Logging()) LOGI("event: frame %llu main menu #%u", mkda::F(), n);
    __imp__sub_000E24E0(ctx, base);
}

X86_FUNC(sub_000E25C0) {
    if (mkda::Logging()) LOGI("event: frame %llu main menu page", mkda::F());
    __imp__sub_000E25C0(ctx, base);
}

// the fight task saw both fighters loaded (9 frames before the fight setup)
X86_FUNC(sub_000719C0) {
    if (mkda::Logging())
        LOGI("event: frame %llu fighters loaded, counters %u %u", mkda::F(), kernel::R32(mkda::kFrameCounter),
             kernel::R32(mkda::kGameCounter));
    __imp__sub_000719C0(ctx, base);
}

// a round starts (health refilled, round timer)
X86_FUNC(sub_0006F7F0) {
    __imp__sub_0006F7F0(ctx, base);
    if (mkda::Logging())
        LOGI("event: frame %llu round %u starts, wins %u %u", mkda::F(), kernel::R32(mkda::kRound),
             kernel::R32(mkda::kRoundWins[0]), kernel::R32(mkda::kRoundWins[1]));
}

X86_FUNC(sub_0012FDF0) {
    if (mkda::Logging())
        LOGI("event: frame %llu player %u state %u -> %u (from %08X)", mkda::F(), kernel::R32(ctx.esp + 4),
             kernel::R32(ctx.esp + 4) < 2 ? kernel::R32(mkda::kPlayerState[kernel::R32(ctx.esp + 4)]) : 0xFFFFFFFFu,
             kernel::R32(ctx.esp + 8), kernel::R32(ctx.esp));
    __imp__sub_0012FDF0(ctx, base);
}

// character select: a player's pick (sets the character from the cursor's slot, state 3)
X86_FUNC(sub_001112A0) {
    uint32_t player = kernel::R32(ctx.esp + 4);
    __imp__sub_001112A0(ctx, base);
    if (mkda::Logging() && player < 2)
        LOGI("event: frame %llu player %u picks slot %u: character %u, state %u", mkda::F(), player,
             kernel::R32(mkda::kSelectCursor[player]), kernel::R32(mkda::kCharacter[player]),
             kernel::R32(mkda::kPlayerState[player]));
}

// the fight task picks the arena it loads (sub_0006F570): a requested one goes where the engine takes a chosen arena
X86_EXTERN_FUNC(__imp__sub_0006F570);
X86_FUNC(sub_0006F570) {
    int arena = mkda::g_next_arena.load();
    if (arena >= 0) kernel::W32(mkda::kNextArena, uint32_t(arena));
    __imp__sub_0006F570(ctx, base);
    if (mkda::Logging())
        LOGI("event: frame %llu arena %u%s", mkda::F(), kernel::R32(mkda::kArena), arena >= 0 ? " (requested)" : "");
}

// the engine's random generator is seeded once, from main (0xCACFB)
X86_FUNC(sub_000EC810) {
    if (mkda::Logging()) LOGI("event: frame %llu rng seed %08X", mkda::F(), kernel::R32(ctx.esp + 4));
    __imp__sub_000EC810(ctx, base);
}
