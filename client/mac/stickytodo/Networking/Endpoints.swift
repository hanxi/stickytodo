//
//  Endpoints.swift
//  stickytodo
//
//  集中管理后端 URL。所有路径在此声明，便于随后端演进统一修改。
//

import Foundation

/// API 错误：路径拼接阶段的错误（非 HTTP 错误）。
enum EndpointError: Error, Equatable {
    /// Base URL 无法解析成 URLComponents。
    case invalidBaseURL(String)
    /// 最终拼装出的 URL 为 nil（极少见，通常意味着 base + path 非法）。
    case composeFailed(String)
}

/// 路由与 URL 构造器。每个方法对应后端 router 中的一条路径。
///
/// 不保存 Base URL，所有方法都要求显式传入，避免隐式全局状态带来的误用。
/// APIClient 内部会把当前 AppState.serverBaseURL 传进来。
struct Endpoints {
    /// `GET /health` 健康检查。无需登录。
    static func health(base: String) throws -> URL {
        try make(base: base, path: "/health")
    }

    /// `POST /api/login`。
    static func login(base: String) throws -> URL {
        try make(base: base, path: "/api/login")
    }

    // MARK: Todos

    /// `GET /api/todos` + 查询参数；或者 `POST /api/todos`。
    static func todos(base: String, query: [URLQueryItem] = []) throws -> URL {
        try make(base: base, path: "/api/todos", query: query)
    }

    /// `/api/todos/:id`。
    static func todo(base: String, id: UInt64) throws -> URL {
        try make(base: base, path: "/api/todos/\(id)")
    }

    /// `/api/todos/:id?include_deleted=1`。
    /// 只在 includeDeleted=true 时附加 query；false 时等同于 `todo(base:id:)`。
    static func todo(base: String, id: UInt64, includeDeleted: Bool) throws -> URL {
        let query: [URLQueryItem] = includeDeleted
            ? [URLQueryItem(name: "include_deleted", value: "1")]
            : []
        return try make(base: base, path: "/api/todos/\(id)", query: query)
    }

    /// `/api/todos/:id/complete`。
    static func todoComplete(base: String, id: UInt64) throws -> URL {
        try make(base: base, path: "/api/todos/\(id)/complete")
    }

    /// `/api/todos/:id/reopen`。
    static func todoReopen(base: String, id: UInt64) throws -> URL {
        try make(base: base, path: "/api/todos/\(id)/reopen")
    }

    /// `/api/todos/:id/restore`。
    static func todoRestore(base: String, id: UInt64) throws -> URL {
        try make(base: base, path: "/api/todos/\(id)/restore")
    }

    /// `/api/todos/:id/history`（分页）。
    static func todoHistory(base: String, id: UInt64, page: Int, pageSize: Int) throws -> URL {
        try make(base: base, path: "/api/todos/\(id)/history", query: [
            .init(name: "page", value: String(max(1, page))),
            .init(name: "page_size", value: String(min(max(1, pageSize), 200))),
        ])
    }

    // MARK: Sticky Notes

    /// `GET /api/sticky-notes`（列表）。
    ///
    /// 后端返回 `{"items": [...]}`，由 APIClient 解码为 `[StickyNoteDTO]` 后再
    /// 映射为视图层 `[StickyNote]`。
    static func stickyNotes(base: String) throws -> URL {
        try make(base: base, path: "/api/sticky-notes")
    }

    /// `GET|PUT|DELETE /api/sticky-notes/:id`。
    ///
    /// 同一条 URL 承载 3 种方法：
    ///   - GET 取单条（本客户端暂未用到，但保留入口便于未来主动刷新）
    ///   - PUT 幂等 upsert（handler 端会兼容 create / update 两种语义）
    ///   - DELETE 软删（后端返回 {"id":..., "deleted":true}）
    ///
    /// 参数：
    ///   - id：客户端生成的 sticky id；必须是非空字符串。URL-path 段内
    ///     特殊字符（如空格、`#`、`?`）会破坏路径解析，调用前通过
    ///     `addingPercentEncoding(withAllowedCharacters: .urlPathAllowed)`
    ///     做百分号编码；编码失败时降级为原串，让后端用 400 拒绝错误输入，
    ///     而不是在客户端静默抛错掩盖 bug。
    static func stickyNote(base: String, id: String) throws -> URL {
        let encoded = id.addingPercentEncoding(
            withAllowedCharacters: .urlPathAllowed
        ) ?? id
        return try make(base: base, path: "/api/sticky-notes/\(encoded)")
    }

    // MARK: Audit & Tags

    /// `/api/audit-logs` 全局审计列表。
    static func auditLogs(base: String, query: [URLQueryItem] = []) throws -> URL {
        try make(base: base, path: "/api/audit-logs", query: query)
    }

    /// `/api/tags` 标签列表。
    static func tags(base: String) throws -> URL {
        try make(base: base, path: "/api/tags")
    }

    // MARK: - Private

    /// 拼 URL 的统一实现。
    /// - `base` 必须是带 scheme 的合法地址，如 `http://127.0.0.1:8080`。
    /// - `path` 以 `/` 开头；base 中既有的非根 path 会被视为 API 前缀拼接到 path 前面。
    ///   例如 base=`http://host/api` + path=`/api/login` → `http://host/api/api/login`。
    ///   如果用户不希望双 `/api`，应把 base 配成 `http://host`。
    /// - `query` 非空时会附加到 URL。
    private static func make(base: String, path: String, query: [URLQueryItem] = []) throws -> URL {
        let trimmed = base.trimmingCharacters(in: .whitespacesAndNewlines)
        guard var components = URLComponents(string: trimmed) else {
            throw EndpointError.invalidBaseURL(trimmed)
        }
        // URLComponents 要求 scheme 必须存在（例如 http/https），否则视为非法——
        // 避免把 "127.0.0.1:8080" 这种把 host 当作 scheme 的误输入放过。
        guard let scheme = components.scheme, !scheme.isEmpty else {
            throw EndpointError.invalidBaseURL(trimmed)
        }
        // 规范化 base 的 path：
        //   - "" 或 "/"   → 视为无前缀，等价于直接覆盖为 path
        //   - 其他         → 作为前缀拼接到 path 前，并在末尾去掉多余 `/`
        let basePath = components.path
        let trimmedBasePath = basePath.trimmingCharacters(in: CharacterSet(charactersIn: "/"))
        if trimmedBasePath.isEmpty {
            components.path = path
        } else {
            // path 以 "/" 开头；前缀加上后不会出现双 "/"。
            components.path = "/" + trimmedBasePath + path
        }
        components.queryItems = query.isEmpty ? nil : query
        guard let url = components.url else {
            throw EndpointError.composeFailed("\(trimmed) + \(path)")
        }
        return url
    }
}
