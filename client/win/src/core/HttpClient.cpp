#include "core/HttpClient.h"
#include "codec/JsonHelper.h"
#include "codec/StickyCodec.h"

#include <nlohmann/json.hpp>
#include <windows.h>
#include <winhttp.h>
#include <sstream>
#include <algorithm>

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
    // Remove trailing slash
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

    // Extract host:port
    auto colonPos = url.find(':');
    if (colonPos != std::string::npos) {
        result.host = Utf8ToWide(url.substr(0, colonPos));
        result.port = static_cast<INTERNET_PORT>(std::stoi(url.substr(colonPos + 1)));
    } else {
        result.host = Utf8ToWide(url);
    }

    // Build path
    std::string fullPath = path;
    if (!query.empty()) {
        fullPath += "?" + query;
    }
    result.path = Utf8ToWide(fullPath);

    return result;
}

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

} // namespace stickytodo::core
