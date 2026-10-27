#include "settings.h"
#include "management.h"
#include "app_process.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <algorithm>
#include <memory>
#include <thread>
#include <vector>

namespace pluck
{
namespace
{
constexpr UINT ProgressMessage = WM_APP + 20, ResultMessage = WM_APP + 21;
enum : int
{
    Install = 101,
    Check,
    Update,
    Uninstall,
    Shortcuts,
    Repository,
    Close
};
struct WorkResult
{
    management::Release release;
    std::wstring error;
    bool helper{};
};
class Settings
{
  public:
    HINSTANCE instance{};
    HWND window{}, subtitle{}, install_status{}, location{}, update_status{}, footer{};
    HWND install{}, check{}, update{}, uninstall{}, shortcuts{}, repository{}, close{};
    HFONT normal{}, title{}, heading{};
    HICON brand_icon{};
    HBRUSH background{};
    COLORREF canvas{}, ink{}, muted{}, line{}, accent{}, accent_ink{}, button{};
    UINT dpi{96};
    bool installed{}, busy{}, close_requested{};
    management::Release release;
    std::jthread worker;
    std::vector<HWND> labels;

    ~Settings()
    {
        if (worker.joinable())
        {
            worker.request_stop();
            worker.join();
        }
        for (auto font : {normal, title, heading})
            if (font)
                DeleteObject(font);
        if (background)
            DeleteObject(background);
        if (brand_icon)
            DestroyIcon(brand_icon);
    }
    int px(int value) const
    {
        return MulDiv(value, static_cast<int>(dpi), 96);
    }
    int py(int value) const
    {
        RECT client{};
        GetClientRect(window, &client);
        return MulDiv(value, std::min(client.bottom, static_cast<LONG>(px(570))), 570);
    }
    void theme()
    {
        DWORD light = 1, size = sizeof(light);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
        HIGHCONTRASTW contrast{sizeof(contrast)};
        SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
        const bool high = (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
        canvas = high ? GetSysColor(COLOR_WINDOW) : light ? RGB(249, 252, 251) : RGB(12, 18, 17);
        ink = high ? GetSysColor(COLOR_WINDOWTEXT) : light ? RGB(25, 42, 37) : RGB(237, 248, 244);
        muted = high ? ink : light ? RGB(80, 101, 94) : RGB(158, 180, 172);
        line = high ? ink : light ? RGB(215, 229, 222) : RGB(42, 59, 52);
        accent = high ? GetSysColor(COLOR_HIGHLIGHT) : RGB(119, 244, 213);
        accent_ink = high ? GetSysColor(COLOR_HIGHLIGHTTEXT) : RGB(8, 42, 36);
        button = high ? canvas : light ? RGB(232, 242, 237) : RGB(28, 44, 37);
        if (background)
            DeleteObject(background);
        background = CreateSolidBrush(canvas);
        const BOOL dark = !light && !high;
        DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
        InvalidateRect(window, nullptr, TRUE);
        for (auto child : labels)
            InvalidateRect(child, nullptr, TRUE);
        for (auto child : {install, check, update, uninstall, shortcuts, repository, close})
            if (child)
                InvalidateRect(child, nullptr, TRUE);
    }
    void fonts()
    {
        for (auto font : {normal, title, heading})
            if (font)
                DeleteObject(font);
        const auto make = [this](int height, int weight)
        {
            return CreateFontW(-px(height), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                               L"Segoe UI");
        };
        normal = make(14, FW_NORMAL);
        title = make(30, FW_SEMIBOLD);
        heading = make(19, FW_SEMIBOLD);
        if (brand_icon)
            DestroyIcon(brand_icon);
        brand_icon = static_cast<HICON>(
            LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, px(48), px(48), LR_DEFAULTCOLOR));
        for (auto child : labels)
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(normal), TRUE);
        for (auto child : {install, check, update, uninstall, shortcuts, repository, close})
            if (child)
                SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(normal), TRUE);
    }
    HWND label(const wchar_t* text, DWORD style = 0)
    {
        const auto child = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_NOPREFIX | style, 0,
                                           0, 0, 0, window, nullptr, instance, nullptr);
        if (!child)
            throw std::runtime_error("Cannot create settings text.");
        labels.push_back(child);
        return child;
    }
    HWND control(const wchar_t* text, int id)
    {
        const auto child =
            CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0,
                            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        if (!child)
            throw std::runtime_error("Cannot create a settings button.");
        return child;
    }
    void create()
    {
        installed = management::is_installed();
        dpi = GetDpiForWindow(window);
        subtitle = label(
            (L"Version " + management::wide(PLUCK_VERSION) + (installed ? L" · Installed" : L" · Portable"))
                .c_str());
        install_status = label(installed ? L"Installed for this user.\nCapture and settings are in Start."
                                         : L"Install for this Windows user.\nAdd two shortcuts to Start.");
        location = label(management::install_directory().c_str(), SS_PATHELLIPSIS);
        update_status = label(L"Check for a newer stable release.");
        footer = label(L"Open Pluck to capture. Press F2 during selection to return here.");
        install = control(L"Install Pluck", Install);
        check = control(L"Check for updates", Check);
        update = control(L"Update", Update);
        shortcuts = control(L"Restore shortcuts", Shortcuts);
        uninstall = control(L"Uninstall", Uninstall);
        repository = control(L"GitHub repository", Repository);
        close = control(L"Close", Close);
        fonts();
        theme();
        layout();
        buttons();
        try
        {
            management::cleanup_jobs();
        }
        catch (...)
        { /* Cleanup can retry at the next settings launch. */
        }
    }
    void layout()
    {
        RECT client{};
        GetClientRect(window, &client);
        const int width = MulDiv(client.right, 96, static_cast<int>(dpi));
        const int left = 32, right = width - 32;
        const auto move = [&](HWND child, int x, int y, int w, int h)
        { MoveWindow(child, px(x), py(y), px(w), py(h), TRUE); };
        move(subtitle, 96, 72, right - 96, 24);
        move(install_status, left, 163, right - left - 174, 54);
        move(location, left, 226, right - left, 24);
        move(install, right - 154, 166, 154, 38);
        move(shortcuts, right - 154, 155, 154, 38);
        move(uninstall, right - 154, 201, 154, 34);
        move(update_status, left, 307, right - left, 64);
        move(check, left, 378, 174, 38);
        move(update, right - 120, 378, 120, 38);
        move(footer, left, 448, right - left, 42);
        move(repository, left, 507, 174, 36);
        move(close, right - 100, 507, 100, 36);
    }
    void buttons()
    {
        ShowWindow(install, installed ? SW_HIDE : SW_SHOW);
        ShowWindow(shortcuts, installed ? SW_SHOW : SW_HIDE);
        ShowWindow(uninstall, installed ? SW_SHOW : SW_HIDE);
        const bool newer = !release.version.empty() &&
                           management::version(release.version) > management::version(PLUCK_VERSION);
        ShowWindow(update, installed && newer && !release.url.empty() && !release.checksum_url.empty()
                               ? SW_SHOW
                               : SW_HIDE);
        for (auto child : {install, check, update, uninstall, shortcuts, repository})
            EnableWindow(child, !busy);
        SetWindowTextW(close, busy ? L"Cancel" : L"Close");
    }
    void text(HDC dc, const wchar_t* value, RECT rect, HFONT font, COLORREF color)
    {
        const auto previous = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, color);
        DrawTextW(dc, value, -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, previous);
    }
    void paint()
    {
        PAINTSTRUCT ps{};
        const auto dc = BeginPaint(window, &ps);
        RECT client{};
        GetClientRect(window, &client);
        FillRect(dc, &ps.rcPaint, background);
        DrawIconEx(dc, px(32), py(33), brand_icon, px(48), px(48), 0, nullptr, DI_NORMAL);
        text(dc, L"Pluck", {px(96), py(27), client.right - px(32), py(70)}, title, ink);
        text(dc, L"Installation", {px(32), py(125), client.right - px(32), py(156)}, heading, ink);
        text(dc, L"Updates", {px(32), py(270), client.right - px(32), py(301)}, heading, ink);
        const auto pen = CreatePen(PS_SOLID, 1, line);
        const auto previous = SelectObject(dc, pen);
        for (const auto y : {111, 258, 433})
        {
            MoveToEx(dc, px(32), py(y), nullptr);
            LineTo(dc, client.right - px(32), py(y));
        }
        SelectObject(dc, previous);
        DeleteObject(pen);
        EndPaint(window, &ps);
    }
    void draw_button(const DRAWITEMSTRUCT& item)
    {
        FillRect(item.hDC, &item.rcItem, background);
        const bool primary = item.CtlID == Install || item.CtlID == Update;
        const bool disabled = (item.itemState & ODS_DISABLED) != 0;
        const auto fill = CreateSolidBrush(primary && !disabled ? accent : button);
        const auto pen = CreatePen(PS_SOLID, 1, primary && !disabled ? accent : line);
        const auto old_brush = SelectObject(item.hDC, fill), old_pen = SelectObject(item.hDC, pen);
        RoundRect(item.hDC, item.rcItem.left, item.rcItem.top, item.rcItem.right, item.rcItem.bottom, px(8),
                  px(8));
        SelectObject(item.hDC, old_brush);
        SelectObject(item.hDC, old_pen);
        DeleteObject(fill);
        DeleteObject(pen);
        wchar_t label_text[128]{};
        GetWindowTextW(item.hwndItem, label_text, 128);
        const auto old_font = SelectObject(item.hDC, normal);
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, disabled ? muted : primary ? accent_ink : ink);
        RECT text_rect = item.rcItem;
        if (item.itemState & ODS_SELECTED)
            OffsetRect(&text_rect, 0, px(1));
        DrawTextW(item.hDC, label_text, -1, &text_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(item.hDC, old_font);
        if ((item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT))
        {
            RECT focus = item.rcItem;
            InflateRect(&focus, -px(4), -px(4));
            DrawFocusRect(item.hDC, &focus);
        }
    }
    void start(int action)
    {
        if (busy)
            return;
        if (worker.joinable())
            worker.join();
        busy = true;
        buttons();
        SetWindowTextW(update_status, action == Check ? L"Checking for updates..." : L"Preparing...");
        const auto selected = release;
        worker = std::jthread(
            [this, action, selected](std::stop_token stop)
            {
                auto result = std::make_unique<WorkResult>();
                const auto progress = [this](std::wstring value)
                {
                    auto message = std::make_unique<std::wstring>(std::move(value));
                    if (PostMessageW(window, ProgressMessage, 0, reinterpret_cast<LPARAM>(message.get())))
                        message.release();
                };
                try
                {
                    if (action == Check)
                        result->release = management::check_release(stop);
                    else if (action == Install)
                    {
                        management::prepare_install(progress, stop);
                        result->helper = true;
                    }
                    else if (action == Update)
                    {
                        management::prepare_update(selected, progress, stop);
                        result->helper = true;
                    }
                    else if (action == Uninstall)
                    {
                        management::prepare_uninstall();
                        result->helper = true;
                    }
                }
                catch (const std::exception& error)
                {
                    result->error = management::wide(error.what());
                }
                catch (...)
                {
                    result->error = L"The operation failed. Try again.";
                }
                if (PostMessageW(window, ResultMessage, 0, reinterpret_cast<LPARAM>(result.get())))
                    result.release();
            });
    }
    void command(int id)
    {
        if (id == Close || id == IDCANCEL)
        {
            SendMessageW(window, WM_CLOSE, 0, 0);
            return;
        }
        if (busy)
            return;
        if (id == Install || id == Check || id == Update)
            start(id);
        else if (id == Uninstall && installed &&
                 MessageBoxW(window, L"Remove the installed Pluck app and its Start menu shortcuts?",
                             L"Uninstall Pluck", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES)
            start(Uninstall);
        else if (id == Shortcuts)
        {
            management::create_shortcuts(management::install_directory(), management::shortcut_directory());
            SetWindowTextW(install_status, L"Start menu shortcuts were restored.");
        }
        else if (id == Repository)
        {
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open", L"https://github.com/tuncb/pluck",
                                                        nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
                throw std::runtime_error("Cannot open the GitHub repository.");
        }
    }
    LRESULT handle(UINT message, WPARAM w, LPARAM l)
    {
        switch (message)
        {
        case WM_CREATE:
            create();
            return 0;
        case WM_PAINT:
            paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLORSTATIC:
            SetTextColor(reinterpret_cast<HDC>(w), muted);
            SetBkColor(reinterpret_cast<HDC>(w), canvas);
            return reinterpret_cast<LRESULT>(background);
        case WM_DRAWITEM:
            draw_button(*reinterpret_cast<DRAWITEMSTRUCT*>(l));
            return TRUE;
        case WM_COMMAND:
            command(LOWORD(w));
            return 0;
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            theme();
            return 0;
        case WM_DPICHANGED:
        {
            dpi = HIWORD(w);
            fonts();
            const auto rect = reinterpret_cast<RECT*>(l);
            SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left,
                         rect->bottom - rect->top, SWP_NOZORDER);
            layout();
            InvalidateRect(window, nullptr, TRUE);
            return 0;
        }
        case ProgressMessage:
        {
            std::unique_ptr<std::wstring> value(reinterpret_cast<std::wstring*>(l));
            if (!close_requested)
                SetWindowTextW(update_status, value->c_str());
            return 0;
        }
        case ResultMessage:
        {
            std::unique_ptr<WorkResult> result(reinterpret_cast<WorkResult*>(l));
            if (worker.joinable())
                worker.join();
            busy = false;
            if (result->helper || close_requested)
            {
                DestroyWindow(window);
                return 0;
            }
            if (!result->error.empty())
                SetWindowTextW(update_status, result->error.c_str());
            else
            {
                release = std::move(result->release);
                std::wstring message_text = L"Pluck is up to date.";
                if (management::version(release.version) > management::version(PLUCK_VERSION))
                {
                    message_text = L"Pluck " + management::wide(release.version) + L" is available.";
                    if (!installed)
                        message_text += L" Install this copy to enable updates.";
                    else if (release.url.empty() || release.checksum_url.empty())
                        message_text += L" Download this release from GitHub.";
                }
                SetWindowTextW(update_status, message_text.c_str());
            }
            buttons();
            return 0;
        }
        case WM_CLOSE:
            if (busy)
            {
                close_requested = true;
                worker.request_stop();
                SetWindowTextW(update_status, L"Closing...");
            }
            else
                DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, w, l);
    }
    static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM w, LPARAM l) noexcept
    {
        auto* self = reinterpret_cast<Settings*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<Settings*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            self->window = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self)
            return DefWindowProcW(hwnd, message, w, l);
        try
        {
            return self->handle(message, w, l);
        }
        catch (const std::exception& error)
        {
            MessageBoxA(hwnd, error.what(), "Pluck Settings", MB_OK | MB_ICONERROR);
            if (message == WM_CREATE)
                return -1;
            return 0;
        }
    }
};
} // namespace
int run_settings(HINSTANCE instance, bool uninstall_requested)
{
    if (management::Handle maintenance{OpenMutexW(SYNCHRONIZE, FALSE, MaintenanceMutex)}; maintenance.value)
        return 0;
    management::Handle mutex(CreateMutexW(nullptr, FALSE, L"Local\\Pluck.Settings"));
    if (!mutex.value)
        throw std::runtime_error("Cannot open Settings.");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        for (unsigned retry = 0; retry < 40; ++retry)
        {
            if (const auto window = FindWindowW(SettingsClass, nullptr))
            {
                ShowWindow(window, SW_RESTORE);
                SetForegroundWindow(window);
                if (uninstall_requested)
                    PostMessageW(window, WM_COMMAND, Uninstall, 0);
                return 0;
            }
            Sleep(50);
        }
        throw std::runtime_error("Settings is still opening. Try again.");
    }
    if (management::Handle maintenance{OpenMutexW(SYNCHRONIZE, FALSE, MaintenanceMutex)}; maintenance.value)
        return 0;
    Settings settings;
    settings.instance = instance;
    WNDCLASSW wc{};
    wc.hInstance = instance;
    wc.lpfnWndProc = Settings::procedure;
    wc.lpszClassName = SettingsClass;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&wc))
        throw std::runtime_error("Cannot prepare Settings.");
    POINT pointer{};
    GetCursorPos(&pointer);
    MONITORINFO monitor{sizeof(monitor)};
    const auto display = MonitorFromPoint(pointer, MONITOR_DEFAULTTOPRIMARY);
    GetMonitorInfoW(display, &monitor);
    UINT dpi = GetDpiForSystem(), dpi_y{};
    GetDpiForMonitor(display, MDT_EFFECTIVE_DPI, &dpi, &dpi_y);
    RECT rect{0, 0, MulDiv(600, static_cast<int>(dpi), 96), MulDiv(570, static_cast<int>(dpi), 96)};
    constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectExForDpi(&rect, style, FALSE, 0, dpi);
    const auto width = std::min(rect.right - rect.left, monitor.rcWork.right - monitor.rcWork.left);
    const auto height = std::min(rect.bottom - rect.top, monitor.rcWork.bottom - monitor.rcWork.top);
    const auto window =
        CreateWindowExW(0, SettingsClass, L"Pluck Settings", style,
                        monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2,
                        monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2, width,
                        height, nullptr, nullptr, instance, &settings);
    if (!window)
        throw std::runtime_error("Cannot open Settings.");
    // Creation can choose a display with a different scale from the system display.
    rect = {0, 0, settings.px(600), settings.px(570)};
    AdjustWindowRectExForDpi(&rect, style, FALSE, 0, settings.dpi);
    const auto final_width = std::min(rect.right - rect.left, monitor.rcWork.right - monitor.rcWork.left);
    const auto final_height = std::min(rect.bottom - rect.top, monitor.rcWork.bottom - monitor.rcWork.top);
    SetWindowPos(window, nullptr,
                 monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - final_width) / 2,
                 monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - final_height) / 2,
                 final_width, final_height, SWP_NOZORDER);
    settings.layout();
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    SetFocus(settings.close);
    UpdateWindow(window);
    if (uninstall_requested)
        PostMessageW(window, WM_COMMAND, Uninstall, 0);
    MSG message{};
    BOOL result{};
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0)
        if (!IsDialogMessageW(window, &message))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    return result == -1 ? 1 : 0;
}
int management_command(HINSTANCE instance)
{
    int count{};
    auto** args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args)
        throw std::runtime_error("Cannot read the command.");
    struct Arguments
    {
        wchar_t** value;
        ~Arguments()
        {
            LocalFree(value);
        }
    } arguments{args};
    if (count == 2 && (wcscmp(args[1], L"--settings") == 0 || wcscmp(args[1], L"--install") == 0))
        return run_settings(instance);
    if (count == 2 && wcscmp(args[1], L"--uninstall") == 0)
        return run_settings(instance, true);
    if (count == 4 && wcscmp(args[1], L"--finish-job") == 0)
    {
        wchar_t* end{};
        const auto parent = wcstoul(args[3], &end, 10);
        if (!parent || !end || *end)
            throw std::runtime_error("Invalid update process.");
        return management::finish_job(args[2], parent);
    }
    throw std::runtime_error("Use Pluck.exe to capture, or Pluck.exe --settings to open Settings.");
}
} // namespace pluck
