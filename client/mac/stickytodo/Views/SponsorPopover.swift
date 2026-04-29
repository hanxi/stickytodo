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

            // 爱发电行：
            //   - `HStack(alignment: .top)` + `.fixedSize(horizontal: false, vertical: true)`
            //     让中文前缀 + URL 在窄列（Settings 嵌入 Form 时的 Section 可用宽度
            //     会被 Form 的标签列挤压）里能自然换行，不再被截成 "…"。
            //   - Link 单独拎出一行文案「爱发电」，避免 Link 自身在窄列下被直接
            //     压缩成 "afdian.com/a/im…"（Link 内部默认 `.lineLimit(1)` + 截断）。
            VStack(alignment: .leading, spacing: 4) {
                HStack(alignment: .firstTextBaseline, spacing: 6) {
                    Image(systemName: "heart.fill")
                        .foregroundStyle(.pink)
                    Text("爱发电")
                }
                Link(
                    "afdian.com/a/imhanxi",
                    destination: URL(string: "https://afdian.com/a/imhanxi")!
                )
                .lineLimit(1)
                .truncationMode(.middle)
            }
            .font(.callout)
            .fixedSize(horizontal: false, vertical: true)

            VStack(spacing: 4) {
                // Asset Catalog 中的 SponsorQRCode.imageset（单一真相源：
                // assets/branding/sponsor-qrcode.png 的副本）。
                //
                // popover 模式宽度 260pt，减去左右各 16 的 padding 后内容可用
                // ≈228pt；Settings Section 里的可用宽度也在 300pt 以内。
                // 220×220 在两处都能完整显示，扫码识别率也够（源图 1037×1037
                // 放到 220pt 仍是明显的放大空间）。
                Image("SponsorQRCode")
                    .resizable()
                    .interpolation(.high)
                    .scaledToFit()
                    .frame(width: 220, height: 220)
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
        // popover 外框跟随二维码拓宽到 280pt，留出 30pt 水平空白；Settings
        // 嵌入时宽度仍由外层 Form 决定，不固定。
        .frame(width: standalone ? 280 : nil)
    }
}
