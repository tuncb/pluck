#include "management.h"
#include "app_process.h"
#include <bcrypt.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>
#include <vector>

namespace pluck::management
{
namespace
{
constexpr wchar_t UninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Pluck";
constexpr wchar_t SettingsKey[] = L"Software\\Pluck";
constexpr wchar_t LaunchHotkeyValue[] = L"LaunchHotkey";
constexpr wchar_t OperationMutex[] = L"Local\\Pluck.InstallOperation";
struct ComApartment
{
    bool active{};
    ComApartment()
    {
        const auto result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(result) && result != RPC_E_CHANGED_MODE)
            throw std::runtime_error("Cannot prepare Start menu shortcuts.");
        active = SUCCEEDED(result);
    }
    ~ComApartment()
    {
        if (active)
            CoUninitialize();
    }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
};
bool is_function_vk(UINT vk) noexcept
{
    return vk >= VK_F1 && vk <= VK_F24;
}
bool is_numpad_vk(UINT vk) noexcept
{
    return (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) || vk == VK_MULTIPLY || vk == VK_ADD ||
           vk == VK_SUBTRACT || vk == VK_DECIMAL || vk == VK_DIVIDE;
}
bool is_alphanumeric_vk(UINT vk) noexcept
{
    return (vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9');
}
int modifier_count(BYTE mods) noexcept
{
    return ((mods & HotkeyControl) ? 1 : 0) + ((mods & HotkeyAlt) ? 1 : 0) + ((mods & HotkeyShift) ? 1 : 0);
}
void notify_shortcut_change(LONG event_id, const fs::path& path) noexcept
{
    SHChangeNotify(event_id, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, path.c_str(), nullptr);
}
void clear_shortcut_hotkey(const fs::path& link_path) noexcept
{
    try
    {
        require_plain_path(link_path);
        if (!fs::exists(link_path))
            return;
        ComApartment apartment;
        Microsoft::WRL::ComPtr<IShellLinkW> link;
        Microsoft::WRL::ComPtr<IPersistFile> persist;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) &&
            SUCCEEDED(link.As(&persist)) && SUCCEEDED(persist->Load(link_path.c_str(), STGM_READWRITE)) &&
            SUCCEEDED(link->SetHotkey(0)) && SUCCEEDED(persist->Save(link_path.c_str(), TRUE)))
            notify_shortcut_change(SHCNE_UPDATEITEM, link_path);
    }
    catch (...)
    {
        // Best-effort cleanup before deleting a shortcut during uninstall.
    }
}
void delete_saved_launch_hotkey() noexcept
{
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SettingsKey, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS)
        return;
    RegDeleteValueW(key, LaunchHotkeyValue);
    DWORD subkeys{}, values{};
    const bool empty =
        RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values, nullptr, nullptr,
                         nullptr, nullptr) == ERROR_SUCCESS &&
        subkeys == 0 && values == 0;
    RegCloseKey(key);
    if (empty)
        RegDeleteKeyW(HKEY_CURRENT_USER, SettingsKey);
}
fs::path known_folder(REFKNOWNFOLDERID id)
{
#ifdef PLUCK_TEST_ENVIRONMENT
    // Only the offline fixture target has this override. The shipped app has no path override.
    wchar_t root[32768]{};
    const auto length = GetEnvironmentVariableW(L"PLUCK_FIXTURE_ROOT", root, 32768);
    if (!length || length >= 32768)
        throw std::runtime_error("Missing fixture root.");
    return fs::path(root) / (id == FOLDERID_LocalAppData ? L"Local" : L"Start Menu");
#else
    PWSTR buffer{};
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &buffer)))
        throw std::runtime_error("Cannot find the user folder.");
    fs::path result(buffer);
    CoTaskMemFree(buffer);
    return result;
#endif
}
fs::path jobs_directory()
{
    return known_folder(FOLDERID_LocalAppData) / L"Pluck" / L"Maintenance";
}
void cancelled(std::stop_token stop)
{
    if (stop.stop_requested())
        throw std::runtime_error("Operation cancelled.");
}
void register_installation(const fs::path& root, std::string_view installed_version)
{
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, UninstallKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
        throw std::runtime_error("Cannot register Pluck with Windows.");
    const auto put = [key](const wchar_t* name, const std::wstring& value)
    {
        return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                              static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    };
    const auto exe = root / L"Pluck.exe";
    const bool ok = put(L"DisplayName", L"Pluck") && put(L"DisplayVersion", wide(installed_version)) &&
                    put(L"Publisher", L"tuncb") && put(L"InstallLocation", root.wstring()) &&
                    put(L"DisplayIcon", exe.wstring()) &&
                    put(L"UninstallString", quote_argument(exe.wstring()) + L" --uninstall");
    RegCloseKey(key);
    if (!ok)
        throw std::runtime_error("Cannot save the Windows installation entry.");
}
fs::path make_job()
{
    const auto root = jobs_directory();
    require_plain_path(root);
    fs::create_directories(root);
    for (unsigned attempt = 0; attempt < 8; ++attempt)
    {
        std::array<unsigned char, 16> random{};
        if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            throw std::runtime_error("Cannot prepare the update folder.");
        std::wstring name = L"job-";
        for (const auto byte : random)
        {
            name += L"0123456789abcdef"[byte >> 4];
            name += L"0123456789abcdef"[byte & 15];
        }
        const auto path = root / name;
        if (fs::create_directory(path))
            return path;
    }
    throw std::runtime_error("Cannot create the update folder.");
}
void write_request(const fs::path& job, const std::string& action, const std::string& target_version,
                   const std::string& hash)
{
    std::ofstream output(job / L"request.txt", std::ios::binary);
    output << action << '\n' << target_version << '\n' << hash << '\n';
    output.close();
    if (!output)
        throw std::runtime_error("Cannot save the update request.");
}
void start_helper(const fs::path& job)
{
    const auto helper = job / L"helper.exe";
    fs::copy_file(executable_path(), helper);
    if (file_hash(helper) != file_hash(executable_path()))
        throw std::runtime_error("Cannot verify the update helper.");
    launch_process(helper.wstring(), L"--finish-job " + quote_argument(job.wstring()) + L" " +
                                         std::to_wstring(GetCurrentProcessId()));
}
void check_operation(HANDLE handle)
{
    if (!handle || GetLastError() == ERROR_ALREADY_EXISTS)
        throw std::runtime_error("Another Pluck installation or update is in progress.");
}
void wait_for_parent(DWORD parent)
{
    if (!parent || parent == GetCurrentProcessId())
        throw std::runtime_error("Invalid update process.");
    Handle process(OpenProcess(SYNCHRONIZE, FALSE, parent));
    if (!process.value)
    {
        if (GetLastError() == ERROR_INVALID_PARAMETER)
            return;
        throw std::runtime_error("Cannot wait for Pluck to close.");
    }
    if (WaitForSingleObject(process.value, 60000) != WAIT_OBJECT_0)
        throw std::runtime_error("Pluck is still open. Close Settings and try again.");
}
void wait_for_capture()
{
    const auto deadline = GetTickCount64() + 60000;
    for (;;)
    {
        Handle capture(OpenMutexW(SYNCHRONIZE, FALSE, CaptureMutex));
        if (capture.value)
        {
            if (GetTickCount64() >= deadline)
                throw std::runtime_error("Finish the current capture, then try the update again.");
            Sleep(50);
            continue;
        }
        if (GetLastError() != ERROR_FILE_NOT_FOUND)
            throw std::runtime_error("Cannot check the active capture.");
        Handle editor(OpenMutexW(SYNCHRONIZE, FALSE, EditorMutex));
        if (editor.value)
        {
            if (GetTickCount64() >= deadline)
                throw std::runtime_error("Close the editor window, then try the update again.");
            Sleep(50);
            continue;
        }
        if (GetLastError() != ERROR_FILE_NOT_FOUND)
            throw std::runtime_error("Cannot check the active editor.");
        return;
    }
}
void clean_job(const fs::path& job)
{
    // Delete only our known files. Never recurse into user data or junctions.
    require_plain_path(job);
    for (const auto name : {L"Pluck.next.exe", L"helper.exe", L"request.txt", L"done"})
    {
        const auto path = job / name;
        require_plain_path(path);
        std::error_code ignored;
        if (!fs::is_directory(path, ignored))
            fs::remove(path, ignored);
    }
    std::error_code ignored;
    fs::remove(job, ignored);
}
} // namespace
std::wstring wide(std::string_view text)
{
    if (text.empty())
        return {};
    const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                            static_cast<int>(text.size()), nullptr, 0);
    if (!length)
        throw std::runtime_error("Invalid text encoding.");
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                        result.data(), length);
    return result;
}
std::string utf8(std::wstring_view text)
{
    if (text.empty())
        return {};
    const auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                            static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!length)
        throw std::runtime_error("Invalid text encoding.");
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                        result.data(), length, nullptr, nullptr);
    return result;
}
std::array<unsigned, 3> version(std::string_view text)
{
    std::array<unsigned, 3> result{};
    for (size_t i = 0; i < result.size(); ++i)
    {
        const auto end = text.find('.');
        const auto part = text.substr(0, end);
        if (part.empty() || part.size() > 5 || (part.size() > 1 && part.front() == '0'))
            throw std::runtime_error("Invalid release version.");
        const auto parsed = std::from_chars(part.data(), part.data() + part.size(), result[i]);
        if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size() || result[i] > 65535 ||
            (i < 2 ? end == text.npos : end != text.npos))
            throw std::runtime_error("Invalid release version.");
        text.remove_prefix(i < 2 ? end + 1 : part.size());
    }
    return result;
}
std::string checksum(std::string_view text)
{
    // A raw hash, or the exact checksum-file format published with Pluck.exe.
    if (text.size() < 64)
        throw std::runtime_error("The update checksum is invalid.");
    std::string hash(text.substr(0, 64));
    for (auto& c : hash)
    {
        if (c >= 'A' && c <= 'F')
            c = static_cast<char>(c - 'A' + 'a');
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            throw std::runtime_error("The update checksum is invalid.");
    }
    auto suffix = text.substr(64);
    if (suffix != "" && suffix != "\n" && suffix != "\r\n" && suffix != "  Pluck.exe\n" &&
        suffix != "  Pluck.exe\r\n" && suffix != "  Pluck.exe")
        throw std::runtime_error("The checksum is for a different file.");
    return hash;
}
std::string file_hash(const fs::path& file)
{
    struct Algorithm
    {
        BCRYPT_ALG_HANDLE value{};
        ~Algorithm()
        {
            if (value)
                BCryptCloseAlgorithmProvider(value, 0);
        }
    } algorithm;
    struct Hash
    {
        BCRYPT_HASH_HANDLE value{};
        ~Hash()
        {
            if (value)
                BCryptDestroyHash(value);
        }
    } hash;
    if (BCryptOpenAlgorithmProvider(&algorithm.value, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptCreateHash(algorithm.value, &hash.value, nullptr, 0, nullptr, 0, 0) < 0)
        throw std::runtime_error("Cannot start the checksum check.");
    std::ifstream input(file, std::ios::binary);
    if (!input)
        throw std::runtime_error("Cannot read the update file.");
    std::array<char, 65536> buffer{};
    while (input.read(buffer.data(), buffer.size()) || input.gcount())
        if (BCryptHashData(hash.value, reinterpret_cast<PUCHAR>(buffer.data()),
                           static_cast<ULONG>(input.gcount()), 0) < 0)
            throw std::runtime_error("Cannot check the update file.");
    if (!input.eof())
        throw std::runtime_error("Cannot read the update file.");
    std::array<unsigned char, 32> digest{};
    if (BCryptFinishHash(hash.value, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
        throw std::runtime_error("Cannot finish the checksum check.");
    std::string result;
    for (auto byte : digest)
    {
        result += "0123456789abcdef"[byte >> 4];
        result += "0123456789abcdef"[byte & 15];
    }
    return result;
}
fs::path install_directory()
{
    return known_folder(FOLDERID_LocalAppData) / L"Programs" / L"Pluck";
}
fs::path shortcut_directory()
{
    return known_folder(FOLDERID_Programs) / L"Pluck";
}
bool same_path(const fs::path& a, const fs::path& b)
{
    return _wcsicmp(fs::weakly_canonical(a).c_str(), fs::weakly_canonical(b).c_str()) == 0;
}
bool is_installed()
{
    return same_path(executable_path(), install_directory() / L"Pluck.exe");
}
void require_plain_path(const fs::path& path)
{
    if (!path.is_absolute() || path.lexically_normal() != path)
        throw std::runtime_error("The installation path is invalid.");
    for (auto current = path; !current.empty(); current = current.parent_path())
    {
        const auto attrs = GetFileAttributesW(current.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("The installation path contains a link.");
        if (current == current.root_path())
            break;
    }
}
void verify_executable(const fs::path& file, std::string_view expected_version)
{
    require_plain_path(file);
    const auto expected = version(expected_version);
    if (!fs::is_regular_file(file) || fs::file_size(file) > 50 * 1024 * 1024)
        throw std::runtime_error("The update executable is missing or too large.");
    std::ifstream input(file, std::ios::binary);
    IMAGE_DOS_HEADER dos{};
    input.read(reinterpret_cast<char*>(&dos), sizeof(dos));
    if (!input || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < sizeof(dos) || dos.e_lfanew > 1048576)
        throw std::runtime_error("The update is not a Windows executable.");
    input.seekg(dos.e_lfanew);
    IMAGE_NT_HEADERS64 nt{};
    input.read(reinterpret_cast<char*>(&nt), sizeof(nt));
    if (!input || nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.OptionalHeader.Subsystem != IMAGE_SUBSYSTEM_WINDOWS_GUI ||
        nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR].Size != 0)
        throw std::runtime_error("The update is not a native Windows x64 app.");
    DWORD ignored{};
    const auto size = GetFileVersionInfoSizeW(file.c_str(), &ignored);
    if (!size || size > 1024 * 1024)
        throw std::runtime_error("The update has no version information.");
    std::vector<unsigned char> bytes(size);
    if (!GetFileVersionInfoW(file.c_str(), 0, size, bytes.data()))
        throw std::runtime_error("Cannot read the update version.");
    VS_FIXEDFILEINFO* info{};
    UINT length{};
    if (!VerQueryValueW(bytes.data(), L"\\", reinterpret_cast<void**>(&info), &length) ||
        length < sizeof(*info) || info->dwSignature != 0xfeef04bd ||
        HIWORD(info->dwProductVersionMS) != expected[0] || LOWORD(info->dwProductVersionMS) != expected[1] ||
        HIWORD(info->dwProductVersionLS) != expected[2] || LOWORD(info->dwProductVersionLS) != 0)
        throw std::runtime_error("The update version does not match the release.");
    wchar_t* name{};
    if (!VerQueryValueW(bytes.data(), L"\\StringFileInfo\\040904b0\\ProductName",
                        reinterpret_cast<void**>(&name), &length) ||
        !length || wcscmp(name, L"Pluck") != 0)
        throw std::runtime_error("The update is not Pluck.");
}
bool is_valid_launch_hotkey(WORD hotkey) noexcept
{
    if (hotkey == 0)
        return true;
    const UINT vk = LOBYTE(hotkey);
    const BYTE mods = HIBYTE(hotkey);
    if ((mods & ~(HotkeyShift | HotkeyControl | HotkeyAlt | HotkeyExtended)) != 0)
        return false;
    const int count = modifier_count(mods);
    if (is_function_vk(vk) || is_numpad_vk(vk))
        return count >= 1;
    if (is_alphanumeric_vk(vk))
        return count >= 2;
    return false;
}
HotkeyValidation validate_launch_hotkey(UINT vk, bool ctrl, bool alt, bool shift, bool win, bool extended)
{
    if (win)
        return {false, 0, L"Windows shortcuts cannot use the Win key."};
    if (vk == VK_SNAPSHOT)
        return {false, 0, L"Windows shortcuts cannot use Print Screen."};
    BYTE mods{};
    if (shift)
        mods |= HotkeyShift;
    if (ctrl)
        mods |= HotkeyControl;
    if (alt)
        mods |= HotkeyAlt;
    if (extended && vk == VK_DIVIDE)
        mods |= HotkeyExtended;
    const int count = modifier_count(mods);
    if (is_function_vk(vk) || is_numpad_vk(vk))
    {
        if (count < 1)
            return {false, 0, L"Add Ctrl, Alt, or Shift with that key."};
        return {true, MAKEWORD(static_cast<BYTE>(vk), mods), {}};
    }
    if (is_alphanumeric_vk(vk))
    {
        if (count < 2)
            return {false, 0, L"Use at least two modifiers (such as Ctrl + Alt) with a letter or number."};
        return {true, MAKEWORD(static_cast<BYTE>(vk), mods), {}};
    }
    return {false, 0, L"Use Ctrl, Alt, or Shift with F1-F24, or two modifiers with a letter or number."};
}
std::wstring format_launch_hotkey(WORD hotkey)
{
    if (hotkey == 0)
        return L"None";
    const UINT vk = LOBYTE(hotkey);
    const BYTE mods = HIBYTE(hotkey);
    std::wstring text;
    const auto append = [&text](std::wstring_view part)
    {
        if (!text.empty())
            text += L" + ";
        text += part;
    };
    if (mods & HotkeyControl)
        append(L"Ctrl");
    if (mods & HotkeyAlt)
        append(L"Alt");
    if (mods & HotkeyShift)
        append(L"Shift");
    if (is_function_vk(vk))
        append(L"F" + std::to_wstring(vk - VK_F1 + 1));
    else if (vk >= 'A' && vk <= 'Z')
        append(std::wstring(1, static_cast<wchar_t>(vk)));
    else if (vk >= '0' && vk <= '9')
        append(std::wstring(1, static_cast<wchar_t>(vk)));
    else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        append(L"Num " + std::to_wstring(vk - VK_NUMPAD0));
    else if (vk == VK_MULTIPLY)
        append(L"Num *");
    else if (vk == VK_ADD)
        append(L"Num +");
    else if (vk == VK_SUBTRACT)
        append(L"Num -");
    else if (vk == VK_DECIMAL)
        append(L"Num .");
    else if (vk == VK_DIVIDE)
        append(L"Num /");
    else
    {
        const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        LONG lparam = static_cast<LONG>(scan << 16);
        if (mods & HotkeyExtended)
            lparam |= 1 << 24;
        wchar_t name[64]{};
        if (scan && GetKeyNameTextW(lparam, name, static_cast<int>(std::size(name))) > 0)
            append(name);
        else
            append(L"Key " + std::to_wstring(vk));
    }
    return text;
}
std::optional<WORD> read_shortcut_hotkey(const fs::path& link_path)
{
    require_plain_path(link_path);
    if (!fs::exists(link_path))
        return std::nullopt;
    ComApartment apartment;
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    Microsoft::WRL::ComPtr<IPersistFile> persist;
    WORD hotkey{};
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
        FAILED(link.As(&persist)) || FAILED(persist->Load(link_path.c_str(), STGM_READ)) ||
        FAILED(link->GetHotkey(&hotkey)))
        return std::nullopt;
    return hotkey;
}
std::optional<WORD> read_saved_launch_hotkey()
{
    DWORD value{};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, SettingsKey, LaunchHotkeyValue, RRF_RT_REG_DWORD, nullptr, &value,
                     &size) != ERROR_SUCCESS ||
        value > 0xffff)
        return std::nullopt;
    const auto hotkey = static_cast<WORD>(value);
    if (!is_valid_launch_hotkey(hotkey))
        return std::nullopt;
    return hotkey;
}
void write_saved_launch_hotkey(WORD hotkey)
{
    if (!is_valid_launch_hotkey(hotkey))
        throw std::runtime_error("The launch shortcut is not supported by Windows.");
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SettingsKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
        throw std::runtime_error("Cannot save the launch shortcut setting.");
    const DWORD value = hotkey;
    const bool ok = RegSetValueExW(key, LaunchHotkeyValue, 0, REG_DWORD,
                                   reinterpret_cast<const BYTE*>(&value), sizeof(value)) == ERROR_SUCCESS;
    RegCloseKey(key);
    if (!ok)
        throw std::runtime_error("Cannot save the launch shortcut setting.");
}
WORD effective_launch_hotkey(const fs::path& directory)
{
    if (const auto from_link = read_shortcut_hotkey(directory / L"Pluck.lnk"))
        return *from_link;
    if (const auto saved = read_saved_launch_hotkey())
        return *saved;
    return DefaultLaunchHotkey;
}
void create_shortcuts(const fs::path& root, const fs::path& directory, WORD hotkey)
{
    if (!is_valid_launch_hotkey(hotkey))
        throw std::runtime_error("The launch shortcut is not supported by Windows.");
    require_plain_path(directory);
    fs::create_directories(directory);
    ComApartment apartment;
    for (const bool settings : {false, true})
    {
        const auto path = directory / (settings ? L"Pluck Settings.lnk" : L"Pluck.lnk");
        require_plain_path(path);
        const bool existed = fs::exists(path);
        Microsoft::WRL::ComPtr<IShellLinkW> link;
        Microsoft::WRL::ComPtr<IPersistFile> persist;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
            FAILED(link->SetPath((root / L"Pluck.exe").c_str())) ||
            FAILED(link->SetArguments(settings ? L"--settings" : L"")) ||
            FAILED(link->SetWorkingDirectory(root.c_str())) ||
            FAILED(link->SetIconLocation((root / L"Pluck.exe").c_str(), 0)) ||
            FAILED(link->SetHotkey(settings ? 0 : hotkey)) || FAILED(link.As(&persist)) ||
            FAILED(persist->Save(path.c_str(), TRUE)))
            throw std::runtime_error("Cannot create Start menu shortcuts.");
        notify_shortcut_change(existed ? SHCNE_UPDATEITEM : SHCNE_CREATE, path);
    }
}
void update_launch_hotkey(const fs::path& root, const fs::path& directory, WORD hotkey)
{
    if (!is_valid_launch_hotkey(hotkey))
        throw std::runtime_error("The launch shortcut is not supported by Windows.");
    const auto path = directory / L"Pluck.lnk";
    require_plain_path(directory);
    require_plain_path(path);
    if (!fs::exists(path))
        create_shortcuts(root, directory, hotkey);
    else
    {
        ComApartment apartment;
        Microsoft::WRL::ComPtr<IShellLinkW> link;
        Microsoft::WRL::ComPtr<IPersistFile> persist;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
            FAILED(link.As(&persist)) || FAILED(persist->Load(path.c_str(), STGM_READWRITE)) ||
            FAILED(link->SetHotkey(hotkey)) || FAILED(persist->Save(path.c_str(), TRUE)))
            throw std::runtime_error("Cannot update the Start menu shortcut.");
        notify_shortcut_change(SHCNE_UPDATEITEM, path);
    }
    write_saved_launch_hotkey(hotkey);
}
void replace_executable(const fs::path& source, const fs::path& destination)
{
    require_plain_path(source);
    require_plain_path(destination);
    const auto staged = destination.parent_path() / L"Pluck.pending.exe";
    const auto backup = destination.parent_path() / L"Pluck.previous.exe";
    require_plain_path(staged);
    require_plain_path(backup);
    // Do not overwrite an unknown file or a backup from an interrupted transaction.
    if (fs::exists(staged) || fs::exists(backup))
        throw std::runtime_error(
            "A previous installation is incomplete. Keep Pluck.previous.exe and try again after recovery.");
    try
    {
        fs::copy_file(source, staged);
        if (file_hash(staged) != file_hash(source))
            throw std::runtime_error("The staged update did not pass its checksum check.");
    }
    catch (...)
    {
        std::error_code ignored;
        fs::remove(staged, ignored);
        throw;
    }
    const bool existed = fs::exists(destination);
    const bool ok =
        existed ? ReplaceFileW(destination.c_str(), staged.c_str(), backup.c_str(), 0, nullptr, nullptr)
                : MoveFileExW(staged.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH);
    if (!ok)
    {
        std::error_code ignored;
        if (existed && !fs::exists(destination) && fs::exists(backup))
            fs::rename(backup, destination, ignored);
        fs::remove(staged, ignored);
        throw std::runtime_error("Cannot replace Pluck.exe. Close all Pluck windows and try again.");
    }
    std::error_code ignored;
    fs::remove(backup, ignored);
}
void prepare_install(const Progress& progress, std::stop_token stop)
{
    Handle operation(CreateMutexW(nullptr, FALSE, OperationMutex));
    check_operation(operation.value);
    const auto root = install_directory();
    require_plain_path(root);
    if (fs::exists(root / L"Pluck.exe"))
        throw std::runtime_error("Pluck is already installed. Open Pluck Settings from Start to update it.");
    const auto job = make_job();
    try
    {
        progress(L"Preparing installation...");
        const auto staged = job / L"Pluck.next.exe";
        fs::copy_file(executable_path(), staged);
        verify_executable(staged, PLUCK_VERSION);
        write_request(job, "install", PLUCK_VERSION, file_hash(staged));
        cancelled(stop);
        start_helper(job);
    }
    catch (...)
    {
        clean_job(job);
        throw;
    }
}
// The network implementation writes only to a newly created job.
void download_release(const Release&, const fs::path&, const Progress&, std::stop_token);
void prepare_update(const Release& release, const Progress& progress, std::stop_token stop)
{
    if (!is_installed() || version(release.version) <= version(PLUCK_VERSION))
        throw std::runtime_error("Open the installed copy to update to a newer release.");
    Handle operation(CreateMutexW(nullptr, FALSE, OperationMutex));
    check_operation(operation.value);
    const auto job = make_job();
    try
    {
        download_release(release, job / L"Pluck.next.exe", progress, stop);
        verify_executable(job / L"Pluck.next.exe", release.version);
        write_request(job, "update", release.version, file_hash(job / L"Pluck.next.exe"));
        cancelled(stop);
        start_helper(job);
    }
    catch (...)
    {
        clean_job(job);
        throw;
    }
}
void prepare_uninstall()
{
    if (!is_installed())
        throw std::runtime_error("Open the installed copy to uninstall Pluck.");
    Handle operation(CreateMutexW(nullptr, FALSE, OperationMutex));
    check_operation(operation.value);
    const auto job = make_job();
    try
    {
        write_request(job, "uninstall", PLUCK_VERSION, "-");
        start_helper(job);
    }
    catch (...)
    {
        clean_job(job);
        throw;
    }
}
void cleanup_jobs()
{
    const auto root = jobs_directory();
    require_plain_path(root);
    if (!fs::exists(root))
        return;
    for (const auto& entry : fs::directory_iterator(root))
    {
        const auto job = entry.path();
        if (!job.filename().wstring().starts_with(L"job-") || entry.is_symlink())
            continue;
        require_plain_path(job);
        if (!fs::exists(job / L"done"))
            continue;
        // A running helper is locked against deletion. Retry on a later settings launch.
        Handle probe(
            CreateFileW((job / L"helper.exe").c_str(), DELETE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
        if (probe.value == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_NOT_FOUND)
            continue;
        if (probe.value != INVALID_HANDLE_VALUE)
        {
            CloseHandle(probe.value);
            probe.value = nullptr;
        }
        clean_job(job);
    }
}
int finish_job(const fs::path& job, DWORD parent)
{
    require_plain_path(job);
    if (!same_path(job.parent_path(), jobs_directory()) || !job.filename().wstring().starts_with(L"job-") ||
        !same_path(executable_path(), job / L"helper.exe"))
        throw std::runtime_error("Invalid update helper location.");
    require_plain_path(job / L"request.txt");
    if (fs::file_size(job / L"request.txt") > 256)
        throw std::runtime_error("Invalid update request.");
    std::ifstream request(job / L"request.txt", std::ios::binary);
    std::string action, target, hash, extra;
    if (!std::getline(request, action) || !std::getline(request, target) || !std::getline(request, hash) ||
        std::getline(request, extra) || (action != "install" && action != "update" && action != "uninstall"))
        throw std::runtime_error("Invalid update request.");
    version(target);
    if ((action == "update" && version(target) <= version(PLUCK_VERSION)) ||
        (action != "update" && target != PLUCK_VERSION))
        throw std::runtime_error("The requested version is not valid for this operation.");
    const auto root = install_directory();
    require_plain_path(root);
    const auto staged = job / L"Pluck.next.exe";
    if (action != "uninstall")
    {
        if (file_hash(staged) != checksum(hash))
            throw std::runtime_error("The update file changed after download.");
        verify_executable(staged, target);
    }
    Handle maintenance(CreateMutexW(nullptr, FALSE, MaintenanceMutex));
    check_operation(maintenance.value);
    wait_for_parent(parent);
    wait_for_capture();
    const auto executable = root / L"Pluck.exe";
    require_plain_path(executable);
    if (action == "uninstall")
    {
        verify_executable(executable, target);
        fs::remove(executable);
        const auto shortcuts = shortcut_directory();
        clear_shortcut_hotkey(shortcuts / L"Pluck.lnk");
        for (const auto name : {L"Pluck.lnk", L"Pluck Settings.lnk"})
        {
            const auto link = shortcuts / name;
            require_plain_path(link);
            if (fs::remove(link))
                notify_shortcut_change(SHCNE_DELETE, link);
        }
        std::error_code ignored;
        fs::remove(shortcuts, ignored);
        fs::remove(root, ignored);
        delete_saved_launch_hotkey();
        const auto removed = RegDeleteTreeW(HKEY_CURRENT_USER, UninstallKey);
        if (removed != ERROR_SUCCESS && removed != ERROR_FILE_NOT_FOUND)
            throw std::runtime_error(
                "Pluck was removed, but its Windows installation entry could not be removed.");
    }
    else
    {
        if (action == "install" && fs::exists(executable))
            throw std::runtime_error("Pluck is already installed.");
        if (action == "update")
            verify_executable(executable, PLUCK_VERSION);
        const auto shortcuts = shortcut_directory();
        const WORD hotkey =
            action == "update" ? effective_launch_hotkey(shortcuts) : DefaultLaunchHotkey;
        fs::create_directories(root);
        replace_executable(staged, executable);
        create_shortcuts(root, shortcuts, hotkey);
        write_saved_launch_hotkey(hotkey);
        register_installation(root, target);
    }
    std::ofstream(job / L"done") << "complete\n";
    // Release the maintenance gate before the new settings process starts.
    CloseHandle(maintenance.value);
    maintenance.value = nullptr;
    if (action != "uninstall")
        launch_process(executable.wstring(), L"--settings");
    return 0;
}
} // namespace pluck::management
