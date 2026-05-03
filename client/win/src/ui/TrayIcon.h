#pragma once

#include <windows.h>
#include <shellapi.h>
#include <string>

namespace stickytodo::ui {

/// System tray icon with right-click context menu.
/// Analogous to macOS MenuBarContent (client/mac/stickytodo/Views/MenuBarContent.swift).
///
/// Menu items when authenticated:
///   - "New Sticky Note"  (ID_TRAY_NEW_STICKY)
///   - "Settings"         (ID_TRAY_SETTINGS)
///   - separator
///   - "Logout"           (ID_TRAY_LOGOUT)
///   - "Quit"             (ID_TRAY_QUIT)
///
/// Menu items when not authenticated:
///   - "Settings"         (ID_TRAY_SETTINGS)
///   - separator
///   - "Quit"             (ID_TRAY_QUIT)
class TrayIcon {
public:
    // Command IDs for tray menu items
    static constexpr UINT ID_TRAY_NEW_STICKY = 40001;
    static constexpr UINT ID_TRAY_SETTINGS   = 40002;
    static constexpr UINT ID_TRAY_LOGOUT     = 40003;
    static constexpr UINT ID_TRAY_QUIT       = 40004;

    explicit TrayIcon(HINSTANCE hInstance);
    ~TrayIcon();

    /// Create the tray icon and hidden message window. Returns false on failure.
    bool Create();

    /// Remove the tray icon from the system tray.
    void Destroy();

    /// Access the underlying message-only HWND. Used by App to wire this
    /// window up as the UI-thread marshalling target for WS-worker-thread
    /// events (see AppState::SetUIThreadTarget). Returns nullptr before
    /// Create() or after Destroy(). The window is created on and pumped by
    /// the UI thread (the main thread's GetMessage loop), so any message
    /// delivered to it via PostMessageW is guaranteed to be handled on the
    /// UI thread.
    HWND GetMessageHwnd() const { return hwnd_; }

    /// Show a balloon notification (NIF_INFO).
    void ShowBalloon(const std::wstring& title, const std::wstring& message);

    /// Update tooltip text.
    void SetTooltip(const std::wstring& tip);

private:
    static LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void ShowContextMenu();
    void HandleMenuCommand(UINT cmdId);

    HINSTANCE hInstance_ = nullptr;
    HWND hwnd_ = nullptr;
    NOTIFYICONDATAW nid_ = {};
    bool created_ = false;

    static constexpr UINT WM_TRAYICON = WM_APP + 100;
    static constexpr wchar_t kTrayWndClass[] = L"StickyTodo_TrayWnd";
    static bool classRegistered_;
};

} // namespace stickytodo::ui
