#include "ui/Preferences.h"

#include <windows.h>

namespace stickytodo::ui {

namespace {

// All preferences live under this single key so uninstallation / reset is a
// one-shot RegDeleteTree on HKCU\Software\stickytodo.
constexpr wchar_t kRootKey[] = L"Software\\stickytodo";
constexpr wchar_t kValueSkipTodoDelete[]   = L"skipTodoDeleteConfirm";
constexpr wchar_t kValueSkipStickyDelete[] = L"skipStickyDeleteConfirm";

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

} // namespace stickytodo::ui
