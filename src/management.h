#pragma once
#include <windows.h>
#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace pluck::management
{
namespace fs = std::filesystem;
inline constexpr BYTE HotkeyShift = 0x01;
inline constexpr BYTE HotkeyControl = 0x02;
inline constexpr BYTE HotkeyAlt = 0x04;
inline constexpr BYTE HotkeyExtended = 0x08;
inline constexpr WORD DefaultLaunchHotkey = static_cast<WORD>((HotkeyControl << 8) | VK_F12);
struct HotkeyValidation
{
    bool valid{};
    WORD hotkey{};
    std::wstring error;
};
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
bool is_valid_launch_hotkey(WORD hotkey) noexcept;
HotkeyValidation validate_launch_hotkey(UINT vk, bool ctrl, bool alt, bool shift, bool win,
                                        bool extended = false);
std::wstring format_launch_hotkey(WORD hotkey);
std::optional<WORD> read_shortcut_hotkey(const fs::path& link_path);
std::optional<WORD> read_saved_launch_hotkey();
void write_saved_launch_hotkey(WORD hotkey);
WORD effective_launch_hotkey(const fs::path& directory);
void create_shortcuts(const fs::path& root, const fs::path& directory, WORD hotkey = DefaultLaunchHotkey);
void update_launch_hotkey(const fs::path& root, const fs::path& directory, WORD hotkey);
void replace_executable(const fs::path& source, const fs::path& destination);
// Stages all files before starting a helper. A successful call requires the UI to exit.
void prepare_install(const Progress& progress, std::stop_token stop);
void prepare_update(const Release& release, const Progress& progress, std::stop_token stop);
void prepare_uninstall();
void cleanup_jobs();
int finish_job(const fs::path& job, DWORD parent);
} // namespace pluck::management
