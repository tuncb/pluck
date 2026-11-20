#include "editor_core.h"
#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace pluck
{
namespace
{
using Microsoft::WRL::ComPtr;

struct ComApartment
{
    bool active{};
    ComApartment()
    {
        const auto hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
            throw std::runtime_error("Cannot start the image encoder.");
        active = SUCCEEDED(hr);
    }
    ~ComApartment()
    {
        if (active)
            CoUninitialize();
    }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
};

void check_hr(HRESULT hr, const char* message)
{
    if (FAILED(hr))
        throw std::runtime_error(message);
}
} // namespace

EditorDocument::EditorDocument(std::unique_ptr<Bitmap> base_image) : base_(std::move(base_image))
{
    if (!base_ || base_->width() <= 0 || base_->height() <= 0 || !base_->pixels())
        throw std::runtime_error("The screenshot image is empty.");
}

const Bitmap& EditorDocument::base() const noexcept
{
    return *base_;
}

const Bitmap& EditorDocument::render_output() const noexcept
{
    return *base_;
}

int EditorDocument::width() const noexcept
{
    return base_->width();
}

int EditorDocument::height() const noexcept
{
    return base_->height();
}

GlobalMemory EditorDocument::make_clipboard_dib() const
{
    const auto& output = render_output();
    return pluck::make_clipboard_dib(output, {0, 0, output.width(), output.height()});
}

void EditorDocument::save_png(const fs::path& path) const
{
    pluck::save_png(render_output(), path);
}

void save_png(const Bitmap& source, const fs::path& path)
{
    if (source.width() <= 0 || source.height() <= 0 || !source.pixels())
        throw std::runtime_error("The screenshot image is empty.");
    if (path.empty())
        throw std::runtime_error("The file path is empty.");
    const auto width = static_cast<UINT>(source.width());
    const auto height = static_cast<UINT>(source.height());
    const auto stride = static_cast<uint64_t>(width) * 4u;
    const auto bytes = stride * height;
    if (stride > std::numeric_limits<UINT>::max() || bytes > std::numeric_limits<UINT>::max())
        throw std::runtime_error("The screenshot is too large to save.");

    std::error_code status_error;
    const bool existed = fs::exists(path, status_error);
    if (path.has_parent_path())
    {
        std::error_code dir_error;
        fs::create_directories(path.parent_path(), dir_error);
        if (dir_error)
            throw std::runtime_error("Cannot create the destination folder.");
    }

    ComApartment apartment;
    bool committed{};
    try
    {
        ComPtr<IWICImagingFactory> factory;
        check_hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory)),
                 "Cannot start the PNG encoder.");
        ComPtr<IWICBitmap> wic_source;
        // Treat the GDI 32-bit DIB section as opaque BGRX so unused alpha bytes never produce transparent PNGs.
        check_hr(factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGR,
                                                 static_cast<UINT>(stride), static_cast<UINT>(bytes),
                                                 reinterpret_cast<BYTE*>(source.pixels()), &wic_source),
                 "Cannot read the screenshot pixels.");
        ComPtr<IWICStream> stream;
        check_hr(factory->CreateStream(&stream), "Cannot prepare the output file.");
        check_hr(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE),
                 "Cannot open the file for writing.");
        ComPtr<IWICBitmapEncoder> encoder;
        check_hr(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder),
                 "Cannot create the PNG encoder.");
        check_hr(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache),
                 "Cannot initialize the PNG file.");
        ComPtr<IWICBitmapFrameEncode> frame;
        check_hr(encoder->CreateNewFrame(&frame, nullptr), "Cannot create the PNG frame.");
        check_hr(frame->Initialize(nullptr), "Cannot initialize the PNG frame.");
        check_hr(frame->SetSize(width, height), "Cannot set the PNG image size.");
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        check_hr(frame->SetPixelFormat(&format), "Cannot set the PNG pixel format.");
        check_hr(frame->WriteSource(wic_source.Get(), nullptr), "Cannot write the PNG image.");
        check_hr(frame->Commit(), "Cannot finalize the PNG image.");
        check_hr(encoder->Commit(), "Cannot save the PNG file.");
        committed = true;
    }
    catch (...)
    {
        if (!committed && !existed)
        {
            std::error_code ignored;
            fs::remove(path, ignored);
        }
        throw;
    }
}

std::wstring default_save_filename(const SYSTEMTIME& local_time)
{
    wchar_t name[64]{};
    std::swprintf(name, std::size(name), L"Pluck-%04u-%02u-%02u-%02u%02u%02u.png", local_time.wYear,
                  local_time.wMonth, local_time.wDay, local_time.wHour, local_time.wMinute,
                  local_time.wSecond);
    return name;
}

fs::path default_save_folder()
{
    PWSTR buffer{};
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &buffer)) && buffer)
    {
        fs::path folder(buffer);
        CoTaskMemFree(buffer);
        return folder;
    }
    return {};
}
} // namespace pluck
