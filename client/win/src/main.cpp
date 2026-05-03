#include "App.h"

#include <windows.h>
// CoInitializeEx / CoUninitialize live in <objbase.h>. When WIN32_LEAN_AND_MEAN
// is defined (we do, project-wide, in CMakeLists.txt) <windows.h> intentionally
// skips including <objbase.h>, so the COM APIs used below (CoInitializeEx at
// startup to enable WinRT / apartment-threaded COM, CoUninitialize on every
// exit path) become unresolved identifiers — observed as
// "error C3861: 'CoUninitialize': identifier not found" on the CI build.
// Pull it in explicitly here to restore the symbols without giving up the
// compile-time savings WIN32_LEAN_AND_MEAN gives every other TU.
#include <objbase.h>
#include <shellapi.h>

// Single-instance mutex name
static constexpr wchar_t kMutexName[] = L"Global\\StickyTodo_SingleInstance";

int WINAPI wWinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE /*hPrevInstance*/,
    _In_ LPWSTR /*lpCmdLine*/,
    _In_ int nCmdShow)
{
    // ---------- Single instance check ----------
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Another instance is running. Optionally signal it to come to foreground.
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    // ---------- COM initialization (for WinRT) ----------
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        return 1;
    }

    // ---------- DPI awareness ----------
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // ---------- Application ----------
    stickytodo::App app(hInstance);
    if (!app.Initialize(nCmdShow)) {
        CoUninitialize();
        if (hMutex) {
            ReleaseMutex(hMutex);
            CloseHandle(hMutex);
        }
        return 1;
    }

    // ---------- Message loop ----------
    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // ---------- Cleanup ----------
    app.Shutdown();
    CoUninitialize();

    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }

    return static_cast<int>(msg.wParam);
}
