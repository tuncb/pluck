#pragma once
#include "capture.h"
#include <filesystem>
#include <memory>
#include <string>

namespace pluck
{
namespace fs = std::filesystem;

class EditorDocument
{
  public:
    explicit EditorDocument(std::unique_ptr<Bitmap> base_image);
    const Bitmap& base() const noexcept;
    const Bitmap& render_output() const noexcept;
    int width() const noexcept;
    int height() const noexcept;
    GlobalMemory make_clipboard_dib() const;
    void save_png(const fs::path& path) const;

  private:
    std::unique_ptr<Bitmap> base_;
};

void save_png(const Bitmap& source, const fs::path& path);
std::wstring default_save_filename(const SYSTEMTIME& local_time);
fs::path default_save_folder();
} // namespace pluck
