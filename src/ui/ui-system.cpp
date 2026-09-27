#include "ui-system.h"
#include "card.h"
#include "chat-panel.h"
#include "dock-area.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "threads-panel.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cfloat>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace {
std::filesystem::path executable_directory() {
#ifdef _WIN32
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
                                            static_cast<DWORD>(executable.size()));
    if (length > 0 && length < static_cast<DWORD>(executable.size())) {
        executable.resize(length);
        return std::filesystem::path(executable).parent_path();
    }
    return std::filesystem::current_path();
#elif defined(__APPLE__)
    uint32_t length = 0;
    _NSGetExecutablePath(nullptr, &length);
    std::vector<char> executable(length);
    if (length > 0 && _NSGetExecutablePath(executable.data(), &length) == 0)
        return std::filesystem::path(executable.data()).parent_path();
    return std::filesystem::current_path();
#elif defined(__linux__)
    std::error_code error;
    const std::filesystem::path executable = std::filesystem::read_symlink(
        "/proc/self/exe", error);
    return error ? std::filesystem::current_path() : executable.parent_path();
#else
    return std::filesystem::current_path();
#endif
}

std::filesystem::path bundled_font_path(const char* family, const char* filename) {
    const std::filesystem::path binary_directory = executable_directory();
    std::vector<std::filesystem::path> font_directories;
#ifdef __APPLE__
    font_directories.push_back(binary_directory.parent_path() / "Resources" / "fonts");
#endif
    font_directories.push_back(binary_directory / "assets" / "fonts");
    font_directories.push_back(std::filesystem::current_path() / "assets" / "fonts");
    for (const std::filesystem::path& directory : font_directories) {
        const std::filesystem::path path = directory / family / filename;
        if (std::filesystem::is_regular_file(path))
            return path;
    }
    return {};
}

ImFont* load_bundled_font(const char* family, const char* filename, bool pixel_snap) {
    const std::filesystem::path path = bundled_font_path(family, filename);
    if (path.empty())
        return nullptr;
    if (pixel_snap) {
        ImFontConfig config;
        config.PixelSnapH = true;
        config.OversampleH = 1;
        config.OversampleV = 1;
        config.RasterizerMultiply = 1.1f;
        return ImGui::GetIO().Fonts->AddFontFromFileTTF(path.string().c_str(), 16.0f, &config);
    }
    return ImGui::GetIO().Fonts->AddFontFromFileTTF(path.string().c_str(), 16.0f);
}

void set_premiere_theme() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.Alpha = 1.0f;
    style.DisabledAlpha = 0.55f;
    style.WindowPadding = ImVec2(12.0f, 12.0f);
    style.WindowRounding = 0.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildRounding = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupRounding = 6.0f;
    style.PopupBorderSize = 1.0f;
    style.MenuItemRounding = 6.0f;
    style.FramePadding = ImVec2(10.0f, 6.0f);
    style.FrameRounding = 0.0f;
    style.FrameBorderSize = 0.0f;
    style.SelectableRounding = 6.0f;
    style.ItemSpacing = ImVec2(10.0f, 8.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    style.ScrollbarSize = 8.0f;
    style.ScrollbarRounding = style.ScrollbarSize * 0.5f;
    style.GrabMinSize = 10.0f;
    style.GrabRounding = 6.0f;
    style.TabRounding = 0.0f;
    style.TabBorderSize = 0.0f;

    const ImVec4 background(0.075f, 0.075f, 0.075f, 1.0f);
    const ImVec4 panel(0.105f, 0.105f, 0.105f, 1.0f);
    const ImVec4 raised(0.145f, 0.145f, 0.145f, 1.0f);
    const ImVec4 hover(0.20f, 0.20f, 0.20f, 1.0f);
    const ImVec4 border(0.25f, 0.25f, 0.25f, 1.0f);
    const ImVec4 accent(1.0f, 1.0f, 1.0f, 1.0f);
    const ImVec4 accent_hover(0.86f, 0.86f, 0.86f, 1.0f);
    ImVec4* colors = style.Colors;

    colors[ImGuiCol_Text] = ImVec4(0.91f, 0.91f, 0.91f, 1.0f);
    colors[ImGuiCol_TextLink] = ImVec4(0.82f, 0.84f, 0.90f, 1.0f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
    colors[ImGuiCol_WindowBg] = background;
    colors[ImGuiCol_ChildBg] = panel;
    colors[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.12f, 0.12f, 0.98f);
    colors[ImGuiCol_Border] = border;
    colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_FrameBg] = raised;
    colors[ImGuiCol_FrameBgHovered] = hover;
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.23f, 0.23f, 0.23f, 1.0f);
    colors[ImGuiCol_TitleBg] = panel;
    colors[ImGuiCol_TitleBgActive] = raised;
    colors[ImGuiCol_TitleBgCollapsed] = panel;
    colors[ImGuiCol_MenuBarBg] = panel;
    colors[ImGuiCol_ScrollbarBg] = background;
    colors[ImGuiCol_ScrollbarGrab] = border;
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.34f, 0.34f, 0.34f, 1.0f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.42f, 0.42f, 0.42f, 1.0f);
    colors[ImGuiCol_CheckMark] = accent_hover;
    colors[ImGuiCol_SliderGrab] = accent;
    colors[ImGuiCol_SliderGrabActive] = accent_hover;
    colors[ImGuiCol_Button] = raised;
    colors[ImGuiCol_ButtonHovered] = hover;
    colors[ImGuiCol_ButtonActive] = ImVec4(0.24f, 0.24f, 0.24f, 1.0f);
    colors[ImGuiCol_Header] = raised;
    colors[ImGuiCol_HeaderHovered] = hover;
    colors[ImGuiCol_HeaderActive] = ImVec4(0.28f, 0.28f, 0.28f, 1.0f);
    colors[ImGuiCol_Separator] = border;
    colors[ImGuiCol_SeparatorHovered] = accent;
    colors[ImGuiCol_SeparatorActive] = accent_hover;
    colors[ImGuiCol_ResizeGrip] = ImVec4(accent.x, accent.y, accent.z, 0.25f);
    colors[ImGuiCol_ResizeGripHovered] = accent;
    colors[ImGuiCol_ResizeGripActive] = accent_hover;
    colors[ImGuiCol_Tab] = panel;
    colors[ImGuiCol_TabHovered] = hover;
    colors[ImGuiCol_TabSelected] = raised;
    colors[ImGuiCol_TabSelectedOverline] = accent;
    colors[ImGuiCol_TabDimmed] = background;
    colors[ImGuiCol_TabDimmedSelected] = panel;
    colors[ImGuiCol_DockingPreview] = ImVec4(accent.x, accent.y, accent.z, 0.65f);
    colors[ImGuiCol_DockingEmptyBg] = background;
    colors[ImGuiCol_TextSelectedBg] = ImVec4(accent.x, accent.y, accent.z, 0.35f);
    colors[ImGuiCol_NavHighlight] = accent_hover;
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.55f);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.65f);
}

template <typename EventFn>
void add_settings_input(GLFWwindow* window, EventFn&& add_event) {
    ImGuiContext* context = static_cast<ImGuiContext*>(glfwGetWindowUserPointer(window));
    if (context == nullptr)
        return;

    ImGuiContext* previous_context = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(context);
    add_event(ImGui::GetIO());
    ImGui::SetCurrentContext(previous_context);
}

void settings_cursor_position_callback(GLFWwindow* window, double x, double y) {
    add_settings_input(window, [x, y](ImGuiIO& io) {
        io.AddMousePosEvent(static_cast<float>(x), static_cast<float>(y));
    });
}

void settings_cursor_enter_callback(GLFWwindow* window, int entered) {
    if (entered == GLFW_TRUE) {
        double x;
        double y;
        glfwGetCursorPos(window, &x, &y);
        settings_cursor_position_callback(window, x, y);
    } else {
        add_settings_input(window, [](ImGuiIO& io) {
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        });
    }
}

void settings_mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
    (void)mods;
    if (button < 0 || button >= ImGuiMouseButton_COUNT)
        return;
    add_settings_input(window, [button, action](ImGuiIO& io) {
        io.AddMouseButtonEvent(button, action == GLFW_PRESS);
    });
}

void settings_scroll_callback(GLFWwindow* window, double x_offset, double y_offset) {
    add_settings_input(window, [x_offset, y_offset](ImGuiIO& io) {
        io.AddMouseWheelEvent(static_cast<float>(x_offset), static_cast<float>(y_offset));
    });
}

void settings_focus_callback(GLFWwindow* window, int focused) {
    add_settings_input(window, [focused](ImGuiIO& io) {
        io.AddFocusEvent(focused == GLFW_TRUE);
    });
}
}

UISystem::UISystem(GLFWwindow* window, ApplicationState& state, Provider& provider)
    : m_window(window), m_state(state), m_provider(provider),
      m_selected_model(provider.default_model) {}

Result UISystem::init() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    m_main_context = ImGui::GetCurrentContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImFont* interface_font = load_bundled_font(
        "IBM-Plex-Sans", "IBMPlexSans-Regular.ttf", false);
    if (interface_font == nullptr)
        interface_font = io.Fonts->AddFontDefault();
    io.FontDefault = interface_font;
    m_monospace_font = load_bundled_font(
        "JetBrains-Mono", "JetBrainsMono-Regular.ttf", true);
    set_premiere_theme();

    if (!ImGui_ImplGlfw_InitForOpenGL(m_window, true)) {
        ImGui::DestroyContext();
        m_main_context = nullptr;
        return result_error("Failed to initialize Dear ImGui Glfw OpenGL backend");
    }

    if (!ImGui_ImplOpenGL3_Init("#version 150")) {
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        m_main_context = nullptr;
        return result_error("Failed to initialize Dear ImGui OpenGL 3 backend");
    }

    m_initialized = true;

    return result_ok();
}

void UISystem::deinit() {
    close_settings_window();
    glfwMakeContextCurrent(m_window);
    ImGui::SetCurrentContext(m_main_context);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    m_main_context = nullptr;
    m_initialized = false;
}

void UISystem::new_frame() {
    glfwMakeContextCurrent(m_window);
    ImGui::SetCurrentContext(m_main_context);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void UISystem::prepare_backbuffer() {
    ImGui::Render();

    int width;
    int height;
    glfwGetFramebufferSize(m_window, &width, &height);
    glViewport(0, 0, width, height);
    // TODO: This color should be a part of the pallete/theme
    glClearColor(0.075f, 0.075f, 0.075f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void UISystem::render_frame_to_backbuffer() {
    if (m_open_settings_requested) {
        m_open_settings_requested = false;
        open_settings_window();
    }

    new_frame();

    for (const Event& event : m_provider.poll_events(&m_provider)) {
        try {
            ChatThread* thread = nullptr;
            for (ChatProject& project : m_state.projects) {
                const auto match = std::find_if(project.threads.begin(), project.threads.end(),
                    [&event](const ChatThread& value) {
                        return value.id == event.conversation_id;
                    });
                if (match != project.threads.end()) {
                    thread = &*match;
                    break;
                }
            }
            if (thread == nullptr)
                continue;
            auto& messages = thread->messages;
            if (event.kind == EventKind::ReasoningSummaryDelta ||
                event.kind == EventKind::AssistantReasoningDelta ||
                event.kind == EventKind::ToolActivity) {
                if (event.kind == EventKind::ReasoningSummaryDelta ||
                    event.kind == EventKind::AssistantReasoningDelta) {
                    if (messages.empty() || messages.back().role != ChatMessageRole::Assistant)
                        messages.push_back({ChatMessageRole::Assistant, {}, {}, {}, {}});
                    messages.back().reasoning += event.text;
                } else {
                    if (messages.empty() || messages.back().role != ChatMessageRole::Assistant)
                        messages.push_back({ChatMessageRole::Assistant, {}, {}, {}, {}});
                    ChatMessage& message = messages.back();
                    auto segment = message.segments.end();
                    if (!event.item_id.empty()) {
                        segment = std::find_if(message.segments.begin(), message.segments.end(),
                                               [&event](const ChatSegment& value) {
                                                   return value.kind == ChatSegment::Kind::Tool &&
                                                          value.tool.id == event.item_id;
                                               });
                    }
                    if (segment == message.segments.end()) {
                        ChatSegment value;
                        value.kind = ChatSegment::Kind::Tool;
                        value.tool.id = event.item_id;
                        message.segments.push_back(std::move(value));
                        segment = std::prev(message.segments.end());
                    }
                    if (!event.tool_name.empty())
                        segment->tool.name = event.tool_name;
                    if (!event.text.empty())
                        segment->tool.command = event.text;
                    if (!event.tool_arguments.empty())
                        segment->tool.arguments = event.tool_arguments;
                    if (event.is_terminal)
                        segment->tool.is_terminal = true;
                    if (!event.cwd.empty())
                        segment->tool.cwd = event.cwd;
                    if (!event.output.empty()) {
                        if (event.output_is_delta)
                            segment->tool.output += event.output;
                        else if (segment->tool.output.empty())
                            segment->tool.output = event.output;
                    }
                    if (!event.status.empty())
                        segment->tool.status = event.status;
                    if (event.exit_code >= 0)
                        segment->tool.exit_code = event.exit_code;
                    if (event.duration_ms >= 0)
                        segment->tool.duration_ms = event.duration_ms;
                    if (event.tool_completed)
                        segment->tool.completed = true;
                }
            } else if (event.kind == EventKind::AssistantTextDelta) {
                if (messages.empty() || messages.back().role != ChatMessageRole::Assistant)
                    messages.push_back({ChatMessageRole::Assistant, {}, {}, {}, {}});
                ChatMessage& message = messages.back();
                message.content += event.text;
                if (message.segments.empty() || message.segments.back().kind != ChatSegment::Kind::Text)
                    message.segments.push_back({ChatSegment::Kind::Text, {}, {}});
                message.segments.back().text += event.text;
            } else if (event.kind == EventKind::TurnFailed) {
                if (event.turn_id == m_active_turn_id) {
                    m_is_generating = false;
                    m_active_turn_id = 0;
                }
                messages.push_back({ChatMessageRole::Assistant, event.text, {}, {}, {}});
            } else if (event.kind == EventKind::TurnCompleted) {
                if (event.turn_id == m_active_turn_id) {
                    m_is_generating = false;
                    m_active_turn_id = 0;
                }
            }
        } catch (...) {
        }
    }
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Settings"))
                m_open_settings_requested = true;
            if (ImGui::MenuItem("Close"))
                glfwSetWindowShouldClose(m_window, GLFW_TRUE);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
    render_dock_area();
    render_threads_panel(m_state);
    render_chat_panel(m_state, m_message_input, m_provider, m_selected_model,
                      m_selected_reasoning_effort, m_is_generating, m_active_turn_id,
                      m_next_turn_id, m_monospace_font);

    prepare_backbuffer();
    render_settings_window();
}

bool UISystem::open_settings_window() {
    if (m_settings_window != nullptr) {
        glfwShowWindow(m_settings_window);
        glfwFocusWindow(m_settings_window);
        return true;
    }

    glfwMakeContextCurrent(m_window);
    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif

    GLFWwindow* settings_window = glfwCreateWindow(760, 560, "Zenith Settings", nullptr,
                                                   m_window);
    if (settings_window == nullptr) {
        glfwMakeContextCurrent(m_window);
        return false;
    }

    ImGui::SetCurrentContext(m_main_context);
    ImGuiContext* settings_context = ImGui::CreateContext(ImGui::GetIO().Fonts);
    ImGui::SetCurrentContext(settings_context);
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    set_premiere_theme();

    m_settings_window = settings_window;
    m_settings_context = settings_context;
    glfwSetWindowUserPointer(m_settings_window, m_settings_context);
    glfwSetCursorPosCallback(m_settings_window, settings_cursor_position_callback);
    glfwSetCursorEnterCallback(m_settings_window, settings_cursor_enter_callback);
    glfwSetMouseButtonCallback(m_settings_window, settings_mouse_button_callback);
    glfwSetScrollCallback(m_settings_window, settings_scroll_callback);
    glfwSetWindowFocusCallback(m_settings_window, settings_focus_callback);
    glfwMakeContextCurrent(m_window);
    ImGui::SetCurrentContext(m_main_context);
    glfwShowWindow(m_settings_window);
    glfwFocusWindow(m_settings_window);
    return true;
}

void UISystem::close_settings_window() {
    if (m_settings_window == nullptr)
        return;

    ImGui::SetCurrentContext(m_settings_context);
    ImGui::DestroyContext(m_settings_context);
    glfwDestroyWindow(m_settings_window);
    m_settings_window = nullptr;
    m_settings_context = nullptr;
    m_settings_last_frame_time = 0.0;
    glfwMakeContextCurrent(m_window);
    ImGui::SetCurrentContext(m_main_context);
}

void UISystem::render_settings_window() {
    if (m_settings_window == nullptr)
        return;
    if (glfwWindowShouldClose(m_settings_window)) {
        close_settings_window();
        return;
    }

    glfwMakeContextCurrent(m_settings_window);
    ImGui::SetCurrentContext(m_settings_context);
    ImGuiIO& io = ImGui::GetIO();
    int window_width;
    int window_height;
    int framebuffer_width;
    int framebuffer_height;
    glfwGetWindowSize(m_settings_window, &window_width, &window_height);
    glfwGetFramebufferSize(m_settings_window, &framebuffer_width, &framebuffer_height);
    io.DisplaySize = ImVec2(static_cast<float>(window_width), static_cast<float>(window_height));
    io.DisplayFramebufferScale = ImVec2(
        window_width > 0 ? static_cast<float>(framebuffer_width) / window_width : 1.0f,
        window_height > 0 ? static_cast<float>(framebuffer_height) / window_height : 1.0f);
    const double current_time = glfwGetTime();
    io.DeltaTime = m_settings_last_frame_time > 0.0
                       ? std::max(0.001f, static_cast<float>(current_time - m_settings_last_frame_time))
                       : 1.0f / 60.0f;
    m_settings_last_frame_time = current_time;
    ImGui::NewFrame();
    render_settings_contents();
    ImGui::Render();
    ImDrawData* settings_draw_data = ImGui::GetDrawData();

    int width;
    int height;
    glfwGetFramebufferSize(m_settings_window, &width, &height);
    ImGui::SetCurrentContext(m_main_context);
    glfwMakeContextCurrent(m_settings_window);
    glViewport(0, 0, width, height);
    glClearColor(0.075f, 0.075f, 0.075f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(settings_draw_data);
    glfwSwapBuffers(m_settings_window);

    glfwMakeContextCurrent(m_window);
    ImGui::SetCurrentContext(m_main_context);
}

void UISystem::render_settings_contents() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    constexpr ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar |
                                              ImGuiWindowFlags_NoResize |
                                              ImGuiWindowFlags_NoMove |
                                              ImGuiWindowFlags_NoCollapse |
                                              ImGuiWindowFlags_NoSavedSettings |
                                              ImGuiWindowFlags_NoScrollbar |
                                              ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("Settings", nullptr, window_flags);

    ImGui::BeginChild("##settings_sidebar", ImVec2(180.0f, 0.0f),
                      ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::TextDisabled("SETTINGS");
    ImGui::Spacing();
    ImGui::Selectable("Providers", true);
    ImGui::EndChild();
    ImGui::SameLine();

    const char* availability = "Unknown";
    ImVec4 availability_color = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
    if (m_provider.availability == ProviderAvailability::Available) {
        availability = "Available";
        availability_color = ImVec4(0.42f, 0.78f, 0.50f, 1.0f);
    } else if (m_provider.availability == ProviderAvailability::Unavailable) {
        availability = "Unavailable";
        availability_color = ImVec4(0.90f, 0.38f, 0.34f, 1.0f);
    }

    if (begin_ui_card("##codex_provider")) {
        ImGui::TextUnformatted("Codex");
        ImGui::Spacing();
        ImGui::TextDisabled("Status");
        ImGui::SameLine(112.0f);
        ImGui::TextColored(availability_color, "%s", availability);
        ImGui::TextDisabled("Location");
        ImGui::SameLine(112.0f);
        if (m_provider.location.empty())
            ImGui::TextWrapped("Codex executable not found");
        else
            ImGui::TextWrapped("%s", m_provider.location.string().c_str());
    }
    end_ui_card();

    ImGui::End();
}
