#include "matcher.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <numeric>
#include <stdexcept>
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define TEA_SSE2
#endif

namespace tea::vision
{
namespace
{
constexpr int PollInterval = 32;
constexpr int MinimumTemplateOpacity = 64;

int Brightness(Pixel pixel) { return pixel.red + pixel.green + pixel.blue; }

int MinimumBlendedChannel(int reference, int alpha)
{
    return (reference * alpha + 127) / 255;
}

struct ColorExtreme { const Pixel* pixel = nullptr; int bound = 0; };
using ColorExtremes = std::array<ColorExtreme, 6>;

void TrackColorExtremes(ColorExtremes& extremes, const Pixel* pixel)
{
    constexpr std::array channels{&Pixel::blue, &Pixel::green, &Pixel::red};
    const int transparency = 255 - pixel->alpha;
    for (std::size_t i = 0; i < channels.size(); ++i)
    {
        const int color = pixel->*channels[i];
        const int darkest = transparency ? MinimumBlendedChannel(color, pixel->alpha) : color;
        const int lightest = darkest + transparency;
        auto& lowestCeiling = extremes[2 * i];
        auto& highestFloor = extremes[2 * i + 1];
        if (!lowestCeiling.pixel || lightest < lowestCeiling.bound) lowestCeiling = {pixel, lightest};
        if (!highestFloor.pixel || darkest > highestFloor.bound) highestFloor = {pixel, darkest};
    }
}

int BlendedChannelError(int actual, int reference, int alpha)
{
    // an unknown background can contribute any value from black to white
    const int darkest = MinimumBlendedChannel(reference, alpha);
    const int lightest = darkest + 255 - alpha;
    return std::max({0, darkest - actual, actual - lightest});
}

int MaxChannelDifference(Pixel a, Pixel b)
{
    if (b.alpha != 255)
        return std::max({BlendedChannelError(a.red, b.red, b.alpha), BlendedChannelError(a.green, b.green, b.alpha),
            BlendedChannelError(a.blue, b.blue, b.alpha)});
    return std::max({std::abs(int(a.red) - b.red), std::abs(int(a.green) - b.green),
        std::abs(int(a.blue) - b.blue)});
}

double OpaqueRowError(const Pixel* source, const Pixel* reference, int width, double limit, int maximumChannelError)
{
    double error = 0;
    int column = 0;
#ifdef TEA_SSE2
    const __m128i rgb = _mm_set1_epi32(0x00ffffff);
    const __m128i maximum = _mm_set1_epi8(static_cast<char>(maximumChannelError));
    const __m128i zero = _mm_setzero_si128();
    for (; column <= width - 4; column += 4)
    {
        const auto left = _mm_and_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(source + column)), rgb);
        const auto right = _mm_and_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(reference + column)), rgb);
        if (maximumChannelError < 255)
        {
            const auto delta = _mm_sub_epi8(_mm_max_epu8(left, right), _mm_min_epu8(left, right));
            const auto excess = _mm_subs_epu8(delta, maximum);
            if (_mm_movemask_epi8(_mm_cmpeq_epi8(excess, zero)) != 0xffff)
                return std::numeric_limits<double>::infinity();
        }
        const auto difference = _mm_sad_epu8(left, right);
        error += _mm_cvtsi128_si32(difference) + _mm_cvtsi128_si32(_mm_srli_si128(difference, 8));
        if (error > limit) return error;
    }
#endif
    for (; column < width; ++column)
    {
        const int red = std::abs(int(source[column].red) - reference[column].red);
        const int green = std::abs(int(source[column].green) - reference[column].green);
        const int blue = std::abs(int(source[column].blue) - reference[column].blue);
        if (red > maximumChannelError || green > maximumChannelError || blue > maximumChannelError)
            return std::numeric_limits<double>::infinity();
        error += red + green + blue;
        if (error > limit) break;
    }
    return error;
}

double BlendedRowError(const Pixel* source, const Pixel* reference, int width, double limit, int maximumChannelError)
{
    double error = 0;
    for (int column = 0; column < width; ++column)
    {
        const Pixel pixel = reference[column];
        if (!pixel.alpha) continue;
        const int blue = BlendedChannelError(source[column].blue, pixel.blue, pixel.alpha);
        const int green = BlendedChannelError(source[column].green, pixel.green, pixel.alpha);
        const int red = BlendedChannelError(source[column].red, pixel.red, pixel.alpha);
        if (red > maximumChannelError || green > maximumChannelError || blue > maximumChannelError)
            return std::numeric_limits<double>::infinity();
        error += (blue + green + red) * pixel.alpha;
        if (error > limit) break;
    }
    return error;
}
}

void ValidateTemplate(const Image& image)
{
    if (!image.Valid()) throw std::invalid_argument("invalid image");
    if (std::none_of(image.pixels.begin(), image.pixels.end(), [](Pixel pixel) { return pixel.alpha >= MinimumTemplateOpacity; }))
        throw std::invalid_argument("the image is too transparent for matching; choose one with pixels at least 25% opaque");
}

void ValidateTarget(const Target& target)
{
    const auto& options = target.options;
    if (target.id.empty()) throw std::invalid_argument("a target needs an id");
    if (!std::isfinite(options.threshold) || options.threshold < 0 || options.threshold > 1)
        throw std::invalid_argument("match threshold must be between 0 and 1");
    if (options.maximumChannelError < 0 || options.maximumChannelError > 255)
        throw std::invalid_argument("maximum channel error must be between 0 and 255");
    if (options.region && (options.region->x < 0 || options.region->y < 0 ||
        options.region->width <= 0 || options.region->height <= 0))
        throw std::invalid_argument("search region must have a nonnegative origin and positive size");
    ValidateTemplate(target.image);
}

std::optional<Detection> Match(const Image& frame, const Target& target,
    const std::function<bool()>& keepSearching, bool firstMatch)
{
    ValidateTarget(target);
    if (!frame.Valid()) throw std::invalid_argument("invalid search image");
    const Rect area = target.options.region.value_or(Rect{0, 0, frame.width, frame.height});
    if (area.x > frame.width || area.y > frame.height ||
        area.width > frame.width - area.x || area.height > frame.height - area.y)
        throw std::invalid_argument("search region must be inside the frame");
    const Image& pattern = target.image;
    if (pattern.width > area.width || pattern.height > area.height) return std::nullopt;

    long long targetSum = 0;
    std::uint64_t opacity = 0;
    bool opaque = true;
    std::vector<bool> opaqueRows(pattern.height, true);
    std::vector<long long> columns(area.width);
    const int maximumChannelError = target.options.maximumChannelError;
    ColorExtremes extremes{};
    for (int row = 0; row < pattern.height; ++row)
    {
        if (keepSearching && row % PollInterval == 0 && !keepSearching()) return std::nullopt;
        const auto* reference = pattern.pixels.data() + static_cast<std::size_t>(row) * pattern.width;
        bool rowOpaque = true;
        for (int column = 0; column < pattern.width; ++column)
        {
            const auto* pixel = reference + column;
            opacity += pixel->alpha;
            rowOpaque = rowOpaque && pixel->alpha == 255;
            targetSum += Brightness(*pixel);
            if (maximumChannelError < 255 && pixel->alpha > maximumChannelError) TrackColorExtremes(extremes, pixel);
        }
        opaqueRows[row] = rowOpaque;
        opaque = opaque && rowOpaque;
        if (opaque)
        {
            const auto* source = frame.pixels.data() + static_cast<std::size_t>(area.y + row) * frame.width + area.x;
            for (int column = 0; column < area.width; ++column)
                columns[column] += Brightness(source[column]);
        }
    }
    // probe the tightest color bounds to reject sparse mismatches early
    struct Probe { const Pixel* pixel; std::size_t frameOffset; };
    std::array<Probe, 6> probes{};
    auto probesEnd = probes.begin();
    for (const auto& extreme : extremes)
    {
        const auto* pixel = extreme.pixel;
        if (!pixel || std::any_of(probes.begin(), probesEnd, [pixel](const Probe& probe) { return probe.pixel == pixel; }))
            continue;
        const auto offset = static_cast<std::size_t>(pixel - pattern.pixels.data());
        *probesEnd++ = {pixel, (offset / pattern.width) * frame.width + offset % pattern.width};
    }

    std::optional<Detection> best;
    double minimum = target.options.threshold;
    const double normalizer = 765.0 * (opaque ? pattern.pixels.size() : opacity);
    double limit = (1 - minimum) * normalizer + normalizer * std::numeric_limits<double>::epsilon();
    double bestError = std::numeric_limits<double>::infinity();
    const double opaqueScale = opaque ? 1.0 : 255.0;

    for (int y = area.y; y <= area.y + area.height - pattern.height; ++y)
    {
        if (keepSearching && !keepSearching()) return std::nullopt;
        if (opaque && y > area.y)
        {
            const Pixel* outgoing = frame.pixels.data() + static_cast<std::size_t>(y - 1) * frame.width + area.x;
            const Pixel* incoming = outgoing + static_cast<std::size_t>(pattern.height) * frame.width;
            for (int column = 0; column < area.width; ++column)
                columns[column] += Brightness(incoming[column]) - Brightness(outgoing[column]);
        }
        auto windowSum = opaque ? std::accumulate(columns.begin(), columns.begin() + pattern.width, 0LL) : 0LL;
        const auto* sourceRow = frame.pixels.data() + static_cast<std::size_t>(y) * frame.width;
        for (int x = area.x; x <= area.x + area.width - pattern.width; ++x)
        {
            if (keepSearching && (x - area.x) % PollInterval == 0 && !keepSearching()) return std::nullopt;
            if (opaque)
            {
                if (x > area.x) windowSum += columns[x - area.x + pattern.width - 1] - columns[x - area.x - 1];
                const double lowerBound = static_cast<double>(std::abs(windowSum - targetSum));
                if (lowerBound > limit || lowerBound >= bestError) continue;
            }
            const auto* candidate = sourceRow + x;
            if (std::any_of(probes.begin(), probesEnd, [candidate, maximumChannelError](const Probe& probe) {
                return MaxChannelDifference(candidate[probe.frameOffset], *probe.pixel) > maximumChannelError;
            })) continue;
            double error = 0;
            for (int row = 0; row < pattern.height && error <= limit; ++row)
            {
                if (keepSearching && row && row % PollInterval == 0 && !keepSearching()) return std::nullopt;
                const Pixel* source = candidate + static_cast<std::size_t>(row) * frame.width;
                const Pixel* reference = pattern.pixels.data() + static_cast<std::size_t>(row) * pattern.width;
                if (opaqueRows[row])
                    error += OpaqueRowError(source, reference, pattern.width, (limit - error) / opaqueScale,
                        maximumChannelError) * opaqueScale;
                else error += BlendedRowError(source, reference, pattern.width, limit - error, maximumChannelError);
            }
            if (error > limit) continue;
            const double score = 1 - error / normalizer;
            if (score < minimum || (best && score <= best->confidence)) continue;
            best = Detection{target.id, {frame.x + x, frame.y + y, pattern.width, pattern.height},
                score, std::chrono::steady_clock::now()};
            minimum = score;
            bestError = limit = error;
            if (firstMatch || score == 1) return best;
        }
    }
    return best;
}
}
