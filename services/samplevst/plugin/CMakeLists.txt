# SampleVST: the generic sampler every instrument made from samples is a copy of (see ../samplevst/vst3.py).
#
#   cmake -B build -Dvst3sdk_SOURCE_DIR=/path/to/vst3sdk -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --config Release
#
# The bundle is build/VST3/Release/SampleVST.vst3. CI (.github/workflows/samplevst.yml) builds it for Windows,
# macOS and Linux and merges the three into one bundle.
cmake_minimum_required(VERSION 3.25)
set(CMAKE_OSX_DEPLOYMENT_TARGET 10.15 CACHE STRING "")   # std::filesystem; MuseScore 4 needs 10.15 too

if(NOT vst3sdk_SOURCE_DIR)
    message(FATAL_ERROR "Set vst3sdk_SOURCE_DIR to the VST 3 SDK (https://github.com/steinbergmedia/vst3sdk, MIT)")
endif()

project(SampleVST VERSION 1.0.0 DESCRIPTION "Sampler made from the user's own samples")

set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE)
set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "" FORCE)      # CI runs it on a real instrument
set(SMTG_CREATE_MODULE_INFO OFF CACHE BOOL "" FORCE)     # it would freeze the template's ID: every copy has its own
set(SMTG_USE_STATIC_CRT ON CACHE BOOL "" FORCE)          # no Visual C++ runtime to install on Windows
option(SAMPLEVST_HOSTING "Build the SDK's validator too" OFF)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES ${SAMPLEVST_HOSTING} CACHE BOOL "" FORCE)

add_subdirectory(${vst3sdk_SOURCE_DIR} ${PROJECT_BINARY_DIR}/vst3sdk)
smtg_enable_vst3_sdk()

smtg_add_vst3plugin(SampleVST
    source/controller.cpp
    source/controller.h
    source/entry.cpp
    source/instrument.cpp
    source/instrument.h
    source/params.h
    source/processor.cpp
    source/processor.h
    source/sampler.cpp
    source/sampler.h
    source/version.h
)
target_compile_features(SampleVST PUBLIC cxx_std_17)
target_link_libraries(SampleVST PRIVATE sdk)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(SampleVST PRIVATE dl)
endif()

if(SMTG_MAC)
    smtg_target_set_bundle(SampleVST
        BUNDLE_IDENTIFIER work.appgate.samplevst
        COMPANY_NAME "appgate.work"
    )
endif()
