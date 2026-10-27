#include "management.h"
#include "app_process.h"
#include <shellapi.h>
#include <shobjidl.h>
#include <winrt/base.h>
#include <wrl/client.h>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace
{
using namespace pluck::management;
void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
template <typename F> void rejects(F action)
{
    bool rejected{};
    try
    {
        action();
    }
    catch (const std::exception&)
    {
        rejected = true;
    }
    require(rejected, "Invalid management input was accepted.");
}
void metadata()
{
    require(version("1.10.2") > version("1.9.9"), "Versions were compared as text.");
    for (auto invalid :
         {"", "v1.2.3", "01.2.3", "1.2", "1.2.3.4", "1.2.3-beta", "-1.2.3", "65536.0.0", "1.2.3 "})
        rejects([&] { version(invalid); });
    const std::string hash(64, 'a');
    require(checksum(hash + "  Pluck.exe\n") == hash, "Checksum did not parse.");
    rejects([&] { checksum(hash + "  Other.exe\n"); });
    rejects([&] { checksum("prefix " + hash); });
    rejects([&] { checksum(std::string(64, 'z')); });
    const std::string response = R"({"draft":false,"prerelease":false,"tag_name":"v1.2.3","assets":[
      {"name":"Pluck.exe","size":2000,"browser_download_url":"https://github.com/tuncb/pluck/releases/download/v1.2.3/Pluck.exe"},
      {"name":"Pluck.exe.sha256","size":76,"browser_download_url":"https://github.com/tuncb/pluck/releases/download/v1.2.3/Pluck.exe.sha256"}]})";
    const auto release = parse_release(response);
    require(release.version == "1.2.3" && release.size == 2000 && !release.checksum_url.empty(),
            "Release parsing failed.");
    for (const auto& change :
         {std::pair{"\"draft\":false", "\"draft\":true"},
          std::pair{"\"prerelease\":false", "\"prerelease\":true"}, std::pair{"\"size\":2000", "\"size\":-1"},
          std::pair{"github.com/tuncb/pluck", "github.com/other/pluck"},
          std::pair{"\"v1.2.3\"", "\"v1.2.3-beta\""}})
    {
        auto invalid = response;
        invalid.replace(invalid.find(change.first), strlen(change.first), change.second);
        rejects([&] { parse_release(invalid); });
    }
    rejects([] { parse_release("not json"); });
    const auto legacy =
        parse_release(R"({"draft":false,"prerelease":false,"tag_name":"v0.1.0","assets":[]})");
    require(legacy.url.empty(), "Legacy releases must not fabricate download URLs.");
    const std::wstring argument = L"C:\\Users\\Test User\\trailing\\";
    const auto command = L"app.exe " + pluck::quote_argument(argument);
    int count{};
    auto** values = CommandLineToArgvW(command.c_str(), &count);
    require(values && count == 2 && argument == values[1], "Windows argument quoting failed.");
    LocalFree(values);
}
void files(const fs::path& root, const fs::path& application)
{
    require_plain_path(root);
    rejects([&] { require_plain_path(root / L".." / L"outside"); });
    fs::create_directories(root);
    const auto source = root / L"new.exe", target = root / L"Pluck.exe";
    std::ofstream(source) << "abc";
    require(file_hash(source) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "SHA-256 differs.");
    replace_executable(source, target);
    require(file_hash(source) == file_hash(target), "Initial install differs.");
    std::ofstream(source) << "replacement";
    const auto original = file_hash(target);
    {
        Handle locked(
            CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
        require(locked.value != INVALID_HANDLE_VALUE, "Cannot lock test executable.");
        rejects([&] { replace_executable(source, target); });
        require(file_hash(target) == original, "Failed replacement changed the original.");
    }
    replace_executable(source, target);
    require(file_hash(source) == file_hash(target), "Replacement differs.");
    std::ofstream(root / L"Pluck.previous.exe") << "unrelated";
    rejects([&] { replace_executable(source, target); });
    require(fs::exists(root / L"Pluck.previous.exe"), "Existing backup was deleted.");
    verify_executable(application, PLUCK_VERSION);
    rejects([&] { verify_executable(application, "65535.65535.65535"); });
    rejects([&] { verify_executable(source, PLUCK_VERSION); });
    const auto shortcuts = root / L"Start Menu";
    create_shortcuts(root, shortcuts);
    for (const auto settings : {false, true})
    {
        Microsoft::WRL::ComPtr<IShellLinkW> link;
        Microsoft::WRL::ComPtr<IPersistFile> persist;
        require(SUCCEEDED(
                    CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) &&
                    SUCCEEDED(link.As(&persist)),
                "Cannot read test shortcut.");
        require(SUCCEEDED(persist->Load(
                    (shortcuts / (settings ? L"Pluck Settings.lnk" : L"Pluck.lnk")).c_str(), STGM_READ)),
                "Shortcut was not created.");
        wchar_t args[256]{}, path[32768]{};
        require(SUCCEEDED(link->GetArguments(args, 256)) &&
                    SUCCEEDED(link->GetPath(path, 32768, nullptr, SLGP_RAWPATH)),
                "Cannot inspect shortcut.");
        require(same_path(path, target) && std::wstring(args) == (settings ? L"--settings" : L""),
                "Shortcut mode differs.");
    }
}
} // namespace
int wmain(int count, wchar_t** arguments)
{
    if (count != 2)
        return 1;
    const auto root =
        fs::temp_directory_path() / (L"pluck-management-test-" + std::to_wstring(GetCurrentProcessId()) +
                                     L"-" + std::to_wstring(GetTickCount64()));
    int result{};
    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        metadata();
        files(root, arguments[1]);
        std::puts("PASS: release validation, hashes, executable checks, replacement failure recovery, and "
                  "shortcut modes.");
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        result = 1;
    }
    catch (...)
    {
        std::fputs("FAIL: Windows management API failed.\n", stderr);
        result = 1;
    }
    winrt::uninit_apartment();
    require_plain_path(root);
    fs::remove_all(root);
    return result;
}
