#include "editor_core.h"
#include <shlobj.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace pluck
{
namespace
{
using Microsoft::WRL::ComPtr;

constexpr size_t MaxUndoSteps = 100;
constexpr float TextBadgePadX = 10.0f;
constexpr float TextBadgePadY = 5.0f;
constexpr float TextPlainPad = 3.0f;

struct ComApartment
{
    bool active{};
    ComApartment()
    {
        const auto hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
            throw std::runtime_error("Cannot start the image encoder.");
        active = SUCCEEDED(hr);
    }
    ~ComApartment()
    {
        if (active)
            CoUninitialize();
    }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
};

void check_hr(HRESULT hr, const char* message)
{
    if (FAILED(hr))
        throw std::runtime_error(message);
}

int preset_index(SizePreset size) noexcept
{
    return std::clamp(static_cast<int>(size), 0, 3);
}

float point_distance(PointF a, PointF b) noexcept
{
    return std::hypot(b.x - a.x, b.y - a.y);
}

float segment_distance(PointF p, PointF a, PointF b) noexcept
{
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float len_sq = dx * dx + dy * dy;
    if (len_sq <= 1e-6f)
        return point_distance(p, a);
    const float t = std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / len_sq, 0.0f, 1.0f);
    return std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
}

bool create_round_stroke_style(ID2D1Factory* d2d, ComPtr<ID2D1StrokeStyle>& style)
{
    if (!d2d)
        return false;
    const auto props = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                   D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND, 10.0f,
                                                   D2D1_DASH_STYLE_SOLID, 0.0f);
    return SUCCEEDED(d2d->CreateStrokeStyle(props, nullptr, 0, &style));
}

bool build_polyline_geometry(ID2D1Factory* d2d, const std::vector<PointF>& points,
                             ComPtr<ID2D1PathGeometry>& geometry)
{
    if (!d2d || points.size() < 2)
        return false;
    if (FAILED(d2d->CreatePathGeometry(&geometry)))
        return false;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(&sink)))
        return false;
    sink->BeginFigure(D2D1::Point2F(points[0].x, points[0].y), D2D1_FIGURE_BEGIN_HOLLOW);
    for (size_t i = 1; i + 1 < points.size(); ++i)
    {
        const float mid_x = (points[i].x + points[i + 1].x) * 0.5f;
        const float mid_y = (points[i].y + points[i + 1].y) * 0.5f;
        sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(D2D1::Point2F(points[i].x, points[i].y),
                                                              D2D1::Point2F(mid_x, mid_y)));
    }
    const auto& last = points.back();
    sink->AddLine(D2D1::Point2F(last.x, last.y));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    return SUCCEEDED(sink->Close());
}

void draw_stroke_geometry(ID2D1RenderTarget* target, ID2D1Factory* d2d, const std::vector<PointF>& points,
                          ID2D1Brush* brush, float width, ID2D1StrokeStyle* style)
{
    if (points.empty())
        return;
    if (points.size() == 1)
    {
        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(points[0].x, points[0].y), width * 0.5f,
                                          width * 0.5f),
                            brush);
        return;
    }
    ComPtr<ID2D1PathGeometry> geometry;
    if (build_polyline_geometry(d2d, points, geometry))
        target->DrawGeometry(geometry.Get(), brush, width, style);
}

ComPtr<IDWriteFactory> fallback_dwrite_factory()
{
    ComPtr<IDWriteFactory> factory;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(factory.GetAddressOf()));
    return factory;
}

void pixelate_buffer_region(std::uint32_t* pixels, int stride_pixels, int img_width, int img_height,
                            RECT region, int block_size) noexcept
{
    const int left = std::clamp<int>(region.left, 0, img_width);
    const int top = std::clamp<int>(region.top, 0, img_height);
    const int right = std::clamp<int>(region.right, 0, img_width);
    const int bottom = std::clamp<int>(region.bottom, 0, img_height);
    if (right <= left || bottom <= top)
        return;
    const int block = std::max(2, block_size);
    for (int by = top; by < bottom; by += block)
    {
        const int y_end = std::min(by + block, bottom);
        for (int bx = left; bx < right; bx += block)
        {
            const int x_end = std::min(bx + block, right);
            std::uint64_t sum_b = 0;
            std::uint64_t sum_g = 0;
            std::uint64_t sum_r = 0;
            std::uint64_t count = 0;
            for (int y = by; y < y_end; ++y)
            {
                const auto* row = pixels + static_cast<size_t>(y) * stride_pixels;
                for (int x = bx; x < x_end; ++x)
                {
                    const auto px = row[x];
                    sum_b += px & 0xffu;
                    sum_g += (px >> 8) & 0xffu;
                    sum_r += (px >> 16) & 0xffu;
                    ++count;
                }
            }
            if (count == 0)
                continue;
            const auto avg_b = static_cast<std::uint32_t>((sum_b + count / 2) / count);
            const auto avg_g = static_cast<std::uint32_t>((sum_g + count / 2) / count);
            const auto avg_r = static_cast<std::uint32_t>((sum_r + count / 2) / count);
            const std::uint32_t value = 0xff000000u | (avg_r << 16) | (avg_g << 8) | avg_b;
            for (int y = by; y < y_end; ++y)
            {
                auto* row = pixels + static_cast<size_t>(y) * stride_pixels;
                for (int x = bx; x < x_end; ++x)
                    row[x] = value;
            }
        }
    }
}

bool render_highlighter_mask(const Annotation& item, RECT bounds, ID2D1Factory* d2d, IWICImagingFactory* wic,
                             std::vector<std::uint8_t>& alpha_out)
{
    const int w = bounds.right - bounds.left;
    const int h = bounds.bottom - bounds.top;
    if (w <= 0 || h <= 0 || !d2d || !wic)
        return false;
    ComPtr<IWICBitmap> mask_bitmap;
    if (FAILED(wic->CreateBitmap(static_cast<UINT>(w), static_cast<UINT>(h), GUID_WICPixelFormat32bppPBGRA,
                                 WICBitmapCacheOnLoad, &mask_bitmap)))
        return false;
    const auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    ComPtr<ID2D1RenderTarget> target;
    if (FAILED(d2d->CreateWicBitmapRenderTarget(mask_bitmap.Get(), props, &target)))
        return false;
    ComPtr<ID2D1SolidColorBrush> white;
    if (FAILED(target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &white)))
        return false;
    ComPtr<ID2D1StrokeStyle> style;
    create_round_stroke_style(d2d, style);

    target->BeginDraw();
    target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    target->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(-bounds.left),
                                                       static_cast<float>(-bounds.top)));
    draw_stroke_geometry(target.Get(), d2d, item.points, white.Get(),
                         stroke_width_for(AnnotationKind::Highlighter, item.size), style.Get());
    if (FAILED(target->EndDraw()))
        return false;

    WICRect lock_rect{0, 0, w, h};
    ComPtr<IWICBitmapLock> lock;
    if (FAILED(mask_bitmap->Lock(&lock_rect, WICBitmapLockRead, &lock)))
        return false;
    UINT stride{};
    UINT buffer_size{};
    BYTE* data{};
    if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&buffer_size, &data)) || !data)
        return false;

    alpha_out.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
    for (int y = 0; y < h; ++y)
    {
        const auto* row = reinterpret_cast<const std::uint32_t*>(data + static_cast<size_t>(y) * stride);
        auto* dst = alpha_out.data() + static_cast<size_t>(y) * static_cast<size_t>(w);
        for (int x = 0; x < w; ++x)
            dst[x] = static_cast<std::uint8_t>((row[x] >> 24) & 0xffu);
    }
    return true;
}

bool is_light_region(const std::uint32_t* pixels, int stride_pixels, RECT bounds,
                     const std::uint8_t* alpha_mask) noexcept
{
    const int w = bounds.right - bounds.left;
    const int h = bounds.bottom - bounds.top;
    double luma_sum = 0.0;
    std::uint64_t count = 0;
    for (int y = 0; y < h; ++y)
    {
        const auto* row = pixels + static_cast<size_t>(bounds.top + y) * stride_pixels + bounds.left;
        const auto* mask_row = alpha_mask + static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x)
        {
            if (mask_row[x] < 96)
                continue;
            const auto px = row[x];
            const float b = static_cast<float>(px & 0xffu) / 255.0f;
            const float g = static_cast<float>((px >> 8) & 0xffu) / 255.0f;
            const float r = static_cast<float>((px >> 16) & 0xffu) / 255.0f;
            luma_sum += 0.299 * r + 0.587 * g + 0.114 * b;
            ++count;
        }
    }
    if (count == 0)
        return true;
    return (luma_sum / static_cast<double>(count)) > 0.48;
}

void blend_highlighter_pixel(std::uint32_t base_px, COLORREF color, bool use_multiply, float& out_r,
                             float& out_g, float& out_b) noexcept
{
    const float bb = static_cast<float>(base_px & 0xffu) / 255.0f;
    const float bg = static_cast<float>((base_px >> 8) & 0xffu) / 255.0f;
    const float br = static_cast<float>((base_px >> 16) & 0xffu) / 255.0f;
    const float cr = static_cast<float>(GetRValue(color)) / 255.0f;
    const float cg = static_cast<float>(GetGValue(color)) / 255.0f;
    const float cb = static_cast<float>(GetBValue(color)) / 255.0f;
    if (use_multiply)
    {
        // Keep dark text sharp while tinting light paper backgrounds vibrantly.
        const float tr = 0.22f + 0.78f * cr;
        const float tg = 0.22f + 0.78f * cg;
        const float tb = 0.22f + 0.78f * cb;
        out_r = br * tr;
        out_g = bg * tg;
        out_b = bb * tb;
    }
    else
    {
        // Over dark UI surfaces, a soft translucent tint stays clearly visible.
        out_r = br * 0.60f + cr * 0.40f;
        out_g = bg * 0.60f + cg * 0.40f;
        out_b = bb * 0.60f + cb * 0.40f;
    }
}

void apply_highlighter_to_buffer(std::uint32_t* pixels, int stride_pixels, int img_width, int img_height,
                                 const Annotation& item, POINT offset, ID2D1Factory* d2d,
                                 IWICImagingFactory* wic)
{
    if (item.points.empty())
        return;
    Annotation shifted = item;
    for (auto& pt : shifted.points)
    {
        pt.x -= static_cast<float>(offset.x);
        pt.y -= static_cast<float>(offset.y);
    }
    const RectF raw = annotation_bounds(shifted, nullptr);
    const RECT bounds{
        std::clamp(static_cast<int>(std::floor(raw.left)) - 2, 0, img_width),
        std::clamp(static_cast<int>(std::floor(raw.top)) - 2, 0, img_height),
        std::clamp(static_cast<int>(std::ceil(raw.right)) + 2, 0, img_width),
        std::clamp(static_cast<int>(std::ceil(raw.bottom)) + 2, 0, img_height),
    };
    if (!has_area(bounds))
        return;
    std::vector<std::uint8_t> mask;
    if (!render_highlighter_mask(shifted, bounds, d2d, wic, mask))
        return;
    const bool multiply = is_light_region(pixels, stride_pixels, bounds, mask.data());
    const int w = bounds.right - bounds.left;
    const int h = bounds.bottom - bounds.top;
    for (int y = 0; y < h; ++y)
    {
        auto* row = pixels + static_cast<size_t>(bounds.top + y) * stride_pixels + bounds.left;
        const auto* mask_row = mask.data() + static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x)
        {
            const std::uint8_t cov = mask_row[x];
            if (cov == 0)
                continue;
            const auto base_px = row[x];
            float tr = 0.0f;
            float tg = 0.0f;
            float tb = 0.0f;
            blend_highlighter_pixel(base_px, item.color, multiply, tr, tg, tb);
            const float t = static_cast<float>(cov) / 255.0f;
            const float bb = static_cast<float>(base_px & 0xffu) / 255.0f;
            const float bg = static_cast<float>((base_px >> 8) & 0xffu) / 255.0f;
            const float br = static_cast<float>((base_px >> 16) & 0xffu) / 255.0f;
            const auto out_r = static_cast<std::uint32_t>(std::clamp(std::lround((br * (1.0f - t) + tr * t) * 255.0f), 0l, 255l));
            const auto out_g = static_cast<std::uint32_t>(std::clamp(std::lround((bg * (1.0f - t) + tg * t) * 255.0f), 0l, 255l));
            const auto out_b = static_cast<std::uint32_t>(std::clamp(std::lround((bb * (1.0f - t) + tb * t) * 255.0f), 0l, 255l));
            row[x] = 0xff000000u | (out_r << 16) | (out_g << 8) | out_b;
        }
    }
}

bool state_equals(const std::vector<Annotation>& a, const std::vector<Annotation>& b) noexcept
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
    {
        if (a[i].id != b[i].id || a[i].kind != b[i].kind || a[i].color != b[i].color ||
            a[i].size != b[i].size || a[i].filled != b[i].filled || a[i].start.x != b[i].start.x ||
            a[i].start.y != b[i].start.y || a[i].end.x != b[i].end.x || a[i].end.y != b[i].end.y ||
            a[i].text != b[i].text || a[i].step_number != b[i].step_number ||
            a[i].points.size() != b[i].points.size())
            return false;
        for (size_t p = 0; p < a[i].points.size(); ++p)
        {
            if (a[i].points[p].x != b[i].points[p].x || a[i].points[p].y != b[i].points[p].y)
                return false;
        }
    }
    return true;
}
} // namespace

float stroke_width_for(AnnotationKind kind, SizePreset size) noexcept
{
    const int idx = preset_index(size);
    if (kind == AnnotationKind::Highlighter)
    {
        constexpr float widths[4]{14.0f, 22.0f, 32.0f, 46.0f};
        return widths[idx];
    }
    constexpr float widths[4]{2.0f, 4.0f, 7.0f, 11.0f};
    return widths[idx];
}

float font_size_for(SizePreset size) noexcept
{
    constexpr float sizes[4]{16.0f, 22.0f, 30.0f, 42.0f};
    return sizes[preset_index(size)];
}

float step_radius_for(SizePreset size) noexcept
{
    constexpr float radii[4]{12.0f, 15.0f, 20.0f, 26.0f};
    return radii[preset_index(size)];
}

int pixelate_block_for(SizePreset size) noexcept
{
    constexpr int blocks[4]{8, 12, 18, 26};
    return blocks[preset_index(size)];
}

bool is_light_color(COLORREF color) noexcept
{
    const double luma = 0.299 * GetRValue(color) + 0.587 * GetGValue(color) + 0.114 * GetBValue(color);
    return luma >= 165.0;
}

D2D1_COLOR_F to_d2d_color(COLORREF color, float alpha) noexcept
{
    return D2D1::ColorF(GetRValue(color) / 255.0f, GetGValue(color) / 255.0f, GetBValue(color) / 255.0f,
                        alpha);
}

PointF constrain_angle_45(PointF start, PointF end) noexcept
{
    const float dx = end.x - start.x;
    const float dy = end.y - start.y;
    const float len = std::hypot(dx, dy);
    if (len < 1e-4f)
        return end;
    constexpr float step = 3.14159265358979323846f / 4.0f;
    const float angle = std::round(std::atan2(dy, dx) / step) * step;
    return {start.x + std::cos(angle) * len, start.y + std::sin(angle) * len};
}

PointF constrain_square(PointF start, PointF end) noexcept
{
    const float dx = end.x - start.x;
    const float dy = end.y - start.y;
    const float side = std::max(std::abs(dx), std::abs(dy));
    return {start.x + (dx < 0.0f ? -side : side), start.y + (dy < 0.0f ? -side : side)};
}

RectF normalize_rect(PointF a, PointF b) noexcept
{
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y)};
}

RECT clamp_crop_rect(RECT rect, int max_width, int max_height) noexcept
{
    if (max_width <= 0 || max_height <= 0)
        return {0, 0, 0, 0};
    LONG left = std::clamp<LONG>(std::min(rect.left, rect.right), 0, max_width - 1);
    LONG top = std::clamp<LONG>(std::min(rect.top, rect.bottom), 0, max_height - 1);
    LONG right = std::clamp<LONG>(std::max(rect.left, rect.right), left + 1, max_width);
    LONG bottom = std::clamp<LONG>(std::max(rect.top, rect.bottom), top + 1, max_height);
    return {left, top, right, bottom};
}

void measure_text_annotation(IDWriteFactory* dwrite, const Annotation& item, float& out_width,
                             float& out_height, ComPtr<IDWriteTextLayout>* out_layout)
{
    const float font_size = font_size_for(item.size);
    const std::wstring& sample = item.text.empty() ? std::wstring(L" ") : item.text;
    ComPtr<IDWriteFactory> owned;
    if (!dwrite)
    {
        owned = fallback_dwrite_factory();
        dwrite = owned.Get();
    }
    if (dwrite)
    {
        ComPtr<IDWriteTextFormat> format;
        if (SUCCEEDED(dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                               DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                               font_size, L"", &format)))
        {
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(dwrite->CreateTextLayout(item.text.c_str(), static_cast<UINT32>(item.text.size()),
                                                   format.Get(), 10000.0f, 10000.0f, &layout)))
            {
                DWRITE_TEXT_METRICS metrics{};
                if (SUCCEEDED(layout->GetMetrics(&metrics)))
                {
                    out_width = std::max(font_size * 0.6f, metrics.widthIncludingTrailingWhitespace);
                    out_height = std::max(font_size * 1.25f, metrics.height);
                    if (out_layout)
                        *out_layout = layout;
                    return;
                }
            }
        }
    }
    size_t max_chars = 1;
    size_t lines = 1;
    size_t current = 0;
    for (wchar_t ch : sample)
    {
        if (ch == L'\n')
        {
            ++lines;
            max_chars = std::max(max_chars, current);
            current = 0;
        }
        else if (ch != L'\r')
        {
            ++current;
        }
    }
    max_chars = std::max(max_chars, current);
    out_width = std::max(font_size * 0.6f, static_cast<float>(max_chars) * font_size * 0.56f);
    out_height = static_cast<float>(lines) * font_size * 1.3f;
}

RectF annotation_bounds(const Annotation& item, IDWriteFactory* dwrite)
{
    switch (item.kind)
    {
    case AnnotationKind::Pen:
    case AnnotationKind::Highlighter:
    {
        if (item.points.empty())
            return {0, 0, 0, 0};
        float min_x = item.points[0].x;
        float min_y = item.points[0].y;
        float max_x = min_x;
        float max_y = min_y;
        for (const auto& pt : item.points)
        {
            min_x = std::min(min_x, pt.x);
            min_y = std::min(min_y, pt.y);
            max_x = std::max(max_x, pt.x);
            max_y = std::max(max_y, pt.y);
        }
        const float half = stroke_width_for(item.kind, item.size) * 0.5f;
        return {min_x - half, min_y - half, max_x + half, max_y + half};
    }
    case AnnotationKind::Arrow:
    case AnnotationKind::Line:
    {
        const RectF r = normalize_rect(item.start, item.end);
        const float w = stroke_width_for(item.kind, item.size);
        const float pad = item.kind == AnnotationKind::Arrow ? std::max(w * 2.0f, 10.0f) : w * 0.5f;
        return {r.left - pad, r.top - pad, r.right + pad, r.bottom + pad};
    }
    case AnnotationKind::Rectangle:
    case AnnotationKind::Oval:
    {
        const RectF r = normalize_rect(item.start, item.end);
        const float half = stroke_width_for(item.kind, item.size) * 0.5f;
        return {r.left - half, r.top - half, r.right + half, r.bottom + half};
    }
    case AnnotationKind::Redact:
        return normalize_rect(item.start, item.end);
    case AnnotationKind::Text:
    {
        float text_w = 0.0f;
        float text_h = 0.0f;
        measure_text_annotation(dwrite, item, text_w, text_h);
        const float pad_x = item.filled ? TextBadgePadX : TextPlainPad;
        const float pad_y = item.filled ? TextBadgePadY : TextPlainPad;
        return {item.start.x - pad_x, item.start.y - pad_y, item.start.x + text_w + pad_x,
                item.start.y + text_h + pad_y};
    }
    case AnnotationKind::Step:
    {
        const float r = step_radius_for(item.size);
        return {item.start.x - r, item.start.y - r, item.start.x + r, item.start.y + r};
    }
    }
    return {0, 0, 0, 0};
}

bool hit_test_annotation(const Annotation& item, PointF point, float tolerance, IDWriteFactory* dwrite)
{
    switch (item.kind)
    {
    case AnnotationKind::Pen:
    case AnnotationKind::Highlighter:
    {
        if (item.points.empty())
            return false;
        const float radius = stroke_width_for(item.kind, item.size) * 0.5f + tolerance;
        if (item.points.size() == 1)
            return point_distance(point, item.points[0]) <= radius;
        for (size_t i = 0; i + 1 < item.points.size(); ++i)
        {
            if (segment_distance(point, item.points[i], item.points[i + 1]) <= radius)
                return true;
        }
        return false;
    }
    case AnnotationKind::Line:
    case AnnotationKind::Arrow:
    {
        const float w = stroke_width_for(item.kind, item.size);
        const float radius = w * 0.5f + tolerance + (item.kind == AnnotationKind::Arrow ? 3.0f : 0.0f);
        return segment_distance(point, item.start, item.end) <= radius;
    }
    case AnnotationKind::Rectangle:
    {
        const RectF r = normalize_rect(item.start, item.end);
        const float half = stroke_width_for(item.kind, item.size) * 0.5f + tolerance;
        if (item.filled || (r.right - r.left) < 20.0f || (r.bottom - r.top) < 20.0f)
        {
            return point.x >= r.left - half && point.x <= r.right + half && point.y >= r.top - half &&
                   point.y <= r.bottom + half;
        }
        const bool in_outer = point.x >= r.left - half && point.x <= r.right + half &&
                              point.y >= r.top - half && point.y <= r.bottom + half;
        const bool in_inner = point.x > r.left + half && point.x < r.right - half &&
                              point.y > r.top + half && point.y < r.bottom - half;
        return in_outer && !in_inner;
    }
    case AnnotationKind::Oval:
    {
        const RectF r = normalize_rect(item.start, item.end);
        const float cx = (r.left + r.right) * 0.5f;
        const float cy = (r.top + r.bottom) * 0.5f;
        const float rx = std::max(1.0f, (r.right - r.left) * 0.5f);
        const float ry = std::max(1.0f, (r.bottom - r.top) * 0.5f);
        const float half = stroke_width_for(item.kind, item.size) * 0.5f + tolerance;
        const float dx = point.x - cx;
        const float dy = point.y - cy;
        const float outer = (dx * dx) / ((rx + half) * (rx + half)) + (dy * dy) / ((ry + half) * (ry + half));
        if (outer > 1.0f)
            return false;
        if (item.filled || rx <= half || ry <= half)
            return true;
        const float inner = (dx * dx) / ((rx - half) * (rx - half)) + (dy * dy) / ((ry - half) * (ry - half));
        return inner >= 1.0f;
    }
    case AnnotationKind::Redact:
    {
        const RectF r = normalize_rect(item.start, item.end);
        return point.x >= r.left - tolerance && point.x <= r.right + tolerance &&
               point.y >= r.top - tolerance && point.y <= r.bottom + tolerance;
    }
    case AnnotationKind::Text:
    {
        const RectF r = annotation_bounds(item, dwrite);
        return point.x >= r.left - tolerance && point.x <= r.right + tolerance &&
               point.y >= r.top - tolerance && point.y <= r.bottom + tolerance;
    }
    case AnnotationKind::Step:
        return point_distance(point, item.start) <= step_radius_for(item.size) + tolerance;
    }
    return false;
}

HandleKind hit_test_handles(const Annotation& item, PointF point, float handle_radius, IDWriteFactory* dwrite)
{
    if (item.kind == AnnotationKind::Line || item.kind == AnnotationKind::Arrow)
    {
        if (point_distance(point, item.end) <= handle_radius)
            return HandleKind::End;
        if (point_distance(point, item.start) <= handle_radius)
            return HandleKind::Start;
        return HandleKind::None;
    }
    if (item.kind == AnnotationKind::Rectangle || item.kind == AnnotationKind::Oval ||
        item.kind == AnnotationKind::Redact)
    {
        const RectF r = normalize_rect(item.start, item.end);
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
            if (point_distance(point, pos) <= handle_radius)
                return kind;
        }
        return HandleKind::None;
    }
    if (item.kind == AnnotationKind::Pen || item.kind == AnnotationKind::Highlighter)
    {
        const RectF r = annotation_bounds(item, dwrite);
        const std::pair<HandleKind, PointF> corners[] = {
            {HandleKind::TopLeft, {r.left, r.top}},
            {HandleKind::TopRight, {r.right, r.top}},
            {HandleKind::BottomRight, {r.right, r.bottom}},
            {HandleKind::BottomLeft, {r.left, r.bottom}},
        };
        for (const auto& [kind, pos] : corners)
        {
            if (point_distance(point, pos) <= handle_radius)
                return kind;
        }
    }
    return HandleKind::None;
}

void translate_annotation(Annotation& item, float dx, float dy) noexcept
{
    item.start.x += dx;
    item.start.y += dy;
    item.end.x += dx;
    item.end.y += dy;
    for (auto& pt : item.points)
    {
        pt.x += dx;
        pt.y += dy;
    }
    ++item.revision;
}

void resize_annotation(Annotation& item, HandleKind handle, PointF target, bool keep_aspect,
                       const RectF& initial_bounds, const std::vector<PointF>& initial_points) noexcept
{
    if (item.kind == AnnotationKind::Line || item.kind == AnnotationKind::Arrow)
    {
        if (handle == HandleKind::Start)
            item.start = keep_aspect ? constrain_angle_45(item.end, target) : target;
        else if (handle == HandleKind::End)
            item.end = keep_aspect ? constrain_angle_45(item.start, target) : target;
        ++item.revision;
        return;
    }

    if (item.kind == AnnotationKind::Rectangle || item.kind == AnnotationKind::Oval ||
        item.kind == AnnotationKind::Redact)
    {
        float left = initial_bounds.left;
        float top = initial_bounds.top;
        float right = initial_bounds.right;
        float bottom = initial_bounds.bottom;
        switch (handle)
        {
        case HandleKind::TopLeft:
            if (keep_aspect)
            {
                const auto p = constrain_square({right, bottom}, target);
                left = p.x;
                top = p.y;
            }
            else
            {
                left = target.x;
                top = target.y;
            }
            break;
        case HandleKind::TopRight:
            if (keep_aspect)
            {
                const auto p = constrain_square({left, bottom}, target);
                right = p.x;
                top = p.y;
            }
            else
            {
                right = target.x;
                top = target.y;
            }
            break;
        case HandleKind::BottomRight:
            if (keep_aspect)
            {
                const auto p = constrain_square({left, top}, target);
                right = p.x;
                bottom = p.y;
            }
            else
            {
                right = target.x;
                bottom = target.y;
            }
            break;
        case HandleKind::BottomLeft:
            if (keep_aspect)
            {
                const auto p = constrain_square({right, top}, target);
                left = p.x;
                bottom = p.y;
            }
            else
            {
                left = target.x;
                bottom = target.y;
            }
            break;
        case HandleKind::Top:
            top = target.y;
            break;
        case HandleKind::Bottom:
            bottom = target.y;
            break;
        case HandleKind::Left:
            left = target.x;
            break;
        case HandleKind::Right:
            right = target.x;
            break;
        default:
            break;
        }
        item.start = {std::min(left, right), std::min(top, bottom)};
        item.end = {std::max(left, right), std::max(top, bottom)};
        ++item.revision;
        return;
    }

    if ((item.kind == AnnotationKind::Pen || item.kind == AnnotationKind::Highlighter) &&
        !initial_points.empty())
    {
        const float half = stroke_width_for(item.kind, item.size) * 0.5f;
        const float old_l = initial_bounds.left + half;
        const float old_t = initial_bounds.top + half;
        const float old_r = std::max(old_l + 1.0f, initial_bounds.right - half);
        const float old_b = std::max(old_t + 1.0f, initial_bounds.bottom - half);
        float new_l = old_l;
        float new_t = old_t;
        float new_r = old_r;
        float new_b = old_b;
        switch (handle)
        {
        case HandleKind::TopLeft:
            new_l = target.x + half;
            new_t = target.y + half;
            break;
        case HandleKind::TopRight:
            new_r = target.x - half;
            new_t = target.y + half;
            break;
        case HandleKind::BottomRight:
            new_r = target.x - half;
            new_b = target.y - half;
            break;
        case HandleKind::BottomLeft:
            new_l = target.x + half;
            new_b = target.y - half;
            break;
        default:
            break;
        }
        const float old_w = old_r - old_l;
        const float old_h = old_b - old_t;
        item.points.resize(initial_points.size());
        for (size_t i = 0; i < initial_points.size(); ++i)
        {
            const float ux = (initial_points[i].x - old_l) / old_w;
            const float uy = (initial_points[i].y - old_t) / old_h;
            item.points[i] = {new_l + ux * (new_r - new_l), new_t + uy * (new_b - new_t)};
        }
        ++item.revision;
    }
}

PixelatePatch compute_pixelate_patch(const Bitmap& base, const Annotation& item)
{
    PixelatePatch patch{};
    const RectF r = normalize_rect(item.start, item.end);
    const RECT bounds{
        std::clamp(static_cast<int>(std::floor(r.left)), 0, base.width()),
        std::clamp(static_cast<int>(std::floor(r.top)), 0, base.height()),
        std::clamp(static_cast<int>(std::ceil(r.right)), 0, base.width()),
        std::clamp(static_cast<int>(std::ceil(r.bottom)), 0, base.height()),
    };
    if (!has_area(bounds))
        return patch;
    patch.bounds = bounds;
    const int w = bounds.right - bounds.left;
    const int h = bounds.bottom - bounds.top;
    patch.bgra.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
    for (int y = 0; y < h; ++y)
    {
        const auto* src = base.pixels() + static_cast<size_t>(bounds.top + y) * base.width() + bounds.left;
        auto* dst = patch.bgra.data() + static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x)
            dst[x] = 0xff000000u | (src[x] & 0x00ffffffu);
    }
    pixelate_buffer_region(patch.bgra.data(), w, w, h, {0, 0, w, h}, pixelate_block_for(item.size));
    return patch;
}

HighlighterPatch compute_highlighter_patch(const Bitmap& base, const Annotation& item, ID2D1Factory* d2d,
                                           IWICImagingFactory* wic)
{
    HighlighterPatch patch{};
    if (item.points.empty() || !d2d || !wic)
        return patch;
    const RectF raw = annotation_bounds(item, nullptr);
    const RECT bounds{
        std::clamp(static_cast<int>(std::floor(raw.left)) - 2, 0, base.width()),
        std::clamp(static_cast<int>(std::floor(raw.top)) - 2, 0, base.height()),
        std::clamp(static_cast<int>(std::ceil(raw.right)) + 2, 0, base.width()),
        std::clamp(static_cast<int>(std::ceil(raw.bottom)) + 2, 0, base.height()),
    };
    if (!has_area(bounds))
        return patch;
    std::vector<std::uint8_t> mask;
    if (!render_highlighter_mask(item, bounds, d2d, wic, mask))
        return patch;

    const bool multiply = is_light_region(base.pixels(), base.width(), bounds, mask.data());
    const int w = bounds.right - bounds.left;
    const int h = bounds.bottom - bounds.top;
    patch.bounds = bounds;
    patch.premul_bgra.resize(static_cast<size_t>(w) * static_cast<size_t>(h), 0u);
    for (int y = 0; y < h; ++y)
    {
        const auto* base_row = base.pixels() + static_cast<size_t>(bounds.top + y) * base.width() + bounds.left;
        const auto* mask_row = mask.data() + static_cast<size_t>(y) * w;
        auto* dst_row = patch.premul_bgra.data() + static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x)
        {
            const std::uint8_t cov = mask_row[x];
            if (cov == 0)
                continue;
            float tr = 0.0f;
            float tg = 0.0f;
            float tb = 0.0f;
            blend_highlighter_pixel(base_row[x], item.color, multiply, tr, tg, tb);
            const float a = static_cast<float>(cov) / 255.0f;
            const auto pr = static_cast<std::uint32_t>(std::clamp(std::lround(tr * a * 255.0f), 0l, 255l));
            const auto pg = static_cast<std::uint32_t>(std::clamp(std::lround(tg * a * 255.0f), 0l, 255l));
            const auto pb = static_cast<std::uint32_t>(std::clamp(std::lround(tb * a * 255.0f), 0l, 255l));
            dst_row[x] = (static_cast<std::uint32_t>(cov) << 24) | (pr << 16) | (pg << 8) | pb;
        }
    }
    return patch;
}

void draw_vector_annotation(ID2D1RenderTarget* target, ID2D1Factory* d2d, IDWriteFactory* dwrite,
                            const Annotation& item, bool preview_highlighter)
{
    if (!target || !d2d)
        return;

    ComPtr<ID2D1SolidColorBrush> brush;
    const float alpha = (item.kind == AnnotationKind::Highlighter && preview_highlighter) ? 0.42f : 1.0f;
    if (FAILED(target->CreateSolidColorBrush(to_d2d_color(item.color, alpha), &brush)))
        return;

    ComPtr<ID2D1StrokeStyle> round_style;
    create_round_stroke_style(d2d, round_style);

    switch (item.kind)
    {
    case AnnotationKind::Pen:
        draw_stroke_geometry(target, d2d, item.points, brush.Get(),
                             stroke_width_for(item.kind, item.size), round_style.Get());
        break;

    case AnnotationKind::Highlighter:
        if (preview_highlighter)
        {
            draw_stroke_geometry(target, d2d, item.points, brush.Get(),
                                 stroke_width_for(item.kind, item.size), round_style.Get());
        }
        break;

    case AnnotationKind::Line:
        target->DrawLine(D2D1::Point2F(item.start.x, item.start.y), D2D1::Point2F(item.end.x, item.end.y),
                         brush.Get(), stroke_width_for(item.kind, item.size), round_style.Get());
        break;

    case AnnotationKind::Arrow:
    {
        const float dx = item.end.x - item.start.x;
        const float dy = item.end.y - item.start.y;
        const float len = std::hypot(dx, dy);
        if (len < 1.0f)
            break;
        const float w = stroke_width_for(item.kind, item.size);
        const float ux = dx / len;
        const float uy = dy / len;
        const float nx = -uy;
        const float ny = ux;
        const float head_len = std::min(len * 0.65f, std::max(13.0f, w * 3.8f));
        const float head_half_w = head_len * 0.46f;
        const float shaft_trim = head_len * 0.72f;
        if (len > shaft_trim)
        {
            target->DrawLine(D2D1::Point2F(item.start.x, item.start.y),
                             D2D1::Point2F(item.end.x - ux * shaft_trim, item.end.y - uy * shaft_trim),
                             brush.Get(), w, round_style.Get());
        }
        ComPtr<ID2D1PathGeometry> head;
        if (SUCCEEDED(d2d->CreatePathGeometry(&head)))
        {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(head->Open(&sink)))
            {
                const D2D1_POINT_2F tip = D2D1::Point2F(item.end.x, item.end.y);
                const D2D1_POINT_2F left = D2D1::Point2F(item.end.x - ux * head_len + nx * head_half_w,
                                                         item.end.y - uy * head_len + ny * head_half_w);
                const D2D1_POINT_2F notch = D2D1::Point2F(item.end.x - ux * (head_len * 0.78f),
                                                          item.end.y - uy * (head_len * 0.78f));
                const D2D1_POINT_2F right = D2D1::Point2F(item.end.x - ux * head_len - nx * head_half_w,
                                                          item.end.y - uy * head_len - ny * head_half_w);
                sink->BeginFigure(tip, D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(left);
                sink->AddLine(notch);
                sink->AddLine(right);
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                if (SUCCEEDED(sink->Close()))
                    target->FillGeometry(head.Get(), brush.Get());
            }
        }
        break;
    }

    case AnnotationKind::Rectangle:
    {
        const RectF r = normalize_rect(item.start, item.end);
        const D2D1_RECT_F d2d_rect = D2D1::RectF(r.left, r.top, r.right, r.bottom);
        if (item.filled)
        {
            ComPtr<ID2D1SolidColorBrush> fill;
            if (SUCCEEDED(target->CreateSolidColorBrush(to_d2d_color(item.color, 0.22f), &fill)))
                target->FillRectangle(d2d_rect, fill.Get());
        }
        target->DrawRectangle(d2d_rect, brush.Get(), stroke_width_for(item.kind, item.size));
        break;
    }

    case AnnotationKind::Oval:
    {
        const RectF r = normalize_rect(item.start, item.end);
        const D2D1_ELLIPSE ellipse = D2D1::Ellipse(
            D2D1::Point2F((r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f),
            (r.right - r.left) * 0.5f, (r.bottom - r.top) * 0.5f);
        if (item.filled)
        {
            ComPtr<ID2D1SolidColorBrush> fill;
            if (SUCCEEDED(target->CreateSolidColorBrush(to_d2d_color(item.color, 0.22f), &fill)))
                target->FillEllipse(ellipse, fill.Get());
        }
        target->DrawEllipse(ellipse, brush.Get(), stroke_width_for(item.kind, item.size));
        break;
    }

    case AnnotationKind::Redact:
    {
        if (item.filled)
        {
            const RectF r = normalize_rect(item.start, item.end);
            target->FillRectangle(D2D1::RectF(r.left, r.top, r.right, r.bottom), brush.Get());
        }
        break;
    }

    case AnnotationKind::Text:
    {
        if (item.text.empty())
            break;
        float text_w = 0.0f;
        float text_h = 0.0f;
        ComPtr<IDWriteTextLayout> layout;
        measure_text_annotation(dwrite, item, text_w, text_h, &layout);
        if (!layout)
            break;
        if (item.filled)
        {
            const D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(
                D2D1::RectF(item.start.x - TextBadgePadX, item.start.y - TextBadgePadY,
                            item.start.x + text_w + TextBadgePadX, item.start.y + text_h + TextBadgePadY),
                6.0f, 6.0f);
            target->FillRoundedRectangle(pill, brush.Get());
            const COLORREF ink = is_light_color(item.color) ? RGB(17, 24, 39) : RGB(255, 255, 255);
            ComPtr<ID2D1SolidColorBrush> text_brush;
            if (SUCCEEDED(target->CreateSolidColorBrush(to_d2d_color(ink, 1.0f), &text_brush)))
                target->DrawTextLayout(D2D1::Point2F(item.start.x, item.start.y), layout.Get(),
                                       text_brush.Get());
        }
        else
        {
            const bool dark_ink =
                (0.299 * GetRValue(item.color) + 0.587 * GetGValue(item.color) + 0.114 * GetBValue(item.color)) <
                65.0;
            const D2D1_COLOR_F halo_color =
                dark_ink ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.72f) : D2D1::ColorF(0.03f, 0.05f, 0.05f, 0.58f);
            ComPtr<ID2D1SolidColorBrush> halo_brush;
            if (SUCCEEDED(target->CreateSolidColorBrush(halo_color, &halo_brush)))
            {
                constexpr PointF offsets[]{{-1.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, -1.0f}, {0.0f, 1.2f}};
                for (const auto& off : offsets)
                {
                    target->DrawTextLayout(D2D1::Point2F(item.start.x + off.x, item.start.y + off.y),
                                           layout.Get(), halo_brush.Get());
                }
            }
            target->DrawTextLayout(D2D1::Point2F(item.start.x, item.start.y), layout.Get(), brush.Get());
        }
        break;
    }

    case AnnotationKind::Step:
    {
        const float r = step_radius_for(item.size);
        const D2D1_POINT_2F center = D2D1::Point2F(item.start.x, item.start.y);
        ComPtr<ID2D1SolidColorBrush> shadow;
        if (SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.28f), &shadow)))
        {
            target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(center.x, center.y + 1.5f), r + 1.0f, r + 1.0f),
                                shadow.Get());
        }
        target->FillEllipse(D2D1::Ellipse(center, r, r), brush.Get());
        const COLORREF contrast = is_light_color(item.color) ? RGB(17, 24, 39) : RGB(255, 255, 255);
        ComPtr<ID2D1SolidColorBrush> ring;
        if (SUCCEEDED(target->CreateSolidColorBrush(to_d2d_color(contrast, 0.92f), &ring)))
            target->DrawEllipse(D2D1::Ellipse(center, r - 0.8f, r - 0.8f), ring.Get(), 1.8f);

        ComPtr<IDWriteFactory> owned;
        if (!dwrite)
        {
            owned = fallback_dwrite_factory();
            dwrite = owned.Get();
        }
        if (dwrite && ring)
        {
            const std::wstring label = std::to_wstring(std::max(1, item.step_number));
            const float label_size = label.size() > 1 ? r * 0.96f : r * 1.15f;
            ComPtr<IDWriteTextFormat> fmt;
            if (SUCCEEDED(dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
                                                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                   label_size, L"", &fmt)))
            {
                fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                target->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()), fmt.Get(),
                                  D2D1::RectF(center.x - r, center.y - r, center.x + r, center.y + r),
                                  ring.Get());
            }
        }
        break;
    }
    }
}

EditorDocument::EditorDocument(std::unique_ptr<Bitmap> base_image) : base_(std::move(base_image))
{
    if (!base_ || base_->width() <= 0 || base_->height() <= 0 || !base_->pixels())
        throw std::runtime_error("The screenshot image is empty.");
    state_.crop = {0, 0, base_->width(), base_->height()};
    state_.next_step = 1;
}

const Bitmap& EditorDocument::base() const noexcept
{
    return *base_;
}

int EditorDocument::base_width() const noexcept
{
    return base_->width();
}

int EditorDocument::base_height() const noexcept
{
    return base_->height();
}

RECT EditorDocument::crop_rect() const noexcept
{
    return state_.crop;
}

bool EditorDocument::is_cropped() const noexcept
{
    return state_.crop.left != 0 || state_.crop.top != 0 || state_.crop.right != base_->width() ||
           state_.crop.bottom != base_->height();
}

int EditorDocument::width() const noexcept
{
    return static_cast<int>(state_.crop.right - state_.crop.left);
}

int EditorDocument::height() const noexcept
{
    return static_cast<int>(state_.crop.bottom - state_.crop.top);
}

const std::vector<Annotation>& EditorDocument::annotations() const noexcept
{
    return state_.annotations;
}

const Annotation* EditorDocument::find_annotation(std::uint64_t id) const noexcept
{
    for (const auto& item : state_.annotations)
        if (item.id == id)
            return &item;
    return nullptr;
}

Annotation* EditorDocument::find_annotation_mutable(std::uint64_t id) noexcept
{
    for (auto& item : state_.annotations)
        if (item.id == id)
            return &item;
    return nullptr;
}

std::uint64_t EditorDocument::hit_test(PointF image_point, float tolerance, IDWriteFactory* dwrite) const
{
    for (auto it = state_.annotations.rbegin(); it != state_.annotations.rend(); ++it)
    {
        if (hit_test_annotation(*it, image_point, tolerance, dwrite))
            return it->id;
    }
    return 0;
}

int EditorDocument::next_step_number() const noexcept
{
    return state_.next_step;
}

void EditorDocument::reset_step_number() noexcept
{
    state_.next_step = 1;
}

void EditorDocument::push_undo_state(State previous)
{
    if (undo_stack_.size() >= MaxUndoSteps)
        undo_stack_.erase(undo_stack_.begin());
    undo_stack_.push_back(std::move(previous));
    redo_stack_.clear();
}

std::uint64_t EditorDocument::add_annotation(Annotation item)
{
    push_undo_state(state_);
    item.id = next_id_++;
    item.revision = 1;
    if (item.kind == AnnotationKind::Step)
    {
        if (item.step_number <= 0)
            item.step_number = state_.next_step;
        state_.next_step = std::max(state_.next_step + 1, item.step_number + 1);
    }
    state_.annotations.push_back(std::move(item));
    mark_dirty();
    return state_.annotations.back().id;
}

bool EditorDocument::delete_annotation(std::uint64_t id)
{
    return delete_annotations({id});
}

bool EditorDocument::delete_annotations(const std::vector<std::uint64_t>& ids)
{
    if (ids.empty())
        return false;
    State next = state_;
    const auto removed = std::erase_if(next.annotations,
                                       [&](const Annotation& item)
                                       {
                                           return std::find(ids.begin(), ids.end(), item.id) != ids.end();
                                       });
    if (removed == 0)
        return false;
    push_undo_state(std::move(state_));
    state_ = std::move(next);
    mark_dirty();
    return true;
}

bool EditorDocument::clear_annotations()
{
    if (state_.annotations.empty())
        return false;
    push_undo_state(state_);
    state_.annotations.clear();
    state_.next_step = 1;
    mark_dirty();
    return true;
}

bool EditorDocument::reset_all()
{
    if (state_.annotations.empty() && !is_cropped())
        return false;
    push_undo_state(state_);
    state_.annotations.clear();
    state_.crop = {0, 0, base_->width(), base_->height()};
    state_.next_step = 1;
    mark_dirty();
    return true;
}

bool EditorDocument::set_crop_rect(RECT rect)
{
    const RECT clamped = clamp_crop_rect(rect, base_->width(), base_->height());
    if (EqualRect(&clamped, &state_.crop))
        return false;
    push_undo_state(state_);
    state_.crop = clamped;
    mark_dirty();
    return true;
}

bool EditorDocument::reset_crop()
{
    return set_crop_rect({0, 0, base_->width(), base_->height()});
}

bool EditorDocument::update_annotation_style(std::uint64_t id, COLORREF color, SizePreset size, bool filled)
{
    auto* existing = find_annotation_mutable(id);
    if (!existing)
        return false;
    if (existing->color == color && existing->size == size && existing->filled == filled)
        return false;
    push_undo_state(state_);
    auto* target = find_annotation_mutable(id);
    target->color = color;
    target->size = size;
    target->filled = filled;
    ++target->revision;
    mark_dirty();
    return true;
}

bool EditorDocument::update_annotation_text(std::uint64_t id, std::wstring text)
{
    auto* existing = find_annotation_mutable(id);
    if (!existing || existing->kind != AnnotationKind::Text)
        return false;
    if (existing->text == text)
        return false;
    push_undo_state(state_);
    auto* target = find_annotation_mutable(id);
    target->text = std::move(text);
    ++target->revision;
    mark_dirty();
    return true;
}

void EditorDocument::begin_mutation()
{
    if (mutating_)
        return;
    mutation_snapshot_ = state_;
    mutating_ = true;
}

void EditorDocument::commit_mutation()
{
    if (!mutating_)
        return;
    mutating_ = false;
    const bool changed = !EqualRect(&mutation_snapshot_.crop, &state_.crop) ||
                         mutation_snapshot_.next_step != state_.next_step ||
                         !state_equals(mutation_snapshot_.annotations, state_.annotations);
    if (changed)
    {
        push_undo_state(std::move(mutation_snapshot_));
        mark_dirty();
    }
    mutation_snapshot_ = {};
}

void EditorDocument::cancel_mutation()
{
    if (!mutating_)
        return;
    mutating_ = false;
    state_ = std::move(mutation_snapshot_);
    mutation_snapshot_ = {};
    mark_dirty();
}

void EditorDocument::mark_dirty() noexcept
{
    ++revision_;
}

bool EditorDocument::can_undo() const noexcept
{
    return !undo_stack_.empty();
}

bool EditorDocument::can_redo() const noexcept
{
    return !redo_stack_.empty();
}

bool EditorDocument::undo()
{
    if (undo_stack_.empty())
        return false;
    redo_stack_.push_back(std::move(state_));
    state_ = std::move(undo_stack_.back());
    undo_stack_.pop_back();
    mark_dirty();
    return true;
}

bool EditorDocument::redo()
{
    if (redo_stack_.empty())
        return false;
    undo_stack_.push_back(std::move(state_));
    state_ = std::move(redo_stack_.back());
    redo_stack_.pop_back();
    mark_dirty();
    return true;
}

std::uint64_t EditorDocument::revision() const noexcept
{
    return revision_;
}

const Bitmap& EditorDocument::render_output() const
{
    if (state_.annotations.empty() && !is_cropped())
        return *base_;
    if (cached_output_ && cached_revision_ == revision_)
        return *cached_output_;

    auto result = crop_bitmap(*base_, state_.crop);
    if (state_.annotations.empty())
    {
        cached_output_ = std::move(result);
        cached_revision_ = revision_;
        return *cached_output_;
    }

    ComApartment apartment;
    ComPtr<IWICImagingFactory> wic;
    check_hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)),
             "Cannot start the image renderer.");
    ComPtr<ID2D1Factory> d2d;
    check_hr(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()),
             "Cannot start the vector renderer.");
    ComPtr<IDWriteFactory> dwrite;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()));

    const UINT out_w = static_cast<UINT>(result->width());
    const UINT out_h = static_cast<UINT>(result->height());
    const UINT stride = out_w * 4u;
    const UINT bytes = stride * out_h;

    ComPtr<IWICBitmap> wic_canvas;
    check_hr(wic->CreateBitmapFromMemory(out_w, out_h, GUID_WICPixelFormat32bppPBGRA, stride, bytes,
                                         reinterpret_cast<BYTE*>(result->pixels()), &wic_canvas),
             "Cannot create the offscreen render buffer.");

    const auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    ComPtr<ID2D1RenderTarget> target;
    check_hr(d2d->CreateWicBitmapRenderTarget(wic_canvas.Get(), props, &target),
             "Cannot create the vector render target.");

    bool drawing = false;
    const auto begin_batch = [&]()
    {
        if (!drawing)
        {
            target->BeginDraw();
            target->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(-state_.crop.left),
                                                               static_cast<float>(-state_.crop.top)));
            drawing = true;
        }
    };
    const auto flush_batch = [&]()
    {
        if (drawing)
        {
            drawing = false;
            check_hr(target->EndDraw(), "Cannot finish rendering markup.");
        }
    };

    const POINT crop_origin{state_.crop.left, state_.crop.top};
    for (const auto& item : state_.annotations)
    {
        if (item.kind == AnnotationKind::Redact && !item.filled)
        {
            flush_batch();
            WICRect lock_rect{0, 0, static_cast<INT>(out_w), static_cast<INT>(out_h)};
            ComPtr<IWICBitmapLock> lock;
            check_hr(wic_canvas->Lock(&lock_rect, WICBitmapLockRead | WICBitmapLockWrite, &lock),
                     "Cannot access render pixels for pixelation.");
            UINT lock_stride{};
            UINT lock_size{};
            BYTE* lock_data{};
            check_hr(lock->GetStride(&lock_stride), "Cannot read render buffer stride.");
            check_hr(lock->GetDataPointer(&lock_size, &lock_data), "Cannot read render buffer pointer.");
            const RectF r = normalize_rect(item.start, item.end);
            const RECT shifted{
                static_cast<LONG>(std::floor(r.left)) - crop_origin.x,
                static_cast<LONG>(std::floor(r.top)) - crop_origin.y,
                static_cast<LONG>(std::ceil(r.right)) - crop_origin.x,
                static_cast<LONG>(std::ceil(r.bottom)) - crop_origin.y,
            };
            pixelate_buffer_region(reinterpret_cast<std::uint32_t*>(lock_data),
                                   static_cast<int>(lock_stride / 4u), static_cast<int>(out_w),
                                   static_cast<int>(out_h), shifted, pixelate_block_for(item.size));
        }
        else if (item.kind == AnnotationKind::Highlighter)
        {
            flush_batch();
            WICRect lock_rect{0, 0, static_cast<INT>(out_w), static_cast<INT>(out_h)};
            ComPtr<IWICBitmapLock> lock;
            check_hr(wic_canvas->Lock(&lock_rect, WICBitmapLockRead | WICBitmapLockWrite, &lock),
                     "Cannot access render pixels for highlighting.");
            UINT lock_stride{};
            UINT lock_size{};
            BYTE* lock_data{};
            check_hr(lock->GetStride(&lock_stride), "Cannot read render buffer stride.");
            check_hr(lock->GetDataPointer(&lock_size, &lock_data), "Cannot read render buffer pointer.");
            apply_highlighter_to_buffer(reinterpret_cast<std::uint32_t*>(lock_data),
                                        static_cast<int>(lock_stride / 4u), static_cast<int>(out_w),
                                        static_cast<int>(out_h), item, crop_origin, d2d.Get(), wic.Get());
        }
        else
        {
            begin_batch();
            draw_vector_annotation(target.Get(), d2d.Get(), dwrite.Get(), item, false);
        }
    }
    flush_batch();

    check_hr(wic_canvas->CopyPixels(nullptr, stride, bytes, reinterpret_cast<BYTE*>(result->pixels())),
             "Cannot copy the rendered screenshot.");
    const size_t pixel_count = static_cast<size_t>(out_w) * static_cast<size_t>(out_h);
    for (size_t i = 0; i < pixel_count; ++i)
        result->pixels()[i] = 0xff000000u | (result->pixels()[i] & 0x00ffffffu);

    cached_output_ = std::move(result);
    cached_revision_ = revision_;
    return *cached_output_;
}

GlobalMemory EditorDocument::make_clipboard_dib() const
{
    const auto& output = render_output();
    return pluck::make_clipboard_dib(output, {0, 0, output.width(), output.height()});
}

void EditorDocument::save_png(const fs::path& path) const
{
    pluck::save_png(render_output(), path);
}

void save_png(const Bitmap& source, const fs::path& path)
{
    if (source.width() <= 0 || source.height() <= 0 || !source.pixels())
        throw std::runtime_error("The screenshot image is empty.");
    if (path.empty())
        throw std::runtime_error("The file path is empty.");
    const auto width = static_cast<UINT>(source.width());
    const auto height = static_cast<UINT>(source.height());
    const auto stride = static_cast<uint64_t>(width) * 4u;
    const auto bytes = stride * height;
    if (stride > std::numeric_limits<UINT>::max() || bytes > std::numeric_limits<UINT>::max())
        throw std::runtime_error("The screenshot is too large to save.");

    std::error_code status_error;
    const bool existed = fs::exists(path, status_error);
    if (path.has_parent_path())
    {
        std::error_code dir_error;
        fs::create_directories(path.parent_path(), dir_error);
        if (dir_error)
            throw std::runtime_error("Cannot create the destination folder.");
    }

    ComApartment apartment;
    bool committed{};
    try
    {
        ComPtr<IWICImagingFactory> factory;
        check_hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory)),
                 "Cannot start the PNG encoder.");
        ComPtr<IWICBitmap> wic_source;
        // Treat the GDI 32-bit DIB section as opaque BGRX so unused alpha bytes never produce transparent PNGs.
        check_hr(factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGR,
                                                 static_cast<UINT>(stride), static_cast<UINT>(bytes),
                                                 reinterpret_cast<BYTE*>(source.pixels()), &wic_source),
                 "Cannot read the screenshot pixels.");
        ComPtr<IWICStream> stream;
        check_hr(factory->CreateStream(&stream), "Cannot prepare the output file.");
        check_hr(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE),
                 "Cannot open the file for writing.");
        ComPtr<IWICBitmapEncoder> encoder;
        check_hr(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder),
                 "Cannot create the PNG encoder.");
        check_hr(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache),
                 "Cannot initialize the PNG file.");
        ComPtr<IWICBitmapFrameEncode> frame;
        check_hr(encoder->CreateNewFrame(&frame, nullptr), "Cannot create the PNG frame.");
        check_hr(frame->Initialize(nullptr), "Cannot initialize the PNG frame.");
        check_hr(frame->SetSize(width, height), "Cannot set the PNG image size.");
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        check_hr(frame->SetPixelFormat(&format), "Cannot set the PNG pixel format.");
        check_hr(frame->WriteSource(wic_source.Get(), nullptr), "Cannot write the PNG image.");
        check_hr(frame->Commit(), "Cannot finalize the PNG image.");
        check_hr(encoder->Commit(), "Cannot save the PNG file.");
        committed = true;
    }
    catch (...)
    {
        if (!committed && !existed)
        {
            std::error_code ignored;
            fs::remove(path, ignored);
        }
        throw;
    }
}

std::wstring default_save_filename(const SYSTEMTIME& local_time)
{
    wchar_t name[64]{};
    std::swprintf(name, std::size(name), L"Pluck-%04u-%02u-%02u-%02u%02u%02u.png", local_time.wYear,
                  local_time.wMonth, local_time.wDay, local_time.wHour, local_time.wMinute,
                  local_time.wSecond);
    return name;
}

fs::path default_save_folder()
{
    PWSTR buffer{};
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &buffer)) && buffer)
    {
        fs::path folder(buffer);
        CoTaskMemFree(buffer);
        return folder;
    }
    return {};
}
} // namespace pluck
