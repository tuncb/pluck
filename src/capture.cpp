#include "capture.h"
#include <emmintrin.h>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace pluck
{
namespace
{
constexpr size_t ParallelPixelThreshold = 512 * 1024;
constexpr unsigned MaxWorkerThreads = 4;

unsigned worker_count() noexcept
{
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    return std::clamp(static_cast<unsigned>(info.dwNumberOfProcessors), 1u, MaxWorkerThreads);
}

void prefault_span(std::uint32_t* pixels, size_t count) noexcept
{
    // Touch one pixel per 4 KiB page so demand-zero faults run in parallel.
    for (size_t i = 0; i < count; i += 1024)
        pixels[i] = 0;
    if (count > 0)
        pixels[count - 1] = 0;
}

inline std::uint32_t dim_pixel_scalar(std::uint32_t pixel) noexcept
{
    const auto red = ((pixel >> 16) & 255u) * 135u / 255u;
    const auto green = ((pixel >> 8) & 255u) * 135u / 255u;
    const auto blue = (pixel & 255u) * 135u / 255u;
    return (red << 16) | (green << 8) | blue;
}

bool dim_span_stream(const std::uint32_t* source, std::uint32_t* destination, size_t count) noexcept
{
    size_t i = 0;
    while (i < count && (reinterpret_cast<std::uintptr_t>(destination + i) & 15u) != 0)
    {
        destination[i] = dim_pixel_scalar(source[i]);
        ++i;
    }
    bool streamed = false;
    if (i + 4 <= count)
    {
        // floor(x * 34697 / 65536) equals floor(x * 135 / 255) for every byte 0..255.
        const __m128i zero = _mm_setzero_si128();
        const __m128i factor = _mm_set1_epi16(static_cast<short>(34697));
        const __m128i rgb_mask = _mm_set1_epi32(0x00ffffff);
        streamed = true;
        for (; i + 4 <= count; i += 4)
        {
            const __m128i pixels =
                _mm_and_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(source + i)), rgb_mask);
            const __m128i low = _mm_mulhi_epu16(_mm_unpacklo_epi8(pixels, zero), factor);
            const __m128i high = _mm_mulhi_epu16(_mm_unpackhi_epi8(pixels, zero), factor);
            _mm_stream_si128(reinterpret_cast<__m128i*>(destination + i), _mm_packus_epi16(low, high));
        }
    }
    for (; i < count; ++i)
        destination[i] = dim_pixel_scalar(source[i]);
    return streamed;
}
} // namespace

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

void prefault_bitmap(Bitmap& image) noexcept
{
    const size_t total = static_cast<size_t>(image.width()) * static_cast<size_t>(image.height());
    const unsigned threads = total >= ParallelPixelThreshold ? worker_count() : 1u;
    if (threads <= 1u)
    {
        prefault_span(image.pixels(), total);
        return;
    }
    struct Slice
    {
        std::uint32_t* pixels{};
        size_t count{};
    };
    std::array<Slice, MaxWorkerThreads> slices{};
    std::array<PTP_WORK, MaxWorkerThreads> works{};
    const size_t chunk = (total / threads) & ~size_t{1023};
    for (unsigned i = 0; i < threads; ++i)
    {
        const size_t offset = i * chunk;
        slices[i] = {image.pixels() + offset, i + 1 == threads ? total - offset : chunk};
    }
    const auto run = [](PTP_CALLBACK_INSTANCE, PVOID context, PTP_WORK) noexcept
    {
        const auto* slice = static_cast<const Slice*>(context);
        prefault_span(slice->pixels, slice->count);
    };
    for (unsigned i = 1; i < threads; ++i)
    {
        works[i] = CreateThreadpoolWork(run, &slices[i], nullptr);
        if (works[i])
            SubmitThreadpoolWork(works[i]);
    }
    prefault_span(slices[0].pixels, slices[0].count);
    for (unsigned i = 1; i < threads; ++i)
    {
        if (works[i])
        {
            WaitForThreadpoolWorkCallbacks(works[i], FALSE);
            CloseThreadpoolWork(works[i]);
        }
        else
        {
            prefault_span(slices[i].pixels, slices[i].count);
        }
    }
}

void capture_desktop_into(Bitmap& image, RECT bounds)
{
    if (bounds.right - bounds.left != image.width() || bounds.bottom - bounds.top != image.height())
        throw std::runtime_error("The capture buffer size differs from the desktop.");
    const auto screen = GetDC(nullptr);
    if (!screen)
        throw std::runtime_error("Cannot access the desktop.");
    const auto captured = BitBlt(image.dc(), 0, 0, image.width(), image.height(), screen, bounds.left,
                                 bounds.top, SRCCOPY | CAPTUREBLT);
    ReleaseDC(nullptr, screen);
    GdiFlush(); // Complete GDI writes before the CPU reads the DIB section.
    if (!captured)
        throw std::runtime_error("Cannot capture this desktop.");
}

std::unique_ptr<Bitmap> capture_desktop(RECT bounds)
{
    auto image = std::make_unique<Bitmap>(bounds.right - bounds.left, bounds.bottom - bounds.top);
    prefault_bitmap(*image);
    capture_desktop_into(*image, bounds);
    return image;
}

void dim_pixels(const std::uint32_t* source, std::uint32_t* destination, size_t count) noexcept
{
    if (dim_span_stream(source, destination, count))
        _mm_sfence();
}

void dim_bitmap(const Bitmap& source, Bitmap& destination, const RECT* regions, size_t region_count) noexcept
{
    if (source.width() != destination.width() || source.height() != destination.height())
        return;
    const int width = source.width();
    const int height = source.height();
    const size_t total = static_cast<size_t>(width) * static_cast<size_t>(height);

    // Clamp requested regions to the bitmap bounds.
    std::vector<RECT> clamped;
    size_t region_pixels = 0;
    bool covers_full = !regions || region_count == 0;
    if (!covers_full)
    {
        clamped.reserve(region_count);
        for (size_t i = 0; i < region_count; ++i)
        {
            const RECT rect{std::clamp<LONG>(regions[i].left, 0, width),
                            std::clamp<LONG>(regions[i].top, 0, height),
                            std::clamp<LONG>(regions[i].right, 0, width),
                            std::clamp<LONG>(regions[i].bottom, 0, height)};
            if (has_area(rect))
            {
                if (rect.left == 0 && rect.top == 0 && rect.right == width && rect.bottom == height)
                {
                    covers_full = true;
                    break;
                }
                clamped.push_back(rect);
                region_pixels += static_cast<size_t>(rect.right - rect.left) *
                                 static_cast<size_t>(rect.bottom - rect.top);
            }
        }
        if (!covers_full && clamped.empty())
            return;
    }

    if (covers_full)
    {
        const unsigned threads = total >= ParallelPixelThreshold ? worker_count() : 1u;
        if (threads <= 1u)
        {
            dim_pixels(source.pixels(), destination.pixels(), total);
            return;
        }
        struct Slice
        {
            const std::uint32_t* source{};
            std::uint32_t* destination{};
            size_t count{};
        };
        std::array<Slice, MaxWorkerThreads> slices{};
        std::array<PTP_WORK, MaxWorkerThreads> works{};
        const size_t chunk = (total / threads) & ~size_t{3};
        for (unsigned i = 0; i < threads; ++i)
        {
            const size_t offset = i * chunk;
            slices[i] = {source.pixels() + offset, destination.pixels() + offset,
                         i + 1 == threads ? total - offset : chunk};
        }
        const auto run = [](PTP_CALLBACK_INSTANCE, PVOID context, PTP_WORK) noexcept
        {
            const auto* slice = static_cast<const Slice*>(context);
            dim_pixels(slice->source, slice->destination, slice->count);
        };
        for (unsigned i = 1; i < threads; ++i)
        {
            works[i] = CreateThreadpoolWork(run, &slices[i], nullptr);
            if (works[i])
                SubmitThreadpoolWork(works[i]);
        }
        dim_pixels(slices[0].source, slices[0].destination, slices[0].count);
        for (unsigned i = 1; i < threads; ++i)
        {
            if (works[i])
            {
                WaitForThreadpoolWorkCallbacks(works[i], FALSE);
                CloseThreadpoolWork(works[i]);
            }
            else
            {
                dim_pixels(slices[i].source, slices[i].destination, slices[i].count);
            }
        }
        return;
    }

    // Dim only the visible monitor rectangles so non-monitor regions stay untouched.
    struct RowTask
    {
        const std::uint32_t* source{};
        std::uint32_t* destination{};
        int width{};
        const RECT* regions{};
        size_t region_count{};
        int row_start{};
        int row_end{};
    };
    const auto run_rows = [](const RowTask& task) noexcept
    {
        bool streamed = false;
        for (size_t r = 0; r < task.region_count; ++r)
        {
            const auto& rect = task.regions[r];
            const int y_start = std::max<int>(task.row_start, rect.top);
            const int y_end = std::min<int>(task.row_end, rect.bottom);
            const size_t span = static_cast<size_t>(rect.right - rect.left);
            for (int y = y_start; y < y_end; ++y)
            {
                const size_t offset =
                    static_cast<size_t>(y) * static_cast<size_t>(task.width) + static_cast<size_t>(rect.left);
                streamed |= dim_span_stream(task.source + offset, task.destination + offset, span);
            }
        }
        if (streamed)
            _mm_sfence();
    };

    const unsigned threads =
        (region_pixels >= ParallelPixelThreshold && height >= 4) ? worker_count() : 1u;
    if (threads <= 1u)
    {
        run_rows({source.pixels(), destination.pixels(), width, clamped.data(), clamped.size(), 0, height});
        return;
    }
    std::array<RowTask, MaxWorkerThreads> tasks{};
    std::array<PTP_WORK, MaxWorkerThreads> works{};
    const int rows_per_task = height / static_cast<int>(threads);
    for (unsigned i = 0; i < threads; ++i)
    {
        const int y_start = static_cast<int>(i) * rows_per_task;
        const int y_end = i + 1 == threads ? height : y_start + rows_per_task;
        tasks[i] = {source.pixels(), destination.pixels(), width, clamped.data(), clamped.size(), y_start,
                    y_end};
    }
    const auto callback = [](PTP_CALLBACK_INSTANCE, PVOID context, PTP_WORK) noexcept
    {
        const auto& task = *static_cast<const RowTask*>(context);
        bool streamed = false;
        for (size_t r = 0; r < task.region_count; ++r)
        {
            const auto& rect = task.regions[r];
            const int y_start = std::max<int>(task.row_start, rect.top);
            const int y_end = std::min<int>(task.row_end, rect.bottom);
            const size_t span = static_cast<size_t>(rect.right - rect.left);
            for (int y = y_start; y < y_end; ++y)
            {
                const size_t offset =
                    static_cast<size_t>(y) * static_cast<size_t>(task.width) + static_cast<size_t>(rect.left);
                streamed |= dim_span_stream(task.source + offset, task.destination + offset, span);
            }
        }
        if (streamed)
            _mm_sfence();
    };
    for (unsigned i = 1; i < threads; ++i)
    {
        works[i] = CreateThreadpoolWork(callback, &tasks[i], nullptr);
        if (works[i])
            SubmitThreadpoolWork(works[i]);
    }
    run_rows(tasks[0]);
    for (unsigned i = 1; i < threads; ++i)
    {
        if (works[i])
        {
            WaitForThreadpoolWorkCallbacks(works[i], FALSE);
            CloseThreadpoolWork(works[i]);
        }
        else
        {
            run_rows(tasks[i]);
        }
    }
}

std::unique_ptr<Bitmap> crop_bitmap(const Bitmap& source, RECT crop)
{
    if (!has_area(crop) || crop.left < 0 || crop.top < 0 || crop.right > source.width() ||
        crop.bottom > source.height())
        throw std::runtime_error("The selected area is outside the screenshot.");
    const int width = crop.right - crop.left;
    const int height = crop.bottom - crop.top;
    auto result = std::make_unique<Bitmap>(width, height);
    for (int y = 0; y < height; ++y)
    {
        const auto* input = source.pixels() + static_cast<size_t>(crop.top + y) * source.width() + crop.left;
        auto* output = result->pixels() + static_cast<size_t>(y) * width;
        for (int x = 0; x < width; ++x)
            output[x] = 0xff000000u | (input[x] & 0x00ffffffu);
    }
    return result;
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
