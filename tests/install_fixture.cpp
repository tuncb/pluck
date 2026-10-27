// This executable runs the production installer in an isolated user-folder and registry fixture.
// It is never included in a published package.
#include "management.h"
#include "app_process.h"
#include <shellapi.h>
#include <fstream>
#include <vector>

using namespace pluck::management;
namespace
{
std::string next_version()
{
    auto parts = version(PLUCK_VERSION);
    return std::to_string(parts[0]) + "." + std::to_string(parts[1]) + "." + std::to_string(parts[2] + 1);
}
void make_update(const fs::path& destination)
{
    fs::copy_file(pluck::executable_path(), destination);
    const auto module = LoadLibraryExW(destination.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
    if (!module)
        throw std::runtime_error("Cannot read fixture resources.");
    const auto resource = FindResourceW(module, MAKEINTRESOURCEW(1), RT_VERSION);
    const auto loaded = LoadResource(module, resource);
    const auto bytes = SizeofResource(module, resource);
    const auto* data = static_cast<const unsigned char*>(LockResource(loaded));
    if (!resource || !loaded || !bytes || !data)
        throw std::runtime_error("Missing fixture version.");
    std::vector<unsigned char> version_data(data, data + bytes);
    FreeLibrary(module);
    bool changed{};
    const auto expected = version(next_version());
    for (size_t offset = 0; offset + sizeof(VS_FIXEDFILEINFO) <= version_data.size(); offset += 4)
    {
        auto* info = reinterpret_cast<VS_FIXEDFILEINFO*>(version_data.data() + offset);
        if (info->dwSignature == 0xfeef04bd)
        {
            info->dwProductVersionMS = info->dwFileVersionMS = MAKELONG(expected[1], expected[0]);
            info->dwProductVersionLS = info->dwFileVersionLS = MAKELONG(0, expected[2]);
            changed = true;
            break;
        }
    }
    if (!changed)
        throw std::runtime_error("Missing fixed version.");
    const auto update = BeginUpdateResourceW(destination.c_str(), FALSE);
    if (!update ||
        !UpdateResourceW(update, RT_VERSION, MAKEINTRESOURCEW(1),
                         MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), version_data.data(),
                         static_cast<DWORD>(version_data.size())) ||
        !EndUpdateResourceW(update, FALSE))
        throw std::runtime_error("Cannot update fixture resources.");
    verify_executable(destination, next_version());
}
} // namespace
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    wchar_t buffer[32768]{};
    if (!GetEnvironmentVariableW(L"PLUCK_FIXTURE_ROOT", buffer, 32768))
        return 1;
    const fs::path root(buffer);
    HKEY hive{};
    int result{};
    try
    {
        require_plain_path(root);
        const auto key = L"Software\\Pluck.Tests\\" + root.filename().wstring();
        if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &hive,
                            nullptr) != ERROR_SUCCESS ||
            RegOverridePredefKey(HKEY_CURRENT_USER, hive) != ERROR_SUCCESS)
            throw std::runtime_error("Cannot isolate fixture registry.");
        int count{};
        auto** args = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!args)
            throw std::runtime_error("Cannot read fixture arguments.");
        struct Args
        {
            wchar_t** value;
            ~Args()
            {
                LocalFree(value);
            }
        } owned{args};
        if (count == 2 && wcscmp(args[1], L"--install") == 0)
            prepare_install([](auto) {}, {});
        else if (count == 2 && wcscmp(args[1], L"--uninstall") == 0)
            prepare_uninstall();
        else if (count == 2 && wcscmp(args[1], L"--settings") == 0)
            std::ofstream(root / L"settings-opened.txt") << utf8(pluck::executable_path());
        else if (count == 3 && wcscmp(args[1], L"--make-update") == 0)
            make_update(args[2]);
        else if ((count == 3 || count == 4) && wcscmp(args[1], L"--stage-update") == 0)
        {
            const auto job = root / L"Local" / L"Pluck" / L"Maintenance" /
                             (L"job-fixture-" + std::to_wstring(GetCurrentProcessId()));
            fs::create_directories(job);
            fs::copy_file(args[2], job / L"Pluck.next.exe");
            fs::copy_file(pluck::executable_path(), job / L"helper.exe");
            std::ofstream(job / L"request.txt", std::ios::binary)
                << "update\n"
                << next_version() << '\n'
                << (count == 4 ? std::string(64, '0') : file_hash(job / L"Pluck.next.exe")) << '\n';
            pluck::launch_process((job / L"helper.exe").wstring(),
                                  L"--finish-job " + pluck::quote_argument(job.wstring()) + L" " +
                                      std::to_wstring(GetCurrentProcessId()));
        }
        else if (count == 4 && wcscmp(args[1], L"--finish-job") == 0)
            result = finish_job(args[2], wcstoul(args[3], nullptr, 10));
        else
            throw std::runtime_error("Invalid fixture command.");
    }
    catch (const std::exception& error)
    {
        std::ofstream(root / L"last-error.txt") << error.what();
        result = 1;
    }
    RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    if (hive)
        RegCloseKey(hive);
    return result;
}
