//
//  StickyTodoApp.swift
//  stickytodo
//
//  菜单栏常驻 App 入口。LSUIElement=YES，所以没有 Dock 图标，
//  通过 MenuBarExtra 在菜单栏显示图标；Settings Scene 提供偏好设置窗口。
//

import AppKit
import Combine
import os
import SwiftUI

@main
struct StickyTodoApp: App {
    /// 全局应用状态。@StateObject 保证 App 生命周期内唯一。
    @StateObject private var appState: AppState

    /// 便签窗口桥接器：由 App init 创建并在 init 里立刻与 appState 绑定，
    /// 避免 MenuBarExtra 面板未展开时窗口不同步（onAppear 只在用户点击菜单栏
    /// 图标后才触发）。
    ///
    /// Bridge 在 attach 里用 Combine sink 直接订阅 `appState.$stickies` 和
    /// `appState.$isAuthenticated`，**不**依赖挂在 SwiftUI body 上的 `.onChange`——
    /// 后者在 MenuBarExtra 面板未展开时整个子树不挂载、不求值，会导致
    /// WS 推送的 sticky 增删直到用户点开菜单栏才生效（历史 bug）。
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
            // ⚠️ 不要在此挂 `.onChange(of: appState.stickies)` / `.onChange(of: appState.isAuthenticated)`。
            // MenuBarExtra 面板折叠时 body 子树不参与渲染树，SwiftUI 的 .onChange
            // 会被跳过，导致窗口无法响应 WS 推送的便签增删。桥接改由 Bridge 自己
            // 用 Combine sink 订阅 AppState 的 @Published 源完成。
            MenuBarContent()
                .environmentObject(appState)
        } label: {
            // 品牌菜单栏图标（模板图 / TEMPLATE）：由 scripts/generate-icons.sh 从
            // assets/branding/stickytodo-menubar.svg 渲染，落在
            // Assets.xcassets/MenuBarIcon.imageset/（@1x 18px / @2x 36px / @3x 54px）。
            // 该 imageset 的 Contents.json 声明了 "template-rendering-intent":"template"，
            // 系统会在明/暗菜单栏与选中态下自动反色。⚠️ 这里必须用 Image("name")
            // 名称加载；改写成 Image(systemName: "note.text") 会退回 SF Symbol，
            // 失去品牌识别度。
            Image("MenuBarIcon")
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
///     manager.onStickyClosed       → appState.removeSticky (async)
///     manager.onStickyFrameChanged → appState.frameStore.save（纯本机，不发网络请求）
///
/// 「云端数据源重构」后变更：
///   - StickyNote.id 由 UUID 变更为 String，所有闭包签名同步；
///   - addSticky / removeSticky / updateSticky 变成 async throws，
///     bridge 只在同步闭包里发起 Task，错误通过日志降级（便签窗口的
///     操作失败不应冒泡到 App 层）；
///   - 不再需要 `willTerminate → flushStickiesSave`：stickies 唯一数据源
///     是服务端，没有本机缓存需要"退出前冲刷"。窗口位置（FrameStore）在
///     每次移动/缩放时已同步落盘，同样无需 flush。
@MainActor
final class StickyWindowBridge: ObservableObject {

    /// - Parameter appState: 用于在每个便签窗口里注入 apiClient 与回调。
    ///   contentBuilder 闭包捕获 appState 的弱引用；AppState 销毁后便签窗口
    ///   本就应该消失（App 退出），所以弱引用不会造成显示异常。
    init(appState: AppState) {
        // 先把 appState 绑进 builder，这样每次打开/更新便签都能拿到最新的 client。
        // frameStore 由 AppState 统一持有：Manager / Bridge 都不做持久化，只
        // 通过 FrameStore 访问本机窗口位置缓存。
        self.manager = StickyWindowManager(frameStore: appState.frameStore) { [weak appState] note in
            // appState 为 nil 时（理论上不会发生）回退到空视图以避免崩溃。
            guard let appState else {
                return AnyView(Color(nsColor: note.bgColor.nsColor))
            }
            return AnyView(
                StickyView(
                    initialNote: note,
                    apiClient: appState.apiClient,
                    onNewSticky: {
                        // 同步闭包 → 异步 API：起 Task 触发创建；失败只记日志。
                        // 失败的常见原因是网络抖动或 token 失效（后者会由 APIClient
                        // 的 onUnauthorized 触发 logout，无需在此再处理）。
                        Task { @MainActor [weak appState] in
                            do {
                                _ = try await appState?.addSticky()
                            } catch {
                                Self.log.error("addSticky failed: \(String(describing: error), privacy: .public)")
                            }
                        }
                    },
                    onCloseSticky: { [weak appState] id in
                        Task { @MainActor [weak appState] in
                            do {
                                try await appState?.removeSticky(id: id)
                            } catch {
                                Self.log.error("removeSticky failed: \(String(describing: error), privacy: .public)")
                            }
                        }
                    },
                    onNoteChange: { [weak appState] updated in
                        Task { @MainActor [weak appState] in
                            do {
                                try await appState?.updateSticky(updated)
                            } catch {
                                Self.log.error("updateSticky failed: \(String(describing: error), privacy: .public)")
                            }
                        }
                    }
                )
            )
        }
    }

    /// 把 manager 与 AppState 绑定。幂等——多次调用会先清理旧的 cancellables，
    /// 然后重新接上 manager 回调、重新订阅 @Published 源。
    ///
    /// ## 为什么用 Combine sink 而不是 SwiftUI `.onChange`
    /// AppState 的 `stickies` / `isAuthenticated` 变化必须**无条件驱动**
    /// StickyWindowManager 的增删窗口——这是一个纯业务副作用（桌面上一个
    /// NSWindow 的生命周期），和 SwiftUI 视图树的渲染策略无关。
    /// 如果把"监听 → syncWindows"写成 `.onChange` 挂在 MenuBarExtra 的 body 里，
    /// 面板折叠时子树不挂载、`.onChange` 不求值，WS 推送的 sticky 增删将
    /// 在用户点开菜单栏之前完全不生效（这就是历史 bug 的现象）。
    /// 而 Bridge 是 App 级 @StateObject，生命周期贯穿整个 App，在这里用
    /// Combine sink 订阅能保证事件总是被处理。
    func attach(appState: AppState) {
        manager.onStickyClosed = { [weak appState] id in
            Task { @MainActor [weak appState] in
                do {
                    try await appState?.removeSticky(id: id)
                } catch {
                    Self.log.error("removeSticky (window close) failed: \(String(describing: error), privacy: .public)")
                }
            }
        }
        // 窗口位置属于本机 UI 偏好，不走网络：直接写 FrameStore 即可。
        // 同步调用不需要 Task 包装；FrameStore 内部 UserDefaults 写入对小体量
        // 数据足够快（见 FrameStore.save 的性能注释）。
        manager.onStickyFrameChanged = { [weak appState] id, rect in
            appState?.frameStore.save(id: id, rect: CodableRect(rect))
        }

        // 重新绑定前先清理旧订阅，保证幂等（多次 attach 不会累积 sink）。
        cancellables.removeAll()

        // 订阅 stickies 变化。`.dropFirst()` 省略 CurrentValue 的首次重播，
        // 首次同步交给下方的显式 `manager.sync` 调用，避免重复触发。
        appState.$stickies
            .dropFirst()
            .receive(on: RunLoop.main)
            .sink { [weak self, weak appState] newStickies in
                guard let self, let appState else { return }
                self.manager.sync(stickies: newStickies, isVisible: appState.isAuthenticated)
            }
            .store(in: &cancellables)

        // 订阅登录态变化——登出 → hide，登录 → show。`isAuthenticated` 本身是
        // 计算属性（没有 $ 投影器），但它的两个底层 `@Published` 源是 authToken
        // 和 username；CombineLatest 合流并在最新快照上重算 isAuthenticated 等价，
        // 且去抖到真值变化（`removeDuplicates()`）避免对同一个布尔重复触发 sync。
        Publishers.CombineLatest(appState.$authToken, appState.$username)
            .map { token, name -> Bool in
                guard let t = token, !t.isEmpty else { return false }
                guard let n = name, !n.isEmpty else { return false }
                return true
            }
            .removeDuplicates()
            .dropFirst()
            .receive(on: RunLoop.main)
            .sink { [weak self, weak appState] newVisible in
                guard let self, let appState else { return }
                self.manager.sync(stickies: appState.stickies, isVisible: newVisible)
            }
            .store(in: &cancellables)

        // 首次绑定立刻同步一次，确保启动即已登录场景下窗口恢复；
        // 后续的增量变化由上面两条 sink 驱动。
        manager.sync(stickies: appState.stickies, isVisible: appState.isAuthenticated)
    }

    // MARK: - Private

    private let manager: StickyWindowManager

    /// Combine 订阅句柄。持有 AppState 的 `$stickies` / `$isAuthenticated`
    /// 两条 sink；Bridge dealloc 或 `attach` 重绑时一起取消。
    private var cancellables: Set<AnyCancellable> = []

    private static let log = Logger(subsystem: "com.hanxi.stickytodo", category: "StickyWindowBridge")
}

