#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "models/Todo.h"
#include "models/StickyNote.h"
#include "models/Filter.h"
#include "ui/Controls.h"

namespace stickytodo::ui {

/// A sticky note window displayed on the desktop.
/// Style: WS_POPUP | WS_THICKFRAME (no title bar, resizable).
/// Level: WS_EX_TOPMOST (always on top).
/// Analogous to macOS StickyWindowController + StickyView.
///
/// Default size: 300x420 (aligned with macOS StickyNote.defaultFrame).
/// Background color derived from StickyNote.bg_color (CodableRGBA JSON).
class StickyWindow {
public:
    StickyWindow(HINSTANCE hInstance, const std::string& stickyId);
    ~StickyWindow();

    /// Create and show the window. Returns false on failure.
    bool Create();

    /// Bring the window to front.
    void BringToFront();

    /// Refresh the TODO list from server data.
    void Refresh();

    /// Get the sticky note ID this window represents.
    const std::string& GetStickyId() const { return stickyId_; }

    /// Get the underlying HWND. Returns nullptr if the window has not yet been
    /// created or has already been destroyed. Used by App to route WS-driven
    /// messages (WM_STICKYTODO_REFRESH / STICKY_UPSERTED / STICKY_DELETED)
    /// precisely to this window's message queue from the WS worker thread.
    HWND GetHwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void OnPaint();
    void OnResize();
    void OnMove();
    void OnMouseMove(int x, int y);
    void OnMouseLeave();
    void OnLButtonDown(int x, int y);
    void OnLButtonUp(int x, int y);
    void OnMouseWheel(short delta);
    void OnChar(wchar_t ch);
    void OnKeyDown(WPARAM vk, LPARAM lParam);
    LRESULT OnNcHitTest(int x, int y);

    void CreateRenderTarget();
    void DiscardRenderTarget();
    void LoadData();
    void DrawTitleBar(ID2D1RenderTarget* rt, IDWriteFactory* dw, float dpi);
    void DrawDraftRow(ID2D1RenderTarget* rt, IDWriteFactory* dw, float dpi, float width, float rowY);
    void DrawTodoList(ID2D1RenderTarget* rt, IDWriteFactory* dw, float dpi);
    void DrawFilterButton(ID2D1RenderTarget* rt, IDWriteFactory* dw, float dpi, float width, float height);
    void SaveFramePosition();

    // Draft (inline new-todo) state
    void BeginDraft();
    void CommitDraft();
    void CancelDraft();

    // Title in-place edit state
    void BeginTitleEdit(size_t rowIndex);
    void CommitTitleEdit();
    void CancelTitleEdit();

    // Per-row actions
    void DoComplete(size_t rowIndex);
    void DoReopen(size_t rowIndex);
    void DoDelete(size_t rowIndex);
    void DoRestore(size_t rowIndex);

    // Sticky-level delete (title bar trash button). Mirrors macOS StickyView's
    // trash button: fires DELETE /api/sticky-notes/:id, honours the
    // skipStickyDeleteConfirm preference, then asks Win32 to destroy the
    // window (App cleans up its map entry in OnStickyWindowDestroyed).
    void DoDeleteSticky();

    // Filter editor entry
    void ShowFilterEditor();

    // Layout helpers
    // Returns the filter bar height in physical pixels. Reads the
    // DPI-aware cache populated by RefreshLayoutMetrics(). Safe to
    // call from both drawing (after OnPaint refreshed the cache)
    // and hit-test paths (which also ran through at least one
    // OnPaint before the user could click).
    float FilterBarHeight() const { return filterBarHeight_; }

    // Refreshes titleBarHeight_ / filterBarHeight_ from the current
    // monitor DPI. Called at the top of OnPaint so every subsequent
    // hit-test through the frame sees the same values the frame was
    // rendered with. Cheap — two multiplies and a GetDpiForWindow.
    void RefreshLayoutMetrics();

    HINSTANCE hInstance_;
    HWND hwnd_ = nullptr;
    std::string stickyId_;

    ID2D1HwndRenderTarget* renderTarget_ = nullptr;

    // Data
    models::StickyNote stickyNote_;
    models::Filter filter_;
    std::vector<models::Todo> todos_;

    // UI State
    ScrollView scrollView_;
    Button closeButton_;
    Button settingsButton_;
    Button plusButton_;
    Button trashButton_;     // Title-bar sticky-delete button; only rendered
                             // while the cursor is hovering the title bar
                             // (mirrors macOS StickyView's hover-fade trash).
    Button filterButton_;

    // ---- DPI-aware layout cache ----
    //
    // These are *physical pixel* values refreshed at the top of every
    // OnPaint by `RefreshLayoutMetrics()` from the current monitor
    // DPI. We keep them as members (rather than recomputing on every
    // caller) because OnNcHitTest / OnMouseMove / OnLButtonDown all
    // need the same thresholds and those handlers don't have easy
    // access to a cached `dpi` local. The initial 96-DPI values are
    // used only for hit-tests that arrive before the first OnPaint
    // (WM_GETMINMAXINFO, WM_NCHITTEST during window creation) — on
    // those early messages the window isn't visible yet anyway, so
    // any minor mis-hit is harmless.
    float titleBarHeight_ = 32.0f;   // physical pixels
    float filterBarHeight_ = 28.0f;  // physical pixels

    // True while the cursor is inside the title-bar band. Drives the
    // hover-fade trash button (only drawn / hit-tested when this flag is
    // set) so the destructive action is not a persistent visual target.
    bool titleBarHovered_ = false;

    // Draft row state: when drafting_ is true, an inline TextBox shows above the list.
    bool drafting_ = false;
    TextBox draftBox_;

    // Title edit state: when editingRowIndex_>=0, that row's title renders as a TextBox.
    int editingRowIndex_ = -1;
    TextBox editBox_;

    // Hover tracking for row action buttons.
    int hoveredRowIndex_ = -1;

    // ---------- Async request state ----------
    //
    // True between LoadData()/Refresh() kicking off AsyncListTodos and
    // the callback landing. Drives the "Loading..." placeholder shown
    // in DrawTodoList when todos_ is still empty on first load. Does
    // NOT clear todos_ on entry — subsequent refreshes keep the stale
    // list visible (no flicker) and only the bottom status line
    // indicates the in-flight load, which matches the UX the user
    // asked for ("读操作用悲观 loading 态").
    bool todosLoading_ = false;

    // True between DoDeleteSticky kicking off AsyncDeleteSticky and
    // either a success (window closed by WS event) or a failure
    // (callback resets the flag). Drives the trash button's disabled
    // state so the user can't retry-bomb the DELETE. Follows the
    // "DeleteSticky 特殊用悲观" decision — no local optimistic close
    // because the sticky window IS the thing being removed, and
    // closing it early would leave no surface to report a failure.
    bool stickyDeleting_ = false;

    // Monotonic counter for client-side placeholder Todo IDs while a
    // CreateTodo request is in flight. We reserve UINT64_MAX and
    // decrement down — the server allocates small sequential IDs
    // (uint64 but realistically under 1e12 for any real deployment)
    // so the upper range is collision-free. On success the callback
    // replaces the placeholder row with the server-returned Todo; on
    // failure it removes the placeholder row.
    uint64_t nextPendingTodoId_ = UINT64_MAX;

    // Monotonic generation counter for LoadData()'s AsyncListTodos
    // request. Incremented on every LoadData entry; the callback
    // captures its own generation and only applies the result if it
    // still matches loadDataGeneration_ at the time the reply lands.
    //
    // This closes a real race in ShowFilterEditor: when the user
    // commits a new filter we fire (a) AsyncUpsertSticky + (b)
    // LoadData() in parallel. If (a)'s callback later fails, it
    // rolls back filter_ and calls LoadData() AGAIN — producing two
    // concurrent AsyncListTodos requests with different filters.
    // Whichever callback lands last "wins" in terms of todos_
    // contents, but there's no guarantee HTTP order preserves send
    // order, so without this counter we could end up with todos_
    // matching one filter while filter_ matches the other.
    //
    // Starts at 0; todosLoading_ flips to true on LoadData entry
    // and flips back to false only when the callback for the
    // CURRENT generation lands (earlier-generation callbacks
    // silently drop).
    uint64_t loadDataGeneration_ = 0;

    // ---------- Lifetime guard for async callbacks ----------
    //
    // StickyWindow can be destroyed at any time on the UI thread:
    //   • user clicks × (close) → App::CloseStickyWindow → erase
    //   • user clicks 🗑 → DoDeleteSticky success → posts
    //     WM_STICKYTODO_STICKY_DELETED → DestroyWindow → erase
    //   • server broadcasts sticky.deleted over WS → same as above
    //   • user logs out → SyncStickyWindows mass-closes all
    //   • App shutdown → stickyWindows_.clear()
    //
    // Meanwhile up to ~40s of detached HTTP worker threads may be
    // mid-flight per request, with their completion lambdas holding
    // `this` pointers. When the lambda eventually posts back to the
    // UI thread via PostToUIThread (tray HWND), the target HWND is
    // still valid (tray outlives stickies), so the UI-thread invoke
    // runs — and `this->hwnd_` / `this->todos_` is freed memory.
    //
    // This atomic is the single source of truth for "is this
    // StickyWindow still alive?" for callback-side guarding. We
    // hold it by shared_ptr so the atomic's backing memory outlives
    // the StickyWindow instance — each in-flight callback bumps
    // the refcount by copy-capturing the shared_ptr, so the atomic
    // stays readable even after StickyWindow itself is freed.
    //
    // Usage pattern in async callbacks:
    //     [this, alive = alive_, ...](auto result) {
    //         if (!alive->load()) return;   // object gone — drop
    //         // ... safe to touch this->*
    //     }
    //
    // Constructor initialises to true; destructor sets to false
    // before any member cleanup. See StickyWindow.cpp.
    std::shared_ptr<std::atomic<bool>> alive_ =
        std::make_shared<std::atomic<bool>>(true);

    // TrackMouseEvent state. Win32 only posts WM_MOUSELEAVE if we registered
    // for it via TrackMouseEvent(TME_LEAVE); the registration is one-shot per
    // WM_MOUSELEAVE delivery, so we set the flag when (re)arming and clear it
    // on leave to re-arm on the next WM_MOUSEMOVE.
    bool mouseTracking_ = false;

    // Window class
    static bool classRegistered_;
    static constexpr wchar_t kClassName[] = L"StickyTodo_StickyWnd";
};

} // namespace stickytodo::ui
