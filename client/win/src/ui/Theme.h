#pragma once

#include <d2d1.h>
#include <cstdint>

namespace stickytodo::ui {

/// Color constants and layout metrics for the application UI.
/// Supports light and dark modes.
struct Theme {
    // ---------- Mode ----------
    enum class Mode { Light, Dark };
    static Mode currentMode;
    static void SetMode(Mode mode) { currentMode = mode; }
    static bool IsDark() { return currentMode == Mode::Dark; }

    // ---------- Colors (as D2D1_COLOR_F) ----------
    static D2D1_COLOR_F WindowBackground();
    static D2D1_COLOR_F TextPrimary();
    static D2D1_COLOR_F TextSecondary();
    static D2D1_COLOR_F TextPlaceholder();
    static D2D1_COLOR_F ButtonNormal();
    static D2D1_COLOR_F ButtonHover();
    static D2D1_COLOR_F ButtonPressed();
    static D2D1_COLOR_F ButtonText();
    static D2D1_COLOR_F CheckboxBorder();
    static D2D1_COLOR_F CheckboxFill();
    static D2D1_COLOR_F CheckboxCheck();
    static D2D1_COLOR_F ScrollbarTrack();
    static D2D1_COLOR_F ScrollbarThumb();
    static D2D1_COLOR_F InputBorder();
    static D2D1_COLOR_F InputBackground();
    static D2D1_COLOR_F StickyTitleBar();
    static D2D1_COLOR_F TabActive();
    static D2D1_COLOR_F TabInactive();
    static D2D1_COLOR_F Separator();

    // ---------- Priority colors ----------
    static D2D1_COLOR_F PriorityColor(int priority);

    // ---------- Preset sticky colors (aligned with macOS presetColors) ----------
    struct PresetColor {
        const char* name;
        float r, g, b;
    };
    static constexpr PresetColor kPresetColors[] = {
        {"Sticky Yellow", 1.00f, 0.92f, 0.54f},
        {"Mint Green",    0.78f, 0.95f, 0.83f},
        {"Sky Blue",      0.80f, 0.90f, 0.98f},
        {"Sakura Pink",   0.99f, 0.83f, 0.88f},
        {"Lavender",      0.88f, 0.82f, 0.98f},
    };
    static constexpr int kPresetColorCount = 5;

    // ---------- Font sizes (in DIP, pre-DPI-scaling) ----------
    static constexpr float kFontSizeTitle = 14.0f;
    static constexpr float kFontSizeBody = 13.0f;
    static constexpr float kFontSizeSmall = 11.0f;
    static constexpr float kFontSizeHeading = 16.0f;

    // ---------- Layout metrics ----------
    static constexpr float kPadding = 8.0f;
    static constexpr float kPaddingLarge = 16.0f;
    static constexpr float kCornerRadius = 6.0f;
    static constexpr float kTitleBarHeight = 32.0f;
    static constexpr float kButtonHeight = 28.0f;
    static constexpr float kInputHeight = 28.0f;
    static constexpr float kCheckboxSize = 16.0f;
    static constexpr float kScrollbarWidth = 8.0f;
    static constexpr float kTabHeight = 32.0f;
    static constexpr float kTodoRowHeight = 36.0f;
    static constexpr float kPriorityBarWidth = 4.0f;

    // ---------- Default window sizes (in pixels, pre-DPI) ----------
    static constexpr int kStickyDefaultWidth = 300;
    static constexpr int kStickyDefaultHeight = 420;
    static constexpr int kSettingsWidth = 520;
    static constexpr int kSettingsHeight = 420;

    // ---------- Helper ----------
    static D2D1_COLOR_F MakeColor(float r, float g, float b, float a = 1.0f);
    static D2D1_COLOR_F MakeColorHex(uint32_t hex, float a = 1.0f);
};

} // namespace stickytodo::ui
