#include "capture.h"
#include "selector.h"
#include "app_process.h"
#include <dwmapi.h>
#include <ole2.h>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
void pump(DWORD duration = 30)
{
    const auto until = GetTickCount64() + duration;
    do
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(1);
    } while (GetTickCount64() < until);
}
void open_clipboard(HWND window)
{
    const auto until = GetTickCount64() + 2000;
    while (!OpenClipboard(window))
    {
        require(GetTickCount64() < until, "Cannot open the clipboard.");
        pump(10);
    }
}
// Copy each format before the test changes the clipboard. OleDuplicateData handles
// standard bitmap, metafile, palette, and global-memory formats correctly.
class ClipboardBackup
{
  public:
    explicit ClipboardBackup(HWND owner) : owner_(owner)
    {
        open_clipboard(owner_);
        for (UINT format = EnumClipboardFormats(0); format; format = EnumClipboardFormats(format))
        {
            if (auto source = GetClipboardData(format))
                if (auto copy = OleDuplicateData(source, static_cast<CLIPFORMAT>(format), 0))
                    data_.push_back({format, copy});
        }
        CloseClipboard();
    }
    ~ClipboardBackup()
    {
        bool opened{};
        for (int attempt = 0; attempt < 100 && !(opened = OpenClipboard(owner_)); ++attempt)
            Sleep(10);
        if (opened)
        {
            EmptyClipboard();
            for (const auto& entry : data_)
                SetClipboardData(entry.first, entry.second);
            CloseClipboard();
        }
    }

  private:
    HWND owner_{};
    std::vector<std::pair<UINT, HANDLE>> data_;
};
unsigned fixture_escape_presses{};
unsigned fixture_settings_presses{};
unsigned fixture_edit_presses{};
unsigned fixture_tab_presses{};
LRESULT CALLBACK pattern_proc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    if (message == WM_KEYDOWN && w == VK_ESCAPE)
    {
        ++fixture_escape_presses;
        return 0;
    }
    if (message == WM_KEYDOWN && w == VK_F2)
    {
        ++fixture_settings_presses;
        return 0;
    }
    if (message == WM_KEYDOWN && w == 'E')
    {
        ++fixture_edit_presses;
        return 0;
    }
    if (message == WM_KEYDOWN && w == VK_TAB)
    {
        ++fixture_tab_presses;
        return 0;
    }
    if (message == WM_PAINT)
    {
        PAINTSTRUCT ps{};
        auto dc = BeginPaint(window, &ps);
        RECT client{};
        GetClientRect(window, &client);
        auto brush = CreateSolidBrush(RGB(32, 96, 192));
        FillRect(dc, &client, brush);
        DeleteObject(brush);
        RECT red{120, 100, 180, 140};
        brush = CreateSolidBrush(RGB(224, 48, 64));
        FillRect(dc, &red, brush);
        DeleteObject(brush);
        EndPaint(window, &ps);
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}
struct Child
{
    PROCESS_INFORMATION process{};
    HWND window{};
    double startup_ms{};
    explicit Child(const wchar_t* executable, bool foreground = true)
    {
        std::wstring command = L"\"" + std::wstring(executable) + L"\"";
        STARTUPINFOW startup{sizeof(startup)};
        LARGE_INTEGER frequency{}, start{}, end{};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
        require(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                               &startup, &process),
                "Cannot start Pluck.");
        const auto until = GetTickCount64() + 10000;
        while (!window)
        {
            pump(1);
            window = FindWindowW(pluck::SelectorClass, nullptr);
            DWORD owner{};
            if (window)
                GetWindowThreadProcessId(window, &owner);
            if (window && (owner != process.dwProcessId || !IsWindowVisible(window) ||
                           (foreground && GetForegroundWindow() != window)))
                window = nullptr;
            if (GetTickCount64() >= until || WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0)
            {
                TerminateProcess(process.hProcess, 1);
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
                process = {};
                throw std::runtime_error("Pluck did not enter the required selection mode.");
            }
        }
        // A round trip waits for the first synchronous UpdateWindow to finish.
        DWORD_PTR ignored{};
        SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 2000, &ignored);
        DwmFlush();
        QueryPerformanceCounter(&end);
        startup_ms = 1000.0 * (end.QuadPart - start.QuadPart) / frequency.QuadPart;
    }
    ~Child()
    {
        if (process.hProcess)
        {
            if (WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT)
            {
                if (IsWindow(window))
                    PostMessageW(window, WM_CLOSE, 0, 0);
                HWND editor = nullptr;
                while ((editor = FindWindowExW(nullptr, editor, pluck::EditorClass, nullptr)) != nullptr)
                {
                    DWORD owner{};
                    GetWindowThreadProcessId(editor, &owner);
                    if (owner == process.dwProcessId)
                        PostMessageW(editor, WM_CLOSE, 0, 0);
                }
                if (WaitForSingleObject(process.hProcess, 2000) == WAIT_TIMEOUT)
                    TerminateProcess(process.hProcess, 1);
            }
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
    }
    void exited()
    {
        const auto until = GetTickCount64() + 5000;
        while (WaitForSingleObject(process.hProcess, 0) != WAIT_OBJECT_0)
        {
            require(GetTickCount64() < until, "Pluck did not exit.");
            pump(10);
        }
        DWORD code{};
        GetExitCodeProcess(process.hProcess, &code);
        require(code == 0, "Pluck returned a failure exit code.");
    }
};
class ForegroundLock
{
  public:
    ForegroundLock()
    {
        require(LockSetForegroundWindow(LSFW_LOCK), "Cannot lock foreground focus for the test.");
    }
    ~ForegroundLock()
    {
        LockSetForegroundWindow(LSFW_UNLOCK);
    }
};
class PopupMenu
{
  public:
    explicit PopupMenu(POINT point) : thread_([this, point] { run(point); }) {}
    ~PopupMenu()
    {
        if (!finished_)
            PostThreadMessageW(GetThreadId(thread_.native_handle()), WM_QUIT, 0, 0);
        thread_.join();
    }
    RECT bounds()
    {
        const auto until = GetTickCount64() + 5000;
        while (!menu_window_ || !IsWindowVisible(menu_window_))
        {
            require(!finished_ && GetTickCount64() < until, "Cannot open the popup menu fixture.");
            pump(10);
        }
        pump(100);
        DwmFlush();
        RECT rect{};
        require(GetWindowRect(menu_window_, &rect), "Cannot locate the popup menu.");
        return rect;
    }
    bool closed() const
    {
        return finished_;
    }

  private:
    std::atomic<HWND> menu_window_{};
    std::atomic<bool> finished_{};
    std::thread thread_;
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w, LPARAM l)
    {
        if (message == WM_NCCREATE)
            SetWindowLongPtrW(
                window, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
        auto* self = reinterpret_cast<PopupMenu*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self && message == WM_ENTERIDLE && w == MSGF_MENU)
            self->menu_window_ = reinterpret_cast<HWND>(l);
        return DefWindowProcW(window, message, w, l);
    }
    void run(POINT point)
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = procedure;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"Pluck.Test.Menu";
        RegisterClassW(&wc);
        const auto owner = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Pluck menu test", WS_POPUP,
                                           point.x, point.y, 1, 1, nullptr, nullptr, wc.hInstance, this);
        const auto menu = CreatePopupMenu();
        if (owner && menu && AppendMenuW(menu, MF_STRING, 1, L"Open test menu") &&
            AppendMenuW(menu, MF_STRING, 2, L"Keep these pixels") &&
            AppendMenuW(menu, MF_STRING, 3, L"Exit test menu"))
        {
            ShowWindow(owner, SW_SHOW);
            SetForegroundWindow(owner);
            TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NOANIMATION, point.x, point.y, 0,
                           owner, nullptr);
        }
        if (menu)
            DestroyMenu(menu);
        if (owner)
            DestroyWindow(owner);
        finished_ = true;
    }
};
void mouse(POINT point, DWORD button)
{
    require(SetCursorPos(point.x, point.y), "Cannot move the test pointer.");
    if (button)
    {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = button;
        require(SendInput(1, &input, sizeof(input)) == 1, "Cannot send mouse input.");
    }
    pump();
}
void escape()
{
    INPUT input[2]{};
    input[0].type = input[1].type = INPUT_KEYBOARD;
    input[0].ki.wVk = input[1].ki.wVk = VK_ESCAPE;
    input[1].ki.dwFlags = KEYEVENTF_KEYUP;
    require(SendInput(2, input, sizeof(INPUT)) == 2, "Cannot send Escape.");
}
void settings_key()
{
    INPUT input[2]{};
    input[0].type = input[1].type = INPUT_KEYBOARD;
    input[0].ki.wVk = input[1].ki.wVk = VK_F2;
    input[1].ki.dwFlags = KEYEVENTF_KEYUP;
    require(SendInput(2, input, sizeof(INPUT)) == 2, "Cannot send F2.");
}
void edit_key()
{
    INPUT input[2]{};
    input[0].type = input[1].type = INPUT_KEYBOARD;
    input[0].ki.wVk = input[1].ki.wVk = 'E';
    input[1].ki.dwFlags = KEYEVENTF_KEYUP;
    require(SendInput(2, input, sizeof(INPUT)) == 2, "Cannot send E.");
}
void tab_key()
{
    INPUT input[2]{};
    input[0].type = input[1].type = INPUT_KEYBOARD;
    input[0].ki.wVk = input[1].ki.wVk = VK_TAB;
    input[1].ki.dwFlags = KEYEVENTF_KEYUP;
    require(SendInput(2, input, sizeof(INPUT)) == 2, "Cannot send Tab.");
}
void send_vk(WORD key)
{
    INPUT input[2]{};
    input[0].type = input[1].type = INPUT_KEYBOARD;
    input[0].ki.wVk = input[1].ki.wVk = key;
    input[1].ki.dwFlags = KEYEVENTF_KEYUP;
    require(SendInput(2, input, sizeof(INPUT)) == 2, "Cannot send key input.");
}
void ctrl_key(WORD key)
{
    INPUT input[4]{};
    for (auto& item : input)
        item.type = INPUT_KEYBOARD;
    input[0].ki.wVk = VK_CONTROL;
    input[1].ki.wVk = key;
    input[2].ki.wVk = key;
    input[2].ki.dwFlags = KEYEVENTF_KEYUP;
    input[3].ki.wVk = VK_CONTROL;
    input[3].ki.dwFlags = KEYEVENTF_KEYUP;
    require(SendInput(4, input, sizeof(INPUT)) == 4, "Cannot send a Ctrl shortcut.");
}
HWND find_owned_window(const wchar_t* class_name, DWORD pid)
{
    HWND current = nullptr;
    while ((current = FindWindowExW(nullptr, current, class_name, nullptr)) != nullptr)
    {
        DWORD owner{};
        GetWindowThreadProcessId(current, &owner);
        if (owner == pid && IsWindowVisible(current))
            return current;
    }
    return nullptr;
}
HWND wait_for_editor(DWORD pid)
{
    const auto deadline = GetTickCount64() + 10000;
    HWND editor = nullptr;
    while (!(editor = find_owned_window(pluck::EditorClass, pid)))
    {
        require(GetTickCount64() < deadline, "Edit mode did not open the editor window.");
        pump(10);
    }
    DWORD_PTR ignored{};
    SendMessageTimeoutW(editor, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 2000, &ignored);
    pump(100);
    DwmFlush();
    return editor;
}
class SettingsWindow
{
  public:
    HWND window{};
    HANDLE process{};
    SettingsWindow()
    {
        const auto deadline = GetTickCount64() + 10000;
        while (!(window = FindWindowW(pluck::SettingsClass, nullptr)) || !IsWindowVisible(window))
        {
            require(GetTickCount64() < deadline, "F2 did not open Settings.");
            pump(10);
        }
        DWORD pid{};
        GetWindowThreadProcessId(window, &pid);
        process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        require(process != nullptr, "Cannot track the settings process.");
        DWORD_PTR ignored{};
        SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 2000, &ignored);
        pump(100);
    }
    ~SettingsWindow()
    {
        if (window)
            PostMessageW(window, WM_CLOSE, 0, 0);
        if (process)
        {
            WaitForSingleObject(process, 6000);
            CloseHandle(process);
        }
    }
};
class WindowChanges
{
  public:
    explicit WindowChanges(HWND window)
    {
        window_ = window;
        count_ = 0;
        DWORD process{};
        const auto thread = GetWindowThreadProcessId(window, &process);
        hook_ = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, changed,
                                process, thread, WINEVENT_OUTOFCONTEXT);
        require(hook_ != nullptr, "Cannot monitor selection window changes.");
    }
    ~WindowChanges()
    {
        UnhookWinEvent(hook_);
        window_ = nullptr;
    }
    unsigned count() const noexcept
    {
        return count_;
    }

  private:
    HWINEVENTHOOK hook_{};
    inline static HWND window_{};
    inline static unsigned count_{};
    static void CALLBACK changed(HWINEVENTHOOK, DWORD, HWND window, LONG object, LONG, DWORD, DWORD)
    {
        if (window == window_ && object == OBJID_WINDOW)
            ++count_;
    }
};
void verify_held_selection(HWND window, POINT point, const WindowChanges& changes)
{
    // Let the last real pointer move and its window notifications finish first.
    pump(150);
    const auto before = changes.count();
    POINT client = point;
    ScreenToClient(window, &client);
    const auto until = GetTickCount64() + 5000;
    do
    {
        // Windows can send mouse-move messages without a change in pointer position.
        require(PostMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(client.x, client.y)),
                "Cannot send a stationary mouse-move message.");
        pump(50);
    } while (GetTickCount64() < until);
    std::printf("Held selection: %u window changes in five seconds.\n", changes.count() - before);
    require(changes.count() == before, "A stationary selection repeatedly changes its window region.");
}
void marker(HWND window)
{
    constexpr wchar_t text[] = L"Pluck cancellation test";
    auto memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(text));
    require(memory != nullptr, "Cannot allocate marker.");
    auto* buffer = GlobalLock(memory);
    require(buffer != nullptr, "Cannot lock marker.");
    memcpy(buffer, text, sizeof(text));
    GlobalUnlock(memory);
    open_clipboard(window);
    EmptyClipboard();
    require(SetClipboardData(CF_UNICODETEXT, memory) != nullptr, "Cannot set marker.");
    CloseClipboard();
}
void verify_image(HWND owner)
{
    open_clipboard(owner);
    const auto memory = GetClipboardData(CF_DIB);
    require(memory != nullptr, "No bitmap remains after Pluck exits.");
    auto* header = static_cast<BITMAPINFOHEADER*>(GlobalLock(memory));
    require(header && header->biWidth == 100 && header->biHeight == 80 && header->biBitCount == 24,
            "Clipboard image dimensions differ.");
    const auto* bytes = reinterpret_cast<const unsigned char*>(header + 1);
    for (int y = 0; y < 80; ++y)
        for (int x = 0; x < 100; ++x)
        {
            const auto* pixel = bytes + (79 - y) * 300 + x * 3;
            const bool red = x >= 20 && x < 80 && y >= 20 && y < 60;
            require(pixel[0] == (red ? 64 : 192) && pixel[1] == (red ? 48 : 96) &&
                        pixel[2] == (red ? 224 : 32),
                    "Crop contains shifted, dimmed, or overlay pixels.");
        }
    GlobalUnlock(memory);
    CloseClipboard();
}
void verify_window_image(HWND owner, int width, int height)
{
    open_clipboard(owner);
    const auto memory = GetClipboardData(CF_DIB);
    require(memory != nullptr, "No window bitmap remains after Pluck exits.");
    auto* header = static_cast<BITMAPINFOHEADER*>(GlobalLock(memory));
    require(header && header->biWidth == width && header->biHeight == height && header->biBitCount == 24,
            "Window clipboard image dimensions differ.");
    const size_t stride = (static_cast<size_t>(width) * 3 + 3) & ~size_t{3};
    const auto* bytes = reinterpret_cast<const unsigned char*>(header + 1);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
        {
            const auto* pixel = bytes + static_cast<size_t>(height - 1 - y) * stride + static_cast<size_t>(x) * 3;
            const bool red = x >= 120 && x < 180 && y >= 100 && y < 140;
            require(pixel[0] == (red ? 64 : 192) && pixel[1] == (red ? 48 : 96) &&
                        pixel[2] == (red ? 224 : 32),
                    "Window capture contains shifted, dimmed, or overlay pixels.");
        }
    GlobalUnlock(memory);
    CloseClipboard();
}
bool menu_pixels_match(const unsigned char* actual, const unsigned char* expected, size_t bytes)
{
    // Separate desktop captures can round antialiased menu edges by one level.
    for (size_t i = 0; i < bytes; ++i)
        if (actual[i] + 1 < expected[i] || expected[i] + 1 < actual[i])
            return false;
    return true;
}
void verify_popup(HWND owner, const pluck::Bitmap& expected)
{
    auto expected_dib = pluck::make_clipboard_dib(expected, {0, 0, expected.width(), expected.height()});
    open_clipboard(owner);
    const auto memory = GetClipboardData(CF_DIB);
    require(memory != nullptr, "No popup menu image remains on the clipboard.");
    const auto* actual = static_cast<const BITMAPINFOHEADER*>(GlobalLock(memory));
    const auto* wanted = static_cast<const BITMAPINFOHEADER*>(GlobalLock(expected_dib.get()));
    require(actual && wanted, "Cannot read the popup menu image.");
    const bool matches =
        actual->biWidth == wanted->biWidth && actual->biHeight == wanted->biHeight &&
        actual->biBitCount == wanted->biBitCount && actual->biSizeImage == wanted->biSizeImage &&
        menu_pixels_match(reinterpret_cast<const unsigned char*>(actual + 1),
                          reinterpret_cast<const unsigned char*>(wanted + 1), wanted->biSizeImage);
    GlobalUnlock(expected_dib.get());
    GlobalUnlock(memory);
    CloseClipboard();
    require(matches, "The copied image lost or changed the open popup menu.");
}
void save_bitmap(const pluck::Bitmap& image, const std::wstring& path)
{
    auto data = pluck::make_clipboard_dib(image, {0, 0, image.width(), image.height()});
    const auto* dib = static_cast<BITMAPINFOHEADER*>(GlobalLock(data.get()));
    BITMAPFILEHEADER header{};
    header.bfType = 0x4d42;
    header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
    header.bfSize = header.bfOffBits + dib->biSizeImage;
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(dib), sizeof(BITMAPINFOHEADER) + dib->biSizeImage);
    GlobalUnlock(data.get());
    require(file.good(), "Cannot save the selection screenshot.");
}
void save_view(const wchar_t* directory)
{
    const auto image = pluck::capture_desktop(pluck::desktop_bounds());
    save_bitmap(*image, std::wstring(directory) + L"/selection.bmp");
}
BOOL CALLBACK collect_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM parameter)
{
    MONITORINFO info{sizeof(info)};
    if (GetMonitorInfoW(monitor, &info))
        reinterpret_cast<std::vector<RECT>*>(parameter)->push_back(info.rcMonitor);
    return TRUE;
}
} // namespace
int wmain(int count, wchar_t** arguments)
{
    if (count != 3)
    {
        std::fputs("Usage: pluck_desktop_tests.exe <Pluck.exe> <output directory>\n", stderr);
        return 1;
    }
    OleInitialize(nullptr);
    const auto previous_foreground = GetForegroundWindow();
    POINT previous_cursor{};
    GetCursorPos(&previous_cursor);
    WNDCLASSW wc{};
    wc.lpfnWndProc = pattern_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Pluck.Test.Pattern";
    RegisterClassW(&wc);
    const auto fixture =
        CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"Pluck desktop test", WS_POPUP,
                        80, 80, 500, 350, nullptr, nullptr, wc.hInstance, nullptr);
    int result{};
    try
    {
        require(fixture != nullptr, "Cannot create the pattern window.");
        ClipboardBackup backup(fixture);
        ShowWindow(fixture, SW_SHOW);
        SetForegroundWindow(fixture);
        UpdateWindow(fixture);
        pump(100);
        RECT actual{};
        GetWindowRect(fixture, &actual);
        // Use actual bounds if a desktop window manager moves the fixture.
        const POINT a{actual.left + 100, actual.top + 80};
        const POINT b{a.x + 100, a.y + 80};
        for (int retry = 0; retry < 2; ++retry)
        {
            std::printf("Checking popup menu capture%s.\n", retry ? " after a clipboard retry" : "");
            SetForegroundWindow(fixture);
            mouse({actual.left + 10, actual.top + 10}, 0);
            PopupMenu menu({a.x, a.y});
            const auto rect = menu.bounds();
            const auto expected = pluck::capture_desktop(rect);
            // Windows can refuse foreground focus while another app has an open menu.
            Child child(arguments[1], false);
            if (retry)
                open_clipboard(fixture);
            mouse({rect.right, rect.top}, MOUSEEVENTF_LEFTDOWN);
            mouse({rect.left, rect.bottom}, 0);
            require(menu.closed(), "The selection click did not dismiss the popup menu fixture.");
            DwmFlush();
            const auto view = pluck::capture_desktop(rect);
            save_bitmap(*expected, std::wstring(arguments[2]) + L"/menu-expected.bmp");
            save_bitmap(*view, std::wstring(arguments[2]) + L"/menu-selection.bmp");
            for (int i = 0; i < expected->width() * expected->height(); ++i)
                require(menu_pixels_match(reinterpret_cast<const unsigned char*>(view->pixels() + i),
                                          reinterpret_cast<const unsigned char*>(expected->pixels() + i), 3),
                        "The popup menu disappeared or changed inside the selection rectangle.");
            mouse({rect.left, rect.bottom}, MOUSEEVENTF_LEFTUP);
            if (retry)
            {
                pump(1800);
                CloseClipboard();
                require(IsWindowVisible(child.window), "A clipboard failure did not restore selection.");
                mouse({rect.right, rect.top}, MOUSEEVENTF_LEFTDOWN);
                mouse({rect.left, rect.bottom}, MOUSEEVENTF_LEFTUP);
            }
            child.exited();
            verify_popup(fixture, *expected);
        }
        std::puts("PASS: popup menu pixels remain in selection, on the clipboard, and after a copy retry.");
        {
            marker(fixture);
            const auto sequence = GetClipboardSequenceNumber();
            SetForegroundWindow(fixture);
            require(GetForegroundWindow() == fixture, "The test fixture does not have foreground focus.");
            ForegroundLock lock;
            Child child(arguments[1], false);
            require(GetForegroundWindow() == fixture, "The launch test did not retain keyboard focus.");
            const auto presses = fixture_escape_presses;
            escape();
            child.exited();
            pump();
            require(GetClipboardSequenceNumber() == sequence, "Escape before drawing changed the clipboard.");
            require(fixture_escape_presses == presses, "The cancel key reached the app behind Pluck.");
            require(GetForegroundWindow() == fixture, "Cancellation changed foreground focus.");
            escape();
            pump();
            require(fixture_escape_presses == presses + 1, "Escape did not return to the previous app.");
        }
        std::puts("PASS: Escape before drawing exits without foreground focus and preserves the clipboard.");
        for (int mode = 0; mode < 3; ++mode)
        {
            marker(fixture);
            const auto sequence = GetClipboardSequenceNumber();
            const auto presses = fixture_settings_presses;
            SetForegroundWindow(fixture);
            std::unique_ptr<ForegroundLock> lock;
            if (mode == 2)
                lock = std::make_unique<ForegroundLock>();
            Child child(arguments[1], mode != 2);
            if (mode == 1)
            {
                mouse(a, MOUSEEVENTF_LEFTDOWN);
                mouse(b, 0);
            }
            settings_key();
            child.exited();
            lock.reset();
            SettingsWindow settings;
            if (mode == 1)
                mouse(b, MOUSEEVENTF_LEFTUP);
            require(GetClipboardSequenceNumber() == sequence, "F2 changed the clipboard.");
            require(fixture_settings_presses == presses, "F2 reached the app behind capture.");
            require(FindWindowW(pluck::SelectorClass, nullptr) == nullptr, "F2 left capture open.");
            if (mode == 0)
            {
                RECT bounds{};
                GetWindowRect(settings.window, &bounds);
                const auto image = pluck::capture_desktop(bounds);
                save_bitmap(*image, std::wstring(arguments[2]) + L"/settings.bmp");
                SetForegroundWindow(fixture);
                Child another(arguments[1]);
                settings_key();
                another.exited();
                pump(100);
                require(FindWindowW(pluck::SettingsClass, nullptr) == settings.window,
                        "F2 opened a second settings window.");
                require(WaitForSingleObject(settings.process, 0) == WAIT_TIMEOUT,
                        "Capture exit closed Settings.");
            }
        }
        std::puts("PASS: F2 before/during selection and without focus opens one settings process and "
                  "preserves the clipboard.");
        {
            // Mode 0: E before drag opens the editor, preserves clipboard, allows concurrent capture,
            // and Ctrl+C copies exact pixels and closes the editor.
            marker(fixture);
            const auto sequence = GetClipboardSequenceNumber();
            const auto presses = fixture_edit_presses;
            SetForegroundWindow(fixture);
            Child child(arguments[1]);
            edit_key();
            pump(60);
            mouse(a, MOUSEEVENTF_LEFTDOWN);
            mouse(b, 0);
            mouse(b, MOUSEEVENTF_LEFTUP);
            const auto editor = wait_for_editor(child.process.dwProcessId);
            require(fixture_edit_presses == presses, "E reached the app behind capture.");
            require(FindWindowW(pluck::SelectorClass, nullptr) == nullptr, "Edit mode left selector open.");
            require(GetClipboardSequenceNumber() == sequence, "Opening the editor changed the clipboard.");
            RECT editor_bounds{};
            GetWindowRect(editor, &editor_bounds);
            const auto editor_shot = pluck::capture_desktop(editor_bounds);
            save_bitmap(*editor_shot, std::wstring(arguments[2]) + L"/editor.bmp");

            // A second capture must work while an editor window is open.
            SetForegroundWindow(fixture);
            pump(50);
            {
                Child concurrent(arguments[1]);
                mouse(a, MOUSEEVENTF_LEFTDOWN);
                mouse(b, MOUSEEVENTF_LEFTUP);
                concurrent.exited();
                verify_image(fixture);
            }
            require(IsWindow(editor), "A concurrent capture closed the open editor.");

            marker(fixture);
            const auto copy_sequence = GetClipboardSequenceNumber();
            SetForegroundWindow(editor);
            pump(60);
            ctrl_key('C');
            const auto copy_deadline = GetTickCount64() + 3000;
            while (GetClipboardSequenceNumber() == copy_sequence)
            {
                require(GetTickCount64() < copy_deadline, "Ctrl+C in the editor did not copy the image.");
                pump(10);
            }
            pump(100);
            require(IsWindow(editor) && IsWindowVisible(editor),
                    "Ctrl+C closed the editor window.");
            verify_image(fixture);
            PostMessageW(editor, WM_CLOSE, 0, 0);
            child.exited();
            verify_image(fixture);
        }
        {
            // Mode 1: E during drag opens the editor; Ctrl+C retries a busy clipboard and stays open.
            marker(fixture);
            const auto sequence = GetClipboardSequenceNumber();
            SetForegroundWindow(fixture);
            Child child(arguments[1]);
            mouse(a, MOUSEEVENTF_LEFTDOWN);
            mouse(b, 0);
            edit_key();
            pump(60);
            mouse(b, MOUSEEVENTF_LEFTUP);
            const auto editor = wait_for_editor(child.process.dwProcessId);
            require(GetClipboardSequenceNumber() == sequence, "Opening the editor during drag changed the clipboard.");
            open_clipboard(fixture);
            SetForegroundWindow(editor);
            pump(50);
            ctrl_key('C');
            pump(250);
            require(IsWindow(editor), "A locked clipboard closed the editor early.");
            CloseClipboard();
            const auto retry_deadline = GetTickCount64() + 3000;
            while (GetClipboardSequenceNumber() == sequence)
            {
                require(GetTickCount64() < retry_deadline, "Editor clipboard retry did not copy the image.");
                pump(10);
            }
            pump(100);
            require(IsWindow(editor) && IsWindowVisible(editor),
                    "Clipboard retry copy closed the editor window.");
            verify_image(fixture);
            PostMessageW(editor, WM_CLOSE, 0, 0);
            child.exited();
            verify_image(fixture);
        }
        {
            // Mode 2: E without foreground focus does not leak to the background app, and Esc does not close editor.
            marker(fixture);
            const auto sequence = GetClipboardSequenceNumber();
            const auto presses = fixture_edit_presses;
            SetForegroundWindow(fixture);
            ForegroundLock lock;
            Child child(arguments[1], false);
            edit_key();
            pump(60);
            mouse(a, MOUSEEVENTF_LEFTDOWN);
            mouse(b, MOUSEEVENTF_LEFTUP);
            const auto editor = wait_for_editor(child.process.dwProcessId);
            require(fixture_edit_presses == presses, "E without focus reached the app behind capture.");
            require(GetClipboardSequenceNumber() == sequence, "Editor changed the clipboard before Esc.");
            SetForegroundWindow(editor);
            pump(50);
            escape();
            pump(80);
            escape();
            pump(80);
            require(IsWindow(editor) && IsWindowVisible(editor), "Esc closed the editor window.");
            PostMessageW(editor, WM_CLOSE, 0, 0);
            child.exited();
            require(GetClipboardSequenceNumber() == sequence, "Closing editor without copy changed the clipboard.");
        }
        {
            // Mode 3: Pressing E twice cancels edit mode and restores direct copy-and-exit.
            SetForegroundWindow(fixture);
            Child child(arguments[1]);
            edit_key();
            pump(50);
            edit_key();
            pump(50);
            mouse(a, MOUSEEVENTF_LEFTDOWN);
            mouse(b, MOUSEEVENTF_LEFTUP);
            child.exited();
            require(FindWindowW(pluck::EditorClass, nullptr) == nullptr,
                    "Toggling edit mode off still opened the editor.");
            verify_image(fixture);
        }
        {
            // Mode 4: Draw markup in the editor, copy modified pixels, save screenshot, undo back to exact base.
            marker(fixture);
            SetForegroundWindow(fixture);
            Child child(arguments[1]);
            edit_key();
            pump(60);
            mouse(a, MOUSEEVENTF_LEFTDOWN);
            mouse(b, MOUSEEVENTF_LEFTUP);
            const auto editor = wait_for_editor(child.process.dwProcessId);
            SetForegroundWindow(editor);
            pump(80);
            const auto canvas = FindWindowExW(editor, nullptr, L"Pluck.Native.EditorCanvas", nullptr);
            require(canvas != nullptr, "Editor canvas child window was not created.");
            RECT canvas_rect{};
            GetWindowRect(canvas, &canvas_rect);
            const POINT center{(canvas_rect.left + canvas_rect.right) / 2,
                               (canvas_rect.top + canvas_rect.bottom) / 2};

            // Draw a red rectangle across the 100x80 captured image centered in the canvas.
            send_vk('R');
            pump(40);
            mouse({center.x - 42, center.y - 32}, MOUSEEVENTF_LEFTDOWN);
            mouse({center.x + 42, center.y + 32}, 0);
            mouse({center.x + 42, center.y + 32}, MOUSEEVENTF_LEFTUP);
            pump(60);

            // Also stamp a step badge at the top-left of the image so the visual artifact shows both.
            send_vk('N');
            pump(40);
            mouse({center.x - 24, center.y - 16}, MOUSEEVENTF_LEFTDOWN);
            mouse({center.x - 24, center.y - 16}, MOUSEEVENTF_LEFTUP);
            pump(80);

            RECT editor_bounds{};
            GetWindowRect(editor, &editor_bounds);
            const auto annotated_shot = pluck::capture_desktop(editor_bounds);
            save_bitmap(*annotated_shot, std::wstring(arguments[2]) + L"/editor-annotated.bmp");

            const auto seq_before_copy = GetClipboardSequenceNumber();
            ctrl_key('C');
            const auto copy_deadline = GetTickCount64() + 3000;
            while (GetClipboardSequenceNumber() == seq_before_copy)
            {
                require(GetTickCount64() < copy_deadline, "Ctrl+C with markup did not copy.");
                pump(10);
            }
            // Verify the copied pixels differ from the unedited base fixture.
            bool changed = false;
            open_clipboard(fixture);
            if (const auto mem = GetClipboardData(CF_DIB))
            {
                const auto* hdr = static_cast<BITMAPINFOHEADER*>(GlobalLock(mem));
                require(hdr && hdr->biWidth == 100 && hdr->biHeight == 80,
                        "Annotated clipboard image dimensions differ.");
                const auto* bytes = reinterpret_cast<const unsigned char*>(hdr + 1);
                for (int y = 0; y < 80 && !changed; ++y)
                    for (int x = 0; x < 100 && !changed; ++x)
                    {
                        const auto* p = bytes + (79 - y) * 300 + x * 3;
                        const bool red = x >= 20 && x < 80 && y >= 20 && y < 60;
                        if (p[0] != (red ? 64 : 192) || p[1] != (red ? 48 : 96) || p[2] != (red ? 224 : 32))
                            changed = true;
                    }
                GlobalUnlock(mem);
            }
            CloseClipboard();
            require(changed, "Annotated editor copy did not contain drawn markup.");

            // Undo both annotations and verify Ctrl+C restores the exact untouched capture pixels.
            ctrl_key('Z');
            pump(40);
            ctrl_key('Z');
            pump(60);
            const auto seq_undo_copy = GetClipboardSequenceNumber();
            ctrl_key('C');
            const auto undo_deadline = GetTickCount64() + 3000;
            while (GetClipboardSequenceNumber() == seq_undo_copy)
            {
                require(GetTickCount64() < undo_deadline, "Ctrl+C after undo did not copy.");
                pump(10);
            }
            verify_image(fixture);

            // Escape switches to Select/Move and keeps the editor open; WM_CLOSE exits cleanly.
            escape();
            pump(60);
            require(IsWindow(editor) && IsWindowVisible(editor), "Esc closed the editor window.");
            PostMessageW(editor, WM_CLOSE, 0, 0);
            child.exited();
        }
        std::puts("PASS: edit mode (E before/during drag, without focus, toggle off, concurrent capture, "
                  "markup draw/undo, Esc to select, Ctrl+C copy + retry, and window close).");
        {
            const int fixture_width = actual.right - actual.left;
            const int fixture_height = actual.bottom - actual.top;
            const POINT inside{actual.left + 20, actual.top + 20};

            // Window mode direct copy: Tab switches to window selection, click copies exact window pixels.
            const auto tab_presses = fixture_tab_presses;
            SetForegroundWindow(fixture);
            mouse(inside, 0);
            {
                Child child(arguments[1]);
                tab_key();
                pump(80);
                DwmFlush();
                const auto preview = pluck::capture_desktop(pluck::desktop_bounds());
                save_bitmap(*preview, std::wstring(arguments[2]) + L"/window-mode.bmp");
                mouse(inside, MOUSEEVENTF_LEFTDOWN);
                mouse(inside, MOUSEEVENTF_LEFTUP);
                child.exited();
            }
            require(fixture_tab_presses == tab_presses, "Tab reached the app behind capture.");
            verify_window_image(fixture, fixture_width, fixture_height);

            // Window mode + edit mode: Tab + E opens the editor with the full window image.
            marker(fixture);
            const auto sequence = GetClipboardSequenceNumber();
            SetForegroundWindow(fixture);
            mouse(inside, 0);
            {
                Child child(arguments[1]);
                tab_key();
                pump(50);
                edit_key();
                pump(50);
                mouse(inside, MOUSEEVENTF_LEFTDOWN);
                mouse(inside, MOUSEEVENTF_LEFTUP);
                const auto editor = wait_for_editor(child.process.dwProcessId);
                require(GetClipboardSequenceNumber() == sequence,
                        "Window + edit mode changed the clipboard before Ctrl+C.");
                SetForegroundWindow(editor);
                pump(60);
                ctrl_key('C');
                const auto deadline = GetTickCount64() + 3000;
                while (GetClipboardSequenceNumber() == sequence)
                {
                    require(GetTickCount64() < deadline, "Ctrl+C in window edit mode did not copy.");
                    pump(10);
                }
                verify_window_image(fixture, fixture_width, fixture_height);
                PostMessageW(editor, WM_CLOSE, 0, 0);
                child.exited();
            }

            // Window mode without foreground focus: Tab does not leak and still selects the window.
            const auto locked_tabs = fixture_tab_presses;
            SetForegroundWindow(fixture);
            mouse(inside, 0);
            {
                ForegroundLock lock;
                Child child(arguments[1], false);
                tab_key();
                pump(60);
                mouse(inside, MOUSEEVENTF_LEFTDOWN);
                mouse(inside, MOUSEEVENTF_LEFTUP);
                child.exited();
            }
            require(fixture_tab_presses == locked_tabs, "Tab without focus reached the app behind capture.");
            verify_window_image(fixture, fixture_width, fixture_height);

            // Tab during a rectangle drag cancels the drag, and pressing Tab again returns to rectangle mode.
            SetForegroundWindow(fixture);
            {
                Child child(arguments[1]);
                mouse(a, MOUSEEVENTF_LEFTDOWN);
                mouse(b, 0);
                tab_key();
                pump(60);
                mouse(b, MOUSEEVENTF_LEFTUP);
                require(IsWindow(child.window), "Releasing after mid-drag Tab closed Pluck unexpectedly.");
                tab_key();
                pump(60);
                mouse(a, MOUSEEVENTF_LEFTDOWN);
                mouse(b, MOUSEEVENTF_LEFTUP);
                child.exited();
            }
            verify_image(fixture);
        }
        std::puts("PASS: window mode (Tab toggle, exact window copy, window + edit mode, without focus, "
                  "mid-drag switch, and return to rectangle mode).");
        std::vector<double> times;
        for (int direction = 0; direction < 4; ++direction)
        {
            SetForegroundWindow(fixture);
            Child child(arguments[1]);
            times.push_back(child.startup_ms);
            WindowChanges changes(child.window);
            const POINT from{direction & 1 ? b.x : a.x, direction & 2 ? b.y : a.y};
            const POINT to{direction & 1 ? a.x : b.x, direction & 2 ? a.y : b.y};
            mouse(from, MOUSEEVENTF_LEFTDOWN);
            mouse(to, 0);
            if (direction == 0)
            {
                verify_held_selection(child.window, to, changes);
                save_view(arguments[2]);
            }
            mouse(to, MOUSEEVENTF_LEFTUP);
            child.exited();
            verify_image(fixture);
        }
        std::puts(
            "PASS: real mouse drags in all four directions; exact colors, crop, and clipboard after exit.");
        for (int mode = 0; mode < 4; ++mode)
        {
            marker(fixture);
            const auto sequence = GetClipboardSequenceNumber();
            SetForegroundWindow(fixture);
            Child child(arguments[1]);
            if (mode == 0)
            {
                mouse(a, MOUSEEVENTF_LEFTDOWN);
                mouse(b, 0);
                escape();
                mouse(b, MOUSEEVENTF_LEFTUP);
            }
            else if (mode == 1)
            {
                mouse(a, MOUSEEVENTF_RIGHTDOWN);
                mouse(a, MOUSEEVENTF_RIGHTUP);
            }
            else if (mode == 2)
            {
                mouse(a, MOUSEEVENTF_LEFTDOWN);
                mouse(a, MOUSEEVENTF_LEFTUP);
                require(IsWindow(child.window), "An empty selection closed Pluck.");
                escape();
            }
            else
                escape();
            child.exited();
            require(GetClipboardSequenceNumber() == sequence, "Cancellation changed the clipboard.");
        }
        std::puts(
            "PASS: Escape before and during drag, right-click, and empty selections preserve the clipboard.");
        SetForegroundWindow(fixture);
        {
            Child child(arguments[1]);
            open_clipboard(fixture);
            mouse(a, MOUSEEVENTF_LEFTDOWN);
            mouse(b, MOUSEEVENTF_LEFTUP);
            pump(250);
            require(IsWindow(child.window), "A locked clipboard closed Pluck.");
            CloseClipboard();
            child.exited();
            verify_image(fixture);
        }
        std::puts("PASS: clipboard contention retries and succeeds after the lock is released.");
        {
            marker(fixture);
            const auto sequence = GetClipboardSequenceNumber();
            SetForegroundWindow(fixture);
            Child child(arguments[1]);
            open_clipboard(fixture);
            mouse(a, MOUSEEVENTF_LEFTDOWN);
            mouse(b, MOUSEEVENTF_LEFTUP);
            pump(1800);
            CloseClipboard();
            require(IsWindowVisible(child.window), "A persistent clipboard lock did not restore selection.");
            require(GetClipboardSequenceNumber() == sequence, "A busy clipboard was changed.");
            mouse(a, MOUSEEVENTF_LEFTDOWN);
            mouse(b, MOUSEEVENTF_LEFTUP);
            child.exited();
            verify_image(fixture);
        }
        std::puts("PASS: persistent clipboard contention returns to selection and permits a new capture.");
        std::vector<RECT> monitors;
        EnumDisplayMonitors(nullptr, nullptr, collect_monitor, reinterpret_cast<LPARAM>(&monitors));
        auto check_position = [&](LONG left, LONG top)
        {
            require(SetWindowPos(fixture, HWND_TOPMOST, left, top, 500, 350, SWP_NOACTIVATE),
                    "Cannot position the test pattern.");
            InvalidateRect(fixture, nullptr, TRUE);
            UpdateWindow(fixture);
            pump(100);
            RECT position{};
            GetWindowRect(fixture, &position);
            require(position.left == left && position.top == top, "A window manager moved the test pattern.");
            SetForegroundWindow(fixture);
            Child child(arguments[1]);
            mouse({position.left + 100, position.top + 80}, MOUSEEVENTF_LEFTDOWN);
            mouse({position.left + 200, position.top + 160}, MOUSEEVENTF_LEFTUP);
            child.exited();
            verify_image(fixture);
        };
        for (const auto& monitor : monitors)
        {
            check_position(monitor.left + 40, monitor.top + 40);
            std::printf("PASS: display at (%ld, %ld), %ld x %ld; exact physical crop pixels.\n", monitor.left,
                        monitor.top, monitor.right - monitor.left, monitor.bottom - monitor.top);
        }
        for (const auto& left : monitors)
            for (const auto& right : monitors)
            {
                const auto top = std::max(left.top, right.top);
                const auto bottom = std::min(left.bottom, right.bottom);
                if (left.right == right.left && bottom - top >= 400)
                {
                    check_position(right.left - 150, top + 20);
                    std::puts("PASS: one selection across adjacent displays; exact pixels on both sides.");
                }
            }
        {
            MONITORINFO monitor{sizeof(monitor)};
            GetMonitorInfoW(MonitorFromWindow(fixture, MONITOR_DEFAULTTOPRIMARY), &monitor);
            const int width = std::min(1000L, monitor.rcWork.right - monitor.rcWork.left - 40);
            const int left = monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2;
            const RECT region{left + 10, monitor.rcWork.bottom - 150, left + width - 10,
                              monitor.rcWork.bottom - 5};
            SetWindowPos(fixture, HWND_TOPMOST, left, monitor.rcWork.bottom - 200, width, 200,
                         SWP_SHOWWINDOW);
            SetForegroundWindow(fixture);
            UpdateWindow(fixture);
            pump(100);
            mouse({region.left, region.top}, 0);
            const auto expected = pluck::capture_desktop(region);
            Child child(arguments[1]);
            pump(100);
            DwmFlush();
            const auto ready = pluck::capture_desktop(region);
            save_bitmap(*ready, std::wstring(arguments[2]) + L"/settings-hint.bmp");
            const auto dimmed_matches = [&](const pluck::Bitmap& image)
            {
                for (int i = 0; i < image.width() * image.height(); ++i)
                    for (unsigned shift : {0u, 8u, 16u})
                    {
                        const auto want = ((expected->pixels()[i] >> shift) & 255u) * 135u / 255u;
                        const auto actual_pixel = (image.pixels()[i] >> shift) & 255u;
                        if (want + 1 < actual_pixel || actual_pixel + 1 < want)
                            return false;
                    }
                return true;
            };
            size_t hint_pixels{};
            for (int i = 0; i < ready->width() * ready->height(); ++i)
                if ((ready->pixels()[i] & 0x00ffffffu) == 0x00122620u)
                    ++hint_pixels;
            require(hint_pixels > 20, "The idle overlay has no settings hint.");
            edit_key();
            pump(100);
            DwmFlush();
            const auto armed = pluck::capture_desktop(region);
            save_bitmap(*armed, std::wstring(arguments[2]) + L"/edit-mode-hint.bmp");
            size_t armed_pixels{};
            for (int i = 0; i < armed->width() * armed->height(); ++i)
                if ((armed->pixels()[i] & 0x00ffffffu) == 0x0077f4d5u)
                    ++armed_pixels;
            require(armed_pixels > 20, "Edit mode did not highlight the shortcut hint.");
            edit_key();
            pump(100);
            DwmFlush();
            mouse({region.left, region.top}, MOUSEEVENTF_LEFTDOWN);
            DwmFlush();
            require(dimmed_matches(*pluck::capture_desktop(region)),
                    "The hint did not disappear on mouse-down.");
            mouse({region.left, region.top}, MOUSEEVENTF_LEFTUP);
            DwmFlush();
            require(!dimmed_matches(*pluck::capture_desktop(region)),
                    "The hint did not return after an empty click.");
            mouse({region.left, region.top}, MOUSEEVENTF_LEFTDOWN);
            mouse({region.right, region.bottom}, MOUSEEVENTF_LEFTUP);
            child.exited();
            verify_popup(fixture, *expected);
        }
        std::puts("PASS: the hint hides on mouse-down, returns after an empty click, and is absent from "
                  "copied pixels.");
        for (int i = 0; i < 6; ++i)
        {
            SetForegroundWindow(fixture);
            Child child(arguments[1]);
            times.push_back(child.startup_ms);
            escape();
            child.exited();
        }
        std::sort(times.begin(), times.end());
        std::printf("Startup to painted foreground overlay: median %.1f ms, min %.1f ms, max %.1f ms (%zu "
                    "launches).\n",
                    times[times.size() / 2], times.front(), times.back(), times.size());
    }
    catch (const std::exception& error)
    {
        INPUT release{};
        release.type = INPUT_MOUSE;
        release.mi.dwFlags = MOUSEEVENTF_LEFTUP | MOUSEEVENTF_RIGHTUP;
        SendInput(1, &release, sizeof(release));
        CloseClipboard();
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        result = 1;
    }
    DestroyWindow(fixture);
    SetCursorPos(previous_cursor.x, previous_cursor.y);
    if (IsWindow(previous_foreground))
        SetForegroundWindow(previous_foreground);
    OleUninitialize();
    return result;
}
