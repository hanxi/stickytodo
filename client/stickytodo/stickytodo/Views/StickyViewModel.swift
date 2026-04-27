//
//  StickyViewModel.swift
//  stickytodo
//
//  单个便签窗口的视图模型。负责：
//    1. 持有当前 TodoFilter（绑定到 StickyNote.filter，双向同步到 AppState.stickies）
//    2. 调用 apiClient.listTodos 并维护 todos / loading / error 三态
//    3. 暴露 create / complete / reopen / delete / restore / update 操作
//    4. 接收外部（App 级）的刷新请求（如从 MenuBarContent 点"刷新"）
//
//  设计注意：
//    - @MainActor：SwiftUI View 绑定本 VM 的 @Published 字段
//    - 操作返回 async throws，由 View 层 await + 处理错误展示
//    - 每次发请求前将 loading 置 true，请求结束恢复；即使快速连续调用也保证状态一致
//

import Foundation
import SwiftUI

/// 便签视图的错误展示态。独立于 APIError，保留 `String` 以便 View 直接绑定 Alert。
struct StickyViewError: Identifiable, Equatable {
    let id = UUID()
    let message: String

    init(_ error: Error) {
        if let api = error as? APIError {
            self.message = api.userMessage
        } else {
            self.message = error.localizedDescription
        }
    }

    init(message: String) {
        self.message = message
    }
}

@MainActor
final class StickyViewModel: ObservableObject {

    // MARK: - 对外状态

    /// 当前的 Todo 列表。排序由后端决定（创建倒序），View 层保持原顺序渲染。
    @Published private(set) var todos: [Todo] = []

    /// 请求中标记。用于 View 显示 ProgressView。连续请求只保留最新一次的 loading。
    @Published private(set) var isLoading: Bool = false

    /// 当前错误。非 nil 时 View 展示 Alert 并提供"重试"按钮；用户关闭 Alert 后置 nil。
    @Published var currentError: StickyViewError?

    /// 总条数；来自后端分页响应，便于分页 UI 展示。
    @Published private(set) var total: Int = 0

    /// 当前筛选条件。View 层直接绑定到 FilterEditor。setter 内会自动触发 refresh。
    @Published var filter: TodoFilter {
        didSet {
            guard filter != oldValue else { return }
            onFilterChanged?(filter)
            Task { await self.refresh() }
        }
    }

    /// 就地草稿模式标记。
    ///
    /// 取代旧的"点 + → NewTodoSheet → 填表单 → 点创建" 3 步流程：
    /// - true 时 StickyView 会在列表顶端插入一个 DraftTodoRow（TextField）
    /// - 用户输入标题后回车 → `commitDraft` 创建真实 Todo 并置回 false
    /// - 按 Esc 或留空回车 → `cancelDraft` 丢弃
    @Published var isDrafting: Bool = false

    // MARK: - 对外回调

    /// 筛选条件改变时通知外部。App 层据此把新的 filter 写回 StickyNote（AppState.stickies）。
    var onFilterChanged: ((TodoFilter) -> Void)?

    // MARK: - Init

    /// - Parameters:
    ///   - initialFilter: 便签创建时的初始筛选
    ///   - apiClient: 真实或 mock 的 API 客户端
    init(initialFilter: TodoFilter, apiClient: APIClient) {
        self.filter = initialFilter
        self.apiClient = apiClient
    }

    // MARK: - 数据加载

    /// 拉取当前 filter 对应的 Todo 列表。View 通常在 .task 里调用一次。
    func refresh() async {
        isLoading = true
        defer { isLoading = false }
        do {
            let resp = try await apiClient.listTodos(filter: filter)
            self.todos = resp.items
            self.total = resp.total
        } catch {
            self.currentError = StickyViewError(error)
        }
    }

    // MARK: - 编辑操作

    /// 新建 Todo。成功后把新记录追加到 todos 头部，避免全量刷新引发的滚动跳动。
    func createTodo(_ req: CreateTodoRequest) async {
        await perform {
            let todo = try await self.apiClient.createTodo(req)
            // 后端列表是创建倒序，新建的排在最前
            self.todos.insert(todo, at: 0)
            self.total += 1
        }
    }

    // MARK: - 就地草稿

    /// 开启草稿行。由 StickyView 的 "+" 按钮触发。幂等：已在草稿中再次点击不会报错。
    func beginDraft() {
        isDrafting = true
    }

    /// 取消草稿（Esc / 失焦 / 空标题回车）。不发请求、不改 todos。
    func cancelDraft() {
        isDrafting = false
    }

    /// 提交草稿。
    ///
    /// 规则：
    /// - trim 后为空 → 等同 cancelDraft，不调接口
    /// - 非空 → 走 createTodo（仅填 title，其他属性通过 TodoRow 编辑菜单补充）
    /// - 不论成功失败都把 isDrafting 置回 false：失败时错误通过 currentError 展示，
    ///   草稿行关闭避免阻塞用户；如需重试用户可再次点 +。
    func commitDraft(title: String) async {
        let trimmed = title.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else {
            isDrafting = false
            return
        }
        let req = CreateTodoRequest(title: trimmed)
        await createTodo(req)
        isDrafting = false
    }

    /// 标记完成。通过 complete 接口拿回更新后的 Todo，就地替换。
    func complete(id: UInt64) async {
        await perform {
            let updated = try await self.apiClient.completeTodo(id: id)
            self.replace(updated)
        }
    }

    /// 重新打开（未完成）。
    func reopen(id: UInt64) async {
        await perform {
            let updated = try await self.apiClient.reopenTodo(id: id)
            self.replace(updated)
        }
    }

    /// 软删除。后端返回 { id, deleted: true }；本地直接从数组中移除（无论当前筛选是否
    /// 包含软删，都保证 UI 立即反馈；如用户切到 only_deleted 视图会看到它）。
    func softDelete(id: UInt64) async {
        await perform {
            _ = try await self.apiClient.deleteTodo(id: id)
            self.todos.removeAll { $0.id == id }
            self.total = max(0, self.total - 1)
        }
    }

    /// 恢复软删。从 onlyDeleted 视图使用；恢复后从当前列表移除（因为筛选仍是 onlyDeleted）。
    func restore(id: UInt64) async {
        await perform {
            let restored = try await self.apiClient.restoreTodo(id: id)
            if self.filter.onlyDeleted {
                self.todos.removeAll { $0.id == id }
                self.total = max(0, self.total - 1)
            } else {
                self.replace(restored)
            }
        }
    }

    /// 更新 Todo 的 title/content/priority/tag/dueAt。
    func update(id: UInt64, req: UpdateTodoRequest) async {
        guard req.hasAny else {
            self.currentError = StickyViewError(message: "没有需要更新的字段")
            return
        }
        await perform {
            let updated = try await self.apiClient.updateTodo(id: id, req: req)
            self.replace(updated)
        }
    }

    // MARK: - Private

    private let apiClient: APIClient

    /// 用 id 替换 todos 中的一条记录；若不存在则忽略。
    private func replace(_ todo: Todo) {
        guard let idx = todos.firstIndex(where: { $0.id == todo.id }) else { return }
        todos[idx] = todo
    }

    /// 统一的操作包装：loading 状态 + 错误展示 + 异常不冒泡。
    private func perform(_ body: @MainActor () async throws -> Void) async {
        isLoading = true
        defer { isLoading = false }
        do {
            try await body()
        } catch {
            self.currentError = StickyViewError(error)
        }
    }
}
