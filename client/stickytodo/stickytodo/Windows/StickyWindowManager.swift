//
//  StickyWindowManager.swift
//  stickytodo
//
//  便签窗口集合的总管。订阅 AppState.stickies，将数组差异增量同步到一组
//  StickyWindowController：
//    - 新增的 StickyNote → 新建 controller 并 show
//    - 已删除的 StickyNote → close 并释放 controller
//    - 更新的 StickyNote（title/frame/bgColor 等）→ controller.update(note:)
//
//  未登录时不显示任何便签窗口（隐藏但保留 controller），登录后再恢复。
//
//  所有方法在 @MainActor；窗口 API 全部在主线程。
//

import AppKit
import SwiftUI

/// 便签窗口管理器。单例由 App 入口持有。
@MainActor
final class StickyWindowManager {

    // MARK: - Init

    /// - Parameter contentBuilder: 为每个便签窗口构造 SwiftUI 视图的工厂。
    ///   阶段十一传占位视图；阶段十二换成完整 StickyView。
    init(contentBuilder: @escaping @MainActor (StickyNote) -> AnyView) {
        self.contentBuilder = contentBuilder
    }

    // MARK: - 对外回调

    /// 用户关闭了某个便签窗口。App 层据此从 AppState.stickies 删除。
    var onStickyClosed: ((UUID) -> Void)?

    /// 用户拖动/改尺寸了某个便签窗口。App 层据此更新 AppState 中对应 StickyNote。
    var onStickyFrameChanged: ((UUID, CGRect) -> Void)?

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

    private let contentBuilder: @MainActor (StickyNote) -> AnyView
    private var controllers: [UUID: StickyWindowController] = [:]

    private func makeController(for note: StickyNote) -> StickyWindowController {
        let ctrl = StickyWindowController(note: note, contentBuilder: contentBuilder)
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
}
