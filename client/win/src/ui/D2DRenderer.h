#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>

namespace stickytodo::ui {

/// Global Direct2D / DirectWrite resource manager.
/// Creates shared factories used by all windows.
/// Each window creates its own ID2D1HwndRenderTarget from these factories.
class D2DRenderer {
public:
    D2DRenderer();
    ~D2DRenderer();

    /// Initialize D2D and DWrite factories. Returns false on failure.
    bool Initialize();

    /// Get the D2D factory (shared, thread-safe for single-threaded use).
    ID2D1Factory1* GetD2DFactory() const { return d2dFactory_.Get(); }

    /// Get the DirectWrite factory.
    IDWriteFactory* GetDWriteFactory() const { return dwriteFactory_.Get(); }

    /// Create a render target for a specific window.
    /// Caller owns the returned pointer (Release when done or window resizes).
    ID2D1HwndRenderTarget* CreateRenderTarget(HWND hwnd);

    /// Get DPI scale factor for a given window.
    static float GetDpiScale(HWND hwnd);

private:
    Microsoft::WRL::ComPtr<ID2D1Factory1> d2dFactory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwriteFactory_;
};

} // namespace stickytodo::ui
