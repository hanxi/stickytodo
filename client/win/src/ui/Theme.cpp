#include "ui/Theme.h"

namespace stickytodo::ui {

Theme::Mode Theme::currentMode = Theme::Mode::Light;

D2D1_COLOR_F Theme::MakeColor(float r, float g, float b, float a) {
    return D2D1::ColorF(r, g, b, a);
}

D2D1_COLOR_F Theme::MakeColorHex(uint32_t hex, float a) {
    float r = ((hex >> 16) & 0xFF) / 255.0f;
    float g = ((hex >> 8) & 0xFF) / 255.0f;
    float b = (hex & 0xFF) / 255.0f;
    return D2D1::ColorF(r, g, b, a);
}

D2D1_COLOR_F Theme::WindowBackground() {
    return IsDark() ? MakeColorHex(0x1E1E1E) : MakeColorHex(0xFFFFFF);
}

D2D1_COLOR_F Theme::TextPrimary() {
    return IsDark() ? MakeColorHex(0xE0E0E0) : MakeColorHex(0x1A1A1A);
}

D2D1_COLOR_F Theme::TextSecondary() {
    return IsDark() ? MakeColorHex(0xA0A0A0) : MakeColorHex(0x666666);
}

D2D1_COLOR_F Theme::TextPlaceholder() {
    return IsDark() ? MakeColorHex(0x606060) : MakeColorHex(0xAAAAAA);
}

D2D1_COLOR_F Theme::ButtonNormal() {
    return IsDark() ? MakeColorHex(0x3A3A3A) : MakeColorHex(0xF0F0F0);
}

D2D1_COLOR_F Theme::ButtonHover() {
    return IsDark() ? MakeColorHex(0x4A4A4A) : MakeColorHex(0xE0E0E0);
}

D2D1_COLOR_F Theme::ButtonPressed() {
    return IsDark() ? MakeColorHex(0x2A2A2A) : MakeColorHex(0xD0D0D0);
}

D2D1_COLOR_F Theme::ButtonText() {
    return IsDark() ? MakeColorHex(0xE0E0E0) : MakeColorHex(0x1A1A1A);
}

D2D1_COLOR_F Theme::CheckboxBorder() {
    return IsDark() ? MakeColorHex(0x808080) : MakeColorHex(0x999999);
}

D2D1_COLOR_F Theme::CheckboxFill() {
    return MakeColorHex(0x0078D4); // Windows accent blue
}

D2D1_COLOR_F Theme::CheckboxCheck() {
    return MakeColorHex(0xFFFFFF);
}

D2D1_COLOR_F Theme::ScrollbarTrack() {
    return IsDark() ? MakeColor(1, 1, 1, 0.05f) : MakeColor(0, 0, 0, 0.05f);
}

D2D1_COLOR_F Theme::ScrollbarThumb() {
    return IsDark() ? MakeColor(1, 1, 1, 0.3f) : MakeColor(0, 0, 0, 0.2f);
}

D2D1_COLOR_F Theme::InputBorder() {
    return IsDark() ? MakeColorHex(0x555555) : MakeColorHex(0xCCCCCC);
}

D2D1_COLOR_F Theme::InputBackground() {
    return IsDark() ? MakeColorHex(0x2D2D2D) : MakeColorHex(0xFFFFFF);
}

D2D1_COLOR_F Theme::StickyTitleBar() {
    return MakeColor(0, 0, 0, 0.08f);
}

D2D1_COLOR_F Theme::TabActive() {
    return IsDark() ? MakeColorHex(0x3A3A3A) : MakeColorHex(0xFFFFFF);
}

D2D1_COLOR_F Theme::TabInactive() {
    return IsDark() ? MakeColorHex(0x252525) : MakeColorHex(0xF0F0F0);
}

D2D1_COLOR_F Theme::Separator() {
    return IsDark() ? MakeColor(1, 1, 1, 0.1f) : MakeColor(0, 0, 0, 0.1f);
}

D2D1_COLOR_F Theme::PriorityColor(int priority) {
    switch (priority) {
        case 1: return MakeColorHex(0x4CAF50); // Low - green
        case 2: return MakeColorHex(0xFF9800); // Medium - orange
        case 3: return MakeColorHex(0xF44336); // High - red
        default: return MakeColor(0, 0, 0, 0); // None - transparent
    }
}

} // namespace stickytodo::ui
