cmake_minimum_required(VERSION 3.25)
project(stickytodo-win VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
