#include "tea_wraps.hpp"
#include "macros.hpp"
#include "2d/capture_tool.hpp"
#include "2d/image_library.hpp"
#include <dwmapi.h>
#include <uxtheme.h>
#include <shlobj.h>
#include <shellapi.h>
#include <WebView2.h>
#include <json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

void SleepThread(float f)   //Sleeps thread for 'f' seconds
{
    int seconds = f;
    int milliseconds = (f - seconds) * 1000;

    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

Coordinates::Coordinates(int x, int y) : pos_x(x), pos_y(y) {}

bool Coordinates::operator==(const Coordinates& other) const
{
    return pos_x == other.pos_x && pos_y == other.pos_y;
}

std::ostream& operator<<(std::ostream& out, const Coordinates& cords)
{
    out << "(X: " << cords.pos_x << ", Y: " << cords.pos_y << ")";
    return out;
}

using Json = nlohmann::json;
namespace fs = std::filesystem;

void tea::PreserveUnreadableFile(const fs::path& path)
{
    const auto prefix = path.wstring() + L".unreadable-" + std::to_wstring(
        std::chrono::system_clock::now().time_since_epoch().count());
    for (unsigned suffix = 0;; ++suffix)
    {
        const auto copy = prefix + L"-" + std::to_wstring(suffix);
        std::error_code error;
        if (fs::copy_file(path, copy, error)) return;
        if (error != std::errc::file_exists)
            throw std::runtime_error("the original data file could not be backed up; check its permissions and try again");
    }
}

namespace
{
constexpr UINT WEB_EVENT = WM_APP + 1;
constexpr UINT_PTR STARTUP_TIMER = 1;

fs::path DataDirectory(fs::path path)
{
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
        throw std::runtime_error("the application data folder could not be opened");
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> folder(local, CoTaskMemFree);
    return path.empty() ? fs::path(folder.get()) / L"tea" : std::move(path);
}

std::optional<RECT> MonitorWorkArea(HWND window)
{
    MONITORINFO monitor{sizeof(MONITORINFO)};
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) return std::nullopt;
    return monitor.rcWork;
}

POINT MinimumWindowSize(HWND window)
{
    UINT dpi = GetDpiForWindow(window);
    if (!dpi) dpi = 96;
    RECT bounds{0, 0, MulDiv(1280, dpi, 96), MulDiv(720, dpi, 96)};
    AdjustWindowRectExForDpi(&bounds, GetWindowLongPtrW(window, GWL_STYLE), FALSE,
        GetWindowLongPtrW(window, GWL_EXSTYLE), dpi);
    POINT size{bounds.right - bounds.left, bounds.bottom - bounds.top};
    if (const auto work = MonitorWorkArea(window))
    {
        size.x = std::min(size.x, work->right - work->left);
        size.y = std::min(size.y, work->bottom - work->top);
    }
    return size;
}

void StyleWindow(HWND window)
{
    WTA_OPTIONS caption{WTNCA_NODRAWCAPTION | WTNCA_NODRAWICON, WTNCA_NODRAWCAPTION | WTNCA_NODRAWICON};
    SetWindowThemeAttribute(window, WTA_NONCLIENT, &caption, sizeof(caption));
    const BOOL dark = TRUE;
    const COLORREF background = RGB(0, 0, 0), text = RGB(0, 0, 0);
    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DwmSetWindowAttribute(window, DWMWA_CAPTION_COLOR, &background, sizeof(background));
    DwmSetWindowAttribute(window, DWMWA_TEXT_COLOR, &text, sizeof(text));
}

std::wstring Wide(const std::string& text)
{
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!size) throw std::invalid_argument("invalid text encoding");
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
     result.data(), size);
    return result;
}

std::string Utf8(const std::wstring& text)
{
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
     static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
     result.data(), size, nullptr, nullptr);
    return result;
}

std::string Resource(int id)
{
    const HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(id),
     RT_RCDATA);
    if (!resource) throw std::runtime_error("an application resource is missing");
    return {static_cast<const char*>(LockResource(LoadResource(nullptr, resource))),
         SizeofResource(nullptr, resource)};
}

std::string Base64(const std::string& data)
{
    const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    unsigned value = 0;
    int bits = -6;
    for (unsigned char character : data)
    {
        value = (value << 8) | character;
        bits += 8;
        while (bits >= 0) { result += alphabet[(value >> bits) & 63]; bits -= 6; }
    }
    if (bits > -6) result += alphabet[((value << 8) >> (bits + 8)) & 63];
    while (result.size() % 4) result += '=';
    return result;
}

template<class Interface, class... Args>
class Callback final : public Interface
{
public:
    Callback(const wchar_t* uuid, std::function<HRESULT(Args...)> function) : function(std::move(function))
    {
        const auto text = std::wstring(L"{") + uuid + L"}";
        if (FAILED(IIDFromString(text.c_str(), &iid))) throw std::logic_error("invalid callback interface");
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID requested, void** value) override
    {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (requested != IID_IUnknown && requested != iid) return E_NOINTERFACE;
        *value = static_cast<Interface*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG count = --references; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE Invoke(Args... args) override
    { try { return function(args...); } catch (...) { return E_FAIL; } }
private:
    std::atomic<ULONG> references{1};
    IID iid{};
    std::function<HRESULT(Args...)> function;
};

int Integer(const Json& value, const char* key, int fallback, int minimum, int maximum)
{
    const auto item = value.find(key);
    if (item == value.end()) return fallback;
    if (!item->is_number_integer() || *item < minimum || *item > maximum)
        throw std::invalid_argument(std::string("invalid ") + key);
    return item->get<int>();
}

bool UsesImage(const Json& action)
{
    const auto type = action.value("type", "");
    return type == "image-on-screen" || type == "wait-for-image" ||
        (type == "move" && action.value("destination", "coordinates") == "image");
}

int ReadImageId(const Json& item, const tea::vision::ImageLibrary* images)
{
    const int id = Integer(item, "imageId", 0, 1, INT_MAX);
    if (!id || (images && !images->Has(id))) throw std::invalid_argument("choose an image from your library");
    return id;
}

MacroCondition ReadCondition(const Json& item, const tea::vision::ImageLibrary* images)
{
    if (item.at("type") == "image-on-screen") return ImageOnScreen{ReadImageId(item, images)};
    throw std::invalid_argument("unsupported macro condition");
}

std::vector<MacroAction> ReadActions(const Json& values, const tea::vision::ImageLibrary* images, std::size_t& remaining)
{
    if (!values.is_array()) throw std::invalid_argument("macro actions must be an array");
    if (values.size() > remaining)
        throw std::invalid_argument("a macro can contain up to 256 actions");
    remaining -= values.size();
    std::vector<MacroAction> actions;
    actions.reserve(values.size());
    for (const auto& item : values)
    {
        MacroAction action;
        const auto type = item.at("type").get<std::string>();
        if (type == "key") action.type = MacroAction::Type::Key;
        else if (type == "mouse") action.type = MacroAction::Type::Mouse;
        else if (type == "text") action.type = MacroAction::Type::Text;
        else if (type == "wait") action.type = MacroAction::Type::Wait;
        else if (type == "move")
        {
            action.type = MacroAction::Type::Move;
            const auto destination = item.value("destination", "coordinates");
            if (destination != "coordinates" && destination != "image")
                throw std::invalid_argument("unsupported mouse move destination");
        }
        else if (type == "wait-for-image") action.type = MacroAction::Type::WaitForImage;
        else if (type == "if-else" || type == "while")
        {
            action.type = type == "if-else" ? MacroAction::Type::IfElse : MacroAction::Type::While;
            action.condition = ReadCondition(item.at("condition"), images);
            action.actions = ReadActions(item.at("actions"), images, remaining);
            if (type == "if-else") action.elseActions = ReadActions(item.at("elseActions"), images, remaining);
            else if (item.contains("elseActions")) throw std::invalid_argument("a while block cannot have an else branch");
        }
        else throw std::invalid_argument("unsupported macro action");
        if (action.type != MacroAction::Type::IfElse && action.type != MacroAction::Type::While &&
            (item.contains("condition") || item.contains("actions") || item.contains("elseActions")))
            throw std::invalid_argument("only conditional blocks can contain branches and conditions");
        if (UsesImage(item)) action.imageId = ReadImageId(item, images);
        const auto mode = item.value("mode", std::string{"press"});
        if (mode == "down") action.mode = MacroAction::Mode::Down;
        else if (mode == "up") action.mode = MacroAction::Mode::Up;
        else if (mode != "press") throw std::invalid_argument("unsupported input mode");
        action.key = Integer(item, "key", 0, 0, 511);
        action.modifiers = Integer(item, "modifiers", 0, 0, 7);
        action.button = Integer(item, "button", 0, 0, 1);
        action.text = Wide(item.value("text", std::string{}));
        action.duration = std::chrono::milliseconds(Integer(item, "duration", 0, 0, 60000));
        const bool textEvent = action.type == MacroAction::Type::Text;
        if (item.contains(textEvent ? "holdRange" : "delayRange"))
            throw std::invalid_argument("duration range does not match the event type");
        if (const auto range = item.find(textEvent ? "delayRange" : "holdRange"); range != item.end())
        {
            if (!range->is_object()) throw std::invalid_argument("invalid duration range");
            action.durationRange = MacroAction::DurationRange{
                std::chrono::milliseconds(Integer(*range, "min", -1, 0, 60000)),
                std::chrono::milliseconds(Integer(*range, "max", -1, 0, 60000))};
        }
        action.position = {Integer(item, "x", 0, INT_MIN, INT_MAX), Integer(item, "y", 0, INT_MIN, INT_MAX)};
        action.positioned = item.value("positioned", false);
        actions.push_back(std::move(action));
    }
    return actions;
}

template<class Predicate>
bool AnyImageReference(const Json& actions, const Predicate& test)
{
    for (const auto& action : actions)
    {
        if (UsesImage(action) && test(action.at("imageId").get<int>())) return true;
        const auto& type = action.at("type");
        if (type == "if-else" || type == "while")
        {
            if (test(action.at("condition").at("imageId").get<int>())) return true;
            if (AnyImageReference(action.at("actions"), test)) return true;
            if (action.contains("elseActions") && AnyImageReference(action.at("elseActions"), test)) return true;
        }
    }
    return false;
}

bool ReferencesImage(const Json& actions, int id)
{
    return AnyImageReference(actions, [id](int reference) { return reference == id; });
}

std::shared_ptr<const Macro> ReadMacro(const Json& value, const tea::vision::ImageLibrary* images)
{
    std::size_t remaining = 256;
    auto actions = ReadActions(value.at("actions"), images, remaining);
    const auto type = value.value("type", std::string{"no-repeat"});
    if (type != "no-repeat" && type != "toggle") throw std::invalid_argument("invalid macro type");
    return std::make_shared<Macro>(value.at("name").get<std::string>(), std::move(actions),
        type == "toggle" ? Macro::Type::Toggle : Macro::Type::NoRepeat);
}

MacroRecorder::Options ReadRecordingOptions(const Json& state)
{
    const auto options = state.value("recordingOptions", Json::object());
    if (!options.is_object()) throw std::invalid_argument("invalid recording settings");
    const bool clicks = options.value("mouseButtons", true);
    const bool positioned = options.value("clickPosition", options.value("mouseMovement", false));
    if (options.contains("clickPosition") && positioned && !clicks)
        throw std::invalid_argument("enable mouse click recording before capturing click positions");
    return {options.value("delays", true), positioned && clicks, options.value("keyboard", true), clicks,
        options.value("splitKeyEvents", false)};
}

std::vector<Binding> ReadBindings(const Json& state, const tea::vision::ImageLibrary* images = nullptr)
{
    const auto layout = state.value("layout", std::string{"60%"});
    if (layout != "60%" && layout != "65%" && layout != "75%" && layout != "TKL" && layout != "100%")
        throw std::invalid_argument("choose a supported keyboard form factor");
    const UINT stopKey = Integer(state, "stopKey", 0, 0, 511);
    if (stopKey && !Trigger::ValidStopKey(stopKey)) throw std::invalid_argument("choose a supported stop hotkey");
    const UINT recordKey = Integer(state, "recordKey", 0, 0, 511);
    if (recordKey && (!Trigger::ValidStopKey(recordKey) || recordKey == stopKey))
        throw std::invalid_argument("choose a record hotkey different from the stop hotkey");
    ReadRecordingOptions(state);
    if (state.contains("bindingOptions"))
    {
        const auto& options = state.at("bindingOptions");
        if (!options.is_object() || !options.at("replace").is_boolean())
            throw std::invalid_argument("invalid binding options");
        Integer(options, "modifiers", 3, 0, 7);
    }
    if (state.at("version") != 1 || !state.at("enabled").is_boolean() ||
        !state.at("macros").is_array() || !state.at("bindings").is_array() ||
        state.at("macros").size() > 256 || state.at("bindings").size() > 256)
        throw std::invalid_argument("invalid macro configuration");
    std::map<int, std::shared_ptr<const Macro>> macros;
    for (const auto& value : state.at("macros"))
    {
        const int id = Integer(value, "id", 0, 1, INT_MAX);
        if (id <= 0 || !macros.emplace(id, ReadMacro(value, images)).second)
            throw std::invalid_argument("invalid macro id");
    }
    std::vector<Binding> result;
    std::map<UINT, bool> keys;
    for (const auto& value : state.at("bindings"))
    {
        Trigger trigger{UINT(Integer(value, "code", 0, 1, 511)), UINT(Integer(value, "modifiers", 0, 0, 7))};
        const auto macro = macros.find(Integer(value, "macroId", 0, 1, INT_MAX));
        if (trigger.key == stopKey) throw std::invalid_argument("the stop hotkey cannot also trigger a macro");
        if (trigger.key == recordKey) throw std::invalid_argument("the record hotkey cannot also trigger a macro");
        if (!Trigger::Valid(trigger) || macro == macros.end() || !keys.emplace(trigger.key, true).second ||
            value.at("key").get<std::string>().size() > 32)
            throw std::invalid_argument("choose a supported key and macro");
        result.push_back({trigger, macro->second, value.at("replace").get<bool>()});
    }
    return result;
}

void ValidateNewImageReferences(const Json& next, const Json& previous, const tea::vision::ImageLibrary& images)
{
    for (const auto& macro : next.at("macros"))
    {
        const auto& saved = previous.at("macros");
        const auto original = std::find_if(saved.begin(), saved.end(), [&](const Json& candidate) {
            return candidate.at("id") == macro.at("id");
        });
        if (original != saved.end() && *original == macro) continue;
        if (AnyImageReference(macro.at("actions"), [&](int id) {
            return !images.Has(id) && (original == saved.end() || !ReferencesImage(original->at("actions"), id));
        })) throw std::invalid_argument("choose an image from your library");
    }
}

std::optional<Json> ReadSavedConfiguration(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    try
    {
        auto value = Json::parse(file);
        if (file.bad()) return std::nullopt;
        ReadBindings(value);
        return value;
    }
    catch (const Json::exception&) { return std::nullopt; }
    catch (const std::invalid_argument&) { return std::nullopt; }
}

bool ConfigurationFileMissing(const fs::path& path)
{
    return GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND;
}

class App
{
public:
    App(fs::path data = {}, bool smoke = false, bool desktopSmoke = false, bool verifyInstall = false)
        : smoke(smoke), desktopSmoke(desktopSmoke), verifyInstall(verifyInstall), directory(DataDirectory(std::move(data))),
        images(directory / L"images"), listener(binder, WindowsMacroOutput(), [this](const std::string& error) {
        Post({{"event", "error"}, {"message", error}});
    }, [this](bool active, bool paused) {
        Post({{"event", "playback"}, {"active", active}, {"paused", paused}});
    }, [this] {
        return (recordHotkeyEnabled || recordingActive) && !imageCaptureActive &&
            Post({{"event", "record-hotkey"}});
    }, [this](int id) {
        return images.Load(id);
    }, [this] {
        if (recordingActive && !this->smoke) return true;
        return (imageCaptureActive || recordingActive) && Post({{"event", "stop-hotkey"}});
    }, [this](Trigger trigger) {
        if (trigger.key != VK_F11 || trigger.modifiers || !imageCaptureActive) return false;
        Post({{"event", "image-capture-hotkey"}});
        return true;
    }), recorder([this](const MacroAction& action) {
        Json value = {{"type", "wait"}, {"duration", action.duration.count()}};
        if (action.type != MacroAction::Type::Wait)
        {
            value["type"] = action.type == MacroAction::Type::Mouse ? "mouse" : "key";
            value["mode"] = action.mode == MacroAction::Mode::Press ? "press" : action.mode == MacroAction::Mode::Down ? "down" : "up";
            if (action.type == MacroAction::Type::Mouse) value["button"] = action.button;
            else { value["key"] = action.key; value["modifiers"] = 0; }
            if (action.positioned)
            {
                value["positioned"] = true;
                value["x"] = action.position.pos_x; value["y"] = action.position.pos_y;
            }
        }
        Post({{"event", "recorded"}, {"session", recordingSession}, {"action", std::move(value)}});
    }, [this] {
        recordingActive = false;
        UpdatePlaybackEnabled();
        Post({{"event", "recording"}, {"session", recordingSession}, {"active", false}});
    })
    {
        fs::create_directories(directory);
        const auto configuration = directory / L"macros.json";
        const auto backup = directory / L"macros.backup.json";
        const bool primaryMissing = ConfigurationFileMissing(configuration);
        if (auto saved = ReadSavedConfiguration(configuration))
        {
            state = std::move(*saved);
            hasSavedConfiguration = true;
        }
        else if (auto savedBackup = ReadSavedConfiguration(backup))
        {
            state = std::move(*savedBackup);
            configurationNotice = "Tea loaded the previous saved configuration. Review it, then save the recovered configuration to resume playback.";
        }
        else if (!primaryMissing)
        {
            configurationNotice = "Tea could not read the saved macros. Save the empty configuration to start over and keep a copy of the original file.";
        }
        else if (!ConfigurationFileMissing(backup))
        {
            archiveBackupConfiguration = true;
            configurationNotice = "Tea could not read the backup macros. Save the empty configuration to start over and keep a copy of the backup file.";
        }
        archiveConfiguration = !primaryMissing && !hasSavedConfiguration;
        state["enabled"] = true;
        for (const auto& macro : state.at("macros"))
            AnyImageReference(macro.at("actions"), [this](int id) { images.ReserveThrough(id); return false; });
        if (!state.value("stopKey", 0u))
        {
            auto next = state;
            next["stopKey"] = state.value("recordKey", 0u) == VK_ESCAPE ? VK_F12 : VK_ESCAPE;
            if (configurationNotice.empty()) Save(next);
            else state = std::move(next);
        }
        binder.Replace(ReadBindings(state), state.value("stopKey", 0u), state.value("recordKey", 0u));
    }

    ~App()
    {
        viewLifetime.reset();
        Close();
        if (controller) { controller->Close(); controller->Release(); }
        if (webview) webview->Release();
        if (loader) FreeLibrary(loader);
    }

    void Open(HWND window)
    {
        hwnd = window;
        Trace("opening");
        std::wstring executable(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (!length || length >= executable.size()) throw std::runtime_error("tea's installation folder could not be found");
        executable.resize(length);
        const auto dll = fs::path(executable).parent_path() / L"WebView2Loader.dll";
        loader = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!loader) throw std::runtime_error("the webview loader could not be loaded; extract the complete tea package");
        using Create = HRESULT(STDAPICALLTYPE*)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
        using RuntimeVersion = HRESULT(STDAPICALLTYPE*)(PCWSTR, LPWSTR*);
        const auto create = reinterpret_cast<Create>(GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions"));
        const auto runtimeVersion = reinterpret_cast<RuntimeVersion>(GetProcAddress(loader, "GetAvailableCoreWebView2BrowserVersionString"));
        if (!create || !runtimeVersion) throw std::runtime_error("the webview loader is incompatible");
        LPWSTR version = nullptr;
        const HRESULT runtime = runtimeVersion(nullptr, &version);
        needsRuntime = runtime == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || (SUCCEEDED(runtime) && (!version || !*version));
        CoTaskMemFree(version);
        if (needsRuntime) { Fail(L"tea requires the microsoft edge webview2 runtime."); return; }
        if (FAILED(runtime)) { Fail(L"the webview runtime could not be checked"); return; }
        auto* callback = new Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, HRESULT, ICoreWebView2Environment*>(
            L"4e8a3389-c9d8-4bd2-b6b5-124fee6cc14d", [this, lifetime = std::weak_ptr(viewLifetime)](HRESULT result, ICoreWebView2Environment* environment) {
                if (lifetime.expired() || closing) return S_OK;
                Trace("environment " + std::to_string(result));
                if (FAILED(result) || !environment) return Fail(L"the webview runtime could not be started");
                auto* ready = new Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, HRESULT, ICoreWebView2Controller*>(
                    L"6c4819f3-c9b7-4260-8127-c9f5bde7f68c", [this, lifetime](HRESULT status, ICoreWebView2Controller* value) {
                        if (lifetime.expired() || closing) return S_OK;
                        Trace("controller " + std::to_string(status));
                        if (FAILED(status) || !value) return Fail(L"the application window could not be initialized");
                        controller = value; controller->AddRef();
                        controller->get_CoreWebView2(&webview);
                        if (!webview) return Fail(L"the application view could not be initialized");
                        try { InitializeView(); }
                        catch (const std::exception& error) { return Fail(Wide(error.what()).c_str()); }
                        return S_OK;
                    });
                const HRESULT status = environment->CreateCoreWebView2Controller(hwnd, ready);
                ready->Release();
                if (FAILED(status)) return Fail(L"the application window could not be initialized");
                return status;
            });
        const HRESULT result = create(nullptr, (directory / L"webview").c_str(), nullptr, callback);
        callback->Release();
        if (FAILED(result)) Fail(L"the webview runtime could not be started");
    }

    void Resize()
    {
        if (controller) { RECT bounds{}; GetClientRect(hwnd, &bounds); controller->put_Bounds(bounds); }
    }

    void Close()
    {
        {
            std::lock_guard<std::mutex> lock(eventMutex);
            if (closing) return;
            closing = true;
            pendingEvents.clear();
        }
        if (hwnd) Trace("closing");
        captureTool.Finish();
        recorder.Stop();
        listener.Stop();
    }

    void DeliverPostedEvent()
    {
        Json message;
        {
            std::lock_guard<std::mutex> lock(eventMutex);
            if (pendingEvents.empty()) return;
            message = std::move(pendingEvents.front());
            pendingEvents.pop_front();
        }
        Deliver(message);
    }

    bool Passed() const { return passed; }
    bool NeedsRuntime() const { return needsRuntime; }
    const std::wstring& Failure() const { return failure; }
    Json RecoveryStatus() const { return {{"configuration", configurationNotice}, {"images", images.RecoveryNotice()}}; }
    void SaveRecoveredConfiguration() { if (!configurationNotice.empty()) Save(state); }
    void StartupTimedOut() { if (!interfaceReady) Fail(L"the interface did not finish loading"); }

private:
    void Deliver(const Json& message)
    {
        if (!webview || closing) return;
        const auto event = message.value("event", std::string{});
        if (event == "stop-hotkey") { captureTool.Finish(); recorder.Stop(GetTickCount64()); return; }
        if (event == "image-capture-hotkey")
        {
            // If F11 is also the stop key, a second press exits selection.
            if (binder.StopKey() == VK_F11 && captureTool.SelectionWindow()) captureTool.Finish();
            else captureTool.StartSelection();
            return;
        }
        if (event == "record-hotkey" && captureTool.Active()) return;
        webview->PostWebMessageAsJson(Wide(message.dump()).c_str());
    }

    void Trace(const std::string& text)
    { if (smoke || verifyInstall) std::ofstream(directory / L"trace.txt", std::ios::app) << text << '\n'; }

    void ReportSmokeFailure(const std::string& message)
    {
        Trace(message);
        std::cerr << message << '\n';
    }

    HRESULT Fail(const wchar_t* message)
    {
        if (closing || !failure.empty()) return E_FAIL;
        failure = message;
        recordHotkeyEnabled = false;
        listener.SetEnabled(false);
        if (smoke || verifyInstall)
        {
            const auto description = Utf8(message);
            std::ofstream(directory / L"error.txt") << description;
            ReportSmokeFailure(description);
        }
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return E_FAIL;
    }

    bool Post(Json message)
    {
        std::lock_guard<std::mutex> lock(eventMutex);
        if (closing || !hwnd) return false;
        pendingEvents.push_back(std::move(message));
        if (PostMessageW(hwnd, WEB_EVENT, 0, 0)) return true;
        pendingEvents.pop_back();
        return false;
    }

    void WriteConfiguration(const Json& value, const wchar_t* filename, const wchar_t* scratch, bool replace)
    {
        const auto temporary = directory / scratch;
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        const std::string text = value.dump(2);
        if (!file.write(text.data(), static_cast<std::streamsize>(text.size())))
            throw std::runtime_error("macros could not be saved");
        file.flush();
        if (!file) throw std::runtime_error("macros could not be saved");
        file.close();
        if (file.fail()) throw std::runtime_error("macros could not be saved");
        const auto destination = directory / filename;
        const DWORD flags = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0);
        for (int retries = 0; !MoveFileExW(temporary.c_str(), destination.c_str(), flags); ++retries)
        {
            const DWORD error = GetLastError();
            if (!replace && (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS))
                throw std::runtime_error("macros.json appeared while tea was open; reopen tea to load it");
            // Windows can report access denied when a reader opens the file without delete sharing.
            if (retries == 4 || (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION))
            {
                Trace("macro configuration replacement failed with Windows error " + std::to_string(error));
                throw std::runtime_error("macros could not be saved");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    void UpdatePlaybackEnabled()
    {
        listener.SetEnabled(!closing && configurationNotice.empty() && !imageCaptureActive &&
            !recordingActive && state.at("enabled").get<bool>());
    }

    void Save(const Json& next)
    {
        auto bindings = ReadBindings(next);
        ValidateNewImageReferences(next, state, images);
        const bool edit = next.at("macros") != state.at("macros") || next.at("bindings") != state.at("bindings");
        const auto stopKey = next.value("stopKey", 0u), recordKey = next.value("recordKey", 0u);
        if (!stopKey && edit) throw std::invalid_argument("choose a stop hotkey before creating or assigning macros");
        if (archiveConfiguration) tea::PreserveUnreadableFile(directory / L"macros.json");
        if (archiveBackupConfiguration)
        {
            tea::PreserveUnreadableFile(directory / L"macros.backup.json");
            archiveBackupConfiguration = false;
        }
        if (hasSavedConfiguration && next != state)
            WriteConfiguration(state, L"macros.backup.json", L"macros.backup.tmp", true);
        WriteConfiguration(next, L"macros.json", L"macros.tmp", hasSavedConfiguration || archiveConfiguration);
        if (edit || stopKey != binder.StopKey() || recordKey != binder.RecordKey())
            listener.ReplaceBindings(std::move(bindings), stopKey, recordKey);
        state = next;
        hasSavedConfiguration = true;
        archiveConfiguration = false;
        configurationNotice.clear();
        UpdatePlaybackEnabled();
    }

    void RemoveImage(int id)
    {
        for (const auto& macro : state.at("macros"))
            if (ReferencesImage(macro.at("actions"), id))
                throw std::invalid_argument("this image is used by the macro " + macro.at("name").get<std::string>());
        images.Remove(id);
    }

    void BeginImageCapture()
    {
        if (captureTool.Active()) return;
        listener.SetEnabled(false);
        try
        {
            binder.CancelCapture();
            if (!listener.WaitIdle(std::chrono::milliseconds(2000))) throw std::runtime_error("playback could not be stopped before screen capture");
            captureTool.Open(hwnd, binder.StopKey(), [this](tea::vision::Image image) {
                images.Add("screen capture " + std::to_string(images.NextId()), image);
                Post({{"event", "images-changed"}, {"images", images.List()}});
            }, [this] {
                imageCaptureActive = false;
                if (closing) return;
                UpdatePlaybackEnabled();
                ShowWindow(hwnd, smoke ? SW_SHOWNOACTIVATE : SW_SHOW);
                if (!smoke) SetForegroundWindow(hwnd);
            });
            imageCaptureActive = true;
            ShowWindow(hwnd, SW_HIDE);
        }
        catch (...) { UpdatePlaybackEnabled(); throw; }
    }

    void Receive(const Json& message)
    {
        const auto id = message.value("id", 0);
        try
        {
            const auto command = message.at("command").get<std::string>();
            Trace("message " + command);
            if (verifyInstall && command != "ready" && command != "state" && command != "recovery-status" &&
                command != "image-library" && command != "js-error")
                throw std::invalid_argument("command unavailable during installation verification");
            if (command == "recovery-status")
            {
                Deliver({{"id", id}, {"ok", true}, {"state", RecoveryStatus()}});
                return;
            }
            if (command == "image-library" || command == "import-image" || command == "rename-image" ||
                command == "delete-image" || (smoke && command == "test-import-image"))
            {
                if (command == "import-image")
                {
                    if (recorder.Active()) throw std::invalid_argument("stop recording before importing images");
                    images.Import(hwnd);
                }
                else if (command == "rename-image")
                    images.Rename(Integer(message, "imageId", 0, 1, INT_MAX), message.at("name").get<std::string>());
                else if (command == "delete-image") RemoveImage(Integer(message, "imageId", 0, 1, INT_MAX));
                else if (command == "test-import-image") images.Import(directory / L"image-target.png");
                Deliver({{"id", id}, {"ok", true}, {"state", images.List()}});
                return;
            }
            if (command == "image-preview")
            {
                auto image = images.Preview(Integer(message, "imageId", 0, 1, INT_MAX));
                Deliver({{"id", id}, {"ok", true}, {"state", "data:image/png;base64," + Base64(image)}});
                return;
            }
            if (smoke && command == "test-preview")
            {
                IStream* output = nullptr;
                if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &output))) throw std::runtime_error("preview stream failed");
                const auto stream = std::shared_ptr<IStream>(output, [](IStream* value) { value->Release(); });
                std::wstring filename = message.value("minimum", false) ? L"block-editor-minimum.png" : L"block-editor.png";
                const auto page = message.value("page", std::string{});
                if (page == "name") filename = L"macro-name.png";
                else if (page == "type") filename = L"macro-type.png";
                else if (page == "home") filename = L"record-home.png";
                else if (page == "unselected") filename = L"events-unselected.png";
                else if (page == "record-settings") filename = L"record-settings.png";
                else if (page == "recording") filename = L"recording.png";
                else if (page == "image-target") filename = L"image-target.png";
                else if (page == "images") filename = L"images.png";
                auto* callback = new Callback<ICoreWebView2CapturePreviewCompletedHandler, HRESULT>(
                    L"697e05e9-3d8f-45fa-96f4-8ffe1ededaf5", [this, id, stream, filename](HRESULT result) {
                        HGLOBAL memory = nullptr;
                        bool ok = SUCCEEDED(result) && SUCCEEDED(GetHGlobalFromStream(stream.get(), &memory));
                        if (ok)
                        {
                            const auto* bytes = static_cast<const char*>(GlobalLock(memory));
                            std::ofstream file(directory / filename, std::ios::binary);
                            ok = bytes && bool(file.write(bytes, static_cast<std::streamsize>(GlobalSize(memory))));
                            if (bytes) GlobalUnlock(memory);
                        }
                        Deliver({{"id", id}, {"ok", ok}, {"message", "preview capture failed"}});
                        return S_OK;
                    });
                const auto result = webview->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, output, callback);
                callback->Release();
                if (FAILED(result)) throw std::runtime_error("preview capture failed");
                return;
            }
            if (smoke && command == "test-window")
            {
                const HWND foreground = GetForegroundWindow();
                if (message.contains("captionActive"))
                    SendMessageW(hwnd, WM_NCACTIVATE, message.at("captionActive").get<bool>(), 0);
                if (message.value("minimize", false)) ShowWindow(hwnd, SW_MINIMIZE);
                if (message.value("restore", false)) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                MINMAXINFO limits{};
                SendMessageW(hwnd, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&limits));
                if (message.contains("edge"))
                {
                    RECT proposed{100, 100, 420, 340};
                    const bool handled = SendMessageW(hwnd, WM_SIZING, Integer(message, "edge", WMSZ_BOTTOMRIGHT, 1, 8),
                        reinterpret_cast<LPARAM>(&proposed)) != 0;
                    Deliver({{"id", id}, {"ok", true}, {"state", {{"handled", handled},
                        {"left", proposed.left}, {"top", proposed.top}, {"right", proposed.right}, {"bottom", proposed.bottom},
                        {"minWidth", limits.ptMinTrackSize.x}, {"minHeight", limits.ptMinTrackSize.y}}}});
                    return;
                }
                if (message.value("minimum", false) || message.value("restore", false) || message.contains("width") || message.contains("height"))
                    SetWindowPos(hwnd, nullptr, 0, 0,
                        message.value("minimum", false) ? limits.ptMinTrackSize.x : Integer(message, "width", 1440, 1, 10000),
                        message.value("minimum", false) ? limits.ptMinTrackSize.y : Integer(message, "height", 900, 1, 10000),
                        SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER |
                        (message.value("skipChanging", false) ? SWP_NOSENDCHANGING : 0));
                RECT client{};
                GetClientRect(hwnd, &client);
                RECT outer{};
                GetWindowRect(hwnd, &outer);
                const auto work = MonitorWorkArea(hwnd).value_or(RECT{});
                Trace("window " + std::to_string(outer.right - outer.left) + "x" + std::to_string(outer.bottom - outer.top) +
                    " client " + std::to_string(client.right) + "x" + std::to_string(client.bottom) +
                    " minimum " + std::to_string(limits.ptMinTrackSize.x) + "x" + std::to_string(limits.ptMinTrackSize.y));
                COLORREF caption = 0;
                COLORREF captionText = 0;
                wchar_t title[128]{};
                GetWindowTextW(hwnd, title, 128);
                BOOL dark = FALSE;
                WINDOWINFO info{sizeof(WINDOWINFO)};
                GetWindowInfo(hwnd, &info);
                const bool captionSupported = SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption)));
                DwmGetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &captionText, sizeof(captionText));
                DwmGetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
                Deliver({{"id", id}, {"ok", true}, {"state", {{"icon", GetClassLongPtrW(hwnd, GCLP_HICON) != 0},
                    {"smallIcon", GetClassLongPtrW(hwnd, GCLP_HICONSM) != 0}, {"caption", caption},
                    {"captionIcon", SendMessageW(hwnd, WM_GETICON, ICON_SMALL2, 0) != 0},
                    {"taskbarIcon", SendMessageW(hwnd, WM_GETICON, ICON_BIG, 0) != 0},
                    {"title", Utf8(title)}, {"captionText", captionText},
                    {"activeCaption", (info.dwWindowStatus & WS_ACTIVECAPTION) != 0},
                    {"focusUnchanged", foreground == GetForegroundWindow()},
                    {"captionSupported", captionSupported}, {"dark", bool(dark)},
                    {"minWidth", limits.ptMinTrackSize.x}, {"minHeight", limits.ptMinTrackSize.y},
                    {"workWidth", work.right - work.left}, {"workHeight", work.bottom - work.top},
                    {"windowWidth", outer.right - outer.left}, {"windowHeight", outer.bottom - outer.top},
                    {"minimized", IsIconic(hwnd) != FALSE},
                    {"dpi", GetDpiForWindow(hwnd)},
                    {"width", client.right}, {"height", client.bottom}}}});
                return;
            }
            if (smoke && command == "test-input")
            {
                auto* callback = new Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler, HRESULT, LPCWSTR>(
                    L"5c4889f0-5ef6-4c5a-952c-d8f1b92d0574", [this, id](HRESULT result, LPCWSTR) {
                        Deliver({{"id", id}, {"ok", SUCCEEDED(result)}, {"message", "test input failed"}});
                        return S_OK;
                    });
                const auto result = webview->CallDevToolsProtocolMethod(message.value("key", false) ?
                    L"Input.dispatchKeyEvent" : L"Input.dispatchMouseEvent", Wide(message.at("input").dump()).c_str(), callback);
                callback->Release();
                if (FAILED(result)) throw std::runtime_error("test input failed");
                return;
            }
            if (smoke && command == "test-browser-settings")
            {
                ICoreWebView2Settings* settings = nullptr;
                if (FAILED(webview->get_Settings(&settings)) || !settings)
                    throw std::runtime_error("browser settings could not be read");
                BOOL devTools = TRUE, zoom = TRUE, accelerators = TRUE;
                settings->get_AreDevToolsEnabled(&devTools);
                settings->get_IsZoomControlEnabled(&zoom);
                ICoreWebView2Settings3* shortcuts = nullptr;
                const HRESULT query = settings->QueryInterface(IID_ICoreWebView2Settings3, reinterpret_cast<void**>(&shortcuts));
                settings->Release();
                if (FAILED(query)) throw std::runtime_error("browser shortcuts could not be read");
                const HRESULT read = shortcuts->get_AreBrowserAcceleratorKeysEnabled(&accelerators);
                shortcuts->Release();
                if (FAILED(read)) throw std::runtime_error("browser shortcuts could not be read");
                Deliver({{"id", id}, {"ok", true}, {"state", {{"accelerators", bool(accelerators)},
                    {"devTools", bool(devTools)}, {"zoom", bool(zoom)}}}});
                return;
            }
            if (desktopSmoke && command == "test-cursor")
            {
                POINT point{};
                if (!GetCursorPos(&point)) throw std::runtime_error("the cursor could not be read");
                MONITORINFO monitor{sizeof(MONITORINFO)};
                if (!GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &monitor))
                    throw std::runtime_error("the screen could not be read");
                Deliver({{"id", id}, {"ok", true}, {"state", {{"x", point.x}, {"y", point.y},
                    {"targetX", (monitor.rcMonitor.left + monitor.rcMonitor.right) / 2},
                    {"targetY", (monitor.rcMonitor.top + monitor.rcMonitor.bottom) / 2}}}});
                return;
            }
            if (desktopSmoke && command == "test-image-capture")
            {
                const auto mouse = message.value("mouse", std::string{});
                if (!mouse.empty())
                {
                    const HWND selection = captureTool.SelectionWindow();
                    if (!selection) throw std::invalid_argument("screen selection is not active");
                    const int x = Integer(message, "x", 0, 0, SHRT_MAX), y = Integer(message, "y", 0, 0, SHRT_MAX);
                    const UINT input = mouse == "down" ? WM_LBUTTONDOWN : mouse == "move" ? WM_MOUSEMOVE : WM_LBUTTONUP;
                    SendMessageW(selection, input, input == WM_LBUTTONUP ? 0 : MK_LBUTTON, MAKELPARAM(x, y));
                }
                Deliver({{"id", id}, {"ok", true}, {"state", {
                    {"toolbar", IsWindowVisible(captureTool.Toolbar()) != FALSE},
                    {"selection", IsWindowVisible(captureTool.SelectionWindow()) != FALSE},
                    {"app", IsWindowVisible(hwnd) != FALSE}}}});
                return;
            }
            if ((smoke && command == "test-trigger") || (desktopSmoke && command == "test-move"))
            {
                const Trigger trigger{UINT(message.at("code").get<int>()), 0};
                if (binder.Capturing() || trigger.key == binder.StopKey() || trigger.key == binder.RecordKey() ||
                    (imageCaptureActive && trigger.key == VK_F11))
                {
                    listener.ProcessEvent(trigger, true);
                    listener.ProcessEvent(trigger, false);
                    Deliver({{"id", id}, {"ok", true}, {"state", state}});
                    return;
                }
                if (!binder.StopKey()) throw std::invalid_argument("choose a stop hotkey before running macros");
                const auto binding = binder.Find(trigger);
                if (!binding) throw std::invalid_argument("test binding is missing");
                for (const auto& action : binding->macro->Actions())
                    if (action.type != MacroAction::Type::Wait && !(desktopSmoke && action.type == MacroAction::Type::Move))
                        throw std::invalid_argument("playback tests may only use supported test actions");
                if (command == "test-move")
                {
                    POINT before{}, after{};
                    if (!GetCursorPos(&before)) throw std::runtime_error("the cursor could not be read");
                    try { binding->macro->Execute(*WindowsMacroOutput(), [](auto) { return true; }); }
                    catch (...) { SetCursorPos(before.x, before.y); throw; }
                    const bool read = GetCursorPos(&after) != FALSE;
                    SetCursorPos(before.x, before.y);
                    if (!read) throw std::runtime_error("the cursor could not be read");
                    Deliver({{"id", id}, {"ok", true}, {"state", {{"x", after.x}, {"y", after.y}}}});
                    return;
                }
                listener.ProcessEvent(trigger, true);
                listener.ProcessEvent(trigger, false);
                Deliver({{"id", id}, {"ok", true}, {"state", state}});
                return;
            }
            if (command == "js-error")
            {
                Trace(message.value("message", std::string{}));
                if (!interfaceReady) { Fail(L"the interface failed to initialize"); return; }
            }
            if (recorder.Active() && (command == "save" || command == "save-recovered" || command == "capture-stop" || command == "capture-record"))
                throw std::invalid_argument("stop recording before changing macros");
            if (command == "save")
            {
                if (!configurationNotice.empty()) throw std::invalid_argument("save the recovered configuration before making changes");
                Save(message.at("state"));
            }
            else if (command == "save-recovered") SaveRecoveredConfiguration();
            else if (command == "start-image-capture")
            {
                if (smoke && !desktopSmoke) throw std::invalid_argument("screen capture requires a desktop test");
                if (recorder.Active()) throw std::invalid_argument("stop recording before capturing images");
                BeginImageCapture();
            }
            else if (command == "start-recording")
            {
                const int session = Integer(message, "session", 1, 1, INT_MAX);
                if (recorder.Active()) throw std::invalid_argument("recording is already active");
                if (captureTool.Active()) throw std::invalid_argument("finish screen capture before recording");
                listener.SetEnabled(false);
                try
                {
                    binder.CancelCapture();
                    if (!listener.WaitIdle(std::chrono::milliseconds(2000))) throw std::runtime_error("playback could not be stopped before recording");
                    recorder.Start(binder.StopKey(), binder.RecordKey(), ReadRecordingOptions(state),
                        unsigned(Integer(message, "limit", 256, 2, 256)), !smoke);
                    recordingSession = session;
                    recordingActive = true;
                }
                catch (...) { UpdatePlaybackEnabled(); throw; }
            }
            else if (command == "stop-recording") recorder.Stop(GetTickCount64());
            else if (command == "stop-playback") listener.Cancel();
            else if (smoke && command == "test-record")
                recorder.Input(message.value("mouse", false), unsigned(Integer(message, "key", 65, 0, 511)),
                    message.value("down", true), message.value("time", GetTickCount64()), message.value("injected", false),
                    Coordinates(Integer(message, "x", 0, INT_MIN, INT_MAX), Integer(message, "y", 0, INT_MIN, INT_MAX)));
            else if (command == "capture-stop" || command == "capture-record")
            {
                const int captureId = Integer(message, "captureId", 0, 1, INT_MAX);
                listener.BeginCapture([this, captureId](auto trigger) {
                    Json event = {{"event", "captured"}, {"captureId", captureId}};
                    if (trigger) event["trigger"] = {{"code", trigger->key}};
                    else event["trigger"] = nullptr;
                    Post(std::move(event));
                }, true);
            }
            else if (command == "cancel-capture") binder.CancelCapture();
            else if (command == "editing")
            {
                listener.IgnoreWindow(message.at("active").get<bool>() ? hwnd : nullptr);
                recordHotkeyEnabled = message.value("canRecord", false);
            }
            else if (command == "ready" && !interfaceReady && failure.empty())
            {
                if (verifyInstall)
                {
                    interfaceReady = passed = true;
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                    return;
                }
                try { listener.Start(!smoke); }
                catch (const std::exception& error) { Fail(Wide(error.what()).c_str()); return; }
                UpdatePlaybackEnabled();
                interfaceReady = true;
                if (smoke)
                {
                    const auto script = std::string(desktopSmoke ? "window.teaDesktopSmoke = true;\n" : "") + Resource(104);
                    if (FAILED(webview->ExecuteScript(Wide(script).c_str(), nullptr)))
                    {
                        Fail(L"the interface test could not start");
                        return;
                    }
                }
                else KillTimer(hwnd, STARTUP_TIMER);
            }
            else if (command == "test-result" && smoke)
            {
                passed = message.value("ok", false);
                if (!passed) ReportSmokeFailure(message.value("message", std::string{"ui test failed"}));
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }
            else if (command != "state" && command != "ready" && command != "js-error") throw std::invalid_argument("unknown command");
            Deliver({{"id", id}, {"ok", true}, {"state", state}});
        }
        catch (const Json::exception&) { Deliver({{"id", id}, {"ok", false}, {"message", "invalid macro configuration"}}); }
        catch (const std::exception& error) { Deliver({{"id", id}, {"ok", false}, {"message", error.what()}}); }
    }

    void InitializeView()
    {
        Trace("initializing view");
        Resize();
        ICoreWebView2Settings* settings = nullptr;
        webview->get_Settings(&settings);
        if (settings)
        {
            BOOL scripts = FALSE, messages = FALSE;
            settings->get_IsScriptEnabled(&scripts); settings->get_IsWebMessageEnabled(&messages);
            Trace("settings " + std::to_string(scripts) + " " + std::to_string(messages));
            settings->put_IsScriptEnabled(TRUE); settings->put_IsWebMessageEnabled(TRUE);
            settings->put_AreDefaultContextMenusEnabled(FALSE);
            settings->put_AreDevToolsEnabled(FALSE);
            settings->put_IsZoomControlEnabled(FALSE);
            ICoreWebView2Settings3* shortcuts = nullptr;
            const HRESULT query = settings->QueryInterface(IID_ICoreWebView2Settings3, reinterpret_cast<void**>(&shortcuts));
            settings->Release();
            if (FAILED(query)) throw std::runtime_error("update the webview2 runtime to disable browser shortcuts");
            const HRESULT disabled = shortcuts->put_AreBrowserAcceleratorKeysEnabled(FALSE);
            shortcuts->Release();
            if (FAILED(disabled)) throw std::runtime_error("browser shortcuts could not be disabled");
        }
        else throw std::runtime_error("the application settings could not be initialized");
        auto* messages = new Callback<ICoreWebView2WebMessageReceivedEventHandler, ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*>(
            L"57213f19-00e6-49fa-8e07-898ea01ecbd2", [this](auto*, auto* args) {
                LPWSTR source = nullptr, text = nullptr;
                args->get_Source(&source);
                const bool trusted = source && (std::wstring(source) == L"about:blank" || std::wstring(source) == documentUri);
                Trace(trusted ? "trusted message" : "untrusted message");
                CoTaskMemFree(source);
                if (!trusted) return E_ACCESSDENIED;
                args->get_WebMessageAsJson(&text);
                if (!text) return E_INVALIDARG;
                const auto value = Utf8(text);
                CoTaskMemFree(text);
                try { Receive(Json::parse(value)); }
                catch (...) { Deliver({{"event", "error"}, {"message", "invalid application message"}}); }
                return S_OK;
            });
        EventRegistrationToken token{};
        const HRESULT received = webview->add_WebMessageReceived(messages, &token);
        messages->Release();
        if (FAILED(received)) throw std::runtime_error("the interface could not connect to tea");
        auto* navigation = new Callback<ICoreWebView2NavigationStartingEventHandler, ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*>(
            L"9adbe429-f36d-432b-9ddc-f8881fbd76e3", [this](auto*, auto* args) {
                LPWSTR uri = nullptr;
                args->get_Uri(&uri);
                const bool trusted = uri && (std::wstring(uri) == L"about:blank" || std::wstring(uri) == documentUri);
                Trace(trusted ? "trusted navigation" : "blocked navigation");
                args->put_Cancel(trusted ? FALSE : TRUE);
                CoTaskMemFree(uri);
                return S_OK;
            });
        const HRESULT navigating = webview->add_NavigationStarting(navigation, &token);
        navigation->Release();
        if (FAILED(navigating)) throw std::runtime_error("interface navigation could not be initialized");
        auto* completed = new Callback<ICoreWebView2NavigationCompletedEventHandler, ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*>(
            L"d33a35bf-1c49-4f98-93ab-006e0533fe1c", [this](auto*, auto* args) {
                BOOL success = FALSE;
                args->get_IsSuccess(&success);
                COREWEBVIEW2_WEB_ERROR_STATUS error{};
                args->get_WebErrorStatus(&error);
                Trace(success ? "navigation complete" : "navigation failed " + std::to_string(error));
                if (!success && !interfaceReady) return Fail(L"the interface could not be loaded");
                if (smoke)
                {
                    auto* inspect = new Callback<ICoreWebView2ExecuteScriptCompletedHandler, HRESULT, LPCWSTR>(
                        L"49511172-cc67-4bca-9923-137112f4c4cc", [this](HRESULT result, LPCWSTR text) {
                            Trace("script " + std::to_string(result) + " " + (text ? Utf8(text) : "none"));
                            return S_OK;
                        });
                    webview->ExecuteScript(L"JSON.stringify({ready:document.readyState,title:document.title,buttons:document.querySelectorAll('button').length,bridge:!!window.chrome?.webview,toast:document.querySelector('.toast')?.textContent})", inspect);
                    inspect->Release();
                }
                return S_OK;
            });
        const HRESULT navigated = webview->add_NavigationCompleted(completed, &token);
        completed->Release();
        if (FAILED(navigated)) throw std::runtime_error("interface navigation could not be monitored");
        auto* failed = new Callback<ICoreWebView2ProcessFailedEventHandler, ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*>(
            L"79e0aea4-990b-42d9-aa1d-0fcc2e5bc7f1", [this](auto*, auto* args) {
                COREWEBVIEW2_PROCESS_FAILED_KIND kind{};
                if (FAILED(args->get_ProcessFailedKind(&kind))) return S_OK;
                Trace("webview process failure " + std::to_string(kind));
                if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED ||
                    kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED ||
                    (interfaceReady && kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE))
                    return Fail(L"the interface stopped responding; reopen tea to continue");
                return S_OK;
            });
        const HRESULT monitored = webview->add_ProcessFailed(failed, &token);
        failed->Release();
        if (FAILED(monitored)) throw std::runtime_error("the interface process could not be monitored");
        webview->AddScriptToExecuteOnDocumentCreated(L"window.addEventListener('error', e => window.chrome.webview.postMessage({command:'js-error',message:e.message})); window.addEventListener('unhandledrejection', e => window.chrome.webview.postMessage({command:'js-error',message:String(e.reason)}));", nullptr);
        auto html = Resource(101);
        Trace("html bytes " + std::to_string(html.size()));
        const std::string fontPath = "fonts/JetBrainsMono-Regular.woff2";
        const auto font = html.find(fontPath);
        if (font != std::string::npos) html.replace(font, fontPath.size(), "data:font/woff2;base64," + Base64(Resource(102)));
        Trace("navigating");
        documentUri = L"data:text/html;charset=utf-8;base64," + Wide(Base64(html));
        if (FAILED(webview->NavigateToString(Wide(html).c_str()))) Fail(L"the interface could not be opened");
    }

    HWND hwnd = nullptr;
    std::mutex eventMutex;
    std::deque<Json> pendingEvents;
    HMODULE loader = nullptr;
    ICoreWebView2Controller* controller = nullptr;
    ICoreWebView2* webview = nullptr;
    std::shared_ptr<int> viewLifetime = std::make_shared<int>(0);
    bool closing = false, interfaceReady = false, needsRuntime = false;
    const bool smoke, desktopSmoke, verifyInstall;
    bool passed = false;
    const fs::path directory;
    std::wstring documentUri, failure;
    std::string configurationNotice;
    bool hasSavedConfiguration = false, archiveConfiguration = false;
    bool archiveBackupConfiguration = false;
    Json state = {{"version", 1}, {"enabled", true}, {"macros", Json::array()}, {"bindings", Json::array()}};
    tea::vision::ImageLibrary images;
    tea::vision::ScreenCaptureTool captureTool{[](const tea::vision::Image&) {}};
    std::atomic_bool imageCaptureActive{false}, recordHotkeyEnabled{false}, recordingActive{false};
    Binder binder;
    MacroListener listener;
    MacroRecorder recorder;
    int recordingSession = 0;
};

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCACTIVATE)
    {
        DefWindowProcW(window, message, TRUE, lparam);
        return TRUE;
    }
    if (message == WM_GETICON && (wparam == ICON_SMALL || wparam == ICON_SMALL2)) return 0;
    if (message == WM_SIZING)
    {
        auto& bounds = *reinterpret_cast<RECT*>(lparam);
        const POINT minimum = MinimumWindowSize(window);
        const bool left = wparam == WMSZ_LEFT || wparam == WMSZ_TOPLEFT || wparam == WMSZ_BOTTOMLEFT;
        const bool top = wparam == WMSZ_TOP || wparam == WMSZ_TOPLEFT || wparam == WMSZ_TOPRIGHT;
        if (bounds.right - bounds.left < minimum.x)
        {
            if (left) bounds.left = bounds.right - minimum.x;
            else bounds.right = bounds.left + minimum.x;
        }
        if (bounds.bottom - bounds.top < minimum.y)
        {
            if (top) bounds.top = bounds.bottom - minimum.y;
            else bounds.bottom = bounds.top + minimum.y;
        }
        return TRUE;
    }
    if (message == WM_GETMINMAXINFO)
    {
        reinterpret_cast<MINMAXINFO*>(lparam)->ptMinTrackSize = MinimumWindowSize(window);
        return 0;
    }
    if (message == WM_WINDOWPOSCHANGING)
    {
        const LRESULT result = DefWindowProcW(window, message, wparam, lparam);
        auto* position = reinterpret_cast<WINDOWPOS*>(lparam);
        if (!(position->flags & SWP_NOSIZE) && !IsIconic(window))
        {
            const POINT minimum = MinimumWindowSize(window);
            position->cx = std::max(position->cx, int(minimum.x));
            position->cy = std::max(position->cy, int(minimum.y));
        }
        return result;
    }
    if (message == WM_THEMECHANGED || message == WM_DWMCOLORIZATIONCOLORCHANGED) StyleWindow(window);
    if (message == WM_DPICHANGED)
    {
        const auto& bounds = *reinterpret_cast<RECT*>(lparam);
        SetWindowPos(window, nullptr, bounds.left, bounds.top, bounds.right - bounds.left,
            bounds.bottom - bounds.top, SWP_NOACTIVATE | SWP_NOZORDER);
        return 0;
    }
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (message == WM_SIZE && wparam != SIZE_MINIMIZED)
    {
        RECT bounds{};
        GetWindowRect(window, &bounds);
        const POINT minimum = MinimumWindowSize(window);
        const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
        if (width < minimum.x || height < minimum.y)
        {
            SetWindowPos(window, nullptr, 0, 0, std::max(width, int(minimum.x)), std::max(height, int(minimum.y)),
                SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER);
            return 0;
        }
        if (app) app->Resize();
    }
    if (message == WM_TIMER && wparam == STARTUP_TIMER)
    {
        KillTimer(window, STARTUP_TIMER);
        if (app) app->StartupTimedOut();
        return 0;
    }
    if (message == WEB_EVENT)
    {
        if (app) app->DeliverPostedEvent();
        return 0;
    }
    if (message == WM_CLOSE) { if (app) app->Close(); DestroyWindow(window); return 0; }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}
}

int RunTea(HINSTANCE instance, int show)
{
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    const std::wstring_view option = arguments && count >= 2 ? arguments[1] : L"";
    const bool desktopSmoke = count == 3 && option == L"--smoke-test";
    const bool smoke = desktopSmoke || (count == 3 && option == L"--interface-test");
    const bool verifyInstall = option == L"--verify-install";
    const bool offscreen = smoke || verifyInstall;
    const fs::path data = offscreen && count == 3 ? fs::path(arguments[2]) : fs::path{};
    LocalFree(arguments);
#ifndef TEA_DESKTOP_SMOKE
    if (smoke) return 2;
#endif
    if (verifyInstall)
    {
        std::error_code error;
        if (data.empty() || (fs::exists(data, error) && !fs::is_empty(data, error)) || error) return 2;
    }
    const HANDLE singleInstance = CreateMutexW(nullptr, TRUE, offscreen ? L"Local\\TeaMacroToolSmoke" : L"Local\\TeaMacroTool");
    if (!singleInstance || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (!verifyInstall) MessageBoxW(nullptr, L"tea is already running", L"tea", MB_OK);
        if (singleInstance) CloseHandle(singleInstance);
        return verifyInstall ? 1 : 0;
    }
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) SetProcessDPIAware();
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) { CloseHandle(singleInstance); return 1; }
    int result = 0;
    try
    {
        App app(data, smoke, desktopSmoke, verifyInstall);
        WNDCLASSEXW type{};
        type.cbSize = sizeof(type);
        type.hInstance = instance; type.lpszClassName = L"TeaMacroWindow";
        type.lpfnWndProc = WindowProcedure; type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        type.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        type.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
            GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED));
        RegisterClassExW(&type);
        HWND window = CreateWindowW(type.lpszClassName, L"Tea, Macro Tool", WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 1440, 900, nullptr, nullptr, instance, &app);
        if (!window) throw std::runtime_error("the application window could not be created");
        SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(type.hIcon));
        StyleWindow(window);
        if (offscreen) SetWindowPos(window, nullptr, -30000, -30000, 1440, 900, SWP_NOACTIVATE | SWP_NOZORDER);
        else if (const auto work = MonitorWorkArea(window))
        {
            RECT bounds{};
            GetWindowRect(window, &bounds);
            const auto width = std::min(bounds.right - bounds.left, work->right - work->left);
            const auto height = std::min(bounds.bottom - bounds.top, work->bottom - work->top);
            SetWindowPos(window, nullptr, std::clamp(bounds.left, work->left, work->right - width),
                std::clamp(bounds.top, work->top, work->bottom - height), width, height, SWP_NOACTIVATE | SWP_NOZORDER);
        }
        ShowWindow(window, offscreen ? SW_SHOWNOACTIVATE : show);
        SetTimer(window, STARTUP_TIMER, offscreen ? 15000 : 30000, nullptr);
        try
        {
            app.Open(window);
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        catch (...) { if (IsWindow(window)) SendMessageW(window, WM_CLOSE, 0, 0); throw; }
        if (!app.Failure().empty())
        {
            result = 1;
            if (!offscreen && app.NeedsRuntime())
            {
                if (MessageBoxW(nullptr, L"tea requires the microsoft edge webview2 runtime. open microsoft's download page?",
                    L"tea", MB_YESNO | MB_ICONERROR) == IDYES)
                    ShellExecuteW(nullptr, L"open", L"https://developer.microsoft.com/en-us/microsoft-edge/webview2/#download", nullptr, nullptr, SW_SHOWNORMAL);
            }
            else if (!offscreen) MessageBoxW(nullptr, app.Failure().c_str(), L"tea", MB_OK | MB_ICONERROR);
        }
        else if (offscreen && !app.Passed()) result = 1;
    }
    catch (const std::exception& error)
    {
        if (verifyInstall) std::cerr << error.what() << '\n';
        else MessageBoxW(nullptr, Wide(error.what()).c_str(), L"tea", MB_OK | MB_ICONERROR);
        result = 1;
    }
    CoUninitialize();
    ReleaseMutex(singleInstance);
    CloseHandle(singleInstance);
    return result;
}
