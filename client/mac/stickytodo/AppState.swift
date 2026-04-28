//
//  AppState.swift
//  stickytodo
//
//  应用全局状态：认证信息、服务器配置、APIClient 注入、便签数组（云端源）、
//  RealtimeClient 生命周期、本机窗口位置（FrameStore）。
//
//  「云端数据源重构」阶段变更要点：
//    - stickies 不再从 UserDefaults 反序列化，而是登录后通过 /api/sticky-notes 全量拉取；
//    - StickyNote.frame 不再存在；窗口位置由 FrameStore 独立保存到本机 UserDefaults；
//    - 新增 RealtimeClient：登录后建立 WS 长连接，收到服务端事件后：
//        · sticky.upserted / sticky.deleted：直接在 AppState.stickies 里 merge
//        · todo.*：通过 NotificationCenter 转发给各个 StickyViewModel
//    - addSticky / updateSticky / removeSticky 全部改为 async：
//        先请求服务端（获取权威时间戳/落盘），成功后再改本地 @Published；
//        失败时抛错由 UI 以 alert 展示，不做乐观更新——macOS 端的交互频率
//        远低于 Web，保守的"先写后读"足以避免误导。
//

import Foundation
import os
import SwiftUI

/// NotificationCenter 事件常量。
///
/// J2 的 StickyViewModel 通过订阅这些 name 来响应 todo.* 事件做 refetch。
/// 用 `Notification.Name` 而非直接跨文件调用是为了解耦：ViewModel 不需要
/// 持有 AppState / RealtimeClient 的引用，只需要知道"事件到了就重拉自己那份"。
///
/// userInfo 字段约定：
///   - AppStateNotification.todoKey（"todo"）：对应 RealtimeEvent
extension Notification.Name {
    /// 服务端推送的 todo.created 事件；userInfo[AppStateNotification.todoKey] = RealtimeEvent。
    static let stickyTodoCreated = Notification.Name("com.hanxi.stickytodo.todoCreated")
    /// 服务端推送的 todo.updated 事件。
    static let stickyTodoUpdated = Notification.Name("com.hanxi.stickytodo.todoUpdated")
    /// 服务端推送的 todo.deleted 事件。
    static let stickyTodoDeleted = Notification.Name("com.hanxi.stickytodo.todoDeleted")
    /// WS 重连成功信号；ViewModel 收到后应无条件全量 refetch。
    static let stickyRealtimeReconnected = Notification.Name("com.hanxi.stickytodo.reconnected")
}

/// NotificationCenter userInfo 里使用的固定 key。
enum AppStateNotification {
    /// userInfo[todoKey] 承载的是 RealtimeEvent。
    static let todoKey = "todo"
}

/// 全局应用状态。@MainActor 保证所有 UI 订阅都在主线程被通知，
/// 避免 ObservableObject 的 objectWillChange 从后台线程触发 SwiftUI 重绘（会 crash）。
@MainActor
final class AppState: ObservableObject {

    // MARK: - 持久化 Key

    /// UserDefaults key：服务器 Base URL。
    static let defaultsKeyServerBaseURL = "stickytodo.serverBaseURL"
    /// UserDefaults key：上次登录的用户名（仅用于 UI 回填，不含密码/token）。
    static let defaultsKeyUsername = "stickytodo.username"

    /// 默认的本地开发后端地址。
    static let defaultServerBaseURL = "http://127.0.0.1:8080"

    // MARK: - 可观察字段

    /// 服务器地址。修改后会被持久化到 UserDefaults。
    /// 仅允许通过 `updateServerBaseURL` 修改，确保 trim + 持久化原子完成。
    @Published private(set) var serverBaseURL: String

    /// 当前登录用户名；nil 表示未登录。
    @Published private(set) var username: String?

    /// 当前 access token。内存 + Keychain 双写；启动时优先从 Keychain 恢复。
    /// `private(set)` 防止外部 UI 绕过 login/logout 流程直接写入。
    @Published private(set) var authToken: String?

    /// 当前云端便签列表的视图层快照。
    ///
    /// 写入路径：
    ///   1. 登录后 `loadStickies()` 全量拉取覆盖
    ///   2. `sticky.upserted` / `sticky.deleted` WS 事件 → `applyStickyUpserted/Deleted`
    ///   3. `addSticky/updateSticky/removeSticky` 的 `await` 完成后本地同步（避免等 WS 绕一圈）
    /// 读者：UI + StickyWindowManager（通过 StickyWindowBridge 订阅）。
    ///
    /// 注意：本客户端**不再**做任何 UserDefaults 持久化——离线时 stickies 为空数组
    /// 是正确行为（提示用户未登录 / 无网络时没有数据），否则会出现离线"假数据"与
    /// 上线后真实数据不一致的二态。
    @Published private(set) var stickies: [StickyNote] = []

    /// 是否正在首次 loadStickies；UI 可据此显示 spinner。
    @Published private(set) var isLoadingStickies: Bool = false

    /// 最近一次 loadStickies 的错误；仅用于 UI 展示 "加载失败，点击重试" 这类提示。
    @Published private(set) var lastStickiesError: String?

    // MARK: - 网络 / 本地存储客户端

    /// 共享的 HTTP 客户端。与 AppState 同生命周期。
    ///
    /// 所有 provider 闭包只在 MainActor 上下文中被调用（API 请求入口都在
    /// `@MainActor AppState` 的方法内 `await`），满足 Swift 并发模型。
    let apiClient: APIClient

    /// 本机窗口位置存储。便签 id → CGRect 的映射；与服务端无关。
    /// 暴露给 `StickyWindowManager` / `StickyWindowController` 使用。
    let frameStore: FrameStore

    /// 实时事件客户端；登录后创建，登出或 401 后销毁。
    ///
    /// 不是 `let` 因为 token/base 变更需要重新构造（RealtimeClient 的 baseURLProvider /
    /// tokenProvider 通过 box 读最新值，理论上不重建也行；但在 logout 路径明确销毁
    /// 旧实例更简单，避免 disconnect() 之后又被 reconnected 信号误触）。
    private(set) var realtime: RealtimeClient?

    // MARK: - 构造

    init(
        userDefaults: UserDefaults = .standard,
        session: URLSession = .shared,
        frameStore: FrameStore? = nil,
        keychainStore: KeychainStore = KeychainStore()
    ) {
        self.userDefaults = userDefaults
        self.frameStore = frameStore ?? FrameStore(defaults: userDefaults)
        self.keychainStore = keychainStore

        let rawURL = userDefaults.string(forKey: Self.defaultsKeyServerBaseURL)?
            .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        let initialURL = rawURL.isEmpty ? Self.defaultServerBaseURL : rawURL
        self.serverBaseURL = initialURL

        let rawName = userDefaults.string(forKey: Self.defaultsKeyUsername)?
            .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        let effectiveName: String? = rawName.isEmpty ? nil : rawName
        self.username = effectiveName

        // 尝试从 Keychain 恢复 token：仅在 username 非空时才有意义。
        // 失败时静默当作未登录，避免启动就弹窗打扰用户。
        var initialToken: String? = nil
        if let name = effectiveName {
            do {
                initialToken = try keychainStore.readToken(username: name)
            } catch {
                Self.log.error("restore token from keychain failed: \(String(describing: error), privacy: .public)")
            }
        }
        self.authToken = initialToken

        // 为了避免在初始化中捕获尚未完全构造的 self，用 box 做闭包的可变中转：
        // AppState init 结束后再绑定最终值。APIClient 只会在 init 完成后才
        // 发起第一次请求，所以此时 box 里的值已经就位。
        let urlBox = _MutableBox<String>(value: initialURL)
        let tokenBox = _MutableBox<String?>(value: initialToken)
        let unauthorizedBox = _MutableBox<() -> Void>(value: {})

        self.serverBaseURLBox = urlBox
        self.tokenBox = tokenBox
        self.unauthorizedBox = unauthorizedBox

        self.apiClient = APIClient(
            baseURLProvider: { urlBox.value },
            tokenProvider: { tokenBox.value },
            onUnauthorized: { unauthorizedBox.value() },
            session: session
        )

        // init 结束后绑定 onUnauthorized 到 self 的 logout（MainActor 里执行）。
        // 用 [weak self] 避免保留环。URLSession 的回调可能不在 MainActor，
        // 用 Task { @MainActor } 切回。
        unauthorizedBox.value = { [weak self] in
            Task { @MainActor [weak self] in
                self?.logout()
            }
        }

        // 如果启动时已有有效 token，立即进入"登录后状态"：拉 sticky 列表 + 建 WS。
        // 用 Task 异步执行，不阻塞 UI 首帧；失败会通过 lastStickiesError 展示。
        if initialToken != nil {
            let self_ = self
            Task { @MainActor in
                await self_.bootstrapAfterAuth()
            }
        }
    }

    // MARK: - 派生属性

    /// 是否已登录：同时要求用户名非空 + token 非空。
    var isAuthenticated: Bool {
        guard let token = authToken, !token.isEmpty else { return false }
        guard let name = username, !name.isEmpty else { return false }
        return true
    }

    // MARK: - 变更方法

    /// 更新服务器地址。会去除首尾空白；空串回退到默认地址。
    /// 所有修改统一走这里，保证"内存状态 + UserDefaults + APIClient provider"三方同步。
    func updateServerBaseURL(_ value: String) {
        let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
        let effective = trimmed.isEmpty ? Self.defaultServerBaseURL : trimmed
        guard effective != serverBaseURL else { return }
        serverBaseURL = effective
        serverBaseURLBox.value = effective
        userDefaults.set(effective, forKey: Self.defaultsKeyServerBaseURL)

        // 服务器地址变化必然意味着原连接作废；断掉旧 WS，下次 bootstrap 会重建。
        realtime?.disconnect()
        realtime = nil
    }

    /// 登录：调用 `POST /api/login`，成功则把 token 存到内存 + Keychain，
    /// username 落到 UserDefaults；随后启动 stickies 全量拉取 + WS 连接。
    /// 失败向调用方抛 APIError（bootstrap 阶段的错误不再抛出，而是 UI 级降级展示）。
    func login(username: String, password: String) async throws {
        let trimmedName = username.trimmingCharacters(in: .whitespacesAndNewlines)
        let trimmedPass = password // 不 trim 密码，保留前后空格
        guard !trimmedName.isEmpty, !trimmedPass.isEmpty else {
            throw APIError.badRequest("用户名或密码不能为空")
        }
        let resp = try await apiClient.login(username: trimmedName, password: trimmedPass)

        // Keychain 写入失败不阻塞登录流程（用户至少能用本次会话），
        // 只记日志；下次启动就只能重新登录。
        do {
            try keychainStore.saveToken(username: resp.username, token: resp.token)
        } catch {
            Self.log.error("save token to keychain failed: \(String(describing: error), privacy: .public)")
        }

        self.username = resp.username
        self.authToken = resp.token
        self.tokenBox.value = resp.token
        userDefaults.set(resp.username, forKey: Self.defaultsKeyUsername)

        await bootstrapAfterAuth()
    }

    /// 登出：清空内存 token + Keychain 记录 + 断开 WS + 清空便签。
    /// 用户名保留在 UserDefaults 以便下次登录 UI 回填。
    ///
    /// 窗口位置（FrameStore）**不**清理——用户重新登录后能恢复之前的窗口布局。
    func logout() {
        if let name = username {
            do {
                try keychainStore.deleteToken(username: name)
            } catch {
                Self.log.error("delete token from keychain failed: \(String(describing: error), privacy: .public)")
            }
        }
        authToken = nil
        tokenBox.value = nil

        realtime?.disconnect()
        realtime = nil

        // 清空内存中的便签——登出后不应继续显示上一个会话的数据
        stickies = []
        isLoadingStickies = false
        lastStickiesError = nil
    }

    /// 从服务器全量拉取便签列表。
    /// 失败时把错误落到 `lastStickiesError`，不抛出（UI 通过字段决定是否展示重试）。
    func loadStickies() async {
        guard isAuthenticated else { return }
        isLoadingStickies = true
        defer { isLoadingStickies = false }
        do {
            let list = try await apiClient.listStickies()
            self.stickies = list
            self.lastStickiesError = nil
            // 拉完一次就清理 FrameStore 里的孤儿位置（对应的便签在服务端已删除）。
            let ids = Set(list.map { $0.id })
            frameStore.pruneOrphans(aliveIDs: ids)
        } catch {
            let msg = Self.describe(error)
            Self.log.error("loadStickies failed: \(msg, privacy: .public)")
            self.lastStickiesError = msg
        }
    }

    // MARK: - 便签变更封装

    /// 新建一个便签。
    ///
    /// 流程：
    ///   1. 生成客户端 UUID id + 默认颜色/filter；
    ///   2. 本机 FrameStore 先保存默认窗口位置（叠加偏移防重合）；
    ///   3. `PUT /api/sticky-notes/:id` 幂等创建；服务端回真实时间戳；
    ///   4. 把响应写入本地 stickies（不等 WS，避免 WindowManager 创建窗口延迟）。
    ///
    /// 失败时回滚 FrameStore 的占位并抛错。
    @discardableResult
    func addSticky() async throws -> StickyNote {
        let id = StickyNote.newID()

        // 先给本机一个默认 frame（含偏移），便于窗口立刻按预期位置打开；
        // 即使后续 API 失败也只是留一条孤儿 frame，会在下次 loadStickies 时被 prune。
        let offset = CGFloat(stickies.count % 12) * 24
        var initialFrame = StickyNote.defaultFrame
        initialFrame.origin.x += offset
        initialFrame.origin.y += offset
        frameStore.save(id: id, rect: CodableRect(initialFrame))

        let draft = StickyNote(id: id)
        do {
            let saved = try await apiClient.upsertSticky(id: id, view: draft)
            applyStickyUpserted(saved)
            return saved
        } catch {
            // 回滚 FrameStore，避免留孤儿
            frameStore.remove(id: id)
            throw error
        }
    }

    /// 更新一个便签（标题 / 颜色 / filter）。
    ///
    /// 注意：note.id 必须已存在于 self.stickies；若调用方传入了不存在的 id，
    /// 服务端会幂等创建出一个新便签——这通常不是用户意图，所以这里做一次前置校验。
    func updateSticky(_ note: StickyNote) async throws {
        guard stickies.contains(where: { $0.id == note.id }) else {
            throw APIError.badRequest("sticky not found: \(note.id)")
        }
        let saved = try await apiClient.upsertSticky(id: note.id, view: note)
        applyStickyUpserted(saved)
    }

    /// 删除一个便签。
    ///
    /// 服务器软删成功后：
    ///   - 从 stickies 中移除；
    ///   - 从 FrameStore 清位置；
    ///   - 不关闭窗口（由 StickyWindowManager 订阅 stickies 变化后自行处理）。
    func removeSticky(id: String) async throws {
        _ = try await apiClient.deleteSticky(id: id)
        applyStickyDeleted(id: id)
    }

    // MARK: - 内部：WS 事件合并

    /// 把 `sticky.upserted` 事件或 `upsertSticky` 的响应合并进 stickies。
    /// id 已存在 → 替换；不存在 → 追加。
    fileprivate func applyStickyUpserted(_ note: StickyNote) {
        if let idx = stickies.firstIndex(where: { $0.id == note.id }) {
            if stickies[idx] != note {
                stickies[idx] = note
            }
        } else {
            stickies.append(note)
        }
    }

    /// 把 `sticky.deleted` 事件或 `removeSticky` 的响应合并进 stickies。
    /// 同时清理 FrameStore 里的本机位置（避免孤儿）。
    fileprivate func applyStickyDeleted(id: String) {
        let before = stickies.count
        stickies.removeAll { $0.id == id }
        if stickies.count != before {
            frameStore.remove(id: id)
        }
    }

    // MARK: - Private

    private let userDefaults: UserDefaults
    private let keychainStore: KeychainStore

    /// APIClient 的三个 provider 闭包共享的"可变盒子"。AppState 更新状态时
    /// 直接写 box.value，APIClient 下次读到新值；无需重建 APIClient。
    private let serverBaseURLBox: _MutableBox<String>
    private let tokenBox: _MutableBox<String?>
    private let unauthorizedBox: _MutableBox<() -> Void>

    private static let log = Logger(subsystem: "com.hanxi.stickytodo", category: "AppState")

    /// auth 成功（或启动时 Keychain 恢复出 token）后的统一入口：
    ///   1. 全量拉便签列表（失败只落 lastStickiesError，不阻塞）
    ///   2. 建立 WS 长连接
    private func bootstrapAfterAuth() async {
        await loadStickies()
        startRealtime()
    }

    /// 创建并启动 RealtimeClient。幂等：已有活着的实例时不重建。
    private func startRealtime() {
        if realtime != nil { return }
        guard isAuthenticated else { return }

        // 这些闭包会在 @MainActor 上被 RealtimeClient 调用，所以 self 的访问是安全的。
        let urlBox = serverBaseURLBox
        let tokenBox = tokenBox
        let client = RealtimeClient(
            baseURLProvider: { urlBox.value },
            tokenProvider:   { tokenBox.value },
            onEvent: { [weak self] event in
                self?.handleRealtimeEvent(event)
            },
            onSignal: { [weak self] signal in
                self?.handleRealtimeSignal(signal)
            }
        )
        self.realtime = client
        client.connect()
    }

    /// 分发 RealtimeEvent。sticky.* 直接 merge；todo.* 通过 NotificationCenter 广播。
    private func handleRealtimeEvent(_ event: RealtimeEvent) {
        switch event.type {
        case "sticky.upserted":
            guard let data = event.data else {
                Self.log.warning("sticky.upserted without data; ignoring")
                return
            }
            do {
                let dto = try Self.stickyDTODecoder.decode(StickyNoteDTO.self, from: data)
                let view = try dto.toStickyNote()
                applyStickyUpserted(view)
            } catch {
                Self.log.error("decode sticky.upserted failed: \(String(describing: error), privacy: .public)")
            }

        case "sticky.deleted":
            guard let id = event.id, !id.isEmpty else {
                Self.log.warning("sticky.deleted without id; ignoring")
                return
            }
            applyStickyDeleted(id: id)

        case "todo.created":
            postTodoNotification(name: .stickyTodoCreated, event: event)
        case "todo.updated":
            postTodoNotification(name: .stickyTodoUpdated, event: event)
        case "todo.deleted":
            postTodoNotification(name: .stickyTodoDeleted, event: event)

        default:
            // 未识别事件：不静默吞，记日志便于后续 schema 演进时定位
            Self.log.debug("unknown realtime event: \(event.type, privacy: .public)")
        }
    }

    /// 分发 RealtimeSignal。
    ///   - .unauthorized → 触发 logout（token 已失效，不留无效态）
    ///   - .reconnected  → 全量 refetch + 广播给 ViewModel
    ///   - .ready/.disconnected → 当前没有 UI 依赖，仅记录
    private func handleRealtimeSignal(_ signal: RealtimeSignal) {
        switch signal {
        case .unauthorized:
            Self.log.info("realtime unauthorized; logging out")
            logout()
        case .reconnected:
            Self.log.info("realtime reconnected; re-syncing stickies")
            Task { @MainActor [weak self] in
                await self?.loadStickies()
            }
            NotificationCenter.default.post(name: .stickyRealtimeReconnected, object: nil)
        case .ready, .disconnected:
            break
        }
    }

    private func postTodoNotification(name: Notification.Name, event: RealtimeEvent) {
        NotificationCenter.default.post(
            name: name,
            object: nil,
            userInfo: [AppStateNotification.todoKey: event]
        )
    }

    /// 用于解码 WS 推送的 StickyNoteDTO data 字段。
    /// 见本文件末尾的 `stickyDTODecoder` 顶层常量；挂成 static 会继承 AppState
    /// 的 @MainActor 隔离，而 `.custom` 闭包本身是 Sendable，Swift 6 会告警
    /// "不能从 Sendable 闭包引用 MainActor-isolated static"。拆到文件顶层
    /// 即解除隔离继承，也避免多实例重复构造 formatter。
    private static var stickyDTODecoder: JSONDecoder { _stickyDTODecoder }

    /// 给 UI / 日志用的错误文案。
    private static func describe(_ error: Error) -> String {
        if let api = error as? APIError {
            return api.localizedDescription
        }
        return error.localizedDescription
    }
}

// MARK: - File-level constants（不继承 AppState 的 @MainActor 隔离）

/// WS 推送 StickyNoteDTO 的专用 decoder。必须与 APIClient 的 ISO8601 解码策略
/// 保持一致（两种格式：带小数秒 / 不带小数秒），否则同一 sticky 通过 REST vs WS
/// 到达时会解出不同 Date，UI diff 将误判"变化"。
private let _stickyDTODecoder: JSONDecoder = {
    let d = JSONDecoder()
    d.dateDecodingStrategy = .custom { decoder in
        let raw = try decoder.singleValueContainer().decode(String.self)
        if let date = _iso8601Full.date(from: raw) { return date }
        if let date = _iso8601NoFraction.date(from: raw) { return date }
        throw APIError.decoding("unrecognized ISO8601 date: \(raw)")
    }
    return d
}()

/// 带小数秒 + 时区，形如 `2025-04-27T12:00:00.123456789Z`（Go time 序列化可能带）。
/// ISO8601DateFormatter 是线程安全的（Apple 文档承诺），可安全跨 actor 共享。
private let _iso8601Full: ISO8601DateFormatter = {
    let f = ISO8601DateFormatter()
    f.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
    return f
}()

/// 不带小数秒，形如 `2025-04-27T12:00:00Z`。Go `time.RFC3339` 默认输出。
private let _iso8601NoFraction: ISO8601DateFormatter = {
    let f = ISO8601DateFormatter()
    f.formatOptions = [.withInternetDateTime]
    return f
}()

/// 一个可变引用盒，仅在 AppState 内部用于给 APIClient 的闭包提供"可更新的读取视角"。
/// 显式 class 以便所有闭包共享同一引用，不走值拷贝。
///
/// 命名用下划线前缀 + fileprivate 等效（`private` 在 top-level 即 file 私有），
/// 表示外部不应直接依赖。
private final class _MutableBox<Value> {
    var value: Value
    init(value: Value) { self.value = value }
}