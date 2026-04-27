//
//  MenuBarContent.swift
//  stickytodo
//
//  MenuBarExtra 点开后展示的主面板。两种状态：
//    - 未登录：提示 + 打开设置按钮
//    - 已登录：用户名、便签总数、"新建便签"、"全局历史"、"打开设置"、"登出"、"退出"
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

    /// 全局历史查看器的展示开关。在 Menu 的 Button 里不能直接用 .sheet，
    /// 因此开一个不可见辅助 window 的 workaround。这里用 `NSWindow` 直接弹出更简单。
    @State private var showingGlobalHistory = false

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
        .sheet(isPresented: $showingGlobalHistory) {
            HistoryView(mode: .global, apiClient: appState.apiClient)
                .frame(minWidth: 520, minHeight: 420)
        }
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
        VStack(alignment: .leading, spacing: 8) {
            Text("便签数量：\(appState.stickies.count)")
                .font(.callout)
                .foregroundStyle(.secondary)

            HStack {
                Button {
                    appState.addSticky()
                } label: {
                    Label("新建便签", systemImage: "plus.rectangle.on.rectangle")
                }
                .buttonStyle(.borderedProminent)
                .keyboardShortcut("n", modifiers: [.command])

                Button {
                    showingGlobalHistory = true
                } label: {
                    Label("历史", systemImage: "clock.arrow.circlepath")
                }
                .buttonStyle(.bordered)
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
        HStack(spacing: 6) {
            settingsButton(
                title: "设置",
                systemImage: "gearshape",
                prominent: false
            )

            if appState.isAuthenticated {
                Button(role: .destructive) {
                    appState.logout()
                } label: {
                    Label("登出", systemImage: "rectangle.portrait.and.arrow.right")
                }
                .buttonStyle(.bordered)
            }

            Spacer()

            Button(role: .destructive) {
                NSApplication.shared.terminate(nil)
            } label: {
                Label("退出", systemImage: "power")
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
