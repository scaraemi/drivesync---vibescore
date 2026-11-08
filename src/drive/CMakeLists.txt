# SPDX-License-Identifier: GPL-3.0-only
#
# VibeScore: a shared Google Drive folder of scores (the Drive tab of the Scores page).
# The sync core (internal/core) is the drivesync library, also used by MuseScore Studio on Windows.
#
# The Google OAuth client ("Desktop app" type) is set at build time:
#   -DVS_DRIVE_CLIENT_ID=... -DVS_DRIVE_CLIENT_SECRET=... [-DVS_DRIVE_FOLDER_ID=...]
# Without a client id the module builds, but the Drive tab is not shown.

set(VS_DRIVE_CLIENT_ID "" CACHE STRING "Google OAuth client id (Desktop app) for the Drive tab")
set(VS_DRIVE_CLIENT_SECRET "" CACHE STRING "Google OAuth client secret of that client")
set(VS_DRIVE_FOLDER_ID "" CACHE STRING "Drive folder used until the user chooses one")

# CI passes them as environment variables too (repository secrets)
foreach(VAR VS_DRIVE_CLIENT_ID VS_DRIVE_CLIENT_SECRET VS_DRIVE_FOLDER_ID)
    if ("${${VAR}}" STREQUAL "" AND DEFINED ENV{${VAR}})
        set(${VAR} "$ENV{${VAR}}")
    endif()
endforeach()

muse_create_module(drive NO_UNITY)

target_sources(drive PRIVATE
    drivemodule.cpp
    drivemodule.h
    idriveservice.h

    internal/driveservice.cpp
    internal/driveservice.h
    internal/heartbeatfile.cpp
    internal/heartbeatfile.h

    internal/core/driveclient.cpp
    internal/core/driveclient.h
    internal/core/drivetypes.cpp
    internal/core/drivetypes.h
    internal/core/googleauth.cpp
    internal/core/googleauth.h
    internal/core/itokenprovider.h
    internal/core/syncengine.cpp
    internal/core/syncengine.h
    internal/core/syncmanifest.cpp
    internal/core/syncmanifest.h
    internal/core/syncplanner.cpp
    internal/core/syncplanner.h
)

target_compile_definitions(drive PRIVATE
    VS_DRIVE_CLIENT_ID="${VS_DRIVE_CLIENT_ID}"
    VS_DRIVE_CLIENT_SECRET="${VS_DRIVE_CLIENT_SECRET}"
    VS_DRIVE_FOLDER_ID="${VS_DRIVE_FOLDER_ID}"
)

target_link_libraries(drive PRIVATE Qt::Network)
