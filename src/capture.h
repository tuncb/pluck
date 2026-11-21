#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <memory>

namespace pluck
{
// All rectangles use physical pixels and an exclusive right/bottom edge.
inline RECT selection(POINT start, POINT end, RECT bounds) noexcept
{
    start.x = std::clamp(start.x, bounds.left, bounds.right);
    start.y = std::clamp(start.y, bounds.top, bounds.bottom);
    end.x = std::clamp(end.x, bounds.left, bounds.right);
    end.y = std::clamp(end.y, bounds.top, bounds.bottom);
    return {std::min(start.x, end.x), std::min(start.y, end.y), std::max(start.x, end.x),
            std::max(start.y, end.y)};
}
inline bool has_area(RECT rect) noexcept
{
    return rect.right > rect.left && rect.bottom > rect.top;
}
inline RECT clamp_window_rect(RECT window_rect, RECT desktop_bounds, const RECT* clip_bounds = nullptr) noexcept
{
    RECT clipped = window_rect;
    if (clip_bounds)
    {
        clipped.left = std::max(clipped.left, clip_bounds->left);
        clipped.top = std::max(clipped.top, clip_bounds->top);
        clipped.right = std::min(clipped.right, clip_bounds->right);
        clipped.bottom = std::min(clipped.bottom, clip_bounds->bottom);
    }
    clipped.left = std::max(clipped.left, desktop_bounds.left);
    clipped.top = std::max(clipped.top, desktop_bounds.top);
    clipped.right = std::min(clipped.right, desktop_bounds.right);
    clipped.bottom = std::min(clipped.bottom, desktop_bounds.bottom);
    if (!has_area(clipped))
        return {};
    OffsetRect(&clipped, -desktop_bounds.left, -desktop_bounds.top);
    return clipped;
}
inline RECT hit_test_rects(const RECT* rects, size_t count, POINT point) noexcept
{
    if (!rects)
        return {};
    for (size_t i = 0; i < count; ++i)
    {
        const auto& rect = rects[i];
        if (has_area(rect) && point.x >= rect.left && point.x < rect.right && point.y >= rect.top &&
            point.y < rect.bottom)
            return rect;
    }
    return {};
}

class Bitmap
{
  public:
    Bitmap(int width, int height);
    ~Bitmap();
    Bitmap(const Bitmap&) = delete;
    Bitmap& operator=(const Bitmap&) = delete;
    HDC dc() const noexcept
    {
        return dc_;
    }
    std::uint32_t* pixels() const noexcept
    {
        return pixels_;
    }
    int width() const noexcept
    {
        return width_;
    }
    int height() const noexcept
    {
        return height_;
    }

  private:
    HDC dc_{};
    HBITMAP bitmap_{};
    HGDIOBJ previous_{};
    std::uint32_t* pixels_{};
    int width_{}, height_{};
};

struct GlobalDeleter
{
    void operator()(void* data) const noexcept
    {
        GlobalFree(data);
    }
};
using GlobalMemory = std::unique_ptr<void, GlobalDeleter>;

RECT desktop_bounds() noexcept;
void prefault_bitmap(Bitmap& image) noexcept;
void capture_desktop_into(Bitmap& image, RECT bounds);
std::unique_ptr<Bitmap> capture_desktop(RECT bounds);
void dim_pixels(const std::uint32_t* source, std::uint32_t* destination, size_t count) noexcept;
void dim_bitmap(const Bitmap& source, Bitmap& destination, const RECT* regions = nullptr,
                size_t region_count = 0) noexcept;
std::unique_ptr<Bitmap> crop_bitmap(const Bitmap& source, RECT crop);
GlobalMemory make_clipboard_dib(const Bitmap& source, RECT crop);
enum class CopyResult
{
    Copied,
    Busy,
    Failed
};
// Ownership transfers only on success. Data is fully rendered before process exit.
CopyResult copy_to_clipboard(HWND owner, GlobalMemory& data) noexcept;
} // namespace pluck
