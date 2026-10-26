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
std::unique_ptr<Bitmap> capture_desktop(RECT bounds);
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
