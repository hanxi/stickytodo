#include "ui/TrayIcon.h"
#include "App.h"
#include "core/AppState.h"
#include "core/UIThreadMarshal.h" // For WM_STICKYTODO_RUN_ON_UI.

#include <strsafe.h>
#include <objbase.h>
#include <cwctype>
#include <functional>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

// Icon resource ID (defined in app.rc)
#define IDI_APPICON 101

namespace stickytodo::ui {

bool TrayIcon::classRegistered_ = false;

TrayIcon::TrayIcon(HINSTANCE hInstance)
    : hInstance_(hInstance)
{
}

TrayIcon::~TrayIcon() {
    Destroy();
}

bool TrayIcon::Create() {
    if (created_) return true;

    // Register window class for message-only window
    if (!classRegistered_) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.lpfnWndProc = TrayWndProc;
        wc.hInstance = hInstance_;
        wc.lpszClassName = kTrayWndClass;
        if (!RegisterClassExW(&wc)) return false;
        classRegistered_ = true;
    }

    // Create message-only window
    hwnd_ = CreateWindowExW(0, kTrayWndClass, L"", 0,
                             0, 0, 0, 0,
                             HWND_MESSAGE, nullptr, hInstance_, this);
    if (!hwnd_) return false;

    // Add tray icon
    ZeroMemory(&nid_, sizeof(nid_));
    nid_.cbSize = sizeof(NOTIFYICONDATAW);
    nid_.hWnd = hwnd_;
    nid_.uID = 1;
    nid_.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    nid_.uCallbackMessage = WM_TRAYICON;

    // DPI-aware tray icon: the system-tray area wants SM_CXSMICON /
    // SM_CYSMICON (16 px at 96-DPI, 20 px at 125 %, 24 px at 150 %,
    // 32 px at 200 %). LoadIconW always returns SM_CXICON (32 px
    // baseline) which Windows then down-scales for the tray — that
    // down-scale is bilinear and looks blurry on HiDPI displays.
    //
    // Fix: use LoadImageW with the exact small-icon metrics for the
    // system DPI (we pick the system DPI here because the tray
    // itself lives on the primary monitor and isn't easily per-hwnd
    // DPI-aware; if the user moves the taskbar to a different DPI
    // monitor they'll see the baseline-scaled variant, which is still
    // acceptable fallback). `LR_SHARED` makes the system manage the
    // HICON's lifetime — we MUST NOT call DestroyIcon on it, so the
    // Destroy() path below is adjusted accordingly.
    UINT sysDpi = GetDpiForSystem();
    if (sysDpi == 0) sysDpi = 96;
    int cxSmall = GetSystemMetricsForDpi(SM_CXSMICON, sysDpi);
    int cySmall = GetSystemMetricsForDpi(SM_CYSMICON, sysDpi);
    nid_.hIcon = static_cast<HICON>(LoadImageW(
        hInstance_, MAKEINTRESOURCEW(IDI_APPICON),
        IMAGE_ICON, cxSmall, cySmall,
        LR_DEFAULTCOLOR | LR_SHARED));
    if (!nid_.hIcon) {
        // Fallback: load the generic application icon via LoadIconW.
        // We deliberately DON'T use LoadImageW(nullptr, OIC_SAMPLE, ...)
        // here because the OIC_* constants are only exposed when the
        // TU defines OEMRESOURCE before <windows.h> — and flipping
        // that flag globally would pull in a large set of legacy
        // GDI/OEM symbols we don't want. LoadIconW always returns a
        // system-shared HICON (no DestroyIcon needed), so it matches
        // the LR_SHARED semantics of the primary path above — the
        // Destroy() path below handles both uniformly.
        nid_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    // Icon lifetime: both branches above yield a system-shared HICON,
    // so Destroy() must NOT call DestroyIcon — it just nulls the
    // handle. If both loads fail we end up with a null icon and
    // Shell_NotifyIconW(NIM_ADD) below will reject the payload.
    StringCchCopyW(nid_.szTip, ARRAYSIZE(nid_.szTip), L"StickyTodo");
    nid_.uVersion = NOTIFYICON_VERSION_4;

    if (!Shell_NotifyIconW(NIM_ADD, &nid_)) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }

    Shell_NotifyIconW(NIM_SETVERSION, &nid_);
    created_ = true;
    return true;
}

void TrayIcon::Destroy() {
    if (!created_) return;

    Shell_NotifyIconW(NIM_DELETE, &nid_);
    // Icon lifetime: Create() uses LoadImageW with LR_SHARED, so
    // the system owns the HICON — calling DestroyIcon on it is
    // undefined behaviour (MSDN: "Do not use this function to
    // destroy a shared icon."). Just null the handle.
    nid_.hIcon = nullptr;
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    created_ = false;
}

void TrayIcon::ShowBalloon(const std::wstring& title, const std::wstring& message) {
    if (!created_) return;

    nid_.uFlags = NIF_INFO;
    nid_.dwInfoFlags = NIIF_INFO;
    StringCchCopyW(nid_.szInfoTitle, ARRAYSIZE(nid_.szInfoTitle), title.c_str());
    StringCchCopyW(nid_.szInfo, ARRAYSIZE(nid_.szInfo), message.c_str());
    Shell_NotifyIconW(NIM_MODIFY, &nid_);

    // Reset flags
    nid_.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
}

void TrayIcon::SetTooltip(const std::wstring& tip) {
    if (!created_) return;

    StringCchCopyW(nid_.szTip, ARRAYSIZE(nid_.szTip), tip.c_str());
    nid_.uFlags = NIF_TIP | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
    nid_.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
}

void TrayIcon::ShowContextMenu() {
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    auto* app = stickytodo::GetApp();
    bool authenticated = app && app->GetState() && app->GetState()->IsAuthenticated();

    if (authenticated) {
        AppendMenuW(hMenu, MF_STRING, ID_TRAY_NEW_STICKY, L"New Sticky Note");
        AppendMenuW(hMenu, MF_STRING, ID_TRAY_SETTINGS, L"Settings");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING, ID_TRAY_LOGOUT, L"Logout");
        AppendMenuW(hMenu, MF_STRING, ID_TRAY_QUIT, L"Quit");
    } else {
        AppendMenuW(hMenu, MF_STRING, ID_TRAY_SETTINGS, L"Settings");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING, ID_TRAY_QUIT, L"Quit");
    }

    // Required for TrackPopupMenu to work correctly with a tray icon
    SetForegroundWindow(hwnd_);

    POINT pt;
    GetCursorPos(&pt);
    UINT cmd = TrackPopupMenu(hMenu,
                               TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                               pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(hMenu);

    // Dismiss the menu
    PostMessageW(hwnd_, WM_NULL, 0, 0);

    if (cmd != 0) {
        HandleMenuCommand(cmd);
    }
}

void TrayIcon::HandleMenuCommand(UINT cmdId) {
    auto* app = stickytodo::GetApp();
    if (!app) return;

    switch (cmdId) {
        case ID_TRAY_NEW_STICKY: {
            // Generate a UUID for the new sticky note
            GUID guid;
            CoCreateGuid(&guid);
            wchar_t guidStr[40];
            StringFromGUID2(guid, guidStr, 40);
            // Convert GUID to lowercase hex without braces: {XXXXXXXX-...} → xxxxxxxx...
            std::wstring wGuid(guidStr + 1, guidStr + 37); // strip braces
            std::string stickyId;
            for (wchar_t ch : wGuid) {
                if (ch != L'-') stickyId += static_cast<char>(towlower(ch));
            }

            // Create on server with default title and default sticky yellow
            // color. Successful creation only mutates the server-side store;
            // we then call FetchStickiesAsync which updates the local cache
            // AND fires onStickiesChanged_ → App::SyncStickyWindows(), which
            // in turn opens the new window. This is deliberately a SINGLE
            // path — we do NOT also call OpenStickyWindow(stickyId) here.
            // Calling both would be harmless (App::OpenStickyWindow de-dupes
            // via its map) but would set up a confusing "new-sticky window
            // open" UX with two entry points that must stay in sync forever.
            // Keeping SyncStickyWindows as the single owner of window
            // open/close means any future creation path (tray, keyboard
            // shortcut, WS-driven sticky.upserted from another device, …)
            // benefits from one canonical behaviour.
            //
            // Async, not sync: UpsertSticky + ListStickies together can take
            // up to 2 × (resolve+connect+send+receive) = ~80s on a bad
            // network, and this runs on the UI thread after the tray menu's
            // TrackPopupMenu returns. Blocking the UI thread for that long
            // is exactly the "window not responding" regression we're
            // eliminating in this refactor. Async callbacks are marshalled
            // back to the UI thread via PostToUIThread (tray HWND), so by
            // the time we touch AppState state we're on the UI thread again
            // with the same thread-safety story as the previous sync code.
            //
            // Callback captures ONLY stickyId by value (not `this` / `app`):
            // after the menu closes, this TrayIcon instance may still exist
            // but the safe way to re-reach AppState in a callback is via
            // stickytodo::GetApp() which handles the Shutdown race cleanly
            // (returns nullptr if App has already torn down).
            if (app->GetState() && app->GetState()->IsAuthenticated()) {
                std::string defaultBgColor = R"({"red":1.0,"green":0.92,"blue":0.54,"alpha":1.0})";
                app->GetState()->UpsertStickyAsync(
                    stickyId, "New Note", defaultBgColor, "{}",
                    [stickyId](std::optional<stickytodo::models::StickyNote> result) {
                        if (!result.has_value()) {
                            // Silent failure: the tray menu is already
                            // closed, there's no status control to surface
                            // an error to. A WS reconnect-driven refresh
                            // will reconcile eventually if the server did
                            // persist. Consistent with the pre-refactor
                            // behaviour (which also silently no-op'd on
                            // UpsertSticky returning nullopt).
                            return;
                        }
                        auto* app2 = stickytodo::GetApp();
                        if (!app2 || !app2->GetState()) return;
                        // Refresh the authoritative sticky list so
                        // SyncStickyWindows opens the new window. Nested
                        // callback intentionally empty — onStickiesChanged_
                        // inside FetchStickiesAsync drives the UI update,
                        // we don't need the bool success signal here
                        // (same silent-on-failure policy as above).
                        app2->GetState()->FetchStickiesAsync(
                            [](bool /*ok*/) {});
                    });
            }
            break;
        }

        case ID_TRAY_SETTINGS:
            app->ShowSettings();
            break;

        case ID_TRAY_LOGOUT:
            if (app->GetState()) {
                app->GetState()->Logout();
            }
            break;

        case ID_TRAY_QUIT:
            app->Quit();
            break;
    }
}

LRESULT CALLBACK TrayIcon::TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_CREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return 0;
    }

    auto* self = reinterpret_cast<TrayIcon*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    if (msg == WM_TRAYICON) {
        switch (LOWORD(lParam)) {
            case WM_RBUTTONUP:
            case WM_CONTEXTMENU:
                self->ShowContextMenu();
                return 0;

            case WM_LBUTTONDBLCLK:
                // Double-click → open settings
                if (auto* app = stickytodo::GetApp()) {
                    app->ShowSettings();
                }
                return 0;
        }
    }

    // WS worker thread marshalling. AppState::PostWsEventToUIThread /
    // PostWsSignalToUIThread post these two custom messages to this window
    // (whose WndProc runs on the UI thread). Dispatch them back into
    // AppState on the UI thread, then — for WS_EVENT — free the heap-
    // allocated WsEvent the producer allocated. This is the ONLY place the
    // heap pointer is released; any return path that skips the delete will
    // leak one event per message (see the failure-to-post delete in
    // AppState::PostWsEventToUIThread for the complementary cleanup path).
    if (msg == core::WM_STICKYTODO_WS_EVENT) {
        auto* heap = reinterpret_cast<core::WsEvent*>(wParam);
        if (heap) {
            if (auto* app = stickytodo::GetApp()) {
                if (auto* state = app->GetState()) {
                    state->HandleWsEventOnUIThread(*heap);
                }
            }
            delete heap;
        }
        return 0;
    }

    if (msg == core::WM_STICKYTODO_WS_SIGNAL) {
        auto signal = static_cast<core::WsSignal>(wParam);
        if (auto* app = stickytodo::GetApp()) {
            if (auto* state = app->GetState()) {
                state->HandleWsSignalOnUIThread(signal);
            }
        }
        return 0;
    }

    // Generic "run arbitrary callable on the UI thread" marshalling. The
    // wParam is a heap-allocated std::function<void()>* produced by
    // core::PostToUIThread (from any thread — typically an HTTP async
    // worker). We invoke it on the UI thread then delete the allocation.
    //
    // Why this lives in TrayIcon's WndProc rather than in a dedicated
    // window: the tray's message-only HWND already exists for WS event
    // marshalling (SetUIThreadTarget in App::Initialize), lives as long
    // as the app does, and is created on + pumped by the UI thread. Any
    // *new* hidden window we added for this would duplicate that setup
    // with zero semantic difference, and introduce a second HWND that
    // App::Shutdown would have to tear down in a careful order. Reusing
    // this single tray HWND keeps the marshalling story — WS events,
    // WS signals, and arbitrary callables — consistent and
    // single-ownership.
    //
    // Exception safety: if the invoked function throws, we still need
    // to delete the heap allocation to avoid a leak. The try/catch
    // swallows the exception because propagating out of a WndProc
    // crosses Win32 message-dispatch boundaries (the OS, not our code,
    // is between the throw site and any C++ catch block in WinMain),
    // which is undefined behaviour. Worker-thread-originated callbacks
    // should not normally throw anyway.
    if (msg == core::WM_STICKYTODO_RUN_ON_UI) {
        auto* fn = reinterpret_cast<std::function<void()>*>(wParam);
        if (fn) {
            try {
                (*fn)();
            } catch (...) {
                // Intentionally swallowed — see rationale above.
            }
            delete fn;
        }
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace stickytodo::ui
