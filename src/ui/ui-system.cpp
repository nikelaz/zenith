#include "ui-system.h"
#include "card.h"
#include "application-icon.h"
#include "chat-panel.h"
#include "dock-area.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "threads-panel.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#ifdef ZENITH_HAS_WAYLAND_WINDOW_DRAG
extern "C" int zenith_begin_wayland_window_drag(GLFWwindow* window);
#endif

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

unsigned int create_menu_icon_texture() {
    unsigned int texture = 0;
    glGenTextures(1, &texture);
    if (texture == 0)
        return 0;

    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, application_icon::width,
                 application_icon::height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 application_icon::pixels);
    glBindTexture(GL_TEXTURE_2D, 0);
    return texture;
}

enum class WindowControlIcon {
    Minimize,
    Maximize,
    Close
};

bool window_control_button(const char* id, WindowControlIcon icon,
                           float width, float height) {
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(width, height));
    const ImVec2 button_min = ImGui::GetItemRectMin();
    const ImVec2 button_max = ImGui::GetItemRectMax();
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    if (hovered || active) {
        const ImGuiCol background = active ? ImGuiCol_ButtonActive
                                           : ImGuiCol_ButtonHovered;
        draw_list->AddRectFilled(button_min, button_max,
                                 ImGui::GetColorU32(background));
    }

    const ImVec2 center((button_min.x + button_max.x) * 0.5f,
                        (button_min.y + button_max.y) * 0.5f);
    const ImU32 color = ImGui::GetColorU32(
        hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    constexpr float stroke_width = 1.4f;
    if (icon == WindowControlIcon::Minimize) {
        constexpr float half_line_width = 4.7f;
        draw_list->AddLine(ImVec2(center.x - half_line_width, center.y),
                           ImVec2(center.x + half_line_width, center.y),
                           color, stroke_width);
    } else if (icon == WindowControlIcon::Maximize) {
        constexpr float half_icon_size = 4.5f;
        draw_list->AddRect(
            ImVec2(center.x - half_icon_size, center.y - half_icon_size),
            ImVec2(center.x + half_icon_size, center.y + half_icon_size),
            color, 1.5f, 0, stroke_width);
    } else {
        draw_list->AddLine(
            ImVec2(center.x - 4.0f, center.y - 4.0f),
            ImVec2(center.x + 4.0f, center.y + 4.0f), color, stroke_width);
        draw_list->AddLine(
            ImVec2(center.x + 4.0f, center.y - 4.0f),
            ImVec2(center.x - 4.0f, center.y + 4.0f), color, stroke_width);
    }
    return clicked;
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

    m_menu_icon_texture = create_menu_icon_texture();
    m_initialized = true;

    return result_ok();
}

void UISystem::deinit() {
    close_settings_window();
    glfwMakeContextCurrent(m_window);
    ImGui::SetCurrentContext(m_main_context);
    if (m_menu_icon_texture != 0) {
        glDeleteTextures(1, &m_menu_icon_texture);
        m_menu_icon_texture = 0;
    }
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
        const ImVec2 menu_row_pos = ImGui::GetCursorScreenPos();
        const float menu_row_height = ImGui::GetFrameHeight();
        float next_item_x = menu_row_pos.x;
        if (m_menu_icon_texture != 0) {
            constexpr float icon_size = 16.0f;
            constexpr float icon_label_spacing = 6.0f;
            ImGui::GetWindowDrawList()->AddImage(
                ImTextureRef(static_cast<ImTextureID>(m_menu_icon_texture)),
                ImVec2(next_item_x, menu_row_pos.y + (menu_row_height - icon_size) * 0.5f),
                ImVec2(next_item_x + icon_size,
                       menu_row_pos.y + (menu_row_height + icon_size) * 0.5f));
            next_item_x += icon_size + icon_label_spacing;

            constexpr const char* app_name = "Zenith";
            const ImVec2 label_size = ImGui::CalcTextSize(app_name);
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(next_item_x,
                       menu_row_pos.y + (menu_row_height - label_size.y) * 0.5f),
                ImGui::GetColorU32(ImGuiCol_Text), app_name);
            next_item_x += label_size.x + 12.0f;
            ImGui::SetCursorScreenPos(ImVec2(next_item_x, menu_row_pos.y));
        }
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open Project..."))
                open_project_dialog(m_state);
            ImGui::Separator();
            if (ImGui::MenuItem("Settings"))
                m_open_settings_requested = true;
            if (ImGui::MenuItem("Close"))
                glfwSetWindowShouldClose(m_window, GLFW_TRUE);
            ImGui::EndMenu();
        }

        const ImGuiStyle& style = ImGui::GetStyle();
        constexpr float control_width = 36.0f;
        constexpr float control_count = 3.0f;
        const ImVec2 menu_window_pos = ImGui::GetWindowPos();
        const ImVec2 menu_window_size = ImGui::GetWindowSize();
        const float controls_right = menu_window_pos.x + menu_window_size.x -
                                     style.WindowBorderSize;
        const float controls_left = controls_right - control_width * control_count;
        ImGui::SetCursorScreenPos(ImVec2(controls_left, menu_row_pos.y));
        if (window_control_button("##MinimizeWindow", WindowControlIcon::Minimize,
                                  control_width, menu_row_height))
            glfwIconifyWindow(m_window);
        ImGui::SetCursorScreenPos(ImVec2(controls_left + control_width,
                                         menu_row_pos.y));
        if (window_control_button("##MaximizeWindow", WindowControlIcon::Maximize,
                                  control_width, menu_row_height)) {
            if (glfwGetWindowAttrib(m_window, GLFW_MAXIMIZED))
                glfwRestoreWindow(m_window);
            else
                glfwMaximizeWindow(m_window);
        }
        ImGui::SetCursorScreenPos(ImVec2(controls_left + control_width * 2.0f,
                                         menu_row_pos.y));
        if (window_control_button("##CloseWindow", WindowControlIcon::Close,
                                  control_width, menu_row_height))
            glfwSetWindowShouldClose(m_window, GLFW_TRUE);

        const ImVec2 mouse_pos = ImGui::GetMousePos();
        const ImVec2 menu_window_max(menu_window_pos.x + menu_window_size.x,
                                     menu_row_pos.y + menu_row_height);
        const bool mouse_in_title_bar =
            mouse_pos.x >= menu_window_pos.x && mouse_pos.x < menu_window_max.x &&
            mouse_pos.y >= menu_row_pos.y && mouse_pos.y < menu_window_max.y;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            m_dragging_title_bar = mouse_in_title_bar &&
                                   !ImGui::IsAnyItemHovered() &&
                                   !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup);
#ifdef ZENITH_HAS_WAYLAND_WINDOW_DRAG
            if (m_dragging_title_bar && glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
                zenith_begin_wayland_window_drag(m_window);
                m_dragging_title_bar = false;
            }
#endif
            if (m_dragging_title_bar) {
                glfwGetWindowPos(m_window, &m_title_bar_drag_window_x,
                                 &m_title_bar_drag_window_y);
                double cursor_x = 0.0;
                double cursor_y = 0.0;
                glfwGetCursorPos(m_window, &cursor_x, &cursor_y);
                m_title_bar_drag_cursor_x = m_title_bar_drag_window_x + cursor_x;
                m_title_bar_drag_cursor_y = m_title_bar_drag_window_y + cursor_y;
            }
        }
        if (m_dragging_title_bar) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                int window_x = 0;
                int window_y = 0;
                double cursor_x = 0.0;
                double cursor_y = 0.0;
                glfwGetWindowPos(m_window, &window_x, &window_y);
                glfwGetCursorPos(m_window, &cursor_x, &cursor_y);
                const double cursor_screen_x = window_x + cursor_x;
                const double cursor_screen_y = window_y + cursor_y;
                const int target_x = m_title_bar_drag_window_x +
                    static_cast<int>(std::lround(cursor_screen_x -
                                                 m_title_bar_drag_cursor_x));
                const int target_y = m_title_bar_drag_window_y +
                    static_cast<int>(std::lround(cursor_screen_y -
                                                 m_title_bar_drag_cursor_y));
                glfwSetWindowPos(m_window, target_x, target_y);
            } else {
                m_dragging_title_bar = false;
            }
        }
        ImGui::EndMainMenuBar();
    }
    render_dock_area();
    render_threads_panel(m_state);
    render_chat_panel(m_state, m_message_input, m_provider, m_selected_model,
                      m_selected_reasoning_effort, m_is_generating, m_active_turn_id,
                      m_next_turn_id, m_monospace_font);

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 panel_area_max(viewport->WorkPos.x + viewport->WorkSize.x,
                                viewport->WorkPos.y + viewport->WorkSize.y);
    ImGui::GetForegroundDrawList(viewport)->AddRect(
        viewport->WorkPos, panel_area_max,
        ImGui::GetColorU32(ImGuiCol_Border), 0.0f, 0,
        ImGui::GetStyle().WindowBorderSize);

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
