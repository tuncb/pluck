#pragma once
#include "capture.h"
#include <memory>

namespace pluck
{
int run_editor(HINSTANCE instance, std::unique_ptr<Bitmap> image, RECT selection_screen_rect);
} // namespace pluck
