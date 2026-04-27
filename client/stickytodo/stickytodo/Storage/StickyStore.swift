//
//  StickyStore.swift
//  stickytodo
//
//  便签数组的本地持久化：UserDefaults + JSONEncoder/Decoder。
//  选 UserDefaults 而非文件：
//    1. 便签元数据体量小（几十条、每条几百字节），UserDefaults 完全够用
//    2. 系统保证"进程退出 + 重启"的原子一致性，不需要我们手写文件锁
//    3. 无需自己管理文件路径、权限、沙盒容器
//
//  读写均为同步、非抛出。底层错误只记录日志——持久化失败不应当中断 UI 流程，
//  最坏情况是用户下次启动看不到旧便签，不会丢数据之外的副作用。
//

import Foundation
import os

/// 便签数组的持久化存储。
final class StickyStore {

    /// UserDefaults 里存储的 key。
    static let defaultsKey = "stickytodo.stickies"

    /// 用户可注入的 UserDefaults 实例；便于单测。
    private let defaults: UserDefaults

    /// os.Logger：比 print 更好的结构化日志；subsystem 统一到 App Bundle ID。
    private let log = Logger(subsystem: "com.hanxi.stickytodo", category: "StickyStore")

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    /// 读取所有便签。
    /// - 返回：成功时返回反序列化后的数组（可能为空）；失败时返回空数组并记录日志。
    func load() -> [StickyNote] {
        guard let data = defaults.data(forKey: Self.defaultsKey) else {
            return []
        }
        do {
            return try Self.decoder.decode([StickyNote].self, from: data)
        } catch {
            // 数据损坏不应让应用崩溃：记录日志后当作空数组。
            // 若用户连续多次观察到丢失，我们再考虑迁移/清理策略。
            log.error("decode stickies failed: \(String(describing: error), privacy: .public)")
            return []
        }
    }

    /// 保存所有便签。
    /// - 参数 `stickies`：当前快照。即使为空也要写入（覆盖掉旧值）。
    func save(_ stickies: [StickyNote]) {
        do {
            let data = try Self.encoder.encode(stickies)
            defaults.set(data, forKey: Self.defaultsKey)
        } catch {
            log.error("encode stickies failed: \(String(describing: error), privacy: .public)")
        }
    }

    /// 清空所有便签。
    func clear() {
        defaults.removeObject(forKey: Self.defaultsKey)
    }

    // MARK: - JSON Coders

    /// 共享 encoder。使用 ISO8601 输出时间（虽然 StickyNote 当前没有 Date 字段，
    /// 但 TodoFilter.dueBefore 是 Date，未来也可能添加——统一策略避免遗漏）。
    private static let encoder: JSONEncoder = {
        let e = JSONEncoder()
        e.dateEncodingStrategy = .iso8601
        // 输出稳定顺序便于 debug；生产环境没有体积顾虑（便签数据小）。
        e.outputFormatting = [.sortedKeys]
        return e
    }()

    /// 共享 decoder。与 encoder 对称——只接受 ISO8601 字符串。
    private static let decoder: JSONDecoder = {
        let d = JSONDecoder()
        d.dateDecodingStrategy = .iso8601
        return d
    }()
}
