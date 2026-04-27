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

import SwiftUI

struct SettingsView: View {
    @EnvironmentObject private var appState: AppState

    // 服务器地址草稿与校验
    @State private var urlDraft: String = ""
    @State private var urlValidation: URLValidationState = .valid

    // 登录表单
    @State private var usernameDraft: String = ""
    @State private var passwordDraft: String = ""
    @State private var loginSubmitting: Bool = false
    @State private var loginError: String?

    // "测试连接"（GET /health）按钮状态
    @State private var pingSubmitting: Bool = false
    @State private var pingResult: PingResult?

    var body: some View {
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
            }

            Section("账号") {
                if appState.isAuthenticated {
                    authenticatedBlock
                } else {
                    unauthenticatedBlock
                }
            }

            Section("关于") {
                aboutBlock
            }
        }
        .formStyle(.grouped)
        .padding()
        .frame(minWidth: 460, minHeight: 420)
        .onAppear {
            urlDraft = appState.serverBaseURL
            if let name = appState.username { usernameDraft = name }
        }
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
