#pragma once
// Deadly Alliance's match state as far as netplay needs it (addresses for XBE sha1 5aba0b06..., research/mkda-rev1/notes.md).
#include <cstdint>

struct X86Context;

namespace mkda {

// The game mode, written by sub_0012FDE0 (and set to 8 directly by the attract and main menu setups)
constexpr uint32_t kGameMode = 0x42A89C;
constexpr uint32_t kModeArcade = 0, kModeVersus = 1, kModeKonquest = 2, kModeKrypt = 3, kModeOptions = 4,
                   kModeProfile = 5, kModePractice = 6, kModeKontent = 7, kModeFrontEnd = 8;
constexpr uint32_t kSetGameMode = 0x12FDE0;  // (mode)

// Players. A player takes part while its state is not 0: 2 in (choosing), 3 picked (select screens reset it to 2),
// 4 a demo player reading recorded input; sub_0012FDF0(player, state) sets it.
constexpr uint32_t kPlayerState[2] = {0x2EA550, 0x2EA554};
constexpr uint32_t kSetPlayerState = 0x12FDF0;
constexpr uint32_t kCharacter[2] = {0x271F00, 0x271F04};  // character index, 0x18 none; names below
// each player's pointer to their costume (0 the usual one): the pick (sub_001112A0) sets it from the button held while
// confirming (logical action 0xB, sub_00075B50) when sub_00116380 has the alternate open, and a mirror match turns the
// costume of the player picking to the other one
constexpr uint32_t kCostume[2] = {0x273DD4, 0x273E0C};
constexpr uint32_t kCharacterNames = 0x243BDC;              // + index * 0x110: pointer to the name ("SCORPION")
constexpr uint32_t kCharacterStride = 0x110;
constexpr uint32_t kHealth[2] = {0x42C774, 0x42C770};       // float, 1.0 full (sub_0006F7F0 refills it every round)
constexpr uint32_t kRoundWins[2] = {0x42C76C, 0x42C768};    // zeroed by the fight setup
constexpr uint32_t kFighter[2] = {0x2EA540, 0x2EA544};      // fighter objects (+0x144 character, +0x148 player)
constexpr uint32_t kRoundsToWin = 0x31FD18, kRound = 0x42C778, kRoundTime = 0x42C77C;
constexpr uint32_t kMatchWinner = 0x42C758;                  // 0 P1, 1 P2, -1 while nobody has won
constexpr uint32_t kRand = 0x1C60C4;                         // the C runtime's rand; no srand is linked

// The front end's menus: a stack of menu ids (0x2D4C98, depth 0x2D4CFC; menus take input while 0x2CE63C is set),
// each menu's selected item at kMenuSelection + id * kMenuStride (UP/DOWN of any player move it, sub_000CC6F0). The
// main menu is menu 1: ARCADE 0, VERSUS 1, PRACTICE 2, KONQUEST 3, THE KRYPT 4, PLAYER PROFILE 5, OPTIONS 6, KONTENT 7
// (items {name, draw, confirm} at 0x2371C0); the pause menus are 0 and 4.
constexpr uint32_t kMenuSelection = 0x268D68, kMenuStride = 0x1C, kMainMenu = 1;
int TopMenu();  // the menu on top of the stack, -1 when no menu takes input
// controllers the game has open (XInput handle per port at 0x2EFD9C + port * 0x78, capabilities at 0x2EFD80 + port *
// 0x78, sub_001435E0 counts gamepads); the main menu's VERSUS needs two
int OpenPads();

// the next arcade, versus or practice confirmation in the main menu becomes this game mode with both players taking
// part (MKDA_MODE sets it at start, by number or name); -1 turns it off. Not needed with a second controller: the
// menu offers VERSUS once two controllers are open, and the second one joins at character select by pressing a button.
void RequestMode(int mode);
// "arcade" 0, "versus"/"kombat" 1, "practice" 6, a number as it is; -1 if unknown
int ModeByName(const char* name);

// called on the main thread right before the fight setup (sub_0006F680) runs, inside the frame: netplay meets the
// other game there, before the round loop and its timers start
using FightSetupHook = void (*)(X86Context& ctx);
void BeforeFightSetup(FightSetupHook hook);

// The engine's state task: sub_000CB140(priority, task) switches to it (ends the current state, starts the new task,
// longjmps to the main loop). Front end states: the main menu (0xE24E0, its page loop 0xE25C0) and character select
// (0x10C640, modes 0, 1 and 6); a fight is the task 0x6E010 (loads, then the round loop 0x6E290).
constexpr uint32_t kMainTask = 0x2D4BF8;
constexpr uint32_t kMainMenuTask = 0xE24E0, kSelectTask = 0x10C640, kFightTask = 0x6E010, kLadderTask = 0xC7DF0;
// the attract sequence (title, movies, demo fights); a fight round that ends with both fighters untouched (time out
// at full health, sub_000702B0) sends the game back to it
constexpr uint32_t kAttractTask = 0x238E0;
constexpr uint32_t kStateSwitch = 0xCB140;  // (priority, task); the priority is only main's setjmp result
bool InFrontEnd();

// The next fight's arena: the fight task's arena pick (sub_0006F570) takes 0x42C754 when it holds one (0 to 24, then
// -1 again), else in a versus fight the next unlocked one of the list 0x24D800 (-1 ends it) from the cycle index
// 0x2C5DE8, which moves on with every fight. kArena is the arena the fight loads (and, 0x19, "the next one").
constexpr uint32_t kNextArena = 0x42C754, kArena = 0x24C6E8, kArenaList = 0x24D800, kArenaCycle = 0x2C5DE8;
constexpr uint32_t kArenaDefaults = 0x273584;  // arenas open without a profile, a bit each (profiles add 0x421538)
// every fight from now on gets this arena (written to kNextArena right before the pick); -1 off
void SetNextArena(int arena);

// GAME OPTIONS (sub_000CE210): difficulty, rounds to win, round time in seconds (below 20: no time limit; sub_0006F8C0
// sets 0x42C77C from it), a bonus game (Test Your Might or Sight) instead of the next fight once the fight count
// 0x42C788 reaches it (0xB never; decided by the fight task sub_0006E010 before anything loads), fatalities on
constexpr uint32_t kOptDifficulty = 0x31FD14, kOptRounds = 0x31FD18, kOptRoundTime = 0x31FD1C,
                   kOptBonusGames = 0x31FD20, kOptFatalities = 0x31FD24;
constexpr uint32_t kBonusGamesNever = 0xB;

// how many fight setups (sub_0006F680) have run, and the main loop frame of the last one
uint32_t FightSetups();
uint64_t LastFightSetupFrame();
// main menu starts (the state task 0xE24E0) and main menu confirmations (sub_000E3CC0) so far
uint32_t MainMenus();
uint32_t MenuActions();
// state task switches (sub_000CB140) so far: the frame that switches ends there, the new task loads from the next one
uint32_t StateSwitches();
uint32_t GameMode();

}  // namespace mkda
