#include "ui/TrayIcon.h"
#include "App.h"
#include "core/AppState.h"

#include <strsafe.h>
#include <objbase.h>
#include <cwctype>

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
    nid_.hIcon = LoadIconW(hInstance_, MAKEINTRESOURCEW(IDI_APPICON));
    if (!nid_.hIcon) {
        // Fallback to system icon if our icon fails to load
        nid_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
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
    if (nid_.hIcon) {
        DestroyIcon(nid_.hIcon);
        nid_.hIcon = nullptr;
    }
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
            // we then call FetchStickies() which updates the local cache AND
            // fires onStickiesChanged_ → App::SyncStickyWindows(), which in
            // turn opens the new window. This is deliberately a SINGLE path
            // — we do NOT also call OpenStickyWindow(stickyId) here. Calling
            // both would be harmless (App::OpenStickyWindow de-dupes via its
            // map) but would set up a confusing "new-sticky window open" UX
            // with two entry points that must stay in sync forever. Keeping
            // SyncStickyWindows as the single owner of window open/close
            // means any future creation path (tray, keyboard shortcut,
            // WS-driven sticky.upserted from another device, …) benefits
            // from one canonical behaviour.
            if (app->GetState() && app->GetState()->IsAuthenticated()) {
                std::string defaultBgColor = R"({"red":1.0,"green":0.92,"blue":0.54,"alpha":1.0})";
                auto result = app->GetState()->GetHttp()->UpsertSticky(
                    stickyId, "New Note", defaultBgColor, "{}");
                if (result.has_value()) {
                    app->GetState()->FetchStickies();
                }
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

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace stickytodo::ui
