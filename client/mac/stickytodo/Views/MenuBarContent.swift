//
//  MenuBarContent.swift
//  stickytodo
//
//  MenuBarExtra 点开后展示的主面板。两种状态：
//    - 未登录：提示 + 打开设置按钮
//    - 已登录：用户名、便签总数、一行并排的「新建便签 + 设置」、底部「赞赏 / 登出 / 退出」
//
//  布局演进：
//    - 最早：footerRow = [设置] [赞赏] [登出]  spacer  [退出]，4 个中文按钮
//      挤在 300pt 宽的面板里，容易触发 SwiftUI 对某个 Label 做 `…` 截断。
//    - 中间：把面板加宽到 340pt 让 4 个按钮都排开。够用但偏宽，菜单栏面板
//      整体观感偏大。
//    - 当前：把「设置」上移到 `authenticatedBody` 里，与「新建便签」并排共享
//      一行（各自 `frame(maxWidth: .infinity)` 等分宽度）。footerRow 因此
//      只剩 3 个按钮（赞赏 / 登出 / 退出），空间充裕，面板宽度回到 300pt。
//      语义上也更合理：「新建便签」和「打开设置」都是高频主操作，放同一层级；
//      footerRow 留给赞赏 + 账号/进程级的操作（登出 / 退出）。
//
//  历史查看器已迁移到 Settings → 历史 Tab，不再在菜单栏面板中提供独立入口。
//
//  打开 Settings Scene 的方案（跨版本）：
//    - macOS 14+：使用 SwiftUI 官方 `SettingsLink`。这是 macOS 14 引入的
//      首选 API；在 14+ 上若仍用 `NSApp.sendAction(#selector(showSettingsWindow:))`
//      会触发运行时警告 "Please use SettingsLink for opening the Settings scene."
//    - macOS 13：`SettingsLink` 不可用，回退到 `NSApp.sendAction` +
//      `showSettingsWindow:` selector（这在 13 上是正确且唯一的打开方式）。
//

import AppKit
import SwiftUI

struct MenuBarContent: View {
    @EnvironmentObject private var appState: AppState

    /// 「赞赏」按钮弹出的 popover 可见性。爱心按钮在 footerRow，
    /// 登录 / 未登录两种状态下都展示——赞赏入口与登录态无关。
    @State private var showSponsor: Bool = false

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            headerRow
            Divider()

            if appState.isAuthenticated {
                authenticatedBody
            } else {
                unauthenticatedBody
            }

            Divider()
            footerRow
        }
        .padding(14)
        .frame(width: 300)
    }

    // MARK: - Sub-views

    @ViewBuilder
    private var headerRow: some View {
        HStack(spacing: 8) {
            // 顶部品牌：StickyTodo（对应项目 github.com/hanxi/stickytodo）
            Label("StickyTodo", systemImage: "note.text")
                .font(.headline)
            Spacer()
            if appState.isAuthenticated, let name = appState.username {
                Text(name)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
        }
    }

    @ViewBuilder
    private var authenticatedBody: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("便签数量：\(appState.stickies.count)")
                .font(.callout)
                .foregroundStyle(.secondary)

            // 主操作行：「新建便签 + 设置」并排，各占一半宽度。
            //
            // 为什么把「设置」从 footerRow 移上来：
            //   历史上 footerRow 挤着 [设置] [赞赏] [登出] [退出] 四个中文按钮，
            //   300pt 面板宽度下容易触发 SwiftUI 对某个 Label 做 `…` 截断。把
            //   「设置」上移后 footerRow 只剩 3 个，空间充裕不再需要 fixedSize 挣扎。
            //
            // 样式统一用 `.bordered`（非 prominent）：
            //   `.borderedProminent` 按下瞬间会切到高亮填充 + 白色前景，在浅/深
            //   模式交叉下观感失衡。`.bordered` 用半透明灰底 + 系统默认前景色，
            //   天然跟随模式反色。新建便签 / 设置 / 赞赏 / 登出 / 退出 全行统一。
            HStack(spacing: 8) {
                Button {
                    // 新建便签：async API，失败只记日志（用户多数情况是网络抖动，
                    // 可再次点击按钮重试，无需打断 MenuBarExtra 面板流程）。
                    Task { @MainActor in
                        do {
                            _ = try await appState.addSticky()
                        } catch {
                            // MenuBarContent 没有独立 Logger；通过 print 落到系统日志，
                            // AppState 的 addSticky 内部也会有 os.Logger 记录同一错误。
                            print("[MenuBarContent] addSticky failed: \(error)")
                        }
                    }
                } label: {
                    Label("新建便签", systemImage: "plus.rectangle.on.rectangle")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
                .keyboardShortcut("n", modifiers: [.command])

                // 设置按钮：占据右半宽度，与「新建便签」等分。`prominent: false`
                // 对应 `.bordered`，与左侧按钮样式一致。
                settingsButton(
                    title: "设置",
                    systemImage: "gearshape",
                    prominent: false
                )
                .frame(maxWidth: .infinity)
            }
        }
    }

    @ViewBuilder
    private var unauthenticatedBody: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("尚未登录。请在\"设置\"中配置服务器地址并登录。")
                .font(.callout)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            settingsButton(
                title: "打开设置",
                systemImage: "gearshape",
                prominent: true
            )
        }
    }

    @ViewBuilder
    private var footerRow: some View {
        // footerRow 按钮：已登录 3 个 `[赞赏] [登出] [退出]`、未登录 2 个
        // `[赞赏] [退出]`。「设置」已在 `authenticatedBody` 里与「新建便签」
        // 并排，不再出现在这里。
        //
        // 为什么用 `frame(maxWidth: .infinity)` 等分宽度而不是 `fixedSize() + Spacer`：
        //   之前一版写的是 `[赞赏] [登出]  ~Spacer~  [退出]`，视觉上前两个
        //   挨得紧、退出被推到最右，三个按钮之间的间距肉眼就不均匀。改成
        //   每个按钮 `frame(maxWidth: .infinity)` 等分 HStack 宽度，三个
        //   按钮块等宽、间距完全一致，也与上面 `[新建便签] [设置]` 等分行
        //   的风格统一，整体观感更规整。
        //
        // 视觉纪律（§authenticatedBody 有详述）：
        //   footerRow 的按钮都是裸 `.bordered`（半透明灰底 + 系统默认前景），
        //   整行视觉统一。**不要**在「赞赏」按钮上叠 `.tint(.pink)`——macOS
        //   26 SDK 上 `.bordered + .tint(.pink)` 会把整个按钮渲染成粉红色
        //   填充块（等价 `.borderedProminent` 的观感），与相邻的灰色按钮
        //   严重割裂。正确做法：按钮底色保持默认，只把 **爱心图标** 染成粉色。
        //   `.foregroundStyle(.pink)` 作用在 `Image` 上只染图标，不影响
        //   Label 的 Text（Text 继承 Button 的默认前景色）。
        HStack(spacing: 8) {
            Button {
                showSponsor.toggle()
            } label: {
                Label {
                    Text("赞赏")
                } icon: {
                    Image(systemName: "heart.fill")
                        .foregroundStyle(.pink)
                }
                .frame(maxWidth: .infinity)
            }
            .buttonStyle(.bordered)
            .popover(isPresented: $showSponsor, arrowEdge: .bottom) {
                SponsorPopover(standalone: true)
            }

            if appState.isAuthenticated {
                Button(role: .destructive) {
                    appState.logout()
                } label: {
                    Label("登出", systemImage: "rectangle.portrait.and.arrow.right")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
            }

            Button(role: .destructive) {
                NSApplication.shared.terminate(nil)
            } label: {
                Label("退出", systemImage: "power")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.bordered)
            .keyboardShortcut("q", modifiers: [.command])
        }
    }

    // MARK: - Helpers

    /// 跨 macOS 13 / 14+ 的「打开设置」按钮。
    ///
    /// - macOS 14+：用官方 `SettingsLink` 包装内部 Label；点击后 SwiftUI 负责
    ///   激活 App 并打开 Settings Scene，无运行时警告。
    /// - macOS 13：`SettingsLink` 不可用，回退到 `NSApp.sendAction` +
    ///   `showSettingsWindow:` selector。MenuBarExtra 触发按钮不会让进程
    ///   变为 active，故先 `activate` 再 `sendAction`。
    ///
    /// ## 「窗口已打开却被遮挡」的兜底
    /// `SettingsLink` 与 `showSettingsWindow:` 都只负责"打开"——窗口已存在
    /// 但被其他窗口/Space 遮挡时不会主动 makeKey/orderFront，对用户呈现为
    /// "按了没反应、以为没打开"。这里在两种实现路径上都额外调用
    /// `bringSettingsWindowToFrontIfNeeded()`：窗口未打开时它 no-op，由
    /// 系统 API 负责创建；窗口已存在时它把窗口提到最前。
    /// macOS 14+ 路径上 `SettingsLink` 没有 action 闭包，用
    /// `.simultaneousGesture` 在点击同时执行该兜底，不影响 `SettingsLink`
    /// 自身的打开行为。
    @ViewBuilder
    private func settingsButton(
        title: String,
        systemImage: String,
        prominent: Bool
    ) -> some View {
        if #available(macOS 14.0, *) {
            SettingsLink {
                Label(title, systemImage: systemImage)
            }
            .buttonStyle(prominent ? AnyButtonStyle(.borderedProminent) : AnyButtonStyle(.bordered))
            .simultaneousGesture(TapGesture().onEnded {
                NSApplication.shared.bringSettingsWindowToFrontIfNeeded()
            })
        } else {
            Button {
                NSApplication.shared.activate(ignoringOtherApps: true)
                NSApp.sendAction(Selector(("showSettingsWindow:")), to: nil, from: nil)
                NSApplication.shared.bringSettingsWindowToFrontIfNeeded()
            } label: {
                Label(title, systemImage: systemImage)
            }
            .buttonStyle(prominent ? AnyButtonStyle(.borderedProminent) : AnyButtonStyle(.bordered))
        }
    }
}

// MARK: - AnyButtonStyle

/// 把「按条件选择 ButtonStyle」的分支合并到一个类型里。
/// 直接三元 `prominent ? .borderedProminent : .bordered` 在 SwiftUI 里会
/// 因为两个分支返回不同具体类型而编译失败，所以这里做一次类型擦除。
private struct AnyButtonStyle: PrimitiveButtonStyle {
    private let _make: (Configuration) -> AnyView

    init<S: PrimitiveButtonStyle>(_ style: S) {
        self._make = { config in
            AnyView(style.makeBody(configuration: config))
        }
    }

    func makeBody(configuration: Configuration) -> some View {
        _make(configuration)
    }
}

// MARK: - Settings 窗口前置工具

/// 「打开设置」入口的辅助工具：把已存在的 SwiftUI Settings 窗口拉到最前。
///
/// ## 解决的问题
/// SwiftUI 的 `SettingsLink`（macOS 14+）和 `NSApp.sendAction(showSettingsWindow:)`
/// （macOS 13）只负责"打开"Settings Scene——若窗口尚未创建则创建并显示，
/// 但若窗口**已经打开却被其他窗口/Space 遮挡**，再次点击不会主动 makeKey/orderFront，
/// 对用户表现为"按了没反应、以为没打开"。
///
/// 本工具扫描 `NSApp.windows`，识别出 Settings Scene 关联的 NSWindow，
/// 显式 `activate` + `makeKeyAndOrderFront`，弥补这个空窗。
///
/// ## 使用方式
/// - macOS 14+：在 `SettingsLink` 上叠 `.simultaneousGesture(TapGesture().onEnded {
///     NSApplication.shared.bringSettingsWindowToFrontIfNeeded()
///   })`，与 `SettingsLink` 自身的"打开 Scene"动作并行——窗口未打开时本函数找不到窗口
///   自然 no-op，由 `SettingsLink` 负责打开；窗口已存在时本函数把它拉到最前。
/// - macOS 13：在 `Button` action 里 `sendAction` 之后直接调用一次即可。
///
/// ## 识别策略（按优先级，命中任一即视为 Settings 窗口）
///   1. `identifier?.rawValue` 包含子串 `"Settings"`（macOS 14+ 通常为
///      `com_apple_SwiftUI_Settings_window`）
///   2. `frameAutosaveName` 包含 `"Settings"`（部分版本会落在这里）
///   3. window 的 `contentViewController` 类型名包含 `"Settings"`
/// 三条都不命中视为非 Settings 窗口（避免误把便签窗口拉到最前）。
///
/// ## 放置位置
/// 这段 extension 原本想放独立文件，但 Xcode project 的 build target 不会
/// 自动扫描目录，新增 .swift 必须改 `project.pbxproj` 才能参与编译。
/// 三处「打开设置」入口里 `MenuBarContent.settingsButton` 是最早也是主要的
/// 消费方，把工具直接放在这里既能保持单文件可读、又能避开 pbxproj 改动。
extension NSApplication {

    /// 若已存在 Settings 窗口，则把它拉到最前并激活 App。
    /// 没有命中的 Settings 窗口时直接返回（本函数对"窗口未打开"场景安全 no-op）。
    @MainActor
    func bringSettingsWindowToFrontIfNeeded() {
        guard let settingsWindow = findSettingsWindow() else {
            // 窗口未打开：留给 SettingsLink / showSettingsWindow: 负责打开。
            return
        }

        // 1) 让 App 进入 active（菜单栏 App / LSUIElement 触发的点击不会自动激活进程）。
        // 2) 解最小化 → makeKey + orderFront 把窗口提到最顶层。
        // 三步顺序：activate → deminiaturize → makeKeyAndOrderFront
        // 避免在最小化状态下 makeKeyAndOrderFront 的视觉跳变。
        activate(ignoringOtherApps: true)
        if settingsWindow.isMiniaturized {
            settingsWindow.deminiaturize(nil)
        }
        settingsWindow.makeKeyAndOrderFront(nil)
    }

    /// 在当前进程的所有 NSWindow 中查找 Settings Scene 关联的窗口。
    /// 命中策略见 `bringSettingsWindowToFrontIfNeeded` 文档注释。
    private func findSettingsWindow() -> NSWindow? {
        for window in windows where Self.isLikelySettingsWindow(window) {
            return window
        }
        return nil
    }

    /// 单个窗口是否疑似 Settings Scene 创建的窗口。
    private static func isLikelySettingsWindow(_ window: NSWindow) -> Bool {
        if let identifier = window.identifier?.rawValue,
           identifier.localizedCaseInsensitiveContains("Settings") {
            return true
        }
        if window.frameAutosaveName.localizedCaseInsensitiveContains("Settings") {
            return true
        }
        if let controller = window.contentViewController {
            let typeName = String(describing: type(of: controller))
            if typeName.localizedCaseInsensitiveContains("Settings") {
                return true
            }
        }
        return false
    }
}
