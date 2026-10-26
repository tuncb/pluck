#include "selector.h"
#include <dwmapi.h>
#include <stdexcept>
#include <string>

namespace pluck
{
namespace
{
class Selector
{
  public:
    explicit Selector(HINSTANCE instance) : bounds_(desktop_bounds())
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = procedure;
        wc.hInstance = instance;
        wc.lpszClassName = SelectorClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        if (!RegisterClassW(&wc))
            throw std::runtime_error("Cannot register the selection window.");
        window_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED, SelectorClass,
                                  L"Pluck - select area", WS_POPUP, bounds_.left, bounds_.top,
                                  bounds_.right - bounds_.left, bounds_.bottom - bounds_.top, nullptr,
                                  nullptr, instance, this);
        if (!window_)
            throw std::runtime_error("Cannot open the selection window.");
        if (!SetLayeredWindowAttributes(window_, 0, 120, LWA_ALPHA))
        {
            DestroyWindow(window_);
            throw std::runtime_error("Cannot prepare the selection overlay.");
        }
        ShowWindow(window_, SW_SHOW);
        SetForegroundWindow(window_);
        SetFocus(window_);
        UpdateWindow(window_);
    }
    ~Selector()
    {
        if (IsWindow(window_))
            DestroyWindow(window_);
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
    HWND window_{};
    POINT start_{}, end_{};
    bool dragging_{}, closing_{}, copying_{};
    GlobalMemory pending_;
    ULONGLONG copy_started_{};
    const wchar_t* hint_{};

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
    void finish() noexcept
    {
        if (closing_)
            return;
        closing_ = true;
        dragging_ = false;
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
            update_region();
            ShowWindow(window_, SW_SHOW);
            SetForegroundWindow(window_);
            InvalidateRect(window_, nullptr, FALSE);
        }
    }
    void update_region()
    {
        const auto selected = area();
        if (!has_area(selected))
        {
            SetWindowRgn(window_, nullptr, TRUE);
            return;
        }
        const auto full = CreateRectRgn(0, 0, bounds_.right - bounds_.left, bounds_.bottom - bounds_.top);
        const auto hole = CreateRectRgn(selected.left, selected.top, selected.right, selected.bottom);
        if (!full || !hole)
        {
            if (full)
                DeleteObject(full);
            if (hole)
                DeleteObject(hole);
            throw std::runtime_error("Cannot draw the selected area.");
        }
        const auto combined = CombineRgn(full, full, hole, RGN_DIFF);
        DeleteObject(hole);
        if (combined == ERROR || !SetWindowRgn(window_, full, TRUE))
        {
            DeleteObject(full);
            throw std::runtime_error("Cannot update the selected area.");
        }
        // Windows owns full after SetWindowRgn succeeds. The hole shows live pixels.
    }
    void capture_selection(RECT selected)
    {
        copying_ = true;
        // Remove the complete overlay before reading screen pixels. DwmFlush waits
        // for the compositor to present the desktop without the dimming or border.
        ShowWindow(window_, SW_HIDE);
        if (FAILED(DwmFlush()))
            throw std::runtime_error("Cannot wait for the desktop to redraw.");
        const RECT screen{selected.left + bounds_.left, selected.top + bounds_.top,
                          selected.right + bounds_.left, selected.bottom + bounds_.top};
        const auto image = capture_desktop(screen);
        pending_ = make_clipboard_dib(*image, {0, 0, image->width(), image->height()});
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
        FillRect(dc, &ps.rcPaint, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        if (has_area(selected))
        {
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
                dragging_ = true;
                start_ = end_ = cursor();
                SetCapture(window_);
                update_region();
                InvalidateRect(window_, nullptr, FALSE);
            }
            return 0;
        case WM_MOUSEMOVE:
            if (dragging_)
            {
                const auto previous = area();
                end_ = cursor();
                const auto selected = area();
                // SetWindowRgn can cause another WM_MOUSEMOVE at the same position.
                // Do not change the region or repaint until the selection changes.
                if (EqualRect(&previous, &selected))
                    return 0;
                update_region();
            }
            InvalidateRect(window_, nullptr, FALSE);
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
                InvalidateRect(window_, nullptr, FALSE);
            }
            return 0;
        case WM_CAPTURECHANGED:
        case WM_CANCELMODE:
            if (dragging_)
            {
                dragging_ = false;
                end_ = start_;
                update_region();
                if (GetCapture() == window_)
                    ReleaseCapture();
                InvalidateRect(window_, nullptr, FALSE);
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
