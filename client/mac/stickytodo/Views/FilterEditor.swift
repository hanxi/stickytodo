//
//  FilterEditor.swift
//  stickytodo
//
//  便签筛选条件编辑弹窗。通过 @Binding<TodoFilter> 直接双向同步；
//  保存按钮仅在字段有变化时可用，取消按钮回滚到进入 sheet 时的快照。
//
//  对齐后端 handler.List 的参数语义：
//    - status：pending / done / nil(全部)
//    - tag：单值字符串；空串代表"不过滤"
//    - keyword：title/content 模糊查询
//    - due_before：截止时间上限
//    - include_deleted / only_deleted：后端逻辑互斥（only_deleted 优先）
//

import SwiftUI

struct FilterEditor: View {

    @Binding var filter: TodoFilter
    @Environment(\.dismiss) private var dismiss

    /// 进入 sheet 时的原始快照；"取消"时用于回滚。
    @State private var snapshot: TodoFilter

    /// 本地编辑副本。保存时才写回 @Binding。
    @State private var draft: TodoFilter

    /// UI 开关：是否设置了"截止前"。Date 本身可 nil，但 DatePicker 不支持可空，
    /// 因此单独拿一个 Bool 控制是否发送 dueBefore。
    @State private var hasDueBefore: Bool

    @State private var dueBeforeDraft: Date

    init(filter: Binding<TodoFilter>) {
        self._filter = filter
        let current = filter.wrappedValue
        _snapshot = State(initialValue: current)
        _draft = State(initialValue: current)
        _hasDueBefore = State(initialValue: current.dueBefore != nil)
        _dueBeforeDraft = State(initialValue: current.dueBefore ?? Date())
    }

    var body: some View {
        VStack(spacing: 0) {
            header
            Divider()
            // 用 ScrollView 包一层 Form：当 Form 的 intrinsic 高度超出可用空间时，让内容
            // 滚动而不是去挤压上方 header。在 borderless 窗口（无标题栏便签）弹出的
            // sheet 中，SwiftUI 对 idealSize 的计算不太稳定，不加 ScrollView 会出现
            // header 被向上挤出可见区、顶部"筛选/重置/保存"按钮被裁切的问题。
            ScrollView {
                Form {
                    Section("状态") {
                        Picker("完成状态", selection: statusBinding) {
                            ForEach(StatusChoice.allCases, id: \.self) { choice in
                                Text(choice.label).tag(choice)
                            }
                        }
                        .pickerStyle(.segmented)
                        .labelsHidden()
                    }

                    Section("标签与关键词") {
                        TextField("标签（精确匹配）", text: $draft.tag)
                            .textFieldStyle(.roundedBorder)
                        TextField("关键词（标题/备注）", text: $draft.keyword)
                            .textFieldStyle(.roundedBorder)
                    }

                    Section("截止时间") {
                        Toggle("按截止时间过滤", isOn: $hasDueBefore)
                        if hasDueBefore {
                            DatePicker("截止时间早于", selection: $dueBeforeDraft)
                        }
                    }

                    Section("软删") {
                        Toggle("包含已删除", isOn: $draft.includeDeleted)
                            .disabled(draft.onlyDeleted)
                        Toggle("仅显示已删除", isOn: $draft.onlyDeleted)
                    }

                    Section("分页") {
                        Stepper("每页 \(draft.pageSize) 条", value: $draft.pageSize, in: 10...200, step: 10)
                    }
                }
                .formStyle(.grouped)
            }
        }
        // 放宽尺寸并给出 idealSize，避免和 Form intrinsic 内容相互竞争导致 header 被裁。
        .frame(minWidth: 460, idealWidth: 480, minHeight: 500, idealHeight: 560)
    }

    // MARK: - Sub-views

    @ViewBuilder
    private var header: some View {
        HStack {
            Button("取消") {
                dismiss()
            }
            .keyboardShortcut(.cancelAction)

            Spacer()

            Text("筛选").font(.headline)

            Spacer()

            Button("重置") {
                draft = TodoFilter()
                hasDueBefore = false
                dueBeforeDraft = Date()
            }

            Button("保存") {
                commit()
                dismiss()
            }
            .buttonStyle(.borderedProminent)
            .keyboardShortcut(.defaultAction)
            .disabled(isUnchanged)
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 12)
        // 关键：锁定 header 的 vertical intrinsic 高度，阻止下方 Form 抢它的高度。
        // 缺这一行时 SwiftUI 会在高度紧张时把 HStack 向上压扁直到按钮行不可见。
        .fixedSize(horizontal: false, vertical: true)
    }

    // MARK: - Bindings

    /// 真正的 status Binding：nil ↔ StatusChoice.all 往返。
    private var statusBinding: Binding<StatusChoice> {
        Binding(
            get: {
                switch draft.status {
                case .none: return .all
                case .pending: return .pending
                case .done: return .done
                }
            },
            set: { newVal in
                draft.status = newVal.asStatus
            }
        )
    }

    // MARK: - Derived

    private var isUnchanged: Bool {
        // 把 hasDueBefore/dueBeforeDraft 合成到 draft 再跟 snapshot 比较。
        var synth = draft
        synth.dueBefore = hasDueBefore ? dueBeforeDraft : nil
        return synth == snapshot
    }

    private func commit() {
        var synth = draft
        synth.dueBefore = hasDueBefore ? dueBeforeDraft : nil
        // only_deleted 的语义优先级最高；开启后默认视觉上禁用 include_deleted，
        // 同时在提交时也清掉避免后端同时看到两个标志。
        if synth.onlyDeleted {
            synth.includeDeleted = false
        }
        filter = synth
    }
}

// MARK: - StatusChoice

/// 给 Picker 用的三态枚举。TodoStatus 本身没有"全部"概念，这里单独包一层。
private enum StatusChoice: Hashable, CaseIterable {
    case all
    case pending
    case done

    var label: String {
        switch self {
        case .all: return "全部"
        case .pending: return "未完成"
        case .done: return "已完成"
        }
    }

    var asStatus: TodoStatus? {
        switch self {
        case .all: return nil
        case .pending: return .pending
        case .done: return .done
        }
    }
}
