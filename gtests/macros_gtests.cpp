#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include "macros.hpp"

using namespace std::chrono_literals;

namespace
{
MacroAction Key(UINT key = 'A', UINT modifiers = 0, int milliseconds = 0)
{
    MacroAction action;
    action.type = MacroAction::Type::Key;
    action.key = key;
    action.modifiers = modifiers;
    action.duration = std::chrono::milliseconds(milliseconds);
    return action;
}

std::shared_ptr<const Macro> Sample(std::vector<MacroAction> actions = {Key()}, Macro::Type type = Macro::Type::NoRepeat)
{
    return std::make_shared<Macro>("sample", std::move(actions), type);
}

class Output : public MacroOutput
{
public:
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::string> events;
    std::atomic<HWND> foreground{nullptr};
    std::atomic_bool foregroundOnce{false};
    UINT heldKey = 0;
    int failAt = -1;
    void Record(std::string value)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (static_cast<int>(events.size()) == failAt)
        { failAt = -1; throw std::runtime_error("output failed"); }
        events.push_back(std::move(value));
        changed.notify_all();
    }
    void Key(UINT key, bool down) override { Record("key:" + std::to_string(key) + (down ? ":down" : ":up")); }
    void Mouse(int button, bool down) override { Record("mouse:" + std::to_string(button) + (down ? ":down" : ":up")); }
    void Character(wchar_t value, bool down) override { Record("text:" + std::to_string(value) + (down ? ":down" : ":up")); }
    bool Move(Coordinates value, const Wait&) override { Record("move:" + std::to_string(value.pos_x) + ":" + std::to_string(value.pos_y)); return true; }
    bool IsDown(UINT key) override { return key == heldKey; }
    bool IsForeground(HWND window) override { return foregroundOnce.exchange(false) || window == foreground; }
    bool WaitFor(std::size_t count)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, 2s, [&] { return events.size() >= count; });
    }
};

TEST(TriggerTests, RejectsReservedKeysModifiersAndUnsupportedFlags)
{
    for (UINT key : {0u, 255u, 512u, UINT(VK_ESCAPE), UINT(VK_F12), UINT(VK_SHIFT),
                     UINT(VK_LCONTROL), UINT(VK_RMENU), UINT(VK_LWIN)})
        EXPECT_FALSE(Trigger::Valid({key, 0})) << key;
    EXPECT_FALSE(Trigger::Valid({'A', MOD_WIN}));
    EXPECT_FALSE(Trigger::Valid({'A', MOD_NOREPEAT}));
    EXPECT_TRUE(Trigger::Valid({'A', MOD_CONTROL | MOD_ALT | MOD_SHIFT}));
    EXPECT_TRUE(Trigger::Valid({VK_F1, 0}));
    EXPECT_TRUE(Trigger::Valid({0x100 | VK_RETURN, 0}));
}

TEST(MacroTests, RejectsEmptyOversizedAndInvalidDefinitions)
{
    EXPECT_THROW(Macro("", {Key()}), std::invalid_argument);
    EXPECT_THROW(Macro(std::string(129, 'x'), {Key()}), std::invalid_argument);
    EXPECT_THROW(Macro("x", {}), std::invalid_argument);
    EXPECT_THROW(Macro("x", std::vector<MacroAction>(257, Key())), std::invalid_argument);
    for (int duration : {-1, 60001}) EXPECT_THROW(Sample({Key('A', 0, duration)}), std::invalid_argument);
    EXPECT_THROW(Sample({Key(0)}), std::invalid_argument);
    EXPECT_THROW(Sample({Key('A', MOD_WIN)}), std::invalid_argument);
    MacroAction action;
    action.type = MacroAction::Type::Text;
    EXPECT_THROW(Sample({action}), std::invalid_argument);
    action.text.assign(65537, L'x');
    EXPECT_THROW(Sample({action}), std::invalid_argument);
    action.type = MacroAction::Type::Mouse;
    for (int button : {-1, 2}) { action.button = button; EXPECT_THROW(Sample({action}), std::invalid_argument); }
    action.type = static_cast<MacroAction::Type>(100);
    EXPECT_THROW(Sample({action}), std::invalid_argument);
    EXPECT_NO_THROW(Macro(std::string(128, 'x'), std::vector<MacroAction>(256, Key())));
    EXPECT_NO_THROW(Sample({Key('A', 0, 60000)}));
}

TEST(MacroTests, ExecutesKeysModifiersMouseTextWaitAndMoveInOrder)
{
    MacroAction mouse, text, pause, move;
    mouse.type = MacroAction::Type::Mouse; mouse.button = 1;
    text.type = MacroAction::Type::Text; text.text = L"Az\u00e9";
    pause.duration = 125ms;
    move.type = MacroAction::Type::Move; move.position = {-200, 350};
    auto macro = Sample({Key('C', MOD_CONTROL, 30), mouse, text, pause, move});
    Output output;
    std::vector<std::chrono::milliseconds> waits;
    EXPECT_TRUE(macro->Execute(output, [&](auto duration) { waits.push_back(duration); return true; }));
    EXPECT_EQ(output.events, (std::vector<std::string>{"key:17:down", "key:67:down", "key:67:up", "key:17:up",
        "mouse:1:down", "mouse:1:up", "text:65:down", "text:65:up", "text:122:down", "text:122:up",
        "text:233:down", "text:233:up", "move:-200:350"}));
    EXPECT_NE(std::find(waits.begin(), waits.end(), 30ms), waits.end());
    EXPECT_NE(std::find(waits.begin(), waits.end(), 125ms), waits.end());
}

TEST(MacroTests, SamplesASeparateHoldForEachPressAndReleasesInputs)
{
    auto key = Key('A', 0, 5);
    key.durationRange = MacroAction::DurationRange{20ms, 90ms};
    MacroAction mouse;
    mouse.type = MacroAction::Type::Mouse;
    mouse.button = 1;
    mouse.durationRange = MacroAction::DurationRange{60000ms, 60000ms};
    const auto macro = Sample({key, mouse});
    std::vector<std::chrono::milliseconds> keyHolds;
    for (int run = 0; run < 16; ++run)
    {
        Output output;
        EXPECT_FALSE(macro->Execute(output, [&](auto duration) {
            if (duration == 0ms) return true;
            if (output.events.back() == "key:65:down")
            {
                EXPECT_GE(duration, 20ms);
                EXPECT_LE(duration, 90ms);
                keyHolds.push_back(duration);
                return true;
            }
            EXPECT_EQ(output.events.back(), "mouse:1:down");
            EXPECT_EQ(duration, 60000ms);
            return false;
        }));
        EXPECT_EQ(output.events, (std::vector<std::string>{"key:65:down", "key:65:up", "mouse:1:down", "mouse:1:up"}));
    }
    ASSERT_EQ(keyHolds.size(), 16u);
    EXPECT_NE(*std::min_element(keyHolds.begin(), keyHolds.end()), *std::max_element(keyHolds.begin(), keyHolds.end()));
}

TEST(MacroTests, ValidatesRandomHoldBoundsAndPressMode)
{
    auto key = Key();
    for (auto range : {MacroAction::DurationRange{-1ms, 20ms}, {20ms, 10ms}, {0ms, 60001ms}})
    {
        key.durationRange = range;
        EXPECT_THROW(Sample({key}), std::invalid_argument);
    }
    key.durationRange = MacroAction::DurationRange{0ms, 0ms};
    EXPECT_NO_THROW(Sample({key}));
    for (auto mode : {MacroAction::Mode::Down, MacroAction::Mode::Up})
    {
        key.mode = mode;
        EXPECT_THROW(Sample({key}), std::invalid_argument);
    }
    key.mode = MacroAction::Mode::Press;
    key.type = MacroAction::Type::Wait;
    EXPECT_THROW(Sample({key}), std::invalid_argument);
}

TEST(MacroTests, AppliesOneCancellableRandomDelayAfterTheEntireText)
{
    MacroAction text;
    text.type = MacroAction::Type::Text;
    text.text = L"AB";
    text.duration = 10ms;
    text.durationRange = MacroAction::DurationRange{20ms, 60ms};
    const auto macro = Sample({text, Key('C')});
    const std::vector<std::string> typed{"text:65:down", "text:65:up", "text:66:down", "text:66:up"};
    for (bool finish : {true, false})
    {
        Output output;
        int delays = 0;
        EXPECT_EQ(macro->Execute(output, [&](auto duration) {
            if (duration == 0ms) return true;
            ++delays;
            EXPECT_GE(duration, 20ms);
            EXPECT_LE(duration, 60ms);
            EXPECT_EQ(output.events, typed);
            return finish;
        }), finish);
        EXPECT_EQ(delays, 1);
        auto expected = typed;
        if (finish) expected.insert(expected.end(), {"key:67:down", "key:67:up"});
        EXPECT_EQ(output.events, expected);
    }
}

TEST(MacroTests, WaitsForTheSelectedImageBeforeContinuing)
{
    using tea::vision::Image;
    using tea::vision::Pixel;
    const Image absent{16, 16, std::vector<Pixel>(16 * 16), -20, 30};
    auto present = absent;
    std::fill_n(present.pixels.begin(), 5, Pixel{255, 255, 255});
    MacroAction action;
    action.type = MacroAction::Type::WaitForImage;
    EXPECT_THROW(Sample({action}), std::invalid_argument);
    action.imageId = 7;
    auto macro = Sample({Key('A'), action, Key('B')});
    Output output;
    int loads = 0, scans = 0, retries = 0;
    EXPECT_TRUE(macro->Execute(output, [&](auto duration) {
        return duration == 0ms || ++retries <= 2;
    }, [&](int id) {
        EXPECT_EQ(id, 7);
        ++loads;
        return present;
    }, [&] {
        EXPECT_EQ(output.events, (std::vector<std::string>{"key:65:down", "key:65:up"}));
        return ++scans < 3 ? absent : present;
    }));
    EXPECT_EQ(loads, 1);
    EXPECT_EQ(scans, 3);
    EXPECT_EQ(retries, 2);
    EXPECT_EQ(output.events, (std::vector<std::string>{"key:65:down", "key:65:up", "key:66:down", "key:66:up"}));
}

TEST(MacroTests, MatchesTransparentIconsInWaitMoveAndConditions)
{
    using tea::vision::Image;
    using tea::vision::Pixel;
    using Type = MacroAction::Type;
    Image target{3, 3, std::vector<Pixel>(9, {30, 120, 220})};
    for (int corner : {0, 2, 6, 8}) target.pixels[corner] = {0, 0, 0, 0};

    MacroAction wait, branch, move;
    wait.type = Type::WaitForImage;
    wait.imageId = 1;
    branch.type = Type::IfElse;
    branch.condition = ImageOnScreen{1};
    branch.actions = {Key('X')};
    branch.elseActions = {Key('Y')};
    move.type = Type::Move;
    move.imageId = 1;
    const auto macro = Sample({wait, branch, move});

    for (const Pixel background : {Pixel{255, 255, 255}, Pixel{120, 70, 10}})
    {
        auto frame = target;
        for (int corner : {0, 2, 6, 8}) frame.pixels[corner] = background;
        Output output;
        ASSERT_TRUE(macro->Execute(output, [](auto duration) { return duration == 0ms; },
            [&](int) { return target; }, [&] { return frame; }));
        ASSERT_EQ(output.events.size(), 3u);
        EXPECT_EQ(output.events[0], "key:88:down");
        EXPECT_EQ(output.events[1], "key:88:up");
        EXPECT_EQ(output.events[2].find("move:"), 0u);
    }
}

TEST(MacroTests, ReportsUnmatchableSavedImagesBeforeCapturingTheScreen)
{
    using tea::vision::Image;
    using Type = MacroAction::Type;
    for (const auto type : {Type::WaitForImage, Type::Move, Type::IfElse, Type::While})
    {
        MacroAction action;
        action.type = type;
        if (type == Type::IfElse || type == Type::While) action.condition = ImageOnScreen{1};
        else action.imageId = 1;
        const auto macro = Sample({action, Key('A')});
        for (std::uint8_t alpha : {0, 5})
        {
            Output output;
            EXPECT_THROW(macro->Execute(output, [](auto duration) { return duration == 0ms; },
                [&](int) { return Image{1, 1, {{255, 255, 255, alpha}}}; }, [] {
                    ADD_FAILURE() << "an unmatchable image triggered screen capture";
                    return Image{1, 1, {{}}};
                }), std::invalid_argument);
            EXPECT_TRUE(output.events.empty());
        }
    }
}

TEST(MacroTests, CancellingAnImageWaitReleasesHeldInputAndSkipsTheNextEvent)
{
    MacroAction held = Key('A');
    held.mode = MacroAction::Mode::Down;
    MacroAction action;
    action.type = MacroAction::Type::WaitForImage;
    action.imageId = 1;
    auto macro = Sample({held, action, Key('B')});
    Output output;
    int scans = 0;
    EXPECT_FALSE(macro->Execute(output, [](auto duration) { return duration == 0ms; },
        [](int) { return tea::vision::Image{1, 1, {{255, 255, 255}}}; },
        [&] {
            ++scans;
            return tea::vision::Image{1, 1, {{}}};
        }));
    EXPECT_EQ(scans, 1);
    EXPECT_EQ(output.events, (std::vector<std::string>{"key:65:down", "key:65:up"}));
}

TEST(MacroTests, SelectsEitherBranchAndRechecksNestedWhileBeforeEachIteration)
{
    using tea::vision::Image;
    using Type = MacroAction::Type;
    const Image present{1, 1, {{255, 255, 255}}}, absent{1, 1, {{}}};
    MacroAction loop;
    loop.type = Type::While;
    loop.condition = ImageOnScreen{2};
    loop.actions = {Key('A')};
    MacroAction branch;
    branch.type = Type::IfElse;
    branch.condition = ImageOnScreen{1};
    branch.actions = {loop, Key('B')};
    branch.elseActions = {Key('C')};
    const auto macro = Sample({branch, Key('D')});

    for (bool matches : {true, false})
    {
        Output output;
        std::vector<int> checkedImages;
        int scans = 0, intervals = 0;
        ASSERT_TRUE(macro->Execute(output, [&](auto duration) {
            if (duration == 150ms) ++intervals;
            return true;
        }, [&](int id) {
            checkedImages.push_back(id);
            return present;
        }, [&] { return ++scans <= (matches ? 3 : 0) ? present : absent; }));
        if (matches)
        {
            EXPECT_EQ(checkedImages, (std::vector<int>{1, 2, 2, 2}));
            EXPECT_EQ(intervals, 2);
            EXPECT_EQ(output.events, (std::vector<std::string>{"key:65:down", "key:65:up", "key:65:down",
                "key:65:up", "key:66:down", "key:66:up", "key:68:down", "key:68:up"}));
        }
        else
        {
            EXPECT_EQ(checkedImages, (std::vector<int>{1}));
            EXPECT_EQ(intervals, 0);
            EXPECT_EQ(output.events, (std::vector<std::string>{"key:67:down", "key:67:up", "key:68:down", "key:68:up"}));
        }
    }
}

TEST(MacroTests, FalseWhileSkipsItsBodyAndCancelledConditionsDoNotSelectElse)
{
    using tea::vision::Image;
    using Type = MacroAction::Type;
    const auto load = [](int) { return Image{1, 1, {{255, 255, 255}}}; };
    bool captured = false;
    const auto capture = [&] { captured = true; return Image{1, 1, {{}}}; };
    MacroAction held = Key('A');
    held.mode = MacroAction::Mode::Down;
    MacroAction conditional;
    conditional.type = Type::While;
    conditional.condition = ImageOnScreen{1};
    conditional.actions = {Key('B')};
    Output skipped;
    ASSERT_TRUE(Sample({conditional, Key('C')})->Execute(skipped, [](auto) { return true; }, load, capture));
    EXPECT_EQ(skipped.events, (std::vector<std::string>{"key:67:down", "key:67:up"}));

    captured = false;
    conditional.type = Type::IfElse;
    conditional.elseActions = {Key('D')};
    Output stopped;
    EXPECT_FALSE(Sample({held, conditional, Key('C')})->Execute(stopped, [&](auto) { return !captured; }, load, capture));
    EXPECT_EQ(stopped.events, (std::vector<std::string>{"key:65:down", "key:65:up"}));
}

TEST(MacroTests, EmptyWhileCanBeStoppedAndNestedEventsCountTowardTheLimit)
{
    using tea::vision::Image;
    using Type = MacroAction::Type;
    MacroAction conditional;
    conditional.type = Type::While;
    EXPECT_THROW(Sample({conditional}), std::invalid_argument);
    conditional.condition = ImageOnScreen{0};
    EXPECT_THROW(Sample({conditional}), std::invalid_argument);
    conditional.condition = ImageOnScreen{1};
    conditional.elseActions = {Key()};
    EXPECT_THROW(Sample({conditional}), std::invalid_argument);
    conditional.elseActions.clear();

    Output output;
    int scans = 0;
    const Image target{1, 1, {{255, 255, 255}}};
    EXPECT_FALSE(Sample({conditional, Key()})->Execute(output, [](auto duration) { return duration != 150ms; },
        [&](int) { return target; }, [&] { ++scans; return target; }));
    EXPECT_TRUE(output.events.empty());
    EXPECT_EQ(scans, 1);

    conditional.actions.assign(255, Key());
    EXPECT_NO_THROW(Sample({conditional}));
    EXPECT_THROW(Sample({conditional, Key()}), std::invalid_argument);
}

TEST(MacroTests, ImageMoveWaitsForTheBestMatchAndVariesItsLandingNearTheCenter)
{
    using tea::vision::Image;
    using tea::vision::Pixel;
    const Image target{32, 24, std::vector<Pixel>(32 * 24, {200, 175, 150})};
    const Image absent{96, 48, std::vector<Pixel>(96 * 48), -180, -70};
    auto present = absent;
    for (int y = 0; y < target.height; ++y)
    {
        auto* row = present.pixels.data() + y * present.width;
        std::fill_n(row, target.width, Pixel{203, 178, 153});
        std::fill_n(row + 50, target.width, target.pixels.front());
    }
    MacroAction action;
    action.type = MacroAction::Type::Move;
    action.imageId = 7;
    auto macro = Sample({action, Key('B')});
    struct MoveOutput : Output
    {
        Coordinates landing;
        bool Move(Coordinates position, const Wait& wait) override
        {
            landing = position;
            return Output::Move(position, wait);
        }
    };

    std::vector<Coordinates> landings;
    for (int trial = 0; trial < 16; ++trial)
    {
        MoveOutput output;
        int loads = 0, scans = 0, retries = 0;
        ASSERT_TRUE(macro->Execute(output, [&](auto duration) {
            return duration == 0ms || ++retries == 1;
        }, [&](int id) {
            EXPECT_EQ(id, 7);
            ++loads;
            return target;
        }, [&] {
            EXPECT_TRUE(output.events.empty());
            return ++scans == 1 ? absent : present;
        }));
        EXPECT_EQ(loads, 1);
        EXPECT_EQ(scans, 2);
        EXPECT_EQ(retries, 1);
        EXPECT_EQ(output.events.size(), 3u);
        EXPECT_EQ(output.events.back(), "key:66:up");
        EXPECT_LE(std::abs(output.landing.pos_x - (-180 + 50 + 16)), 4);
        EXPECT_LE(std::abs(output.landing.pos_y - (-70 + 12)), 3);
        landings.push_back(output.landing);
    }
    EXPECT_TRUE(std::any_of(landings.begin(), landings.end(), [&](auto landing) { return !(landing == landings.front()); }));
}

TEST(MacroTests, ImageMoveHandlesSinglePixelTargetsAndCanBeStoppedWhileWaiting)
{
    using tea::vision::Image;
    const Image target{1, 1, {{255, 255, 255}}};
    MacroAction action;
    action.type = MacroAction::Type::Move;
    action.imageId = 1;
    auto macro = Sample({action, Key('B')});
    Output output;
    const auto load = [&](int) { return target; };
    EXPECT_FALSE(macro->Execute(output, [](auto duration) { return duration == 0ms; },
        load, [] { return Image{1, 1, {{}}}; }));
    EXPECT_TRUE(output.events.empty());
    EXPECT_TRUE(macro->Execute(output, [](auto) { return true; },
        load, [] { return Image{2, 1, {{}, {255, 255, 255}}, -10, -20}; }));
    EXPECT_EQ(output.events, (std::vector<std::string>{"move:-9:-20", "key:66:down", "key:66:up"}));
}

TEST(MacroTests, CancellationReleasesHeldKeyAndModifiers)
{
    Output output;
    EXPECT_FALSE(Sample({Key('C', MOD_CONTROL | MOD_ALT, 20), Key('B')})->Execute(
        output, [](auto duration) { return duration == 0ms; }));
    EXPECT_EQ(output.events, (std::vector<std::string>{"key:17:down", "key:18:down", "key:67:down",
        "key:67:up", "key:18:up", "key:17:up"}));
}

TEST(MacroTests, CancellationBeforeExecutionSendsNothing)
{
    Output output;
    EXPECT_FALSE(Sample()->Execute(output, [](auto) { return false; }));
    EXPECT_TRUE(output.events.empty());
}

TEST(MacroTests, OutputFailureReleasesPreviouslyPressedModifier)
{
    Output output;
    output.failAt = 1;
    EXPECT_THROW(Sample({Key('C', MOD_CONTROL)})->Execute(output, [](auto) { return true; }), std::runtime_error);
    EXPECT_EQ(output.events, (std::vector<std::string>{"key:17:down", "key:17:up"}));
}

TEST(MacroTests, DoesNotReleasePhysicallyHeldModifiers)
{
    Output output;
    output.heldKey = VK_CONTROL;
    EXPECT_TRUE(Sample({Key('C', MOD_CONTROL)})->Execute(output, [](auto) { return true; }));
    EXPECT_EQ(output.events, (std::vector<std::string>{"key:67:down", "key:67:up"}));
}

TEST(MacroTests, FailedReleaseIsReportedAndRetriedDuringCleanup)
{
    Output output;
    output.failAt = 1;
    EXPECT_THROW(Sample()->Execute(output, [](auto) { return true; }), std::runtime_error);
    EXPECT_EQ(output.events, (std::vector<std::string>{"key:65:down", "key:65:up"}));
}

TEST(MacroTests, CancellationReleasesMouseAndStopsTextBetweenCharacters)
{
    Output mouse;
    MacroAction click;
    click.type = MacroAction::Type::Mouse; click.duration = 100ms;
    EXPECT_FALSE(Sample({click})->Execute(mouse, [](auto duration) { return duration == 0ms; }));
    EXPECT_EQ(mouse.events, (std::vector<std::string>{"mouse:0:down", "mouse:0:up"}));
    Output text;
    MacroAction type;
    type.type = MacroAction::Type::Text; type.text = L"ab";
    int checks = 0;
    EXPECT_FALSE(Sample({type})->Execute(text, [&](auto) { return ++checks < 3; }));
    EXPECT_EQ(text.events, (std::vector<std::string>{"text:97:down", "text:97:up"}));
}

TEST(MacroTests, SupportsSurrogatePairsAndDoesNotDuplicateModifiers)
{
    Output output;
    MacroAction text;
    text.type = MacroAction::Type::Text;
    text.text = {wchar_t(0xd83d), wchar_t(0xde00)};
    EXPECT_TRUE(Sample({text})->Execute(output, [](auto) { return true; }));
    EXPECT_EQ(output.events, (std::vector<std::string>{"text:55357:down", "text:55357:up", "text:56832:down", "text:56832:up"}));
    output.events.clear();
    EXPECT_TRUE(Sample({Key(VK_CONTROL, MOD_CONTROL)})->Execute(output, [](auto) { return true; }));
    EXPECT_EQ(output.events, (std::vector<std::string>{"key:17:down", "key:17:up"}));
}

TEST(MacroTests, HoldsRecordedInputsAcrossDelaysAndOtherEvents)
{
    auto down = Key(VK_CONTROL); down.mode = MacroAction::Mode::Down;
    auto up = down; up.mode = MacroAction::Mode::Up;
    MacroAction delay, mouse;
    delay.duration = 100ms;
    mouse.type = MacroAction::Type::Mouse;
    Output output;
    EXPECT_TRUE(Sample({down, delay, Key('C', MOD_CONTROL), mouse, up})->Execute(output, [](auto) { return true; }));
    EXPECT_EQ(output.events, (std::vector<std::string>{"key:17:down", "key:67:down", "key:67:up",
        "mouse:0:down", "mouse:0:up", "key:17:up"}));
}

TEST(MacroTests, ReleasesRecordedInputsOnCancellationCompletionAndFailure)
{
    auto key = Key('A'); key.mode = MacroAction::Mode::Down;
    MacroAction mouse, delay;
    mouse.type = MacroAction::Type::Mouse; mouse.mode = MacroAction::Mode::Down;
    delay.duration = 100ms;
    for (int mode : {0, 1, 2})
    {
        Output output;
        auto macro = Sample({key, mouse, delay, Key('B')});
        if (mode == 2)
        {
            output.failAt = 2;
            EXPECT_THROW(macro->Execute(output, [](auto) { return true; }), std::runtime_error);
        }
        else EXPECT_EQ(macro->Execute(output, [mode](auto duration) { return mode == 0 || duration == 0ms; }), mode == 0);
        ASSERT_GE(output.events.size(), 4u);
        EXPECT_EQ(output.events[output.events.size() - 2], "mouse:0:up");
        EXPECT_EQ(output.events.back(), "key:65:up");
    }
}

TEST(MacroTests, RejectsInvalidModesAndDoesNotReleaseInputsItDidNotPress)
{
    auto key = Key('A'); key.mode = MacroAction::Mode::Up;
    Output output;
    EXPECT_TRUE(Sample({key})->Execute(output, [](auto) { return true; }));
    EXPECT_TRUE(output.events.empty());
    key.mode = static_cast<MacroAction::Mode>(99);
    EXPECT_THROW(Sample({key}), std::invalid_argument);
    key.mode = MacroAction::Mode::Down; key.modifiers = MOD_CONTROL;
    EXPECT_THROW(Sample({key}), std::invalid_argument);
    MacroAction delay; delay.mode = MacroAction::Mode::Up;
    EXPECT_THROW(Sample({delay}), std::invalid_argument);
}

TEST(RecorderTests, RecordsOverlappingInputsAndTimingWithoutRepeatsInjectedOrOrphanedReleases)
{
    std::vector<MacroAction> actions;
    int stopped = 0;
    MacroRecorder recorder([&](const auto& action) { actions.push_back(action); }, [&] { ++stopped; });
    recorder.Start(VK_ESCAPE, VK_F10, {true}, 256, false);
    recorder.Input(false, 'A', false, 5);
    recorder.Input(false, 'Z', true, 8, true);
    recorder.Input(false, VK_LCONTROL, true, 10);
    recorder.Input(false, VK_LCONTROL, true, 20);
    recorder.Input(true, 1, true, 30);
    recorder.Input(true, 1, false, 50);
    recorder.Input(false, VK_LCONTROL, false, 80);
    recorder.Input(false, VK_ESCAPE, true, 100);
    ASSERT_EQ(actions.size(), 7u);
    EXPECT_EQ(actions[0].key, UINT(VK_LCONTROL));
    EXPECT_EQ(actions[0].mode, MacroAction::Mode::Down);
    EXPECT_EQ(actions[1].duration, 20ms);
    EXPECT_EQ(actions[2].type, MacroAction::Type::Mouse);
    EXPECT_EQ(actions[2].button, 1);
    EXPECT_EQ(actions[3].duration, 20ms);
    EXPECT_EQ(actions[4].mode, MacroAction::Mode::Up);
    EXPECT_EQ(actions[5].duration, 30ms);
    EXPECT_EQ(actions[6].mode, MacroAction::Mode::Up);
    EXPECT_EQ(stopped, 1);
    EXPECT_FALSE(recorder.Active());
    recorder.Stop(); EXPECT_EQ(stopped, 1);
}

TEST(RecorderTests, SupportsNoDelaysAndSplitsLongDelays)
{
    for (bool delays : {false, true})
    {
        std::vector<MacroAction> actions;
        MacroRecorder recorder([&](const auto& action) { actions.push_back(action); }, [] {});
        recorder.Start(VK_F9, VK_F10, {delays, false, true, true, true}, 256, false);
        recorder.Input(false, 'A', true, 0);
        recorder.Input(false, 'A', false, 100000);
        recorder.Stop();
        ASSERT_EQ(actions.size(), delays ? 4u : 2u);
        if (delays) { EXPECT_EQ(actions[1].duration, 60000ms); EXPECT_EQ(actions[2].duration, 40000ms); }
    }
}

TEST(RecorderTests, ReservesRoomForReleasesAndStopsAtTheEventLimit)
{
    for (bool split : {false, true})
    for (unsigned limit : {2u, 3u, 4u, 256u})
    {
        std::vector<MacroAction> actions;
        MacroRecorder recorder([&](const auto& action) { actions.push_back(action); }, [] {});
        recorder.Start(VK_ESCAPE, VK_F10, {true, false, true, true, split}, limit, false);
        for (UINT key = 'A'; key <= 'Z'; ++key) recorder.Input(false, key, true, key * 10);
        recorder.Stop();
        EXPECT_LE(actions.size(), limit);
        EXPECT_FALSE(recorder.Active());
        Output output;
        EXPECT_TRUE(Sample(actions)->Execute(output, [](auto) { return true; }));
        EXPECT_EQ(std::count_if(output.events.begin(), output.events.end(), [](const auto& value) { return value.find(":down") != std::string::npos; }),
            std::count_if(output.events.begin(), output.events.end(), [](const auto& value) { return value.find(":up") != std::string::npos; }));
    }
}

TEST(RecorderTests, RejectsInvalidSessionsAndCanRestartAfterStopping)
{
    MacroRecorder recorder([](const auto&) {}, [] {});
    for (unsigned limit : {0u, 1u, 257u}) EXPECT_THROW(recorder.Start(VK_ESCAPE, VK_F10, {true}, limit, false), std::invalid_argument);
    EXPECT_THROW(recorder.Start(VK_CONTROL, VK_F10, {true}, 256, false), std::invalid_argument);
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        recorder.Start(VK_ESCAPE, VK_F10, {true}, 256, false);
        EXPECT_THROW(recorder.Start(VK_ESCAPE, VK_F10, {true}, 256, false), std::invalid_argument);
        recorder.Stop();
    }
}

TEST(RecorderTests, StopPreservesFinalHoldAndReleasesInReversePressOrder)
{
    std::vector<MacroAction> actions;
    MacroRecorder recorder([&](const auto& action) { actions.push_back(action); }, [] {});
    recorder.Start(VK_ESCAPE, VK_F10, {true, false, true, true, true}, 256, false);
    recorder.Input(false, VK_LCONTROL, true, 100);
    recorder.Input(false, 'A', true, 120);
    recorder.Input(false, VK_ESCAPE, true, 200);
    ASSERT_EQ(actions.size(), 6u);
    EXPECT_EQ(actions[3].duration, 80ms);
    EXPECT_EQ(actions[4].key, UINT('A'));
    EXPECT_EQ(actions[5].key, UINT(VK_LCONTROL));
    EXPECT_EQ(actions[4].mode, MacroAction::Mode::Up);
    EXPECT_EQ(actions[5].mode, MacroAction::Mode::Up);
}

TEST(RecorderTests, NativeStopKeyIsConsumedThroughReleaseAndPreservesTheFinalHold)
{
    std::vector<MacroAction> actions;
    int stops = 0;
    MacroRecorder recorder([&](const auto& action) { actions.push_back(action); }, [&] { ++stops; });
    Binder binder;
    binder.Replace({}, VK_DELETE);
    const auto output = std::make_shared<Output>();
    MacroListener listener(binder, output, {}, {}, {}, {}, [&] { return recorder.Active(); });
    listener.Start(false);
    listener.SetEnabled(false);
    recorder.Start(VK_DELETE, VK_F10, {}, 256, false);
    recorder.Input(false, 'A', true, 100);

    EXPECT_EQ(recorder.KeyboardInput(VK_DELETE, true, 150, 0, [&] {
        return static_cast<LRESULT>(listener.ProcessEvent({VK_DELETE, 0}, true));
    }), 1);
    EXPECT_FALSE(recorder.Active());
    EXPECT_EQ(stops, 1);
    EXPECT_TRUE(listener.ProcessEvent({VK_DELETE, 0}, true));
    EXPECT_TRUE(listener.ProcessEvent({VK_DELETE, 0}, false));
    EXPECT_FALSE(listener.ProcessEvent({VK_DELETE, 0}, true));
    EXPECT_FALSE(listener.ProcessEvent({VK_DELETE, 0}, false));

    ASSERT_EQ(actions.size(), 1u);
    EXPECT_EQ(actions.front().mode, MacroAction::Mode::Press);
    EXPECT_EQ(actions.front().key, UINT('A'));
    EXPECT_EQ(actions.front().duration, 50ms);
}

TEST(RecorderTests, CombinesKeyPressesByDefaultAndFinishesHeldKeysWhenStopped)
{
    std::vector<MacroAction> actions;
    MacroRecorder recorder([&](const auto& action) { actions.push_back(action); }, [] {});
    recorder.Start(VK_ESCAPE, VK_F10, {}, 256, false);
    recorder.Input(false, 'A', false, 90);
    recorder.Input(false, 'A', true, 100);
    recorder.Input(false, 'A', true, 110);
    recorder.Input(false, 'Z', true, 120, true);
    EXPECT_TRUE(actions.empty());
    recorder.Input(false, 'A', false, 125);
    recorder.Input(false, VK_F10, true, 130);
    recorder.Input(false, 'B', true, 145);
    recorder.Input(false, VK_ESCAPE, true, 185);
    ASSERT_EQ(actions.size(), 3u);
    EXPECT_EQ(actions[0].mode, MacroAction::Mode::Press);
    EXPECT_EQ(actions[0].key, UINT('A'));
    EXPECT_EQ(actions[0].duration, 25ms);
    EXPECT_EQ(actions[1].type, MacroAction::Type::Wait);
    EXPECT_EQ(actions[1].duration, 20ms);
    EXPECT_EQ(actions[2].mode, MacroAction::Mode::Press);
    EXPECT_EQ(actions[2].key, UINT('B'));
    EXPECT_EQ(actions[2].duration, 40ms);
    EXPECT_FALSE(recorder.Active());
    Output output;
    EXPECT_TRUE(Sample(actions)->Execute(output, [](auto) { return true; }));
    EXPECT_EQ(output.events, (std::vector<std::string>{"key:65:down", "key:65:up", "key:66:down", "key:66:up"}));
}

TEST(RecorderTests, CombinedKeysPreserveShortcutsAndInterleavedInputOrder)
{
    std::vector<MacroAction> actions;
    MacroRecorder recorder([&](const auto& action) { actions.push_back(action); }, [] {});
    recorder.Start(VK_ESCAPE, VK_F10, {}, 256, false);
    recorder.Input(false, VK_LCONTROL, true, 100);
    recorder.Input(false, 'C', true, 120);
    recorder.Input(false, 'C', false, 140);
    recorder.Input(false, VK_LCONTROL, false, 160);
    ASSERT_EQ(actions.size(), 5u);
    EXPECT_EQ(actions[0].mode, MacroAction::Mode::Down);
    EXPECT_EQ(actions[2].mode, MacroAction::Mode::Press);
    EXPECT_EQ(actions[2].duration, 20ms);
    EXPECT_EQ(actions[4].mode, MacroAction::Mode::Up);
    recorder.Input(false, 'A', true, 180);
    recorder.Input(false, 'B', true, 200);
    recorder.Input(false, 'A', false, 220);
    recorder.Input(true, 0, true, 240);
    recorder.Input(false, 'B', false, 260);
    recorder.Stop(280);
    Output output;
    auto timing = 0ms;
    EXPECT_TRUE(Sample(actions)->Execute(output, [&](auto duration) { timing += duration; return true; }));
    EXPECT_EQ(timing, 180ms);
    EXPECT_EQ(output.events, (std::vector<std::string>{"key:162:down", "key:67:down", "key:67:up", "key:162:up",
        "key:65:down", "key:66:down", "key:65:up", "mouse:0:down", "key:66:up", "mouse:0:up"}));
}

TEST(RecorderTests, CombinedKeysRespectTimingOptionsAndLongHoldLimits)
{
    for (bool delays : {false, true})
    for (bool stopHeld : {false, true})
    {
        std::vector<MacroAction> actions;
        MacroRecorder recorder([&](const auto& action) { actions.push_back(action); }, [] {});
        recorder.Start(VK_ESCAPE, VK_F10, {delays}, 256, false);
        recorder.Input(false, 'A', true, 100);
        if (stopHeld) recorder.Stop(100100);
        else { recorder.Input(false, 'A', false, 100100); recorder.Stop(); }
        ASSERT_EQ(actions.size(), delays ? 4u : 1u);
        if (delays)
        {
            EXPECT_EQ(actions.front().mode, MacroAction::Mode::Down);
            EXPECT_EQ(actions[1].duration, 60000ms);
            EXPECT_EQ(actions[2].duration, 40000ms);
            EXPECT_EQ(actions.back().mode, MacroAction::Mode::Up);
        }
        else { EXPECT_EQ(actions[0].mode, MacroAction::Mode::Press); EXPECT_EQ(actions[0].duration, 0ms); }
    }
}

TEST(RecorderTests, CombinedKeysUseTheFullEventLimitAndRestartWithoutPendingInputs)
{
    for (unsigned limit : {2u, 3u, 256u})
    {
        std::vector<MacroAction> actions;
        MacroRecorder recorder([&](const auto& action) { actions.push_back(action); }, [] {});
        recorder.Start(VK_ESCAPE, VK_F10, {false}, limit, false);
        for (unsigned index = 0; index < limit; ++index)
        {
            recorder.Input(false, 'A', true, index * 10);
            recorder.Input(false, 'A', false, index * 10 + 5);
        }
        EXPECT_FALSE(recorder.Active());
        ASSERT_EQ(actions.size(), limit);
        for (const auto& action : actions) EXPECT_EQ(action.mode, MacroAction::Mode::Press);
        actions.clear();
        recorder.Start(VK_ESCAPE, VK_F10, {}, limit, false);
        recorder.Input(false, 'B', true, 100);
        recorder.Stop(125);
        ASSERT_EQ(actions.size(), 1u);
        EXPECT_EQ(actions[0].key, UINT('B'));
        EXPECT_EQ(actions[0].duration, 25ms);
    }
}

TEST(RecorderTests, InstallsAndRemovesNativeHooks)
{
    MacroRecorder recorder([](const auto&) {}, [] {});
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        EXPECT_NO_THROW(recorder.Start(VK_ESCAPE, VK_F10, {true}, 256));
        EXPECT_TRUE(recorder.Active());
        recorder.Stop();
        EXPECT_FALSE(recorder.Active());
    }
}

TEST(RecorderTests, FiltersInputTypesAndExcludesBothControlHotkeys)
{
    std::vector<MacroAction> actions;
    MacroRecorder recorder([&](const auto& action) { actions.push_back(action); }, [] {});
    recorder.Start(VK_ESCAPE, VK_F10, {false, false, false, true}, 256, false);
    recorder.Input(false, 'A', true, 0);
    recorder.Input(false, 'A', false, 10);
    recorder.Input(false, VK_F10, true, 20);
    recorder.Input(false, VK_F10, false, 30);
    EXPECT_TRUE(recorder.Active());
    recorder.MouseInput(WM_MOUSEMOVE, {1, 2}, 40);
    recorder.Input(true, 0, true, 50);
    recorder.Input(true, 0, false, 60);
    recorder.Input(false, VK_ESCAPE, true, 70);
    ASSERT_EQ(actions.size(), 2u);
    EXPECT_EQ(actions[0].type, MacroAction::Type::Mouse);
    EXPECT_EQ(actions[1].mode, MacroAction::Mode::Up);
    EXPECT_FALSE(recorder.Active());
    EXPECT_THROW(recorder.Start(VK_ESCAPE, VK_ESCAPE, {}, 256, false), std::invalid_argument);
    EXPECT_THROW(recorder.Start(VK_ESCAPE, VK_F10, {true, false, false, false}, 256, false), std::invalid_argument);

    actions.clear();
    recorder.Start(VK_ESCAPE, 0, {}, 256, false);
    recorder.Input(false, VK_F10, true, 100);
    recorder.Input(false, VK_F10, false, 120);
    recorder.Input(false, VK_ESCAPE, true, 130);
    EXPECT_FALSE(recorder.Active());
    ASSERT_EQ(actions.size(), 1u);
    EXPECT_EQ(actions[0].key, VK_F10);
}

TEST(RecorderTests, CapturesClickPositionsAndMovesBeforeEachMouseEvent)
{
    std::vector<MacroAction> actions;
    MacroRecorder recorder([&](const auto& action) { actions.push_back(action); }, [] {});
    recorder.Start(VK_ESCAPE, VK_F10, {true, true, true, true}, 256, false);
    recorder.MouseInput(WM_LBUTTONDOWN, {10, 20}, 100);
    recorder.MouseInput(WM_LBUTTONUP, {10, 20}, 130);
    recorder.MouseInput(WM_MOUSEMOVE, {99, 99}, 140);
    recorder.MouseInput(WM_RBUTTONDOWN, {-30, 40}, 150);
    recorder.MouseInput(WM_RBUTTONUP, {-30, 40}, 160);
    recorder.MouseInput(WM_LBUTTONDOWN, {99, 99}, 180, 0x544541);
    recorder.Stop(200);
    ASSERT_EQ(actions.size(), 7u);
    EXPECT_EQ(actions[0].position, Coordinates(10, 20));
    EXPECT_TRUE(actions[0].positioned);
    EXPECT_EQ(actions[1].duration, 30ms);
    EXPECT_EQ(actions[3].duration, 20ms);
    EXPECT_EQ(actions[4].position, Coordinates(-30, 40));
    EXPECT_EQ(actions[5].duration, 10ms);
    Output output;
    EXPECT_TRUE(Sample(actions)->Execute(output, [](auto) { return true; }));
    EXPECT_EQ(output.events, (std::vector<std::string>{"move:10:20", "mouse:0:down", "move:10:20", "mouse:0:up",
        "move:-30:40", "mouse:1:down", "move:-30:40", "mouse:1:up"}));
}

TEST(RecorderTests, PositionedClicksRequireMouseRecordingAndReserveReleases)
{
    std::vector<MacroAction> actions;
    MacroRecorder recorder([&](const auto& action) { actions.push_back(action); }, [] {});
    EXPECT_THROW(recorder.Start(VK_ESCAPE, VK_F10, {true, true, true, false}, 256, false), std::invalid_argument);
    recorder.Start(VK_ESCAPE, VK_F10, {false, true, true, true}, 2, false);
    recorder.MouseInput(WM_LBUTTONDOWN, {10, 20}, 10);
    ASSERT_EQ(actions.size(), 2u);
    EXPECT_TRUE(actions[0].positioned);
    EXPECT_EQ(actions.back().mode, MacroAction::Mode::Up);
    EXPECT_FALSE(recorder.Active());
}

TEST(MacroTests, FailedPositionedMovementDoesNotClickAndReleasesHeldMouseButtons)
{
    MacroAction down;
    down.type = MacroAction::Type::Mouse; down.mode = MacroAction::Mode::Down;
    down.positioned = true; down.position = {10, 20};
    auto up = down; up.mode = MacroAction::Mode::Up; up.position = {30, 40};
    Output beforePress;
    beforePress.failAt = 0;
    EXPECT_THROW(Sample({down})->Execute(beforePress, [](auto) { return true; }), std::runtime_error);
    EXPECT_TRUE(beforePress.events.empty());
    Output beforeRelease;
    beforeRelease.failAt = 2;
    EXPECT_THROW(Sample({down, up})->Execute(beforeRelease, [](auto) { return true; }), std::runtime_error);
    EXPECT_EQ(beforeRelease.events, (std::vector<std::string>{"move:10:20", "mouse:0:down", "mouse:0:up"}));
}

TEST(BinderTests, RecordHotkeyReservationIsAtomicAndDistinctFromStop)
{
    Binder binder;
    binder.Replace({}, VK_ESCAPE, VK_F10);
    EXPECT_EQ(binder.RecordKey(), UINT(VK_F10));
    EXPECT_THROW(binder.Bind({{VK_F10, MOD_CONTROL}, Sample(), false}), std::invalid_argument);
    EXPECT_THROW(binder.Replace({}, VK_F10), std::invalid_argument);
    EXPECT_THROW(binder.Replace({}, VK_ESCAPE, VK_CONTROL), std::invalid_argument);
    EXPECT_THROW(binder.Replace({{{VK_F10, 0}, Sample(), false}}, VK_ESCAPE, VK_F10), std::invalid_argument);
    EXPECT_EQ(binder.StopKey(), UINT(VK_ESCAPE));
    EXPECT_EQ(binder.RecordKey(), UINT(VK_F10));
    binder.Replace({});
    EXPECT_EQ(binder.RecordKey(), UINT(VK_F10));
    binder.Replace({}, std::nullopt, 0);
    EXPECT_EQ(binder.RecordKey(), 0u);
}

TEST(ListenerNativeTests, RecordHotkeyFiresOncePerPressWhilePlaybackIsDisabled)
{
    Binder binder;
    binder.Replace({}, VK_ESCAPE, VK_F10);
    int recordings = 0;
    bool armed = false;
    MacroListener listener(binder, std::make_shared<Output>(), {}, {}, [&] {
        if (!armed) return false;
        ++recordings;
        return true;
    });
    listener.Start(false);
    listener.SetEnabled(false);
    EXPECT_FALSE(listener.ProcessEvent({VK_F10, 0}, true));
    armed = true;
    EXPECT_FALSE(listener.ProcessEvent({VK_F10, 0}, true));
    EXPECT_FALSE(listener.ProcessEvent({VK_F10, 0}, false));
    EXPECT_EQ(recordings, 0);
    EXPECT_FALSE(listener.ProcessEvent({VK_F10, MOD_ALT}, true));
    EXPECT_FALSE(listener.ProcessEvent({VK_F10, MOD_ALT}, false));
    EXPECT_EQ(recordings, 0);
    EXPECT_TRUE(listener.ProcessEvent({VK_F10, 0}, true, false, false));
    EXPECT_TRUE(listener.ProcessEvent({VK_F10, 0}, true, false, false));
    EXPECT_TRUE(listener.ProcessEvent({VK_F10, 0}, false, false, false));
    EXPECT_EQ(recordings, 1);
    EXPECT_FALSE(listener.ProcessEvent({VK_F10, 0}, true, true));
    EXPECT_EQ(recordings, 1);
}

TEST(BinderTests, BindReplaceFindUnbindAndSharedMacro)
{
    Binder binder;
    auto macro = Sample();
    binder.Bind({{'A', MOD_CONTROL}, macro, true});
    binder.Bind({{'B', 0}, macro, false});
    ASSERT_TRUE(binder.Find({'A', MOD_CONTROL}));
    EXPECT_EQ(binder.Find({'A', MOD_CONTROL})->macro, binder.Find({'B', 0})->macro);
    EXPECT_TRUE(binder.Find({'A', MOD_CONTROL})->replace);
    EXPECT_FALSE(binder.Find({'A', 0}));
    binder.Bind({{'A', MOD_CONTROL}, Sample({Key('Z')}), false});
    EXPECT_FALSE(binder.Find({'A', MOD_CONTROL})->replace);
    EXPECT_TRUE(binder.Unbind({'A', MOD_CONTROL}));
    EXPECT_FALSE(binder.Unbind({'A', MOD_CONTROL}));
}

TEST(BinderTests, InvalidBatchIsAtomicAndValidBatchReplacesAll)
{
    Binder binder;
    binder.Bind({{'A', 0}, Sample(), false});
    EXPECT_THROW(binder.Bind({{'B', 0}, nullptr, false}), std::invalid_argument);
    EXPECT_THROW(binder.Replace({{{'B', 0}, Sample(), false}, {{0, 0}, Sample(), false}}), std::invalid_argument);
    EXPECT_TRUE(binder.Find({'A', 0}));
    EXPECT_FALSE(binder.Find({'B', 0}));
    EXPECT_THROW(binder.Replace({{{'B', 0}, Sample(), false}, {{'B', 0}, Sample(), true}}), std::invalid_argument);
    binder.Replace({{{'C', 0}, Sample(), false}});
    EXPECT_FALSE(binder.Find({'A', 0}));
    EXPECT_TRUE(binder.Find({'C', 0}));
    binder.Replace({});
    EXPECT_FALSE(binder.Find({'C', 0}));
}

TEST(BinderTests, CaptureIsOneShotCancelableAndCallsOutsideLock)
{
    Binder binder;
    int captured = 0, cancelled = 0;
    EXPECT_THROW(binder.BeginCapture({}), std::invalid_argument);
    binder.BeginCapture([&](auto trigger) {
        if (!trigger) { ++cancelled; return; }
        ++captured;
        binder.Bind({*trigger, Sample(), false});
    });
    EXPECT_TRUE(binder.Capturing());
    EXPECT_FALSE(binder.CaptureKey({VK_CONTROL, 0}));
    EXPECT_TRUE(binder.Capturing());
    EXPECT_TRUE(binder.CaptureKey({'A', MOD_ALT}));
    EXPECT_FALSE(binder.Capturing());
    EXPECT_FALSE(binder.CaptureKey({'B', 0}));
    EXPECT_EQ(captured, 1);
    EXPECT_TRUE(binder.Find({'A', MOD_ALT}));
    binder.BeginCapture([&](auto value) { if (!value) ++cancelled; });
    binder.CancelCapture();
    binder.CancelCapture();
    EXPECT_EQ(cancelled, 1);
}

class ListenerTests : public ::testing::Test
{
protected:
    Binder binder;
    std::shared_ptr<Output> output = std::make_shared<Output>();
    std::vector<std::string> errors;
    MacroListener listener{binder, output, [&](const auto& message) { errors.push_back(message); }};
    void SetUp() override { binder.Replace({}, VK_ESCAPE); listener.Start(false); }
    void TearDown() override { listener.Stop(); }
    void Press(Trigger trigger) { listener.ProcessEvent(trigger, true); listener.ProcessEvent(trigger, false); }
};

TEST_F(ListenerTests, IgnoresUnboundInjectedAndWrongModifierInput)
{
    binder.Bind({{'A', MOD_CONTROL}, Sample(), true});
    Press({'A', 0});
    EXPECT_FALSE(listener.ProcessEvent({'A', MOD_CONTROL}, true, true));
    EXPECT_FALSE(listener.ProcessEvent({'A', MOD_CONTROL}, false, true));
    Press({'B', MOD_CONTROL});
    ASSERT_TRUE(listener.WaitIdle(2s));
    EXPECT_TRUE(output->events.empty());
}

TEST_F(ListenerTests, FiresOncePerPressAndReleasesSuppressionAfterUnbind)
{
    binder.Bind({{'A', 0}, Sample(), true});
    EXPECT_TRUE(listener.ProcessEvent({'A', 0}, true));
    EXPECT_TRUE(listener.ProcessEvent({'A', 0}, true));
    binder.Unbind({'A', 0});
    EXPECT_TRUE(listener.ProcessEvent({'A', 0}, false));
    ASSERT_TRUE(listener.WaitIdle(2s));
    EXPECT_EQ(output->events.size(), 2u);
    EXPECT_FALSE(listener.ProcessEvent({'A', 0}, true));
    EXPECT_FALSE(listener.ProcessEvent({'A', 0}, false));
}

TEST_F(ListenerTests, PassThroughBindingAndDisabledListenerDoNotSuppress)
{
    binder.Bind({{'A', 0}, Sample(), false});
    EXPECT_FALSE(listener.ProcessEvent({'A', 0}, true));
    EXPECT_FALSE(listener.ProcessEvent({'A', 0}, false));
    ASSERT_TRUE(listener.WaitIdle(2s));
    EXPECT_EQ(output->events.size(), 2u);
    listener.SetEnabled(false);
    Press({'A', 0});
    ASSERT_TRUE(listener.WaitIdle(2s));
    EXPECT_EQ(output->events.size(), 2u);
    listener.SetEnabled(true);
    Press({'A', 0});
    ASSERT_TRUE(listener.WaitIdle(2s));
    EXPECT_EQ(output->events.size(), 4u);
}

TEST(ListenerConcurrencyTests, DoesNotAdmitInFlightHotkeysAfterCancelDisableOrBindingReplacement)
{
    enum class Transition { Cancel, DisableAndEnable, Replace };
    for (const auto transition : {Transition::Cancel, Transition::DisableAndEnable, Transition::Replace})
    {
        Binder binder;
        binder.Replace({{{'A', 0}, Sample({Key('X')}), true}}, VK_ESCAPE);
        const auto output = std::make_shared<Output>();
        std::promise<void> handlingHotkey, resumeHotkey;
        auto handling = handlingHotkey.get_future();
        auto resume = resumeHotkey.get_future();
        bool firstPress = true;
        MacroListener listener(binder, output, {}, {}, {}, {}, {}, [&](Trigger) {
            if (firstPress)
            {
                firstPress = false;
                handlingHotkey.set_value();
                resume.wait();
            }
            return false;
        });
        listener.Start(false);
        bool suppressed = true;
        std::thread press([&] {
            suppressed = listener.ProcessEvent({'A', 0}, true);
            listener.ProcessEvent({'A', 0}, false);
        });
        const bool received = handling.wait_for(2s) == std::future_status::ready;
        if (received)
        {
            if (transition == Transition::Cancel) listener.Cancel();
            else if (transition == Transition::DisableAndEnable)
            {
                listener.SetEnabled(false);
                listener.SetEnabled(true);
            }
            else listener.ReplaceBindings({{{'A', 0}, Sample({Key('Y')}), true}}, VK_ESCAPE, 0);
        }
        resumeHotkey.set_value();
        press.join();
        ASSERT_TRUE(received);
        EXPECT_FALSE(suppressed);
        ASSERT_TRUE(listener.WaitIdle(2s));
        EXPECT_TRUE(output->events.empty());
        EXPECT_TRUE(listener.ProcessEvent({'A', 0}, true));
        EXPECT_TRUE(listener.ProcessEvent({'A', 0}, false));
        ASSERT_TRUE(listener.WaitIdle(2s));
        const std::string key = transition == Transition::Replace ? "key:89" : "key:88";
        EXPECT_EQ(output->events, (std::vector<std::string>{key + ":down", key + ":up"}));
    }
}

TEST_F(ListenerTests, CaptureDoesNotExecuteExistingBindingAndEscapeCancels)
{
    binder.Bind({{'A', 0}, Sample(), true});
    std::optional<Trigger> captured;
    listener.BeginCapture([&](auto value) { captured = value; });
    Press({VK_LCONTROL, MOD_CONTROL});
    EXPECT_TRUE(binder.Capturing());
    Press({'A', MOD_CONTROL});
    ASSERT_TRUE(captured);
    EXPECT_EQ(captured->modifiers, UINT(MOD_CONTROL));
    EXPECT_TRUE(output->events.empty());
    int cancelled = 0;
    listener.BeginCapture([&](auto value) { if (!value) ++cancelled; });
    EXPECT_TRUE(listener.ProcessEvent({VK_ESCAPE, 0}, true));
    EXPECT_TRUE(listener.ProcessEvent({VK_ESCAPE, 0}, false));
    EXPECT_EQ(cancelled, 1);
}

TEST_F(ListenerTests, CancelInterruptsLongActionAndDiscardsQueuedWork)
{
    binder.Bind({{'A', 0}, Sample({Key('Z', MOD_CONTROL, 60000)}), false});
    Press({'A', 0});
    ASSERT_TRUE(output->WaitFor(2));
    Press({'A', 0});
    listener.Cancel();
    ASSERT_TRUE(listener.WaitIdle(2s));
    EXPECT_EQ(output->events, (std::vector<std::string>{"key:17:down", "key:90:down", "key:90:up", "key:17:up"}));
}

TEST_F(ListenerTests, ForegroundExclusionAndFailuresDoNotKillWorker)
{
    binder.Bind({{'A', 0}, Sample(), false});
    EXPECT_FALSE(listener.ProcessEvent({'A', 0}, true, false, false));
    listener.ProcessEvent({'A', 0}, false);
    EXPECT_TRUE(output->events.empty());
    output->failAt = 0;
    Press({'A', 0});
    ASSERT_TRUE(listener.WaitIdle(2s));
    ASSERT_EQ(errors.size(), 1u);
    Press({'A', 0});
    ASSERT_TRUE(listener.WaitIdle(2s));
    EXPECT_EQ(output->events.size(), 2u);
}

TEST_F(ListenerTests, FocusReturningToIgnoredWindowReleasesHeldInputAndPreventsNewOutput)
{
    const HWND editor = reinterpret_cast<HWND>(1);
    listener.IgnoreWindow(editor);
    binder.Bind({{'A', 0}, Sample({Key('X', 0, 60000), Key('Y')}), true});
    binder.Bind({{'B', 0}, Sample({Key('Z')}), true});
    Press({'A', 0});
    ASSERT_TRUE(output->WaitFor(1));
    Press({'B', 0});
    output->foreground = editor;
    ASSERT_TRUE(listener.WaitIdle(2s));
    EXPECT_EQ(output->events, (std::vector<std::string>{"key:88:down", "key:88:up"}));
    EXPECT_FALSE(listener.ProcessEvent({'B', 0}, true));
    EXPECT_FALSE(listener.ProcessEvent({'B', 0}, false));
    output->foreground = nullptr;
    Press({'B', 0});
    ASSERT_TRUE(listener.WaitIdle(2s));
    EXPECT_EQ(output->events, (std::vector<std::string>{"key:88:down", "key:88:up", "key:90:down", "key:90:up"}));
}

TEST(ListenerConcurrencyTests, FocusCancelsPlaybackWhileWaitingForTheTriggerToBeReleased)
{
    class TriggerOutput : public Output
    {
        bool IsDown(UINT key) override
        {
            if (!Output::IsDown(key)) return false;
            heldKey = 0;
            foregroundOnce = true;
            return true;
        }
    };
    auto output = std::make_shared<TriggerOutput>();
    output->heldKey = 'A';
    Binder binder;
    binder.Replace({{{'A', 0}, Sample({Key('X')}), true}}, VK_ESCAPE);
    MacroListener listener(binder, output);
    listener.IgnoreWindow(reinterpret_cast<HWND>(1));
    listener.Start(false);

    EXPECT_TRUE(listener.ProcessEvent({'A', 0}, true));
    EXPECT_TRUE(listener.ProcessEvent({'A', 0}, false));
    ASSERT_TRUE(listener.WaitIdle(2s));
    EXPECT_FALSE(output->foregroundOnce);
    EXPECT_TRUE(output->events.empty());
}

TEST_F(ListenerTests, ABriefReturnToTheIgnoredWindowDiscardsQueuedPlayback)
{
    listener.IgnoreWindow(reinterpret_cast<HWND>(1));
    binder.Bind({{'A', 0}, Sample({Key('X', 0, 60000)}), true});
    binder.Bind({{'B', 0}, Sample({Key('Z')}), true});
    Press({'A', 0});
    ASSERT_TRUE(output->WaitFor(1));
    Press({'B', 0});
    output->foregroundOnce = true;
    ASSERT_TRUE(listener.WaitIdle(2s));
    EXPECT_FALSE(output->foregroundOnce);
    EXPECT_EQ(output->events, (std::vector<std::string>{"key:88:down", "key:88:up"}));

    Press({'B', 0});
    ASSERT_TRUE(listener.WaitIdle(2s));
    EXPECT_EQ(output->events, (std::vector<std::string>{"key:88:down", "key:88:up", "key:90:down", "key:90:up"}));
}

TEST_F(ListenerTests, FullQueuePassesThroughAnUnacceptedHotkey)
{
    binder.Bind({{'A', 0}, Sample({Key('X', 0, 60000)}), true});
    Press({'A', 0});
    ASSERT_TRUE(output->WaitFor(1));
    for (int i = 0; i < 32; ++i)
    {
        EXPECT_TRUE(listener.ProcessEvent({'A', 0}, true));
        EXPECT_TRUE(listener.ProcessEvent({'A', 0}, false));
    }
    EXPECT_FALSE(listener.ProcessEvent({'A', 0}, true));
    EXPECT_FALSE(listener.ProcessEvent({'A', 0}, false));
    listener.Cancel();
    ASSERT_TRUE(listener.WaitIdle(2s));
    EXPECT_EQ(output->events, (std::vector<std::string>{"key:88:down", "key:88:up"}));
}

TEST_F(ListenerTests, StopIsIdempotentReleasesKeysAndCanRestart)
{
    binder.Bind({{'A', 0}, Sample({Key('Z', 0, 60000)}), false});
    Press({'A', 0});
    ASSERT_TRUE(output->WaitFor(1));
    listener.Stop();
    listener.Stop();
    ASSERT_EQ(output->events.size(), 2u);
    EXPECT_EQ(output->events.back(), "key:90:up");
    listener.Start(false);
    binder.Bind({{'A', 0}, Sample(), false});
    Press({'A', 0});
    ASSERT_TRUE(listener.WaitIdle(2s));
    EXPECT_EQ(output->events.size(), 4u);
}

TEST_F(ListenerTests, EscapeStopsExecutionAndWindowsCombinationsPassThrough)
{
    binder.Bind({{'A', 0}, Sample({Key('Z', 0, 60000)}), true});
    Press({'A', MOD_WIN});
    EXPECT_TRUE(output->events.empty());
    Press({'A', 0});
    ASSERT_TRUE(output->WaitFor(1));
    EXPECT_TRUE(listener.ProcessEvent({VK_ESCAPE, 0}, true));
    EXPECT_TRUE(listener.ProcessEvent({VK_ESCAPE, 0}, false));
    ASSERT_TRUE(listener.WaitIdle(2s));
    EXPECT_EQ(output->events, (std::vector<std::string>{"key:90:down", "key:90:up"}));
}

TEST_F(ListenerTests, EscapePassesThroughWhenIdleEvenWithModifiers)
{
    binder.Bind({{'A', 0}, Sample(), true});
    for (UINT modifiers : {0u, UINT(MOD_CONTROL)})
    {
        EXPECT_FALSE(listener.ProcessEvent({VK_ESCAPE, modifiers}, true, false, false));
        EXPECT_FALSE(listener.ProcessEvent({VK_ESCAPE, modifiers}, true));
        EXPECT_FALSE(listener.ProcessEvent({VK_ESCAPE, modifiers}, false));
    }
    EXPECT_TRUE(output->events.empty());
}

TEST(MacroTests, ReportsAndRetriesAFailedReleaseDuringCancellation)
{
    Output output;
    output.failAt = 1;
    EXPECT_THROW(Sample({Key('A', 0, 500)})->Execute(output, [](auto delay) { return delay == 0ms; }), std::runtime_error);
    EXPECT_EQ(output.events, (std::vector<std::string>{"key:65:down", "key:65:up"}));
}

TEST_F(ListenerTests, ExecutesQueuedMacrosSeriallyInOrder)
{
    binder.Bind({{'A', 0}, Sample({Key('X', 0, 5)}), false});
    binder.Bind({{'B', 0}, Sample({Key('Y')}), false});
    Press({'A', 0});
    Press({'B', 0});
    ASSERT_TRUE(listener.WaitIdle(2s));
    EXPECT_EQ(output->events, (std::vector<std::string>{"key:88:down", "key:88:up", "key:89:down", "key:89:up"}));
}

TEST_F(ListenerTests, ToggleRepeatsUntilPressedAgainAndAllowsOtherMacrosToRun)
{
    binder.Bind({{'A', 0}, Sample({Key('X', 0, 1)}, Macro::Type::Toggle), false});
    binder.Bind({{'B', 0}, Sample({Key('Y')}), false});
    Press({'A', 0});
    ASSERT_TRUE(output->WaitFor(4));
    Press({'B', 0});
    {
        std::unique_lock<std::mutex> lock(output->mutex);
        ASSERT_TRUE(output->changed.wait_for(lock, 2s, [&] {
            return std::find(output->events.begin(), output->events.end(), "key:89:up") != output->events.end();
        }));
    }
    Press({'A', 0});
    ASSERT_TRUE(listener.WaitIdle(2s));
    EXPECT_GE(std::count(output->events.begin(), output->events.end(), "key:88:down"), 2);
    EXPECT_EQ(std::count(output->events.begin(), output->events.end(), "key:88:down"),
        std::count(output->events.begin(), output->events.end(), "key:88:up"));
    EXPECT_TRUE(errors.empty());
}

TEST_F(ListenerTests, TogglingOffInterruptsLongHoldsAndReleasesInputs)
{
    binder.Bind({{'A', 0}, Sample({Key('Z', 0, 60000)}, Macro::Type::Toggle), true});
    Press({'A', 0});
    ASSERT_TRUE(output->WaitFor(1));
    Press({'A', 0});
    ASSERT_TRUE(listener.WaitIdle(2s));
    EXPECT_EQ(output->events, (std::vector<std::string>{"key:90:down", "key:90:up"}));
}

TEST_F(ListenerTests, QueuedToggleCanBeStoppedBeforeItsFirstExecution)
{
    binder.Bind({{'A', 0}, Sample({Key('X', 0, 100)}), false});
    binder.Bind({{'B', 0}, Sample({Key('Y')}, Macro::Type::Toggle), false});
    Press({'A', 0});
    ASSERT_TRUE(output->WaitFor(1));
    Press({'B', 0});
    Press({'B', 0});
    ASSERT_TRUE(listener.WaitIdle(2s));
    EXPECT_EQ(output->events, (std::vector<std::string>{"key:88:down", "key:88:up"}));
}

TEST_F(ListenerTests, StopHotkeyClearsToggleStateAndAllowsRestart)
{
    binder.Bind({{'A', 0}, Sample({Key('X', 0, 60000)}, Macro::Type::Toggle), false});
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        Press({'A', 0});
        ASSERT_TRUE(output->WaitFor(attempt * 2 + 1));
        Press({VK_ESCAPE, 0});
        ASSERT_TRUE(listener.WaitIdle(2s));
    }
    EXPECT_EQ(output->events, (std::vector<std::string>{"key:88:down", "key:88:up", "key:88:down", "key:88:up"}));
}

TEST_F(ListenerTests, PauseBlocksFollowingActionsAndResumeCompletesPlayback)
{
    MacroAction delay;
    delay.duration = 200ms;
    binder.Bind({{'A', 0}, Sample({Key('X', 0, 60000), delay, Key('Y')}), false});
    listener.Pause(true);
    Press({'A', 0});
    ASSERT_TRUE(output->WaitFor(1));
    listener.Pause(true);
    EXPECT_FALSE(listener.WaitIdle(30ms));
    listener.Cancel();
    ASSERT_TRUE(listener.WaitIdle(2s));
    EXPECT_EQ(output->events, (std::vector<std::string>{"key:88:down", "key:88:up"}));

    output->events.clear();
    binder.Bind({{'A', 0}, Sample({Key('X', 0, 200), Key('Y')}), false});
    Press({'A', 0});
    ASSERT_TRUE(output->WaitFor(1));
    listener.Pause(true);
    EXPECT_FALSE(listener.WaitIdle(250ms));
    {
        std::lock_guard<std::mutex> lock(output->mutex);
        EXPECT_EQ(output->events, (std::vector<std::string>{"key:88:down"}));
    }
    listener.Pause(false);
    ASSERT_TRUE(listener.WaitIdle(2s));
    EXPECT_EQ(output->events, (std::vector<std::string>{"key:88:down", "key:88:up", "key:89:down", "key:89:up"}));
}

TEST_F(ListenerTests, StopWhilePausedReleasesInputsAndCanRestart)
{
    binder.Bind({{'A', 0}, Sample({Key('X', 0, 60000)}), false});
    Press({'A', 0});
    ASSERT_TRUE(output->WaitFor(1));
    listener.Pause(true);
    listener.Stop();
    EXPECT_EQ(output->events, (std::vector<std::string>{"key:88:down", "key:88:up"}));
    listener.Start(false);
    binder.Bind({{'A', 0}, Sample(), false});
    Press({'A', 0});
    ASSERT_TRUE(listener.WaitIdle(2s));
    EXPECT_EQ(output->events.size(), 4u);
}

TEST(ListenerNativeTests, ReportsActivePausedResumedAndIdlePlayback)
{
    Binder binder;
    binder.Replace({}, VK_ESCAPE);
    auto output = std::make_shared<Output>();
    std::mutex statusMutex;
    std::vector<std::pair<bool, bool>> statuses;
    MacroListener listener(binder, output, {}, [&](bool active, bool paused) {
        std::lock_guard<std::mutex> lock(statusMutex);
        statuses.emplace_back(active, paused);
    });
    binder.Bind({{'A', 0}, Sample({Key('X', 0, 60000)}), false});
    listener.Start(false);
    listener.ProcessEvent({'A', 0}, true);
    listener.ProcessEvent({'A', 0}, false);
    ASSERT_TRUE(output->WaitFor(1));
    listener.Pause(true);
    listener.Pause(false);
    listener.Cancel();
    ASSERT_TRUE(listener.WaitIdle(2s));
    std::lock_guard<std::mutex> lock(statusMutex);
    EXPECT_EQ(statuses, (std::vector<std::pair<bool, bool>>{{true, false}, {true, true}, {true, false}, {false, false}}));
}

TEST(BinderTests, StopKeyReservationRejectsConflictsAndChangesAtomically)
{
    Binder binder;
    binder.Replace({{{'A', MOD_CONTROL}, Sample(), false}}, VK_F9);
    EXPECT_EQ(binder.StopKey(), UINT(VK_F9));
    EXPECT_THROW(binder.Bind({{VK_F9, 0}, Sample(), false}), std::invalid_argument);
    EXPECT_THROW(binder.Bind({{VK_F9, MOD_ALT}, Sample(), false}), std::invalid_argument);
    EXPECT_THROW(binder.Replace({{{'B', 0}, Sample(), false}}, VK_CONTROL), std::invalid_argument);
    EXPECT_THROW(binder.Replace({{{VK_F8, MOD_SHIFT}, Sample(), false}}, VK_F8), std::invalid_argument);
    EXPECT_EQ(binder.StopKey(), UINT(VK_F9));
    EXPECT_TRUE(binder.Find({'A', MOD_CONTROL}));
    binder.Replace({{{VK_F9, 0}, Sample(), false}}, VK_F8);
    EXPECT_EQ(binder.StopKey(), UINT(VK_F8));
    EXPECT_TRUE(binder.Find({VK_F9, 0}));
    binder.Replace({});
    EXPECT_EQ(binder.StopKey(), UINT(VK_F8));
}

TEST(BinderTests, StopKeyCaptureAcceptsEscapeAndDiscardsModifiers)
{
    Binder binder;
    std::optional<Trigger> captured;
    binder.BeginCapture([&](auto value) { captured = value; }, true);
    EXPECT_FALSE(binder.CaptureKey({VK_CONTROL, MOD_CONTROL}));
    EXPECT_TRUE(binder.Capturing());
    EXPECT_TRUE(binder.CaptureKey({VK_ESCAPE, MOD_CONTROL | MOD_SHIFT}));
    ASSERT_TRUE(captured);
    EXPECT_EQ(captured->key, UINT(VK_ESCAPE));
    EXPECT_EQ(captured->modifiers, 0u);
    EXPECT_FALSE(binder.Capturing());
    EXPECT_TRUE(Trigger::ValidStopKey(VK_F12));
    EXPECT_TRUE(Trigger::ValidStopKey(0x100 | VK_RETURN));
    EXPECT_FALSE(Trigger::ValidStopKey(VK_LWIN));
    EXPECT_FALSE(Trigger::ValidStopKey(0));
}

TEST_F(ListenerTests, MacrosCannotRunWithoutAConfiguredStopKey)
{
    binder.Replace({{{'A', 0}, Sample(), true}}, 0);
    EXPECT_FALSE(listener.ProcessEvent({'A', 0}, true));
    EXPECT_FALSE(listener.ProcessEvent({'A', 0}, false));
    ASSERT_TRUE(listener.WaitIdle(2s));
    EXPECT_TRUE(output->events.empty());
    binder.Replace({{{'A', 0}, Sample(), true}}, VK_F9);
    Press({'A', 0});
    ASSERT_TRUE(listener.WaitIdle(2s));
    EXPECT_EQ(output->events.size(), 2u);
}

TEST_F(ListenerTests, StopKeyCancelsActiveAndQueuedMacrosEvenWithModifiersAndWhileEditing)
{
    binder.Replace({{{'A', 0}, Sample({Key('X', MOD_CONTROL, 60000)}), false}}, VK_F9);
    Press({'A', 0});
    ASSERT_TRUE(output->WaitFor(2));
    Press({'A', 0});
    EXPECT_TRUE(listener.ProcessEvent({VK_F9, MOD_ALT}, true, false, false));
    EXPECT_TRUE(listener.ProcessEvent({VK_F9, MOD_ALT}, true, false, false));
    EXPECT_TRUE(listener.ProcessEvent({VK_F9, MOD_ALT}, false, false, false));
    ASSERT_TRUE(listener.WaitIdle(2s));
    EXPECT_EQ(output->events, (std::vector<std::string>{"key:17:down", "key:88:down", "key:88:up", "key:17:up"}));
}

TEST_F(ListenerTests, InjectedStopKeyDoesNotInterruptPlaybackAndCaptureDoesNotRunMacros)
{
    binder.Replace({{{'A', 0}, Sample({Key('X', 0, 60000)}), false}}, VK_F9);
    Press({'A', 0});
    ASSERT_TRUE(output->WaitFor(1));
    EXPECT_FALSE(listener.ProcessEvent({VK_F9, 0}, true, true));
    EXPECT_FALSE(listener.ProcessEvent({VK_F9, 0}, false, true));
    EXPECT_FALSE(listener.WaitIdle(20ms));
    Press({VK_F9, 0});
    ASSERT_TRUE(listener.WaitIdle(2s));
    std::optional<Trigger> captured;
    binder.BeginCapture([&](auto value) { captured = value; }, true);
    Press({VK_ESCAPE, 0});
    ASSERT_TRUE(captured);
    EXPECT_EQ(captured->key, UINT(VK_ESCAPE));
    EXPECT_EQ(output->events.size(), 2u);
}

TEST(ListenerNativeTests, InstallsRemovesAndRestartsNativeHookWithoutSendingInput)
{
    Binder binder;
    const auto output = std::make_shared<Output>();
    EXPECT_THROW(MacroListener(binder, nullptr), std::invalid_argument);
    MacroListener listener(binder, output);
    ASSERT_NO_THROW(listener.Start());
    EXPECT_NO_THROW(listener.Start());
    EXPECT_NO_THROW(listener.Stop());
    ASSERT_NO_THROW(listener.Start());
    EXPECT_NO_THROW(listener.Stop());
    EXPECT_TRUE(output->events.empty());
}
}
