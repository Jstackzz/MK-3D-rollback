#pragma once
// The game's side of the launcher protocol (MK3D_NET=1), shared by the games; see link.cpp, linkgame.h for what each
// game supplies, and docs/PROTOCOL.md section 3.

namespace mk3d::link {

// the launcher owns this process
bool Active();
// the player's own controller is kept from the game (menus are the autopilot's, the player plays the match)
bool SuppressPhysicalPads();
// GameInfo::init: the display the launcher's Settings chose, over the settings file's, for a game the launcher started
// (MK3D_DISPLAY windowed|borderless|fullscreen, MK3D_RESOLUTION WIDTHxHEIGHT or desktop, MK3D_VSYNC 0|1); its log goes
// to the session folder (MK3D_LOG)
void ApplyLauncherDisplay();

}  // namespace mk3d::link
