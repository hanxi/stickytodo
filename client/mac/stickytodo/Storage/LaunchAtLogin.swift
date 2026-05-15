//
//  LaunchAtLogin.swift
//  stickytodo
//
//  封装 ServiceManagement.SMAppService.mainApp，提供「开机自动启动」开关。
//
//  为什么不用旧的 SMLoginItemSetEnabled？
//    - SMLoginItemSetEnabled 在 macOS 13 起标记 deprecated，需要内嵌一个独立的
//      Helper Bundle 才能工作；维护成本高。
//    - SMAppService.mainApp 是 macOS 13+ 提供的现代 API：直接以主 App 自身为
//      LoginItem 注册，**不需要额外 entitlement、不需要 Helper bundle**，沙盒
//      App 也可以调用。最低部署目标已经是 13.0（见 project.pbxproj），可直接用。
//
//  状态语义：
//    - .enabled              已注册并启用
//    - .notRegistered        未注册（默认；用户从未开过开关）
//    - .notFound             已被系统清理（如 App 被移动 / 重新签名）
//    - .requiresApproval     已注册但用户在「系统设置 → 通用 → 登录项」里关掉了
//      ↑ 注意：requiresApproval 在 UI 上仍应展示为「未启用」，并提示用户去系统
//      设置里允许；register() 只能让 status=enabled，无法越过用户的拒绝。
//

import Foundation
import ServiceManagement
import os

/// 「开机自动启动」开关。封装为命名空间（无状态 enum），所有方法都是 static。
///
/// 线程安全：SMAppService 的 register/unregister/status 文档未明示线程要求，
/// 实测主线程调用最稳；调用方（SettingsView）本就在 MainActor，所以这里
/// 不再额外做并发处理。
@MainActor
enum LaunchAtLogin {

    private static let log = Logger(subsystem: "com.hanxi.stickytodo", category: "LaunchAtLogin")

    /// 当前是否已启用开机自启。
    /// 仅 `.enabled` 视为真启用；`.requiresApproval` 表示用户在系统设置里禁用了，
    /// 此时 UI 应当呈现关闭态并允许用户重新点开重新触发授权流程。
    static var isEnabled: Bool {
        SMAppService.mainApp.status == .enabled
    }

    /// 原始状态，UI 需要区分 `.requiresApproval` 给出额外提示时使用。
    static var rawStatus: SMAppService.Status {
        SMAppService.mainApp.status
    }

    /// 设置开机自启开关。
    ///
    /// 幂等：当前已是目标状态时不重复调用 register/unregister，避免出现一闪
    /// 而过的 LoginItem 异常。
    ///
    /// 失败常见原因：App 未签名 / 沙盒配置异常 / 用户在系统设置里硬禁用了登录项。
    /// 失败时把错误抛给调用方，UI 决定如何展示。
    static func setEnabled(_ enabled: Bool) throws {
        let service = SMAppService.mainApp
        let status = service.status
        if enabled {
            // .enabled / .requiresApproval 都视为「已注册过」，无需 register；
            // 但 .requiresApproval 时 register 也无害，留给后续观察 (Apple
            // 文档建议依然允许调用 register 触发系统重新审批)。
            if status != .enabled {
                try service.register()
            }
        } else {
            // 仅在确实有注册项时反注册，避免对 .notRegistered 调 unregister 抛 NSError。
            if status == .enabled || status == .requiresApproval {
                try service.unregister()
            }
        }
        log.info("LaunchAtLogin set \(enabled, privacy: .public) → status=\(String(describing: SMAppService.mainApp.status), privacy: .public)")
    }
}
