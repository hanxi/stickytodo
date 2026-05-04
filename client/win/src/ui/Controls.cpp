#include "ui/Controls.h"
#include "ui/Theme.h"

#include <algorithm>
#include <cmath>
#include <cwctype>   // iswspace — used for Ctrl+Left/Right word jump
#include <cstring>   // memcpy — used for clipboard GlobalLock copy

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
        // DPI-aware corner radius: Theme::kCornerRadius is a 96-DPI
        // baseline (6 px). Without dpi scaling, a 36 px-tall button
        // at 150 % DPI (54 px tall physical) would still round at 6
        // physical pixels → visually flat corners. × dpi keeps the
        // corner's *visual* roundness constant across DPI.
        float cr = Theme::kCornerRadius * dpi;
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(rect.ToD2D(), cr, cr);
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

namespace {

// Selection-related local helpers. These are TU-local so TextBox's
// public surface stays focused on the state machine.

// Build a IDWriteTextFormat identical to the one Draw / HitTestCharIndex
// use. Factoring it out keeps font metrics perfectly consistent across
// all three code paths (draw, caret positioning, selection hit-test);
// if any one of them used a different font / size, clicks would land
// on the wrong character.
IDWriteTextFormat* MakeTextBoxFormat(IDWriteFactory* dw, float dpi) {
    IDWriteTextFormat* format = nullptr;
    dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_REGULAR,
                          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                          Theme::kFontSizeBody * dpi, L"en-us", &format);
    if (format) {
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    return format;
}

} // namespace

int TextBox::HitTestCharIndex(float mx, float dpi, IDWriteFactory* dw) const {
    // `mx` is the window-client-space X (physical pixels, PerMonitorV2).
    // We need the character index inside `displayText` that corresponds
    // to the same pixel column that Draw would render.
    //
    // Steps:
    //   1. Subtract rect.x + kTextInset to get layout-local X.
    //   2. Build a TextLayout at the same font / size Draw uses.
    //   3. DWrite's HitTestPoint returns the char index + a trailing
    //      flag (before/after the midpoint of the char). We snap to
    //      the nearest boundary so clicking on the right half of a
    //      char places the caret AFTER it — matches the standard
    //      OS-wide text input behavior.
    const float kTextInset = 8.0f * dpi;
    std::wstring displayText = GetDisplayText();
    if (displayText.empty() || !dw) return 0;

    IDWriteTextFormat* format = MakeTextBoxFormat(dw, dpi);
    if (!format) return 0;

    float layoutWidth = rect.width - 2.0f * kTextInset;
    float layoutHeight = rect.height;
    IDWriteTextLayout* layout = nullptr;
    dw->CreateTextLayout(displayText.c_str(),
                         static_cast<UINT32>(displayText.size()),
                         format, layoutWidth, layoutHeight, &layout);
    format->Release();
    if (!layout) return 0;

    float localX = mx - (rect.x + kTextInset);
    if (localX < 0.0f) { layout->Release(); return 0; }

    BOOL isTrailing = FALSE, isInside = FALSE;
    DWRITE_HIT_TEST_METRICS hm = {};
    HRESULT hr = layout->HitTestPoint(localX, rect.height * 0.5f,
                                      &isTrailing, &isInside, &hm);
    layout->Release();
    if (FAILED(hr)) return static_cast<int>(displayText.size());

    int idx = static_cast<int>(hm.textPosition);
    if (isTrailing) idx += 1;
    if (idx < 0) idx = 0;
    int maxIdx = static_cast<int>(displayText.size());
    if (idx > maxIdx) idx = maxIdx;
    return idx;
}

void TextBox::Draw(ID2D1RenderTarget* rt, IDWriteFactory* dw, float dpi) const {
    // DPI-aware constants — 96-DPI baselines for this control's
    // chrome (corner radius, border stroke, horizontal text inset).
    const float kCorner = 4.0f * dpi;
    const float kBorderStroke = 1.0f * dpi;
    const float kTextInset = 8.0f * dpi;

    // Background
    ID2D1SolidColorBrush* bgBrush = nullptr;
    rt->CreateSolidColorBrush(Theme::InputBackground(), &bgBrush);
    if (bgBrush) {
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(rect.ToD2D(), kCorner, kCorner);
        rt->FillRoundedRectangle(rr, bgBrush);
        bgBrush->Release();
    }

    // Border
    ID2D1SolidColorBrush* borderBrush = nullptr;
    D2D1_COLOR_F borderColor = focused ? Theme::CheckboxFill() : Theme::InputBorder();
    rt->CreateSolidColorBrush(borderColor, &borderBrush);
    if (borderBrush) {
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(rect.ToD2D(), kCorner, kCorner);
        rt->DrawRoundedRectangle(rr, borderBrush, kBorderStroke);
        borderBrush->Release();
    }

    // Text
    std::wstring displayText = GetDisplayText();
    bool showPlaceholder = displayText.empty() && !placeholder.empty() && !focused;
    const std::wstring& drawText = showPlaceholder ? placeholder : displayText;
    D2D1_COLOR_F textColor = showPlaceholder ? Theme::TextPlaceholder() : Theme::TextPrimary();

    IDWriteTextFormat* format = MakeTextBoxFormat(dw, dpi);
    if (format) {
        D2D1_RECT_F textRect = rect.ToD2D();
        textRect.left += kTextInset;
        textRect.right -= kTextInset;

        // ---- Selection highlight (drawn BEFORE the text so glyphs sit
        // on top of the blue band, not behind it). We compute the pixel
        // extents of [selStart, selEnd) via the same TextLayout that
        // Draw uses for caret positioning, guaranteeing pixel-perfect
        // alignment between what the user sees and what they've
        // selected. Only rendered when focused+enabled+non-empty
        // selection — hiding selection on blur matches the Win32 Edit
        // control default (GWS_OWNERDRAW aside).
        bool hasSelection = focused && enabled && !showPlaceholder &&
                            selStart >= 0 && selEnd >= 0 &&
                            selStart != selEnd;
        if (hasSelection) {
            int lo = std::min(selStart, selEnd);
            int hi = std::max(selStart, selEnd);
            int textLen = static_cast<int>(drawText.size());
            lo = std::max(0, std::min(lo, textLen));
            hi = std::max(0, std::min(hi, textLen));

            IDWriteTextLayout* selLayout = nullptr;
            float layoutWidth = textRect.right - textRect.left;
            float layoutHeight = textRect.bottom - textRect.top;
            dw->CreateTextLayout(drawText.c_str(),
                                 static_cast<UINT32>(drawText.size()),
                                 format, layoutWidth, layoutHeight, &selLayout);
            if (selLayout) {
                DWRITE_HIT_TEST_METRICS hmLo = {}, hmHi = {};
                FLOAT xLo = 0, yLo = 0, xHi = 0, yHi = 0;
                HRESULT hr1 = selLayout->HitTestTextPosition(
                    static_cast<UINT32>(lo), FALSE, &xLo, &yLo, &hmLo);
                HRESULT hr2 = selLayout->HitTestTextPosition(
                    static_cast<UINT32>(hi), FALSE, &xHi, &yHi, &hmHi);
                if (SUCCEEDED(hr1) && SUCCEEDED(hr2)) {
                    float selLeft = textRect.left + xLo;
                    float selRight = textRect.left + xHi;
                    // Clamp to the visible text rect (so a selection
                    // that extends past the right edge doesn't draw
                    // over the input's border/rounded corner).
                    if (selRight > textRect.right) selRight = textRect.right;
                    if (selLeft < textRect.left) selLeft = textRect.left;
                    if (selRight > selLeft) {
                        // Use Theme::CheckboxFill (accent/brand blue)
                        // with alpha 0.30 — matches the "subtle-but-
                        // visible" density of Edit control selection
                        // on Win11, and reads well on both light and
                        // dark backgrounds. We reuse CheckboxFill
                        // rather than adding a new theme entry because
                        // the selection highlight and the checkbox
                        // fill serve the same "active affordance"
                        // role visually.
                        D2D1_COLOR_F selColor = Theme::CheckboxFill();
                        selColor.a = 0.30f;
                        ID2D1SolidColorBrush* selBrush = nullptr;
                        rt->CreateSolidColorBrush(selColor, &selBrush);
                        if (selBrush) {
                            // Vertical extent: full text rect height,
                            // inset by 2 px * dpi top/bottom so the
                            // band doesn't touch the control border.
                            float inset = 2.0f * dpi;
                            D2D1_RECT_F selRect = D2D1::RectF(
                                selLeft, textRect.top + inset,
                                selRight, textRect.bottom - inset);
                            rt->FillRectangle(selRect, selBrush);
                            selBrush->Release();
                        }
                    }
                }
                selLayout->Release();
            }
        }

        ID2D1SolidColorBrush* textBrush = nullptr;
        rt->CreateSolidColorBrush(textColor, &textBrush);
        if (textBrush) {
            rt->DrawText(drawText.c_str(), static_cast<UINT32>(drawText.size()),
                          format, textRect, textBrush);
            textBrush->Release();
        }

        // Caret (text cursor) — drawn as a 1px vertical line at the
        // cursorPos offset inside the display text. Only visible while
        // the TextBox has focus, is enabled, and we are NOT rendering
        // the grey placeholder (drawing a caret on top of a
        // placeholder would be misleading — as soon as the user types
        // a char, the placeholder disappears and the cursor snaps to
        // position 1, so showing a caret alongside the placeholder
        // doesn't match what Win32 Edit / macOS NSTextField do either).
        //
        // Positioning: we need the pixel X of the char boundary
        // `cursorPos` INSIDE displayText. IDWriteTextLayout's
        // HitTestTextPosition maps a character index to a point
        // relative to the layout origin. We create a layout sized to
        // the same textRect and query the index, then add
        // textRect.left for the absolute X.
        //
        // Rendering choice — solid (non-blinking) caret. A blinking
        // caret on a D2D-rendered surface requires a SetTimer-driven
        // InvalidateRect loop plus a shared blink-phase boolean,
        // which would have to be plumbed through every window that
        // hosts a TextBox. A solid caret is visually acceptable
        // (Microsoft Edge's omnibox, VS Code's command palette input,
        // and most modern D2D/Skia apps render a solid caret) and
        // matches what the user explicitly asked for — "看不到光标".
        if (focused && enabled && !showPlaceholder) {
            IDWriteTextLayout* layout = nullptr;
            float layoutWidth = textRect.right - textRect.left;
            float layoutHeight = textRect.bottom - textRect.top;
            // Build the layout over `drawText` (which is `displayText`
            // in the focused branch, i.e. the bullet-masked password
            // or the raw text) so caret math matches what's rendered.
            dw->CreateTextLayout(drawText.c_str(),
                                 static_cast<UINT32>(drawText.size()),
                                 format, layoutWidth, layoutHeight, &layout);
            if (layout) {
                // Clamp cursorPos to the valid range to tolerate any
                // transient mismatch between `text` and `cursorPos`
                // (e.g. programmatic text replacement that forgot to
                // update cursorPos — defensive only, should never
                // trip in practice).
                UINT32 clampedPos = static_cast<UINT32>(std::max(
                    0, std::min(cursorPos, static_cast<int>(drawText.size()))));

                DWRITE_HIT_TEST_METRICS hitMetrics = {};
                FLOAT caretX = 0.0f, caretY = 0.0f;
                // isTrailingHit=FALSE → leading edge of the char at
                // clampedPos. For clampedPos == drawText.size() this
                // correctly returns the trailing edge of the last
                // char, which is what "cursor at end" should show.
                HRESULT hr = layout->HitTestTextPosition(
                    clampedPos, /*isTrailingHit=*/FALSE,
                    &caretX, &caretY, &hitMetrics);
                if (SUCCEEDED(hr)) {
                    // caretX / caretY are relative to the layout
                    // origin (textRect.left/top). Translate to the
                    // render target's coordinate space.
                    float absX = textRect.left + caretX;

                    // Vertical extent: center the caret on the text
                    // rect but shrink by 2px top/bottom so the line
                    // doesn't touch the input border. hitMetrics.height
                    // is the line height of the char; we use it when
                    // available for proper font metrics alignment.
                    // Fallback caret height: subtract a 96-DPI 8-px
                    // vertical inset (4 px top + 4 px bottom) scaled
                    // to physical pixels so the caret doesn't touch
                    // the input border at any DPI.
                    float caretHeight = hitMetrics.height > 0.0f
                        ? hitMetrics.height
                        : (textRect.bottom - textRect.top - 8.0f * dpi);
                    float caretTop = textRect.top +
                        ((textRect.bottom - textRect.top) - caretHeight) * 0.5f;
                    float caretBottom = caretTop + caretHeight;

                    ID2D1SolidColorBrush* caretBrush = nullptr;
                    rt->CreateSolidColorBrush(Theme::TextPrimary(),
                                              &caretBrush);
                    if (caretBrush) {
                        // Caret visuals scale with DPI:
                        //   • half-pixel offset for crisp line (must
                        //     be half of the *physical* stroke width,
                        //     so 0.5 px * dpi).
                        //   • stroke width 1 px * dpi so the caret
                        //     stays visible (a hairline at 1 phys px
                        //     under 200 % DPI is sub-pixel-dim after
                        //     antialiasing).
                        float halfPx = 0.5f * dpi;
                        float strokeW = 1.0f * dpi;
                        rt->DrawLine(
                            D2D1::Point2F(absX + halfPx, caretTop),
                            D2D1::Point2F(absX + halfPx, caretBottom),
                            caretBrush, strokeW);
                        caretBrush->Release();
                    }
                }
                layout->Release();
            }
        }

        format->Release();
    }
}

bool TextBox::HandleMouse(UINT msg, float mx, float my, float dpi,
                          IDWriteFactory* dw) {
    if (!enabled) return false;

    switch (msg) {
        case WM_LBUTTONDOWN: {
            bool inside = rect.Contains(mx, my);
            SetFocus(inside);
            if (!inside) {
                // Click outside: focus loss already clears selection
                // in SetFocus(false); ensure drag flag is also reset
                // in case a previous LBUTTONUP was missed (can happen
                // if the host window doesn't forward mouse-release).
                draggingSelection = false;
                return false;
            }
            // Click inside: place caret at the nearest char boundary
            // and START a drag-selection. Shift+Click extends the
            // current selection instead of resetting it (matches
            // Win32 Edit / NSTextField).
            int idx = HitTestCharIndex(mx, dpi, dw);
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (shift) {
                // Extend existing selection (or start one from the
                // current cursor) to the click point.
                if (selStart < 0) selStart = cursorPos;
                selEnd = idx;
                cursorPos = idx;
            } else {
                cursorPos = idx;
                selStart = idx;
                selEnd = idx;
            }
            draggingSelection = true;
            return true;
        }
        case WM_MOUSEMOVE: {
            if (!draggingSelection) return false;
            // Live-extend the selection. Host window MUST have called
            // SetCapture on LBUTTONDOWN so we still see these messages
            // even when the cursor leaves `rect` — that's why we do
            // NOT gate on rect.Contains here.
            int idx = HitTestCharIndex(mx, dpi, dw);
            selEnd = idx;
            cursorPos = idx;
            return true;
        }
        case WM_LBUTTONUP: {
            if (!draggingSelection) return false;
            draggingSelection = false;
            // If the drag didn't move (selStart == selEnd), clear the
            // selection markers so the caret-only state shows a plain
            // cursor instead of a zero-width highlight.
            if (selStart == selEnd) {
                selStart = selEnd = -1;
            }
            return true;
        }
        default:
            return false;
    }
}

namespace {

// Clipboard helpers — kept TU-local because they're only needed here.
// OpenClipboard(nullptr) is explicitly allowed by MSDN ("If this
// parameter is NULL, the open clipboard is associated with the current
// task"), so we don't need the owning HWND threaded through.

bool CopyToClipboard(const std::wstring& w) {
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    // SetClipboardData takes ownership of the HGLOBAL on success, so
    // the GlobalFree only runs on the failure path.
    size_t bytes = (w.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (h) {
        if (void* p = GlobalLock(h)) {
            memcpy(p, w.c_str(), bytes);
            GlobalUnlock(h);
            ok = (SetClipboardData(CF_UNICODETEXT, h) != nullptr);
        }
        if (!ok) GlobalFree(h);
    }
    CloseClipboard();
    return ok;
}

std::wstring PasteFromClipboard() {
    if (!OpenClipboard(nullptr)) return {};
    std::wstring out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            out.assign(p);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    // Strip control chars (newlines, tabs) — TextBox is single-line,
    // and pasting a multi-line clip into a single-line input should
    // behave like every other native Win32 Edit: keep only the first
    // line / printable chars.
    std::wstring filtered;
    filtered.reserve(out.size());
    for (wchar_t ch : out) {
        if (ch == L'\r' || ch == L'\n') break;
        if (ch >= 32 || ch == L'\t') filtered.push_back(ch);
    }
    return filtered;
}

} // namespace

bool TextBox::HandleKey(UINT msg, WPARAM wParam, LPARAM /*lParam*/) {
    if (!focused || !enabled) return false;
    if (msg != WM_KEYDOWN) return false;

    bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

    // Helper: before moving the caret, anchor selStart at the current
    // cursor if shift is held and there's no active anchor yet; if
    // shift is NOT held, collapse any existing selection.
    auto beginMove = [&]() {
        if (shift) {
            if (selStart < 0) selStart = cursorPos;
        } else {
            selStart = selEnd = -1;
        }
    };
    auto endMove = [&]() {
        if (shift) {
            selEnd = cursorPos;
        }
    };

    switch (wParam) {
        case VK_LEFT:
            beginMove();
            if (ctrl) {
                // Ctrl+Left: jump to previous word boundary (skip
                // trailing whitespace, then skip the word itself).
                while (cursorPos > 0 && iswspace(text[cursorPos - 1])) cursorPos--;
                while (cursorPos > 0 && !iswspace(text[cursorPos - 1])) cursorPos--;
            } else if (cursorPos > 0) {
                cursorPos--;
            }
            endMove();
            return true;
        case VK_RIGHT:
            beginMove();
            if (ctrl) {
                int n = static_cast<int>(text.size());
                while (cursorPos < n && !iswspace(text[cursorPos])) cursorPos++;
                while (cursorPos < n && iswspace(text[cursorPos])) cursorPos++;
            } else if (cursorPos < static_cast<int>(text.size())) {
                cursorPos++;
            }
            endMove();
            return true;
        case VK_HOME:
            beginMove();
            cursorPos = 0;
            endMove();
            return true;
        case VK_END:
            beginMove();
            cursorPos = static_cast<int>(text.size());
            endMove();
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
            if (ctrl) {
                int n = static_cast<int>(text.size());
                if (n == 0) return true;
                selStart = 0;
                selEnd = n;
                cursorPos = n;
                return true;
            }
            return false;
        case 'C':
            if (ctrl) {
                // Password fields: refuse to copy masked chars to
                // the clipboard. Returning true still swallows the
                // accelerator so nothing else processes Ctrl+C.
                if (isPassword) return true;
                if (selStart >= 0 && selStart != selEnd) {
                    int lo = std::min(selStart, selEnd);
                    int hi = std::max(selStart, selEnd);
                    CopyToClipboard(text.substr(lo, hi - lo));
                }
                return true;
            }
            return false;
        case 'X':
            if (ctrl) {
                if (isPassword) return true;
                if (selStart >= 0 && selStart != selEnd) {
                    int lo = std::min(selStart, selEnd);
                    int hi = std::max(selStart, selEnd);
                    CopyToClipboard(text.substr(lo, hi - lo));
                    DeleteSelection();
                    if (onChanged) onChanged(text);
                }
                return true;
            }
            return false;
        case 'V':
            if (ctrl) {
                std::wstring pasted = PasteFromClipboard();
                if (pasted.empty()) return true;
                if (selStart >= 0 && selStart != selEnd) {
                    DeleteSelection();
                }
                text.insert(cursorPos, pasted);
                cursorPos += static_cast<int>(pasted.size());
                selStart = selEnd = -1;
                if (onChanged) onChanged(text);
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

    // DPI-aware dimensions. kCheckboxSize is a 96-DPI baseline; the
    // corner radius, border stroke and checkmark stroke are all
    // 96-DPI baseline px too. The label-offset "6 px gap" between
    // the box and its text label also scales.
    const float boxSize = Theme::kCheckboxSize * dpi;
    const float kCorner = 3.0f * dpi;
    const float kBorderStroke = 1.5f * dpi;
    const float kCheckStroke = 2.0f * dpi;
    const float kLabelGap = 6.0f * dpi;
    float boxY = rect.y + (rect.height - boxSize) / 2.0f;

    D2D1_RECT_F boxRect = D2D1::RectF(rect.x, boxY, rect.x + boxSize, boxY + boxSize);

    if (checked) {
        // Filled box
        ID2D1SolidColorBrush* fillBrush = nullptr;
        D2D1_COLOR_F fillColor = Theme::CheckboxFill();
        fillColor.a *= alphaScale;
        rt->CreateSolidColorBrush(fillColor, &fillBrush);
        if (fillBrush) {
            D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(boxRect, kCorner, kCorner);
            rt->FillRoundedRectangle(rr, fillBrush);
            fillBrush->Release();
        }
        // Checkmark (simple line). All points are derived from `boxSize`
        // so the mark automatically scales with the box itself.
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
            rt->DrawLine(p1, p2, checkBrush, kCheckStroke);
            rt->DrawLine(p2, p3, checkBrush, kCheckStroke);
            checkBrush->Release();
        }
    } else {
        // Border only
        ID2D1SolidColorBrush* borderBrush = nullptr;
        D2D1_COLOR_F borderColor = Theme::CheckboxBorder();
        borderColor.a *= alphaScale;
        rt->CreateSolidColorBrush(borderColor, &borderBrush);
        if (borderBrush) {
            D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(boxRect, kCorner, kCorner);
            rt->DrawRoundedRectangle(rr, borderBrush, kBorderStroke);
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
                rect.x + boxSize + kLabelGap, rect.y,
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
    // Scrollbar width is a 96-DPI baseline, scaled by the `dpi` member
    // set by the owning window before every paint/hit-test batch.
    float sbW = Theme::kScrollbarWidth * dpi;
    return D2D1::RectF(rect.x, rect.y, rect.x + rect.width - sbW, rect.y + rect.height);
}

float ScrollView::MaxScroll() const {
    float max = contentHeight - rect.height;
    return max > 0 ? max : 0;
}

bool ScrollView::HandleWheel(float delta) {
    // Step of 40 px at 96-DPI (≈ one row of text); scale by dpi so
    // the scroll "speed" stays consistent across DPI.
    float newOffset = scrollOffset - delta * 40.0f * dpi;
    newOffset = std::clamp(newOffset, 0.0f, MaxScroll());
    if (newOffset != scrollOffset) {
        scrollOffset = newOffset;
        return true;
    }
    return false;
}

bool ScrollView::HandleMouse(UINT msg, float mx, float my) {
    // Scrollbar is on the right edge of the control. Width scales
    // with dpi so hit-test region matches what DrawScrollbar draws.
    float sbW = Theme::kScrollbarWidth * dpi;
    float sbX = rect.x + rect.width - sbW;
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

    // DPI-aware chrome. kScrollbarWidth is a 96-DPI baseline; the
    // 20-px minimum thumb, 1-px track margins and 3-px corner radius
    // all scale with dpi so the bar keeps its visual weight.
    const float sbW = Theme::kScrollbarWidth * dpi;
    const float kMinThumbH = 20.0f * dpi;
    const float kThumbInset = 1.0f * dpi;
    const float kThumbCorner = 3.0f * dpi;

    float sbX = rect.x + rect.width - sbW;
    float trackHeight = rect.height;

    // Track
    ID2D1SolidColorBrush* trackBrush = nullptr;
    rt->CreateSolidColorBrush(Theme::ScrollbarTrack(), &trackBrush);
    if (trackBrush) {
        D2D1_RECT_F trackRect = D2D1::RectF(sbX, rect.y, sbX + sbW, rect.y + trackHeight);
        rt->FillRectangle(trackRect, trackBrush);
        trackBrush->Release();
    }

    // Thumb
    float thumbRatio = rect.height / contentHeight;
    float thumbHeight = std::max(trackHeight * thumbRatio, kMinThumbH);
    float thumbY = rect.y + (scrollOffset / MaxScroll()) * (trackHeight - thumbHeight);

    ID2D1SolidColorBrush* thumbBrush = nullptr;
    rt->CreateSolidColorBrush(Theme::ScrollbarThumb(), &thumbBrush);
    if (thumbBrush) {
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
            D2D1::RectF(sbX + kThumbInset, thumbY, sbX + sbW - kThumbInset, thumbY + thumbHeight),
            kThumbCorner, kThumbCorner
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
    // Honour caller-requested horizontal alignment (default LEADING ==
    // DirectWrite default; CENTER used by empty-state placeholders in
    // StickyWindow::DrawTodoList).
    format->SetTextAlignment(alignment);

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
