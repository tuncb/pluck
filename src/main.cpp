#include "selector.h"
#include "settings.h"
#include "app_process.h"
#include <cwchar>
#include <exception>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR arguments, int)
{
    if (std::wcscmp(arguments, L"--version") == 0)
    {
        constexpr char version[] = PLUCK_VERSION "\n";
        DWORD written{};
        return WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), version, sizeof(version) - 1, &written, nullptr)
                   ? 0
                   : 1;
    }
    if (*arguments)
    {
        try
        {
            return pluck::management_command(instance);
        }
        catch (const std::exception& error)
        {
            MessageBoxA(nullptr, error.what(), "Pluck", MB_OK | MB_ICONERROR);
            return 1;
        }
    }
    // The helper closes this gate only for file replacement, never for downloads.
    if (const auto maintenance = OpenMutexW(SYNCHRONIZE, FALSE, pluck::MaintenanceMutex))
    {
        CloseHandle(maintenance);
        return 0;
    }
    const auto mutex = CreateMutexW(nullptr, FALSE, pluck::CaptureMutex);
    if (!mutex)
        return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (const auto existing = FindWindowW(pluck::SelectorClass, nullptr))
            SetForegroundWindow(existing);
        CloseHandle(mutex);
        return 0;
    }
    // Close the race between the helper opening its gate and this capture starting.
    if (const auto maintenance = OpenMutexW(SYNCHRONIZE, FALSE, pluck::MaintenanceMutex))
    {
        CloseHandle(maintenance);
        CloseHandle(mutex);
        return 0;
    }
    int result{};
    try
    {
        result = pluck::run_selector(instance);
    }
    catch (const std::exception& error)
    {
        MessageBoxA(nullptr, error.what(), "Pluck", MB_OK | MB_ICONERROR);
        result = 1;
    }
    CloseHandle(mutex);
    return result;
}
