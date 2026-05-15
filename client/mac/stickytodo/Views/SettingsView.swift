//
//  SettingsView.swift
//  stickytodo
//
//  Settings Scene 的主视图。两部分：
//    1. 服务器 Base URL 表单（保存时自动补 http:// + 简单校验）
//    2. 认证：未登录 → 登录表单；已登录 → 显示账号 + 登出
//
//  URL 校验规则：
//    - trim 后若未带 scheme，自动前缀 http://
//    - URL(string:) 解析通过视为合法；否则标红提示
//

import ServiceManagement
import SwiftUI

struct SettingsView: View {
    @EnvironmentObject private var appState: AppState

    // 服务器地址草稿与校验
    @State private var urlDraft: String = ""
    @State private var urlValidation: URLValidationState = .valid

    // HTTP 代理草稿与校验
    @State private var proxyDraft: String = ""
    @State private var proxyValidation: URLValidationState = .valid

    // 登录表单
    @State private var usernameDraft: String = ""
    @State private var passwordDraft: String = ""
    @State private var loginSubmitting: Bool = false
    @State private var loginError: String?

    // "测试连接"（GET /health）按钮状态
    @State private var pingSubmitting: Bool = false
    @State private var pingResult: PingResult?

    // 「删除便签前弹出确认」偏好。
    //
    // 存储侧 key 沿用 StickyView 的 `sticky.skipDeleteConfirm`（"跳过确认"，值语义 reverse）。
    // 不在这里单独起新 key 是为了避免双 key 同义造成状态分裂——StickyView 是该偏好的唯一
    // 消费点，`@AppStorage` 直接指向同一个 UserDefaults 条目，两处视图会自动同步。
    //
    // UI 侧用 `showStickyDeleteConfirmBinding`（计算属性，下面定义）把"跳过=true / 弹窗=false"
    // 反转成面向用户的"弹窗=true / 跳过=false"，让 Toggle 的正向语义（打开=弹提示）
    // 更符合直觉。
    @AppStorage("sticky.skipDeleteConfirm") private var skipStickyDeleteConfirm = false

    // 「删除待办前弹出确认」偏好。
    //
    // key 与便签分离（`todo.skipDeleteConfirm`），让用户能分别控制两种资源的确认策略——
    // 待办走软删且可从"仅已删除"筛选恢复，风险明显低于便签（便签删除是云端硬删不可恢复），
    // 很多用户更倾向把待办的提示关掉、但保留便签的提示。两套 key 完全独立不共享状态。
    // 真值消费点在 TodoRow（同名 @AppStorage），两处视图自动同步。
    @AppStorage("todo.skipDeleteConfirm") private var skipTodoDeleteConfirm = false

    // 「开机自动启动」UI 态。真值在系统（SMAppService.mainApp.status），这里只缓存
    // 给 Toggle 用；onAppear 时拉一次最新值，Toggle 切换时调用 LaunchAtLogin.setEnabled。
    @State private var launchAtLoginOn: Bool = LaunchAtLogin.isEnabled
    /// 设置 LaunchAtLogin 失败时的错误文案（可空 = 无错误）；典型场景是用户在
    /// 「系统设置 → 通用 → 登录项」里硬禁用了，register() 抛出 NSError。
    @State private var launchAtLoginError: String?

    var body: some View {
        TabView {
            generalTab
                .tabItem { Label("设置", systemImage: "gearshape") }

            historyTab
                .tabItem { Label("历史", systemImage: "clock.arrow.circlepath") }

            aboutTab
                .tabItem { Label("关于", systemImage: "info.circle") }
        }
        .frame(width: 520, height: 420)
        .onAppear {
            urlDraft = appState.serverBaseURL
            proxyDraft = appState.httpProxy
            if let name = appState.username {
                usernameDraft = name
                // 未登录时把 Keychain 里保存的密码回填到密码框，让用户「打开设置就能直接登录」。
                // 已登录时不动 passwordDraft——它本来就是隐藏的，无需赋值。
                if !appState.isAuthenticated, passwordDraft.isEmpty,
                   let saved = appState.savedPassword(for: name) {
                    passwordDraft = saved
                }
            }
            // 重新拉一次系统的 LoginItem 状态，避免用户在「系统设置」里改过后这边显示陈旧值。
            launchAtLoginOn = LaunchAtLogin.isEnabled
            launchAtLoginError = nil
        }
    }

    // MARK: - Tabs

    /// 「设置」Tab：服务器 Base URL + 账号登录/登出。
    @ViewBuilder
    private var generalTab: some View {
        Form {
            Section("服务器") {
                TextField("Base URL", text: $urlDraft, prompt: Text("例如 http://127.0.0.1:8080"))
                    .textFieldStyle(.roundedBorder)
                    .onSubmit { commitURL() }
                    // 草稿变更时清掉上一次 ping 结果，避免"改了地址仍显示旧 ❌"的误导
                    .onChange(of: urlDraft) { _ in pingResult = nil }

                if case .invalid(let reason) = urlValidation {
                    Text(reason)
                        .font(.caption)
                        .foregroundStyle(.red)
                }

                HStack {
                    Button("保存地址") { commitURL() }
                        .buttonStyle(.borderedProminent)
                        .disabled(!isURLDirty)
                    Button("恢复") {
                        urlDraft = appState.serverBaseURL
                        urlValidation = .valid
                    }
                    .disabled(!isURLDirty)
                    Button {
                        Task { await submitPing() }
                    } label: {
                        if pingSubmitting {
                            ProgressView().controlSize(.small)
                        } else {
                            Label("测试连接", systemImage: "network")
                        }
                    }
                    .buttonStyle(.bordered)
                    .disabled(pingSubmitting)
                    Spacer()
                    Text("当前：\(appState.serverBaseURL)")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }

                if let result = pingResult {
                    switch result {
                    case .success(let summary):
                        Text(summary)
                            .font(.caption)
                            .foregroundStyle(.green)
                            .fixedSize(horizontal: false, vertical: true)
                    case .failure(let message):
                        Text(message)
                            .font(.caption)
                            .foregroundStyle(.red)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }

                // HTTP Proxy 配置：留空表示直连。
                // 仅支持 `http://host[:port]`，HTTPS 流量会被指向同一代理（mitm 场景）。
                Divider()
                TextField(
                    "HTTP Proxy",
                    text: $proxyDraft,
                    prompt: Text("例如 http://127.0.0.1:7890；留空表示直连")
                )
                .textFieldStyle(.roundedBorder)
                .onSubmit { commitProxy() }

                if case .invalid(let reason) = proxyValidation {
                    Text(reason)
                        .font(.caption)
                        .foregroundStyle(.red)
                }

                HStack {
                    Button("保存代理") { commitProxy() }
                        .buttonStyle(.borderedProminent)
                        .disabled(!isProxyDirty)
                    Button("恢复") {
                        proxyDraft = appState.httpProxy
                        proxyValidation = .valid
                    }
                    .disabled(!isProxyDirty)
                    Spacer()
                    Text(appState.httpProxy.isEmpty ? "当前：直连" : "当前：\(appState.httpProxy)")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }
            }

            Section("账号") {
                if appState.isAuthenticated {
                    authenticatedBlock
                } else {
                    unauthenticatedBlock
                }
            }

            Section("通用") {
                Toggle(isOn: launchAtLoginBinding) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("开机自动启动")
                        Text("登录系统时自动打开 StickyTodo 并常驻菜单栏。")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
                if let err = launchAtLoginError {
                    Text(err)
                        .font(.caption)
                        .foregroundStyle(.red)
                        .fixedSize(horizontal: false, vertical: true)
                }

                Toggle(isOn: showStickyDeleteConfirmBinding) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("删除便签前弹出确认")
                        Text("关闭后，便签标题栏的删除按钮将直接执行删除，不再二次确认。")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
                Toggle(isOn: showTodoDeleteConfirmBinding) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("删除待办前弹出确认")
                        Text("关闭后，待办菜单里的删除将直接执行软删。软删仍可通过\"仅已删除\"筛选恢复。")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
            }
        }
        .formStyle(.grouped)
        .padding()
    }

    /// 便签：面向 UI 的正向绑定：`true` = 删除时弹确认，`false` = 直接删除。
    ///
    /// 底层存储 `skipStickyDeleteConfirm` 是反向语义（"跳过确认=true"），在这里做一次反转：
    /// `get { !skipStickyDeleteConfirm } / set { skipStickyDeleteConfirm = !newValue }`。
    /// 这样 Toggle 的视觉态（"开=弹提示"）和用户语义一致，且完全不需要改 StickyView 端。
    private var showStickyDeleteConfirmBinding: Binding<Bool> {
        Binding(
            get: { !skipStickyDeleteConfirm },
            set: { skipStickyDeleteConfirm = !$0 }
        )
    }

    /// 待办：面向 UI 的正向绑定。与便签那套完全同构——只是 key 不同（`todo.skipDeleteConfirm`），
    /// 这样两项偏好彼此独立、互不影响。TodoRow 的 @AppStorage 消费同一个 key，Toggle 切换会
    /// 通过 UserDefaults 自动广播到所有打开的待办行。
    private var showTodoDeleteConfirmBinding: Binding<Bool> {
        Binding(
            get: { !skipTodoDeleteConfirm },
            set: { skipTodoDeleteConfirm = !$0 }
        )
    }

    /// 「开机自动启动」绑定。Toggle 设值时立刻调 SMAppService 注册/反注册；
    /// 失败把 UI 状态回滚到旧值并把错误文案落到 `launchAtLoginError`，避免 UI
    /// 与系统真实状态不一致。
    private var launchAtLoginBinding: Binding<Bool> {
        Binding(
            get: { launchAtLoginOn },
            set: { newValue in
                let previous = launchAtLoginOn
                launchAtLoginOn = newValue
                launchAtLoginError = nil
                do {
                    try LaunchAtLogin.setEnabled(newValue)
                    // 写完后再以系统真实状态对齐，处理 .requiresApproval 这种「用户在系统
                    // 设置里硬禁用过」的情况——register() 不抛错但 status 不会变成 .enabled。
                    let resolved = LaunchAtLogin.isEnabled
                    launchAtLoginOn = resolved
                    if newValue && !resolved {
                        launchAtLoginError = "已请求启用，但系统当前不允许；请在「系统设置 → 通用 → 登录项」中允许 StickyTodo。"
                    }
                } catch {
                    launchAtLoginOn = previous
                    launchAtLoginError = "设置失败：\(error.localizedDescription)"
                }
            }
        )
    }

    /// 「记住密码」绑定。直接把开关持久化到 AppState（同时会处理 Keychain 里残留密码的清理）。
    private var rememberPasswordBinding: Binding<Bool> {
        Binding(
            get: { appState.rememberPassword },
            set: { appState.updateRememberPassword($0) }
        )
    }

    /// 「历史」Tab：已登录时嵌入 HistoryView(global)；未登录时显示提示。
    @ViewBuilder
    private var historyTab: some View {
        if appState.isAuthenticated {
            HistoryView(
                mode: .global,
                apiClient: appState.apiClient,
                embedded: true
            )
        } else {
            VStack(spacing: 10) {
                Spacer()
                Image(systemName: "lock")
                    .imageScale(.large)
                    .foregroundStyle(.secondary)
                Text("请先在「设置」Tab 登录后查看历史")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                Spacer()
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .padding()
        }
    }

    /// 「关于」Tab：展示产品信息、版本号、链接、版权 + 支持项目入口。
    ///
    /// 「支持项目」Section 复用 `SponsorPopover(standalone: false)`，
    /// 和 MenuBarContent 的爱心按钮 popover 共用一份视图代码，保证视觉与
    /// 文案一致。`standalone: false` 让内部去掉独立 popover 的外边距和
    /// 标题，交由外层 Section 渲染。
    @ViewBuilder
    private var aboutTab: some View {
        Form {
            Section("关于") {
                aboutBlock
            }
            Section("支持项目") {
                SponsorPopover(standalone: false)
            }
        }
        .formStyle(.grouped)
        .padding()
    }

    // MARK: - Authenticated block

    @ViewBuilder
    private var authenticatedBlock: some View {
        LabeledContent("状态") {
            Text("已登录")
                .foregroundStyle(.green)
        }
        if let name = appState.username {
            LabeledContent("用户名") {
                Text(name)
            }
        }
        HStack {
            Spacer()
            Button(role: .destructive) {
                appState.logout()
                passwordDraft = ""
                loginError = nil
            } label: {
                Label("登出", systemImage: "rectangle.portrait.and.arrow.right")
            }
            .buttonStyle(.bordered)
        }
    }

    // MARK: - Unauthenticated block

    @ViewBuilder
    private var unauthenticatedBlock: some View {
        TextField("用户名", text: $usernameDraft)
            .textFieldStyle(.roundedBorder)
            .disabled(loginSubmitting)
        SecureField("密码", text: $passwordDraft)
            .textFieldStyle(.roundedBorder)
            .disabled(loginSubmitting)
            .onSubmit {
                Task { await submitLogin() }
            }

        Toggle(isOn: rememberPasswordBinding) {
            Text("记住密码")
        }
        .toggleStyle(.checkbox)
        .disabled(loginSubmitting)

        if let err = loginError {
            Text(err)
                .font(.caption)
                .foregroundStyle(.red)
                .fixedSize(horizontal: false, vertical: true)
        }

        HStack {
            Spacer()
            if loginSubmitting {
                ProgressView().controlSize(.small)
            }
            Button("登录") {
                Task { await submitLogin() }
            }
            .buttonStyle(.borderedProminent)
            .keyboardShortcut(.defaultAction)
            .disabled(loginSubmitting || !canSubmitLogin)
        }
    }

    // MARK: - About block

    /// "关于" 区块：展示产品信息、项目代号、版本号、bundle id、仓库/镜像链接、版权。
    ///
    /// 版本号与构建号从 Info.plist 的 CFBundleShortVersionString / CFBundleVersion 读取，
    /// 和 release pipeline 里注入的 xcodebuild MARKETING_VERSION / CURRENT_PROJECT_VERSION 对齐。
    @ViewBuilder
    private var aboutBlock: some View {
        LabeledContent("产品") {
            Text("StickyTodo · 便签 Todo")
        }
        LabeledContent("项目代号") {
            Text("stickytodo")
                .font(.body.monospaced())
                .textSelection(.enabled)
        }
        LabeledContent("版本") {
            Text("\(Self.appVersion) (build \(Self.appBuild))")
                .textSelection(.enabled)
        }
        LabeledContent("Bundle ID") {
            Text(Self.bundleIdentifier)
                .font(.caption.monospaced())
                .foregroundStyle(.secondary)
                .textSelection(.enabled)
        }
        LabeledContent("GitHub") {
            Link("github.com/hanxi/stickytodo",
                 destination: URL(string: "https://github.com/hanxi/stickytodo")!)
        }
        LabeledContent("Docker") {
            Text("docker.io/hanxi/stickytodo")
                .font(.caption.monospaced())
                .foregroundStyle(.secondary)
                .textSelection(.enabled)
        }
        LabeledContent("版权") {
            Text(Self.copyright)
                .font(.caption)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    // MARK: - Bundle helpers

    private static var appVersion: String {
        (Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String) ?? "dev"
    }

    private static var appBuild: String {
        (Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String) ?? "0"
    }

    private static var bundleIdentifier: String {
        Bundle.main.bundleIdentifier ?? "com.hanxi.stickytodo"
    }

    private static var copyright: String {
        (Bundle.main.object(forInfoDictionaryKey: "NSHumanReadableCopyright") as? String)
            ?? "Copyright © 2026 StickyTodo (hanxi)."
    }

    // MARK: - Logic

    private var isURLDirty: Bool {
        normalizedURLDraft() != appState.serverBaseURL
    }

    /// 代理草稿是否与已生效值不同（trim 后比较）。
    private var isProxyDirty: Bool {
        proxyDraft.trimmingCharacters(in: .whitespacesAndNewlines) != appState.httpProxy
    }

    private var canSubmitLogin: Bool {
        !usernameDraft.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty &&
        !passwordDraft.isEmpty
    }

    /// 对草稿做"trim + 自动补 http://"，不做持久化。
    private func normalizedURLDraft() -> String {
        var v = urlDraft.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !v.isEmpty else { return "" }
        // 没有 scheme（://）时自动补 http://，兼容用户只填 host:port 的场景
        if !v.contains("://") {
            v = "http://" + v
        }
        return v
    }

    private func commitURL() {
        let normalized = normalizedURLDraft()
        guard !normalized.isEmpty else {
            urlValidation = .invalid(reason: "地址不能为空")
            return
        }
        // URLComponents 要求 scheme + host 至少齐全
        guard let comps = URLComponents(string: normalized),
              let scheme = comps.scheme, !scheme.isEmpty,
              let host = comps.host, !host.isEmpty else {
            urlValidation = .invalid(reason: "地址格式不合法：\(normalized)")
            return
        }
        _ = host // 仅用于保证 URLComponents 解析完整
        urlValidation = .valid
        urlDraft = normalized // 把 http:// 补上的值回填到输入框
        appState.updateServerBaseURL(normalized)
    }

    /// 提交 Proxy 草稿。规则：
    ///   - trim 后空串：直接接受（直连）
    ///   - 非空：必须能解析为 URL，scheme=http，host 非空
    /// 校验通过后调用 `appState.updateHttpProxy`，失败则把原因落到 `proxyValidation`。
    private func commitProxy() {
        let trimmed = proxyDraft.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty {
            proxyValidation = .valid
            proxyDraft = ""
            appState.updateHttpProxy("")
            return
        }
        guard let comps = URLComponents(string: trimmed),
              let scheme = comps.scheme?.lowercased(), scheme == "http",
              let host = comps.host, !host.isEmpty
        else {
            proxyValidation = .invalid(
                reason: "代理格式不合法：仅支持 http://host[:port]"
            )
            return
        }
        _ = host
        proxyValidation = .valid
        proxyDraft = trimmed
        appState.updateHttpProxy(trimmed)
    }

    private func submitLogin() async {
        guard canSubmitLogin else { return }
        loginSubmitting = true
        loginError = nil
        defer { loginSubmitting = false }
        do {
            try await appState.login(
                username: usernameDraft.trimmingCharacters(in: .whitespacesAndNewlines),
                password: passwordDraft
            )
            passwordDraft = ""
        } catch {
            loginError = (error as? APIError)?.userMessage ?? error.localizedDescription
        }
    }

    /// 向**当前草稿**的地址发 `GET /health`，用于登录前验证地址是否可达。
    ///
    /// 关键点：ping 的必须是用户"现在看到"的地址，而不是上次点过"保存地址"的值；
    /// 否则用户改了地址但没保存时，这个按钮会测旧地址，造成误导。
    /// 做法：若草稿校验通过且与已保存值不一致，则隐式 commit（相当于"保存"），
    /// 再让 APIClient 读取到新的 baseURL 发请求；草稿非法时直接短路成 failure。
    private func submitPing() async {
        // 1) 校验草稿
        let normalized = normalizedURLDraft()
        guard !normalized.isEmpty,
              let comps = URLComponents(string: normalized),
              let scheme = comps.scheme, !scheme.isEmpty,
              let host = comps.host, !host.isEmpty
        else {
            urlValidation = .invalid(reason: "地址格式不合法：\(urlDraft)")
            pingResult = .failure("❌ 无法测试：当前 Base URL 格式不合法")
            return
        }
        _ = host
        urlValidation = .valid

        // 2) 若与已保存值不一致，先隐式 commit，让 APIClient 从下一次请求起用新地址
        if normalized != appState.serverBaseURL {
            urlDraft = normalized
            appState.updateServerBaseURL(normalized)
        }

        // 3) 发请求
        pingSubmitting = true
        pingResult = nil
        defer { pingSubmitting = false }
        do {
            let resp = try await appState.apiClient.health()
            pingResult = .success(
                "✅ 连接成功：\(normalized) · server=\(resp.server) · version=\(resp.version)"
            )
        } catch {
            let msg = (error as? APIError)?.userMessage ?? error.localizedDescription
            pingResult = .failure("❌ 连接失败（\(normalized)）：\(msg)")
        }
    }
}

// MARK: - URLValidationState

private enum URLValidationState: Equatable {
    case valid
    case invalid(reason: String)
}

// MARK: - PingResult

/// "测试连接" 按钮的展示状态。独立于 APIError 以便同时承载网络错误与
/// 业务层成功文案（含 server 名称和版本号）。
private enum PingResult: Equatable {
    case success(String)
    case failure(String)
}
