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
    if ("${${VAR}}" STREQUAL "" AND NOT "$ENV{${VAR}}" STREQUAL "")
        # In the cache: the MSI packaging configures the build again, without these variables
        set(${VAR} "$ENV{${VAR}}" CACHE STRING "" FORCE)
    endif()
endforeach()

muse_create_module(drive NO_UNITY)

target_sources(drive PRIVATE
    drivemodule.cpp
    drivemodule.h
    idriveservice.h

    internal/driveservice.cpp
    internal/driveservice.h
    internal/driveservice_extras.cpp
    internal/crashhandler.cpp
    internal/crashhandler.h
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

# Which build this is, and where its updates are published: from CI (GitHub Actions) when there
if (NOT DEFINED CACHE{VS_BUILD_ID})
    string(SUBSTRING "$ENV{GITHUB_SHA}" 0 7 VS_BUILD_ID_NOW)
    if (VS_BUILD_ID_NOW STREQUAL "")
        set(VS_BUILD_ID_NOW "local")
    endif()
    set(VS_BUILD_ID "${VS_BUILD_ID_NOW}" CACHE STRING "Which build this is (commit)")
endif()
# Kept in the cache: the MSI packaging configures the build again, and a new value would rebuild the module there
if (NOT DEFINED CACHE{VS_BUILD_TIME})
    string(TIMESTAMP VS_BUILD_TIME_NOW "%s" UTC)
    set(VS_BUILD_TIME "${VS_BUILD_TIME_NOW}" CACHE STRING "When this build was made (seconds since 1970)")
endif()
set(VS_UPDATE_REPO "$ENV{GITHUB_REPOSITORY}" CACHE STRING "GitHub repository whose releases are this app's updates")

target_compile_definitions(drive PRIVATE
    VS_BUILD_ID="${VS_BUILD_ID}"
    VS_BUILD_TIME=${VS_BUILD_TIME}
    VS_UPDATE_REPO="${VS_UPDATE_REPO}"
    VS_DRIVE_CLIENT_ID="${VS_DRIVE_CLIENT_ID}"
    VS_DRIVE_CLIENT_SECRET="${VS_DRIVE_CLIENT_SECRET}"
    VS_DRIVE_FOLDER_ID="${VS_DRIVE_FOLDER_ID}"
)

target_link_libraries(drive PRIVATE Qt::Network)
