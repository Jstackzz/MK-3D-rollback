cmake_minimum_required(VERSION 3.21)
project(mk3d_launcher VERSION 0.1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Network)
qt_standard_project_setup()

qt_add_executable(MK3DLauncher WIN32
    src/main.cpp
    src/AppPaths.cpp src/AppPaths.h
    src/WinUtil.cpp src/WinUtil.h
    src/GameData.cpp src/GameData.h
    src/Style.cpp src/Style.h
    src/ServerClient.cpp src/ServerClient.h
    src/UdpRendezvous.cpp src/UdpRendezvous.h
    src/GameProcess.cpp src/GameProcess.h
    src/MatchController.cpp src/MatchController.h
    src/Widgets.cpp src/Widgets.h
    src/Dialogs.cpp src/Dialogs.h
    src/PlayPages.cpp src/PlayPages.h
    src/LobbyPage.cpp src/LobbyPage.h
    src/ToolPages.cpp src/ToolPages.h
    src/Controls.cpp src/Controls.h
    src/ControlsPage.cpp src/ControlsPage.h
    src/Xiso.cpp src/Xiso.h
    src/Upnp.cpp src/Upnp.h
    src/MainWindow.cpp src/MainWindow.h
)

qt_add_resources(MK3DLauncher "gamedata"
    PREFIX "/data"
    BASE data
    FILES data/mkda.json data/mkd.json data/mka.json
)

target_compile_definitions(MK3DLauncher PRIVATE
    MK3D_LAUNCHER_VERSION="${PROJECT_VERSION}"
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
)
target_link_libraries(MK3DLauncher PRIVATE Qt6::Widgets Qt6::Network dwmapi)
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
