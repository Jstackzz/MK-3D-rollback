#include "app/app.h"
#include "game/pads.h"
#include "mk3d/framedata.h"
#include "mk3d/link.h"

// what the runtime asks of the game; strings must be set, hooks may stay null
const app::GameInfo app::kGame = {
    .title = L"Mortal Kombat: Deception",  // window and splash
    .short_title = L"MKD",                 // message boxes
    .name = L"MKD",                        // window classes, crash dumps, handover event
    .log = "game",                         // log file prefix
    .settings = "settings.toml",           // settings file beside the exe
    .splash = L"splash",                   // optional splash.png or splash.bmp beside the exe
    .missing = L"Put the game in the assets folder: its disc image (.iso), or default.xbe with its folders beside it.",
    .images = {{"default.xbe", "MKD"}},    // each recompiled xbe and its exe, for title launches
    .options = {},                         // game settings kept in the settings file
    .load = nullptr,                       // reads more from the settings file's toml::table
    .save = nullptr,                       // writes more into it
    .init = mk3d::link::ApplyLauncherDisplay,  // after the settings load, before the window and boot
    .loaded = nullptr,                     // image in memory, before its entry point runs
    .window = mk3d::framedata::Window,     // the game window is up
    .message = mk3d::framedata::Message,   // window messages first; true takes one
    .ui = mk3d::framedata::Ui,             // ImGui drawing each frame
    .pad = mkd::PadHook,                   // scripted test input now, netplay input later
    .rumble = nullptr,                     // changes a rumble before it is set
};
