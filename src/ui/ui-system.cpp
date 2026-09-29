#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include "ui-system.h"
#include "ui-scale.h"
#include "card.h"
#include "application-icon.h"
#include "file-attachment-icon.h"
#include "paperclip-icon.h"
#include "chat-panel.h"
#include "dock-area.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"
#include "threads-panel.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#ifdef __APPLE__
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

ImFont* load_bundled_font(const char* family, const char* filename, bool pixel_snap,
                          float font_size) {
    const std::filesystem::path path = bundled_font_path(family, filename);
    if (path.empty())
        return nullptr;
    if (pixel_snap) {
        ImFontConfig config;
        config.PixelSnapH = true;
        config.OversampleH = 1;
        config.OversampleV = 1;
        config.RasterizerMultiply = 1.1f;
        return ImGui::GetIO().Fonts->AddFontFromFileTTF(path.string().c_str(), font_size, &config);
    }
    return ImGui::GetIO().Fonts->AddFontFromFileTTF(path.string().c_str(), font_size);
}

SDL_GPUTexture* create_icon_texture(SDL_GPUDevice* device, int width, int height,
                                    const unsigned char* pixels) {
    const Uint32 byte_count = static_cast<Uint32>(width * height * 4);
    SDL_GPUTextureCreateInfo texture_info{};
    texture_info.type = SDL_GPU_TEXTURETYPE_2D;
    texture_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    texture_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    texture_info.width = static_cast<Uint32>(width);
    texture_info.height = static_cast<Uint32>(height);
    texture_info.layer_count_or_depth = 1;
    texture_info.num_levels = 1;
    texture_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture* texture = SDL_CreateGPUTexture(device, &texture_info);
    if (texture == nullptr)
        return nullptr;

    SDL_GPUTransferBufferCreateInfo transfer_info{};
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size = byte_count;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device, &transfer_info);
    if (transfer == nullptr) {
        SDL_ReleaseGPUTexture(device, texture);
        return nullptr;
    }
    void* transfer_data = SDL_MapGPUTransferBuffer(device, transfer, false);
    if (transfer_data == nullptr) {
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        SDL_ReleaseGPUTexture(device, texture);
        return nullptr;
    }
    std::memcpy(transfer_data, pixels, byte_count);
    SDL_UnmapGPUTransferBuffer(device, transfer);

    SDL_GPUCommandBuffer* command_buffer = SDL_AcquireGPUCommandBuffer(device);
    if (command_buffer == nullptr) {
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        SDL_ReleaseGPUTexture(device, texture);
        return nullptr;
    }
    SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(command_buffer);
    SDL_GPUTextureTransferInfo source{transfer, 0, static_cast<Uint32>(width),
                                      static_cast<Uint32>(height)};
    SDL_GPUTextureRegion destination{texture, 0, 0, 0, 0, 0,
                                     static_cast<Uint32>(width),
                                     static_cast<Uint32>(height), 1};
    SDL_UploadToGPUTexture(copy_pass, &source, &destination, false);
    SDL_EndGPUCopyPass(copy_pass);
    if (!SDL_SubmitGPUCommandBuffer(command_buffer)) {
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        SDL_ReleaseGPUTexture(device, texture);
        return nullptr;
    }
    SDL_ReleaseGPUTransferBuffer(device, transfer);
    return texture;
}

ImTextureID texture_id(SDL_GPUTexture* texture) {
    return static_cast<ImTextureID>(reinterpret_cast<intptr_t>(texture));
}

float window_content_scale(SDL_Window* window) {
    // ImGui sizes use window coordinates, so remove SDL's pixel density component.
    const float display_scale = SDL_GetWindowDisplayScale(window);
    const float pixel_density = SDL_GetWindowPixelDensity(window);
    if (display_scale <= 0.0f || pixel_density <= 0.0f)
        return 1.0f;
    return display_scale / pixel_density;
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
    const float stroke_width = ui_size(1.4f);
    if (icon == WindowControlIcon::Minimize) {
        const float half_line_width = ui_size(4.7f);
        draw_list->AddLine(ImVec2(center.x - half_line_width, center.y),
                           ImVec2(center.x + half_line_width, center.y),
                           color, stroke_width);
    } else if (icon == WindowControlIcon::Maximize) {
        const float half_icon_size = ui_size(4.5f);
        draw_list->AddRect(
            ImVec2(center.x - half_icon_size, center.y - half_icon_size),
            ImVec2(center.x + half_icon_size, center.y + half_icon_size),
            color, ui_size(1.5f), 0, stroke_width);
    } else {
        draw_list->AddLine(
            ImVec2(center.x - ui_size(4.0f), center.y - ui_size(4.0f)),
            ImVec2(center.x + ui_size(4.0f), center.y + ui_size(4.0f)), color, stroke_width);
        draw_list->AddLine(
            ImVec2(center.x + ui_size(4.0f), center.y - ui_size(4.0f)),
            ImVec2(center.x - ui_size(4.0f), center.y + ui_size(4.0f)), color, stroke_width);
    }
    return clicked;
}

void set_premiere_theme(const ApplicationState& state) {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
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
    style.ScaleAllSizes(state.ui_scale);
    style.FontSizeBase = static_cast<float>(state.base_font_size);
    style.FontScaleMain = state.ui_scale;
}


}

UISystem::UISystem(SDL_Window* window, SDL_GPUDevice* gpu_device,
                   ApplicationState& state, std::vector<ProviderPtr>& providers)
    : m_window(window), m_gpu_device(gpu_device), m_state(state), m_providers(providers),
      m_chat_panel_state(), m_usage_snapshots(providers.size()),
      m_usage_loading(providers.size(), false) {
    m_chat_panel_state.selected_model = providers.empty()
        ? std::string{} : providers.front()->default_model;
    for (std::size_t index = 0; index < providers.size(); ++index) {
        if (providers[index]->request_usage != nullptr && providers[index]->poll_usage != nullptr) {
            providers[index]->request_usage(providers[index].get());
            m_usage_loading[index] = true;
        }
        if (providers[index]->name == "GitHub Copilot") {
            m_chat_panel_state.selected_provider = index;
            m_chat_panel_state.selected_model = providers[index]->default_model;
            break;
        }
    }
}

Result UISystem::init() {
    m_dpi_scale = window_content_scale(m_window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    m_main_context = ImGui::GetCurrentContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    apply_appearance_settings();
    ImFontConfig font_config;
    font_config.SizePixels = static_cast<float>(m_state.base_font_size);
    io.FontDefault = load_bundled_font(
        "IBM-Plex-Sans", "IBMPlexSans-Regular.ttf", false, font_config.SizePixels);
    if (io.FontDefault == nullptr)
        io.FontDefault = io.Fonts->AddFontDefault(&font_config);
    m_chat_panel_state.monospace_font = load_bundled_font(
        "JetBrains-Mono", "JetBrainsMono-Regular.ttf", true, font_config.SizePixels);

    if (!ImGui_ImplSDL3_InitForSDLGPU(m_window)) {
        ImGui::DestroyContext();
        m_main_context = nullptr;
        return result_error("Failed to initialize Dear ImGui SDL3 backend");
    }

    ImGui_ImplSDLGPU3_InitInfo init_info{};
    init_info.Device = m_gpu_device;
    init_info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(m_gpu_device, m_window);
    init_info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    init_info.SwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
    init_info.PresentMode = SDL_GPU_PRESENTMODE_VSYNC;
    if (!ImGui_ImplSDLGPU3_Init(&init_info)) {
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        m_main_context = nullptr;
        return result_error("Failed to initialize Dear ImGui SDL GPU backend");
    }

    m_menu_icon_texture = create_icon_texture(m_gpu_device, application_icon::width,
        application_icon::height, application_icon::pixels);
    m_chat_panel_state.attachment_icon_texture = create_icon_texture(
        m_gpu_device, file_attachment_icon::width, file_attachment_icon::height,
        file_attachment_icon::pixels);
    m_chat_panel_state.paperclip_icon_texture = create_icon_texture(
        m_gpu_device, paperclip_icon::width, paperclip_icon::height,
        paperclip_icon::pixels);
    if (m_menu_icon_texture == nullptr || m_chat_panel_state.attachment_icon_texture == nullptr ||
        m_chat_panel_state.paperclip_icon_texture == nullptr) {
        deinit();
        return result_error(std::string("Failed to create UI textures: ") + SDL_GetError());
    }
    SDL_SetWindowHitTest(m_window, title_bar_hit_test, this);
    m_initialized = true;
    return result_ok();
}

SDL_HitTestResult SDLCALL UISystem::title_bar_hit_test(SDL_Window*, const SDL_Point* point,
                                                       void* user_data) {
    auto* ui = static_cast<UISystem*>(user_data);
    int window_width = 0;
    int window_height = 0;
    SDL_GetWindowSize(ui->m_window, &window_width, &window_height);
    constexpr int resize_border = 8;
    const bool left = point->x < resize_border;
    const bool right = point->x >= window_width - resize_border;
    const bool top = point->y < resize_border;
    const bool bottom = point->y >= window_height - resize_border;
    if (top && left)
        return SDL_HITTEST_RESIZE_TOPLEFT;
    if (top && right)
        return SDL_HITTEST_RESIZE_TOPRIGHT;
    if (bottom && left)
        return SDL_HITTEST_RESIZE_BOTTOMLEFT;
    if (bottom && right)
        return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    if (top)
        return SDL_HITTEST_RESIZE_TOP;
    if (bottom)
        return SDL_HITTEST_RESIZE_BOTTOM;
    if (left)
        return SDL_HITTEST_RESIZE_LEFT;
    if (right)
        return SDL_HITTEST_RESIZE_RIGHT;
    if (point->y < 0 || point->y >= ui->m_title_bar_height)
        return SDL_HITTEST_NORMAL;
    for (const SDL_Rect& bounds : ui->m_title_bar_interactive_bounds) {
        if (point->x >= bounds.x && point->x < bounds.x + bounds.w &&
            point->y >= bounds.y && point->y < bounds.y + bounds.h)
            return SDL_HITTEST_NORMAL;
    }
    return SDL_HITTEST_DRAGGABLE;
}

void UISystem::process_event(const SDL_Event& event) {
    if (m_main_context != nullptr) {
        ImGui::SetCurrentContext(m_main_context);
        ImGui_ImplSDL3_ProcessEvent(&event);
    }
    if (m_settings_context != nullptr) {
        ImGui::SetCurrentContext(m_settings_context);
        ImGui_ImplSDL3_ProcessEvent(&event);
    }
    if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && m_settings_window != nullptr &&
        event.window.windowID == SDL_GetWindowID(m_settings_window))
        m_close_settings_requested = true;
    if (m_main_context != nullptr)
        ImGui::SetCurrentContext(m_main_context);
}

void UISystem::apply_appearance_settings() {
    m_state.base_font_size = std::clamp(m_state.base_font_size, 12, 24);
    m_state.ui_scale = std::clamp(m_state.ui_scale, 0.75f, 2.0f);
    ApplicationState appearance_state = m_state;
    appearance_state.ui_scale *= m_dpi_scale;
    set_premiere_theme(appearance_state);
    if (m_settings_context != nullptr) {
        appearance_state.ui_scale = m_state.ui_scale * m_settings_dpi_scale;
        ImGui::SetCurrentContext(m_settings_context);
        set_premiere_theme(appearance_state);
        ImGui::SetCurrentContext(m_main_context);
    }
    m_applied_base_font_size = m_state.base_font_size;
    m_applied_ui_scale = m_state.ui_scale;
    m_applied_dpi_scale = m_dpi_scale;
}

void UISystem::deinit() {
    if (m_main_context == nullptr)
        return;

    close_settings_window();
    ImGui::SetCurrentContext(m_main_context);
    if (m_menu_icon_texture != nullptr) {
        SDL_ReleaseGPUTexture(m_gpu_device, m_menu_icon_texture);
        m_menu_icon_texture = nullptr;
    }
    if (m_chat_panel_state.attachment_icon_texture != nullptr) {
        SDL_ReleaseGPUTexture(m_gpu_device, m_chat_panel_state.attachment_icon_texture);
        m_chat_panel_state.attachment_icon_texture = nullptr;
    }
    if (m_chat_panel_state.paperclip_icon_texture != nullptr) {
        SDL_ReleaseGPUTexture(m_gpu_device, m_chat_panel_state.paperclip_icon_texture);
        m_chat_panel_state.paperclip_icon_texture = nullptr;
    }
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    m_main_context = nullptr;
    m_initialized = false;
}

void UISystem::new_frame() {
    ImGui::SetCurrentContext(m_main_context);
    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void UISystem::prepare_backbuffer() {
    ImGui::Render();

    ImDrawData* draw_data = ImGui::GetDrawData();
    SDL_GPUCommandBuffer* command_buffer = SDL_AcquireGPUCommandBuffer(m_gpu_device);
    if (command_buffer == nullptr)
        return;
    SDL_GPUTexture* swapchain_texture = nullptr;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(command_buffer, m_window,
                                               &swapchain_texture, nullptr, nullptr)) {
        SDL_CancelGPUCommandBuffer(command_buffer);
        return;
    }
    if (swapchain_texture != nullptr && draw_data->DisplaySize.x > 0.0f &&
        draw_data->DisplaySize.y > 0.0f) {
        ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, command_buffer);
        SDL_GPUColorTargetInfo target_info{};
        target_info.texture = swapchain_texture;
        target_info.clear_color = SDL_FColor{0.075f, 0.075f, 0.075f, 1.0f};
        target_info.load_op = SDL_GPU_LOADOP_CLEAR;
        target_info.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* render_pass = SDL_BeginGPURenderPass(command_buffer,
                                                                &target_info, 1, nullptr);
        ImGui_ImplSDLGPU3_RenderDrawData(draw_data, command_buffer, render_pass);
        SDL_EndGPURenderPass(render_pass);
    }
    SDL_SubmitGPUCommandBuffer(command_buffer);
}

void UISystem::render_frame_to_backbuffer() {
    for (const FileDialogResult& result : take_file_dialog_results(*m_file_dialog_queue)) {
        if (result.purpose == FileDialogPurpose::OpenProject) {
            apply_open_project_result(m_state, result, m_window);
        } else {
            if (m_state.selected_project < m_state.projects.size() &&
                m_state.selected_thread < m_state.projects[m_state.selected_project].threads.size()) {
                const std::string& id =
                    m_state.projects[m_state.selected_project].threads[m_state.selected_thread].id;
                apply_attachment_result(m_thread_panels[id], result);
            }
        }
    }

    if (m_open_settings_requested) {
        m_open_settings_requested = false;
        open_settings_window();
    }

    m_dpi_scale = window_content_scale(m_window);
    if (!m_appearance_edit_active &&
        (m_applied_base_font_size != m_state.base_font_size ||
         m_applied_ui_scale != m_state.ui_scale ||
         m_applied_dpi_scale != m_dpi_scale)) {
        ImGui::SetCurrentContext(m_main_context);
        apply_appearance_settings();
    }

    new_frame();

    for (ProviderPtr& provider : m_providers) {
      for (const Event& event : provider->poll_events(provider.get())) {
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
                        messages.push_back({ChatMessageRole::Assistant, {}, {}, {}, {}, {}});
                    messages.back().reasoning += event.text;
                } else {
                    if (messages.empty() || messages.back().role != ChatMessageRole::Assistant)
                        messages.push_back({ChatMessageRole::Assistant, {}, {}, {}, {}, {}});
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
                    messages.push_back({ChatMessageRole::Assistant, {}, {}, {}, {}, {}});
                ChatMessage& message = messages.back();
                message.content += event.text;
                if (message.segments.empty() || message.segments.back().kind != ChatSegment::Kind::Text)
                    message.segments.push_back({ChatSegment::Kind::Text, {}, {}});
                message.segments.back().text += event.text;
            } else if (event.kind == EventKind::TurnFailed) {
                auto panel = m_thread_panels.find(event.conversation_id);
                if (panel != m_thread_panels.end() &&
                    event.turn_id == panel->second.active_turn_id) {
                    panel->second.is_generating = false;
                    panel->second.active_turn_id = 0;
                }
                messages.push_back({ChatMessageRole::Assistant, event.text, {}, {}, {}, {}});
            } else if (event.kind == EventKind::TurnCompleted) {
                auto panel = m_thread_panels.find(event.conversation_id);
                if (panel != m_thread_panels.end() &&
                    event.turn_id == panel->second.active_turn_id) {
                    panel->second.is_generating = false;
                    panel->second.active_turn_id = 0;
                }
            }
        } catch (...) {
        }
      }
    }
    if (ImGui::BeginMainMenuBar()) {
        const ImVec2 menu_row_pos = ImGui::GetCursorScreenPos();
        const float menu_row_height = ImGui::GetFrameHeight();
        float next_item_x = menu_row_pos.x;
        if (m_menu_icon_texture != nullptr) {
            const float icon_size = ui_size(16.0f);
            const float icon_label_spacing = ui_size(6.0f);
            ImGui::GetWindowDrawList()->AddImage(
                ImTextureRef(texture_id(m_menu_icon_texture)),
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
            next_item_x += label_size.x + ui_size(12.0f);
            ImGui::SetCursorScreenPos(ImVec2(next_item_x, menu_row_pos.y));
        }
        const bool file_menu_open = ImGui::BeginMenu("File");
        const ImVec2 file_menu_min = ImGui::GetItemRectMin();
        const ImVec2 file_menu_max = ImGui::GetItemRectMax();
        m_title_bar_interactive_bounds[0] = SDL_Rect{
            static_cast<int>(file_menu_min.x), static_cast<int>(file_menu_min.y),
            static_cast<int>(file_menu_max.x - file_menu_min.x),
            static_cast<int>(file_menu_max.y - file_menu_min.y)};
        if (file_menu_open) {
            if (ImGui::MenuItem("Open Project..."))
                open_project_dialog(m_state, m_file_dialog_queue, m_window);
            ImGui::Separator();
            if (ImGui::MenuItem("Settings"))
                m_open_settings_requested = true;
            if (ImGui::MenuItem("Close")) {
                SDL_Event quit_event{};
                quit_event.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&quit_event);
            }
            ImGui::EndMenu();
        }
        const bool view_menu_open = ImGui::BeginMenu("View");
        const ImVec2 view_menu_min = ImGui::GetItemRectMin();
        const ImVec2 view_menu_max = ImGui::GetItemRectMax();
        m_title_bar_interactive_bounds[1] = SDL_Rect{
            static_cast<int>(view_menu_min.x), static_cast<int>(view_menu_min.y),
            static_cast<int>(view_menu_max.x - view_menu_min.x),
            static_cast<int>(view_menu_max.y - view_menu_min.y)};
        if (view_menu_open) {
            ImGui::MenuItem("Threads", nullptr, &m_threads_panel_open);
            ImGui::MenuItem("Chat", nullptr, &m_chat_panel_open);
            ImGui::MenuItem("Usage & Limits", nullptr, &m_usage_panel_open);
            ImGui::Separator();
            if (ImGui::MenuItem("Hide All Panes")) {
                m_threads_panel_open = false;
                m_chat_panel_open = false;
                m_usage_panel_open = false;
            }
            if (ImGui::MenuItem("Show All Panes")) {
                m_threads_panel_open = true;
                m_chat_panel_open = true;
                m_usage_panel_open = true;
            }
            ImGui::EndMenu();
        }

        const ImGuiStyle& style = ImGui::GetStyle();
        const float control_width = ui_size(36.0f);
        constexpr float control_count = 3.0f;
        const ImVec2 menu_window_pos = ImGui::GetWindowPos();
        const ImVec2 menu_window_size = ImGui::GetWindowSize();
        const float controls_right = menu_window_pos.x + menu_window_size.x -
                                     style.WindowBorderSize;
        const float controls_left = controls_right - control_width * control_count;
        ImGui::SetCursorScreenPos(ImVec2(controls_left, menu_row_pos.y));
        if (window_control_button("##MinimizeWindow", WindowControlIcon::Minimize,
                                  control_width, menu_row_height))
            SDL_MinimizeWindow(m_window);
        ImGui::SetCursorScreenPos(ImVec2(controls_left + control_width,
                                         menu_row_pos.y));
        if (window_control_button("##MaximizeWindow", WindowControlIcon::Maximize,
                                  control_width, menu_row_height)) {
            if ((SDL_GetWindowFlags(m_window) & SDL_WINDOW_MAXIMIZED) != 0)
                SDL_RestoreWindow(m_window);
            else
                SDL_MaximizeWindow(m_window);
        }
        ImGui::SetCursorScreenPos(ImVec2(controls_left + control_width * 2.0f,
                                         menu_row_pos.y));
        if (window_control_button("##CloseWindow", WindowControlIcon::Close,
                                  control_width, menu_row_height)) {
            SDL_Event quit_event{};
            quit_event.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit_event);
        }
        m_title_bar_height = static_cast<int>(std::ceil(menu_row_height));
        m_title_bar_interactive_bounds[2] = SDL_Rect{
            static_cast<int>(controls_left), 0,
            static_cast<int>(control_width * control_count), m_title_bar_height};
        ImGui::EndMainMenuBar();
    }
    render_dock_area();
    if (m_threads_panel_open)
        render_threads_panel(m_state, m_file_dialog_queue, m_window);
    if (m_chat_panel_open) {
        if (m_state.selected_project < m_state.projects.size() &&
            m_state.selected_thread < m_state.projects[m_state.selected_project].threads.size()) {
            ChatThread& thread =
                m_state.projects[m_state.selected_project].threads[m_state.selected_thread];
            ChatPanelState& panel = m_thread_panels.try_emplace(thread.id).first->second;
            if (!panel.initialized) {
                panel.initialized = true;
                panel.selected_provider = m_chat_panel_state.selected_provider;
                panel.selected_model = m_chat_panel_state.selected_model;
                if (!thread.provider.empty()) {
                    for (std::size_t index = 0; index < m_providers.size(); ++index) {
                        if (m_providers[index]->name == thread.provider) {
                            panel.selected_provider = index;
                            panel.selected_model = thread.model;
                            break;
                        }
                    }
                }
                panel.selected_reasoning_effort = thread.reasoning_effort;
                panel.selected_permission_mode = thread.permission_mode;
            }
            panel.monospace_font = m_chat_panel_state.monospace_font;
            panel.attachment_icon_texture = m_chat_panel_state.attachment_icon_texture;
            panel.paperclip_icon_texture = m_chat_panel_state.paperclip_icon_texture;
            const std::string id = thread.id;
            render_chat_panel(m_state, m_providers, panel, m_next_turn_id,
                              m_file_dialog_queue, m_window);
            if (panel.selected_provider < m_providers.size()) {
                for (ChatProject& project : m_state.projects) {
                    auto found = std::find_if(project.threads.begin(), project.threads.end(),
                        [&id](const ChatThread& value) { return value.id == id; });
                    if (found != project.threads.end()) {
                        found->provider = m_providers[panel.selected_provider]->name;
                        found->model = panel.selected_model;
                        found->reasoning_effort = panel.selected_reasoning_effort;
                        found->permission_mode = panel.selected_permission_mode;
                        break;
                    }
                }
            }
        } else {
            render_chat_panel(m_state, m_providers, m_chat_panel_state, m_next_turn_id,
                              m_file_dialog_queue, m_window);
        }
    }

    for (std::size_t index = 0; index < m_providers.size(); ++index) {
        Provider* provider = m_providers[index].get();
        if (provider->poll_usage == nullptr)
            continue;
        std::optional<UsageSnapshot> updated = provider->poll_usage(provider);
        if (updated.has_value()) {
            m_usage_snapshots[index] = std::move(updated);
            m_usage_loading[index] = false;
        }
    }
    if (m_usage_panel_open) {
        ImGui::Begin("Usage & Limits");
        if (ImGui::Button("Refresh")) {
            for (std::size_t index = 0; index < m_providers.size(); ++index) {
                Provider* provider = m_providers[index].get();
                if (provider->request_usage != nullptr && provider->poll_usage != nullptr) {
                    provider->request_usage(provider);
                    m_usage_loading[index] = true;
                }
            }
        }
        for (std::size_t index = 0; index < m_providers.size(); ++index) {
            Provider* provider = m_providers[index].get();
            ImGui::SeparatorText(std::string(provider->name).c_str());
            if (provider->request_usage == nullptr || provider->poll_usage == nullptr) {
                ImGui::TextDisabled("Usage information is not available for this provider.");
                continue;
            }
            if (m_usage_loading[index] && !m_usage_snapshots[index].has_value()) {
                ImGui::TextDisabled("Loading usage information…");
                continue;
            }
            if (!m_usage_snapshots[index].has_value() ||
                m_usage_snapshots[index]->metrics.empty()) {
                ImGui::TextDisabled("No usage limits were reported.");
                continue;
            }
            const UsageSnapshot& snapshot = *m_usage_snapshots[index];
            if (m_usage_loading[index])
                ImGui::TextDisabled("Refreshing…");
            for (const UsageMetric& metric : snapshot.metrics) {
                ImGui::SeparatorText(metric.name.c_str());
                const bool has_quota = metric.limit.has_value() &&
                    (metric.remaining.has_value() || metric.used.has_value());
                if (!metric.value.empty() && !has_quota)
                    ImGui::TextWrapped("%s", metric.value.c_str());
                if (has_quota) {
                    const double remaining = metric.remaining.value_or(
                        *metric.limit - metric.used.value_or(0.0));
                    const double fraction = *metric.limit > 0.0
                        ? std::clamp(remaining / *metric.limit, 0.0, 1.0) : 0.0;
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.16f, 0.16f, 0.16f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
                                          ImVec4(0.48f, 0.48f, 0.48f, 1.0f));
                    ImGui::ProgressBar(static_cast<float>(fraction),
                                       ImVec2(-FLT_MIN, ui_size(4.0f)), "");
                    ImGui::PopStyleColor(2);
                }
                if (!metric.period.empty())
                    ImGui::TextDisabled("Period: %s", metric.period.c_str());
                if (!metric.reset_at.empty())
                    ImGui::TextDisabled("Resets: %s", metric.reset_at.c_str());
            }
            if (!snapshot.updated_at.empty())
                ImGui::TextDisabled("Updated: %s", snapshot.updated_at.c_str());
        }
        ImGui::End();
    }

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
        SDL_ShowWindow(m_settings_window);
        SDL_RaiseWindow(m_settings_window);
        return true;
    }

    m_settings_window = SDL_CreateWindow("Zenith Settings", 760, 560,
                                         SDL_WINDOW_RESIZABLE |
                                             SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                             SDL_WINDOW_HIDDEN);
    if (m_settings_window == nullptr)
        return false;
    if (!SDL_ClaimWindowForGPUDevice(m_gpu_device, m_settings_window)) {
        SDL_DestroyWindow(m_settings_window);
        m_settings_window = nullptr;
        return false;
    }
    if (!SDL_SetGPUSwapchainParameters(m_gpu_device, m_settings_window,
                                      SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                      SDL_GPU_PRESENTMODE_VSYNC)) {
        SDL_ReleaseWindowFromGPUDevice(m_gpu_device, m_settings_window);
        SDL_DestroyWindow(m_settings_window);
        m_settings_window = nullptr;
        return false;
    }

    ImGui::SetCurrentContext(m_main_context);
    m_settings_context = ImGui::CreateContext();
    ImGui::SetCurrentContext(m_settings_context);
    ImFontConfig font_config;
    font_config.SizePixels = static_cast<float>(m_state.base_font_size);
    ImGui::GetIO().FontDefault = load_bundled_font(
        "IBM-Plex-Sans", "IBMPlexSans-Regular.ttf", false, font_config.SizePixels);
    if (ImGui::GetIO().FontDefault == nullptr)
        ImGui::GetIO().FontDefault = ImGui::GetIO().Fonts->AddFontDefault(&font_config);
    ImGui::GetIO().IniFilename = nullptr;
    ApplicationState appearance_state = m_state;
    m_settings_dpi_scale = window_content_scale(m_settings_window);
    appearance_state.ui_scale *= m_settings_dpi_scale;
    set_premiere_theme(appearance_state);

    if (!ImGui_ImplSDL3_InitForSDLGPU(m_settings_window)) {
        ImGui::DestroyContext(m_settings_context);
        m_settings_context = nullptr;
        SDL_ReleaseWindowFromGPUDevice(m_gpu_device, m_settings_window);
        SDL_DestroyWindow(m_settings_window);
        m_settings_window = nullptr;
        ImGui::SetCurrentContext(m_main_context);
        return false;
    }
    ImGui_ImplSDLGPU3_InitInfo init_info{};
    init_info.Device = m_gpu_device;
    init_info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(m_gpu_device,
                                                                  m_settings_window);
    init_info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    init_info.SwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
    init_info.PresentMode = SDL_GPU_PRESENTMODE_VSYNC;
    if (!ImGui_ImplSDLGPU3_Init(&init_info)) {
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext(m_settings_context);
        m_settings_context = nullptr;
        SDL_ReleaseWindowFromGPUDevice(m_gpu_device, m_settings_window);
        SDL_DestroyWindow(m_settings_window);
        m_settings_window = nullptr;
        ImGui::SetCurrentContext(m_main_context);
        return false;
    }
    m_close_settings_requested = false;
    SDL_ShowWindow(m_settings_window);
    SDL_RaiseWindow(m_settings_window);
    ImGui::SetCurrentContext(m_main_context);
    return true;
}

void UISystem::close_settings_window() {
    if (m_settings_window == nullptr)
        return;

    ImGui::SetCurrentContext(m_settings_context);
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(m_settings_context);
    SDL_ReleaseWindowFromGPUDevice(m_gpu_device, m_settings_window);
    SDL_DestroyWindow(m_settings_window);
    m_settings_window = nullptr;
    m_settings_context = nullptr;
    m_appearance_edit_active = false;
    m_close_settings_requested = false;
    ImGui::SetCurrentContext(m_main_context);
}

void UISystem::render_settings_window() {
    if (m_settings_window == nullptr)
        return;
    if (m_close_settings_requested) {
        close_settings_window();
        return;
    }

    const float settings_dpi_scale = window_content_scale(m_settings_window);
    if (settings_dpi_scale != m_settings_dpi_scale) {
        m_settings_dpi_scale = settings_dpi_scale;
        ApplicationState appearance_state = m_state;
        appearance_state.ui_scale *= m_settings_dpi_scale;
        ImGui::SetCurrentContext(m_settings_context);
        set_premiere_theme(appearance_state);
    }
    ImGui::SetCurrentContext(m_settings_context);
    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    render_settings_contents();
    ImGui::Render();
    ImDrawData* draw_data = ImGui::GetDrawData();

    SDL_GPUCommandBuffer* command_buffer = SDL_AcquireGPUCommandBuffer(m_gpu_device);
    if (command_buffer == nullptr) {
        ImGui::SetCurrentContext(m_main_context);
        return;
    }
    SDL_GPUTexture* swapchain_texture = nullptr;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(command_buffer, m_settings_window,
                                               &swapchain_texture, nullptr, nullptr)) {
        SDL_CancelGPUCommandBuffer(command_buffer);
        ImGui::SetCurrentContext(m_main_context);
        return;
    }
    if (swapchain_texture != nullptr && draw_data->DisplaySize.x > 0.0f &&
        draw_data->DisplaySize.y > 0.0f) {
        ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, command_buffer);
        SDL_GPUColorTargetInfo target_info{};
        target_info.texture = swapchain_texture;
        target_info.clear_color = SDL_FColor{0.075f, 0.075f, 0.075f, 1.0f};
        target_info.load_op = SDL_GPU_LOADOP_CLEAR;
        target_info.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* render_pass = SDL_BeginGPURenderPass(command_buffer,
                                                                &target_info, 1, nullptr);
        ImGui_ImplSDLGPU3_RenderDrawData(draw_data, command_buffer, render_pass);
        SDL_EndGPURenderPass(render_pass);
    }
    SDL_SubmitGPUCommandBuffer(command_buffer);
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

    const float sidebar_width = std::min(ui_size(180.0f),
                                         ImGui::GetContentRegionAvail().x * 0.45f);
    ImGui::BeginChild("##settings_sidebar", ImVec2(sidebar_width, 0.0f),
                      ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::TextDisabled("SETTINGS");
    ImGui::Spacing();
    if (ImGui::Selectable("Appearance", m_settings_page == SettingsPage::Appearance))
        m_settings_page = SettingsPage::Appearance;
    if (ImGui::Selectable("Chat", m_settings_page == SettingsPage::Chat))
        m_settings_page = SettingsPage::Chat;
    if (ImGui::Selectable("Providers", m_settings_page == SettingsPage::Providers))
        m_settings_page = SettingsPage::Providers;
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("##settings_content", ImVec2(0.0f, 0.0f));
    m_appearance_edit_active = false;
    if (m_settings_page == SettingsPage::Appearance) {
        ImGui::TextUnformatted("Appearance");
        ImGui::Spacing();
        ImGui::TextUnformatted("Base font size");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderInt("##base_font_size", &m_state.base_font_size, 12, 24, "%d px",
                          ImGuiSliderFlags_AlwaysClamp);
        m_appearance_edit_active = ImGui::IsItemActive();
        ImGui::TextUnformatted("UI scale");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##ui_scale", &m_state.ui_scale, 0.75f, 2.0f, "%.2fx",
                           ImGuiSliderFlags_AlwaysClamp);
        m_appearance_edit_active |= ImGui::IsItemActive();
        ImGui::Spacing();
        if (ImGui::Button("Reset to defaults")) {
            m_state.base_font_size = 16;
            m_state.ui_scale = 1.0f;
        }
    }
    if (m_settings_page == SettingsPage::Chat) {
        ImGui::TextUnformatted("Chat");
        ImGui::Spacing();
        ImGui::Checkbox("Collapse tool calls", &m_state.collapse_tool_calls);
        ImGui::TextDisabled("Group consecutive tool calls under an expandable heading.");
    }
    for (std::size_t index = 0; m_settings_page == SettingsPage::Providers && index < m_providers.size(); ++index) {
        Provider& provider = *m_providers[index];
        const char* availability = "Checking...";
        ImVec4 availability_color = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
        if (provider.availability == ProviderAvailability::Available) {
            availability = "Available";
            availability_color = ImVec4(0.42f, 0.78f, 0.50f, 1.0f);
        } else if (provider.availability == ProviderAvailability::Unavailable) {
            availability = "Unavailable";
            availability_color = ImVec4(0.90f, 0.38f, 0.34f, 1.0f);
        }

        ImGui::PushID(static_cast<int>(index));
        if (begin_ui_card("##provider")) {
            ImGui::TextUnformatted(provider.name.data(),
                                   provider.name.data() + provider.name.size());
            ImGui::Spacing();
            if (ImGui::BeginTable("##details", 2, ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed,
                                        ImGui::CalcTextSize("Location").x);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("Status");
                ImGui::TableNextColumn();
                ImGui::TextColored(availability_color, "%s", availability);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("Location");
                ImGui::TableNextColumn();
                if (!provider.location.empty())
                    ImGui::TextWrapped("%s", provider.location.string().c_str());
                else if (provider.availability == ProviderAvailability::Unknown)
                    ImGui::TextUnformatted("Discovery in progress...");
                else
                    ImGui::TextUnformatted("Executable not found");
                ImGui::EndTable();
            }

        }
        end_ui_card();
        ImGui::PopID();
        ImGui::Spacing();
    }
    ImGui::EndChild();

    ImGui::End();
}
