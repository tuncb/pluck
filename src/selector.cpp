#include "selector.h"
#include "app_process.h"
#include <shellscalingapi.h>
#include <stdexcept>
#include <string>

namespace pluck
{
namespace
{
class Selector
{
  public:
    explicit Selector(HINSTANCE instance)
        : bounds_(desktop_bounds()), image_(capture_desktop(bounds_)),
          dimmed_(image_->width(), image_->height())
    {
        // Save the desktop before a selection window can close menus or take focus.
        // End a native menu's input loop before it can consume the first drag or
        // release mouse capture after the selector has started that drag.
        GUITHREADINFO foreground{sizeof(foreground)};
        if (GetGUIThreadInfo(0, &foreground) && (foreground.flags & GUI_INMENUMODE) &&
            foreground.hwndMenuOwner)
        {
            DWORD_PTR ignored{};
            SendMessageTimeoutW(foreground.hwndMenuOwner, WM_CANCELMODE, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK,
                                200, &ignored);
        }
        // Keep a dimmed copy so mouse moves need only bitmap copies during paint.
        for (int i = 0; i < image_->width() * image_->height(); ++i)
        {
            const auto pixel = image_->pixels()[i];
            const auto red = ((pixel >> 16) & 255) * 135 / 255;
            const auto green = ((pixel >> 8) & 255) * 135 / 255;
            const auto blue = (pixel & 255) * 135 / 255;
            dimmed_.pixels()[i] = (red << 16) | (green << 8) | blue;
        }
        WNDCLASSW wc{};
        wc.lpfnWndProc = procedure;
        wc.hInstance = instance;
        wc.lpszClassName = SelectorClass;
        wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
        wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        if (!RegisterClassW(&wc))
            throw std::runtime_error("Cannot register the selection window.");
        window_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, SelectorClass, L"Pluck - select area",
                                  WS_POPUP, bounds_.left, bounds_.top, bounds_.right - bounds_.left,
                                  bounds_.bottom - bounds_.top, nullptr, nullptr, instance, this);
        if (!window_)
            throw std::runtime_error("Cannot open the selection window.");
        prepare_shortcut_hint();
        // A launcher can show the overlay without giving it keyboard focus.
        if (!start_escape_hook())
        {
            DestroyWindow(window_);
            throw std::runtime_error("Cannot prepare the Escape key.");
        }
        ShowWindow(window_, SW_SHOW);
        SetForegroundWindow(window_);
        SetFocus(window_);
        UpdateWindow(window_);
    }
    ~Selector()
    {
        stop_escape_hook();
        if (IsWindow(window_))
            DestroyWindow(window_);
        if (shortcut_font_)
            DeleteObject(shortcut_font_);
    }
    int run()
    {
        MSG message{};
        BOOL result{};
        while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0)
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return result == -1 ? 1 : static_cast<int>(message.wParam);
    }

  private:
    RECT bounds_{};
    std::unique_ptr<Bitmap> image_;
    Bitmap dimmed_;
    HWND window_{};
    HHOOK escape_hook_{};
    inline static HWND escape_window_{};
    POINT start_{}, end_{};
    bool dragging_{}, closing_{}, copying_{};
    GlobalMemory pending_;
    ULONGLONG copy_started_{};
    const wchar_t* hint_{};
    HFONT shortcut_font_{};
    RECT shortcut_rect_{};
    bool opening_settings_{};
    inline static bool settings_key_down_{};

    void prepare_shortcut_hint()
    {
        POINT point{};
        GetCursorPos(&point);
        const auto display = MonitorFromPoint(point, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(display, &monitor))
            return;
        UINT dpi = GetDpiForWindow(window_), dpi_y{};
        GetDpiForMonitor(display, MDT_EFFECTIVE_DPI, &dpi, &dpi_y);
        shortcut_font_ = CreateFontW(-MulDiv(14, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        const auto dc = GetDC(window_);
        const auto old = SelectObject(dc, shortcut_font_ ? shortcut_font_ : GetStockObject(DEFAULT_GUI_FONT));
        SIZE size{};
        constexpr wchar_t text[] = L"Press F2 to open settings";
        GetTextExtentPoint32W(dc, text, static_cast<int>(std::size(text) - 1), &size);
        SelectObject(dc, old);
        ReleaseDC(window_, dc);
        const auto margin = MulDiv(20, dpi, 96), padding = MulDiv(12, dpi, 96);
        const auto width =
            std::min(size.cx + 2 * padding, monitor.rcWork.right - monitor.rcWork.left - 2 * margin);
        const auto height = size.cy + MulDiv(16, dpi, 96);
        const auto left = monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2;
        shortcut_rect_ = {left - bounds_.left, monitor.rcWork.bottom - margin - height - bounds_.top,
                          left + width - bounds_.left, monitor.rcWork.bottom - margin - bounds_.top};
    }
    void paint_shortcut_hint(HDC dc)
    {
        if (dragging_ || copying_ || closing_ || hint_ || IsRectEmpty(&shortcut_rect_))
            return;
        const auto old_brush = SelectObject(dc, GetStockObject(DC_BRUSH));
        const auto old_pen = SelectObject(dc, GetStockObject(DC_PEN));
        const auto old_font =
            SelectObject(dc, shortcut_font_ ? shortcut_font_ : GetStockObject(DEFAULT_GUI_FONT));
        SetDCBrushColor(dc, RGB(18, 38, 32));
        SetDCPenColor(dc, RGB(55, 184, 173));
        RoundRect(dc, shortcut_rect_.left, shortcut_rect_.top, shortcut_rect_.right, shortcut_rect_.bottom, 8,
                  8);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(208, 255, 241));
        auto text = shortcut_rect_;
        DrawTextW(dc, L"Press F2 to open settings", -1, &text,
                  DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
        SelectObject(dc, old_font);
        SelectObject(dc, old_pen);
        SelectObject(dc, old_brush);
    }
    void open_settings()
    {
        if (opening_settings_ || closing_ || copying_)
            return;
        opening_settings_ = true;
        try
        {
            launch_settings();
            finish();
        }
        catch (const std::exception&)
        {
            opening_settings_ = false;
            hint_ = L"Cannot open settings. Press F2 to try again, or Esc to cancel.";
            InvalidateRect(window_, nullptr, FALSE);
        }
    }

    RECT area() const noexcept
    {
        return selection(start_, end_, {0, 0, bounds_.right - bounds_.left, bounds_.bottom - bounds_.top});
    }
    POINT cursor() const noexcept
    {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(window_, &point);
        return point;
    }
    bool start_escape_hook() noexcept
    {
        escape_window_ = window_;
        settings_key_down_ = false;
        escape_hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard_procedure, GetModuleHandleW(nullptr), 0);
        if (!escape_hook_)
            escape_window_ = nullptr;
        return escape_hook_ != nullptr;
    }
    void stop_escape_hook() noexcept
    {
        escape_window_ = nullptr;
        if (escape_hook_)
        {
            UnhookWindowsHookEx(escape_hook_);
            escape_hook_ = nullptr;
        }
    }
    void finish() noexcept
    {
        if (closing_)
            return;
        closing_ = true;
        dragging_ = false;
        stop_escape_hook();
        KillTimer(window_, 1);
        if (GetCapture() == window_)
            ReleaseCapture();
        DestroyWindow(window_);
    }
    void try_copy()
    {
        const auto result = copy_to_clipboard(window_, pending_);
        if (result == CopyResult::Copied)
            finish();
        else if (result == CopyResult::Failed || GetTickCount64() - copy_started_ >= 1500)
        {
            KillTimer(window_, 1);
            pending_.reset();
            copying_ = false;
            hint_ = L"Cannot copy. Drag to try again, or press Esc to cancel.";
            start_ = end_ = {};
            if (!start_escape_hook())
                throw std::runtime_error("Cannot prepare the Escape key.");
            ShowWindow(window_, SW_SHOW);
            SetForegroundWindow(window_);
            InvalidateRect(window_, nullptr, FALSE);
        }
    }
    void redraw_selection(RECT previous)
    {
        const auto selected = area();
        RECT changed{};
        UnionRect(&changed, &previous, &selected);
        InflateRect(&changed, 1, 1);
        InvalidateRect(window_, &changed, FALSE);
    }
    void capture_selection(RECT selected)
    {
        copying_ = true;
        // Image conversion can block this thread. Remove the hook before that work.
        stop_escape_hook();
        ShowWindow(window_, SW_HIDE);
        pending_ = make_clipboard_dib(*image_, selected);
        copy_started_ = GetTickCount64();
        // Retry without blocking the UI or replacing a clipboard held by another app.
        if (!SetTimer(window_, 1, 25, nullptr))
            throw std::runtime_error("Cannot start the clipboard retry timer.");
        try_copy();
    }
    void paint()
    {
        PAINTSTRUCT ps{};
        const auto dc = BeginPaint(window_, &ps);
        const auto selected = area();
        // Paint each part once. The selected pixels come from the original image.
        // The window stays opaque, including inside the selection rectangle.
        const auto saved = SaveDC(dc);
        if (has_area(selected))
            ExcludeClipRect(dc, selected.left, selected.top, selected.right, selected.bottom);
        BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
               ps.rcPaint.bottom - ps.rcPaint.top, dimmed_.dc(), ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        RestoreDC(dc, saved);
        if (has_area(selected))
        {
            BitBlt(dc, selected.left, selected.top, selected.right - selected.left,
                   selected.bottom - selected.top, image_->dc(), selected.left, selected.top, SRCCOPY);
            RECT border = selected;
            InflateRect(&border, 1, 1);
            FrameRect(dc, &border, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        }
        if (hint_)
        {
            // Keep the error hint on the display under the pointer.
            POINT point{};
            GetCursorPos(&point);
            MONITORINFO monitor{sizeof(monitor)};
            GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTOPRIMARY), &monitor);
            const auto dpi = GetDpiForWindow(window_);
            const int padding = MulDiv(16, dpi, 96);
            RECT text{monitor.rcMonitor.left - bounds_.left + padding,
                      monitor.rcMonitor.top - bounds_.top + padding,
                      monitor.rcMonitor.right - bounds_.left - padding, 0};
            text.bottom = text.top + MulDiv(32, dpi, 96);
            const auto font = CreateFontW(-MulDiv(14, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                          CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            const auto old_font = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, RGB(25, 25, 25));
            SetTextColor(dc, RGB(255, 255, 255));
            DrawTextW(dc, hint_, -1, &text, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
            SelectObject(dc, old_font);
            if (font)
                DeleteObject(font);
        }
        paint_shortcut_hint(dc);
        EndPaint(window_, &ps);
    }
    LRESULT handle(UINT message, WPARAM w, LPARAM l)
    {
        switch (message)
        {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            paint();
            return 0;
        case WM_KEYDOWN:
            if (w == VK_ESCAPE)
                finish();
            else if (w == VK_F2)
                open_settings();
            return 0;
        case OpenSettingsMessage:
            open_settings();
            return 0;
        case WM_RBUTTONDOWN:
        case WM_CLOSE:
        case WM_DISPLAYCHANGE:
            finish();
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(w) == WA_INACTIVE && !copying_)
                finish();
            return 0;
        case WM_LBUTTONDOWN:
            if (!copying_)
            {
                const auto previous = area();
                dragging_ = true;
                InvalidateRect(window_, &shortcut_rect_, FALSE);
                start_ = end_ = cursor();
                SetCapture(window_);
                redraw_selection(previous);
            }
            return 0;
        case WM_MOUSEMOVE:
            if (dragging_)
            {
                const auto previous = area();
                end_ = cursor();
                const auto selected = area();
                // Windows can send mouse moves at the same pointer position.
                if (EqualRect(&previous, &selected))
                    return 0;
                redraw_selection(previous);
            }
            return 0;
        case WM_LBUTTONUP:
            if (dragging_)
            {
                end_ = cursor();
                dragging_ = false;
                ReleaseCapture();
                const auto selected = area();
                if (has_area(selected))
                    capture_selection(selected);
                InvalidateRect(window_, &shortcut_rect_, FALSE);
                InvalidateRect(window_, nullptr, FALSE);
            }
            return 0;
        case WM_CAPTURECHANGED:
        case WM_CANCELMODE:
            if (dragging_)
            {
                const auto previous = area();
                dragging_ = false;
                end_ = start_;
                redraw_selection(previous);
                InvalidateRect(window_, &shortcut_rect_, FALSE);
                if (GetCapture() == window_)
                    ReleaseCapture();
            }
            return 0;
        case WM_TIMER:
            if (w == 1 && pending_)
                try_copy();
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window_, message, w, l);
    }
    static LRESULT CALLBACK keyboard_procedure(int code, WPARAM w, LPARAM l) noexcept
    {
        if (code == HC_ACTION && reinterpret_cast<const KBDLLHOOKSTRUCT*>(l)->vkCode == VK_F2 &&
            escape_window_ && IsWindowVisible(escape_window_))
        {
            if (w == WM_KEYUP || w == WM_SYSKEYUP)
            {
                settings_key_down_ = false;
                return 1;
            }
            if (w == WM_KEYDOWN || w == WM_SYSKEYDOWN)
            {
                if (!settings_key_down_)
                    settings_key_down_ = PostMessageW(escape_window_, OpenSettingsMessage, 0, 0) != FALSE;
                return settings_key_down_ ? 1 : CallNextHookEx(nullptr, code, w, l);
            }
        }
        if (code == HC_ACTION && (w == WM_KEYDOWN || w == WM_SYSKEYDOWN) &&
            reinterpret_cast<const KBDLLHOOKSTRUCT*>(l)->vkCode == VK_ESCAPE && escape_window_ &&
            IsWindowVisible(escape_window_))
        {
            // Keep the hook short. Close through the normal window message loop,
            // and do not pass the cancel key to the app behind the overlay.
            if (PostMessageW(escape_window_, WM_CLOSE, 0, 0))
                return 1;
        }
        return CallNextHookEx(nullptr, code, w, l);
    }
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w, LPARAM l) noexcept
    {
        auto* self = reinterpret_cast<Selector*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<Selector*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self)
            return DefWindowProcW(window, message, w, l);
        try
        {
            return self->handle(message, w, l);
        }
        catch (const std::exception& error)
        {
            // Never unwind a C++ exception through a Windows callback.
            ShowWindow(window, SW_HIDE);
            MessageBoxA(nullptr, error.what(), "Pluck", MB_OK | MB_ICONERROR);
            self->finish();
            PostQuitMessage(1);
            return 0;
        }
    }
};
} // namespace
int run_selector(HINSTANCE instance)
{
    Selector selector(instance);
    return selector.run();
}
} // namespace pluck
