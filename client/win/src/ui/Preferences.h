#pragma once

#include <string>

// User preference storage backed by HKCU\Software\stickytodo. Mirrors the
// macOS client's @AppStorage-driven preferences 1:1:
//
//   sticky.skipDeleteConfirm   ↔  HKCU\Software\stickytodo\skipStickyDeleteConfirm
//   todo.skipDeleteConfirm     ↔  HKCU\Software\stickytodo\skipTodoDeleteConfirm
//
// Stored as REG_DWORD (0/1). HKCU is used so no admin rights are required
// and the values are scoped to the current user, consistent with macOS
// UserDefaults.standard semantics.
//
// The consumer flow mirrors macOS:
//   - StickyWindow / DoDelete reads ShouldSkipTodoDeleteConfirm before
//     showing the confirmation dialog; the dialog's "delete and don't ask
//     again" button writes back true via SetSkipTodoDeleteConfirm.
//   - StickyWindow / the trash button on the title bar reads
//     ShouldSkipStickyDeleteConfirm before showing the confirmation dialog;
//     the dialog's "delete and don't ask again" button writes back true via
//     SetSkipStickyDeleteConfirm.
//   - SettingsWindow renders two CheckBox controls bound to these four
//     functions so the user can re-enable the confirmations they previously
//     silenced. The CheckBox's "show confirm" semantic is the *inverse* of
//     the stored "skip" flag, matching the macOS UI (Toggle: 开=弹提示).

namespace stickytodo::ui {

/// Returns true if the user has previously chosen to suppress the TODO
/// soft-delete confirmation dialog. Defaults to false on a fresh install
/// or if the registry value is missing / malformed.
bool ShouldSkipTodoDeleteConfirm();

/// Persist the TODO soft-delete confirmation preference.
/// Pass true to suppress future dialogs; false to re-enable them.
void SetSkipTodoDeleteConfirm(bool skip);

/// Returns true if the user has previously chosen to suppress the sticky-note
/// delete confirmation dialog. Defaults to false.
bool ShouldSkipStickyDeleteConfirm();

/// Persist the sticky-note delete confirmation preference.
void SetSkipStickyDeleteConfirm(bool skip);

/// Returns the persisted HTTP proxy URL (e.g. `http://127.0.0.1:7890`).
/// Empty string means "direct connection / no proxy" — callers must treat
/// the empty case explicitly because WinHTTP's NAMED_PROXY mode rejects
/// empty proxy strings.
///
/// Stored in registry as REG_SZ under HKCU\Software\stickytodo\httpProxy
/// to mirror macOS UserDefaults `stickytodo.httpProxy`.
std::wstring GetHttpProxy();

/// Persist the HTTP proxy URL. Pass empty string to clear (direct connection).
/// HttpClient/WebSocketClient pick up the new value on their next request /
/// reconnect cycle — caller is responsible for triggering a reconnect via
/// AppState::SetHttpProxy.
void SetHttpProxy(const std::wstring& value);

} // namespace stickytodo::ui
