//
//  StickyWindowManager.swift
//  stickytodo
//
//  便签窗口集合的总管。订阅 AppState.stickies，将数组差异增量同步到一组
//  StickyWindowController：
//    - 新增的 StickyNote → 新建 controller 并 show
//    - 已删除的 StickyNote → close 并释放 controller
//    - 更新的 StickyNote（title/bgColor/filter 等）→ controller.update(note:)
//
//  未登录时不显示任何便签窗口（隐藏但保留 controller），登录后再恢复。
//
//  「云端数据源重构」后变更：
//    - StickyNote.id 由 UUID 变更为 String，所有闭包签名同步；
//    - StickyNote 不再携带 frame；窗口位置从注入的 FrameStore 查询。
//      新建窗口时若 FrameStore 未命中，按当前窗口总数叠加 24px 偏移，
//      避免所有新建便签堆在同一位置。
//
//  所有方法在 @MainActor；窗口 API 全部在主线程。
//

import AppKit
import SwiftUI

/// 便签窗口管理器。单例由 App 入口持有。
@MainActor
final class StickyWindowManager {

    // MARK: - Init

    /// - Parameters:
    ///   - frameStore: 本机窗口位置缓存。`sync` 新开窗口时按 sticky.id 查询；
    ///     未命中时用 `StickyNote.defaultFrame` + 位置偏移兜底。
    ///   - contentBuilder: 为每个便签窗口构造 SwiftUI 视图的工厂。
    init(
        frameStore: FrameStore,
        contentBuilder: @escaping @MainActor (StickyNote) -> AnyView
    ) {
        self.frameStore = frameStore
        self.contentBuilder = contentBuilder
    }

    // MARK: - 对外回调

    /// 用户关闭了某个便签窗口。App 层据此从 AppState.stickies 删除。
    /// id 是 sticky 的字符串主键（StickyNote.id）。
    var onStickyClosed: ((String) -> Void)?

    /// 用户拖动/改尺寸了某个便签窗口。App 层据此写入 FrameStore。
    /// 注意：本事件**不**对应服务端的 sticky.upserted——窗口位置是纯本机偏好，
    /// 不会通过 API 同步。
    var onStickyFrameChanged: ((String, CGRect) -> Void)?

    // MARK: - 对外操作

    /// 按 `stickies` 做一次全量 diff：
    /// - 以 id 为键；新出现 → 新建 controller
    /// - 已消失 → close controller
    /// - 仍存在但内容变化 → controller.update(note:)
    ///
    /// `isVisible` 控制窗口可见性：false（未登录）时已有 controller 全部 hide，
    /// 但不销毁——登录后再次调用 sync(stickies:isVisible:true) 可快速恢复。
    func sync(stickies: [StickyNote], isVisible: Bool) {
        let incoming = Dictionary(uniqueKeysWithValues: stickies.map { ($0.id, $0) })

        // 1. 删除：controllers 里有、incoming 里没有的。
        let toRemove = controllers.keys.filter { incoming[$0] == nil }
        for id in toRemove {
            if let ctrl = controllers.removeValue(forKey: id) {
                ctrl.close()
            }
        }

        // 2. 新增 / 更新。
        for note in stickies {
            if let existing = controllers[note.id] {
                existing.update(note: note)
                if isVisible {
                    existing.show()
                } else {
                    existing.hide()
                }
            } else {
                let ctrl = makeController(for: note)
                controllers[note.id] = ctrl
                if isVisible {
                    ctrl.show()
                }
            }
        }
    }

    /// 隐藏所有便签窗口但保留 controller。登出时使用。
    func hideAll() {
        for ctrl in controllers.values {
            ctrl.hide()
        }
    }

    /// 销毁所有 controller。App 退出/重置时使用。
    func closeAll() {
        for ctrl in controllers.values {
            ctrl.close()
        }
        controllers.removeAll()
    }

    // MARK: - Private

    private let frameStore: FrameStore
    private let contentBuilder: @MainActor (StickyNote) -> AnyView
    private var controllers: [String: StickyWindowController] = [:]

    private func makeController(for note: StickyNote) -> StickyWindowController {
        let frame = resolveFrame(for: note.id)
        let ctrl = StickyWindowController(
            note: note,
            initialFrame: frame,
            contentBuilder: contentBuilder
        )
        ctrl.onFrameChange = { [weak self] id, rect in
            self?.onStickyFrameChanged?(id, rect)
        }
        ctrl.onClose = { [weak self] id in
            // 用户关闭窗口：先从本地 map 移除 controller，再通知上层，避免
            // 上层立即删 sticky 后本方法又被 sync 触发二次 close。
            guard let self else { return }
            self.controllers.removeValue(forKey: id)
            self.onStickyClosed?(id)
        }
        return ctrl
    }

    /// 确定新窗口的初始 frame：
    ///   1) FrameStore 有记录 → 原样还原（用户上次拖动后的位置）
    ///   2) 未命中 → 用 defaultFrame + 偏移（按"当前活跃窗口数 % 12"叠加 24px）
    ///
    /// 偏移是 UI 层的兜底：即便 AppState.addSticky 已经给 FrameStore 写过一次
    /// 带偏移的 frame，在"从另一端通过 WS sticky.upserted 得到新便签"这种
    /// 路径下本机 FrameStore 还是空的，依然需要就地计算一次偏移。
    private func resolveFrame(for stickyID: String) -> CGRect {
        if let cached = frameStore.load(id: stickyID) {
            return cached.cgRect
        }
        let offset = CGFloat(controllers.count % 12) * 24
        var frame = StickyNote.defaultFrame
        frame.origin.x += offset
        frame.origin.y += offset
        return frame
    }
}
