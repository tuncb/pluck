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
                PostMessageW(window, WM_CLOSE, 0, 0);
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
