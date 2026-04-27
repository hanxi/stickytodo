//
//  KeychainStore.swift
//  stickytodo
//
//  使用 Security.framework 原生 SecItemAdd / SecItemCopyMatching / SecItemUpdate /
//  SecItemDelete 管理登录 token。不引入 KeychainAccess 等第三方库。
//
//  设计：
//    - service = Bundle ID（与 App 绑定，避免与其他 App 冲突）
//    - account = username（多账号维度；同一 App 不同账号隔离）
//    - accessible = kSecAttrAccessibleAfterFirstUnlock
//      （系统首次解锁后可读；StickyTodo 菜单栏常驻，需要开机自动拉起时也能取到）
//

import Foundation
import Security

/// Keychain 访问错误。
enum KeychainError: Error, Equatable {
    /// OSStatus 非成功；附带原始状态码，便于日志定位。
    case unexpectedStatus(OSStatus)
    /// 账号为空。API 要求 username 非空字符串。
    case emptyAccount
    /// Token 字符串在 UTF-8 编码时失败（理论上不会发生）。
    case encoding
    /// 预期存在一条记录，但实际未找到。
    case itemNotFound

    /// 用户可读描述。
    var userMessage: String {
        switch self {
        case .unexpectedStatus(let s): return "Keychain 错误（状态码 \(s)）"
        case .emptyAccount: return "账号不能为空"
        case .encoding: return "Token 编码失败"
        case .itemNotFound: return "Keychain 中未找到对应记录"
        }
    }
}

/// Keychain 存取器。
///
/// 所有方法线程安全（Security.framework 的 SecItem* 接口本身线程安全）。
/// 不持有状态，因此不需要 actor 约束。
struct KeychainStore {

    /// Keychain service 标识。固定为 App Bundle ID。
    static let service = "com.hanxi.stickytodo"

    /// 允许自定义 service，用于单测；生产代码默认 `Self.service`。
    private let service: String

    init(service: String = KeychainStore.service) {
        self.service = service
    }

    // MARK: - 公共 API

    /// 保存 token。若同 account 已有记录，则原子地更新（SecItemUpdate）。
    func saveToken(username: String, token: String) throws {
        let account = try Self.normalize(username)
        guard let data = token.data(using: .utf8) else {
            throw KeychainError.encoding
        }

        // 先尝试更新：
        //   - errSecSuccess      → 已更新，结束
        //   - errSecItemNotFound → 不存在，降级为 add
        //   - 其他               → 抛错
        let updateAttrs: [String: Any] = [
            kSecValueData as String: data
        ]
        let updateStatus = SecItemUpdate(
            Self.baseQuery(service: service, account: account) as CFDictionary,
            updateAttrs as CFDictionary
        )
        switch updateStatus {
        case errSecSuccess:
            return
        case errSecItemNotFound:
            break // 继续到 add
        default:
            throw KeychainError.unexpectedStatus(updateStatus)
        }

        var addAttrs = Self.baseQuery(service: service, account: account)
        addAttrs[kSecValueData as String] = data
        addAttrs[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlock

        let addStatus = SecItemAdd(addAttrs as CFDictionary, nil)
        guard addStatus == errSecSuccess else {
            throw KeychainError.unexpectedStatus(addStatus)
        }
    }

    /// 读取 token。
    /// - 返回 `nil` 表示该账号在 Keychain 中没有记录（errSecItemNotFound）。
    /// - 抛错仅在遇到其它 OSStatus 或账号为空时发生，调用方据此决定是否降级。
    func readToken(username: String) throws -> String? {
        let account = try Self.normalize(username)
        var query = Self.baseQuery(service: service, account: account)
        query[kSecReturnData as String] = kCFBooleanTrue
        query[kSecMatchLimit as String] = kSecMatchLimitOne

        var item: CFTypeRef?
        let status = SecItemCopyMatching(query as CFDictionary, &item)
        switch status {
        case errSecSuccess:
            guard let data = item as? Data,
                  let token = String(data: data, encoding: .utf8),
                  !token.isEmpty else {
                throw KeychainError.encoding
            }
            return token
        case errSecItemNotFound:
            return nil
        default:
            throw KeychainError.unexpectedStatus(status)
        }
    }

    /// 删除 token。已不存在时视为成功（幂等）。
    func deleteToken(username: String) throws {
        let account = try Self.normalize(username)
        let status = SecItemDelete(Self.baseQuery(service: service, account: account) as CFDictionary)
        switch status {
        case errSecSuccess, errSecItemNotFound:
            return
        default:
            throw KeychainError.unexpectedStatus(status)
        }
    }

    // MARK: - Private helpers

    /// 构造 SecItem 操作的基础 query：class + service + account。
    /// kSecAttrAccessible 不放在 baseQuery 里，因为 SecItemUpdate 的 query 部分
    /// 放 accessible 会引起匹配失败（accessible 只用于 add 的 attributes 里）。
    private static func baseQuery(service: String, account: String) -> [String: Any] {
        [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
        ]
    }

    /// 校验并规范化 username：去空白后不能为空。
    private static func normalize(_ username: String) throws -> String {
        let trimmed = username.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else {
            throw KeychainError.emptyAccount
        }
        return trimmed
    }
}
