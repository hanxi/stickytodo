#pragma once

#include <windows.h>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <string>
#include <vector>

namespace stickytodo {

// Forward declarations
namespace core { class AppState; }
namespace ui {
    class TrayIcon;
    class SettingsWindow;
    class StickyWindow;
    class D2DRenderer;
}

/// Top-level application class. Owns the tray icon, settings window,
/// and all sticky windows. Manages lifecycle and inter-component wiring.
class App {
public:
    explicit App(HINSTANCE hInstance);
    ~App();

    /// Initialize subsystems; returns false on fatal error.
    bool Initialize(int nCmdShow);

    /// Tear down subsystems cleanly before exit.
    void Shutdown();

    /// Access the global HINSTANCE.
    HINSTANCE GetInstance() const { return hInstance_; }

    /// Access shared D2D renderer.
    ui::D2DRenderer* GetRenderer() const { return renderer_.get(); }

    /// Access the app state (auth, HTTP, WS, data).
    core::AppState* GetState() const { return state_.get(); }

    /// Access the singleton settings window (may be nullptr if never
    /// shown yet). Used by UI-thread async callbacks to verify the
    /// original issuing window is still alive before mutating its
    /// state — the async HTTP layer's PostToUIThread only guarantees
    /// the callback runs on the UI thread, NOT that the widget the
    /// callback wants to update still exists. Compare against a
    /// captured `this` before touching window state:
    ///   if (GetApp()->GetSettingsWindow() != self) return;
    ui::SettingsWindow* GetSettingsWindow() const { return settings_.get(); }

    /// Open/focus the settings window.
    void ShowSettings();

    /// Create a sticky window for the given sticky ID.
    void OpenStickyWindow(const std::string& stickyId);

    /// Close and destroy a sticky window. Safe to call from the UI thread.
    /// Must NOT be called from inside a StickyWindow's own WndProc — doing
    /// so would destroy `this` on the current stack frame. StickyWindow
    /// self-destruction paths call DestroyWindow(hwnd_) instead and rely on
    /// OnStickyWindowDestroyed below to clean up the map entry.
    void CloseStickyWindow(const std::string& stickyId);

    /// Called from StickyWindow's WM_DESTROY handler so App can erase its
    /// owning unique_ptr. The window HWND is already gone at this point;
    /// destruction of StickyWindow itself is completed by the unique_ptr's
    /// deleter once this function returns.
    void OnStickyWindowDestroyed(const std::string& stickyId);

    /// Refresh all sticky windows (e.g. after reconnect).
    void RefreshAllStickies();

    /// Reconcile the set of open sticky windows with the canonical list
    /// held by AppState. Invoked (on the UI thread) whenever
    /// AppState::onStickiesChanged_ fires — i.e. after FetchStickies(),
    /// MergeStickyUpserted, MergeStickyDeleted, or Logout.
    ///
    /// Behaviour:
    ///   • For every sticky present in state that has no window yet →
    ///     OpenStickyWindow(id).
    ///   • For every open window whose id is no longer in state (e.g.
    ///     server-driven delete that reached us via sticky.deleted, or
    ///     Logout() clearing the whole list) → post
    ///     WM_STICKYTODO_STICKY_DELETED to that window so it runs the
    ///     same self-destroy path as MergeStickyDeleted's per-sticky
    ///     fan-out. We deliberately DO NOT call CloseStickyWindow here:
    ///     a direct call would destroy the window on the current stack
    ///     frame, which is unsafe if the caller chain ever re-enters
    ///     from inside a StickyWindow WndProc in the future.
    ///
    /// Single-ownership rule: window open/close happens ONLY through
    /// this function (for the bulk reconcile path) and through
    /// MergeStickyDeleted's direct PostMessageToSticky (for the
    /// precise per-sticky delete path). Both funnel into the same
    /// WM_STICKYTODO_STICKY_DELETED self-destroy handler in
    /// StickyWindow, so there is exactly one teardown code path.
    void SyncStickyWindows();

    /// Post a Windows message to every open sticky window. Used by AppState
    /// to fan out WS events (todo.*, sticky.*) precisely to each StickyWindow's
    /// HWND. Prefer this over HWND_BROADCAST so we never leak messages to
    /// other applications' top-level windows.
    void PostMessageToAllStickies(UINT msg, WPARAM wParam = 0, LPARAM lParam = 0);

    /// Post a Windows message to the sticky window with the given id, if any.
    /// Used for sticky.upserted / sticky.deleted routing.
    void PostMessageToSticky(const std::string& stickyId, UINT msg,
                             WPARAM wParam = 0, LPARAM lParam = 0);

    /// Post WM_QUIT to terminate the message loop.
    void Quit();

private:
    HINSTANCE hInstance_ = nullptr;

    std::unique_ptr<ui::D2DRenderer> renderer_;
    std::unique_ptr<core::AppState> state_;
    std::unique_ptr<ui::TrayIcon> tray_;
    std::unique_ptr<ui::SettingsWindow> settings_;

    /// Guards stickyWindows_ across the UI thread (Open/Close) and the WS
    /// worker thread (PostMessageTo*). The map is also cleared during
    /// Shutdown() on the UI thread after WS has been disconnected.
    mutable std::mutex stickyWindowsMutex_;
    std::unordered_map<std::string, std::unique_ptr<ui::StickyWindow>> stickyWindows_;
};

/// Global app pointer (set during Initialize, cleared during Shutdown).
App* GetApp();

} // namespace stickytodo
