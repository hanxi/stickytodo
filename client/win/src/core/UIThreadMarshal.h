#pragma once

#include <windows.h>
#include <functional>

namespace stickytodo::core {

/// Generic "run this callable on the UI thread" marshalling primitive.
///
/// Win32 equivalent of macOS's `DispatchQueue.main.async { … }`. Any
/// background thread (HTTP async worker, WebSocket worker, arbitrary
/// std::thread) that produces a result destined for UI mutation must
/// funnel the mutation through this helper so that HWNDs, Direct2D
/// render targets, control focus, `std::vector<StickyNote>` members
/// etc. are only ever touched from the single UI thread that owns them.
///
/// Implementation (see UIThreadMarshal.cpp):
///   1. Heap-allocate a std::function<void()> copy of the caller's
///      callable.
///   2. PostMessageW(target, WM_STICKYTODO_RUN_ON_UI,
///                   reinterpret_cast<WPARAM>(ptr), 0).
///   3. The receiving WndProc (TrayIcon::TrayWndProc, which runs on the
///      UI thread) invokes the function exactly once and deletes the
///      heap allocation — see its WM_STICKYTODO_RUN_ON_UI branch.
///
/// Behaviour when `target` is null OR PostMessageW fails (target HWND
/// just destroyed during shutdown): the heap allocation is freed
/// immediately and the callable is **dropped** — NOT run inline on
/// the calling thread. Dropping is the correct behaviour because the
/// only reason the target disappears is that the UI that wanted to
/// observe the result is gone (app shutdown / tray destroyed), so
/// there is nothing left to update; running on the worker thread
/// would break the UI-thread-only invariant of whatever the callable
/// is trying to touch.
///
/// Thread-safety: safe to call from any thread. PostMessageW is
/// explicitly documented as safe for cross-thread delivery. The
/// target HWND is typically published once by App::Initialize on the
/// UI thread before any worker thread is spawned, and never
/// reassigned; plain read access from workers is fine for that
/// publish-before-use pattern.
bool PostToUIThread(HWND target, std::function<void()> fn);

/// Windows message consumed by TrayIcon::TrayWndProc to deliver
/// PostToUIThread payloads. wParam is a heap-allocated
/// std::function<void()>* that the receiver owns (invoke, then delete).
/// Exposed here so TrayIcon's WndProc (a different translation unit)
/// can name the constant without pulling in AppState.h.
///
/// Value chosen as WM_APP + 6, staying in the block that
/// AppState.h reserves for WS event marshalling (WM_APP + 1..5). See
/// AppState.h's block comment above the WM_STICKYTODO_* constants for
/// the full numeric assignment map.
constexpr UINT WM_STICKYTODO_RUN_ON_UI = WM_APP + 6;

} // namespace stickytodo::core
