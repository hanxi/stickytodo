//
//  StickyNote.swift
//  stickytodo
//
//  单个便签窗口的持久化数据：位置/大小、背景色、筛选条件、标题。
//  存储格式需要能被 JSONEncoder / JSONDecoder 稳定往返（UserDefaults 存 Data），
//  因此 CGRect 与 NSColor 都定义成显式的 Codable 辅助结构，不依赖平台
//  默认的 NSKeyedArchiver。
//

import AppKit
import Foundation

/// 一个便签。ID 在本地生成（UUID），永远不与服务端 Todo.id 混用。
struct StickyNote: Codable, Equatable, Hashable, Identifiable, Sendable {
    let id: UUID
    /// 用户自定义便签标题；显示在窗口顶部，仅本地有意义。
    var title: String
    /// 便签窗口在屏幕坐标系下的 frame。
    var frame: CodableRect
    /// 背景色。与系统主题无关，由用户显式选择。
    var bgColor: CodableRGBA
    /// 该便签的 TODO 筛选条件。
    var filter: TodoFilter

    init(
        id: UUID = UUID(),
        title: String = "新便签",
        frame: CGRect = StickyNote.defaultFrame,
        bgColor: CodableRGBA = .defaultSticky,
        filter: TodoFilter = TodoFilter()
    ) {
        self.id = id
        self.title = title
        self.frame = CodableRect(frame)
        self.bgColor = bgColor
        self.filter = filter
    }

    /// 打开便签时如果没有缓存的 frame，用这个默认尺寸。屏幕坐标取 (100, 100) 作为起点。
    static let defaultFrame = CGRect(x: 100, y: 100, width: 300, height: 420)
}

// MARK: - CodableRect

/// 可编解码的矩形：等价于 CGRect 但明确列出字段名。
/// 不直接为 CGRect 添加 Codable extension，避免跨模块潜在冲突。
struct CodableRect: Codable, Equatable, Hashable, Sendable {
    var x: CGFloat
    var y: CGFloat
    var width: CGFloat
    var height: CGFloat

    init(_ rect: CGRect) {
        self.x = rect.origin.x
        self.y = rect.origin.y
        self.width = rect.size.width
        self.height = rect.size.height
    }

    var cgRect: CGRect {
        CGRect(x: x, y: y, width: width, height: height)
    }
}

// MARK: - CodableRGBA

/// 可编解码的 sRGB 颜色。
/// 选择 sRGB 是因为：
///   1. macOS 下 NSColor.withAlphaComponent/RGB accessors 只在 RGB 色彩空间里有定义；
///   2. 跨不同显示器或不同主题，sRGB 的数值含义稳定一致。
struct CodableRGBA: Codable, Equatable, Hashable, Sendable {
    /// 0...1 归一化分量。
    var red: Double
    var green: Double
    var blue: Double
    var alpha: Double

    init(red: Double, green: Double, blue: Double, alpha: Double = 1.0) {
        self.red = Self.clamp(red)
        self.green = Self.clamp(green)
        self.blue = Self.clamp(blue)
        self.alpha = Self.clamp(alpha)
    }

    /// 从 NSColor 构造。若颜色无法转换到 sRGB（如命名颜色空间），回退为默认便签色。
    /// 避免传入的 NSColor 在某些色彩空间下读取 RGB 组件抛异常。
    init(nsColor: NSColor) {
        guard let c = nsColor.usingColorSpace(.sRGB) else {
            self = .defaultSticky
            return
        }
        self.init(
            red: Double(c.redComponent),
            green: Double(c.greenComponent),
            blue: Double(c.blueComponent),
            alpha: Double(c.alphaComponent)
        )
    }

    /// 转换回 NSColor，供 AppKit 渲染使用。
    var nsColor: NSColor {
        NSColor(srgbRed: CGFloat(red), green: CGFloat(green), blue: CGFloat(blue), alpha: CGFloat(alpha))
    }

    /// 经典便签黄：#FFEB8A，不透明。
    static let defaultSticky = CodableRGBA(red: 1.0, green: 0.92, blue: 0.54, alpha: 1.0)

    /// 相对亮度（Rec. 709 标准）。范围 [0, 1]：0 纯黑，1 纯白。
    ///
    /// 用途：前景控件（如优先级文字）根据背景亮度选择亮色/深色调色板，
    /// 避免黄色 flag 在便签黄背景上糊成一片。
    ///
    /// 公式采用人眼感知加权（绿色贡献最大）：0.2126*R + 0.7152*G + 0.0722*B。
    /// alpha 不参与计算——便签背景恒为不透明，没必要按透明度修正。
    var luminance: Double {
        0.2126 * red + 0.7152 * green + 0.0722 * blue
    }

    /// true 表示背景偏亮（luminance ≥ 0.6），前景应选用深色以保证对比度。
    ///
    /// 阈值 0.6 的选择：5 种预设便签色（黄/绿/蓝/粉/紫）luminance 均 > 0.8，
    /// 均判定为亮色；若未来引入深色便签，阈值仍能正确切分。
    var isLightBackground: Bool {
        luminance >= 0.6
    }

    private static func clamp(_ v: Double) -> Double {
        v.isFinite ? min(1.0, max(0.0, v)) : 0.0
    }
}
