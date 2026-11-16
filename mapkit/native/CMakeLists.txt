# DeusEdixNav: the native packages the GAME loads (native/README.md), and the routing check that needs no engine.
#
#   DeusEdixNav.dll   UCNavNative.Route over a UCNavMesh's polygons (DeusEdixNav.cpp, NavRoute.h), 32-bit, linked
#                     against Deus Ex's Core.dll: the SDK's headers and the import library dev/prepare_sdk.py makes
#   nav_test.exe      NavRoute.h alone, for check_routes.py
#
# This is the same build as native/build_native.ps1 (which stays, and is what the Windows release uses) without
# PowerShell, for Visual Studio's own CMake generator:
#
#   cmake -S mapkit/native -B build/native -A Win32 -DUC_SDK_DIR=C:/path/to/build_tools/sdk
#   cmake --build build/native --config Release
#   -> mapkit/bin/DeusEdixNav.dll, mapkit/bin/nav_test.exe
#   then native/build_package.py compiles DeusEdixNav.u against the DLL.
#
# Windows only, and not for want of a script: DeusEdixNav.dll is an x86 PE image that the game (DeusEx.exe /
# Revision.exe) loads into its own process, and it is compiled against the SDK's headers and linked against the
# game's Core.dll. Neither exists on macOS or Linux, and an ARM64 or ELF library cannot be loaded there.
#
# Nothing on macOS or Linux needs it: the native port's requests for a route on a map's walkable area are answered
# by OpenDE's own path builder (mapkit/host/openbridge), not by this DLL, which only ever runs inside the game.
if(NOT WIN32)
    message(WARNING
        "DeusEdixNav is not built on this platform.\n"
        "  DeusEdixNav.dll is the package the GAME loads: 32-bit Windows code, compiled against the Deus Ex SDK\n"
        "  headers and linked against the game's Core.dll. The game is a Windows program and loads a PE32 DLL.\n"
        "  The native port does not need it: on macOS and Linux the walkable area's routes come from OpenDE's own\n"
        "  path builder, through mapkit/host/openbridge. Nothing was configured and nothing was built.")
    return()
endif()

cmake_minimum_required(VERSION 3.20)
project(DeusEdixNav LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR
        "DeusEdixNav is built with Visual Studio's 32-bit x86 compiler: its exports must match the names the\n"
        "game's Visual C++ 6 built Core.dll imports, and the game loads a PE32 DLL. (native/build_native.ps1 does\n"
        "the same build from PowerShell.)")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "DeusEdixNav.dll must be 32 bit (x86): the game is a 32-bit program and cannot load a 64-bit DLL.\n"
        "Configure with -A Win32, e.g.\n"
        "  cmake -S mapkit/native -B build/native -A Win32 -DUC_SDK_DIR=<build_tools/sdk>")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # /MT: the C runtime linked in

# ---- the SDK headers and import libraries (dev/prepare_sdk.py): Core alone, the one package this DLL imports from
if(NOT UC_SDK_DIR)
    foreach(_candidate "${CMAKE_CURRENT_SOURCE_DIR}/../../build_tools/sdk"
                       "${CMAKE_CURRENT_SOURCE_DIR}/../../editor_bridge/editor_lab")
        if(EXISTS "${_candidate}/Core.lib" AND EXISTS "${_candidate}/headers/Core/Inc/Core.h")
            set(UC_SDK_DIR "${_candidate}")
            break()
        endif()
    endforeach()
    unset(_candidate)
endif()
if(NOT UC_SDK_DIR)
    message(FATAL_ERROR
        "The SDK headers and import libraries were not found (build_tools/sdk). Make them with dev/prepare_sdk.py\n"
        "on Windows, or pass -DUC_SDK_DIR=<the folder with Core.lib and headers/Core/Inc>.")
endif()

# The built files go where build_package.py and the editor look for them (mapkit/bin), as with the PowerShell script
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/../bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_SOURCE_DIR}/../bin")

# C++17, the security checks (/sdl), Control Flow Guard, CET shadow stacks; ASLR and DEP are the linker's defaults
add_compile_options(/O2 /EHsc /sdl /guard:cf /D_UNICODE /DUNICODE /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS)
add_link_options(/DYNAMICBASE /NXCOMPAT /GUARD:CF /CETCOMPAT)

# ---- DeusEdixNav.dll
add_library(DeusEdixNav SHARED DeusEdixNav.cpp NavRoute.h)
# /Zc:wchar_t-: the game's DLLs were built by Visual C++ 6, whose wchar_t was an unsigned short; their exports are
# named with it.
target_compile_options(DeusEdixNav PRIVATE /Zc:wchar_t-)
target_include_directories(DeusEdixNav PRIVATE "${UC_SDK_DIR}/headers/Core/Inc")
target_link_libraries(DeusEdixNav PRIVATE "${UC_SDK_DIR}/Core.lib" kernel32)

# ---- nav_test.exe: NavRoute.h alone (no engine header), for check_routes.py and edge_cases.py
add_executable(nav_test nav_test.cpp NavRoute.h)
target_link_libraries(nav_test PRIVATE kernel32)
