#ifndef TEA_2D_TYPES_HPP
#define TEA_2D_TYPES_HPP

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace tea::vision
{
struct Rect
{
    int x = 0, y = 0, width = 0, height = 0;
};

struct Pixel
{
    std::uint8_t blue = 0, green = 0, red = 0, alpha = 255;
};

struct Image
{
    int width = 0, height = 0;
    std::vector<Pixel> pixels; // Top-down, row-major straight BGRA.
    int x = 0, y = 0; // Origin in screen coordinates, or zero for standalone images.

    bool Valid() const
    {
        return width > 0 && height > 0 && pixels.size() / width == static_cast<std::size_t>(height) &&
            pixels.size() % width == 0 && x <= (std::numeric_limits<int>::max)() - width &&
            y <= (std::numeric_limits<int>::max)() - height;
    }
};

struct SearchOptions
{
    double threshold = 0.98; // 1 - opacity-weighted mean RGB error / 255, in [0, 1].
    std::optional<Rect> region; // Relative to the frame, not its screen origin.
    int maximumChannelError = 255; // Maximum error in any visible RGB channel.
};

struct Target
{
    std::string id;
    Image image;
    SearchOptions options;
};

struct Detection
{
    std::string targetId;
    Rect bounds; // Includes the frame's origin.
    double confidence = 0;
    std::chrono::steady_clock::time_point timestamp;
};
}

#endif
