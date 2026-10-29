#include "app/app.h"
#include "game/diag.h"
#include "game/pads.h"
#include "mk3d/framedata.h"
#include "mk3d/link.h"
#include "mk3d/names.h"

namespace {

// window messages: F7 for the practice frame data, then the close requests' log
bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result) {
    mk3d::framedata::Message(hwnd, msg, wp, lp, result);
    return mka::WindowMessage(hwnd, msg, wp, lp, result);
}

// the overlays: the players' names over an online fight, the practice frame data
void OverlayWindow(void* hwnd) {
    mk3d::framedata::Window(hwnd);
    mk3d::names::Window(hwnd);
}

void OverlayUi(double fps) {
    mk3d::names::Ui(fps);
    mk3d::framedata::Ui(fps);
}

}  // namespace

// what the runtime asks of the game; strings must be set, hooks may stay null
const app::GameInfo app::kGame = {
    .title = L"Mortal Kombat: Armageddon", // window and splash
    .short_title = L"MKA",                 // message boxes
    .name = L"MKA",                        // window classes, crash dumps, handover event
    .log = "game",                         // log file prefix
    .settings = "settings.toml",           // settings file beside the exe
    .splash = L"splash",                   // optional splash.png or splash.bmp beside the exe
    .missing = L"Put the game in the assets folder: its disc image (.iso), or default.xbe with its folders beside it.",
    .images = {{"default.xbe", "MKA"}},    // each recompiled xbe and its exe, for title launches
    .options = {},                         // game settings kept in the settings file
    .load = nullptr,                       // reads more from the settings file's toml::table
    .save = nullptr,                       // writes more into it
    .init = mk3d::link::ApplyLauncherDisplay,  // after the settings load, before the window and boot
    .loaded = nullptr,                     // image in memory, before its entry point runs
    .window = OverlayWindow,               // the game window is up
    .message = Message,                    // window messages first; true takes one (F7, logs close requests)
    .ui = OverlayUi,                       // ImGui drawing each frame
    .pad = mka::PadHook,                   // scripted test input now, netplay input later
    .rumble = nullptr,                     // changes a rumble before it is set
};
