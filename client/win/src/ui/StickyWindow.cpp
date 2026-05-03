#include "ui/StickyWindow.h"
#include "ui/D2DRenderer.h"
#include "ui/Theme.h"
#include "ui/Preferences.h"
#include "ui/FilterEditor.h"
#include "App.h"
#include "core/AppState.h"
#include "codec/StickyCodec.h"

#include <windowsx.h>
#include <algorithm>

#include "models/Filter.h"

namespace stickytodo::ui {

namespace {

/// Widths of the three per-row action buttons shown on hover.
// Action-button icon slot size / gap at 96-DPI baseline. Multiplied by
// `dpi` at use sites; see ComputeRowLayout for the full scaling chain.
constexpr float kActionIconSize = 22.0f;
constexpr float kActionGap = 4.0f;
constexpr int   kActionCount = 3; // check / edit / delete (or restore)

/// Compose the row rectangle in scroll-content coordinates.
/// rowTop is y-offset from the top of the scrollable content area (excluding titleBar + filterBar).
///
/// All fields are in **physical pixels** — callers must pass a
/// physical-pixel `contentWidth` and `rowTop`, and the `dpi` used to
/// scale the Theme constants must match the `dpi` the frame is
/// being rendered with. This keeps draw-time and hit-test-time
/// geometry in lock-step.
struct RowLayout {
    float rowTop;     // y in content coords (physical pixels)
    float rowHeight;
    float priorityBarRight;
    float checkboxLeft;
    float textLeft;
    float textRight;  // text region right edge (before action buttons)
    float actionsLeft;
    // Echo the DPI used to build this layout so callers hit-testing
    // against these fields can reuse it without calling GetDpiScale
    // themselves (e.g. HitTestRow below scales kActionIconSize).
    float dpi;
    float checkboxSize;   // Theme::kCheckboxSize * dpi
    float actionIconSize; // kActionIconSize * dpi
    float actionGap;      // kActionGap * dpi
};

static RowLayout ComputeRowLayout(float contentWidth, float rowTop, float dpi) {
    RowLayout L{};
    L.rowTop = rowTop;
    L.dpi = dpi;
    L.rowHeight = Theme::kTodoRowHeight * dpi;
    L.priorityBarRight = Theme::kPriorityBarWidth * dpi;
    L.checkboxSize = Theme::kCheckboxSize * dpi;
    L.actionIconSize = kActionIconSize * dpi;
    L.actionGap = kActionGap * dpi;
    L.checkboxLeft = (Theme::kPadding + Theme::kPriorityBarWidth) * dpi;
    L.textLeft = L.checkboxLeft + L.checkboxSize + 8.0f * dpi;

    float actionsWidth = kActionCount * L.actionIconSize + (kActionCount - 1) * L.actionGap;
    L.actionsLeft = contentWidth - Theme::kPadding * dpi - Theme::kScrollbarWidth * dpi - actionsWidth;
    L.textRight = L.actionsLeft - 6.0f * dpi;
    return L;
}

/// Convert std::string (UTF-8 assumed) to std::wstring via MultiByteToWideChar.
static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int sz = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(sz, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), sz);
    return out;
}

static std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return "";
    int sz = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(sz, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), sz, nullptr, nullptr);
    return out;
}

static std::wstring BuildFilterSummary(const models::Filter& f) {
    if (f.IsEmpty()) return L"\u2630 \u5168\u90E8"; // ☰ 全部
    std::wstring out = L"\u2630 ";
    bool first = true;
    auto sep = [&]() {
        if (!first) out += L" \u00B7 ";
        first = false;
    };
    if (f.status == "pending")      { sep(); out += L"\u672A\u5B8C\u6210"; }    // 未完成
    else if (f.status == "done")    { sep(); out += L"\u5DF2\u5B8C\u6210"; }    // 已完成
    if (!f.tag.empty())             { sep(); out += L"#" + Utf8ToWide(f.tag); }
    if (!f.keyword.empty())         { sep(); out += L"\"" + Utf8ToWide(f.keyword) + L"\""; }
    // Prefix "≤ " (U+2264 plus space) before the due_before timestamp.
    if (!f.due_before.empty())      { sep(); out += L"\u2264 " + Utf8ToWide(f.due_before); }
    if (f.include_deleted)          { sep(); out += L"\u542B\u5DF2\u5220"; }    // 含已删
    if (f.only_deleted)             { sep(); out += L"\u4EC5\u5DF2\u5220"; }    // 仅已删
    return out;
}

} // namespace

bool StickyWindow::classRegistered_ = false;

StickyWindow::StickyWindow(HINSTANCE hInstance, const std::string& stickyId)
    : hInstance_(hInstance), stickyId_(stickyId)
{
    // alive_ is initialised to std::make_shared<std::atomic<bool>>(true)
    // by the default member initialiser in StickyWindow.h. No explicit
    // initialisation needed here — but the async callbacks in this file
    // depend on it being non-null and true before any mutation call
    // site runs, which is guaranteed because member-init runs before
    // the body of this ctor, and no HTTP call is fired from the ctor.
}

StickyWindow::~StickyWindow() {
    // Flip the liveness flag BEFORE any other teardown. In-flight HTTP
    // worker threads may be about to post their completion lambdas to
    // the tray HWND; once this store() runs, every such lambda that
    // value-captured `alive_` will see false on the UI thread and
    // early-return without touching `this`. The shared_ptr keeps the
    // atomic's backing memory alive as long as at least one callback
    // still holds a ref, so the load() on the callback side is safe
    // even after ~StickyWindow finishes and our own alive_ member is
    // destroyed.
    //
    // We use memory_order_seq_cst (the default) rather than release
    // because ordering matters in BOTH directions: the store must
    // happen-before any callback load on another thread (release
    // would suffice), AND we rely on the store being visible to the
    // UI thread immediately so the very next pumped message sees
    // false (sequential consistency makes this trivial to reason
    // about; the perf cost is negligible for a once-per-destructor
    // store).
    if (alive_) {
        alive_->store(false);
    }

    DiscardRenderTarget();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

bool StickyWindow::Create() {
    if (!classRegistered_) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hInstance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc)) return false;
        classRegistered_ = true;
    }

    auto* app = GetApp();
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    int w = Theme::kStickyDefaultWidth, h = Theme::kStickyDefaultHeight;

    if (app && app->GetState()) {
        auto frame = app->GetState()->GetFrameStore()->Load(stickyId_);
        if (frame.has_value()) {
            x = static_cast<int>(frame->x);
            y = static_cast<int>(frame->y);
            w = static_cast<int>(frame->width);
            h = static_cast<int>(frame->height);
        }
    }

    hwnd_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kClassName,
        L"StickyTodo",
        WS_POPUP | WS_THICKFRAME,
        x, y, w, h,
        nullptr, nullptr, hInstance_, this
    );

    if (!hwnd_) return false;

    CreateRenderTarget();
    LoadData();

    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd_);

    return true;
}

void StickyWindow::BringToFront() {
    if (hwnd_) {
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    }
}

void StickyWindow::Refresh() {
    LoadData();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void StickyWindow::CreateRenderTarget() {
    if (renderTarget_) return;
    auto* app = GetApp();
    if (app && app->GetRenderer()) {
        renderTarget_ = app->GetRenderer()->CreateRenderTarget(hwnd_);
    }
}

void StickyWindow::DiscardRenderTarget() {
    if (renderTarget_) {
        renderTarget_->Release();
        renderTarget_ = nullptr;
    }
}

void StickyWindow::LoadData() {
    auto* app = GetApp();
    if (!app || !app->GetState()) return;

    // AppState::GetStickies() returns by value (see AppState.h) — it
    // must, because the authoritative vector is guarded by a mutex and
    // the returned snapshot is the caller's safe copy. Bind by value,
    // not by `const auto&`: the latter only works by virtue of C++'s
    // temporary-lifetime-extension rule and silently obscures the fact
    // that we're holding a full vector copy on the stack.
    auto stickies = app->GetState()->GetStickies();
    for (const auto& s : stickies) {
        if (s.id == stickyId_) {
            stickyNote_ = s;
            break;
        }
    }

    filter_ = codec::StickyCodec::JsonToFilter(stickyNote_.filter);

    // Any in-flight draft/edit state must not outlive a data reload.
    CancelDraft();
    CancelTitleEdit();
    hoveredRowIndex_ = -1;

    // Pessimistic read: flag loading, kick off async list, the
    // callback swaps todos_ in on the UI thread. Keeping the previous
    // todos_ in place (not cleared) avoids flicker — on first load
    // todos_ is already empty so the "Loading..." placeholder in
    // DrawTodoList will show; on refresh the stale list stays visible
    // until the new one lands.
    // Bump generation token BEFORE firing the async. Any earlier
    // in-flight LoadData's callback will see its captured myGen <
    // loadDataGeneration_ when it lands and silently drop — this
    // prevents a stale filter's result from overwriting the current
    // filter's todos_ when two LoadData() calls overlap (the
    // ShowFilterEditor rollback path is the prime offender; see the
    // field doc on loadDataGeneration_).
    uint64_t myGen = ++loadDataGeneration_;
    todosLoading_ = true;
    app->GetState()->GetHttp()->AsyncListTodos(filter_,
        [this, alive = alive_, myGen]
        (std::optional<core::TodoListResult> result) {
            // Qualifier is `core::TodoListResult` (namespace member, not
            // class-nested) to match HttpClient.h's declaration:
            //
            //   namespace stickytodo::core {
            //     struct TodoListResult { ... };   // ← top-level in core
            //     class HttpClient {
            //       using TodoListCallback =
            //         std::function<void(std::optional<TodoListResult>)>;
            //       // ↑ inside class body, name-lookup finds the
            //       //   enclosing namespace's TodoListResult, so the
            //       //   TodoListCallback's real type is
            //       //   std::function<void(
            //       //     std::optional<stickytodo::core::TodoListResult>)>
            //     };
            //   }
            //
            // This file is in `namespace stickytodo::ui`, so to form the
            // same type we must qualify as `core::TodoListResult` (ADL
            // / unqualified lookup does NOT reach sibling namespace
            // `core` from inside `ui`). `models::Todo` in the other
            // callbacks works for the same reason: `models` is also a
            // sibling of `ui` under `stickytodo`, and we always qualify
            // it (never write bare `Todo`).
            // Liveness guard — the StickyWindow may have been destroyed
            // (user closed it / WS sticky.deleted / App shutdown)
            // between AsyncListTodos firing on the worker thread and
            // this callback landing on the UI thread. alive_ is a
            // shared_ptr<atomic<bool>>; the worker thread's captured
            // copy keeps the atomic alive even after ~StickyWindow, so
            // this load() is safe to call on a dead object. All the
            // other captures in this file follow the same pattern.
            if (!alive->load()) return;
            // Secondary defense: hwnd_ can be null in the brief
            // window between WM_DESTROY handler running (which nulls
            // hwnd_) and ~StickyWindow running (which flips alive_).
            // Both guards redundantly protect against that window.
            if (!hwnd_) return;
            // Generation guard: if a newer LoadData has already
            // started (loadDataGeneration_ advanced), this callback
            // is stale — drop its result but leave todosLoading_
            // alone (the newer LoadData already set it to true and
            // its own callback will flip it back).
            if (myGen != loadDataGeneration_) return;
            todosLoading_ = false;
            if (result.has_value()) {
                todos_ = std::move(result->items);
            }
            // On failure: keep whatever todos_ had before — the
            // list stays stale but visible, matching the "don't
            // blow away the UI on a transient network error" rule.
            InvalidateRect(hwnd_, nullptr, FALSE);
        });
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void StickyWindow::RefreshLayoutMetrics() {
    // Recompute physical-pixel title/filter bar heights from the
    // current monitor DPI. Called at the top of OnPaint so every
    // hit-test that runs between this paint and the next reads the
    // same numbers the frame was rendered with. Also safe to call
    // more often — it's just two multiplies.
    float dpi = hwnd_ ? D2DRenderer::GetDpiScale(hwnd_) : 1.0f;
    titleBarHeight_ = 32.0f * dpi;
    filterBarHeight_ = 28.0f * dpi;
}

void StickyWindow::OnPaint() {
    if (!renderTarget_) {
        CreateRenderTarget();
        if (!renderTarget_) return;
    }

    auto* app = GetApp();
    if (!app || !app->GetRenderer()) return;

    IDWriteFactory* dw = app->GetRenderer()->GetDWriteFactory();
    float dpi = D2DRenderer::GetDpiScale(hwnd_);
    // Refresh DPI-aware layout cache so title/filter bar hit-tests
    // that run between this paint and the next see values matching
    // what we're about to draw.
    RefreshLayoutMetrics();

    codec::RgbaColor bgColor = codec::StickyCodec::ParseBgColor(stickyNote_.bg_color);
    D2D1_COLOR_F d2dBgColor = D2D1::ColorF(
        static_cast<float>(bgColor.red),
        static_cast<float>(bgColor.green),
        static_cast<float>(bgColor.blue),
        static_cast<float>(bgColor.alpha)
    );

    RECT rc;
    GetClientRect(hwnd_, &rc);
    float width = static_cast<float>(rc.right - rc.left);
    float height = static_cast<float>(rc.bottom - rc.top);

    renderTarget_->BeginDraw();
    renderTarget_->Clear(d2dBgColor);

    DrawTitleBar(renderTarget_, dw, dpi);
    DrawTodoList(renderTarget_, dw, dpi);
    DrawFilterButton(renderTarget_, dw, dpi, width, height);

    HRESULT hr = renderTarget_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        DiscardRenderTarget();
    }
}

void StickyWindow::OnResize() {
    if (renderTarget_) {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);
        renderTarget_->Resize(size);
    }
    SaveFramePosition();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void StickyWindow::OnMove() {
    SaveFramePosition();
}

void StickyWindow::SaveFramePosition() {
    if (!hwnd_) return;
    auto* app = GetApp();
    if (!app || !app->GetState()) return;

    RECT rc;
    GetWindowRect(hwnd_, &rc);
    core::FrameRect frame;
    frame.x = static_cast<double>(rc.left);
    frame.y = static_cast<double>(rc.top);
    frame.width = static_cast<double>(rc.right - rc.left);
    frame.height = static_cast<double>(rc.bottom - rc.top);
    app->GetState()->GetFrameStore()->Save(stickyId_, frame);
}

LRESULT StickyWindow::OnNcHitTest(int x, int y) {
    POINT pt = {x, y};
    ScreenToClient(hwnd_, &pt);

    RECT rc;
    GetClientRect(hwnd_, &rc);

    // Resize grip margin — scale by dpi so the corner hit target
    // matches the visual weight of the window chrome at high DPI.
    float dpi = D2DRenderer::GetDpiScale(hwnd_);
    int margin = static_cast<int>(16.0f * dpi);
    if (pt.x >= rc.right - margin && pt.y >= rc.bottom - margin) {
        return HTBOTTOMRIGHT;
    }

    // titleBarHeight_ is the physical-pixel value refreshed at the
    // top of every OnPaint via RefreshLayoutMetrics(). Reading the
    // cache here (rather than recomputing) guarantees this hit-test
    // matches the geometry DrawTitleBar used for the last frame.
    if (pt.y < static_cast<LONG>(titleBarHeight_)) {
        float fx = static_cast<float>(pt.x);
        float fy = static_cast<float>(pt.y);
        // Any region covered by a title-bar button must report HTCLIENT so
        // Win32 delivers regular WM_LBUTTONDOWN to us instead of treating the
        // click as a window drag. trashButton_ is only interactive while the
        // title bar is hovered, but Win32 runs OnNcHitTest *before* the hover
        // flag is updated for the current message, so we check its rect
        // unconditionally — the rect is stable across frames (DrawTitleBar
        // rewrites it every paint but always to the same screen coordinates
        // when titleBarHovered_ is true; when false the rect is zero-width
        // and Contains() naturally returns false).
        if (closeButton_.rect.Contains(fx, fy)
            || settingsButton_.rect.Contains(fx, fy)
            || plusButton_.rect.Contains(fx, fy)
            || trashButton_.rect.Contains(fx, fy)) {
            return HTCLIENT;
        }
        return HTCAPTION;
    }

    return HTCLIENT;
}

// ---------------------------------------------------------------------------
// Title bar + buttons
// ---------------------------------------------------------------------------

void StickyWindow::DrawTitleBar(ID2D1RenderTarget* rt, IDWriteFactory* dw, float dpi) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    float width = static_cast<float>(rc.right - rc.left);
    // DPI-aware constants. titleBarHeight_ is already physical pixels
    // (refreshed by RefreshLayoutMetrics in OnPaint); every other
    // size/offset below comes from a 96-DPI baseline literal × dpi.
    const float btnSize = 24.0f * dpi;
    const float btnTop = 4.0f * dpi;
    const float btnGap = 4.0f * dpi;   // 28px slot = 24 btn + 4 gap at 96-DPI
    const float btnSlot = btnSize + btnGap;

    // Darkened title bar overlay
    ID2D1SolidColorBrush* brush = nullptr;
    rt->CreateSolidColorBrush(Theme::StickyTitleBar(), &brush);
    if (brush) {
        D2D1_RECT_F titleRect = D2D1::RectF(0, 0, width, titleBarHeight_);
        rt->FillRectangle(titleRect, brush);
        brush->Release();
    }

    // Title label. Reserve enough horizontal space for the four right-aligned
    // buttons: close (×) + settings (⚙) + plus (+) + trash (🗑). Each button
    // occupies btnSlot pixels (including its gap); reserve 4 slots + trailing
    // padding so the title never overlaps a button even at extreme DPI.
    // The +12*dpi gives the visual breathing room between title text and
    // the first button.
    const float buttonsReserved = 4 * btnSlot + 12.0f * dpi;
    Label titleLabel;
    titleLabel.rect = {Theme::kPadding * dpi, 0,
                       width - buttonsReserved, titleBarHeight_};
    titleLabel.text = Utf8ToWide(stickyNote_.title);
    titleLabel.fontSize = Theme::kFontSizeTitle;
    titleLabel.bold = true;
    titleLabel.color = Theme::TextPrimary();
    titleLabel.Draw(rt, dw, dpi);

    // Button rects: right-aligned, spaced by btnSlot. close > settings > plus > trash
    // (trash is drawn conditionally below, but its slot index stays reserved in the
    // x math so the other three don't shift when hover state flips).
    // The "+8*dpi" in closeX is the right-edge breathing room before the
    // window border (≈ 8px at 96-DPI).
    const float closeX    = width - btnSize - 8.0f * dpi;
    const float settingsX = closeX - btnSlot;
    const float plusX     = settingsX - btnSlot;
    const float trashX    = plusX - btnSlot;

    // Close button (top-right). "Close" here means closing the window locally
    // — it does NOT delete the sticky from the server. The trash button to
    // the left is the destructive action (DELETE /api/sticky-notes/:id).
    closeButton_.rect = {closeX, btnTop, btnSize, btnSize};
    closeButton_.text = L"\u00D7"; // ×
    closeButton_.onClick = [this]() {
        if (auto* app = GetApp()) {
            app->CloseStickyWindow(stickyId_);
        }
    };
    closeButton_.Draw(rt, dw, dpi);

    // Settings button
    settingsButton_.rect = {settingsX, btnTop, btnSize, btnSize};
    settingsButton_.text = L"\u2699"; // ⚙
    settingsButton_.onClick = [this]() {
        if (auto* app = GetApp()) {
            app->ShowSettings();
        }
    };
    settingsButton_.Draw(rt, dw, dpi);

    // "+" new-todo button (left of settings)
    plusButton_.rect = {plusX, btnTop, btnSize, btnSize};
    plusButton_.text = L"\u002B"; // +
    plusButton_.onClick = [this]() { BeginDraft(); };
    plusButton_.Draw(rt, dw, dpi);

    // Trash (delete sticky) — mirrors macOS StickyView's hover-fade trash
    // button: only drawn and interactive while the cursor is over the title
    // bar, so it's not a persistent visual destroyer. When hidden, we zero
    // its rect so OnNcHitTest / OnLButtonDown naturally skip it.
    if (titleBarHovered_) {
        trashButton_.rect = {width - 116.0f, 4.0f, 24.0f, 24.0f};
        trashButton_.text = L"\U0001F5D1"; // 🗑 (U+1F5D1 WASTEBASKET) — Segoe
                                           // UI Emoji on Windows 10+ renders
                                           // it monochrome at this size; on
                                           // older systems DirectWrite falls
                                           // back to tofu, but the adjacent
                                           // hover-only UX already signals
                                           // destructive intent.
        // Disable while a DELETE /api/sticky-notes/:id is in flight so
        // the user can't retry-bomb the request. Button::Draw fades
        // the fill/text alpha by 0.5 when !enabled, and
        // Button::HandleMouse short-circuits at its top when !enabled,
        // so both the visual and the click paths are covered by this
        // single assignment.
        trashButton_.enabled = !stickyDeleting_;
        trashButton_.onClick = [this]() { DoDeleteSticky(); };
        trashButton_.Draw(rt, dw, dpi);
    } else {
        trashButton_.rect = {0.0f, 0.0f, 0.0f, 0.0f};
    }
}

// ---------------------------------------------------------------------------
// Draft row (inline new-todo input, above list)
// ---------------------------------------------------------------------------

void StickyWindow::DrawDraftRow(ID2D1RenderTarget* rt, IDWriteFactory* dw, float dpi, float width, float rowY) {
    // DPI note: `rowY` is already in physical pixels (caller computed
    // it from DrawTodoList which uses titleBarHeight_ cache + rowHeight*dpi).
    // All Theme::k* and literal px values below are 96-DPI baselines
    // multiplied by `dpi` here at the use site.
    const float padX = Theme::kPadding * dpi;
    const float priBarW = Theme::kPriorityBarWidth * dpi;
    const float cbSize = Theme::kCheckboxSize * dpi;
    const float rowH = Theme::kTodoRowHeight * dpi;
    const float scrollbarW = Theme::kScrollbarWidth * dpi;
    const float inputH = Theme::kInputHeight * dpi;

    // Left: placeholder circle (unclickable; symmetry with TodoRow checkbox).
    ID2D1SolidColorBrush* borderBrush = nullptr;
    rt->CreateSolidColorBrush(Theme::CheckboxBorder(), &borderBrush);
    if (borderBrush) {
        float cx = padX + priBarW + cbSize * 0.5f;
        float cy = rowY + rowH * 0.5f;
        D2D1_ELLIPSE circle = D2D1::Ellipse(D2D1::Point2F(cx, cy), cbSize * 0.45f, cbSize * 0.45f);
        rt->DrawEllipse(circle, borderBrush, 1.5f * dpi);
        borderBrush->Release();
    }

    // TextBox occupying the rest of the row.
    float textLeft = padX + priBarW + cbSize + 8.0f * dpi;
    float tbY = rowY + (rowH - inputH) * 0.5f;
    draftBox_.rect = {textLeft, tbY, width - textLeft - padX - scrollbarW, inputH};
    if (draftBox_.placeholder.empty()) {
        // 待办内容，回车保存 / Esc 取消
        draftBox_.placeholder = L"\u5F85\u529E\u5185\u5BB9\uFF0C\u56DE\u8F66\u4FDD\u5B58 / Esc \u53D6\u6D88";
    }
    draftBox_.Draw(rt, dw, dpi);
}

// ---------------------------------------------------------------------------
// TODO list + per-row hover actions
// ---------------------------------------------------------------------------

void StickyWindow::DrawTodoList(ID2D1RenderTarget* rt, IDWriteFactory* dw, float dpi) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    float width = static_cast<float>(rc.right - rc.left);
    float height = static_cast<float>(rc.bottom - rc.top);

    // titleBarHeight_ and FilterBarHeight() already return physical
    // pixels (populated by RefreshLayoutMetrics in OnPaint). All
    // per-row sizes derived from Theme::k* are still 96-DPI baselines
    // and must be scaled by dpi explicitly below.
    float listTop = titleBarHeight_;
    float listHeight = height - titleBarHeight_ - FilterBarHeight();
    if (listHeight < 0) listHeight = 0;

    scrollView_.rect = {0, listTop, width, listHeight};
    // Propagate current DPI so ScrollView's internal constants
    // (scrollbar width, thumb min-height, wheel step) scale with the
    // frame. Set BEFORE contentHeight / scrollOffset are used by
    // HandleMouse/HandleWheel/DrawScrollbar — i.e. at the top of
    // every paint so all reads see the same value.
    scrollView_.dpi = dpi;

    const float rowHeight = Theme::kTodoRowHeight * dpi;

    // Content height accounts for optional draft row sitting above the todos.
    float draftRowH = drafting_ ? rowHeight : 0.0f;
    scrollView_.contentHeight = draftRowH + static_cast<float>(todos_.size()) * rowHeight;

    scrollView_.BeginContent(rt);

    // y is relative to content origin (== scrollView_.rect.y)
    float y = listTop;

    if (drafting_) {
        DrawDraftRow(rt, dw, dpi, width, y);
        y += rowHeight;
    }

    for (size_t i = 0; i < todos_.size(); ++i) {
        const auto& todo = todos_[i];
        float rowY = y + static_cast<float>(i) * rowHeight;
        RowLayout L = ComputeRowLayout(width, rowY, dpi);

        // Priority color bar
        if (todo.priority > 0) {
            ID2D1SolidColorBrush* priBrush = nullptr;
            rt->CreateSolidColorBrush(Theme::PriorityColor(todo.priority), &priBrush);
            if (priBrush) {
                D2D1_RECT_F bar = D2D1::RectF(0, rowY, L.priorityBarRight, rowY + L.rowHeight);
                rt->FillRectangle(bar, priBrush);
                priBrush->Release();
            }
        }

        // Checkbox (soft-deleted rows render a disabled square).
        if (!todo.IsDeleted()) {
            CheckBox cb;
            cb.rect = {L.checkboxLeft, rowY + (L.rowHeight - L.checkboxSize) / 2.0f,
                       L.checkboxSize, L.checkboxSize};
            cb.checked = todo.IsDone();
            cb.Draw(rt, dw, dpi);
        } else {
            // Draw a faint dashed marker for deleted rows.
            ID2D1SolidColorBrush* brush = nullptr;
            rt->CreateSolidColorBrush(Theme::TextPlaceholder(), &brush);
            if (brush) {
                D2D1_RECT_F box = D2D1::RectF(
                    L.checkboxLeft, rowY + (L.rowHeight - L.checkboxSize) / 2.0f,
                    L.checkboxLeft + L.checkboxSize,
                    rowY + (L.rowHeight + L.checkboxSize) / 2.0f);
                rt->DrawRectangle(box, brush, 1.0f * dpi);
                brush->Release();
            }
        }

        // Title: TextBox when editing this row, otherwise Label.
        if (editingRowIndex_ == static_cast<int>(i)) {
            float inputH = Theme::kInputHeight * dpi;
            float tbY = rowY + (L.rowHeight - inputH) * 0.5f;
            editBox_.rect = {L.textLeft, tbY, L.textRight - L.textLeft, inputH};
            editBox_.Draw(rt, dw, dpi);
        } else {
            Label titleLabel;
            titleLabel.rect = {L.textLeft, rowY, L.textRight - L.textLeft, L.rowHeight};
            titleLabel.text = Utf8ToWide(todo.title);
            titleLabel.fontSize = Theme::kFontSizeBody;
            titleLabel.color = (todo.IsDone() || todo.IsDeleted()) ? Theme::TextSecondary()
                                                                    : Theme::TextPrimary();
            titleLabel.Draw(rt, dw, dpi);
        }

        // Hover actions: three icon slots on the right. All sizes come
        // from the RowLayout (which already scaled them by dpi) so the
        // hit-test math in HitTestRow stays in sync automatically.
        if (static_cast<int>(i) == hoveredRowIndex_ && editingRowIndex_ != static_cast<int>(i)) {
            float ax = L.actionsLeft;
            float ay = rowY + (L.rowHeight - L.actionIconSize) * 0.5f;

            // Slot 1: complete / reopen
            if (!todo.IsDeleted()) {
                Label l;
                l.rect = {ax, ay, L.actionIconSize, L.actionIconSize};
                l.text = todo.IsDone() ? L"\u21BA" : L"\u2713"; // ↺ / ✓
                l.fontSize = Theme::kFontSizeBody;
                l.color = Theme::TextSecondary();
                l.Draw(rt, dw, dpi);
            }
            ax += L.actionIconSize + L.actionGap;

            // Slot 2: edit title. Prefer U+270F (PENCIL) over U+270E (LOWER
            // RIGHT PENCIL) — the latter is not covered by Segoe UI on older
            // Windows 10 builds and falls back to tofu. U+270F is carried by
            // Segoe UI Symbol since Vista, so we get consistent glyphs.
            if (!todo.IsDeleted()) {
                Label l;
                l.rect = {ax, ay, L.actionIconSize, L.actionIconSize};
                l.text = L"\u270F"; // ✏
                l.fontSize = Theme::kFontSizeBody;
                l.color = Theme::TextSecondary();
                l.Draw(rt, dw, dpi);
            }
            ax += L.actionIconSize + L.actionGap;

            // Slot 3: delete / restore. Use ✕ (U+2716) instead of the 🗑 emoji because
            // Segoe UI does not ship emoji glyphs — DirectWrite would fall back and
            // render tofu. ✕ is monochrome and always available in Segoe UI Symbol.
            Label l;
            l.rect = {ax, ay, L.actionIconSize, L.actionIconSize};
            l.text = todo.IsDeleted() ? L"\u21B6" : L"\u2716"; // ↶ / ✖
            l.fontSize = Theme::kFontSizeBody;
            l.color = Theme::TextSecondary();
            l.Draw(rt, dw, dpi);
        }

        // Separator line
        ID2D1SolidColorBrush* sepBrush = nullptr;
        rt->CreateSolidColorBrush(Theme::Separator(), &sepBrush);
        if (sepBrush) {
            float sepY = rowY + L.rowHeight - 0.5f * dpi;
            float padX = Theme::kPadding * dpi;
            rt->DrawLine(D2D1::Point2F(padX, sepY),
                          D2D1::Point2F(width - padX, sepY),
                          sepBrush, 0.5f * dpi);
            sepBrush->Release();
        }
    }

    // Empty-state / loading placeholder. Drawn centered in the
    // scroll-content area when there are no rows to display. Two
    // distinct states:
    //   • todosLoading_          → "加载中…" (initial load only —
    //                               todosLoading_ is also true during
    //                               refetches, but in that case todos_
    //                               usually has the previous list so
    //                               this branch doesn't fire and the
    //                               stale rows stay visible, avoiding
    //                               flicker)
    //   • !todosLoading_ & empty → "暂无待办" (server returned an
    //                               empty list for the current filter;
    //                               without this the sticky would be
    //                               completely blank which looks like
    //                               a render bug)
    //
    // Drafting rows don't count as "empty" — when the draft row is
    // visible the sticky isn't blank. The filter bar at the bottom is
    // drawn by DrawFilterButton so it's always present regardless.
    if (todos_.empty() && !drafting_) {
        const wchar_t* msg = todosLoading_
            ? L"\u52A0\u8F7D\u4E2D\u2026"       // 加载中…
            : L"\u6682\u65E0\u5F85\u529E";      // 暂无待办
        Label placeholder;
        // Center vertically in the list area; horizontally the Label's
        // own DWRITE_TEXT_ALIGNMENT_CENTER handles centering across
        // the full width. rowHeight is already physical pixels (see top
        // of DrawTodoList where we computed `rowHeight = Theme::kTodoRowHeight * dpi`).
        float phY = listTop + (listHeight - rowHeight) * 0.5f;
        placeholder.rect = {0, phY, width, rowHeight};
        placeholder.text = msg;
        placeholder.fontSize = Theme::kFontSizeBody;
        placeholder.color = Theme::TextPlaceholder();
        placeholder.alignment = DWRITE_TEXT_ALIGNMENT_CENTER;
        placeholder.Draw(rt, dw, dpi);
    }

    scrollView_.EndContent(rt);
    scrollView_.DrawScrollbar(rt);
}

// ---------------------------------------------------------------------------
// Filter bar (bottom): shows filter summary; click to open FilterEditor.
// ---------------------------------------------------------------------------

void StickyWindow::DrawFilterButton(ID2D1RenderTarget* rt, IDWriteFactory* dw, float dpi, float width, float height) {
    // FilterBarHeight() is already physical pixels (cached via
    // RefreshLayoutMetrics). Theme::kPadding is a 96-DPI baseline
    // and must be scaled by `dpi`. The 2px top inset / 4px vertical
    // deflation are also 96-DPI baselines.
    const float padX = Theme::kPadding * dpi;
    const float barInsetY = 2.0f * dpi;
    const float barDeflateY = 4.0f * dpi;

    float barY = height - FilterBarHeight();
    filterButton_.rect = {padX, barY + barInsetY,
                           width - padX * 2.0f,
                           FilterBarHeight() - barDeflateY};
    filterButton_.text = BuildFilterSummary(filter_);
    filterButton_.onClick = [this]() { ShowFilterEditor(); };
    filterButton_.Draw(rt, dw, dpi);
}

// ---------------------------------------------------------------------------
// Hit test helpers (operate on the CURRENT frame layout; safe to call after
// any OnPaint because every Draw*() writes back its rect into the member).
// ---------------------------------------------------------------------------

namespace {

struct RowHitTest {
    int rowIndex = -1;          // index into todos_ (-1 = miss)
    enum class Zone {
        None,
        Checkbox,
        Title,
        ActionComplete,     // slot 1: ✓ / ↺
        ActionEdit,         // slot 2: ✎
        ActionDelete,       // slot 3: ✖ / ↶
    } zone = Zone::None;
};

/// Locate which row / which zone a client-area point belongs to.
/// contentY = clientY + scrollOffset (i.e. point in scroll-content coord system).
///
/// All inputs are in **physical pixels**. `dpi` is used exclusively
/// to scale the 96-DPI Theme constants / per-row row height so the
/// arithmetic matches the geometry DrawTodoList produced for the same
/// frame.
static RowHitTest HitTestRow(float mx, float contentY, float contentWidth,
                             float listTop, bool drafting, float dpi,
                             const std::vector<models::Todo>& todos) {
    RowHitTest r;
    float rowHeight = Theme::kTodoRowHeight * dpi;
    float y = listTop;
    if (drafting) y += rowHeight;

    if (contentY < y) return r;

    int idx = static_cast<int>((contentY - y) / rowHeight);
    if (idx < 0 || idx >= static_cast<int>(todos.size())) return r;

    r.rowIndex = idx;
    float rowTop = y + idx * rowHeight;
    float rowBottom = rowTop + rowHeight;
    RowLayout L = ComputeRowLayout(contentWidth, rowTop, dpi);

    // Checkbox
    float cbLeft = L.checkboxLeft;
    float cbTop = rowTop + (L.rowHeight - L.checkboxSize) / 2.0f;
    if (mx >= cbLeft && mx < cbLeft + L.checkboxSize
        && contentY >= cbTop && contentY < cbTop + L.checkboxSize) {
        r.zone = RowHitTest::Zone::Checkbox;
        return r;
    }

    // Action icons (only active region; caller decides whether to dispatch
    // depending on hover state — we still return the zone unconditionally so
    // keyboard/focus logic can reuse this helper in the future).
    float ax = L.actionsLeft;
    float ay = rowTop + (L.rowHeight - L.actionIconSize) * 0.5f;
    auto inSlot = [&](int slot) {
        float left = ax + slot * (L.actionIconSize + L.actionGap);
        return mx >= left && mx < left + L.actionIconSize
            && contentY >= ay && contentY < ay + L.actionIconSize;
    };
    if (inSlot(0)) { r.zone = RowHitTest::Zone::ActionComplete; return r; }
    if (inSlot(1)) { r.zone = RowHitTest::Zone::ActionEdit;     return r; }
    if (inSlot(2)) { r.zone = RowHitTest::Zone::ActionDelete;   return r; }

    // Title: anywhere between textLeft and textRight, within the row band.
    if (mx >= L.textLeft && mx < L.textRight
        && contentY >= rowTop && contentY < rowBottom) {
        r.zone = RowHitTest::Zone::Title;
        return r;
    }

    return r;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Message pump
// ---------------------------------------------------------------------------

LRESULT CALLBACK StickyWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    auto* self = reinterpret_cast<StickyWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && self->hwnd_ == nullptr) {
        // Early messages (WM_GETMINMAXINFO etc.) may arrive before CreateWindowExW returns.
        self->hwnd_ = hwnd;
    }
    if (self) {
        return self->HandleMessage(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT StickyWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd_, &ps);
            OnPaint();
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_SIZE:          OnResize(); return 0;
        case WM_MOVE:          OnMove(); return 0;
        case WM_MOUSEMOVE:     OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)); return 0;
        case WM_LBUTTONDOWN:   OnLButtonDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)); return 0;
        case WM_LBUTTONUP:     OnLButtonUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)); return 0;
        case WM_MOUSEWHEEL:    OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam)); return 0;
        case WM_CHAR:          OnChar(static_cast<wchar_t>(wParam)); return 0;
        case WM_KEYDOWN:       OnKeyDown(wParam, lParam); return 0;
        case WM_NCHITTEST:     return OnNcHitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        case WM_ERASEBKGND:    return 1;  // prevent flicker; OnPaint covers full client area
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_MOUSELEAVE:
            // Re-arm on next WM_MOUSEMOVE.
            mouseTracking_ = false;
            OnMouseLeave();
            return 0;
        case WM_SETCURSOR: {
            // I-beam cursor over the inline draft / title-edit
            // TextBoxes. Gate each on its visibility flag because
            // their rects are leftover from the previous frame when
            // the box is not currently rendered (drafting_=false or
            // editingRowIndex_=-1), and we don't want a phantom
            // I-beam over an invisible hit zone.
            if (LOWORD(lParam) == HTCLIENT) {
                POINT pt;
                if (GetCursorPos(&pt) && ScreenToClient(hwnd_, &pt)) {
                    float mx = static_cast<float>(pt.x);
                    float my = static_cast<float>(pt.y);
                    bool overInput =
                        (drafting_ && draftBox_.rect.Contains(mx, my)) ||
                        (editingRowIndex_ >= 0 && editBox_.rect.Contains(mx, my));
                    if (overInput) {
                        SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
                        return TRUE;
                    }
                }
            }
            return DefWindowProcW(hwnd_, msg, wParam, lParam);
        }
        case core::WM_STICKYTODO_REFRESH:
            // Fan-out from AppState::HandleWsEventOnUIThread on todo.*
            // events, routed precisely via App::PostMessageToAllStickies
            // (never HWND_BROADCAST). The WS worker thread first posts
            // WM_STICKYTODO_WS_EVENT to the tray hwnd, which runs the
            // handler on the UI thread; that handler in turn does the
            // fan-out, so by the time this case fires we're already on
            // the UI thread with no cross-thread concerns. Each sticky
            // filters via its own ListTodos(filter_), so spurious
            // cross-sticky events only cost one redundant refresh.
            Refresh();
            return 0;
        case core::WM_STICKYTODO_STICKY_UPSERTED:
            // Server-authoritative sticky metadata changed (title/bg/filter).
            // LoadData() re-parses stickyNote_.bg_color / filter and re-runs
            // ListTodos(filter_), which is exactly what we need here.
            LoadData();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case core::WM_STICKYTODO_STICKY_DELETED:
            // Sticky removed server-side. We **must not** synchronously call
            // App::CloseStickyWindow(stickyId_) from inside WndProc — that
            // would delete the owning unique_ptr and run `~StickyWindow` on
            // the current stack frame, leaving `this` dangling before we
            // return. Instead, call DestroyWindow on our own HWND; Win32
            // will post WM_DESTROY which we handle below. App cleans up its
            // map entry via OnStickyWindowDestroyed during that path.
            if (hwnd_) {
                DestroyWindow(hwnd_);
            }
            return 0;
        case WM_DPICHANGED: {
            // User moved the sticky to a different-DPI monitor (or
            // the system scaling changed). lParam carries a RECT*
            // pre-scaled by Win32 for the new DPI — adopt it so the
            // window keeps its visual size / position on the new
            // monitor. Then proactively refresh the physical-pixel
            // layout cache so any WM_NCHITTEST arriving before the
            // next WM_PAINT sees the new titleBarHeight_ /
            // filterBarHeight_ (otherwise clicks in the first ~16 ms
            // after the move could hit the old threshold).
            RECT* suggested = reinterpret_cast<RECT*>(lParam);
            if (suggested) {
                SetWindowPos(hwnd_, nullptr,
                             suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            RefreshLayoutMetrics();
            // Also invalidate any persisted frame bookkeeping in
            // AppState so the sticky's new physical rect is saved,
            // not the pre-move one — piggyback on the existing
            // SaveFramePosition() helper which reads GetWindowRect.
            SaveFramePosition();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_DESTROY:
            // Notify App so it can erase its unique_ptr entry. Must happen
            // before clearing hwnd_ since App doesn't take the HWND here —
            // it uses the sticky id, which is stable across the lifetime of
            // this object.
            if (auto* app = GetApp()) {
                app->OnStickyWindowDestroyed(stickyId_);
            }
            hwnd_ = nullptr;
            return 0;
        default:
            return DefWindowProcW(hwnd_, msg, wParam, lParam);
    }
}

// ---------------------------------------------------------------------------
// Mouse handling
// ---------------------------------------------------------------------------

void StickyWindow::OnMouseMove(int x, int y) {
    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);

    // Arm TrackMouseEvent so Win32 will post WM_MOUSELEAVE when the cursor
    // exits our client area. Registration is one-shot per WM_MOUSELEAVE
    // delivery — we flip mouseTracking_ off in OnMouseLeave to re-arm on the
    // next WM_MOUSEMOVE. Without this, hoveredRowIndex_ / button hover state
    // would stick when the pointer left the window (e.g. user hovers row 3,
    // then mouses straight out the side — row 3 would stay highlighted).
    if (!mouseTracking_) {
        TRACKMOUSEEVENT tme = {};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd_;
        tme.dwHoverTime = HOVER_DEFAULT;
        if (TrackMouseEvent(&tme)) {
            mouseTracking_ = true;
        }
    }

    // Track title-bar hover so the trash button fades in/out at the right
    // time. Recompute on every mouse move: cheap, and keeps the flag honest
    // when the user drags the cursor across the title-bar boundary.
    bool wasTitleBarHovered = titleBarHovered_;
    titleBarHovered_ = (fy >= 0.0f && fy < titleBarHeight_);
    if (wasTitleBarHovered && !titleBarHovered_) {
        // Leaving the title bar — reset the trash button's state machine so
        // it doesn't linger in Hover appearance next time the band is
        // re-entered with the cursor already over some other button.
        trashButton_.HandleMouse(WM_MOUSEMOVE, -1.0f, -1.0f);
    }

    // Title bar buttons receive hover first so the state machine (Normal/Hover/Pressed)
    // tracked inside Button can redraw its background.
    closeButton_.HandleMouse(WM_MOUSEMOVE, fx, fy);
    settingsButton_.HandleMouse(WM_MOUSEMOVE, fx, fy);
    plusButton_.HandleMouse(WM_MOUSEMOVE, fx, fy);
    if (titleBarHovered_) {
        trashButton_.HandleMouse(WM_MOUSEMOVE, fx, fy);
    }
    filterButton_.HandleMouse(WM_MOUSEMOVE, fx, fy);

    // Scrollbar drag
    scrollView_.HandleMouse(WM_MOUSEMOVE, fx, fy);

    // Row hover — translate client y into content coords by adding scrollOffset.
    // titleBarHeight_ / FilterBarHeight() already return physical pixels;
    // pass the current DPI to HitTestRow so it scales the internal
    // Theme::k* constants to match the rendered geometry.
    RECT rc;
    GetClientRect(hwnd_, &rc);
    float contentWidth = static_cast<float>(rc.right - rc.left);
    float listTop = titleBarHeight_;
    float listBottom = static_cast<float>(rc.bottom - rc.top) - FilterBarHeight();
    float dpi = D2DRenderer::GetDpiScale(hwnd_);

    int newHover = -1;
    if (fy >= listTop && fy < listBottom) {
        float contentY = fy + scrollView_.scrollOffset;
        auto hit = HitTestRow(fx, contentY, contentWidth, listTop, drafting_, dpi, todos_);
        newHover = hit.rowIndex;
    }
    if (newHover != hoveredRowIndex_) {
        hoveredRowIndex_ = newHover;
    }

    // Always repaint — buttons track their own hover state, hover indicator is visual.
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void StickyWindow::OnMouseLeave() {
    // Cursor left the client area. Reset every piece of hover state so the
    // UI doesn't render a stale highlight while the pointer is elsewhere.
    // We send each Button a synthetic WM_MOUSEMOVE at (-1, -1) — well outside
    // any possible rect — so Button::HandleMouse flips its state back to
    // Normal without needing a dedicated "leave" entry point.
    closeButton_.HandleMouse(WM_MOUSEMOVE, -1.0f, -1.0f);
    settingsButton_.HandleMouse(WM_MOUSEMOVE, -1.0f, -1.0f);
    plusButton_.HandleMouse(WM_MOUSEMOVE, -1.0f, -1.0f);
    trashButton_.HandleMouse(WM_MOUSEMOVE, -1.0f, -1.0f);
    filterButton_.HandleMouse(WM_MOUSEMOVE, -1.0f, -1.0f);
    titleBarHovered_ = false;
    hoveredRowIndex_ = -1;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void StickyWindow::OnLButtonDown(int x, int y) {
    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);

    // Title bar: four buttons in the top-right corner. The trash button is
    // only hit-testable while the title bar is hovered — OnMouseMove sets the
    // flag, and a left-click always follows at least one WM_MOUSEMOVE onto
    // the button (mouse can't teleport), so the flag is guaranteed to be true
    // whenever the cursor is actually over the trash rect on a down-click.
    if (closeButton_.HandleMouse(WM_LBUTTONDOWN, fx, fy) ||
        settingsButton_.HandleMouse(WM_LBUTTONDOWN, fx, fy) ||
        plusButton_.HandleMouse(WM_LBUTTONDOWN, fx, fy) ||
        (titleBarHovered_ && trashButton_.HandleMouse(WM_LBUTTONDOWN, fx, fy)) ||
        filterButton_.HandleMouse(WM_LBUTTONDOWN, fx, fy)) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    // Scrollbar track
    if (scrollView_.HandleMouse(WM_LBUTTONDOWN, fx, fy)) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    // Draft / edit TextBox focus — must be handled before row dispatch
    // so a click inside the TextBox grabs focus rather than being eaten by
    // the row logic. Important: draftBox_/editBox_ rects are stored in
    // *scroll-content* coordinates (because DrawTodoList sets them while
    // ScrollView::BeginContent has shifted the transform by -scrollOffset).
    // We must translate the mouse into the same coordinate space before
    // hit-testing, and we must only do this translation when the click lands
    // inside the list band (otherwise a click in the title bar at
    // clientY=10 with scrollOffset=100 would hit a TextBox drawn at
    // contentY=110 that's not actually visible there).
    RECT rc;
    GetClientRect(hwnd_, &rc);
    float contentWidth = static_cast<float>(rc.right - rc.left);
    float listTop = titleBarHeight_;
    float listBottom = static_cast<float>(rc.bottom - rc.top) - FilterBarHeight();

    if (fy >= listTop && fy < listBottom) {
        float contentFy = fy + scrollView_.scrollOffset;
        if (drafting_ && draftBox_.HandleMouse(WM_LBUTTONDOWN, fx, contentFy)) {
            if (editingRowIndex_ >= 0) CancelTitleEdit();
            SetFocus(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (editingRowIndex_ >= 0 && editBox_.HandleMouse(WM_LBUTTONDOWN, fx, contentFy)) {
            SetFocus(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
    }

    // Row dispatch (below titleBar, above filterBar)
    if (fy < listTop || fy >= listBottom) {
        // Click in blank area → commit any in-flight draft/edit ("focus lost" semantics).
        if (drafting_) CommitDraft();
        if (editingRowIndex_ >= 0) CommitTitleEdit();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    // Match the DPI used by the last paint frame so row-index arithmetic
    // inside HitTestRow is consistent with the rendered rowHeight.
    float dpi = D2DRenderer::GetDpiScale(hwnd_);
    float contentY = fy + scrollView_.scrollOffset;
    auto hit = HitTestRow(fx, contentY, contentWidth, listTop, drafting_, dpi, todos_);
    if (hit.rowIndex < 0) {
        // Empty row gap → same as blank-area click: commit in-flight editors.
        if (drafting_) CommitDraft();
        if (editingRowIndex_ >= 0) CommitTitleEdit();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    // If user clicks into a different row while a title edit is active, commit the old one first.
    if (editingRowIndex_ >= 0 && editingRowIndex_ != hit.rowIndex) {
        CommitTitleEdit();
    }

    const auto& todo = todos_[hit.rowIndex];
    switch (hit.zone) {
        case RowHitTest::Zone::Checkbox:
            if (!todo.IsDeleted()) {
                if (todo.IsDone()) DoReopen(hit.rowIndex);
                else               DoComplete(hit.rowIndex);
            }
            break;
        case RowHitTest::Zone::ActionComplete:
            if (!todo.IsDeleted()) {
                if (todo.IsDone()) DoReopen(hit.rowIndex);
                else               DoComplete(hit.rowIndex);
            }
            break;
        case RowHitTest::Zone::ActionEdit:
            if (!todo.IsDeleted()) BeginTitleEdit(static_cast<size_t>(hit.rowIndex));
            break;
        case RowHitTest::Zone::ActionDelete:
            if (todo.IsDeleted()) DoRestore(hit.rowIndex);
            else                  DoDelete(hit.rowIndex);
            break;
        case RowHitTest::Zone::Title:
            if (!todo.IsDeleted()) BeginTitleEdit(static_cast<size_t>(hit.rowIndex));
            break;
        case RowHitTest::Zone::None:
        default:
            break;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void StickyWindow::OnLButtonUp(int x, int y) {
    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);
    closeButton_.HandleMouse(WM_LBUTTONUP, fx, fy);
    settingsButton_.HandleMouse(WM_LBUTTONUP, fx, fy);
    plusButton_.HandleMouse(WM_LBUTTONUP, fx, fy);
    // Trash only fires on LBUTTONUP while the title bar is still hovered;
    // otherwise the click was released outside and should not trigger the
    // destructive onClick handler.
    if (titleBarHovered_) {
        trashButton_.HandleMouse(WM_LBUTTONUP, fx, fy);
    }
    filterButton_.HandleMouse(WM_LBUTTONUP, fx, fy);
    scrollView_.HandleMouse(WM_LBUTTONUP, fx, fy);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void StickyWindow::OnMouseWheel(short delta) {
    float fDelta = static_cast<float>(delta) / WHEEL_DELTA;
    if (scrollView_.HandleWheel(fDelta)) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

// ---------------------------------------------------------------------------
// Keyboard
// ---------------------------------------------------------------------------

void StickyWindow::OnChar(wchar_t ch) {
    // Route printable characters to whichever TextBox currently owns focus.
    if (drafting_ && draftBox_.focused) {
        if (draftBox_.HandleChar(ch)) {
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }
    if (editingRowIndex_ >= 0 && editBox_.focused) {
        if (editBox_.HandleChar(ch)) {
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }
}

void StickyWindow::OnKeyDown(WPARAM vk, LPARAM lParam) {
    // Escape cancels whichever editor is active. Enter commits.
    // Route other navigation keys to the focused TextBox.
    if (drafting_ && draftBox_.focused) {
        if (vk == VK_ESCAPE) {
            CancelDraft();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (vk == VK_RETURN) {
            CommitDraft();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (draftBox_.HandleKey(WM_KEYDOWN, vk, lParam)) {
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }
    if (editingRowIndex_ >= 0 && editBox_.focused) {
        if (vk == VK_ESCAPE) {
            CancelTitleEdit();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (vk == VK_RETURN) {
            CommitTitleEdit();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (editBox_.HandleKey(WM_KEYDOWN, vk, lParam)) {
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }
}

// ---------------------------------------------------------------------------
// Draft (new-todo inline input)
// ---------------------------------------------------------------------------

void StickyWindow::BeginDraft() {
    // Entering draft mode implicitly commits any in-flight title edit so we never
    // have two TextBoxes active simultaneously.
    if (editingRowIndex_ >= 0) CommitTitleEdit();

    drafting_ = true;
    draftBox_.text.clear();
    draftBox_.cursorPos = 0;
    draftBox_.selStart = draftBox_.selEnd = -1;
    draftBox_.SetFocus(true);

    SetFocus(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void StickyWindow::CommitDraft() {
    if (!drafting_) return;

    std::wstring raw = draftBox_.text;
    // Trim whitespace / control chars on both ends.
    auto notSpace = [](wchar_t c) { return !(c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'); };
    auto first = std::find_if(raw.begin(), raw.end(), notSpace);
    auto last  = std::find_if(raw.rbegin(), raw.rend(), notSpace).base();
    std::wstring trimmed = (first < last) ? std::wstring(first, last) : std::wstring();

    if (trimmed.empty()) {
        // Empty draft behaves the same as Esc — per calling convention agreed
        // with macOS DraftTodoRow.
        CancelDraft();
        return;
    }

    auto* app = GetApp();
    if (!app || !app->GetState() || !app->GetState()->GetHttp()) {
        CancelDraft();
        return;
    }

    std::string title = WideToUtf8(trimmed);

    // ---- Optimistic append ---------------------------------------
    //
    // Reserve a placeholder uint64 id from the top of the id space
    // (UINT64_MAX, UINT64_MAX-1, ...) — the server allocates small
    // sequential ids so this range is collision-free. Construct a
    // local Todo with the user's title and insert into todos_ so the
    // row appears immediately. Store the placeholder id in a local
    // variable: the async callback will use it to locate the row for
    // swap (on success) or removal (on failure).
    uint64_t placeholderId = nextPendingTodoId_--;
    models::Todo placeholder;
    placeholder.id = placeholderId;
    placeholder.title = title;
    placeholder.priority = 0;
    // Leave other fields at default (created_at empty, deleted_at
    // empty, tag empty). The real server-returned Todo will replace
    // this wholesale on success, so these placeholder defaults are
    // only visible for the ~round-trip duration.
    todos_.push_back(placeholder);

    // Exit draft mode immediately — the user sees their text become
    // a real row and can start a new draft right away.
    drafting_ = false;
    draftBox_.text.clear();
    draftBox_.SetFocus(false);
    InvalidateRect(hwnd_, nullptr, FALSE);

    // Fire the async create. Capture placeholderId + trimmed text by
    // value so the rollback path can re-seed the draft box with the
    // user's original input on failure.
    app->GetState()->GetHttp()->AsyncCreateTodo(title, 0, "", "",
        [this, alive = alive_, placeholderId, originalText = trimmed]
        (std::optional<models::Todo> created) {
            // See LoadData's callback for the alive_ / hwnd_ guard
            // rationale.
            if (!alive->load()) return;
            if (!hwnd_) return;
            // Locate the placeholder in todos_ by id. It may have
            // moved (unlikely but possible — a WS-driven refresh
            // could have reordered the list during the round-trip)
            // or been removed (e.g. user clicked the trash on their
            // own placeholder row, which shouldn't really be
            // possible since the row id is unknown to the UI but
            // we guard anyway).
            auto it = std::find_if(todos_.begin(), todos_.end(),
                [placeholderId](const models::Todo& t) {
                    return t.id == placeholderId;
                });
            if (created.has_value()) {
                // Success: swap placeholder for the real row in
                // place. If the placeholder is gone (unusual), just
                // append the real row — the server-side WS
                // broadcast will cause a full refresh shortly anyway
                // so a transient duplicate is harmless.
                if (it != todos_.end()) {
                    *it = std::move(*created);
                } else {
                    todos_.push_back(std::move(*created));
                }
            } else {
                // Failure: rollback — remove placeholder row and
                // restore the user's text into the draft box so
                // they can retry without retyping. Re-enter
                // draft mode to make that path immediately usable.
                if (it != todos_.end()) {
                    todos_.erase(it);
                }
                drafting_ = true;
                draftBox_.text = originalText;
                draftBox_.cursorPos = static_cast<int>(originalText.size());
                draftBox_.selStart = draftBox_.selEnd = -1;
                draftBox_.SetFocus(true);
                SetFocus(hwnd_);
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
        });
}

void StickyWindow::CancelDraft() {
    if (!drafting_) return;
    drafting_ = false;
    draftBox_.text.clear();
    draftBox_.cursorPos = 0;
    draftBox_.selStart = draftBox_.selEnd = -1;
    draftBox_.SetFocus(false);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Title in-place edit
// ---------------------------------------------------------------------------

void StickyWindow::BeginTitleEdit(size_t rowIndex) {
    if (rowIndex >= todos_.size()) return;
    if (todos_[rowIndex].IsDeleted()) return;

    if (drafting_) CancelDraft();
    if (editingRowIndex_ >= 0) CommitTitleEdit();

    editingRowIndex_ = static_cast<int>(rowIndex);
    editBox_.text = Utf8ToWide(todos_[rowIndex].title);
    editBox_.cursorPos = static_cast<int>(editBox_.text.size());
    editBox_.selStart = editBox_.selEnd = -1;
    editBox_.SetFocus(true);

    SetFocus(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void StickyWindow::CommitTitleEdit() {
    if (editingRowIndex_ < 0 || editingRowIndex_ >= static_cast<int>(todos_.size())) {
        editingRowIndex_ = -1;
        editBox_.SetFocus(false);
        return;
    }

    models::Todo& todo = todos_[editingRowIndex_];
    std::wstring raw = editBox_.text;
    auto notSpace = [](wchar_t c) { return !(c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'); };
    auto first = std::find_if(raw.begin(), raw.end(), notSpace);
    auto last  = std::find_if(raw.rbegin(), raw.rend(), notSpace).base();
    std::wstring trimmed = (first < last) ? std::wstring(first, last) : std::wstring();
    std::string newTitle = WideToUtf8(trimmed);

    int savingIndex = editingRowIndex_;
    editingRowIndex_ = -1;
    editBox_.SetFocus(false);
    editBox_.text.clear();

    // Empty or unchanged → no-op (matches macOS TodoRow.commitTitleEdit behaviour).
    if (newTitle.empty() || newTitle == todo.title) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    auto* app = GetApp();
    if (!app || !app->GetState() || !app->GetState()->GetHttp()) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    // ---- Optimistic update ---------------------------------------
    //
    // Snapshot the row's current state for rollback, apply the new
    // title locally, then fire async update. On success the callback
    // swaps in the server-authoritative row (which may differ from
    // our optimistic version in e.g. updated_at); on failure we
    // restore the snapshot so the user sees their pre-edit title.
    //
    // Locate by id (not savingIndex) because a concurrent WS refresh
    // could reorder the list during the round-trip.
    uint64_t todoId = todo.id;
    models::Todo snapshot = todo;
    todo.title = newTitle;  // in-place optimistic
    InvalidateRect(hwnd_, nullptr, FALSE);

    app->GetState()->GetHttp()->AsyncUpdateTodo(
        todoId, newTitle, snapshot.priority, snapshot.tag, snapshot.due_at,
        [this, alive = alive_, todoId, snapshot = std::move(snapshot)]
        (std::optional<models::Todo> updated) mutable {
            if (!alive->load()) return;
            if (!hwnd_) return;
            auto it = std::find_if(todos_.begin(), todos_.end(),
                [todoId](const models::Todo& t) { return t.id == todoId; });
            if (it == todos_.end()) {
                // Row disappeared (WS-driven delete?) — nothing to
                // reconcile. Leave the list as-is.
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            if (updated.has_value()) {
                *it = std::move(*updated);
            } else {
                // Rollback to pre-edit snapshot.
                *it = std::move(snapshot);
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
        });
}

void StickyWindow::CancelTitleEdit() {
    if (editingRowIndex_ < 0) return;
    editingRowIndex_ = -1;
    editBox_.text.clear();
    editBox_.cursorPos = 0;
    editBox_.selStart = editBox_.selEnd = -1;
    editBox_.SetFocus(false);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Per-row actions
// ---------------------------------------------------------------------------

// ---- Optimistic toggle pattern ------------------------------------------
//
// DoComplete / DoReopen / DoRestore all share the same shape:
//   1. capture todoId + a pre-mutation snapshot of todos_[rowIndex]
//   2. apply an optimistic local mutation (local lambda `applyLocal`)
//   3. fire the async HTTP; on success swap in the server row, on
//      failure restore the snapshot
//
// An earlier draft factored this into a free-function helper
// `FireOptimisticToggle(vector&, HWND&, ...)` to keep each call site
// to ~10 lines. That helper captured `todos_` and `hwnd_` BY
// REFERENCE into the async callback — which breaks the `this`-based
// lifetime guard used by every other async call site in this file:
// if the StickyWindow is destroyed before the callback lands, the
// reference dangles and we read garbage instead of seeing
// hwnd_==nullptr. Rather than smuggle `this` through the helper (at
// which point the helper is no longer saving meaningful lines), each
// call site is written out inline so the `this` capture and
// `!hwnd_` guard are uniform across the whole file.

void StickyWindow::DoComplete(size_t rowIndex) {
    if (rowIndex >= todos_.size()) return;
    auto* app = GetApp();
    if (!app || !app->GetState() || !app->GetState()->GetHttp()) return;

    uint64_t todoId = todos_[rowIndex].id;
    models::Todo snapshot = todos_[rowIndex];
    // Optimistic: mark as completed. The real updated_at / completed_at
    // will come back from the server and overwrite this, but for the
    // visual state (strike-through keyed off IsDone()) a sentinel
    // suffices.
    todos_[rowIndex].completed_at = std::string("pending");
    InvalidateRect(hwnd_, nullptr, FALSE);

    app->GetState()->GetHttp()->AsyncCompleteTodo(todoId,
        [this, alive = alive_, todoId, snapshot = std::move(snapshot)]
        (std::optional<models::Todo> updated) mutable {
            if (!alive->load()) return;
            if (!hwnd_) return;
            auto it = std::find_if(todos_.begin(), todos_.end(),
                [todoId](const models::Todo& t) { return t.id == todoId; });
            if (it == todos_.end()) {
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            if (updated.has_value()) {
                *it = std::move(*updated);
            } else {
                *it = std::move(snapshot);
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
        });
}

void StickyWindow::DoReopen(size_t rowIndex) {
    if (rowIndex >= todos_.size()) return;
    auto* app = GetApp();
    if (!app || !app->GetState() || !app->GetState()->GetHttp()) return;

    uint64_t todoId = todos_[rowIndex].id;
    models::Todo snapshot = todos_[rowIndex];
    // Optimistic: clear completed_at so IsDone() returns false and the
    // row immediately re-renders as active.
    todos_[rowIndex].completed_at.clear();
    InvalidateRect(hwnd_, nullptr, FALSE);

    app->GetState()->GetHttp()->AsyncReopenTodo(todoId,
        [this, alive = alive_, todoId, snapshot = std::move(snapshot)]
        (std::optional<models::Todo> updated) mutable {
            if (!alive->load()) return;
            if (!hwnd_) return;
            auto it = std::find_if(todos_.begin(), todos_.end(),
                [todoId](const models::Todo& t) { return t.id == todoId; });
            if (it == todos_.end()) {
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            if (updated.has_value()) {
                *it = std::move(*updated);
            } else {
                *it = std::move(snapshot);
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
        });
}

void StickyWindow::DoDelete(size_t rowIndex) {
    if (rowIndex >= todos_.size()) return;
    auto* app = GetApp();
    if (!app || !app->GetState() || !app->GetState()->GetHttp()) return;

    const auto& todo = todos_[rowIndex];

    // Soft-delete confirmation — matches macOS TodoRow alert, including the
    // "don't ask again" preference persisted via Preferences (HKCU).
    // MB_YESNOCANCEL offers three outcomes without pulling in the TaskDialog
    // API: Yes = delete once; No = delete and never ask again; Cancel = abort.
    if (!ShouldSkipTodoDeleteConfirm()) {
        // 确认删除这条待办？\n此操作会放入回收状态，可通过「仅已删除」筛选恢复。
        // \n[是] 删除  [否] 删除并不再提示  [取消] 放弃
        std::wstring msg =
            L"\u786E\u8BA4\u5220\u9664\u8FD9\u6761\u5F85\u529E\uFF1F\n"
            L"\u6B64\u64CD\u4F5C\u4F1A\u653E\u5165\u56DE\u6536\u72B6\u6001\uFF0C"
            L"\u53EF\u901A\u8FC7\u300C\u4EC5\u5DF2\u5220\u9664\u300D\u7B5B\u9009\u6062\u590D\u3002\n\n"
            L"[\u662F] \u5220\u9664    [\u5426] \u5220\u9664\u5E76\u4E0D\u518D\u63D0\u793A    [\u53D6\u6D88] \u653E\u5F03";
        int reply = MessageBoxW(hwnd_, msg.c_str(), L"StickyTodo",
                                MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON3);
        if (reply == IDCANCEL) return;
        if (reply == IDNO) {
            // "Delete and don't ask again" — persist preference before deleting.
            SetSkipTodoDeleteConfirm(true);
        }
        // IDYES and IDNO both fall through to actually perform the delete.
    }

    // ---- Optimistic soft-delete ----------------------------------
    //
    // Snapshot the row for rollback, flip deleted_at locally so the
    // row immediately shows the "deleted" style, then fire async
    // DELETE. On failure clear deleted_at back (no server-returned
    // row to swap — DELETE's bool API doesn't carry one) so the row
    // returns to its live state. On success keep our optimistic
    // state; the WS-driven refresh will replace it with the
    // server-authoritative row (with a real deleted_at timestamp)
    // shortly.
    //
    // `deleted_at` is `std::optional<std::string>` (Todo.h:22 — null
    // for not-deleted, populated with an ISO8601 string for deleted).
    // prevDeletedAt must preserve the optional semantics for
    // rollback: when the user deletes a previously-live todo, the
    // snapshot is std::nullopt and we must restore that exact state
    // (not an empty string, which would still count as "deleted" per
    // IsDeleted()). Copy-capture the optional into the lambda rather
    // than moving to keep the ctor-initialiser readable.
    uint64_t todoId = todo.id;
    std::optional<std::string> prevDeletedAt = todos_[rowIndex].deleted_at;
    todos_[rowIndex].deleted_at = std::string("pending");
    InvalidateRect(hwnd_, nullptr, FALSE);

    app->GetState()->GetHttp()->AsyncDeleteTodo(todoId,
        [this, alive = alive_, todoId, prevDeletedAt = std::move(prevDeletedAt)]
        (bool ok) {
            if (!alive->load()) return;
            if (!hwnd_) return;
            if (!ok) {
                auto it = std::find_if(todos_.begin(), todos_.end(),
                    [todoId](const models::Todo& t) { return t.id == todoId; });
                if (it != todos_.end()) {
                    it->deleted_at = prevDeletedAt;
                }
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
        });
}

// ---------------------------------------------------------------------------
// Sticky-level delete (trash button in title bar).
// Mirrors macOS StickyView's trash button: fires DELETE /api/sticky-notes/:id,
// then lets the server-driven sticky.deleted WS event close our window via
// WM_STICKYTODO_STICKY_DELETED (which in turn calls DestroyWindow — see the
// handler in HandleMessage). The confirmation dialog uses a separate
// preference key (sticky.skipDeleteConfirm on macOS /
// skipStickyDeleteConfirm in HKCU) so users can independently silence the
// two dialogs.
void StickyWindow::DoDeleteSticky() {
    auto* app = GetApp();
    if (!app || !app->GetState() || !app->GetState()->GetHttp()) return;

    if (!ShouldSkipStickyDeleteConfirm()) {
        // 确认删除此便签？\n删除后该便签及其筛选条件将从云端永久移除，无法恢复。
        // \n[是] 删除  [否] 删除并不再提示  [取消] 放弃
        std::wstring msg =
            L"\u786E\u8BA4\u5220\u9664\u6B64\u4FBF\u7B7E\uFF1F\n"
            L"\u5220\u9664\u540E\u8BE5\u4FBF\u7B7E\u53CA\u5176\u7B5B\u9009\u6761\u4EF6\u5C06"
            L"\u4ECE\u4E91\u7AEF\u6C38\u4E45\u79FB\u9664\uFF0C\u65E0\u6CD5\u6062\u590D\u3002\n\n"
            L"[\u662F] \u5220\u9664    [\u5426] \u5220\u9664\u5E76\u4E0D\u518D\u63D0\u793A    [\u53D6\u6D88] \u653E\u5F03";
        int reply = MessageBoxW(hwnd_, msg.c_str(), L"StickyTodo",
                                MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON3);
        if (reply == IDCANCEL) return;
        if (reply == IDNO) {
            SetSkipStickyDeleteConfirm(true);
        }
    }

    // ---- Pessimistic delete --------------------------------------
    //
    // Don't optimistically close the window: DeleteSticky IS the
    // action that removes the window, so if the HTTP fails we need
    // the window to still be around to surface the failure (and
    // re-enable the trash button). Set stickyDeleting_ to disable
    // the trash button UI while the request is in flight.
    //
    // Guard re-entry: if the user somehow clicks trash twice between
    // the async call and its callback, the second click is a no-op.
    if (stickyDeleting_) return;
    stickyDeleting_ = true;
    InvalidateRect(hwnd_, nullptr, FALSE);

    app->GetState()->GetHttp()->AsyncDeleteSticky(stickyId_,
        [this, alive = alive_](bool ok) {
            if (!alive->load()) return;
            if (!hwnd_) return;
            if (!ok) {
                // Failure: re-enable the trash button for a retry.
                // Leave the sticky visible and unchanged. macOS's
                // equivalent path also silently leaves the sticky
                // on failure (no user-facing error — best we can do
                // without a toast system).
                stickyDeleting_ = false;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            // Success: the server broadcast sticky.deleted will
            // land shortly and trigger WM_STICKYTODO_STICKY_DELETED
            // → DestroyWindow. Post it directly to close faster —
            // the handler is idempotent so a duplicate WS-driven
            // post later is harmless. Leave stickyDeleting_ = true
            // so no further clicks can slip through between this
            // post and WM_DESTROY.
            PostMessageW(hwnd_, core::WM_STICKYTODO_STICKY_DELETED, 0, 0);
        });
}

void StickyWindow::DoRestore(size_t rowIndex) {
    if (rowIndex >= todos_.size()) return;
    auto* app = GetApp();
    if (!app || !app->GetState() || !app->GetState()->GetHttp()) return;

    uint64_t todoId = todos_[rowIndex].id;
    models::Todo snapshot = todos_[rowIndex];
    // Optimistic: clear deleted_at so IsDeleted() returns false and the
    // row re-renders as active. Todo::deleted_at is
    // std::optional<std::string>, so reset the optional to clear.
    todos_[rowIndex].deleted_at.reset();
    InvalidateRect(hwnd_, nullptr, FALSE);

    app->GetState()->GetHttp()->AsyncRestoreTodo(todoId,
        [this, alive = alive_, todoId, snapshot = std::move(snapshot)]
        (std::optional<models::Todo> updated) mutable {
            if (!alive->load()) return;
            if (!hwnd_) return;
            auto it = std::find_if(todos_.begin(), todos_.end(),
                [todoId](const models::Todo& t) { return t.id == todoId; });
            if (it == todos_.end()) {
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            if (updated.has_value()) {
                *it = std::move(*updated);
            } else {
                *it = std::move(snapshot);
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
        });
}

// ---------------------------------------------------------------------------
// Filter editor entry — opens a modal FilterEditor window. On success the
// edited filter is serialised into this sticky's filter field via UpsertSticky,
// then LoadData() re-runs to pull the now-filtered TODO list.
// ---------------------------------------------------------------------------

void StickyWindow::ShowFilterEditor() {
    auto* app = GetApp();
    if (!app || !app->GetState() || !app->GetState()->GetHttp()) return;

    // FilterEditor::ShowModal is a locally-driven modal window (no
    // HTTP), so it's fine to keep that call synchronous — it returns
    // after the user closes the filter dialog. Only the subsequent
    // persistence + refetch need to go async.
    models::Filter edited = filter_;
    bool ok = FilterEditor::ShowModal(hwnd_, hInstance_, edited);
    if (!ok) return;

    // ---- Optimistic filter apply ---------------------------------
    //
    // Apply the new filter locally immediately so the sticky's
    // filter label in the title bar updates right away and the
    // upcoming LoadData uses the new filter. Snapshot the previous
    // filter JSON + local filter for rollback on UpsertSticky
    // failure.
    std::string filterJson = codec::StickyCodec::FilterToJson(edited);
    std::string prevFilterJson = stickyNote_.filter;
    models::Filter prevFilter = filter_;

    filter_ = edited;
    stickyNote_.filter = filterJson;

    // Kick off async persist. The filter list refresh is handled by
    // LoadData() — called unconditionally below — which itself runs
    // AsyncListTodos, so we don't double-fetch here.
    app->GetState()->GetHttp()->AsyncUpsertSticky(
        stickyId_, stickyNote_.title, stickyNote_.bg_color, filterJson,
        [this, alive = alive_,
         prevFilterJson = std::move(prevFilterJson),
         prevFilter = std::move(prevFilter)]
        (std::optional<models::StickyNote> updated) {
            if (!alive->load()) return;
            if (!hwnd_) return;
            if (updated.has_value()) {
                // Server-authoritative sticky: accept it wholesale
                // (preserves server-side fields like frame). Don't
                // overwrite filter_ — we already set it locally and
                // the server's filter echoes ours.
                stickyNote_ = std::move(*updated);
            } else {
                // Rollback: restore prev filter so the next refresh
                // uses the pre-edit filter. Avoid flicker by only
                // re-loading if we're still in a state consistent
                // with the rollback (filter_ still == edited). A
                // concurrent WS event could have already overwritten
                // stickyNote_, in which case rollback is moot.
                filter_ = prevFilter;
                stickyNote_.filter = prevFilterJson;
                LoadData();  // refetch with the reverted filter
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
        });

    // Kick off the list refresh with the new filter in parallel
    // with UpsertSticky. If UpsertSticky later fails, its callback
    // will LoadData again with the reverted filter — that's the
    // only surface that needs to care about persist failure.
    LoadData();
}

} // namespace stickytodo::ui

