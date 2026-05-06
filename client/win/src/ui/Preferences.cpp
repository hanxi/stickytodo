#include "ui/Preferences.h"

#include <string>
#include <windows.h>

namespace stickytodo::ui {

namespace {

// All preferences live under this single key so uninstallation / reset is a
// one-shot RegDeleteTree on HKCU\Software\stickytodo.
constexpr wchar_t kRootKey[] = L"Software\\stickytodo";
constexpr wchar_t kValueSkipTodoDelete[]   = L"skipTodoDeleteConfirm";
constexpr wchar_t kValueSkipStickyDelete[] = L"skipStickyDeleteConfirm";
// HTTP proxy URL (REG_SZ). Mirrors macOS UserDefaults
// `stickytodo.httpProxy`. Empty string == direct connection.
constexpr wchar_t kValueHttpProxy[]        = L"httpProxy";

bool ReadDword(const wchar_t* valueName) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRootKey, 0, KEY_READ, &key)
            != ERROR_SUCCESS) {
        return false;
    }
    DWORD value = 0;
    DWORD type = 0;
    DWORD size = sizeof(value);
    LONG r = RegQueryValueExW(key, valueName, nullptr, &type,
                              reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);
    if (r != ERROR_SUCCESS || type != REG_DWORD) return false;
    return value != 0;
}

void WriteDword(const wchar_t* valueName, bool flag) {
    HKEY key = nullptr;
    // KEY_SET_VALUE is enough; KEY_WRITE also implies creating subkeys, which
    // we do not need since kRootKey is the whole tree we touch.
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRootKey, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr,
                        &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    DWORD value = flag ? 1u : 0u;
    RegSetValueExW(key, valueName, 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
}

// REG_SZ helpers — symmetric to ReadDword/WriteDword but for wide-string
// values. Used by GetHttpProxy/SetHttpProxy. Empty string is a valid value
// here (means "direct connection / no proxy"); missing key returns "" too,
// so callers cannot distinguish "never set" from "explicitly empty" — that
// is intentional: both should behave as "no proxy".
//
// IMPORTANT — naming: do NOT call these `ReadString` / `WriteString`. Both
// names are macro-defined by `<windows.h>` (the A/W console & GDI families
// expand `WriteString` → `WriteStringW` etc.), and the macro substitution
// happens BEFORE C++ name lookup, so a same-named function gets its
// signature silently rewritten and downstream `std::wstring` references in
// the same parameter list start failing with bogus "ambiguous wstring"
// errors. Prefix with `Reg` to dodge the macro entirely.
std::wstring ReadRegString(const wchar_t* valueName) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRootKey, 0, KEY_READ, &key)
            != ERROR_SUCCESS) {
        return L"";
    }
    DWORD type = 0;
    DWORD sizeBytes = 0;
    LONG r = RegQueryValueExW(key, valueName, nullptr, &type,
                              nullptr, &sizeBytes);
    if (r != ERROR_SUCCESS || type != REG_SZ || sizeBytes == 0) {
        RegCloseKey(key);
        return L"";
    }
    // sizeBytes includes the trailing null (REG_SZ convention).
    std::wstring buf(sizeBytes / sizeof(wchar_t), L'\0');
    r = RegQueryValueExW(key, valueName, nullptr, nullptr,
                         reinterpret_cast<LPBYTE>(buf.data()), &sizeBytes);
    RegCloseKey(key);
    if (r != ERROR_SUCCESS) return L"";
    // Strip any embedded trailing null(s) — RegQueryValueEx counts them in
    // sizeBytes but std::wstring should not contain them.
    while (!buf.empty() && buf.back() == L'\0') buf.pop_back();
    return buf;
}

void WriteRegString(const wchar_t* valueName, const std::wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRootKey, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr,
                        &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    // +1 for trailing null; REG_SZ MUST be null-terminated per Win32 contract.
    const DWORD bytes =
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    RegSetValueExW(key, valueName, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(value.c_str()), bytes);
    RegCloseKey(key);
}

} // namespace

bool ShouldSkipTodoDeleteConfirm() {
    return ReadDword(kValueSkipTodoDelete);
}

void SetSkipTodoDeleteConfirm(bool skip) {
    WriteDword(kValueSkipTodoDelete, skip);
}

bool ShouldSkipStickyDeleteConfirm() {
    return ReadDword(kValueSkipStickyDelete);
}

void SetSkipStickyDeleteConfirm(bool skip) {
    WriteDword(kValueSkipStickyDelete, skip);
}

std::wstring GetHttpProxy() {
    return ReadRegString(kValueHttpProxy);
}

void SetHttpProxy(const std::wstring& value) {
    WriteRegString(kValueHttpProxy, value);
}

} // namespace stickytodo::ui
