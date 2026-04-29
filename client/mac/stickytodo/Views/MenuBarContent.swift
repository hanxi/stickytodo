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
        } else {
            Button {
                NSApplication.shared.activate(ignoringOtherApps: true)
                NSApp.sendAction(Selector(("showSettingsWindow:")), to: nil, from: nil)
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
