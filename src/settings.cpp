#include "settings.h"
#include "management.h"
#include "app_process.h"
#include "theme.h"
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
    ShortcutBox,
    ShortcutReset,
    ShortcutClear,
    Repository,
    Close
};
struct WorkResult
{
    management::Release release;
    std::wstring error;
    bool helper{};
};
bool is_modifier_vk(UINT vk) noexcept
{
    return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_SHIFT || vk == VK_LSHIFT ||
           vk == VK_RSHIFT || vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN ||
           vk == VK_RWIN;
}
std::vector<std::wstring> split_hotkey_tokens(const std::wstring& formatted)
{
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (start < formatted.size())
    {
        const auto pos = formatted.find(L" + ", start);
        if (pos == std::wstring::npos)
        {
            parts.push_back(formatted.substr(start));
            break;
        }
        parts.push_back(formatted.substr(start, pos - start));
        start = pos + 3;
    }
    return parts;
}
class Settings
{
  public:
    static constexpr int BaseWidth = 600;
    static constexpr int BaseHeight = 700;
    HINSTANCE instance{};
    HWND window{}, subtitle{}, install_status{}, location{}, shortcut_desc{}, shortcut_status{},
        update_status{}, footer{};
    HWND install{}, check{}, update{}, uninstall{}, shortcuts{}, shortcut_box{}, shortcut_reset{},
        shortcut_clear{}, repository{}, close{};
    HFONT normal{}, small_font{}, key_font{}, title{}, heading{};
    HICON brand_icon{};
    HBRUSH background{};
    ThemePalette palette{};
    UINT dpi{96};
    bool installed{}, busy{}, close_requested{}, recording_shortcut{}, shortcut_error{};
    WORD launch_hotkey{management::DefaultLaunchHotkey};
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
        for (auto font : {normal, small_font, key_font, title, heading})
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
        return MulDiv(value, std::min(client.bottom, static_cast<LONG>(px(BaseHeight))), BaseHeight);
    }
    void theme()
    {
        palette = read_theme();
        if (background)
            DeleteObject(background);
        background = CreateSolidBrush(palette.canvas);
        apply_window_theme(window, palette);
        InvalidateRect(window, nullptr, TRUE);
        for (auto child : labels)
            InvalidateRect(child, nullptr, TRUE);
        for (auto child : {install, check, update, uninstall, shortcuts, shortcut_box, shortcut_reset,
                           shortcut_clear, repository, close})
            if (child)
                InvalidateRect(child, nullptr, TRUE);
    }
    void fonts()
    {
        for (auto font : {normal, small_font, key_font, title, heading})
            if (font)
                DeleteObject(font);
        const auto make = [this](int height, int weight)
        {
            return CreateFontW(-px(height), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                               L"Segoe UI");
        };
        normal = make(14, FW_NORMAL);
        small_font = make(12, FW_NORMAL);
        key_font = make(12, FW_SEMIBOLD);
        title = make(30, FW_SEMIBOLD);
        heading = make(19, FW_SEMIBOLD);
        if (brand_icon)
            DestroyIcon(brand_icon);
        brand_icon = static_cast<HICON>(
            LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, px(48), px(48), LR_DEFAULTCOLOR));
        for (auto child : labels)
            SendMessageW(child, WM_SETFONT,
                         reinterpret_cast<WPARAM>(child == shortcut_status ? small_font : normal), TRUE);
        for (auto child : {install, check, update, uninstall, shortcuts, shortcut_box, shortcut_reset,
                           shortcut_clear, repository, close})
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
    void set_shortcut_status(const std::wstring& message, bool is_error)
    {
        shortcut_error = is_error;
        SetWindowTextW(shortcut_status, message.c_str());
        InvalidateRect(shortcut_status, nullptr, TRUE);
        InvalidateRect(shortcut_box, nullptr, FALSE);
    }
    void sync_shortcut_accessibility()
    {
        if (!shortcut_box)
            return;
        std::wstring text;
        if (recording_shortcut)
            text = L"Press launch shortcut keys, or Escape to cancel";
        else if (!installed)
            text = L"Launch shortcut: Ctrl + F12 (available after install)";
        else
            text = L"Launch shortcut: " + management::format_launch_hotkey(launch_hotkey);
        SetWindowTextW(shortcut_box, text.c_str());
    }
    void begin_shortcut_recording()
    {
        if (!installed || busy)
            return;
        recording_shortcut = true;
        set_shortcut_status(
            L"Use Ctrl, Alt, or Shift with F1-F24, or two modifiers with a letter or number.", false);
        buttons();
        SetFocus(shortcut_box);
    }
    void cancel_shortcut_recording(bool show_message)
    {
        if (!recording_shortcut)
            return;
        recording_shortcut = false;
        if (show_message)
            set_shortcut_status(L"Cancelled. Shortcut unchanged.", false);
        else
        {
            shortcut_error = false;
            InvalidateRect(shortcut_status, nullptr, TRUE);
            InvalidateRect(shortcut_box, nullptr, FALSE);
        }
        buttons();
    }
    void handle_shortcut_key(UINT vk, LPARAM lparam)
    {
        if (!recording_shortcut)
            return;
        if (vk == VK_ESCAPE)
        {
            cancel_shortcut_recording(true);
            return;
        }
        if (is_modifier_vk(vk))
        {
            InvalidateRect(shortcut_box, nullptr, FALSE);
            return;
        }
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool win = ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) != 0;
        const bool extended = (lparam & (1LL << 24)) != 0;
        const auto validated = management::validate_launch_hotkey(vk, ctrl, alt, shift, win, extended);
        recording_shortcut = false;
        if (!validated.valid)
        {
            const std::wstring kept = launch_hotkey
                                          ? L" Kept " + management::format_launch_hotkey(launch_hotkey) + L"."
                                          : L"";
            set_shortcut_status(validated.error + kept, true);
            buttons();
            return;
        }
        management::update_launch_hotkey(management::install_directory(), management::shortcut_directory(),
                                         validated.hotkey);
        launch_hotkey = validated.hotkey;
        set_shortcut_status(L"Saved " + management::format_launch_hotkey(launch_hotkey) + L" to Pluck.lnk.",
                            false);
        buttons();
    }
    void create()
    {
        installed = management::is_installed();
        if (installed)
        {
            try
            {
                launch_hotkey = management::effective_launch_hotkey(management::shortcut_directory());
            }
            catch (...)
            {
                launch_hotkey = management::DefaultLaunchHotkey;
            }
        }
        else
            launch_hotkey = management::DefaultLaunchHotkey;
        dpi = GetDpiForWindow(window);
        subtitle = label(
            (L"Version " + management::wide(PLUCK_VERSION) + (installed ? L" · Installed" : L" · Portable"))
                .c_str());
        install_status = label(installed ? L"Installed for this user.\nCapture and settings are in Start."
                                         : L"Install for this Windows user.\nAdd two shortcuts to Start.");
        location = label(management::install_directory().c_str(), SS_PATHELLIPSIS);
        shortcut_desc = label(
            installed ? L"Start a capture while Pluck is closed. Handled by Windows Explorer."
                      : L"Install Pluck to register a launch shortcut in Start. Default: Ctrl + F12.");
        shortcut_status = label(
            installed ? (launch_hotkey ? L"Saved to Pluck.lnk in Start menu. Default: Ctrl + F12."
                                       : L"No launch shortcut is set. Default: Ctrl + F12.")
                      : L"No background process is used. Uninstall clears the shortcut automatically.");
        update_status = label(L"Check for a newer stable release.");
        footer = label(L"Open Pluck to capture. Press F2 during selection to return here.");
        install = control(L"Install Pluck", Install);
        check = control(L"Check for updates", Check);
        update = control(L"Update", Update);
        shortcuts = control(L"Restore shortcuts", Shortcuts);
        uninstall = control(L"Uninstall", Uninstall);
        shortcut_box = control(L"Launch shortcut", ShortcutBox);
        shortcut_reset = control(L"Reset", ShortcutReset);
        shortcut_clear = control(L"Clear", ShortcutClear);
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
        move(shortcut_desc, left, 305, right - left, 22);
        move(shortcut_box, left, 333, right - left - 188, 38);
        move(shortcut_reset, right - 178, 333, 84, 38);
        move(shortcut_clear, right - 84, 333, 84, 38);
        move(shortcut_status, left, 378, right - left, 24);
        move(update_status, left, 459, right - left, 52);
        move(check, left, 518, 174, 38);
        move(update, right - 120, 518, 120, 38);
        move(footer, left, 586, right - left, 40);
        move(repository, left, 640, 174, 36);
        move(close, right - 100, 640, 100, 36);
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
        EnableWindow(shortcut_box, installed && !busy);
        EnableWindow(shortcut_reset,
                     installed && !busy && (recording_shortcut || launch_hotkey != management::DefaultLaunchHotkey));
        EnableWindow(shortcut_clear, installed && !busy && !recording_shortcut && launch_hotkey != 0);
        SetWindowTextW(shortcut_reset, recording_shortcut ? L"Cancel" : L"Reset");
        SetWindowTextW(close, busy ? L"Cancel" : L"Close");
        sync_shortcut_accessibility();
        InvalidateRect(shortcut_box, nullptr, FALSE);
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
        text(dc, L"Pluck", {px(96), py(27), client.right - px(32), py(70)}, title, palette.ink);
        text(dc, L"Installation", {px(32), py(125), client.right - px(32), py(156)}, heading, palette.ink);
        text(dc, L"Launch shortcut", {px(32), py(270), client.right - px(32), py(301)}, heading, palette.ink);
        text(dc, L"Updates", {px(32), py(422), client.right - px(32), py(453)}, heading, palette.ink);
        const auto pen = CreatePen(PS_SOLID, 1, palette.line);
        const auto previous = SelectObject(dc, pen);
        for (const auto y : {111, 258, 410, 572})
        {
            MoveToEx(dc, px(32), py(y), nullptr);
            LineTo(dc, client.right - px(32), py(y));
        }
        SelectObject(dc, previous);
        DeleteObject(pen);
        EndPaint(window, &ps);
    }
    void draw_shortcut_box(const DRAWITEMSTRUCT& item)
    {
        FillRect(item.hDC, &item.rcItem, background);
        const bool disabled = (item.itemState & ODS_DISABLED) != 0;
        const COLORREF rec_fill = palette.high_contrast ? palette.canvas
                                  : palette.dark        ? RGB(18, 36, 33)
                                                        : RGB(224, 246, 239);
        const COLORREF fill_color = recording_shortcut ? rec_fill : palette.surface;
        const COLORREF border_color = recording_shortcut ? palette.accent
                                      : shortcut_error   ? palette.error_ink
                                                         : palette.line;
        const int border_width = recording_shortcut ? std::max(2, px(2)) : 1;
        const auto fill = CreateSolidBrush(fill_color);
        const auto pen = CreatePen(PS_SOLID, border_width, border_color);
        const auto old_brush = SelectObject(item.hDC, fill);
        const auto old_pen = SelectObject(item.hDC, pen);
        RECT box = item.rcItem;
        if (border_width > 1)
            InflateRect(&box, -border_width / 2, -border_width / 2);
        RoundRect(item.hDC, box.left, box.top, box.right, box.bottom, px(8), px(8));
        SelectObject(item.hDC, old_brush);
        SelectObject(item.hDC, old_pen);
        DeleteObject(fill);
        DeleteObject(pen);

        SetBkMode(item.hDC, TRANSPARENT);
        const int pad_x = px(12);
        int cursor_x = item.rcItem.left + pad_x;
        const int box_height = item.rcItem.bottom - item.rcItem.top;

        if (recording_shortcut)
        {
            std::wstring live;
            if (GetKeyState(VK_CONTROL) & 0x8000)
                live += L"Ctrl + ";
            if (GetKeyState(VK_MENU) & 0x8000)
                live += L"Alt + ";
            if (GetKeyState(VK_SHIFT) & 0x8000)
                live += L"Shift + ";
            live += live.empty() ? L"Press keys..." : L"...";
            RECT left_rect{cursor_x, item.rcItem.top, item.rcItem.right - pad_x, item.rcItem.bottom};
            const auto old_font = SelectObject(item.hDC, normal);
            SetTextColor(item.hDC, palette.ink);
            DrawTextW(item.hDC, live.c_str(), -1, &left_rect,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SIZE measured{};
            GetTextExtentPoint32W(item.hDC, live.c_str(), static_cast<int>(live.size()), &measured);
            cursor_x += measured.cx + px(8);
            SelectObject(item.hDC, old_font);
        }
        else if (launch_hotkey == 0 && installed)
        {
            RECT left_rect{cursor_x, item.rcItem.top, item.rcItem.right - pad_x, item.rcItem.bottom};
            const auto old_font = SelectObject(item.hDC, normal);
            SetTextColor(item.hDC, palette.muted);
            DrawTextW(item.hDC, L"None", -1, &left_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SIZE measured{};
            GetTextExtentPoint32W(item.hDC, L"None", 4, &measured);
            cursor_x += measured.cx + px(8);
            SelectObject(item.hDC, old_font);
        }
        else
        {
            const auto tokens = split_hotkey_tokens(management::format_launch_hotkey(launch_hotkey));
            const COLORREF cap_fill = palette.high_contrast ? palette.canvas
                                      : palette.dark        ? RGB(19, 30, 27)
                                                            : RGB(255, 255, 255);
            const auto cap_brush = CreateSolidBrush(cap_fill);
            const auto cap_pen = CreatePen(PS_SOLID, 1, palette.line);
            const int cap_h = std::min(px(24), std::max(px(18), box_height - px(10)));
            const int cap_top = item.rcItem.top + (box_height - cap_h) / 2;
            const auto old_font = SelectObject(item.hDC, key_font);
            for (size_t i = 0; i < tokens.size(); ++i)
            {
                if (i > 0)
                {
                    SelectObject(item.hDC, small_font);
                    SetTextColor(item.hDC, palette.muted);
                    RECT plus_rect{cursor_x, item.rcItem.top, cursor_x + px(14), item.rcItem.bottom};
                    DrawTextW(item.hDC, L"+", 1, &plus_rect,
                              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                    cursor_x += px(14);
                    SelectObject(item.hDC, key_font);
                }
                SIZE size{};
                GetTextExtentPoint32W(item.hDC, tokens[i].c_str(), static_cast<int>(tokens[i].size()), &size);
                const int cap_w = size.cx + px(14);
                const auto prev_b = SelectObject(item.hDC, cap_brush);
                const auto prev_p = SelectObject(item.hDC, cap_pen);
                RoundRect(item.hDC, cursor_x, cap_top, cursor_x + cap_w, cap_top + cap_h, px(5), px(5));
                SelectObject(item.hDC, prev_b);
                SelectObject(item.hDC, prev_p);
                SetTextColor(item.hDC, disabled ? palette.muted : palette.ink);
                RECT cap_rect{cursor_x, cap_top, cursor_x + cap_w, cap_top + cap_h};
                DrawTextW(item.hDC, tokens[i].c_str(), -1, &cap_rect,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                cursor_x += cap_w;
            }
            SelectObject(item.hDC, old_font);
            DeleteObject(cap_brush);
            DeleteObject(cap_pen);
            cursor_x += px(8);
        }

        const wchar_t* hint = recording_shortcut     ? L"Esc to cancel"
                              : !installed           ? L"Available after install"
                              : launch_hotkey == 0   ? L"Click to set"
                                                     : L"Click to change";
        const auto old_hint_font = SelectObject(item.hDC, small_font);
        SIZE hint_size{};
        GetTextExtentPoint32W(item.hDC, hint, static_cast<int>(wcslen(hint)), &hint_size);
        if (cursor_x + hint_size.cx <= item.rcItem.right - pad_x)
        {
            RECT hint_rect{cursor_x, item.rcItem.top, item.rcItem.right - pad_x, item.rcItem.bottom};
            SetTextColor(item.hDC, palette.muted);
            DrawTextW(item.hDC, hint, -1, &hint_rect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        SelectObject(item.hDC, old_hint_font);

        if ((item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT) && !recording_shortcut)
        {
            RECT focus = item.rcItem;
            InflateRect(&focus, -px(4), -px(4));
            DrawFocusRect(item.hDC, &focus);
        }
    }
    void draw_button(const DRAWITEMSTRUCT& item)
    {
        if (item.CtlID == ShortcutBox)
        {
            draw_shortcut_box(item);
            return;
        }
        const bool primary = item.CtlID == Install || item.CtlID == Update;
        draw_theme_button(item, palette, background, normal, dpi, primary);
    }
    void start(int action)
    {
        if (busy)
            return;
        cancel_shortcut_recording(false);
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
        if (id == IDCANCEL && recording_shortcut)
        {
            cancel_shortcut_recording(true);
            return;
        }
        if (id == Close || id == IDCANCEL)
        {
            SendMessageW(window, WM_CLOSE, 0, 0);
            return;
        }
        if (busy)
            return;
        if (id == ShortcutBox)
        {
            if (recording_shortcut)
                cancel_shortcut_recording(true);
            else
                begin_shortcut_recording();
            return;
        }
        if (id == ShortcutReset)
        {
            if (recording_shortcut)
                cancel_shortcut_recording(true);
            else if (installed)
            {
                management::update_launch_hotkey(management::install_directory(),
                                                 management::shortcut_directory(),
                                                 management::DefaultLaunchHotkey);
                launch_hotkey = management::DefaultLaunchHotkey;
                set_shortcut_status(L"Restored default Ctrl + F12 in Pluck.lnk.", false);
                buttons();
            }
            return;
        }
        if (id == ShortcutClear)
        {
            if (installed)
            {
                recording_shortcut = false;
                management::update_launch_hotkey(management::install_directory(),
                                                 management::shortcut_directory(), 0);
                launch_hotkey = 0;
                set_shortcut_status(L"Shortcut cleared from Pluck.lnk.", false);
                buttons();
            }
            return;
        }
        cancel_shortcut_recording(false);
        if (id == Install || id == Check || id == Update)
            start(id);
        else if (id == Uninstall && installed &&
                 MessageBoxW(window, L"Remove the installed Pluck app and its Start menu shortcuts?",
                             L"Uninstall Pluck", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES)
            start(Uninstall);
        else if (id == Shortcuts)
        {
            const WORD hotkey = management::effective_launch_hotkey(management::shortcut_directory());
            management::create_shortcuts(management::install_directory(), management::shortcut_directory(),
                                         hotkey);
            management::write_saved_launch_hotkey(hotkey);
            launch_hotkey = hotkey;
            SetWindowTextW(install_status, L"Start menu shortcuts were restored.");
            set_shortcut_status(
                launch_hotkey
                    ? L"Restored Pluck.lnk with " + management::format_launch_hotkey(launch_hotkey) + L"."
                    : L"Restored Pluck.lnk with no launch shortcut.",
                false);
            buttons();
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
            SetTextColor(reinterpret_cast<HDC>(w),
                         reinterpret_cast<HWND>(l) == shortcut_status && shortcut_error ? palette.error_ink
                                                                                        : palette.muted);
            SetBkColor(reinterpret_cast<HDC>(w), palette.canvas);
            return reinterpret_cast<LRESULT>(background);
        case WM_DRAWITEM:
            draw_button(*reinterpret_cast<DRAWITEMSTRUCT*>(l));
            return TRUE;
        case WM_COMMAND:
            command(LOWORD(w));
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(w) == WA_INACTIVE)
                cancel_shortcut_recording(false);
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
    RECT rect{0, 0, MulDiv(Settings::BaseWidth, static_cast<int>(dpi), 96),
              MulDiv(Settings::BaseHeight, static_cast<int>(dpi), 96)};
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
    rect = {0, 0, settings.px(Settings::BaseWidth), settings.px(Settings::BaseHeight)};
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
    {
        if (settings.recording_shortcut &&
            (message.hwnd == window || IsChild(window, message.hwnd)))
        {
            if (message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN)
            {
                try
                {
                    settings.handle_shortcut_key(static_cast<UINT>(message.wParam), message.lParam);
                }
                catch (const std::exception& error)
                {
                    MessageBoxA(window, error.what(), "Pluck Settings", MB_OK | MB_ICONERROR);
                }
                continue;
            }
            if (message.message == WM_KEYUP || message.message == WM_SYSKEYUP)
            {
                if (is_modifier_vk(static_cast<UINT>(message.wParam)))
                    InvalidateRect(settings.shortcut_box, nullptr, FALSE);
                continue;
            }
            if (message.message == WM_CHAR || message.message == WM_SYSCHAR)
                continue;
        }
        if (!IsDialogMessageW(window, &message))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
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
