cmake_minimum_required(VERSION 3.21)
project(mk3d_launcher VERSION 1.0.4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Network)
qt_standard_project_setup()

# SDL3 for controllers (GamepadNav): the games' version and settings (external/XboxXBERecomp/cmake/XbeRecomp.cmake),
# static so it links into the exe
include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-3.4.16 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(SDL3)

qt_add_executable(MK3DLauncher WIN32
    src/main.cpp
    src/AppPaths.cpp src/AppPaths.h
    src/WinUtil.cpp src/WinUtil.h
    src/GameData.cpp src/GameData.h
    src/Style.cpp src/Style.h
    src/ServerClient.cpp src/ServerClient.h
    src/UdpRendezvous.cpp src/UdpRendezvous.h
    src/TunnelBridge.cpp src/TunnelBridge.h
    src/GameProcess.cpp src/GameProcess.h
    src/MatchController.cpp src/MatchController.h
    src/FeedLink.cpp src/FeedLink.h
    src/ReportSender.cpp src/ReportSender.h
    src/GamepadNav.cpp src/GamepadNav.h
    src/Widgets.cpp src/Widgets.h
    src/Dialogs.cpp src/Dialogs.h
    src/PlayPages.cpp src/PlayPages.h
    src/LobbyPage.cpp src/LobbyPage.h
    src/ToolPages.cpp src/ToolPages.h
    src/Controls.cpp src/Controls.h
    src/ControlsPage.cpp src/ControlsPage.h
    src/Jukebox.cpp src/Jukebox.h
    src/JukeboxPage.cpp src/JukeboxPage.h
    src/Xiso.cpp src/Xiso.h
    src/TitleImage.cpp src/TitleImage.h
    src/Upnp.cpp src/Upnp.h
    src/Updater.cpp src/Updater.h
    src/MainWindow.cpp src/MainWindow.h
    data/logo/MK3DLauncher.rc  # the exe's icon
)
# its version and publisher, as Windows and antivirus programs read them (the games share it)
include(${CMAKE_CURRENT_SOURCE_DIR}/../recomp/common/version/version.cmake)
mk3d_version_info(MK3DLauncher MK3DLauncher "MK3D Online launcher")

qt_add_resources(MK3DLauncher "gamedata"
    PREFIX "/data"
    BASE data
    FILES data/mkda.json data/mkd.json data/mka.json data/jukebox.json
)

# the jukebox's built-in soundtrack descriptions (tools/py/make_soundtracks.py): a music folder named like one of
# them gets its tracks' kinds, keywords and select-screen track from here when it has no soundtrack.json of its own
file(GLOB JUKEBOX_SOUNDTRACKS CONFIGURE_DEPENDS data/soundtracks/*.json)
qt_add_resources(MK3DLauncher "soundtracks"
    PREFIX "/soundtracks"
    BASE data/soundtracks
    FILES ${JUKEBOX_SOUNDTRACKS}
)

# the MK3D logo: the header and dialogs (mk3d-logo.png) and the window icon (icon-N.png); tools/py/make_logo.py makes
# them and mk3d.ico from the logo art
qt_add_resources(MK3DLauncher "logo"
    PREFIX "/logo"
    BASE data/logo
    FILES data/logo/mk3d-logo.png data/logo/icon-16.png data/logo/icon-24.png data/logo/icon-32.png
          data/logo/icon-48.png data/logo/icon-64.png data/logo/icon-128.png data/logo/icon-256.png
)

# the ranks' badges by ranked title (Fighter: fighter.png), lowest first as the server's ladder; tools/py/make_ranks.py
# makes them from the rank art in art/ranks
qt_add_resources(MK3DLauncher "ranks"
    PREFIX "/ranks"
    BASE data/ranks
    FILES data/ranks/beginner.png data/ranks/learner.png data/ranks/fighter.png data/ranks/expert.png
          data/ranks/warrior.png data/ranks/gladiator.png data/ranks/master.png data/ranks/commander.png
          data/ranks/grandmaster.png data/ranks/warlord.png data/ranks/chosen-one.png data/ranks/demi-god.png
          data/ranks/god.png data/ranks/elder-god.png data/ranks/titan.png data/ranks/eldest-god.png
)

qt_add_resources(MK3DLauncher "sounds"
    PREFIX "/sounds"
    BASE data/sounds
    FILES data/sounds/match-found.wav  # tools/py/make_chime.py
)

# the jukebox page (docs/JUKEBOX.md): a test-build feature for now, off in the public launcher
option(MK3D_JUKEBOX "The Jukebox page and the --jukebox command" OFF)
target_compile_definitions(MK3DLauncher PRIVATE
    MK3D_LAUNCHER_VERSION="${PROJECT_VERSION}"
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    $<$<BOOL:${MK3D_JUKEBOX}>:MK3D_JUKEBOX=1>
)
target_link_libraries(MK3DLauncher PRIVATE Qt6::Widgets Qt6::Network SDL3::SDL3-static dwmapi advapi32 winmm
    mfplat mfreadwrite mfuuid xaudio2 ole32)  # the jukebox: Media Foundation decoding, XAudio2 previews
set_target_properties(MK3DLauncher PROPERTIES OUTPUT_NAME "MK3DLauncher")

if(MSVC)
    target_compile_options(MK3DLauncher PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
endif()

# Copy the Qt runtime next to the exe:  cmake --build <dir> --target deploy
find_program(WINDEPLOYQT windeployqt HINTS "${Qt6_DIR}/../../../bin")
if(WINDEPLOYQT)
    add_custom_target(deploy
        COMMAND "${WINDEPLOYQT}" --no-translations --no-system-d3d-compiler --no-opengl-sw
                --no-compiler-runtime "$<TARGET_FILE:MK3DLauncher>"
        DEPENDS MK3DLauncher
        COMMENT "windeployqt MK3DLauncher"
        VERBATIM)
endif()
