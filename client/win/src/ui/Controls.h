#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <functional>

namespace stickytodo::ui {

/// Hit-test result for controls within a window.
struct HitResult {
    bool hit = false;
    int controlId = -1;
};

/// Base rectangle for control layout.
struct ControlRect {
    float x = 0, y = 0, width = 0, height = 0;
    bool Contains(float px, float py) const {
        return px >= x && px < x + width && py >= y && py < y + height;
    }
    D2D1_RECT_F ToD2D() const {
        return D2D1::RectF(x, y, x + width, y + height);
    }
};

/// Button control state.
enum class ButtonState { Normal, Hover, Pressed };

/// Self-drawn button. Renders a rounded rectangle with centered text.
/// Supports Normal/Hover/Pressed states with corresponding theme colors.
///
/// The `selected` flag is an *independent* visual indicator (orthogonal to
/// `state`): used by segmented pickers / tab rows to show "this is the
/// currently chosen option" without interfering with the transient
/// Normal→Hover→Pressed state machine driven by mouse events. When
/// `selected` is true and `state` is Normal, the button renders with the
/// Pressed background so the user sees a persistent selection indicator
/// even after the mouse has moved elsewhere. Hover / Pressed states still
/// override `selected` so the user gets the usual feedback while actively
/// clicking.
struct Button {
    ControlRect rect;
    std::wstring text;
    ButtonState state = ButtonState::Normal;
    bool enabled = true;
    bool selected = false;
    std::function<void()> onClick;

    void Draw(ID2D1RenderTarget* rt, IDWriteFactory* dw, float dpi) const;
    bool HandleMouse(UINT msg, float mx, float my);
};

/// Single-line text input control with cursor, selection, and clipboard support.
struct TextBox {
    ControlRect rect;
    std::wstring text;
    std::wstring placeholder;
    bool isPassword = false;
    bool focused = false;
    bool enabled = true;
    int cursorPos = 0;
    int selStart = -1;
    int selEnd = -1;
    std::function<void(const std::wstring&)> onChanged;
    std::function<void()> onSubmit; // Enter key

    void Draw(ID2D1RenderTarget* rt, IDWriteFactory* dw, float dpi) const;
    bool HandleMouse(UINT msg, float mx, float my);
    bool HandleKey(UINT msg, WPARAM wParam, LPARAM lParam);
    bool HandleChar(wchar_t ch);
    void SetFocus(bool f);

private:
    void DeleteSelection();
    std::wstring GetDisplayText() const;
};

/// Checkbox control (square box + optional checkmark).
struct CheckBox {
    ControlRect rect;
    std::wstring label;
    bool checked = false;
    bool enabled = true;
    bool hovered = false;
    std::function<void(bool)> onToggle;

    void Draw(ID2D1RenderTarget* rt, IDWriteFactory* dw, float dpi) const;
    bool HandleMouse(UINT msg, float mx, float my);
};

/// Vertical scroll view container. Handles scroll offset and mouse wheel.
struct ScrollView {
    ControlRect rect;
    float contentHeight = 0;    // Total height of scrollable content
    float scrollOffset = 0;     // Current scroll position (pixels from top)
    bool thumbDragging = false;
    float dragStartY = 0;
    float dragStartOffset = 0;

    /// Get the visible content area (clipped).
    D2D1_RECT_F GetContentClipRect() const;

    /// Get maximum scroll offset.
    float MaxScroll() const;

    /// Handle mouse wheel. Returns true if consumed.
    bool HandleWheel(float delta);

    /// Handle mouse events on the scrollbar thumb.
    bool HandleMouse(UINT msg, float mx, float my);

    /// Draw the scrollbar track and thumb.
    void DrawScrollbar(ID2D1RenderTarget* rt) const;

    /// Apply scroll transform before drawing content.
    void BeginContent(ID2D1RenderTarget* rt) const;

    /// Restore transform after drawing content.
    void EndContent(ID2D1RenderTarget* rt) const;
};

/// Multi-line label with DirectWrite text layout for automatic line wrapping.
struct Label {
    ControlRect rect;
    std::wstring text;
    float fontSize = 13.0f;
    bool bold = false;
    D2D1_COLOR_F color = D2D1::ColorF(0, 0, 0, 1);

    void Draw(ID2D1RenderTarget* rt, IDWriteFactory* dw, float dpi) const;
};

} // namespace stickytodo::ui
