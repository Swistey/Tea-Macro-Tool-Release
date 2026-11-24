#include "capture.hpp"
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace tea::vision
{
namespace
{
struct CaptureResources
{
    HDC screen = GetDC(nullptr);
    HDC memory = screen ? CreateCompatibleDC(screen) : nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;

    ~CaptureResources()
    {
        if (previous && previous != HGDI_ERROR) SelectObject(memory, previous);
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        if (screen) ReleaseDC(nullptr, screen);
    }
};
}

Image CaptureScreen(std::optional<Rect> region)
{
    const Rect desktop{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    const Rect area = region.value_or(desktop);
    if (area.width <= 0 || area.height <= 0 || area.x < desktop.x || area.y < desktop.y ||
        static_cast<long long>(area.x) + area.width > static_cast<long long>(desktop.x) + desktop.width ||
        static_cast<long long>(area.y) + area.height > static_cast<long long>(desktop.y) + desktop.height)
        throw std::invalid_argument("capture region must be inside the virtual desktop");

    Image image{area.width, area.height, {}, area.x, area.y};
    image.pixels.resize(static_cast<std::size_t>(area.width) * area.height);
    CaptureResources resources;
    if (!resources.screen || !resources.memory)
        throw std::runtime_error("could not create screen capture context");

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = area.width;
    info.bmiHeader.biHeight = -area.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    resources.bitmap = CreateDIBSection(resources.screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!resources.bitmap || !pixels)
        throw std::runtime_error("could not allocate screen capture bitmap");
    resources.previous = SelectObject(resources.memory, resources.bitmap);
    if (!resources.previous || resources.previous == HGDI_ERROR)
        throw std::runtime_error("could not select screen capture bitmap");
    if (!BitBlt(resources.memory, 0, 0, area.width, area.height,
        resources.screen, area.x, area.y, SRCCOPY | CAPTUREBLT))
        throw std::system_error(GetLastError(), std::system_category(), "screen capture failed");
    if (!GdiFlush()) throw std::runtime_error("could not flush screen capture");

    static_assert(sizeof(Pixel) == 4);
    std::memcpy(image.pixels.data(), pixels, image.pixels.size() * sizeof(Pixel));
    for (auto& pixel : image.pixels) pixel.alpha = 255;
    return image;
}

Image CaptureWindow(HWND window)
{
    RECT client{};
    POINT origin{};
    if (!window || !IsWindow(window) || !IsWindowVisible(window) || IsIconic(window) ||
        !GetClientRect(window, &client) || !ClientToScreen(window, &origin))
        throw std::invalid_argument("capture requires a visible, non-minimized window");
    return CaptureScreen(Rect{origin.x, origin.y, client.right, client.bottom});
}
}
