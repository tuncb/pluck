#include "capture.h"
#include "editor_core.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
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
} // namespace
int main()
{
    try
    {
        geometry();
        pixels();
        dimming();
        editor_engine();
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
