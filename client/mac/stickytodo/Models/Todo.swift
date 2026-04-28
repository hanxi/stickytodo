//
//  Todo.swift
//  stickytodo
//
//  与后端 server/internal/model/models.go 的 Todo 结构一一对应。
//  所有时间字段使用后端输出的 RFC3339（含纳秒）格式，由 APIClient 的 JSONDecoder 统一解码。
//

import Foundation

/// Todo 的完成状态。对应后端 StatusPending / StatusDone。
///
/// 使用 RawRepresentable 的 String 枚举 + `unknown` 分支：
/// 后端未来新增枚举值不会导致旧客户端直接解码失败。
enum TodoStatus: String, Codable, Equatable, Hashable, CaseIterable, Sendable {
    case pending
    case done

    /// 自定义 decode：未知值解码为 `.pending`，避免因服务端扩展枚举导致整个列表解码失败。
    init(from decoder: Decoder) throws {
        let raw = try decoder.singleValueContainer().decode(String.self)
        self = TodoStatus(rawValue: raw) ?? .pending
    }
}

/// 单条待办事项。字段顺序与后端 GORM model 对齐，便于对比审阅。
struct Todo: Codable, Equatable, Hashable, Identifiable, Sendable {
    /// 主键。后端 `uint` → 客户端使用 `UInt64` 足够覆盖。
    let id: UInt64
    var title: String
    var content: String
    var status: TodoStatus
    /// 优先级 0~3，0 最低。后端用 int 存储。
    var priority: Int
    /// 单值标签字符串（后端是单值，不是数组）。
    var tag: String
    /// 截止时间，可为 nil。
    var dueAt: Date?
    /// 完成时间，状态为 done 时非 nil。
    var completedAt: Date?
    let createdAt: Date
    let updatedAt: Date
    /// 软删时间；后端使用 `gorm.DeletedAt`，未软删为 nil。
    let deletedAt: Date?

    enum CodingKeys: String, CodingKey {
        case id
        case title
        case content
        case status
        case priority
        case tag
        case dueAt = "due_at"
        case completedAt = "completed_at"
        case createdAt = "created_at"
        case updatedAt = "updated_at"
        case deletedAt = "deleted_at"
    }
}

/// 列表接口 `GET /api/todos` 的分页响应。
struct TodoListResponse: Codable, Equatable, Sendable {
    let items: [Todo]
    let total: Int
    let page: Int
    let pageSize: Int

    enum CodingKeys: String, CodingKey {
        case items
        case total
        case page
        case pageSize = "page_size"
    }
}

/// 创建 Todo 的请求体。对应后端 handler.createRequest。
///
/// 与 updateRequest 分开定义是为了编码时能正确区分：
///   - create：全部字段按值传；tag 空串表示无标签
///   - update：所有字段都是可选（nil = 不更新）
struct CreateTodoRequest: Codable, Equatable, Sendable {
    var title: String
    var content: String
    var priority: Int
    var tag: String
    var dueAt: Date?

    enum CodingKeys: String, CodingKey {
        case title
        case content
        case priority
        case tag
        case dueAt = "due_at"
    }

    init(title: String, content: String = "", priority: Int = 0, tag: String = "", dueAt: Date? = nil) {
        self.title = title
        self.content = content
        self.priority = priority
        self.tag = tag
        self.dueAt = dueAt
    }
}

/// 更新 Todo 的请求体。字段全部可选。
///
/// `clearDueAt=true` 对应后端 updateRequest.ClearDueAt，用于把截止时间写 NULL。
/// 注意：`clearDueAt=true` 时后端会忽略 `dueAt` 的值。
struct UpdateTodoRequest: Codable, Equatable, Sendable {
    var title: String?
    var content: String?
    var priority: Int?
    var tag: String?
    var dueAt: Date?
    var clearDueAt: Bool

    enum CodingKeys: String, CodingKey {
        case title
        case content
        case priority
        case tag
        case dueAt = "due_at"
        case clearDueAt = "clear_due_at"
    }

    init(
        title: String? = nil,
        content: String? = nil,
        priority: Int? = nil,
        tag: String? = nil,
        dueAt: Date? = nil,
        clearDueAt: Bool = false
    ) {
        self.title = title
        self.content = content
        self.priority = priority
        self.tag = tag
        self.dueAt = dueAt
        self.clearDueAt = clearDueAt
    }

    /// 是否至少包含一个要变更的字段；用于前端在发请求前做一次本地校验，
    /// 与后端 `UpdateInput.HasAny()` 保持同样的判定。
    var hasAny: Bool {
        title != nil || content != nil || priority != nil ||
        tag != nil || dueAt != nil || clearDueAt
    }

    /// 自定义编码：`clearDueAt` 为 false 时不写入 JSON，减少体积且与 Go 的
    /// `omitempty` bool=false 语义一致。
    func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encodeIfPresent(title, forKey: .title)
        try c.encodeIfPresent(content, forKey: .content)
        try c.encodeIfPresent(priority, forKey: .priority)
        try c.encodeIfPresent(tag, forKey: .tag)
        try c.encodeIfPresent(dueAt, forKey: .dueAt)
        if clearDueAt {
            try c.encode(true, forKey: .clearDueAt)
        }
    }
}

/// `DELETE /api/todos/:id` 的响应。
struct DeleteTodoResponse: Codable, Equatable, Sendable {
    let id: UInt64
    let deleted: Bool
}

/// `GET /api/tags` 的响应。
struct TagListResponse: Codable, Equatable, Sendable {
    let tags: [String]
}
