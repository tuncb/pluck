#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

namespace pluck
{
enum class ToolbarIcon
{
    Select,
    Pen,
    Highlighter,
    Arrow,
    Rectangle,
    Oval,
    Line,
    Text,
    Step,
    Redact,
    Crop,
    Undo,
    Redo,
    Reset,
    StrokeThin,
    StrokeMedium,
    StrokeThick,
    StrokeExtraThick,
    ZoomOut,
    ZoomIn
};

inline void draw_toolbar_icon(ID2D1RenderTarget* target, ID2D1Factory* factory, IDWriteFactory* dwrite,
                              ToolbarIcon icon, const D2D1_RECT_F& bounds, ID2D1Brush* stroke_brush,
                              ID2D1Brush* accent_brush, ID2D1StrokeStyle* round_style, float dpi_scale)
{
    if (!target || !factory || !stroke_brush)
        return;

    const float cx = (bounds.left + bounds.right) * 0.5f;
    const float cy = (bounds.top + bounds.bottom) * 0.5f;
    const float s = (bounds.right - bounds.left) * 0.5f;
    const float u = s / 10.0f;
    const float w = 1.6f * dpi_scale;
    const auto pt = [cx, cy, u](float x, float y) noexcept { return D2D1::Point2F(cx + x * u, cy + y * u); };

    const auto draw_poly = [&](const D2D1_POINT_2F* pts, size_t count, bool closed, bool fill,
                               ID2D1Brush* brush, float thickness)
    {
        Microsoft::WRL::ComPtr<ID2D1PathGeometry> geom;
        if (FAILED(factory->CreatePathGeometry(&geom)))
            return;
        Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(geom->Open(&sink)))
            return;
        sink->BeginFigure(pts[0], fill ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
        for (size_t i = 1; i < count; ++i)
            sink->AddLine(pts[i]);
        sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
        if (FAILED(sink->Close()))
            return;
        if (fill)
            target->FillGeometry(geom.Get(), brush);
        else
            target->DrawGeometry(geom.Get(), brush, thickness, round_style);
    };

    switch (icon)
    {
    case ToolbarIcon::Select:
    {
        const D2D1_POINT_2F pts[] = {pt(-4.5f, -6.5f), pt(-4.5f, 5.5f), pt(-1.2f, 2.3f),
                                     pt(1.5f, 7.0f),   pt(3.5f, 6.0f),  pt(0.9f, 1.4f),
                                     pt(5.2f, 1.4f)};
        draw_poly(pts, 7, true, false, stroke_brush, w);
        break;
    }
    case ToolbarIcon::Pen:
    {
        const D2D1_POINT_2F body[] = {pt(-5.8f, 5.8f), pt(-4.4f, 1.4f), pt(3.2f, -6.2f),
                                      pt(6.2f, -3.2f), pt(-1.4f, 4.4f)};
        draw_poly(body, 5, true, false, stroke_brush, w);
        target->DrawLine(pt(1.4f, -4.4f), pt(4.4f, -1.4f), stroke_brush, w, round_style);
        if (accent_brush)
            target->FillRectangle(D2D1::RectF(cx - 6.0f * u, cy + 7.2f * u, cx + 6.0f * u, cy + 9.4f * u),
                                  accent_brush);
        break;
    }
    case ToolbarIcon::Highlighter:
    {
        const D2D1_POINT_2F body[] = {pt(-2.5f, 4.5f), pt(-5.5f, 1.5f), pt(2.0f, -6.0f),
                                      pt(5.8f, -2.2f)};
        draw_poly(body, 4, true, false, stroke_brush, w);
        const D2D1_POINT_2F tip[] = {pt(-5.5f, 1.5f), pt(-6.5f, 5.5f), pt(-2.5f, 4.5f)};
        draw_poly(tip, 3, true, true, accent_brush ? accent_brush : stroke_brush, w);
        if (accent_brush)
            target->FillRectangle(D2D1::RectF(cx - 6.0f * u, cy + 7.2f * u, cx + 6.0f * u, cy + 9.4f * u),
                                  accent_brush);
        break;
    }
    case ToolbarIcon::Arrow:
    {
        target->DrawLine(pt(-5.5f, 5.5f), pt(5.0f, -5.0f), stroke_brush, w * 1.05f, round_style);
        const D2D1_POINT_2F head[] = {pt(-1.0f, -5.5f), pt(5.5f, -5.5f), pt(5.5f, 1.0f)};
        draw_poly(head, 3, false, false, stroke_brush, w * 1.05f);
        break;
    }
    case ToolbarIcon::Rectangle:
    {
        target->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(cx - 6.0f * u, cy - 4.8f * u, cx + 6.0f * u, cy + 4.8f * u),
                              1.4f * u, 1.4f * u),
            stroke_brush, w);
        break;
    }
    case ToolbarIcon::Oval:
    {
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 6.0f * u, 5.0f * u), stroke_brush, w);
        break;
    }
    case ToolbarIcon::Line:
    {
        target->DrawLine(pt(-5.6f, 5.6f), pt(5.6f, -5.6f), stroke_brush, w * 1.1f, round_style);
        break;
    }
    case ToolbarIcon::Text:
    {
        target->DrawLine(pt(-5.5f, -5.5f), pt(5.5f, -5.5f), stroke_brush, w * 1.15f, round_style);
        target->DrawLine(pt(0.0f, -5.5f), pt(0.0f, 6.0f), stroke_brush, w * 1.15f, round_style);
        target->DrawLine(pt(-2.8f, 6.0f), pt(2.8f, 6.0f), stroke_brush, w, round_style);
        break;
    }
    case ToolbarIcon::Step:
    {
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 6.4f * u, 6.4f * u), stroke_brush, w);
        if (dwrite)
        {
            Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
            if (SUCCEEDED(dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
                                                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                   9.5f * u, L"", &fmt)))
            {
                fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                target->DrawTextW(L"1", 1, fmt.Get(), bounds, stroke_brush);
            }
        }
        break;
    }
    case ToolbarIcon::Redact:
    {
        const float cell = 3.6f * u;
        const float x0 = cx - 5.4f * u;
        const float y0 = cy - 5.4f * u;
        for (int row = 0; row < 3; ++row)
        {
            for (int col = 0; col < 3; ++col)
            {
                if (((row + col) & 1) == 0)
                {
                    target->FillRectangle(
                        D2D1::RectF(x0 + col * cell, y0 + row * cell, x0 + (col + 1) * cell,
                                    y0 + (row + 1) * cell),
                        stroke_brush);
                }
            }
        }
        target->DrawRectangle(D2D1::RectF(x0, y0, x0 + 3.0f * cell, y0 + 3.0f * cell), stroke_brush,
                              w * 0.85f);
        break;
    }
    case ToolbarIcon::Crop:
    {
        const D2D1_POINT_2F tl[] = {pt(-6.2f, -3.5f), pt(3.5f, -3.5f), pt(3.5f, 6.2f)};
        const D2D1_POINT_2F br[] = {pt(-3.5f, -6.2f), pt(-3.5f, 3.5f), pt(6.2f, 3.5f)};
        draw_poly(tl, 3, false, false, stroke_brush, w);
        draw_poly(br, 3, false, false, stroke_brush, w);
        break;
    }
    case ToolbarIcon::Undo:
    {
        const D2D1_POINT_2F shaft[] = {pt(-5.5f, -1.5f), pt(2.2f, -1.5f), pt(5.0f, 0.3f), pt(5.2f, 3.4f),
                                       pt(2.6f, 5.2f),   pt(-2.5f, 5.2f)};
        draw_poly(shaft, 6, false, false, stroke_brush, w);
        const D2D1_POINT_2F head[] = {pt(-2.0f, -5.0f), pt(-5.8f, -1.5f), pt(-2.0f, 2.0f)};
        draw_poly(head, 3, false, false, stroke_brush, w);
        break;
    }
    case ToolbarIcon::Redo:
    {
        const D2D1_POINT_2F shaft[] = {pt(5.5f, -1.5f), pt(-2.2f, -1.5f), pt(-5.0f, 0.3f), pt(-5.2f, 3.4f),
                                       pt(-2.6f, 5.2f), pt(2.5f, 5.2f)};
        draw_poly(shaft, 6, false, false, stroke_brush, w);
        const D2D1_POINT_2F head[] = {pt(2.0f, -5.0f), pt(5.8f, -1.5f), pt(2.0f, 2.0f)};
        draw_poly(head, 3, false, false, stroke_brush, w);
        break;
    }
    case ToolbarIcon::Reset:
    {
        const float r = 5.6f * u;
        Microsoft::WRL::ComPtr<ID2D1PathGeometry> arc;
        if (SUCCEEDED(factory->CreatePathGeometry(&arc)))
        {
            Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(arc->Open(&sink)))
            {
                sink->BeginFigure(pt(-4.3f, 3.6f), D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddArc(D2D1::ArcSegment(pt(-4.3f, -3.6f), D2D1::SizeF(r, r), 0.0f,
                                              D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                if (SUCCEEDED(sink->Close()))
                    target->DrawGeometry(arc.Get(), stroke_brush, w, round_style);
            }
        }
        const D2D1_POINT_2F head[] = {pt(-4.8f, -6.6f), pt(-4.4f, -3.3f), pt(-1.1f, -3.1f)};
        draw_poly(head, 3, false, false, stroke_brush, w);
        break;
    }
    case ToolbarIcon::StrokeThin:
    case ToolbarIcon::StrokeMedium:
    case ToolbarIcon::StrokeThick:
    case ToolbarIcon::StrokeExtraThick:
    {
        float thickness = 1.6f * dpi_scale;
        if (icon == ToolbarIcon::StrokeMedium)
            thickness = 3.0f * dpi_scale;
        else if (icon == ToolbarIcon::StrokeThick)
            thickness = 5.0f * dpi_scale;
        else if (icon == ToolbarIcon::StrokeExtraThick)
            thickness = 7.5f * dpi_scale;
        const float half_w = 6.25f * dpi_scale;
        const float half_h = thickness * 0.5f;
        target->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(cx - half_w, cy - half_h, cx + half_w, cy + half_h), half_h,
                              half_h),
            stroke_brush);
        break;
    }
    case ToolbarIcon::ZoomOut:
    {
        target->DrawLine(pt(-4.8f, 0.0f), pt(4.8f, 0.0f), stroke_brush, w * 1.1f, round_style);
        break;
    }
    case ToolbarIcon::ZoomIn:
    {
        target->DrawLine(pt(-4.8f, 0.0f), pt(4.8f, 0.0f), stroke_brush, w * 1.1f, round_style);
        target->DrawLine(pt(0.0f, -4.8f), pt(0.0f, 4.8f), stroke_brush, w * 1.1f, round_style);
        break;
    }
    }
}
} // namespace pluck
