# Mortal Kombat: Armageddon (original Xbox, title 4D570034), statically recompiled

cmake_minimum_required(VERSION 3.25)
project(game LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# the SDK: sdk/ here, two folders up inside the SDK's templates, a checkout beside this repository, else downloaded
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/sdk/cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../..)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../XboxXBE-Recomp/cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../XboxXBE-Recomp)
else()
    include(FetchContent)
    FetchContent_Declare(xbe_sdk GIT_REPOSITORY https://github.com/TGP482/XboxXBE-Recomp.git GIT_TAG main)
    FetchContent_MakeAvailable(xbe_sdk)
    set(SDK ${xbe_sdk_SOURCE_DIR})
endif()
include(${SDK}/cmake/XbeRecomp.cmake)

file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/game/")
# the code the three games share (netplay, rollback, the launcher link): recomp/common/src/mk3d
set(MK3D_COMMON ${CMAKE_CURRENT_SOURCE_DIR}/../common/src)
file(GLOB MK3D_COMMON_SOURCES CONFIGURE_DEPENDS ${MK3D_COMMON}/mk3d/*.cpp)
list(APPEND XBE_HOST_SOURCES ${MK3D_COMMON_SOURCES})

# partitions as xbe_symbols wrote them into config/game.toml
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS config/game.toml)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/config/game.toml)
    file(STRINGS config/game.toml GAME_PARTITIONS REGEX "^partitions *=")
    string(REGEX MATCH "[0-9]+" GAME_PARTITIONS "${GAME_PARTITIONS}")
endif()
# no config yet: build.bat writes it with xbe_symbols, the change reconfigures
if(NOT GAME_PARTITIONS)
    set(GAME_PARTITIONS 8)
endif()

# target name = GAME_EXE in build.bat, xbe = GAME_XBE
add_xbe_image(MKA default.xbe game.toml ${GAME_PARTITIONS} game)

# the executable and the game code DLL share the name MKA and so the PDB name MKA.pdb (the clang driver picks it, a
# /pdb: option does not stick); the DLL links last and overwrites it. Keep each module's copy in pdb-exe/ and pdb-dll/, which
# the diagnostics in src/game/diag.cpp add to dbghelp's search path (it matches each module's PDB by GUID).
add_custom_command(TARGET MKA POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/pdb-exe
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_BINARY_DIR}/MKA.pdb ${CMAKE_CURRENT_BINARY_DIR}/pdb-exe/MKA.pdb)
add_custom_command(TARGET MKA_code POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/pdb-dll
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_BINARY_DIR}/MKA.pdb ${CMAKE_CURRENT_BINARY_DIR}/pdb-dll/MKA.pdb)
target_link_libraries(MKA PRIVATE dbghelp)
target_include_directories(MKA PRIVATE ${MK3D_COMMON})
