//
//  FrameStore.swift
//  stickytodo
//
//  便签窗口位置（frame）的"纯本机"持久化。
//
//  为什么要独立一个 Store：
//    阶段"云端数据源重构"后，StickyNote 本体完全走服务端，窗口位置属于本机 UI 偏好，
//    不应跨设备同步（用户在 Mac 上摆的位置放到 iPad 上没意义，两端屏幕坐标系也不同）。
//    因此把 frame 从 sticky 模型中剥离出来，按 sticky.id 映射到 CodableRect，
//    整体存到 UserDefaults 的一个独立 key。
//
//  可失败但不抛错：
//    持久化失败只记录日志。最坏情况是用户下次启动便签位置回落到默认值，
//    不会丢业务数据，不应当中断 UI 流程。
//

import Foundation
import os

/// 便签窗口位置的本地存储。
final class FrameStore {

    /// UserDefaults 里存储的 key。与 StickyStore 的旧 key（"stickytodo.stickies"）不同，
    /// 两者语义分离、互不干扰。
    static let defaultsKey = "stickytodo.frames"

    /// 可注入的 UserDefaults 实例；便于单测。
    private let defaults: UserDefaults

    private let log = Logger(subsystem: "com.hanxi.stickytodo", category: "FrameStore")

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    /// 读取所有 sticky id → frame 的映射。
    /// - 返回：成功时返回反序列化后的字典（可能为空）；失败时返回空字典并记录日志。
    func loadAll() -> [String: CodableRect] {
        guard let data = defaults.data(forKey: Self.defaultsKey) else {
            return [:]
        }
        do {
            return try Self.decoder.decode([String: CodableRect].self, from: data)
        } catch {
            log.error("decode frames failed: \(String(describing: error), privacy: .public)")
            return [:]
        }
    }

    /// 读取单个便签的 frame。不存在时返回 nil，由调用方决定 fallback 策略
    /// （通常是 `StickyNote.defaultFrame` + 叠加偏移）。
    func load(id: String) -> CodableRect? {
        loadAll()[id]
    }

    /// 写入单个便签的 frame。空 id 静默忽略（防御性）。
    ///
    /// 性能说明：实现上每次调用都会 `loadAll → 改一项 → persist` 整张表。
    /// 对于 ≤ 百张便签、单次 ~KB 级 JSON 的体量，实测 < 1ms 可忽略；但
    /// 调用方如果来自高频回调（如 NSWindow didMove/didResize 每次拖动
    /// 都触发），仍应自行做节流/去抖，避免把 UserDefaults 写成热点。
    func save(id: String, rect: CodableRect) {
        guard !id.isEmpty else { return }
        var map = loadAll()
        if map[id] == rect { return }
        map[id] = rect
        persist(map)
    }

    /// 批量覆盖整张 frame 表。`flushStickiesSave` 类的"即将退出前同步落盘"场景使用。
    func saveAll(_ map: [String: CodableRect]) {
        persist(map)
    }

    /// 删除指定 id 的 frame（便签被删除时调用）。不存在时静默忽略。
    func remove(id: String) {
        var map = loadAll()
        guard map.removeValue(forKey: id) != nil else { return }
        persist(map)
    }

    /// 清理所有存量 frame 里不在 `aliveIDs` 集合中的条目。
    /// 用于启动时对齐服务端便签列表：服务端已删除的便签本地 frame 也不再有意义。
    /// - Returns: 实际清理掉的 id 数量。
    @discardableResult
    func pruneOrphans(aliveIDs: Set<String>) -> Int {
        let map = loadAll()
        let orphanKeys = map.keys.filter { !aliveIDs.contains($0) }
        guard !orphanKeys.isEmpty else { return 0 }
        var trimmed = map
        for k in orphanKeys {
            trimmed.removeValue(forKey: k)
        }
        persist(trimmed)
        return orphanKeys.count
    }

    // MARK: - Private

    private func persist(_ map: [String: CodableRect]) {
        do {
            let data = try Self.encoder.encode(map)
            defaults.set(data, forKey: Self.defaultsKey)
        } catch {
            log.error("encode frames failed: \(String(describing: error), privacy: .public)")
        }
    }

    private static let encoder: JSONEncoder = {
        let e = JSONEncoder()
        e.outputFormatting = [.sortedKeys]
        return e
    }()

    private static let decoder = JSONDecoder()
}
