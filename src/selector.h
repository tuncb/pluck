#pragma once
#include "capture.h"

namespace pluck
{
inline constexpr wchar_t SelectorClass[] = L"Pluck.Native.Selector";
struct SelectorResult
{
    int exit_code{};
    std::unique_ptr<Bitmap> edit_image;
    RECT selection_screen_rect{};
};
SelectorResult run_selector(HINSTANCE instance);
} // namespace pluck
