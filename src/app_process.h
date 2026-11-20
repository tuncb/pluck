#pragma once
#include <windows.h>
#include <string>
#include <string_view>

namespace pluck
{
inline constexpr wchar_t SettingsClass[] = L"Pluck.Native.Settings";
inline constexpr wchar_t EditorClass[] = L"Pluck.Native.Editor";
inline constexpr wchar_t CaptureMutex[] = L"Local\\Pluck.Screenshot";
inline constexpr wchar_t EditorMutex[] = L"Local\\Pluck.Editor";
inline constexpr wchar_t MaintenanceMutex[] = L"Local\\Pluck.Maintenance";
inline constexpr UINT OpenSettingsMessage = WM_APP + 1;
inline constexpr UINT ToggleEditModeMessage = WM_APP + 2;
std::wstring executable_path();
std::wstring quote_argument(std::wstring_view value);
bool launch_settings();
void launch_process(const std::wstring& executable, const std::wstring& arguments);
} // namespace pluck
