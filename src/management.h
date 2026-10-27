#pragma once
#include <windows.h>
#include <array>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>

namespace pluck::management
{
namespace fs = std::filesystem;
struct Handle
{
    HANDLE value{};
    explicit Handle(HANDLE v = nullptr) : value(v) {}
    ~Handle()
    {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
struct Release
{
    std::string version, url, checksum_url;
    uint64_t size{};
};
using Progress = std::function<void(std::wstring)>;
std::array<unsigned, 3> version(std::string_view text);
std::string checksum(std::string_view text);
std::string file_hash(const fs::path& file);
std::wstring wide(std::string_view text);
std::string utf8(std::wstring_view text);
fs::path install_directory();
fs::path shortcut_directory();
bool is_installed();
bool same_path(const fs::path& a, const fs::path& b);
void require_plain_path(const fs::path& path);
void verify_executable(const fs::path& file, std::string_view expected_version);
Release parse_release(std::string_view json);
Release check_release(std::stop_token stop);
void create_shortcuts(const fs::path& root, const fs::path& directory);
void replace_executable(const fs::path& source, const fs::path& destination);
// Stages all files before starting a helper. A successful call requires the UI to exit.
void prepare_install(const Progress& progress, std::stop_token stop);
void prepare_update(const Release& release, const Progress& progress, std::stop_token stop);
void prepare_uninstall();
void cleanup_jobs();
int finish_job(const fs::path& job, DWORD parent);
} // namespace pluck::management
