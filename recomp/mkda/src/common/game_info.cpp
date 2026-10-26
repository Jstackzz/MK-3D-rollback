#include "app/app.h"
#include "game/diag.h"
#include "game/pads.h"
#include "mk3d/link.h"

// what the runtime asks of the game; strings must be set, hooks may stay null
const app::GameInfo app::kGame = {
    .title = L"Mortal Kombat: Deadly Alliance", // window and splash
    .short_title = L"MKDA",                     // message boxes
    .name = L"MKDA",                            // window classes, crash dumps, handover event
    .log = "game",                              // log file prefix
    .settings = "settings.toml",                // settings file beside the exe
    .splash = L"splash",                        // optional splash.png or splash.bmp beside the exe
    .missing = L"Put the game in the assets folder: its disc image (.iso), or default.xbe with its folders beside it.",
    .images = {{"default.xbe", "MKDA"}},        // each recompiled xbe and its exe, for title launches
    .options = {},                              // game settings kept in the settings file
    .load = nullptr,                            // reads more from the settings file's toml::table
    .save = nullptr,                            // writes more into it
    .init = mk3d::link::ApplyLauncherDisplay,   // after the settings load, before the window and boot
    .loaded = nullptr,                          // image in memory, before its entry point runs
    .window = nullptr,                          // the game window is up
    .message = mkda::WindowMessage,             // window messages first; true takes one (logs close requests)
    .ui = nullptr,                              // ImGui drawing each frame
    .pad = mkda::PadHook,                       // scripted test input now, netplay input later
    .rumble = nullptr,                          // changes a rumble before it is set
};
