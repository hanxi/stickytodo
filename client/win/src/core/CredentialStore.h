#pragma once

#include <string>
#include <optional>

namespace stickytodo::core {

/// Persists JWT token and server URL using the Windows Credential Manager.
/// Analogous to macOS KeychainStore (client/mac/stickytodo/Storage/KeychainStore.swift).
///
/// Credential Manager entry:
///   Target: "stickytodo/<username>"
///   UserName: <username>
///   CredentialBlob: JSON {"token":"...","base_url":"..."}
class CredentialStore {
public:
    struct Credentials {
        std::string username;
        std::string token;      // JWT
        std::string base_url;   // Server base URL (e.g. "http://localhost:8080")
    };

    /// Save credentials for the given username.
    /// If an entry already exists for this username, it is overwritten.
    static bool Save(const Credentials& creds);

    /// Load credentials for the given username.
    /// Returns nullopt if not found.
    static std::optional<Credentials> Load(const std::string& username);

    /// Delete stored credentials for the given username.
    /// Returns true if deleted or already absent (idempotent).
    static bool Delete(const std::string& username);

    /// Load the most recently saved credentials (any username).
    /// Used at startup to restore session. Returns nullopt if none stored.
    static std::optional<Credentials> LoadLast();

    /// Save the "last used username" for auto-restore at startup.
    static void SaveLastUsername(const std::string& username);

    /// Load the "last used username".
    static std::string LoadLastUsername();

private:
    static std::wstring MakeTargetName(const std::string& username);
    static constexpr wchar_t kLastUserTarget[] = L"stickytodo/__last_user__";
};

} // namespace stickytodo::core
