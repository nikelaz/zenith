cmake_minimum_required(VERSION 3.24)

project(Zenith VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

function(set_project_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL/CMakeLists.txt")
    message(FATAL_ERROR "Dependencies are missing. Run: git submodule update --init --recursive")
endif()

set(SDL_SHARED OFF CACHE BOOL "Build SDL shared library" FORCE)
set(SDL_STATIC ON CACHE BOOL "Build SDL static library" FORCE)
set(SDL_TEST OFF CACHE BOOL "Build SDL tests" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "Build SDL examples" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "Install SDL" FORCE)
set(SDL_OPENGL OFF CACHE BOOL "Build SDL OpenGL support" FORCE)
set(SDL_OPENGLES OFF CACHE BOOL "Build SDL OpenGL ES support" FORCE)
set(SDL_RENDER OFF CACHE BOOL "Build SDL 2D renderer" FORCE)
set(SDL_RENDER_GPU OFF CACHE BOOL "Build SDL GPU 2D renderer" FORCE)
set(SDL_X11_XSCRNSAVER OFF CACHE BOOL "Build SDL X11 screensaver support" FORCE)
set(SDL_X11_XTEST OFF CACHE BOOL "Build SDL X11 test support" FORCE)
add_subdirectory(third_party/SDL EXCLUDE_FROM_ALL)

add_subdirectory(third_party/sqlite3 EXCLUDE_FROM_ALL)

add_subdirectory(third_party/googletest EXCLUDE_FROM_ALL)

if(WIN32)
    set(CHILD_PROCESS_SOURCE src/process/child-process-win32.cpp)
else()
    set(CHILD_PROCESS_SOURCE src/process/child-process-posix.cpp)
endif()

add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/misc/cpp/imgui_stdlib.cpp
    third_party/imgui/backends/imgui_impl_sdl3.cpp
    third_party/imgui/backends/imgui_impl_sdlgpu3.cpp
)
target_include_directories(imgui PUBLIC
    third_party/imgui
    third_party/imgui/backends
)
target_link_libraries(imgui PUBLIC SDL3::SDL3-static)

add_library(md4c STATIC third_party/md4c/src/md4c.c)
target_include_directories(md4c PUBLIC third_party/md4c/src)

add_library(imgui_md STATIC third_party/imgui_md/imgui_md.cpp)
target_include_directories(imgui_md PUBLIC third_party/imgui_md)
target_link_libraries(imgui_md PUBLIC imgui md4c)

add_executable(Zenith
    src/main.cpp
    src/base/result.cpp
    ${CHILD_PROCESS_SOURCE}
    src/platform/clipboard-image.cpp
    src/platform/application-paths.cpp
    src/platform/file-dialogs.cpp
    src/platform/message-box.cpp
    src/providers/provider_runtime.cpp
    src/providers/provider_mcp_utils.cpp
    src/providers/codex_provider.cpp
    src/providers/github_copilot_provider.cpp
    src/providers/fake_provider.cpp
    src/application.cpp
    src/persistence/persistent-store.cpp
    src/ui/card.cpp
    src/ui/chat-panel.cpp
    src/ui/dock-area.cpp
    src/ui/threads-panel.cpp
    src/ui/ui-system.cpp
)
if(WIN32)
    file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/zenith.ico" ZENITH_WINDOWS_ICON_PATH)
    configure_file(src/platform/zenith.rc.in "${CMAKE_CURRENT_BINARY_DIR}/zenith.rc" @ONLY)
    target_sources(Zenith PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/zenith.rc")
    set_target_properties(Zenith PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
if(APPLE)
    set_target_properties(Zenith PROPERTIES MACOSX_BUNDLE TRUE)
    add_custom_command(TARGET Zenith POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_DIR:Zenith>/Contents/Resources/fonts"
            "$<TARGET_BUNDLE_DIR:Zenith>/Contents/Resources/icons"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts"
            "$<TARGET_BUNDLE_DIR:Zenith>/Contents/Resources/fonts"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons"
            "$<TARGET_BUNDLE_DIR:Zenith>/Contents/Resources/icons"
    )
else()
    add_custom_command(TARGET Zenith POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:Zenith>/assets/fonts"
            "$<TARGET_FILE_DIR:Zenith>/assets/icons"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts"
            "$<TARGET_FILE_DIR:Zenith>/assets/fonts"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons"
            "$<TARGET_FILE_DIR:Zenith>/assets/icons"
    )
endif()
target_include_directories(Zenith PRIVATE third_party/sqlite3 third_party/json/include)
target_link_libraries(Zenith PRIVATE imgui_md SQLite3)
if(WIN32)
    target_link_libraries(Zenith PRIVATE gdiplus ole32 shell32)
elseif(APPLE)
    target_link_libraries(Zenith PRIVATE "-framework AppKit")
endif()
set_project_warnings(Zenith)

include(CTest)
if(BUILD_TESTING)
    add_executable(tests tests/provider_tests.cpp src/base/result.cpp ${CHILD_PROCESS_SOURCE} src/providers/provider_runtime.cpp src/providers/provider_mcp_utils.cpp src/providers/codex_provider.cpp src/providers/github_copilot_provider.cpp src/providers/fake_provider.cpp src/persistence/persistent-store.cpp)
    target_include_directories(tests PRIVATE third_party/json/include third_party/sqlite3)
    target_link_libraries(tests PRIVATE GTest::gtest_main SQLite3)
    set_project_warnings(tests)
    include(GoogleTest)
    gtest_discover_tests(tests)
    add_executable(chat_render_tests tests/chat_render_tests.cpp
        src/base/result.cpp src/platform/clipboard-image.cpp src/platform/file-dialogs.cpp)
    target_link_libraries(chat_render_tests PRIVATE GTest::gtest_main imgui_md)
    if(WIN32)
        target_link_libraries(chat_render_tests PRIVATE gdiplus ole32 shell32)
    elseif(APPLE)
        target_link_libraries(chat_render_tests PRIVATE "-framework AppKit")
    endif()
    set_project_warnings(chat_render_tests)
    gtest_discover_tests(chat_render_tests)
endif()
