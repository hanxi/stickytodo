#include "core/HttpClient.h"
#include "core/UIThreadMarshal.h" // For PostToUIThread().
#include "codec/JsonHelper.h"
#include "codec/StickyCodec.h"

#include <nlohmann/json.hpp>
#include <windows.h>
#include <winhttp.h>
#include <sstream>
#include <algorithm>
#include <thread>
#include <utility>

#pragma comment(lib, "winhttp.lib")

namespace stickytodo::core {

// ---------- URL parsing helper ----------
struct ParsedUrl {
    std::wstring host;
    INTERNET_PORT port = 80;
    bool useHttps = false;
    std::wstring path;
};

static std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int sz = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), nullptr, 0);
    std::wstring result(sz, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), result.data(), sz);
    return result;
}

static std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int sz = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    std::string result(sz, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), result.data(), sz, nullptr, nullptr);
    return result;
}

static ParsedUrl ParseUrl(const std::string& baseUrl, const std::string& path, const std::string& query) {
    ParsedUrl result;

    std::string url = baseUrl;
    // Remove trailing slash from the entire baseUrl (applied AFTER path
    // extraction so we don't accidentally trim a meaningful "/" inside a
    // base URL like "http://host/api/").
    while (!url.empty() && url.back() == '/') url.pop_back();

    // Determine scheme
    if (url.find("https://") == 0) {
        result.useHttps = true;
        url = url.substr(8);
        result.port = 443;
    } else if (url.find("http://") == 0) {
        result.useHttps = false;
        url = url.substr(7);
        result.port = 80;
    }

    // P1 FIX — split host[:port] from any trailing path carried in baseUrl.
    //
    // Previously ParseUrl treated the whole `url` remainder as host:port,
    // silently dropping any path prefix the user typed into Settings → Server
    // URL. So a base URL like "http://127.0.0.1:8080/api" would:
    //   (a) set host = "127.0.0.1:8080/api"  — the `:` / port scan below
    //       would misparse "8080/api" and either throw (std::stoi) or pick
    //       a nonsense port, depending on compiler behaviour;
    //   (b) silently lose "/api" so every subsequent request hit the wrong
    //       path on the server, producing "Connection failed" in the UI
    //       even when the server was perfectly reachable.
    //
    // Correct behaviour: split on the first '/' AFTER the scheme. Whatever
    // precedes the slash is host[:port]; whatever follows becomes a base
    // path that must be PREPENDED to every per-request path.
    std::string basePath;
    auto slashPos = url.find('/');
    if (slashPos != std::string::npos) {
        basePath = url.substr(slashPos);       // e.g. "/api" (keeps leading /)
        url = url.substr(0, slashPos);         // host[:port] only
    }

    // Extract host:port
    auto colonPos = url.find(':');
    if (colonPos != std::string::npos) {
        result.host = Utf8ToWide(url.substr(0, colonPos));
        try {
            result.port = static_cast<INTERNET_PORT>(std::stoi(url.substr(colonPos + 1)));
        } catch (...) {
            // Fall back to scheme default port — stoi throwing here means
            // the user typed something like "host:foo", no point crashing
            // the whole app over a typo in a settings field.
            // result.port already holds 80/443 from the scheme branch above.
        }
    } else {
        result.host = Utf8ToWide(url);
    }

    // Build final path = basePath (from baseUrl) + caller-supplied path.
    // Caller paths always start with "/" by convention (see every Do*
    // call site below — "/api/login", "/health", etc.), so simple string
    // concat avoids the "//" / missing-slash corner cases.
    std::string fullPath = basePath + path;
    if (!query.empty()) {
        fullPath += "?" + query;
    }
    result.path = Utf8ToWide(fullPath);

    return result;
}

// ---------- Default WinHTTP timeouts ----------
//
// Values are milliseconds, 10 000 ms each for (resolve, connect, send,
// receive). Rationale:
//   - WinHTTP's system defaults are absurdly long for interactive UI flows:
//     60 s resolve + 60 s connect + 30 s send + 30 s receive, summing to
//     a worst case of 3 minutes where a single stuck request could freeze
//     an unresponsive-feeling UI (in sync code paths) or leave async
//     callers waiting past any reasonable user patience window.
//   - 10 s is the Goldilocks pick per user direction: fast enough that a
//     dead server or mis-typed URL fails "now-ish" (users will retry
//     rather than assume the app is broken), but long enough to tolerate
//     ordinary cross-region LAN/VPN latency plus a slow TLS handshake.
//     Matches the macOS client's URLSession timeoutIntervalForRequest.
//   - Uniform 10 s (not a skewed profile like resolve=3s/connect=5s) on
//     purpose: user wanted simplicity, and WinHTTP's timeout semantics
//     are not strictly additive (resolve + connect are sequential for a
//     cold DNS, but send/receive overlap with server-side processing),
//     so a single number is easier to reason about than four.
//
// If this becomes a hotspot (e.g. we need different timeouts for /health
// vs a large POST), split via a per-call override in DoRequest and keep
// these as the default fallback.
static constexpr int kResolveTimeoutMs = 10'000;
static constexpr int kConnectTimeoutMs = 10'000;
static constexpr int kSendTimeoutMs    = 10'000;
static constexpr int kReceiveTimeoutMs = 10'000;

// ---------- HttpClient ----------

HttpClient::HttpClient() = default;
HttpClient::~HttpClient() = default;

void HttpClient::SetBaseUrl(const std::string& url) { baseUrl_ = url; }
void HttpClient::SetToken(const std::string& token) { token_ = token; }
void HttpClient::SetOnUnauthorized(UnauthorizedCallback cb) { onUnauthorized_ = std::move(cb); }

HttpResponse HttpClient::DoRequest(const std::string& method, const std::string& path,
                                    const std::string& body, const std::string& queryString) {
    HttpResponse response;
    if (baseUrl_.empty()) {
        response.statusCode = 0;
        return response;
    }

    auto parsed = ParseUrl(baseUrl_, path, queryString);

    HINTERNET hSession = WinHttpOpen(L"StickyTodo/1.0",
                                      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return response;

    // Apply short, interactive-grade timeouts to the whole session. This
    // MUST precede WinHttpConnect — WinHttpSetTimeouts on a session handle
    // is inherited by every request handle created from it, but only for
    // handles created AFTER the call. (Docs: "Sets time-outs ... timeouts
    // are applied to subsequent HTTP requests.")
    //
    // Return value of WinHttpSetTimeouts is intentionally unchecked: on
    // failure the handle falls back to the platform default, which is
    // strictly a worse experience but not a correctness bug.
    WinHttpSetTimeouts(hSession, kResolveTimeoutMs, kConnectTimeoutMs,
                       kSendTimeoutMs, kReceiveTimeoutMs);

    HINTERNET hConnect = WinHttpConnect(hSession, parsed.host.c_str(), parsed.port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return response; }

    std::wstring wMethod = Utf8ToWide(method);
    DWORD flags = parsed.useHttps ? WINHTTP_FLAG_SECURE : 0;

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, wMethod.c_str(),
                                             parsed.path.c_str(),
                                             nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return response;
    }

    // Add headers
    std::wstring headers = L"Content-Type: application/json\r\n";
    if (!token_.empty()) {
        headers += L"Authorization: Bearer " + Utf8ToWide(token_) + L"\r\n";
    }
    WinHttpAddRequestHeaders(hRequest, headers.c_str(),
                              static_cast<DWORD>(headers.size()),
                              WINHTTP_ADDREQ_FLAG_ADD);

    // Send request
    LPVOID bodyData = body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data());
    DWORD bodyLen = static_cast<DWORD>(body.size());

    BOOL sendOk = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                      bodyData, bodyLen, bodyLen, 0);
    if (!sendOk) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return response;
    }

    // Receive response
    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return response;
    }

    // Get status code
    DWORD statusCode = 0;
    DWORD size = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                         WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &size,
                         WINHTTP_NO_HEADER_INDEX);
    response.statusCode = static_cast<int>(statusCode);

    // Read body
    std::string responseBody;
    DWORD bytesAvailable = 0;
    while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
        std::vector<char> buffer(bytesAvailable);
        DWORD bytesRead = 0;
        WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead);
        responseBody.append(buffer.data(), bytesRead);
    }
    response.body = std::move(responseBody);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    // Handle 401
    if (response.IsUnauthorized() && onUnauthorized_) {
        onUnauthorized_();
    }

    return response;
}

// ---------- Auth ----------

std::optional<LoginResult> HttpClient::Login(const std::string& username, const std::string& password) {
    nlohmann::json body;
    body["username"] = username;
    body["password"] = password;

    auto resp = DoRequest("POST", "/api/login", body.dump());
    if (!resp.IsSuccess()) return std::nullopt;

    try {
        auto j = nlohmann::json::parse(resp.body);
        LoginResult result;
        result.token = j.value("token", "");
        result.expires_at = j.value("expires_at", "");
        result.username = j.value("username", "");
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<HealthResult> HttpClient::HealthCheck() {
    auto resp = DoRequest("GET", "/health");
    if (!resp.IsSuccess()) return std::nullopt;

    try {
        auto j = nlohmann::json::parse(resp.body);
        HealthResult result;
        result.status = j.value("status", "");
        result.version = j.value("version", "");
        result.time = j.value("time", "");
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

// ---------- Todos ----------

std::optional<TodoListResult> HttpClient::ListTodos(const models::Filter& filter) {
    std::ostringstream qs;
    if (!filter.status.empty()) qs << "status=" << filter.status << "&";
    if (!filter.tag.empty()) qs << "tag=" << filter.tag << "&";
    if (!filter.keyword.empty()) qs << "keyword=" << filter.keyword << "&";
    if (!filter.due_before.empty()) qs << "due_before=" << filter.due_before << "&";
    if (filter.include_deleted) qs << "include_deleted=1&";
    if (filter.only_deleted) qs << "only_deleted=1&";
    qs << "page=" << filter.page << "&page_size=" << filter.page_size;

    auto resp = DoRequest("GET", "/api/todos", "", qs.str());
    if (!resp.IsSuccess()) return std::nullopt;

    try {
        auto j = nlohmann::json::parse(resp.body);
        TodoListResult result;
        if (j.contains("items") && j["items"].is_array()) {
            for (const auto& item : j["items"]) {
                result.items.push_back(codec::JsonHelper::ParseTodo(item));
            }
        }
        result.total = j.value("total", 0);
        result.page = j.value("page", 1);
        result.page_size = j.value("page_size", 50);
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<models::Todo> HttpClient::GetTodo(uint64_t id, bool includeDeleted) {
    std::string path = "/api/todos/" + std::to_string(id);
    std::string qs = includeDeleted ? "include_deleted=1" : "";
    auto resp = DoRequest("GET", path, "", qs);
    if (!resp.IsSuccess()) return std::nullopt;

    try {
        auto j = nlohmann::json::parse(resp.body);
        return codec::JsonHelper::ParseTodo(j);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<models::Todo> HttpClient::CreateTodo(const std::string& title, int priority,
                                                    const std::string& tag,
                                                    const std::string& dueAt) {
    nlohmann::json body;
    body["title"] = title;
    if (priority > 0) body["priority"] = priority;
    if (!tag.empty()) body["tag"] = tag;
    if (!dueAt.empty()) body["due_at"] = dueAt;

    auto resp = DoRequest("POST", "/api/todos", body.dump());
    if (!resp.IsSuccess()) return std::nullopt;

    try {
        auto j = nlohmann::json::parse(resp.body);
        return codec::JsonHelper::ParseTodo(j);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<models::Todo> HttpClient::UpdateTodo(uint64_t id, const std::string& title,
                                                    int priority, const std::string& tag,
                                                    const std::string& dueAt) {
    nlohmann::json body;
    body["title"] = title;
    body["priority"] = priority;
    body["tag"] = tag;
    if (!dueAt.empty()) body["due_at"] = dueAt;

    std::string path = "/api/todos/" + std::to_string(id);
    auto resp = DoRequest("PUT", path, body.dump());
    if (!resp.IsSuccess()) return std::nullopt;

    try {
        auto j = nlohmann::json::parse(resp.body);
        return codec::JsonHelper::ParseTodo(j);
    } catch (...) {
        return std::nullopt;
    }
}

bool HttpClient::DeleteTodo(uint64_t id) {
    std::string path = "/api/todos/" + std::to_string(id);
    auto resp = DoRequest("DELETE", path);
    return resp.IsSuccess();
}

std::optional<models::Todo> HttpClient::CompleteTodo(uint64_t id) {
    std::string path = "/api/todos/" + std::to_string(id) + "/complete";
    auto resp = DoRequest("POST", path);
    if (!resp.IsSuccess()) return std::nullopt;

    try {
        auto j = nlohmann::json::parse(resp.body);
        return codec::JsonHelper::ParseTodo(j);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<models::Todo> HttpClient::ReopenTodo(uint64_t id) {
    std::string path = "/api/todos/" + std::to_string(id) + "/reopen";
    auto resp = DoRequest("POST", path);
    if (!resp.IsSuccess()) return std::nullopt;

    try {
        auto j = nlohmann::json::parse(resp.body);
        return codec::JsonHelper::ParseTodo(j);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<models::Todo> HttpClient::RestoreTodo(uint64_t id) {
    std::string path = "/api/todos/" + std::to_string(id) + "/restore";
    auto resp = DoRequest("POST", path);
    if (!resp.IsSuccess()) return std::nullopt;

    try {
        auto j = nlohmann::json::parse(resp.body);
        return codec::JsonHelper::ParseTodo(j);
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<models::AuditLog> HttpClient::ListTodoHistory(uint64_t id, int page, int pageSize) {
    std::string path = "/api/todos/" + std::to_string(id) + "/history";
    std::string qs = "page=" + std::to_string(page) + "&page_size=" + std::to_string(pageSize);
    auto resp = DoRequest("GET", path, "", qs);
    if (!resp.IsSuccess()) return {};

    return codec::JsonHelper::ParseAuditLogs(resp.body);
}

// ---------- Sticky Notes ----------

std::optional<StickyListResult> HttpClient::ListStickies() {
    auto resp = DoRequest("GET", "/api/sticky-notes");
    if (!resp.IsSuccess()) return std::nullopt;

    try {
        StickyListResult result;
        result.items = codec::JsonHelper::ParseStickyNotes(resp.body);
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<models::StickyNote> HttpClient::GetSticky(const std::string& id) {
    std::string path = "/api/sticky-notes/" + id;
    auto resp = DoRequest("GET", path);
    if (!resp.IsSuccess()) return std::nullopt;

    try {
        auto j = nlohmann::json::parse(resp.body);
        return codec::JsonHelper::ParseStickyNote(j);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<models::StickyNote> HttpClient::UpsertSticky(const std::string& id,
                                                            const std::string& title,
                                                            const std::string& bgColor,
                                                            const std::string& filter) {
    nlohmann::json body;
    body["title"] = title;
    body["frame"] = "{}";  // Windows client does not upload frame
    body["bg_color"] = bgColor.empty() ? "{}" : bgColor;
    body["filter"] = filter.empty() ? "{}" : filter;

    std::string path = "/api/sticky-notes/" + id;
    auto resp = DoRequest("PUT", path, body.dump());
    if (!resp.IsSuccess()) return std::nullopt;

    try {
        auto j = nlohmann::json::parse(resp.body);
        return codec::JsonHelper::ParseStickyNote(j);
    } catch (...) {
        return std::nullopt;
    }
}

bool HttpClient::DeleteSticky(const std::string& id) {
    std::string path = "/api/sticky-notes/" + id;
    auto resp = DoRequest("DELETE", path);
    return resp.IsSuccess();
}

// ---------- Audit & Tags ----------

std::vector<models::AuditLog> HttpClient::ListAuditLogs(int page, int pageSize,
                                                         const std::string& action) {
    std::ostringstream qs;
    qs << "page=" << page << "&page_size=" << pageSize;
    if (!action.empty()) qs << "&action=" << action;

    auto resp = DoRequest("GET", "/api/audit-logs", "", qs.str());
    if (!resp.IsSuccess()) return {};

    return codec::JsonHelper::ParseAuditLogs(resp.body);
}

std::vector<std::string> HttpClient::ListTags() {
    auto resp = DoRequest("GET", "/api/tags");
    if (!resp.IsSuccess()) return {};

    try {
        auto j = nlohmann::json::parse(resp.body);
        std::vector<std::string> tags;
        if (j.is_array()) {
            for (const auto& item : j) {
                if (item.is_string()) tags.push_back(item.get<std::string>());
            }
        }
        return tags;
    } catch (...) {
        return {};
    }
}

// ==========================================================================
// Async API
// ==========================================================================
//
// Implementation contract shared by every AsyncXxx method below:
//
//   1) Snapshot any state the worker will need (baseUrl, token, callbacks,
//      arguments) into value-captured locals BEFORE the std::thread
//      launches. The worker must NEVER dereference `this` — the caller's
//      HttpClient instance might be destroyed while the request is in
//      flight (e.g. user clicks Logout mid-Login, which tears down
//      AppState), and reaching into member variables from the worker
//      would trigger use-after-free.
//
//   2) Inside the worker, construct a local HttpClient temporary seeded
//      with the snapshotted state and call its SYNCHRONOUS counterpart
//      (HealthCheck / Login / ListAuditLogs / …). This intentionally
//      ignores the caller's `uiThreadTarget_` for the WinHTTP work —
//      target is only used for the completion marshal.
//
//   3) On completion, PostToUIThread a lambda that invokes the user's
//      callback with the result. If the target HWND is gone
//      (PostToUIThread returns false), the callback is silently dropped
//      — the only UI that would have wanted the result is the window
//      that just disappeared, so there's no one to notify.
//
//   4) std::thread::detach() — we do NOT join on this thread anywhere.
//      Lifetime of the detached thread is bounded by WinHTTP's timeout
//      budget (max ~40 s for a DoRequest: 10 s resolve + 10 s connect +
//      10 s send + 10 s receive). That is acceptable for app shutdown:
//      the Windows process exit will reap the thread regardless of its
//      state; no persistent OS resource is leaked. If we ever wanted
//      clean "wait for all in-flight HTTP to finish before exit" we
//      would swap detach() for a thread pool with join-on-shutdown —
//      not needed today.

void HttpClient::AsyncHealthCheck(const std::string& baseUrl, HealthCallback onDone) {
    // /health is unauthenticated, so no token capture needed. Snapshot
    // only baseUrl + target + callback.
    HWND target = uiThreadTarget_;
    std::string baseUrlCopy = baseUrl;
    HealthCallback cb = std::move(onDone);

    std::thread([target, baseUrlCopy = std::move(baseUrlCopy),
                 cb = std::move(cb)]() mutable {
        HttpClient local;
        local.SetBaseUrl(baseUrlCopy);
        auto result = local.HealthCheck();

        // Marshal result → UI thread. The inner lambda value-captures
        // `cb` and `result` so they outlive this worker thread's stack.
        if (!PostToUIThread(target,
                            [cb = std::move(cb),
                             result = std::move(result)]() mutable {
            if (cb) cb(std::move(result));
        })) {
            // Post failed — target HWND is gone. Drop the callback; no
            // UI left to update. `cb` and `result` destruct here.
        }
    }).detach();
}

void HttpClient::AsyncLogin(const std::string& baseUrl,
                            const std::string& username,
                            const std::string& password,
                            LoginCallback onDone) {
    // Login is ALSO unauthenticated (it's the call that issues a token),
    // so no token capture. Snapshot baseUrl + credentials + target.
    HWND target = uiThreadTarget_;
    std::string baseUrlCopy = baseUrl;
    std::string usernameCopy = username;
    std::string passwordCopy = password;
    LoginCallback cb = std::move(onDone);

    std::thread([target,
                 baseUrlCopy = std::move(baseUrlCopy),
                 usernameCopy = std::move(usernameCopy),
                 passwordCopy = std::move(passwordCopy),
                 cb = std::move(cb)]() mutable {
        HttpClient local;
        local.SetBaseUrl(baseUrlCopy);
        auto result = local.Login(usernameCopy, passwordCopy);

        if (!PostToUIThread(target,
                            [cb = std::move(cb),
                             result = std::move(result)]() mutable {
            if (cb) cb(std::move(result));
        })) {
            // Drop on shutdown race; see AsyncHealthCheck above.
        }
    }).detach();
}

// ---------- Authenticated capture helper ----------
//
// The 10 authenticated Async* methods below all follow the same
// worker setup: build a local HttpClient seeded with snapshotted
// baseUrl/token/onUnauthorized so it behaves like `this->*` but
// without the lifetime coupling. Factor the boilerplate into one
// inline helper to keep each Async* body focused on its payload.
//
// We cannot put this in an anonymous namespace at file scope (the
// helper needs to be in namespace stickytodo::core to access
// UnauthorizedCallback); leaving it as a local static lambda-like
// helper inside each function would duplicate ~15 lines per method.
// So a private free function it is.
namespace {

// Configure `local` with the snapshotted state + a marshalled
// onUnauthorized relay. The relay is exactly the same pattern as
// AsyncListAuditLogs: worker-thread 401 → PostToUIThread so the
// subscriber (AppState::HandleUnauthorized) sees it on the UI
// thread where it's safe to tear down state.
void ConfigureAuthedLocal(HttpClient& local,
                          HWND target,
                          const std::string& baseUrlCopy,
                          const std::string& tokenCopy,
                          const HttpClient::UnauthorizedCallback& onUnauthCopy) {
    local.SetBaseUrl(baseUrlCopy);
    local.SetToken(tokenCopy);
    if (onUnauthCopy) {
        local.SetOnUnauthorized([target, onUnauthCopy]() {
            if (!PostToUIThread(target, [onUnauthCopy]() {
                if (onUnauthCopy) onUnauthCopy();
            })) {
                // UI gone — drop, nothing useful left to notify.
            }
        });
    }
}

} // namespace

void HttpClient::AsyncListAuditLogs(int page, int pageSize,
                                    const std::string& action,
                                    AuditLogsCallback onDone) {
    // Audit logs require auth. Snapshot token + onUnauthorized_ along
    // with baseUrl so the worker's local HttpClient can behave exactly
    // like this->*. We capture onUnauthorized_ by value (std::function
    // copy) so the callback survives even if `this` dies mid-request.
    HWND target = uiThreadTarget_;
    std::string baseUrlCopy = baseUrl_;
    std::string tokenCopy = token_;
    UnauthorizedCallback onUnauthCopy = onUnauthorized_;
    std::string actionCopy = action;
    AuditLogsCallback cb = std::move(onDone);

    std::thread([target,
                 page, pageSize,
                 baseUrlCopy = std::move(baseUrlCopy),
                 tokenCopy = std::move(tokenCopy),
                 onUnauthCopy = std::move(onUnauthCopy),
                 actionCopy = std::move(actionCopy),
                 cb = std::move(cb)]() mutable {
        HttpClient local;
        local.SetBaseUrl(baseUrlCopy);
        local.SetToken(tokenCopy);
        if (onUnauthCopy) {
            // Relay the 401 signal through the snapshotted callback. It
            // fires on THIS worker thread; AppState::HandleUnauthorized
            // is the only subscriber we know of today and it only
            // touches UI state via Logout() — which itself needs to run
            // on the UI thread. Marshal the unauth hit to the UI thread
            // explicitly rather than leave that responsibility to the
            // subscriber, so the sync surface keeps working unchanged.
            local.SetOnUnauthorized([target, onUnauthCopy]() {
                if (!PostToUIThread(target, [onUnauthCopy]() {
                    if (onUnauthCopy) onUnauthCopy();
                })) {
                    // UI gone — drop, nothing useful left to notify.
                }
            });
        }
        auto result = local.ListAuditLogs(page, pageSize, actionCopy);

        if (!PostToUIThread(target,
                            [cb = std::move(cb),
                             result = std::move(result)]() mutable {
            if (cb) cb(std::move(result));
        })) {
            // Drop on shutdown race.
        }
    }).detach();
}

// ==========================================================================
// Todo / Sticky Async API
// ==========================================================================
//
// All of these follow the common Async contract documented atop the
// async block. Differences per method are in the worker payload only;
// setup (snapshot state, configure local, post result) is uniform via
// ConfigureAuthedLocal().

void HttpClient::AsyncListTodos(const models::Filter& filter, TodoListCallback onDone) {
    HWND target = uiThreadTarget_;
    std::string baseUrlCopy = baseUrl_;
    std::string tokenCopy = token_;
    UnauthorizedCallback onUnauthCopy = onUnauthorized_;
    // Filter is a POD-of-strings/ints — value-capture is cheap and
    // decouples the worker from any UI-thread mutation of the
    // caller's filter state.
    models::Filter filterCopy = filter;
    TodoListCallback cb = std::move(onDone);

    std::thread([target,
                 baseUrlCopy = std::move(baseUrlCopy),
                 tokenCopy = std::move(tokenCopy),
                 onUnauthCopy = std::move(onUnauthCopy),
                 filterCopy = std::move(filterCopy),
                 cb = std::move(cb)]() mutable {
        HttpClient local;
        ConfigureAuthedLocal(local, target, baseUrlCopy, tokenCopy, onUnauthCopy);
        auto result = local.ListTodos(filterCopy);

        if (!PostToUIThread(target,
                            [cb = std::move(cb),
                             result = std::move(result)]() mutable {
            if (cb) cb(std::move(result));
        })) {
            // Drop on shutdown race.
        }
    }).detach();
}

void HttpClient::AsyncCreateTodo(const std::string& title, int priority,
                                 const std::string& tag, const std::string& dueAt,
                                 TodoOptCallback onDone) {
    HWND target = uiThreadTarget_;
    std::string baseUrlCopy = baseUrl_;
    std::string tokenCopy = token_;
    UnauthorizedCallback onUnauthCopy = onUnauthorized_;
    std::string titleCopy = title;
    std::string tagCopy = tag;
    std::string dueAtCopy = dueAt;
    TodoOptCallback cb = std::move(onDone);

    std::thread([target, priority,
                 baseUrlCopy = std::move(baseUrlCopy),
                 tokenCopy = std::move(tokenCopy),
                 onUnauthCopy = std::move(onUnauthCopy),
                 titleCopy = std::move(titleCopy),
                 tagCopy = std::move(tagCopy),
                 dueAtCopy = std::move(dueAtCopy),
                 cb = std::move(cb)]() mutable {
        HttpClient local;
        ConfigureAuthedLocal(local, target, baseUrlCopy, tokenCopy, onUnauthCopy);
        auto result = local.CreateTodo(titleCopy, priority, tagCopy, dueAtCopy);

        if (!PostToUIThread(target,
                            [cb = std::move(cb),
                             result = std::move(result)]() mutable {
            if (cb) cb(std::move(result));
        })) {
            // Drop on shutdown race.
        }
    }).detach();
}

void HttpClient::AsyncUpdateTodo(uint64_t id, const std::string& title,
                                 int priority, const std::string& tag,
                                 const std::string& dueAt,
                                 TodoOptCallback onDone) {
    HWND target = uiThreadTarget_;
    std::string baseUrlCopy = baseUrl_;
    std::string tokenCopy = token_;
    UnauthorizedCallback onUnauthCopy = onUnauthorized_;
    std::string titleCopy = title;
    std::string tagCopy = tag;
    std::string dueAtCopy = dueAt;
    TodoOptCallback cb = std::move(onDone);

    std::thread([target, id, priority,
                 baseUrlCopy = std::move(baseUrlCopy),
                 tokenCopy = std::move(tokenCopy),
                 onUnauthCopy = std::move(onUnauthCopy),
                 titleCopy = std::move(titleCopy),
                 tagCopy = std::move(tagCopy),
                 dueAtCopy = std::move(dueAtCopy),
                 cb = std::move(cb)]() mutable {
        HttpClient local;
        ConfigureAuthedLocal(local, target, baseUrlCopy, tokenCopy, onUnauthCopy);
        auto result = local.UpdateTodo(id, titleCopy, priority, tagCopy, dueAtCopy);

        if (!PostToUIThread(target,
                            [cb = std::move(cb),
                             result = std::move(result)]() mutable {
            if (cb) cb(std::move(result));
        })) {
            // Drop on shutdown race.
        }
    }).detach();
}

void HttpClient::AsyncDeleteTodo(uint64_t id, BoolCallback onDone) {
    HWND target = uiThreadTarget_;
    std::string baseUrlCopy = baseUrl_;
    std::string tokenCopy = token_;
    UnauthorizedCallback onUnauthCopy = onUnauthorized_;
    BoolCallback cb = std::move(onDone);

    std::thread([target, id,
                 baseUrlCopy = std::move(baseUrlCopy),
                 tokenCopy = std::move(tokenCopy),
                 onUnauthCopy = std::move(onUnauthCopy),
                 cb = std::move(cb)]() mutable {
        HttpClient local;
        ConfigureAuthedLocal(local, target, baseUrlCopy, tokenCopy, onUnauthCopy);
        bool ok = local.DeleteTodo(id);

        if (!PostToUIThread(target,
                            [cb = std::move(cb), ok]() mutable {
            if (cb) cb(ok);
        })) {
            // Drop on shutdown race.
        }
    }).detach();
}

void HttpClient::AsyncCompleteTodo(uint64_t id, TodoOptCallback onDone) {
    HWND target = uiThreadTarget_;
    std::string baseUrlCopy = baseUrl_;
    std::string tokenCopy = token_;
    UnauthorizedCallback onUnauthCopy = onUnauthorized_;
    TodoOptCallback cb = std::move(onDone);

    std::thread([target, id,
                 baseUrlCopy = std::move(baseUrlCopy),
                 tokenCopy = std::move(tokenCopy),
                 onUnauthCopy = std::move(onUnauthCopy),
                 cb = std::move(cb)]() mutable {
        HttpClient local;
        ConfigureAuthedLocal(local, target, baseUrlCopy, tokenCopy, onUnauthCopy);
        auto result = local.CompleteTodo(id);

        if (!PostToUIThread(target,
                            [cb = std::move(cb),
                             result = std::move(result)]() mutable {
            if (cb) cb(std::move(result));
        })) {
            // Drop on shutdown race.
        }
    }).detach();
}

void HttpClient::AsyncReopenTodo(uint64_t id, TodoOptCallback onDone) {
    HWND target = uiThreadTarget_;
    std::string baseUrlCopy = baseUrl_;
    std::string tokenCopy = token_;
    UnauthorizedCallback onUnauthCopy = onUnauthorized_;
    TodoOptCallback cb = std::move(onDone);

    std::thread([target, id,
                 baseUrlCopy = std::move(baseUrlCopy),
                 tokenCopy = std::move(tokenCopy),
                 onUnauthCopy = std::move(onUnauthCopy),
                 cb = std::move(cb)]() mutable {
        HttpClient local;
        ConfigureAuthedLocal(local, target, baseUrlCopy, tokenCopy, onUnauthCopy);
        auto result = local.ReopenTodo(id);

        if (!PostToUIThread(target,
                            [cb = std::move(cb),
                             result = std::move(result)]() mutable {
            if (cb) cb(std::move(result));
        })) {
            // Drop on shutdown race.
        }
    }).detach();
}

void HttpClient::AsyncRestoreTodo(uint64_t id, TodoOptCallback onDone) {
    HWND target = uiThreadTarget_;
    std::string baseUrlCopy = baseUrl_;
    std::string tokenCopy = token_;
    UnauthorizedCallback onUnauthCopy = onUnauthorized_;
    TodoOptCallback cb = std::move(onDone);

    std::thread([target, id,
                 baseUrlCopy = std::move(baseUrlCopy),
                 tokenCopy = std::move(tokenCopy),
                 onUnauthCopy = std::move(onUnauthCopy),
                 cb = std::move(cb)]() mutable {
        HttpClient local;
        ConfigureAuthedLocal(local, target, baseUrlCopy, tokenCopy, onUnauthCopy);
        auto result = local.RestoreTodo(id);

        if (!PostToUIThread(target,
                            [cb = std::move(cb),
                             result = std::move(result)]() mutable {
            if (cb) cb(std::move(result));
        })) {
            // Drop on shutdown race.
        }
    }).detach();
}

void HttpClient::AsyncListStickies(StickyListCallback onDone) {
    HWND target = uiThreadTarget_;
    std::string baseUrlCopy = baseUrl_;
    std::string tokenCopy = token_;
    UnauthorizedCallback onUnauthCopy = onUnauthorized_;
    StickyListCallback cb = std::move(onDone);

    std::thread([target,
                 baseUrlCopy = std::move(baseUrlCopy),
                 tokenCopy = std::move(tokenCopy),
                 onUnauthCopy = std::move(onUnauthCopy),
                 cb = std::move(cb)]() mutable {
        HttpClient local;
        ConfigureAuthedLocal(local, target, baseUrlCopy, tokenCopy, onUnauthCopy);
        auto result = local.ListStickies();

        if (!PostToUIThread(target,
                            [cb = std::move(cb),
                             result = std::move(result)]() mutable {
            if (cb) cb(std::move(result));
        })) {
            // Drop on shutdown race.
        }
    }).detach();
}

void HttpClient::AsyncUpsertSticky(const std::string& id,
                                   const std::string& title,
                                   const std::string& bgColor,
                                   const std::string& filter,
                                   StickyOptCallback onDone) {
    HWND target = uiThreadTarget_;
    std::string baseUrlCopy = baseUrl_;
    std::string tokenCopy = token_;
    UnauthorizedCallback onUnauthCopy = onUnauthorized_;
    std::string idCopy = id;
    std::string titleCopy = title;
    std::string bgColorCopy = bgColor;
    std::string filterCopy = filter;
    StickyOptCallback cb = std::move(onDone);

    std::thread([target,
                 baseUrlCopy = std::move(baseUrlCopy),
                 tokenCopy = std::move(tokenCopy),
                 onUnauthCopy = std::move(onUnauthCopy),
                 idCopy = std::move(idCopy),
                 titleCopy = std::move(titleCopy),
                 bgColorCopy = std::move(bgColorCopy),
                 filterCopy = std::move(filterCopy),
                 cb = std::move(cb)]() mutable {
        HttpClient local;
        ConfigureAuthedLocal(local, target, baseUrlCopy, tokenCopy, onUnauthCopy);
        auto result = local.UpsertSticky(idCopy, titleCopy, bgColorCopy, filterCopy);

        if (!PostToUIThread(target,
                            [cb = std::move(cb),
                             result = std::move(result)]() mutable {
            if (cb) cb(std::move(result));
        })) {
            // Drop on shutdown race.
        }
    }).detach();
}

void HttpClient::AsyncDeleteSticky(const std::string& id, BoolCallback onDone) {
    HWND target = uiThreadTarget_;
    std::string baseUrlCopy = baseUrl_;
    std::string tokenCopy = token_;
    UnauthorizedCallback onUnauthCopy = onUnauthorized_;
    std::string idCopy = id;
    BoolCallback cb = std::move(onDone);

    std::thread([target,
                 baseUrlCopy = std::move(baseUrlCopy),
                 tokenCopy = std::move(tokenCopy),
                 onUnauthCopy = std::move(onUnauthCopy),
                 idCopy = std::move(idCopy),
                 cb = std::move(cb)]() mutable {
        HttpClient local;
        ConfigureAuthedLocal(local, target, baseUrlCopy, tokenCopy, onUnauthCopy);
        bool ok = local.DeleteSticky(idCopy);

        if (!PostToUIThread(target,
                            [cb = std::move(cb), ok]() mutable {
            if (cb) cb(ok);
        })) {
            // Drop on shutdown race.
        }
    }).detach();
}

} // namespace stickytodo::core
