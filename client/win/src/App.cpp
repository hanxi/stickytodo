#include "App.h"
#include "core/AppState.h"
#include "ui/D2DRenderer.h"
#include "ui/TrayIcon.h"
#include "ui/SettingsWindow.h"
#include "ui/StickyWindow.h"

#include <windows.h>
#include <vector>
#include <unordered_set>

namespace stickytodo {

static App* g_app = nullptr;

App* GetApp() { return g_app; }

App::App(HINSTANCE hInstance)
    : hInstance_(hInstance)
{
}

App::~App() = default;

bool App::Initialize(int /*nCmdShow*/)
{
    g_app = this;

    // 1) Renderer first — stateless, no HWND ownership. Must be up before
    //    any window creation because StickyWindow / SettingsWindow use its
    //    shared D2D / DWrite factories during CreateRenderTarget.
    renderer_ = std::make_unique<ui::D2DRenderer>();
    if (!renderer_->Initialize()) {
        return false;
    }

    // 2) AppState instance — construct now but DO NOT call Initialize yet.
    //    Initialize() internally calls ConnectWebSocket() if stored
    //    credentials are present, and WS events must be marshalled to a
    //    UI-thread HWND that does not yet exist at this point.
    state_ = std::make_unique<core::AppState>();

    // 3) Tray icon — creates the message-only HWND that serves as the
    //    UI-thread marshalling target for WS events.
    tray_ = std::make_unique<ui::TrayIcon>(hInstance_);
    if (!tray_->Create()) {
        return false;
    }

    // 4) Wire AppState → UI thread. This MUST happen before
    //    state_->Initialize() so that any WS events triggered by the
    //    initial (auto-resumed) connection have somewhere to land. The
    //    one-shot publish of uiThreadTarget_ happens before
    //    ConnectWebSocket spawns the worker thread, so there's no data
    //    race between the UI thread's write and the worker thread's read.
    //
    //    AppState owns HttpClient by value (AppState::http_). We publish
    //    the SAME tray HWND into HttpClient so its Async* methods (used
    //    by SettingsWindow's Test / Login / LoadAuditLogs buttons and
    //    eventually by AppState's own async API) can marshal their
    //    completion callbacks back onto the UI thread via the same
    //    PostToUIThread path. If this line is missing, every Async*
    //    callback is silently dropped — see HttpClient::SetUIThreadTarget
    //    contract in HttpClient.h.
    HWND uiTarget = tray_->GetMessageHwnd();
    state_->SetUIThreadTarget(uiTarget);
    state_->GetHttp()->SetUIThreadTarget(uiTarget);

    // 5) Subscribe to the stickies-changed callback. Every code path that
    //    mutates the authoritative sticky list (FetchStickies,
    //    MergeStickyUpserted, MergeStickyDeleted, Logout) fires this, so
    //    SyncStickyWindows is the single funnel that opens new windows
    //    and tears down stale ones.
    state_->SetOnStickiesChanged([this]() { SyncStickyWindows(); });

    // 6) Now it is safe to restore session + connect WS. If token was
    //    previously saved, AppState::Initialize will auto-connect. The
    //    server's `ready` signal (or the subsequent `FetchStickies` we
    //    do below) will drive SyncStickyWindows to open windows.
    state_->Initialize();

    // 7) If we had a session restored, proactively pull stickies now —
    //    this lets windows appear even before the WS "ready" signal
    //    arrives, and even when WS connection is still flapping. The
    //    later "ready" callback also calls FetchStickies; the duplicate
    //    cost is exactly one extra ListStickies HTTP round trip in the
    //    happy path, which is acceptable and keeps cold-start snappy.
    if (state_->IsAuthenticated()) {
        state_->FetchStickies();
    }

    return true;
}

void App::Shutdown()
{
    // Teardown order is the reverse of Initialize(), and every step
    // matters for the detached HTTP async workers to land safely.
    // The detached std::thread workers spawned by
    // HttpClient::AsyncHealthCheck / AsyncLogin / AsyncListAuditLogs
    // may still be mid-WinHTTP (up to ~40s per request) when we
    // start tearing down; they eventually come back and call
    // core::PostToUIThread on their captured HWND. We must make
    // both "still running" and "just finished" cases non-leaking
    // and non-crashing.
    //
    // 1) Stop the WS worker (joins inside WebSocketClient::Disconnect).
    //    After state_->Shutdown() returns, no more WS-originated
    //    PostMessageW fires. HTTP async workers may still be
    //    mid-flight — that's fine, they target the tray HWND which
    //    is still alive here.
    if (state_) {
        state_->Shutdown();
    }

    // 2) "Cut off new arrivals" — null out the UI-thread targets
    //    inside AppState and HttpClient. Any HTTP async worker that
    //    finishes AFTER this point sees a null target in
    //    core::PostToUIThread, which returns false, frees the
    //    heap-allocated std::function, and drops the callback.
    //    This has to run BEFORE tray_->Destroy() so that a worker
    //    completing during the window between here and the drain
    //    below cannot add new enqueued messages we'd then fail to
    //    handle.
    if (state_) {
        state_->SetUIThreadTarget(nullptr);
        if (auto* http = state_->GetHttp()) {
            http->SetUIThreadTarget(nullptr);
        }
    }

    // 3) "Drain already-enqueued messages" — by the time we reach
    //    step 2, zero to N  WM_STICKYTODO_RUN_ON_UI messages may
    //    already sit in the tray HWND's queue (posted by HTTP
    //    workers that completed earlier but whose messages the main
    //    loop never got to process because the user quit). If we
    //    went straight to DestroyWindow, those would be discarded
    //    by Win32 and their heap-allocated std::function payloads
    //    would leak.
    //
    //    PeekMessageW with PM_REMOVE pulls each pending message off
    //    the queue; DispatchMessageW routes it to TrayIcon::TrayWndProc
    //    which runs its WM_STICKYTODO_RUN_ON_UI branch (invoke then
    //    delete). We only drain our own custom messages, so
    //    DispatchMessageW doesn't trigger unrelated side-effects.
    //
    //    There's no loop bound issue: the set of drainable messages
    //    is finite because step 2 blocked new additions. We pass
    //    both the tray HWND and the message filter range to
    //    PeekMessageW so we don't accidentally dispatch unrelated
    //    windows' messages (sticky windows / settings window may
    //    still have queued repaint messages and we want those
    //    discarded on their own destroy).
    if (tray_) {
        HWND trayHwnd = tray_->GetMessageHwnd();
        if (trayHwnd) {
            MSG msg;
            while (PeekMessageW(&msg, trayHwnd,
                                core::WM_STICKYTODO_RUN_ON_UI,
                                core::WM_STICKYTODO_RUN_ON_UI,
                                PM_REMOVE)) {
                DispatchMessageW(&msg);
            }
            // Also drain the WS event/signal messages for the same
            // reason — state_->Shutdown already joined the WS
            // worker so no new WS messages can be posted, but
            // previously-enqueued ones still need their WsEvent
            // heap allocations freed by the TrayWndProc branch.
            while (PeekMessageW(&msg, trayHwnd,
                                core::WM_STICKYTODO_WS_EVENT,
                                core::WM_STICKYTODO_WS_EVENT,
                                PM_REMOVE)) {
                DispatchMessageW(&msg);
            }
            while (PeekMessageW(&msg, trayHwnd,
                                core::WM_STICKYTODO_WS_SIGNAL,
                                core::WM_STICKYTODO_WS_SIGNAL,
                                PM_REMOVE)) {
                DispatchMessageW(&msg);
            }
        }
    }

    // 4) Destroy the tray icon (and its message-only HWND). Safe now:
    //    no new messages can be posted (step 2 nulled the target),
    //    and all previously-enqueued ones have been drained (step 3).
    //    Any HTTP worker that finishes from this point on will find
    //    the target HWND already nulled inside HttpClient/AppState
    //    and drop its callback without touching Win32 at all.
    if (tray_) {
        tray_->Destroy();
        tray_.reset();
    }

    // 5) Settings window — independent, just a normal top-level window.
    settings_.reset();

    // 6) Close all sticky windows. Lock is taken for form (no concurrent
    //    writer remains at this point because WS is dead and the UI
    //    thread is calling Shutdown), but cheap and clarifies intent.
    {
        std::lock_guard<std::mutex> lock(stickyWindowsMutex_);
        stickyWindows_.clear();
    }

    // 7) Finally drop AppState itself (its destructor is trivial at this
    //    point because Shutdown already disconnected the WS).
    //
    //    Note on detached HTTP workers: at this point up to ~40 s of
    //    worker lifetime may remain (WinHTTP timeout budget). They
    //    hold value-captured snapshots (baseUrl, token, callback
    //    lambdas) on their own stack — NOT references into state_
    //    or http_ — so dropping state_ here is safe. The process
    //    itself will tear the workers down as part of ExitProcess
    //    right after WinMain returns; no clean join is attempted
    //    because std::thread::detach was a deliberate choice (see
    //    HttpClient.cpp's async implementation contract).
    state_.reset();

    // 8) Release D2D resources last — everything above that drew into
    //    a render target is gone.
    renderer_.reset();

    g_app = nullptr;
}

void App::ShowSettings()
{
    if (!settings_) {
        settings_ = std::make_unique<ui::SettingsWindow>(hInstance_);
    }
    settings_->Show();
}

void App::OpenStickyWindow(const std::string& stickyId)
{
    // Must be called on the UI thread. Creating a StickyWindow is a Win32 HWND
    // creation, which is only valid from the thread that will pump the window's
    // messages.
    {
        std::lock_guard<std::mutex> lock(stickyWindowsMutex_);
        auto it = stickyWindows_.find(stickyId);
        if (it != stickyWindows_.end()) {
            it->second->BringToFront();
            return;
        }
    }

    auto win = std::make_unique<ui::StickyWindow>(hInstance_, stickyId);
    if (!win->Create()) return;

    std::lock_guard<std::mutex> lock(stickyWindowsMutex_);
    stickyWindows_[stickyId] = std::move(win);
}

void App::CloseStickyWindow(const std::string& stickyId)
{
    // Destruction order matters: the StickyWindow destructor calls
    // DestroyWindow(), which must happen on the UI thread. Move the unique_ptr
    // out under the lock, then drop the lock before destroying so we don't
    // hold the lock during arbitrary destructor work (including D2D release).
    std::unique_ptr<ui::StickyWindow> condemned;
    {
        std::lock_guard<std::mutex> lock(stickyWindowsMutex_);
        auto it = stickyWindows_.find(stickyId);
        if (it == stickyWindows_.end()) return;
        condemned = std::move(it->second);
        stickyWindows_.erase(it);
    }
    // condemned's destructor runs here.
}

void App::OnStickyWindowDestroyed(const std::string& stickyId)
{
    // Invoked from the StickyWindow's own WM_DESTROY handler. Win32 guarantees
    // that by the time WM_DESTROY fires, no further messages will arrive for
    // the window, so erasing the unique_ptr now is safe with respect to the
    // message pump. Moving the unique_ptr out before erasing lets us run
    // ~StickyWindow (which itself does nothing harmful — hwnd_ is already
    // cleared by the WM_DESTROY handler before it called us) outside the map
    // lock.
    std::unique_ptr<ui::StickyWindow> condemned;
    {
        std::lock_guard<std::mutex> lock(stickyWindowsMutex_);
        auto it = stickyWindows_.find(stickyId);
        if (it == stickyWindows_.end()) return;
        condemned = std::move(it->second);
        stickyWindows_.erase(it);
    }
}

void App::RefreshAllStickies()
{
    // UI-thread only. Take a snapshot of HWNDs under the lock, then dispatch
    // Refresh() outside the lock to avoid holding it during arbitrary work.
    std::vector<ui::StickyWindow*> snapshot;
    {
        std::lock_guard<std::mutex> lock(stickyWindowsMutex_);
        snapshot.reserve(stickyWindows_.size());
        for (auto& [id, win] : stickyWindows_) {
            snapshot.push_back(win.get());
        }
    }
    for (auto* win : snapshot) {
        win->Refresh();
    }
}

void App::PostMessageToAllStickies(UINT msg, WPARAM wParam, LPARAM lParam)
{
    // Current callers are all on the UI thread (e.g. AppState's
    // HandleWsEventOnUIThread, which is invoked via TrayIcon's WndProc
    // after PostMessageW marshalling from the WS worker thread). The
    // lock-then-snapshot design is still worthwhile regardless: (a) it
    // makes the function defensible should a future caller appear on a
    // non-UI thread — PostMessageW itself is documented thread-safe by
    // Win32; (b) we avoid holding the lock while the message pump
    // processes side effects. PostMessageW never re-enters WndProcs, so
    // the "dispatch outside the lock" guarantee here is more about
    // discipline than correctness for the current set of callers.
    std::vector<HWND> targets;
    {
        std::lock_guard<std::mutex> lock(stickyWindowsMutex_);
        targets.reserve(stickyWindows_.size());
        for (auto& [id, win] : stickyWindows_) {
            HWND h = win->GetHwnd();
            if (h) targets.push_back(h);
        }
    }
    for (HWND h : targets) {
        PostMessageW(h, msg, wParam, lParam);
    }
}

void App::PostMessageToSticky(const std::string& stickyId, UINT msg,
                              WPARAM wParam, LPARAM lParam)
{
    // Same UI-thread-today-but-defensive-lock contract as
    // PostMessageToAllStickies above.
    HWND target = nullptr;
    {
        std::lock_guard<std::mutex> lock(stickyWindowsMutex_);
        auto it = stickyWindows_.find(stickyId);
        if (it == stickyWindows_.end()) return;
        target = it->second->GetHwnd();
    }
    if (target) PostMessageW(target, msg, wParam, lParam);
}

void App::SyncStickyWindows()
{
    // Called on the UI thread after any authoritative mutation of the
    // sticky list (FetchStickies / MergeStickyUpserted /
    // MergeStickyDeleted / Logout). Computes the two set differences
    // between AppState's stickies and our open windows:
    //   • stickies ∉ windows → OpenStickyWindow(id)
    //   • windows ∉ stickies → post WM_STICKYTODO_STICKY_DELETED
    //
    // The "missing → post close message" path intentionally does NOT
    // call CloseStickyWindow directly. Having a single self-destroy
    // handler inside StickyWindow::HandleMessage ensures every teardown
    // (per-sticky WS delete, logout-time mass close, server-side UI
    // reconcile) runs exactly the same code, saving frame positions and
    // running the WM_DESTROY cleanup uniformly. See App.h docstring on
    // SyncStickyWindows for the "single ownership" rationale.
    if (!state_) return;
    auto stickies = state_->GetStickies();  // value copy; list is small

    // Build a set of canonical IDs.
    std::unordered_set<std::string> canonicalIds;
    canonicalIds.reserve(stickies.size());
    for (const auto& note : stickies) {
        canonicalIds.insert(note.id);
    }

    // Snapshot the window-id → HWND pairs that should close. Do it under
    // the lock but defer the actual PostMessageW until after releasing,
    // so we never hold the map lock while we might re-enter via the
    // message pump (defensive even though PostMessageW itself never
    // pumps).
    std::vector<HWND> toClose;
    {
        std::lock_guard<std::mutex> lock(stickyWindowsMutex_);
        toClose.reserve(stickyWindows_.size());
        for (auto& [id, win] : stickyWindows_) {
            if (canonicalIds.find(id) == canonicalIds.end()) {
                if (HWND h = win->GetHwnd(); h) {
                    toClose.push_back(h);
                }
            }
        }
    }
    for (HWND h : toClose) {
        // WM_STICKYTODO_STICKY_DELETED → StickyWindow handler calls
        // DestroyWindow(hwnd_) → WM_DESTROY → OnStickyWindowDestroyed
        // erases the unique_ptr from stickyWindows_. The whole chain
        // runs on the UI thread via message pump, so there is no lock
        // contention here.
        PostMessageW(h, core::WM_STICKYTODO_STICKY_DELETED, 0, 0);
    }

    // Open any sticky that doesn't yet have a window. OpenStickyWindow
    // itself de-dupes by id under the lock, so double-calling it during
    // a reconcile storm is harmless. We issue each call outside the
    // lock to avoid holding it during CreateWindowExW (which can pump
    // messages internally).
    for (const auto& note : stickies) {
        bool needOpen = false;
        {
            std::lock_guard<std::mutex> lock(stickyWindowsMutex_);
            needOpen = (stickyWindows_.find(note.id) == stickyWindows_.end());
        }
        if (needOpen) {
            OpenStickyWindow(note.id);
        }
    }
}

void App::Quit()
{
    PostQuitMessage(0);
}

} // namespace stickytodo
