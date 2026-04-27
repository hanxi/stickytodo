//
//  HistoryView.swift
//  stickytodo
//
//  审计日志查看器。两种模式：
//    - .todo(id:title:)：单条 todo 的历史，调 apiClient.todoHistory
//    - .global：全局审计，调 apiClient.auditLogs
//
//  简单分页：首屏 50 条；点击"加载更多"请求下一页，追加到末尾。
//

import SwiftUI

struct HistoryView: View {

    enum Mode: Equatable {
        case todo(id: UInt64, title: String)
        case global
    }

    let mode: Mode

    /// API 客户端。所有调用方必须显式注入，确保历史视图始终可加载。
    let apiClient: APIClient

    @Environment(\.dismiss) private var dismiss

    @State private var items: [AuditLog] = []
    @State private var page: Int = 1
    @State private var total: Int = 0
    @State private var isLoading: Bool = false
    @State private var errorMessage: String?

    private let pageSize = 50

    var body: some View {
        VStack(spacing: 0) {
            header
            Divider()
            content
        }
        .task {
            await loadFirstPage()
        }
    }

    // MARK: - Sub-views

    @ViewBuilder
    private var header: some View {
        HStack {
            Button("关闭") { dismiss() }
                .keyboardShortcut(.cancelAction)
            Spacer()
            Text(title).font(.headline)
            Spacer()
            Button {
                Task { await loadFirstPage() }
            } label: {
                Image(systemName: "arrow.clockwise")
            }
            .disabled(isLoading)
        }
        .padding()
    }

    @ViewBuilder
    private var content: some View {
        if let msg = errorMessage, items.isEmpty {
            errorState(message: msg)
        } else if items.isEmpty {
            if isLoading {
                centerLoading
            } else {
                centerEmpty
            }
        } else {
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 0) {
                    ForEach(items) { log in
                        row(for: log)
                        Divider().opacity(0.35)
                    }
                    if items.count < total {
                        loadMoreButton
                    } else if items.count > 0 {
                        footerText
                    }
                }
                .padding(.vertical, 4)
            }
        }
    }

    @ViewBuilder
    private func row(for log: AuditLog) -> some View {
        HStack(alignment: .top, spacing: 10) {
            VStack(alignment: .leading, spacing: 2) {
                Text(actionLabel(log.action))
                    .font(.callout)
                    .foregroundStyle(actionColor(log.action))
                Text(Self.timeFormatter.string(from: log.createdAt))
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
            .frame(width: 108, alignment: .leading)

            VStack(alignment: .leading, spacing: 3) {
                HStack(spacing: 6) {
                    if let id = log.todoID {
                        Text("#\(id)")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                    if !log.actor.isEmpty {
                        Text(log.actor)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
                if !log.detail.isEmpty {
                    Text(log.detail)
                        .font(.caption)
                        .foregroundStyle(.primary)
                        .lineLimit(4)
                        .textSelection(.enabled)
                }
                if !log.ip.isEmpty || !log.userAgent.isEmpty {
                    Text("\(log.ip) · \(log.userAgent)")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 8)
    }

    @ViewBuilder
    private var loadMoreButton: some View {
        HStack {
            Spacer()
            Button {
                Task { await loadNextPage() }
            } label: {
                if isLoading {
                    ProgressView().controlSize(.mini)
                } else {
                    Text("加载更多（\(items.count) / \(total)）")
                }
            }
            .buttonStyle(.borderless)
            .disabled(isLoading)
            Spacer()
        }
        .padding(.vertical, 10)
    }

    @ViewBuilder
    private var footerText: some View {
        HStack {
            Spacer()
            Text("— 共 \(total) 条 —")
                .font(.caption2)
                .foregroundStyle(.secondary)
            Spacer()
        }
        .padding(.vertical, 10)
    }

    @ViewBuilder
    private var centerLoading: some View {
        VStack { Spacer(); ProgressView(); Spacer() }
            .frame(maxWidth: .infinity)
    }

    @ViewBuilder
    private var centerEmpty: some View {
        VStack(spacing: 6) {
            Spacer()
            Image(systemName: "tray")
                .imageScale(.large)
                .foregroundStyle(.secondary)
            Text("暂无审计记录").font(.callout).foregroundStyle(.secondary)
            Spacer()
        }
        .frame(maxWidth: .infinity)
    }

    @ViewBuilder
    private func errorState(message: String) -> some View {
        VStack(spacing: 8) {
            Spacer()
            Image(systemName: "exclamationmark.triangle")
                .imageScale(.large)
                .foregroundStyle(.orange)
            Text(message)
                .font(.callout)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .padding(.horizontal)
            Button("重试") {
                Task { await loadFirstPage() }
            }
            Spacer()
        }
        .frame(maxWidth: .infinity)
    }

    // MARK: - Derived

    private var title: String {
        switch mode {
        case .todo(_, let t): return "历史 · \(t)"
        case .global: return "审计日志"
        }
    }

    private func actionLabel(_ a: AuditAction) -> String {
        switch a {
        case .create: return "创建"
        case .update: return "更新"
        case .complete: return "完成"
        case .reopen: return "重开"
        case .delete: return "删除"
        case .restore: return "恢复"
        case .login: return "登录"
        case .loginFailed: return "登录失败"
        case .unknown: return "其他"
        }
    }

    private func actionColor(_ a: AuditAction) -> Color {
        switch a {
        case .delete, .loginFailed: return .red
        case .complete, .restore, .login: return .green
        case .update: return .blue
        case .create: return .purple
        case .reopen: return .orange
        case .unknown: return .secondary
        }
    }

    // MARK: - Networking

    private func loadFirstPage() async {
        isLoading = true
        errorMessage = nil
        defer { isLoading = false }
        do {
            let resp = try await fetchPage(page: 1)
            items = resp.items
            total = resp.total
            page = 1
        } catch {
            errorMessage = (error as? APIError)?.userMessage ?? error.localizedDescription
        }
    }

    private func loadNextPage() async {
        guard !isLoading, items.count < total else { return }
        isLoading = true
        defer { isLoading = false }
        let nextPage = page + 1
        do {
            let resp = try await fetchPage(page: nextPage)
            items.append(contentsOf: resp.items)
            page = nextPage
            total = resp.total
        } catch {
            errorMessage = (error as? APIError)?.userMessage ?? error.localizedDescription
        }
    }

    private func fetchPage(page: Int) async throws -> AuditListResponse {
        switch mode {
        case .todo(let id, _):
            return try await apiClient.todoHistory(id: id, page: page, pageSize: pageSize)
        case .global:
            let query: [URLQueryItem] = [
                .init(name: "page", value: String(page)),
                .init(name: "page_size", value: String(pageSize)),
            ]
            return try await apiClient.auditLogs(query: query)
        }
    }

    // MARK: - Helpers

    private static let timeFormatter: DateFormatter = {
        let f = DateFormatter()
        f.locale = Locale.current
        f.dateFormat = "MM-dd HH:mm:ss"
        return f
    }()
}
