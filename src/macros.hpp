#ifndef TEA_MACROS_HPP
#define TEA_MACROS_HPP

#include <windows.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>
#include "2d/types.hpp"
#include "tea_wraps.hpp"

struct Trigger
{
    UINT key = 0;
    UINT modifiers = 0;
    bool operator<(const Trigger& other) const;
    static bool Valid(Trigger trigger);
    static bool ValidStopKey(UINT key);
};

struct ImageOnScreen { int imageId; };
using MacroCondition = std::variant<ImageOnScreen>;

struct MacroAction
{
    enum class Type { Key, Mouse, Text, Wait, Move, WaitForImage, IfElse, While };
    enum class Mode { Press, Down, Up };
    struct DurationRange { std::chrono::milliseconds min, max; };
    Type type = Type::Wait;
    Mode mode = Mode::Press;
    UINT key = 0;
    UINT modifiers = 0;
    int button = 0;
    std::wstring text;
    std::chrono::milliseconds duration{0};
    Coordinates position;
    bool positioned = false;
    int imageId = 0;
    std::optional<MacroCondition> condition;
    std::vector<MacroAction> actions, elseActions;
    std::optional<DurationRange> durationRange;
};

class MacroRecorder
{
public:
    using Event = std::function<void(const MacroAction&)>;
    struct Options { bool delays = true, clickPosition = false, keyboard = true, mouseButtons = true, splitKeyEvents = false; };
    MacroRecorder(Event event, std::function<void()> stopped);
    ~MacroRecorder();
    MacroRecorder(const MacroRecorder&) = delete;
    MacroRecorder& operator=(const MacroRecorder&) = delete;
    void Start(UINT stopKey, UINT recordKey, Options options, unsigned limit, bool native = true);
    void Stop(ULONGLONG time = 0);
    bool Active() const { return active; }
    void Input(bool mouse, UINT key, bool down, ULONGLONG time, bool injected = false,
        std::optional<Coordinates> position = std::nullopt);
    LRESULT KeyboardInput(UINT key, bool down, ULONGLONG time, ULONG_PTR extraInfo, const std::function<LRESULT()>& nextHook);
    void MouseInput(UINT message, Coordinates position, ULONGLONG time, ULONG_PTR extraInfo = 0);

private:
    static LRESULT CALLBACK KeyboardHook(int code, WPARAM message, LPARAM data);
    static LRESULT CALLBACK MouseHook(int code, WPARAM message, LPARAM data);
    void RecordDelay(ULONGLONG time, unsigned reserved);
    void FlushKey();
    bool FinishKey(ULONGLONG time);
    bool Holding(bool mouse, UINT key) const;
    Event event;
    std::function<void()> stopped;
    HHOOK keyboard = nullptr, mouse = nullptr;
    UINT stopKey = 0, recordKey = 0;
    unsigned remaining = 0;
    bool active = false, hasTime = false;
    Options options;
    ULONGLONG lastTime = 0;
    std::optional<UINT> pendingKey;
    std::vector<std::pair<bool, UINT>> held;
};

class MacroOutput
{
public:
    using Wait = std::function<bool(std::chrono::milliseconds)>;
    virtual ~MacroOutput() = default;
    virtual void Key(UINT key, bool down) = 0;
    virtual void Mouse(int button, bool down) = 0;
    virtual void Character(wchar_t character, bool down) = 0;
    virtual bool Move(Coordinates position, const Wait& wait) = 0;
    virtual bool IsDown(UINT) { return false; }
    virtual bool IsForeground(HWND) { return false; }
};

std::shared_ptr<MacroOutput> WindowsMacroOutput();

class Macro
{
public:
    using Wait = MacroOutput::Wait;
    using LoadImage = std::function<tea::vision::Image(int)>;
    using CaptureScreen = std::function<tea::vision::Image()>;
    enum class Type { NoRepeat, Toggle };
    Macro(std::string name, std::vector<MacroAction> actions, Type type = Type::NoRepeat);
    const std::string& Name() const { return name; }
    Type PlaybackType() const { return type; }
    const std::vector<MacroAction>& Actions() const { return actions; }
    bool Execute(MacroOutput& output, const Wait& wait, const LoadImage& loadImage = {},
        const CaptureScreen& captureScreen = {}) const;

private:
    std::string name;
    std::vector<MacroAction> actions;
    Type type;
};

struct Binding
{
    Trigger trigger;
    std::shared_ptr<const Macro> macro;
    bool replace = false;
};

class Binder
{
public:
    using Capture = std::function<void(std::optional<Trigger>)>;
    void Bind(Binding binding);
    bool Unbind(Trigger trigger);
    void Replace(std::vector<Binding> bindings, std::optional<UINT> stopKey = std::nullopt,
        std::optional<UINT> recordKey = std::nullopt);
    UINT StopKey() const;
    UINT RecordKey() const;
    std::optional<Binding> Find(Trigger trigger) const;
    void BeginCapture(Capture callback, bool stopKey = false);
    void CancelCapture();
    bool Capturing() const;
    bool CaptureKey(Trigger trigger);

private:
    mutable std::mutex mutex;
    std::map<Trigger, Binding> bindings;
    Capture capture;
    UINT stopKey = 0, recordKey = 0;
    bool captureStopKey = false;
};

class MacroListener
{
public:
    using Error = std::function<void(const std::string&)>;
    using Playback = std::function<void(bool, bool)>;
    explicit MacroListener(Binder& binder,
        std::shared_ptr<MacroOutput> output = WindowsMacroOutput(), Error error = {}, Playback playback = {},
        std::function<bool()> recordHotkey = {}, Macro::LoadImage loadImage = {},
        std::function<bool()> stopHotkey = {}, std::function<bool(Trigger)> scopedHotkey = {});
    ~MacroListener();
    MacroListener(const MacroListener&) = delete;
    MacroListener& operator=(const MacroListener&) = delete;
    void Start(bool native = true);
    void Stop();
    bool Cancel();
    void Pause(bool value);
    void SetEnabled(bool value);
    void IgnoreWindow(HWND window);
    void ReplaceBindings(std::vector<Binding> bindings, UINT stopKey, UINT recordKey);
    void BeginCapture(Binder::Capture callback, bool stopKey = false);
    bool ProcessEvent(Trigger trigger, bool down, bool injected = false, bool allow = true);
    bool WaitIdle(std::chrono::milliseconds timeout);

private:
    static LRESULT CALLBACK KeyboardHook(int code, WPARAM message, LPARAM data);
    bool ProcessKey(Trigger trigger, bool down, bool allow, bool recordOnly);
    bool CancelLocked();
    bool ForegroundAllowed() const;
    void Work();
    bool Wait(std::chrono::milliseconds duration);
    void Report(const std::string& message) noexcept;
    void NotifyPlayback(bool active, bool paused) noexcept;

    Binder& binder;
    std::shared_ptr<MacroOutput> output;
    Error error;
    Playback playback;
    std::function<bool()> recordHotkey;
    Macro::LoadImage loadImage;
    std::function<bool()> stopHotkey;
    std::function<bool(Trigger)> scopedHotkey;
    std::atomic_uint64_t playbackGeneration{0};
    std::atomic<DWORD> hookThreadId{0};
    std::thread hookThread, worker;
    std::mutex mutex, eventMutex;
    std::condition_variable changed;
    struct Job { Binding binding; std::shared_ptr<bool> cancelled; };
    std::deque<Job> queue;
    std::map<Trigger, std::shared_ptr<bool>> toggles;
    std::shared_ptr<bool> activeCancellation;
    std::array<bool, 512> pressed{}, suppressed{};
    std::array<bool, 256> physical{};
    HWND ignoredWindow = nullptr;
    bool enabled = true, stopping = true, cancelled = false, busy = false, paused = false;
};

#endif
