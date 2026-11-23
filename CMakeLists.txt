cmake_minimum_required(VERSION 3.22)
project(Tea VERSION 1.0.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Tea requires a Windows target.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
include(CTest)

add_library(tea_core STATIC src/winput.cpp src/tea_wraps.cpp src/macros.cpp
    src/2d/capture.cpp src/2d/capture_tool.cpp src/2d/image_library.cpp
    src/2d/matcher.cpp src/2d/recognizer.cpp)
target_compile_features(tea_core PUBLIC cxx_std_17)
target_include_directories(tea_core PUBLIC src)
target_link_libraries(tea_core PUBLIC user32 ole32 shell32 uuid gdi32 dwmapi uxtheme windowscodecs)
if(MINGW)
    target_link_options(tea_core INTERFACE -static)
endif()

set(TEA_DEPS "${CMAKE_BINARY_DIR}/deps")
set(WEBVIEW2 "${TEA_DEPS}/webview2")
function(tea_download path url hash)
    if(EXISTS "${path}")
        file(SHA256 "${path}" actual)
        if(actual STREQUAL hash)
            return()
        endif()
    endif()
    get_filename_component(parent "${path}" DIRECTORY)
    file(MAKE_DIRECTORY "${parent}")
    file(DOWNLOAD "${url}" "${path}.download" TLS_VERIFY ON TIMEOUT 120 INACTIVITY_TIMEOUT 30
        EXPECTED_HASH "SHA256=${hash}")
    file(RENAME "${path}.download" "${path}")
endfunction()

tea_download("${TEA_DEPS}/webview2.zip"
    "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.2903.40/microsoft.web.webview2.1.0.2903.40.nupkg"
    ef128016dd1e51c59178c827ed5b8aa3322c57afa8675d930f8109505542ad74)
file(ARCHIVE_EXTRACT INPUT "${TEA_DEPS}/webview2.zip" DESTINATION "${WEBVIEW2}")
tea_download("${TEA_DEPS}/json/json.hpp"
    "https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp"
    9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6)
if(MINGW)
    get_filename_component(COMPILER_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(CMAKE_RC_COMPILER windres HINTS "${COMPILER_BIN}" REQUIRED)
endif()
enable_language(RC)
if(CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "ARM64")
    set(WEBVIEW_ARCH arm64)
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(WEBVIEW_ARCH x64)
else()
    set(WEBVIEW_ARCH x86)
endif()
set(WEBVIEW_LOADER "${WEBVIEW2}/runtimes/win-${WEBVIEW_ARCH}/native/WebView2Loader.dll")
set(TEA_UI "${CMAKE_CURRENT_SOURCE_DIR}/src/ui")
set(TEA_SMOKE_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/gtests/ui_smoke.js")
if(BUILD_TESTING)
    set(SMOKE_RESOURCE "104 RCDATA \"${TEA_SMOKE_SCRIPT}\"\n")
    set(SMOKE_DEPENDENCY "${TEA_SMOKE_SCRIPT}")
    target_compile_definitions(tea_core PRIVATE TEA_DESKTOP_SMOKE)
endif()
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/ui.rc" CONTENT
    "1 ICON \"${TEA_UI}/tea.ico\"\n101 RCDATA \"${TEA_UI}/index.html\"\n102 RCDATA \"${TEA_UI}/fonts/JetBrainsMono-Regular.woff2\"\n${SMOKE_RESOURCE}")
set_source_files_properties("${CMAKE_BINARY_DIR}/ui.rc" PROPERTIES OBJECT_DEPENDS
    "${TEA_UI}/tea.ico;${TEA_UI}/index.html;${TEA_UI}/fonts/JetBrainsMono-Regular.woff2;${SMOKE_DEPENDENCY}")
target_include_directories(tea_core PRIVATE "${WEBVIEW2}/build/native/include" "${TEA_DEPS}/json")
target_compile_definitions(tea_core PUBLIC UNICODE _UNICODE NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00)

add_executable(tea WIN32 src/main.cpp "${CMAKE_BINARY_DIR}/ui.rc")
target_link_libraries(tea PRIVATE tea_core)
add_custom_command(TARGET tea POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${WEBVIEW_LOADER}" "$<TARGET_FILE_DIR:tea>/WebView2Loader.dll")

if(BUILD_TESTING)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    add_subdirectory(external/googletest EXCLUDE_FROM_ALL)

    add_executable(tea_tests
        gtests/tea_wraps_gtests.cpp
        gtests/winput_gtests.cpp
        gtests/macros_gtests.cpp
        gtests/recognizer_gtests.cpp)
    target_include_directories(tea_tests PRIVATE "${WEBVIEW2}/build/native/include" "${TEA_DEPS}/json")
    target_compile_definitions(tea_tests PRIVATE TEA_DESKTOP_SMOKE)
    target_link_libraries(tea_tests PRIVATE tea_core GTest::gtest_main)

    include(GoogleTest)
    set(DESKTOP_CASES "CaptureTests.CapturesScreenRegionAndReleasesGdiResources:CaptureTests.CapturesVisibleWindowClientAndRejectsHiddenWindow:RecorderTests.InstallsAndRemovesNativeHooks:ListenerNativeTests.InstallsRemovesAndRestartsNativeHookWithoutSendingInput")
    gtest_discover_tests(tea_tests TEST_FILTER "*-${DESKTOP_CASES}" PROPERTIES LABELS unit)
    gtest_discover_tests(tea_tests TEST_FILTER "${DESKTOP_CASES}" PROPERTIES LABELS desktop RESOURCE_LOCK tea_desktop TIMEOUT 10)
    add_executable(tea_recording_tests gtests/recording_native_gtests.cpp)
    target_link_libraries(tea_recording_tests PRIVATE tea_core GTest::gtest_main)
    gtest_discover_tests(tea_recording_tests PROPERTIES LABELS desktop RESOURCE_LOCK tea_desktop TIMEOUT 10)
    add_test(NAME TeaInterfaceSmoke COMMAND tea --interface-test "${CMAKE_BINARY_DIR}/interface-smoke")
    set_tests_properties(TeaInterfaceSmoke PROPERTIES LABELS interface TIMEOUT 60 RESOURCE_LOCK tea_desktop)
    add_test(NAME TeaDesktopSmoke COMMAND tea --smoke-test "${CMAKE_BINARY_DIR}/smoke")
    set_tests_properties(TeaDesktopSmoke PROPERTIES LABELS desktop TIMEOUT 60 RESOURCE_LOCK tea_desktop)
else()
    install(TARGETS tea RUNTIME DESTINATION .)
    install(FILES "${WEBVIEW_LOADER}" DESTINATION .)
    install(FILES LICENSE DESTINATION .)
    install(FILES "${WEBVIEW2}/LICENSE.txt" DESTINATION licenses RENAME webview2-license.txt)
    install(FILES "${WEBVIEW2}/NOTICE.txt" DESTINATION licenses RENAME webview2-notice.txt)
    install(FILES "${TEA_UI}/fonts/OFL.txt" DESTINATION licenses RENAME jetbrains-mono.txt)
    install(FILES licenses/nlohmann-json.txt DESTINATION licenses)

    set(TEA_SIGNING_CERTIFICATE "" CACHE STRING "Code-signing certificate thumbprint in the current user's My store")
    set(TEA_SIGNING_TIMESTAMP_URL "" CACHE STRING "RFC 3161 timestamp service URL for signed packages")
    if(TEA_SIGNING_CERTIFICATE)
        if(NOT TEA_SIGNING_TIMESTAMP_URL)
            message(FATAL_ERROR "Set TEA_SIGNING_TIMESTAMP_URL to create a timestamped release signature.")
        endif()
        find_program(TEA_SIGNTOOL NAMES signtool HINTS "$ENV{WindowsSdkVerBinPath}x64" REQUIRED)
        string(CONFIGURE [[
            set(executable "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/tea.exe")
            execute_process(COMMAND "@TEA_SIGNTOOL@" sign /sha1 "@TEA_SIGNING_CERTIFICATE@"
                /fd SHA256 /tr "@TEA_SIGNING_TIMESTAMP_URL@" /td SHA256 "${executable}"
                COMMAND_ERROR_IS_FATAL ANY)
            execute_process(COMMAND "@TEA_SIGNTOOL@" verify /pa /tw "${executable}"
                COMMAND_ERROR_IS_FATAL ANY)
        ]] signing_script @ONLY)
        install(CODE "${signing_script}")
    elseif(TEA_SIGNING_TIMESTAMP_URL)
        message(FATAL_ERROR "Set TEA_SIGNING_CERTIFICATE to select the release signing certificate.")
    endif()

    set(CPACK_GENERATOR ZIP)
    set(CPACK_SYSTEM_NAME "windows-${WEBVIEW_ARCH}")
    set(CPACK_PACKAGE_CHECKSUM SHA256)
    include(CPack)
endif()
