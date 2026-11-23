#ifndef TEA_CAPTURE_TOOL_HPP
#define TEA_CAPTURE_TOOL_HPP

#include "capture.hpp"
#include <functional>
#include <optional>

namespace tea::vision
{
class ScreenCaptureTool
{
public:
    using Copy = std::function<void(const Image&)>;
    using Capture = std::function<void(Image)>;
    explicit ScreenCaptureTool(Copy copy = {});
    ~ScreenCaptureTool();
    ScreenCaptureTool(const ScreenCaptureTool&) = delete;
    ScreenCaptureTool& operator=(const ScreenCaptureTool&) = delete;
    void Open(HWND owner, UINT stopKey, Capture capture = {}, std::function<void()> closed = {});
    void StartSelection();
    void Finish();
    bool Active() const { return toolbar != nullptr; }
    HWND Toolbar() const { return toolbar; }
    HWND SelectionWindow() const { return selectionWindow; }

private:
    static LRESULT CALLBACK ToolbarProcedure(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK SelectionProcedure(HWND, UINT, WPARAM, LPARAM);
    void DismissSelection();
    void CaptureSelection(Rect area);
    HWND toolbar = nullptr, status = nullptr, selectionWindow = nullptr;
    HFONT font = nullptr;
    std::optional<POINT> dragStart;
    RECT selection{};
    unsigned count = 0;
    Copy copy;
    Capture capture;
    std::function<void()> closed;
};
}
#endif
