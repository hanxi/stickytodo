//
//  TodoRow.swift
//  stickytodo
//
//  单条 Todo 的行视图。左 checkbox、中 title/tag/due、右 操作菜单。
//
//  操作：
//    - 勾选：complete / reopen（软删记录禁用）
//    - 编辑：弹 EditTodoSheet（内联在本文件）调 viewModel.update
//    - 删除：二次确认 → viewModel.softDelete（软删不可逆前用 Alert 确认）
//    - 恢复：仅软删记录可见 → viewModel.restore
//    - 历史：弹 HistoryView（sheet；只看该 todo 的历史）
//

import SwiftUI

struct TodoRow: View {
    @ObservedObject var viewModel: StickyViewModel
    let todo: Todo
    /// 用于打开历史弹窗时传给 HistoryView。由父视图注入，不要在 VM 里暴露。
    let apiClient: APIClient

    /// 当前便签的背景色。用于决定优先级等前景控件选择亮/深配色，保证对比度。
    /// 由父视图（StickyView）透传；便签换色时 SwiftUI 会自动重绘本行。
    let bgColor: CodableRGBA

    @State private var showingDeleteConfirm = false
    @State private var showingEditSheet = false
    @State private var showingHistorySheet = false

    /// 整行 hover 态：用于控制右侧「⋯」操作菜单按钮的可见性。
    /// 未 hover 时按钮 opacity=0（仍占位，行高不跳动）；hover 时淡入显示。
    /// 降低待办密集时的视觉噪声，贴近原生 Notes.app / Reminders.app 的体验。
    @State private var isHovering = false

    // MARK: - 就地编辑态

    /// 进入标题编辑态。单击标题文字触发；Text 与 TextField 之间的切换由此标记驱动。
    ///
    /// 仅本地 @State，不需要同步到 VM：编辑过程中不改远端，回车/失焦时才调 update。
    /// Esc 时丢弃 draftTitle 不提交，保持 todo.title 原值不变。
    @State private var isEditingTitle = false
    @State private var draftTitle: String = ""
    @FocusState private var titleFocused: Bool

    /// 进入标签编辑态。单击 tag Label（或空 tag 时点 "+ 标签"）触发。
    @State private var isEditingTag = false
    @State private var draftTag: String = ""
    @FocusState private var tagFocused: Bool

    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            // 左：checkbox / 软删禁用
            checkbox

            // 中：title + meta
            VStack(alignment: .leading, spacing: 4) {
                titleView

                metaLine
            }

            Spacer(minLength: 6)

            // 右：操作菜单
            // 未 hover 时 opacity=0 但仍占位，保证行高不跳动；鼠标滑入时淡入。
            // 不用条件渲染（`if isHovering { actionsMenu }`）的原因：条件渲染会让
            // 按钮从布局中移除，鼠标移入的瞬间行宽重算 → 视觉抖动。
            // 注意：本行整体已有一层 `.opacity(todo.deletedAt == nil ? 1.0 : 0.55)`，
            // 软删行 hover 时实际 opacity = 0.55（透明度相乘），这是期望行为——
            // 软删行的操作按钮本来就该视觉上弱化，和整行保持一致。
            actionsMenu
                .opacity(isHovering ? 1 : 0)
                .animation(.easeInOut(duration: 0.15), value: isHovering)
        }
        .padding(.vertical, 4)
        .opacity(todo.deletedAt == nil ? 1.0 : 0.55)
        // 整行作为 hover 热区：透明间隙也参与命中测试，避免鼠标滑过 Spacer 时按钮闪烁。
        .contentShape(Rectangle())
        .onHover { hovering in
            isHovering = hovering
        }
        .alert("确认删除", isPresented: $showingDeleteConfirm) {
            Button("取消", role: .cancel) { }
            Button("删除", role: .destructive) {
                Task { await viewModel.softDelete(id: todo.id) }
            }
        } message: {
            Text("此操作会将待办放入回收状态，可通过\"仅已删除\"筛选恢复。")
        }
        .sheet(isPresented: $showingEditSheet) {
            EditTodoSheet(todo: todo) { req in
                await viewModel.update(id: todo.id, req: req)
            }
        }
        .sheet(isPresented: $showingHistorySheet) {
            HistoryView(mode: .todo(id: todo.id, title: todo.title),
                        apiClient: apiClient)
                .frame(minWidth: 480, minHeight: 360)
        }
    }

    // MARK: - Sub-components

    @ViewBuilder
    private var checkbox: some View {
        if todo.deletedAt != nil {
            Image(systemName: "trash.slash")
                .foregroundStyle(.secondary)
                .frame(width: 22, height: 22)
        } else {
            Button {
                Task {
                    if todo.status == .done {
                        await viewModel.reopen(id: todo.id)
                    } else {
                        await viewModel.complete(id: todo.id)
                    }
                }
            } label: {
                Image(systemName: todo.status == .done ? "checkmark.circle.fill" : "circle")
                    .imageScale(.large)
                    .foregroundStyle(todo.status == .done ? .green : .secondary)
            }
            .buttonStyle(.plain)
            .help(todo.status == .done ? "标记为未完成" : "标记为完成")
        }
    }

    // MARK: - Title 就地编辑

    /// 标题视图：根据 isEditingTitle 在只读 Text / 可编辑 TextField 间切换。
    ///
    /// 设计点：
    /// - 软删 todo 不可编辑（下方直接渲染 Text，不挂点击手势）
    /// - 进入编辑：把 todo.title 拷贝到 draftTitle，然后 isEditingTitle = true
    /// - 提交：trim 后与原值比较；相等则不发请求，只退出编辑态；不等才走 VM.update
    /// - Esc：直接丢弃 draftTitle，退出编辑态，不发请求
    @ViewBuilder
    private var titleView: some View {
        if isEditingTitle && todo.deletedAt == nil {
            TextField("标题", text: $draftTitle)
                .textFieldStyle(.plain)
                .font(.body)
                .focused($titleFocused)
                .onSubmit { Task { await commitTitleEdit() } }
                .onExitCommand { cancelTitleEdit() }
                .onAppear { titleFocused = true }
        } else {
            Text(todo.title)
                .font(.body)
                .foregroundStyle(todo.status == .done ? .secondary : .primary)
                .strikethrough(todo.status == .done)
                .lineLimit(2)
                // 软删记录保留只读视图，避免用户误触进入编辑态后又发不出请求。
                .contentShape(Rectangle())
                .onTapGesture {
                    guard todo.deletedAt == nil else { return }
                    beginTitleEdit()
                }
        }
    }

    private func beginTitleEdit() {
        draftTitle = todo.title
        isEditingTitle = true
    }

    private func cancelTitleEdit() {
        isEditingTitle = false
        draftTitle = ""
    }

    private func commitTitleEdit() async {
        let trimmed = draftTitle.trimmingCharacters(in: .whitespacesAndNewlines)
        // 空标题视为非法修改，回滚到原值；不发请求，用户可再次点击重新输入。
        guard !trimmed.isEmpty else {
            cancelTitleEdit()
            return
        }
        // 未变化直接退出编辑态，节省一次 update 请求。
        guard trimmed != todo.title else {
            cancelTitleEdit()
            return
        }
        var req = UpdateTodoRequest()
        req.title = trimmed
        isEditingTitle = false
        draftTitle = ""
        await viewModel.update(id: todo.id, req: req)
    }

    // MARK: - Tag 就地编辑

    private func beginTagEdit() {
        draftTag = todo.tag
        isEditingTag = true
    }

    private func cancelTagEdit() {
        isEditingTag = false
        draftTag = ""
    }

    private func commitTagEdit() async {
        let trimmed = draftTag.trimmingCharacters(in: .whitespacesAndNewlines)
        guard trimmed != todo.tag else {
            cancelTagEdit()
            return
        }
        var req = UpdateTodoRequest()
        req.tag = trimmed
        isEditingTag = false
        draftTag = ""
        await viewModel.update(id: todo.id, req: req)
    }

    /// 是否需要渲染 metaLine 整行。
    ///
    /// 三项（tag / due / priority）全空且未处于 tag 编辑态时返回 false，
    /// 此时整行直接不进入视图树，VStack 会自然收缩高度——
    /// 解决"空 meta 留出一行空白"的视觉问题。
    ///
    /// 进入 tag 编辑态时恒显示（保证 TextField 可见）。
    private var shouldShowMeta: Bool {
        if isEditingTag { return true }
        if !todo.tag.isEmpty { return true }
        if todo.dueAt != nil { return true }
        if todo.priority > 0 { return true }
        return false
    }

    @ViewBuilder
    private var metaLine: some View {
        if shouldShowMeta {
            HStack(spacing: 6) {
                tagView

                if let due = todo.dueAt {
                    Label(Self.dueFormatter.string(from: due), systemImage: "calendar")
                        .font(.caption)
                        .foregroundStyle(isOverdue(due) ? .red : .secondary)
                }
                if todo.priority > 0 {
                    Label("P\(todo.priority)", systemImage: "flag")
                        .font(.caption)
                        .foregroundStyle(priorityColor(todo.priority))
                }
            }
        }
    }

    /// 标签视图：根据 isEditingTag 在 Label / TextField 间切换。
    ///
    /// 空 tag 不再渲染占位入口——首次添加 tag 走「⋯」菜单 → "编辑"。
    /// 这样未加过 tag 的 todo 在无 due / 无优先级时能整行收缩消失，视觉更紧凑。
    /// 软删 todo 不允许编辑 tag。
    @ViewBuilder
    private var tagView: some View {
        if isEditingTag && todo.deletedAt == nil {
            HStack(spacing: 4) {
                Image(systemName: "tag")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                TextField("标签", text: $draftTag)
                    .textFieldStyle(.plain)
                    .font(.caption)
                    .frame(minWidth: 60, maxWidth: 120)
                    .focused($tagFocused)
                    .onSubmit { Task { await commitTagEdit() } }
                    .onExitCommand { cancelTagEdit() }
                    .onAppear { tagFocused = true }
            }
        } else if !todo.tag.isEmpty {
            Label(todo.tag, systemImage: "tag")
                .font(.caption)
                .foregroundStyle(.secondary)
                .contentShape(Rectangle())
                .onTapGesture {
                    guard todo.deletedAt == nil else { return }
                    beginTagEdit()
                }
        }
        // 空 tag 不渲染任何内容；依赖 metaLine 的条件渲染让整行消失。
    }

    @ViewBuilder
    private var actionsMenu: some View {
        Menu {
            if todo.deletedAt == nil {
                Button {
                    showingEditSheet = true
                } label: {
                    Label("编辑", systemImage: "pencil")
                }
                Button(role: .destructive) {
                    showingDeleteConfirm = true
                } label: {
                    Label("删除", systemImage: "trash")
                }
            } else {
                Button {
                    Task { await viewModel.restore(id: todo.id) }
                } label: {
                    Label("恢复", systemImage: "arrow.uturn.backward")
                }
            }
            Divider()
            Button {
                showingHistorySheet = true
            } label: {
                Label("历史", systemImage: "clock.arrow.circlepath")
            }
        } label: {
            Image(systemName: "ellipsis.circle")
                .imageScale(.medium)
                .foregroundStyle(.secondary)
        }
        .menuStyle(.borderlessButton)
        .fixedSize()
    }

    // MARK: - Helpers

    private func isOverdue(_ date: Date) -> Bool {
        todo.status != .done && date < Date()
    }

    /// 优先级文字颜色。
    ///
    /// 目前 5 种预设便签色（黄/绿/蓝/粉/紫）的 luminance 都 > 0.8，全部走深色分支；
    /// 原本的 .yellow / .orange 在这些浅背景上对比度不足几乎看不清。
    ///
    /// 阈值判断封装在 CodableRGBA.isLightBackground 里，未来若新增深色便签，
    /// 这里会自动切到亮色分支，不需要调分支逻辑。
    private func priorityColor(_ p: Int) -> Color {
        if bgColor.isLightBackground {
            // 浅背景：用加深配色。sRGB 值参考 Material / Apple Human Interface 可读配色。
            switch p {
            case 3: return Color(red: 0.64, green: 0.00, blue: 0.00)   // 深红 #A30000
            case 2: return Color(red: 0.75, green: 0.29, blue: 0.00)   // 深橙 #C04A00
            case 1: return Color(red: 0.54, green: 0.44, blue: 0.00)   // 深黄棕 #8A6F00
            default: return .secondary
            }
        } else {
            // 深背景：保留原本的亮色调色板。
            switch p {
            case 3: return .red
            case 2: return .orange
            case 1: return .yellow
            default: return .secondary
            }
        }
    }

    private static let dueFormatter: DateFormatter = {
        let f = DateFormatter()
        f.locale = Locale.current
        f.dateFormat = "MM-dd HH:mm"
        return f
    }()
}

// MARK: - EditTodoSheet

/// 编辑 Todo 的弹窗；通过回调把 UpdateTodoRequest 反馈给调用方（VM）。
private struct EditTodoSheet: View {
    let todo: Todo
    /// 点击保存后调用；由调用方决定是否真的发请求。
    let onSave: (UpdateTodoRequest) async -> Void

    @Environment(\.dismiss) private var dismiss

    @State private var title: String
    @State private var content: String
    @State private var tag: String
    @State private var priority: Int
    @State private var hasDueDate: Bool
    @State private var dueDate: Date
    @State private var submitting: Bool = false

    init(todo: Todo, onSave: @escaping (UpdateTodoRequest) async -> Void) {
        self.todo = todo
        self.onSave = onSave
        _title = State(initialValue: todo.title)
        _content = State(initialValue: todo.content)
        _tag = State(initialValue: todo.tag)
        _priority = State(initialValue: todo.priority)
        _hasDueDate = State(initialValue: todo.dueAt != nil)
        // 没有截止时间时用"今天同一时刻"作为默认值，切开关 on/off 用户体验好。
        _dueDate = State(initialValue: todo.dueAt ?? Date())
    }

    var body: some View {
        VStack(spacing: 0) {
            headerBar
            Divider()
            Form {
                Section("内容") {
                    TextField("标题", text: $title)
                    TextField("备注", text: $content, axis: .vertical)
                        .lineLimit(3...6)
                }
                Section("属性") {
                    TextField("标签", text: $tag)
                    Picker("优先级", selection: $priority) {
                        Text("P0").tag(0)
                        Text("P1").tag(1)
                        Text("P2").tag(2)
                        Text("P3").tag(3)
                    }
                    Toggle("设置截止时间", isOn: $hasDueDate)
                    if hasDueDate {
                        DatePicker("截止时间", selection: $dueDate)
                    }
                }
            }
            .formStyle(.grouped)
        }
        .frame(minWidth: 420, minHeight: 420)
    }

    @ViewBuilder
    private var headerBar: some View {
        HStack {
            Button("取消") { dismiss() }
                .keyboardShortcut(.cancelAction)
            Spacer()
            Text("编辑待办 #\(todo.id)")
                .font(.headline)
            Spacer()
            Button("保存") {
                Task { await submit() }
            }
            .buttonStyle(.borderedProminent)
            .keyboardShortcut(.defaultAction)
            .disabled(submitting || title.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
        }
        .padding()
    }

    private func submit() async {
        submitting = true
        defer { submitting = false }

        // 只把真的发生变化的字段带过去，避免无意义的写入与审计噪声。
        var req = UpdateTodoRequest()
        let trimmedTitle = title.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmedTitle != todo.title { req.title = trimmedTitle }
        if content != todo.content { req.content = content }
        if tag != todo.tag { req.tag = tag }
        if priority != todo.priority { req.priority = priority }

        if hasDueDate {
            // 如果原来也有值且相等则不传；否则传新值
            if todo.dueAt != dueDate { req.dueAt = dueDate }
        } else if todo.dueAt != nil {
            // 用户把截止时间关掉了
            req.clearDueAt = true
        }

        guard req.hasAny else {
            dismiss()
            return
        }
        await onSave(req)
        dismiss()
    }
}
