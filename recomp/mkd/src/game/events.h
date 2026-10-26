#pragma once
// Deception's match state as far as netplay needs it (addresses for XBE sha1 ccbcfe58...).
#include <cstdint>

struct X86Context;

namespace mkd {

constexpr uint32_t kGameMode = 0x61C414;      // written only by sub_001BC170; 0 in a plain Kombat fight, 4 refills health
constexpr uint32_t kP1Character = 0x621018;   // character index, names at 0x3F89E0 + index * 16
constexpr uint32_t kP2Character = 0x621084;
constexpr uint32_t kP1Health = 0x620FD0;      // float, 1.0 full: player record + 0xC (the next float, +0x10, is the start value)
constexpr uint32_t kP2Health = 0x62103C;      // P2 record 0x621030 + 0xC
constexpr uint32_t kHudHealth = 0x471C44;     // the bars' animated copies of the two above
constexpr uint32_t kFlags = 0x620F20;         // bit 13: the game counter 0x61C418 stops (paused)
constexpr uint32_t kSrand = 0x29B89F, kRand = 0x29B8AC;
constexpr uint32_t kP1Record = 0x620FC4, kP2Record = 0x621030;  // per-player records, 0x6C apart
constexpr uint32_t kSetPlayerActive = 0x16FBC0;                // (record, taking part)
constexpr uint32_t kMenuAction = 0x46CDB8;                     // 0x18 while nothing is chosen

// the next arcade, versus or practice confirmation in the front end becomes this menu action with both players
// taking part (MKD_ACTION sets it at start); -1 turns it off
void RequestAction(int action);

// called on the main thread right before a fight setup (sub_00083570) runs, inside the frame: netplay meets the other
// game there, before the fight's timers start from the frame counter
using FightSetupHook = void (*)(X86Context& ctx);
void BeforeFightSetup(FightSetupHook hook);

// how many side mode matches started (Chess Kombat's board game 0x11DD20, Puzzle Kombat's game 0xF2850 or, against
// the computer, 0xDCE60)
uint32_t MatchStarts();
constexpr uint32_t kModeChess = 9, kModePuzzle = 6;  // GameMode() from their select screens on
constexpr uint32_t kModeChessBattle = 10;             // a chess battle (a fight between two pieces)
constexpr uint32_t kArena = 0x620F38;  // the next fight's arena: the select tasks set it (sub_00040640 cycles a list)

// The engine's state task: sub_000E0190(priority, task) switches to it. A match ends in the front end again (the
// main menu or a select screen); Chess Kombat's game over screen (sub_0010E6D0) first sets mode 12, its winner in
// kChessWinner (0: P1) and each side's pieces left in kChessPieces (P1, then P2 0x748 further).
constexpr uint32_t kMainTask = 0x46CCB8;
constexpr uint32_t kMainMenuTask = 0xE3770, kVersusSelectTask = 0x179A60, kPuzzleSelectTask = 0x17A270,
                   kChessSelectTask = 0x17A500;
constexpr uint32_t kModeChessOver = 12;
constexpr uint32_t kChessWinner = 0x600518, kChessPieces = 0x5FFD44, kChessPiecesStride = 0x748;
bool InFrontEnd();

// Puzzle Kombat's match end (sub_000F7180, once a match): the matches finished since its select screen opened
// (0x620324, zeroed when the player leaves it), the winner (0x620320: 1 P1, 2 P2) and each player's wins in a row
// ("Wins: n" under the wells; the loser's goes back to 0)
constexpr uint32_t kPuzzleMatches = 0x620324, kPuzzleWinner = 0x620320;
constexpr uint32_t kPuzzleStreak[2] = {0x62100C, 0x621078};

// Chess Kombat's select screen (task 0x17A500): its team block, 0 before the first one. Five picks a player (leader,
// champion, sorcerer, shifter, grunt), P1's from +0xC and P2's from +0x2C, kChessEmpty while not picked; a pick is a
// character index. When the match starts the select task copies them to kChessTeams (P1) and kChessTeams + 0x1C (P2).
constexpr uint32_t kChessSelectScreen = 0x3A2840;  // the screen's name, "common/board_game/bg_player_select..."
constexpr uint32_t kChessEmpty = 0x29;
constexpr uint32_t kChessTeams = 0x60166C;
uint32_t ChessTeams();

// how many fight setups (sub_00083570) have run, and the main loop frame of the last one
uint32_t FightSetups();
// main menu task starts and front end menu confirmations so far
uint32_t MainMenus();
uint32_t MenuActions();
// state task switches (sub_000E0190) so far: the frame that switches ends there, the new task loads from the next one
uint32_t StateSwitches();
uint64_t LastFightSetupFrame();
uint32_t GameMode();

}  // namespace mkd
