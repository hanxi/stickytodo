#pragma once

#include <windows.h>
#include <functional>
#include <cstdint>

namespace stickytodo::core {

/// Simple wrapper around Win32 SetTimer / KillTimer.
/// Each Timer instance is associated with a hidden message-only window
/// to receive WM_TIMER messages on the main thread.
class Timer {
public:
    using Callback = std::function<void()>;

    Timer();
    ~Timer();

    /// Start a repeating timer with the given interval in milliseconds.
    /// If already running, the previous timer is stopped first.
    void Start(uint32_t intervalMs, Callback callback);

    /// Start a one-shot timer. After firing once, the timer stops automatically.
    void StartOnce(uint32_t delayMs, Callback callback);

    /// Stop the timer if running.
    void Stop();

    /// Returns true if the timer is currently active.
    bool IsRunning() const { return timerId_ != 0; }

private:
    static LRESULT CALLBACK TimerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void CreateMessageWindow();
    void DestroyMessageWindow();

    HWND hwnd_ = nullptr;
    UINT_PTR timerId_ = 0;
    Callback callback_;
    bool oneShot_ = false;

    static constexpr UINT_PTR kTimerId = 1;
    static constexpr wchar_t kWindowClassName[] = L"StickyTodo_TimerWnd";
    static bool classRegistered_;
};

} // namespace stickytodo::core
