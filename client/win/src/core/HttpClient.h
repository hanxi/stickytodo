#pragma once

#include "models/Todo.h"
#include "models/StickyNote.h"
#include "models/AuditLog.h"
#include "models/Filter.h"

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

/// REST API client using WinHTTP (synchronous, called from background thread or co_await).
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

    /// Set callback invoked when a 401 response is received.
    void SetOnUnauthorized(UnauthorizedCallback cb);

    // ---------- Auth ----------
    std::optional<LoginResult> Login(const std::string& username, const std::string& password);
    std::optional<HealthResult> HealthCheck();

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
};

} // namespace stickytodo::core
