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

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/glfw/CMakeLists.txt")
    message(FATAL_ERROR "Dependencies are missing. Run: git submodule update --init --recursive")
endif()

set(GLFW_BUILD_DOCS OFF CACHE BOOL "Build GLFW documentation" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "Build GLFW tests" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "Build GLFW examples" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "Install GLFW" FORCE)

add_subdirectory(third_party/glfw EXCLUDE_FROM_ALL)
if(GLFW_BUILD_WAYLAND)
    target_sources(glfw PRIVATE src/platform/glfw-wayland-window-drag.c)
endif()
if(GLFW_BUILD_WIN32)
    target_sources(glfw PRIVATE src/platform/glfw-win32-window-drag.c)
endif()

find_package(OpenGL REQUIRED)

set_source_files_properties(third_party/tinyfiledialogs/tinyfiledialogs.c PROPERTIES LANGUAGE CXX)
add_subdirectory(third_party/sqlite3 EXCLUDE_FROM_ALL)

add_subdirectory(third_party/googletest EXCLUDE_FROM_ALL)

if(WIN32)
    set(CHILD_PROCESS_SOURCE src/process/child-process-win32.cpp)
else()
    set(CHILD_PROCESS_SOURCE src/process/child-process-posix.cpp)
endif()

if(WIN32)
    set(CLIPBOARD_IMAGE_SOURCE src/platform/clipboard-image-win32.cpp)
elseif(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 20)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    set(CLIPBOARD_IMAGE_SOURCE src/platform/clipboard-image-macos.mm)
else()
    set(CLIPBOARD_IMAGE_SOURCE src/platform/clipboard-image-linux.cpp)
endif()

add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/misc/cpp/imgui_stdlib.cpp
    third_party/imgui/backends/imgui_impl_glfw.cpp
    third_party/imgui/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
    third_party/imgui
    third_party/imgui/backends
)
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

add_library(md4c STATIC third_party/md4c/src/md4c.c)
target_include_directories(md4c PUBLIC third_party/md4c/src)

add_library(imgui_md STATIC third_party/imgui_md/imgui_md.cpp)
target_include_directories(imgui_md PUBLIC third_party/imgui_md)
target_link_libraries(imgui_md PUBLIC imgui md4c)

add_executable(Zenith
    src/main.cpp
    src/base/result.cpp
    ${CHILD_PROCESS_SOURCE}
    ${CLIPBOARD_IMAGE_SOURCE}
    src/providers/provider_runtime.cpp
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
    third_party/tinyfiledialogs/tinyfiledialogs.c
)
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
target_include_directories(Zenith PRIVATE third_party/tinyfiledialogs third_party/sqlite3 third_party/json/include)
target_link_libraries(Zenith PRIVATE imgui_md SQLite3)
if(WIN32)
    target_link_libraries(Zenith PRIVATE gdiplus ole32)
elseif(APPLE)
    target_link_libraries(Zenith PRIVATE "-framework AppKit")
endif()
if(GLFW_BUILD_WAYLAND)
    target_compile_definitions(Zenith PRIVATE ZENITH_HAS_WAYLAND_WINDOW_DRAG)
endif()
if(GLFW_BUILD_WIN32)
    target_compile_definitions(Zenith PRIVATE ZENITH_HAS_WIN32_WINDOW_DRAG)
endif()

if(APPLE)
    target_compile_definitions(Zenith PRIVATE GL_SILENCE_DEPRECATION)
endif()

set_project_warnings(Zenith)

include(CTest)
if(BUILD_TESTING)
    add_executable(tests tests/provider_tests.cpp src/base/result.cpp ${CHILD_PROCESS_SOURCE} src/providers/provider_runtime.cpp src/providers/codex_provider.cpp src/providers/github_copilot_provider.cpp src/providers/fake_provider.cpp src/persistence/persistent-store.cpp)
    target_include_directories(tests PRIVATE third_party/json/include third_party/sqlite3)
    target_link_libraries(tests PRIVATE GTest::gtest_main SQLite3)
    set_project_warnings(tests)
    include(GoogleTest)
    gtest_discover_tests(tests)
endif()
