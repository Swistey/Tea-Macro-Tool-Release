#include "capture_tool.hpp"
#include <dwmapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace tea::vision
{
namespace
{
constexpr COLORREF Green = RGB(74,222,128), Red = RGB(255,100,100);

void RegisterCaptureWindow(const wchar_t* name, WNDPROC procedure, LPCWSTR cursor)
{
    WNDCLASSW type{};
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpfnWndProc = procedure;
    type.lpszClassName = name;
    type.hCursor = LoadCursorW(nullptr,cursor);
    type.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw std::runtime_error("capture overlay could not be registered");
}

ScreenCaptureTool* WindowTool(HWND window, UINT message, LPARAM lparam)
{
    if (message == WM_NCCREATE)
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
    return reinterpret_cast<ScreenCaptureTool*>(GetWindowLongPtrW(window,GWLP_USERDATA));
}

POINT SelectionPoint(HWND window, LPARAM lparam)
{
    RECT client{};
    GetClientRect(window,&client);
    return {std::clamp<LONG>(GET_X_LPARAM(lparam),0,client.right),
        std::clamp<LONG>(GET_Y_LPARAM(lparam),0,client.bottom)};
}

std::wstring KeyName(UINT key)
{
    if (!key) return L"not set";
    wchar_t name[64]{};
    const auto scan = MapVirtualKeyW(key & 255, MAPVK_VK_TO_VSC);
    const auto extended = key & 256 || key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN ||
        key == VK_INSERT || key == VK_DELETE || key == VK_HOME || key == VK_END || key == VK_PRIOR || key == VK_NEXT || key == VK_DIVIDE;
    if (!GetKeyNameTextW((scan << 16) | (extended ? 1 << 24 : 0),name,64)) return L"key " + std::to_wstring(key);
    std::wstring result = name;
    std::transform(result.begin(),result.end(),result.begin(),[](wchar_t c) { return std::towlower(c); });
    return result;
}

void CopyImageToClipboard(const Image& image, HWND owner)
{
    const auto size = image.pixels.size()*sizeof(Pixel);
    auto release = [](void* value) { if (value) GlobalFree(value); };
    std::unique_ptr<void,decltype(release)> memory(GlobalAlloc(GMEM_MOVEABLE,sizeof(BITMAPINFOHEADER)+size),release);
    if (!memory) throw std::runtime_error("clipboard image allocation failed");
    auto* bytes = static_cast<unsigned char*>(GlobalLock(memory.get()));
    if (!bytes) throw std::runtime_error("clipboard image could not be prepared");
    const BITMAPINFOHEADER header{sizeof(BITMAPINFOHEADER),image.width,-image.height,1,32,BI_RGB,0,0,0,0,0};
    std::memcpy(bytes,&header,sizeof(header)); std::memcpy(bytes+sizeof(header),image.pixels.data(),size);
    GlobalUnlock(memory.get());
    if (!OpenClipboard(owner)) throw std::runtime_error("clipboard is busy; try capturing again");
    struct Close { ~Close() { CloseClipboard(); } } close;
    if (!EmptyClipboard() || !SetClipboardData(CF_DIB,memory.get())) throw std::runtime_error("screenshot could not be copied to the clipboard");
    memory.release();
}
}

ScreenCaptureTool::ScreenCaptureTool(Copy copy)
    : copy(copy ? std::move(copy) : Copy([this](const Image& image) { CopyImageToClipboard(image,toolbar); })) {}
ScreenCaptureTool::~ScreenCaptureTool() { Finish(); }

void ScreenCaptureTool::Open(HWND owner, UINT stopKey, Capture capture, std::function<void()> closed)
{
    if (Active()) return;
    RegisterCaptureWindow(L"TeaCaptureToolbar",ToolbarProcedure,IDC_ARROW);
    RegisterCaptureWindow(L"TeaCaptureSelection",SelectionProcedure,IDC_CROSS);
    const auto instance = GetModuleHandleW(nullptr);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(owner ? owner : GetForegroundWindow(),MONITOR_DEFAULTTOPRIMARY),&monitor);
    const auto area = monitor.rcWork;
    constexpr DWORD extended = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    int panelWidth = std::min(700L,area.right-area.left-16);
    toolbar = CreateWindowExW(extended,L"TeaCaptureToolbar",L"screen capture tool",WS_POPUP,
        area.left+(area.right-area.left-panelWidth)/2,area.top+4,panelWidth,116,nullptr,nullptr,instance,this);
    if (!toolbar) throw std::runtime_error("capture toolbar could not be opened");
    this->capture = std::move(capture);
    this->closed = std::move(closed);
    count = 0;
    const auto px = [dpi=GetDpiForWindow(toolbar)](int value) { return MulDiv(value,dpi ? dpi : 96,96); };
    panelWidth = std::min(px(700),int(area.right-area.left)-16);
    SetWindowPos(toolbar,HWND_TOPMOST,area.left+(area.right-area.left-panelWidth)/2,area.top+4,panelWidth,px(116),SWP_NOACTIVATE);
    font = CreateFontW(-px(15),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Consolas");
    auto button = [&](const wchar_t* text,int id,int x,int y,int w) {
        auto control = CreateWindowW(L"BUTTON",text,WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,x,y,w,px(32),toolbar,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);
        SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
    };
    button(L"finish",100,(panelWidth-px(120))/2,px(3),px(120));
    button((L"stop hotkey: "+KeyName(stopKey)).c_str(),101,px(8),px(42),(panelWidth-px(24))/2);
    button(L"capture image (f11)",102,(panelWidth+px(8))/2,px(42),(panelWidth-px(24))/2);
    status = CreateWindowW(L"STATIC",L"drag to select an area, then release to capture.",WS_CHILD|WS_VISIBLE|SS_CENTER|SS_ENDELLIPSIS,
        px(8),px(85),panelWidth-px(16),px(24),toolbar,reinterpret_cast<HMENU>(103),instance,nullptr);
    SendMessageW(status,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
    ShowWindow(toolbar,SW_SHOWNOACTIVATE);
    SetTimer(toolbar,1,500,nullptr);
    UpdateWindow(toolbar);
}

void ScreenCaptureTool::StartSelection()
{
    if (!Active() || selectionWindow) return;
    selectionWindow = CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_LAYERED,
        L"TeaCaptureSelection",L"select a screen area",WS_POPUP,
        GetSystemMetrics(SM_XVIRTUALSCREEN),GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_CXVIRTUALSCREEN),GetSystemMetrics(SM_CYVIRTUALSCREEN),
        nullptr,nullptr,GetModuleHandleW(nullptr),this);
    if (!selectionWindow || !SetLayeredWindowAttributes(selectionWindow,0,96,LWA_ALPHA))
    {
        DismissSelection();
        SetWindowTextW(status,L"could not start screen selection. try again.");
        return;
    }
    ShowWindow(toolbar,SW_HIDE);
    ShowWindow(selectionWindow,SW_SHOWNOACTIVATE);
}

void ScreenCaptureTool::DismissSelection()
{
    dragStart.reset();
    selection = {};
    if (const HWND window = std::exchange(selectionWindow,nullptr))
    {
        if (GetCapture() == window) ReleaseCapture();
        DestroyWindow(window);
    }
}

void ScreenCaptureTool::CaptureSelection(Rect area)
{
    DismissSelection();
    try
    {
        if (area.width > 8192 || area.height > 8192 || std::uint64_t(area.width)*area.height > 16000000)
            throw std::runtime_error("select a smaller area (up to 8192 pixels per side and 16 megapixels)");
        DwmFlush();
        auto image = CaptureScreen(area);
        copy(image);
        if (capture) capture(std::move(image));
        ++count;
        const auto summary = std::to_wstring(count) + (count == 1 ? L" screenshot captured" : L" screenshots captured");
        SetWindowTextW(status,summary.c_str());
    }
    catch (const std::exception& error)
    {
        const auto size = MultiByteToWideChar(CP_UTF8,0,error.what(),-1,nullptr,0);
        std::wstring text(size,L'\0');
        if (size) MultiByteToWideChar(CP_UTF8,0,error.what(),-1,text.data(),size);
        SetWindowTextW(status,text.c_str());
    }
    if (Active()) ShowWindow(toolbar,SW_SHOWNOACTIVATE);
}

void ScreenCaptureTool::Finish()
{
    DismissSelection();
    const HWND window = std::exchange(toolbar,nullptr);
    if (window) DestroyWindow(window);
    status = nullptr;
    if (font) DeleteObject(std::exchange(font,nullptr));
    capture = {};
    auto callback = std::move(closed);
    if (window && callback) callback();
}

LRESULT CALLBACK ScreenCaptureTool::ToolbarProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto* tool = WindowTool(window,message,lparam);
    if (!tool) return DefWindowProcW(window,message,wparam,lparam);
    switch (message)
    {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_TIMER:
        if (IsWindowVisible(window))
            SetWindowPos(window,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        return 0;
    case WM_CTLCOLORSTATIC:
        SetTextColor(reinterpret_cast<HDC>(wparam),RGB(255,255,255)); SetBkColor(reinterpret_cast<HDC>(wparam),RGB(0,0,0));
        return reinterpret_cast<LRESULT>(GetStockObject(BLACK_BRUSH));
    case WM_DRAWITEM:
        {
            const auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
            FillRect(item.hDC,&item.rcItem,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            const auto color = item.CtlID == 101 ? Red : Green;
            const auto brush = CreateSolidBrush(color); FrameRect(item.hDC,&item.rcItem,brush); DeleteObject(brush);
            wchar_t text[128]{}; GetWindowTextW(item.hwndItem,text,128);
            SetTextColor(item.hDC,color); SetBkMode(item.hDC,TRANSPARENT);
            auto rectangle = item.rcItem; DrawTextW(item.hDC,text,-1,&rectangle,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
            return TRUE;
        }
    case WM_COMMAND:
        if (LOWORD(wparam) == 102) tool->StartSelection();
        else if (LOWORD(wparam) == 100 || LOWORD(wparam) == 101) tool->Finish();
        return 0;
    case WM_CLOSE: tool->Finish(); return 0;
    }
    return DefWindowProcW(window,message,wparam,lparam);
}

LRESULT CALLBACK ScreenCaptureTool::SelectionProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto* tool = WindowTool(window,message,lparam);
    if (!tool) return DefWindowProcW(window,message,wparam,lparam);
    switch (message)
    {
    case WM_LBUTTONDOWN:
        SetCapture(window);
        if (GetCapture() == window) tool->dragStart = SelectionPoint(window,lparam);
        return 0;
    case WM_MOUSEMOVE:
    case WM_LBUTTONUP:
        if (tool->dragStart)
        {
            const auto start = *tool->dragStart;
            const auto end = SelectionPoint(window,lparam);
            InvalidateRect(window,&tool->selection,FALSE);
            tool->selection = {std::min(start.x,end.x),std::min(start.y,end.y),
                std::max(start.x,end.x),std::max(start.y,end.y)};
            if (message == WM_MOUSEMOVE) InvalidateRect(window,&tool->selection,FALSE);
            else
            {
                const auto rectangle = tool->selection;
                tool->dragStart.reset();
                tool->selection = {};
                ReleaseCapture();
                if (!IsRectEmpty(&rectangle))
                {
                    POINT origin{rectangle.left,rectangle.top};
                    ClientToScreen(window,&origin);
                    tool->CaptureSelection({origin.x,origin.y,rectangle.right-rectangle.left,rectangle.bottom-rectangle.top});
                }
            }
        }
        return 0;
    case WM_CAPTURECHANGED:
        tool->dragStart.reset();
        InvalidateRect(window,&tool->selection,FALSE);
        tool->selection = {};
        return 0;
    case WM_PAINT:
        {
            PAINTSTRUCT paint{};
            const auto dc = BeginPaint(window,&paint);
            FillRect(dc,&paint.rcPaint,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            if (!IsRectEmpty(&tool->selection))
            {
                FillRect(dc,&tool->selection,static_cast<HBRUSH>(GetStockObject(DKGRAY_BRUSH)));
                const auto brush = CreateSolidBrush(Green);
                FrameRect(dc,&tool->selection,brush);
                DeleteObject(brush);
            }
            EndPaint(window,&paint);
            return 0;
        }
    case WM_CLOSE: tool->Finish(); return 0;
    }
    return DefWindowProcW(window,message,wparam,lparam);
}
}
