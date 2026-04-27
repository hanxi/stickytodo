//
//  WindowDragHandle.swift
//  stickytodo
//
//  无标题栏便签窗口的"拖动手柄"控件。
//
//  背景：
//    NSWindow 的 isMovableByWindowBackground 只在用户点击的位置不是 first responder
//    控件时生效；一旦 TextField（便签标题、就地编辑的 Todo 标题/tag）处于编辑态，
//    按住它们拖动会被 TextField 的选区/光标操作吃掉事件，无法再整体拖动窗口。
//
//    为在编辑态下仍提供可靠的拖动途径，这里在 titleBar 左侧放一个专门的手柄区域。
//    无论任何控件是否聚焦，按住手柄都能拖整窗。
//
//  视觉设计：
//    - 2 列 × 3 行的圆点阵（grip dots），macOS 原生"可抓取"语感，不撞 ellipsis 菜单图标
//    - 常态 opacity 0.35（淡淡可见，不抢标题的视觉焦点）
//    - hover 上去 opacity 0.85，同时光标变 openHand —— 强提示用户这里可拖动
//    - 尺寸 10×16：宽度刚好容纳两列点 + 点距，高度对齐标题文字基线
//
//  实现分层：
//    SwiftUI 层负责绘制点阵 + hover 态切换；AppKit 层（DragHandleNSView）接管 mouseDown
//    并调 window.performDrag。二者通过 ZStack 叠放：NSView 透明在上接事件，
//    点阵视图 allowsHitTesting(false) 只负责显示，不干扰点击。
//

import AppKit
import SwiftUI

/// SwiftUI 包装的窗口拖动手柄。放到 titleBar 上即可；默认尺寸 10×16。
///
/// 外部可通过 .frame 覆盖尺寸，但建议保留默认值——点阵几何是按 10×16 微调过的，
/// 改大改小都会让点位对不齐。
struct WindowDragHandle: View {

    /// 鼠标 hover 态。影响点阵透明度，让 hover 提示比光标变化更早被用户感知。
    @State private var isHovering = false

    var body: some View {
        ZStack {
            // 底层：纯视觉点阵。不响应任何点击——所有鼠标事件都穿透到上层 NSView。
            GripDotsView(highlighted: isHovering)
                .allowsHitTesting(false)

            // 上层：AppKit 事件接收器。负责 performDrag + 光标切换。
            DragHandleRepresentable()
                .onHover { hovering in
                    // SwiftUI 的 onHover 在被 NSView 覆盖时依然能触发——AppKit 的
                    // mouseEntered/Exited 会冒泡给 SwiftUI 层处理。
                    isHovering = hovering
                }
        }
        .frame(width: 10, height: 16)
        .help("按住拖动便签")
    }
}

/// 6 个小圆点组成的 grip 视觉。
///
/// 参考 macOS 原生「resize grip」「drawer handle」的点阵风格。
/// 之所以自己画而不用 SF Symbol：
///   - `line.3.horizontal` 容易被理解成"菜单/列表"
///   - `ellipsis` 是三点横排，和「⋯」菜单图标冲突
///   - 自己画可以精细控制点径、间距、色调
private struct GripDotsView: View {

    /// 是否处于高亮态（鼠标悬停）。高亮时 opacity 从 0.35 提到 0.85。
    let highlighted: Bool

    /// 单个圆点直径。2.2pt 在视网膜屏上约 4 物理像素，边缘柔和不突兀。
    private let dotSize: CGFloat = 2.2

    /// 列间距 / 行间距。相同值能让整体保持正方网格感。
    private let spacing: CGFloat = 2.5

    var body: some View {
        HStack(spacing: spacing) {
            column
            column
        }
        .foregroundStyle(.primary)
        .opacity(highlighted ? 0.85 : 0.35)
        // 透明度切换加动效：避免 hover 时生硬闪动。
        .animation(.easeInOut(duration: 0.12), value: highlighted)
    }

    @ViewBuilder
    private var column: some View {
        VStack(spacing: spacing) {
            dot
            dot
            dot
        }
    }

    private var dot: some View {
        Circle()
            .frame(width: dotSize, height: dotSize)
    }
}

/// AppKit 事件接收层：承载 mouseDown → performDrag + openHand 光标。
private struct DragHandleRepresentable: NSViewRepresentable {

    func makeNSView(context: Context) -> DragHandleNSView {
        DragHandleNSView()
    }

    func updateNSView(_ nsView: DragHandleNSView, context: Context) {
        // 本控件无状态，无需响应外部变化。
    }
}

/// 真正承载鼠标事件的 AppKit 视图。
///
/// 为什么不能用 SwiftUI 的 .gesture + DragGesture 来做：
///   DragGesture 会给出相对位移，但 AppKit 的拖窗真正能正确处理菜单栏吸附、
///   空间切换、多屏边界等系统语义的入口是 NSWindow.performDrag；只要把 mouseDown
///   事件转手给 performDrag 就能得到完全等价于标题栏拖动的行为。
final class DragHandleNSView: NSView {

    override var mouseDownCanMoveWindow: Bool {
        // 让 AppKit 知道这个视图响应 mouseDown 不是为了点击，而是为了拖窗；
        // 配合下方 performDrag 一起使用，避免 SwiftUI 命中测试拦截。
        true
    }

    override func resetCursorRects() {
        // hover 进入手柄区域时变成"可抓取"手型，提示用户这里能按住拖动。
        discardCursorRects()
        addCursorRect(bounds, cursor: .openHand)
    }

    override func mouseDown(with event: NSEvent) {
        // performDrag 会阻塞当前 runloop 直到用户松手，期间 AppKit 负责驱动所有位移。
        // 即使窗口是 .borderless（无标题栏），这个调用一样生效。
        window?.performDrag(with: event)
    }
}
