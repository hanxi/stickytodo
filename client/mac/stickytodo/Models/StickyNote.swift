//
//  StickyNote.swift
//  stickytodo
//
//  便签的云端数据模型（与后端 /api/sticky-notes DTO 对齐）。
//
//  设计变更（阶段"云端数据源重构"）：
//    - id 从 UUID 改为 String：后端主键是字符串，由客户端生成；两端互通需要统一类型。
//    - 移除 frame：窗口位置属于"本机 UI 偏好"，不应该跨设备同步（用户在 Mac 上摆的
//      位置拿到 iPad 没意义）。位置改由独立的 FrameStore 持久化到本机 UserDefaults。
//    - 新增 createdAt/updatedAt：服务端权威时间戳，客户端只读。
//
//  后端 DTO 约定（server/internal/model/models.go：StickyNote）：
//    - id:         string, 主键
//    - title:      string
//    - frame:      string（JSON，服务端不关心内容，本客户端永远传 "{}"）
//    - bg_color:   string（{red,green,blue,alpha} 的 JSON 序列化）
//    - filter:     string（TodoFilter 的 JSON 序列化）
//    - created_at: time.Time
//    - updated_at: time.Time
//
//  客户端在 APIClient 里完成 string ↔ JSON 的双向编解码，上层视图直接看 StickyNote 值类型。
//

import AppKit
import Foundation

/// 一个便签的本地视图模型（已解码完 bg_color / filter）。
///
/// 注意："window frame" 不在这里——它由 FrameStore 按 sticky.id 独立保存。
struct StickyNote: Codable, Equatable, Hashable, Identifiable, Sendable {
    /// 服务端主键（客户端生成的 UUID 字符串；必须匹配后端 `[A-Za-z0-9_-]+` 正则，≤64 字符）。
    let id: String
    /// 用户自定义便签标题；显示在窗口顶部。
    var title: String
    /// 背景色。与系统主题无关，由用户显式选择；跨端同步。
    var bgColor: CodableRGBA
    /// 该便签的 TODO 筛选条件。
    var filter: TodoFilter
    /// 服务端创建时间；客户端只读。可选，用于本地新建后尚未收到服务端响应时的占位。
    var createdAt: Date?
    /// 服务端最后更新时间；客户端只读。可选，理由同上。
    var updatedAt: Date?

    init(
        id: String,
        title: String = "新便签",
        bgColor: CodableRGBA = .defaultSticky,
        filter: TodoFilter = TodoFilter(),
        createdAt: Date? = nil,
        updatedAt: Date? = nil
    ) {
        self.id = id
        self.title = title
        self.bgColor = bgColor
        self.filter = filter
        self.createdAt = createdAt
        self.updatedAt = updatedAt
    }

    /// 生成一个新的 sticky id。
    ///
    /// 使用 UUID v4 的字符串形式（含连字符，36 字符），满足：
    ///   - 后端 `[A-Za-z0-9_-]+` 正则（连字符允许）
    ///   - 后端 ≤64 字符限制（36 < 64）
    ///   - 跨设备零冲突（UUID v4 空间 2^122）
    static func newID() -> String {
        UUID().uuidString
    }

    /// 打开便签时若 FrameStore 里没有缓存位置，用这个默认尺寸。
    /// 屏幕坐标取 (100, 100) 作为起点；StickyWindowManager 会按便签数量叠加偏移。
    static let defaultFrame = CGRect(x: 100, y: 100, width: 300, height: 420)
}

// MARK: - CodableRect

/// 可编解码的矩形：等价于 CGRect 但明确列出字段名。
///
/// 继续保留此类型：虽然 StickyNote 不再携带 frame 字段，FrameStore 按 sticky id
/// 存 `[String: CodableRect]` 时仍然依赖它把 CGRect 稳定 JSON 化。
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
///
/// 字段命名直接对齐后端 `bg_color` JSON 结构（`{red, green, blue, alpha}`，
/// 各分量均为 0...1 的 Double），因此可以直接用 `JSONEncoder/JSONDecoder`
/// 和后端完成互操作，无需额外映射。
///
/// 选择 sRGB 是因为：
///   1. macOS 下 NSColor 的 RGB accessors 只在 RGB 色彩空间里有定义；
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
