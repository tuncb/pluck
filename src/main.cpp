#include "selector.h"
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
    const auto mutex = CreateMutexW(nullptr, FALSE, L"Local\\Pluck.Screenshot");
    if (!mutex)
        return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (const auto existing = FindWindowW(pluck::SelectorClass, nullptr))
            SetForegroundWindow(existing);
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
