#include "capture.h"
#include <cstdio>
#include <stdexcept>

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
} // namespace
int main()
{
    try
    {
        geometry();
        pixels();
        // Make sure each capture buffer releases its bitmap and device context.
        const auto before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        for (int i = 0; i < 200; ++i)
        {
            pluck::Bitmap image(3, 3);
            auto dib = pluck::make_clipboard_dib(image, {0, 0, 3, 3});
        }
        require(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == before, "GDI objects leaked.");
        std::puts("PASS: rectangle geometry, pixel crops, DIB padding, invalid inputs, and GDI lifetime.");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
