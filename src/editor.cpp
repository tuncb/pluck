#include "editor.h"
#include "editor_core.h"
#include "editor_icons.h"
#include "app_process.h"
#include "theme.h"
#include <d2d1.h>
#include <dwrite.h>
#include <shellscalingapi.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace pluck
{
namespace
{
using Microsoft::WRL::ComPtr;

constexpr wchar_t EditorToolbarClass[] = L"Pluck.Native.EditorToolbar";
constexpr wchar_t EditorCanvasClass[] = L"Pluck.Native.EditorCanvas";
constexpr wchar_t EditorTooltipClass[] = L"Pluck.Native.EditorTooltip";
constexpr GUID EditorSaveDialogGuid{
    0x4e7a9d31, 0x8b42, 0x4c95, {0xa6, 0x3d, 0x71, 0x2f, 0x58, 0x9c, 0x14, 0xe2}};

constexpr UINT_PTR CopyRetryTimer = 1;
constexpr UINT_PTR CaretBlinkTimer = 2;
constexpr UINT_PTR TooltipHoverTimer = 3;

constexpr int Row1HeightDip = 44;
constexpr int Row2HeightDip = 38;
constexpr int ToolbarHeightDip = Row1HeightDip + Row2HeightDip;
constexpr int CanvasPaddingDip = 20;
constexpr int MinClientWidthDip = 780;
constexpr int MinClientHeightDip = 360;
constexpr float MinZoom = 0.10f;
constexpr float MaxZoom = 8.0f;
constexpr COLORREF SelectionColor = RGB(29, 111, 242);

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

bool read_clipboard_text(HWND owner, std::wstring& out)
{
    if (!OpenClipboard(owner))
        return false;
    bool ok = false;
    if (const auto handle = GetClipboardData(CF_UNICODETEXT))
    {
        if (const auto* ptr = static_cast<const wchar_t*>(GlobalLock(handle)))
        {
            out = ptr;
            GlobalUnlock(handle);
            ok = true;
        }
    }
    CloseClipboard();
    return ok;
}

bool write_clipboard_text(HWND owner, const std::wstring& text)
{
    if (!OpenClipboard(owner))
        return false;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    auto memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (memory)
    {
        if (auto* dst = static_cast<wchar_t*>(GlobalLock(memory)))
        {
            std::copy(text.begin(), text.end(), dst);
            dst[text.size()] = L'\0';
            GlobalUnlock(memory);
            if (SetClipboardData(CF_UNICODETEXT, memory))
            {
                memory = nullptr;
                ok = true;
            }
        }
        if (memory)
            GlobalFree(memory);
    }
    CloseClipboard();
    return ok;
}

class TooltipPopup
{
  public:
    explicit TooltipPopup(HINSTANCE instance) : instance_(instance)
    {
        WNDCLASSW wc{};
        if (!GetClassInfoW(instance_, EditorTooltipClass, &wc))
        {
            wc.lpfnWndProc = procedure;
            wc.hInstance = instance_;
            wc.lpszClassName = EditorTooltipClass;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            RegisterClassW(&wc);
        }
        window_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, EditorTooltipClass,
                                  L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance_, this);
    }

    ~TooltipPopup()
    {
        if (font_)
            DeleteObject(font_);
        if (IsWindow(window_))
            DestroyWindow(window_);
    }

    TooltipPopup(const TooltipPopup&) = delete;
    TooltipPopup& operator=(const TooltipPopup&) = delete;

    void set_theme(const ThemePalette& palette, UINT dpi)
    {
        palette_ = palette;
        dpi_ = dpi;
        if (font_)
            DeleteObject(font_);
        font_ = CreateFontW(-MulDiv(12, static_cast<int>(dpi_), 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                            FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        if (visible_ && window_)
            InvalidateRect(window_, nullptr, FALSE);
    }

    void show(const std::wstring& text, POINT anchor_screen)
    {
        if (!window_ || text.empty())
            return;
        text_ = text;
        const auto dc = GetDC(window_);
        const auto old = SelectObject(dc, font_ ? font_ : GetStockObject(DEFAULT_GUI_FONT));
        SIZE sz{};
        GetTextExtentPoint32W(dc, text_.c_str(), static_cast<int>(text_.size()), &sz);
        SelectObject(dc, old);
        ReleaseDC(window_, dc);

        const int pad_x = MulDiv(9, static_cast<int>(dpi_), 96);
        const int pad_y = MulDiv(5, static_cast<int>(dpi_), 96);
        const int w = sz.cx + pad_x * 2;
        const int h = sz.cy + pad_y * 2;

        const auto monitor = MonitorFromPoint(anchor_screen, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(monitor, &info);
        const int x = std::clamp<int>(anchor_screen.x - w / 2, info.rcWork.left + 4,
                                      std::max<int>(info.rcWork.left + 4, info.rcWork.right - w - 4));
        const int y = std::clamp<int>(anchor_screen.y, info.rcWork.top + 4,
                                      std::max<int>(info.rcWork.top + 4, info.rcWork.bottom - h - 4));
        SetWindowPos(window_, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        visible_ = true;
        InvalidateRect(window_, nullptr, FALSE);
    }

    void hide() noexcept
    {
        if (visible_ && window_)
            ShowWindow(window_, SW_HIDE);
        visible_ = false;
    }

  private:
    HINSTANCE instance_{};
    HWND window_{};
    HFONT font_{};
    ThemePalette palette_{};
    UINT dpi_{96};
    std::wstring text_;
    bool visible_{};

    LRESULT handle(UINT message, WPARAM w, LPARAM l)
    {
        if (message == WM_ERASEBKGND)
            return 1;
        if (message == WM_PAINT)
        {
            PAINTSTRUCT ps{};
            const auto dc = BeginPaint(window_, &ps);
            RECT r{};
            GetClientRect(window_, &r);
            const auto bg = CreateSolidBrush(palette_.canvas);
            const auto pen = CreatePen(PS_SOLID, 1, palette_.line);
            const auto old_b = SelectObject(dc, bg);
            const auto old_p = SelectObject(dc, pen);
            Rectangle(dc, r.left, r.top, r.right, r.bottom);
            SelectObject(dc, old_b);
            SelectObject(dc, old_p);
            DeleteObject(bg);
            DeleteObject(pen);

            const auto old_f = SelectObject(dc, font_ ? font_ : GetStockObject(DEFAULT_GUI_FONT));
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, palette_.ink);
            DrawTextW(dc, text_.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, old_f);
            EndPaint(window_, &ps);
            return 0;
        }
        return DefWindowProcW(window_, message, w, l);
    }

    static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM w, LPARAM l) noexcept
    {
        auto* self = reinterpret_cast<TooltipPopup*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<TooltipPopup*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            self->window_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self)
            return DefWindowProcW(hwnd, message, w, l);
        return self->handle(message, w, l);
    }
};

struct EditorViewState
{
    ToolKind tool{ToolKind::Pen};
    COLORREF ink_color{DefaultInkColor};
    COLORREF highlighter_color{DefaultHighlighterColor};
    COLORREF redact_color{DefaultRedactColor};
    SizePreset ink_size{SizePreset::Medium};
    SizePreset highlighter_size{SizePreset::Medium};
    SizePreset redact_size{SizePreset::Medium};
    bool shape_filled{false};
    bool text_background{false};
    bool redact_solid{false};

    std::uint64_t selected_id{0};

    bool text_editing{false};
    std::uint64_t text_edit_id{0};
    Annotation text_draft{};
    size_t text_caret{0};
    size_t text_anchor{0};
    bool caret_visible{true};

    bool crop_active{false};
    RECT crop_draft{};

    bool fit_mode{true};
    float zoom{1.0f};
    int display_zoom_percent{100};
    float pan_x{0.0f};
    float pan_y{0.0f};

    bool busy{false};
    std::wstring status_text;
    bool status_error{false};
};

enum class ActionId
{
    None,
    ToolSelect,
    ToolPen,
    ToolHighlighter,
    ToolArrow,
    ToolRectangle,
    ToolOval,
    ToolLine,
    ToolText,
    ToolStep,
    ToolRedact,
    ToolCrop,
    Undo,
    Redo,
    Reset,
    Save,
    Copy,
    Color0,
    Color1,
    Color2,
    Color3,
    Color4,
    Color5,
    Color6,
    Color7,
    SizeS,
    SizeM,
    SizeL,
    SizeXL,
    ToggleFill,
    ToggleTextBackground,
    RedactPixelate,
    RedactSolid,
    ResetStep,
    DeleteSelected,
    CropApply,
    CropReset,
    CropCancel,
    ZoomOut,
    ZoomActual,
    ZoomIn,
    ZoomFit
};

struct ToolbarControl
{
    ActionId id{ActionId::None};
    D2D1_RECT_F rect{};
    ToolbarIcon icon{ToolbarIcon::Select};
    bool has_icon{};
    bool is_color{};
    COLORREF swatch{};
    bool has_accent_bar{};
    COLORREF accent_bar{};
    std::wstring label;
    std::wstring tooltip;
    bool active{};
    bool primary{};
    bool pill{};
    bool enabled{true};
};

class EditorToolbar
{
  public:
    EditorToolbar(HINSTANCE instance, HWND parent, const EditorDocument& document,
                  const EditorViewState& state, const ThemePalette& palette, UINT dpi,
                  std::function<void(ActionId)> on_action)
        : instance_(instance), document_(document), state_(state), palette_(palette), dpi_(dpi),
          on_action_(std::move(on_action)), tooltip_(instance)
    {
        WNDCLASSW wc{};
        if (!GetClassInfoW(instance_, EditorToolbarClass, &wc))
        {
            wc.lpfnWndProc = procedure;
            wc.hInstance = instance_;
            wc.lpszClassName = EditorToolbarClass;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            if (!RegisterClassW(&wc))
                throw std::runtime_error("Cannot prepare the editor toolbar.");
        }
        window_ = CreateWindowExW(0, EditorToolbarClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, parent,
                                  nullptr, instance_, this);
        if (!window_)
            throw std::runtime_error("Cannot open the editor toolbar.");
        tooltip_.set_theme(palette_, dpi_);
    }

    ~EditorToolbar()
    {
        tooltip_.hide();
        discard_device_resources();
        if (IsWindow(window_))
            DestroyWindow(window_);
    }

    EditorToolbar(const EditorToolbar&) = delete;
    EditorToolbar& operator=(const EditorToolbar&) = delete;

    void set_theme(const ThemePalette& palette)
    {
        palette_ = palette;
        tooltip_.set_theme(palette_, dpi_);
        invalidate();
    }

    void set_dpi(UINT dpi)
    {
        dpi_ = dpi;
        text_format_.Reset();
        small_format_.Reset();
        tooltip_.set_theme(palette_, dpi_);
        invalidate();
    }

    void resize(int x, int y, int width, int height)
    {
        MoveWindow(window_, x, y, std::max(0, width), std::max(0, height), TRUE);
        invalidate();
    }

    void invalidate() noexcept
    {
        if (window_)
            InvalidateRect(window_, nullptr, FALSE);
    }

    void hide_tooltip() noexcept
    {
        tooltip_.hide();
        if (window_)
            KillTimer(window_, TooltipHoverTimer);
    }

  private:
    HINSTANCE instance_{};
    const EditorDocument& document_;
    const EditorViewState& state_;
    ThemePalette palette_{};
    UINT dpi_{96};
    std::function<void(ActionId)> on_action_;
    TooltipPopup tooltip_;
    HWND window_{};
    ComPtr<ID2D1Factory> factory_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<ID2D1HwndRenderTarget> target_;
    ComPtr<ID2D1StrokeStyle> round_style_;
    ComPtr<IDWriteTextFormat> text_format_;
    ComPtr<IDWriteTextFormat> small_format_;
    std::vector<ToolbarControl> controls_;
    std::vector<std::pair<float, std::pair<float, float>>> dividers_;
    D2D1_RECT_F context_hint_rect_{};
    std::wstring context_hint_;
    D2D1_RECT_F status_rect_{};
    D2D1_RECT_F dims_rect_{};
    ActionId hovered_{ActionId::None};
    ActionId pressed_{ActionId::None};

    float px(float dip) const noexcept
    {
        return dip * static_cast<float>(dpi_) / 96.0f;
    }

    void discard_device_resources() noexcept
    {
        target_.Reset();
    }

    bool ensure_resources(UINT w, UINT h)
    {
        if (!factory_)
        {
            if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf())))
                return false;
            const auto props = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                           D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
            factory_->CreateStrokeStyle(props, nullptr, 0, &round_style_);
        }
        if (!dwrite_)
        {
            DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()));
        }
        if (!text_format_ && dwrite_)
        {
            dwrite_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px(12.5f), L"",
                                      &text_format_);
            if (text_format_)
            {
                text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                text_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            }
        }
        if (!small_format_ && dwrite_)
        {
            dwrite_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px(11.8f), L"",
                                      &small_format_);
            if (small_format_)
            {
                small_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                small_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            }
        }
        if (!target_)
        {
            const auto rt_props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
            const auto hwnd_props = D2D1::HwndRenderTargetProperties(window_, D2D1::SizeU(w, h));
            if (FAILED(factory_->CreateHwndRenderTarget(rt_props, hwnd_props, &target_)))
                return false;
        }
        else
        {
            const auto sz = target_->GetPixelSize();
            if (sz.width != w || sz.height != h)
            {
                const auto hr = target_->Resize(D2D1::SizeU(w, h));
                if (hr == D2DERR_RECREATE_TARGET)
                {
                    discard_device_resources();
                    return ensure_resources(w, h);
                }
                if (FAILED(hr))
                    return false;
            }
        }
        return true;
    }

    COLORREF effective_color() const noexcept
    {
        if (state_.text_editing)
            return state_.text_draft.color;
        if (const auto* sel = document_.find_annotation(state_.selected_id))
            return sel->color;
        if (state_.tool == ToolKind::Highlighter)
            return state_.highlighter_color;
        if (state_.tool == ToolKind::Redact)
            return state_.redact_color;
        return state_.ink_color;
    }

    SizePreset effective_size() const noexcept
    {
        if (state_.text_editing)
            return state_.text_draft.size;
        if (const auto* sel = document_.find_annotation(state_.selected_id))
            return sel->size;
        if (state_.tool == ToolKind::Highlighter)
            return state_.highlighter_size;
        if (state_.tool == ToolKind::Redact)
            return state_.redact_size;
        return state_.ink_size;
    }

    void rebuild_layout(float client_w)
    {
        controls_.clear();
        dividers_.clear();
        context_hint_.clear();
        context_hint_rect_ = {};
        status_rect_ = {};
        dims_rect_ = {};

        const float r1_h = px(static_cast<float>(Row1HeightDip));
        const float r2_h = px(static_cast<float>(Row2HeightDip));
        const float pad_x = px(12.0f);
        const float btn_s = px(30.0f);
        const float btn_gap = px(2.0f);
        const float div_gap = px(10.0f);
        const float r1_y = (r1_h - btn_s) * 0.5f;

        float x = pad_x;

        const auto add_div = [&](float dx, float y0, float y1)
        { dividers_.push_back({dx, {y0, y1}}); };

        const auto add_tool = [&](ActionId id, ToolbarIcon icon, ToolKind kind, const wchar_t* tip,
                                  bool has_bar = false, COLORREF bar_color = 0)
        {
            ToolbarControl c{};
            c.id = id;
            c.rect = D2D1::RectF(x, r1_y, x + btn_s, r1_y + btn_s);
            c.icon = icon;
            c.has_icon = true;
            c.tooltip = tip;
            c.active = (state_.tool == kind && !state_.crop_active) ||
                       (kind == ToolKind::Crop && state_.crop_active);
            c.enabled = !state_.busy;
            c.has_accent_bar = has_bar;
            c.accent_bar = bar_color;
            controls_.push_back(std::move(c));
            x += btn_s + btn_gap;
        };

        // Row 1 Left: Markup tools
        add_tool(ActionId::ToolSelect, ToolbarIcon::Select, ToolKind::Select, L"Select / Move (V)");
        x += div_gap * 0.4f;
        add_div(x, px(10.0f), r1_h - px(10.0f));
        x += div_gap * 0.6f;

        add_tool(ActionId::ToolPen, ToolbarIcon::Pen, ToolKind::Pen, L"Pen (P) · Shift for straight", true,
                 state_.ink_color);
        add_tool(ActionId::ToolHighlighter, ToolbarIcon::Highlighter, ToolKind::Highlighter,
                 L"Highlighter (H) · Shift for straight", true, state_.highlighter_color);
        x += div_gap * 0.4f;
        add_div(x, px(10.0f), r1_h - px(10.0f));
        x += div_gap * 0.6f;

        add_tool(ActionId::ToolArrow, ToolbarIcon::Arrow, ToolKind::Arrow, L"Arrow (A) · Shift for 45°");
        add_tool(ActionId::ToolRectangle, ToolbarIcon::Rectangle, ToolKind::Rectangle,
                 L"Rectangle (R) · Shift for square");
        add_tool(ActionId::ToolOval, ToolbarIcon::Oval, ToolKind::Oval, L"Oval (O) · Shift for circle");
        add_tool(ActionId::ToolLine, ToolbarIcon::Line, ToolKind::Line, L"Line (L) · Shift for 45°");
        x += div_gap * 0.4f;
        add_div(x, px(10.0f), r1_h - px(10.0f));
        x += div_gap * 0.6f;

        add_tool(ActionId::ToolText, ToolbarIcon::Text, ToolKind::Text, L"Text (T)");
        add_tool(ActionId::ToolStep, ToolbarIcon::Step, ToolKind::Step, L"Step number (N)");
        add_tool(ActionId::ToolRedact, ToolbarIcon::Redact, ToolKind::Redact, L"Redact / Pixelate (B)");
        add_tool(ActionId::ToolCrop, ToolbarIcon::Crop, ToolKind::Crop, L"Crop (C)");

        // Row 1 Right: Undo, Redo, Clear | Save PNG, Copy
        const float copy_w = px(72.0f);
        const float save_w = px(84.0f);
        const float action_gap = px(8.0f);
        float rx = client_w - pad_x;

        {
            ToolbarControl c{};
            c.id = ActionId::Copy;
            c.rect = D2D1::RectF(rx - copy_w, r1_y, rx, r1_y + btn_s);
            c.label = L"Copy";
            c.tooltip = L"Copy to clipboard (Ctrl+C)";
            c.primary = true;
            c.enabled = !state_.busy;
            controls_.push_back(std::move(c));
            rx -= copy_w + action_gap;
        }
        {
            ToolbarControl c{};
            c.id = ActionId::Save;
            c.rect = D2D1::RectF(rx - save_w, r1_y, rx, r1_y + btn_s);
            c.label = L"Save PNG";
            c.tooltip = L"Save PNG (Ctrl+S)";
            c.enabled = !state_.busy;
            controls_.push_back(std::move(c));
            rx -= save_w + div_gap * 0.6f;
        }
        add_div(rx, px(10.0f), r1_h - px(10.0f));
        rx -= div_gap * 0.6f;

        const auto add_right_icon = [&](ActionId id, ToolbarIcon icon, const wchar_t* tip, bool enabled)
        {
            ToolbarControl c{};
            c.id = id;
            c.rect = D2D1::RectF(rx - btn_s, r1_y, rx, r1_y + btn_s);
            c.icon = icon;
            c.has_icon = true;
            c.tooltip = tip;
            c.enabled = enabled && !state_.busy;
            controls_.push_back(std::move(c));
            rx -= btn_s + btn_gap;
        };

        add_right_icon(ActionId::Reset, ToolbarIcon::Reset, L"Reset all edits",
                       !document_.annotations().empty() || document_.is_cropped());
        add_right_icon(ActionId::Redo, ToolbarIcon::Redo, L"Redo (Ctrl+Y)", document_.can_redo());
        add_right_icon(ActionId::Undo, ToolbarIcon::Undo, L"Undo (Ctrl+Z)", document_.can_undo());

        // Row 2 Left: Crop controls OR Style + Context controls
        const float r2_top = r1_h;
        const float pill_h = px(24.0f);
        const float pill_y = r2_top + (r2_h - pill_h) * 0.5f;
        float lx = pad_x;

        if (state_.crop_active)
        {
            const auto add_pill = [&](ActionId id, const wchar_t* label, float dip_w, bool primary,
                                      bool enabled, const wchar_t* tip)
            {
                const float w = px(dip_w);
                ToolbarControl c{};
                c.id = id;
                c.rect = D2D1::RectF(lx, pill_y, lx + w, pill_y + pill_h);
                c.label = label;
                c.tooltip = tip;
                c.primary = primary;
                c.enabled = enabled && !state_.busy;
                controls_.push_back(std::move(c));
                lx += w + px(6.0f);
            };
            add_pill(ActionId::CropApply, L"Apply crop", 80.0f, true, true, L"Apply crop (Enter)");
            const bool is_full = state_.crop_draft.left == 0 && state_.crop_draft.top == 0 &&
                                 state_.crop_draft.right == document_.base_width() &&
                                 state_.crop_draft.bottom == document_.base_height();
            add_pill(ActionId::CropReset, L"Reset", 54.0f, false, !is_full, L"Reset crop to full image");
            add_pill(ActionId::CropCancel, L"Cancel", 58.0f, false, true, L"Cancel crop (Esc)");
            const int cw = std::max(0, static_cast<int>(state_.crop_draft.right - state_.crop_draft.left));
            const int ch = std::max(0, static_cast<int>(state_.crop_draft.bottom - state_.crop_draft.top));
            context_hint_ = std::to_wstring(cw) + L" \u00d7 " + std::to_wstring(ch) + L" px";
            context_hint_rect_ = D2D1::RectF(lx + px(6.0f), r2_top, lx + px(180.0f), r2_top + r2_h);
        }
        else
        {
            const COLORREF cur_color = effective_color();
            const float swatch_s = px(20.0f);
            const float swatch_y = r2_top + (r2_h - swatch_s) * 0.5f;
            static constexpr const wchar_t* ColorNames[8]{
                L"Red", L"Orange", L"Yellow", L"Emerald", L"Blue", L"Violet", L"White", L"Charcoal"};
            for (size_t i = 0; i < MarkupColors.size(); ++i)
            {
                ToolbarControl c{};
                c.id = static_cast<ActionId>(static_cast<int>(ActionId::Color0) + static_cast<int>(i));
                c.rect = D2D1::RectF(lx, swatch_y, lx + swatch_s, swatch_y + swatch_s);
                c.is_color = true;
                c.swatch = MarkupColors[i];
                c.active = (cur_color == MarkupColors[i]);
                c.tooltip = ColorNames[i];
                c.enabled = !state_.busy;
                controls_.push_back(std::move(c));
                lx += swatch_s + px(4.0f);
            }

            lx += px(4.0f);
            add_div(lx, r2_top + px(8.0f), r2_top + r2_h - px(8.0f));
            lx += px(8.0f);

            const auto* selected = document_.find_annotation(state_.selected_id);
            const bool is_shape_ctx =
                selected ? (selected->kind == AnnotationKind::Rectangle || selected->kind == AnnotationKind::Oval)
                         : (state_.tool == ToolKind::Rectangle || state_.tool == ToolKind::Oval);
            const bool is_text_ctx =
                state_.text_editing || (selected ? selected->kind == AnnotationKind::Text : state_.tool == ToolKind::Text);
            const bool is_redact_ctx =
                selected ? selected->kind == AnnotationKind::Redact : state_.tool == ToolKind::Redact;
            const bool is_step_selected_or_tool =
                selected ? selected->kind == AnnotationKind::Step : state_.tool == ToolKind::Step;
            const bool is_step_ctx = !selected && state_.tool == ToolKind::Step;
            const bool is_thickness_ctx = !is_text_ctx && !is_redact_ctx && !is_step_selected_or_tool;

            const SizePreset cur_size = effective_size();
            static constexpr const wchar_t* SizeLabels[4]{L"S", L"M", L"L", L"XL"};
            static constexpr const wchar_t* ThicknessTips[4]{L"Thin ([ / ])", L"Medium ([ / ])",
                                                             L"Thick ([ / ])", L"Extra thick ([ / ])"};
            static constexpr ToolbarIcon ThicknessIcons[4]{
                ToolbarIcon::StrokeThin, ToolbarIcon::StrokeMedium, ToolbarIcon::StrokeThick,
                ToolbarIcon::StrokeExtraThick};
            const float size_w = px(26.0f);
            for (int i = 0; i < 4; ++i)
            {
                ToolbarControl c{};
                c.id = static_cast<ActionId>(static_cast<int>(ActionId::SizeS) + i);
                c.rect = D2D1::RectF(lx, pill_y, lx + size_w, pill_y + pill_h);
                c.pill = true;
                if (is_thickness_ctx)
                {
                    c.has_icon = true;
                    c.icon = ThicknessIcons[i];
                    c.tooltip = ThicknessTips[i];
                }
                else
                {
                    c.label = SizeLabels[i];
                    c.tooltip = std::wstring(L"Size ") + SizeLabels[i] + L" ([ / ])";
                }
                c.active = (static_cast<int>(cur_size) == i);
                c.enabled = !state_.busy;
                controls_.push_back(std::move(c));
                lx += size_w + px(2.0f);
            }

            lx += px(6.0f);
            add_div(lx, r2_top + px(8.0f), r2_top + r2_h - px(8.0f));
            lx += px(8.0f);

            const auto add_toggle = [&](ActionId id, const wchar_t* label, float dip_w, bool active,
                                        const wchar_t* tip)
            {
                const float w = px(dip_w);
                ToolbarControl c{};
                c.id = id;
                c.rect = D2D1::RectF(lx, pill_y, lx + w, pill_y + pill_h);
                c.label = label;
                c.active = active;
                c.tooltip = tip;
                c.enabled = !state_.busy;
                controls_.push_back(std::move(c));
                lx += w + px(4.0f);
            };

            if (is_shape_ctx)
            {
                const bool filled = selected ? selected->filled : state_.shape_filled;
                add_toggle(ActionId::ToggleFill, L"Fill", 46.0f, filled, L"Toggle translucent shape fill");
            }
            else if (is_text_ctx)
            {
                const bool bg = state_.text_editing ? state_.text_draft.filled
                                                    : (selected ? selected->filled : state_.text_background);
                add_toggle(ActionId::ToggleTextBackground, L"Background", 78.0f, bg,
                           L"Toggle text background");
            }
            else if (is_redact_ctx)
            {
                const bool solid = selected ? selected->filled : state_.redact_solid;
                add_toggle(ActionId::RedactPixelate, L"Pixelate", 62.0f, !solid, L"Mosaic pixelation");
                add_toggle(ActionId::RedactSolid, L"Solid", 48.0f, solid, L"Solid color cover");
            }
            else if (is_step_ctx)
            {
                context_hint_ = L"Next #" + std::to_wstring(document_.next_step_number());
                context_hint_rect_ = D2D1::RectF(lx, r2_top, lx + px(54.0f), r2_top + r2_h);
                lx += px(56.0f);
                if (document_.next_step_number() > 1)
                    add_toggle(ActionId::ResetStep, L"Reset", 50.0f, false, L"Reset next step to #1");
            }

            if (selected && !state_.text_editing)
            {
                add_toggle(ActionId::DeleteSelected, L"Delete", 54.0f, false, L"Delete selected (Del)");
            }
            else if (!is_shape_ctx && !is_text_ctx && !is_redact_ctx && !is_step_ctx)
            {
                if (state_.tool == ToolKind::Pen || state_.tool == ToolKind::Highlighter)
                    context_hint_ = L"Shift: straight";
                else if (state_.tool == ToolKind::Arrow || state_.tool == ToolKind::Line)
                    context_hint_ = L"Shift: 45\u00b0";
                else if (state_.tool == ToolKind::Select)
                    context_hint_ = L"Click to select";
                context_hint_rect_ = D2D1::RectF(lx, r2_top, lx + px(110.0f), r2_top + r2_h);
            }
        }

        // Row 2 Right: Zoom controls + Dimensions + Status
        float r2x = client_w - pad_x;
        const float zoom_btn = px(24.0f);
        const float fit_w = px(32.0f);
        const float pct_w = px(44.0f);

        {
            ToolbarControl c{};
            c.id = ActionId::ZoomFit;
            c.rect = D2D1::RectF(r2x - fit_w, pill_y, r2x, pill_y + pill_h);
            c.label = L"Fit";
            c.active = state_.fit_mode;
            c.tooltip = L"Fit to window (0)";
            c.enabled = !state_.busy;
            controls_.push_back(std::move(c));
            r2x -= fit_w + px(3.0f);
        }
        {
            ToolbarControl c{};
            c.id = ActionId::ZoomIn;
            c.rect = D2D1::RectF(r2x - zoom_btn, pill_y, r2x, pill_y + pill_h);
            c.icon = ToolbarIcon::ZoomIn;
            c.has_icon = true;
            c.tooltip = L"Zoom in (+)";
            c.enabled = !state_.busy && state_.zoom < MaxZoom - 0.01f;
            controls_.push_back(std::move(c));
            r2x -= zoom_btn + px(2.0f);
        }
        {
            ToolbarControl c{};
            c.id = ActionId::ZoomActual;
            c.rect = D2D1::RectF(r2x - pct_w, pill_y, r2x, pill_y + pill_h);
            c.label = std::to_wstring(state_.display_zoom_percent) + L"%";
            c.active = !state_.fit_mode && state_.display_zoom_percent == 100;
            c.tooltip = L"Actual size 100% (1)";
            c.enabled = !state_.busy;
            controls_.push_back(std::move(c));
            r2x -= pct_w + px(2.0f);
        }
        {
            ToolbarControl c{};
            c.id = ActionId::ZoomOut;
            c.rect = D2D1::RectF(r2x - zoom_btn, pill_y, r2x, pill_y + pill_h);
            c.icon = ToolbarIcon::ZoomOut;
            c.has_icon = true;
            c.tooltip = L"Zoom out (-)";
            c.enabled = !state_.busy && state_.zoom > MinZoom + 0.01f;
            controls_.push_back(std::move(c));
            r2x -= zoom_btn + px(8.0f);
        }
        add_div(r2x, r2_top + px(8.0f), r2_top + r2_h - px(8.0f));
        r2x -= px(8.0f);

        const float dims_w = px(94.0f);
        dims_rect_ = D2D1::RectF(std::max(lx + px(8.0f), r2x - dims_w), r2_top, r2x, r2_top + r2_h);
        const float left_limit = context_hint_.empty() ? (lx + px(8.0f)) : (context_hint_rect_.right + px(6.0f));
        if (dims_rect_.left > left_limit + px(36.0f))
        {
            status_rect_ = D2D1::RectF(left_limit, r2_top, dims_rect_.left - px(8.0f), r2_top + r2_h);
        }
    }

    const ToolbarControl* hit_test(float x, float y) const noexcept
    {
        for (const auto& c : controls_)
        {
            if (x >= c.rect.left && x <= c.rect.right && y >= c.rect.top && y <= c.rect.bottom)
                return &c;
        }
        return nullptr;
    }

    void paint()
    {
        PAINTSTRUCT ps{};
        BeginPaint(window_, &ps);
        RECT rc{};
        GetClientRect(window_, &rc);
        const UINT w = static_cast<UINT>(std::max<LONG>(1, rc.right - rc.left));
        const UINT h = static_cast<UINT>(std::max<LONG>(1, rc.bottom - rc.top));
        if (!ensure_resources(w, h))
        {
            EndPaint(window_, &ps);
            return;
        }

        rebuild_layout(static_cast<float>(w));

        ComPtr<ID2D1SolidColorBrush> ink_brush;
        ComPtr<ID2D1SolidColorBrush> muted_brush;
        ComPtr<ID2D1SolidColorBrush> line_brush;
        ComPtr<ID2D1SolidColorBrush> accent_brush;
        ComPtr<ID2D1SolidColorBrush> accent_hover_brush;
        ComPtr<ID2D1SolidColorBrush> accent_ink_brush;
        ComPtr<ID2D1SolidColorBrush> button_brush;
        ComPtr<ID2D1SolidColorBrush> button_hover_brush;
        ComPtr<ID2D1SolidColorBrush> active_bg_brush;
        ComPtr<ID2D1SolidColorBrush> error_brush;

        target_->CreateSolidColorBrush(to_d2d_color(palette_.ink), &ink_brush);
        target_->CreateSolidColorBrush(to_d2d_color(palette_.muted), &muted_brush);
        target_->CreateSolidColorBrush(to_d2d_color(palette_.line), &line_brush);
        target_->CreateSolidColorBrush(to_d2d_color(palette_.accent), &accent_brush);
        target_->CreateSolidColorBrush(to_d2d_color(palette_.accent_hover), &accent_hover_brush);
        target_->CreateSolidColorBrush(to_d2d_color(palette_.accent_ink), &accent_ink_brush);
        target_->CreateSolidColorBrush(to_d2d_color(palette_.button), &button_brush);
        target_->CreateSolidColorBrush(to_d2d_color(palette_.button_hover), &button_hover_brush);
        target_->CreateSolidColorBrush(to_d2d_color(palette_.accent, palette_.dark ? 0.22f : 0.42f),
                                       &active_bg_brush);
        target_->CreateSolidColorBrush(to_d2d_color(palette_.error_ink), &error_brush);

        target_->BeginDraw();
        target_->Clear(to_d2d_color(palette_.canvas));

        // Row separator and bottom border
        const float r1_h = std::floor(px(static_cast<float>(Row1HeightDip))) + 0.5f;
        target_->DrawLine(D2D1::Point2F(0.0f, r1_h), D2D1::Point2F(static_cast<float>(w), r1_h),
                          line_brush.Get(), 1.0f);
        const float bottom_y = static_cast<float>(h) - 0.5f;
        target_->DrawLine(D2D1::Point2F(0.0f, bottom_y), D2D1::Point2F(static_cast<float>(w), bottom_y),
                          line_brush.Get(), 1.0f);

        for (const auto& div : dividers_)
        {
            const float dx = std::floor(div.first) + 0.5f;
            target_->DrawLine(D2D1::Point2F(dx, div.second.first), D2D1::Point2F(dx, div.second.second),
                              line_brush.Get(), 1.0f);
        }

        const float dpi_scale = static_cast<float>(dpi_) / 96.0f;
        for (const auto& c : controls_)
        {
            const bool is_hover = c.enabled && hovered_ == c.id;
            const bool is_press = c.enabled && pressed_ == c.id;

            if (c.is_color)
            {
                const float cx = (c.rect.left + c.rect.right) * 0.5f;
                const float cy = (c.rect.top + c.rect.bottom) * 0.5f;
                const float r = (c.rect.right - c.rect.left) * 0.5f - px(2.5f);
                if (c.active || is_hover)
                {
                    target_->DrawEllipse(
                        D2D1::Ellipse(D2D1::Point2F(cx, cy), r + px(2.3f), r + px(2.3f)),
                        c.active ? (palette_.high_contrast ? ink_brush.Get() : accent_brush.Get())
                                 : muted_brush.Get(),
                        px(c.active ? 1.8f : 1.2f));
                }
                ComPtr<ID2D1SolidColorBrush> swatch_brush;
                target_->CreateSolidColorBrush(to_d2d_color(c.swatch, c.enabled ? 1.0f : 0.4f), &swatch_brush);
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), swatch_brush.Get());
                target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), line_brush.Get(), 1.0f);
                continue;
            }

            const float radius = px(6.0f);
            const auto rounded = D2D1::RoundedRect(c.rect, radius, radius);
            if (c.primary && c.enabled)
            {
                target_->FillRoundedRectangle(rounded,
                                              is_hover ? accent_hover_brush.Get() : accent_brush.Get());
            }
            else if (c.active && c.enabled)
            {
                target_->FillRoundedRectangle(rounded, active_bg_brush.Get());
                target_->DrawRoundedRectangle(rounded,
                                              palette_.high_contrast ? ink_brush.Get() : accent_brush.Get(),
                                              px(1.25f));
            }
            else if (is_press || is_hover || !c.has_icon || c.pill)
            {
                ID2D1Brush* bg = (is_hover || is_press) ? button_hover_brush.Get() : button_brush.Get();
                if (!c.has_icon || c.pill || is_hover || is_press)
                {
                    target_->FillRoundedRectangle(rounded, bg);
                    if (!c.has_icon || c.pill)
                        target_->DrawRoundedRectangle(rounded, line_brush.Get(), 1.0f);
                }
            }

            ID2D1Brush* fg = !c.enabled ? muted_brush.Get()
                                        : (c.primary ? accent_ink_brush.Get() : ink_brush.Get());
            if (c.has_icon)
            {
                D2D1_RECT_F icon_box = c.rect;
                const float inset = px(5.0f);
                icon_box.left += inset;
                icon_box.top += inset;
                icon_box.right -= inset;
                icon_box.bottom -= inset;
                if (is_press)
                {
                    icon_box.top += px(0.8f);
                    icon_box.bottom += px(0.8f);
                }
                ComPtr<ID2D1SolidColorBrush> bar_brush;
                if (c.has_accent_bar)
                    target_->CreateSolidColorBrush(to_d2d_color(c.accent_bar, c.enabled ? 1.0f : 0.4f),
                                                   &bar_brush);
                draw_toolbar_icon(target_.Get(), factory_.Get(), dwrite_.Get(), c.icon, icon_box, fg,
                                  bar_brush.Get(), round_style_.Get(), dpi_scale);
            }
            else if (!c.label.empty() && text_format_)
            {
                D2D1_RECT_F tr = c.rect;
                if (is_press)
                {
                    tr.top += px(0.8f);
                    tr.bottom += px(0.8f);
                }
                target_->DrawTextW(c.label.c_str(), static_cast<UINT32>(c.label.size()), text_format_.Get(),
                                   tr, fg);
            }
        }

        if (small_format_)
        {
            if (!context_hint_.empty() && context_hint_rect_.right > context_hint_rect_.left)
            {
                small_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                target_->DrawTextW(context_hint_.c_str(), static_cast<UINT32>(context_hint_.size()),
                                   small_format_.Get(), context_hint_rect_, muted_brush.Get());
            }
            if (dims_rect_.right > dims_rect_.left)
            {
                const std::wstring dims = std::to_wstring(document_.width()) + L" \u00d7 " +
                                          std::to_wstring(document_.height()) + L" px";
                small_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                target_->DrawTextW(dims.c_str(), static_cast<UINT32>(dims.size()), small_format_.Get(),
                                   dims_rect_, muted_brush.Get());
            }
            if (!state_.status_text.empty() && status_rect_.right > status_rect_.left)
            {
                small_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                target_->DrawTextW(state_.status_text.c_str(), static_cast<UINT32>(state_.status_text.size()),
                                   small_format_.Get(), status_rect_,
                                   state_.status_error ? error_brush.Get() : ink_brush.Get(),
                                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }

        const auto hr = target_->EndDraw();
        EndPaint(window_, &ps);
        if (hr == D2DERR_RECREATE_TARGET)
        {
            discard_device_resources();
            invalidate();
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
            invalidate();
            return 0;
        case WM_MOUSEMOVE:
        {
            const float x = static_cast<float>(GET_X_LPARAM(l));
            const float y = static_cast<float>(GET_Y_LPARAM(l));
            const auto* hit = hit_test(x, y);
            const ActionId next = hit ? hit->id : ActionId::None;
            if (next != hovered_)
            {
                hovered_ = next;
                tooltip_.hide();
                KillTimer(window_, TooltipHoverTimer);
                if (hit && !hit->tooltip.empty())
                    SetTimer(window_, TooltipHoverTimer, 340, nullptr);
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0};
                TrackMouseEvent(&track);
                invalidate();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            hovered_ = ActionId::None;
            pressed_ = ActionId::None;
            hide_tooltip();
            invalidate();
            return 0;
        case WM_TIMER:
            if (w == TooltipHoverTimer)
            {
                KillTimer(window_, TooltipHoverTimer);
                for (const auto& c : controls_)
                {
                    if (c.id == hovered_ && !c.tooltip.empty())
                    {
                        POINT pt{static_cast<LONG>((c.rect.left + c.rect.right) * 0.5f),
                                 static_cast<LONG>(c.rect.bottom + px(6.0f))};
                        ClientToScreen(window_, &pt);
                        tooltip_.show(c.tooltip, pt);
                        break;
                    }
                }
            }
            return 0;
        case WM_LBUTTONDOWN:
        {
            hide_tooltip();
            const float x = static_cast<float>(GET_X_LPARAM(l));
            const float y = static_cast<float>(GET_Y_LPARAM(l));
            if (const auto* hit = hit_test(x, y); hit && hit->enabled)
            {
                pressed_ = hit->id;
                SetCapture(window_);
                invalidate();
            }
            return 0;
        }
        case WM_LBUTTONUP:
        {
            const ActionId was_pressed = pressed_;
            pressed_ = ActionId::None;
            if (GetCapture() == window_)
                ReleaseCapture();
            const float x = static_cast<float>(GET_X_LPARAM(l));
            const float y = static_cast<float>(GET_Y_LPARAM(l));
            if (const auto* hit = hit_test(x, y); hit && hit->enabled && hit->id == was_pressed)
            {
                if (on_action_)
                    on_action_(hit->id);
            }
            invalidate();
            return 0;
        }
        case WM_CAPTURECHANGED:
            pressed_ = ActionId::None;
            invalidate();
            return 0;
        }
        return DefWindowProcW(window_, message, w, l);
    }

    static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM w, LPARAM l) noexcept
    {
        auto* self = reinterpret_cast<EditorToolbar*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<EditorToolbar*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
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

class EditorCanvas
{
  public:
    EditorCanvas(HINSTANCE instance, HWND parent, EditorDocument& document, EditorViewState& state,
                 const ThemePalette& palette, UINT dpi, std::function<void()> on_changed,
                 std::function<void()> on_crop_commit)
        : document_(document), state_(state), palette_(palette), dpi_(dpi), on_changed_(std::move(on_changed)),
          on_crop_commit_(std::move(on_crop_commit))
    {
        WNDCLASSW wc{};
        if (!GetClassInfoW(instance, EditorCanvasClass, &wc))
        {
            wc.lpfnWndProc = procedure;
            wc.hInstance = instance;
            wc.lpszClassName = EditorCanvasClass;
            wc.style = CS_DBLCLKS;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            if (!RegisterClassW(&wc))
                throw std::runtime_error("Cannot prepare the image canvas.");
        }
        window_ = CreateWindowExW(0, EditorCanvasClass, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0,
                                  parent, nullptr, instance, this);
        if (!window_)
            throw std::runtime_error("Cannot open the image canvas.");
    }

    ~EditorCanvas()
    {
        if (window_)
            KillTimer(window_, CaretBlinkTimer);
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

    IDWriteFactory* dwrite() noexcept
    {
        if (!dwrite_)
        {
            DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()));
        }
        return dwrite_.Get();
    }

    void set_theme(const ThemePalette& palette) noexcept
    {
        palette_ = palette;
        invalidate();
    }

    void set_dpi(UINT dpi) noexcept
    {
        dpi_ = dpi;
        invalidate();
    }

    void resize(int x, int y, int width, int height) noexcept
    {
        MoveWindow(window_, x, y, std::max(0, width), std::max(0, height), TRUE);
        invalidate();
    }

    void invalidate() noexcept
    {
        if (window_)
            InvalidateRect(window_, nullptr, FALSE);
    }

    void clear_patch_cache() noexcept
    {
        patch_cache_.clear();
    }

    void set_zoom_fit()
    {
        state_.fit_mode = true;
        state_.pan_x = 0.0f;
        state_.pan_y = 0.0f;
        update_viewport_metrics();
        invalidate();
        notify_changed();
    }

    void set_zoom_level(float next_zoom, const POINT* anchor_client = nullptr)
    {
        next_zoom = std::clamp(next_zoom, MinZoom, MaxZoom);
        update_viewport_metrics();
        if (anchor_client)
        {
            const PointF before = client_to_image(*anchor_client);
            state_.fit_mode = false;
            state_.zoom = next_zoom;
            update_viewport_metrics();
            const PointF after_client = image_to_client(before);
            state_.pan_x += static_cast<float>(anchor_client->x) - after_client.x;
            state_.pan_y += static_cast<float>(anchor_client->y) - after_client.y;
        }
        else
        {
            state_.fit_mode = false;
            state_.zoom = next_zoom;
        }
        clamp_pan();
        update_viewport_metrics();
        invalidate();
        notify_changed();
    }

    void zoom_step(float factor)
    {
        update_viewport_metrics();
        set_zoom_level(effective_scale_ * factor);
    }

    void start_text_edit(std::uint64_t existing_id, PointF at_image)
    {
        commit_text_edit();
        if (existing_id != 0)
        {
            const auto* existing = document_.find_annotation(existing_id);
            if (!existing || existing->kind != AnnotationKind::Text)
                return;
            state_.text_edit_id = existing_id;
            state_.text_draft = *existing;
            state_.text_caret = state_.text_draft.text.size();
            state_.text_anchor = state_.text_caret;
        }
        else
        {
            state_.text_edit_id = 0;
            state_.text_draft = {};
            state_.text_draft.kind = AnnotationKind::Text;
            state_.text_draft.color = state_.ink_color;
            state_.text_draft.size = state_.ink_size;
            state_.text_draft.filled = state_.text_background;
            state_.text_draft.start = at_image;
            state_.text_draft.text.clear();
            state_.text_caret = 0;
            state_.text_anchor = 0;
        }
        state_.selected_id = existing_id;
        state_.text_editing = true;
        state_.caret_visible = true;
        SetFocus(window_);
        SetTimer(window_, CaretBlinkTimer, 530, nullptr);
        invalidate();
        notify_changed();
    }

    void commit_text_edit()
    {
        if (!state_.text_editing)
            return;
        KillTimer(window_, CaretBlinkTimer);
        state_.text_editing = false;
        const bool empty = std::all_of(state_.text_draft.text.begin(), state_.text_draft.text.end(),
                                       [](wchar_t ch) { return std::iswspace(ch) != 0; });
        if (state_.text_edit_id != 0)
        {
            if (empty)
            {
                document_.delete_annotation(state_.text_edit_id);
                state_.selected_id = 0;
            }
            else
            {
                const auto* cur = document_.find_annotation(state_.text_edit_id);
                if (cur && (cur->text != state_.text_draft.text || cur->color != state_.text_draft.color ||
                            cur->size != state_.text_draft.size || cur->filled != state_.text_draft.filled))
                {
                    document_.begin_mutation();
                    if (auto* mut = document_.find_annotation_mutable(state_.text_edit_id))
                    {
                        mut->text = state_.text_draft.text;
                        mut->color = state_.text_draft.color;
                        mut->size = state_.text_draft.size;
                        mut->filled = state_.text_draft.filled;
                        ++mut->revision;
                    }
                    document_.commit_mutation();
                }
                state_.selected_id = state_.text_edit_id;
            }
        }
        else if (!empty)
        {
            state_.selected_id = document_.add_annotation(state_.text_draft);
        }
        state_.text_edit_id = 0;
        state_.text_draft = {};
        invalidate();
        notify_changed();
    }

    bool handle_text_key(WPARAM key, bool ctrl, bool shift)
    {
        if (!state_.text_editing)
            return false;
        auto& text = state_.text_draft.text;
        const auto delete_selection = [&]() -> bool
        {
            if (state_.text_caret == state_.text_anchor)
                return false;
            const size_t lo = std::min(state_.text_caret, state_.text_anchor);
            const size_t hi = std::max(state_.text_caret, state_.text_anchor);
            text.erase(lo, hi - lo);
            state_.text_caret = state_.text_anchor = lo;
            return true;
        };
        const auto reset_blink = [&]()
        {
            state_.caret_visible = true;
            SetTimer(window_, CaretBlinkTimer, 530, nullptr);
            invalidate();
        };

        if (ctrl && key == 'A')
        {
            state_.text_anchor = 0;
            state_.text_caret = text.size();
            reset_blink();
            return true;
        }
        if (ctrl && key == 'C')
        {
            if (state_.text_caret == state_.text_anchor)
                return false;
            const size_t lo = std::min(state_.text_caret, state_.text_anchor);
            const size_t hi = std::max(state_.text_caret, state_.text_anchor);
            write_clipboard_text(window_, text.substr(lo, hi - lo));
            return true;
        }
        if (ctrl && key == 'X')
        {
            if (state_.text_caret != state_.text_anchor)
            {
                const size_t lo = std::min(state_.text_caret, state_.text_anchor);
                const size_t hi = std::max(state_.text_caret, state_.text_anchor);
                write_clipboard_text(window_, text.substr(lo, hi - lo));
                delete_selection();
                reset_blink();
            }
            return true;
        }
        if (ctrl && key == 'V')
        {
            std::wstring clip;
            if (read_clipboard_text(window_, clip) && !clip.empty())
            {
                std::wstring normalized;
                normalized.reserve(clip.size());
                for (size_t i = 0; i < clip.size(); ++i)
                {
                    if (clip[i] == L'\r')
                    {
                        if (i + 1 < clip.size() && clip[i + 1] == L'\n')
                            continue;
                        normalized.push_back(L'\n');
                    }
                    else if (clip[i] == L'\n' || clip[i] >= 0x20)
                    {
                        normalized.push_back(clip[i]);
                    }
                }
                delete_selection();
                text.insert(state_.text_caret, normalized);
                state_.text_caret += normalized.size();
                state_.text_anchor = state_.text_caret;
                reset_blink();
            }
            return true;
        }
        if (ctrl && key == VK_RETURN)
        {
            commit_text_edit();
            return true;
        }
        if (key == VK_ESCAPE)
        {
            commit_text_edit();
            state_.tool = ToolKind::Select;
            notify_changed();
            return true;
        }
        if (key == VK_BACK)
        {
            if (!delete_selection() && state_.text_caret > 0)
            {
                if (ctrl)
                {
                    size_t pos = state_.text_caret;
                    while (pos > 0 && std::iswspace(text[pos - 1]))
                        --pos;
                    while (pos > 0 && !std::iswspace(text[pos - 1]))
                        --pos;
                    text.erase(pos, state_.text_caret - pos);
                    state_.text_caret = state_.text_anchor = pos;
                }
                else
                {
                    text.erase(state_.text_caret - 1, 1);
                    --state_.text_caret;
                    state_.text_anchor = state_.text_caret;
                }
            }
            reset_blink();
            return true;
        }
        if (key == VK_DELETE)
        {
            if (!delete_selection() && state_.text_caret < text.size())
                text.erase(state_.text_caret, 1);
            reset_blink();
            return true;
        }
        if (key == VK_LEFT)
        {
            if (!shift && state_.text_caret != state_.text_anchor)
                state_.text_caret = std::min(state_.text_caret, state_.text_anchor);
            else if (state_.text_caret > 0)
                --state_.text_caret;
            if (!shift)
                state_.text_anchor = state_.text_caret;
            reset_blink();
            return true;
        }
        if (key == VK_RIGHT)
        {
            if (!shift && state_.text_caret != state_.text_anchor)
                state_.text_caret = std::max(state_.text_caret, state_.text_anchor);
            else if (state_.text_caret < text.size())
                ++state_.text_caret;
            if (!shift)
                state_.text_anchor = state_.text_caret;
            reset_blink();
            return true;
        }
        if (key == VK_HOME)
        {
            size_t line_start = state_.text_caret;
            while (line_start > 0 && text[line_start - 1] != L'\n')
                --line_start;
            state_.text_caret = line_start;
            if (!shift)
                state_.text_anchor = state_.text_caret;
            reset_blink();
            return true;
        }
        if (key == VK_END)
        {
            size_t line_end = state_.text_caret;
            while (line_end < text.size() && text[line_end] != L'\n')
                ++line_end;
            state_.text_caret = line_end;
            if (!shift)
                state_.text_anchor = state_.text_caret;
            reset_blink();
            return true;
        }
        if (key == VK_UP || key == VK_DOWN)
        {
            float tw = 0.0f;
            float th = 0.0f;
            ComPtr<IDWriteTextLayout> layout;
            measure_text_annotation(dwrite(), state_.text_draft, tw, th, &layout);
            if (layout)
            {
                float cx = 0.0f;
                float cy = 0.0f;
                DWRITE_HIT_TEST_METRICS hm{};
                if (SUCCEEDED(layout->HitTestTextPosition(static_cast<UINT32>(state_.text_caret), FALSE, &cx,
                                                          &cy, &hm)))
                {
                    const float step_y = (hm.height > 1.0f ? hm.height : font_size_for(state_.text_draft.size)) *
                                         (key == VK_UP ? -0.8f : 1.2f);
                    BOOL trailing = FALSE;
                    BOOL inside = FALSE;
                    DWRITE_HIT_TEST_METRICS next_hm{};
                    if (SUCCEEDED(layout->HitTestPoint(cx, std::max(0.0f, cy + step_y), &trailing, &inside,
                                                       &next_hm)))
                    {
                        state_.text_caret = std::min(
                            text.size(), static_cast<size_t>(next_hm.textPosition + (trailing ? 1u : 0u)));
                        if (!shift)
                            state_.text_anchor = state_.text_caret;
                        reset_blink();
                        return true;
                    }
                }
            }
            return true;
        }
        return false;
    }

    void handle_text_char(wchar_t ch)
    {
        if (!state_.text_editing)
            return;
        if (ch == L'\r')
            ch = L'\n';
        else if (ch < 0x20)
            return;
        auto& text = state_.text_draft.text;
        if (state_.text_caret != state_.text_anchor)
        {
            const size_t lo = std::min(state_.text_caret, state_.text_anchor);
            const size_t hi = std::max(state_.text_caret, state_.text_anchor);
            text.erase(lo, hi - lo);
            state_.text_caret = state_.text_anchor = lo;
        }
        text.insert(state_.text_caret, 1, ch);
        ++state_.text_caret;
        state_.text_anchor = state_.text_caret;
        state_.caret_visible = true;
        SetTimer(window_, CaretBlinkTimer, 530, nullptr);
        invalidate();
    }

  private:
    enum class DragMode
    {
        None,
        Pan,
        Draw,
        MoveAnnotation,
        ResizeAnnotation,
        CropHandle,
        CropMove,
        CropNew,
        TextSelect
    };

    struct CachedPatch
    {
        std::uint64_t revision{};
        RECT bounds{};
        ComPtr<ID2D1Bitmap> bitmap;
    };

    EditorDocument& document_;
    EditorViewState& state_;
    ThemePalette palette_{};
    UINT dpi_{96};
    std::function<void()> on_changed_;
    std::function<void()> on_crop_commit_;
    HWND window_{};

    ComPtr<ID2D1Factory> factory_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<IWICImagingFactory> wic_;
    ComPtr<ID2D1HwndRenderTarget> target_;
    ComPtr<ID2D1StrokeStyle> dash_style_;
    ComPtr<ID2D1Bitmap> base_bitmap_;
    RECT cached_view_rect_{};
    UINT cached_base_w_{};
    UINT cached_base_h_{};
    std::unordered_map<std::uint64_t, CachedPatch> patch_cache_;

    RECT view_rect_{};
    float effective_scale_{1.0f};
    float origin_x_{0.0f};
    float origin_y_{0.0f};
    float draw_w_{1.0f};
    float draw_h_{1.0f};

    DragMode drag_mode_{DragMode::None};
    bool space_down_{false};
    POINT drag_start_client_{};
    float drag_start_pan_x_{0.0f};
    float drag_start_pan_y_{0.0f};
    PointF drag_start_img_{};
    PointF drag_last_img_{};
    Annotation draft_{};
    bool has_draft_{false};
    HandleKind active_handle_{HandleKind::None};
    RectF resize_initial_bounds_{};
    std::vector<PointF> resize_initial_points_;
    RECT crop_initial_{};

    float px(float dip) const noexcept
    {
        return dip * static_cast<float>(dpi_) / 96.0f;
    }

    void notify_changed()
    {
        if (on_changed_)
            on_changed_();
    }

    void discard_device_resources() noexcept
    {
        base_bitmap_.Reset();
        patch_cache_.clear();
        target_.Reset();
        cached_base_w_ = 0;
        cached_base_h_ = 0;
    }

    bool ensure_target(UINT client_w, UINT client_h)
    {
        if (!factory_)
        {
            if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf())))
                return false;
            const auto dash_props = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                                D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER,
                                                                10.0f, D2D1_DASH_STYLE_DASH, 0.0f);
            factory_->CreateStrokeStyle(dash_props, nullptr, 0, &dash_style_);
        }
        if (!dwrite_)
        {
            DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()));
        }
        if (!wic_)
        {
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_));
        }
        if (!target_)
        {
            const auto properties = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
            const auto hwnd_properties =
                D2D1::HwndRenderTargetProperties(window_, D2D1::SizeU(client_w, client_h));
            if (FAILED(factory_->CreateHwndRenderTarget(properties, hwnd_properties, &target_)))
                return false;
            return true;
        }
        const auto current = target_->GetPixelSize();
        if (current.width != client_w || current.height != client_h)
        {
            const auto hr = target_->Resize(D2D1::SizeU(client_w, client_h));
            if (hr == D2DERR_RECREATE_TARGET)
            {
                discard_device_resources();
                return ensure_target(client_w, client_h);
            }
            if (FAILED(hr))
                return false;
        }
        return true;
    }

    void update_viewport_metrics()
    {
        RECT client{};
        if (window_)
            GetClientRect(window_, &client);
        const float cw = static_cast<float>(std::max<LONG>(1, client.right - client.left));
        const float ch = static_cast<float>(std::max<LONG>(1, client.bottom - client.top));

        view_rect_ = state_.crop_active ? RECT{0, 0, document_.base_width(), document_.base_height()}
                                        : document_.crop_rect();
        const float vw = static_cast<float>(std::max<LONG>(1, view_rect_.right - view_rect_.left));
        const float vh = static_cast<float>(std::max<LONG>(1, view_rect_.bottom - view_rect_.top));

        const float pad = px(static_cast<float>(CanvasPaddingDip));
        const float avail_w = std::max(1.0f, cw - 2.0f * pad);
        const float avail_h = std::max(1.0f, ch - 2.0f * pad);
        const float fit_scale = std::min(1.0f, std::min(avail_w / vw, avail_h / vh));

        effective_scale_ = state_.fit_mode ? fit_scale : std::clamp(state_.zoom, MinZoom, MaxZoom);
        state_.zoom = effective_scale_;
        const int next_pct = std::max(1, static_cast<int>(std::lround(effective_scale_ * 100.0f)));
        if (next_pct != state_.display_zoom_percent)
            state_.display_zoom_percent = next_pct;

        draw_w_ = std::max(1.0f, vw * effective_scale_);
        draw_h_ = std::max(1.0f, vh * effective_scale_);
        if (state_.fit_mode)
        {
            state_.pan_x = 0.0f;
            state_.pan_y = 0.0f;
        }
        origin_x_ = std::floor((cw - draw_w_) * 0.5f + state_.pan_x);
        origin_y_ = std::floor((ch - draw_h_) * 0.5f + state_.pan_y);
    }

    void clamp_pan()
    {
        RECT client{};
        if (!window_)
            return;
        GetClientRect(window_, &client);
        const float cw = static_cast<float>(std::max<LONG>(1, client.right - client.left));
        const float ch = static_cast<float>(std::max<LONG>(1, client.bottom - client.top));
        const float max_x = std::max(0.0f, (draw_w_ + cw) * 0.5f - px(48.0f));
        const float max_y = std::max(0.0f, (draw_h_ + ch) * 0.5f - px(48.0f));
        state_.pan_x = std::clamp(state_.pan_x, -max_x, max_x);
        state_.pan_y = std::clamp(state_.pan_y, -max_y, max_y);
    }

    PointF client_to_image(POINT pt) const noexcept
    {
        return {static_cast<float>(view_rect_.left) + (static_cast<float>(pt.x) - origin_x_) / effective_scale_,
                static_cast<float>(view_rect_.top) + (static_cast<float>(pt.y) - origin_y_) / effective_scale_};
    }

    PointF image_to_client(PointF pt) const noexcept
    {
        return {origin_x_ + (pt.x - static_cast<float>(view_rect_.left)) * effective_scale_,
                origin_y_ + (pt.y - static_cast<float>(view_rect_.top)) * effective_scale_};
    }

    bool ensure_base_bitmap()
    {
        const UINT max_size = std::max(1u, target_->GetMaximumBitmapSize());
        const UINT src_w = static_cast<UINT>(std::max<LONG>(1, view_rect_.right - view_rect_.left));
        const UINT src_h = static_cast<UINT>(std::max<LONG>(1, view_rect_.bottom - view_rect_.top));

        UINT target_w = src_w;
        UINT target_h = src_h;
        if (effective_scale_ < 0.999f)
        {
            target_w = std::max(1u, static_cast<UINT>(std::lround(draw_w_)));
            target_h = std::max(1u, static_cast<UINT>(std::lround(draw_h_)));
        }
        if (target_w > max_size || target_h > max_size)
        {
            const float limit = std::min(static_cast<float>(max_size) / static_cast<float>(target_w),
                                         static_cast<float>(max_size) / static_cast<float>(target_h));
            target_w = std::max(1u, static_cast<UINT>(std::floor(target_w * limit)));
            target_h = std::max(1u, static_cast<UINT>(std::floor(target_h * limit)));
        }

        if (base_bitmap_ && EqualRect(&cached_view_rect_, &view_rect_) && cached_base_w_ == target_w &&
            cached_base_h_ == target_h)
        {
            return true;
        }

        base_bitmap_.Reset();
        cached_base_w_ = 0;
        cached_base_h_ = 0;

        const auto& base = document_.base();
        std::vector<std::uint32_t> view_pixels(static_cast<size_t>(src_w) * static_cast<size_t>(src_h));
        for (UINT y = 0; y < src_h; ++y)
        {
            const auto* src = base.pixels() + static_cast<size_t>(view_rect_.top + static_cast<LONG>(y)) *
                                                  base.width() +
                              view_rect_.left;
            auto* dst = view_pixels.data() + static_cast<size_t>(y) * src_w;
            for (UINT x = 0; x < src_w; ++x)
                dst[x] = 0xff000000u | (src[x] & 0x00ffffffu);
        }

        const BYTE* upload = reinterpret_cast<const BYTE*>(view_pixels.data());
        std::vector<BYTE> scaled;
        if ((target_w != src_w || target_h != src_h) && wic_)
        {
            ComPtr<IWICBitmap> wic_source;
            const UINT src_bytes = src_w * src_h * 4u;
            if (SUCCEEDED(wic_->CreateBitmapFromMemory(src_w, src_h, GUID_WICPixelFormat32bppBGRA, src_w * 4u,
                                                       src_bytes, const_cast<BYTE*>(upload), &wic_source)))
            {
                ComPtr<IWICBitmapScaler> scaler;
                if (SUCCEEDED(wic_->CreateBitmapScaler(&scaler)) &&
                    SUCCEEDED(scaler->Initialize(wic_source.Get(), target_w, target_h,
                                                 WICBitmapInterpolationModeFant)))
                {
                    scaled.resize(static_cast<size_t>(target_w) * target_h * 4u);
                    if (SUCCEEDED(scaler->CopyPixels(nullptr, target_w * 4u,
                                                     static_cast<UINT>(scaled.size()), scaled.data())))
                        upload = scaled.data();
                }
            }
        }
        const auto props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        if (FAILED(target_->CreateBitmap(D2D1::SizeU(target_w, target_h), upload, target_w * 4u, props,
                                         &base_bitmap_)))
            return false;
        cached_view_rect_ = view_rect_;
        cached_base_w_ = target_w;
        cached_base_h_ = target_h;
        return true;
    }

    void draw_annotation_live(const Annotation& item, bool is_draft)
    {
        if (item.kind == AnnotationKind::Redact && !item.filled)
        {
            if (is_draft)
            {
                const auto patch = compute_pixelate_patch(document_.base(), item);
                if (!patch.bgra.empty())
                {
                    const UINT pw = static_cast<UINT>(patch.bounds.right - patch.bounds.left);
                    const UINT ph = static_cast<UINT>(patch.bounds.bottom - patch.bounds.top);
                    ComPtr<ID2D1Bitmap> bmp;
                    const auto props = D2D1::BitmapProperties(
                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
                    if (SUCCEEDED(target_->CreateBitmap(D2D1::SizeU(pw, ph), patch.bgra.data(), pw * 4u,
                                                        props, &bmp)))
                    {
                        target_->DrawBitmap(
                            bmp.Get(),
                            D2D1::RectF(static_cast<float>(patch.bounds.left),
                                        static_cast<float>(patch.bounds.top),
                                        static_cast<float>(patch.bounds.right),
                                        static_cast<float>(patch.bounds.bottom)),
                            1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
                    }
                }
                return;
            }
            auto& entry = patch_cache_[item.id];
            if (!entry.bitmap || entry.revision != item.revision)
            {
                entry.bitmap.Reset();
                const auto patch = compute_pixelate_patch(document_.base(), item);
                entry.bounds = patch.bounds;
                entry.revision = item.revision;
                if (!patch.bgra.empty())
                {
                    const UINT pw = static_cast<UINT>(patch.bounds.right - patch.bounds.left);
                    const UINT ph = static_cast<UINT>(patch.bounds.bottom - patch.bounds.top);
                    const auto props = D2D1::BitmapProperties(
                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
                    target_->CreateBitmap(D2D1::SizeU(pw, ph), patch.bgra.data(), pw * 4u, props,
                                          &entry.bitmap);
                }
            }
            if (entry.bitmap)
            {
                target_->DrawBitmap(entry.bitmap.Get(),
                                    D2D1::RectF(static_cast<float>(entry.bounds.left),
                                                static_cast<float>(entry.bounds.top),
                                                static_cast<float>(entry.bounds.right),
                                                static_cast<float>(entry.bounds.bottom)),
                                    1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            }
            return;
        }

        if (item.kind == AnnotationKind::Highlighter && !is_draft && wic_)
        {
            auto& entry = patch_cache_[item.id];
            if (!entry.bitmap || entry.revision != item.revision)
            {
                entry.bitmap.Reset();
                const auto patch = compute_highlighter_patch(document_.base(), item, factory_.Get(), wic_.Get());
                entry.bounds = patch.bounds;
                entry.revision = item.revision;
                if (!patch.premul_bgra.empty())
                {
                    const UINT pw = static_cast<UINT>(patch.bounds.right - patch.bounds.left);
                    const UINT ph = static_cast<UINT>(patch.bounds.bottom - patch.bounds.top);
                    const auto props = D2D1::BitmapProperties(
                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                    target_->CreateBitmap(D2D1::SizeU(pw, ph), patch.premul_bgra.data(), pw * 4u, props,
                                          &entry.bitmap);
                }
            }
            if (entry.bitmap)
            {
                target_->DrawBitmap(entry.bitmap.Get(),
                                    D2D1::RectF(static_cast<float>(entry.bounds.left),
                                                static_cast<float>(entry.bounds.top),
                                                static_cast<float>(entry.bounds.right),
                                                static_cast<float>(entry.bounds.bottom)),
                                    1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                return;
            }
        }

        draw_vector_annotation(target_.Get(), factory_.Get(), dwrite(), item, is_draft);
    }

    void draw_handle_dot(PointF client_pt, ID2D1Brush* fill, ID2D1Brush* stroke, ID2D1Brush* shadow,
                         bool circle = false)
    {
        const float r = px(4.5f);
        if (circle)
        {
            if (shadow)
                target_->FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(client_pt.x, client_pt.y), r + px(1.2f), r + px(1.2f)),
                    shadow);
            const auto el = D2D1::Ellipse(D2D1::Point2F(client_pt.x, client_pt.y), r, r);
            target_->FillEllipse(el, fill);
            target_->DrawEllipse(el, stroke, px(1.8f));
        }
        else
        {
            if (shadow)
            {
                const float sr = r + px(1.2f);
                target_->FillRectangle(
                    D2D1::RectF(client_pt.x - sr, client_pt.y - sr, client_pt.x + sr, client_pt.y + sr),
                    shadow);
            }
            const auto rc =
                D2D1::RectF(client_pt.x - r, client_pt.y - r, client_pt.x + r, client_pt.y + r);
            target_->FillRectangle(rc, fill);
            target_->DrawRectangle(rc, stroke, px(1.8f));
        }
    }

    void draw_contrast_dashed_rect(const D2D1_RECT_F& rect, ID2D1Brush* select_brush, ID2D1Brush* halo_brush)
    {
        if (halo_brush)
            target_->DrawRectangle(rect, halo_brush, px(2.6f));
        target_->DrawRectangle(rect, select_brush, px(1.6f), dash_style_.Get());
    }

    void draw_selection_chrome(const Annotation& item, ID2D1Brush* select_brush, ID2D1Brush* white_brush,
                               ID2D1Brush* halo_brush, ID2D1Brush* shadow_brush)
    {
        if (item.kind == AnnotationKind::Line || item.kind == AnnotationKind::Arrow)
        {
            draw_handle_dot(image_to_client(item.start), white_brush, select_brush, shadow_brush, true);
            draw_handle_dot(image_to_client(item.end), white_brush, select_brush, shadow_brush, true);
            return;
        }
        const RectF r = (item.kind == AnnotationKind::Rectangle || item.kind == AnnotationKind::Oval ||
                         item.kind == AnnotationKind::Redact)
                            ? normalize_rect(item.start, item.end)
                            : annotation_bounds(item, dwrite());
        const PointF tl = image_to_client({r.left, r.top});
        const PointF br = image_to_client({r.right, r.bottom});
        draw_contrast_dashed_rect(D2D1::RectF(tl.x, tl.y, br.x, br.y), select_brush, halo_brush);

        if (item.kind == AnnotationKind::Rectangle || item.kind == AnnotationKind::Oval ||
            item.kind == AnnotationKind::Redact)
        {
            const float mx = (tl.x + br.x) * 0.5f;
            const float my = (tl.y + br.y) * 0.5f;
            const PointF pts[] = {{tl.x, tl.y}, {mx, tl.y}, {br.x, tl.y}, {br.x, my},
                                  {br.x, br.y}, {mx, br.y}, {tl.x, br.y}, {tl.x, my}};
            for (const auto& pt : pts)
                draw_handle_dot(pt, white_brush, select_brush, shadow_brush, false);
        }
        else if (item.kind == AnnotationKind::Pen || item.kind == AnnotationKind::Highlighter)
        {
            const PointF pts[] = {{tl.x, tl.y}, {br.x, tl.y}, {br.x, br.y}, {tl.x, br.y}};
            for (const auto& pt : pts)
                draw_handle_dot(pt, white_brush, select_brush, shadow_brush, false);
        }
    }

    void draw_text_edit_chrome(ID2D1Brush* select_brush, ID2D1Brush* halo_brush)
    {
        float text_w = 0.0f;
        float text_h = 0.0f;
        ComPtr<IDWriteTextLayout> layout;
        measure_text_annotation(dwrite(), state_.text_draft, text_w, text_h, &layout);

        const RectF b = annotation_bounds(state_.text_draft, dwrite());
        const PointF tl = image_to_client({b.left, b.top});
        const PointF br = image_to_client({b.right, b.bottom});
        draw_contrast_dashed_rect(
            D2D1::RectF(tl.x - px(2.0f), tl.y - px(2.0f), br.x + px(2.0f), br.y + px(2.0f)), select_brush,
            halo_brush);

        if (!layout)
            return;

        // Highlight selected character range
        if (state_.text_caret != state_.text_anchor)
        {
            const UINT32 lo = static_cast<UINT32>(std::min(state_.text_caret, state_.text_anchor));
            const UINT32 len = static_cast<UINT32>(std::max(state_.text_caret, state_.text_anchor)) - lo;
            UINT32 count = 0;
            layout->HitTestTextRange(lo, len, 0.0f, 0.0f, nullptr, 0, &count);
            if (count > 0)
            {
                std::vector<DWRITE_HIT_TEST_METRICS> metrics(count);
                if (SUCCEEDED(layout->HitTestTextRange(lo, len, 0.0f, 0.0f, metrics.data(), count, &count)))
                {
                    ComPtr<ID2D1SolidColorBrush> sel_brush;
                    if (SUCCEEDED(target_->CreateSolidColorBrush(to_d2d_color(SelectionColor, 0.32f),
                                                                 &sel_brush)))
                    {
                        for (UINT32 i = 0; i < count; ++i)
                        {
                            const PointF s0 = image_to_client(
                                {state_.text_draft.start.x + metrics[i].left,
                                 state_.text_draft.start.y + metrics[i].top});
                            const PointF s1 = image_to_client(
                                {state_.text_draft.start.x + metrics[i].left + metrics[i].width,
                                 state_.text_draft.start.y + metrics[i].top + metrics[i].height});
                            target_->FillRectangle(D2D1::RectF(s0.x, s0.y, s1.x, s1.y), sel_brush.Get());
                        }
                    }
                }
            }
        }

        // Caret
        if (state_.caret_visible)
        {
            float cx = 0.0f;
            float cy = 0.0f;
            DWRITE_HIT_TEST_METRICS hm{};
            if (SUCCEEDED(layout->HitTestTextPosition(static_cast<UINT32>(state_.text_caret), FALSE, &cx, &cy,
                                                      &hm)))
            {
                const float caret_h = hm.height > 1.0f ? hm.height : font_size_for(state_.text_draft.size) * 1.2f;
                const PointF p0 = image_to_client({state_.text_draft.start.x + cx, state_.text_draft.start.y + cy});
                const PointF p1 =
                    image_to_client({state_.text_draft.start.x + cx, state_.text_draft.start.y + cy + caret_h});
                if (halo_brush)
                    target_->DrawLine(D2D1::Point2F(p0.x, p0.y), D2D1::Point2F(p1.x, p1.y), halo_brush,
                                      px(3.6f));
                target_->DrawLine(D2D1::Point2F(p0.x, p0.y), D2D1::Point2F(p1.x, p1.y), select_brush,
                                  px(1.9f));
            }
        }
    }

    void draw_crop_overlay(ID2D1Brush* select_brush, ID2D1Brush* white_brush, ID2D1Brush* shadow_brush)
    {
        const PointF full_tl{origin_x_, origin_y_};
        const PointF full_br{origin_x_ + draw_w_, origin_y_ + draw_h_};
        const PointF c_tl =
            image_to_client({static_cast<float>(state_.crop_draft.left), static_cast<float>(state_.crop_draft.top)});
        const PointF c_br = image_to_client(
            {static_cast<float>(state_.crop_draft.right), static_cast<float>(state_.crop_draft.bottom)});

        ComPtr<ID2D1SolidColorBrush> scrim;
        target_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.56f), &scrim);
        if (scrim)
        {
            target_->FillRectangle(D2D1::RectF(full_tl.x, full_tl.y, full_br.x, c_tl.y), scrim.Get());
            target_->FillRectangle(D2D1::RectF(full_tl.x, c_br.y, full_br.x, full_br.y), scrim.Get());
            target_->FillRectangle(D2D1::RectF(full_tl.x, c_tl.y, c_tl.x, c_br.y), scrim.Get());
            target_->FillRectangle(D2D1::RectF(c_br.x, c_tl.y, full_br.x, c_br.y), scrim.Get());
        }

        // Subtle rule-of-thirds grid inside crop area
        ComPtr<ID2D1SolidColorBrush> grid;
        target_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.28f), &grid);
        if (grid && (c_br.x - c_tl.x) > px(40.0f) && (c_br.y - c_tl.y) > px(40.0f))
        {
            for (int i = 1; i <= 2; ++i)
            {
                const float gx = c_tl.x + (c_br.x - c_tl.x) * (static_cast<float>(i) / 3.0f);
                const float gy = c_tl.y + (c_br.y - c_tl.y) * (static_cast<float>(i) / 3.0f);
                target_->DrawLine(D2D1::Point2F(gx, c_tl.y), D2D1::Point2F(gx, c_br.y), grid.Get(), 1.0f);
                target_->DrawLine(D2D1::Point2F(c_tl.x, gy), D2D1::Point2F(c_br.x, gy), grid.Get(), 1.0f);
            }
        }

        const D2D1_RECT_F crop_box = D2D1::RectF(c_tl.x, c_tl.y, c_br.x, c_br.y);
        target_->DrawRectangle(crop_box, white_brush, px(2.6f));
        target_->DrawRectangle(crop_box, select_brush, px(1.5f));
        const float mx = (c_tl.x + c_br.x) * 0.5f;
        const float my = (c_tl.y + c_br.y) * 0.5f;
        const PointF pts[] = {{c_tl.x, c_tl.y}, {mx, c_tl.y}, {c_br.x, c_tl.y}, {c_br.x, my},
                              {c_br.x, c_br.y}, {mx, c_br.y}, {c_tl.x, c_br.y}, {c_tl.x, my}};
        for (const auto& pt : pts)
            draw_handle_dot(pt, white_brush, select_brush, shadow_brush, false);
    }

    void paint()
    {
        PAINTSTRUCT ps{};
        BeginPaint(window_, &ps);
        RECT client{};
        GetClientRect(window_, &client);
        const int cw = client.right - client.left;
        const int ch = client.bottom - client.top;
        if (cw <= 0 || ch <= 0)
        {
            EndPaint(window_, &ps);
            return;
        }
        if (!ensure_target(static_cast<UINT>(cw), static_cast<UINT>(ch)))
        {
            EndPaint(window_, &ps);
            return;
        }

        const int prev_pct = state_.display_zoom_percent;
        update_viewport_metrics();
        const bool has_base = ensure_base_bitmap();

        ComPtr<ID2D1SolidColorBrush> border_brush;
        ComPtr<ID2D1SolidColorBrush> select_brush;
        ComPtr<ID2D1SolidColorBrush> white_brush;
        ComPtr<ID2D1SolidColorBrush> halo_brush;
        ComPtr<ID2D1SolidColorBrush> shadow_brush;
        target_->CreateSolidColorBrush(to_d2d_color(palette_.line), &border_brush);
        target_->CreateSolidColorBrush(
            to_d2d_color(palette_.high_contrast ? palette_.accent : SelectionColor), &select_brush);
        target_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &white_brush);
        target_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.92f), &halo_brush);
        target_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.35f), &shadow_brush);

        target_->BeginDraw();
        target_->SetTransform(D2D1::Matrix3x2F::Identity());
        target_->Clear(to_d2d_color(palette_.surface));

        if (has_base && base_bitmap_)
        {
            const D2D1_RECT_F img_rect =
                D2D1::RectF(origin_x_, origin_y_, origin_x_ + draw_w_, origin_y_ + draw_h_);
            const auto interp = effective_scale_ > 1.01f ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR
                                                         : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;
            target_->DrawBitmap(base_bitmap_.Get(), img_rect, 1.0f, interp);

            target_->PushAxisAlignedClip(img_rect, D2D1_ANTIALIAS_MODE_ALIASED);
            const auto transform =
                D2D1::Matrix3x2F::Translation(static_cast<float>(-view_rect_.left),
                                              static_cast<float>(-view_rect_.top)) *
                D2D1::Matrix3x2F::Scale(effective_scale_, effective_scale_) *
                D2D1::Matrix3x2F::Translation(origin_x_, origin_y_);
            target_->SetTransform(transform);

            for (const auto& item : document_.annotations())
            {
                if (state_.text_editing && state_.text_edit_id == item.id)
                    continue;
                draw_annotation_live(item, false);
            }
            if (has_draft_)
                draw_annotation_live(draft_, true);
            if (state_.text_editing)
                draw_vector_annotation(target_.Get(), factory_.Get(), dwrite(), state_.text_draft, false);

            target_->SetTransform(D2D1::Matrix3x2F::Identity());
            target_->PopAxisAlignedClip();

            if (border_brush)
            {
                target_->DrawRectangle(
                    D2D1::RectF(img_rect.left - 0.5f, img_rect.top - 0.5f, img_rect.right + 0.5f,
                                img_rect.bottom + 0.5f),
                    border_brush.Get(), 1.0f);
            }

            if (state_.crop_active && select_brush && white_brush)
            {
                draw_crop_overlay(select_brush.Get(), white_brush.Get(), shadow_brush.Get());
            }
            else if (state_.text_editing && select_brush)
            {
                draw_text_edit_chrome(select_brush.Get(), halo_brush.Get());
            }
            else if (state_.selected_id != 0 && select_brush && white_brush)
            {
                if (const auto* sel = document_.find_annotation(state_.selected_id))
                    draw_selection_chrome(*sel, select_brush.Get(), white_brush.Get(), halo_brush.Get(),
                                          shadow_brush.Get());
            }
        }

        const auto hr = target_->EndDraw();
        EndPaint(window_, &ps);
        if (hr == D2DERR_RECREATE_TARGET)
        {
            discard_device_resources();
            invalidate();
        }
        if (state_.display_zoom_percent != prev_pct)
            notify_changed();
    }

    HandleKind hit_test_crop_handles(PointF img_pt) const noexcept
    {
        const float tol = px(9.0f) / effective_scale_;
        const RectF r{static_cast<float>(state_.crop_draft.left), static_cast<float>(state_.crop_draft.top),
                      static_cast<float>(state_.crop_draft.right),
                      static_cast<float>(state_.crop_draft.bottom)};
        const float mx = (r.left + r.right) * 0.5f;
        const float my = (r.top + r.bottom) * 0.5f;
        const std::pair<HandleKind, PointF> handles[] = {
            {HandleKind::TopLeft, {r.left, r.top}},
            {HandleKind::TopRight, {r.right, r.top}},
            {HandleKind::BottomRight, {r.right, r.bottom}},
            {HandleKind::BottomLeft, {r.left, r.bottom}},
            {HandleKind::Top, {mx, r.top}},
            {HandleKind::Right, {r.right, my}},
            {HandleKind::Bottom, {mx, r.bottom}},
            {HandleKind::Left, {r.left, my}},
        };
        for (const auto& [kind, pos] : handles)
        {
            if (std::hypot(img_pt.x - pos.x, img_pt.y - pos.y) <= tol)
                return kind;
        }
        return HandleKind::None;
    }

    size_t text_index_from_image_point(PointF img_pt)
    {
        float tw = 0.0f;
        float th = 0.0f;
        ComPtr<IDWriteTextLayout> layout;
        measure_text_annotation(dwrite(), state_.text_draft, tw, th, &layout);
        if (!layout)
            return state_.text_draft.text.size();
        BOOL trailing = FALSE;
        BOOL inside = FALSE;
        DWRITE_HIT_TEST_METRICS hm{};
        if (FAILED(layout->HitTestPoint(img_pt.x - state_.text_draft.start.x,
                                        img_pt.y - state_.text_draft.start.y, &trailing, &inside, &hm)))
            return state_.text_draft.text.size();
        return std::min(state_.text_draft.text.size(), static_cast<size_t>(hm.textPosition + (trailing ? 1u : 0u)));
    }

    LPCWSTR cursor_for_handle(HandleKind handle) const noexcept
    {
        switch (handle)
        {
        case HandleKind::TopLeft:
        case HandleKind::BottomRight:
            return IDC_SIZENWSE;
        case HandleKind::TopRight:
        case HandleKind::BottomLeft:
            return IDC_SIZENESW;
        case HandleKind::Top:
        case HandleKind::Bottom:
            return IDC_SIZENS;
        case HandleKind::Left:
        case HandleKind::Right:
            return IDC_SIZEWE;
        case HandleKind::Start:
        case HandleKind::End:
            return IDC_CROSS;
        default:
            return IDC_ARROW;
        }
    }

    void update_cursor(POINT client_pt)
    {
        if (drag_mode_ == DragMode::Pan || space_down_)
        {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            return;
        }
        const PointF img_pt = client_to_image(client_pt);
        if (state_.crop_active)
        {
            const auto h = hit_test_crop_handles(img_pt);
            if (h != HandleKind::None)
            {
                SetCursor(LoadCursorW(nullptr, cursor_for_handle(h)));
                return;
            }
            if (img_pt.x >= state_.crop_draft.left && img_pt.x <= state_.crop_draft.right &&
                img_pt.y >= state_.crop_draft.top && img_pt.y <= state_.crop_draft.bottom)
            {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                return;
            }
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            return;
        }
        if (state_.text_editing)
        {
            const RectF b = annotation_bounds(state_.text_draft, dwrite());
            if (img_pt.x >= b.left && img_pt.x <= b.right && img_pt.y >= b.top && img_pt.y <= b.bottom)
            {
                SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
                return;
            }
        }
        const float handle_tol = px(9.0f) / effective_scale_;
        if (state_.selected_id != 0)
        {
            if (const auto* sel = document_.find_annotation(state_.selected_id))
            {
                const auto h = hit_test_handles(*sel, img_pt, handle_tol, dwrite());
                if (h != HandleKind::None)
                {
                    SetCursor(LoadCursorW(nullptr, cursor_for_handle(h)));
                    return;
                }
            }
        }
        if (state_.tool == ToolKind::Select)
        {
            const float tol = px(7.0f) / effective_scale_;
            if (document_.hit_test(img_pt, tol, dwrite()) != 0)
            {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                return;
            }
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return;
        }
        if (state_.tool == ToolKind::Text)
        {
            SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
            return;
        }
        SetCursor(LoadCursorW(nullptr, IDC_CROSS));
    }

    void on_left_down(POINT client_pt, bool double_click)
    {
        if (state_.busy)
            return;
        SetFocus(window_);
        update_viewport_metrics();
        const PointF img_pt = client_to_image(client_pt);

        if (space_down_)
        {
            drag_mode_ = DragMode::Pan;
            drag_start_client_ = client_pt;
            drag_start_pan_x_ = state_.pan_x;
            drag_start_pan_y_ = state_.pan_y;
            SetCapture(window_);
            return;
        }

        if (state_.crop_active)
        {
            const auto h = hit_test_crop_handles(img_pt);
            crop_initial_ = state_.crop_draft;
            drag_start_img_ = img_pt;
            if (h != HandleKind::None)
            {
                drag_mode_ = DragMode::CropHandle;
                active_handle_ = h;
            }
            else if (img_pt.x >= state_.crop_draft.left && img_pt.x <= state_.crop_draft.right &&
                     img_pt.y >= state_.crop_draft.top && img_pt.y <= state_.crop_draft.bottom)
            {
                if (double_click && on_crop_commit_)
                {
                    on_crop_commit_();
                    return;
                }
                drag_mode_ = DragMode::CropMove;
            }
            else
            {
                drag_mode_ = DragMode::CropNew;
                const LONG ix = std::clamp(static_cast<LONG>(std::lround(img_pt.x)), 0l,
                                           static_cast<LONG>(document_.base_width()));
                const LONG iy = std::clamp(static_cast<LONG>(std::lround(img_pt.y)), 0l,
                                           static_cast<LONG>(document_.base_height()));
                state_.crop_draft = clamp_crop_rect({ix, iy, ix + 1, iy + 1}, document_.base_width(),
                                                    document_.base_height());
            }
            SetCapture(window_);
            invalidate();
            notify_changed();
            return;
        }

        if (state_.text_editing)
        {
            const RectF b = annotation_bounds(state_.text_draft, dwrite());
            if (img_pt.x >= b.left - 4.0f && img_pt.x <= b.right + 4.0f && img_pt.y >= b.top - 4.0f &&
                img_pt.y <= b.bottom + 4.0f)
            {
                const size_t idx = text_index_from_image_point(img_pt);
                if (double_click)
                {
                    const auto& t = state_.text_draft.text;
                    size_t lo = idx;
                    size_t hi = idx;
                    while (lo > 0 && !std::iswspace(t[lo - 1]))
                        --lo;
                    while (hi < t.size() && !std::iswspace(t[hi]))
                        ++hi;
                    state_.text_anchor = lo;
                    state_.text_caret = hi;
                }
                else
                {
                    state_.text_caret = idx;
                    if ((GetKeyState(VK_SHIFT) & 0x8000) == 0)
                        state_.text_anchor = idx;
                    drag_mode_ = DragMode::TextSelect;
                    SetCapture(window_);
                }
                state_.caret_visible = true;
                invalidate();
                return;
            }
            commit_text_edit();
            if (state_.tool == ToolKind::Text)
            {
                start_text_edit(0, img_pt);
                return;
            }
        }

        const float handle_tol = px(9.0f) / effective_scale_;
        const float hit_tol = px(7.0f) / effective_scale_;

        // Allow immediate resize of handles on the currently selected annotation
        if (state_.selected_id != 0 && state_.tool != ToolKind::Text)
        {
            if (const auto* sel = document_.find_annotation(state_.selected_id))
            {
                const auto h = hit_test_handles(*sel, img_pt, handle_tol, dwrite());
                if (h != HandleKind::None)
                {
                    drag_mode_ = DragMode::ResizeAnnotation;
                    active_handle_ = h;
                    resize_initial_bounds_ =
                        (sel->kind == AnnotationKind::Rectangle || sel->kind == AnnotationKind::Oval ||
                         sel->kind == AnnotationKind::Redact)
                            ? normalize_rect(sel->start, sel->end)
                            : annotation_bounds(*sel, dwrite());
                    resize_initial_points_ = sel->points;
                    document_.begin_mutation();
                    SetCapture(window_);
                    return;
                }
            }
        }

        if (state_.tool == ToolKind::Select)
        {
            const auto hit_id = document_.hit_test(img_pt, hit_tol, dwrite());
            state_.selected_id = hit_id;
            if (hit_id != 0)
            {
                if (double_click)
                {
                    if (const auto* item = document_.find_annotation(hit_id);
                        item && item->kind == AnnotationKind::Text)
                    {
                        start_text_edit(hit_id, img_pt);
                        return;
                    }
                }
                drag_mode_ = DragMode::MoveAnnotation;
                drag_start_img_ = img_pt;
                drag_last_img_ = img_pt;
                document_.begin_mutation();
                SetCapture(window_);
            }
            invalidate();
            notify_changed();
            return;
        }

        if (state_.tool == ToolKind::Text)
        {
            const auto hit_id = document_.hit_test(img_pt, hit_tol, dwrite());
            if (hit_id != 0)
            {
                if (const auto* item = document_.find_annotation(hit_id);
                    item && item->kind == AnnotationKind::Text)
                {
                    start_text_edit(hit_id, img_pt);
                    return;
                }
            }
            start_text_edit(0, img_pt);
            return;
        }

        // Drawing tools
        state_.selected_id = 0;
        draft_ = {};
        has_draft_ = true;
        drag_mode_ = DragMode::Draw;
        drag_start_img_ = img_pt;
        switch (state_.tool)
        {
        case ToolKind::Pen:
            draft_.kind = AnnotationKind::Pen;
            draft_.color = state_.ink_color;
            draft_.size = state_.ink_size;
            draft_.points.push_back(img_pt);
            break;
        case ToolKind::Highlighter:
            draft_.kind = AnnotationKind::Highlighter;
            draft_.color = state_.highlighter_color;
            draft_.size = state_.highlighter_size;
            draft_.points.push_back(img_pt);
            break;
        case ToolKind::Arrow:
            draft_.kind = AnnotationKind::Arrow;
            draft_.color = state_.ink_color;
            draft_.size = state_.ink_size;
            draft_.start = img_pt;
            draft_.end = img_pt;
            break;
        case ToolKind::Rectangle:
            draft_.kind = AnnotationKind::Rectangle;
            draft_.color = state_.ink_color;
            draft_.size = state_.ink_size;
            draft_.filled = state_.shape_filled;
            draft_.start = img_pt;
            draft_.end = img_pt;
            break;
        case ToolKind::Oval:
            draft_.kind = AnnotationKind::Oval;
            draft_.color = state_.ink_color;
            draft_.size = state_.ink_size;
            draft_.filled = state_.shape_filled;
            draft_.start = img_pt;
            draft_.end = img_pt;
            break;
        case ToolKind::Line:
            draft_.kind = AnnotationKind::Line;
            draft_.color = state_.ink_color;
            draft_.size = state_.ink_size;
            draft_.start = img_pt;
            draft_.end = img_pt;
            break;
        case ToolKind::Redact:
            draft_.kind = AnnotationKind::Redact;
            draft_.color = state_.redact_color;
            draft_.size = state_.redact_size;
            draft_.filled = state_.redact_solid;
            draft_.start = img_pt;
            draft_.end = img_pt;
            break;
        case ToolKind::Step:
            draft_.kind = AnnotationKind::Step;
            draft_.color = state_.ink_color;
            draft_.size = state_.ink_size;
            draft_.start = img_pt;
            draft_.step_number = document_.next_step_number();
            break;
        default:
            has_draft_ = false;
            drag_mode_ = DragMode::None;
            return;
        }
        SetCapture(window_);
        invalidate();
        notify_changed();
    }

    void on_mouse_move(POINT client_pt)
    {
        const PointF img_pt = client_to_image(client_pt);
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

        if (drag_mode_ == DragMode::None)
            return;

        if (drag_mode_ == DragMode::Pan)
        {
            state_.fit_mode = false;
            state_.pan_x = drag_start_pan_x_ + static_cast<float>(client_pt.x - drag_start_client_.x);
            state_.pan_y = drag_start_pan_y_ + static_cast<float>(client_pt.y - drag_start_client_.y);
            clamp_pan();
            invalidate();
            return;
        }

        if (drag_mode_ == DragMode::TextSelect)
        {
            state_.text_caret = text_index_from_image_point(img_pt);
            invalidate();
            return;
        }

        if (drag_mode_ == DragMode::CropHandle)
        {
            LONG left = crop_initial_.left;
            LONG top = crop_initial_.top;
            LONG right = crop_initial_.right;
            LONG bottom = crop_initial_.bottom;
            const LONG ix = std::clamp(static_cast<LONG>(std::lround(img_pt.x)), 0l,
                                       static_cast<LONG>(document_.base_width()));
            const LONG iy = std::clamp(static_cast<LONG>(std::lround(img_pt.y)), 0l,
                                       static_cast<LONG>(document_.base_height()));
            switch (active_handle_)
            {
            case HandleKind::TopLeft:
                left = ix;
                top = iy;
                break;
            case HandleKind::Top:
                top = iy;
                break;
            case HandleKind::TopRight:
                right = ix;
                top = iy;
                break;
            case HandleKind::Right:
                right = ix;
                break;
            case HandleKind::BottomRight:
                right = ix;
                bottom = iy;
                break;
            case HandleKind::Bottom:
                bottom = iy;
                break;
            case HandleKind::BottomLeft:
                left = ix;
                bottom = iy;
                break;
            case HandleKind::Left:
                left = ix;
                break;
            default:
                break;
            }
            state_.crop_draft =
                clamp_crop_rect({left, top, right, bottom}, document_.base_width(), document_.base_height());
            invalidate();
            notify_changed();
            return;
        }

        if (drag_mode_ == DragMode::CropMove)
        {
            const LONG dx = static_cast<LONG>(std::lround(img_pt.x - drag_start_img_.x));
            const LONG dy = static_cast<LONG>(std::lround(img_pt.y - drag_start_img_.y));
            const LONG w = crop_initial_.right - crop_initial_.left;
            const LONG h = crop_initial_.bottom - crop_initial_.top;
            const LONG left = std::clamp<LONG>(crop_initial_.left + dx, 0, document_.base_width() - w);
            const LONG top = std::clamp<LONG>(crop_initial_.top + dy, 0, document_.base_height() - h);
            state_.crop_draft = {left, top, left + w, top + h};
            invalidate();
            notify_changed();
            return;
        }

        if (drag_mode_ == DragMode::CropNew)
        {
            const RECT raw{static_cast<LONG>(std::lround(drag_start_img_.x)),
                           static_cast<LONG>(std::lround(drag_start_img_.y)),
                           static_cast<LONG>(std::lround(img_pt.x)),
                           static_cast<LONG>(std::lround(img_pt.y))};
            state_.crop_draft = clamp_crop_rect(raw, document_.base_width(), document_.base_height());
            invalidate();
            notify_changed();
            return;
        }

        if (drag_mode_ == DragMode::MoveAnnotation)
        {
            if (auto* item = document_.find_annotation_mutable(state_.selected_id))
            {
                translate_annotation(*item, img_pt.x - drag_last_img_.x, img_pt.y - drag_last_img_.y);
                drag_last_img_ = img_pt;
                document_.mark_dirty();
                invalidate();
            }
            return;
        }

        if (drag_mode_ == DragMode::ResizeAnnotation)
        {
            if (auto* item = document_.find_annotation_mutable(state_.selected_id))
            {
                resize_annotation(*item, active_handle_, img_pt, shift, resize_initial_bounds_,
                                  resize_initial_points_);
                document_.mark_dirty();
                invalidate();
            }
            return;
        }

        if (drag_mode_ == DragMode::Draw && has_draft_)
        {
            switch (draft_.kind)
            {
            case AnnotationKind::Pen:
            case AnnotationKind::Highlighter:
                if (shift)
                {
                    const PointF anchor = draft_.points.front();
                    const PointF end = constrain_angle_45(anchor, img_pt);
                    draft_.points.assign({anchor, end});
                }
                else
                {
                    const auto& last = draft_.points.back();
                    if (std::hypot(img_pt.x - last.x, img_pt.y - last.y) >= 1.5f / effective_scale_)
                        draft_.points.push_back(img_pt);
                }
                break;
            case AnnotationKind::Arrow:
            case AnnotationKind::Line:
                draft_.end = shift ? constrain_angle_45(draft_.start, img_pt) : img_pt;
                break;
            case AnnotationKind::Rectangle:
            case AnnotationKind::Oval:
            case AnnotationKind::Redact:
                draft_.end = shift ? constrain_square(draft_.start, img_pt) : img_pt;
                break;
            case AnnotationKind::Step:
                draft_.start = img_pt;
                break;
            default:
                break;
            }
            invalidate();
        }
    }

    void finish_drag()
    {
        if (GetCapture() == window_)
            ReleaseCapture();
        const DragMode mode = drag_mode_;
        drag_mode_ = DragMode::None;

        if (mode == DragMode::MoveAnnotation || mode == DragMode::ResizeAnnotation)
        {
            document_.commit_mutation();
            invalidate();
            notify_changed();
            return;
        }

        if (mode == DragMode::Draw && has_draft_)
        {
            has_draft_ = false;
            const auto kind = draft_.kind;
            bool commit = true;
            if (kind == AnnotationKind::Arrow || kind == AnnotationKind::Line ||
                kind == AnnotationKind::Rectangle || kind == AnnotationKind::Oval ||
                kind == AnnotationKind::Redact)
            {
                commit = std::hypot(draft_.end.x - draft_.start.x, draft_.end.y - draft_.start.y) >= 2.0f;
            }
            if (commit)
            {
                const auto id = document_.add_annotation(std::move(draft_));
                if (kind == AnnotationKind::Arrow || kind == AnnotationKind::Line ||
                    kind == AnnotationKind::Rectangle || kind == AnnotationKind::Oval ||
                    kind == AnnotationKind::Redact)
                {
                    state_.selected_id = id;
                }
            }
            draft_ = {};
            invalidate();
            notify_changed();
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
            invalidate();
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT)
            {
                POINT pt{};
                GetCursorPos(&pt);
                ScreenToClient(window_, &pt);
                update_cursor(pt);
                return TRUE;
            }
            break;
        case WM_TIMER:
            if (w == CaretBlinkTimer && state_.text_editing)
            {
                state_.caret_visible = !state_.caret_visible;
                invalidate();
            }
            return 0;
        case WM_LBUTTONDOWN:
            on_left_down({GET_X_LPARAM(l), GET_Y_LPARAM(l)}, false);
            return 0;
        case WM_LBUTTONDBLCLK:
            on_left_down({GET_X_LPARAM(l), GET_Y_LPARAM(l)}, true);
            return 0;
        case WM_MBUTTONDOWN:
            SetFocus(window_);
            drag_mode_ = DragMode::Pan;
            drag_start_client_ = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            drag_start_pan_x_ = state_.pan_x;
            drag_start_pan_y_ = state_.pan_y;
            SetCapture(window_);
            return 0;
        case WM_MOUSEMOVE:
            on_mouse_move({GET_X_LPARAM(l), GET_Y_LPARAM(l)});
            return 0;
        case WM_LBUTTONUP:
        case WM_MBUTTONUP:
            if (drag_mode_ != DragMode::None)
                finish_drag();
            return 0;
        case WM_CAPTURECHANGED:
            if (drag_mode_ != DragMode::None)
                finish_drag();
            return 0;
        case WM_MOUSEWHEEL:
        {
            const short delta = GET_WHEEL_DELTA_WPARAM(w);
            const bool ctrl = (LOWORD(w) & MK_CONTROL) != 0;
            const bool shift = (LOWORD(w) & MK_SHIFT) != 0;
            if (ctrl)
            {
                POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                ScreenToClient(window_, &pt);
                const float steps = static_cast<float>(delta) / 120.0f;
                const float factor = std::pow(1.22f, steps);
                update_viewport_metrics();
                set_zoom_level(effective_scale_ * factor, &pt);
            }
            else if (!state_.fit_mode)
            {
                const float amount = static_cast<float>(delta) * 0.45f;
                if (shift)
                    state_.pan_x += amount;
                else
                    state_.pan_y += amount;
                clamp_pan();
                invalidate();
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (w == VK_SPACE && !state_.text_editing)
            {
                space_down_ = true;
                POINT pt{};
                GetCursorPos(&pt);
                ScreenToClient(window_, &pt);
                update_cursor(pt);
                return 0;
            }
            break;
        case WM_KEYUP:
            if (w == VK_SPACE)
            {
                space_down_ = false;
                POINT pt{};
                GetCursorPos(&pt);
                ScreenToClient(window_, &pt);
                update_cursor(pt);
                return 0;
            }
            break;
        case WM_CHAR:
            if (state_.text_editing)
            {
                handle_text_char(static_cast<wchar_t>(w));
                return 0;
            }
            break;
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
    Editor(HINSTANCE instance, std::unique_ptr<Bitmap> image)
        : instance_(instance), document_(std::move(image))
    {
        state_.crop_draft = document_.crop_rect();
    }

    ~Editor()
    {
        canvas_.reset();
        toolbar_.reset();
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
        if (canvas_)
            SetFocus(canvas_->handle());
        UpdateWindow(hwnd);
        return hwnd;
    }

    bool busy() const noexcept
    {
        return copying_ || saving_dialog_;
    }

    bool handle_shortcut(WPARAM key, bool ctrl, bool alt, bool shift)
    {
        if (busy() || alt)
            return false;

        if (state_.text_editing && canvas_)
        {
            if (ctrl && !shift && key == 'S')
            {
                canvas_->commit_text_edit();
                save_as_png();
                return true;
            }
            if (ctrl && !shift && key == 'C' && state_.text_caret == state_.text_anchor)
            {
                canvas_->commit_text_edit();
                start_copy();
                return true;
            }
            if (canvas_->handle_text_key(key, ctrl, shift))
                return true;
            return false;
        }

        if (ctrl)
        {
            if (!shift && key == 'C')
            {
                execute_action(ActionId::Copy);
                return true;
            }
            if (!shift && key == 'S')
            {
                execute_action(ActionId::Save);
                return true;
            }
            if (!shift && key == 'Z')
            {
                execute_action(ActionId::Undo);
                return true;
            }
            if ((!shift && key == 'Y') || (shift && key == 'Z'))
            {
                execute_action(ActionId::Redo);
                return true;
            }
            if (key == '0' || key == VK_NUMPAD0)
            {
                execute_action(ActionId::ZoomFit);
                return true;
            }
            if (key == '1' || key == VK_NUMPAD1)
            {
                execute_action(ActionId::ZoomActual);
                return true;
            }
            if (key == VK_OEM_PLUS || key == VK_ADD)
            {
                execute_action(ActionId::ZoomIn);
                return true;
            }
            if (key == VK_OEM_MINUS || key == VK_SUBTRACT)
            {
                execute_action(ActionId::ZoomOut);
                return true;
            }
            return false;
        }

        if (state_.crop_active)
        {
            if (key == VK_RETURN)
            {
                execute_action(ActionId::CropApply);
                return true;
            }
            if (key == VK_ESCAPE)
            {
                execute_action(ActionId::CropCancel);
                return true;
            }
            return false;
        }

        switch (key)
        {
        case VK_ESCAPE:
            if (state_.tool != ToolKind::Select)
            {
                select_tool(ToolKind::Select);
            }
            else if (state_.selected_id != 0)
            {
                state_.selected_id = 0;
                refresh_views();
            }
            return true;
        case VK_DELETE:
        case VK_BACK:
            if (state_.selected_id != 0)
            {
                execute_action(ActionId::DeleteSelected);
                return true;
            }
            return false;
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
            if (state_.selected_id != 0)
            {
                const float step = shift ? 10.0f : 1.0f;
                const float dx = key == VK_LEFT ? -step : (key == VK_RIGHT ? step : 0.0f);
                const float dy = key == VK_UP ? -step : (key == VK_DOWN ? step : 0.0f);
                document_.begin_mutation();
                if (auto* item = document_.find_annotation_mutable(state_.selected_id))
                    translate_annotation(*item, dx, dy);
                document_.commit_mutation();
                refresh_views();
                return true;
            }
            return false;
        case 'V':
            execute_action(ActionId::ToolSelect);
            return true;
        case 'P':
            execute_action(ActionId::ToolPen);
            return true;
        case 'H':
            execute_action(ActionId::ToolHighlighter);
            return true;
        case 'A':
            execute_action(ActionId::ToolArrow);
            return true;
        case 'R':
            execute_action(ActionId::ToolRectangle);
            return true;
        case 'O':
            execute_action(ActionId::ToolOval);
            return true;
        case 'L':
            execute_action(ActionId::ToolLine);
            return true;
        case 'T':
            execute_action(ActionId::ToolText);
            return true;
        case 'N':
            execute_action(ActionId::ToolStep);
            return true;
        case 'B':
            execute_action(ActionId::ToolRedact);
            return true;
        case 'C':
            execute_action(ActionId::ToolCrop);
            return true;
        case VK_OEM_4: // '['
            step_size(-1);
            return true;
        case VK_OEM_6: // ']'
            step_size(1);
            return true;
        case VK_OEM_PLUS:
        case VK_ADD:
            execute_action(ActionId::ZoomIn);
            return true;
        case VK_OEM_MINUS:
        case VK_SUBTRACT:
            execute_action(ActionId::ZoomOut);
            return true;
        case '0':
            execute_action(ActionId::ZoomFit);
            return true;
        case '1':
            execute_action(ActionId::ZoomActual);
            return true;
        }
        return false;
    }

  private:
    HINSTANCE instance_{};
    EditorDocument document_;
    EditorViewState state_{};
    HWND window_{};
    ThemePalette palette_{};
    UINT dpi_{96};
    std::unique_ptr<EditorToolbar> toolbar_;
    std::unique_ptr<EditorCanvas> canvas_;
    bool copying_{};
    bool saving_dialog_{};
    GlobalMemory pending_;
    ULONGLONG copy_started_{};

    int px(int value) const noexcept
    {
        return MulDiv(value, static_cast<int>(dpi_), 96);
    }

    int toolbar_height() const noexcept
    {
        return px(ToolbarHeightDip);
    }

    void refresh_views() noexcept
    {
        state_.busy = busy();
        if (toolbar_)
            toolbar_->invalidate();
        if (canvas_)
            canvas_->invalidate();
    }

    void set_status(std::wstring text, bool error = false)
    {
        state_.status_text = std::move(text);
        state_.status_error = error;
        if (toolbar_)
            toolbar_->invalidate();
    }

    void clear_status()
    {
        if (!state_.status_text.empty())
        {
            state_.status_text.clear();
            state_.status_error = false;
            if (toolbar_)
                toolbar_->invalidate();
        }
    }

    void select_tool(ToolKind tool)
    {
        if (canvas_)
            canvas_->commit_text_edit();
        if (state_.crop_active && tool != ToolKind::Crop)
            state_.crop_active = false;
        state_.tool = tool;
        if (tool != ToolKind::Select)
            state_.selected_id = 0;
        if (tool == ToolKind::Crop)
        {
            state_.crop_active = true;
            state_.crop_draft = document_.crop_rect();
            if (canvas_)
                canvas_->set_zoom_fit();
        }
        clear_status();
        refresh_views();
    }

    void apply_color(COLORREF color)
    {
        if (state_.text_editing)
        {
            state_.text_draft.color = color;
            state_.ink_color = color;
            refresh_views();
            return;
        }
        if (const auto* sel = document_.find_annotation(state_.selected_id))
        {
            document_.update_annotation_style(sel->id, color, sel->size, sel->filled);
            if (sel->kind == AnnotationKind::Highlighter)
                state_.highlighter_color = color;
            else if (sel->kind == AnnotationKind::Redact)
                state_.redact_color = color;
            else
                state_.ink_color = color;
        }
        else if (state_.tool == ToolKind::Highlighter)
        {
            state_.highlighter_color = color;
        }
        else if (state_.tool == ToolKind::Redact)
        {
            state_.redact_color = color;
        }
        else
        {
            state_.ink_color = color;
        }
        clear_status();
        refresh_views();
    }

    void apply_size(SizePreset size)
    {
        if (state_.text_editing)
        {
            state_.text_draft.size = size;
            state_.ink_size = size;
            refresh_views();
            return;
        }
        if (const auto* sel = document_.find_annotation(state_.selected_id))
        {
            document_.update_annotation_style(sel->id, sel->color, size, sel->filled);
            if (sel->kind == AnnotationKind::Highlighter)
                state_.highlighter_size = size;
            else if (sel->kind == AnnotationKind::Redact)
                state_.redact_size = size;
            else
                state_.ink_size = size;
        }
        else if (state_.tool == ToolKind::Highlighter)
        {
            state_.highlighter_size = size;
        }
        else if (state_.tool == ToolKind::Redact)
        {
            state_.redact_size = size;
        }
        else
        {
            state_.ink_size = size;
        }
        clear_status();
        refresh_views();
    }

    void step_size(int delta)
    {
        SizePreset current = state_.ink_size;
        if (const auto* sel = document_.find_annotation(state_.selected_id))
            current = sel->size;
        else if (state_.tool == ToolKind::Highlighter)
            current = state_.highlighter_size;
        else if (state_.tool == ToolKind::Redact)
            current = state_.redact_size;
        const int next = std::clamp(static_cast<int>(current) + delta, 0, 3);
        apply_size(static_cast<SizePreset>(next));
    }

    void execute_action(ActionId action)
    {
        if (busy())
            return;
        switch (action)
        {
        case ActionId::ToolSelect:
            select_tool(ToolKind::Select);
            break;
        case ActionId::ToolPen:
            select_tool(ToolKind::Pen);
            break;
        case ActionId::ToolHighlighter:
            select_tool(ToolKind::Highlighter);
            break;
        case ActionId::ToolArrow:
            select_tool(ToolKind::Arrow);
            break;
        case ActionId::ToolRectangle:
            select_tool(ToolKind::Rectangle);
            break;
        case ActionId::ToolOval:
            select_tool(ToolKind::Oval);
            break;
        case ActionId::ToolLine:
            select_tool(ToolKind::Line);
            break;
        case ActionId::ToolText:
            select_tool(ToolKind::Text);
            break;
        case ActionId::ToolStep:
            select_tool(ToolKind::Step);
            break;
        case ActionId::ToolRedact:
            select_tool(ToolKind::Redact);
            break;
        case ActionId::ToolCrop:
            if (state_.crop_active)
                execute_action(ActionId::CropCancel);
            else
                select_tool(ToolKind::Crop);
            break;
        case ActionId::Undo:
            if (canvas_)
                canvas_->commit_text_edit();
            if (document_.undo())
            {
                if (canvas_)
                    canvas_->clear_patch_cache();
                if (!document_.find_annotation(state_.selected_id))
                    state_.selected_id = 0;
                state_.crop_draft = document_.crop_rect();
                clear_status();
                refresh_views();
            }
            break;
        case ActionId::Redo:
            if (canvas_)
                canvas_->commit_text_edit();
            if (document_.redo())
            {
                if (canvas_)
                    canvas_->clear_patch_cache();
                if (!document_.find_annotation(state_.selected_id))
                    state_.selected_id = 0;
                state_.crop_draft = document_.crop_rect();
                clear_status();
                refresh_views();
            }
            break;
        case ActionId::Reset:
            if (canvas_)
                canvas_->commit_text_edit();
            if (document_.reset_all())
            {
                if (canvas_)
                {
                    canvas_->clear_patch_cache();
                    canvas_->set_zoom_fit();
                }
                state_.selected_id = 0;
                state_.crop_active = false;
                state_.crop_draft = document_.crop_rect();
                set_status(L"Reset edits.");
                refresh_views();
            }
            break;
        case ActionId::Save:
            if (canvas_)
                canvas_->commit_text_edit();
            if (state_.crop_active)
            {
                document_.set_crop_rect(state_.crop_draft);
                state_.crop_active = false;
                state_.tool = ToolKind::Select;
            }
            save_as_png();
            break;
        case ActionId::Copy:
            if (canvas_)
                canvas_->commit_text_edit();
            if (state_.crop_active)
            {
                document_.set_crop_rect(state_.crop_draft);
                state_.crop_active = false;
                state_.tool = ToolKind::Select;
            }
            start_copy();
            break;
        case ActionId::Color0:
        case ActionId::Color1:
        case ActionId::Color2:
        case ActionId::Color3:
        case ActionId::Color4:
        case ActionId::Color5:
        case ActionId::Color6:
        case ActionId::Color7:
        {
            const size_t idx =
                static_cast<size_t>(static_cast<int>(action) - static_cast<int>(ActionId::Color0));
            if (idx < MarkupColors.size())
                apply_color(MarkupColors[idx]);
            break;
        }
        case ActionId::SizeS:
        case ActionId::SizeM:
        case ActionId::SizeL:
        case ActionId::SizeXL:
        {
            const int idx = static_cast<int>(action) - static_cast<int>(ActionId::SizeS);
            apply_size(static_cast<SizePreset>(idx));
            break;
        }
        case ActionId::ToggleFill:
            if (const auto* sel = document_.find_annotation(state_.selected_id);
                sel && (sel->kind == AnnotationKind::Rectangle || sel->kind == AnnotationKind::Oval))
            {
                state_.shape_filled = !sel->filled;
                document_.update_annotation_style(sel->id, sel->color, sel->size, state_.shape_filled);
            }
            else
            {
                state_.shape_filled = !state_.shape_filled;
            }
            refresh_views();
            break;
        case ActionId::ToggleTextBackground:
            if (state_.text_editing)
            {
                state_.text_draft.filled = !state_.text_draft.filled;
                state_.text_background = state_.text_draft.filled;
            }
            else if (const auto* sel = document_.find_annotation(state_.selected_id);
                     sel && sel->kind == AnnotationKind::Text)
            {
                state_.text_background = !sel->filled;
                document_.update_annotation_style(sel->id, sel->color, sel->size, state_.text_background);
            }
            else
            {
                state_.text_background = !state_.text_background;
            }
            refresh_views();
            break;
        case ActionId::RedactPixelate:
        case ActionId::RedactSolid:
        {
            const bool solid = (action == ActionId::RedactSolid);
            state_.redact_solid = solid;
            if (const auto* sel = document_.find_annotation(state_.selected_id);
                sel && sel->kind == AnnotationKind::Redact)
            {
                document_.update_annotation_style(sel->id, sel->color, sel->size, solid);
            }
            refresh_views();
            break;
        }
        case ActionId::ResetStep:
            document_.reset_step_number();
            refresh_views();
            break;
        case ActionId::DeleteSelected:
            if (state_.selected_id != 0 && document_.delete_annotation(state_.selected_id))
            {
                state_.selected_id = 0;
                refresh_views();
            }
            break;
        case ActionId::CropApply:
            if (state_.crop_active)
            {
                document_.set_crop_rect(state_.crop_draft);
                state_.crop_active = false;
                state_.tool = ToolKind::Select;
                if (canvas_)
                    canvas_->set_zoom_fit();
                set_status(L"Cropped image.");
                refresh_views();
            }
            break;
        case ActionId::CropReset:
            if (state_.crop_active)
            {
                state_.crop_draft = {0, 0, document_.base_width(), document_.base_height()};
                refresh_views();
            }
            break;
        case ActionId::CropCancel:
            if (state_.crop_active)
            {
                state_.crop_active = false;
                state_.crop_draft = document_.crop_rect();
                state_.tool = ToolKind::Select;
                if (canvas_)
                    canvas_->set_zoom_fit();
                refresh_views();
            }
            break;
        case ActionId::ZoomIn:
            if (canvas_)
                canvas_->zoom_step(1.25f);
            break;
        case ActionId::ZoomOut:
            if (canvas_)
                canvas_->zoom_step(0.80f);
            break;
        case ActionId::ZoomActual:
            if (canvas_)
                canvas_->set_zoom_level(1.0f);
            break;
        case ActionId::ZoomFit:
            if (canvas_)
                canvas_->set_zoom_fit();
            break;
        case ActionId::None:
            break;
        }
    }

    void refresh_theme()
    {
        palette_ = read_theme();
        apply_window_theme(window_, palette_);
        if (toolbar_)
            toolbar_->set_theme(palette_);
        if (canvas_)
            canvas_->set_theme(palette_);
    }

    void create()
    {
        dpi_ = GetDpiForWindow(window_);
        palette_ = read_theme();
        apply_window_theme(window_, palette_);

        toolbar_ = std::make_unique<EditorToolbar>(
            instance_, window_, document_, state_, palette_, dpi_,
            [this](ActionId action) { execute_action(action); });
        canvas_ = std::make_unique<EditorCanvas>(
            instance_, window_, document_, state_, palette_, dpi_,
            [this]()
            {
                if (toolbar_)
                    toolbar_->invalidate();
            },
            [this]() { execute_action(ActionId::CropApply); });
        layout();
    }

    void layout()
    {
        if (!window_)
            return;
        RECT client{};
        GetClientRect(window_, &client);
        const int bar_h = toolbar_height();
        if (toolbar_)
            toolbar_->resize(0, 0, client.right, bar_h);
        if (canvas_)
            canvas_->resize(0, bar_h, client.right, std::max(0, static_cast<int>(client.bottom) - bar_h));
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
        refresh_views();
        set_status(L"Copying...");
        if (!SetTimer(window_, CopyRetryTimer, 25, nullptr))
        {
            pending_.reset();
            copying_ = false;
            refresh_views();
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
            refresh_views();
            set_status(L"Copied to clipboard.");
            return;
        }
        if (result == CopyResult::Failed || GetTickCount64() - copy_started_ >= 1500)
        {
            KillTimer(window_, CopyRetryTimer);
            pending_.reset();
            copying_ = false;
            refresh_views();
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

            if (toolbar_)
                toolbar_->hide_tooltip();
            saving_dialog_ = true;
            refresh_views();
            const HRESULT chosen = dialog->Show(window_);
            saving_dialog_ = false;
            refresh_views();

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
            refresh_views();
            set_status(wide_error(error.what()), true);
            return;
        }

        try
        {
            document_.save_png(destination);
            const auto saved_name = destination.filename().wstring();
            set_status(saved_name.empty() ? L"Saved PNG." : L"Saved " + saved_name + L".");
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
        case WM_TIMER:
            if (w == CopyRetryTimer && copying_ && pending_)
                try_copy();
            return 0;
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            refresh_theme();
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(w) == WA_INACTIVE && toolbar_)
                toolbar_->hide_tooltip();
            return 0;
        case WM_DPICHANGED:
        {
            dpi_ = HIWORD(w);
            if (toolbar_)
                toolbar_->set_dpi(dpi_);
            if (canvas_)
                canvas_->set_dpi(dpi_);
            const auto* suggested = reinterpret_cast<const RECT*>(l);
            SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER);
            layout();
            return 0;
        }
        case WM_CLOSE:
            if (saving_dialog_)
                return 0;
            if (toolbar_)
                toolbar_->hide_tooltip();
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
            toolbar_.reset();
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window_, message, w, l);
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
    editor.create_window(selection_screen_rect);

    MSG message{};
    BOOL result{};
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0)
    {
        if (message.message == WM_KEYDOWN && !editor.busy())
        {
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (editor.handle_shortcut(message.wParam, ctrl, alt, shift))
                continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return result == -1 ? 1 : static_cast<int>(message.wParam);
}
} // namespace pluck
