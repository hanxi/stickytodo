#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
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
    float FilterBarHeight() const { return 28.0f; }

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
    float titleBarHeight_ = 32.0f;

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
