#include "ui/SettingsWindow.h"
#include "ui/D2DRenderer.h"
#include "ui/Preferences.h"
#include "ui/Theme.h"
#include "App.h"
#include "core/AppState.h"

#include <windowsx.h>

namespace stickytodo::ui {

bool SettingsWindow::classRegistered_ = false;

SettingsWindow::SettingsWindow(HINSTANCE hInstance)
    : hInstance_(hInstance)
{
    // Initialize controls with default values
    urlInput_.placeholder = L"http://localhost:8080";
    usernameInput_.placeholder = L"Username";
    passwordInput_.placeholder = L"Password";
    passwordInput_.isPassword = true;

    testButton_.text = L"Test Connection";
    loginButton_.text = L"Login";
    logoutButton_.text = L"Logout";

    // Pre-fill URL from stored state
    auto* app = GetApp();
    if (app && app->GetState()) {
        std::string url = app->GetState()->GetBaseUrl();
        urlInput_.text = std::wstring(url.begin(), url.end());
    }
}

SettingsWindow::~SettingsWindow() {
    DiscardRenderTarget();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void SettingsWindow::Show() {
    if (!hwnd_) {
        if (!CreateWindow_()) return;
    }
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);

    // Refresh state
    auto* app = GetApp();
    if (app && app->GetState()) {
        std::string url = app->GetState()->GetBaseUrl();
        if (!url.empty()) {
            urlInput_.text = std::wstring(url.begin(), url.end());
        }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsWindow::Hide() {
    if (hwnd_) {
        ShowWindow(hwnd_, SW_HIDE);
    }
}

bool SettingsWindow::CreateWindow_() {
    if (!classRegistered_) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hInstance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc)) return false;
        classRegistered_ = true;
    }

    DWORD style = WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX;
    hwnd_ = CreateWindowExW(
        0, kClassName, L"StickyTodo - Settings",
        style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        Theme::kSettingsWidth, Theme::kSettingsHeight,
        nullptr, nullptr, hInstance_, this
    );

    if (!hwnd_) return false;
    CreateRenderTarget();
    return true;
}

void SettingsWindow::CreateRenderTarget() {
    if (renderTarget_) return;
    auto* app = GetApp();
    if (app && app->GetRenderer()) {
        renderTarget_ = app->GetRenderer()->CreateRenderTarget(hwnd_);
    }
}

void SettingsWindow::DiscardRenderTarget() {
    if (renderTarget_) {
        renderTarget_->Release();
        renderTarget_ = nullptr;
    }
}

void SettingsWindow::OnPaint() {
    if (!renderTarget_) {
        CreateRenderTarget();
        if (!renderTarget_) return;
    }

    auto* app = GetApp();
    if (!app || !app->GetRenderer()) return;

    IDWriteFactory* dw = app->GetRenderer()->GetDWriteFactory();
    float dpi = D2DRenderer::GetDpiScale(hwnd_);

    renderTarget_->BeginDraw();
    renderTarget_->Clear(Theme::WindowBackground());

    DrawTabs(renderTarget_, dw, dpi);

    switch (activeTab_) {
        case Tab::Settings: DrawSettingsTab(renderTarget_, dw, dpi); break;
        case Tab::History:  DrawHistoryTab(renderTarget_, dw, dpi); break;
        case Tab::About:    DrawAboutTab(renderTarget_, dw, dpi); break;
    }

    HRESULT hr = renderTarget_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        DiscardRenderTarget();
    }
}

void SettingsWindow::OnResize() {
    if (renderTarget_) {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);
        renderTarget_->Resize(size);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsWindow::OnMouseMove(int x, int y) {
    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);
    testButton_.HandleMouse(WM_MOUSEMOVE, fx, fy);
    loginButton_.HandleMouse(WM_MOUSEMOVE, fx, fy);
    logoutButton_.HandleMouse(WM_MOUSEMOVE, fx, fy);
    // CheckBox hover repaint (fills `hovered` flag — currently only used by
    // the checkbox's own render, but keeping the dispatch consistent with the
    // other controls makes future visual feedback trivial to add).
    showStickyDeleteConfirmBox_.HandleMouse(WM_MOUSEMOVE, fx, fy);
    showTodoDeleteConfirmBox_.HandleMouse(WM_MOUSEMOVE, fx, fy);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsWindow::OnLButtonDown(int x, int y) {
    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);

    // Tab clicking
    RECT rc;
    GetClientRect(hwnd_, &rc);
    float tabWidth = static_cast<float>(rc.right - rc.left) / 3.0f;
    if (fy < Theme::kTabHeight) {
        if (fx < tabWidth) activeTab_ = Tab::Settings;
        else if (fx < tabWidth * 2) {
            activeTab_ = Tab::History;
            if (!logsLoaded_) LoadAuditLogs();
        }
        else activeTab_ = Tab::About;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    // Forward to controls
    urlInput_.HandleMouse(WM_LBUTTONDOWN, fx, fy);
    usernameInput_.HandleMouse(WM_LBUTTONDOWN, fx, fy);
    passwordInput_.HandleMouse(WM_LBUTTONDOWN, fx, fy);
    testButton_.HandleMouse(WM_LBUTTONDOWN, fx, fy);
    loginButton_.HandleMouse(WM_LBUTTONDOWN, fx, fy);
    logoutButton_.HandleMouse(WM_LBUTTONDOWN, fx, fy);
    // CheckBox toggles on LBUTTONUP (see CheckBox::HandleMouse) so down does
    // nothing meaningful here, but forward the event for symmetry with the
    // other controls and to keep the hover state machine consistent.
    showStickyDeleteConfirmBox_.HandleMouse(WM_LBUTTONDOWN, fx, fy);
    showTodoDeleteConfirmBox_.HandleMouse(WM_LBUTTONDOWN, fx, fy);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsWindow::OnLButtonUp(int x, int y) {
    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);
    testButton_.HandleMouse(WM_LBUTTONUP, fx, fy);
    loginButton_.HandleMouse(WM_LBUTTONUP, fx, fy);
    logoutButton_.HandleMouse(WM_LBUTTONUP, fx, fy);
    // CheckBox actually flips `checked` on LBUTTONUP and calls onToggle — the
    // persistence of the flipped value is driven by the onToggle callback set
    // each frame in DrawSettingsTab (the callback writes to Preferences).
    showStickyDeleteConfirmBox_.HandleMouse(WM_LBUTTONUP, fx, fy);
    showTodoDeleteConfirmBox_.HandleMouse(WM_LBUTTONUP, fx, fy);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsWindow::OnChar(wchar_t ch) {
    urlInput_.HandleChar(ch);
    usernameInput_.HandleChar(ch);
    passwordInput_.HandleChar(ch);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsWindow::OnKeyDown(WPARAM vk, LPARAM lParam) {
    urlInput_.HandleKey(WM_KEYDOWN, vk, lParam);
    usernameInput_.HandleKey(WM_KEYDOWN, vk, lParam);
    passwordInput_.HandleKey(WM_KEYDOWN, vk, lParam);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsWindow::DrawTabs(ID2D1RenderTarget* rt, IDWriteFactory* dw, float dpi) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    float width = static_cast<float>(rc.right - rc.left);
    float tabWidth = width / 3.0f;

    const wchar_t* tabNames[] = {L"Settings", L"History", L"About"};
    Tab tabs[] = {Tab::Settings, Tab::History, Tab::About};

    for (int i = 0; i < 3; i++) {
        float tx = tabWidth * i;
        bool isActive = (tabs[i] == activeTab_);

        ID2D1SolidColorBrush* bgBrush = nullptr;
        rt->CreateSolidColorBrush(isActive ? Theme::TabActive() : Theme::TabInactive(), &bgBrush);
        if (bgBrush) {
            D2D1_RECT_F tabRect = D2D1::RectF(tx, 0, tx + tabWidth, Theme::kTabHeight);
            rt->FillRectangle(tabRect, bgBrush);
            bgBrush->Release();
        }

        // Tab text
        IDWriteTextFormat* format = nullptr;
        dw->CreateTextFormat(L"Segoe UI", nullptr,
                              isActive ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_REGULAR,
                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                              Theme::kFontSizeBody * dpi, L"en-us", &format);
        if (format) {
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

            ID2D1SolidColorBrush* textBrush = nullptr;
            rt->CreateSolidColorBrush(Theme::TextPrimary(), &textBrush);
            if (textBrush) {
                D2D1_RECT_F textRect = D2D1::RectF(tx, 0, tx + tabWidth, Theme::kTabHeight);
                rt->DrawText(tabNames[i], static_cast<UINT32>(wcslen(tabNames[i])),
                              format, textRect, textBrush);
                textBrush->Release();
            }
            format->Release();
        }
    }

    // Separator below tabs
    ID2D1SolidColorBrush* sepBrush = nullptr;
    rt->CreateSolidColorBrush(Theme::Separator(), &sepBrush);
    if (sepBrush) {
        rt->DrawLine(D2D1::Point2F(0, Theme::kTabHeight),
                      D2D1::Point2F(width, Theme::kTabHeight), sepBrush, 1.0f);
        sepBrush->Release();
    }
}

void SettingsWindow::DrawSettingsTab(ID2D1RenderTarget* rt, IDWriteFactory* dw, float dpi) {
    float y = Theme::kTabHeight + Theme::kPaddingLarge;
    float x = Theme::kPaddingLarge;
    float w = static_cast<float>(Theme::kSettingsWidth) - 2 * Theme::kPaddingLarge - 20.0f;

    auto* app = GetApp();
    bool authenticated = app && app->GetState() && app->GetState()->IsAuthenticated();

    // "Server URL" label
    Label urlLabel;
    urlLabel.rect = {x, y, w, 20.0f};
    urlLabel.text = L"Server URL";
    urlLabel.fontSize = Theme::kFontSizeSmall;
    urlLabel.color = Theme::TextSecondary();
    urlLabel.Draw(rt, dw, dpi);
    y += 22.0f;

    // URL input
    urlInput_.rect = {x, y, w, Theme::kInputHeight};
    urlInput_.enabled = !authenticated;
    urlInput_.Draw(rt, dw, dpi);
    y += Theme::kInputHeight + Theme::kPadding;

    // Test connection button.
    //
    // `enabled` is gated on `!testInFlight_` so repeated clicks while
    // a request is in flight are swallowed by Button::HandleMouse
    // (which short-circuits on !enabled) and the user gets a visual
    // "grayed out" cue courtesy of Button::Draw's alpha halving. The
    // inflight flag is cleared by the UI-thread completion callback
    // posted via AppState::TestConnectionAsync → PostToUIThread.
    testButton_.rect = {x, y, 140.0f, Theme::kButtonHeight};
    testButton_.enabled = !testInFlight_;
    testButton_.onClick = [this]() { DoTestConnection(); };
    testButton_.Draw(rt, dw, dpi);

    // Connection status text
    if (!connectionStatus_.empty()) {
        Label statusLbl;
        statusLbl.rect = {x + 150.0f, y, w - 150.0f, Theme::kButtonHeight};
        statusLbl.text = connectionStatus_;
        statusLbl.fontSize = Theme::kFontSizeSmall;
        statusLbl.color = Theme::TextSecondary();
        statusLbl.Draw(rt, dw, dpi);
    }
    y += Theme::kButtonHeight + Theme::kPaddingLarge;

    if (!authenticated) {
        // Username label + input
        Label userLabel;
        userLabel.rect = {x, y, w, 20.0f};
        userLabel.text = L"Username";
        userLabel.fontSize = Theme::kFontSizeSmall;
        userLabel.color = Theme::TextSecondary();
        userLabel.Draw(rt, dw, dpi);
        y += 22.0f;

        usernameInput_.rect = {x, y, w, Theme::kInputHeight};
        usernameInput_.Draw(rt, dw, dpi);
        y += Theme::kInputHeight + Theme::kPadding;

        // Password label + input
        Label passLabel;
        passLabel.rect = {x, y, w, 20.0f};
        passLabel.text = L"Password";
        passLabel.fontSize = Theme::kFontSizeSmall;
        passLabel.color = Theme::TextSecondary();
        passLabel.Draw(rt, dw, dpi);
        y += 22.0f;

        passwordInput_.rect = {x, y, w, Theme::kInputHeight};
        passwordInput_.Draw(rt, dw, dpi);
        y += Theme::kInputHeight + Theme::kPaddingLarge;

        // Login button — gated on !loginInFlight_, see testButton_
        // above for the identical rationale.
        loginButton_.rect = {x, y, 100.0f, Theme::kButtonHeight};
        loginButton_.enabled = !loginInFlight_;
        loginButton_.onClick = [this]() { DoLogin(); };
        loginButton_.Draw(rt, dw, dpi);
    } else {
        // Logged in state
        Label loggedInLabel;
        loggedInLabel.rect = {x, y, w, 24.0f};
        std::string username = app->GetState()->GetUsername();
        loggedInLabel.text = L"Logged in as: " + std::wstring(username.begin(), username.end());
        loggedInLabel.fontSize = Theme::kFontSizeBody;
        loggedInLabel.color = Theme::TextPrimary();
        loggedInLabel.Draw(rt, dw, dpi);
        y += 32.0f;

        // Logout button
        logoutButton_.rect = {x, y, 100.0f, Theme::kButtonHeight};
        logoutButton_.onClick = [this]() { DoLogout(); };
        logoutButton_.Draw(rt, dw, dpi);
        y += Theme::kButtonHeight + Theme::kPaddingLarge;
    }

    // --------- 通用 section — delete-confirm toggles (mirrors macOS) ---------
    //
    // The macOS SettingsView renders two `Toggle`s — "删除便签前弹出确认" and
    // "删除待办前弹出确认" — whose values are the *inverse* of the stored
    // skipStickyDeleteConfirm / skipTodoDeleteConfirm flags (see
    // showStickyDeleteConfirmBinding / showTodoDeleteConfirmBinding in
    // SettingsView.swift). We reproduce the exact same semantics: the
    // checkbox reads as "show confirm", stored key means "skip confirm",
    // so `checked = !ShouldSkip...` and `onToggle → SetSkip...(!checked)`.
    //
    // We lay them out regardless of `authenticated` because the preferences
    // are per-machine, not per-session: a logged-out user who previously
    // silenced a dialog should still be able to re-enable it.
    Label generalHeader;
    generalHeader.rect = {x, y, w, 20.0f};
    generalHeader.text = L"\u901A\u7528"; // 通用
    generalHeader.fontSize = Theme::kFontSizeSmall;
    generalHeader.bold = true;
    generalHeader.color = Theme::TextSecondary();
    generalHeader.Draw(rt, dw, dpi);
    y += 24.0f;

    // Row 1: sticky delete confirmation
    showStickyDeleteConfirmBox_.rect = {x, y, w, 22.0f};
    showStickyDeleteConfirmBox_.label = L"\u5220\u9664\u4FBF\u7B7E\u524D\u5F39\u51FA\u786E\u8BA4"; // 删除便签前弹出确认
    showStickyDeleteConfirmBox_.checked = !ShouldSkipStickyDeleteConfirm();
    showStickyDeleteConfirmBox_.onToggle = [](bool showConfirm) {
        // Invert UI → stored semantics: show=true means skip=false.
        SetSkipStickyDeleteConfirm(!showConfirm);
    };
    showStickyDeleteConfirmBox_.Draw(rt, dw, dpi);
    y += 22.0f + Theme::kPadding;

    // Row 2: todo delete confirmation
    showTodoDeleteConfirmBox_.rect = {x, y, w, 22.0f};
    showTodoDeleteConfirmBox_.label = L"\u5220\u9664\u5F85\u529E\u524D\u5F39\u51FA\u786E\u8BA4"; // 删除待办前弹出确认
    showTodoDeleteConfirmBox_.checked = !ShouldSkipTodoDeleteConfirm();
    showTodoDeleteConfirmBox_.onToggle = [](bool showConfirm) {
        SetSkipTodoDeleteConfirm(!showConfirm);
    };
    showTodoDeleteConfirmBox_.Draw(rt, dw, dpi);
}

void SettingsWindow::DrawHistoryTab(ID2D1RenderTarget* rt, IDWriteFactory* dw, float dpi) {
    float y = Theme::kTabHeight + Theme::kPadding;
    float x = Theme::kPaddingLarge;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    float width = static_cast<float>(rc.right - rc.left);
    float height = static_cast<float>(rc.bottom - rc.top);

    auto* app = GetApp();
    bool authenticated = app && app->GetState() && app->GetState()->IsAuthenticated();

    if (!authenticated) {
        Label hint;
        hint.rect = {x, y + 40.0f, width - 2 * x, 40.0f};
        hint.text = L"Please login in the Settings tab to view history.";
        hint.fontSize = Theme::kFontSizeBody;
        hint.color = Theme::TextSecondary();
        hint.Draw(rt, dw, dpi);
        return;
    }

    if (auditLogs_.empty()) {
        Label hint;
        hint.rect = {x, y + 40.0f, width - 2 * x, 40.0f};
        // Three distinct empty states:
        //   • auditInFlight_ → request is mid-flight, show "Loading..."
        //     so the user knows the blank space isn't a bug.
        //   • !logsLoaded_   → user just opened History tab for the
        //     first time this session and LoadAuditLogs hasn't kicked
        //     off the fetch yet (defensive — OnLButtonDown schedules
        //     it immediately so this should be a transient state).
        //   • everything else → genuine empty result from the server.
        if (auditInFlight_) {
            hint.text = L"Loading...";
        } else if (!logsLoaded_) {
            hint.text = L"Loading...";
        } else {
            hint.text = L"No audit log entries.";
        }
        hint.fontSize = Theme::kFontSizeBody;
        hint.color = Theme::TextSecondary();
        hint.Draw(rt, dw, dpi);
        return;
    }

    // Scroll view for audit logs
    historyScroll_.rect = {0, y, width, height - y};
    historyScroll_.contentHeight = static_cast<float>(auditLogs_.size()) * 48.0f;

    historyScroll_.BeginContent(rt);

    float rowY = y;
    for (const auto& log : auditLogs_) {
        // Action
        Label actionLabel;
        actionLabel.rect = {x, rowY + 4.0f, width - 2 * x, 18.0f};
        actionLabel.text = std::wstring(log.action.begin(), log.action.end());
        actionLabel.fontSize = Theme::kFontSizeBody;
        actionLabel.bold = true;
        actionLabel.color = Theme::TextPrimary();
        actionLabel.Draw(rt, dw, dpi);

        // Timestamp + actor
        std::string info = log.created_at + " by " + log.actor;
        Label infoLabel;
        infoLabel.rect = {x, rowY + 24.0f, width - 2 * x, 16.0f};
        infoLabel.text = std::wstring(info.begin(), info.end());
        infoLabel.fontSize = Theme::kFontSizeSmall;
        infoLabel.color = Theme::TextSecondary();
        infoLabel.Draw(rt, dw, dpi);

        // Separator
        ID2D1SolidColorBrush* sepBrush = nullptr;
        rt->CreateSolidColorBrush(Theme::Separator(), &sepBrush);
        if (sepBrush) {
            float sepLineY = rowY + 47.0f;
            rt->DrawLine(D2D1::Point2F(x, sepLineY), D2D1::Point2F(width - x, sepLineY), sepBrush, 0.5f);
            sepBrush->Release();
        }

        rowY += 48.0f;
    }

    historyScroll_.EndContent(rt);
    historyScroll_.DrawScrollbar(rt);
}

void SettingsWindow::DrawAboutTab(ID2D1RenderTarget* rt, IDWriteFactory* dw, float dpi) {
    float y = Theme::kTabHeight + Theme::kPaddingLarge;
    float x = Theme::kPaddingLarge;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    float width = static_cast<float>(rc.right - rc.left);

    // App name
    Label nameLabel;
    nameLabel.rect = {x, y, width - 2 * x, 28.0f};
    nameLabel.text = L"StickyTodo";
    nameLabel.fontSize = Theme::kFontSizeHeading;
    nameLabel.bold = true;
    nameLabel.color = Theme::TextPrimary();
    nameLabel.Draw(rt, dw, dpi);
    y += 36.0f;

    // Version
    Label versionLabel;
    versionLabel.rect = {x, y, width - 2 * x, 20.0f};
    versionLabel.text = L"Version 1.0.0 (Windows)";
    versionLabel.fontSize = Theme::kFontSizeBody;
    versionLabel.color = Theme::TextSecondary();
    versionLabel.Draw(rt, dw, dpi);
    y += 28.0f;

    // Description
    Label descLabel;
    descLabel.rect = {x, y, width - 2 * x, 40.0f};
    descLabel.text = L"Desktop sticky notes with cloud sync. "
                     L"Keeps your TODO items visible and in sync across devices.";
    descLabel.fontSize = Theme::kFontSizeBody;
    descLabel.color = Theme::TextPrimary();
    descLabel.Draw(rt, dw, dpi);
    y += 52.0f;

    // Links
    Label linksLabel;
    linksLabel.rect = {x, y, width - 2 * x, 60.0f};
    linksLabel.text = L"GitHub: https://github.com/hanxi/stickytodo\n"
                      L"License: MIT";
    linksLabel.fontSize = Theme::kFontSizeSmall;
    linksLabel.color = Theme::TextSecondary();
    linksLabel.Draw(rt, dw, dpi);
}

// ---------------------------------------------------------------------------
// Async network button handlers
// ---------------------------------------------------------------------------
//
// All three handlers below follow the same pattern:
//
//   1. Input validation + short-circuit if a prior request is still
//      in flight (defensive — Button::HandleMouse already gates on
//      enabled=!inFlight_, but handling the re-entry here too makes
//      this code safe against any future non-button caller).
//   2. Flip `*InFlight_ = true`, set status text to a progress
//      string, request a repaint. The next frame renders with the
//      button grayed and the "Testing..." / "Logging in..." /
//      "Loading..." hint.
//   3. Kick off AppState's async API. The callback is marshalled
//      back onto the UI thread by core::PostToUIThread — see
//      AppState::TestConnectionAsync / LoginAsync / ListAuditLogsAsync.
//   4. In the callback:
//        a. Liveness check — during the 10s×4 WinHTTP budget the
//           user may have closed Settings. GetApp()->GetSettingsWindow()
//           returns nullptr if the owning App destroyed settings_,
//           or a DIFFERENT pointer if (hypothetically) a new
//           SettingsWindow was instantiated in the meantime.
//           Compare against the captured `self` to cover both.
//        b. Clear `*InFlight_`, update status text / audit logs,
//           request a repaint.
//
// The callbacks capture `self = this`. That is safe because:
//   - App owns SettingsWindow in a unique_ptr; destruction happens on
//     the UI thread during App::Shutdown AFTER the message loop has
//     exited, so at that point no PostToUIThread callback can still
//     reach us — the tray HWND is already gone, PostToUIThread
//     returns false, and the lambda is dropped on the worker thread
//     without ever dereferencing `self`.
//   - The liveness check above handles the "Settings currently
//     hidden / destroyed but App still alive" case (which today
//     doesn't actually happen because App keeps the unique_ptr alive
//     for the whole app lifetime, but we code defensively for future
//     changes).

void SettingsWindow::DoTestConnection() {
    if (testInFlight_) return;

    std::string url(urlInput_.text.begin(), urlInput_.text.end());
    if (url.empty()) {
        connectionStatus_ = L"Please enter a URL";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    auto* app = GetApp();
    if (!app || !app->GetState()) return;

    testInFlight_ = true;
    connectionStatus_ = L"Testing...";
    InvalidateRect(hwnd_, nullptr, FALSE);

    SettingsWindow* self = this;
    app->GetState()->TestConnectionAsync(url,
        [self](std::string version) {
            auto* app2 = GetApp();
            if (!app2 || app2->GetSettingsWindow() != self) {
                // Window vanished during the request. Drop the
                // result — we cannot touch `self` safely.
                return;
            }
            self->testInFlight_ = false;
            if (!version.empty()) {
                self->connectionStatus_ = L"Connected (v" +
                    std::wstring(version.begin(), version.end()) + L")";
            } else {
                self->connectionStatus_ = L"Connection failed";
            }
            if (self->hwnd_) InvalidateRect(self->hwnd_, nullptr, FALSE);
        });
}

void SettingsWindow::DoLogin() {
    if (loginInFlight_) return;

    std::string url(urlInput_.text.begin(), urlInput_.text.end());
    std::string username(usernameInput_.text.begin(), usernameInput_.text.end());
    std::string password(passwordInput_.text.begin(), passwordInput_.text.end());

    if (url.empty() || username.empty() || password.empty()) {
        connectionStatus_ = L"Please fill all fields";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    auto* app = GetApp();
    if (!app || !app->GetState()) return;

    loginInFlight_ = true;
    connectionStatus_ = L"Logging in...";
    InvalidateRect(hwnd_, nullptr, FALSE);

    SettingsWindow* self = this;
    app->GetState()->LoginAsync(url, username, password,
        [self](bool success) {
            auto* app2 = GetApp();
            if (!app2 || app2->GetSettingsWindow() != self) {
                // Window closed mid-login. AppState already applied
                // the success side effects (token persistence, WS
                // connect, onAuthChanged_) from inside LoginAsync's
                // UI-thread callback — those mutations happened on
                // the AppState level, independent of whether any
                // particular UI widget is still around to observe.
                return;
            }
            self->loginInFlight_ = false;
            if (success) {
                self->connectionStatus_ = L"Login successful";
                self->passwordInput_.text.clear();
            } else {
                self->connectionStatus_ = L"Login failed";
            }
            if (self->hwnd_) InvalidateRect(self->hwnd_, nullptr, FALSE);
        });
}

void SettingsWindow::DoLogout() {
    // Synchronous — no network round-trip (AppState::Logout just
    // closes WS + clears local state + deletes saved credential).
    auto* app = GetApp();
    if (app && app->GetState()) {
        app->GetState()->Logout();
    }
    connectionStatus_.clear();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsWindow::LoadAuditLogs() {
    if (auditInFlight_) return;

    auto* app = GetApp();
    if (!app || !app->GetState() || !app->GetState()->IsAuthenticated()) return;

    auditInFlight_ = true;
    InvalidateRect(hwnd_, nullptr, FALSE);

    SettingsWindow* self = this;
    // Empty action filter = all actions; matches the legacy sync
    // ListAuditLogs(1, 50) call that had no third argument.
    app->GetState()->ListAuditLogsAsync(1, 50, std::string{},
        [self](std::vector<models::AuditLog> logs) {
            auto* app2 = GetApp();
            if (!app2 || app2->GetSettingsWindow() != self) return;
            self->auditInFlight_ = false;
            self->auditLogs_ = std::move(logs);
            self->logsLoaded_ = true;
            if (self->hwnd_) InvalidateRect(self->hwnd_, nullptr, FALSE);
        });
}

LRESULT CALLBACK SettingsWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_CREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return 0;
    }

    auto* self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
        return self->HandleMessage(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT SettingsWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd_, &ps);
            OnPaint();
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_SIZE:
            OnResize();
            return 0;
        case WM_MOUSEMOVE:
            OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_LBUTTONDOWN:
            OnLButtonDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_LBUTTONUP:
            OnLButtonUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_CHAR:
            OnChar(static_cast<wchar_t>(wParam));
            return 0;
        case WM_KEYDOWN:
            OnKeyDown(wParam, lParam);
            return 0;
        case WM_SETCURSOR: {
            // Show an I-beam text cursor when the mouse is over any
            // TextBox in the client area. Without this the window's
            // WNDCLASS IDC_ARROW is used everywhere — which for an
            // input field looks "not clickable" even though it is.
            //
            // LPARAM low word = hit-test code; we only override for
            // HTCLIENT so title bar / resize borders keep their
            // normal system cursors. WM_SETCURSOR does NOT carry
            // the mouse position; pull it from GetCursorPos +
            // ScreenToClient.
            if (LOWORD(lParam) == HTCLIENT) {
                POINT pt;
                if (GetCursorPos(&pt) && ScreenToClient(hwnd_, &pt)) {
                    float mx = static_cast<float>(pt.x);
                    float my = static_cast<float>(pt.y);
                    // urlInput_ is disabled while authenticated
                    // (DrawSettingsTab sets .enabled = !authenticated),
                    // and username/password inputs are ONLY laid out
                    // in the unauthenticated branch — so gate each
                    // hit-test on its current enabled flag to avoid
                    // showing an I-beam over a stale/hidden rect.
                    bool overInput =
                        (urlInput_.enabled && urlInput_.rect.Contains(mx, my)) ||
                        (usernameInput_.enabled && usernameInput_.rect.Contains(mx, my)) ||
                        (passwordInput_.enabled && passwordInput_.rect.Contains(mx, my));
                    if (overInput) {
                        SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
                        return TRUE;
                    }
                }
            }
            return DefWindowProcW(hwnd_, msg, wParam, lParam);
        }
        case WM_CLOSE:
            Hide();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        default:
            return DefWindowProcW(hwnd_, msg, wParam, lParam);
    }
}

} // namespace stickytodo::ui
