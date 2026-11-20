#include "editor.h"
#include "editor_core.h"
#include "app_process.h"
#include "theme.h"
#include <d2d1.h>
#include <shellscalingapi.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace pluck
{
namespace
{
using Microsoft::WRL::ComPtr;

constexpr wchar_t EditorCanvasClass[] = L"Pluck.Native.EditorCanvas";
constexpr GUID EditorSaveDialogGuid{
    0x4e7a9d31, 0x8b42, 0x4c95, {0xa6, 0x3d, 0x71, 0x2f, 0x58, 0x9c, 0x14, 0xe2}};
constexpr UINT_PTR CopyRetryTimer = 1;
constexpr int ToolbarHeightDip = 56;
constexpr int CanvasPaddingDip = 20;
constexpr int MinClientWidthDip = 460;
constexpr int MinClientHeightDip = 300;

D2D1_COLOR_F to_d2d_color(COLORREF color) noexcept
{
    return D2D1::ColorF(GetRValue(color) / 255.0f, GetGValue(color) / 255.0f, GetBValue(color) / 255.0f);
}

std::wstring wide_error(const char* text)
{
    if (!text || !*text)
        return L"The operation failed. Try again.";
    const int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (length <= 1)
        return L"The operation failed. Try again.";
    std::wstring result(static_cast<size_t>(length - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), length - 1);
    return result;
}

class EditorCanvas
{
  public:
    EditorCanvas(HINSTANCE instance, HWND parent, const EditorDocument& document, const ThemePalette& palette,
                 UINT dpi, std::function<void(int)> on_zoom_changed)
        : document_(document), palette_(palette), dpi_(dpi), on_zoom_changed_(std::move(on_zoom_changed))
    {
        WNDCLASSW wc{};
        if (!GetClassInfoW(instance, EditorCanvasClass, &wc))
        {
            wc.lpfnWndProc = procedure;
            wc.hInstance = instance;
            wc.lpszClassName = EditorCanvasClass;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            if (!RegisterClassW(&wc))
                throw std::runtime_error("Cannot prepare the image canvas.");
        }
        window_ = CreateWindowExW(0, EditorCanvasClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, parent,
                                  nullptr, instance, this);
        if (!window_)
            throw std::runtime_error("Cannot open the image canvas.");
    }

    ~EditorCanvas()
    {
        discard_device_resources();
        if (IsWindow(window_))
            DestroyWindow(window_);
    }

    EditorCanvas(const EditorCanvas&) = delete;
    EditorCanvas& operator=(const EditorCanvas&) = delete;

    HWND handle() const noexcept
    {
        return window_;
    }

    void set_theme(const ThemePalette& palette) noexcept
    {
        palette_ = palette;
        if (border_brush_)
            border_brush_->SetColor(to_d2d_color(palette_.line));
        InvalidateRect(window_, nullptr, FALSE);
    }

    void set_dpi(UINT dpi) noexcept
    {
        dpi_ = dpi;
        InvalidateRect(window_, nullptr, FALSE);
    }

    void resize(int x, int y, int width, int height) noexcept
    {
        MoveWindow(window_, x, y, std::max(0, width), std::max(0, height), TRUE);
        InvalidateRect(window_, nullptr, FALSE);
    }

    int zoom_percent() const noexcept
    {
        return zoom_percent_;
    }

  private:
    const EditorDocument& document_;
    ThemePalette palette_{};
    UINT dpi_{96};
    std::function<void(int)> on_zoom_changed_;
    HWND window_{};
    ComPtr<ID2D1Factory> factory_;
    ComPtr<ID2D1HwndRenderTarget> target_;
    ComPtr<ID2D1SolidColorBrush> border_brush_;
    ComPtr<ID2D1Bitmap> bitmap_;
    UINT cached_width_{};
    UINT cached_height_{};
    int zoom_percent_{100};

    void discard_device_resources() noexcept
    {
        bitmap_.Reset();
        border_brush_.Reset();
        target_.Reset();
        cached_width_ = 0;
        cached_height_ = 0;
    }

    bool ensure_target(UINT client_width, UINT client_height)
    {
        if (!factory_)
        {
            if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf())))
                return false;
        }
        if (!target_)
        {
            // Render in physical pixels (96 DPI) so 1:1 captures display without DPI resampling blur.
            const auto properties = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
            const auto hwnd_properties =
                D2D1::HwndRenderTargetProperties(window_, D2D1::SizeU(client_width, client_height));
            if (FAILED(factory_->CreateHwndRenderTarget(properties, hwnd_properties, &target_)))
                return false;
            if (FAILED(target_->CreateSolidColorBrush(to_d2d_color(palette_.line), &border_brush_)))
            {
                discard_device_resources();
                return false;
            }
            return true;
        }
        const auto current = target_->GetPixelSize();
        if (current.width != client_width || current.height != client_height)
        {
            const auto hr = target_->Resize(D2D1::SizeU(client_width, client_height));
            if (hr == D2DERR_RECREATE_TARGET)
            {
                discard_device_resources();
                return ensure_target(client_width, client_height);
            }
            if (FAILED(hr))
                return false;
        }
        return true;
    }

    bool ensure_bitmap(UINT draw_width, UINT draw_height)
    {
        const UINT max_size = std::max(1u, target_->GetMaximumBitmapSize());
        UINT pixel_width = std::min(draw_width, max_size);
        UINT pixel_height = std::min(draw_height, max_size);
        const auto& source = document_.render_output();
        const auto source_width = static_cast<UINT>(source.width());
        const auto source_height = static_cast<UINT>(source.height());
        if (source_width > max_size || source_height > max_size)
        {
            const float limit = std::min(static_cast<float>(max_size) / static_cast<float>(source_width),
                                         static_cast<float>(max_size) / static_cast<float>(source_height));
            pixel_width = std::min(pixel_width,
                                   std::max(1u, static_cast<UINT>(std::floor(source_width * limit))));
            pixel_height = std::min(pixel_height,
                                    std::max(1u, static_cast<UINT>(std::floor(source_height * limit))));
        }
        if (bitmap_ && cached_width_ == pixel_width && cached_height_ == pixel_height)
            return true;

        bitmap_.Reset();
        cached_width_ = 0;
        cached_height_ = 0;

        const BYTE* pixels = reinterpret_cast<const BYTE*>(source.pixels());
        std::vector<BYTE> scaled;
        if (pixel_width != source_width || pixel_height != source_height)
        {
            // Area resampling preserves thin text when large captures are fitted to the window.
            ComPtr<IWICImagingFactory> wic;
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&wic))))
                return false;
            ComPtr<IWICBitmap> wic_source;
            const auto source_bytes = static_cast<UINT>(static_cast<size_t>(source_width) * source_height * 4);
            if (FAILED(wic->CreateBitmapFromMemory(source_width, source_height, GUID_WICPixelFormat32bppBGRA,
                                                   source_width * 4, source_bytes,
                                                   const_cast<BYTE*>(pixels), &wic_source)))
                return false;
            ComPtr<IWICBitmapScaler> scaler;
            if (FAILED(wic->CreateBitmapScaler(&scaler)) ||
                FAILED(scaler->Initialize(wic_source.Get(), pixel_width, pixel_height,
                                          WICBitmapInterpolationModeFant)))
                return false;
            scaled.resize(static_cast<size_t>(pixel_width) * pixel_height * 4);
            if (FAILED(scaler->CopyPixels(nullptr, pixel_width * 4, static_cast<UINT>(scaled.size()),
                                          scaled.data())))
                return false;
            pixels = scaled.data();
        }
        const auto properties = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        if (FAILED(target_->CreateBitmap(D2D1::SizeU(pixel_width, pixel_height), pixels, pixel_width * 4,
                                         properties, &bitmap_)))
            return false;
        cached_width_ = pixel_width;
        cached_height_ = pixel_height;
        return true;
    }

    void paint()
    {
        PAINTSTRUCT ps{};
        BeginPaint(window_, &ps);
        RECT client{};
        GetClientRect(window_, &client);
        const int client_width = client.right - client.left;
        const int client_height = client.bottom - client.top;
        if (client_width <= 0 || client_height <= 0)
        {
            EndPaint(window_, &ps);
            return;
        }
        if (!ensure_target(static_cast<UINT>(client_width), static_cast<UINT>(client_height)))
        {
            EndPaint(window_, &ps);
            return;
        }

        const int padding = MulDiv(CanvasPaddingDip, static_cast<int>(dpi_), 96);
        const int available_width = std::max(1, client_width - 2 * padding);
        const int available_height = std::max(1, client_height - 2 * padding);
        const float image_width = static_cast<float>(document_.width());
        const float image_height = static_cast<float>(document_.height());
        const float scale = std::min(
            1.0f, std::min(static_cast<float>(available_width) / image_width,
                           static_cast<float>(available_height) / image_height));
        const UINT draw_width = static_cast<UINT>(
            std::clamp(static_cast<int>(std::lround(image_width * scale)), 1, available_width));
        const UINT draw_height = static_cast<UINT>(
            std::clamp(static_cast<int>(std::lround(image_height * scale)), 1, available_height));
        const int next_zoom = std::max(1, static_cast<int>(std::lround(scale * 100.0f)));

        const bool has_bitmap = ensure_bitmap(draw_width, draw_height);
        target_->BeginDraw();
        target_->Clear(to_d2d_color(palette_.surface));
        if (has_bitmap && bitmap_)
        {
            const float left = std::floor((static_cast<float>(client_width) - static_cast<float>(draw_width)) /
                                          2.0f);
            const float top = std::floor((static_cast<float>(client_height) - static_cast<float>(draw_height)) /
                                         2.0f);
            const float right = left + static_cast<float>(draw_width);
            const float bottom = top + static_cast<float>(draw_height);
            target_->DrawBitmap(bitmap_.Get(), D2D1::RectF(left, top, right, bottom), 1.0f,
                                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            if (border_brush_)
            {
                // Snap the 1-pixel frame outside the screenshot to pixel centers.
                target_->DrawRectangle(D2D1::RectF(left - 0.5f, top - 0.5f, right + 0.5f, bottom + 0.5f),
                                       border_brush_.Get(), 1.0f);
            }
        }
        const auto hr = target_->EndDraw();
        EndPaint(window_, &ps);
        if (hr == D2DERR_RECREATE_TARGET)
        {
            discard_device_resources();
            InvalidateRect(window_, nullptr, FALSE);
        }
        if (next_zoom != zoom_percent_)
        {
            zoom_percent_ = next_zoom;
            if (on_zoom_changed_)
                on_zoom_changed_(zoom_percent_);
        }
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
        case WM_SIZE:
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        return DefWindowProcW(window_, message, w, l);
    }

    static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM w, LPARAM l) noexcept
    {
        auto* self = reinterpret_cast<EditorCanvas*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<EditorCanvas*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            self->window_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self)
            return DefWindowProcW(hwnd, message, w, l);
        try
        {
            return self->handle(message, w, l);
        }
        catch (...)
        {
            return 0;
        }
    }
};

class Editor
{
  public:
    enum ControlId : int
    {
        Copy = 201,
        Save = 202
    };

    Editor(HINSTANCE instance, std::unique_ptr<Bitmap> image)
        : instance_(instance), document_(std::move(image))
    {
    }

    ~Editor()
    {
        canvas_.reset();
        if (font_)
            DeleteObject(font_);
        if (background_)
            DeleteObject(background_);
        if (brand_icon_)
            DestroyIcon(brand_icon_);
    }

    Editor(const Editor&) = delete;
    Editor& operator=(const Editor&) = delete;

    HWND create_window(RECT selection_screen_rect)
    {
        WNDCLASSW wc{};
        if (!GetClassInfoW(instance_, EditorClass, &wc))
        {
            wc.hInstance = instance_;
            wc.lpfnWndProc = procedure;
            wc.lpszClassName = EditorClass;
            wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(1));
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            if (!RegisterClassW(&wc))
                throw std::runtime_error("Cannot prepare the editor window.");
        }

        const auto display = has_area(selection_screen_rect)
                                 ? MonitorFromRect(&selection_screen_rect, MONITOR_DEFAULTTONEAREST)
                                 : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(display, &monitor);
        UINT target_dpi = GetDpiForSystem();
        UINT dpi_y{};
        GetDpiForMonitor(display, MDT_EFFECTIVE_DPI, &target_dpi, &dpi_y);

        constexpr DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
        const auto compute_bounds = [&](UINT scale_dpi)
        {
            const auto scaled = [scale_dpi](int dip) { return MulDiv(dip, static_cast<int>(scale_dpi), 96); };
            const int toolbar_h = scaled(ToolbarHeightDip);
            const int pad = scaled(CanvasPaddingDip);
            const int min_client_w = scaled(MinClientWidthDip);
            const int min_client_h = scaled(MinClientHeightDip);
            const int desired_client_w = std::max(min_client_w, document_.width() + 2 * pad);
            const int desired_client_h = std::max(min_client_h, toolbar_h + document_.height() + 2 * pad);
            RECT outer{0, 0, desired_client_w, desired_client_h};
            AdjustWindowRectExForDpi(&outer, style, FALSE, 0, scale_dpi);
            RECT min_outer{0, 0, min_client_w, min_client_h};
            AdjustWindowRectExForDpi(&min_outer, style, FALSE, 0, scale_dpi);

            const int work_w = static_cast<int>(monitor.rcWork.right - monitor.rcWork.left);
            const int work_h = static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top);
            const int min_outer_w = static_cast<int>(min_outer.right - min_outer.left);
            const int min_outer_h = static_cast<int>(min_outer.bottom - min_outer.top);
            const int outer_w = static_cast<int>(outer.right - outer.left);
            const int outer_h = static_cast<int>(outer.bottom - outer.top);
            const int max_w = std::min(work_w, std::max(min_outer_w, work_w * 85 / 100));
            const int max_h = std::min(work_h, std::max(min_outer_h, work_h * 85 / 100));
            const int width = std::min(outer_w, max_w);
            const int height = std::min(outer_h, max_h);
            return RECT{monitor.rcWork.left + (work_w - width) / 2,
                        monitor.rcWork.top + (work_h - height) / 2,
                        monitor.rcWork.left + (work_w - width) / 2 + width,
                        monitor.rcWork.top + (work_h - height) / 2 + height};
        };

        const auto initial = compute_bounds(target_dpi);
        const auto hwnd = CreateWindowExW(0, EditorClass, L"Pluck Editor", style, initial.left, initial.top,
                                          initial.right - initial.left, initial.bottom - initial.top,
                                          nullptr, nullptr, instance_, this);
        if (!hwnd)
            throw std::runtime_error("Cannot open the editor window.");

        const auto final_bounds = compute_bounds(dpi_);
        SetWindowPos(hwnd, nullptr, final_bounds.left, final_bounds.top,
                     final_bounds.right - final_bounds.left, final_bounds.bottom - final_bounds.top,
                     SWP_NOZORDER);
        layout();
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
        SetFocus(copy_button_);
        UpdateWindow(hwnd);
        return hwnd;
    }

    void command(int id)
    {
        if (id == IDCANCEL)
        {
            SendMessageW(window_, WM_CLOSE, 0, 0);
            return;
        }
        if (busy())
            return;
        if (id == IDOK)
        {
            id = GetFocus() == save_button_ ? Save : Copy;
        }
        if (id == Copy)
            start_copy();
        else if (id == Save)
            save_as_png();
    }

    bool busy() const noexcept
    {
        return copying_ || saving_dialog_;
    }

  private:
    HINSTANCE instance_{};
    EditorDocument document_;
    HWND window_{};
    HWND save_button_{};
    HWND copy_button_{};
    HWND hovered_button_{};
    WNDPROC base_button_proc_{};
    HFONT font_{};
    HICON brand_icon_{};
    HBRUSH background_{};
    ThemePalette palette_{};
    UINT dpi_{96};
    std::unique_ptr<EditorCanvas> canvas_;
    int zoom_percent_{100};
    bool copying_{};
    bool saving_dialog_{};
    GlobalMemory pending_;
    ULONGLONG copy_started_{};
    std::wstring status_text_;
    bool status_error_{};

    int px(int value) const noexcept
    {
        return MulDiv(value, static_cast<int>(dpi_), 96);
    }

    int toolbar_height() const noexcept
    {
        return px(ToolbarHeightDip);
    }

    void set_status(std::wstring text, bool error = false)
    {
        status_text_ = std::move(text);
        status_error_ = error;
        invalidate_toolbar();
    }

    void invalidate_toolbar() noexcept
    {
        if (!window_)
            return;
        RECT client{};
        GetClientRect(window_, &client);
        RECT bar{0, 0, client.right, toolbar_height()};
        InvalidateRect(window_, &bar, FALSE);
    }

    void update_buttons() noexcept
    {
        const BOOL enabled = busy() ? FALSE : TRUE;
        if (copy_button_)
            EnableWindow(copy_button_, enabled);
        if (save_button_)
            EnableWindow(save_button_, enabled);
    }

    void refresh_theme()
    {
        palette_ = read_theme();
        if (background_)
            DeleteObject(background_);
        background_ = CreateSolidBrush(palette_.canvas);
        apply_window_theme(window_, palette_);
        if (canvas_)
            canvas_->set_theme(palette_);
        invalidate_toolbar();
        if (copy_button_)
            InvalidateRect(copy_button_, nullptr, TRUE);
        if (save_button_)
            InvalidateRect(save_button_, nullptr, TRUE);
    }

    void refresh_fonts()
    {
        if (font_)
            DeleteObject(font_);
        font_ = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                            L"Segoe UI");
        if (brand_icon_)
            DestroyIcon(brand_icon_);
        brand_icon_ = static_cast<HICON>(
            LoadImageW(instance_, MAKEINTRESOURCEW(1), IMAGE_ICON, px(22), px(22), LR_DEFAULTCOLOR));
        for (auto button : {save_button_, copy_button_})
            if (button)
                SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }

    HWND create_button(const wchar_t* label, int id)
    {
        const auto button =
            CreateWindowExW(0, L"BUTTON", label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0,
                            0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        if (!button)
            throw std::runtime_error("Cannot create an editor button.");
        const auto previous = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(button, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(button_procedure)));
        if (!base_button_proc_)
            base_button_proc_ = previous;
        return button;
    }

    void create()
    {
        dpi_ = GetDpiForWindow(window_);
        palette_ = read_theme();
        background_ = CreateSolidBrush(palette_.canvas);
        apply_window_theme(window_, palette_);

        save_button_ = create_button(L"Save PNG", Save);
        copy_button_ = create_button(L"Copy", Copy);
        refresh_fonts();

        canvas_ = std::make_unique<EditorCanvas>(
            instance_, window_, document_, palette_, dpi_,
            [this](int percent)
            {
                zoom_percent_ = percent;
                invalidate_toolbar();
            });
        layout();
    }

    void layout()
    {
        if (!window_)
            return;
        RECT client{};
        GetClientRect(window_, &client);
        const int bar_h = toolbar_height();
        const int margin = px(16);
        const int gap = px(10);
        const int button_h = px(34);
        const int button_y = (bar_h - button_h) / 2;
        const int copy_w = px(92);
        const int save_w = px(108);
        const int copy_x = std::max(margin, static_cast<int>(client.right) - margin - copy_w);
        const int save_x = std::max(margin, copy_x - gap - save_w);

        if (copy_button_)
            MoveWindow(copy_button_, copy_x, button_y, copy_w, button_h, TRUE);
        if (save_button_)
            MoveWindow(save_button_, save_x, button_y, save_w, button_h, TRUE);
        if (canvas_)
            canvas_->resize(0, bar_h, client.right, std::max(0, static_cast<int>(client.bottom) - bar_h));
        invalidate_toolbar();
    }

    void paint()
    {
        PAINTSTRUCT ps{};
        const auto dc = BeginPaint(window_, &ps);
        RECT client{};
        GetClientRect(window_, &client);
        FillRect(dc, &ps.rcPaint, background_);

        const int bar_h = toolbar_height();
        const int margin = px(16);
        const int icon_size = px(22);
        if (brand_icon_)
        {
            DrawIconEx(dc, margin, (bar_h - icon_size) / 2, brand_icon_, icon_size, icon_size, 0, nullptr,
                       DI_NORMAL);
        }

        const int buttons_left = std::max(margin, static_cast<int>(client.right) - margin - px(92) - px(10) -
                                                      px(108) - px(12));
        std::wstring info =
            std::to_wstring(document_.width()) + L" \u00d7 " + std::to_wstring(document_.height()) + L" px";
        if (zoom_percent_ < 100)
            info += L" \u00b7 " + std::to_wstring(zoom_percent_) + L"%";

        const auto old_font = SelectObject(dc, font_ ? font_ : GetStockObject(DEFAULT_GUI_FONT));
        SetBkMode(dc, TRANSPARENT);
        const int info_left = margin + icon_size + px(10);
        SIZE info_size{};
        GetTextExtentPoint32W(dc, info.c_str(), static_cast<int>(info.size()), &info_size);
        RECT info_rect{info_left, 0,
                       std::min(buttons_left, info_left + static_cast<int>(info_size.cx) + px(8)), bar_h};
        if (info_rect.right > info_rect.left)
        {
            SetTextColor(dc, palette_.muted);
            DrawTextW(dc, info.c_str(), -1, &info_rect,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }

        if (!status_text_.empty())
        {
            RECT status_rect{info_rect.right + px(8), 0, buttons_left, bar_h};
            if (status_rect.right > status_rect.left)
            {
                SetTextColor(dc, status_error_ ? palette_.error_ink : palette_.muted);
                DrawTextW(dc, status_text_.c_str(), -1, &status_rect,
                          DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            }
        }
        SelectObject(dc, old_font);

        const auto pen = CreatePen(PS_SOLID, 1, palette_.line);
        const auto old_pen = SelectObject(dc, pen);
        MoveToEx(dc, 0, bar_h - 1, nullptr);
        LineTo(dc, client.right, bar_h - 1);
        SelectObject(dc, old_pen);
        DeleteObject(pen);

        EndPaint(window_, &ps);
    }

    void start_copy()
    {
        try
        {
            pending_ = document_.make_clipboard_dib();
        }
        catch (const std::exception& error)
        {
            set_status(wide_error(error.what()), true);
            return;
        }
        copying_ = true;
        copy_started_ = GetTickCount64();
        update_buttons();
        set_status(L"Copying...");
        if (!SetTimer(window_, CopyRetryTimer, 25, nullptr))
        {
            pending_.reset();
            copying_ = false;
            update_buttons();
            set_status(L"Cannot start clipboard copy. Try again.", true);
            return;
        }
        try_copy();
    }

    void try_copy()
    {
        const auto result = copy_to_clipboard(window_, pending_);
        if (result == CopyResult::Copied)
        {
            KillTimer(window_, CopyRetryTimer);
            copying_ = false;
            DestroyWindow(window_);
            return;
        }
        if (result == CopyResult::Failed || GetTickCount64() - copy_started_ >= 1500)
        {
            KillTimer(window_, CopyRetryTimer);
            pending_.reset();
            copying_ = false;
            update_buttons();
            set_status(L"Cannot copy the image. Try again.", true);
        }
    }

    void save_as_png()
    {
        fs::path destination;
        try
        {
            ComPtr<IFileSaveDialog> dialog;
            if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&dialog))))
                throw std::runtime_error("Cannot open the save dialog.");

            dialog->SetClientGuid(EditorSaveDialogGuid);
            constexpr COMDLG_FILTERSPEC filter{L"PNG image", L"*.png"};
            dialog->SetFileTypes(1, &filter);
            dialog->SetFileTypeIndex(1);
            dialog->SetDefaultExtension(L"png");
            dialog->SetTitle(L"Save screenshot");

            DWORD flags{};
            if (SUCCEEDED(dialog->GetOptions(&flags)))
            {
                dialog->SetOptions(flags | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST |
                                   FOS_NOREADONLYRETURN);
            }

            SYSTEMTIME local_time{};
            GetLocalTime(&local_time);
            const auto name = default_save_filename(local_time);
            dialog->SetFileName(name.c_str());

            const auto folder_path = default_save_folder();
            if (!folder_path.empty())
            {
                ComPtr<IShellItem> folder;
                if (SUCCEEDED(SHCreateItemFromParsingName(folder_path.c_str(), nullptr,
                                                          IID_PPV_ARGS(&folder))))
                    dialog->SetDefaultFolder(folder.Get());
            }

            saving_dialog_ = true;
            update_buttons();
            const HRESULT chosen = dialog->Show(window_);
            saving_dialog_ = false;
            update_buttons();

            if (chosen == HRESULT_FROM_WIN32(ERROR_CANCELLED))
                return;
            if (FAILED(chosen))
                throw std::runtime_error("Cannot open the save dialog.");

            ComPtr<IShellItem> item;
            if (FAILED(dialog->GetResult(&item)))
                throw std::runtime_error("Cannot read the selected file path.");
            PWSTR raw_path{};
            if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw_path)) || !raw_path)
                throw std::runtime_error("Cannot read the selected file path.");
            destination = raw_path;
            CoTaskMemFree(raw_path);
        }
        catch (const std::exception& error)
        {
            saving_dialog_ = false;
            update_buttons();
            set_status(wide_error(error.what()), true);
            return;
        }

        try
        {
            document_.save_png(destination);
            DestroyWindow(window_);
        }
        catch (const std::exception& error)
        {
            set_status(wide_error(error.what()), true);
        }
    }

    LRESULT handle(UINT message, WPARAM w, LPARAM l)
    {
        switch (message)
        {
        case WM_CREATE:
            create();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            paint();
            return 0;
        case WM_SIZE:
            layout();
            return 0;
        case WM_GETMINMAXINFO:
        {
            auto* info = reinterpret_cast<MINMAXINFO*>(l);
            RECT min_rect{0, 0, px(MinClientWidthDip), px(MinClientHeightDip)};
            AdjustWindowRectExForDpi(&min_rect, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, FALSE, 0, dpi_);
            info->ptMinTrackSize.x = min_rect.right - min_rect.left;
            info->ptMinTrackSize.y = min_rect.bottom - min_rect.top;
            return 0;
        }
        case WM_DRAWITEM:
        {
            const auto& item = *reinterpret_cast<const DRAWITEMSTRUCT*>(l);
            const bool primary = item.CtlID == Copy;
            const bool hovered = item.hwndItem == hovered_button_;
            draw_theme_button(item, palette_, background_, font_, dpi_, primary, hovered);
            return TRUE;
        }
        case WM_COMMAND:
            command(LOWORD(w));
            return 0;
        case WM_TIMER:
            if (w == CopyRetryTimer && copying_ && pending_)
                try_copy();
            return 0;
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            refresh_theme();
            return 0;
        case WM_DPICHANGED:
        {
            dpi_ = HIWORD(w);
            refresh_fonts();
            if (canvas_)
                canvas_->set_dpi(dpi_);
            const auto* suggested = reinterpret_cast<const RECT*>(l);
            SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER);
            layout();
            InvalidateRect(window_, nullptr, TRUE);
            return 0;
        }
        case WM_CLOSE:
            if (saving_dialog_)
                return 0;
            if (copying_)
            {
                KillTimer(window_, CopyRetryTimer);
                pending_.reset();
                copying_ = false;
            }
            DestroyWindow(window_);
            return 0;
        case WM_DESTROY:
            KillTimer(window_, CopyRetryTimer);
            canvas_.reset();
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window_, message, w, l);
    }

    static LRESULT CALLBACK button_procedure(HWND button, UINT message, WPARAM w, LPARAM l) noexcept
    {
        const auto parent = GetParent(button);
        auto* self = parent ? reinterpret_cast<Editor*>(GetWindowLongPtrW(parent, GWLP_USERDATA)) : nullptr;
        if (!self || !self->base_button_proc_)
            return DefWindowProcW(button, message, w, l);
        if (message == WM_MOUSEMOVE && self->hovered_button_ != button)
        {
            const auto previous = self->hovered_button_;
            self->hovered_button_ = button;
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, button, 0};
            TrackMouseEvent(&track);
            if (previous)
                InvalidateRect(previous, nullptr, FALSE);
            InvalidateRect(button, nullptr, FALSE);
        }
        else if (message == WM_MOUSELEAVE && self->hovered_button_ == button)
        {
            self->hovered_button_ = nullptr;
            InvalidateRect(button, nullptr, FALSE);
        }
        return CallWindowProcW(self->base_button_proc_, button, message, w, l);
    }

    static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM w, LPARAM l) noexcept
    {
        auto* self = reinterpret_cast<Editor*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<Editor*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            self->window_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self)
            return DefWindowProcW(hwnd, message, w, l);
        try
        {
            return self->handle(message, w, l);
        }
        catch (const std::exception& error)
        {
            MessageBoxA(hwnd, error.what(), "Pluck Editor", MB_OK | MB_ICONERROR);
            if (message == WM_CREATE)
                return -1;
            return 0;
        }
    }
};
} // namespace

int run_editor(HINSTANCE instance, std::unique_ptr<Bitmap> image, RECT selection_screen_rect)
{
    // Keep the named mutex open while any editor runs so updates wait before replacing Pluck.exe.
    const auto editor_mutex = CreateMutexW(nullptr, FALSE, EditorMutex);
    struct MutexGuard
    {
        HANDLE handle{};
        ~MutexGuard()
        {
            if (handle)
                CloseHandle(handle);
        }
    } mutex_guard{editor_mutex};

    const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(com_hr) && com_hr != RPC_E_CHANGED_MODE)
        throw std::runtime_error("Cannot start the editor.");
    struct ComGuard
    {
        bool active{};
        ~ComGuard()
        {
            if (active)
                CoUninitialize();
        }
    } com_guard{SUCCEEDED(com_hr)};

    Editor editor(instance, std::move(image));
    const auto window = editor.create_window(selection_screen_rect);

    MSG message{};
    BOOL result{};
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0)
    {
        if (message.message == WM_KEYDOWN && !editor.busy())
        {
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (ctrl && !alt && !shift && message.wParam == 'C')
            {
                editor.command(Editor::Copy);
                continue;
            }
            if (ctrl && !alt && !shift && message.wParam == 'S')
            {
                editor.command(Editor::Save);
                continue;
            }
            if (!ctrl && !alt && message.wParam == VK_ESCAPE)
            {
                SendMessageW(window, WM_CLOSE, 0, 0);
                continue;
            }
        }
        if (!IsDialogMessageW(window, &message))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return result == -1 ? 1 : static_cast<int>(message.wParam);
}
} // namespace pluck
