//
//  RealtimeClient.swift
//  stickytodo
//
//  基于 URLSessionWebSocketTask 的实时事件客户端。与浏览器端
//  `client/web/src/api/ws.ts` 保持协议与行为一致：
//    1. URL：`ws(s)://<host>/api/ws`（由 serverBaseURL 按 scheme 翻译）
//    2. 首帧：客户端发 `{"type":"auth","token":"<jwt>"}`，服务端 2s 内校验，
//       通过回 `{"type":"ready","server_time":"..."}`。
//    3. 任何其它 close code → 指数退避重连 [1,2,4,8,16,30]s；
//       close code 4401 → 不重连，回调 onUnauthorized。
//    4. 业务事件只有 5 种（对齐 server/internal/ws/event.go）：
//         todo.created / todo.updated / todo.deleted
//         sticky.upserted / sticky.deleted
//       其他 complete / reopen / restore 操作在服务端是作为 todo.updated/
//       todo.deleted 广播的，不存在独立事件类型。
//    5. 心跳：URLSessionWebSocketTask 不会自动响应后端的 ping，必须手动
//       周期性 sendPing 才能让后端感知到活性（否则 60s 后后端会断开）。
//
//  线程模型：
//    - 类本身标记 @MainActor，所有对外 API 与内部状态都在主线程访问，
//      避免对 Sendable 属性加锁。
//    - `URLSessionWebSocketTask.receive` 的循环用 Task 在后台驱动，
//      但解码后的事件派发通过 `await MainActor.run` 回到主线程调用 onEvent。
//
//  不做的事：
//    - 事件缓冲 / 断线期补偿：reconnected 信号回调里由上层（AppState）做
//      全量 refetch，不在客户端做事件 replay。
//    - 业务上行：除首帧 auth 外，永远不向服务端发业务消息。
//

import Foundation
import os

/// 连接生命周期信号。与 Web 端 WSSignal 同名同义。
enum RealtimeSignal: String, Sendable {
    /// 首次 auth 成功并收到 ready 帧。
    case ready
    /// 至少经历过一次断线后的 auth 成功 + ready 帧。
    case reconnected
    /// close code 4401：token 失效，必须登出，不会再自动重连。
    case unauthorized
    /// 任意原因导致连接断开（不论是否会重连）。
    case disconnected
}

/// 解码自服务端的实时事件。与 `server/internal/ws/event.go` 的 Event 结构对齐。
///
/// Data / ID 字段互斥（由事件类型决定）：
///   - 资源变更类事件（todo.created / todo.updated / sticky.upserted）：Data 非空，
///     承载完整资源 JSON（Todo 或 StickyNote 的 DTO）
///   - 删除类事件（todo.deleted / sticky.deleted）：ID 非空
///
/// 本客户端不尝试把 Data 预解码成具体资源类型，因为这会把 RealtimeClient 和
/// 业务 DTO 强耦合；转而原样透传 raw Data，由订阅者按需解码。
struct RealtimeEvent: Sendable, Equatable {
    /// 事件类型字符串，如 "todo.updated"、"sticky.upserted"。
    let type: String
    /// 资源变更事件携带的原始 JSON bytes；删除事件为 nil。
    let data: Data?
    /// 删除事件携带的主键；字符串承载以覆盖 uint（todo）和 string（sticky）两种主键类型。
    let id: String?
}

/// WebSocket 实时客户端。
///
/// 使用方式：
/// ```swift
/// let client = RealtimeClient(
///     baseURLProvider: { [weak self] in self?.serverBaseURL ?? "" },
///     tokenProvider:   { [weak self] in self?.token },
///     onEvent:         { [weak self] event in ... },
///     onSignal:        { [weak self] signal in ... }
/// )
/// client.connect()
/// // ...登出时：
/// client.disconnect()
/// ```
@MainActor
final class RealtimeClient {

    // MARK: - 注入点

    /// 每次连接前都会重新调用，用来拿到最新的 server base URL
    /// （用户可能在 SettingsView 里改过地址）。返回空串视为"没有地址，暂不连"。
    private let baseURLProvider: @MainActor () -> String

    /// 首帧 auth 会从这里拿 token。返回 nil 视为未登录，不连。
    private let tokenProvider: @MainActor () -> String?

    /// 业务事件回调。调用方需处理异步操作，回调本身是同步的 `@MainActor` 闭包。
    private let onEvent: @MainActor (RealtimeEvent) -> Void

    /// 连接生命周期信号回调。
    private let onSignal: @MainActor (RealtimeSignal) -> Void

    // MARK: - 状态

    /// 当前 WebSocket 任务；nil 表示未连接或已清理。
    private var task: URLSessionWebSocketTask?

    /// 驱动 `receive` 循环的 Task；task 被替换时必须 cancel 旧循环避免泄漏。
    private var receiveLoop: Task<Void, Never>?

    /// 心跳 Task：周期性 sendPing，每 15s 一次；比服务端 30s ping 更勤快，
    /// 这样即使服务端 ping 到来时我们还在处理上条消息，也有独立的保活通道。
    private var pingLoop: Task<Void, Never>?

    /// auth 首帧的"必须在这个 Task 里完成"超时守卫。2s 到达仍未收到 ready 帧就主动 close。
    private var authTimeoutTask: Task<Void, Never>?

    /// 退避重连的 Task；disconnect 时必须 cancel。
    private var reconnectTask: Task<Void, Never>?

    /// 当前是否经历过一次断线（用于区分首次 ready 与 reconnected 信号）。
    private var hasBeenDisconnected = false

    /// 退避重连的已尝试次数，索引 Self.backoffSeconds。
    private var retryAttempt = 0

    /// 用户显式 disconnect 时置 true，阻断所有自动重连路径。
    private var disconnectedByUser = false

    /// 首帧 auth 是否已通过（收到 ready 帧）。没通过前不允许派发业务事件。
    private var didHandshake = false

    /// 独立 URLSession：不复用 `URLSession.shared` 以便未来需要独立的
    /// TLS / 超时策略时能就地调整，也避免 sharing session 的配置漂移。
    private let session: URLSession

    /// 日志。subsystem 与其他模块保持一致，便于 Console.app 集中查看。
    private let logger = Logger(subsystem: "com.hanxi.stickytodo", category: "realtime")

    // MARK: - 常量

    /// 指数退避时间表（秒）。走到末尾后恒定用最后一档。
    /// 与 Web 端 BACKOFF_MS 保持一致，保证两端在网络抖动下的体验对齐。
    private static let backoffSeconds: [UInt64] = [1, 2, 4, 8, 16, 30]

    /// 首帧 auth 超时（秒）；服务端也是 2s。
    private static let authTimeoutSeconds: UInt64 = 2

    /// 主动保活 ping 周期（秒）。服务端 pongWait=60，ping 周期 30；
    /// 这里 15s 是"更保守"的客户端保活，保证在弱网下也不会被误判为死连接。
    private static let pingIntervalSeconds: UInt64 = 15

    /// 应用自定义 close code：与后端 CloseCodeUnauthorized 对齐。
    private static let closeCodeUnauthorized = 4401

    // MARK: - 生命周期

    init(
        baseURLProvider: @escaping @MainActor () -> String,
        tokenProvider:   @escaping @MainActor () -> String?,
        onEvent:         @escaping @MainActor (RealtimeEvent) -> Void,
        onSignal:        @escaping @MainActor (RealtimeSignal) -> Void,
        session:         URLSession = .shared
    ) {
        self.baseURLProvider = baseURLProvider
        self.tokenProvider = tokenProvider
        self.onEvent = onEvent
        self.onSignal = onSignal
        self.session = session
    }

    deinit {
        // deinit 不是 @MainActor；直接同步 cancel 所有 Task，不能调用主 actor 上的方法。
        task?.cancel(with: .goingAway, reason: nil)
        receiveLoop?.cancel()
        pingLoop?.cancel()
        authTimeoutTask?.cancel()
        reconnectTask?.cancel()
    }

    // MARK: - 对外 API

    /// 建立连接。幂等：
    ///   - 当前已有连接且 token/base 未变 → 直接返回
    ///   - 参数变化或无连接 → 关闭旧连接并新建
    func connect() {
        disconnectedByUser = false

        guard let token = tokenProvider(), !token.isEmpty else {
            logger.debug("connect skipped: no token")
            return
        }
        let base = baseURLProvider()
        guard !base.isEmpty else {
            logger.debug("connect skipped: no baseURL")
            return
        }

        // 已有进行中的连接尝试：不重建。task.state == .running 表示握手已完成；
        // 任何其他状态都说明旧连接无效，应重建。
        if let existing = task, existing.state == .running {
            return
        }

        openOnce(base: base, token: token)
    }

    /// 主动断开连接；不再触发自动重连。之后再调 connect() 可恢复。
    func disconnect() {
        disconnectedByUser = true
        cancelReconnect()
        cleanupTask()
        retryAttempt = 0
        hasBeenDisconnected = false
        didHandshake = false
    }

    /// 当前是否已完成 auth + ready 握手。
    var isConnected: Bool {
        didHandshake && task?.state == .running
    }

    // MARK: - 连接流程

    /// 打开一次连接。失败（URL 非法 / 构造抛错）会直接调度下一次退避重连。
    private func openOnce(base: String, token: String) {
        guard let wsURL = Self.makeWSURL(from: base) else {
            logger.error("invalid base URL for ws: \(base, privacy: .public)")
            // base URL 非法不应无限重试——但也不能完全不试，用户可能在 Settings 里改对；
            // 按退避节奏试，把恢复机会交给用户输入。
            scheduleReconnect()
            return
        }

        logger.debug("ws connecting to \(wsURL.absoluteString, privacy: .public)")

        // URLSession 不支持在 WS 握手里加自定义 header 做鉴权；走首帧 auth 协议。
        let newTask = session.webSocketTask(with: wsURL)
        task = newTask
        didHandshake = false

        newTask.resume()

        // 握手 HTTP Upgrade 完成后即可 send；直接把 auth 帧排入队列。
        // URLSessionWebSocketTask.send 在 resume 之前入队会在 resume 后自动发送，
        // 但此时 task 已 resume，所以 send 会立刻走。失败会通过 completion 抛出。
        sendAuthFrame(token: token, on: newTask)

        // 2s auth 超时守卫：到期时若握手仍未完成，主动 close。
        authTimeoutTask?.cancel()
        authTimeoutTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: Self.authTimeoutSeconds * 1_000_000_000)
            guard let self else { return }
            await MainActor.run {
                if !self.didHandshake, self.task === newTask {
                    self.logger.warning("auth timeout: closing ws task")
                    // 用户侧的 auth 超时会触发重连；这里主动关闭，close handler 会接管。
                    newTask.cancel(with: .normalClosure, reason: nil)
                }
            }
        }

        // 启动 receive 循环。
        receiveLoop?.cancel()
        receiveLoop = Task { [weak self, newTask] in
            await self?.runReceiveLoop(task: newTask)
        }
    }

    /// 发送首帧 auth。失败直接 close，让 receive 循环的退出路径触发重连。
    private func sendAuthFrame(token: String, on task: URLSessionWebSocketTask) {
        struct AuthFrame: Encodable {
            let type: String
            let token: String
        }
        let payload = AuthFrame(type: "auth", token: token)
        do {
            let data = try JSONEncoder().encode(payload)
            // send 是 async 但我们不 await——任何发送错误都会体现在 receive 循环的错误退出上，
            // 不需要在这里重复处理。completion 闭包仍然打日志以便排查。
            task.send(.data(data)) { [weak self] error in
                if let error {
                    // 回到 main actor 记日志（Logger 本身线程安全，但我们习惯统一主线程派发）
                    Task { @MainActor in
                        self?.logger.warning("send auth failed: \(error.localizedDescription, privacy: .public)")
                    }
                }
            }
        } catch {
            logger.error("encode auth frame failed: \(error.localizedDescription, privacy: .public)")
            task.cancel(with: .normalClosure, reason: nil)
        }
    }

    /// 接收循环。退出路径唯一：receive 抛错（含正常关闭），随后触发 onClose 处理。
    private func runReceiveLoop(task: URLSessionWebSocketTask) async {
        // 启动保活 ping 循环（不依赖 await：receiveLoop 和 pingLoop 各跑各的）。
        // 延迟到 receive 实际启动再开 ping，避免在 auth 尚未被服务端处理时就发 ping。
        await MainActor.run { [weak self] in
            self?.startPingLoop(on: task)
        }

        while !Task.isCancelled {
            let message: URLSessionWebSocketTask.Message
            do {
                message = try await task.receive()
            } catch {
                await MainActor.run { [weak self] in
                    self?.handleClose(task: task, error: error)
                }
                return
            }

            switch message {
            case .string(let text):
                if let data = text.data(using: .utf8) {
                    await handleFrame(data: data)
                } else {
                    // 极端编码异常：丢掉即可，服务端不会靠它恢复状态
                    logger.warning("drop non-utf8 text frame")
                }
            case .data(let data):
                // 服务端约定用 TextMessage；收到二进制按 JSON 解析兜底
                await handleFrame(data: data)
            @unknown default:
                logger.warning("drop unknown ws message case")
            }
        }
    }

    /// 处理一帧解码后的消息。`ready` 走握手分支；其他派发到 onEvent。
    private func handleFrame(data: Data) async {
        // 只解顶层 type / data / id 三个字段；data 字段以 RawValue 承载，避免提前解码。
        struct Envelope: Decodable {
            let type: String
            let data: JSONRaw?
            let id: JSONRaw?
        }
        let env: Envelope
        do {
            env = try JSONDecoder().decode(Envelope.self, from: data)
        } catch {
            logger.warning("drop unparsable frame: \(error.localizedDescription, privacy: .public)")
            return
        }

        if env.type == "ready" {
            await MainActor.run { [weak self] in
                self?.handleReady()
            }
            return
        }

        // 业务事件：必须已经握手
        let ev = RealtimeEvent(
            type: env.type,
            data: env.data?.rawData,
            id: env.id?.asStringOrNumberString
        )

        await MainActor.run { [weak self] in
            guard let self else { return }
            guard self.didHandshake else {
                self.logger.warning("drop event before handshake: \(env.type, privacy: .public)")
                return
            }
            self.onEvent(ev)
        }
    }

    /// auth + ready 成功：重置退避计数，发 ready / reconnected 信号。
    private func handleReady() {
        didHandshake = true
        retryAttempt = 0
        authTimeoutTask?.cancel()
        authTimeoutTask = nil

        let signal: RealtimeSignal = hasBeenDisconnected ? .reconnected : .ready
        onSignal(signal)
    }

    /// 连接关闭处理。
    ///
    /// - 用户主动 disconnect → 不重连、不发信号（disconnect() 已做）
    /// - close code 4401   → 发 unauthorized + disconnected 信号，不重连
    /// - 其他              → 发 disconnected 信号，指数退避重连
    private func handleClose(task closedTask: URLSessionWebSocketTask, error: Error?) {
        // 只处理当前 task 的关闭；迟到的回调（connect 已切到新 task）忽略
        guard task === closedTask else { return }

        let closeCode = closedTask.closeCode
        cleanupTask()

        if disconnectedByUser {
            return
        }

        onSignal(.disconnected)

        if closeCode.rawValue == Self.closeCodeUnauthorized {
            logger.info("ws closed with 4401 unauthorized; will not reconnect")
            onSignal(.unauthorized)
            return
        }

        if let error {
            logger.info("ws receive error, scheduling reconnect: \(error.localizedDescription, privacy: .public)")
        } else {
            logger.info("ws closed (code=\(closeCode.rawValue, privacy: .public)), scheduling reconnect")
        }

        hasBeenDisconnected = true
        scheduleReconnect()
    }

    // MARK: - 心跳

    /// 启动主动 ping 循环。task 切换 / 断开时自动退出。
    private func startPingLoop(on task: URLSessionWebSocketTask) {
        pingLoop?.cancel()
        pingLoop = Task { [weak self, weak task] in
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: Self.pingIntervalSeconds * 1_000_000_000)
                guard !Task.isCancelled else { return }
                guard let task, task.state == .running else { return }
                // sendPing 的 completion 只打日志；失败会反映在下一次 receive 的 error 上
                task.sendPing { [weak self] error in
                    if let error {
                        Task { @MainActor in
                            self?.logger.warning("ping failed: \(error.localizedDescription, privacy: .public)")
                        }
                    }
                }
            }
        }
    }

    // MARK: - 重连

    /// 安排下一次重连。带幂等：
    ///   - 用户显式 disconnect → 直接返回
    ///   - 已有 pending 重连任务 → 不重复安排
    private func scheduleReconnect() {
        if disconnectedByUser { return }
        if reconnectTask != nil { return }
        guard tokenProvider() != nil else {
            // 没 token 就不排重连，等下一次 connect() 重新开始状态机
            return
        }

        let idx = min(retryAttempt, Self.backoffSeconds.count - 1)
        let delay = Self.backoffSeconds[idx]
        retryAttempt += 1

        logger.debug("reconnect in \(delay, privacy: .public)s (attempt \(self.retryAttempt, privacy: .public))")

        reconnectTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: delay * 1_000_000_000)
            guard !Task.isCancelled else { return }
            await MainActor.run {
                guard let self else { return }
                self.reconnectTask = nil
                guard !self.disconnectedByUser else { return }
                self.connect()
            }
        }
    }

    private func cancelReconnect() {
        reconnectTask?.cancel()
        reconnectTask = nil
    }

    // MARK: - 清理

    /// 销毁当前 task 与关联循环。不发送信号；调用方按需发。
    private func cleanupTask() {
        authTimeoutTask?.cancel()
        authTimeoutTask = nil
        receiveLoop?.cancel()
        receiveLoop = nil
        pingLoop?.cancel()
        pingLoop = nil
        if let t = task, t.state == .running || t.state == .suspended {
            t.cancel(with: .goingAway, reason: nil)
        }
        task = nil
        didHandshake = false
    }

    // MARK: - URL 翻译

    /// http://host → ws://host/api/ws；https://host → wss://host/api/ws。
    /// 非 http(s) 前缀直接返回 nil 交给上层做退避重试。
    static func makeWSURL(from baseURL: String) -> URL? {
        let trimmed = baseURL.trimmingCharacters(in: .whitespacesAndNewlines)
        var prefix: String
        if trimmed.hasPrefix("https://") {
            prefix = "wss://" + trimmed.dropFirst("https://".count)
        } else if trimmed.hasPrefix("http://") {
            prefix = "ws://" + trimmed.dropFirst("http://".count)
        } else {
            return nil
        }
        // 去掉末尾 "/" 再挂 /api/ws，避免 "//api/ws"
        if prefix.hasSuffix("/") {
            prefix.removeLast()
        }
        return URL(string: prefix + "/api/ws")
    }
}

// MARK: - JSONRaw

/// 延迟解码的 JSON 值容器：保留原始 JSON bytes，让调用方按需二次解码。
///
/// 为什么需要：Event.data 的实际结构随 type 而变（todo.created 是 Todo；
/// sticky.upserted 是 StickyNote；删除类事件没有 data），在 RealtimeClient
/// 层把 data 具化会把这个文件和所有业务 DTO 强耦合。透传原始 bytes 让 J2 的
/// 调用方自己决定是否解码、解码成什么类型。
///
/// `id` 字段也用同一个类型承载：后端 `id` 对 Todo 是 uint，对 Sticky 是 string；
/// `asStringOrNumberString` 会把 number 转成字符串表示，让调用方只需处理一种类型。
private struct JSONRaw: Decodable {
    let rawData: Data
    private let underlying: AnyDecodable

    init(from decoder: Decoder) throws {
        // 先解成 AnyDecodable 以便后续 asStringOrNumberString 读原始值；
        // 同时保留 raw bytes 给需要完整 JSON 的消费者。
        self.underlying = try AnyDecodable(from: decoder)
        // 再把同一内容重新编码回 bytes。虽然多了一次 encode 开销，但收益是
        // 调用方得到一段干净、可被任意其他 Decoder（比如 APIClient.jsonDecoder
        // 那套带 Date 策略的）重新解码的 JSON。
        let encoder = JSONEncoder()
        self.rawData = (try? encoder.encode(self.underlying)) ?? Data("null".utf8)
    }

    /// 把 id 值转成 String：
    ///   - String → 原样
    ///   - Int / Double（整数值）→ 十进制字符串
    ///   - 其他 → nil（交给上层判定是否算异常）
    var asStringOrNumberString: String? {
        switch underlying.value {
        case .string(let s): return s
        case .int(let i):    return String(i)
        case .double(let d):
            // 只在"无分数部分"时允许转换：后端 id 永远是整数
            if d.rounded() == d {
                return String(Int64(d))
            }
            return nil
        default:
            return nil
        }
    }
}

/// 最小的 JSON 值 AST，够撑 JSONRaw 用。
///
/// 不使用 `Any` + JSONSerialization 的原因：
///   1) JSONSerialization 不是 Sendable-friendly，跨 actor 传递会被警告
///   2) 我们只需要"保留 raw bytes + 取 id 的原始标量类型"，不需要通用容器
private struct AnyDecodable: Codable {
    enum Value {
        case string(String)
        case int(Int64)
        case double(Double)
        case bool(Bool)
        case null
        case array([AnyDecodable])
        case object([String: AnyDecodable])
    }
    let value: Value

    init(from decoder: Decoder) throws {
        let c = try decoder.singleValueContainer()
        if c.decodeNil() {
            self.value = .null
        } else if let b = try? c.decode(Bool.self) {
            self.value = .bool(b)
        } else if let i = try? c.decode(Int64.self) {
            self.value = .int(i)
        } else if let d = try? c.decode(Double.self) {
            self.value = .double(d)
        } else if let s = try? c.decode(String.self) {
            self.value = .string(s)
        } else if let arr = try? c.decode([AnyDecodable].self) {
            self.value = .array(arr)
        } else if let obj = try? c.decode([String: AnyDecodable].self) {
            self.value = .object(obj)
        } else {
            throw DecodingError.dataCorruptedError(in: c, debugDescription: "unsupported JSON value")
        }
    }

    func encode(to encoder: Encoder) throws {
        var c = encoder.singleValueContainer()
        switch value {
        case .null:          try c.encodeNil()
        case .bool(let b):   try c.encode(b)
        case .int(let i):    try c.encode(i)
        case .double(let d): try c.encode(d)
        case .string(let s): try c.encode(s)
        case .array(let a):  try c.encode(a)
        case .object(let o): try c.encode(o)
        }
    }
}
