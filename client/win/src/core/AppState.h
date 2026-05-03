#pragma once

#include "core/HttpClient.h"
#include "core/WebSocketClient.h"
#include "core/CredentialStore.h"
#include "core/FrameStore.h"
#include "core/UIThreadMarshal.h"   // For WM_STICKYTODO_RUN_ON_UI + PostToUIThread.
#include "models/StickyNote.h"
#include "models/Todo.h"

#include <windows.h>
#include <string>
#include <vector>
#include <functional>
#include <mutex>

namespace stickytodo::core {

/// Custom Windows messages for inter-component communication.
///
/// The WM_STICKYTODO_* messages fall into two routing groups:
///
/// 1) Posted to **sticky windows** by App::PostMessageTo* fan-out, so each
///    window's own WndProc reacts on the UI thread:
///      WM_STICKYTODO_REFRESH          — refetch this sticky's TODO list
///      WM_STICKYTODO_STICKY_UPSERTED  — re-read this sticky's title/bg/filter
///      WM_STICKYTODO_STICKY_DELETED   — window should self-destroy
///
/// 2) Posted to the **UI-thread target** (the tray icon's message-only HWND
///    — see AppState::SetUIThreadTarget) by the WS worker thread and by
///    HTTP async workers, to marshal their results back onto the UI
///    thread. This is the Win32 equivalent of the macOS
///    `DispatchQueue.main.async { … }` we'd otherwise want here. These
///    messages are consumed by TrayIcon's WndProc:
///      WM_STICKYTODO_WS_EVENT         — wParam: heap-allocated WsEvent*
///                                       (TrayWndProc hands it to
///                                       AppState::HandleWsEventOnUIThread
///                                       which must delete after use)
///      WM_STICKYTODO_WS_SIGNAL        — wParam: static_cast<WPARAM>(WsSignal)
///                                       (TrayWndProc forwards to
///                                       AppState::HandleWsSignalOnUIThread)
///      WM_STICKYTODO_RUN_ON_UI        — wParam: heap-allocated
///                                       std::function<void()>* (TrayWndProc
///                                       invokes then deletes). Defined in
///                                       core/UIThreadMarshal.h and used by
///                                       HttpClient's Async* wrappers (and
///                                       eventually AppState's Async API
///                                       layered on top of them) to
///                                       deliver WinHTTP result callbacks
///                                       back onto the UI thread.
constexpr UINT WM_STICKYTODO_REFRESH = WM_APP + 1;
constexpr UINT WM_STICKYTODO_STICKY_UPSERTED = WM_APP + 2;
constexpr UINT WM_STICKYTODO_STICKY_DELETED = WM_APP + 3;
constexpr UINT WM_STICKYTODO_WS_EVENT = WM_APP + 4;
constexpr UINT WM_STICKYTODO_WS_SIGNAL = WM_APP + 5;
// WM_STICKYTODO_RUN_ON_UI (= WM_APP + 6) is defined in
// core/UIThreadMarshal.h alongside its PostToUIThread() helper, so that
// translation units that only need the marshalling primitive (HttpClient,
// TrayIcon) can pull in the small header without dragging in the whole
// AppState surface. The constant is still logically part of this block's
// numeric assignment map — keep future WM_STICKYTODO_* additions at
// WM_APP + 7 or higher to avoid collisions.

/// Central application state manager.
/// Owns HttpClient, WebSocketClient, CredentialStore, FrameStore.
/// Analogous to macOS AppState (client/mac/stickytodo/AppState.swift).
class AppState {
public:
    using StickiesChangedCallback = std::function<void()>;
    using AuthChangedCallback = std::function<void(bool isAuthenticated)>;

    AppState();
    ~AppState();

    /// Initialize: attempt to restore saved session from Credential Manager.
    void Initialize();

    /// Tear down: disconnect WebSocket, release resources.
    void Shutdown();

    // ---------- Auth ----------
    bool IsAuthenticated() const { return !token_.empty(); }
    std::string GetUsername() const { return username_; }
    std::string GetToken() const { return token_; }
    std::string GetBaseUrl() const { return baseUrl_; }

    /// Attempt login. Returns true on success.
    ///
    /// **Blocking.** Calls HttpClient::Login (which can block up to
    /// ~40 s on a misconfigured server, per WinHttpSetTimeouts). DO
    /// NOT call from the UI thread — use LoginAsync instead. Kept on
    /// the public surface for tests / headless tooling that drive
    /// AppState from a worker thread.
    bool Login(const std::string& baseUrl, const std::string& username, const std::string& password);

    /// Log out: clear token, disconnect WS, clear data.
    ///
    /// No network round-trip, so this is safe to call on the UI
    /// thread. DisconnectWebSocket joins the WS worker thread
    /// synchronously but that join is bounded by WinHttpCloseHandle
    /// returning promptly (see WebSocketClient::Disconnect) — not by
    /// an HTTP timeout.
    void Logout();

    /// Test connection to server (GET /health). Returns version string or empty on failure.
    ///
    /// **Blocking.** Same warning as Login() — prefer
    /// TestConnectionAsync from UI code.
    std::string TestConnection(const std::string& baseUrl);

    // ---------- Auth (asynchronous; callback fires on UI thread) ----------
    //
    // These are the UI-safe counterparts of Login / TestConnection /
    // HttpClient::ListAuditLogs. They fan out to HttpClient's Async*
    // primitives, then — when delivery lands back on the UI thread —
    // apply the required AppState-level side effects (token
    // persistence, WS reconnect, onAuthChanged_ fan-out) BEFORE
    // invoking the caller's completion callback.
    //
    // Callback threading contract: the callback runs on the UI thread
    // (same as TrayIcon WndProc) when the tray HWND is still alive;
    // if the HWND is gone (app shutting down) the callback is
    // DROPPED — see core::PostToUIThread in UIThreadMarshal.h. UI
    // callers must therefore be ready for the callback to never fire
    // during a shutdown race, and must not rely on it for teardown
    // sequencing.
    //
    // Lifetime warning: callers that capture `this` of a UI window in
    // their callback MUST verify the window is still alive before
    // touching its state (the marshal only guarantees UI-thread
    // delivery, not widget liveness). See the GetSettingsWindow()
    // contract in App.h.
    using TestConnectionCallback = std::function<void(std::string version)>;
    using LoginAsyncCallback = std::function<void(bool success)>;
    using ListAuditLogsCallback = std::function<void(std::vector<models::AuditLog>)>;

    /// Asynchronous health-probe. The callback's `version` string is
    /// empty on failure, matching the sync TestConnection contract.
    void TestConnectionAsync(const std::string& baseUrl,
                             TestConnectionCallback onDone);

    /// Asynchronous login. On the UI thread, on success: persists
    /// credentials to Credential Manager, sets http_ token, fires
    /// onAuthChanged_(true), and initiates WS connection — i.e. the
    /// exact same side-effect sequence the blocking Login() applies,
    /// but executed on the UI thread after the HTTP round-trip
    /// finished on a worker thread. THEN invokes `onDone(true)`. On
    /// failure, only invokes `onDone(false)` — no state is mutated.
    void LoginAsync(const std::string& baseUrl,
                    const std::string& username,
                    const std::string& password,
                    LoginAsyncCallback onDone);

    /// Asynchronous audit log fetch. No side effects on AppState —
    /// this is a pure query, purely delegated to http_.AsyncListAuditLogs
    /// with the caller's callback relayed through.
    void ListAuditLogsAsync(int page, int pageSize,
                            const std::string& action,
                            ListAuditLogsCallback onDone);

    using FetchStickiesAsyncCallback = std::function<void(bool success)>;
    using UpsertStickyAsyncCallback = std::function<void(std::optional<models::StickyNote>)>;

    /// Asynchronous sticky list fetch. On the UI thread, on success:
    /// updates stickies_ (under dataMutex_) and fires onStickiesChanged_
    /// — i.e. the exact same side-effect sequence the blocking
    /// FetchStickies() applies. THEN invokes `onDone(true)`. On
    /// failure, stickies_ is left untouched and `onDone(false)`.
    ///
    /// Use this from UI-triggered refresh paths (NEW_STICKY menu
    /// handler, user-driven reload). The blocking FetchStickies()
    /// is still used by App::Initialize for the post-login
    /// bootstrap fetch (which runs before any UI interaction is
    /// possible, so blocking the bootstrap caller — the UI thread
    /// at startup — is acceptable there).
    void FetchStickiesAsync(FetchStickiesAsyncCallback onDone);

    /// Asynchronous sticky upsert. Pure delegation to
    /// http_.AsyncUpsertSticky — no AppState-level side effects; the
    /// authoritative stickies_ list is refreshed via the server's
    /// sticky.upserted WS event (which lands on the UI thread through
    /// the existing WS marshal path and calls MergeStickyUpserted).
    /// Callers that want the fresh list locally before WS echoes back
    /// should call FetchStickiesAsync in their own callback.
    void UpsertStickyAsync(const std::string& id,
                           const std::string& title,
                           const std::string& bgColor,
                           const std::string& filter,
                           UpsertStickyAsyncCallback onDone);

    // ---------- Data ----------
    HttpClient* GetHttp() { return &http_; }
    FrameStore* GetFrameStore() { return &frameStore_; }

    /// Fetch sticky notes from server. Updates internal list.
    /// MUST be called from the UI thread (because it invokes
    /// onStickiesChanged_, which drives HWND creation in App).
    void FetchStickies();

    /// Get the current cached list of sticky notes. Callers on threads
    /// other than the UI thread must hold dataMutex_ themselves if they
    /// need a stable snapshot; UI-thread callers already have the
    /// invariant that mutation happens on their own thread.
    std::vector<models::StickyNote> GetStickies() const;

    // ---------- Callbacks ----------
    void SetOnStickiesChanged(StickiesChangedCallback cb) { onStickiesChanged_ = std::move(cb); }
    void SetOnAuthChanged(AuthChangedCallback cb) { onAuthChanged_ = std::move(cb); }

    /// Register the HWND that receives marshalling messages from the WS
    /// worker thread. Typically the tray icon's message-only window
    /// (`TrayIcon::GetMessageHwnd()`), which is created on and pumped by
    /// the UI thread. Must be called before Connect()/Login() so that by
    /// the time WS events arrive there is somewhere to post them.
    void SetUIThreadTarget(HWND hwnd) { uiThreadTarget_ = hwnd; }

    /// UI-thread entry points invoked by TrayIcon's WndProc after it
    /// receives the marshalled WM_STICKYTODO_WS_EVENT / _SIGNAL messages.
    /// Calling these directly from the worker thread would be a bug.
    void HandleWsEventOnUIThread(const WsEvent& event);
    void HandleWsSignalOnUIThread(WsSignal signal);

private:
    void ConnectWebSocket();
    void DisconnectWebSocket();
    // Worker-thread callbacks — they ONLY do PostMessageW marshalling.
    void PostWsSignalToUIThread(WsSignal signal);
    void PostWsEventToUIThread(const WsEvent& event);
    // UI-thread state mutation (safe to touch HWND state, `stickies_`, etc.).
    void HandleUnauthorized();
    void MergeStickyUpserted(const std::string& jsonData);
    void MergeStickyDeleted(const std::string& id);

    /// Shared post-Login side-effect sequence used by BOTH the sync
    /// Login() path (worker/test callers) and the LoginAsync() UI-thread
    /// callback. Factoring avoids drift between the two: whenever a new
    /// side-effect is added to login success, it gets picked up by
    /// both paths automatically.
    ///
    /// Must be called on the thread that owns onAuthChanged_ — for
    /// production that's the UI thread. It mutates baseUrl_ /
    /// username_ / token_, calls http_.SetToken, persists via
    /// CredentialStore::Save, fires onAuthChanged_(true), and spawns
    /// the WS connection.
    void ApplyLoginSuccess(const std::string& baseUrl,
                           const LoginResult& result);

    HttpClient http_;
    WebSocketClient ws_;
    FrameStore frameStore_;

    std::string baseUrl_;
    std::string username_;
    std::string token_;

    std::vector<models::StickyNote> stickies_;
    mutable std::mutex dataMutex_;

    StickiesChangedCallback onStickiesChanged_;
    AuthChangedCallback onAuthChanged_;

    // Set by App::Initialize (UI thread) before Connect(), read by the
    // WS worker thread. HWND is a pointer-sized handle value and its
    // assignment is atomic on all platforms Win32 supports, so plain
    // read/write is safe for this one-shot publish-before-use pattern.
    HWND uiThreadTarget_ = nullptr;
};

} // namespace stickytodo::core
