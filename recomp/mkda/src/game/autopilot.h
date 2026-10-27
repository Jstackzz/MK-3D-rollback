#pragma once
// Drives the front end to a mode and characters on its own (MKDA_AUTOPILOT, or the launcher); see autopilot.cpp.
#include <string>

#include "core/input.h"

namespace mkda::autopilot {

struct Request {
    int mode = -1;             // game mode: 1 versus, 0 arcade, 6 practice (events.h kMode*)
    std::string names[2];      // characters by name or index, empty for whatever the cursor starts on
    int characters[2] = {-1, -1};
    bool unlock_all = false;
    int local_side = -1;       // online: this PC's player, 0 (P1) or 1 (P2)
    bool watch = false;        // a spectator's game: no player here, both picks come from the players' games
    bool by_player[2] = {};    // online: that side's player picks on the select screen (this PC's player with their
                               // own controller, the other one on their PC: their pick comes over netplay)
};

// the port this PC's player steers their own side's cursor with now (they pick on the select screen), -1 otherwise
int LocalPickPort();

// still pressing buttons
bool Active();
// called from the pad hook for every controller read
void Apply(int port, input::Pad& pad);

// launcher control: boot to the main menu and wait there (the hold point) until Begin
void WaitForRequest();
bool AtHoldPoint();
bool Begin(const Request& req);
// the same picks again from character select (a versus match returns there)
void Rematch();
// the same request again from the main menu
void Restart(const Request& req);
// the game is being sent back to the main menu: hold there once it is up
void ExpectMenu();
int ModeForName(const std::string& mode);  // "versus"/"kombat", "arcade", "practice"; -1 if unknown

// a character index by name (any case and punctuation: "SubZero", "Sub-Zero", "BO' RAI CHO", "cage") or number, -1
// if unknown; the names come from the game's own table
int CharacterId(const std::string& name);

}  // namespace mkda::autopilot
