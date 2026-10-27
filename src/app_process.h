#pragma once
#include <windows.h>
#include <string>
#include <string_view>

namespace pluck
{
inline constexpr wchar_t SettingsClass[] = L"Pluck.Native.Settings";
inline constexpr wchar_t CaptureMutex[] = L"Local\\Pluck.Screenshot";
inline constexpr wchar_t MaintenanceMutex[] = L"Local\\Pluck.Maintenance";
inline constexpr UINT OpenSettingsMessage = WM_APP + 1;
std::wstring executable_path();
std::wstring quote_argument(std::wstring_view value);
bool launch_settings();
void launch_process(const std::wstring& executable, const std::wstring& arguments);
} // namespace pluck
