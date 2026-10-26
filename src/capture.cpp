#include "capture.h"
#include <cstring>
#include <limits>
#include <stdexcept>

namespace pluck
{
Bitmap::Bitmap(int width, int height) : width_(width), height_(height)
{
    // Bound allocations and the DWORD-sized DIB byte count before multiplication.
    if (width <= 0 || height <= 0 || static_cast<std::uint64_t>(width) * height > 128 * 1024 * 1024)
        throw std::runtime_error("The desktop is too large to capture.");
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    dc_ = CreateCompatibleDC(nullptr);
    if (!dc_)
        throw std::runtime_error("Cannot create the capture device.");
    bitmap_ = CreateDIBSection(dc_, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels_), nullptr, 0);
    if (!bitmap_)
    {
        DeleteDC(dc_);
        throw std::runtime_error("Cannot allocate the screenshot.");
    }
    previous_ = SelectObject(dc_, bitmap_);
    if (!previous_ || previous_ == HGDI_ERROR)
    {
        DeleteObject(bitmap_);
        DeleteDC(dc_);
        throw std::runtime_error("Cannot select the screenshot bitmap.");
    }
}

Bitmap::~Bitmap()
{
    SelectObject(dc_, previous_);
    DeleteObject(bitmap_);
    DeleteDC(dc_);
}

RECT desktop_bounds() noexcept
{
    const auto x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const auto y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    return {x, y, x + GetSystemMetrics(SM_CXVIRTUALSCREEN), y + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
}

std::unique_ptr<Bitmap> capture_desktop(RECT bounds)
{
    auto image = std::make_unique<Bitmap>(bounds.right - bounds.left, bounds.bottom - bounds.top);
    const auto screen = GetDC(nullptr);
    if (!screen)
        throw std::runtime_error("Cannot access the desktop.");
    const auto captured = BitBlt(image->dc(), 0, 0, image->width(), image->height(), screen, bounds.left,
                                 bounds.top, SRCCOPY | CAPTUREBLT);
    ReleaseDC(nullptr, screen);
    GdiFlush(); // Complete GDI writes before the CPU reads the DIB section.
    if (!captured)
        throw std::runtime_error("Cannot capture this desktop.");
    return image;
}

GlobalMemory make_clipboard_dib(const Bitmap& source, RECT crop)
{
    if (!has_area(crop) || crop.left < 0 || crop.top < 0 || crop.right > source.width() ||
        crop.bottom > source.height())
        throw std::runtime_error("The selected area is outside the screenshot.");
    const auto width = crop.right - crop.left;
    const auto height = crop.bottom - crop.top;
    // A bottom-up, 24-bit CF_DIB has no ambiguous alpha channel and works in image editors.
    const size_t stride = (static_cast<size_t>(width) * 3 + 3) & ~size_t{3};
    const size_t bytes = stride * height;
    if (bytes > std::numeric_limits<DWORD>::max())
        throw std::runtime_error("The selected area is too large for the clipboard.");
    GlobalMemory data(GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(BITMAPINFOHEADER) + bytes));
    if (!data)
        throw std::runtime_error("Cannot allocate the clipboard image.");
    auto* header = static_cast<BITMAPINFOHEADER*>(GlobalLock(data.get()));
    if (!header)
        throw std::runtime_error("Cannot access the clipboard image.");
    header->biSize = sizeof(BITMAPINFOHEADER);
    header->biWidth = width;
    header->biHeight = height;
    header->biPlanes = 1;
    header->biBitCount = 24;
    header->biCompression = BI_RGB;
    header->biSizeImage = static_cast<DWORD>(bytes);
    auto* destination = reinterpret_cast<std::uint8_t*>(header + 1);
    for (LONG y = 0; y < height; ++y)
    {
        const auto* row =
            source.pixels() + static_cast<size_t>(crop.bottom - 1 - y) * source.width() + crop.left;
        auto* output = destination + static_cast<size_t>(y) * stride;
        for (LONG x = 0; x < width; ++x)
        {
            output[x * 3] = static_cast<std::uint8_t>(row[x]);
            output[x * 3 + 1] = static_cast<std::uint8_t>(row[x] >> 8);
            output[x * 3 + 2] = static_cast<std::uint8_t>(row[x] >> 16);
        }
    }
    GlobalUnlock(data.get());
    return data;
}

CopyResult copy_to_clipboard(HWND owner, GlobalMemory& data) noexcept
{
    if (!owner || !data)
        return CopyResult::Failed;
    if (!OpenClipboard(owner))
        return CopyResult::Busy;
    const auto copied = EmptyClipboard() && SetClipboardData(CF_DIB, data.get());
    if (copied)
        data.release();
    CloseClipboard();
    return copied ? CopyResult::Copied : CopyResult::Failed;
}
} // namespace pluck
