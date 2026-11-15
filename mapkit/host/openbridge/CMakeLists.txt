# UCOpenBridge: the DeusEdix engine bridge backed by OpenDE (mapkit/host/bridge.h).
#
# Builds UCOpenBridge.dylib (macOS) / UCOpenBridge.so (Linux) from UCOpenBridge.cpp, linked against the OpenDE
# static libraries of a built engine tree (OPENDE_ROOT, its build directory in OPENDE_BUILD).
#
#   cmake -S mapkit/host/openbridge -B <scratch> -DCMAKE_BUILD_TYPE=Release -DOPENDE_ROOT=/path/to/OpenDE
#   cmake --build <scratch>
#
# The host loads the library with --bridge <path>.
cmake_minimum_required(VERSION 3.20)
project(UCOpenBridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The OpenDE tree this bridge runs on (the engine's headers and its built libraries).
set(OPENDE_ROOT "/Volumes/OWC/Games/OpenDE" CACHE PATH "The OpenDE source tree")
set(OPENDE_BUILD "${OPENDE_ROOT}/build" CACHE PATH "The OpenDE build directory (the libraries)")
set(UCOPENBRIDGE_GAME_DATA "${OPENDE_ROOT}/game-data" CACHE PATH "The game data used when the host names none")

set(UC_HOST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")

if(NOT EXISTS "${OPENDE_BUILD}/src/libdx-engine.a")
    message(FATAL_ERROR "no ${OPENDE_BUILD}/src/libdx-engine.a: build OpenDE first")
endif()

add_library(UCOpenBridge SHARED UCOpenBridge.cpp)

target_include_directories(UCOpenBridge PRIVATE
    "${UC_HOST_DIR}"                                # bridge.h
    "${UC_HOST_DIR}/third_party"                    # nlohmann/json.hpp
    "${OPENDE_ROOT}/src"                            # the engine's headers
    "${OPENDE_ROOT}/vendor/SDL2-2.32.10/include"    # SDL2's headers (core/config.h includes SDL_filesystem.h)
)

target_compile_definitions(UCOpenBridge PRIVATE
    UCOPENBRIDGE_GAME_DATA="${UCOPENBRIDGE_GAME_DATA}"
)

# The engine's libraries: dx-engine has packages, objects, properties, the VM, the builders and the map writer;
# dx-compiler the UnrealScript compiler and the asset import/export. No renderer: the bridge draws nothing.
target_link_libraries(UCOpenBridge PRIVATE
    "${OPENDE_BUILD}/src/libdx-compiler.a"
    "${OPENDE_BUILD}/src/libdx-engine.a"
    "${OPENDE_BUILD}/vendor/SDL2-2.32.10/libSDL2.a"
    "${OPENDE_BUILD}/libminiaudio.a"
)

if(APPLE)
    target_link_libraries(UCOpenBridge PRIVATE
        "-framework CoreVideo" "-framework Cocoa" "-framework IOKit" "-framework ForceFeedback"
        "-framework Carbon" "-framework CoreAudio" "-framework AudioToolbox" "-framework AVFoundation"
        "-framework Foundation" "-framework AppKit" "-framework CoreFoundation" "-framework CoreGraphics"
        "-framework CoreServices" "-framework GameController" "-framework QuartzCore"
        "-framework Metal" "-framework CoreHaptics"
    )
    set_target_properties(UCOpenBridge PROPERTIES SUFFIX ".dylib")
else()
    target_link_libraries(UCOpenBridge PRIVATE dl m pthread)
    set_target_properties(UCOpenBridge PROPERTIES SUFFIX ".so")
endif()

set_target_properties(UCOpenBridge PROPERTIES
    PREFIX ""
    OUTPUT_NAME "UCOpenBridge"
    CXX_VISIBILITY_PRESET default
)
