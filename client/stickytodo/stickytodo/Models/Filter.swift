//
//  Filter.swift
//  stickytodo
//
//  便签窗口使用的筛选条件。每个便签持有独立一份，保证窗口间筛选互不影响。
//  同时作为 `GET /api/todos` 的查询参数构造源，跟后端 handler.List 保持一致。
//

import Foundation

/// 单个便签的筛选条件。所有字段均可为空；空字段等价于"不过滤"。
struct TodoFilter: Codable, Equatable, Hashable, Sendable {
    var status: TodoStatus?
    var tag: String
    var keyword: String
    var dueBefore: Date?
    /// 是否包含软删记录。后端字段 `include_deleted`。
    var includeDeleted: Bool
    /// 仅查询软删记录（优先级高于 `includeDeleted`）。
    var onlyDeleted: Bool
    var page: Int
    var pageSize: Int

    enum CodingKeys: String, CodingKey {
        case status
        case tag
        case keyword
        case dueBefore
        case includeDeleted
        case onlyDeleted
        case page
        case pageSize
    }

    static let defaultPageSize = 50

    init(
        status: TodoStatus? = nil,
        tag: String = "",
        keyword: String = "",
        dueBefore: Date? = nil,
        includeDeleted: Bool = false,
        onlyDeleted: Bool = false,
        page: Int = 1,
        pageSize: Int = TodoFilter.defaultPageSize
    ) {
        self.status = status
        self.tag = tag.trimmingCharacters(in: .whitespacesAndNewlines)
        self.keyword = keyword
        self.dueBefore = dueBefore
        self.includeDeleted = includeDeleted
        self.onlyDeleted = onlyDeleted
        self.page = max(1, page)
        self.pageSize = min(max(1, pageSize), 200)
    }

    /// 编码成 `URLQueryItem` 数组，直接喂给 `Endpoints.todos(query:)`。
    /// 语义必须严格匹配后端 handler.List；空/默认值不发送，减少 URL 体积。
    func queryItems() -> [URLQueryItem] {
        var items: [URLQueryItem] = []
        if let status = status {
            items.append(.init(name: "status", value: status.rawValue))
        }
        if !tag.isEmpty {
            items.append(.init(name: "tag", value: tag))
        }
        if !keyword.isEmpty {
            items.append(.init(name: "keyword", value: keyword))
        }
        if let due = dueBefore {
            items.append(.init(name: "due_before", value: Self.rfc3339Formatter.string(from: due)))
        }
        if includeDeleted {
            items.append(.init(name: "include_deleted", value: "1"))
        }
        if onlyDeleted {
            items.append(.init(name: "only_deleted", value: "1"))
        }
        items.append(.init(name: "page", value: String(page)))
        items.append(.init(name: "page_size", value: String(pageSize)))
        return items
    }

    /// RFC3339 formatter（带时区，无小数秒）。后端 `time.Parse(time.RFC3339, ...)`
    /// 不接受小数秒（Go time.RFC3339 的 layout 不含小数），所以这里也不输出。
    private static let rfc3339Formatter: ISO8601DateFormatter = {
        let f = ISO8601DateFormatter()
        f.formatOptions = [.withInternetDateTime]
        return f
    }()
}
