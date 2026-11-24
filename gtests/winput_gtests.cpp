#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>
#include "winput.hpp"
#include "macros.hpp"

namespace
{
struct InputCalls
{
    std::vector<INPUT> events;
    std::vector<float> sleeps;
    std::vector<Coordinates> moves;
    std::string actions;
    std::chrono::steady_clock::time_point now{};
    POINT cursor{123, -456};
    int cursorQueries = 0;
    std::size_t failMoveAt = 0;
    bool cursorSucceeds = true;
    bool sendSucceeds = true;
    bool clampCursor = false;
};

InputCalls calls;

UINT WINAPI RecordSendInput(UINT count, LPINPUT inputs, int size)
{
    calls.actions += 'I';
    EXPECT_EQ(count, 1u);
    EXPECT_EQ(size, static_cast<int>(sizeof(INPUT)));
    calls.events.insert(calls.events.end(), inputs, inputs + count);
    return calls.sendSucceeds ? count : 0;
}

BOOL WINAPI RecordGetCursorPos(LPPOINT point)
{
    ++calls.cursorQueries;
    if (!calls.cursorSucceeds)
        return FALSE;
    *point = calls.cursor;
    return TRUE;
}

BOOL WINAPI RecordSetCursorPos(int x, int y)
{
    calls.moves.emplace_back(x, y);
    if (calls.moves.size() == calls.failMoveAt) return FALSE;
    calls.cursor = {calls.clampCursor ? std::min(x, 400) : x, y};
    return TRUE;
}

HMONITOR WINAPI RecordMonitorFromPoint(POINT point, DWORD flags)
{
    EXPECT_EQ(flags, DWORD(MONITOR_DEFAULTTONULL));
    constexpr RECT displays[] = {{0, -500, 1920, 1080}, {-1920, -400, 0, 600}};
    for (const auto& display : displays)
        if (PtInRect(&display, point)) return reinterpret_cast<HMONITOR>(1);
    return nullptr;
}

void RecordSleep(float seconds)
{
    calls.actions += 'S';
    calls.sleeps.push_back(seconds);
    calls.now += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<float>(seconds));
}

namespace RecordedChrono
{
using std::chrono::duration;
using std::chrono::milliseconds;

struct steady_clock
{
    static std::chrono::steady_clock::time_point now()
    {
        return calls.now;
    }
};
}
}

// Record Windows calls and advance a virtual clock instead of sending input or waiting.
// tea_core is static, so its winput object is not needed by this test executable.
#define SendInput RecordSendInput
#define GetCursorPos RecordGetCursorPos
#define SetCursorPos RecordSetCursorPos
#define MonitorFromPoint RecordMonitorFromPoint
#define SleepThread RecordSleep
#define chrono RecordedChrono
#include "winput.cpp"
#undef chrono
#undef SleepThread
#undef MonitorFromPoint
#undef SetCursorPos
#undef GetCursorPos
#undef SendInput

namespace
{
class WinputTests : public ::testing::Test
{
protected:
    INPUT input{};
    POINT mouse{11, 22};
    std::random_device random;
    Winput winput{input, mouse, random};
    std::ostringstream errors;
    std::streambuf* oldErrors = nullptr;

    void SetUp() override
    {
        calls = InputCalls{};
        oldErrors = std::cerr.rdbuf(errors.rdbuf());
    }

    void TearDown() override
    {
        std::cerr.rdbuf(oldErrors);
    }

    void ExpectKeyPair(std::size_t offset, WORD key)
    {
        ASSERT_GE(calls.events.size(), offset + 2);
        const auto& down = calls.events[offset];
        const auto& up = calls.events[offset + 1];
        EXPECT_EQ(down.type, static_cast<DWORD>(INPUT_KEYBOARD));
        EXPECT_EQ(up.type, static_cast<DWORD>(INPUT_KEYBOARD));
        EXPECT_EQ(down.ki.wVk, key);
        EXPECT_EQ(up.ki.wVk, key);
        EXPECT_EQ(down.ki.dwFlags, 0u);
        EXPECT_EQ(up.ki.dwFlags, static_cast<DWORD>(KEYEVENTF_KEYUP));
        ASSERT_GE(calls.actions.size(), offset * 2 + 4);
        EXPECT_EQ(calls.actions.substr(offset * 2, 4), "SISI");
    }

    void ExpectDelays(float minimum, float maximum, std::size_t count)
    {
        ASSERT_EQ(calls.sleeps.size(), count);
        const float midpoint = (minimum + maximum) / 2.0f;
        const float tolerance = (maximum - minimum) / 2.0f + 0.000001f;
        for (float delay : calls.sleeps)
            EXPECT_NEAR(delay, midpoint, tolerance);
    }
};

TEST_F(WinputTests, ConstructorLeavesExternalStateUntouchedAndDoesNotSendInput)
{
    static_assert(!std::is_default_constructible<Winput>::value);
    INPUT supplied{};
    supplied.type = INPUT_HARDWARE;
    supplied.hi.uMsg = 123;
    POINT position{-12, 34};
    Winput constructed(supplied, position, random);

    EXPECT_EQ(supplied.type, static_cast<DWORD>(INPUT_HARDWARE));
    EXPECT_EQ(supplied.hi.uMsg, 123u);
    EXPECT_EQ(position.x, -12);
    EXPECT_EQ(position.y, 34);
    EXPECT_TRUE(calls.events.empty());
    EXPECT_TRUE(calls.moves.empty());
    EXPECT_TRUE(calls.sleeps.empty());
    EXPECT_EQ(calls.cursorQueries, 0);

    constructed.RandomKeyPress('a', 0.0f, 0.0f);
    EXPECT_EQ(supplied.type, static_cast<DWORD>(INPUT_KEYBOARD));
    EXPECT_EQ(supplied.ki.wVk, 'A');
    EXPECT_EQ(supplied.ki.dwFlags, static_cast<DWORD>(KEYEVENTF_KEYUP));
}

TEST_F(WinputTests, RandomKeyPressHandlesEverySupportedAsciiCharacter)
{
    for (int character = 0; character <= 127; ++character)
    {
        SCOPED_TRACE(character);
        calls = InputCalls{};
        winput.RandomKeyPress(static_cast<char>(character), 0.0f, 0.0f);

        const WORD expected = character >= 'a' && character <= 'z'
            ? character - 'a' + 'A' : character;
        ASSERT_EQ(calls.events.size(), 2u);
        ExpectKeyPair(0, expected);
        ExpectDelays(0.0f, 0.0f, 2);
        EXPECT_EQ(input.ki.wVk, expected);
        EXPECT_EQ(input.ki.dwFlags, static_cast<DWORD>(KEYEVENTF_KEYUP));
    }
    EXPECT_TRUE(errors.str().empty());
}

TEST_F(WinputTests, RandomKeyPressUsesDefaultKeyAndDelayRange)
{
    winput.RandomKeyPress();
    ASSERT_EQ(calls.events.size(), 2u);
    ExpectKeyPair(0, 0);
    ExpectDelays(DEFAULT_RANDOM_MIN, DEFAULT_RANDOM_MAX, 2);
}

TEST_F(WinputTests, RandomKeyPressUsesCustomDelayRange)
{
    for (int i = 0; i < 32; ++i)
        winput.RandomKeyPress('z', 0.02f, 0.07f);

    ASSERT_EQ(calls.events.size(), 64u);
    for (std::size_t offset = 0; offset < calls.events.size(); offset += 2)
        ExpectKeyPair(offset, 'Z');
    ExpectDelays(0.02f, 0.07f, 64);
}

TEST_F(WinputTests, RandomKeyPressAcceptsEqualPositiveDelayBounds)
{
    winput.RandomKeyPress('a', 0.125f, 0.125f);
    ASSERT_EQ(calls.events.size(), 2u);
    ExpectKeyPair(0, 'A');
    ExpectDelays(0.125f, 0.125f, 2);
}

TEST_F(WinputTests, RandomKeyPressStillAttemptsReleaseWhenSendInputFails)
{
    calls.sendSucceeds = false;
    EXPECT_NO_THROW(winput.RandomKeyPress('a', 0.0f, 0.0f));
    ASSERT_EQ(calls.events.size(), 2u);
    ExpectKeyPair(0, 'A');
}

TEST_F(WinputTests, KeyPressOverwritesStaleKeyboardTypeKeyAndFlags)
{
    input.type = INPUT_MOUSE;
    input.ki.wVk = 'Z';
    input.ki.dwFlags = KEYEVENTF_KEYUP | KEYEVENTF_EXTENDEDKEY;

    winput.RandomKeyPress('b', 0.0f, 0.0f);
    ASSERT_EQ(calls.events.size(), 2u);
    ExpectKeyPair(0, 'B');
}

TEST_F(WinputTests, RandomMousePressHandlesBothButtonsAndOverwritesStaleFlags)
{
    for (int button : {0, 1})
    {
        SCOPED_TRACE(button);
        calls = InputCalls{};
        input.type = INPUT_KEYBOARD;
        input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_WHEEL;
        winput.RandomMousePress(button, 0.0f, 0.0f);

        ASSERT_EQ(calls.events.size(), 2u);
        EXPECT_EQ(calls.events[0].type, static_cast<DWORD>(INPUT_MOUSE));
        EXPECT_EQ(calls.events[1].type, static_cast<DWORD>(INPUT_MOUSE));
        const DWORD down = button == 0 ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_RIGHTDOWN;
        const DWORD up = button == 0 ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_RIGHTUP;
        EXPECT_EQ(calls.events[0].mi.dwFlags, down);
        EXPECT_EQ(calls.events[1].mi.dwFlags, up);
        EXPECT_EQ(input.type, static_cast<DWORD>(INPUT_MOUSE));
        EXPECT_EQ(input.mi.dwFlags, up);
        EXPECT_EQ(calls.actions, "SISI");
        ExpectDelays(0.0f, 0.0f, 2);
    }
    EXPECT_TRUE(errors.str().empty());
}

TEST_F(WinputTests, RandomMousePressDefaultsToLeftButtonAndDefaultDelayRange)
{
    winput.RandomMousePress();
    ASSERT_EQ(calls.events.size(), 2u);
    EXPECT_EQ(calls.events[0].mi.dwFlags, static_cast<DWORD>(MOUSEEVENTF_LEFTDOWN));
    EXPECT_EQ(calls.events[1].mi.dwFlags, static_cast<DWORD>(MOUSEEVENTF_LEFTUP));
    EXPECT_EQ(calls.actions, "SISI");
    ExpectDelays(DEFAULT_RANDOM_MIN, DEFAULT_RANDOM_MAX, 2);
}

TEST_F(WinputTests, RandomMousePressUsesCustomAndFixedDelayRanges)
{
    winput.RandomMousePress(1, 0.03f, 0.08f);
    ExpectDelays(0.03f, 0.08f, 2);
    calls.sleeps.clear();

    winput.RandomMousePress(0, 0.125f, 0.125f);
    ExpectDelays(0.125f, 0.125f, 2);
}

TEST_F(WinputTests, InvalidMouseButtonsReportBothErrorsWithoutSendingOrChangingInput)
{
    for (int button : {-1, 2, (std::numeric_limits<int>::min)(),
                       (std::numeric_limits<int>::max)()})
    {
        SCOPED_TRACE(button);
        calls = InputCalls{};
        errors.str("");
        input.type = INPUT_HARDWARE;
        input.hi.uMsg = 456;
        const INPUT before = input;

        winput.RandomMousePress(button, 0.0f, 0.0f);

        EXPECT_TRUE(calls.events.empty());
        EXPECT_EQ(std::memcmp(&input, &before, sizeof(INPUT)), 0);
        EXPECT_EQ(errors.str(), "Invalid 'MouseDown()' parameter.\n"
                                "Invalid 'MouseUp()' parameter.\n");
        EXPECT_EQ(calls.actions, "SS");
        ExpectDelays(0.0f, 0.0f, 2);
    }
}

TEST_F(WinputTests, RandomMousePressStillAttemptsReleaseWhenSendInputFails)
{
    calls.sendSucceeds = false;
    EXPECT_NO_THROW(winput.RandomMousePress(1, 0.0f, 0.0f));
    ASSERT_EQ(calls.events.size(), 2u);
    EXPECT_EQ(calls.events[0].mi.dwFlags, static_cast<DWORD>(MOUSEEVENTF_RIGHTDOWN));
    EXPECT_EQ(calls.events[1].mi.dwFlags, static_cast<DWORD>(MOUSEEVENTF_RIGHTUP));
}

TEST_F(WinputTests, TypeStringSendsMixedCaseLettersDigitsAndSpacesInOrder)
{
    const std::string text = "aZ 019";
    const std::string expected = "AZ 019";
    winput.TypeString(text.c_str());

    ASSERT_EQ(calls.events.size(), text.size() * 2);
    for (std::size_t i = 0; i < text.size(); ++i)
        ExpectKeyPair(i * 2, expected[i]);
    ExpectDelays(0.001f, 0.005f, text.size() * 2);
    EXPECT_EQ(text, "aZ 019");
}

TEST_F(WinputTests, TypeStringHandlesSingleCharacterAndStopsAtFirstNull)
{
    const char text[] = {'q', '\0', 'x', '\0'};
    winput.TypeString(text);
    ASSERT_EQ(calls.events.size(), 2u);
    ExpectKeyPair(0, 'Q');
    ExpectDelays(0.001f, 0.005f, 2);
}

TEST_F(WinputTests, TypeStringHandlesEmptyStringWithoutChangingInput)
{
    const INPUT before = input;
    winput.TypeString("");
    EXPECT_TRUE(calls.events.empty());
    EXPECT_TRUE(calls.sleeps.empty());
    EXPECT_EQ(std::memcmp(&input, &before, sizeof(INPUT)), 0);
}

TEST_F(WinputTests, TypeStringHandlesLongInputWithoutDroppingCharacters)
{
    const std::string text(1024, 'x');
    winput.TypeString(text.c_str());
    ASSERT_EQ(calls.events.size(), text.size() * 2);
    for (std::size_t i = 0; i < text.size(); ++i)
        ExpectKeyPair(i * 2, 'X');
    ExpectDelays(0.001f, 0.005f, text.size() * 2);
}

TEST_F(WinputTests, TypeStringContinuesAfterFailedInputSubmission)
{
    calls.sendSucceeds = false;
    EXPECT_NO_THROW(winput.TypeString("ab"));
    ASSERT_EQ(calls.events.size(), 4u);
    ExpectKeyPair(0, 'A');
    ExpectKeyPair(2, 'B');
}

TEST_F(WinputTests, GetMousePosReturnsAndUpdatesExternalPointOnEveryCall)
{
    const LONG lowest = (std::numeric_limits<LONG>::min)();
    const LONG highest = (std::numeric_limits<LONG>::max)();
    const POINT positions[] = {{0, 0}, {100, 200}, {-100, -200},
                              {-1, 1}, {lowest, highest}, {highest, lowest}};
    for (const auto& position : positions)
    {
        calls.cursor = position;
        const auto result = winput.GetMousePos();
        EXPECT_EQ(result.pos_x, position.x);
        EXPECT_EQ(result.pos_y, position.y);
        EXPECT_EQ(mouse.x, position.x);
        EXPECT_EQ(mouse.y, position.y);
    }
    EXPECT_EQ(calls.cursorQueries, 6);
    EXPECT_TRUE(calls.events.empty());
    EXPECT_TRUE(calls.moves.empty());
}

TEST_F(WinputTests, GetMousePosThrowsOnFailureAndCanRecover)
{
    calls.cursorSucceeds = false;
    try
    {
        winput.GetMousePos();
        FAIL() << "Expected GetCursorPos failure to throw";
    }
    catch (const std::runtime_error& error)
    {
        EXPECT_STREQ(error.what(), "GetCursorPos failed.");
    }
    EXPECT_EQ(calls.cursorQueries, 1);
    EXPECT_EQ(mouse.x, 11);
    EXPECT_EQ(mouse.y, 22);

    calls.cursorSucceeds = true;
    EXPECT_EQ(winput.GetMousePos(), Coordinates(123, -456));
    EXPECT_EQ(calls.cursorQueries, 2);
}

TEST_F(WinputTests, RealisticMouseMoveDoesNothingForIdenticalEndpoints)
{
    const Coordinates positions[] = {{}, {12, -34},
        {(std::numeric_limits<int>::min)(), (std::numeric_limits<int>::max)()}};
    for (const auto& position : positions)
        winput.RealisticMouseMove(position, position);

    EXPECT_TRUE(calls.moves.empty());
    EXPECT_TRUE(calls.sleeps.empty());
    EXPECT_TRUE(calls.events.empty());
    EXPECT_EQ(calls.cursorQueries, 0);
    EXPECT_TRUE(errors.str().empty());
}

struct MoveCase
{
    Coordinates from;
    Coordinates to;
};

void PrintTo(const MoveCase& move, std::ostream* out)
{
    *out << move.from << " to " << move.to;
}

class MouseMovementTests : public WinputTests, public ::testing::WithParamInterface<MoveCase> {};

TEST_P(MouseMovementTests, FollowsBoundedPathAndFinishesAtExactDestination)
{
    const auto test = GetParam();
    winput.RealisticMouseMove(test.from, test.to);

    ASSERT_GE(calls.moves.size(), 2u);
    EXPECT_EQ(calls.moves.front(), test.from);
    EXPECT_EQ(calls.moves.back(), test.to);
    const double distance = std::hypot(double(test.to.pos_x) - test.from.pos_x,
                                      double(test.to.pos_y) - test.from.pos_y);
    const double margin = distance * 0.5 + 13.0;
    const double centerX = (double(test.from.pos_x) + test.to.pos_x) / 2.0;
    const double centerY = (double(test.from.pos_y) + test.to.pos_y) / 2.0;
    const double radiusX = std::abs(double(test.to.pos_x) - test.from.pos_x) / 2.0 + margin;
    const double radiusY = std::abs(double(test.to.pos_y) - test.from.pos_y) / 2.0 + margin;
    for (const auto& position : calls.moves)
    {
        EXPECT_NEAR(double(position.pos_x), centerX, radiusX);
        EXPECT_NEAR(double(position.pos_y), centerY, radiusY);
    }
    ASSERT_FALSE(calls.sleeps.empty());
    for (float delay : calls.sleeps)
        EXPECT_NEAR(delay, 0.008f, 0.000001f);
    EXPECT_TRUE(calls.events.empty());
    EXPECT_EQ(calls.cursorQueries, 0);
    EXPECT_TRUE(errors.str().empty());
}

INSTANTIATE_TEST_SUITE_P(DirectionsAndDistances, MouseMovementTests, ::testing::Values(
    MoveCase{{0, 0}, {1, 0}}, MoveCase{{0, 0}, {0, -1}},
    MoveCase{{-100, 20}, {100, 20}}, MoveCase{{20, 100}, {20, -100}},
    MoveCase{{-50, -75}, {200, 300}}, MoveCase{{200, 300}, {-50, -75}},
    MoveCase{{-1000000, -1000000}, {1000000, 1000000}}));

TEST_F(WinputTests, ExtremeCoordinatesStayNearThePathAndReachTheirDestination)
{
    constexpr int minimum = (std::numeric_limits<int>::min)();
    constexpr int maximum = (std::numeric_limits<int>::max)();
    const MoveCase paths[] = {{{maximum - 1000, 0}, {maximum, 0}},
        {{minimum + 1000, 0}, {minimum, 0}}, {{0, maximum - 1000}, {0, maximum}},
        {{0, minimum + 1000}, {0, minimum}}, {{maximum, 0}, {maximum, 1000}},
        {{0, minimum}, {1000, minimum}}};
    for (const auto& path : paths)
    {
        SCOPED_TRACE(::testing::PrintToString(path));
        for (int trial = 0; trial < 16; ++trial)
        {
            calls.moves.clear();
            ASSERT_TRUE(winput.RealisticMouseMove(path.from, path.to, [](auto) { return true; }));
            ASSERT_GE(calls.moves.size(), 2u);
            EXPECT_EQ(calls.moves.front(), path.from);
            EXPECT_EQ(calls.moves.back(), path.to);
            for (const auto& position : calls.moves)
            {
                EXPECT_NEAR(double(position.pos_x), double(path.to.pos_x), 2000.0);
                EXPECT_NEAR(double(position.pos_y), double(path.to.pos_y), 2000.0);
            }
        }
    }
}

TEST_F(WinputTests, RealisticMouseMoveStopsImmediatelyIfFirstCursorUpdateFails)
{
    calls.failMoveAt = 1;
    winput.RealisticMouseMove({10, 20}, {500, 600});

    ASSERT_EQ(calls.moves.size(), 1u);
    EXPECT_EQ(calls.moves.front(), Coordinates(10, 20));
    EXPECT_TRUE(calls.sleeps.empty());
    EXPECT_EQ(errors.str(), "SetCursorPos failed.\n");
}

TEST_F(WinputTests, RealisticMouseMoveStopsOnLaterFailureAndCanRecover)
{
    calls.failMoveAt = 2;
    winput.RealisticMouseMove({10, 20}, {500, 600});

    ASSERT_EQ(calls.moves.size(), 2u);
    ASSERT_EQ(calls.sleeps.size(), 1u);
    EXPECT_NEAR(calls.sleeps.front(), 0.008f, 0.000001f);
    EXPECT_EQ(errors.str(), "SetCursorPos failed.\n");

    calls = InputCalls{};
    errors.str("");
    winput.RealisticMouseMove({10, 20}, {11, 20});
    ASSERT_FALSE(calls.moves.empty());
    EXPECT_EQ(calls.moves.back(), Coordinates(11, 20));
    EXPECT_TRUE(errors.str().empty());
}

TEST_F(WinputTests, RealisticMouseMoveCanCancelBeforeOrDuringMovement)
{
    EXPECT_FALSE(winput.RealisticMouseMove({10, 20}, {500, 600}, [](auto) { return false; }));
    EXPECT_TRUE(calls.moves.empty());

    int frames = 0;
    EXPECT_FALSE(winput.RealisticMouseMove({10, 20}, {500, 600}, [&](auto delay) {
        return delay.count() == 0 || ++frames < 4;
    }));
    ASSERT_EQ(calls.moves.size(), 4u);
    EXPECT_EQ(calls.moves.front(), Coordinates(10, 20));
    EXPECT_FALSE(calls.moves.back() == Coordinates(500, 600));
    EXPECT_TRUE(calls.sleeps.empty());
}

TEST_F(WinputTests, RealisticMouseMoveDoesNotSkipAheadAfterAPause)
{
    bool paused = false;
    EXPECT_TRUE(winput.RealisticMouseMove({10, 20}, {500, 600}, [&](auto delay) {
        if (!paused && delay.count() > 0)
        {
            calls.now += std::chrono::hours(1);
            paused = true;
        }
        EXPECT_TRUE(delay.count() == 0 || delay.count() == 8);
        return true;
    }));
    ASSERT_GT(calls.moves.size(), 20u);
    EXPECT_EQ(calls.moves.front(), Coordinates(10, 20));
    EXPECT_EQ(calls.moves.back(), Coordinates(500, 600));
    for (std::size_t i = 1; i < calls.moves.size(); ++i)
        EXPECT_LT(std::hypot(double(calls.moves[i].pos_x) - calls.moves[i - 1].pos_x,
                             double(calls.moves[i].pos_y) - calls.moves[i - 1].pos_y), 100.0);
}

TEST_F(WinputTests, NativeMacroPlaybackUsesRealisticMovementFromCurrentCursorPosition)
{
    MacroAction move;
    move.type = MacroAction::Type::Move;
    move.position = {500, 600};
    Macro macro("move", {move});
    EXPECT_TRUE(macro.Execute(*WindowsMacroOutput(), [](auto) { return true; }));
    EXPECT_EQ(calls.cursorQueries, 2);
    ASSERT_GT(calls.moves.size(), 20u);
    EXPECT_EQ(calls.moves.front(), Coordinates(123, -456));
    EXPECT_EQ(calls.moves.back(), move.position);
    EXPECT_TRUE(calls.events.empty());
}

TEST_F(WinputTests, NativeMacroPlaybackRejectsOffDisplayDestinationsBeforeMoving)
{
    constexpr int minimum = (std::numeric_limits<int>::min)();
    constexpr int maximum = (std::numeric_limits<int>::max)();
    const Coordinates destinations[] = {{minimum, maximum}, {maximum, minimum},
        {1920, 0}, {0, 1080}, {-500, 700}, {-500, -500}};
    const auto output = WindowsMacroOutput();
    for (const auto& destination : destinations)
    {
        SCOPED_TRACE(::testing::PrintToString(destination));
        EXPECT_THROW(output->Move(destination, [](auto) { return true; }), std::runtime_error);
    }
    EXPECT_TRUE(calls.moves.empty());
    EXPECT_EQ(calls.cursorQueries, 0);

    EXPECT_FALSE(output->Move(destinations[0], [](auto) { return false; }));
    EXPECT_TRUE(calls.moves.empty());
}

TEST_F(WinputTests, NativeMacroPlaybackCanMoveToNegativeCoordinatesOnADisplay)
{
    const Coordinates destination(-500, -300);
    EXPECT_TRUE(WindowsMacroOutput()->Move(destination, [](auto) { return true; }));
    ASSERT_FALSE(calls.moves.empty());
    EXPECT_EQ(calls.moves.back(), destination);
}

TEST_F(WinputTests, NativeMacroPlaybackCancelsMovementAndSkipsFollowingActions)
{
    MacroAction move;
    move.type = MacroAction::Type::Move;
    move.position = {500, 600};
    Macro macro("move", {move, move});
    int frames = 0;
    EXPECT_FALSE(macro.Execute(*WindowsMacroOutput(), [&](auto delay) {
        return delay.count() == 0 || ++frames < 4;
    }));
    EXPECT_EQ(calls.cursorQueries, 1);
    ASSERT_EQ(calls.moves.size(), 4u);
    EXPECT_FALSE(calls.moves.back() == move.position);
}

TEST_F(WinputTests, NativeMacroPlaybackReportsCursorReadAndMovementFailures)
{
    const auto output = WindowsMacroOutput();
    calls.cursorSucceeds = false;
    EXPECT_THROW(output->Move({500, 600}, [](auto) { return true; }), std::runtime_error);
    EXPECT_TRUE(calls.moves.empty());
    calls.cursorSucceeds = true;
    calls.failMoveAt = 2;
    EXPECT_THROW(output->Move({500, 600}, [](auto) { return true; }), std::runtime_error);
    EXPECT_EQ(calls.moves.size(), 2u);
}

TEST_F(WinputTests, NativeMacroPlaybackRejectsClampedCursorPosition)
{
    calls.clampCursor = true;
    EXPECT_THROW(WindowsMacroOutput()->Move({500, 600}, [](auto) { return true; }), std::runtime_error);
    EXPECT_EQ(calls.cursor.x, 400);
}
}
