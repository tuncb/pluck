#pragma once
#include "capture.h"

namespace pluck
{
inline constexpr wchar_t SelectorClass[] = L"Pluck.Native.Selector";
inline constexpr COLORREF SelectorShortcutFill = RGB(18, 38, 32);
inline constexpr COLORREF SelectorShortcutEditFill = RGB(119, 244, 213);
inline constexpr COLORREF SelectorShortcutBorder = RGB(55, 184, 173);
inline constexpr COLORREF SelectorShortcutText = RGB(208, 255, 241);
inline constexpr COLORREF SelectorShortcutEditText = RGB(8, 42, 36);

inline constexpr const wchar_t* selector_shortcut_text(bool window_mode, bool edit_mode) noexcept
{
    if (window_mode)
    {
        return edit_mode ? L"Tab for area  \u00b7  E for Edit Off  \u00b7  F2 for settings"
                         : L"Tab for area  \u00b7  E for Edit  \u00b7  F2 for settings";
    }
    return edit_mode ? L"Tab for window  \u00b7  E for Edit Off  \u00b7  F2 for settings"
                     : L"Tab for window  \u00b7  E for Edit  \u00b7  F2 for settings";
}

inline constexpr const wchar_t* selector_copy_error_text(bool window_mode) noexcept
{
    return window_mode ? L"Cannot copy. Click a window to try again, or press Esc to cancel."
                       : L"Cannot copy. Drag to try again, or press Esc to cancel.";
}

inline RECT selector_shortcut_rect(RECT monitor_work, RECT desktop_bounds, SIZE text_size, UINT dpi) noexcept
{
    if (!has_area(monitor_work))
        return {};
    const auto margin = MulDiv(20, static_cast<int>(dpi), 96);
    const auto padding = MulDiv(12, static_cast<int>(dpi), 96);
    const auto max_width = std::max<LONG>(0, monitor_work.right - monitor_work.left - 2 * margin);
    const auto width = std::min<LONG>(text_size.cx + 2 * padding, max_width);
    const auto height = text_size.cy + MulDiv(16, static_cast<int>(dpi), 96);
    if (width <= 0 || height <= 0)
        return {};
    const auto left = monitor_work.left + (monitor_work.right - monitor_work.left - width) / 2;
    return {left - desktop_bounds.left, monitor_work.bottom - margin - height - desktop_bounds.top,
            left + width - desktop_bounds.left, monitor_work.bottom - margin - desktop_bounds.top};
}

inline bool selector_shortcut_visible(bool dragging, bool clicking_window, bool copying, bool closing,
                                      bool has_error_hint, RECT shortcut_rect) noexcept
{
    return !dragging && !clicking_window && !copying && !closing && !has_error_hint &&
           has_area(shortcut_rect);
}

inline void paint_selector_shortcut_pill(HDC dc, RECT rect, HFONT font, bool window_mode,
                                         bool edit_mode) noexcept
{
    const auto old_brush = SelectObject(dc, GetStockObject(DC_BRUSH));
    const auto old_pen = SelectObject(dc, GetStockObject(DC_PEN));
    const auto old_font = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
    SetDCBrushColor(dc, edit_mode ? SelectorShortcutEditFill : SelectorShortcutFill);
    SetDCPenColor(dc, SelectorShortcutBorder);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, 8, 8);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, edit_mode ? SelectorShortcutEditText : SelectorShortcutText);
    DrawTextW(dc, selector_shortcut_text(window_mode, edit_mode), -1, &rect,
              DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
    SelectObject(dc, old_font);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
}

struct SelectorResult
{
    int exit_code{};
    std::unique_ptr<Bitmap> edit_image;
    RECT selection_screen_rect{};
};
SelectorResult run_selector(HINSTANCE instance);
} // namespace pluck
