#include "management.h"
#include <winhttp.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>

namespace pluck::management
{
namespace
{
struct Internet
{
    HINTERNET value{};
    ~Internet()
    {
        if (value)
            WinHttpCloseHandle(value);
    }
};
void request(std::string_view address, std::ostream& output, uint64_t limit, std::stop_token stop,
             const Progress& progress = {})
{
    Internet session{WinHttpOpen(L"Pluck-Updater/"
                                 L"1",
                                 WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.value || !WinHttpSetTimeouts(session.value, 5000, 5000, 5000, 5000))
        throw std::runtime_error("Cannot start the update connection.");
    auto url = wide(address);
    const auto deadline = GetTickCount64() + 120000;
    for (unsigned redirects = 0; redirects < 6; ++redirects)
    {
        if (stop.stop_requested())
            throw std::runtime_error("Operation cancelled.");
        URL_COMPONENTS parts{sizeof(parts)};
        parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = DWORD(-1);
        parts.dwUserNameLength = parts.dwPasswordLength = DWORD(-1);
        if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts) ||
            parts.nScheme != INTERNET_SCHEME_HTTPS || parts.nPort != 443 || parts.dwUserNameLength ||
            parts.dwPasswordLength)
            throw std::runtime_error("The update address is not valid HTTPS.");
        std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        if (host != L"api.github.com" && host != L"github.com" &&
            host != L"release-assets.githubusercontent.com" && host != L"objects.githubusercontent.com")
            throw std::runtime_error("The update address is not a GitHub download.");
        std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
        if (parts.dwExtraInfoLength)
            path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        Internet connection{WinHttpConnect(session.value, host.c_str(), parts.nPort, 0)};
        Internet call{connection.value ? WinHttpOpenRequest(connection.value, L"GET", path.c_str(), nullptr,
                                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                            WINHTTP_FLAG_SECURE)
                                       : nullptr};
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        if (!call.value ||
            !WinHttpSetOption(call.value, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy)))
            throw std::runtime_error("Cannot open the update request.");
        constexpr wchar_t headers[] =
            L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
        if (!WinHttpSendRequest(call.value, headers, DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(call.value, nullptr))
            throw std::runtime_error("Cannot connect to GitHub. Check your connection and try again.");
        DWORD status{}, size = sizeof(status);
        if (!WinHttpQueryHeaders(call.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
            throw std::runtime_error("Cannot read the update response.");
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)
        {
            DWORD bytes{};
            WinHttpQueryHeaders(call.value, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, nullptr,
                                &bytes, WINHTTP_NO_HEADER_INDEX);
            if (!bytes || bytes > 32768)
                throw std::runtime_error("Invalid update redirect.");
            std::wstring next(bytes / sizeof(wchar_t), L'\0');
            if (!WinHttpQueryHeaders(call.value, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                     next.data(), &bytes, WINHTTP_NO_HEADER_INDEX))
                throw std::runtime_error("Cannot read the update redirect.");
            next.resize(wcslen(next.c_str()));
            url = std::move(next);
            continue;
        }
        if (status != 200)
            throw std::runtime_error(status == 403 || status == 429
                                         ? "GitHub is limiting update checks. Try again later."
                                         : "GitHub request failed (HTTP " + std::to_string(status) + ").");
        uint64_t total{};
        std::array<char, 65536> buffer{};
        for (;;)
        {
            if (stop.stop_requested())
                throw std::runtime_error("Operation cancelled.");
            if (GetTickCount64() > deadline)
                throw std::runtime_error("The update download timed out.");
            DWORD count{};
            if (!WinHttpReadData(call.value, buffer.data(), static_cast<DWORD>(buffer.size()), &count))
                throw std::runtime_error("The update download was interrupted. Try again.");
            if (!count)
                break;
            if (count > limit - total)
                throw std::runtime_error("The update download is larger than expected.");
            total += count;
            output.write(buffer.data(), count);
            if (!output)
                throw std::runtime_error("Cannot save the update download.");
            if (progress)
                progress(L"Downloading update... " + std::to_wstring(total * 100 / limit) + L"%");
        }
        return;
    }
    throw std::runtime_error("Too many update redirects.");
}
} // namespace
Release parse_release(std::string_view json)
{
    try
    {
        using winrt::Windows::Data::Json::JsonObject;
        const auto object = JsonObject::Parse(wide(json));
        if (object.GetNamedBoolean(L"draft") || object.GetNamedBoolean(L"prerelease"))
            throw std::runtime_error("The release is not a stable release.");
        const auto tag = utf8(object.GetNamedString(L"tag_name"));
        if (!tag.starts_with('v'))
            throw std::runtime_error("Invalid release tag.");
        Release result;
        result.version = tag.substr(1);
        version(result.version);
        const auto base = "https://github.com/tuncb/pluck/releases/download/" + tag + "/";
        for (const auto& value : object.GetNamedArray(L"assets"))
        {
            const auto asset = value.GetObject();
            const auto name = utf8(asset.GetNamedString(L"name"));
            if (name != "Pluck.exe" && name != "Pluck.exe.sha256")
                continue;
            const auto url = utf8(asset.GetNamedString(L"browser_download_url"));
            const auto bytes = asset.GetNamedNumber(L"size");
            if (url != base + name || !std::isfinite(bytes) || std::trunc(bytes) != bytes || bytes <= 0 ||
                bytes > (name == "Pluck.exe" ? 50 * 1024 * 1024 : 4096))
                throw std::runtime_error("Invalid release download metadata.");
            auto& destination = name == "Pluck.exe" ? result.url : result.checksum_url;
            if (!destination.empty())
                throw std::runtime_error("The release contains duplicate download files.");
            destination = url;
            if (name == "Pluck.exe")
                result.size = static_cast<uint64_t>(bytes);
        }
        return result;
    }
    catch (const winrt::hresult_error&)
    {
        throw std::runtime_error("Cannot read the GitHub release information.");
    }
}
Release check_release(std::stop_token stop)
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    struct Apartment
    {
        ~Apartment()
        {
            winrt::uninit_apartment();
        }
    } apartment;
    std::ostringstream response;
    request("https://api.github.com/repos/tuncb/pluck/releases/latest", response, 2 * 1024 * 1024, stop);
    return parse_release(response.str());
}
void download_release(const Release& release, const fs::path& destination, const Progress& progress,
                      std::stop_token stop)
{
    if (release.url.empty() || release.checksum_url.empty() || !release.size)
        throw std::runtime_error("This release requires a manual download from GitHub.");
    std::ostringstream hash_file;
    request(release.checksum_url, hash_file, 4096, stop);
    const auto expected = checksum(hash_file.str());
    std::ofstream output(destination, std::ios::binary);
    if (!output)
        throw std::runtime_error("Cannot save the update file.");
    request(release.url, output, release.size, stop, progress);
    output.close();
    if (!output || fs::file_size(destination) != release.size || file_hash(destination) != expected)
        throw std::runtime_error("The update did not pass its SHA-256 check. Try again.");
}
} // namespace pluck::management
