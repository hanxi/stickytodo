//
//  DraftTodoRow.swift
//  stickytodo
//
//  就地新建 Todo 的草稿行。
//
//  取代旧的 NewTodoSheet 表单弹窗：用户点 "+" 后在列表顶端直接出现一个空白
//  TextField，聚焦 → 输入 → 回车 → 创建。Esc 或留空回车即取消。
//
//  交互：
//    - onAppear 时把焦点打到 TextField（@FocusState）
//    - .onSubmit → viewModel.commitDraft
//    - .onExitCommand → viewModel.cancelDraft（Esc 在 macOS SwiftUI 下的标准接法）
//
//  取舍：
//    不展示优先级/标签/截止时间等属性输入。计划里明确：极简 1 步新建流程；
//    补充属性请走 TodoRow 的编辑菜单。
//

import SwiftUI

struct DraftTodoRow: View {

    @ObservedObject var viewModel: StickyViewModel

    /// 草稿标题。首次出现时为空，用户输入时更新；提交后由 VM 置 isDrafting=false
    /// 让整个 DraftTodoRow 从父视图树中移除，本 @State 随视图销毁自然释放。
    @State private var draftTitle: String = ""

    /// TextField 焦点控制。@FocusState 是 macOS/iOS SwiftUI 用来编程控制焦点的
    /// 标准工具；onAppear 时把它设为 true 让草稿行出现即进入输入态。
    @FocusState private var focused: Bool

    var body: some View {
        HStack(alignment: .center, spacing: 10) {
            // 与 TodoRow 的完成勾选框视觉对齐：草稿尚未落库，用空心圆占位，
            // 变成真实 Todo 后才有真正的点击/勾选交互。
            Image(systemName: "circle")
                .imageScale(.large)
                .foregroundStyle(.secondary)
                .frame(width: 22, height: 22)

            TextField("待办内容，回车保存", text: $draftTitle)
                .textFieldStyle(.plain)
                .focused($focused)
                // 回车触发提交；trim 后为空 VM 内部会当作取消处理。
                .onSubmit {
                    Task { await viewModel.commitDraft(title: draftTitle) }
                }
                // .onExitCommand 是 macOS SwiftUI 接 Esc 键的官方入口；
                // 不能用 keyboardShortcut(.cancelAction) 因为那通常只对 Button 生效。
                .onExitCommand {
                    viewModel.cancelDraft()
                }
                .onAppear {
                    focused = true
                }

            Spacer(minLength: 6)
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
    }
}
