#pragma once
#include "capture.h"

namespace pluck
{
inline constexpr wchar_t SelectorClass[] = L"Pluck.Native.Selector";
int run_selector(HINSTANCE instance);
} // namespace pluck
