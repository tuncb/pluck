#include "app_process.h"
#include <stdexcept>

namespace pluck
{
std::wstring executable_path()
{
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size())
        throw std::runtime_error("Cannot find Pluck.exe.");
    path.resize(length);
    return path;
}
std::wstring quote_argument(std::wstring_view value)
{
    std::wstring result = L"\"";
    size_t slashes{};
    for (auto c : value)
    {
        if (c == L'\\')
        {
            ++slashes;
            continue;
        }
        result.append(c == L'\"' ? 2 * slashes + 1 : slashes, L'\\');
        slashes = 0;
        result += c;
    }
    result.append(2 * slashes, L'\\');
    return result + L'\"';
}
void launch_process(const std::wstring& executable, const std::wstring& arguments)
{
    auto command = quote_argument(executable) + (arguments.empty() ? L"" : L" " + arguments);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                        &startup, &child))
        throw std::runtime_error("Cannot open Pluck. Try again.");
    AllowSetForegroundWindow(child.dwProcessId);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
}
bool launch_settings()
{
    if (const auto window = FindWindowW(SettingsClass, nullptr))
    {
        ShowWindow(window, SW_RESTORE);
        SetForegroundWindow(window);
        return true;
    }
    launch_process(executable_path(), L"--settings");
    return true;
}
} // namespace pluck
