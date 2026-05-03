#include "ui/D2DRenderer.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace stickytodo::ui {

D2DRenderer::D2DRenderer() = default;
D2DRenderer::~D2DRenderer() = default;

bool D2DRenderer::Initialize() {
    // Create D2D Factory (single-threaded — all UI is on main thread)
    D2D1_FACTORY_OPTIONS options = {};
#ifdef _DEBUG
    options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#else
    options.debugLevel = D2D1_DEBUG_LEVEL_NONE;
#endif

    HRESULT hr = D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory1),
        &options,
        reinterpret_cast<void**>(d2dFactory_.GetAddressOf())
    );
    if (FAILED(hr)) return false;

    // Create DirectWrite Factory
    hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf())
    );
    if (FAILED(hr)) return false;

    return true;
}

ID2D1HwndRenderTarget* D2DRenderer::CreateRenderTarget(HWND hwnd) {
    if (!d2dFactory_) return nullptr;

    RECT rc;
    GetClientRect(hwnd, &rc);

    D2D1_SIZE_U size = D2D1::SizeU(
        static_cast<UINT32>(rc.right - rc.left),
        static_cast<UINT32>(rc.bottom - rc.top)
    );

    // Use default DPI (96) — we handle DPI scaling in drawing code
    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties();
    D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProps = D2D1::HwndRenderTargetProperties(hwnd, size);

    ID2D1HwndRenderTarget* renderTarget = nullptr;
    HRESULT hr = d2dFactory_->CreateHwndRenderTarget(rtProps, hwndProps, &renderTarget);
    if (FAILED(hr)) return nullptr;

    return renderTarget;
}

float D2DRenderer::GetDpiScale(HWND hwnd) {
    UINT dpi = GetDpiForWindow(hwnd);
    if (dpi == 0) dpi = 96;
    return static_cast<float>(dpi) / 96.0f;
}

} // namespace stickytodo::ui
