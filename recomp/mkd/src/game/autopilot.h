#pragma once
// Drives the front end to a mode and characters on its own (MKD_AUTOPILOT, or the launcher); see autopilot.cpp.
#include <string>

#include "core/input.h"

namespace mkd::autopilot {

struct Request {
    int action = -1;           // front end menu action: 7 versus, 8 chess, 9 puzzle, 6 arcade, 0xB practice
    std::string names[2];      // characters by name or index, empty for whatever the cursor starts on
    int characters[2] = {-1, -1};
    bool unlock_all = false;
};

// still pressing buttons
bool Active();
// called from the pad hook for every controller read
void Apply(int port, input::Pad& pad);

// launcher control: boot to the main menu and wait there (the hold point) until Begin
void WaitForRequest();
bool AtHoldPoint();
bool Begin(const Request& req);
// the same picks again from character select (Versus and Puzzle Kombat return there after a match)
void Rematch();
// the same request again from the main menu (Chess Kombat returns there after a game)
void Restart(const Request& req);
// the game is being sent back to the main menu: hold there once it is up
void ExpectMenu();
int ActionForMode(const std::string& mode);  // "kombat"/"versus", "chess", "puzzle", "arcade", "practice"; -1 if unknown

}  // namespace mkd::autopilot
