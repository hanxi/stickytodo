//
//  AppState.swift
//  stickytodo
//
//  应用全局状态：认证信息、服务器配置、APIClient 注入、便签数组 + 本地持久化。
//  阶段十完成：
//    - Keychain 持久化 token（重启保持登录）
//    - StickyStore 持久化便签数组（debounce 保存）
//    - login/logout 打通 Keychain 写/清
//  阶段十一/十二会在此基础上接 UI。
//

import Foundation
import os
import SwiftUI

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

    /// 便签数组自动落盘的去抖延迟。高频编辑时仅以最后一次为准写盘。
    static let stickiesSaveDebounce: Duration = .milliseconds(300)

    // MARK: - 可观察字段

    /// 服务器地址。修改后会被持久化到 UserDefaults。
    /// 仅允许通过 `updateServerBaseURL` 修改，确保 trim + 持久化原子完成。
    @Published private(set) var serverBaseURL: String

    /// 当前登录用户名；nil 表示未登录。
    @Published private(set) var username: String?

    /// 当前 access token。阶段十起：内存 + Keychain 双写；启动时优先从 Keychain 恢复。
    /// `private(set)` 防止外部 UI 绕过 login/logout 流程直接写入。
    @Published private(set) var authToken: String?

    /// 所有便签的快照。写入时会 debounce 触发 StickyStore.save。
    /// 初始化时从 StickyStore 加载；为空表示用户尚未创建任何便签。
    @Published var stickies: [StickyNote] {
        didSet {
            // stickies 的任何变更都安排一次 debounce 落盘。
            guard stickies != oldValue else { return }
            scheduleStickiesSave()
        }
    }

    // MARK: - 网络客户端

    /// 共享的 HTTP 客户端。与 AppState 同生命周期。
    ///
    /// 所有 provider 闭包只在 MainActor 上下文中被调用（API 请求入口都在
    /// `@MainActor AppState` 的方法内 `await`），满足 Swift 并发模型。
    let apiClient: APIClient

    // MARK: - 构造

    init(
        userDefaults: UserDefaults = .standard,
        session: URLSession = .shared,
        stickyStore: StickyStore? = nil,
        keychainStore: KeychainStore = KeychainStore()
    ) {
        self.userDefaults = userDefaults
        // 只解一次默认值，避免在下面第二次读取时重复构造 StickyStore 实例。
        let effectiveStickyStore = stickyStore ?? StickyStore(defaults: userDefaults)
        self.stickyStore = effectiveStickyStore
        self.keychainStore = keychainStore

        let rawURL = userDefaults.string(forKey: Self.defaultsKeyServerBaseURL)?
            .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        let initialURL = rawURL.isEmpty ? Self.defaultServerBaseURL : rawURL
        self.serverBaseURL = initialURL

        let rawName = userDefaults.string(forKey: Self.defaultsKeyUsername)?
            .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        let effectiveName: String? = rawName.isEmpty ? nil : rawName
        self.username = effectiveName

        // 便签：直接从 StickyStore 拉一份快照；失败或空都返回空数组。
        self.stickies = effectiveStickyStore.load()

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
    }

    /// 登录：调用 `POST /api/login`，成功则把 token 存到内存 + Keychain，
    /// username 落到 UserDefaults。失败向调用方抛 APIError。
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
    }

    /// 登出：清空内存 token + Keychain 记录。
    /// 用户名保留在 UserDefaults 以便下次登录 UI 回填。
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
    }

    /// 立即把当前便签数组落盘。用于 App 即将退出时同步写，绕过 debounce。
    func flushStickiesSave() {
        stickiesSaveTask?.cancel()
        stickiesSaveTask = nil
        stickyStore.save(stickies)
    }

    // MARK: - 便签变更封装

    /// 新增一个便签，附加到数组末尾。StickyStore 会被 debounce 写盘。
    /// 若调用方未显式指定便签位置，则基于当前便签数量叠加偏移，避免与已有便签
    /// 窗口完全重合（默认 StickyNote() 的 frame 起点都是 (100,100)）。
    /// - Returns: 新增的便签（含自动生成的 id）。
    @discardableResult
    func addSticky(_ note: StickyNote = StickyNote()) -> StickyNote {
        // 仅当调用方传入的是默认位置时才做叠加偏移，避免覆盖调用方显式指定的位置。
        let defaultFrame = CodableRect(StickyNote.defaultFrame)
        let final: StickyNote
        if note.frame == defaultFrame {
            let offset = CGFloat(stickies.count % 12) * 24
            var shifted = StickyNote.defaultFrame
            shifted.origin.x += offset
            shifted.origin.y += offset
            var copy = note
            copy.frame = CodableRect(shifted)
            final = copy
        } else {
            final = note
        }
        stickies.append(final)
        return final
    }

    /// 按 id 删除便签。不存在时静默忽略。
    func removeSticky(id: UUID) {
        stickies.removeAll { $0.id == id }
    }

    /// 按 id 更新便签的 frame（窗口移动/缩放回调专用）。
    /// 不存在时静默忽略（可能是用户已删除）。
    func updateStickyFrame(id: UUID, frame: CGRect) {
        guard let idx = stickies.firstIndex(where: { $0.id == id }) else { return }
        let newFrame = CodableRect(frame)
        guard stickies[idx].frame != newFrame else { return }
        stickies[idx].frame = newFrame
    }

    /// 按 id 替换整个 StickyNote。阶段十二的 UI 编辑（标题/颜色/筛选）会走这里。
    /// 不存在时静默忽略。
    func updateSticky(_ note: StickyNote) {
        guard let idx = stickies.firstIndex(where: { $0.id == note.id }) else { return }
        guard stickies[idx] != note else { return }
        stickies[idx] = note
    }

    // MARK: - Private

    private let userDefaults: UserDefaults
    private let stickyStore: StickyStore
    private let keychainStore: KeychainStore

    /// APIClient 的三个 provider 闭包共享的"可变盒子"。AppState 更新状态时
    /// 直接写 box.value，APIClient 下次读到新值；无需重建 APIClient。
    private let serverBaseURLBox: _MutableBox<String>
    private let tokenBox: _MutableBox<String?>
    private let unauthorizedBox: _MutableBox<() -> Void>

    /// 正在等待落盘的 task；新变更到来时取消旧 task，实现去抖。
    private var stickiesSaveTask: Task<Void, Never>?

    private static let log = Logger(subsystem: "com.hanxi.stickytodo", category: "AppState")

    /// 安排一次去抖落盘。在 `stickiesSaveDebounce` 窗口内的连续变更只写最后一次。
    private func scheduleStickiesSave() {
        stickiesSaveTask?.cancel()
        let snapshotStore = stickyStore
        // Task 在 MainActor 上继承，但 sleep 期间挂起不阻塞 UI。
        stickiesSaveTask = Task { [weak self] in
            try? await Task.sleep(for: Self.stickiesSaveDebounce)
            guard !Task.isCancelled, let self else { return }
            // 读取最新快照再写，避免拿到 schedule 时的旧值。
            snapshotStore.save(self.stickies)
        }
    }
}

/// 一个可变引用盒，仅在 AppState 内部用于给 APIClient 的闭包提供"可更新的读取视角"。
/// 显式 class 以便所有闭包共享同一引用，不走值拷贝。
///
/// 命名用下划线前缀 + fileprivate 等效（`private` 在 top-level 即 file 私有），
/// 表示外部不应直接依赖。
private final class _MutableBox<Value> {
    var value: Value
    init(value: Value) { self.value = value }
}
