//
//  AuditLog.swift
//  stickytodo
//
//  与后端 server/internal/model/models.go 的 AuditLog 对齐。
//  `detail` 字段后端存的是 JSON 字符串；客户端保留为 String，不做二次解析，
//  需要展示时由 UI 层自行按场景解读。
//

import Foundation

/// 后端已定义的所有 action 值。使用非穷举枚举以便未来扩展。
enum AuditAction: String, Codable, Equatable, Hashable, Sendable, CaseIterable {
    case create
    case update
    case complete
    case reopen
    case delete
    case restore
    case login
    case loginFailed = "login_failed"
    case unknown

    /// 未知值降级为 `.unknown`，不丢整条记录。
    init(from decoder: Decoder) throws {
        let raw = try decoder.singleValueContainer().decode(String.self)
        self = AuditAction(rawValue: raw) ?? .unknown
    }
}

/// 审计日志条目。
struct AuditLog: Codable, Equatable, Hashable, Identifiable, Sendable {
    let id: UInt64
    /// 关联的 Todo ID；登录类事件为 nil（后端 `todo_id,omitempty`）。
    let todoID: UInt64?
    let action: AuditAction
    /// 详情，JSON 字符串。字段非必填，默认空串。
    let detail: String
    let actor: String
    let ip: String
    let userAgent: String
    let createdAt: Date

    enum CodingKeys: String, CodingKey {
        case id
        case todoID = "todo_id"
        case action
        case detail
        case actor
        case ip
        case userAgent = "user_agent"
        case createdAt = "created_at"
    }

    /// 兼容后端 `detail` 字段缺省的场景：字段缺失或为 null 都落到空串。
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        self.id = try c.decode(UInt64.self, forKey: .id)
        self.todoID = try c.decodeIfPresent(UInt64.self, forKey: .todoID)
        self.action = try c.decode(AuditAction.self, forKey: .action)
        self.detail = (try c.decodeIfPresent(String.self, forKey: .detail)) ?? ""
        self.actor = (try c.decodeIfPresent(String.self, forKey: .actor)) ?? ""
        self.ip = (try c.decodeIfPresent(String.self, forKey: .ip)) ?? ""
        self.userAgent = (try c.decodeIfPresent(String.self, forKey: .userAgent)) ?? ""
        self.createdAt = try c.decode(Date.self, forKey: .createdAt)
    }
}

/// 审计列表响应。后端 `AuditService.ListResult` 与 `TodoListResponse` 同构，
/// 但类型不同，因此单独声明以避免泛型带来的额外复杂度。
struct AuditListResponse: Codable, Equatable, Sendable {
    let items: [AuditLog]
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
