#include "core/UIThreadMarshal.h"

#include <utility>

namespace stickytodo::core {

bool PostToUIThread(HWND target, std::function<void()> fn) {
    if (!target) {
        // No UI target registered (or app is shutting down). Dropping
        // the callable is the correct behaviour — running it now on the
        // calling (worker) thread would violate the marshal contract.
        // See the header's block comment for the full rationale.
        return false;
    }
    auto* heap = new std::function<void()>(std::move(fn));
    if (!::PostMessageW(target, WM_STICKYTODO_RUN_ON_UI,
                        reinterpret_cast<WPARAM>(heap), 0)) {
        // Race against tray window destruction during shutdown. Free
        // the allocation we just made — nobody else will.
        delete heap;
        return false;
    }
    return true;
}

} // namespace stickytodo::core
