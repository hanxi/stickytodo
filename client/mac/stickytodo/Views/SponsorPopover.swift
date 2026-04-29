//
//  SponsorPopover.swift
//  stickytodo
//
//  赞赏支持内容块。两处复用：
//    - MenuBarContent footerRow 的爱心按钮 `.popover` 展开（standalone = true）
//    - SettingsView 的「关于」Tab 作为 Section 内联显示（standalone = false）
//
//  只展示「爱发电 + 二维码」，不重复 Star 入口：
//    - 「关于」Tab 已经有独立的 GitHub 行
//    - MenuBarContent footerRow 位置紧凑，不适合再放长链接
//  这与 Web 端 SponsorModal 三合一（Star / 爱发电 / 二维码）不同，属于
//  平台差异化取舍：Web 弹窗有充足空间；Mac 两个载体都属于菜单/设置场景。
//

import SwiftUI

struct SponsorPopover: View {
    /// 是否以独立 popover 形态展示（控制外边距、固定宽度、标题可见性）。
    ///
    /// - `true`：MenuBarExtra 爱心按钮的 `.popover` 弹层，自带标题与 260pt 宽度，
    ///   视觉上是一个完整的子面板。
    /// - `false`：嵌入 `SettingsView` 的 `Section("支持项目")`，外层 Section
    ///   已有标题与 Form 内边距，故本组件不再重复渲染标题和外边距，避免
    ///   出现"双重标题 + 双层 padding"的割裂感。
    let standalone: Bool

    init(standalone: Bool = true) {
        self.standalone = standalone
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            if standalone {
                Text("💖 支持项目")
                    .font(.headline)
            }

            Text("如果这个项目对你有帮助，欢迎赞赏支持作者：")
                .font(.callout)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)

            HStack(spacing: 6) {
                Image(systemName: "heart.fill")
                    .foregroundStyle(.pink)
                Link(
                    "爱发电：afdian.com/a/imhanxi",
                    destination: URL(string: "https://afdian.com/a/imhanxi")!
                )
            }
            .font(.callout)

            VStack(spacing: 4) {
                // Asset Catalog 中的 SponsorQRCode.imageset（单一真相源：
                // assets/branding/sponsor-qrcode.png 的副本）
                Image("SponsorQRCode")
                    .resizable()
                    .interpolation(.high)
                    .scaledToFit()
                    .frame(width: 160, height: 160)
                    .overlay(
                        RoundedRectangle(cornerRadius: 4)
                            .stroke(Color.secondary.opacity(0.3), lineWidth: 1)
                    )
                Text("或扫码请作者喝杯奶茶 ☕")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            .frame(maxWidth: .infinity)
        }
        .padding(standalone ? 16 : 0)
        .frame(width: standalone ? 260 : nil)
    }
}
