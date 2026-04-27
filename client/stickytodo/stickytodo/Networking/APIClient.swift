//
//  APIClient.swift
//  stickytodo
//
//  URLSession + async/await 的 HTTP 客户端。
//
//  职责：
//    1. 组装请求、注入 `Authorization: Bearer <token>`
//    2. 统一 JSON 编解码（含 RFC3339 / RFC3339Nano 两级 Date 解析）
//    3. 映射 HTTP 错误到 APIError；401 时通知上层 onUnauthorized
//    4. 全部方法对齐阶段九 plan 的 API 白名单，不暴露原始 URLRequest
//

import Foundation

// MARK: - APIError

/// HTTP 错误的统一类型。
enum APIError: Error, Equatable {
    /// 非 HTTP 响应（URLSession 返回了非 HTTPURLResponse，理论上不应发生）。
    case invalidResponse
    /// 400 类请求错误；附带后端返回的 `error` 文案。
    case badRequest(String)
    /// 401。APIClient 会同时触发 `onUnauthorized` 回调。
    case unauthorized(String)
    /// 404。
    case notFound(String)
    /// 其他 4xx/5xx。
    case http(status: Int, message: String)
    /// 解码/编码错误。
    case decoding(String)
    /// 底层传输错误（NSURLError 等）。
    case transport(String)
    /// Endpoint 构造失败。
    case endpoint(EndpointError)

    /// 用户可读描述。
    var userMessage: String {
        switch self {
        case .invalidResponse: return "服务器返回了不识别的响应"
        case .badRequest(let m): return m.isEmpty ? "请求参数有误" : m
        case .unauthorized(let m): return m.isEmpty ? "未登录或登录已过期" : m
        case .notFound(let m): return m.isEmpty ? "资源不存在" : m
        case .http(let s, let m): return m.isEmpty ? "HTTP 错误 \(s)" : "HTTP \(s): \(m)"
        case .decoding(let m): return "解析响应失败：\(m)"
        case .transport(let m): return "网络错误：\(m)"
        case .endpoint(let e):
            switch e {
            case .invalidBaseURL(let s): return "服务器地址不合法：\(s)"
            case .composeFailed(let s): return "URL 构造失败：\(s)"
            }
        }
    }
}

// MARK: - APIClient

/// HTTP 客户端。
///
/// 设计约定：本类由调用方（通常是 `@MainActor AppState`）持有，其公开方法可以
/// 从 MainActor 调用（`await` 会把挂起期间的网络 I/O 切到后台，返回后回到调用
/// 者的 actor，无需 Sendable 屏障）。
///
/// 因此：
/// - `baseURLProvider` / `tokenProvider` 只在发起请求那一刻被调用，且都发生在
///   调用者 actor 上——不跨 actor、不逃逸，不需要标 `@Sendable`。
/// - `onUnauthorized` 也同样在调用者 actor 上触发（`perform` 里的 `await`
///   之后代码仍在原 actor），不需要 `@Sendable`。
/// - 不声明 APIClient 为 `Sendable`；这样 MainActor 持有它不会被 strict
///   concurrency 模式判定为需要跨 actor 同步。
final class APIClient {

    // MARK: Init

    /// - Parameters:
    ///   - baseURLProvider: 每次请求前取当前 base URL；支持运行时切换服务器地址。
    ///   - tokenProvider: 每次请求前取当前 token；nil 表示未登录（仅 login/health 可发）。
    ///   - onUnauthorized: 401 时回调；AppState 需据此清掉内存 token 并引导重登录。
    ///   - session: 方便单测注入；默认 `.shared`。
    init(
        baseURLProvider: @escaping () -> String,
        tokenProvider: @escaping () -> String?,
        onUnauthorized: @escaping () -> Void,
        session: URLSession = .shared
    ) {
        self.baseURLProvider = baseURLProvider
        self.tokenProvider = tokenProvider
        self.onUnauthorized = onUnauthorized
        self.session = session
    }

    // MARK: Public endpoints

    /// `GET /health`。无需登录；用于测试服务器连通性。
    func health() async throws -> HealthResponse {
        let url = try endpoint { try Endpoints.health(base: $0) }
        return try await send(url: url, method: "GET", requireAuth: false)
    }

    /// `POST /api/login`。登录成功返回 token 与过期时间。
    /// 不会自动保存 token——调用方（AppState）决定如何持久化。
    func login(username: String, password: String) async throws -> LoginResponse {
        let url = try endpoint { try Endpoints.login(base: $0) }
        let body = LoginRequest(username: username, password: password)
        return try await send(url: url, method: "POST", body: body, requireAuth: false)
    }

    /// `GET /api/todos`。
    func listTodos(filter: TodoFilter) async throws -> TodoListResponse {
        let url = try endpoint { try Endpoints.todos(base: $0, query: filter.queryItems()) }
        return try await send(url: url, method: "GET")
    }

    /// `POST /api/todos`。
    func createTodo(_ req: CreateTodoRequest) async throws -> Todo {
        let url = try endpoint { try Endpoints.todos(base: $0) }
        return try await send(url: url, method: "POST", body: req)
    }

    /// `GET /api/todos/:id`，支持 `include_deleted=1`。
    func getTodo(id: UInt64, includeDeleted: Bool = false) async throws -> Todo {
        let url = try endpoint { base in
            try Endpoints.todo(base: base, id: id, includeDeleted: includeDeleted)
        }
        return try await send(url: url, method: "GET")
    }

    /// `PUT /api/todos/:id`。
    func updateTodo(id: UInt64, req: UpdateTodoRequest) async throws -> Todo {
        guard req.hasAny else {
            throw APIError.badRequest("没有需要更新的字段")
        }
        let url = try endpoint { try Endpoints.todo(base: $0, id: id) }
        return try await send(url: url, method: "PUT", body: req)
    }

    /// `DELETE /api/todos/:id`。软删。
    @discardableResult
    func deleteTodo(id: UInt64) async throws -> DeleteTodoResponse {
        let url = try endpoint { try Endpoints.todo(base: $0, id: id) }
        return try await send(url: url, method: "DELETE")
    }

    /// `POST /api/todos/:id/complete`。
    func completeTodo(id: UInt64) async throws -> Todo {
        let url = try endpoint { try Endpoints.todoComplete(base: $0, id: id) }
        return try await send(url: url, method: "POST", emptyBody: true)
    }

    /// `POST /api/todos/:id/reopen`。
    func reopenTodo(id: UInt64) async throws -> Todo {
        let url = try endpoint { try Endpoints.todoReopen(base: $0, id: id) }
        return try await send(url: url, method: "POST", emptyBody: true)
    }

    /// `POST /api/todos/:id/restore`。
    func restoreTodo(id: UInt64) async throws -> Todo {
        let url = try endpoint { try Endpoints.todoRestore(base: $0, id: id) }
        return try await send(url: url, method: "POST", emptyBody: true)
    }

    /// `GET /api/todos/:id/history`。
    func todoHistory(id: UInt64, page: Int = 1, pageSize: Int = 50) async throws -> AuditListResponse {
        let url = try endpoint {
            try Endpoints.todoHistory(base: $0, id: id, page: page, pageSize: pageSize)
        }
        return try await send(url: url, method: "GET")
    }

    /// `GET /api/audit-logs`。
    func auditLogs(query: [URLQueryItem] = []) async throws -> AuditListResponse {
        let url = try endpoint { try Endpoints.auditLogs(base: $0, query: query) }
        return try await send(url: url, method: "GET")
    }

    /// `GET /api/tags`。
    func listTags() async throws -> TagListResponse {
        let url = try endpoint { try Endpoints.tags(base: $0) }
        return try await send(url: url, method: "GET")
    }

    // MARK: - Private

    private let baseURLProvider: () -> String
    private let tokenProvider: () -> String?
    private let onUnauthorized: () -> Void
    private let session: URLSession

    /// 包装一层 endpoint 构造，把 `EndpointError` 升格为 `APIError.endpoint`。
    private func endpoint(_ builder: (String) throws -> URL) throws -> URL {
        let base = baseURLProvider()
        do {
            return try builder(base)
        } catch let e as EndpointError {
            throw APIError.endpoint(e)
        } catch {
            throw APIError.endpoint(.composeFailed(error.localizedDescription))
        }
    }

    /// 带 body 的请求。
    private func send<Body: Encodable, Out: Decodable>(
        url: URL,
        method: String,
        body: Body,
        requireAuth: Bool = true
    ) async throws -> Out {
        var req = URLRequest(url: url)
        req.httpMethod = method
        req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        req.setValue("application/json", forHTTPHeaderField: "Accept")
        do {
            req.httpBody = try Self.jsonEncoder.encode(body)
        } catch {
            throw APIError.decoding("encode body: \(error.localizedDescription)")
        }
        return try await perform(request: req, requireAuth: requireAuth)
    }

    /// 无 body 或空 body 的请求。
    ///
    /// `emptyBody=true` 时会发送 `{}` 作为 body——后端 complete/reopen/restore 不解析 body，
    /// 但发空 JSON 可让代理/中间件更友好地处理 POST 请求。
    private func send<Out: Decodable>(
        url: URL,
        method: String,
        requireAuth: Bool = true,
        emptyBody: Bool = false
    ) async throws -> Out {
        var req = URLRequest(url: url)
        req.httpMethod = method
        req.setValue("application/json", forHTTPHeaderField: "Accept")
        if emptyBody {
            req.setValue("application/json", forHTTPHeaderField: "Content-Type")
            req.httpBody = Data("{}".utf8)
        }
        return try await perform(request: req, requireAuth: requireAuth)
    }

    /// 实际发送并解码。
    private func perform<Out: Decodable>(request: URLRequest, requireAuth: Bool) async throws -> Out {
        var req = request
        if requireAuth {
            guard let token = tokenProvider(), !token.isEmpty else {
                throw APIError.unauthorized("未登录")
            }
            req.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
        }

        let data: Data
        let response: URLResponse
        do {
            (data, response) = try await session.data(for: req)
        } catch {
            throw APIError.transport(error.localizedDescription)
        }

        guard let http = response as? HTTPURLResponse else {
            throw APIError.invalidResponse
        }

        // 401 需要额外触发回调，让 AppState 把 token 清掉。
        if http.statusCode == 401 {
            onUnauthorized()
            throw APIError.unauthorized(Self.decodeErrorMessage(data))
        }

        // 2xx → 解码；其他 → 组装错误
        guard (200..<300).contains(http.statusCode) else {
            let msg = Self.decodeErrorMessage(data)
            switch http.statusCode {
            case 400: throw APIError.badRequest(msg)
            case 404: throw APIError.notFound(msg)
            default:  throw APIError.http(status: http.statusCode, message: msg)
            }
        }

        // 204 / 空 body：仅在 Out 为 EmptyResponse 时接受。
        if data.isEmpty {
            if let empty = EmptyResponse() as? Out {
                return empty
            }
            throw APIError.decoding("response body is empty but \(Out.self) was expected")
        }

        do {
            return try Self.jsonDecoder.decode(Out.self, from: data)
        } catch {
            throw APIError.decoding(error.localizedDescription)
        }
    }

    /// 从后端错误响应 `{"error":"..."}` 解析文案；失败回落到原始字符串。
    private static func decodeErrorMessage(_ data: Data) -> String {
        if let dict = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
           let msg = dict["error"] as? String {
            return msg
        }
        return String(data: data, encoding: .utf8) ?? ""
    }

    // MARK: JSON Coders

    /// 共享 encoder：snake_case 由各模型的 CodingKeys 手动指定，这里不做自动转换。
    /// Date → ISO8601（带时区；无小数秒），匹配后端 `time.Time` 的 RFC3339。
    private static let jsonEncoder: JSONEncoder = {
        let e = JSONEncoder()
        e.dateEncodingStrategy = .custom { date, encoder in
            var c = encoder.singleValueContainer()
            try c.encode(iso8601Full.string(from: date))
        }
        return e
    }()

    /// 共享 decoder：Date 同时尝试带小数秒与不带小数秒两种格式，兼容 GORM 的两种输出。
    private static let jsonDecoder: JSONDecoder = {
        let d = JSONDecoder()
        d.dateDecodingStrategy = .custom { decoder in
            let raw = try decoder.singleValueContainer().decode(String.self)
            if let date = iso8601Full.date(from: raw) { return date }
            if let date = iso8601NoFraction.date(from: raw) { return date }
            throw APIError.decoding("unrecognized ISO8601 date: \(raw)")
        }
        return d
    }()

    /// 带小数秒 + 时区，形如 `2025-04-27T12:00:00.123456789Z`。
    private static let iso8601Full: ISO8601DateFormatter = {
        let f = ISO8601DateFormatter()
        f.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        return f
    }()

    /// 不带小数秒，形如 `2025-04-27T12:00:00Z`。Go `time.RFC3339` 默认输出。
    private static let iso8601NoFraction: ISO8601DateFormatter = {
        let f = ISO8601DateFormatter()
        f.formatOptions = [.withInternetDateTime]
        return f
    }()
}

// MARK: - Request / Response DTOs

/// 登录请求。
struct LoginRequest: Codable, Equatable, Sendable {
    let username: String
    let password: String
}

/// 登录响应。对齐后端 handler.loginResponse。
struct LoginResponse: Codable, Equatable, Sendable {
    let token: String
    let expiresAt: Date
    let username: String

    enum CodingKeys: String, CodingKey {
        case token
        case expiresAt = "expires_at"
        case username
    }
}

/// 健康检查响应。对齐 router.go 里 /health 的 gin.H。
struct HealthResponse: Codable, Equatable, Sendable {
    let status: String
    let server: String
    let version: String
    /// RFC3339 时间戳字符串（时区 UTC）；后端用 `time.Now().UTC().Format(time.RFC3339)`。
    let time: Date
}

/// 204 / 空 body 的占位类型。
struct EmptyResponse: Codable, Equatable, Sendable {}
