#include "capture.h"
#include "selector.h"
#include <dwmapi.h>
#include <ole2.h>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
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
LRESULT CALLBACK pattern_proc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    if (message == WM_KEYDOWN && w == VK_ESCAPE)
    {
        ++fixture_escape_presses;
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
class WindowChanges
{
  public:
    explicit WindowChanges(HWND window)
    {
        window_ = window;
        count_ = 0;
        DWORD process{};
        const auto thread = GetWindowThreadProcessId(window, &process);
        hook_ = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
                                changed, process, thread, WINEVENT_OUTOFCONTEXT);
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
    require(before > 0, "No selection window changes were observed during the drag.");
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
void save_view(const wchar_t* directory)
{
    const auto bounds = pluck::desktop_bounds();
    const auto image = pluck::capture_desktop(bounds);
    auto data = pluck::make_clipboard_dib(*image, {0, 0, image->width(), image->height()});
    const auto* dib = static_cast<BITMAPINFOHEADER*>(GlobalLock(data.get()));
    BITMAPFILEHEADER header{};
    header.bfType = 0x4d42;
    header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
    header.bfSize = header.bfOffBits + dib->biSizeImage;
    std::ofstream file(std::wstring(directory) + L"/selection.bmp", std::ios::binary);
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(dib), sizeof(BITMAPINFOHEADER) + dib->biSizeImage);
    GlobalUnlock(data.get());
    require(file.good(), "Cannot save the selection screenshot.");
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
        std::puts("PASS: Escape before and during drag, right-click, and empty selections preserve the clipboard.");
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
