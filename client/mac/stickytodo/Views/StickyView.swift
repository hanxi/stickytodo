//
//  StickyView.swift
//  stickytodo
//
//  单个便签窗口的根视图。顶部标题栏 + 中部 Todo 列表 + 底部筛选/操作条。
//  通过 StickyViewModel 封装数据与操作。
//
//  和 AppState 的关系：
//    - 读：从 note.filter 初始化 VM.filter
//    - 写：title/filter/bgColor 变化回调到 onNoteChange → App 层把 note 写回 AppState.stickies
//

import SwiftUI

struct StickyView: View {

    // MARK: - Props

    /// 初始 StickyNote；window 重建时 contentBuilder 会传入新的 note 实例。
    let initialNote: StickyNote

    /// API 客户端。
    let apiClient: APIClient

    /// 新建便签回调（"+" 按钮）。App 层调 appState.addSticky。
    let onNewSticky: () -> Void

    /// 关闭便签回调（"更多"菜单里的"删除此便签"）。
    /// id 类型为 String：云端数据源重构后，sticky 主键改为客户端生成的 UUID 字符串。
    let onCloseSticky: (String) -> Void

    /// note 任何字段变更（title / filter / bgColor）回写到 AppState。
    /// 由 App 层通过 appState.updateSticky（async）把变更落到服务端。
    let onNoteChange: (StickyNote) -> Void

    // MARK: - State

    /// 当前便签的可变副本。本地修改后通过 onNoteChange 回写。
    @State private var note: StickyNote

    /// VM 由 initialNote 构造一次。StickyView 整个实例生命周期共用同一个 VM；
    /// 若 manager 因某种原因重建 StickyView，会连带新建一个 VM 并重新 .task 拉取。
    @StateObject private var viewModel: StickyViewModel

    @State private var showingFilterEditor = false

    /// 标题栏悬停态：为 true 时显示右上角的"删除此便签" 🗑 按钮。
    /// 仿 macOS 原生 Notes.app：鼠标离开时按钮隐藏，保持便签外观干净。
    @State private var isHoveringTitle = false

    /// 「删除此便签」确认 Alert 的开关。点击标题栏 trash 按钮置 true，
    /// 用户在 Alert 中确认后才真正调 onCloseSticky → DELETE /api/sticky-notes/:id。
    /// 该操作不可撤销且会通过 WS 同步删除到所有登录本账号的设备。
    @State private var showingDeleteConfirm = false

    /// 是否跳过"删除便签"的二次确认 Alert。
    ///
    /// 默认 false（首次删除仍弹 Alert，保留安全网）。Alert 中提供"删除并不再提示"按钮，
    /// 点击后把此值写入 UserDefaults，之后再点 trash 将直接删除。
    ///
    /// 设计取舍：
    ///   - trash 按钮本身默认隐身、hover 才出现，误触概率已经很低
    ///   - 便签删除虽不可撤销，但待办数据不受影响（属于全局池），实际损失可控
    ///   - macOS 原生 Notes / Mail / Finder 都是"首次提示 + 允许关闭"的模式
    ///
    /// UserDefaults key 走 `UserDefaults.standard`（与 FrameStore 同一套），
    /// 用户想重新开启提示目前只能手动清 key（未来如有需要可在 SettingsView 暴露开关）。
    @AppStorage("sticky.skipDeleteConfirm") private var skipDeleteConfirm = false

    // MARK: - Init

    init(
        initialNote: StickyNote,
        apiClient: APIClient,
        onNewSticky: @escaping () -> Void,
        onCloseSticky: @escaping (String) -> Void,
        onNoteChange: @escaping (StickyNote) -> Void
    ) {
        self.initialNote = initialNote
        self.apiClient = apiClient
        self.onNewSticky = onNewSticky
        self.onCloseSticky = onCloseSticky
        self.onNoteChange = onNoteChange

        _note = State(initialValue: initialNote)
        _viewModel = StateObject(
            wrappedValue: StickyViewModel(
                initialFilter: initialNote.filter,
                apiClient: apiClient
            )
        )
    }

    // MARK: - Body

    var body: some View {
        VStack(spacing: 0) {
            titleBar
            Divider()
            content
            Divider()
            footerBar
        }
        // 去标题栏后 NSWindow 本身是矩形；这里用 clipShape 自绘圆角，让便签看起来像
        // 原生 Notes.app 的圆角卡片。背景色必须放在 clip 之前，否则矩形背景会超出圆角。
        .background(Color(nsColor: note.bgColor.nsColor))
        .clipShape(RoundedRectangle(cornerRadius: 8))
        .task {
            // 首次加载：绑定 VM 的 filter 变更回调（写回 note.filter），然后拉数据
            viewModel.onFilterChanged = { newFilter in
                // 闭包外部的 self 是 struct 值拷贝——通过 State 的 _note 写入才会
                // 触发 SwiftUI 更新。显式用 note.filter=... 即可，因为整个 View
                // body 在 MainActor 上，State wrapper 会正确处理。
                note.filter = newFilter
                onNoteChange(note)
            }
            await viewModel.refresh()
        }
        .onChange(of: note.title) { _ in onNoteChange(note) }
        .onChange(of: note.bgColor) { _ in onNoteChange(note) }
        .alert(item: $viewModel.currentError) { err in
            Alert(title: Text("发生错误"),
                  message: Text(err.message),
                  dismissButton: .default(Text("好的")))
        }
        // 删除便签二次确认：标题栏 trash 按钮点击后弹出（仅在 skipDeleteConfirm=false 时）。
        // 确认后调 onCloseSticky（即 App 层的 appState.removeSticky → DELETE /api/sticky-notes/:id），
        // 该操作不可撤销，通过 WS sticky.deleted 事件广播到所有登录本账号的设备。
        //
        // Alert 提供三个按钮：
        //   - 取消：关闭 Alert
        //   - 删除：本次删除，下次仍会弹
        //   - 删除并不再提示：本次删除 + 把 skipDeleteConfirm 写入 UserDefaults，之后直接删
        // 之所以用"第三个按钮"而不是"Toggle 勾选框"：SwiftUI .alert 的 actions 只能放
        // Button/TextField，不支持 Toggle；强行换 .confirmationDialog 会丢失 Alert
        // 经典视觉（图标 + 居中 + 粗体标题），macOS 原生 App 也多用此模式（Finder
        // "清倒废纸篓"、Mail "永久删除"都走这一套）。
        .alert("删除此便签？", isPresented: $showingDeleteConfirm) {
            Button("取消", role: .cancel) { }
            Button("删除", role: .destructive) {
                onCloseSticky(note.id)
            }
            Button("删除并不再提示") {
                skipDeleteConfirm = true
                onCloseSticky(note.id)
            }
        } message: {
            Text("此便签将从云端永久移除，所有登录本账号的设备都会同步删除。待办数据不受影响（待办属于全局池，不随便签删除）。")
        }
        .sheet(isPresented: $showingFilterEditor) {
            // 不再强制外层 .frame；尺寸由 FilterEditor 自身 idealWidth/idealHeight 决定，
            // 避免与内部 header/Form 高度竞争导致顶部按钮被裁切。
            FilterEditor(filter: $viewModel.filter)
        }
    }

    // MARK: - Sub-views

    @ViewBuilder
    private var titleBar: some View {
        HStack(spacing: 8) {
            // 拖动手柄：兜底入口。即使便签标题 TextField 或下方 Todo 的就地编辑 TextField
            // 处于聚焦态，按住此手柄仍可拖动整个便签窗口（TextField 会吃掉命中测试，
            // 导致 isMovableByWindowBackground 在编辑态失效，所以必须提供独立区域）。
            WindowDragHandle()
                .frame(width: 18, height: 18)
                .help("按住拖动便签")

            TextField("便签标题", text: $note.title)
                .textFieldStyle(.plain)
                .font(.headline)
                .submitLabel(.done)

            Spacer(minLength: 4)

            Button {
                // 就地草稿模式：在列表顶端插入一个空白输入行，回车保存、Esc 取消。
                // 不再弹 sheet，创建流程从 3 步缩到 1 步。
                viewModel.beginDraft()
            } label: {
                Image(systemName: "plus")
            }
            // 显式钉死样式参数，消除 SwiftUI .bordered 在不同 macOS SDK
            // 下的默认填充差异（曾出现 Xcode 15.4 / SDK 14.5 编译产物是白底、
            // Xcode 26.4 / SDK 26 是灰底的观感分歧，见 AGENTS.md §7.7）：
            //   - .tint(.secondary) 固定按钮填充基调为系统次级色，在浅黄
            //     便签背景上稳定呈现浅灰半透明
            //   - .controlSize(.small) 固定控件尺寸档位，避免 SDK 默认档
            //     位漂移造成和旁边 trash / ⋯ 两个按钮的视觉节律不一致
            .buttonStyle(.bordered)
            .tint(.secondary)
            .controlSize(.small)
            .help("新建待办")

            // 悬停 trash 删除按钮：替代被移除的窗口红灯。
            // 平时不可见（opacity 0），鼠标进入 titleBar 区域才显示，贴近原生 Notes.app 体验。
            // 放在「⋯」菜单左侧。点击行为：
            //   - 若 skipDeleteConfirm=false（默认）：弹删除确认 Alert（showingDeleteConfirm=true）
            //   - 若 skipDeleteConfirm=true（用户已选过"删除并不再提示"）：直接调 onCloseSticky
            //     → DELETE /api/sticky-notes/:id
            // 图标从 xmark.circle.fill 改为 trash 是因为原先 × 图标在 macOS 习惯里
            // 更像"关闭窗口"（非破坏性），容易误导用户；trash 能明确表达"从云端永久删除"。
            Button {
                if skipDeleteConfirm {
                    onCloseSticky(note.id)
                } else {
                    showingDeleteConfirm = true
                }
            } label: {
                Image(systemName: "trash")
                    .imageScale(.medium)
                    .foregroundStyle(.secondary)
            }
            .buttonStyle(.plain)
            .opacity(isHoveringTitle ? 1 : 0)
            .animation(.easeInOut(duration: 0.15), value: isHoveringTitle)
            .help("删除此便签")

            Menu {
                colorMenu
                Divider()
                Button {
                    onNewSticky()
                } label: {
                    Label("新增便签", systemImage: "square.stack.3d.up")
                }
                Divider()
                // 「打开设置」入口：便签上下文里快速打开主 Settings 窗口（三个 Tab：设置/历史/关于）。
                // 放在菜单最底部（符合 macOS 传统：设置类入口常驻末尾）。
                // 「删除此便签」已迁移到标题栏的 trash 按钮，菜单里不再重复。
                //
                // 跨版本实现（与 MenuBarContent.settingsButton 同一套方案，但不套 ButtonStyle
                // ——Menu 的子项由系统原生渲染，强加 .bordered/.borderedProminent 无效且会失焦）：
                //   - macOS 14+：用 SwiftUI 官方 SettingsLink；点击后 SwiftUI 负责激活 App + 打开 Settings Scene
                //   - macOS 13：SettingsLink 不可用，回退 Button + NSApp.sendAction(showSettingsWindow:)
                //
                // 注意：最初错误地尝试"统一走 sendAction 路径"，实测在 macOS 14+ 下该 selector
                // 已完全失效（只打印 "Please use SettingsLink..." 警告，不打开窗口）；必须走
                // SettingsLink 分支。
                if #available(macOS 14.0, *) {
                    SettingsLink {
                        Label("打开设置", systemImage: "gearshape")
                    }
                } else {
                    Button {
                        NSApplication.shared.activate(ignoringOtherApps: true)
                        NSApp.sendAction(Selector(("showSettingsWindow:")), to: nil, from: nil)
                    } label: {
                        Label("打开设置", systemImage: "gearshape")
                    }
                }
            } label: {
                Image(systemName: "ellipsis.circle")
            }
            .menuStyle(.borderlessButton)
            .fixedSize()
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        // 整个 titleBar 作为悬停热区：鼠标进入/离开时翻转 × 按钮的可见性。
        .contentShape(Rectangle())
        .onHover { hovering in
            isHoveringTitle = hovering
        }
    }

    @ViewBuilder
    private var colorMenu: some View {
        Menu {
            ForEach(Self.presetColors, id: \.name) { preset in
                Button(preset.name) {
                    note.bgColor = preset.color
                }
            }
        } label: {
            Label("背景色", systemImage: "paintpalette")
        }
    }

    @ViewBuilder
    private var content: some View {
        // 当列表为空且没有草稿时，显示空态/加载提示；
        // 一旦 isDrafting=true（用户点 +），就算 todos 为空也切到列表视图，
        // 让草稿行能立即出现并聚焦，避免空态提示盖住 TextField。
        if viewModel.todos.isEmpty && !viewModel.isDrafting {
            emptyOrLoadingView
        } else {
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 2) {
                    // 草稿行固定贴在列表顶部：与 TodoRow 结构对齐，保持视觉连贯。
                    // 提交/取消后 VM 把 isDrafting 置回 false，此分支消失、@State 随之销毁。
                    if viewModel.isDrafting {
                        DraftTodoRow(viewModel: viewModel)
                        Divider().opacity(0.35)
                    }
                    ForEach(viewModel.todos) { todo in
                        TodoRow(
                            viewModel: viewModel,
                            todo: todo,
                            apiClient: apiClient,
                            bgColor: note.bgColor
                        )
                        .padding(.horizontal, 10)
                        Divider().opacity(0.35)
                    }
                }
                .padding(.vertical, 6)
            }
        }
    }

    @ViewBuilder
    private var emptyOrLoadingView: some View {
        VStack(spacing: 10) {
            Spacer()
            if viewModel.isLoading {
                ProgressView().controlSize(.small)
                Text("加载中...").font(.caption).foregroundStyle(.secondary)
            } else {
                Image(systemName: "checkmark.circle")
                    .imageScale(.large)
                    .foregroundStyle(.secondary)
                Text("没有匹配的待办")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                Text("点击右上角 + 新建一条，或修改筛选条件。")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
            }
            Spacer()
        }
        .frame(maxWidth: .infinity)
        .padding()
    }

    @ViewBuilder
    private var footerBar: some View {
        HStack(spacing: 6) {
            Button {
                showingFilterEditor = true
            } label: {
                Label(filterSummary, systemImage: "line.3.horizontal.decrease.circle")
                    .labelStyle(.titleAndIcon)
            }
            .buttonStyle(.borderless)
            .help("编辑筛选条件")

            Spacer()

            if viewModel.isLoading {
                ProgressView().controlSize(.mini)
            }

            Button {
                Task { await viewModel.refresh() }
            } label: {
                Image(systemName: "arrow.clockwise")
            }
            .buttonStyle(.borderless)
            .help("刷新")
            .disabled(viewModel.isLoading)

            Text("共 \(viewModel.total) 条")
                .font(.caption2)
                .foregroundStyle(.secondary)
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
        .background(.ultraThinMaterial)
    }

    // MARK: - Derived

    /// 把当前筛选压成一行文案。空筛选显示"全部"。
    private var filterSummary: String {
        var parts: [String] = []
        if let s = viewModel.filter.status {
            parts.append(s == .done ? "已完成" : "未完成")
        }
        if !viewModel.filter.tag.isEmpty { parts.append("#\(viewModel.filter.tag)") }
        if !viewModel.filter.keyword.isEmpty { parts.append("\"\(viewModel.filter.keyword)\"") }
        if viewModel.filter.onlyDeleted { parts.append("仅已删除") }
        else if viewModel.filter.includeDeleted { parts.append("含已删除") }
        if parts.isEmpty { return "全部" }
        return parts.joined(separator: " · ")
    }

    /// 预设便签色，键盘友好、对比度合理。
    private static let presetColors: [(name: String, color: CodableRGBA)] = [
        ("便签黄", CodableRGBA(red: 1.0, green: 0.92, blue: 0.54)),
        ("薄荷绿", CodableRGBA(red: 0.78, green: 0.95, blue: 0.83)),
        ("天空蓝", CodableRGBA(red: 0.80, green: 0.90, blue: 0.98)),
        ("樱花粉", CodableRGBA(red: 0.99, green: 0.83, blue: 0.88)),
        ("薰衣草", CodableRGBA(red: 0.88, green: 0.82, blue: 0.98)),
    ]
}

