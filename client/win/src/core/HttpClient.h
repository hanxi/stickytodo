#pragma once

#include "models/Todo.h"
#include "models/StickyNote.h"
#include "models/AuditLog.h"
#include "models/Filter.h"

// <windows.h> is required for the HWND typedef used by
// SetUIThreadTarget(HWND) and the uiThreadTarget_ member. We could
// forward-declare `struct HWND__; typedef HWND__* HWND;` to avoid
// dragging the whole Win32 header into every translation unit that
// includes us, but the project already globally defines
// WIN32_LEAN_AND_MEAN + NOMINMAX (see CMakeLists.txt), so the
// compile-time cost is bounded and we avoid the maintenance hazard
// of a hand-rolled typedef drifting from the SDK's definition.
//
// Without this include, TUs like AppState.cpp that include HttpClient.h
// before any Win32 header fail with C2061 / C3646 on the HWND tokens.
#include <windows.h>

#include <string>
#include <vector>
#include <functional>
#include <optional>
#include <cstdint>

namespace stickytodo::core {

/// HTTP response container.
struct HttpResponse {
    int statusCode = 0;
    std::string body;
    bool IsSuccess() const { return statusCode >= 200 && statusCode < 300; }
    bool IsUnauthorized() const { return statusCode == 401; }
};

/// Login response from POST /api/login.
struct LoginResult {
    std::string token;
    std::string expires_at;
    std::string username;
};

/// Health check response from GET /health.
struct HealthResult {
    std::string status;
    std::string version;
    std::string time;
};

/// Todo list response (paginated).
struct TodoListResult {
    std::vector<models::Todo> items;
    int total = 0;
    int page = 1;
    int page_size = 50;
};

/// Sticky notes list response.
struct StickyListResult {
    std::vector<models::StickyNote> items;
};

/// REST API client using WinHTTP, exposing TWO parallel surfaces:
///
///   1) **Synchronous** methods (Login, HealthCheck, ListTodos, …) that
///      block the calling thread until the WinHTTP round trip completes.
///      These are the underlying implementation and remain safe to call
///      from any background / worker thread. **DO NOT call them from the
///      UI thread** — WinHttpSendRequest / WinHttpReceiveResponse can
///      block for the full WinHttpSetTimeouts budget (10 s × 4 steps per
///      DoRequest) which would freeze the message loop.
///
///   2) **Asynchronous** methods (AsyncHealthCheck, AsyncLogin,
///      AsyncListAuditLogs, …) that spawn a detached std::thread worker
///      running the corresponding sync method, then marshal the result
///      back onto the UI thread via PostToUIThread (see AppState.h).
///      These are safe — and mandatory — for UI-button onClick handlers.
///      Prerequisite: call SetUIThreadTarget(HWND) once during App init
///      before any Async* method fires (App::Initialize does this right
///      after creating the tray HWND — same target as WS event marshal).
///
/// Aligned with backend routes (server/internal/router/router.go) and macOS APIClient.
///
/// Endpoints:
///   GET    /health
///   POST   /api/login
///   GET    /api/todos           (query params from Filter)
///   POST   /api/todos
///   GET    /api/todos/:id
///   PUT    /api/todos/:id
///   DELETE /api/todos/:id
///   POST   /api/todos/:id/complete
///   POST   /api/todos/:id/reopen
///   POST   /api/todos/:id/restore
///   GET    /api/todos/:id/history
///   GET    /api/audit-logs
///   GET    /api/tags
///   GET    /api/sticky-notes
///   GET    /api/sticky-notes/:id
///   PUT    /api/sticky-notes/:id
///   DELETE /api/sticky-notes/:id
class HttpClient {
public:
    using UnauthorizedCallback = std::function<void()>;

    HttpClient();
    ~HttpClient();

    /// Set the server base URL (e.g. "http://localhost:8080").
    void SetBaseUrl(const std::string& url);
    std::string GetBaseUrl() const { return baseUrl_; }

    /// Set the JWT token for authenticated requests.
    void SetToken(const std::string& token);
    std::string GetToken() const { return token_; }

    /// Set callback invoked when a 401 response is received. The callback
    /// is fired on whichever thread observed the 401 — which for async
    /// paths is the worker thread. Downstream consumers that touch UI
    /// state (e.g. AppState::HandleUnauthorized) are responsible for
    /// marshalling onto the UI thread themselves; the hook stays thin on
    /// purpose so the sync surface keeps working unchanged.
    void SetOnUnauthorized(UnauthorizedCallback cb);

    /// Register the HWND that serves as the UI-thread marshal target for
    /// Async* methods. Typically the tray icon's message-only window —
    /// same HWND AppState::SetUIThreadTarget receives. Must be set
    /// before any Async* call; if it is null (or becomes null because
    /// the tray window was destroyed during shutdown), Async* workers
    /// silently **drop** their result callback — see
    /// core/UIThreadMarshal.h PostToUIThread() contract. Callbacks are
    /// NOT invoked inline on the worker thread because doing so would
    /// violate the UI-thread-only invariant of any UI state the
    /// callback tries to touch.
    void SetUIThreadTarget(HWND hwnd) { uiThreadTarget_ = hwnd; }

    // ---------- Auth (synchronous) ----------
    std::optional<LoginResult> Login(const std::string& username, const std::string& password);
    std::optional<HealthResult> HealthCheck();

    // ---------- Auth (asynchronous; callback fires on UI thread) ----------
    //
    // Common contract for every AsyncXxx method in this class:
    //
    //   • **Non-blocking**: returns immediately after spawning a
    //     detached std::thread. No handle / cancel token — callers who
    //     need cancel semantics should fold a window-scoped
    //     "still-alive" flag into the callback and gate UI mutation on
    //     it. There is no futures-based variant: we deliberately avoid
    //     std::future / std::promise<void> here to sidestep the MSVC
    //     lifetime pitfalls around promise destruction in WinHTTP
    //     error paths.
    //
    //   • **State snapshotting**: the worker lambda value-captures a
    //     snapshot of whatever state it needs (baseUrl, token,
    //     onUnauthorized_, caller args) BEFORE the thread launches. It
    //     does NOT dereference `this` inside the worker, so the
    //     HttpClient instance may safely be destroyed while the
    //     request is still in flight.
    //
    //   • **Async methods do NOT mutate `this`**. In particular
    //     AsyncLogin does NOT write the returned token back into
    //     this->token_ or this->baseUrl_ — it just delivers the
    //     LoginResult to the callback. The higher-level AppState layer
    //     is responsible for persisting the token via SetToken() from
    //     inside its own UI-thread callback. This asymmetry with the
    //     synchronous Login() (which the macOS-style AppState relied
    //     on to auto-persist) is intentional: keeping async methods
    //     side-effect-free on `*this` makes them safe to call from
    //     workers that race with `this`'s destruction.
    //
    //   • **UI-thread marshalling**: on completion the callback is
    //     handed to core::PostToUIThread(uiThreadTarget_, …). If the
    //     target HWND is null or gone, the callback is DROPPED (not
    //     invoked on the worker) — see SetUIThreadTarget rationale.
    //
    //   • **Lifetime-of-callback warning**: callbacks that capture
    //     bare `this` or raw HWND of windows that can close (e.g.
    //     SettingsWindow) must check liveness before touching UI
    //     state. The marshal guarantees UI-thread delivery but NOT
    //     that the widget you intended to update still exists.
    //
    // AsyncHealthCheck takes an explicit baseUrl (not this->baseUrl_),
    // matching the synchronous TestConnection flow where the user types
    // a URL into the Settings form BEFORE login — at that moment
    // this->baseUrl_ may still hold a stale previous server URL.
    using HealthCallback = std::function<void(std::optional<HealthResult>)>;
    using LoginCallback = std::function<void(std::optional<LoginResult>)>;
    using AuditLogsCallback = std::function<void(std::vector<models::AuditLog>)>;

    void AsyncHealthCheck(const std::string& baseUrl, HealthCallback onDone);
    void AsyncLogin(const std::string& baseUrl,
                    const std::string& username,
                    const std::string& password,
                    LoginCallback onDone);
    void AsyncListAuditLogs(int page, int pageSize,
                            const std::string& action,
                            AuditLogsCallback onDone);

    // ---------- Todos / Stickies (asynchronous) ----------
    //
    // One Async* variant per synchronous write method, plus async
    // variants of the two list endpoints (ListTodos / ListStickies).
    //
    // All callbacks follow the same convention as the sync methods:
    //   • std::optional<T> for one-object mutations — empty == failure
    //     (server rejected, HTTP error, JSON parse error)
    //   • bool for delete endpoints — true iff server returned 2xx
    //   • std::optional<TodoListResult> / StickyListResult for lists
    //     — empty == request failed (UI should keep previous items and
    //     show an error, not clear to empty)
    //
    // Same contract as AsyncLogin / AsyncListAuditLogs above:
    //   • worker detached, can outlive `this` (~40s WinHTTP budget)
    //   • snapshots baseUrl_/token_/onUnauthorized_ into the lambda
    //   • callback invoked on UI thread via PostToUIThread; dropped
    //     if uiThreadTarget_ is null / destroyed
    //   • no side effects on `*this`
    using TodoOptCallback = std::function<void(std::optional<models::Todo>)>;
    using TodoListCallback = std::function<void(std::optional<TodoListResult>)>;
    using StickyOptCallback = std::function<void(std::optional<models::StickyNote>)>;
    using StickyListCallback = std::function<void(std::optional<StickyListResult>)>;
    using BoolCallback = std::function<void(bool)>;

    void AsyncListTodos(const models::Filter& filter, TodoListCallback onDone);
    void AsyncCreateTodo(const std::string& title, int priority,
                         const std::string& tag, const std::string& dueAt,
                         TodoOptCallback onDone);
    void AsyncUpdateTodo(uint64_t id, const std::string& title,
                         int priority, const std::string& tag,
                         const std::string& dueAt,
                         TodoOptCallback onDone);
    void AsyncDeleteTodo(uint64_t id, BoolCallback onDone);
    void AsyncCompleteTodo(uint64_t id, TodoOptCallback onDone);
    void AsyncReopenTodo(uint64_t id, TodoOptCallback onDone);
    void AsyncRestoreTodo(uint64_t id, TodoOptCallback onDone);

    void AsyncListStickies(StickyListCallback onDone);
    void AsyncUpsertSticky(const std::string& id,
                           const std::string& title,
                           const std::string& bgColor,
                           const std::string& filter,
                           StickyOptCallback onDone);
    void AsyncDeleteSticky(const std::string& id, BoolCallback onDone);

    // ---------- Todos ----------
    std::optional<TodoListResult> ListTodos(const models::Filter& filter);
    std::optional<models::Todo> GetTodo(uint64_t id, bool includeDeleted = false);
    std::optional<models::Todo> CreateTodo(const std::string& title, int priority = 0,
                                            const std::string& tag = "",
                                            const std::string& dueAt = "");
    std::optional<models::Todo> UpdateTodo(uint64_t id, const std::string& title,
                                            int priority, const std::string& tag,
                                            const std::string& dueAt);
    bool DeleteTodo(uint64_t id);
    std::optional<models::Todo> CompleteTodo(uint64_t id);
    std::optional<models::Todo> ReopenTodo(uint64_t id);
    std::optional<models::Todo> RestoreTodo(uint64_t id);
    std::vector<models::AuditLog> ListTodoHistory(uint64_t id, int page = 1, int pageSize = 20);

    // ---------- Sticky Notes ----------
    std::optional<StickyListResult> ListStickies();
    std::optional<models::StickyNote> GetSticky(const std::string& id);
    std::optional<models::StickyNote> UpsertSticky(const std::string& id,
                                                    const std::string& title,
                                                    const std::string& bgColor,
                                                    const std::string& filter);
    bool DeleteSticky(const std::string& id);

    // ---------- Audit & Tags ----------
    std::vector<models::AuditLog> ListAuditLogs(int page = 1, int pageSize = 20,
                                                 const std::string& action = "");
    std::vector<std::string> ListTags();

private:
    HttpResponse DoRequest(const std::string& method, const std::string& path,
                           const std::string& body = "",
                           const std::string& queryString = "");

    std::string baseUrl_;
    std::string token_;
    UnauthorizedCallback onUnauthorized_;

    // One-shot UI-thread marshal target; see SetUIThreadTarget above.
    // Assigned from the UI thread before any Async* worker spawns; read
    // by worker threads. HWND is pointer-sized and aligned, reads/writes
    // are atomic on the platforms Win32 supports, so plain access is
    // safe for this publish-before-use pattern.
    HWND uiThreadTarget_ = nullptr;
};

} // namespace stickytodo::core
