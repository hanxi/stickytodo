#pragma once

#include <windows.h>

#include "models/Filter.h"

namespace stickytodo::ui {

/// Modal dialog for editing a sticky's Filter criteria.
///
/// Behavioural contract (mirrors macOS FilterEditor.swift):
///
///   - Inputs rendered:
///       * Status segmented picker: 全部 / 未完成 / 已完成
///         (models::Filter::status empty-string ↔ "pending" ↔ "done")
///       * Tag single-line TextBox        (Filter::tag)
///       * Keyword single-line TextBox    (Filter::keyword)
///       * "包含已删除" CheckBox           (Filter::include_deleted)
///       * "仅显示已删除" CheckBox         (Filter::only_deleted)
///           └ If only_deleted is checked, include_deleted is force-disabled
///             and its stored value is cleared on commit — identical to the
///             macOS `synth.includeDeleted = false` guard in `commit()`.
///       * Page-size stepper              (Filter::page_size; range 10-200, step 10)
///       * Header buttons: 取消 / 重置 / 保存
///           └ 重置 → reset `draft` to a fresh `models::Filter{}`
///           └ 保存 → copy `draft` into the supplied filter& and return true
///           └ 取消 / × / ESC → return false, leave filter& untouched
///
///   - Win32 modality is implemented with CreateWindowExW + EnableWindow(owner,
///     FALSE) + a local GetMessage loop (not DialogBox — our rendering is
///     Direct2D-based, so we own the paint path and can't use the system
///     dialog template). The owner is re-enabled and refocused before this
///     function returns, regardless of save/cancel path.
///
///   - `hInstance` must be the same HINSTANCE that registered the
///     application's D2D render target factory (App::GetHInstance()) — the
///     dialog reuses the app-level ID2D1Factory via App::GetRenderer().
///
///   - The function is synchronous: it returns only after the user closes
///     the dialog. Callers must therefore invoke it from the UI thread.
///
/// @param owner     Parent HWND (typically the owning StickyWindow). Must be
///                  non-null and visible; the dialog is centered over it.
/// @param hInstance Module handle used for the dialog's window class. If the
///                  class is already registered (from a previous call), the
///                  registration is skipped — the class name is process-wide.
/// @param filter    In/out filter criteria. On `true` return, `filter` has
///                  been overwritten with the committed values. On `false`
///                  return, `filter` is untouched (including when the dialog
///                  is closed via window chrome × or ESC).
///
/// @return true if the user clicked 保存; false for 取消 / ESC / × close.
class FilterEditor {
public:
    static bool ShowModal(HWND owner, HINSTANCE hInstance, models::Filter& filter);

private:
    // Intentionally non-instantiable: callers only use the static entry above.
    // The actual dialog state is held in a per-invocation pimpl struct
    // defined in FilterEditor.cpp (kept TU-local to avoid leaking Controls
    // / D2D types into every translation unit that includes this header).
    FilterEditor() = delete;
    ~FilterEditor() = delete;
    FilterEditor(const FilterEditor&) = delete;
    FilterEditor& operator=(const FilterEditor&) = delete;
};

} // namespace stickytodo::ui
