#include <gtest/gtest.h>
#include <shlobj.h>
#include <WebView2.h>
#include <json.hpp>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include "macros.hpp"

namespace
{
struct WrapperCalls
{
    std::vector<std::chrono::milliseconds> sleeps;
    std::wstring commandLine, localFolder, mutexName;
    std::vector<std::wstring> dialogs;
    HANDLE mutex = nullptr;
    HRESULT comResult = S_OK;
    bool mutexFails = false, alreadyRunning = false, folderFails = false;
    bool dpiSucceeds = true;
    int initialized = 0, uninitialized = 0, released = 0, closed = 0;
    int registered = 0, windows = 0, dpi = 0, dpiFallback = 0, moves = 0;
    int moveFailures = 0;
    DWORD moveFlags = 0, moveError = ERROR_DISK_FULL;
};

WrapperCalls calls;

template<class Rep, class Period>
std::chrono::duration<Rep, Period> RecordSleep(std::chrono::duration<Rep, Period> duration)
{
    calls.sleeps.push_back(std::chrono::duration_cast<std::chrono::milliseconds>(duration));
    return duration.zero();
}

LPWSTR WINAPI RecordCommandLine() { return calls.commandLine.data(); }

HANDLE WINAPI RecordMutex(LPSECURITY_ATTRIBUTES attributes, BOOL owner, LPCWSTR name)
{
    calls.mutexName = name;
    calls.mutex = calls.mutexFails ? nullptr : CreateMutexW(attributes, owner, nullptr);
    SetLastError(calls.alreadyRunning ? ERROR_ALREADY_EXISTS : ERROR_SUCCESS);
    return calls.mutex;
}

BOOL WINAPI RecordRelease(HANDLE handle)
{
    EXPECT_EQ(handle, calls.mutex);
    ++calls.released;
    return ReleaseMutex(handle);
}

BOOL WINAPI RecordClose(HANDLE handle)
{
    EXPECT_EQ(handle, calls.mutex);
    ++calls.closed;
    return CloseHandle(handle);
}

HRESULT WINAPI RecordInitialize(LPVOID, DWORD mode)
{
    EXPECT_EQ(mode, DWORD(COINIT_APARTMENTTHREADED));
    ++calls.initialized;
    return calls.comResult;
}

void WINAPI RecordUninitialize() { ++calls.uninitialized; }

HRESULT WINAPI RecordFolder(REFKNOWNFOLDERID folder, DWORD, HANDLE, PWSTR* path)
{
    EXPECT_EQ(folder, FOLDERID_LocalAppData);
    *path = nullptr;
    if (calls.folderFails) return E_FAIL;
    *path = static_cast<PWSTR>(CoTaskMemAlloc((calls.localFolder.size() + 1) * sizeof(wchar_t)));
    if (!*path) return E_OUTOFMEMORY;
    std::copy(calls.localFolder.c_str(), calls.localFolder.c_str() + calls.localFolder.size() + 1, *path);
    return S_OK;
}

int WINAPI RecordDialog(HWND, LPCWSTR message, LPCWSTR, UINT)
{
    calls.dialogs.emplace_back(message);
    return IDOK;
}

BOOL WINAPI RecordDpi(DPI_AWARENESS_CONTEXT) { ++calls.dpi; return calls.dpiSucceeds; }
BOOL WINAPI RecordDpiFallback() { ++calls.dpiFallback; return TRUE; }
ATOM WINAPI RecordRegister(const WNDCLASSEXW*) { ++calls.registered; return 1; }

HWND WINAPI RecordWindow(LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID)
{
    ++calls.windows;
    return nullptr;
}

BOOL WINAPI RecordMove(LPCWSTR source, LPCWSTR destination, DWORD flags)
{
    if (std::filesystem::path(destination).filename() != L"macros.json") return MoveFileExW(source, destination, flags);
    ++calls.moves;
    calls.moveFlags = flags;
    if (calls.moves > calls.moveFailures) return MoveFileExW(source, destination, flags);
    SetLastError(calls.moveError);
    return FALSE;
}
}

#define sleep_for(duration) sleep_for(RecordSleep(duration))
#define GetCommandLineW RecordCommandLine
#define CreateMutexW RecordMutex
#define ReleaseMutex RecordRelease
#define CloseHandle RecordClose
#define CoInitializeEx RecordInitialize
#define CoUninitialize RecordUninitialize
#define SHGetKnownFolderPath RecordFolder
#define MessageBoxW RecordDialog
#define SetProcessDpiAwarenessContext RecordDpi
#define SetProcessDPIAware RecordDpiFallback
#define RegisterClassExW RecordRegister
#undef CreateWindowW
#define CreateWindowW RecordWindow
#define MoveFileExW RecordMove
#include "tea_wraps.cpp"
#undef MoveFileExW
#undef CreateWindowW
#undef RegisterClassExW
#undef SetProcessDPIAware
#undef SetProcessDpiAwarenessContext
#undef MessageBoxW
#undef SHGetKnownFolderPath
#undef CoUninitialize
#undef CoInitializeEx
#undef CloseHandle
#undef ReleaseMutex
#undef CreateMutexW
#undef GetCommandLineW
#undef sleep_for

namespace
{
constexpr int LOWEST_INT = (std::numeric_limits<int>::min)();
constexpr int HIGHEST_INT = (std::numeric_limits<int>::max)();

TEST(CoordinatesTests, DefaultConstructorStartsAtOrigin)
{
    const Coordinates coordinates;
    EXPECT_EQ(coordinates.pos_x, 0);
    EXPECT_EQ(coordinates.pos_y, 0);
}

TEST(CoordinatesTests, SingleArgumentDefaultsYToZero)
{
    for (int x : {LOWEST_INT, -1, 0, 1, HIGHEST_INT})
    {
        SCOPED_TRACE(x);
        const Coordinates coordinates(x);
        EXPECT_EQ(coordinates.pos_x, x);
        EXPECT_EQ(coordinates.pos_y, 0);
    }
}

TEST(CoordinatesTests, ConstructorPreservesBothAxesIncludingIntegerLimits)
{
    for (int x : {LOWEST_INT, -42, 0, 42, HIGHEST_INT})
    {
        for (int y : {LOWEST_INT, -42, 0, 42, HIGHEST_INT})
        {
            const Coordinates coordinates(x, y);
            EXPECT_EQ(coordinates.pos_x, x);
            EXPECT_EQ(coordinates.pos_y, y);
        }
    }
}

TEST(CoordinatesTests, EqualityRequiresBothAxesToMatchAndIsSymmetric)
{
    const Coordinates values[] = {
        {}, {1, 0}, {0, 1}, {1, 1}, {-1, 1}, {1, -1}, {-1, -1},
        {LOWEST_INT, HIGHEST_INT}, {HIGHEST_INT, LOWEST_INT}
    };

    for (const auto& left : values)
    {
        EXPECT_TRUE(left == left);
        for (const auto& right : values)
        {
            SCOPED_TRACE(::testing::Message() << left << " versus " << right);
            const bool expected = left.pos_x == right.pos_x && left.pos_y == right.pos_y;
            EXPECT_EQ(left == right, expected);
            EXPECT_EQ(right == left, expected);
        }
    }
}

TEST(CoordinatesTests, StreamFormatsZeroSignedValuesAndIntegerLimits)
{
    struct Case
    {
        Coordinates value;
        std::string expected;
    };
    const Case cases[] = {
        {{}, "(X: 0, Y: 0)"},
        {{12, 34}, "(X: 12, Y: 34)"},
        {{-12, -34}, "(X: -12, Y: -34)"},
        {{-12, 34}, "(X: -12, Y: 34)"},
        {{LOWEST_INT, HIGHEST_INT},
            "(X: " + std::to_string(LOWEST_INT) + ", Y: " + std::to_string(HIGHEST_INT) + ")"}
    };

    for (const auto& test : cases)
    {
        std::ostringstream out;
        EXPECT_EQ(&(out << test.value), &out);
        EXPECT_EQ(out.str(), test.expected);
    }
}

TEST(CoordinatesTests, StreamSupportsChainingAndExistingFormatting)
{
    std::ostringstream out;
    out << "Position: " << std::hex << Coordinates(16, 31) << ' ' << Coordinates();
    EXPECT_EQ(out.str(), "Position: (X: 10, Y: 1f) (X: 0, Y: 0)");
}

TEST(CoordinatesTests, StreamPreservesFailureState)
{
    std::ostringstream out;
    out.setstate(std::ios::failbit);

    EXPECT_EQ(&(out << Coordinates(1, 2)), &out);
    EXPECT_TRUE(out.fail());
    EXPECT_TRUE(out.str().empty());
}

struct SleepCase
{
    float seconds;
    long long milliseconds;
};

void PrintTo(const SleepCase& sleep, std::ostream* out)
{
    *out << std::setprecision(std::numeric_limits<float>::max_digits10) << sleep.seconds << " seconds";
}

class SleepThreadTests : public ::testing::TestWithParam<SleepCase> {};

TEST_P(SleepThreadTests, RequestsTheDurationTruncatedToWholeMilliseconds)
{
    const auto test = GetParam();
    calls.sleeps.clear();
    SleepThread(test.seconds);
    std::chrono::milliseconds requested{0};
    for (auto duration : calls.sleeps)
        if (duration.count() > 0) requested += duration;
    EXPECT_EQ(requested.count(), test.milliseconds);
}

INSTANTIATE_TEST_SUITE_P(Durations, SleepThreadTests, ::testing::Values(
    SleepCase{0.0f, 0}, SleepCase{-0.0f, 0}, SleepCase{-0.001f, 0},
    SleepCase{-0.25f, 0}, SleepCase{-1.0f, 0}, SleepCase{-1.125f, 0},
    SleepCase{0.0009f, 0}, SleepCase{std::nextafter(0.001f, 0.0f), 0},
    SleepCase{0.001f, 1}, SleepCase{0.0019f, 1}, SleepCase{0.1259f, 125},
    SleepCase{std::nextafter(1.0f, 0.0f), 999}, SleepCase{1.0f, 1000},
    SleepCase{1.0009f, 1000}, SleepCase{1.125f, 1125}, SleepCase{1.2349f, 1234},
    SleepCase{1.9999f, 1999}, SleepCase{2147483520.0f, 2147483520000LL}));

TEST(EncodingTests, RoundTripsEmptyAsciiUnicodeAndEmbeddedNulls)
{
    const std::pair<std::string, std::wstring> cases[] = {
        {"", L""}, {"tea", L"tea"}, {std::string("a\0b", 3), std::wstring(L"a\0b", 3)},
        {u8"tea \u00e9 \u6c34 \U0001f375", L"tea \u00e9 \u6c34 \U0001f375"}
    };
    for (const auto& [utf8, wide] : cases)
    {
        EXPECT_EQ(Wide(utf8), wide);
        EXPECT_EQ(Utf8(wide), utf8);
    }
}

TEST(EncodingTests, RejectsMalformedTruncatedAndOutOfRangeUtf8)
{
    for (const auto* text : {"\xff", "\xc0\xaf", "\xe2\x82", "\xed\xa0\x80", "\xf4\x90\x80\x80"})
        EXPECT_THROW(Wide(text), std::invalid_argument);
}

TEST(EncodingTests, Base64HandlesEveryPaddingLengthAndBinaryBytes)
{
    const std::pair<std::string, std::string> cases[] = {
        {"", ""}, {"f", "Zg=="}, {"fo", "Zm8="}, {"foo", "Zm9v"},
        {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"},
        {std::string("\0\xff\x80", 3), "AP+A"}
    };
    for (const auto& [bytes, expected] : cases) EXPECT_EQ(Base64(bytes), expected);
}

TEST(ResourceTests, ReportsMissingResources)
{
    EXPECT_THROW(Resource(65535), std::runtime_error);
}

using ScriptCallback = Callback<ICoreWebView2ExecuteScriptCompletedHandler, HRESULT, LPCWSTR>;
constexpr auto SCRIPT_CALLBACK_ID = L"49511172-cc67-4bca-9923-137112f4c4cc";

TEST(CallbackTests, ValidatesInterfaceIdsAndBalancesQueryReferences)
{
    EXPECT_THROW(new ScriptCallback(L"invalid", [](HRESULT, LPCWSTR) { return S_OK; }), std::logic_error);
    auto release = [](ScriptCallback* value) { value->Release(); };
    std::unique_ptr<ScriptCallback, decltype(release)> callback(
        new ScriptCallback(SCRIPT_CALLBACK_ID, [](HRESULT, LPCWSTR) { return S_OK; }), release);
    EXPECT_EQ(callback->AddRef(), 2u);
    EXPECT_EQ(callback->Release(), 1u);
    EXPECT_EQ(callback->QueryInterface(IID_IUnknown, nullptr), E_POINTER);
    void* value = callback.get();
    EXPECT_EQ(callback->QueryInterface(IID_IDispatch, &value), E_NOINTERFACE);
    EXPECT_EQ(value, nullptr);
    IID script{};
    ASSERT_EQ(IIDFromString((std::wstring(L"{") + SCRIPT_CALLBACK_ID + L"}").c_str(), &script), S_OK);
    for (const auto& id : {IID_IUnknown, script})
    {
        ASSERT_EQ(callback->QueryInterface(id, &value), S_OK);
        EXPECT_EQ(value, static_cast<ICoreWebView2ExecuteScriptCompletedHandler*>(callback.get()));
        EXPECT_EQ(static_cast<ICoreWebView2ExecuteScriptCompletedHandler*>(value)->Release(), 1u);
    }
}

TEST(CallbackTests, ForwardsArgumentsAndContainsExceptionsAtTheComBoundary)
{
    auto release = [](ScriptCallback* value) { value->Release(); };
    bool called = false;
    std::unique_ptr<ScriptCallback, decltype(release)> callback(
        new ScriptCallback(SCRIPT_CALLBACK_ID, [&](HRESULT result, LPCWSTR text) {
            called = true;
            EXPECT_EQ(result, S_FALSE);
            EXPECT_STREQ(text, L"result");
            return E_INVALIDARG;
        }), release);
    EXPECT_EQ(callback->Invoke(S_FALSE, L"result"), E_INVALIDARG);
    EXPECT_TRUE(called);
    callback.reset(new ScriptCallback(SCRIPT_CALLBACK_ID, [](HRESULT, LPCWSTR) -> HRESULT {
        throw std::runtime_error("callback failed");
    }));
    EXPECT_EQ(callback->Invoke(S_OK, nullptr), E_FAIL);
}

TEST(ConfigurationTests, IntegerAcceptsLimitsAndDefaultsOnlyForMissingValues)
{
    EXPECT_EQ(Integer(Json::object(), "value", 42, LOWEST_INT, HIGHEST_INT), 42);
    for (int value : {LOWEST_INT, -1, 0, 1, HIGHEST_INT})
        EXPECT_EQ(Integer({{"value", value}}, "value", 42, LOWEST_INT, HIGHEST_INT), value);
    for (const Json& value : {Json(nullptr), Json(true), Json("1"), Json(1.5),
                             Json(-2147483649LL), Json(2147483648ULL)})
        EXPECT_THROW(Integer({{"value", value}}, "value", 42, LOWEST_INT, HIGHEST_INT), std::invalid_argument);
}

Json Configuration()
{
    return {{"version", 1}, {"enabled", true}, {"stopKey", VK_ESCAPE}, {"layout", "75%"},
        {"bindingOptions", {{"modifiers", 3}, {"replace", true}}},
        {"macros", {{{"id", 1}, {"name", "sample"}, {"actions", {
            {{"type", "key"}, {"key", 'C'}, {"modifiers", 3}, {"duration", 125}},
            {{"type", "mouse"}, {"button", 1}},
            {{"type", "text"}, {"text", u8"Hello, \u6c34!"}},
            {{"type", "wait"}, {"duration", 60000}},
            {{"type", "move"}, {"x", LOWEST_INT}, {"y", HIGHEST_INT}}
        }}}}},
        {"bindings", {
            {{"key", "F1"}, {"code", VK_F1}, {"modifiers", 0}, {"macroId", 1}, {"replace", false}},
            {{"key", "A"}, {"code", 'A'}, {"modifiers", 7}, {"macroId", 1}, {"replace", true}}
        }}};
}

TEST(ConfigurationTests, ReadsNestedConditionsAndRejectsInvalidBranches)
{
    auto state = Configuration();
    const Json condition{{"type", "image-on-screen"}, {"imageId", 7}};
    const Json delay{{"type", "wait"}, {"duration", 50}};
    const Json loop{{"type", "while"}, {"condition", {{"type", "image-on-screen"}, {"imageId", 8}}},
        {"actions", Json::array({delay})}};
    auto& actions = state["macros"][0]["actions"];
    actions = Json::array({{{"type", "if-else"}, {"condition", condition},
        {"actions", Json::array()}, {"elseActions", Json::array({loop})}}});

    const auto bindings = ReadBindings(state);
    const auto& branch = bindings.front().macro->Actions().front();
    EXPECT_EQ(branch.type, MacroAction::Type::IfElse);
    ASSERT_TRUE(branch.condition);
    EXPECT_EQ(std::get<ImageOnScreen>(*branch.condition).imageId, 7);
    EXPECT_TRUE(branch.actions.empty());
    ASSERT_EQ(branch.elseActions.size(), 1u);
    const auto& nested = branch.elseActions.front();
    EXPECT_EQ(nested.type, MacroAction::Type::While);
    ASSERT_EQ(nested.actions.size(), 1u);
    EXPECT_EQ(nested.actions.front().duration.count(), 50);
    EXPECT_TRUE(ReferencesImage(actions, 7));
    EXPECT_TRUE(ReferencesImage(actions, 8));
    EXPECT_FALSE(ReferencesImage(actions, 9));

    auto& nestedJson = actions[0]["elseActions"][0];
    nestedJson["condition"]["type"] = "wait-for-image";
    EXPECT_THROW(ReadBindings(state), std::invalid_argument);
    nestedJson["condition"] = condition;
    nestedJson["actions"] = Json::object();
    EXPECT_THROW(ReadBindings(state), std::invalid_argument);
    nestedJson["actions"] = Json::array({delay});
    nestedJson["elseActions"] = Json::array();
    EXPECT_THROW(ReadBindings(state), std::invalid_argument);
    nestedJson.erase("elseActions");
    nestedJson["actions"] = std::vector<Json>(255, delay);
    EXPECT_THROW(ReadBindings(state), std::invalid_argument);
}

TEST(ConfigurationTests, ReadsRandomHoldDurationsAndRejectsMalformedRanges)
{
    auto state = Configuration();
    auto& action = state["macros"][0]["actions"][1];
    action["holdRange"] = {{"min", 0}, {"max", 60000}};
    const auto bindings = ReadBindings(state);
    const auto& range = bindings.front().macro->Actions()[1].durationRange;
    ASSERT_TRUE(range);
    EXPECT_EQ(range->min.count(), 0);
    EXPECT_EQ(range->max.count(), 60000);
    EXPECT_FALSE(bindings.front().macro->Actions()[0].durationRange);
    EXPECT_EQ(bindings.front().macro->Actions()[0].duration.count(), 125);

    for (const Json& invalid : {Json{}, Json{{"min", 0}}, Json{{"max", 10}}, Json{{"min", 1.5}, {"max", 10}},
        Json{{"min", 20}, {"max", 10}}, Json{{"min", 0}, {"max", 60001}}})
    {
        action["holdRange"] = invalid;
        EXPECT_THROW(ReadBindings(state), std::invalid_argument);
    }
    action["holdRange"] = {{"min", 0}, {"max", 10}};
    action["mode"] = "down";
    EXPECT_THROW(ReadBindings(state), std::invalid_argument);
}

TEST(ConfigurationTests, ReadsRandomTextDelayAndRejectsItOnOtherEvents)
{
    auto state = Configuration();
    auto& actions = state["macros"][0]["actions"];
    actions[2]["duration"] = 30;
    EXPECT_FALSE(ReadBindings(state).front().macro->Actions()[2].durationRange);
    const Json delay{{"min", 40}, {"max", 90}};
    actions[2]["delayRange"] = delay;
    const auto bindings = ReadBindings(state);
    const auto& range = bindings.front().macro->Actions()[2].durationRange;
    ASSERT_TRUE(range);
    EXPECT_EQ(range->min.count(), 40);
    EXPECT_EQ(range->max.count(), 90);
    actions[2]["delayRange"]["max"] = 39;
    EXPECT_THROW(ReadBindings(state), std::invalid_argument);
    actions[2].erase("delayRange");
    actions[2]["holdRange"] = delay;
    EXPECT_THROW(ReadBindings(state), std::invalid_argument);
    actions[2].erase("holdRange");
    actions[0]["delayRange"] = delay;
    EXPECT_THROW(ReadBindings(state), std::invalid_argument);
}

TEST(ConfigurationTests, ReadsAllActionTypesAndPreservesSharedMacroBindings)
{
    const auto bindings = ReadBindings(Configuration());
    ASSERT_EQ(bindings.size(), 2u);
    EXPECT_EQ(bindings[0].trigger.key, UINT(VK_F1));
    EXPECT_EQ(bindings[0].trigger.modifiers, 0u);
    EXPECT_FALSE(bindings[0].replace);
    EXPECT_EQ(bindings[1].trigger.key, UINT('A'));
    EXPECT_EQ(bindings[1].trigger.modifiers, 7u);
    EXPECT_TRUE(bindings[1].replace);
    ASSERT_EQ(bindings[0].macro, bindings[1].macro);
    EXPECT_EQ(bindings[0].macro->Name(), "sample");
    const auto& actions = bindings[0].macro->Actions();
    ASSERT_EQ(actions.size(), 5u);
    EXPECT_EQ(actions[0].type, MacroAction::Type::Key);
    EXPECT_EQ(actions[0].key, UINT('C'));
    EXPECT_EQ(actions[0].modifiers, 3u);
    EXPECT_EQ(actions[0].duration.count(), 125);
    EXPECT_EQ(actions[1].type, MacroAction::Type::Mouse);
    EXPECT_EQ(actions[1].button, 1);
    EXPECT_EQ(actions[2].type, MacroAction::Type::Text);
    EXPECT_EQ(actions[2].text, L"Hello, \u6c34!");
    EXPECT_EQ(actions[3].type, MacroAction::Type::Wait);
    EXPECT_EQ(actions[3].duration.count(), 60000);
    EXPECT_EQ(actions[4].type, MacroAction::Type::Move);
    EXPECT_EQ(actions[4].position, Coordinates(LOWEST_INT, HIGHEST_INT));
}

TEST(ConfigurationTests, AcceptsEmptyStateOptionalFieldsAndSupportedLayouts)
{
    const Json empty = {{"version", 1}, {"enabled", false}, {"macros", Json::array()}, {"bindings", Json::array()}};
    EXPECT_TRUE(ReadBindings(empty).empty());
    for (const auto* layout : {"60%", "65%", "75%", "TKL", "100%"})
    {
        auto state = Configuration();
        state["layout"] = layout;
        EXPECT_EQ(ReadBindings(state).size(), 2u);
    }
    auto state = Configuration();
    state.erase("bindingOptions");
    state.erase("layout");
    state.erase("stopKey");
    EXPECT_EQ(ReadBindings(state).size(), 2u);
}

TEST(ConfigurationTests, DefaultsOptionalActionFieldsAndSupportsExtendedEnter)
{
    auto state = Configuration();
    state["macros"][0]["actions"] = {{{"type", "wait"}}, {{"type", "move"}},
                                    {{"type", "key"}, {"key", 0x100 | VK_RETURN}}};
    auto bindings = ReadBindings(state);
    const auto& actions = bindings[0].macro->Actions();
    ASSERT_EQ(actions.size(), 3u);
    for (const auto& action : actions)
    {
        EXPECT_EQ(action.modifiers, 0u);
        EXPECT_EQ(action.button, 0);
        EXPECT_EQ(action.duration.count(), 0);
        EXPECT_TRUE(action.text.empty());
        EXPECT_EQ(action.position, Coordinates());
    }
    EXPECT_EQ(actions[2].key, UINT(0x100 | VK_RETURN));
}

TEST(ConfigurationTests, RejectsMissingRequiredFields)
{
    for (const auto* path : {"/version", "/enabled", "/macros", "/bindings", "/macros/0/name",
                            "/macros/0/actions", "/macros/0/actions/0/type", "/bindings/0/key",
                            "/bindings/0/replace", "/bindingOptions/replace"})
    {
        SCOPED_TRACE(path);
        auto state = Configuration();
        const Json::json_pointer pointer(path);
        state.at(pointer.parent_pointer()).erase(pointer.back());
        EXPECT_THROW(ReadBindings(state), Json::exception);
    }
    for (const auto* path : {"/macros/0/id", "/bindings/0/code", "/bindings/0/macroId"})
    {
        SCOPED_TRACE(path);
        auto state = Configuration();
        const Json::json_pointer pointer(path);
        state.at(pointer.parent_pointer()).erase(pointer.back());
        EXPECT_THROW(ReadBindings(state), std::invalid_argument);
    }
}

TEST(ConfigurationTests, ReadsRecordedInputModesAndKeepsLegacyPresses)
{
    auto state = Configuration();
    auto& actions = state["macros"][0]["actions"];
    actions = {{{"type", "key"}, {"key", 65}, {"mode", "down"}},
        {{"type", "mouse"}, {"button", 1}, {"mode", "up"}}, {{"type", "key"}, {"key", 66}}};
    const auto bindings = ReadBindings(state);
    const auto& read = bindings[0].macro->Actions();
    EXPECT_EQ(read[0].mode, MacroAction::Mode::Down);
    EXPECT_EQ(read[1].mode, MacroAction::Mode::Up);
    EXPECT_EQ(read[2].mode, MacroAction::Mode::Press);
    for (const Json& mode : {Json("unknown"), Json(1), Json(nullptr)})
    {
        actions[0]["mode"] = mode;
        EXPECT_THROW(ReadBindings(state), std::exception);
    }
    actions = {{{"type", "wait"}, {"mode", "down"}}};
    EXPECT_THROW(ReadBindings(state), std::invalid_argument);
}

TEST(ConfigurationTests, ReadsMacroTypesAndDefaultsLegacyMacrosToNoRepeat)
{
    auto state = Configuration();
    EXPECT_EQ(ReadBindings(state)[0].macro->PlaybackType(), Macro::Type::NoRepeat);
    for (const auto* type : {"no-repeat", "toggle"})
    {
        state["macros"][0]["type"] = type;
        EXPECT_EQ(ReadBindings(state)[0].macro->PlaybackType(),
            std::string(type) == "toggle" ? Macro::Type::Toggle : Macro::Type::NoRepeat);
    }
    for (const Json& type : {Json("repeat"), Json(1), Json(nullptr)})
    {
        state["macros"][0]["type"] = type;
        EXPECT_THROW(ReadBindings(state), std::exception);
    }
}

TEST(ConfigurationTests, PreservesMouseClickPositionsAndRejectsInvalidMetadata)
{
    auto state = Configuration();
    auto& action = state["macros"][0]["actions"][1];
    action["positioned"] = true; action["x"] = -120; action["y"] = 450;
    const auto bindings = ReadBindings(state);
    const auto& read = bindings[0].macro->Actions()[1];
    EXPECT_TRUE(read.positioned);
    EXPECT_EQ(read.position, Coordinates(-120, 450));
    for (const Json& value : {Json("true"), Json(1), Json(nullptr)})
    {
        action["positioned"] = value;
        EXPECT_THROW(ReadBindings(state), std::exception);
    }
    action["positioned"] = true;
    for (const Json& value : {Json("0"), Json(1.5), Json(-2147483649LL), Json(2147483648ULL)})
    {
        action["x"] = value;
        EXPECT_THROW(ReadBindings(state), std::exception);
    }
    action["x"] = 0;
    action["type"] = "wait";
    EXPECT_THROW(ReadBindings(state), std::invalid_argument);
}

TEST(ConfigurationTests, ValidatesRecordHotkeyAndRecordingSettingsWithLegacyDefaults)
{
    auto state = Configuration();
    auto options = ReadRecordingOptions(state);
    EXPECT_TRUE(options.delays && options.keyboard && options.mouseButtons);
    EXPECT_FALSE(options.clickPosition || options.splitKeyEvents);
    state["recordingOptions"] = {{"mouseMovement", true}};
    EXPECT_TRUE(ReadRecordingOptions(state).clickPosition);
    state["recordingOptions"]["mouseButtons"] = false;
    EXPECT_FALSE(ReadRecordingOptions(state).clickPosition);
    state["recordKey"] = VK_F10;
    state["recordingOptions"] = {{"delays", false}, {"clickPosition", true}, {"keyboard", false}, {"mouseButtons", true}};
    EXPECT_NO_THROW(ReadBindings(state));
    options = ReadRecordingOptions(state);
    EXPECT_TRUE(options.clickPosition && options.mouseButtons);
    EXPECT_FALSE(options.delays || options.keyboard);
    state["recordingOptions"]["mouseButtons"] = false;
    EXPECT_THROW(ReadBindings(state), std::invalid_argument);
    state["recordingOptions"]["mouseButtons"] = true;
    for (const Json& key : {Json(VK_ESCAPE), Json(VK_F1), Json(VK_CONTROL), Json(-1), Json(512), Json(1.5), Json("121")})
    {
        state["recordKey"] = key;
        EXPECT_THROW(ReadBindings(state), std::exception);
    }
    state["recordKey"] = VK_F10;
    state["recordingOptions"]["splitKeyEvents"] = true;
    EXPECT_TRUE(ReadRecordingOptions(state).splitKeyEvents);
    for (const auto* name : {"delays", "clickPosition", "keyboard", "mouseButtons", "splitKeyEvents"})
    {
        state["recordingOptions"][name] = "true";
        EXPECT_THROW(ReadBindings(state), std::exception);
        state["recordingOptions"][name] = false;
    }
    state["recordingOptions"] = Json::array();
    EXPECT_THROW(ReadBindings(state), std::exception);
}

TEST(ConfigurationTests, RejectsInvalidTypesRangesAndReferences)
{
    struct Field { const char* path; std::vector<Json> invalid; };
    const Field fields[] = {
        {"/version", {nullptr, true, "1", 0, 2}}, {"/enabled", {nullptr, 1, "true"}},
        {"/macros", {nullptr, Json::object()}}, {"/bindings", {nullptr, Json::object()}},
        {"/layout", {nullptr, 75, "invalid"}},
        {"/stopKey", {nullptr, "27", 1.5, -1, 512, VK_CONTROL, VK_F1}},
        {"/bindingOptions", {nullptr, Json::array(), 1}},
        {"/bindingOptions/replace", {nullptr, 1, "true"}},
        {"/bindingOptions/modifiers", {nullptr, "3", 1.5, -1, 8}},
        {"/macros/0/id", {nullptr, "1", 1.5, 0, -1, 2147483648ULL}},
        {"/macros/0/name", {nullptr, 1, "", std::string(129, 'x')}},
        {"/macros/0/actions", {nullptr, Json::object(), Json::array()}},
        {"/macros/0/actions/0/type", {nullptr, 1, "unknown"}},
        {"/macros/0/actions/0/key", {nullptr, "67", 1.5, -1, 0, 512, 4294967363ULL}},
        {"/macros/0/actions/0/modifiers", {nullptr, "3", 1.5, -1, 8}},
        {"/macros/0/actions/0/duration", {nullptr, "0", 1.5, -1, 60001}},
        {"/macros/0/actions/1/button", {nullptr, "1", 1.5, -1, 2}},
        {"/macros/0/actions/2/text", {nullptr, 1, "", std::string(65537, 'x'), std::string("\xff")}},
        {"/macros/0/actions/4/x", {nullptr, "0", 1.5, -2147483649LL, 2147483648ULL}},
        {"/macros/0/actions/4/y", {nullptr, "0", 1.5, -2147483649LL, 2147483648ULL}},
        {"/bindings/0/code", {nullptr, "112", 1.5, -1, 0, 512, 4294967361ULL, VK_ESCAPE, VK_CONTROL}},
        {"/bindings/0/modifiers", {nullptr, "0", 1.5, -1, 8}},
        {"/bindings/0/macroId", {nullptr, "1", 1.5, 0, -1, 2, 2147483648ULL}},
        {"/bindings/0/key", {nullptr, 1, std::string(33, 'x')}},
        {"/bindings/0/replace", {nullptr, 1, "false"}}
    };
    for (const auto& field : fields)
        for (const auto& value : field.invalid)
        {
            SCOPED_TRACE(field.path);
            SCOPED_TRACE(value.dump(-1, ' ', false, Json::error_handler_t::replace));
            auto state = Configuration();
            state[Json::json_pointer(field.path)] = value;
            EXPECT_THROW(ReadBindings(state), std::exception);
        }
}

TEST(ConfigurationTests, RejectsDuplicateMacroIdsAndKeysEvenWithDifferentModifiers)
{
    auto state = Configuration();
    state["macros"].push_back(state["macros"][0]);
    EXPECT_THROW(ReadBindings(state), std::invalid_argument);
    state = Configuration();
    state["bindings"][1]["code"] = VK_F1;
    EXPECT_THROW(ReadBindings(state), std::invalid_argument);
}

TEST(ConfigurationTests, EnforcesCollectionLimitsAndAcceptsMaximumMacroAndTextSizes)
{
    auto state = Configuration();
    auto macro = state["macros"][0];
    macro["name"] = std::string(128, 'x');
    macro["actions"] = std::vector<Json>(256, Json{{"type", "wait"}});
    state["macros"] = Json::array();
    for (int id = 1; id <= 256; ++id) { macro["id"] = id; state["macros"].push_back(macro); }
    EXPECT_EQ(ReadBindings(state).size(), 2u);
    macro["id"] = 257;
    state["macros"].push_back(macro);
    EXPECT_THROW(ReadBindings(state), std::invalid_argument);
    state = Configuration();
    state["macros"][0]["actions"] = std::vector<Json>(257, Json{{"type", "wait"}});
    EXPECT_THROW(ReadBindings(state), std::invalid_argument);
    state = Configuration();
    state["bindings"] = std::vector<Json>(257, state["bindings"][0]);
    EXPECT_THROW(ReadBindings(state), std::invalid_argument);
    state = Configuration();
    state["macros"][0]["actions"][2]["text"] = std::string(65536, 'x');
    EXPECT_EQ(ReadBindings(state).size(), 2u);
}

class WrapperAppTests : public ::testing::Test
{
protected:
    fs::path directory;
    bool created = false;

    void SetUp() override
    {
        static unsigned sequence = 0;
        directory = fs::temp_directory_path() / ("tea-wraps-tests-" + std::to_string(GetCurrentProcessId()) +
            "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(++sequence));
        created = fs::create_directory(directory);
        ASSERT_TRUE(created);
        calls = WrapperCalls{};
        calls.localFolder = (directory / L"local").wstring();
        calls.commandLine = L"tea-tests.exe --smoke-test \"" + directory.wstring() + L"\"";
    }

    void TearDown() override
    {
        if (calls.mutex && !calls.closed) CloseHandle(calls.mutex);
        if (!created) return;
        ASSERT_TRUE(fs::equivalent(directory.parent_path(), fs::temp_directory_path()));
        std::error_code error;
        fs::remove_all(directory, error);
        EXPECT_FALSE(error) << error.message();
    }

    void Store(const std::string& bytes)
    {
        std::ofstream file(directory / L"macros.json", std::ios::binary);
        ASSERT_TRUE(file.write(bytes.data(), static_cast<std::streamsize>(bytes.size())));
    }

    std::string Stored()
    {
        std::ifstream file(directory / L"macros.json", std::ios::binary);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    int Run() { return RunTea(GetModuleHandleW(nullptr), SW_HIDE); }

    void ExpectCleanup()
    {
        EXPECT_EQ(calls.uninitialized, 1);
        EXPECT_EQ(calls.released, 1);
        EXPECT_EQ(calls.closed, 1);
    }
};

TEST_F(WrapperAppTests, CreatesDefaultStateWithEscapeAndAtomicallyReplacesTemporaryFile)
{
    App app(directory, true);
    const Json expected = {{"version", 1}, {"enabled", true}, {"stopKey", VK_ESCAPE},
                          {"macros", Json::array()}, {"bindings", Json::array()}};
    EXPECT_EQ(Json::parse(Stored()), expected);
    EXPECT_EQ(calls.moves, 1);
    EXPECT_EQ(calls.moveFlags, DWORD(MOVEFILE_WRITE_THROUGH));
    EXPECT_FALSE(fs::exists(directory / L"macros.tmp"));
}

TEST_F(WrapperAppTests, ReloadsExistingStateWithoutRewritingTheSavedFile)
{
    auto state = Configuration();
    state["enabled"] = false;
    const auto bytes = state.dump();
    Store(bytes);
    App app(directory, true);
    EXPECT_EQ(Stored(), bytes);
    EXPECT_EQ(calls.moves, 0);
}

TEST_F(WrapperAppTests, AddsMissingStopKeyWithoutLosingSavedMacrosBindingsOrPreferences)
{
    auto state = Configuration();
    state.erase("stopKey");
    Store(state.dump());
    App app(directory, true);
    state["stopKey"] = VK_ESCAPE;
    EXPECT_EQ(Json::parse(Stored()), state);
}

TEST_F(WrapperAppTests, RestoresMissingStopKeyWithoutConflictingWithTheRecordHotkey)
{
    auto state = Configuration();
    state.erase("stopKey");
    state["recordKey"] = VK_ESCAPE;
    Store(state.dump());
    App app(directory, true);
    state["stopKey"] = VK_F12;
    EXPECT_EQ(Json::parse(Stored()), state);
}

TEST_F(WrapperAppTests, OpensAnEmptyConfigurationWhenSavedDataCannotBeParsed)
{
    auto invalid = Configuration();
    invalid["bindings"][0]["macroId"] = 999;
    for (const auto& bytes : {std::string("{broken"), std::string("null"), invalid.dump(), Configuration().dump() + " garbage"})
    {
        Store(bytes);
        App app(directory, true);
        EXPECT_FALSE(app.RecoveryStatus().at("configuration").get<std::string>().empty());
        EXPECT_EQ(Stored(), bytes);
        EXPECT_EQ(calls.moves, 0);
    }
}

TEST_F(WrapperAppTests, OpensInRecoveryWhenAnExistingConfigurationCannotBeOpened)
{
    const auto bytes = Configuration().dump();
    Store(bytes);
    const auto file = directory / L"macros.json";
    const HANDLE locked = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(locked, INVALID_HANDLE_VALUE);
    App app(directory, true);
    EXPECT_FALSE(app.RecoveryStatus().at("configuration").get<std::string>().empty());
    EXPECT_EQ(calls.moves, 0);
    CloseHandle(locked);
    EXPECT_EQ(Stored(), bytes);
}

TEST_F(WrapperAppTests, FallsBackToTheLastSavedConfigurationWithoutChangingTheDamagedFile)
{
    auto previous = Configuration();
    previous.erase("stopKey");
    Store(previous.dump());
    { App app(directory, true); }
    const auto backup = directory / L"macros.backup.json";
    ASSERT_EQ(ReadSavedConfiguration(backup), previous);
    Store("{broken");

    App recovered(directory, true);
    EXPECT_NE(recovered.RecoveryStatus().at("configuration").get<std::string>().find("previous saved"), std::string::npos);
    EXPECT_EQ(Stored(), "{broken");
    EXPECT_EQ(ReadSavedConfiguration(backup), previous);

    recovered.SaveRecoveredConfiguration();
    auto expected = previous;
    expected["stopKey"] = VK_ESCAPE;
    EXPECT_EQ(Json::parse(Stored()), expected);
    EXPECT_EQ(ReadSavedConfiguration(backup), previous);
    EXPECT_TRUE(recovered.RecoveryStatus().at("configuration").get<std::string>().empty());
    unsigned archived = 0;
    for (const auto& file : fs::directory_iterator(directory))
    {
        if (file.path().filename().wstring().find(L"macros.json.unreadable-") != 0) continue;
        std::ifstream original(file.path(), std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(original), {}};
        EXPECT_EQ(bytes, "{broken");
        ++archived;
    }
    EXPECT_EQ(archived, 1u);
}

TEST_F(WrapperAppTests, RecreatesMissingConfigurationFromBackupWithoutReplacingAFile)
{
    const auto previous = Configuration();
    const auto backup = directory / L"macros.backup.json";
    std::ofstream(backup) << previous.dump();
    App recovered(directory, true);
    ASSERT_FALSE(fs::exists(directory / L"macros.json"));

    recovered.SaveRecoveredConfiguration();

    EXPECT_EQ(Json::parse(Stored()), previous);
    EXPECT_EQ(ReadSavedConfiguration(backup), previous);
    EXPECT_EQ(calls.moveFlags, DWORD(MOVEFILE_WRITE_THROUGH));
    EXPECT_TRUE(recovered.RecoveryStatus().at("configuration").get<std::string>().empty());
}

TEST_F(WrapperAppTests, PreservesPrimaryRestoredWhileBackupRecoveryIsOpen)
{
    const auto previous = Configuration();
    const auto backup = directory / L"macros.backup.json";
    std::ofstream(backup) << previous.dump();
    App recovered(directory, true);
    auto primary = previous;
    primary["macros"][0]["name"] = "restored externally";
    const auto bytes = primary.dump();
    Store(bytes);

    EXPECT_THROW(recovered.SaveRecoveredConfiguration(), std::runtime_error);

    EXPECT_EQ(Stored(), bytes);
    EXPECT_EQ(ReadSavedConfiguration(backup), previous);
    EXPECT_EQ(calls.moveFlags, DWORD(MOVEFILE_WRITE_THROUGH));
    EXPECT_FALSE(recovered.RecoveryStatus().at("configuration").get<std::string>().empty());
}

TEST_F(WrapperAppTests, ArchivesDamagedBackupWhenThePrimaryConfigurationIsMissing)
{
    const auto backup = directory / L"macros.backup.json";
    const std::string damaged = "{\"macros\":[";
    std::ofstream(backup, std::ios::binary) << damaged;
    const auto readBytes = [](const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string{std::istreambuf_iterator<char>(file), {}};
    };

    App recovered(directory, true);
    EXPECT_NE(recovered.RecoveryStatus().at("configuration").get<std::string>().find("backup"), std::string::npos);
    EXPECT_FALSE(fs::exists(directory / L"macros.json"));
    EXPECT_EQ(calls.moves, 0);

    calls.moveFailures = 1;
    EXPECT_THROW(recovered.SaveRecoveredConfiguration(), std::runtime_error);
    EXPECT_FALSE(fs::exists(directory / L"macros.json"));
    EXPECT_FALSE(recovered.RecoveryStatus().at("configuration").get<std::string>().empty());
    EXPECT_EQ(readBytes(backup), damaged);

    recovered.SaveRecoveredConfiguration();
    EXPECT_TRUE(Json::parse(Stored()).at("macros").empty());
    EXPECT_EQ(readBytes(backup), damaged);
    EXPECT_EQ(calls.moveFlags, DWORD(MOVEFILE_WRITE_THROUGH));
    EXPECT_TRUE(recovered.RecoveryStatus().at("configuration").get<std::string>().empty());
    unsigned archived = 0;
    for (const auto& file : fs::directory_iterator(directory))
    {
        if (file.path().filename().wstring().find(L"macros.backup.json.unreadable-") != 0) continue;
        EXPECT_EQ(readBytes(file.path()), damaged);
        ++archived;
    }
    EXPECT_EQ(archived, 1u);
}

TEST_F(WrapperAppTests, CannotResetMissingPrimaryUntilAnUnreadableBackupCanBeArchived)
{
    const auto backup = directory / L"macros.backup.json";
    const auto previous = Configuration();
    std::ofstream(backup) << previous.dump();
    const HANDLE handle = CreateFileW(backup.c_str(), GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    std::unique_ptr<void, decltype(&CloseHandle)> locked(handle, CloseHandle);

    App recovered(directory, true);
    EXPECT_FALSE(recovered.RecoveryStatus().at("configuration").get<std::string>().empty());
    EXPECT_THROW(recovered.SaveRecoveredConfiguration(), std::runtime_error);
    EXPECT_FALSE(fs::exists(directory / L"macros.json"));
    EXPECT_EQ(calls.moves, 0);

    locked.reset();
    recovered.SaveRecoveredConfiguration();
    EXPECT_TRUE(Json::parse(Stored()).at("macros").empty());
    EXPECT_EQ(ReadSavedConfiguration(backup), previous);
    EXPECT_TRUE(recovered.RecoveryStatus().at("configuration").get<std::string>().empty());
}

TEST_F(WrapperAppTests, ExistingMissingImagesDoNotBlockOtherMacroChanges)
{
    const auto missingImage = [](int id) { return Json{{"type", "wait-for-image"}, {"imageId", id}}; };
    auto previous = Configuration();
    previous["macros"][0]["actions"] = Json::array({missingImage(10)});
    auto second = previous["macros"][0];
    second["id"] = 8;
    second["actions"] = Json::array({missingImage(20)});
    previous["macros"].push_back(second);
    const tea::vision::ImageLibrary images(directory / L"images");

    auto next = previous;
    next["macros"].erase(1);
    EXPECT_NO_THROW(ValidateNewImageReferences(next, previous, images));
    next["macros"][0]["name"] = "Renamed macro";
    EXPECT_NO_THROW(ValidateNewImageReferences(next, previous, images));
    next["macros"][0]["actions"].push_back(missingImage(20));
    EXPECT_THROW(ValidateNewImageReferences(next, previous, images), std::invalid_argument);
}

TEST_F(WrapperAppTests, ImageLibraryRetainsSourcePixelsAndStableIdsAcrossReload)
{
    using namespace tea::vision;
    const auto path = directory / L"images";
    const Image original{2, 1, {{10, 20, 30}, {40, 50, 60, 128}}};
    {
        ImageLibrary images(path);
        images.Add("original", original);
        images.Rename(1, "renamed");
    }
    ImageLibrary images(path);
    ASSERT_TRUE(images.Has(1));
    EXPECT_EQ(images.List().at(0).at("name"), "renamed");
    Image loaded;
    std::thread worker([&] {
        try { loaded = images.Load(1); }
        catch (const std::exception& error) { ADD_FAILURE() << error.what(); }
    });
    worker.join();
    ASSERT_TRUE(loaded.Valid());
    EXPECT_EQ(loaded.width, original.width);
    EXPECT_EQ(loaded.height, original.height);
    EXPECT_EQ(loaded.pixels[0].blue, 10);
    EXPECT_EQ(loaded.pixels[1].red, 60);
    EXPECT_EQ(loaded.pixels[1].alpha, 128);
    images.Remove(1);
    images.Add("next", original);
    EXPECT_FALSE(images.Has(1));
    EXPECT_FALSE(fs::exists(path / L"1.png"));
    EXPECT_TRUE(ImageLibrary(path).Has(2));
}

TEST_F(WrapperAppTests, ImageLibraryRejectsInvisibleAndNearlyTransparentImports)
{
    using namespace tea::vision;
    const auto path = directory / L"images";
    ImageLibrary images(path);
    for (std::uint8_t alpha : {0, 5, 63})
        EXPECT_THROW(images.Add("unmatchable", {1, 1, {{10, 20, 30, alpha}}}), std::invalid_argument);
    EXPECT_TRUE(images.List().empty());
    EXPECT_EQ(images.NextId(), 1);
    EXPECT_FALSE(fs::exists(path));
    EXPECT_NO_THROW(images.Add("visible", {1, 1, {{10, 20, 30, 64}}}));
    EXPECT_TRUE(ImageLibrary(path).Has(1));
}

TEST_F(WrapperAppTests, RecoversAnInterruptedImportBesideAHealthyCatalog)
{
    using namespace tea::vision;
    const auto path = directory / L"images";
    {
        ImageLibrary images(path);
        images.Add("original", {1, 1, {{10, 20, 30}}});
        images.Add("deleted", {1, 1, {{20, 30, 40}}});
        images.Remove(2);
    }
    ASSERT_TRUE(fs::copy_file(path / L"1.png", path / L"2.png.deleting"));
    ASSERT_TRUE(fs::copy_file(path / L"1.png", path / L"3.png"));

    ImageLibrary recovered(path);
    EXPECT_TRUE(recovered.Has(1));
    EXPECT_FALSE(recovered.Has(2));
    ASSERT_TRUE(recovered.Has(3));
    EXPECT_EQ(recovered.NextId(), 4);
    EXPECT_EQ(recovered.List()[0].at("name"), "original");
    EXPECT_EQ(recovered.List()[1].at("name"), "recovered image 3");
    EXPECT_FALSE(recovered.RecoveryNotice().empty());
    EXPECT_FALSE(fs::exists(path / L"2.png.deleting"));

    recovered.Rename(3, "restored");
    const ImageLibrary reopened(path);
    EXPECT_TRUE(reopened.Has(3));
    EXPECT_EQ(reopened.List()[1].at("name"), "restored");
    EXPECT_TRUE(reopened.RecoveryNotice().empty());
}

TEST_F(WrapperAppTests, ImageLibraryReservesOrphanedFilesAndRestoredMacroIds)
{
    using namespace tea::vision;
    const auto path = directory / L"images";
    const Image original{1, 1, {{10, 20, 30}}};
    ImageLibrary(path).Add("original", original);
    ASSERT_TRUE(fs::remove(path / L"index.json"));

    ImageLibrary restored(path);
    EXPECT_EQ(restored.NextId(), 2);
    EXPECT_TRUE(restored.Has(1));
    EXPECT_EQ(restored.List()[0].at("name"), "recovered image 1");
    EXPECT_FALSE(restored.RecoveryNotice().empty());
    restored.ReserveThrough(4);
    restored.Add("new", Image{1, 1, {{50, 60, 70}}});
    EXPECT_TRUE(restored.Has(5));
    EXPECT_EQ(restored.Load(1).pixels[0].blue, 10);
    EXPECT_EQ(ImageLibrary(path).NextId(), 6);
}

TEST_F(WrapperAppTests, RebuildsCorruptImageIndexAndPreservesItOnTheNextChange)
{
    const auto path = directory / L"images";
    tea::vision::ImageLibrary(path).Add("original", {1, 1, {{10, 20, 30}}});
    std::ofstream(path / L"index.json") << "{broken";

    tea::vision::ImageLibrary recovered(path);
    ASSERT_TRUE(recovered.Has(1));
    recovered.Rename(1, "restored");
    EXPECT_EQ(tea::vision::ImageLibrary(path).List()[0].at("name"), "restored");
    unsigned archived = 0;
    for (const auto& file : fs::directory_iterator(path))
    {
        if (file.path().filename().wstring().find(L"index.json.unreadable-") != 0) continue;
        std::ifstream original(file.path());
        const std::string bytes{std::istreambuf_iterator<char>(original), {}};
        EXPECT_EQ(bytes, "{broken");
        ++archived;
    }
    EXPECT_EQ(archived, 1u);
}

TEST_F(WrapperAppTests, LockedImageStaysInTheCatalogAndCanBeDeletedAfterUnlocking)
{
    const auto path = directory / L"images";
    tea::vision::ImageLibrary images(path);
    images.Add("locked", {1, 1, {{10, 20, 30}}});
    const auto image = path / L"1.png";
    const HANDLE locked = CreateFileW(image.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(locked, INVALID_HANDLE_VALUE);
    EXPECT_THROW(images.Remove(1), std::runtime_error);
    EXPECT_TRUE(tea::vision::ImageLibrary(path).Has(1));
    CloseHandle(locked);

    EXPECT_NO_THROW(images.Remove(1));
    EXPECT_FALSE(fs::exists(image));
    EXPECT_TRUE(tea::vision::ImageLibrary(path).List().empty());
}

TEST_F(WrapperAppTests, FailedCatalogWriteRestoresTheImage)
{
    const auto path = directory / L"images";
    tea::vision::ImageLibrary images(path);
    images.Add("original", {1, 1, {{10, 20, 30}}});
    ASSERT_TRUE(fs::create_directory(path / L"index.tmp"));

    EXPECT_THROW(images.Remove(1), std::runtime_error);
    EXPECT_TRUE(images.Has(1));
    EXPECT_FALSE(fs::exists(path / L"1.png.deleting"));
    EXPECT_TRUE(tea::vision::ImageLibrary(path).Has(1));
}

TEST_F(WrapperAppTests, RetriesAnUnreadableImageAfterSavingAPartialCatalog)
{
    const auto path = directory / L"images";
    tea::vision::ImageLibrary(path).Add("original", {1, 1, {{10, 20, 30}}});
    ASSERT_TRUE(fs::remove(path / L"index.json"));
    const HANDLE locked = CreateFileW((path / L"1.png").c_str(), GENERIC_READ, 0,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(locked, INVALID_HANDLE_VALUE);
    tea::vision::ImageLibrary partial(path);
    EXPECT_FALSE(partial.Has(1));
    EXPECT_NO_THROW(partial.Add("new", {1, 1, {{50, 60, 70}}}));
    CloseHandle(locked);

    tea::vision::ImageLibrary recovered(path);
    EXPECT_TRUE(recovered.Has(1));
    EXPECT_TRUE(recovered.Has(2));
    EXPECT_EQ(recovered.List()[0].at("id"), 1);
}

TEST_F(WrapperAppTests, RestoresAPendingImageFromAStagedDeletionAfterUnlocking)
{
    using tea::vision::ImageLibrary;
    const auto path = directory / L"images";
    const auto staged = path / L"1.png.deleting";
    ImageLibrary(path).Add("original", {1, 1, {{10, 20, 30}}});
    fs::rename(path / L"1.png", staged);
    ASSERT_TRUE(fs::remove(path / L"index.json"));

    {
        const HANDLE locked = CreateFileW(staged.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        ASSERT_NE(locked, INVALID_HANDLE_VALUE);
        const std::unique_ptr<void, decltype(&CloseHandle)> lock(locked, CloseHandle);
        ImageLibrary partial(path);
        EXPECT_FALSE(partial.Has(1));
        ASSERT_NO_THROW(partial.Add("new", {1, 1, {{50, 60, 70}}}));
        EXPECT_TRUE(fs::exists(staged));
    }

    ImageLibrary recovered(path);
    ASSERT_TRUE(recovered.Has(1));
    EXPECT_TRUE(recovered.Has(2));
    EXPECT_EQ(recovered.Load(1).pixels[0].blue, 10);
    EXPECT_FALSE(fs::exists(staged));
}

TEST_F(WrapperAppTests, RestoresAStagedImageWhenItsCatalogEntryStillExists)
{
    const auto path = directory / L"images";
    tea::vision::ImageLibrary(path).Add("original", {1, 1, {{10, 20, 30}}});
    fs::rename(path / L"1.png", path / L"1.png.deleting");

    EXPECT_TRUE(tea::vision::ImageLibrary(path).Has(1));
    EXPECT_FALSE(fs::exists(path / L"1.png.deleting"));
}

TEST_F(WrapperAppTests, ReportsKnownFolderAndDirectoryCreationFailures)
{
    calls.folderFails = true;
    EXPECT_THROW(App(directory, true), std::runtime_error);
    calls.folderFails = false;
    const auto blocked = directory / L"blocked";
    std::ofstream(blocked) << "blocked";
    EXPECT_THROW(App(blocked, true), fs::filesystem_error);
}

TEST_F(WrapperAppTests, FailedTemporaryWritePreservesTheSavedConfiguration)
{
    auto state = Configuration();
    state.erase("stopKey");
    const auto bytes = state.dump();
    Store(bytes);
    ASSERT_TRUE(fs::create_directory(directory / L"macros.tmp"));
    EXPECT_THROW(App(directory, true), std::runtime_error);
    EXPECT_EQ(Stored(), bytes);
    EXPECT_EQ(calls.moves, 0);
}

TEST_F(WrapperAppTests, FailedAtomicReplacePreservesTheSavedConfiguration)
{
    auto state = Configuration();
    state.erase("stopKey");
    const auto bytes = state.dump();
    Store(bytes);
    calls.moveFailures = 1;
    EXPECT_THROW(App(directory, true), std::runtime_error);
    EXPECT_EQ(Stored(), bytes);
    EXPECT_EQ(calls.moves, 1);
}

TEST_F(WrapperAppTests, RetriesAtomicReplaceAfterATemporaryFileLock)
{
    auto state = Configuration();
    state.erase("stopKey");
    Store(state.dump());
    calls.moveFailures = 1;
    calls.moveError = ERROR_ACCESS_DENIED;

    App app(directory, true);

    state["stopKey"] = VK_ESCAPE;
    EXPECT_EQ(Json::parse(Stored()), state);
    EXPECT_EQ(calls.moves, 2);
    EXPECT_FALSE(fs::exists(directory / L"macros.tmp"));
}

TEST_F(WrapperAppTests, PersistentFileLockPreservesTheSavedConfiguration)
{
    auto state = Configuration();
    state.erase("stopKey");
    const auto bytes = state.dump();
    Store(bytes);
    calls.moveFailures = 20;
    calls.moveError = ERROR_SHARING_VIOLATION;

    EXPECT_THROW(App(directory, true), std::runtime_error);
    EXPECT_EQ(Stored(), bytes);
    EXPECT_EQ(calls.moves, 5);
}

TEST_F(WrapperAppTests, DuplicateInstanceClosesItsHandleWithoutInitializingCom)
{
    calls.alreadyRunning = true;
    EXPECT_EQ(Run(), 0);
    EXPECT_EQ(calls.closed, 1);
    EXPECT_EQ(calls.released, 0);
    EXPECT_EQ(calls.initialized, 0);
    ASSERT_EQ(calls.dialogs.size(), 1u);
    EXPECT_EQ(calls.dialogs[0], L"tea is already running");
}

TEST_F(WrapperAppTests, MutexCreationFailureDoesNotCloseAnInvalidHandle)
{
    calls.mutexFails = true;
    EXPECT_EQ(Run(), 0);
    EXPECT_EQ(calls.closed, 0);
    EXPECT_EQ(calls.initialized, 0);
    EXPECT_EQ(calls.dialogs.size(), 1u);
}

TEST_F(WrapperAppTests, ComFailureClosesTheMutexWithoutUninitializingCom)
{
    calls.comResult = E_FAIL;
    EXPECT_EQ(Run(), 1);
    EXPECT_EQ(calls.initialized, 1);
    EXPECT_EQ(calls.closed, 1);
    EXPECT_EQ(calls.released, 0);
    EXPECT_EQ(calls.uninitialized, 0);
    EXPECT_EQ(calls.registered, 0);
}

TEST_F(WrapperAppTests, WindowCreationFailureReportsTheErrorAndReleasesStartupResources)
{
    EXPECT_EQ(Run(), 1);
    EXPECT_EQ(calls.registered, 1);
    EXPECT_EQ(calls.windows, 1);
    EXPECT_EQ(calls.dpi, 1);
    EXPECT_EQ(calls.dpiFallback, 0);
    ASSERT_EQ(calls.dialogs.size(), 1u);
    EXPECT_EQ(calls.dialogs[0], L"the application window could not be created");
    ExpectCleanup();
}

TEST_F(WrapperAppTests, FallsBackToLegacyDpiAndBalancesPreviouslyInitializedCom)
{
    calls.dpiSucceeds = false;
    calls.comResult = S_FALSE;
    EXPECT_EQ(Run(), 1);
    EXPECT_EQ(calls.dpi, 1);
    EXPECT_EQ(calls.dpiFallback, 1);
    ExpectCleanup();
}

TEST_F(WrapperAppTests, InvalidSavedDataAllowsWindowCreationAndCleansUp)
{
    Store("{broken");
    EXPECT_EQ(Run(), 1);
    EXPECT_EQ(Stored(), "{broken");
    EXPECT_EQ(calls.registered, 1);
    EXPECT_EQ(calls.windows, 1);
    ASSERT_EQ(calls.dialogs.size(), 1u);
    EXPECT_EQ(calls.dialogs[0], L"the application window could not be created");
    ExpectCleanup();
}

TEST_F(WrapperAppTests, SmokeArgumentsUseTheProvidedDirectoryAndSeparateInstanceMutex)
{
    EXPECT_EQ(Run(), 1);
    EXPECT_EQ(calls.mutexName, L"Local\\TeaMacroToolSmoke");
    EXPECT_TRUE(fs::exists(directory / L"macros.json"));
    EXPECT_FALSE(fs::exists(directory / L"local"));
    ExpectCleanup();
}

TEST_F(WrapperAppTests, InstallVerificationRefusesToUseExistingUserData)
{
    Store(Configuration().dump());
    calls.commandLine = L"tea-tests.exe --verify-install \"" + directory.wstring() + L"\"";
    EXPECT_EQ(Run(), 2);
    EXPECT_EQ(calls.initialized, 0);
    EXPECT_EQ(Stored(), Configuration().dump());
}

TEST_F(WrapperAppTests, NormalArgumentsUseLocalAppDataAndTheNormalInstanceMutex)
{
    for (const auto* arguments : {L"", L" --smoke-test", L" --unknown path", L" --smoke-test path extra"})
    {
        calls = WrapperCalls{};
        calls.localFolder = (directory / L"local").wstring();
        calls.commandLine = std::wstring(L"tea-tests.exe") + arguments;
        EXPECT_EQ(Run(), 1);
        EXPECT_EQ(calls.mutexName, L"Local\\TeaMacroTool");
        EXPECT_TRUE(fs::exists(directory / L"local" / L"tea" / L"macros.json"));
        EXPECT_FALSE(fs::exists(directory / L"macros.json"));
        ExpectCleanup();
    }
}
}
