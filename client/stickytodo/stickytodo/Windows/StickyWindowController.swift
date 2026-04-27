//
//  StickyWindowController.swift
//  stickytodo
//
//  单个便签对应的 NSWindow 控制器。
//
//  为何不用 WindowGroup：
//    - WindowGroup 适合文档型 App；便签窗口需要自定义 level（.floating 置顶）、
//      无 Dock 图标下的窗口行为、按 StickyNote.id 精确定位已打开窗口等控制能力，
//      用 AppKit 的 NSWindow + NSWindowController 更直接。
//
//  行为：
//    - 窗口样式：titled + closable + miniaturizable + resizable
//    - 窗口层级：.floating（便签置顶）
//    - 监听 didMove / didResize → 回调 onFrameChange(note.id, 新 frame)
//    - 监听 willClose → 回调 onClose(note.id)
//    - update(note:) 时若 frame 与当前不同，则写回 NSWindow.setFrame（避免多便签联动循环，
//      只在差异超出阈值时才更新；否则忽略）
//

import AppKit
import SwiftUI

/// 便签窗口控制器。由 StickyWindowManager 持有，跟随便签数组生命周期。
@MainActor
final class StickyWindowController {

    // MARK: - 回调

    /// 窗口 frame 变化（用户拖动/改尺寸）回调；manager 据此更新 AppState。
    var onFrameChange: ((UUID, CGRect) -> Void)?

    /// 窗口被关闭（用户点红灯）回调；manager 据此从 AppState.stickies 删除。
    var onClose: ((UUID) -> Void)?

    // MARK: - 只读属性

    /// 当前管理的便签 ID。
    let stickyID: UUID

    // MARK: - 初始化

    /// - Parameters:
    ///   - note: 初始 StickyNote 快照。
    ///   - contentBuilder: 根据当前 note 构造 SwiftUI 视图；每次 update(note:) 会
    ///     重新调用以刷新 hosting view 的 rootView。阶段十一只传占位视图；
    ///     阶段十二由调用方注入 StickyView。
    init(
        note: StickyNote,
        contentBuilder: @escaping @MainActor (StickyNote) -> AnyView
    ) {
        self.stickyID = note.id
        self.contentBuilder = contentBuilder
        self.currentNote = note

        // 使用 borderless 无标题栏窗口，配合 fullSizeContentView 让 SwiftUI 渲染占满全窗。
        // 必须用 StickyNSWindow 子类，否则 borderless 窗口默认 canBecomeKey == false，
        // 会导致便签内的 TextField / 标题输入框无法获得焦点。
        let window = StickyNSWindow(
            contentRect: note.frame.cgRect,
            styleMask: [.borderless, .resizable, .fullSizeContentView],
            backing: .buffered,
            defer: false
        )
        window.level = .floating
        window.isReleasedWhenClosed = false
        window.hidesOnDeactivate = false
        // 背景拖动：用户可按住便签空白区域拖动整个窗口，贴近原生 Notes.app 体验。
        window.isMovableByWindowBackground = true
        // 保留系统阴影，使无边框窗口仍具备"漂浮"感。
        window.hasShadow = true
        // 去标题栏后窗口是方的；让 SwiftUI 层自绘圆角，需要窗口本身透明。
        window.backgroundColor = .clear
        window.isOpaque = false
        // 菜单栏 App（LSUIElement）下，未激活 App 时默认不接收键盘事件；
        // 打开设置/点击便签时 App 会被 activate，这里不需要额外配置。

        let hosting = NSHostingView(rootView: contentBuilder(note))
        hosting.autoresizingMask = [.width, .height]
        window.contentView = hosting

        self.window = window
        self.hostingView = hosting

        registerNotifications()
    }

    deinit {
        // Notification 注册的是 block 观察者，需在销毁时移除。
        // deinit 不在 MainActor 上；NotificationCenter.removeObserver 是线程安全的。
        if let token = frameObserver {
            NotificationCenter.default.removeObserver(token)
        }
        if let token = resizeObserver {
            NotificationCenter.default.removeObserver(token)
        }
        if let token = closeObserver {
            NotificationCenter.default.removeObserver(token)
        }
    }

    // MARK: - 对外操作

    /// 显示窗口（如已显示则置顶）。
    func show() {
        window.makeKeyAndOrderFront(nil)
    }

    /// 隐藏窗口（不销毁）。logout 时使用。
    func hide() {
        window.orderOut(nil)
    }

    /// 关闭窗口并释放资源。由 manager 在便签被删除时调用。
    /// 会触发 willClose 回调一次；manager 内置去重避免二次触发删除。
    func close() {
        isClosingProgrammatically = true
        window.close()
        isClosingProgrammatically = false
    }

    /// 应用新的便签状态到窗口（仅窗口壳层面，不动 rootView）。
    ///
    /// 为什么不重建 rootView：
    ///   StickyView 内部持有 @State note / @StateObject viewModel，它们是便签的
    ///   权威可信源——用户编辑标题/筛选都会就地写入 AppState。如果每次 AppState
    ///   变更都重建 rootView，就会用 AppState 里的快照覆盖用户正在输入的值，并
    ///   导致 @StateObject VM 被重建、todos 列表反复重新拉取。
    ///
    ///   作为权衡：外部路径（例如 MenuBarContent 删除一个便签）只会触发 manager
    ///   的 remove，不会修改"现有便签"的字段；所以 rootView 不需要响应外部字段
    ///   变化。frame 变化由 SwiftUI 外部（NSWindow）承载，不影响 rootView 内容。
    func update(note: StickyNote) {
        currentNote = note

        // 去标题栏后 window.title 不再显示，无需再写入；标题由便签内 UI（StickyView titleBar）承担。

        let newFrame = note.frame.cgRect
        if !Self.frameRoughlyEqual(window.frame, newFrame) {
            // display=true 立即绘制，animate=false 避免便签窗反复抖动。
            window.setFrame(newFrame, display: true, animate: false)
        }
    }

    // MARK: - Private

    private let window: NSWindow
    private let hostingView: NSHostingView<AnyView>
    private let contentBuilder: @MainActor (StickyNote) -> AnyView
    private var currentNote: StickyNote

    /// 标记"当前 close 由代码触发"，避免 willClose 回调把便签又删一次。
    private var isClosingProgrammatically = false

    private var frameObserver: NSObjectProtocol?
    private var resizeObserver: NSObjectProtocol?
    private var closeObserver: NSObjectProtocol?

    /// NSWindow frame 变更的最小阈值（像素）。低于此值视为噪声（如亚像素舍入）。
    private static let frameEqualEpsilon: CGFloat = 0.5

    private static func frameRoughlyEqual(_ a: CGRect, _ b: CGRect) -> Bool {
        abs(a.origin.x - b.origin.x) < frameEqualEpsilon &&
        abs(a.origin.y - b.origin.y) < frameEqualEpsilon &&
        abs(a.size.width - b.size.width) < frameEqualEpsilon &&
        abs(a.size.height - b.size.height) < frameEqualEpsilon
    }

    private func registerNotifications() {
        let center = NotificationCenter.default
        let id = stickyID

        // frame 变化：didMove + didResize 都要监听，didMove 不覆盖 resize，反之亦然。
        frameObserver = center.addObserver(
            forName: NSWindow.didMoveNotification,
            object: window,
            queue: .main
        ) { [weak self] _ in
            // notification 回调在 main runloop；显式切回 MainActor 保险。
            guard let self else { return }
            MainActor.assumeIsolated {
                self.emitFrameChange(id: id)
            }
        }

        resizeObserver = center.addObserver(
            forName: NSWindow.didResizeNotification,
            object: window,
            queue: .main
        ) { [weak self] _ in
            guard let self else { return }
            MainActor.assumeIsolated {
                self.emitFrameChange(id: id)
            }
        }

        closeObserver = center.addObserver(
            forName: NSWindow.willCloseNotification,
            object: window,
            queue: .main
        ) { [weak self] _ in
            guard let self else { return }
            MainActor.assumeIsolated {
                // 代码主动触发的 close 不再回调 onClose，避免 manager 重复删除。
                guard !self.isClosingProgrammatically else { return }
                self.onClose?(id)
            }
        }
    }

    private func emitFrameChange(id: UUID) {
        let newFrame = window.frame
        // 与 currentNote 差异小于阈值不回调，减少写盘频率。
        if Self.frameRoughlyEqual(currentNote.frame.cgRect, newFrame) {
            return
        }
        onFrameChange?(id, newFrame)
    }
}

// MARK: - StickyNSWindow

/// 便签专用 NSWindow 子类。
///
/// 背景：borderless（无标题栏）窗口默认 `canBecomeKey == false` 且 `canBecomeMain == false`，
/// 会导致窗口内部的 TextField 无法获取焦点、键盘输入被系统吞掉。这里显式覆写为 true
/// 恢复正常的输入行为，同时保持无标题栏的视觉效果。
private final class StickyNSWindow: NSWindow {
    override var canBecomeKey: Bool { true }
    override var canBecomeMain: Bool { true }
}
