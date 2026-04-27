//
//  StickyTodoApp.swift
//  stickytodo
//
//  菜单栏常驻 App 入口。LSUIElement=YES，所以没有 Dock 图标，
//  通过 MenuBarExtra 在菜单栏显示图标；Settings Scene 提供偏好设置窗口。
//

import AppKit
import SwiftUI

@main
struct StickyTodoApp: App {
    /// 全局应用状态。@StateObject 保证 App 生命周期内唯一。
    @StateObject private var appState: AppState

    /// 便签窗口桥接器：由 App init 创建并在 init 里立刻与 appState 绑定，
    /// 避免 MenuBarExtra 面板未展开时窗口不同步（onAppear 只在用户点击菜单栏
    /// 图标后才触发）。
    @StateObject private var windowBridge: StickyWindowBridge

    init() {
        // 先构造纯数据层，再构造依赖它的 bridge。全部在主线程 / MainActor 上进行。
        let state = AppState()
        let bridge = StickyWindowBridge(appState: state)
        bridge.attach(appState: state)

        _appState = StateObject(wrappedValue: state)
        _windowBridge = StateObject(wrappedValue: bridge)
    }

    var body: some Scene {
        MenuBarExtra {
            MenuBarContent()
                .environmentObject(appState)
                // macOS 13 只支持 `onChange(of:perform:)` 单参数形式；两参数形式
                // （新/旧值同时给）是 macOS 14+ 才有的。我们的 deployment target
                // 是 13.0，用旧形式即可。
                .onChange(of: appState.stickies) { newValue in
                    windowBridge.syncWindows(stickies: newValue, isVisible: appState.isAuthenticated)
                }
                .onChange(of: appState.isAuthenticated) { newVisible in
                    windowBridge.syncWindows(stickies: appState.stickies, isVisible: newVisible)
                }
        } label: {
            // SF Symbols：note 作为应用图标；系统会根据主题自动切换前景色。
            Image(systemName: "note.text")
        }
        .menuBarExtraStyle(.window)

        // Settings Scene：用户通过 ⌘, 或 MenuBarExtra 里的"设置"按钮打开。
        Settings {
            SettingsView()
                .environmentObject(appState)
        }
    }
}

/// 把 AppState 和 StickyWindowManager 的回调桥接起来。
///
/// 独立一个 ObservableObject 而不是让 AppState 持有 WindowManager：
/// - AppState 是 pure data + networking，不感知 AppKit 窗口
/// - WindowManager 只管 NSWindow 集合，不感知持久化
/// - Bridge 做两边的协议翻译：
///     manager.onStickyClosed → appState.removeSticky
///     manager.onStickyFrameChanged → appState.updateStickyFrame
///     NSApplication.willTerminate → appState.flushStickiesSave()
@MainActor
final class StickyWindowBridge: ObservableObject {

    /// - Parameter appState: 用于在每个便签窗口里注入 apiClient 与回调。
    ///   contentBuilder 闭包捕获 appState 的弱引用；AppState 销毁后便签窗口
    ///   本就应该消失（App 退出），所以弱引用不会造成显示异常。
    init(appState: AppState) {
        // 先把 appState 绑进 builder，这样每次打开/更新便签都能拿到最新的 client。
        self.manager = StickyWindowManager { [weak appState] note in
            // appState 为 nil 时（理论上不会发生）回退到空视图以避免崩溃。
            guard let appState else {
                return AnyView(Color(nsColor: note.bgColor.nsColor))
            }
            return AnyView(
                StickyView(
                    initialNote: note,
                    apiClient: appState.apiClient,
                    onNewSticky: { appState.addSticky() },
                    onCloseSticky: { id in appState.removeSticky(id: id) },
                    onNoteChange: { updated in appState.updateSticky(updated) }
                )
            )
        }
    }

    deinit {
        if let token = terminateObserver {
            NotificationCenter.default.removeObserver(token)
        }
    }

    /// 把 manager 与 AppState 绑定。幂等——多次调用只保留最后一次的闭包。
    func attach(appState: AppState) {
        manager.onStickyClosed = { [weak appState] id in
            appState?.removeSticky(id: id)
        }
        manager.onStickyFrameChanged = { [weak appState] id, rect in
            appState?.updateStickyFrame(id: id, frame: rect)
        }

        // 第一次绑定时才注册 willTerminate。NotificationCenter 的闭包
        // 在主线程回调（App 终止流程在主线程）。
        if terminateObserver == nil {
            terminateObserver = NotificationCenter.default.addObserver(
                forName: NSApplication.willTerminateNotification,
                object: nil,
                queue: .main
            ) { [weak appState] _ in
                // willTerminate 回调虽然在主线程，但不在 MainActor 的静态类型上下文里；
                // flushStickiesSave 是 @MainActor 方法，需要 assumeIsolated 切入。
                MainActor.assumeIsolated {
                    appState?.flushStickiesSave()
                }
            }
        }

        // 首次绑定立刻同步一次，确保启动即已登录场景下窗口恢复。
        syncWindows(stickies: appState.stickies, isVisible: appState.isAuthenticated)
    }

    /// 把最新便签快照同步到窗口集合。
    func syncWindows(stickies: [StickyNote], isVisible: Bool) {
        manager.sync(stickies: stickies, isVisible: isVisible)
    }

    // MARK: - Private

    private let manager: StickyWindowManager
    private var terminateObserver: NSObjectProtocol?
}

