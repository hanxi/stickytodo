#pragma once

#include "core/HttpClient.h"
#include "core/WebSocketClient.h"
#include "core/CredentialStore.h"
#include "core/FrameStore.h"
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
///    — see AppState::SetUIThreadTarget) by the WS worker thread, to marshal
///    events back onto the UI thread. This is the Win32 equivalent of the
///    macOS `DispatchQueue.main.async { … }` we'd otherwise want here. These
///    messages are consumed by TrayIcon's WndProc, which then calls back
///    into AppState::HandleWsEventOnUIThread / HandleWsSignalOnUIThread:
///      WM_STICKYTODO_WS_EVENT         — wParam: heap-allocated WsEvent*
///                                       (receiver takes ownership, must
///                                       delete after handling)
///      WM_STICKYTODO_WS_SIGNAL        — wParam: static_cast<WPARAM>(WsSignal)
constexpr UINT WM_STICKYTODO_REFRESH = WM_APP + 1;
constexpr UINT WM_STICKYTODO_STICKY_UPSERTED = WM_APP + 2;
constexpr UINT WM_STICKYTODO_STICKY_DELETED = WM_APP + 3;
constexpr UINT WM_STICKYTODO_WS_EVENT = WM_APP + 4;
constexpr UINT WM_STICKYTODO_WS_SIGNAL = WM_APP + 5;

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
    bool Login(const std::string& baseUrl, const std::string& username, const std::string& password);

    /// Log out: clear token, disconnect WS, clear data.
    void Logout();

    /// Test connection to server (GET /health). Returns version string or empty on failure.
    std::string TestConnection(const std::string& baseUrl);

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
