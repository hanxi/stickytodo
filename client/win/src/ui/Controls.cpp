#include "ui/Controls.h"
#include "ui/Theme.h"

#include <algorithm>
#include <cmath>

namespace stickytodo::ui {

// ---------- Button ----------

void Button::Draw(ID2D1RenderTarget* rt, IDWriteFactory* dw, float dpi) const {
    D2D1_COLOR_F bgColor;
    switch (state) {
        case ButtonState::Hover: bgColor = Theme::ButtonHover(); break;
        case ButtonState::Pressed: bgColor = Theme::ButtonPressed(); break;
        default:
            // In Normal state, let `selected` surface through so segmented
            // pickers / tabs keep their persistent selection indicator.
            bgColor = selected ? Theme::ButtonPressed() : Theme::ButtonNormal();
            break;
    }
    if (!enabled) {
        bgColor.a *= 0.5f;
    }

    ID2D1SolidColorBrush* brush = nullptr;
    rt->CreateSolidColorBrush(bgColor, &brush);
    if (brush) {
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(rect.ToD2D(), Theme::kCornerRadius, Theme::kCornerRadius);
        rt->FillRoundedRectangle(rr, brush);
        brush->Release();
    }

    // Draw text
    IDWriteTextFormat* format = nullptr;
    dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_REGULAR,
                          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                          Theme::kFontSizeBody * dpi, L"en-us", &format);
    if (format) {
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        D2D1_COLOR_F textColor = Theme::ButtonText();
        if (!enabled) textColor.a *= 0.5f;

        ID2D1SolidColorBrush* textBrush = nullptr;
        rt->CreateSolidColorBrush(textColor, &textBrush);
        if (textBrush) {
            rt->DrawText(text.c_str(), static_cast<UINT32>(text.size()),
                          format, rect.ToD2D(), textBrush);
            textBrush->Release();
        }
        format->Release();
    }
}

bool Button::HandleMouse(UINT msg, float mx, float my) {
    if (!enabled) return false;
    bool inside = rect.Contains(mx, my);

    switch (msg) {
        case WM_MOUSEMOVE:
            state = inside ? ButtonState::Hover : ButtonState::Normal;
            return inside;
        case WM_LBUTTONDOWN:
            if (inside) { state = ButtonState::Pressed; return true; }
            return false;
        case WM_LBUTTONUP:
            if (inside && state == ButtonState::Pressed) {
                state = ButtonState::Hover;
                if (onClick) onClick();
                return true;
            }
            state = ButtonState::Normal;
            return false;
        default:
            return false;
    }
}

// ---------- TextBox ----------

std::wstring TextBox::GetDisplayText() const {
    if (isPassword) {
        return std::wstring(text.size(), L'\u2022'); // bullet character
    }
    return text;
}

void TextBox::Draw(ID2D1RenderTarget* rt, IDWriteFactory* dw, float dpi) const {
    // Background
    ID2D1SolidColorBrush* bgBrush = nullptr;
    rt->CreateSolidColorBrush(Theme::InputBackground(), &bgBrush);
    if (bgBrush) {
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(rect.ToD2D(), 4.0f, 4.0f);
        rt->FillRoundedRectangle(rr, bgBrush);
        bgBrush->Release();
    }

    // Border
    ID2D1SolidColorBrush* borderBrush = nullptr;
    D2D1_COLOR_F borderColor = focused ? Theme::CheckboxFill() : Theme::InputBorder();
    rt->CreateSolidColorBrush(borderColor, &borderBrush);
    if (borderBrush) {
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(rect.ToD2D(), 4.0f, 4.0f);
        rt->DrawRoundedRectangle(rr, borderBrush, 1.0f);
        borderBrush->Release();
    }

    // Text
    std::wstring displayText = GetDisplayText();
    bool showPlaceholder = displayText.empty() && !placeholder.empty() && !focused;
    const std::wstring& drawText = showPlaceholder ? placeholder : displayText;
    D2D1_COLOR_F textColor = showPlaceholder ? Theme::TextPlaceholder() : Theme::TextPrimary();

    IDWriteTextFormat* format = nullptr;
    dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_REGULAR,
                          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                          Theme::kFontSizeBody * dpi, L"en-us", &format);
    if (format) {
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        D2D1_RECT_F textRect = rect.ToD2D();
        textRect.left += 8.0f;
        textRect.right -= 8.0f;

        ID2D1SolidColorBrush* textBrush = nullptr;
        rt->CreateSolidColorBrush(textColor, &textBrush);
        if (textBrush) {
            rt->DrawText(drawText.c_str(), static_cast<UINT32>(drawText.size()),
                          format, textRect, textBrush);
            textBrush->Release();
        }
        format->Release();
    }
}

bool TextBox::HandleMouse(UINT msg, float mx, float my) {
    if (!enabled) return false;
    if (msg == WM_LBUTTONDOWN) {
        bool inside = rect.Contains(mx, my);
        SetFocus(inside);
        return inside;
    }
    return false;
}

bool TextBox::HandleKey(UINT msg, WPARAM wParam, LPARAM /*lParam*/) {
    if (!focused || !enabled) return false;
    if (msg != WM_KEYDOWN) return false;

    switch (wParam) {
        case VK_LEFT:
            if (cursorPos > 0) cursorPos--;
            return true;
        case VK_RIGHT:
            if (cursorPos < static_cast<int>(text.size())) cursorPos++;
            return true;
        case VK_HOME:
            cursorPos = 0;
            return true;
        case VK_END:
            cursorPos = static_cast<int>(text.size());
            return true;
        case VK_BACK:
            if (selStart >= 0 && selStart != selEnd) {
                DeleteSelection();
            } else if (cursorPos > 0) {
                text.erase(cursorPos - 1, 1);
                cursorPos--;
            }
            if (onChanged) onChanged(text);
            return true;
        case VK_DELETE:
            if (selStart >= 0 && selStart != selEnd) {
                DeleteSelection();
            } else if (cursorPos < static_cast<int>(text.size())) {
                text.erase(cursorPos, 1);
            }
            if (onChanged) onChanged(text);
            return true;
        case VK_RETURN:
            if (onSubmit) onSubmit();
            return true;
        case 'A':
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                selStart = 0;
                selEnd = static_cast<int>(text.size());
                cursorPos = selEnd;
                return true;
            }
            return false;
        default:
            return false;
    }
}

bool TextBox::HandleChar(wchar_t ch) {
    if (!focused || !enabled) return false;
    if (ch < 32 && ch != L'\t') return false; // Control chars except tab

    if (selStart >= 0 && selStart != selEnd) {
        DeleteSelection();
    }

    text.insert(text.begin() + cursorPos, ch);
    cursorPos++;
    selStart = selEnd = -1;

    if (onChanged) onChanged(text);
    return true;
}

void TextBox::SetFocus(bool f) {
    focused = f;
    if (!f) {
        selStart = selEnd = -1;
    }
}

void TextBox::DeleteSelection() {
    if (selStart < 0 || selStart == selEnd) return;
    int lo = std::min(selStart, selEnd);
    int hi = std::max(selStart, selEnd);
    text.erase(lo, hi - lo);
    cursorPos = lo;
    selStart = selEnd = -1;
}

// ---------- CheckBox ----------

void CheckBox::Draw(ID2D1RenderTarget* rt, IDWriteFactory* dw, float dpi) const {
    // When disabled we halve alpha on every colour we render so the whole
    // checkbox (box + checkmark + label) visibly greys out. Mirrors the
    // macOS `.disabled(true)` rendering and matches the existing pattern
    // in Button::Draw above (`if (!enabled) bgColor.a *= 0.5f`).
    const float alphaScale = enabled ? 1.0f : 0.5f;

    float boxSize = Theme::kCheckboxSize;
    float boxY = rect.y + (rect.height - boxSize) / 2.0f;

    D2D1_RECT_F boxRect = D2D1::RectF(rect.x, boxY, rect.x + boxSize, boxY + boxSize);

    if (checked) {
        // Filled box
        ID2D1SolidColorBrush* fillBrush = nullptr;
        D2D1_COLOR_F fillColor = Theme::CheckboxFill();
        fillColor.a *= alphaScale;
        rt->CreateSolidColorBrush(fillColor, &fillBrush);
        if (fillBrush) {
            D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(boxRect, 3.0f, 3.0f);
            rt->FillRoundedRectangle(rr, fillBrush);
            fillBrush->Release();
        }
        // Checkmark (simple line)
        ID2D1SolidColorBrush* checkBrush = nullptr;
        D2D1_COLOR_F checkColor = Theme::CheckboxCheck();
        checkColor.a *= alphaScale;
        rt->CreateSolidColorBrush(checkColor, &checkBrush);
        if (checkBrush) {
            float cx = rect.x + boxSize * 0.5f;
            float cy = boxY + boxSize * 0.5f;
            float s = boxSize * 0.3f;
            D2D1_POINT_2F p1 = D2D1::Point2F(cx - s * 0.6f, cy);
            D2D1_POINT_2F p2 = D2D1::Point2F(cx - s * 0.1f, cy + s * 0.5f);
            D2D1_POINT_2F p3 = D2D1::Point2F(cx + s * 0.7f, cy - s * 0.4f);
            rt->DrawLine(p1, p2, checkBrush, 2.0f);
            rt->DrawLine(p2, p3, checkBrush, 2.0f);
            checkBrush->Release();
        }
    } else {
        // Border only
        ID2D1SolidColorBrush* borderBrush = nullptr;
        D2D1_COLOR_F borderColor = Theme::CheckboxBorder();
        borderColor.a *= alphaScale;
        rt->CreateSolidColorBrush(borderColor, &borderBrush);
        if (borderBrush) {
            D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(boxRect, 3.0f, 3.0f);
            rt->DrawRoundedRectangle(rr, borderBrush, 1.5f);
            borderBrush->Release();
        }
    }

    // Label text
    if (!label.empty()) {
        IDWriteTextFormat* format = nullptr;
        dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_REGULAR,
                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                              Theme::kFontSizeBody * dpi, L"en-us", &format);
        if (format) {
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            D2D1_RECT_F labelRect = D2D1::RectF(
                rect.x + boxSize + 6.0f, rect.y,
                rect.x + rect.width, rect.y + rect.height
            );
            ID2D1SolidColorBrush* textBrush = nullptr;
            D2D1_COLOR_F labelColor = Theme::TextPrimary();
            labelColor.a *= alphaScale;
            rt->CreateSolidColorBrush(labelColor, &textBrush);
            if (textBrush) {
                rt->DrawText(label.c_str(), static_cast<UINT32>(label.size()),
                              format, labelRect, textBrush);
                textBrush->Release();
            }
            format->Release();
        }
    }
}

bool CheckBox::HandleMouse(UINT msg, float mx, float my) {
    if (!enabled) return false;
    bool inside = rect.Contains(mx, my);

    if (msg == WM_MOUSEMOVE) {
        hovered = inside;
        return inside;
    }
    if (msg == WM_LBUTTONUP && inside) {
        checked = !checked;
        if (onToggle) onToggle(checked);
        return true;
    }
    return false;
}

// ---------- ScrollView ----------

D2D1_RECT_F ScrollView::GetContentClipRect() const {
    return D2D1::RectF(rect.x, rect.y, rect.x + rect.width - Theme::kScrollbarWidth, rect.y + rect.height);
}

float ScrollView::MaxScroll() const {
    float max = contentHeight - rect.height;
    return max > 0 ? max : 0;
}

bool ScrollView::HandleWheel(float delta) {
    float newOffset = scrollOffset - delta * 40.0f;
    newOffset = std::clamp(newOffset, 0.0f, MaxScroll());
    if (newOffset != scrollOffset) {
        scrollOffset = newOffset;
        return true;
    }
    return false;
}

bool ScrollView::HandleMouse(UINT msg, float mx, float my) {
    // Scrollbar is on the right edge of the control
    float sbX = rect.x + rect.width - Theme::kScrollbarWidth;
    bool inScrollbar = mx >= sbX && mx < rect.x + rect.width && my >= rect.y && my < rect.y + rect.height;

    if (msg == WM_LBUTTONDOWN && inScrollbar) {
        thumbDragging = true;
        dragStartY = my;
        dragStartOffset = scrollOffset;
        return true;
    }
    if (msg == WM_LBUTTONUP) {
        thumbDragging = false;
        return false;
    }
    if (msg == WM_MOUSEMOVE && thumbDragging) {
        float dy = my - dragStartY;
        float trackHeight = rect.height;
        float scrollRange = MaxScroll();
        if (trackHeight > 0 && scrollRange > 0) {
            float ratio = dy / trackHeight;
            scrollOffset = std::clamp(dragStartOffset + ratio * scrollRange * 2.0f, 0.0f, scrollRange);
        }
        return true;
    }
    return false;
}

void ScrollView::DrawScrollbar(ID2D1RenderTarget* rt) const {
    if (contentHeight <= rect.height) return; // No scrollbar needed

    float sbX = rect.x + rect.width - Theme::kScrollbarWidth;
    float trackHeight = rect.height;

    // Track
    ID2D1SolidColorBrush* trackBrush = nullptr;
    rt->CreateSolidColorBrush(Theme::ScrollbarTrack(), &trackBrush);
    if (trackBrush) {
        D2D1_RECT_F trackRect = D2D1::RectF(sbX, rect.y, sbX + Theme::kScrollbarWidth, rect.y + trackHeight);
        rt->FillRectangle(trackRect, trackBrush);
        trackBrush->Release();
    }

    // Thumb
    float thumbRatio = rect.height / contentHeight;
    float thumbHeight = std::max(trackHeight * thumbRatio, 20.0f);
    float thumbY = rect.y + (scrollOffset / MaxScroll()) * (trackHeight - thumbHeight);

    ID2D1SolidColorBrush* thumbBrush = nullptr;
    rt->CreateSolidColorBrush(Theme::ScrollbarThumb(), &thumbBrush);
    if (thumbBrush) {
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
            D2D1::RectF(sbX + 1, thumbY, sbX + Theme::kScrollbarWidth - 1, thumbY + thumbHeight),
            3.0f, 3.0f
        );
        rt->FillRoundedRectangle(rr, thumbBrush);
        thumbBrush->Release();
    }
}

void ScrollView::BeginContent(ID2D1RenderTarget* rt) const {
    rt->PushAxisAlignedClip(GetContentClipRect(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    D2D1_MATRIX_3X2_F transform;
    rt->GetTransform(&transform);
    transform._32 -= scrollOffset;
    rt->SetTransform(transform);
}

void ScrollView::EndContent(ID2D1RenderTarget* rt) const {
    D2D1_MATRIX_3X2_F transform;
    rt->GetTransform(&transform);
    transform._32 += scrollOffset;
    rt->SetTransform(transform);
    rt->PopAxisAlignedClip();
}

// ---------- Label ----------

void Label::Draw(ID2D1RenderTarget* rt, IDWriteFactory* dw, float dpi) const {
    if (text.empty()) return;

    IDWriteTextFormat* format = nullptr;
    DWRITE_FONT_WEIGHT weight = bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_REGULAR;
    dw->CreateTextFormat(L"Segoe UI", nullptr, weight,
                          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                          fontSize * dpi, L"en-us", &format);
    if (!format) return;

    format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);

    ID2D1SolidColorBrush* brush = nullptr;
    rt->CreateSolidColorBrush(color, &brush);
    if (brush) {
        rt->DrawText(text.c_str(), static_cast<UINT32>(text.size()),
                      format, rect.ToD2D(), brush);
        brush->Release();
    }
    format->Release();
}

} // namespace stickytodo::ui
