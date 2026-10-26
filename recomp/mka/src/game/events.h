#pragma once
// Armageddon's match state as far as netplay needs it (addresses for XBE sha1 24074b8f...).
#include <cstdint>

struct X86Context;

namespace mka {

// The game mode, set through sub_00258FF0 (read back by sub_00259000); a few places write it directly (0x11, 9, 8).
// 0 arcade, 1 versus, 4 practice (health refills), 0xC front end, 0xF Motor Kombat select, 7 Motor Kombat race.
constexpr uint32_t kGameMode = 0x8292F8;
constexpr uint32_t kModeArcade = 0, kModeVersus = 1, kModePractice = 4, kModeFrontEnd = 0xC, kModeKartSelect = 0xF,
                   kModeKart = 7;

// Player records, 0x48 apart: P1, P2, then P3 and P4 (Motor Kombat's four karts). The per-port input blocks
// (frame.h) follow the fourth.
constexpr uint32_t kP1Record = 0x831AEC, kP2Record = 0x831B34, kRecordStride = 0x48;
constexpr uint32_t kRecordPort = 0x0;         // the controller port driving it (-1 none)
constexpr uint32_t kRecordSide = 0x4;         // the player's index (0 P1, 1 P2; 5 while unassigned)
constexpr uint32_t kRecordState = 0x8;        // 0 out, 1 taking part (choosing), 2 picked (3 practice's dummy)
constexpr uint32_t kRecordHealth = 0xC;       // float, 1.0 full
constexpr uint32_t kRecordStartHealth = 0x10; // the health each round starts with (float)
constexpr uint32_t kRecordCostume = 0x14;     // bit 0 / bit 1: the alternate costumes picked
constexpr uint32_t kRecordRoundWins = 0x1C;   // zeroed by the fight setup
constexpr uint32_t kRecordMatchWins = 0x20;   // matches won in a row
constexpr uint32_t kRecordCharacter = 0x2C;   // character index (0x40 none), names at kCharacterNames
constexpr uint32_t kRecordFighter = 0x30;     // the fighter object (+0x34 its control object)
constexpr uint32_t kP1Character = kP1Record + kRecordCharacter, kP2Character = kP2Record + kRecordCharacter;
constexpr uint32_t kP1Health = kP1Record + kRecordHealth, kP2Health = kP2Record + kRecordHealth;
constexpr uint32_t kP1RoundWins = kP1Record + kRecordRoundWins, kP2RoundWins = kP2Record + kRecordRoundWins;
constexpr uint32_t kCharacterNames = 0x560FC0;  // 0x14 bytes per character, the name pointer first ("SCORPION")
constexpr uint32_t kSetPlayerActive = 0x1F8C00;  // (record, state): writes the record's state
constexpr uint32_t kArena = 0x831A54;            // the next fight's arena (the select task picks it)
constexpr uint32_t kRoundTimer = 0x831CE0;       // seconds left in the round
constexpr uint32_t kRound = 0x831CCC;            // the round being fought, from 1
constexpr uint32_t kMatchWinner = 0x830934;      // 1 P1, 2 P2; -1 until a side has its rounds
constexpr uint32_t kSrand = 0x39C6AB, kRand = 0x39C6B8;  // the C runtime's (per-thread seed at _getptd()+0x14)

// The front end's menu action: sub_001D9530(player, action) stores it in kMenuAction (0x17 while nothing is chosen)
// for the main menu task and marks the players taking part. 5 arcade, 6 versus, 9 practice, 7 Motor Kombat, 1 Konquest.
constexpr uint32_t kMenuAction = 0x5B8960, kMenuPlayer = 0x5B895C, kMenuNone = 0x17;
constexpr int kActionArcade = 5, kActionVersus = 6, kActionPractice = 9, kActionKart = 7, kActionKonquest = 1;

// the next arcade, versus or practice confirmation in the front end becomes this menu action with both players
// taking part (MKA_ACTION sets it at start); -1 turns it off
void RequestAction(int action);

// called on the main thread right before the fight setup (sub_000983A0) runs, inside the frame: netplay meets the
// other game there, after the fight has loaded and before the round timer starts from the frame counter
using FightSetupHook = void (*)(X86Context& ctx);
void BeforeFightSetup(FightSetupHook hook);

// The engine's state task: sub_0019CA30(priority, task) switches to it (kMainTask holds the task function,
// kMainTaskObject the task). A versus fight returns to its select screen.
constexpr uint32_t kMainTask = 0x5B8844, kMainTaskObject = 0x5B8840;
constexpr uint32_t kMainMenuTask = 0x19F3D0, kSelectTask = 0x2057E0, kFightLoadTask = 0x96C60, kKartSelectTask = 0x1052B0;
bool InFrontEnd();

// Character select (task 0x2057E0, for arcade, versus and practice): its screen block, the third argument of the
// screen builder sub_001DDDA0 for "common/p_select/p_select" (0x5025B0); 0 before the first one. The cursor slots are
// at +0x10 (P1) and +0x44 (P2), the characters under them at +0x1C / +0x50 (copied to the records' +0x2C).
constexpr uint32_t kSelectScreen = 0x5025B0;
constexpr uint32_t kSelectCursor[2] = {0x10, 0x44}, kSelectUnder[2] = {0x1C, 0x50};
uint32_t SelectBlock();

// Motor Kombat (menu action 7): its select task 0x1052B0 (mode 0xF, screen "common/kart/kart_main": the track, then
// each player's kart; per player a kart choice at 0x82ECA0 + player * 0xC, the karts taken in mask 0x82ECD4) confirms
// action 2 once nobody is still choosing; the race load task 0xE47B0 (mode 7, priority 10) sets its mode (the call
// returning to kRaceLoadMode: counted by MatchStarts and met like a fight setup there, before anything of the race
// exists), loads the track (kArena), makes the race object and the karts, the race setup task 0xE9F60 starts the
// race, the race runs in 0xF0940 until the race object's flag bit 17 (set when a kart, in practice a person's,
// completes the laps), and the results task 0xF1330 sorts the finish order, sets the front end mode (returning to
// kKartResultsDone) and goes back to the kart select.
constexpr uint32_t kRace = 0x5B8334;                       // the race object (0x107C bytes; 0 outside a race)
constexpr uint32_t kKarts = 0x24C, kKartOrder = 0x29C;     // in it: 8 kart handles, the finish order (kart slots)
constexpr uint32_t kKartFinisher = 0x28C;                  // in it: the handle of the kart whose finish ended it
constexpr uint32_t kKartResultsDone = 0xF2340;             // return address of the results' sub_00258FF0(0xC)
constexpr uint32_t kRaceLoadMode = 0xE47E8;                // return address of the race load's sub_00258FF0(7)
constexpr uint32_t kKartChoice = 0x82ECA0, kKartTable = 0x560DA0;  // 0x34 bytes a kart: 0 Baraka ... 9 Sub-Zero
// the track the select chose, as it numbers them (43 the first); the load turns it into kArena (+5) after the race's
// meeting point, where kArena still holds the last fight's arena
constexpr uint32_t kTrackChoice = 0x831A58;
// how many Motor Kombat races started (their load task set its mode)
uint32_t MatchStarts();

// how many fight setups (sub_000983A0) have run, and the main loop frame of the last one
uint32_t FightSetups();
uint64_t LastFightSetupFrame();
// main menu task starts and front end menu confirmations so far
uint32_t MainMenus();
uint32_t MenuActions();
// state task switches (sub_0019CA30) so far: the frame that switches ends there, the new task loads from the next one
uint32_t StateSwitches();
uint32_t GameMode();

}  // namespace mka
