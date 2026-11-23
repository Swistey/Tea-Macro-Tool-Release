#ifndef TEA_2D_CAPTURE_HPP
#define TEA_2D_CAPTURE_HPP

#include <windows.h>
#include "types.hpp"

namespace tea::vision
{
Image CaptureScreen(std::optional<Rect> region = std::nullopt);
Image CaptureWindow(HWND window); // Captures the visible client area on screen.
}

#endif
