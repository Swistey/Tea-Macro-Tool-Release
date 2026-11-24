#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <dwmapi.h>
#include "2d/matcher.hpp"
#include "2d/recognizer.hpp"

using namespace tea::vision;

namespace
{
enum class CaptureFailure { None, Screen, Memory, Bitmap, Pixels, SelectNull, SelectError, Blit, Flush,
    Window, Hidden, Minimized, Client, Origin };

struct CaptureCalls
{
    CaptureFailure failure = CaptureFailure::None;
    Rect desktop{-200, -100, 640, 480}, client{0, 0, 3, 2}, region;
    Pixel* pixels = nullptr;
    HBITMAP bitmap = nullptr;
    BITMAPINFO info{};
    std::string cleanup;
    bool selected = false;
} captureCalls;

int WINAPI RecordCaptureMetrics(int metric)
{
    switch (metric)
    {
    case SM_XVIRTUALSCREEN: return captureCalls.desktop.x;
    case SM_YVIRTUALSCREEN: return captureCalls.desktop.y;
    case SM_CXVIRTUALSCREEN: return captureCalls.desktop.width;
    case SM_CYVIRTUALSCREEN: return captureCalls.desktop.height;
    default: return 0;
    }
}

HDC WINAPI RecordCaptureDC(HWND window)
{
    return captureCalls.failure == CaptureFailure::Screen ? nullptr : GetDC(window);
}

HDC WINAPI RecordCaptureMemory(HDC screen)
{
    return captureCalls.failure == CaptureFailure::Memory ? nullptr : CreateCompatibleDC(screen);
}

HBITMAP WINAPI RecordCaptureBitmap(HDC screen, const BITMAPINFO* info, UINT usage, void** pixels,
    HANDLE section, DWORD offset)
{
    captureCalls.info = *info;
    if (captureCalls.failure == CaptureFailure::Bitmap) return nullptr;
    captureCalls.bitmap = CreateDIBSection(screen, info, usage, pixels, section, offset);
    captureCalls.pixels = static_cast<Pixel*>(*pixels);
    if (captureCalls.failure == CaptureFailure::Pixels) *pixels = nullptr;
    return captureCalls.bitmap;
}

HGDIOBJ WINAPI RecordCaptureSelect(HDC context, HGDIOBJ object)
{
    if (!captureCalls.selected)
    {
        if (captureCalls.failure == CaptureFailure::SelectNull) return nullptr;
        if (captureCalls.failure == CaptureFailure::SelectError) return HGDI_ERROR;
        captureCalls.selected = true;
    }
    else
    {
        captureCalls.cleanup += 'S';
        captureCalls.selected = false;
    }
    return SelectObject(context, object);
}

BOOL WINAPI RecordCaptureBlit(HDC, int x, int y, int width, int height, HDC, int sourceX, int sourceY, DWORD flags)
{
    EXPECT_EQ(x, 0);
    EXPECT_EQ(y, 0);
    EXPECT_EQ(flags, DWORD(SRCCOPY | CAPTUREBLT));
    captureCalls.region = {sourceX, sourceY, width, height};
    if (captureCalls.failure == CaptureFailure::Blit)
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    for (int row = 0; row < height; ++row) for (int column = 0; column < width; ++column)
        captureCalls.pixels[row * width + column] =
            {static_cast<std::uint8_t>(column), static_cast<std::uint8_t>(row), 123, 0};
    return TRUE;
}

BOOL WINAPI RecordCaptureFlush() { return captureCalls.failure != CaptureFailure::Flush; }
BOOL WINAPI RecordCaptureDeleteObject(HGDIOBJ object) { captureCalls.cleanup += 'B'; return DeleteObject(object); }
BOOL WINAPI RecordCaptureDeleteDC(HDC context) { captureCalls.cleanup += 'M'; return DeleteDC(context); }
int WINAPI RecordCaptureReleaseDC(HWND window, HDC context) { captureCalls.cleanup += 'D'; return ReleaseDC(window, context); }
BOOL WINAPI RecordCaptureWindow(HWND) { return captureCalls.failure != CaptureFailure::Window; }
BOOL WINAPI RecordCaptureVisible(HWND) { return captureCalls.failure != CaptureFailure::Hidden; }
BOOL WINAPI RecordCaptureIconic(HWND) { return captureCalls.failure == CaptureFailure::Minimized; }

BOOL WINAPI RecordCaptureClient(HWND, RECT* area)
{
    if (captureCalls.failure == CaptureFailure::Client) return FALSE;
    const auto& client = captureCalls.client;
    *area = {client.x, client.y, client.width, client.height};
    return TRUE;
}

BOOL WINAPI RecordCaptureOrigin(HWND, POINT* origin)
{
    *origin = {captureCalls.desktop.x, captureCalls.desktop.y};
    return captureCalls.failure != CaptureFailure::Origin;
}
}

#define GetSystemMetrics RecordCaptureMetrics
#define GetDC RecordCaptureDC
#define CreateCompatibleDC RecordCaptureMemory
#define CreateDIBSection RecordCaptureBitmap
#define SelectObject RecordCaptureSelect
#define BitBlt RecordCaptureBlit
#define GdiFlush RecordCaptureFlush
#define DeleteObject RecordCaptureDeleteObject
#define DeleteDC RecordCaptureDeleteDC
#define ReleaseDC RecordCaptureReleaseDC
#define IsWindow RecordCaptureWindow
#define IsWindowVisible RecordCaptureVisible
#define IsIconic RecordCaptureIconic
#define GetClientRect RecordCaptureClient
#define ClientToScreen RecordCaptureOrigin
#define CaptureScreen InjectedCaptureScreen
#define CaptureWindow InjectedCaptureWindow
#include "2d/capture.cpp"
#undef CaptureWindow
#undef CaptureScreen
#undef ClientToScreen
#undef GetClientRect
#undef IsIconic
#undef IsWindowVisible
#undef IsWindow
#undef ReleaseDC
#undef DeleteDC
#undef DeleteObject
#undef GdiFlush
#undef BitBlt
#undef SelectObject
#undef CreateDIBSection
#undef CreateCompatibleDC
#undef GetDC
#undef GetSystemMetrics

namespace
{
Image Solid(int width, int height, Pixel color = {})
{
    return {width, height, std::vector<Pixel>(static_cast<std::size_t>(width) * height, color)};
}

Target Sample(std::string id = "button", Pixel color = {255, 255, 255, 255})
{
    return {std::move(id), Solid(1, 1, color), {1, std::nullopt}};
}

void ExpectBounds(const Rect& value, int x, int y, int width, int height)
{
    EXPECT_EQ(value.x, x);
    EXPECT_EQ(value.y, y);
    EXPECT_EQ(value.width, width);
    EXPECT_EQ(value.height, height);
}

std::optional<Detection> ReferenceMatch(const Image& frame, const Target& target)
{
    const Rect area = target.options.region.value_or(Rect{0, 0, frame.width, frame.height});
    int opacity = 0;
    for (const auto& pixel : target.image.pixels) opacity += pixel.alpha;
    std::optional<Detection> best;
    for (int y = area.y; y + target.image.height <= area.y + area.height; ++y)
        for (int x = area.x; x + target.image.width <= area.x + area.width; ++x)
        {
            int error = 0;
            for (std::size_t i = 0; i < target.image.pixels.size(); ++i)
            {
                const Pixel a = frame.pixels[(y + i / target.image.width) * frame.width + x + i % target.image.width];
                const Pixel b = target.image.pixels[i];
                const auto difference = [&](int current, int foreground) {
                    const double transparency = 1.0 - b.alpha / 255.0;
                    const double base = foreground * (1.0 - transparency);
                    return std::abs(current - std::clamp(current, int(std::round(base)),
                        int(std::round(base + 255 * transparency))));
                };
                const int red = difference(a.red, b.red), green = difference(a.green, b.green), blue = difference(a.blue, b.blue);
                if (std::max({red, green, blue}) > target.options.maximumChannelError)
                {
                    error = -1;
                    break;
                }
                error += b.alpha * (red + green + blue);
            }
            if (error < 0) continue;
            const double score = 1 - error / (765.0 * opacity);
            if (score >= target.options.threshold && (!best || score > best->confidence))
                best = Detection{target.id, {frame.x + x, frame.y + y, target.image.width, target.image.height}, score, {}};
        }
    return best;
}

TEST(ImageTests, ValidatesDimensionsStorageAndCoordinateOverflow)
{
    EXPECT_FALSE(Image{}.Valid());
    EXPECT_TRUE(Solid(2, 3).Valid());
    Image image = Solid(2, 3);
    image.width = 0;
    EXPECT_FALSE(image.Valid());
    image.width = -2;
    EXPECT_FALSE(image.Valid());
    image.width = 2;
    image.height = -3;
    EXPECT_FALSE(image.Valid());
    image.height = 3;
    image.pixels.pop_back();
    EXPECT_FALSE(image.Valid());
    image.pixels.resize(7);
    EXPECT_FALSE(image.Valid());
    image.pixels.resize(6);
    image.x = std::numeric_limits<int>::max();
    EXPECT_FALSE(image.Valid());
    image.x = std::numeric_limits<int>::min();
    image.y = -100;
    EXPECT_TRUE(image.Valid());
    image.y = std::numeric_limits<int>::max();
    EXPECT_FALSE(image.Valid());
}

TEST(MatcherTests, FindsMulticolorPatternAtBottomRightAndPreservesScreenOrigin)
{
    Image frame = Solid(4, 3);
    frame.x = -100;
    frame.y = 200;
    Target target{"icon", {2, 2, {{0, 0, 255}, {0, 255, 0}, {255, 0, 0}, {255, 255, 255}}}, {1}};
    frame.pixels[6] = target.image.pixels[0];
    frame.pixels[7] = target.image.pixels[1];
    frame.pixels[10] = target.image.pixels[2];
    frame.pixels[11] = target.image.pixels[3];
    const auto before = std::chrono::steady_clock::now();
    auto result = Match(frame, target);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->targetId, "icon");
    ExpectBounds(result->bounds, -98, 201, 2, 2);
    EXPECT_NEAR(result->confidence, 1, 1e-12);
    EXPECT_GE(result->timestamp, before);
    EXPECT_LE(result->timestamp, std::chrono::steady_clock::now());
}

TEST(MatcherTests, HandlesSameSizeImagesAndIgnoresFrameAlpha)
{
    Image frame = Solid(2, 2, {23, 45, 67, 0});
    Target target{"icon", Solid(2, 2, {23, 45, 67, 255}), {1}};
    auto result = Match(frame, target);
    ASSERT_TRUE(result);
    ExpectBounds(result->bounds, 0, 0, 2, 2);
    EXPECT_NEAR(result->confidence, 1, 1e-12);
}

TEST(MatcherTests, MatchesTransparentCornersAgainstTheScreenBackground)
{
    const Pixel foreground{210, 120, 50};
    const Pixel background{255, 255, 255, 0};
    Image frame = Solid(33, 16, background);
    Target target{"icon", Solid(16, 16, foreground), {1, std::nullopt, 24}};
    for (int row = 0; row < 16; ++row)
        std::fill_n(frame.pixels.begin() + 33 * row + 17, 16, foreground);
    for (int row : {0, 15}) for (int column : {0, 15})
    {
        target.image.pixels[16 * row + column] = {0, 0, 0, 0};
        frame.pixels[33 * row + 17 + column] = background;
    }
    for (bool firstMatch : {false, true})
    {
        auto match = Match(frame, target, {}, firstMatch);
        ASSERT_TRUE(match);
        ExpectBounds(match->bounds, 17, 0, 16, 16);
        EXPECT_NEAR(match->confidence, 1, 1e-12);
    }
}

TEST(MatcherTests, ScoresPartialOpacityAgainstTheRangeOfPossibleBackgrounds)
{
    Target target{"icon", {2, 1, {{20, 100, 200, 128}, {100, 100, 100}}}, {0.98, std::nullopt, 24}};
    Image frame{2, 1, {{10, 177, 100}, {100, 100, 100}}};
    auto match = Match(frame, target);
    ASSERT_TRUE(match);
    EXPECT_NEAR(match->confidence, 1, 1e-12);

    frame.pixels[0].red = 97;
    match = Match(frame, target);
    ASSERT_TRUE(match);
    EXPECT_NEAR(match->confidence, 1 - (3.0 * 128) / (765.0 * (255 + 128)), 1e-12);

    frame.pixels[0].red = 75;
    EXPECT_FALSE(Match(frame, target));
}

TEST(MatcherTests, RejectsInvisibleAndNearlyTransparentTargets)
{
    const auto frame = Solid(2, 2, {100, 100, 100});
    for (std::uint8_t alpha : {0, 1, 5, 63})
    {
        const Target target{"too transparent", Solid(1, 1, {255, 255, 255, alpha}), {0.98, std::nullopt, 24}};
        EXPECT_THROW(Match(frame, target), std::invalid_argument);
    }
    const Target visible{"visible", Solid(1, 1, {255, 255, 255, 64}), {0.98, std::nullopt, 24}};
    EXPECT_FALSE(Match(Solid(2, 2), visible));
    EXPECT_TRUE(Match(frame, visible));
}

TEST(MatcherTests, ReturnsNoDetectionForMismatchOrOversizedPattern)
{
    EXPECT_FALSE(Match(Solid(2, 2), Sample()));
    EXPECT_FALSE(Match(Solid(2, 2), Target{"wide", Solid(3, 1)}));
    EXPECT_FALSE(Match(Solid(2, 2), Target{"tall", Solid(1, 3)}));
}

TEST(MatcherTests, FindsBestApproximateMatchAndKeepsFirstTie)
{
    Image frame{4, 1, {{80, 80, 80}, {95, 95, 95}, {95, 95, 95}, {20, 20, 20}}};
    Target target = Sample("gray", {100, 100, 100});
    target.options.threshold = 0.9;
    auto result = Match(frame, target);
    ASSERT_TRUE(result);
    ExpectBounds(result->bounds, 1, 0, 1, 1);
    EXPECT_NEAR(result->confidence, 1 - 5.0 / 255, 1e-12);
}

TEST(MatcherTests, ThresholdIsInclusiveAndMeasuresAllRgbChannels)
{
    Image frame = Solid(1, 1, {10, 20, 30});
    Target target = Sample("color", {0, 0, 0});
    const double score = 1 - 60.0 / 765;
    target.options.threshold = score;
    auto result = Match(frame, target);
    ASSERT_TRUE(result);
    EXPECT_NEAR(result->confidence, score, 1e-12);
    target.options.threshold = std::nextafter(score, 1.0);
    EXPECT_FALSE(Match(frame, target));
    target.options.threshold = 0;
    auto opposite = Match(Solid(1, 1), Target{"white", Solid(1, 1, {255, 255, 255}), {0}});
    ASSERT_TRUE(opposite);
    EXPECT_NEAR(opposite->confidence, 0, 1e-12);
}

TEST(MatcherTests, ChannelLimitKeepsSparseImageFeaturesFromDisappearingIntoTheAverage)
{
    Target target{"icon", Solid(16, 16)};
    std::fill_n(target.image.pixels.begin() + 16 * 7 + 2, 5, Pixel{255, 255, 255});
    Image frame = Solid(33, 16);
    ASSERT_TRUE(Match(frame, target));

    target.options.maximumChannelError = 24;
    EXPECT_FALSE(Match(frame, target));
    EXPECT_FALSE(Match(frame, target, {}, true));

    std::fill_n(frame.pixels.begin() + 33 * 7 + 19, 5, Pixel{252, 252, 252, 0});
    for (bool firstMatch : {false, true})
    {
        const auto match = Match(frame, target, {}, firstMatch);
        ASSERT_TRUE(match);
        ExpectBounds(match->bounds, 17, 0, 16, 16);
        EXPECT_NEAR(match->confidence, 1 - 15.0 / (256 * 255), 1e-12);
    }
}

TEST(MatcherTests, ChannelLimitIsInclusiveAndStillRequiresTheMeanThreshold)
{
    Target target{"gray", Solid(5, 4, {100, 100, 100}), {0.98, std::nullopt, 24}};
    Image frame = Solid(5, 4, {103, 103, 103, 0});
    for (int pixel : {0, 4})
    {
        frame.pixels[pixel].red = 125;
        EXPECT_FALSE(Match(frame, target));

        frame.pixels[pixel].red = 124;
        const auto match = Match(frame, target);
        ASSERT_TRUE(match);
        EXPECT_NEAR(match->confidence, 1 - (19 * 9 + 30.0) / (20 * 765), 1e-12);
        frame.pixels[pixel].red = 103;
    }
    EXPECT_FALSE(Match(Solid(5, 4, {106, 106, 106}), target));

    for (int maximum : {-1, 256})
    {
        target.options.maximumChannelError = maximum;
        EXPECT_THROW(Match(frame, target), std::invalid_argument);
    }
}

TEST(MatcherTests, RejectsSparseColorMismatchBeforeScanningEveryRow)
{
    Target target{"icon", Solid(96, 65, {255, 0, 0}), {0.98, std::nullopt, 24}};
    target.image.pixels.back() = {0, 0, 255};
    Image frame = Solid(160, 100, {255, 0, 0});
    int checks = 0;
    const auto keepSearching = [&] { ++checks; return true; };
    EXPECT_FALSE(Match(frame, target, keepSearching, true));
    EXPECT_LT(checks, 1000);

    frame.pixels.back() = {24, 0, 231};
    const auto match = Match(frame, target, {}, true);
    ASSERT_TRUE(match);
    ExpectBounds(match->bounds, 64, 35, 96, 65);
    EXPECT_NEAR(match->confidence, 1 - 48.0 / (96 * 65 * 765), 1e-12);
}

TEST(MatcherTests, TestsExtremesAcrossChannelsBeforeScanningNearUniformCandidates)
{
    Target target{"icon", Solid(96, 65, {100, 100, 100}), {0.98, std::nullopt, 24}};
    target.image.pixels[0] = {80, 120, 100};
    target.image.pixels[1] = {124, 76, 100};
    target.image.pixels.back() = {100, 100, 125};
    Image frame = Solid(160, 100, {100, 100, 100});
    int checks = 0;
    EXPECT_FALSE(Match(frame, target, [&] { ++checks; return true; }, true));
    EXPECT_LT(checks, 1000);

    frame.pixels.back().red = 101;
    const auto match = Match(frame, target, {}, true);
    ASSERT_TRUE(match);
    ExpectBounds(match->bounds, 64, 35, 96, 65);
}

TEST(MatcherTests, ProbesTheTightestTranslucentColorBoundsBeforeScanningRows)
{
    Target target{"icon", Solid(96, 65, {100, 100, 100, 128}), {0.98, std::nullopt, 24}};
    target.image.pixels.back() = {100, 100, 255, 200};
    Image frame = Solid(160, 100, {100, 100, 100});
    int checks = 0;
    EXPECT_FALSE(Match(frame, target, [&] { ++checks; return true; }, true));
    EXPECT_LT(checks, 1000);

    frame.pixels.back().red = 176;
    const auto match = Match(frame, target, {}, true);
    ASSERT_TRUE(match);
    ExpectBounds(match->bounds, 64, 35, 96, 65);
}

TEST(MatcherTests, ScoresEveryRowAndContinuesAfterRejectingACandidate)
{
    Image frame{3, 2, {{0, 0, 0}, {90, 90, 90}, {95, 95, 95},
        {0, 0, 0}, {95, 95, 95}, {100, 100, 100}}};
    Target target{"gray", Solid(2, 2, {100, 100, 100}), {0.95}};
    auto result = Match(frame, target);
    ASSERT_TRUE(result);
    ExpectBounds(result->bounds, 1, 0, 2, 2);
    EXPECT_NEAR(result->confidence, 1 - 5.0 / 255, 1e-12);
    target.options.threshold = 0.99;
    EXPECT_FALSE(Match(frame, target));
}

TEST(MatcherTests, RestrictsSearchToFrameRelativeRegion)
{
    Image frame = Solid(4, 3, {255, 255, 255});
    frame.x = -50;
    frame.y = -60;
    Target target = Sample();
    target.options.region = Rect{3, 2, 1, 1};
    auto result = Match(frame, target);
    ASSERT_TRUE(result);
    ExpectBounds(result->bounds, -47, -58, 1, 1);
    frame.pixels[11] = {};
    EXPECT_FALSE(Match(frame, target));
    target.image = Solid(2, 1);
    EXPECT_FALSE(Match(frame, target));
}

TEST(MatcherTests, RejectsInvalidFramesTargetsThresholdsAndRegions)
{
    Image frame = Solid(2, 2);
    EXPECT_THROW(Match({}, Sample()), std::invalid_argument);
    EXPECT_THROW(Match(frame, Target{}), std::invalid_argument);
    EXPECT_THROW(Match(frame, Sample("")), std::invalid_argument);
    for (double threshold : {-0.1, 1.1, std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
    {
        Target target = Sample();
        target.options.threshold = threshold;
        EXPECT_THROW(Match(frame, target), std::invalid_argument);
    }
    for (Rect area : {Rect{-1, 0, 1, 1}, Rect{0, -1, 1, 1}, Rect{0, 0, 0, 1},
        Rect{0, 0, 1, -1}, Rect{1, 0, 2, 1}, Rect{0, 1, 1, 2}, Rect{3, 0, 1, 1},
        Rect{0, 3, 1, 1}, Rect{std::numeric_limits<int>::max(), 0, 2, 1}})
    {
        Target target = Sample();
        target.options.region = area;
        EXPECT_THROW(Match(frame, target), std::invalid_argument);
    }
}

TEST(MatcherTests, PollsForCancellationWithinAHorizontalScanAndWithinACandidate)
{
    const Pixel white{255, 255, 255};
    Image strip{256, 1, std::vector<Pixel>(256)};
    strip.pixels[200] = white;
    const Image square{64, 64, std::vector<Pixel>(4096, white)};
    const auto verify = [](const Image& frame, const Image& target) {
        ASSERT_TRUE(Match(frame, {"target", target, {}}));
        int checks = 0;
        EXPECT_FALSE(Match(frame, {"target", target, {}}, [&] { return ++checks < 5; }));
        EXPECT_EQ(checks, 5);
    };
    verify(strip, {1, 1, {white}});
    verify(square, square);
}

TEST(MatcherTests, FindsRandomPatternsAtEveryBorderWithoutChangingInputs)
{
    std::mt19937 random(42);
    for (int width = 1; width <= 6; ++width)
    {
        for (int height = 1; height <= 5; ++height)
        {
            Target target{"random", Solid(width, height), {1}};
            for (auto& pixel : target.image.pixels)
                pixel = {static_cast<std::uint8_t>(random()), static_cast<std::uint8_t>(random()),
                    static_cast<std::uint8_t>(random())};
            for (int y : {0, 3}) for (int x : {0, 4})
            {
                Image frame = Solid(width + 4, height + 3);
                for (int row = 0; row < height; ++row)
                    std::copy_n(target.image.pixels.begin() + row * width, width,
                        frame.pixels.begin() + (row + y) * frame.width + x);
                auto result = Match(frame, target);
                ASSERT_TRUE(result) << width << 'x' << height << " at " << x << ',' << y;
                ExpectBounds(result->bounds, x, y, width, height);
                EXPECT_NEAR(result->confidence, 1, 1e-12);
                EXPECT_EQ(frame.pixels[y * frame.width + x].red, target.image.pixels[0].red);
            }
        }
    }
}

TEST(MatcherTests, AgreesWithExhaustiveReferenceAcrossRandomImagesAndThresholdBoundaries)
{
    std::mt19937 random(857);
    auto randomize = [&](Image& image, bool opaque) {
        for (auto& pixel : image.pixels)
            pixel = {static_cast<std::uint8_t>(random()), static_cast<std::uint8_t>(random()),
                static_cast<std::uint8_t>(random()), static_cast<std::uint8_t>(opaque ? 255 : random())};
    };
    for (int trial = 0; trial < 200; ++trial)
    {
        SCOPED_TRACE(trial);
        Image frame = Solid(6 + random() % 8, 4 + random() % 6);
        frame.x = -20;
        frame.y = 40;
        randomize(frame, false);
        Target target{"random", Solid(1 + trial % 9, 1 + trial % 5), {0}};
        randomize(target.image, trial % 4 < 2);
        if (std::none_of(target.image.pixels.begin(), target.image.pixels.end(), [](Pixel pixel) { return pixel.alpha >= 64; }))
            target.image.pixels.front().alpha = 64;
        if (trial % 3 == 0) target.options.maximumChannelError = 24;
        if (trial % 2) target.options.region = Rect{1, 1, frame.width - 1, frame.height - 1};
        auto best = ReferenceMatch(frame, target);
        const double boundary = best ? best->confidence : 0.5;
        for (double threshold : {0.0, 0.35, 0.7, 0.98, 1.0, boundary, std::nextafter(boundary, 1.0)})
        {
            target.options.threshold = threshold;
            auto expected = ReferenceMatch(frame, target);
            auto actual = Match(frame, target);
            ASSERT_EQ(actual.has_value(), expected.has_value()) << threshold;
            if (actual)
            {
                ExpectBounds(actual->bounds, expected->bounds.x, expected->bounds.y,
                    expected->bounds.width, expected->bounds.height);
                EXPECT_NEAR(actual->confidence, expected->confidence, 1e-12);
            }
        }
    }
}

TEST(MatcherTests, DoesNotRejectEqualBrightnessColorsAndIgnoresFrameAlphaInVectorBlocks)
{
    Image frame = Solid(6, 2, {0, 255, 0, 0});
    Target target{"red", Solid(5, 2, {0, 0, 255}), {0}};
    auto result = Match(frame, target);
    ASSERT_TRUE(result);
    EXPECT_NEAR(result->confidence, 1.0 / 3, 1e-12);
    target.options.threshold = 0.5;
    EXPECT_FALSE(Match(frame, target));
    target.image = Solid(5, 2, {0, 255, 0, 255});
    target.options.threshold = 1;
    result = Match(frame, target);
    ASSERT_TRUE(result);
    EXPECT_NEAR(result->confidence, 1, 1e-12);
}

TEST(RecognizerTests, CapturesOnceAndReportsOnlyMatchesInTargetOrder)
{
    int captures = 0;
    const auto thread = std::this_thread::get_id();
    Recognizer recognizer([&] { ++captures; return Image{2, 1, {{255, 255, 255}, {0, 0, 0}}}; });
    recognizer.SetTargets({Sample("white"), Sample("missing", {10, 20, 30}), Sample("black", {})});
    std::vector<Detection> reported;
    recognizer.SetOnDetection([&](const Detection& detection) {
        EXPECT_EQ(std::this_thread::get_id(), thread);
        reported.push_back(detection);
    });
    auto results = recognizer.Scan();
    EXPECT_EQ(captures, 1);
    ASSERT_EQ(results.size(), 2u);
    ASSERT_EQ(reported.size(), 2u);
    EXPECT_EQ(results[0].targetId, "white");
    EXPECT_EQ(results[1].targetId, "black");
    EXPECT_EQ(reported[0].targetId, results[0].targetId);
    ExpectBounds(reported[1].bounds, 1, 0, 1, 1);
    EXPECT_EQ(results[0].timestamp, results[1].timestamp);
    EXPECT_EQ(reported[0].timestamp, results[0].timestamp);
}

TEST(RecognizerTests, SuppliedFrameBypassesCaptureAndEachScanReportsAgain)
{
    Recognizer recognizer([]() -> Image { throw std::runtime_error("must not capture"); });
    recognizer.SetTargets({Sample()});
    int reports = 0;
    recognizer.SetOnDetection([&](const Detection&) { ++reports; });
    EXPECT_EQ(recognizer.Scan(Solid(1, 1, {255, 255, 255})).size(), 1u);
    EXPECT_EQ(recognizer.Scan(Solid(1, 1, {255, 255, 255})).size(), 1u);
    EXPECT_EQ(reports, 2);
    EXPECT_TRUE(recognizer.Scan(Solid(1, 1)).empty());
    EXPECT_EQ(reports, 2);
}

TEST(RecognizerTests, SupportsEmptyTargetsAndRemovingCallback)
{
    Recognizer recognizer;
    EXPECT_TRUE(recognizer.Scan(Solid(1, 1)).empty());
    recognizer.SetTargets({Sample()});
    int reports = 0;
    recognizer.SetOnDetection([&](const Detection&) { ++reports; });
    recognizer.SetOnDetection({});
    EXPECT_EQ(recognizer.Scan(Solid(1, 1, {255, 255, 255})).size(), 1u);
    EXPECT_EQ(reports, 0);
    recognizer.SetTargets({});
    EXPECT_TRUE(recognizer.Scan(Solid(1, 1, {255, 255, 255})).empty());
}

TEST(RecognizerTests, RejectsInvalidConfigurationWithoutReplacingExistingTargets)
{
    EXPECT_THROW(Recognizer(Recognizer::Capture{}), std::invalid_argument);
    Recognizer recognizer;
    recognizer.SetTargets({Sample("original")});
    EXPECT_THROW(recognizer.SetTargets({Sample("duplicate"), Sample("duplicate")}), std::invalid_argument);
    EXPECT_THROW(recognizer.SetTargets({Target{}}), std::invalid_argument);
    EXPECT_THROW(recognizer.Scan(Image{}), std::invalid_argument);
    auto results = recognizer.Scan(Solid(1, 1, {255, 255, 255}));
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].targetId, "original");
}

TEST(RecognizerTests, AllowsReconfigurationAndNestedScansInsideCallback)
{
    Recognizer recognizer;
    recognizer.SetTargets({Sample("first"), Sample("second")});
    const Image frame = Solid(1, 1, {255, 255, 255});
    int reports = 0;
    recognizer.SetOnDetection([&](const Detection&) {
        ++reports;
        recognizer.SetOnDetection({});
        recognizer.SetTargets({Sample("replacement")});
        auto nested = recognizer.Scan(frame);
        ASSERT_EQ(nested.size(), 1u);
        EXPECT_EQ(nested[0].targetId, "replacement");
    });
    auto results = recognizer.Scan(frame);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(reports, 2);
    EXPECT_EQ(results[1].targetId, "second");
}

TEST(RecognizerTests, PropagatesCaptureAndCallbackErrorsAndCanRecover)
{
    Recognizer recognizer([]() -> Image { throw std::runtime_error("capture failed"); });
    recognizer.SetTargets({Sample()});
    int reports = 0;
    recognizer.SetOnDetection([&](const Detection&) { ++reports; throw std::runtime_error("callback failed"); });
    EXPECT_THROW(recognizer.Scan(), std::runtime_error);
    EXPECT_EQ(reports, 0);
    EXPECT_THROW(recognizer.Scan(Solid(1, 1, {255, 255, 255})), std::runtime_error);
    EXPECT_EQ(reports, 1);
    recognizer.SetOnDetection({});
    EXPECT_EQ(recognizer.Scan(Solid(1, 1, {255, 255, 255})).size(), 1u);
    Recognizer invalid([] { return Image{}; });
    EXPECT_THROW(invalid.Scan(), std::invalid_argument);
}

TEST(RecognizerTests, InvalidSearchRegionDoesNotSendPartialNotifications)
{
    Recognizer recognizer;
    Target target = Sample("outside");
    target.options.region = Rect{0, 0, 2, 2};
    recognizer.SetTargets({Sample(), target});
    int reports = 0;
    recognizer.SetOnDetection([&](const Detection&) { ++reports; });
    EXPECT_THROW(recognizer.Scan(Solid(1, 1, {255, 255, 255})), std::invalid_argument);
    EXPECT_EQ(reports, 0);
}

TEST(RecognizerTests, KeepsInFlightTargetsAndCallbackWhileAnotherThreadReconfigures)
{
    using namespace std::chrono_literals;
    Recognizer recognizer;
    recognizer.SetTargets({Sample("first"), Sample("second")});
    const Image frame = Solid(1, 1, {255, 255, 255});
    std::promise<void> entered, release;
    auto started = entered.get_future();
    auto resume = release.get_future();
    int oldReports = 0, newReports = 0;
    recognizer.SetOnDetection([&](const Detection&) {
        if (++oldReports == 1) { entered.set_value(); resume.wait(); }
    });
    auto scan = std::async(std::launch::async, [&] { return recognizer.Scan(frame); });
    const auto status = started.wait_for(2s);
    EXPECT_EQ(status, std::future_status::ready);
    if (status != std::future_status::ready) { release.set_value(); scan.get(); return; }
    auto update = std::async(std::launch::async, [&] {
        recognizer.SetTargets({Sample("replacement")});
        recognizer.SetOnDetection([&](const Detection&) { ++newReports; });
    });
    EXPECT_EQ(update.wait_for(2s), std::future_status::ready);
    release.set_value();
    update.get();
    auto results = scan.get();
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].targetId, "first");
    EXPECT_EQ(results[1].targetId, "second");
    EXPECT_EQ(oldReports, 2);
    results = recognizer.Scan(frame);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].targetId, "replacement");
    EXPECT_EQ(newReports, 1);
}

TEST(RecognitionBenchmark, DISABLED_ScanPerformance)
{
    std::mt19937 random(42);
    Image noise = Solid(1280, 720);
    for (auto& pixel : noise.pixels)
        pixel = {static_cast<std::uint8_t>(random()), static_cast<std::uint8_t>(random()),
            static_cast<std::uint8_t>(random())};
    Target target{"icon", Solid(32, 32)};
    for (auto& pixel : target.image.pixels)
        pixel = {static_cast<std::uint8_t>(random()), static_cast<std::uint8_t>(random()),
            static_cast<std::uint8_t>(random())};
    auto measure = [](const char* name, const Image& frame, const Target& target, bool found, bool firstMatch = false) {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 3; ++i)
            ASSERT_EQ(Match(frame, target, [] { return true; }, firstMatch).has_value(), found);
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start);
        std::cout << name << ": " << elapsed.count() / 3 << " ms/scan\n";
    };
    measure("720p noise, absent", noise, target, false);
    for (int row = 0; row < target.image.height; ++row)
        std::copy_n(target.image.pixels.begin() + row * target.image.width, target.image.width,
            noise.pixels.end() - (target.image.height - row) * noise.width + noise.width - target.image.width);
    measure("720p noise, bottom-right match", noise, target, true);
    measure("720p solid, absent", Solid(1280, 720), target, false);
    measure("similar backgrounds, tied matches", Solid(256, 128, {101, 101, 101}),
        Target{"gray", Solid(16, 16, {100, 100, 100})}, true);
    Target sparse{"sparse", Solid(256, 256), {0.98, std::nullopt, 24}};
    std::fill_n(sparse.image.pixels.end() - 32, 32, Pixel{255, 255, 255});
    measure("1080p solid, sparse mismatch near the bottom", Solid(1920, 1080), sparse, false, true);
    Target nearUniform{"near uniform", Solid(256, 256, {100, 100, 100}), {0.98, std::nullopt, 24}};
    nearUniform.image.pixels[0] = {80, 120, 100};
    nearUniform.image.pixels[1] = {124, 76, 100};
    nearUniform.image.pixels.back() = {100, 100, 125};
    measure("1080p solid, near-uniform target absent", Solid(1920, 1080, {100, 100, 100}), nearUniform, false, true);
}

TEST(CaptureTests, RejectsInvalidScreenRegionsAndWindowHandles)
{
    for (Rect area : {Rect{0, 0, 0, 1}, Rect{0, 0, 1, -1},
        Rect{std::numeric_limits<int>::max(), 0, 2, 1},
        Rect{std::numeric_limits<int>::min(), 0, 1, 1}})
        EXPECT_THROW(CaptureScreen(area), std::invalid_argument);
    EXPECT_THROW(CaptureWindow(nullptr), std::invalid_argument);
    EXPECT_THROW(CaptureWindow(reinterpret_cast<HWND>(std::uintptr_t(1))), std::invalid_argument);
}

TEST(CaptureTests, ReleasesEveryAcquiredResourceOnNativeFailure)
{
    using Failure = CaptureFailure;
    const std::vector<std::pair<Failure, std::string>> failures{{Failure::Screen, ""}, {Failure::Memory, "D"},
        {Failure::Bitmap, "MD"}, {Failure::Pixels, "BMD"}, {Failure::SelectNull, "BMD"},
        {Failure::SelectError, "BMD"}, {Failure::Blit, "SBMD"}, {Failure::Flush, "SBMD"}};
    for (const auto& failure : failures)
    {
        SCOPED_TRACE(static_cast<int>(failure.first));
        captureCalls = {};
        captureCalls.failure = failure.first;
        EXPECT_THROW(InjectedCaptureScreen(Rect{-200, -100, 3, 2}), std::runtime_error);
        EXPECT_EQ(captureCalls.cleanup, failure.second);
        EXPECT_FALSE(captureCalls.selected);
    }
}

TEST(CaptureTests, PreservesNativeErrorCode)
{
    captureCalls = {};
    captureCalls.failure = CaptureFailure::Blit;
    try
    {
        InjectedCaptureScreen(Rect{-200, -100, 3, 2});
        FAIL() << "expected a native capture error";
    }
    catch (const std::system_error& error) { EXPECT_EQ(error.code().value(), int(ERROR_ACCESS_DENIED)); }
    EXPECT_EQ(captureCalls.cleanup, "SBMD");
}

TEST(CaptureTests, ProducesTopDownBgraPixelsAndPreservesNegativeOrigin)
{
    captureCalls = {};
    const Image frame = InjectedCaptureScreen(Rect{-200, -100, 3, 2});
    ASSERT_TRUE(frame.Valid());
    EXPECT_EQ(frame.x, -200);
    EXPECT_EQ(frame.y, -100);
    EXPECT_EQ(captureCalls.info.bmiHeader.biWidth, 3);
    EXPECT_EQ(captureCalls.info.bmiHeader.biHeight, -2);
    EXPECT_EQ(captureCalls.info.bmiHeader.biBitCount, 32);
    EXPECT_EQ(captureCalls.info.bmiHeader.biCompression, DWORD(BI_RGB));
    ExpectBounds(captureCalls.region, -200, -100, 3, 2);
    for (int row = 0; row < 2; ++row) for (int column = 0; column < 3; ++column)
    {
        const Pixel pixel = frame.pixels[row * 3 + column];
        EXPECT_EQ(pixel.blue, column);
        EXPECT_EQ(pixel.green, row);
        EXPECT_EQ(pixel.red, 123);
        EXPECT_EQ(pixel.alpha, 255);
    }
    EXPECT_EQ(captureCalls.cleanup, "SBMD");
}

TEST(CaptureTests, DefaultsToEntireDesktopAndAcceptsBottomRightRegion)
{
    captureCalls = {};
    EXPECT_TRUE(InjectedCaptureScreen(std::nullopt).Valid());
    ExpectBounds(captureCalls.region, -200, -100, 640, 480);
    captureCalls = {};
    EXPECT_TRUE(InjectedCaptureScreen(Rect{439, 379, 1, 1}).Valid());
    ExpectBounds(captureCalls.region, 439, 379, 1, 1);
    for (Rect area : {Rect{440, 379, 1, 1}, Rect{439, 380, 1, 1}, Rect{-201, -100, 1, 1}, Rect{-200, -101, 1, 1}})
        EXPECT_THROW(InjectedCaptureScreen(area), std::invalid_argument);
}

TEST(CaptureTests, RejectsEveryWindowQueryFailureAndEmptyClientArea)
{
    for (auto failure : {CaptureFailure::Window, CaptureFailure::Hidden, CaptureFailure::Minimized,
        CaptureFailure::Client, CaptureFailure::Origin})
    {
        SCOPED_TRACE(static_cast<int>(failure));
        captureCalls = {};
        captureCalls.failure = failure;
        EXPECT_THROW(InjectedCaptureWindow(reinterpret_cast<HWND>(std::uintptr_t(1))), std::invalid_argument);
        EXPECT_TRUE(captureCalls.cleanup.empty());
    }
    captureCalls = {};
    EXPECT_TRUE(InjectedCaptureWindow(reinterpret_cast<HWND>(std::uintptr_t(1))).Valid());
    ExpectBounds(captureCalls.region, -200, -100, 3, 2);
    captureCalls = {};
    captureCalls.client.width = 0;
    EXPECT_THROW(InjectedCaptureWindow(reinterpret_cast<HWND>(std::uintptr_t(1))), std::invalid_argument);
}

TEST(CaptureTests, CapturesScreenRegionAndReleasesGdiResources)
{
    const Rect area{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 3, 2};
    // Warm up GDI before measuring handle ownership.
    ASSERT_TRUE(CaptureScreen(area).Valid());
    const DWORD before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    for (int i = 0; i < 10; ++i)
    {
        Image frame = CaptureScreen(area);
        EXPECT_TRUE(frame.Valid());
        EXPECT_EQ(frame.pixels.size(), 6u);
        EXPECT_EQ(frame.width, 3);
        EXPECT_EQ(frame.height, 2);
        EXPECT_EQ(frame.x, area.x);
        EXPECT_EQ(frame.y, area.y);
    }
    EXPECT_EQ(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS), before);
}

TEST(CaptureTests, CapturesVisibleWindowClientAndRejectsHiddenWindow)
{
    struct DpiContext
    {
        DPI_AWARENESS_CONTEXT previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        ~DpiContext() { if (previous) SetThreadDpiAwarenessContext(previous); }
    } dpi;
    struct Window
    {
        HWND handle = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"",
            WS_POPUP, 32, 32, 64, 48, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ~Window() { if (handle) DestroyWindow(handle); }
    } window;
    ASSERT_NE(window.handle, nullptr);
    EXPECT_THROW(CaptureWindow(window.handle), std::invalid_argument);
    ShowWindow(window.handle, SW_SHOWNOACTIVATE);
    UpdateWindow(window.handle);
    Target target{"rendered", {2, 2, {{0, 0, 255}, {0, 255, 0}, {255, 0, 0}, {255, 255, 255}}}, {1}};
    HDC context = GetDC(window.handle);
    ASSERT_NE(context, nullptr);
    RECT client{0, 0, 64, 48};
    bool painted = FillRect(context, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH))) != 0;
    for (int row = 0; row < 2; ++row) for (int column = 0; column < 2; ++column)
    {
        const Pixel pixel = target.image.pixels[row * 2 + column];
        if (!SetPixelV(context, column + 20, row + 16, RGB(pixel.red, pixel.green, pixel.blue))) painted = false;
    }
    GdiFlush();
    ReleaseDC(window.handle, context);
    ASSERT_TRUE(painted);
    DwmFlush();
    Image frame = CaptureWindow(window.handle);
    EXPECT_TRUE(frame.Valid());
    EXPECT_EQ(frame.width, 64);
    EXPECT_EQ(frame.height, 48);
    POINT origin{};
    ASSERT_TRUE(ClientToScreen(window.handle, &origin));
    EXPECT_EQ(frame.x, origin.x);
    EXPECT_EQ(frame.y, origin.y);
    Recognizer recognizer;
    recognizer.SetTargets({target});
    int reports = 0;
    recognizer.SetOnDetection([&](const Detection& detection) {
        ++reports;
        ExpectBounds(detection.bounds, origin.x + 20, origin.y + 16, 2, 2);
        EXPECT_EQ(detection.targetId, "rendered");
        EXPECT_NEAR(detection.confidence, 1, 1e-12);
    });
    EXPECT_EQ(recognizer.Scan(frame).size(), 1u);
    EXPECT_EQ(reports, 1);
    ShowWindow(window.handle, SW_MINIMIZE);
    EXPECT_THROW(CaptureWindow(window.handle), std::invalid_argument);
}
}
