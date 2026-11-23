#include "macros.hpp"
#include "winput.hpp"
#include "2d/capture.hpp"
#include "2d/matcher.hpp"
#include <future>
#include <algorithm>
#include <random>
#include <stdexcept>
#include <tuple>

namespace
{
constexpr UINT MODIFIERS = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
constexpr ULONG_PTR INPUT_TAG = 0x544541;
thread_local MacroListener* currentListener = nullptr;
thread_local MacroRecorder* currentRecorder = nullptr;

bool ValidKey(UINT key)
{
    return (key >= VK_BACK && key <= 254) || key == VK_CANCEL || key == (0x100 | VK_RETURN);
}

bool Modifier(UINT key)
{
    return key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU ||
        (key >= VK_LSHIFT && key <= VK_RMENU) || key == VK_LWIN || key == VK_RWIN;
}

void CheckBinding(const Binding& binding)
{
    if (!binding.macro || !Trigger::Valid(binding.trigger))
        throw std::invalid_argument("choose a supported key and macro");
}

tea::vision::Target ImageTarget(int id, const Macro::LoadImage& loadImage)
{
    if (!loadImage) throw std::runtime_error("the image library is unavailable");
    tea::vision::SearchOptions options;
    options.maximumChannelError = 24;
    auto image = loadImage(id);
    tea::vision::ValidateTemplate(image);
    return {std::to_string(id), std::move(image), options};
}

std::optional<tea::vision::Detection> FindImage(const tea::vision::Target& target,
    const Macro::CaptureScreen& captureScreen, const std::function<bool()>& keepSearching, bool firstMatch)
{
    const auto frame = captureScreen ? captureScreen() : tea::vision::CaptureScreen();
    return tea::vision::Match(frame, target, keepSearching, firstMatch);
}

std::optional<tea::vision::Rect> WaitForImage(const MacroAction& action, const Macro::Wait& wait,
    const Macro::LoadImage& loadImage, const Macro::CaptureScreen& captureScreen)
{
    using namespace std::chrono_literals;
    const auto target = ImageTarget(action.imageId, loadImage);
    const bool firstMatch = action.type == MacroAction::Type::WaitForImage;
    bool searching = true;
    const auto keepSearching = [&] { return searching = wait(0ms); };
    if (!keepSearching()) return std::nullopt;
    for (;;)
    {
        if (const auto match = FindImage(target, captureScreen, keepSearching, firstMatch)) return match->bounds;
        if (!searching || !wait(150ms)) return std::nullopt;
    }
}

void ValidateCondition(const ImageOnScreen& condition)
{
    if (condition.imageId <= 0) throw std::invalid_argument("choose an image to check");
}

std::optional<bool> EvaluateCondition(const ImageOnScreen& condition, const Macro::Wait& wait,
    const Macro::LoadImage& loadImage, const Macro::CaptureScreen& captureScreen)
{
    using namespace std::chrono_literals;
    const auto target = ImageTarget(condition.imageId, loadImage);
    bool searching = true;
    const auto keepSearching = [&] { return searching = wait(0ms); };
    if (!keepSearching()) return std::nullopt;
    const auto match = FindImage(target, captureScreen, keepSearching, true);
    if (!searching || !keepSearching()) return std::nullopt;
    return match.has_value();
}

int RandomInRange(int minimum, int maximum)
{
    static thread_local std::mt19937 random{std::random_device{}()};
    return std::uniform_int_distribution<int>(minimum, maximum)(random);
}

Coordinates ImageLanding(tea::vision::Rect bounds)
{
    const auto offset = [](int size) {
        const int center = size / 2;
        const int radius = std::clamp(size / 8, 1, 10);
        return RandomInRange(std::max(0, center - radius), std::min(size - 1, center + radius));
    };
    return {bounds.x + offset(bounds.width), bounds.y + offset(bounds.height)};
}

class WindowsOutput : public MacroOutput
{
    INPUT input{};
    POINT mouse{};
    std::random_device random;
    Winput winput{input, mouse, random};

    void Send(INPUT input)
    {
        if (input.type == INPUT_KEYBOARD) input.ki.dwExtraInfo = INPUT_TAG;
        else input.mi.dwExtraInfo = INPUT_TAG;
        if (SendInput(1, &input, sizeof(input)) != 1)
            throw std::runtime_error("input could not be sent; check the target app's permissions");
    }

public:
    void Key(UINT key, bool down) override
    {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = static_cast<WORD>(key & 0xff);
        const bool extended = key > 255 || key == VK_RCONTROL || key == VK_RMENU ||
            key == VK_LWIN || key == VK_RWIN || key == VK_APPS || key == VK_DIVIDE ||
            key == VK_NUMLOCK || key == VK_SNAPSHOT ||
            (key >= VK_PRIOR && key <= VK_DOWN) || key == VK_INSERT || key == VK_DELETE;
        input.ki.dwFlags = (down ? 0 : KEYEVENTF_KEYUP) | (extended ? KEYEVENTF_EXTENDEDKEY : 0);
        Send(input);
    }

    void Mouse(int button, bool down) override
    {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = button == 0 ? (down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP)
                                     : (down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP);
        Send(input);
    }

    void Character(wchar_t character, bool down) override
    {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = character;
        input.ki.dwFlags = KEYEVENTF_UNICODE | (down ? 0 : KEYEVENTF_KEYUP);
        Send(input);
    }

    bool Move(Coordinates position, const Wait& wait) override
    {
        if (!Winput::IsOnDisplay(position))
        {
            if (!wait(std::chrono::milliseconds(0))) return false;
            throw std::runtime_error("the mouse destination is outside the connected displays");
        }
        bool cancelled = false;
        const bool moved = winput.RealisticMouseMove(winput.GetMousePos(), position, [&](auto duration) {
            cancelled = !wait(duration);
            return !cancelled;
        });
        if (!moved && !cancelled)
            throw std::runtime_error("the mouse could not be moved");
        if (moved && !(winput.GetMousePos() == position))
            throw std::runtime_error("the mouse could not reach the requested position");
        return moved;
    }

    bool IsDown(UINT key) override { return (GetAsyncKeyState(key & 0xff) & 0x8000) != 0; }
    bool IsForeground(HWND window) override { return GetAncestor(GetForegroundWindow(), GA_ROOTOWNER) == window; }
};

class HeldInputs
{
public:
    explicit HeldInputs(MacroOutput& output, const HeldInputs* preserved = nullptr)
        : output(output), preserved(preserved) { held.reserve(4); }
    ~HeldInputs() { Release(); }

    void Down(int type, UINT key)
    {
        if ((type == 0 && output.IsDown(key)) || Contains(type, key) || (preserved && preserved->Contains(type, key))) return;
        Send(type, key, true);
        held.emplace_back(type, key);
    }

    void Up(int type, UINT key)
    {
        const auto found = std::find(held.begin(), held.end(), std::make_pair(type, key));
        if (found == held.end()) return;
        Send(type, key, false);
        held.erase(found);
    }

    void Release(bool report = false)
    {
        std::exception_ptr failure;
        for (std::size_t i = held.size(); i > 0; --i)
        {
            try { Send(held[i - 1].first, held[i - 1].second, false); held.erase(held.begin() + i - 1); }
            catch (...) { if (!failure) failure = std::current_exception(); }
        }
        if (report && failure) std::rethrow_exception(failure);
    }

private:
    bool Contains(int type, UINT key) const
    { return std::find(held.begin(), held.end(), std::make_pair(type, key)) != held.end(); }

    void Send(int type, UINT key, bool down)
    {
        if (type == 0) output.Key(key, down);
        else if (type == 1) output.Mouse(static_cast<int>(key), down);
        else output.Character(static_cast<wchar_t>(key), down);
    }
    MacroOutput& output;
    const HeldInputs* preserved;
    std::vector<std::pair<int, UINT>> held;
};
}

bool Trigger::operator<(const Trigger& other) const
{
    return std::tie(key, modifiers) < std::tie(other.key, other.modifiers);
}

bool Trigger::Valid(Trigger trigger)
{
    return ValidKey(trigger.key) && !Modifier(trigger.key) && trigger.key != VK_ESCAPE &&
        trigger.key != VK_F12 && !(trigger.modifiers & ~MODIFIERS);
}

bool Trigger::ValidStopKey(UINT key) { return ValidKey(key) && !Modifier(key); }

std::shared_ptr<MacroOutput> WindowsMacroOutput() { return std::make_shared<WindowsOutput>(); }

namespace
{
void ValidateActions(const std::vector<MacroAction>& actions, std::size_t& remaining)
{
    using Type = MacroAction::Type;
    if (actions.size() > remaining) throw std::invalid_argument("a macro can contain up to 256 actions");
    remaining -= actions.size();
    for (const auto& action : actions)
    {
        if (action.duration.count() < 0 || action.duration > std::chrono::minutes(1))
            throw std::invalid_argument("action duration must be between 0 and 60000 ms");
        if (action.mode != MacroAction::Mode::Press && action.mode != MacroAction::Mode::Down && action.mode != MacroAction::Mode::Up)
            throw std::invalid_argument("invalid input mode");
        if (action.mode != MacroAction::Mode::Press &&
            ((action.type != MacroAction::Type::Key && action.type != MacroAction::Type::Mouse) || action.modifiers))
            throw std::invalid_argument("press and release events must use individual inputs");
        if (const auto& range = action.durationRange)
        {
            const bool press = action.mode == MacroAction::Mode::Press && (action.type == Type::Key || action.type == Type::Mouse);
            if (!press && action.type != Type::Text)
                throw std::invalid_argument("random durations require a text or press and release event");
            if (range->min.count() < 0 || range->max > std::chrono::minutes(1) || range->min > range->max)
                throw std::invalid_argument("duration bounds must be ordered and between 0 and 60000 ms");
        }
        if (action.positioned && action.type != MacroAction::Type::Mouse)
            throw std::invalid_argument("click positions are only supported for mouse events");
        const bool conditional = action.type == Type::IfElse || action.type == Type::While;
        if (conditional != action.condition.has_value() || (!conditional && !action.actions.empty()) ||
            (action.type != Type::IfElse && !action.elseActions.empty()))
            throw std::invalid_argument("invalid conditional block");
        switch (action.type)
        {
        case MacroAction::Type::Key:
            if (!ValidKey(action.key) || (action.modifiers & ~MODIFIERS))
                throw std::invalid_argument("invalid macro key");
            break;
        case MacroAction::Type::Mouse:
            if (action.button != 0 && action.button != 1)
                throw std::invalid_argument("choose the left or right mouse button");
            break;
        case MacroAction::Type::Text:
            if (action.text.empty() || action.text.size() > 65536)
                throw std::invalid_argument("text must contain 1 to 65536 characters");
            break;
        case MacroAction::Type::Wait:
            break;
        case MacroAction::Type::Move:
            if (action.imageId < 0) throw std::invalid_argument("choose an image to move to");
            break;
        case MacroAction::Type::WaitForImage:
            if (action.imageId <= 0) throw std::invalid_argument("choose an image to wait for");
            break;
        case Type::IfElse:
        case Type::While:
            if (action.duration.count()) throw std::invalid_argument("conditional blocks cannot have a duration");
            std::visit([](const auto& condition) { ValidateCondition(condition); }, *action.condition);
            ValidateActions(action.actions, remaining);
            ValidateActions(action.elseActions, remaining);
            break;
        default:
            throw std::invalid_argument("unsupported macro action");
        }
    }
}

bool ExecuteActions(const std::vector<MacroAction>& actions, HeldInputs& held, MacroOutput& output,
    const Macro::Wait& wait, const Macro::LoadImage& loadImage, const Macro::CaptureScreen& captureScreen)
{
    using namespace std::chrono_literals;
    using Type = MacroAction::Type;
    const auto evaluate = [&](const MacroCondition& condition) {
        return std::visit([&](const auto& value) { return EvaluateCondition(value, wait, loadImage, captureScreen); }, condition);
    };
    for (const auto& action : actions)
    {
        if (!wait(0ms)) return false;
        if (action.type == Type::IfElse || action.type == Type::While)
        {
            for (;;)
            {
                const auto result = evaluate(*action.condition);
                if (!result) return false;
                if (action.type == Type::IfElse || *result)
                {
                    const auto& branch = *result ? action.actions : action.elseActions;
                    if (!ExecuteActions(branch, held, output, wait, loadImage, captureScreen)) return false;
                }
                if (action.type == Type::IfElse || !*result) break;
                if (!wait(150ms)) return false;
            }
            continue;
        }
        if (action.type == MacroAction::Type::Mouse && action.positioned && !output.Move(action.position, wait)) return false;
        if (action.mode != MacroAction::Mode::Press)
        {
            const int type = action.type == MacroAction::Type::Mouse ? 1 : 0;
            const UINT key = type ? UINT(action.button) : action.key;
            if (action.mode == MacroAction::Mode::Down) held.Down(type, key);
            else held.Up(type, key);
            if (!wait(action.duration)) return false;
            continue;
        }
        auto duration = action.duration;
        if (const auto& range = action.durationRange)
            duration = std::chrono::milliseconds(RandomInRange(static_cast<int>(range->min.count()),
                static_cast<int>(range->max.count())));
        HeldInputs pressed(output, &held);
        if (action.type == MacroAction::Type::Key)
        {
            for (auto modifier : {std::make_pair(MOD_CONTROL, VK_CONTROL),
                                  std::make_pair(MOD_ALT, VK_MENU), std::make_pair(MOD_SHIFT, VK_SHIFT)})
                if (action.modifiers & modifier.first) pressed.Down(0, modifier.second);
            pressed.Down(0, action.key);
        }
        else if (action.type == MacroAction::Type::Mouse) pressed.Down(1, action.button);
        else if (action.type == MacroAction::Type::Text)
        {
            for (wchar_t character : action.text)
            {
                if (!wait(std::chrono::milliseconds(0))) return false;
                pressed.Down(2, character);
                pressed.Release(true);
            }
        }
        else if (action.type == MacroAction::Type::Move || action.type == MacroAction::Type::WaitForImage)
        {
            auto position = action.position;
            if (action.imageId)
            {
                const auto bounds = WaitForImage(action, wait, loadImage, captureScreen);
                if (!bounds || !wait(std::chrono::milliseconds(0))) return false;
                if (action.type == MacroAction::Type::Move) position = ImageLanding(*bounds);
            }
            if (action.type == MacroAction::Type::Move && !output.Move(position, wait)) return false;
        }
        const bool completed = wait(duration);
        pressed.Release(true);
        if (!completed) return false;
    }
    return true;
}
}

Macro::Macro(std::string name, std::vector<MacroAction> actions, Type type)
    : name(std::move(name)), actions(std::move(actions)), type(type)
{
    if (type != Type::NoRepeat && type != Type::Toggle) throw std::invalid_argument("invalid macro type");
    if (this->name.empty() || this->name.size() > 128 || this->actions.empty())
        throw std::invalid_argument("a macro needs a name and 1 to 256 actions");
    std::size_t remaining = 256;
    ValidateActions(this->actions, remaining);
}

bool Macro::Execute(MacroOutput& output, const Wait& wait, const LoadImage& loadImage,
    const CaptureScreen& captureScreen) const
{
    HeldInputs held(output);
    const bool completed = ExecuteActions(actions, held, output, wait, loadImage, captureScreen);
    held.Release(true);
    return completed;
}

MacroRecorder::MacroRecorder(Event event, std::function<void()> stopped)
    : event(std::move(event)), stopped(std::move(stopped)) {}

MacroRecorder::~MacroRecorder()
{
    stopped = {};
    event = {};
    Stop();
}

void MacroRecorder::Start(UINT stop, UINT record, Options settings, unsigned limit, bool native)
{
    if (active) throw std::invalid_argument("recording is already active");
    if (!Trigger::ValidStopKey(stop) || (record && (!Trigger::ValidStopKey(record) || stop == record)))
        throw std::invalid_argument("choose a valid stop hotkey and a different record hotkey if configured");
    if (limit < 2 || limit > 256) throw std::invalid_argument("leave room for at least two events to record");
    if (settings.clickPosition && !settings.mouseButtons)
        throw std::invalid_argument("enable mouse click recording before capturing click positions");
    if (!settings.keyboard && !settings.mouseButtons)
        throw std::invalid_argument("enable at least one input type to record");
    stopKey = stop; recordKey = record; options = settings; remaining = limit;
    held.clear(); pendingKey.reset(); hasTime = false;
    if (native)
    {
        if (currentRecorder) throw std::runtime_error("another recording is already active");
        currentRecorder = this;
        keyboard = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHook, GetModuleHandleW(nullptr), 0);
        mouse = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, GetModuleHandleW(nullptr), 0);
        if (!keyboard || !mouse)
        {
            if (keyboard) UnhookWindowsHookEx(keyboard);
            if (mouse) UnhookWindowsHookEx(mouse);
            keyboard = mouse = nullptr; currentRecorder = nullptr;
            throw std::runtime_error("keyboard and mouse recording could not be started");
        }
    }
    active = true;
}

void MacroRecorder::Stop(ULONGLONG time)
{
    if (!active) return;
    active = false;
    if (keyboard) UnhookWindowsHookEx(keyboard);
    if (mouse) UnhookWindowsHookEx(mouse);
    keyboard = mouse = nullptr;
    if (currentRecorder == this) currentRecorder = nullptr;
    if (pendingKey && !FinishKey(time)) FlushKey();
    if (event && !held.empty()) RecordDelay(time, unsigned(held.size()));
    for (auto input = held.rbegin(); input != held.rend(); ++input)
    {
        MacroAction action;
        action.type = input->first ? MacroAction::Type::Mouse : MacroAction::Type::Key;
        action.mode = MacroAction::Mode::Up;
        action.key = input->second; action.button = int(input->second);
        if (event) event(action);
    }
    held.clear();
    if (stopped) stopped();
}

bool MacroRecorder::Holding(bool mouse, UINT key) const
{ return (!mouse && pendingKey == key) || std::find(held.begin(), held.end(), std::make_pair(mouse, key)) != held.end(); }

void MacroRecorder::FlushKey()
{
    MacroAction action;
    action.type = MacroAction::Type::Key; action.mode = MacroAction::Mode::Down; action.key = *pendingKey;
    held.emplace_back(false, action.key);
    pendingKey.reset(); --remaining;
    if (event) event(action);
}

bool MacroRecorder::FinishKey(ULONGLONG time)
{
    const auto duration = options.delays && time > lastTime ? time - lastTime : 0;
    if (duration > 60000 && remaining >= held.size() + 2) return false;
    MacroAction action;
    action.type = MacroAction::Type::Key; action.key = *pendingKey;
    action.duration = std::chrono::milliseconds(std::min<ULONGLONG>(duration, 60000));
    pendingKey.reset(); --remaining; lastTime = time;
    if (event) event(action);
    return true;
}

void MacroRecorder::RecordDelay(ULONGLONG time, unsigned reserved)
{
    auto delay = hasTime && time > lastTime ? time - lastTime : 0;
    while (options.delays && delay && remaining > reserved)
    {
        MacroAction pause;
        const auto chunk = std::min<ULONGLONG>(delay, 60000);
        pause.duration = std::chrono::milliseconds(chunk);
        event(pause); --remaining; delay -= chunk;
    }
}

void MacroRecorder::Input(bool isMouse, UINT key, bool down, ULONGLONG time, bool injected, std::optional<Coordinates> position)
{
    if (!active || injected) return;
    if (!isMouse && key == stopKey) { if (down) Stop(time); return; }
    if (!isMouse && key == recordKey) return;
    if ((isMouse && !options.mouseButtons) || (!isMouse && !options.keyboard)) return;
    if ((isMouse && key > 1) || (!isMouse && !ValidKey(key))) return;
    const auto input = std::make_pair(isMouse, key);
    if (down == Holding(isMouse, key)) return;
    if (pendingKey)
    {
        if (!isMouse && key == *pendingKey && !down && FinishKey(time))
        {
            if (remaining <= held.size()) Stop();
            return;
        }
        if (remaining < held.size() + 2) { Stop(time); return; }
        FlushKey();
    }
    const bool combine = !isMouse && down && !options.splitKeyEvents;
    const unsigned reserved = unsigned(held.size()) + (down ? (combine ? 1 : 2) : 0);
    if (remaining < reserved) { Stop(time); return; }
    RecordDelay(time, reserved);
    if (combine) { pendingKey = key; lastTime = time; hasTime = true; return; }
    MacroAction action;
    action.type = isMouse ? MacroAction::Type::Mouse : MacroAction::Type::Key;
    action.mode = down ? MacroAction::Mode::Down : MacroAction::Mode::Up;
    action.key = key; action.button = int(key);
    if (isMouse && options.clickPosition)
    {
        if (!position)
        {
            POINT cursor{};
            if (!GetCursorPos(&cursor)) throw std::runtime_error("the mouse position could not be recorded");
            position = Coordinates(cursor.x, cursor.y);
        }
        action.positioned = true; action.position = *position;
    }
    if (down) held.push_back(input); else held.erase(std::find(held.begin(), held.end(), input));
    --remaining;
    lastTime = time; hasTime = true;
    event(action);
    if (remaining <= held.size()) Stop();
}

void MacroRecorder::MouseInput(UINT message, Coordinates position, ULONGLONG time, ULONG_PTR extraInfo)
{
    if (message != WM_LBUTTONDOWN && message != WM_LBUTTONUP && message != WM_RBUTTONDOWN && message != WM_RBUTTONUP) return;
    const bool right = message == WM_RBUTTONDOWN || message == WM_RBUTTONUP;
    const bool down = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN;
    Input(true, right ? 1 : 0, down, time, extraInfo == INPUT_TAG, position);
}

LRESULT MacroRecorder::KeyboardInput(UINT key, bool down, ULONGLONG time, ULONG_PTR extraInfo,
    const std::function<LRESULT()>& nextHook)
{
    const bool fromTea = extraInfo == INPUT_TAG;
    const bool stopping = active && !fromTea && down && key == stopKey;
    // the listener checks the active recording before suppressing the stop key
    const LRESULT result = stopping ? nextHook() : 0;
    try { Input(false, key, down, time, fromTea); }
    catch (...) { if (active) Stop(); }
    return stopping ? result : nextHook();
}

LRESULT CALLBACK MacroRecorder::KeyboardHook(int code, WPARAM message, LPARAM data)
{
    if (code == HC_ACTION && currentRecorder)
    {
        const auto& input = *reinterpret_cast<KBDLLHOOKSTRUCT*>(data);
        UINT key = input.vkCode;
        if (key == VK_RETURN && (input.flags & LLKHF_EXTENDED)) key |= 0x100;
        return currentRecorder->KeyboardInput(key, message == WM_KEYDOWN || message == WM_SYSKEYDOWN,
            GetTickCount64(), input.dwExtraInfo, [=] { return CallNextHookEx(nullptr, code, message, data); });
    }
    return CallNextHookEx(nullptr, code, message, data);
}

LRESULT CALLBACK MacroRecorder::MouseHook(int code, WPARAM message, LPARAM data)
{
    if (code == HC_ACTION && currentRecorder)
    {
        const auto& input = *reinterpret_cast<MSLLHOOKSTRUCT*>(data);
        try { currentRecorder->MouseInput(UINT(message), {input.pt.x, input.pt.y}, GetTickCount64(), input.dwExtraInfo); }
        catch (...) { if (currentRecorder) currentRecorder->Stop(); }
    }
    return CallNextHookEx(nullptr, code, message, data);
}

void Binder::Bind(Binding binding)
{
    CheckBinding(binding);
    std::lock_guard<std::mutex> lock(mutex);
    if (binding.trigger.key == stopKey) throw std::invalid_argument("this key is reserved as the stop hotkey");
    if (binding.trigger.key == recordKey) throw std::invalid_argument("this key is reserved as the record hotkey");
    bindings.insert_or_assign(binding.trigger, std::move(binding));
}

bool Binder::Unbind(Trigger trigger)
{
    std::lock_guard<std::mutex> lock(mutex);
    return bindings.erase(trigger) != 0;
}

void Binder::Replace(std::vector<Binding> values, std::optional<UINT> stop, std::optional<UINT> record)
{
    std::map<Trigger, Binding> next;
    for (auto& value : values)
    {
        CheckBinding(value);
        if (!next.emplace(value.trigger, std::move(value)).second)
            throw std::invalid_argument("this shortcut is already assigned");
    }
    std::lock_guard<std::mutex> lock(mutex);
    const UINT key = stop.value_or(stopKey);
    const UINT recording = record.value_or(recordKey);
    if (key && !Trigger::ValidStopKey(key)) throw std::invalid_argument("choose a supported stop hotkey");
    if (recording && (!Trigger::ValidStopKey(recording) || recording == key))
        throw std::invalid_argument("choose a record hotkey different from the stop hotkey");
    for (const auto& value : next)
    {
        if (value.first.key == key) throw std::invalid_argument("the stop hotkey cannot also trigger a macro");
        if (value.first.key == recording) throw std::invalid_argument("the record hotkey cannot also trigger a macro");
    }
    bindings.swap(next);
    stopKey = key;
    recordKey = recording;
}

UINT Binder::StopKey() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return stopKey;
}

UINT Binder::RecordKey() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return recordKey;
}

std::optional<Binding> Binder::Find(Trigger trigger) const
{
    std::lock_guard<std::mutex> lock(mutex);
    const auto binding = bindings.find(trigger);
    return binding == bindings.end() ? std::nullopt : std::optional<Binding>(binding->second);
}

void Binder::BeginCapture(Capture callback, bool stopKey)
{
    if (!callback) throw std::invalid_argument("a capture callback is required");
    std::lock_guard<std::mutex> lock(mutex);
    capture = std::move(callback);
    captureStopKey = stopKey;
}

void Binder::CancelCapture()
{
    Capture callback;
    { std::lock_guard<std::mutex> lock(mutex); callback = std::move(capture); }
    if (callback) callback(std::nullopt);
}

bool Binder::Capturing() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return static_cast<bool>(capture);
}

bool Binder::CaptureKey(Trigger trigger)
{
    Capture callback;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (captureStopKey)
        {
            if (!Trigger::ValidStopKey(trigger.key)) return false;
            trigger.modifiers = 0;
        }
        else if (!Trigger::Valid(trigger)) return false;
        callback = std::move(capture);
    }
    if (!callback) return false;
    callback(trigger);
    return true;
}

MacroListener::MacroListener(Binder& binder, std::shared_ptr<MacroOutput> output, Error error, Playback playback,
    std::function<bool()> recordHotkey, Macro::LoadImage loadImage, std::function<bool()> stopHotkey,
    std::function<bool(Trigger)> scopedHotkey)
    : binder(binder), output(std::move(output)), error(std::move(error)), playback(std::move(playback)),
    recordHotkey(std::move(recordHotkey)), loadImage(std::move(loadImage)), stopHotkey(std::move(stopHotkey)),
    scopedHotkey(std::move(scopedHotkey))
{
    if (!this->output) throw std::invalid_argument("macro output is required");
}

MacroListener::~MacroListener() { Stop(); }

void MacroListener::Start(bool native)
{
    { std::lock_guard<std::mutex> lock(mutex); if (!stopping) return; stopping = false; cancelled = false; }
    pressed.fill(false);
    suppressed.fill(false);
    worker = std::thread(&MacroListener::Work, this);
    if (!native) return;
    std::promise<bool> ready;
    auto result = ready.get_future();
    hookThread = std::thread([this, ready = std::move(ready)]() mutable {
        currentListener = this;
        hookThreadId = GetCurrentThreadId();
        MSG message{};
        PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        for (UINT key = 0; key < physical.size(); ++key)
            physical[key] = key != VK_CONTROL && key != VK_SHIFT && key != VK_MENU &&
                (GetAsyncKeyState(key) & 0x8000) != 0;
        const HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHook, GetModuleHandleW(nullptr), 0);
        ready.set_value(hook != nullptr);
        if (hook)
        {
            while (GetMessageW(&message, nullptr, 0, 0) > 0)
            { TranslateMessage(&message); DispatchMessageW(&message); }
            UnhookWindowsHookEx(hook);
        }
        currentListener = nullptr;
        hookThreadId = 0;
    });
    if (!result.get()) { Stop(); throw std::runtime_error("the keyboard listener could not start"); }
}

bool MacroListener::Cancel()
{
    bool hadPlayback;
    {
        std::lock_guard<std::mutex> lock(mutex);
        hadPlayback = CancelLocked();
    }
    changed.notify_all();
    return hadPlayback;
}

bool MacroListener::CancelLocked()
{
    ++playbackGeneration;
    const bool hadPlayback = (busy && !cancelled) || !queue.empty();
    cancelled = true;
    paused = false;
    queue.clear();
    toggles.clear();
    if (!busy && hadPlayback) NotifyPlayback(false, false);
    return hadPlayback;
}

void MacroListener::Pause(bool value)
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!busy || cancelled || stopping) return;
        paused = value;
        NotifyPlayback(true, paused);
    }
    changed.notify_all();
}

void MacroListener::SetEnabled(bool value)
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (value && !enabled) ++playbackGeneration;
        enabled = value;
        if (!enabled) CancelLocked();
    }
    changed.notify_all();
}

void MacroListener::IgnoreWindow(HWND window)
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        ignoredWindow = window;
    }
    changed.notify_all();
}

bool MacroListener::ForegroundAllowed() const
{
    return !ignoredWindow || !output->IsForeground(ignoredWindow);
}

void MacroListener::ReplaceBindings(std::vector<Binding> bindings, UINT stopKey, UINT recordKey)
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        binder.Replace(std::move(bindings), stopKey, recordKey);
        CancelLocked();
    }
    changed.notify_all();
}

void MacroListener::BeginCapture(Binder::Capture callback, bool stopKey)
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        binder.BeginCapture(std::move(callback), stopKey);
        CancelLocked();
    }
    changed.notify_all();
}

void MacroListener::Stop()
{
    { std::lock_guard<std::mutex> lock(mutex); stopping = true; CancelLocked(); }
    changed.notify_all();
    if (const DWORD id = hookThreadId.load()) PostThreadMessageW(id, WM_QUIT, 0, 0);
    if (hookThread.joinable()) hookThread.join();
    if (worker.joinable()) worker.join();
    try { binder.CancelCapture(); } catch (...) { Report("key capture could not be closed"); }
}

bool MacroListener::Wait(std::chrono::milliseconds duration)
{
    using Clock = std::chrono::steady_clock;
    constexpr auto focusPoll = std::chrono::milliseconds(50);
    std::unique_lock<std::mutex> lock(mutex);
    auto remaining = Clock::duration(duration);
    for (;;)
    {
        if (stopping || cancelled || (activeCancellation && *activeCancellation)) return false;
        if (!ForegroundAllowed())
        {
            CancelLocked();
            return false;
        }
        if (paused)
        {
            if (ignoredWindow) changed.wait_for(lock, focusPoll);
            else changed.wait(lock);
            continue;
        }
        if (remaining <= remaining.zero()) return true;
        const auto start = Clock::now();
        changed.wait_for(lock, ignoredWindow ? std::min(remaining, Clock::duration(focusPoll)) : remaining);
        remaining -= Clock::now() - start;
    }
}

bool MacroListener::WaitIdle(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, timeout, [this] { return !busy && queue.empty(); });
}

void MacroListener::Report(const std::string& message) noexcept
{
    try { if (error) error(message); } catch (...) {}
}

void MacroListener::NotifyPlayback(bool active, bool paused) noexcept
{
    try { if (playback) playback(active, paused); } catch (...) {}
}

void MacroListener::Work()
{
    for (;;)
    {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [this] { return stopping || !queue.empty(); });
            if (stopping) break;
            job = queue.front();
            queue.pop_front();
            activeCancellation = job.cancelled;
            cancelled = false;
            busy = true;
            paused = false;
            NotifyPlayback(true, false);
        }
        bool completed = false;
        try
        {
            bool ready = true;
            while (ready && (output->IsDown(job.binding.trigger.key) || output->IsDown(VK_CONTROL) ||
                   output->IsDown(VK_MENU) || output->IsDown(VK_SHIFT) ||
                   output->IsDown(VK_LWIN) || output->IsDown(VK_RWIN)))
                ready = Wait(std::chrono::milliseconds(10));
            if (ready)
            {
                completed = job.binding.macro->Execute(*output, [this](auto duration) { return Wait(duration); }, loadImage);
                if (completed && job.cancelled) completed = Wait(std::chrono::milliseconds(1));
            }
        }
        catch (const std::exception& exception) { Report(exception.what()); }
        catch (...) { Report("the macro could not finish"); }
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (job.cancelled)
            {
                const auto found = toggles.find(job.binding.trigger);
                if (found != toggles.end() && found->second == job.cancelled)
                {
                    if (completed && !*job.cancelled && !cancelled && !stopping) queue.push_back(job);
                    else toggles.erase(found);
                }
            }
            activeCancellation.reset();
            busy = false; paused = false;
            NotifyPlayback(!queue.empty(), false);
        }
        changed.notify_all();
    }
    { std::lock_guard<std::mutex> lock(mutex); busy = false; paused = false; NotifyPlayback(false, false); }
    changed.notify_all();
}

bool MacroListener::ProcessEvent(Trigger trigger, bool down, bool injected, bool allow)
{
    return !injected && ProcessKey(trigger, down, allow, false);
}

bool MacroListener::ProcessKey(Trigger trigger, bool down, bool allow, bool recordOnly)
{
    if (!ValidKey(trigger.key)) return false;
    const auto generation = playbackGeneration.load();
    std::lock_guard<std::mutex> eventLock(eventMutex);
    const UINT key = trigger.key;
    if (!down)
    {
        pressed[key] = false;
        const bool consume = suppressed[key];
        suppressed[key] = false;
        return consume;
    }
    if (pressed[key]) return suppressed[key];
    pressed[key] = true;
    if (!recordOnly && binder.Capturing())
    {
        const bool captured = binder.CaptureKey(trigger);
        if (!captured && key == VK_ESCAPE) binder.CancelCapture();
        return suppressed[key] = captured || key == VK_ESCAPE;
    }
    if (!recordOnly && scopedHotkey && scopedHotkey(trigger)) return suppressed[key] = true;
    std::unique_lock<std::mutex> lock(mutex);
    if (stopping || binder.Capturing()) return false;
    const UINT stop = binder.StopKey();
    if (!recordOnly && stop && key == stop)
    {
        const bool stopped = CancelLocked();
        lock.unlock();
        changed.notify_all();
        try { if (stopHotkey && stopHotkey()) return suppressed[key] = true; }
        catch (...) { Report("the stop hotkey could not be processed"); }
        return suppressed[key] = stopped;
    }
    if (generation != playbackGeneration) return false;
    if (key == binder.RecordKey())
    {
        lock.unlock();
        try { if (!trigger.modifiers && recordHotkey && recordHotkey()) return suppressed[key] = true; }
        catch (...) { Report("the record hotkey could not be processed"); }
        return false;
    }
    if (recordOnly || !stop || !enabled || !allow || !ForegroundAllowed()) return false;
    const auto binding = binder.Find(trigger);
    if (!binding) return false;
    const auto found = toggles.find(trigger);
    if (found != toggles.end())
    {
        const auto flag = found->second;
        *flag = true;
        toggles.erase(found);
        queue.erase(std::remove_if(queue.begin(), queue.end(), [&](const Job& job) { return job.cancelled == flag; }), queue.end());
        if (!busy) NotifyPlayback(!queue.empty(), false);
    }
    else
    {
        if (queue.size() >= 32) return false;
        std::shared_ptr<bool> flag;
        if (binding->macro->PlaybackType() == Macro::Type::Toggle)
        {
            flag = std::make_shared<bool>(false);
            toggles.emplace(trigger, flag);
        }
        queue.push_back({*binding, flag});
    }
    lock.unlock();
    changed.notify_all();
    return suppressed[key] = binding->replace;
}

LRESULT CALLBACK MacroListener::KeyboardHook(int code, WPARAM message, LPARAM data)
{
    auto* listener = currentListener;
    if (code == HC_ACTION && listener)
    {
        const auto& input = *reinterpret_cast<KBDLLHOOKSTRUCT*>(data);
        const bool recordControl = input.vkCode == listener->binder.RecordKey() && input.dwExtraInfo != INPUT_TAG;
        if ((!(input.flags & LLKHF_INJECTED) || recordControl) && input.vkCode < 256)
        {
            const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
            listener->physical[input.vkCode] = down;
            UINT modifiers = 0;
            if (listener->physical[VK_LCONTROL] || listener->physical[VK_RCONTROL] || listener->physical[VK_CONTROL]) modifiers |= MOD_CONTROL;
            if (listener->physical[VK_LMENU] || listener->physical[VK_RMENU] || listener->physical[VK_MENU]) modifiers |= MOD_ALT;
            if (listener->physical[VK_LSHIFT] || listener->physical[VK_RSHIFT] || listener->physical[VK_SHIFT]) modifiers |= MOD_SHIFT;
            if (listener->physical[VK_LWIN] || listener->physical[VK_RWIN]) modifiers |= MOD_WIN;
            UINT key = input.vkCode;
            if (key == VK_RETURN && (input.flags & LLKHF_EXTENDED)) key |= 0x100;
            const bool injected = (input.flags & LLKHF_INJECTED) != 0;
            try { if (listener->ProcessKey({key, modifiers}, down, true, injected)) return 1; }
            catch (...) { listener->Report("keyboard input could not be processed"); }
        }
    }
    return CallNextHookEx(nullptr, code, message, data);
}
