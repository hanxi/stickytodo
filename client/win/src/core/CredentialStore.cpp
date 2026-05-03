#include "core/CredentialStore.h"

#include <windows.h>
#include <wincred.h>
#include <nlohmann/json.hpp>

// advapi32.lib already linked via CMakeLists target_link_libraries — the
// #pragma comment is redundant under that build but harmless (and kept as
// a belt-and-braces hint for anyone opening this TU in a standalone VS
// project). We explicitly do NOT pull in <codecvt> / <locale> even though
// an earlier revision did: all wide ↔ UTF-8 conversion goes through the
// MultiByteToWideChar / WideCharToMultiByte pair below, which is faster,
// more correct (preserves surrogate pairs), and doesn't drag in a
// deprecated standard header.

#pragma comment(lib, "advapi32.lib")

namespace stickytodo::core {

static std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int sz = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), nullptr, 0);
    std::wstring result(sz, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), result.data(), sz);
    return result;
}

static std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int sz = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    std::string result(sz, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), result.data(), sz, nullptr, nullptr);
    return result;
}

std::wstring CredentialStore::MakeTargetName(const std::string& username) {
    return L"stickytodo/" + Utf8ToWide(username);
}

bool CredentialStore::Save(const Credentials& creds) {
    // Serialize token + base_url as JSON blob
    nlohmann::json j;
    j["token"] = creds.token;
    j["base_url"] = creds.base_url;
    std::string blob = j.dump();

    std::wstring target = MakeTargetName(creds.username);
    std::wstring userName = Utf8ToWide(creds.username);

    CREDENTIALW credential = {};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(target.c_str());
    credential.UserName = const_cast<LPWSTR>(userName.c_str());
    credential.CredentialBlobSize = static_cast<DWORD>(blob.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(blob.data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;

    BOOL ok = CredWriteW(&credential, 0);
    if (ok) {
        // Also save last username
        SaveLastUsername(creds.username);
    }
    return ok != FALSE;
}

std::optional<CredentialStore::Credentials> CredentialStore::Load(const std::string& username) {
    std::wstring target = MakeTargetName(username);

    PCREDENTIALW pCred = nullptr;
    BOOL ok = CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &pCred);
    if (!ok || !pCred) {
        return std::nullopt;
    }

    Credentials creds;
    creds.username = username;

    if (pCred->CredentialBlobSize > 0 && pCred->CredentialBlob) {
        std::string blob(reinterpret_cast<char*>(pCred->CredentialBlob), pCred->CredentialBlobSize);
        try {
            auto j = nlohmann::json::parse(blob);
            if (j.contains("token") && j["token"].is_string()) {
                creds.token = j["token"].get<std::string>();
            }
            if (j.contains("base_url") && j["base_url"].is_string()) {
                creds.base_url = j["base_url"].get<std::string>();
            }
        } catch (...) {
            // Malformed blob — treat as not found
            CredFree(pCred);
            return std::nullopt;
        }
    }

    CredFree(pCred);

    if (creds.token.empty()) {
        return std::nullopt;
    }
    return creds;
}

bool CredentialStore::Delete(const std::string& username) {
    std::wstring target = MakeTargetName(username);
    BOOL ok = CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0);
    // Treat ERROR_NOT_FOUND as success (idempotent)
    return ok != FALSE || GetLastError() == ERROR_NOT_FOUND;
}

std::optional<CredentialStore::Credentials> CredentialStore::LoadLast() {
    std::string lastUser = LoadLastUsername();
    if (lastUser.empty()) {
        return std::nullopt;
    }
    return Load(lastUser);
}

void CredentialStore::SaveLastUsername(const std::string& username) {
    std::wstring userName = Utf8ToWide(username);

    CREDENTIALW credential = {};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(kLastUserTarget);
    credential.UserName = const_cast<LPWSTR>(userName.c_str());
    credential.CredentialBlobSize = static_cast<DWORD>(username.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(username.data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;

    CredWriteW(&credential, 0);
}

std::string CredentialStore::LoadLastUsername() {
    PCREDENTIALW pCred = nullptr;
    BOOL ok = CredReadW(kLastUserTarget, CRED_TYPE_GENERIC, 0, &pCred);
    if (!ok || !pCred) {
        return "";
    }

    std::string result;
    if (pCred->CredentialBlobSize > 0 && pCred->CredentialBlob) {
        result.assign(reinterpret_cast<char*>(pCred->CredentialBlob), pCred->CredentialBlobSize);
    }
    CredFree(pCred);
    return result;
}

} // namespace stickytodo::core
