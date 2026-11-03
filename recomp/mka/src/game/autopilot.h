#pragma once
// Drives the front end to a mode and characters on its own (MKA_AUTOPILOT, or later the launcher); see autopilot.cpp.
#include <cstdint>
#include <string>

#include "core/input.h"

namespace mka::autopilot {

struct Request {
    int action = -1;           // front end menu action: 6 versus, 5 arcade, 9 practice (7 Motor Kombat)
    std::string names[2];      // characters by name or index, empty for whatever the cursor starts on
    int characters[2] = {-1, -1};
    bool unlock_all = false;
    int local_side = -1;       // online: this PC's player, 0 (P1) or 1 (P2)
    bool watch = false;        // a spectator's game: no player here, both picks come from the players' games
    bool by_player[2] = {};    // online: that side's player picks on the select screen (this PC's player with their
                               // own controller, the other one on their PC: their pick comes over netplay)
    int64_t costumes[2] = {-1, -1};  // online, a side the launcher picked with its costume, 0 or 1 (the fighter a
                                     // ranked set's winner keeps): both games give it that once it is picked
};

// the port this PC's player steers their own side's cursor with now (they pick on the select screen), -1 otherwise
int LocalPickPort();
// online, a side its player picked on the select screen: the pick both games fight with (this PC's player's as it
// went to the other game, the other player's as it came; a spectator's game: both as they came), costume 0 or 1 (the
// alternate); a side the launcher picked with its costume: that one; false before the pick, or for a side the
// launcher picked otherwise
bool AgreedPick(int side, int& character, uint32_t& costume);
// online, the arena a side's player chose on the select screen's arena panel before their pick (sent with it), -1
// for none (or before the pick)
int ChosenArena(int side);
// the select screen's arena panel is open (BLACK opened it; A takes an arena, B puts the choice back, both close it)
bool ArenaPanelOpen();

// still pressing buttons
bool Active();
// called from the pad hook for every controller read
void Apply(int port, input::Pad& pad);

// launcher control: boot to the main menu and wait there (the hold point) until Begin
void WaitForRequest();
bool AtHoldPoint();
bool Begin(const Request& req);
// the same picks again from character select (a versus fight returns there)
void Rematch();
// the same request again from the main menu
void Restart(const Request& req);
// the game is being sent back to the main menu: hold there once it is up
void ExpectMenu();
int ActionForMode(const std::string& mode);  // "kombat"/"versus", "arcade", "practice", "motor"; -1 if unknown
// Motor Kombat: the track the next race is on, from the launcher's seed (or its named stage); see autopilot.cpp
void SetTrack(uint32_t seed, const std::string& stage);
// a character's index (kCharacterNames) by the game's name ("SUB-ZERO", "BO' RAI CHO"), the launcher's roster id
// ("SubZero", "BoRaiCho", "JohnnyCage", "NoobSaibot"; letters and digits compared, any case) or number; -1 if unknown
int CharacterId(const std::string& name);

}  // namespace mka::autopilot
