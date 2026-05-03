#include "core/Timer.h"

namespace stickytodo::core {

bool Timer::classRegistered_ = false;

Timer::Timer() {
    CreateMessageWindow();
}

Timer::~Timer() {
    Stop();
    DestroyMessageWindow();
}

void Timer::Start(uint32_t intervalMs, Callback callback) {
    Stop();
    callback_ = std::move(callback);
    oneShot_ = false;
    if (hwnd_) {
        timerId_ = SetTimer(hwnd_, kTimerId, intervalMs, nullptr);
    }
}

void Timer::StartOnce(uint32_t delayMs, Callback callback) {
    Stop();
    callback_ = std::move(callback);
    oneShot_ = true;
    if (hwnd_) {
        timerId_ = SetTimer(hwnd_, kTimerId, delayMs, nullptr);
    }
}

void Timer::Stop() {
    if (timerId_ != 0 && hwnd_) {
        KillTimer(hwnd_, kTimerId);
        timerId_ = 0;
    }
    callback_ = nullptr;
}

void Timer::CreateMessageWindow() {
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    if (!classRegistered_) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.lpfnWndProc = TimerWndProc;
        wc.hInstance = hInst;
        wc.lpszClassName = kWindowClassName;
        RegisterClassExW(&wc);
        classRegistered_ = true;
    }

    // HWND_MESSAGE creates a message-only window (invisible, no Z-order).
    hwnd_ = CreateWindowExW(
        0, kWindowClassName, L"", 0,
        0, 0, 0, 0,
        HWND_MESSAGE, nullptr, hInst, this
    );
}

void Timer::DestroyMessageWindow() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

LRESULT CALLBACK Timer::TimerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_CREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return 0;
    }

    auto* self = reinterpret_cast<Timer*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    if (msg == WM_TIMER && wParam == kTimerId) {
        if (self->oneShot_) {
            KillTimer(hwnd, kTimerId);
            self->timerId_ = 0;
        }
        if (self->callback_) {
            self->callback_();
        }
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace stickytodo::core
