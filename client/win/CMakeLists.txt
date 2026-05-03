cmake_minimum_required(VERSION 3.25)
project(stickytodo-win VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------- Compiler sanity ----------
# This target is a pure Win32 + C++/WinRT + Direct2D desktop app and links
# against vcpkg's x64-windows triplet, whose .lib artifacts are produced by
# MSVC (cl.exe). Building the same sources with mingw's g++ against those
# MSVC .lib files is an ABI mismatch (different name mangling, different
# CRT expectations); even before the link step, mingw's <windows.h> exposes
# a second bug — it selects Unicode vs ANSI strictly off UNICODE / _UNICODE,
# which aren't set by default on mingw, so resource identifier macros like
# IDC_ARROW / IDI_APPLICATION expand to narrow-string literals that the
# W-suffixed APIs (LoadCursorW, LoadIconW) reject at compile time.
#
# We therefore hard-stop here if cl isn't the compiler. The GitHub Actions
# windows-2022 runner has both MSVC 2022 and a mingw at C:\mingw64 on PATH;
# CI is expected to activate the MSVC environment (ilammy/msvc-dev-cmd)
# before invoking CMake so cl becomes the first C++ compiler in PATH.
if(WIN32 AND NOT MSVC)
    message(FATAL_ERROR
        "stickytodo-win must be built with MSVC (cl.exe). Detected "
        "'${CMAKE_CXX_COMPILER_ID}' at '${CMAKE_CXX_COMPILER}'. "
        "On GitHub Actions, ensure the MSVC dev environment is activated "
        "(ilammy/msvc-dev-cmd@v1) before the CMake configure step. "
        "Locally, run CMake from an 'x64 Native Tools Command Prompt for VS 2022' "
        "or pass -DCMAKE_CXX_COMPILER=cl -DCMAKE_C_COMPILER=cl.")
endif()

# Define UNICODE + _UNICODE for every TU. Even on MSVC (where x64 projects
# default these on via the Visual Studio IDE project templates) we set them
# explicitly because CMake's default command line does NOT, and we want
# the CMake build to match the VS-IDE behavior one-for-one. NOMINMAX stops
# <windows.h>'s min/max macros from clobbering std::min/std::max in headers
# we include later; WIN32_LEAN_AND_MEAN trims seldom-used subsystems from
# <windows.h> for faster compiles and fewer macro collisions.
#
# _WIN32_WINNT=0x0A00 / WINVER=0x0A00 pin the API surface to Windows 10 RTM
# or later. We specifically need the per-monitor v2 DPI API surface
# (`SetProcessDpiAwarenessContext`, `DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2`
# in main.cpp) which the SDK only exposes when _WIN32_WINNT ≥ 0x0605 AND
# NTDDI_VERSION ≥ NTDDI_WIN10_RS2 (0x0A000003, Creators Update). Setting
# _WIN32_WINNT=0x0A00 also unblocks the newer WinHTTP WebSocket APIs
# (WinHttpWebSocketCompleteUpgrade etc. in WebSocketClient.cpp). Without
# these defines, MSVC would expand those identifiers to "undeclared" and
# emit C2065 — exactly the kind of error we'd lose to log truncation in CI.
#
# _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING: CredentialStore.cpp
# transitively pulls in <codecvt> (legacy, pre-C++17 wide string helper).
# MSVC stdlib emits a hard deprecation message unless this macro is set;
# under /W3 it's a warning not an error, but keeping the TU quiet is both
# a stability thing (future MSVC may escalate) and a "keep the CI log
# signal-to-noise high" thing — we've seen the real compile error hide
# behind pages of deprecation chatter before.
add_compile_definitions(
    UNICODE _UNICODE
    NOMINMAX WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
    NTDDI_VERSION=0x0A000006   # NTDDI_WIN10_19H1 — covers 20H1/21H2/Win11
    _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
    _CRT_SECURE_NO_WARNINGS    # suppress strncpy / sprintf deprecation
)

# Force verbose ninja output in CI-style builds: when anything fails, we
# want the full cl.exe command line and stderr to land in the log BEFORE
# ninja kills sibling compile jobs. Without this the "ninja: build stopped:
# subcommand failed" line can show up before the offending TU's stderr has
# flushed through the GitHub Actions log streamer (we observed exactly
# this symptom on commit where [1/20] was missing from the log entirely).
# CMAKE_VERBOSE_MAKEFILE is cheap — it only adds a few KB to the log — and
# pays for itself the first time a real error shows up.
set(CMAKE_VERBOSE_MAKEFILE ON)

# ---------- vcpkg dependencies ----------
find_package(nlohmann_json CONFIG REQUIRED)

# ---------- Sources ----------
set(SOURCES
    src/main.cpp
    src/App.cpp
    src/core/AppState.cpp
    src/core/HttpClient.cpp
    src/core/WebSocketClient.cpp
    src/core/CredentialStore.cpp
    src/core/FrameStore.cpp
    src/core/Timer.cpp
    src/ui/D2DRenderer.cpp
    src/ui/Theme.cpp
    src/ui/Controls.cpp
    src/ui/Preferences.cpp
    src/ui/FilterEditor.cpp
    src/ui/StickyWindow.cpp
    src/ui/SettingsWindow.cpp
    src/ui/TrayIcon.cpp
    src/codec/JsonHelper.cpp
    src/codec/StickyCodec.cpp
)

set(HEADERS
    src/App.h
    src/core/AppState.h
    src/core/HttpClient.h
    src/core/WebSocketClient.h
    src/core/CredentialStore.h
    src/core/FrameStore.h
    src/core/Timer.h
    src/models/Todo.h
    src/models/StickyNote.h
    src/models/Filter.h
    src/models/AuditLog.h
    src/ui/D2DRenderer.h
    src/ui/Theme.h
    src/ui/Controls.h
    src/ui/Preferences.h
    src/ui/FilterEditor.h
    src/ui/StickyWindow.h
    src/ui/SettingsWindow.h
    src/ui/TrayIcon.h
    src/codec/JsonHelper.h
    src/codec/StickyCodec.h
)

# ---------- Executable ----------
add_executable(stickytodo WIN32
    ${SOURCES}
    ${HEADERS}
    src/res/app.rc
)

target_include_directories(stickytodo PRIVATE src)

# ---------- Windows SDK libraries ----------
target_link_libraries(stickytodo PRIVATE
    nlohmann_json::nlohmann_json
    d2d1
    dwrite
    dxgi
    windowscodecs
    credui
    advapi32
    shell32
    ws2_32
    ole32
    uuid
    windowsapp
)

# ---------- Manifest for DPI awareness ----------
set_target_properties(stickytodo PROPERTIES
    VS_DPI_AWARE "PerMonitor"
    WIN32_EXECUTABLE TRUE
)

# ---------- Tests (optional) ----------
option(BUILD_TESTS "Build unit tests" OFF)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
