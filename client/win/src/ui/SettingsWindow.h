#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <vector>

#include "ui/Controls.h"
#include "models/AuditLog.h"

namespace stickytodo::ui {

/// Settings window with 3 tabs: Settings / History / About.
/// Analogous to macOS SettingsView (client/mac/stickytodo/Views/SettingsView.swift).
///
/// Settings tab: Server URL, test connection, login/logout.
/// History tab: Audit log list (ScrollView).
/// About tab: Version, links.
class SettingsWindow {
public:
    explicit SettingsWindow(HINSTANCE hInstance);
    ~SettingsWindow();

    /// Show (create if necessary) the settings window.
    void Show();

    /// Hide the settings window.
    void Hide();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void OnPaint();
    void OnResize();
    void OnMouseMove(int x, int y);
    void OnLButtonDown(int x, int y);
    void OnLButtonUp(int x, int y);
    void OnChar(wchar_t ch);
    void OnKeyDown(WPARAM vk, LPARAM lParam);

    bool CreateWindow_();
    void CreateRenderTarget();
    void DiscardRenderTarget();

    void DrawSettingsTab(ID2D1RenderTarget* rt, IDWriteFactory* dw, float dpi);
    void DrawHistoryTab(ID2D1RenderTarget* rt, IDWriteFactory* dw, float dpi);
    void DrawAboutTab(ID2D1RenderTarget* rt, IDWriteFactory* dw, float dpi);
    void DrawTabs(ID2D1RenderTarget* rt, IDWriteFactory* dw, float dpi);

    void DoLogin();
    void DoLogout();
    void DoTestConnection();
    void LoadAuditLogs();

    HINSTANCE hInstance_;
    HWND hwnd_ = nullptr;
    ID2D1HwndRenderTarget* renderTarget_ = nullptr;

    // Tab state
    enum class Tab { Settings, History, About };
    Tab activeTab_ = Tab::Settings;

    // Settings tab controls
    TextBox urlInput_;
    TextBox usernameInput_;
    TextBox passwordInput_;
    Button testButton_;
    Button loginButton_;
    Button logoutButton_;
    Label statusLabel_;
    std::wstring connectionStatus_;

    // "通用" section — mirrors macOS SettingsView's two Toggles.
    //
    // Storage semantics are *inverse* to the UI semantics:
    //   stored skipStickyDeleteConfirm=true  ↔  UI "Show confirm"=unchecked
    //   stored skipStickyDeleteConfirm=false ↔  UI "Show confirm"=checked
    // The inversion is applied by the Draw/HandleMouse code that binds these
    // CheckBoxes to Preferences::{Should,Set}Skip{Sticky,Todo}DeleteConfirm,
    // matching the macOS binding's get/set closure pair (see SettingsView:
    // showStickyDeleteConfirmBinding / showTodoDeleteConfirmBinding).
    CheckBox showStickyDeleteConfirmBox_;
    CheckBox showTodoDeleteConfirmBox_;

    // History tab
    ScrollView historyScroll_;
    std::vector<models::AuditLog> auditLogs_;
    bool logsLoaded_ = false;

    static bool classRegistered_;
    static constexpr wchar_t kClassName[] = L"StickyTodo_SettingsWnd";
};

} // namespace stickytodo::ui
