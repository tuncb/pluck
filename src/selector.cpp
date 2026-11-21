#include "selector.h"
#include "app_process.h"
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <cwchar>
#include <exception>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace pluck
{
namespace
{
void disable_thread_ime() noexcept
{
    // user32 loads imm32 during process startup. Disable IME on the selector thread before
    // window creation so Windows does not load Text Services Framework DLLs during ShowWindow.
    const auto imm = GetModuleHandleW(L"imm32.dll");
    if (!imm)
        return;
    using DisableImeFn = BOOL(WINAPI*)(DWORD);
    if (const auto disable = reinterpret_cast<DisableImeFn>(GetProcAddress(imm, "ImmDisableIME")))
        disable(0);
}

void disable_power_throttling() noexcept
{
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));
}

RECT init_selector_environment() noexcept
{
    disable_thread_ime();
    disable_power_throttling();
    return desktop_bounds();
}

std::vector<RECT> collect_monitors(RECT bounds)
{
    struct Collector
    {
        RECT bounds;
        std::vector<RECT> monitors;
    } collector{bounds, {}};
    collector.monitors.reserve(4);
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR, HDC, LPRECT rect, LPARAM parameter) noexcept -> BOOL
        {
            if (!rect)
                return TRUE;
            auto& state = *reinterpret_cast<Collector*>(parameter);
            RECT visible{};
            if (IntersectRect(&visible, rect, &state.bounds))
            {
                OffsetRect(&visible, -state.bounds.left, -state.bounds.top);
                try
                {
                    state.monitors.push_back(visible);
                }
                catch (...)
                {
                    return FALSE;
                }
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&collector));
    if (collector.monitors.empty())
        collector.monitors.push_back({0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top});
    return collector.monitors;
}

std::vector<RECT> collect_windows(RECT bounds, HWND exclude_window)
{
    struct Collector
    {
        RECT bounds;
        HWND exclude_window;
        HWND shell_window;
        HWND desktop_window;
        std::vector<RECT> rects;
    } collector{bounds, exclude_window, GetShellWindow(), GetDesktopWindow(), {}};
    collector.rects.reserve(32);
    EnumWindows(
        [](HWND hwnd, LPARAM parameter) noexcept -> BOOL
        {
            auto& state = *reinterpret_cast<Collector*>(parameter);
            if (!hwnd || hwnd == state.exclude_window || hwnd == state.shell_window ||
                hwnd == state.desktop_window)
                return TRUE;
            if (!IsWindowVisible(hwnd) || IsIconic(hwnd))
                return TRUE;
            wchar_t class_name[64]{};
            const int length = GetClassNameW(hwnd, class_name, static_cast<int>(std::size(class_name)));
            if (length > 0 &&
                (std::wcscmp(class_name, L"Progman") == 0 || std::wcscmp(class_name, L"WorkerW") == 0))
                return TRUE;
            const auto ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
            if ((ex_style & WS_EX_LAYERED) != 0)
            {
                if ((ex_style & WS_EX_TRANSPARENT) != 0)
                    return TRUE;
                BYTE alpha = 255;
                DWORD flags{};
                if (GetLayeredWindowAttributes(hwnd, nullptr, &alpha, &flags) && (flags & LWA_ALPHA) != 0 &&
                    alpha == 0)
                    return TRUE;
            }
            DWORD cloaked{};
            if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
                cloaked != 0)
                return TRUE;
            RECT rect{};
            if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect))))
            {
                if (!GetWindowRect(hwnd, &rect))
                    return TRUE;
            }
            RECT clip_rect{};
            const RECT* clip = nullptr;
            if (IsZoomed(hwnd))
            {
                MONITORINFO monitor{sizeof(monitor)};
                if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor))
                {
                    const bool covers_monitor =
                        rect.left <= monitor.rcMonitor.left && rect.top <= monitor.rcMonitor.top &&
                        rect.right >= monitor.rcMonitor.right && rect.bottom >= monitor.rcMonitor.bottom;
                    const auto style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
                    clip_rect = (covers_monitor && (style & WS_CAPTION) != WS_CAPTION) ? monitor.rcMonitor
                                                                                       : monitor.rcWork;
                    clip = &clip_rect;
                }
            }
            const RECT clamped = clamp_window_rect(rect, state.bounds, clip);
            if (has_area(clamped))
            {
                try
                {
                    state.rects.push_back(clamped);
                }
                catch (...)
                {
                    return FALSE;
                }
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&collector));
    return collector.rects;
}

class ThreadpoolWork
{
  public:
    ThreadpoolWork() = default;
    ThreadpoolWork(PTP_WORK_CALLBACK callback, PVOID context) noexcept
        : handle_(CreateThreadpoolWork(callback, context, nullptr))
    {
        if (handle_)
            SubmitThreadpoolWork(handle_);
    }
    ~ThreadpoolWork()
    {
        wait();
    }
    ThreadpoolWork(const ThreadpoolWork&) = delete;
    ThreadpoolWork& operator=(const ThreadpoolWork&) = delete;
    bool active() const noexcept
    {
        return handle_ != nullptr;
    }
    void wait() noexcept
    {
        if (handle_)
        {
            WaitForThreadpoolWorkCallbacks(handle_, FALSE);
            CloseThreadpoolWork(handle_);
            handle_ = nullptr;
        }
    }

  private:
    PTP_WORK handle_{};
};

class Selector
{
  public:
    explicit Selector(HINSTANCE instance)
        : bounds_(init_selector_environment()), monitors_(collect_monitors(bounds_)),
          image_(std::make_unique<Bitmap>(bounds_.right - bounds_.left, bounds_.bottom - bounds_.top)),
          dimmed_(image_->width(), image_->height())
    {
        try
        {
            // Fault destination pages in parallel before BitBlt writes the full desktop.
            prefault_bitmap(*image_);

            struct CaptureTask
            {
                Bitmap* image{};
                RECT bounds{};
                std::exception_ptr error;
            } capture_task{image_.get(), bounds_, {}};
            const auto run_capture = [](PTP_CALLBACK_INSTANCE, PVOID context, PTP_WORK) noexcept
            {
                auto& task = *static_cast<CaptureTask*>(context);
                try
                {
                    capture_desktop_into(*task.image, task.bounds);
                }
                catch (...)
                {
                    task.error = std::current_exception();
                }
            };
            const auto run_prefault = [](PTP_CALLBACK_INSTANCE, PVOID context, PTP_WORK) noexcept
            { prefault_bitmap(*static_cast<Bitmap*>(context)); };

            // Capture on a worker while the UI thread builds the hidden window and warms the font.
            // A hidden popup window does not take focus, dismiss menus, or appear in the capture.
            {
                ThreadpoolWork capture_work(run_capture, &capture_task);
                ThreadpoolWork prefault_dimmed(run_prefault, &dimmed_);

                WNDCLASSW wc{};
                wc.lpfnWndProc = procedure;
                wc.hInstance = instance;
                wc.lpszClassName = SelectorClass;
                wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
                cross_cursor_ = LoadCursorW(nullptr, IDC_CROSS);
                window_cursor_ = LoadCursorW(nullptr, IDC_HAND);
                wc.hCursor = cross_cursor_;
                if (!RegisterClassW(&wc))
                    throw std::runtime_error("Cannot register the selection window.");
                window_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, SelectorClass,
                                          L"Pluck - select area", WS_POPUP, bounds_.left, bounds_.top,
                                          bounds_.right - bounds_.left, bounds_.bottom - bounds_.top,
                                          nullptr, nullptr, instance, this);
                if (!window_)
                    throw std::runtime_error("Cannot open the selection window.");
                prepare_shortcut_hint();

                if (!capture_work.active())
                    capture_desktop_into(*image_, bounds_);
                if (!prefault_dimmed.active())
                    prefault_bitmap(dimmed_);
                capture_work.wait();
                prefault_dimmed.wait();
            }

            if (capture_task.error)
                std::rethrow_exception(capture_task.error);

            // End a native menu's input loop after capture finishes so it cannot consume the first drag.
            GUITHREADINFO foreground{sizeof(foreground)};
            if (GetGUIThreadInfo(0, &foreground) && (foreground.flags & GUI_INMENUMODE) &&
                foreground.hwndMenuOwner)
            {
                DWORD_PTR ignored{};
                SendMessageTimeoutW(foreground.hwndMenuOwner, WM_CANCELMODE, 0, 0,
                                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, &ignored);
            }

            // Keep a dimmed copy so mouse moves need only bitmap copies during paint.
            dim_bitmap(*image_, dimmed_, monitors_.data(), monitors_.size());

            // A launcher can show the overlay without giving it keyboard focus.
            if (!start_escape_hook())
                throw std::runtime_error("Cannot prepare the Escape key.");

            ShowWindow(window_, SW_SHOW);
            SetForegroundWindow(window_);
            SetFocus(window_);
            UpdateWindow(window_);
        }
        catch (...)
        {
            cleanup();
            throw;
        }
    }
    ~Selector()
    {
        cleanup();
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
    SelectorResult take_result(int exit_code) noexcept
    {
        return {exit_code, std::move(edit_image_), edit_screen_rect_};
    }

  private:
    RECT bounds_{};
    std::vector<RECT> monitors_;
    std::unique_ptr<Bitmap> image_;
    Bitmap dimmed_;
    HWND window_{};
    HHOOK escape_hook_{};
    inline static HWND escape_window_{};
    POINT start_{}, end_{};
    bool dragging_{}, closing_{}, copying_{};
    bool window_mode_{}, windows_collected_{}, clicking_window_{};
    std::vector<RECT> window_rects_;
    RECT hovered_rect_{};
    HCURSOR cross_cursor_{};
    HCURSOR window_cursor_{};
    GlobalMemory pending_;
    ULONGLONG copy_started_{};
    const wchar_t* hint_{};
    HFONT shortcut_font_{};
    RECT shortcut_monitor_work_{};
    UINT shortcut_dpi_{96};
    RECT shortcut_rect_{};
    bool opening_settings_{};
    bool edit_mode_{};
    std::unique_ptr<Bitmap> edit_image_;
    RECT edit_screen_rect_{};
    inline static bool settings_key_down_{};
    inline static bool edit_key_down_{};
    inline static bool window_key_down_{};

    void cleanup() noexcept
    {
        stop_escape_hook();
        if (window_ && IsWindow(window_))
            DestroyWindow(window_);
        window_ = nullptr;
        if (shortcut_font_)
        {
            DeleteObject(shortcut_font_);
            shortcut_font_ = nullptr;
        }
    }
    const wchar_t* shortcut_text() const noexcept
    {
        if (window_mode_)
        {
            return edit_mode_ ? L"Tab for area  \u00b7  E for Edit Off  \u00b7  F2 for settings"
                              : L"Tab for area  \u00b7  E for Edit  \u00b7  F2 for settings";
        }
        return edit_mode_ ? L"Tab for window  \u00b7  E for Edit Off  \u00b7  F2 for settings"
                          : L"Tab for window  \u00b7  E for Edit  \u00b7  F2 for settings";
    }
    void warm_shortcut_font() noexcept
    {
        // Populate the Segoe UI ClearType glyph cache off-screen so WM_PAINT draws quickly.
        if (!shortcut_font_)
            return;
        const auto dc = CreateCompatibleDC(nullptr);
        if (!dc)
            return;
        const auto pixel = CreateCompatibleBitmap(dc, 1, 1);
        if (pixel)
        {
            const auto old_bitmap = SelectObject(dc, pixel);
            const auto old_font = SelectObject(dc, shortcut_font_);
            RECT box{0, 0, shortcut_rect_.right - shortcut_rect_.left,
                     shortcut_rect_.bottom - shortcut_rect_.top};
            DrawTextW(dc, shortcut_text(), -1, &box, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
            SelectObject(dc, old_font);
            SelectObject(dc, old_bitmap);
            DeleteObject(pixel);
        }
        DeleteDC(dc);
    }
    void update_shortcut_rect()
    {
        if (IsRectEmpty(&shortcut_monitor_work_))
            return;
        const auto dc = GetDC(window_);
        const auto old = SelectObject(dc, shortcut_font_ ? shortcut_font_ : GetStockObject(DEFAULT_GUI_FONT));
        SIZE size{};
        const auto* text = shortcut_text();
        GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &size);
        SelectObject(dc, old);
        ReleaseDC(window_, dc);
        const auto margin = MulDiv(20, shortcut_dpi_, 96);
        const auto padding = MulDiv(12, shortcut_dpi_, 96);
        const auto width = std::min(size.cx + 2 * padding,
                                    shortcut_monitor_work_.right - shortcut_monitor_work_.left - 2 * margin);
        const auto height = size.cy + MulDiv(16, shortcut_dpi_, 96);
        const auto left = shortcut_monitor_work_.left +
                          (shortcut_monitor_work_.right - shortcut_monitor_work_.left - width) / 2;
        shortcut_rect_ = {left - bounds_.left, shortcut_monitor_work_.bottom - margin - height - bounds_.top,
                          left + width - bounds_.left, shortcut_monitor_work_.bottom - margin - bounds_.top};
    }
    void prepare_shortcut_hint()
    {
        POINT point{};
        GetCursorPos(&point);
        const auto display = MonitorFromPoint(point, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(display, &monitor))
            return;
        shortcut_monitor_work_ = monitor.rcWork;
        shortcut_dpi_ = GetDpiForWindow(window_);
        if (monitors_.size() > 1)
        {
            UINT dpi_y{};
            GetDpiForMonitor(display, MDT_EFFECTIVE_DPI, &shortcut_dpi_, &dpi_y);
        }
        shortcut_font_ = CreateFontW(-MulDiv(14, shortcut_dpi_, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        update_shortcut_rect();
        warm_shortcut_font();
    }
    void paint_shortcut_hint(HDC dc)
    {
        if (dragging_ || clicking_window_ || copying_ || closing_ || hint_ || IsRectEmpty(&shortcut_rect_))
            return;
        const auto old_brush = SelectObject(dc, GetStockObject(DC_BRUSH));
        const auto old_pen = SelectObject(dc, GetStockObject(DC_PEN));
        const auto old_font =
            SelectObject(dc, shortcut_font_ ? shortcut_font_ : GetStockObject(DEFAULT_GUI_FONT));
        SetDCBrushColor(dc, edit_mode_ ? RGB(119, 244, 213) : RGB(18, 38, 32));
        SetDCPenColor(dc, RGB(55, 184, 173));
        RoundRect(dc, shortcut_rect_.left, shortcut_rect_.top, shortcut_rect_.right, shortcut_rect_.bottom, 8,
                  8);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, edit_mode_ ? RGB(8, 42, 36) : RGB(208, 255, 241));
        auto text = shortcut_rect_;
        DrawTextW(dc, shortcut_text(), -1, &text, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
        SelectObject(dc, old_font);
        SelectObject(dc, old_pen);
        SelectObject(dc, old_brush);
    }
    void toggle_edit_mode()
    {
        if (opening_settings_ || closing_ || copying_)
            return;
        edit_mode_ = !edit_mode_;
        const auto previous_pill = shortcut_rect_;
        update_shortcut_rect();
        RECT changed{};
        UnionRect(&changed, &previous_pill, &shortcut_rect_);
        const auto selected = active_rect();
        if (has_area(selected))
        {
            auto border = selected;
            InflateRect(&border, 3, 3);
            RECT total{};
            UnionRect(&total, &changed, &border);
            changed = total;
        }
        InvalidateRect(window_, &changed, FALSE);
    }
    void toggle_window_mode()
    {
        if (opening_settings_ || closing_ || copying_)
            return;
        const auto previous = active_rect();
        const auto previous_pill = shortcut_rect_;
        if (dragging_)
        {
            dragging_ = false;
            start_ = end_ = {};
            if (GetCapture() == window_)
                ReleaseCapture();
        }
        if (clicking_window_)
        {
            clicking_window_ = false;
            if (GetCapture() == window_)
                ReleaseCapture();
        }
        window_mode_ = !window_mode_;
        if (window_mode_)
        {
            if (!windows_collected_)
            {
                window_rects_ = collect_windows(bounds_, window_);
                windows_collected_ = true;
            }
            hovered_rect_ = hit_test_rects(window_rects_.data(), window_rects_.size(), cursor());
        }
        else
        {
            hovered_rect_ = {};
        }
        SetCursor(window_mode_ ? window_cursor_ : cross_cursor_);
        update_shortcut_rect();
        const auto current = active_rect();
        RECT changed{};
        UnionRect(&changed, &previous, &current);
        InflateRect(&changed, 3, 3);
        RECT pill_changed{};
        UnionRect(&pill_changed, &previous_pill, &shortcut_rect_);
        RECT total{};
        UnionRect(&total, &changed, &pill_changed);
        InvalidateRect(window_, &total, FALSE);
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
    RECT active_rect() const noexcept
    {
        return window_mode_ ? hovered_rect_ : area();
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
        edit_key_down_ = false;
        window_key_down_ = false;
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
        clicking_window_ = false;
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
            hint_ = window_mode_ ? L"Cannot copy. Click a window to try again, or press Esc to cancel."
                                 : L"Cannot copy. Drag to try again, or press Esc to cancel.";
            start_ = end_ = {};
            if (window_mode_)
                hovered_rect_ = hit_test_rects(window_rects_.data(), window_rects_.size(), cursor());
            if (!start_escape_hook())
                throw std::runtime_error("Cannot prepare the Escape key.");
            ShowWindow(window_, SW_SHOW);
            SetForegroundWindow(window_);
            InvalidateRect(window_, nullptr, FALSE);
        }
    }
    void redraw_selection(RECT previous)
    {
        const auto selected = active_rect();
        RECT changed{};
        UnionRect(&changed, &previous, &selected);
        InflateRect(&changed, 3, 3);
        InvalidateRect(window_, &changed, FALSE);
    }
    void open_editor(RECT selected)
    {
        copying_ = true;
        // Image cropping can block this thread. Remove the hook before that work.
        stop_escape_hook();
        ShowWindow(window_, SW_HIDE);
        edit_image_ = crop_bitmap(*image_, selected);
        edit_screen_rect_ = {bounds_.left + selected.left, bounds_.top + selected.top,
                             bounds_.left + selected.right, bounds_.top + selected.bottom};
        finish();
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
        const auto selected = active_rect();
        const bool has_selection = has_area(selected);
        // Paint each part once. The selected pixels come from the original image.
        // The window stays opaque, including inside the selection rectangle.
        int saved{};
        if (has_selection)
        {
            saved = SaveDC(dc);
            ExcludeClipRect(dc, selected.left, selected.top, selected.right, selected.bottom);
        }
        for (const auto& monitor : monitors_)
        {
            RECT part{};
            if (IntersectRect(&part, &ps.rcPaint, &monitor))
            {
                BitBlt(dc, part.left, part.top, part.right - part.left, part.bottom - part.top,
                       dimmed_.dc(), part.left, part.top, SRCCOPY);
            }
        }
        if (has_selection)
        {
            RestoreDC(dc, saved);
            BitBlt(dc, selected.left, selected.top, selected.right - selected.left,
                   selected.bottom - selected.top, image_->dc(), selected.left, selected.top, SRCCOPY);
            if (window_mode_)
            {
                // Draw an inset border so maximized and edge-aligned windows stay visible on all sides.
                const auto brush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
                const auto previous_color =
                    SetDCBrushColor(dc, edit_mode_ ? RGB(119, 244, 213) : RGB(255, 255, 255));
                RECT border = selected;
                FrameRect(dc, &border, brush);
                if (border.right - border.left > 2 && border.bottom - border.top > 2)
                {
                    InflateRect(&border, -1, -1);
                    FrameRect(dc, &border, brush);
                }
                SetDCBrushColor(dc, previous_color);
            }
            else
            {
                RECT border = selected;
                InflateRect(&border, 1, 1);
                if (edit_mode_)
                {
                    const auto brush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
                    const auto previous_color = SetDCBrushColor(dc, RGB(119, 244, 213));
                    FrameRect(dc, &border, brush);
                    InflateRect(&border, 1, 1);
                    FrameRect(dc, &border, brush);
                    SetDCBrushColor(dc, previous_color);
                }
                else
                    FrameRect(dc, &border, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            }
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
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT)
            {
                SetCursor(window_mode_ ? window_cursor_ : cross_cursor_);
                return TRUE;
            }
            break;
        case WM_PAINT:
            paint();
            return 0;
        case WM_KEYDOWN:
            if (w == VK_ESCAPE)
                finish();
            else if (w == VK_F2)
                open_settings();
            else if (w == 'E' && (l & (1 << 30)) == 0 && (GetKeyState(VK_CONTROL) & 0x8000) == 0 &&
                     (GetKeyState(VK_MENU) & 0x8000) == 0 && (GetKeyState(VK_LWIN) & 0x8000) == 0 &&
                     (GetKeyState(VK_RWIN) & 0x8000) == 0)
                toggle_edit_mode();
            else if (w == VK_TAB && (l & (1 << 30)) == 0 && (GetKeyState(VK_CONTROL) & 0x8000) == 0 &&
                     (GetKeyState(VK_MENU) & 0x8000) == 0 && (GetKeyState(VK_LWIN) & 0x8000) == 0 &&
                     (GetKeyState(VK_RWIN) & 0x8000) == 0)
                toggle_window_mode();
            return 0;
        case OpenSettingsMessage:
            open_settings();
            return 0;
        case ToggleEditModeMessage:
            toggle_edit_mode();
            return 0;
        case ToggleWindowModeMessage:
            toggle_window_mode();
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
                if (window_mode_)
                {
                    const auto previous = hovered_rect_;
                    hovered_rect_ = hit_test_rects(window_rects_.data(), window_rects_.size(), cursor());
                    if (has_area(hovered_rect_))
                    {
                        clicking_window_ = true;
                        InvalidateRect(window_, &shortcut_rect_, FALSE);
                        SetCapture(window_);
                        if (!EqualRect(&previous, &hovered_rect_))
                            redraw_selection(previous);
                    }
                    else if (!EqualRect(&previous, &hovered_rect_))
                    {
                        redraw_selection(previous);
                    }
                }
                else
                {
                    const auto previous = area();
                    dragging_ = true;
                    InvalidateRect(window_, &shortcut_rect_, FALSE);
                    start_ = end_ = cursor();
                    SetCapture(window_);
                    redraw_selection(previous);
                }
            }
            return 0;
        case WM_MOUSEMOVE:
            if (window_mode_)
            {
                if (copying_ || closing_)
                    return 0;
                const auto previous = hovered_rect_;
                hovered_rect_ = hit_test_rects(window_rects_.data(), window_rects_.size(), cursor());
                if (EqualRect(&previous, &hovered_rect_))
                    return 0;
                redraw_selection(previous);
            }
            else if (dragging_)
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
            if (window_mode_)
            {
                if (clicking_window_)
                {
                    clicking_window_ = false;
                    if (GetCapture() == window_)
                        ReleaseCapture();
                    hovered_rect_ = hit_test_rects(window_rects_.data(), window_rects_.size(), cursor());
                    if (has_area(hovered_rect_))
                    {
                        if (edit_mode_)
                            open_editor(hovered_rect_);
                        else
                            capture_selection(hovered_rect_);
                    }
                    else
                    {
                        InvalidateRect(window_, &shortcut_rect_, FALSE);
                        InvalidateRect(window_, nullptr, FALSE);
                    }
                }
            }
            else if (dragging_)
            {
                end_ = cursor();
                dragging_ = false;
                ReleaseCapture();
                const auto selected = area();
                if (has_area(selected))
                {
                    if (edit_mode_)
                        open_editor(selected);
                    else
                        capture_selection(selected);
                }
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
            else if (clicking_window_)
            {
                clicking_window_ = false;
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
        if (code == HC_ACTION && reinterpret_cast<const KBDLLHOOKSTRUCT*>(l)->vkCode == VK_TAB &&
            escape_window_ && IsWindowVisible(escape_window_))
        {
            if (w == WM_KEYUP || w == WM_SYSKEYUP)
            {
                if (window_key_down_)
                {
                    window_key_down_ = false;
                    return 1;
                }
                return CallNextHookEx(nullptr, code, w, l);
            }
            const bool modified = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 ||
                                  (GetAsyncKeyState(VK_MENU) & 0x8000) != 0 ||
                                  (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
                                  (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
            if (!modified && (w == WM_KEYDOWN || w == WM_SYSKEYDOWN))
            {
                if (!window_key_down_)
                    window_key_down_ = PostMessageW(escape_window_, ToggleWindowModeMessage, 0, 0) != FALSE;
                return window_key_down_ ? 1 : CallNextHookEx(nullptr, code, w, l);
            }
        }
        if (code == HC_ACTION && reinterpret_cast<const KBDLLHOOKSTRUCT*>(l)->vkCode == 'E' &&
            escape_window_ && IsWindowVisible(escape_window_))
        {
            if (w == WM_KEYUP || w == WM_SYSKEYUP)
            {
                if (edit_key_down_)
                {
                    edit_key_down_ = false;
                    return 1;
                }
                return CallNextHookEx(nullptr, code, w, l);
            }
            const bool modified = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 ||
                                  (GetAsyncKeyState(VK_MENU) & 0x8000) != 0 ||
                                  (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
                                  (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
            if (!modified && (w == WM_KEYDOWN || w == WM_SYSKEYDOWN))
            {
                if (!edit_key_down_)
                    edit_key_down_ = PostMessageW(escape_window_, ToggleEditModeMessage, 0, 0) != FALSE;
                return edit_key_down_ ? 1 : CallNextHookEx(nullptr, code, w, l);
            }
        }
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
SelectorResult run_selector(HINSTANCE instance)
{
    Selector selector(instance);
    return selector.take_result(selector.run());
}
} // namespace pluck
