// FilterEditor.cpp — modal filter editor dialog (Win32 + Direct2D).
//
// See FilterEditor.h for the public contract. This translation unit owns the
// entire dialog's runtime state in a TU-local `Impl` struct so the header can
// stay free of Controls / D2D includes.
//
// Design notes:
//   * We deliberately DO NOT use DialogBox / DialogBoxParam — those require a
//     Win32 dialog template, and our rendering path is Direct2D self-drawn
//     like every other window in this client. So "modal" here is implemented
//     manually: disable the owner, run a local message loop until the dialog
//     closes, then re-enable and refocus the owner. This is the same pattern
//     the Microsoft docs call a "modal message loop" (see
//     https://learn.microsoft.com/en-us/windows/win32/winmsg/using-messages-and-message-queues#creating-a-message-loop).
//   * Layout mirrors client/mac/stickytodo/Views/FilterEditor.swift closely
//     so users switching between platforms see the same controls in the
//     same order (status segmented row → tag / keyword → include_deleted /
//     only_deleted → page_size stepper → header buttons).

#include "ui/FilterEditor.h"

#include "ui/Controls.h"
#include "ui/D2DRenderer.h"
#include "ui/Theme.h"
#include "App.h"

#include <d2d1.h>
#include <dwrite.h>
#include <windows.h>
#include <windowsx.h>

#include <string>

namespace stickytodo::ui {

namespace {

constexpr wchar_t kClassName[] = L"StickyTodo_FilterEditorWnd";
constexpr int kDialogWidth = 480;
constexpr int kDialogHeight = 560;

// Status choices — indexed by segmented-picker position. Maps to
// models::Filter::status string values used by the backend.
constexpr wchar_t kStatusLabels[3][5] = { L"\u5168\u90E8",   // 全部
                                          L"\u672A\u5B8C",   // 未完 (未完成，截短适应按钮宽度)
                                          L"\u5DF2\u5B8C" }; // 已完 (已完成)
constexpr const char* kStatusValues[3] = { "", "pending", "done" };

int StatusToIndex(const std::string& s) {
    for (int i = 0; i < 3; ++i) {
        if (s == kStatusValues[i]) return i;
    }
    // Any legacy/unknown value (e.g. old "all") collapses to 全部 — matches
    // the macOS StatusChoice fallback where `.none` ↔ "全部".
    return 0;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len, nullptr, nullptr);
    return out;
}

/// Runtime state for one dialog invocation. Lifetime bound to
/// FilterEditor::ShowModal's stack frame; the HWND's GWLP_USERDATA stores a
/// pointer to this struct so WndProc can recover it.
struct Impl {
    // Input/output — committed on 保存, untouched on cancel/ESC/×.
    models::Filter* out = nullptr;

    // Working copy of the filter — all controls bind to this. `out` is only
    // written if the user presses 保存.
    models::Filter draft;

    // Controls
    Button statusSegments_[3];  // 全部 / 未完成 / 已完成
    TextBox tagBox_;
    TextBox keywordBox_;
    CheckBox includeDeletedBox_;
    CheckBox onlyDeletedBox_;
    Button pageSizeMinus_;
    Button pageSizePlus_;
    Button cancelButton_;
    Button resetButton_;
    Button saveButton_;

    // D2D state
    HWND hwnd = nullptr;
    HWND owner = nullptr;
    HINSTANCE hInstance = nullptr;
    ID2D1HwndRenderTarget* renderTarget = nullptr;

    // Result — true only after 保存 has committed to `*out`.
    bool committed = false;

    // Currently focused TextBox (one at a time). Nullptr = keyboard input
    // goes nowhere. Matches how StickyWindow routes WM_CHAR/WM_KEYDOWN.
    TextBox* focusedBox = nullptr;

    // Cached page-size label (redrawn each paint from draft.page_size).
    // Kept as a member so we don't re-format each frame.
    std::wstring pageSizeLabelCache;
};

// WndProc forward decls — defined after the main flow below for readability.
LRESULT CALLBACK FilterEditorWndProc(HWND, UINT, WPARAM, LPARAM);
void InitControls(Impl* impl);
void LayoutAndDraw(Impl* impl);
void OnPaint(Impl* impl);
void OnResize(Impl* impl);
void OnMouseMove(Impl* impl, int x, int y);
void OnLButtonDown(Impl* impl, int x, int y);
void OnLButtonUp(Impl* impl, int x, int y);
void OnChar(Impl* impl, wchar_t ch);
void OnKeyDown(Impl* impl, WPARAM vk, LPARAM lParam);
void ApplyStatusSelection(Impl* impl, int idx);
void ApplyPageSizeDelta(Impl* impl, int delta);
void DoReset(Impl* impl);
void DoCommitAndClose(Impl* impl);
void DoCancelAndClose(Impl* impl);

// ---- Window-class registration ------------------------------------------

bool EnsureClassRegistered(HINSTANCE hInstance) {
    static bool registered = false;
    if (registered) return true;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = FilterEditorWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    // No background brush — we clear in OnPaint via Direct2D so Win32's
    // ERASEBKGND doesn't race the D2D frame (same pattern as StickyWindow /
    // SettingsWindow in this project).
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) {
        // If another module in this process already registered the class,
        // RegisterClassExW fails with ERROR_CLASS_ALREADY_EXISTS — treat
        // that as success (class name is process-global).
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    }
    registered = true;
    return true;
}

} // namespace

// ---- Public entry point --------------------------------------------------

bool FilterEditor::ShowModal(HWND owner, HINSTANCE hInstance, models::Filter& filter) {
    if (!owner || !hInstance) return false;
    if (!EnsureClassRegistered(hInstance)) return false;

    Impl impl;
    impl.out = &filter;
    impl.draft = filter;      // start from current value
    impl.owner = owner;
    impl.hInstance = hInstance;

    // Center dialog over its owner. GetWindowRect gives screen coords — the
    // math is identical regardless of DPI because we're feeding screen
    // coordinates back into CreateWindowExW.
    //
    // DPI scaling: kDialogWidth/kDialogHeight are 96-DPI baseline values
    // (480×560). Under PerMonitorV2 awareness, the sizes we pass to
    // CreateWindowExW are physical pixels — so at 150% we need 720×840,
    // otherwise the content laid out at `Theme::k* × dpi` would overflow
    // a sub-size window. We sample DPI from the owner window (which is
    // already on a specific monitor) rather than the system, so the
    // dialog matches the monitor the parent sticky is sitting on.
    UINT createDpi = GetDpiForWindow(owner);
    if (createDpi == 0) createDpi = 96;
    float createScale = static_cast<float>(createDpi) / 96.0f;
    int scaledW = static_cast<int>(kDialogWidth * createScale);
    int scaledH = static_cast<int>(kDialogHeight * createScale);

    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    int cx = (ownerRect.left + ownerRect.right) / 2 - scaledW / 2;
    int cy = (ownerRect.top + ownerRect.bottom) / 2 - scaledH / 2;

    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        kClassName,
        L"\u7B5B\u9009\u7F16\u8F91",  // 筛选编辑
        WS_POPUPWINDOW | WS_CAPTION | WS_VISIBLE,
        cx, cy, scaledW, scaledH,
        owner, nullptr, hInstance,
        &impl  // delivered via WM_NCCREATE below
    );
    if (!hwnd) return false;

    // Disable the owner so clicks outside the dialog don't steal focus —
    // Win32's built-in modality semantics. We re-enable in the `modal done`
    // path below; the goto-less variant requires the guard because
    // DoCommitAndClose / DoCancelAndClose destroy the window, which breaks
    // out of the message loop naturally.
    EnableWindow(owner, FALSE);

    // Local modal message loop. Runs until the dialog window is destroyed,
    // at which point IsWindow(hwnd) becomes false and we break out.
    MSG msg;
    while (IsWindow(hwnd) && GetMessage(&msg, nullptr, 0, 0) > 0) {
        // Tab / ESC etc. would normally fall through to IsDialogMessage for
        // system dialogs — but since we own the entire key routing (we're
        // not a Win32 dialog template), we skip it and go straight to
        // Translate/Dispatch. ESC is handled inside WM_KEYDOWN.
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Re-enable and refocus owner. Must happen BEFORE returning so the
    // caller's window is interactive again.
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);

    return impl.committed;
}

namespace {

// ---- WndProc -------------------------------------------------------------

LRESULT CALLBACK FilterEditorWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Recover Impl* from GWLP_USERDATA. For the first few messages before
    // WM_NCCREATE has run, userdata is null — fall through to DefWindowProc.
    Impl* impl = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        impl = reinterpret_cast<Impl*>(cs->lpCreateParams);
        impl->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl));
        InitControls(impl);
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    impl = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!impl) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_PAINT:       OnPaint(impl); return 0;
        case WM_SIZE:        OnResize(impl); return 0;
        case WM_MOUSEMOVE:   OnMouseMove(impl, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)); return 0;
        case WM_LBUTTONDOWN: OnLButtonDown(impl, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)); return 0;
        case WM_LBUTTONUP:   OnLButtonUp(impl, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)); return 0;
        case WM_CHAR:        OnChar(impl, static_cast<wchar_t>(wParam)); return 0;
        case WM_KEYDOWN:     OnKeyDown(impl, wParam, lParam); return 0;

        case WM_DPICHANGED: {
            // Adopt Win32's DPI-scaled suggested rect. The next
            // WM_PAINT will re-run LayoutAndDraw, which reads the
            // current DPI via D2DRenderer::GetDpiScale(impl->hwnd)
            // and re-scales every rect / font — i.e. the dialog
            // immediately reflows on the new monitor without any
            // extra plumbing here.
            RECT* suggested = reinterpret_cast<RECT*>(lParam);
            if (suggested) {
                SetWindowPos(hwnd, nullptr,
                             suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_SETCURSOR: {
            // I-beam cursor over tag / keyword TextBoxes. Both are
            // always laid out (the filter editor doesn't conditionally
            // hide them), so a simple rect hit-test is sufficient —
            // no visibility gate like StickyWindow's drafting_ flag.
            if (LOWORD(lParam) == HTCLIENT) {
                POINT pt;
                if (GetCursorPos(&pt) && ScreenToClient(hwnd, &pt)) {
                    float mx = static_cast<float>(pt.x);
                    float my = static_cast<float>(pt.y);
                    if (impl->tagBox_.rect.Contains(mx, my) ||
                        impl->keywordBox_.rect.Contains(mx, my)) {
                        SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
                        return TRUE;
                    }
                }
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        case WM_CLOSE:
            // × chrome button — treat as cancel.
            DoCancelAndClose(impl);
            return 0;

        case WM_DESTROY:
            if (impl->renderTarget) {
                impl->renderTarget->Release();
                impl->renderTarget = nullptr;
            }
            impl->hwnd = nullptr;
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// ---- Control initialisation ---------------------------------------------

void InitControls(Impl* impl) {
    // Seed initial values from `draft` (a copy of the caller's filter) and
    // bind *all* callbacks once here, not per-frame. Rects are (re)assigned
    // each LayoutAndDraw because the dialog is resizable at the OS level
    // (WS_CAPTION allows the user to drag the edges even on a fixed-size
    // design), but `onChanged` / `onClick` / `onToggle` must be installed
    // immediately so early input — e.g. an IME composition that arrives
    // before the first WM_PAINT — is captured into `draft`.
    const auto& f = impl->draft;

    // Status segmented picker. `selected` drives the persistent
    // "this option is chosen" highlight (see Button::Draw). The initial
    // selection index is read from draft.status here; subsequent
    // selection changes are routed through ApplyStatusSelection so the
    // flag and `draft.status` never diverge.
    const int initialStatus = StatusToIndex(impl->draft.status);
    for (int i = 0; i < 3; ++i) {
        impl->statusSegments_[i].text = kStatusLabels[i];
        impl->statusSegments_[i].selected = (i == initialStatus);
        const int idx = i;
        impl->statusSegments_[i].onClick = [impl, idx]() {
            ApplyStatusSelection(impl, idx);
        };
    }

    impl->tagBox_.text = Utf8ToWide(f.tag);
    impl->tagBox_.placeholder = L"\u6807\u7B7E\uFF08\u7CBE\u786E\u5339\u914D\uFF09"; // 标签（精确匹配）
    impl->tagBox_.onChanged = [impl](const std::wstring& s) {
        impl->draft.tag = WideToUtf8(s);
    };

    impl->keywordBox_.text = Utf8ToWide(f.keyword);
    impl->keywordBox_.placeholder = L"\u5173\u952E\u8BCD\uFF08\u6807\u9898/\u5907\u6CE8\uFF09"; // 关键词（标题/备注）
    impl->keywordBox_.onChanged = [impl](const std::wstring& s) {
        impl->draft.keyword = WideToUtf8(s);
    };

    impl->includeDeletedBox_.checked = f.include_deleted;
    impl->includeDeletedBox_.label   = L"\u5305\u542B\u5DF2\u5220\u9664";    // 包含已删除
    impl->includeDeletedBox_.onToggle = [impl](bool v) {
        impl->draft.include_deleted = v;
    };

    impl->onlyDeletedBox_.checked = f.only_deleted;
    impl->onlyDeletedBox_.label   = L"\u4EC5\u663E\u793A\u5DF2\u5220\u9664"; // 仅显示已删除
    impl->onlyDeletedBox_.onToggle = [impl](bool v) {
        impl->draft.only_deleted = v;
        // Enforce macOS's `if synth.onlyDeleted { synth.includeDeleted = false }`
        // immediately in the UI so the user sees the visual coupling instead
        // of only learning it at commit time. The `enabled` flag is consumed
        // by CheckBox::Draw via alphaScale (see Controls.cpp) so the checkbox
        // visibly greys out.
        if (v) {
            impl->draft.include_deleted = false;
            impl->includeDeletedBox_.checked = false;
            impl->includeDeletedBox_.enabled = false;
        } else {
            impl->includeDeletedBox_.enabled = true;
        }
    };

    // Enforce the only_deleted > include_deleted precedence from the start
    // — same rule the macOS FilterEditor applies in commit().
    if (impl->onlyDeletedBox_.checked) {
        impl->includeDeletedBox_.checked = false;
        impl->includeDeletedBox_.enabled = false;
    }

    impl->pageSizeMinus_.text = L"\u2212"; // − (minus sign U+2212)
    impl->pageSizeMinus_.onClick = [impl]() { ApplyPageSizeDelta(impl, -10); };

    impl->pageSizePlus_.text  = L"+";
    impl->pageSizePlus_.onClick = [impl]() { ApplyPageSizeDelta(impl, 10); };

    impl->cancelButton_.text = L"\u53D6\u6D88"; // 取消
    impl->cancelButton_.onClick = [impl]() { DoCancelAndClose(impl); };

    impl->resetButton_.text  = L"\u91CD\u7F6E"; // 重置
    impl->resetButton_.onClick = [impl]() { DoReset(impl); };

    impl->saveButton_.text   = L"\u4FDD\u5B58"; // 保存
    impl->saveButton_.onClick = [impl]() { DoCommitAndClose(impl); };
}

// ---- Resize / render-target lifecycle -----------------------------------

void EnsureRenderTarget(Impl* impl) {
    if (impl->renderTarget) return;
    auto* app = GetApp();
    if (!app || !app->GetRenderer()) return;
    impl->renderTarget = app->GetRenderer()->CreateRenderTarget(impl->hwnd);
}

void DiscardRenderTarget(Impl* impl) {
    if (impl->renderTarget) {
        impl->renderTarget->Release();
        impl->renderTarget = nullptr;
    }
}

void OnResize(Impl* impl) {
    if (impl->renderTarget) {
        RECT rc;
        GetClientRect(impl->hwnd, &rc);
        D2D1_SIZE_U size = D2D1::SizeU(
            static_cast<UINT32>(rc.right - rc.left),
            static_cast<UINT32>(rc.bottom - rc.top));
        impl->renderTarget->Resize(size);
    }
    InvalidateRect(impl->hwnd, nullptr, FALSE);
}

// ---- Layout + paint ------------------------------------------------------

/// Compute all control rects and paint them. Layout mirrors the macOS
/// FilterEditor sections top-to-bottom:
///
///   ┌────────────────────────────────────────┐
///   │ [取消]   筛选   [重置] [保存]           │ ← header (40px)
///   ├────────────────────────────────────────┤
///   │ 状态                                    │
///   │   [全部] [未完成] [已完成]              │ ← segmented picker
///   │ 标签与关键词                            │
///   │   [_______________________]             │ ← tag
///   │   [_______________________]             │ ← keyword
///   │ 软删                                    │
///   │   [ ] 包含已删除                        │
///   │   [ ] 仅显示已删除                      │
///   │ 分页                                    │
///   │   [−]  每页 50 条   [+]                 │
///   └────────────────────────────────────────┘
void LayoutAndDraw(Impl* impl) {
    if (!impl->renderTarget) return;
    auto* app = GetApp();
    if (!app) return;
    IDWriteFactory* dw = app->GetRenderer()->GetDWriteFactory();
    float dpi = D2DRenderer::GetDpiScale(impl->hwnd);

    RECT rc;
    GetClientRect(impl->hwnd, &rc);
    float W = static_cast<float>(rc.right - rc.left);

    ID2D1RenderTarget* rt = impl->renderTarget;
    rt->BeginDraw();
    rt->Clear(Theme::WindowBackground());

    // ---- Header row (固定高度 40px @ 96-DPI) ----
    //
    // DPI-aware layout: every size / offset below is multiplied by
    // `dpi` at the use site so hit-test rects (Button::rect /
    // TextBox::rect read verbatim by HandleMouse) land on the same
    // physical pixel grid as the rendered geometry. The Win32
    // mouse messages (WM_LBUTTONDOWN etc.) deliver coords in
    // physical pixels under PerMonitorV2 awareness.
    const float kHeaderHeight = 40.0f * dpi;
    const float kBtn = Theme::kButtonHeight * dpi;
    const float kPad = Theme::kPadding * dpi;
    const float kPadL = Theme::kPaddingLarge * dpi;
    const float kBtnW = 64.0f * dpi;          // header button width
    const float kSegGap = 4.0f * dpi;         // segmented-picker gap
    const float kSectionHeaderH = 20.0f * dpi;
    const float kSectionHeaderStep = 22.0f * dpi;
    const float kCheckboxRowH = 22.0f * dpi;
    const float kStepperBtnW = 32.0f * dpi;

    // 取消 (left). Rects are recomputed per-frame to survive OS-driven
    // resize; callbacks were bound once in InitControls.
    impl->cancelButton_.rect = {kPadL, (kHeaderHeight - kBtn) * 0.5f, kBtnW, kBtn};
    impl->cancelButton_.Draw(rt, dw, dpi);

    // "筛选" title (center)
    {
        Label title;
        title.rect = {0.0f, 0.0f, W, kHeaderHeight};
        title.text = L"\u7B5B\u9009"; // 筛选
        title.fontSize = Theme::kFontSizeHeading;
        title.bold = true;
        title.color = Theme::TextPrimary();
        title.Draw(rt, dw, dpi);
    }

    // 保存 (right)
    impl->saveButton_.rect = {W - kPadL - kBtnW, (kHeaderHeight - kBtn) * 0.5f, kBtnW, kBtn};
    impl->saveButton_.Draw(rt, dw, dpi);

    // 重置 (left of 保存)
    impl->resetButton_.rect = {W - kPadL - kBtnW - kPad - kBtnW,
                               (kHeaderHeight - kBtn) * 0.5f, kBtnW, kBtn};
    impl->resetButton_.Draw(rt, dw, dpi);

    // Separator under header. Stroke width 1.0f*dpi so the line keeps
    // its visual weight at high DPI (otherwise it would be a hairline
    // that sub-pixel anti-aliases to near invisibility).
    {
        ID2D1SolidColorBrush* sep = nullptr;
        rt->CreateSolidColorBrush(Theme::Separator(), &sep);
        if (sep) {
            rt->DrawLine(D2D1::Point2F(0, kHeaderHeight),
                         D2D1::Point2F(W, kHeaderHeight), sep, 1.0f * dpi);
            sep->Release();
        }
    }

    // ---- Form body ----
    float y = kHeaderHeight + kPadL;
    const float xLabel = kPadL;
    const float contentW = W - 2 * kPadL;
    const float inputH = Theme::kInputHeight * dpi;

    auto drawSectionHeader = [&](const wchar_t* text) {
        Label h;
        h.rect = {xLabel, y, contentW, kSectionHeaderH};
        h.text = text;
        h.fontSize = Theme::kFontSizeSmall;
        h.bold = true;
        h.color = Theme::TextSecondary();
        h.Draw(rt, dw, dpi);
        y += kSectionHeaderStep;
    };

    // --- 状态 ---
    drawSectionHeader(L"\u72B6\u6001"); // 状态
    {
        // 3-way segmented picker. Each segment is a Button with its own
        // `selected` flag — that flag (added in Controls.h specifically for
        // this use-case) is orthogonal to the Normal/Hover/Pressed state
        // machine, so mouse interaction can freely transition without
        // clobbering the "currently chosen status" indicator. The flag
        // itself is updated via ApplyStatusSelection, never here.
        float segW = (contentW - 2 * kSegGap) / 3.0f;
        for (int i = 0; i < 3; ++i) {
            impl->statusSegments_[i].rect = {xLabel + i * (segW + kSegGap), y, segW, kBtn};
            impl->statusSegments_[i].Draw(rt, dw, dpi);
        }
        y += kBtn + kPadL;
    }

    // --- 标签与关键词 ---
    drawSectionHeader(L"\u6807\u7B7E\u4E0E\u5173\u952E\u8BCD"); // 标签与关键词
    impl->tagBox_.rect = {xLabel, y, contentW, inputH};
    impl->tagBox_.Draw(rt, dw, dpi);
    y += inputH + kPad;

    impl->keywordBox_.rect = {xLabel, y, contentW, inputH};
    impl->keywordBox_.Draw(rt, dw, dpi);
    y += inputH + kPadL;

    // --- 软删 ---
    drawSectionHeader(L"\u8F6F\u5220"); // 软删
    impl->includeDeletedBox_.rect = {xLabel, y, contentW, kCheckboxRowH};
    impl->includeDeletedBox_.Draw(rt, dw, dpi);
    y += kCheckboxRowH + kPad;

    impl->onlyDeletedBox_.rect = {xLabel, y, contentW, kCheckboxRowH};
    impl->onlyDeletedBox_.Draw(rt, dw, dpi);
    y += kCheckboxRowH + kPadL;

    // --- 分页 ---
    drawSectionHeader(L"\u5206\u9875"); // 分页
    {
        // Stepper layout:  [−]  每页 NN 条  [+]. Callbacks were bound once
        // in InitControls; only rect + enabled (clamp feedback) are
        // recomputed per-frame.
        impl->pageSizeMinus_.rect = {xLabel, y, kStepperBtnW, kBtn};
        impl->pageSizeMinus_.enabled = (impl->draft.page_size > 10);
        impl->pageSizeMinus_.Draw(rt, dw, dpi);

        impl->pageSizePlus_.rect = {xLabel + contentW - kStepperBtnW, y, kStepperBtnW, kBtn};
        impl->pageSizePlus_.enabled = (impl->draft.page_size < 200);
        impl->pageSizePlus_.Draw(rt, dw, dpi);

        // 每页 N 条 label (centered)
        wchar_t buf[32];
        _snwprintf_s(buf, 32, _TRUNCATE, L"\u6BCF\u9875 %d \u6761", impl->draft.page_size); // 每页 N 条
        impl->pageSizeLabelCache = buf;

        Label pgLabel;
        pgLabel.rect = {xLabel + kStepperBtnW, y, contentW - 2 * kStepperBtnW, kBtn};
        pgLabel.text = impl->pageSizeLabelCache;
        pgLabel.fontSize = Theme::kFontSizeBody;
        pgLabel.color = Theme::TextPrimary();
        pgLabel.Draw(rt, dw, dpi);
    }

    HRESULT hr = rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        DiscardRenderTarget(impl);
    }
}

void OnPaint(Impl* impl) {
    EnsureRenderTarget(impl);
    if (!impl->renderTarget) {
        ValidateRect(impl->hwnd, nullptr);
        return;
    }
    LayoutAndDraw(impl);
    ValidateRect(impl->hwnd, nullptr);
}

// ---- Input -----------------------------------------------------

void OnMouseMove(Impl* impl, int x, int y) {
    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);
    for (int i = 0; i < 3; ++i) {
        impl->statusSegments_[i].HandleMouse(WM_MOUSEMOVE, fx, fy);
    }
    impl->tagBox_.HandleMouse(WM_MOUSEMOVE, fx, fy);
    impl->keywordBox_.HandleMouse(WM_MOUSEMOVE, fx, fy);
    impl->includeDeletedBox_.HandleMouse(WM_MOUSEMOVE, fx, fy);
    impl->onlyDeletedBox_.HandleMouse(WM_MOUSEMOVE, fx, fy);
    impl->pageSizeMinus_.HandleMouse(WM_MOUSEMOVE, fx, fy);
    impl->pageSizePlus_.HandleMouse(WM_MOUSEMOVE, fx, fy);
    impl->cancelButton_.HandleMouse(WM_MOUSEMOVE, fx, fy);
    impl->resetButton_.HandleMouse(WM_MOUSEMOVE, fx, fy);
    impl->saveButton_.HandleMouse(WM_MOUSEMOVE, fx, fy);
    InvalidateRect(impl->hwnd, nullptr, FALSE);
}

void OnLButtonDown(Impl* impl, int x, int y) {
    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);

    // Clicking anywhere outside a TextBox takes focus away from it. We
    // defer the focus grab to post-hit-test because the hit on a TextBox
    // rect should give focus to *that* box, not merely blur the other.
    bool clickedTag = impl->tagBox_.rect.Contains(fx, fy);
    bool clickedKeyword = impl->keywordBox_.rect.Contains(fx, fy);

    if (clickedTag) {
        impl->tagBox_.SetFocus(true);
        impl->keywordBox_.SetFocus(false);
        impl->focusedBox = &impl->tagBox_;
    } else if (clickedKeyword) {
        impl->keywordBox_.SetFocus(true);
        impl->tagBox_.SetFocus(false);
        impl->focusedBox = &impl->keywordBox_;
    } else {
        impl->tagBox_.SetFocus(false);
        impl->keywordBox_.SetFocus(false);
        impl->focusedBox = nullptr;
    }

    for (int i = 0; i < 3; ++i) {
        impl->statusSegments_[i].HandleMouse(WM_LBUTTONDOWN, fx, fy);
    }
    impl->tagBox_.HandleMouse(WM_LBUTTONDOWN, fx, fy);
    impl->keywordBox_.HandleMouse(WM_LBUTTONDOWN, fx, fy);
    impl->includeDeletedBox_.HandleMouse(WM_LBUTTONDOWN, fx, fy);
    impl->onlyDeletedBox_.HandleMouse(WM_LBUTTONDOWN, fx, fy);
    impl->pageSizeMinus_.HandleMouse(WM_LBUTTONDOWN, fx, fy);
    impl->pageSizePlus_.HandleMouse(WM_LBUTTONDOWN, fx, fy);
    impl->cancelButton_.HandleMouse(WM_LBUTTONDOWN, fx, fy);
    impl->resetButton_.HandleMouse(WM_LBUTTONDOWN, fx, fy);
    impl->saveButton_.HandleMouse(WM_LBUTTONDOWN, fx, fy);
    InvalidateRect(impl->hwnd, nullptr, FALSE);
}

void OnLButtonUp(Impl* impl, int x, int y) {
    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);
    // Button / CheckBox fire their onClick/onToggle on LBUTTONUP (see
    // Controls.cpp). DoCommitAndClose / DoCancelAndClose / DoReset may
    // destroy the window — after those run, `impl` is freed (stack frame
    // goes away once ShowModal's GetMessage loop exits), so we MUST NOT
    // touch `impl` after the up-handler fires. Consequence: cancel / save
    // must be the LAST dispatch in the list, and we return immediately if
    // the window was destroyed.
    for (int i = 0; i < 3; ++i) {
        impl->statusSegments_[i].HandleMouse(WM_LBUTTONUP, fx, fy);
    }
    if (!IsWindow(impl->hwnd)) return;
    impl->tagBox_.HandleMouse(WM_LBUTTONUP, fx, fy);
    impl->keywordBox_.HandleMouse(WM_LBUTTONUP, fx, fy);
    impl->includeDeletedBox_.HandleMouse(WM_LBUTTONUP, fx, fy);
    impl->onlyDeletedBox_.HandleMouse(WM_LBUTTONUP, fx, fy);
    impl->pageSizeMinus_.HandleMouse(WM_LBUTTONUP, fx, fy);
    impl->pageSizePlus_.HandleMouse(WM_LBUTTONUP, fx, fy);
    if (!IsWindow(impl->hwnd)) return;
    // Header buttons last — they may destroy the window.
    impl->resetButton_.HandleMouse(WM_LBUTTONUP, fx, fy);
    if (!IsWindow(impl->hwnd)) return;
    impl->cancelButton_.HandleMouse(WM_LBUTTONUP, fx, fy);
    if (!IsWindow(impl->hwnd)) return;
    impl->saveButton_.HandleMouse(WM_LBUTTONUP, fx, fy);
    if (!IsWindow(impl->hwnd)) return;
    InvalidateRect(impl->hwnd, nullptr, FALSE);
}

void OnChar(Impl* impl, wchar_t ch) {
    if (!impl->focusedBox) return;
    if (impl->focusedBox->HandleChar(ch)) {
        // Text changed — propagate to draft via onChanged (called by TextBox
        // internally on mutation).
        InvalidateRect(impl->hwnd, nullptr, FALSE);
    }
}

void OnKeyDown(Impl* impl, WPARAM vk, LPARAM lParam) {
    // ESC closes as cancel — matches macOS `.keyboardShortcut(.cancelAction)`
    // on the 取消 button. Enter commits iff a TextBox currently has focus
    // (otherwise Enter inside an empty form shouldn't trigger 保存; use
    // the 保存 button instead).
    if (vk == VK_ESCAPE) {
        DoCancelAndClose(impl);
        return;
    }
    if (vk == VK_RETURN && impl->focusedBox) {
        DoCommitAndClose(impl);
        return;
    }

    if (impl->focusedBox) {
        if (impl->focusedBox->HandleKey(WM_KEYDOWN, vk, lParam)) {
            InvalidateRect(impl->hwnd, nullptr, FALSE);
        }
    }
}

// ---- Button actions ------------------------------------------------------

void ApplyStatusSelection(Impl* impl, int idx) {
    if (idx < 0 || idx > 2) return;
    impl->draft.status = kStatusValues[idx];
    // Sync persistent selection flag across all three segments so the
    // chosen one shows the Pressed background regardless of hover state.
    for (int i = 0; i < 3; ++i) {
        impl->statusSegments_[i].selected = (i == idx);
    }
    InvalidateRect(impl->hwnd, nullptr, FALSE);
}

void ApplyPageSizeDelta(Impl* impl, int delta) {
    // Range 10-200 step 10 — identical to macOS `Stepper(... in: 10...200, step: 10)`.
    int next = impl->draft.page_size + delta;
    if (next < 10) next = 10;
    if (next > 200) next = 200;
    impl->draft.page_size = next;
    InvalidateRect(impl->hwnd, nullptr, FALSE);
}

void DoReset(Impl* impl) {
    // Fresh filter — same as macOS `draft = TodoFilter()`. page_size falls
    // back to the default 50 via Filter's in-class member initialisers
    // (see models/Filter.h).
    impl->draft = models::Filter{};

    // Sync TextBox visuals to the reset draft. Use SetFocus(false) instead
    // of poking internal fields so the helper's own invariants (focused
    // flag + selection cleared) stay consistent.
    impl->tagBox_.text.clear();
    impl->tagBox_.cursorPos = 0;
    impl->tagBox_.SetFocus(false);
    impl->keywordBox_.text.clear();
    impl->keywordBox_.cursorPos = 0;
    impl->keywordBox_.SetFocus(false);
    impl->focusedBox = nullptr;

    // Sync CheckBox visuals.
    impl->includeDeletedBox_.checked = false;
    impl->includeDeletedBox_.enabled = true;
    impl->onlyDeletedBox_.checked = false;

    // Sync status segmented picker selection. Default status is "" which
    // maps to index 0 (全部) via StatusToIndex.
    for (int i = 0; i < 3; ++i) {
        impl->statusSegments_[i].selected = (i == 0);
    }

    InvalidateRect(impl->hwnd, nullptr, FALSE);
}

void DoCommitAndClose(Impl* impl) {
    // Mirror macOS commit(): enforce only_deleted > include_deleted one last
    // time in case some transient UI state missed the toggle coupling.
    if (impl->draft.only_deleted) {
        impl->draft.include_deleted = false;
    }
    *impl->out = impl->draft;
    impl->committed = true;
    DestroyWindow(impl->hwnd);
}

void DoCancelAndClose(Impl* impl) {
    // `out` deliberately untouched — caller sees the original filter.
    impl->committed = false;
    DestroyWindow(impl->hwnd);
}

} // namespace

} // namespace stickytodo::ui
