#include "capture.h"
#include "editor_core.h"
#include "selector.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
template <typename Action> void rejects(Action action)
{
    bool rejected{};
    try
    {
        action();
    }
    catch (const std::exception&)
    {
        rejected = true;
    }
    require(rejected, "Invalid image input was accepted.");
}
void check_rect(POINT a, POINT b, RECT bounds, RECT expected)
{
    const auto actual = pluck::selection(a, b, bounds);
    require(EqualRect(&actual, &expected), "Selection coordinates differ.");
}
void geometry()
{
    const RECT bounds{-1920, -1080, 2560, 1440};
    check_rect({-100, -50}, {100, 50}, bounds, {-100, -50, 100, 50});
    check_rect({100, 50}, {-100, -50}, bounds, {-100, -50, 100, 50});
    check_rect({100, -50}, {-100, 50}, bounds, {-100, -50, 100, 50});
    check_rect({-100, 50}, {100, -50}, bounds, {-100, -50, 100, 50});
    check_rect({-3000, -2000}, {5000, 3000}, bounds, bounds);
    check_rect({3, 4}, {4, 5}, bounds, {3, 4, 4, 5});
    require(!pluck::has_area(pluck::selection({1, 1}, {1, 10}, bounds)), "Zero width has area.");
    require(!pluck::has_area(pluck::selection({1, 1}, {10, 1}, bounds)), "Zero height has area.");

    // Window clamping to virtual desktop and optional monitor work-area bounds, plus Z-order hit-testing.
    const RECT desktop{-1920, -1080, 2560, 1440};
    const auto shifted = pluck::clamp_window_rect({-100, -50, 300, 250}, desktop);
    const RECT want_shifted{1820, 1030, 2220, 1330};
    require(EqualRect(&shifted, &want_shifted), "Window rect desktop offset differs.");

    const RECT work_area{0, 0, 2560, 1400};
    const auto maximized = pluck::clamp_window_rect({-8, -8, 2568, 1408}, desktop, &work_area);
    const RECT want_maximized{1920, 1080, 4480, 2480};
    require(EqualRect(&maximized, &want_maximized), "Maximized window work-area clamping differs.");

    const auto offscreen = pluck::clamp_window_rect({3000, 2000, 3400, 2300}, desktop);
    require(!pluck::has_area(offscreen), "Off-screen window produced a non-empty rect.");

    const RECT windows[3]{{100, 100, 300, 300}, {50, 50, 400, 400}, {500, 50, 700, 200}};
    const auto front = pluck::hit_test_rects(windows, 3, {150, 150});
    require(EqualRect(&front, &windows[0]), "Z-order hit-test did not pick the frontmost window.");
    const auto back = pluck::hit_test_rects(windows, 3, {60, 60});
    require(EqualRect(&back, &windows[1]), "Z-order hit-test did not fall through to the back window.");
    const auto edge_exclusive = pluck::hit_test_rects(windows, 3, {300, 300});
    require(EqualRect(&edge_exclusive, &windows[1]), "Right/bottom edges were not exclusive.");
    const auto miss = pluck::hit_test_rects(windows, 3, {450, 450});
    require(!pluck::has_area(miss), "Empty desktop hit returned a non-empty rect.");
}
void pixels()
{
    pluck::Bitmap image(5, 4);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 5; ++x)
            image.pixels()[y * 5 + x] =
                0xff000000u | (static_cast<unsigned>(y) << 16) | (static_cast<unsigned>(x) << 8) | 0x7f;
    for (int width = 1; width <= 5; ++width)
    {
        auto dib = pluck::make_clipboard_dib(image, {0, 1, width, 4});
        auto* header = static_cast<BITMAPINFOHEADER*>(GlobalLock(dib.get()));
        require(header != nullptr, "Cannot read the clipboard allocation.");
        const auto stride = (width * 3 + 3) & ~3;
        require(header->biWidth == width && header->biHeight == 3 && header->biBitCount == 24 &&
                    header->biCompression == BI_RGB && header->biSizeImage == static_cast<DWORD>(stride * 3),
                "Clipboard DIB header differs.");
        const auto* data = reinterpret_cast<const unsigned char*>(header + 1);
        for (int y = 0; y < 3; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                const auto* pixel = data + y * stride + x * 3;
                require(pixel[0] == 0x7f && pixel[1] == x && pixel[2] == 3 - y,
                        "Clipboard crop has incorrect colors or row order.");
            }
            for (int padding = width * 3; padding < stride; ++padding)
                require(data[y * stride + padding] == 0, "Clipboard row padding is not cleared.");
        }
        GlobalUnlock(dib.get());
    }
    auto offset = pluck::make_clipboard_dib(image, {2, 1, 3, 2});
    const auto* header = static_cast<BITMAPINFOHEADER*>(GlobalLock(offset.get()));
    const auto* pixel = reinterpret_cast<const unsigned char*>(header + 1);
    require(pixel[0] == 0x7f && pixel[1] == 2 && pixel[2] == 1, "Offset crop differs.");
    GlobalUnlock(offset.get());
    rejects([&] { pluck::make_clipboard_dib(image, {-1, 0, 1, 1}); });
    rejects([&] { pluck::make_clipboard_dib(image, {0, 0, 6, 4}); });
    rejects([&] { pluck::make_clipboard_dib(image, {1, 1, 1, 2}); });
    rejects([] { pluck::Bitmap invalid(0, 2); });
    rejects([] { pluck::Bitmap invalid(-1, 2); });
    rejects([] { pluck::Bitmap invalid(2147483647, 2147483647); });
    pluck::GlobalMemory empty;
    require(pluck::copy_to_clipboard(nullptr, empty) == pluck::CopyResult::Failed,
            "Invalid clipboard owner was accepted.");
}
void dimming()
{
    const auto expected_pixel = [](std::uint32_t pixel) noexcept -> std::uint32_t
    {
        const auto red = ((pixel >> 16) & 255u) * 135u / 255u;
        const auto green = ((pixel >> 8) & 255u) * 135u / 255u;
        const auto blue = (pixel & 255u) * 135u / 255u;
        return (red << 16) | (green << 8) | blue;
    };

    // Check every 8-bit channel value across aligned and unaligned start offsets and lengths.
    std::vector<std::uint32_t> source(260);
    std::vector<std::uint32_t> destination(260, 0xdeadbeefu);
    for (std::uint32_t i = 0; i < 256; ++i)
        source[i] = 0xff000000u | (i << 16) | ((255u - i) << 8) | ((i * 37u) & 255u);
    for (size_t offset = 0; offset < 4; ++offset)
    {
        for (size_t length : {size_t{1}, size_t{2}, size_t{3}, size_t{4}, size_t{5}, size_t{15},
                              size_t{256} - offset})
        {
            std::fill(destination.begin(), destination.end(), 0xdeadbeefu);
            pluck::dim_pixels(source.data() + offset, destination.data() + offset, length);
            for (size_t i = 0; i < length; ++i)
            {
                require(destination[offset + i] == expected_pixel(source[offset + i]),
                        "SIMD dimmed pixel differs from exact integer dimming.");
            }
            if (offset > 0)
                require(destination[offset - 1] == 0xdeadbeefu, "dim_pixels wrote before range.");
            require(destination[offset + length] == 0xdeadbeefu, "dim_pixels wrote past range.");
        }
    }

    // Check multi-region dimming leaves dead-void regions untouched and matches exact pixels.
    pluck::Bitmap full(1024, 600);
    pluck::Bitmap dimmed(1024, 600);
    pluck::prefault_bitmap(full);
    pluck::prefault_bitmap(dimmed);
    for (int y = 0; y < 600; ++y)
        for (int x = 0; x < 1024; ++x)
            full.pixels()[static_cast<size_t>(y) * 1024 + x] =
                0xff000000u | (static_cast<std::uint32_t>(y & 255) << 16) |
                (static_cast<std::uint32_t>(x & 255) << 8) | static_cast<std::uint32_t>((x + y) & 255);
    const RECT regions[2]{{0, 0, 600, 600}, {600, 123, 1024, 590}};
    pluck::dim_bitmap(full, dimmed, regions, 2);
    for (int y = 0; y < 600; ++y)
        for (int x = 0; x < 1024; ++x)
        {
            const bool inside = (x < 600) || (x >= 600 && y >= 123 && y < 590);
            const auto actual = dimmed.pixels()[static_cast<size_t>(y) * 1024 + x];
            const auto want = inside ? expected_pixel(full.pixels()[static_cast<size_t>(y) * 1024 + x]) : 0u;
            require(actual == want, "Multi-region dim_bitmap pixel differs.");
        }

    // Check full-bitmap parallel dimming path (>= 512K pixels).
    pluck::dim_bitmap(full, dimmed);
    for (size_t i = 0; i < static_cast<size_t>(1024) * 600; i += 97)
    {
        require(dimmed.pixels()[i] == expected_pixel(full.pixels()[i]),
                "Full parallel dim_bitmap pixel differs.");
    }
}
void editor_engine()
{
    pluck::Bitmap source(6, 5);
    for (int y = 0; y < 5; ++y)
        for (int x = 0; x < 6; ++x)
            // Leave the top byte zero to mimic GDI BitBlt output.
            source.pixels()[y * 6 + x] =
                (static_cast<unsigned>(y * 40) << 16) | (static_cast<unsigned>(x * 30) << 8) | 0x5au;

    auto cropped = pluck::crop_bitmap(source, {1, 2, 5, 5});
    require(cropped && cropped->width() == 4 && cropped->height() == 3, "Cropped bitmap dimensions differ.");
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 4; ++x)
        {
            const auto expected =
                0xff000000u | (static_cast<unsigned>((y + 2) * 40) << 16) |
                (static_cast<unsigned>((x + 1) * 30) << 8) | 0x5au;
            require(cropped->pixels()[y * 4 + x] == expected,
                    "Cropped bitmap pixels or opaque alpha normalization differ.");
        }

    rejects([&] { pluck::crop_bitmap(source, {-1, 0, 2, 2}); });
    rejects([&] { pluck::crop_bitmap(source, {0, 0, 7, 2}); });
    rejects([&] { pluck::crop_bitmap(source, {2, 2, 2, 4}); });
    rejects([] { pluck::EditorDocument invalid(nullptr); });

    SYSTEMTIME stamp{};
    stamp.wYear = 2026;
    stamp.wMonth = 11;
    stamp.wDay = 20;
    stamp.wHour = 9;
    stamp.wMinute = 8;
    stamp.wSecond = 7;
    require(pluck::default_save_filename(stamp) == L"Pluck-2026-11-20-090807.png",
            "Default PNG filename differs.");

    pluck::EditorDocument document(std::move(cropped));
    require(document.width() == 4 && document.height() == 3, "Document dimensions differ.");
    auto dib = document.make_clipboard_dib();
    auto* header = static_cast<BITMAPINFOHEADER*>(GlobalLock(dib.get()));
    require(header && header->biWidth == 4 && header->biHeight == 3 && header->biBitCount == 24,
            "Document clipboard DIB header differs.");
    GlobalUnlock(dib.get());

    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
                      (L"pluck-core-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                       std::to_wstring(GetTickCount64()));
    const auto png_path = root / L"nested" / L"capture.png";
    try
    {
        document.save_png(png_path);
        require(fs::exists(png_path) && fs::file_size(png_path) > 0, "PNG file was not created.");

        const auto hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        require(SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE, "Cannot initialize WIC for test decode.");
        struct Apartment
        {
            bool active{};
            ~Apartment()
            {
                if (active)
                    CoUninitialize();
            }
        } apartment{SUCCEEDED(hr)};

        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                           IID_PPV_ARGS(&factory))),
                "Cannot create WIC factory.");
        Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
        require(SUCCEEDED(factory->CreateDecoderFromFilename(png_path.c_str(), nullptr, GENERIC_READ,
                                                             WICDecodeMetadataCacheOnLoad, &decoder)),
                "Cannot decode saved PNG.");
        Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
        require(SUCCEEDED(decoder->GetFrame(0, &frame)), "Cannot read PNG frame.");
        Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
        require(SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
                    SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                                                    WICBitmapDitherTypeNone, nullptr, 0.0,
                                                    WICBitmapPaletteTypeCustom)),
                "Cannot convert decoded PNG.");
        UINT w{}, h{};
        require(SUCCEEDED(converter->GetSize(&w, &h)) && w == 4 && h == 3, "Decoded PNG size differs.");
        std::vector<std::uint32_t> decoded(static_cast<size_t>(w) * h);
        require(SUCCEEDED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(decoded.size() * 4),
                                                reinterpret_cast<BYTE*>(decoded.data()))),
                "Cannot read decoded PNG pixels.");
        for (size_t i = 0; i < decoded.size(); ++i)
            require(decoded[i] == document.render_output().pixels()[i],
                    "Decoded PNG pixels or opacity differ from source.");

        rejects([&] { document.save_png(L""); });
    }
    catch (...)
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
        throw;
    }
    std::error_code ignored;
    fs::remove_all(root, ignored);
}
void editor_markup()
{
    // Geometry constraints
    {
        const auto snapped = pluck::constrain_angle_45({10.0f, 10.0f}, {110.0f, 14.0f});
        require(std::abs(snapped.y - 10.0f) < 0.01f && snapped.x > 100.0f,
                "45-degree angle snap failed for horizontal stroke.");
        const auto sq = pluck::constrain_square({10.0f, 20.0f}, {60.0f, 35.0f});
        require(std::abs((sq.x - 10.0f) - (sq.y - 20.0f)) < 0.01f, "Square constraint failed.");
    }

    // Light background with a solid black horizontal bar to test text-safe multiply highlighting
    auto make_light_canvas = []()
    {
        auto bmp = std::make_unique<pluck::Bitmap>(160, 120);
        for (int y = 0; y < 120; ++y)
            for (int x = 0; x < 160; ++x)
                bmp->pixels()[y * 160 + x] =
                    (y >= 48 && y < 52 && x >= 20 && x < 140) ? 0xff000000u : 0xffffffffu;
        return bmp;
    };

    {
        pluck::EditorDocument doc(make_light_canvas());
        pluck::Annotation hl{};
        hl.kind = pluck::AnnotationKind::Highlighter;
        hl.color = pluck::DefaultHighlighterColor;
        hl.size = pluck::SizePreset::Medium;
        hl.points = {{20.0f, 50.0f}, {140.0f, 50.0f}};
        const auto hl_id = doc.add_annotation(hl);
        require(hl_id != 0, "Highlighter annotation id was zero.");
        const auto& out = doc.render_output();
        const auto white_bg_px = out.pixels()[44 * 160 + 80];
        const auto black_bar_px = out.pixels()[50 * 160 + 80];
        const unsigned bg_r = (white_bg_px >> 16) & 255u;
        const unsigned bg_b = white_bg_px & 255u;
        require(bg_r > 230u && bg_b < 140u, "Yellow highlighter did not tint light background.");
        const unsigned bar_r = (black_bar_px >> 16) & 255u;
        const unsigned bar_g = (black_bar_px >> 8) & 255u;
        const unsigned bar_b = black_bar_px & 255u;
        require(bar_r == 0u && bar_g == 0u && bar_b == 0u,
                "Highlighter washed out black text pixels on a light background.");
    }

    // Dark background highlighter should visibly brighten/tint dark UI pixels
    {
        auto dark = std::make_unique<pluck::Bitmap>(120, 80);
        for (int i = 0; i < 120 * 80; ++i)
            dark->pixels()[i] = 0xff141414u;
        pluck::EditorDocument doc(std::move(dark));
        pluck::Annotation hl{};
        hl.kind = pluck::AnnotationKind::Highlighter;
        hl.color = pluck::DefaultHighlighterColor;
        hl.size = pluck::SizePreset::Medium;
        hl.points = {{20.0f, 40.0f}, {100.0f, 40.0f}};
        doc.add_annotation(hl);
        const auto& out = doc.render_output();
        const auto px = out.pixels()[40 * 120 + 60];
        require(((px >> 16) & 255u) > 80u, "Highlighter was invisible on dark background.");
    }

    // Pixelate redaction averages checkerboard blocks and leaves exterior untouched
    {
        auto checker = std::make_unique<pluck::Bitmap>(120, 120);
        for (int y = 0; y < 120; ++y)
            for (int x = 0; x < 120; ++x)
                checker->pixels()[y * 120 + x] = ((x + y) & 1) ? 0xffffffffu : 0xff000000u;
        pluck::EditorDocument doc(std::move(checker));
        pluck::Annotation redact{};
        redact.kind = pluck::AnnotationKind::Redact;
        redact.size = pluck::SizePreset::Small; // 8px block
        redact.filled = false;
        redact.start = {16.0f, 16.0f};
        redact.end = {64.0f, 64.0f};
        const auto id = doc.add_annotation(redact);
        const auto& out = doc.render_output();
        const auto inside0 = out.pixels()[16 * 120 + 16];
        const auto inside1 = out.pixels()[16 * 120 + 17];
        require(inside0 == inside1, "Pixelate block did not unify adjacent checkerboard pixels.");
        const unsigned ch = inside0 & 255u;
        require(ch >= 120u && ch <= 136u, "Pixelate average brightness differs from expected midpoint.");
        require(out.pixels()[2 * 120 + 2] == 0xff000000u && out.pixels()[2 * 120 + 3] == 0xffffffffu,
                "Pixelate modified pixels outside its rectangle.");

        // Switch redact to solid mode and verify undo/redo
        require(doc.update_annotation_style(id, RGB(16, 32, 48), pluck::SizePreset::Small, true),
                "Failed to switch redact to solid.");
        const auto& solid_out = doc.render_output();
        require((solid_out.pixels()[24 * 120 + 24] & 0x00ffffffu) == 0x00102030u,
                "Solid redact did not fill with the selected color.");
        require(doc.undo(), "Undo of redact style failed.");
        require(doc.render_output().pixels()[24 * 120 + 24] == inside0,
                "Undo did not restore pixelated output.");
        require(doc.redo(), "Redo of redact style failed.");
        require((doc.render_output().pixels()[24 * 120 + 24] & 0x00ffffffu) == 0x00102030u,
                "Redo did not restore solid redact output.");
    }

    // All vector markup types, hit-testing, move, resize, step counter, non-destructive crop, and PNG export
    {
        auto canvas = std::make_unique<pluck::Bitmap>(200, 160);
        for (int i = 0; i < 200 * 160; ++i)
            canvas->pixels()[i] = 0xffffffffu;
        pluck::EditorDocument doc(std::move(canvas));

        pluck::Annotation pen{};
        pen.kind = pluck::AnnotationKind::Pen;
        pen.color = RGB(239, 68, 68);
        pen.size = pluck::SizePreset::Large;
        pen.points = {{15.0f, 20.0f}, {55.0f, 20.0f}};
        const auto pen_id = doc.add_annotation(pen);

        pluck::Annotation arrow{};
        arrow.kind = pluck::AnnotationKind::Arrow;
        arrow.color = RGB(59, 130, 246);
        arrow.size = pluck::SizePreset::Medium;
        arrow.start = {20.0f, 50.0f};
        arrow.end = {120.0f, 50.0f};
        const auto arrow_id = doc.add_annotation(arrow);

        pluck::Annotation rect{};
        rect.kind = pluck::AnnotationKind::Rectangle;
        rect.color = RGB(16, 185, 129);
        rect.size = pluck::SizePreset::Medium;
        rect.filled = true;
        rect.start = {30.0f, 70.0f};
        rect.end = {90.0f, 110.0f};
        const auto rect_id = doc.add_annotation(rect);

        pluck::Annotation oval{};
        oval.kind = pluck::AnnotationKind::Oval;
        oval.color = RGB(168, 85, 247);
        oval.size = pluck::SizePreset::Medium;
        oval.filled = false;
        oval.start = {110.0f, 70.0f};
        oval.end = {170.0f, 110.0f};
        const auto oval_id = doc.add_annotation(oval);

        pluck::Annotation line{};
        line.kind = pluck::AnnotationKind::Line;
        line.color = RGB(24, 24, 27);
        line.size = pluck::SizePreset::Medium;
        line.start = {20.0f, 130.0f};
        line.end = {100.0f, 130.0f};
        const auto line_id = doc.add_annotation(line);

        pluck::Annotation text{};
        text.kind = pluck::AnnotationKind::Text;
        text.color = RGB(239, 68, 68);
        text.size = pluck::SizePreset::Medium;
        text.filled = true;
        text.start = {120.0f, 122.0f};
        text.text = L"Note";
        const auto text_id = doc.add_annotation(text);

        pluck::Annotation step1{};
        step1.kind = pluck::AnnotationKind::Step;
        step1.color = RGB(239, 68, 68);
        step1.size = pluck::SizePreset::Medium;
        step1.start = {160.0f, 30.0f};
        step1.step_number = 0; // auto-assign 1
        const auto step1_id = doc.add_annotation(step1);
        require(doc.find_annotation(step1_id)->step_number == 1 && doc.next_step_number() == 2,
                "Step counter did not auto-increment.");
        doc.reset_step_number();
        require(doc.next_step_number() == 1, "Step counter reset failed.");

        // Hit-testing & handle detection
        require(doc.hit_test({35.0f, 20.0f}, 4.0f) == pen_id, "Pen hit-test failed.");
        require(doc.hit_test({70.0f, 50.0f}, 4.0f) == arrow_id, "Arrow hit-test failed.");
        require(doc.hit_test({60.0f, 90.0f}, 4.0f) == rect_id, "Filled rectangle hit-test failed.");
        require(doc.hit_test({140.0f, 70.0f}, 5.0f) == oval_id, "Oval edge hit-test failed.");
        require(doc.hit_test({60.0f, 130.0f}, 4.0f) == line_id, "Line hit-test failed.");
        require(doc.hit_test({135.0f, 132.0f}, 4.0f) == text_id, "Text hit-test failed.");
        require(doc.hit_test({160.0f, 30.0f}, 4.0f) == step1_id, "Step badge hit-test failed.");
        require(pluck::hit_test_handles(*doc.find_annotation(arrow_id), {120.0f, 50.0f}, 8.0f) ==
                    pluck::HandleKind::End,
                "Arrow end handle hit-test failed.");

        // Move + commit_mutation + undo
        doc.begin_mutation();
        pluck::translate_annotation(*doc.find_annotation_mutable(step1_id), 10.0f, 5.0f);
        doc.commit_mutation();
        require(doc.find_annotation(step1_id)->start.x == 170.0f &&
                    doc.find_annotation(step1_id)->start.y == 35.0f,
                "Translate annotation failed.");
        require(doc.undo(), "Undo after translate failed.");
        require(doc.find_annotation(step1_id)->start.x == 160.0f, "Undo did not restore position.");

        // Render output verifies marks changed pixels
        const auto& full_out = doc.render_output();
        require(full_out.pixels()[20 * 200 + 35] != 0xffffffffu, "Pen stroke did not render.");
        require(full_out.pixels()[50 * 200 + 70] != 0xffffffffu, "Arrow did not render.");
        require(full_out.pixels()[90 * 200 + 60] != 0xffffffffu, "Filled rectangle did not render.");
        require(full_out.pixels()[130 * 200 + 60] != 0xffffffffu, "Line did not render.");
        require(full_out.pixels()[30 * 200 + 160] != 0xffffffffu, "Step badge did not render.");
        const auto text_bounds = pluck::annotation_bounds(*doc.find_annotation(text_id));
        const int pad_x = std::clamp(static_cast<int>(text.start.x - 5.0f), 0, 199);
        const int pad_y = std::clamp(static_cast<int>(text.start.y + 6.0f), 0, 159);
        const auto badge_px = full_out.pixels()[pad_y * 200 + pad_x];
        require(((badge_px >> 16) & 255u) > 200u && (badge_px & 255u) < 100u,
                "Text badge background did not render.");
        int white_glyph_pixels = 0;
        const int bx0 = std::clamp(static_cast<int>(text.start.x), 0, 199);
        const int by0 = std::clamp(static_cast<int>(text.start.y), 0, 159);
        const int bx1 = std::clamp(static_cast<int>(text_bounds.right - 10.0f), 0, 199);
        const int by1 = std::clamp(static_cast<int>(text_bounds.bottom - 5.0f), 0, 159);
        for (int y = by0; y < by1; ++y)
            for (int x = bx0; x < bx1; ++x)
            {
                const auto p = full_out.pixels()[y * 200 + x];
                if (((p >> 16) & 255u) > 220u && ((p >> 8) & 255u) > 200u && (p & 255u) > 200u)
                    ++white_glyph_pixels;
            }
        require(white_glyph_pixels > 10, "Text badge glyphs did not render.");

        // Non-destructive crop preserves annotations in cropped coordinates and undoes cleanly
        require(doc.set_crop_rect({20, 40, 140, 120}), "Set crop failed.");
        require(doc.is_cropped() && doc.width() == 120 && doc.height() == 80,
                "Cropped document dimensions differ.");
        const auto& cropped_out = doc.render_output();
        require(cropped_out.width() == 120 && cropped_out.height() == 80,
                "Cropped rendered bitmap size differs.");
        // Arrow was at y=50, x=70 in full image -> y=10, x=50 in cropped image
        require(cropped_out.pixels()[10 * 120 + 50] != 0xffffffffu,
                "Annotation did not align after non-destructive crop.");
        require(doc.undo(), "Undo crop failed.");
        require(!doc.is_cropped() && doc.width() == 200 && doc.height() == 160,
                "Undo did not restore full uncropped size.");

        // Grouped erase + Clear + Reset all + Undo
        require(doc.delete_annotations({pen_id, line_id}), "Grouped delete failed.");
        require(doc.annotations().size() == 5, "Grouped delete count differs.");
        require(doc.set_crop_rect({10, 10, 150, 130}), "Crop before reset failed.");
        require(doc.reset_all() && doc.annotations().empty() && !doc.is_cropped(), "Reset all failed.");
        require(doc.undo() && doc.annotations().size() == 5 && doc.is_cropped(),
                "Undo reset all did not restore annotations and crop.");
        require(doc.undo() && !doc.is_cropped(), "Undo crop failed.");
        require(doc.clear_annotations() && doc.annotations().empty(), "Clear annotations failed.");
        require(doc.undo() && doc.annotations().size() == 5, "Undo clear failed.");
        require(doc.undo() && doc.annotations().size() == 7, "Undo grouped delete failed.");
    }
}
void selector_hints()
{
    using std::wstring_view;
    require(wstring_view(pluck::selector_shortcut_text(false, false)) ==
                L"Tab for window  \u00b7  E for Edit  \u00b7  F2 for settings",
            "Area mode shortcut text differs.");
    require(wstring_view(pluck::selector_shortcut_text(false, true)) ==
                L"Tab for window  \u00b7  E for Edit Off  \u00b7  F2 for settings",
            "Area + edit mode shortcut text differs.");
    require(wstring_view(pluck::selector_shortcut_text(true, false)) ==
                L"Tab for area  \u00b7  E for Edit  \u00b7  F2 for settings",
            "Window mode shortcut text differs.");
    require(wstring_view(pluck::selector_shortcut_text(true, true)) ==
                L"Tab for area  \u00b7  E for Edit Off  \u00b7  F2 for settings",
            "Window + edit mode shortcut text differs.");
    require(wstring_view(pluck::selector_copy_error_text(false)) ==
                L"Cannot copy. Drag to try again, or press Esc to cancel.",
            "Area mode copy error text differs.");
    require(wstring_view(pluck::selector_copy_error_text(true)) ==
                L"Cannot copy. Click a window to try again, or press Esc to cancel.",
            "Window mode copy error text differs.");

    // Pill layout: centered near monitor work-area bottom, offset into virtual desktop coordinates.
    const RECT desktop{-1920, 0, 1920, 1080};
    const RECT left_work{-1920, 0, 0, 1040};
    const RECT pill = pluck::selector_shortcut_rect(left_work, desktop, {300, 18}, 96);
    const RECT want_pill{798, 986, 1122, 1020};
    require(EqualRect(&pill, &want_pill), "Selector shortcut pill layout differs at 96 DPI.");

    // DPI scaling (192 DPI = 2x): margin 40, padding 24, height + 32.
    const RECT hidpi_pill = pluck::selector_shortcut_rect({0, 0, 3840, 2100}, {0, 0, 3840, 2100}, {600, 36}, 192);
    const RECT want_hidpi{1596, 1992, 2244, 2060};
    require(EqualRect(&hidpi_pill, &want_hidpi), "Selector shortcut pill layout differs at 192 DPI.");

    // Narrow monitor clamps pill width inside margins; empty work area returns empty rect.
    const RECT narrow = pluck::selector_shortcut_rect({0, 0, 200, 400}, {0, 0, 200, 400}, {300, 18}, 96);
    require(narrow.left == 20 && narrow.right == 180 && narrow.bottom == 380,
            "Selector shortcut pill did not clamp to narrow monitor.");
    require(!pluck::has_area(pluck::selector_shortcut_rect({}, desktop, {300, 18}, 96)),
            "Empty monitor work area produced a non-empty shortcut rect.");

    // Visibility rules.
    require(pluck::selector_shortcut_visible(false, false, false, false, false, pill),
            "Idle selector shortcut pill should be visible.");
    require(!pluck::selector_shortcut_visible(true, false, false, false, false, pill),
            "Shortcut pill should hide while dragging.");
    require(!pluck::selector_shortcut_visible(false, true, false, false, false, pill),
            "Shortcut pill should hide while clicking a window.");
    require(!pluck::selector_shortcut_visible(false, false, true, false, false, pill),
            "Shortcut pill should hide while copying.");
    require(!pluck::selector_shortcut_visible(false, false, false, true, false, pill),
            "Shortcut pill should hide while closing.");
    require(!pluck::selector_shortcut_visible(false, false, false, false, true, pill),
            "Shortcut pill should hide when an error hint is active.");
    require(!pluck::selector_shortcut_visible(false, false, false, false, false, {}),
            "Shortcut pill should hide when its rect is empty.");

    // Off-screen GDI paint check for all four states (works on headless and isolated desktops).
    const auto font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                  L"Segoe UI");
    require(font != nullptr, "Cannot create test font for selector shortcut pill.");
    const auto to_bgra_rgb = [](COLORREF c) noexcept -> std::uint32_t
    {
        return (static_cast<std::uint32_t>(GetRValue(c)) << 16) |
               (static_cast<std::uint32_t>(GetGValue(c)) << 8) | static_cast<std::uint32_t>(GetBValue(c));
    };
    const auto fill_idle = to_bgra_rgb(pluck::SelectorShortcutFill);
    const auto fill_edit = to_bgra_rgb(pluck::SelectorShortcutEditFill);
    const auto border_rgb = to_bgra_rgb(pluck::SelectorShortcutBorder);
    for (bool window_mode : {false, true})
    {
        for (bool edit_mode : {false, true})
        {
            pluck::Bitmap canvas(520, 64);
            std::fill_n(canvas.pixels(), static_cast<size_t>(canvas.width()) * canvas.height(), 0xff000000u);
            const RECT box{20, 14, 500, 50};
            pluck::paint_selector_shortcut_pill(canvas.dc(), box, font, window_mode, edit_mode);
            const auto expected_fill = edit_mode ? fill_edit : fill_idle;
            size_t fill_pixels{};
            size_t border_pixels{};
            size_t glyph_pixels{};
            for (int y = box.top; y < box.bottom; ++y)
            {
                for (int x = box.left; x < box.right; ++x)
                {
                    const auto rgb = canvas.pixels()[static_cast<size_t>(y) * canvas.width() + x] & 0x00ffffffu;
                    if (rgb == expected_fill)
                        ++fill_pixels;
                    else if (rgb == border_rgb)
                        ++border_pixels;
                    else if (rgb != 0u)
                        ++glyph_pixels;
                }
            }
            require(fill_pixels > 1000, "Painted selector shortcut pill is missing its background fill.");
            require(border_pixels > 100, "Painted selector shortcut pill is missing its border.");
            require(glyph_pixels > 50, "Painted selector shortcut pill did not draw text glyphs.");
        }
    }
    DeleteObject(font);
}
} // namespace
int main()
{
    try
    {
        geometry();
        pixels();
        dimming();
        selector_hints();
        editor_engine();
        editor_markup();
        // Make sure each capture buffer releases its bitmap and device context.
        const auto before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        for (int i = 0; i < 200; ++i)
        {
            pluck::Bitmap image(3, 3);
            auto cropped = pluck::crop_bitmap(image, {0, 0, 3, 3});
            auto dib = pluck::make_clipboard_dib(*cropped, {0, 0, 3, 3});
        }
        require(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == before, "GDI objects leaked.");
        std::puts("PASS: rectangle geometry, pixel crops, DIB padding, PNG export, invalid inputs, and GDI "
                  "lifetime.");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
