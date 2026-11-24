#include <gtest/gtest.h>
#include <algorithm>
#include <dwmapi.h>
#include <future>
#include "2d/capture.hpp"
#include "macros.hpp"

using namespace std::chrono_literals;

namespace
{
struct DpiContext
{
    DPI_AWARENESS_CONTEXT previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ~DpiContext() { if (previous) SetThreadDpiAwarenessContext(previous); }
};

class RecordingWindow
{
    DpiContext dpi;
    POINT original{};
    bool haveCursor = false, sentInput = false;
    ATOM atom = 0;
    static constexpr const wchar_t* name = L"TeaMouseRecordingTest";

    static LRESULT CALLBACK Procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (message == WM_NCCREATE)
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
        auto* window = reinterpret_cast<RecordingWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (window && (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN))
        {
            POINT point{short(LOWORD(lparam)), short(HIWORD(lparam))};
            ClientToScreen(hwnd, &point);
            window->clicks.push_back({message == WM_RBUTTONDOWN ? 1 : 0, {point.x, point.y}});
            return 0;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

public:
    HWND hwnd = nullptr;
    Coordinates first, second;
    std::vector<std::pair<int, Coordinates>> clicks;

    RecordingWindow()
    {
        haveCursor = GetCursorPos(&original) != FALSE;
        MONITORINFO monitor{sizeof(monitor)};
        if (!haveCursor || !GetMonitorInfoW(MonitorFromPoint(original, MONITOR_DEFAULTTONEAREST), &monitor)) return;
        WNDCLASSW type{};
        type.lpfnWndProc = Procedure; type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = name;
        atom = RegisterClassW(&type);
        if (!atom) return;
        const int x = std::clamp(int(original.x) - 100, int(monitor.rcWork.left), int(monitor.rcWork.right) - 220);
        const int y = std::clamp(int(original.y) - 60, int(monitor.rcWork.top), int(monitor.rcWork.bottom) - 140);
        hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, name, L"tea mouse recording test",
            WS_POPUP, x, y, 220, 140, nullptr, nullptr, type.hInstance, this);
        first = {x + 50, y + 50}; second = {x + 160, y + 90};
        if (hwnd) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    }

    ~RecordingWindow()
    {
        if (sentInput)
        {
            INPUT release[2]{};
            for (auto& input : release) { input.type = INPUT_MOUSE; input.mi.dwExtraInfo = 0x544541; }
            release[0].mi.dwFlags = MOUSEEVENTF_LEFTUP; release[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
            SendInput(2, release, sizeof(INPUT));
        }
        if (hwnd) DestroyWindow(hwnd);
        if (atom) UnregisterClassW(name, GetModuleHandleW(nullptr));
        if (haveCursor) SetCursorPos(original.x, original.y);
    }

    static void Drain()
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        { TranslateMessage(&message); DispatchMessageW(&message); }
    }

    bool Until(const std::function<bool()>& done)
    {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!done() && std::chrono::steady_clock::now() < deadline)
        { Drain(); MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT); }
        Drain();
        return done();
    }

    bool RunInput(const std::function<bool()>& send)
    {
        auto result = std::async(std::launch::async, [&] {
            DpiContext dpi;
            return send();
        });
        // The thread that installed the low-level hooks must pump messages during input delivery.
        return Until([&] { return result.wait_for(0ms) == std::future_status::ready; }) && result.get();
    }

    bool Click(Coordinates point, bool right)
    {
        return RunInput([&] {
            if (!SetCursorPos(point.pos_x, point.pos_y)) return false;
            POINT actual{};
            if (!GetCursorPos(&actual) || actual.x != point.pos_x || actual.y != point.pos_y ||
                GetAncestor(WindowFromPoint(actual), GA_ROOT) != hwnd) return false;
            INPUT inputs[2]{};
            for (auto& input : inputs) input.type = INPUT_MOUSE;
            inputs[0].mi.dwFlags = right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
            inputs[1].mi.dwFlags = right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
            sentInput = true;
            return SendInput(2, inputs, sizeof(INPUT)) == 2;
        });
    }
};

TEST(RecorderNativeTests, RecordsActualMouseHooksAndReplaysClicksAtTheirPositions)
{
    if ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000)
        GTEST_SKIP() << "release the mouse buttons to run the native recording check";
    RecordingWindow window;
    ASSERT_NE(window.hwnd, nullptr);
    std::vector<MacroAction> actions;
    MacroRecorder recorder([&](const auto& action) { actions.push_back(action); }, [] {});
    recorder.Start(VK_ESCAPE, VK_F10, {false, true, false, true}, 256);
    ASSERT_TRUE(window.Click(window.first, false));
    ASSERT_TRUE(window.Until([&] { return actions.size() >= 2 && window.clicks.size() >= 1; })) << "events: " << actions.size() << ", clicks: " << window.clicks.size();
    ASSERT_TRUE(window.Click(window.second, true));
    ASSERT_TRUE(window.Until([&] { return actions.size() >= 4 && window.clicks.size() >= 2; })) << "events: " << actions.size() << ", clicks: " << window.clicks.size();
    ASSERT_EQ(actions.size(), 4u);
    const auto recordedClicks = window.clicks;
    ASSERT_EQ(recordedClicks.size(), 2u);
    EXPECT_EQ(recordedClicks[0].first, 0);
    EXPECT_EQ(recordedClicks[1].first, 1);
    EXPECT_EQ(actions[0].position, recordedClicks[0].second);
    EXPECT_EQ(actions[2].position, recordedClicks[1].second);
    EXPECT_TRUE(actions[0].positioned && actions[2].positioned);
    window.clicks.clear();
    const Macro recorded("recorded clicks", actions);
    EXPECT_TRUE(window.RunInput([&] {
        return recorded.Execute(*WindowsMacroOutput(), [](auto duration) {
            std::this_thread::sleep_for(duration); return true;
        });
    }));
    ASSERT_TRUE(window.Until([&] { return window.clicks.size() >= 2; }));
    EXPECT_EQ(actions.size(), 4u) << "tea playback was captured recursively";
    recorder.Stop();
    EXPECT_EQ(window.clicks, recordedClicks);
}

TEST(ImageMoveNativeTests, MovesTheWindowsCursorInsideADetectedImage)
{
    RecordingWindow window;
    ASSERT_NE(window.hwnd, nullptr);
    UpdateWindow(window.hwnd);
    const RECT tiles[] = {{16, 16, 140, 104}, {27, 27, 82, 91}, {101, 53, 134, 99}};
    const COLORREF colors[] = {RGB(147, 37, 217), RGB(39, 230, 111), RGB(244, 191, 62)};
    const auto dc = GetDC(window.hwnd);
    ASSERT_NE(dc, nullptr);
    const auto brush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
    for (int index = 0; index < 3; ++index)
    {
        SetDCBrushColor(dc, colors[index]);
        EXPECT_NE(FillRect(dc, &tiles[index], brush), 0);
    }
    GdiFlush();
    ReleaseDC(window.hwnd, dc);
    DwmFlush();
    POINT origin{tiles[0].left, tiles[0].top};
    ASSERT_TRUE(ClientToScreen(window.hwnd, &origin));
    const tea::vision::Rect bounds{origin.x, origin.y, tiles[0].right - tiles[0].left, tiles[0].bottom - tiles[0].top};
    const auto image = tea::vision::CaptureScreen(bounds);
    ASSERT_EQ(image.pixels.front().red, GetRValue(colors[0]));

    MacroAction move;
    move.type = MacroAction::Type::Move;
    move.imageId = 1;
    const auto deadline = std::chrono::steady_clock::now() + 2500ms;
    ASSERT_TRUE(Macro("image move", {move}).Execute(*WindowsMacroOutput(), [deadline](auto duration) {
        if (duration >= deadline - std::chrono::steady_clock::now()) return false;
        std::this_thread::sleep_for(duration);
        return true;
    }, [&](int) { return image; }));

    POINT actual{};
    ASSERT_TRUE(GetCursorPos(&actual));
    EXPECT_LE(std::abs(actual.x - (bounds.x + bounds.width / 2)), 10);
    EXPECT_LE(std::abs(actual.y - (bounds.y + bounds.height / 2)), 10);
}

}
