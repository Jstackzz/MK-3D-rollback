# mk3d_version_info(<target> <file name> <description>): gives a Windows executable its version and publisher
# (a VERSIONINFO resource: version, product MK3D Online, publisher, description, copyright). Windows shows them in the
# file's Properties, and antivirus programs read them: an executable with none is more often taken for malware by
# their machine learning (Defender's Trojan:Win32/Bearfoos.A!ml took players' 0.1.21 launchers).
#
# The version is MK3D Online's release, set once in launcher/CMakeLists.txt (project(mk3d_launcher VERSION ...)),
# which every release bumps.
set(MK3D_VERSION_DIR ${CMAKE_CURRENT_LIST_DIR})

function(mk3d_version_info target file_name description)
    get_filename_component(launcher_cmake ${MK3D_VERSION_DIR}/../../../launcher/CMakeLists.txt ABSOLUTE)
    file(STRINGS ${launcher_cmake} project_line REGEX "^project\\(mk3d_launcher VERSION [0-9]+\\.[0-9]+\\.[0-9]+")
    if(NOT project_line MATCHES "VERSION ([0-9]+)\\.([0-9]+)\\.([0-9]+)")
        message(FATAL_ERROR "no release version in ${launcher_cmake}")
    endif()
    set(MK3D_VERSION_MAJOR ${CMAKE_MATCH_1})
    set(MK3D_VERSION_MINOR ${CMAKE_MATCH_2})
    set(MK3D_VERSION_PATCH ${CMAKE_MATCH_3})
    set(MK3D_VERSION ${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3})
    set(MK3D_FILE_NAME ${file_name})
    set(MK3D_FILE_DESCRIPTION ${description})
    set(rc ${CMAKE_CURRENT_BINARY_DIR}/${file_name}-version.rc)
    configure_file(${MK3D_VERSION_DIR}/version.rc.in ${rc} @ONLY)
    # a new version configures again (the launcher's own CMakeLists.txt does that already)
    if(NOT launcher_cmake STREQUAL "${CMAKE_SOURCE_DIR}/CMakeLists.txt")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${launcher_cmake})
    endif()
    target_sources(${target} PRIVATE ${rc})
endfunction()
