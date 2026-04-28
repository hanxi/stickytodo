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

    // MARK: Sticky Notes

    /// `GET /api/sticky-notes`。拉取当前用户的全部便签列表。
    ///
    /// 返回值是"视图层 StickyNote"——即 bg_color/filter 已经从 JSON 字符串
    /// 解码回结构化类型；调用方（AppState）无需再做任何解码。
    ///
    /// 解码失败会以 APIError.decoding 抛出，而不是静默用默认值兜底；
    /// 原因：跨端 schema 字段是关键契约，任何一条记录解不出来都意味着
    /// 后端 schema 升级或持久化脏数据，必须在 UI 显式暴露，方便发现回归。
    func listStickies() async throws -> [StickyNote] {
        let url = try endpoint { try Endpoints.stickyNotes(base: $0) }
        let resp: StickyListResponse = try await send(url: url, method: "GET")
        return try resp.items.map { try $0.toStickyNote() }
    }

    /// `GET /api/sticky-notes/:id`。取单条便签。
    /// 当前客户端默认以 list 端点一次性拉取全部便签，因此此方法主要用于
    /// "WS 事件 sticky.upserted 到来时做单条刷新"这种窄场景。
    func getSticky(id: String) async throws -> StickyNote {
        let url = try endpoint { try Endpoints.stickyNote(base: $0, id: id) }
        let dto: StickyNoteDTO = try await send(url: url, method: "GET")
        return try dto.toStickyNote()
    }

    /// `PUT /api/sticky-notes/:id`。幂等 upsert：服务端按 id 做创建或更新。
    ///
    /// - id：客户端生成的字符串 id（必须已存在于本地 StickyNote）
    /// - view：要落盘的视图状态（title / bgColor / filter）
    ///
    /// 注意：frame 字段在 macOS 端**恒为 "{}"**——窗口位置由 FrameStore 独立
    /// 维护到本机 UserDefaults，不跨设备同步。与 Web 端约定一致。
    ///
    /// 服务端会用自己的时间戳覆写 created_at/updated_at，返回的 StickyNote
    /// 才是权威结果，调用方应该用它替换内存中的乐观值。
    func upsertSticky(id: String, view: StickyNote) async throws -> StickyNote {
        let url = try endpoint { try Endpoints.stickyNote(base: $0, id: id) }
        let body = UpsertStickyRequest(
            title: view.title,
            frame: UpsertStickyRequest.frameConstant,
            bgColor: try Self.encodeBgColor(view.bgColor),
            filter: try Self.encodeFilter(view.filter)
        )
        let dto: StickyNoteDTO = try await send(url: url, method: "PUT", body: body)
        return try dto.toStickyNote()
    }

    /// `DELETE /api/sticky-notes/:id`。软删。
    /// 返回值是服务端响应体 `{id, deleted}`；调用方通常只关心 HTTP 2xx。
    @discardableResult
    func deleteSticky(id: String) async throws -> DeleteStickyResponse {
        let url = try endpoint { try Endpoints.stickyNote(base: $0, id: id) }
        return try await send(url: url, method: "DELETE")
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

    // MARK: Sticky codec

    /// 把 CodableRGBA 编码成 JSON 字符串（形如 `{"alpha":1,"blue":0.54,"green":0.92,"red":1}`）。
    /// 用独立的 encoder（.sortedKeys）保证输出稳定、便于调试；
    /// 不走 Self.jsonEncoder 是因为后者带有 Date 的 custom 策略，而颜色结构里
    /// 没有 Date 字段，也不希望未来有人给 CodableRGBA 加 Date 字段时悄悄改变编码。
    fileprivate static func encodeBgColor(_ rgba: CodableRGBA) throws -> String {
        do {
            let data = try stickyAuxEncoder.encode(rgba)
            guard let s = String(data: data, encoding: .utf8) else {
                throw APIError.decoding("encode bg_color: utf8 decode failed")
            }
            return s
        } catch let e as APIError {
            throw e
        } catch {
            throw APIError.decoding("encode bg_color: \(error.localizedDescription)")
        }
    }

    /// 把 JSON 字符串解码成 CodableRGBA。
    /// 失败即抛 APIError.decoding：调用方（StickyNoteDTO.toStickyNote）会把错误
    /// 一路抛到 list/get 接口之外，由 UI 以 alert 展示，避免脏数据被静默吞下。
    fileprivate static func decodeBgColor(_ raw: String) throws -> CodableRGBA {
        guard let data = raw.data(using: .utf8) else {
            throw APIError.decoding("decode bg_color: not utf8")
        }
        do {
            return try stickyAuxDecoder.decode(CodableRGBA.self, from: data)
        } catch {
            throw APIError.decoding("decode bg_color: \(error.localizedDescription)")
        }
    }

    /// 把 TodoFilter 编码成 **snake_case** JSON 字符串。
    ///
    /// 为什么不直接用 `JSONEncoder.encode(filter)`：
    ///   TodoFilter.CodingKeys 是 camelCase（`dueBefore` / `includeDeleted` 等），
    ///   但 Web 端 `filterToJSON` 直接 `JSON.stringify(filter)`，产出的是 snake_case
    ///   （`due_before` / `include_deleted`）。两端写同一账号的便签必须互通，
    ///   因此这里**必须走一层中间 DTO**把 TodoFilter 映射到 snake_case 字段名。
    ///   同理 decodeFilter。
    ///
    /// 直接改 TodoFilter 的 CodingKeys 会破坏其他消费方（FilterEditor / 本地缓存），
    /// 改动面不可控，所以只在 sticky 序列化这一条路径做局部适配。
    fileprivate static func encodeFilter(_ filter: TodoFilter) throws -> String {
        let dto = TodoFilterDTO(filter)
        do {
            let data = try stickyAuxEncoder.encode(dto)
            guard let s = String(data: data, encoding: .utf8) else {
                throw APIError.decoding("encode filter: utf8 decode failed")
            }
            return s
        } catch let e as APIError {
            throw e
        } catch {
            throw APIError.decoding("encode filter: \(error.localizedDescription)")
        }
    }

    /// 把 snake_case JSON 字符串解码回 TodoFilter。见 encodeFilter 的跨端契约说明。
    fileprivate static func decodeFilter(_ raw: String) throws -> TodoFilter {
        guard let data = raw.data(using: .utf8) else {
            throw APIError.decoding("decode filter: not utf8")
        }
        do {
            let dto = try stickyAuxDecoder.decode(TodoFilterDTO.self, from: data)
            return dto.toTodoFilter()
        } catch {
            throw APIError.decoding("decode filter: \(error.localizedDescription)")
        }
    }
}

// MARK: - Sticky codec 辅助

/// sticky 的 bg_color / filter 字段编解码专用 JSONEncoder。
///
/// 与 APIClient.jsonEncoder 分离：
///   - jsonEncoder 有 Date 的 custom 策略，只为服务端顶层 JSON 服务；
///   - 本 encoder 无 Date 策略、带 sortedKeys，保证 bg_color / filter 编码结果
///     稳定可比，便于 diff 调试。
private let stickyAuxEncoder: JSONEncoder = {
    let e = JSONEncoder()
    e.outputFormatting = [.sortedKeys]
    return e
}()

/// sticky 的 bg_color / filter 字段解码专用 JSONDecoder。
/// TodoFilter DTO 里没有 Date 字段（dueBefore 以 ISO8601 字符串承载），所以无需
/// 自定义 dateDecodingStrategy。
private let stickyAuxDecoder = JSONDecoder()

/// TodoFilter 在 sticky.filter JSON 里的 snake_case 表达。
///
/// 所有字段**必须**与 Web 端 types/sticky.ts `TodoFilter` 逐字段对齐：
///   - status:          "pending" | "done" | "all" | null（all 表示"不限状态"）
///   - tag:             string
///   - keyword:         string
///   - due_before:      string（RFC3339，可选）
///   - include_deleted: bool
///   - only_deleted:    bool
///   - page:            int
///   - page_size:       int
///
/// 与 macOS 的 TodoFilter 结构有两点差异：
///   1) camelCase ↔ snake_case（由本 DTO 的 CodingKeys 翻译）
///   2) Web 端的 status 可取 "all"；macOS 端用 `TodoStatus?` 表达（nil 即 all）。
///      toTodoFilter / init 里做互转，保证两端语义等价。
///   3) due_before 字段：macOS 端是 Date，需要以 RFC3339 字符串载入/卸出。
private struct TodoFilterDTO: Codable {
    var status: String?
    var tag: String
    var keyword: String
    var dueBefore: String?
    var includeDeleted: Bool
    var onlyDeleted: Bool
    var page: Int
    var pageSize: Int

    enum CodingKeys: String, CodingKey {
        case status
        case tag
        case keyword
        case dueBefore = "due_before"
        case includeDeleted = "include_deleted"
        case onlyDeleted = "only_deleted"
        case page
        case pageSize = "page_size"
    }

    init(_ f: TodoFilter) {
        // TodoStatus? → String?：nil 映射为 "all"，与 Web 端 defaultFilter.status 一致
        self.status = f.status?.rawValue ?? "all"
        self.tag = f.tag
        self.keyword = f.keyword
        if let due = f.dueBefore {
            self.dueBefore = TodoFilterDTO.rfc3339Formatter.string(from: due)
        } else {
            self.dueBefore = nil
        }
        self.includeDeleted = f.includeDeleted
        self.onlyDeleted = f.onlyDeleted
        self.page = f.page
        self.pageSize = f.pageSize
    }

    func toTodoFilter() -> TodoFilter {
        // status 的三态互转：
        //   "all" / nil / 未知值 → TodoFilter.status = nil（不筛选）
        //   "pending" / "done"   → 对应 TodoStatus
        let mappedStatus: TodoStatus?
        if let raw = status, raw != "all" {
            mappedStatus = TodoStatus(rawValue: raw)
        } else {
            mappedStatus = nil
        }
        let mappedDue: Date?
        if let s = dueBefore, !s.isEmpty {
            mappedDue = TodoFilterDTO.rfc3339Formatter.date(from: s)
                ?? TodoFilterDTO.rfc3339NoFractionFormatter.date(from: s)
        } else {
            mappedDue = nil
        }
        return TodoFilter(
            status: mappedStatus,
            tag: tag,
            keyword: keyword,
            dueBefore: mappedDue,
            includeDeleted: includeDeleted,
            onlyDeleted: onlyDeleted,
            page: page,
            pageSize: pageSize
        )
    }

    /// 与 APIClient.iso8601Full 一致（带小数秒 + 时区）。
    /// 独立一份避免引用 APIClient 的 fileprivate static：TodoFilterDTO 是 private，
    /// 内部持有一份 formatter 更简单直接。
    static let rfc3339Formatter: ISO8601DateFormatter = {
        let f = ISO8601DateFormatter()
        f.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        return f
    }()

    /// 与 APIClient.iso8601NoFraction 一致；Web 端的 due_before 通常由
    /// `<input type="datetime-local">` 产出，没有小数秒。
    static let rfc3339NoFractionFormatter: ISO8601DateFormatter = {
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

// MARK: - Sticky Note DTOs

/// `GET /api/sticky-notes/:id` / `PUT /api/sticky-notes/:id` 的响应体，
/// 精确对齐后端 `server/internal/model/models.go` 的 `StickyNote` JSON 输出。
///
/// 三个 TEXT 字段（frame / bg_color / filter）以 JSON 字符串形式传递，
/// 由 `toStickyNote()` 负责二次解码回视图层结构。
struct StickyNoteDTO: Codable, Equatable, Sendable {
    let id: String
    let title: String
    let frame: String
    let bgColor: String
    let filter: String
    let createdAt: Date
    let updatedAt: Date

    enum CodingKeys: String, CodingKey {
        case id
        case title
        case frame
        case bgColor = "bg_color"
        case filter
        case createdAt = "created_at"
        case updatedAt = "updated_at"
    }

    /// 把 DTO 转成视图层 StickyNote。
    ///
    /// 失败行为：bg_color / filter 任一 JSON 解码失败都会抛 APIError.decoding，
    /// 由 APIClient 的 list/get 调用方捕获。此行为是 note 里明确要求的约束：
    /// "DTO → StickyNote 映射失败时必须抛 APIError.decoding，而不是默认值兜底"。
    ///
    /// 注意：frame 字段在本客户端**有意不消费**——位置由 FrameStore 独立持有，
    /// 与 sticky 业务数据解耦。这里即使 frame 是 "{}" 或服务端存了其他内容都被忽略。
    func toStickyNote() throws -> StickyNote {
        let rgba = try APIClient.decodeBgColor(bgColor)
        let filterValue = try APIClient.decodeFilter(filter)
        return StickyNote(
            id: id,
            title: title,
            bgColor: rgba,
            filter: filterValue,
            createdAt: createdAt,
            updatedAt: updatedAt
        )
    }
}

/// `PUT /api/sticky-notes/:id` 的请求体。字段集与后端 handler.upsertStickyRequest 严格对齐。
///
/// 字段值构造约定：
///   - title：视图层 StickyNote.title 原样传入
///   - frame：**恒为 "{}"**——macOS 端不上报窗口位置到服务端（跨设备位置无意义）
///   - bg_color：CodableRGBA 的 JSON，由 APIClient.encodeBgColor 产出
///   - filter：TodoFilter 的 snake_case JSON，由 APIClient.encodeFilter 产出
struct UpsertStickyRequest: Codable, Equatable, Sendable {
    let title: String
    let frame: String
    let bgColor: String
    let filter: String

    enum CodingKeys: String, CodingKey {
        case title
        case frame
        case bgColor = "bg_color"
        case filter
    }

    /// frame 的恒定值：空 JSON 对象。与 Web 端 `viewToUpsertRequest` 一致。
    /// 不写 "null" 是因为后端字段类型为非 null TEXT，写 "{}" 保证 json.Valid 通过。
    static let frameConstant = "{}"
}

/// `GET /api/sticky-notes` 的响应。对齐后端 handler 分页语义（当前未分页，items 是全量）。
struct StickyListResponse: Codable, Equatable, Sendable {
    let items: [StickyNoteDTO]
}

/// `DELETE /api/sticky-notes/:id` 的响应。
struct DeleteStickyResponse: Codable, Equatable, Sendable {
    let id: String
    let deleted: Bool
}
