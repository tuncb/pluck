#pragma once
#include <windows.h>
#include <dwmapi.h>
#include <iterator>

namespace pluck
{
struct ThemePalette
{
    COLORREF canvas{};
    COLORREF surface{};
    COLORREF ink{};
    COLORREF muted{};
    COLORREF line{};
    COLORREF accent{};
    COLORREF accent_hover{};
    COLORREF accent_ink{};
    COLORREF button{};
    COLORREF button_hover{};
    COLORREF error_ink{};
    bool dark{};
    bool high_contrast{};
};

inline ThemePalette read_theme() noexcept
{
    DWORD light = 1;
    DWORD size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    HIGHCONTRASTW contrast{sizeof(contrast)};
    SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
    const bool high = (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
    const bool dark = !light && !high;

    ThemePalette palette{};
    palette.dark = dark;
    palette.high_contrast = high;
    palette.canvas = high ? GetSysColor(COLOR_WINDOW) : light ? RGB(249, 252, 251) : RGB(12, 18, 17);
    palette.surface = high ? GetSysColor(COLOR_WINDOW) : light ? RGB(236, 243, 240) : RGB(8, 12, 11);
    palette.ink = high ? GetSysColor(COLOR_WINDOWTEXT) : light ? RGB(25, 42, 37) : RGB(237, 248, 244);
    palette.muted = high ? palette.ink : light ? RGB(80, 101, 94) : RGB(158, 180, 172);
    palette.line = high ? palette.ink : light ? RGB(215, 229, 222) : RGB(42, 59, 52);
    palette.accent = high ? GetSysColor(COLOR_HIGHLIGHT) : RGB(119, 244, 213);
    palette.accent_hover = high ? GetSysColor(COLOR_HIGHLIGHT) : light ? RGB(95, 232, 198) : RGB(144, 248, 222);
    palette.accent_ink = high ? GetSysColor(COLOR_HIGHLIGHTTEXT) : RGB(8, 42, 36);
    palette.button = high ? palette.canvas : light ? RGB(232, 242, 237) : RGB(28, 44, 37);
    palette.button_hover = high ? palette.canvas : light ? RGB(219, 234, 227) : RGB(38, 59, 50);
    palette.error_ink = high ? palette.ink : light ? RGB(180, 35, 24) : RGB(255, 138, 128);
    return palette;
}

inline void apply_window_theme(HWND window, const ThemePalette& palette) noexcept
{
    const BOOL dark = palette.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
}

inline void draw_theme_button(const DRAWITEMSTRUCT& item, const ThemePalette& palette, HBRUSH background,
                              HFONT font, UINT dpi, bool primary, bool hovered = false) noexcept
{
    const auto px = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
    FillRect(item.hDC, &item.rcItem, background);
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const COLORREF fill_color = primary && !disabled
                                    ? (hovered ? palette.accent_hover : palette.accent)
                                    : (hovered && !disabled ? palette.button_hover : palette.button);
    const COLORREF border_color = primary && !disabled ? fill_color : palette.line;
    const auto fill = CreateSolidBrush(fill_color);
    const auto pen = CreatePen(PS_SOLID, 1, border_color);
    const auto old_brush = SelectObject(item.hDC, fill);
    const auto old_pen = SelectObject(item.hDC, pen);
    RoundRect(item.hDC, item.rcItem.left, item.rcItem.top, item.rcItem.right, item.rcItem.bottom, px(8),
              px(8));
    SelectObject(item.hDC, old_brush);
    SelectObject(item.hDC, old_pen);
    DeleteObject(fill);
    DeleteObject(pen);

    wchar_t label_text[128]{};
    GetWindowTextW(item.hwndItem, label_text, static_cast<int>(std::size(label_text)));
    const auto old_font = SelectObject(item.hDC, font);
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, disabled ? palette.muted : primary ? palette.accent_ink : palette.ink);
    RECT text_rect = item.rcItem;
    if (item.itemState & ODS_SELECTED)
        OffsetRect(&text_rect, 0, px(1));
    DrawTextW(item.hDC, label_text, -1, &text_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(item.hDC, old_font);
    if ((item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT))
    {
        RECT focus = item.rcItem;
        InflateRect(&focus, -px(4), -px(4));
        DrawFocusRect(item.hDC, &focus);
    }
}
} // namespace pluck
