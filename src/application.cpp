#include "application.h"
#include "base/os.h"
#include "base/result.h"
#include "platform/application-paths.h"
#include "platform/message-box.h"
#include <cmath>
#include <filesystem>
#include <string>

constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 720;

Application::~Application() {
    deinit();
}

Result Application::init() {
    Result window_result = window_init();
    if (window_result.status == ResultStatus::Error) {
        return window_result;
    }

    if (!m_state.projects.empty() && m_state.projects.front().directory.empty()) {
        std::error_code path_error;
        m_state.projects.front().directory = std::filesystem::current_path(path_error);
        if (path_error) {
            deinit();
            return result_error("Failed to determine the initial project directory: " +
                                path_error.message());
        }
    }

    std::filesystem::path database_path;
    Result state_result = application_database_path(&database_path);
    if (state_result.status == ResultStatus::Error) {
        deinit();
        return state_result;
    }
    state_result = m_state_store.open(database_path.string());
    if (state_result.status == ResultStatus::Error) {
        deinit();
        return state_result;
    }

    state_result = m_state_store.load(m_state);
    if (state_result.status == ResultStatus::Error) {
        deinit();
        return state_result;
    }
    state_result = m_state_store.load_window(&m_window_state);
    if (state_result.status == ResultStatus::Error) {
        deinit();
        return state_result;
    }
    if (m_window_state.width > 0 && m_window_state.height > 0) {
        if (!SDL_SetWindowSize(m_window, m_window_state.width, m_window_state.height) ||
            !SDL_SetWindowPosition(m_window, m_window_state.x, m_window_state.y) ||
            (m_window_state.maximized && !SDL_MaximizeWindow(m_window))) {
            const std::string error = SDL_GetError();
            deinit();
            return result_error("Failed to restore window state: " + error);
        }
    } else {
        SDL_GetWindowPosition(m_window, &m_window_state.x, &m_window_state.y);
        SDL_GetWindowSize(m_window, &m_window_state.width, &m_window_state.height);
    }

    m_providers.push_back(make_codex_provider());
    GitHubCopilotOptions copilot_options;
    copilot_options.diagnostics_path = database_path.parent_path() / "diagnostics.jsonl";
    m_providers.push_back(make_github_copilot_provider(&copilot_options));
    for (ProviderPtr& provider : m_providers) {
        Result provider_result = provider->start(provider.get());
        if (provider_result.status == ResultStatus::Error) {
            deinit();
            return provider_result;
        }
    }
    Result mcp_result = mcp_service_start(&m_mcp_service, &m_providers);
    if (mcp_result.status == ResultStatus::Error) {
        deinit();
        return mcp_result;
    }
    m_ui.emplace(m_window, m_gpu_device, m_state, m_providers, m_conversations, m_mcp_service);

    Result ui_init_result = m_ui->init();
    if (ui_init_result.status == ResultStatus::Error) {
        deinit();
        return ui_init_result;
    }

    m_initialized = true;

    return result_ok();
}

void Application::deinit() {
    for (const ProviderPtr& provider : m_providers)
        if (provider->request_shutdown != nullptr)
            provider->request_shutdown(provider.get());

    mcp_service_shutdown(&m_mcp_service);

    if (m_ui) {
        m_ui->deinit();
        m_ui.reset();
    }

    if (m_initialized) {
        Result window_result = m_state_store.save_window(m_window_state);
        if (window_result.status == ResultStatus::Error) {
            const std::string error_message(window_result.error);
            show_error_message(error_message.c_str(), m_window);
        }
        Result state_result = m_state_store.save(m_state);
        if (state_result.status == ResultStatus::Error) {
            const std::string error_message(state_result.error);
            show_error_message(error_message.c_str(), m_window);
        }
    }
    conversation_clear(&m_conversations);
    m_providers.clear();
    m_state_store.close();

    window_deinit();

    m_initialized = false;
}

Result Application::window_init() {
    if (!SDL_Init(SDL_INIT_VIDEO))
        return result_error(std::string("Failed to initialize SDL: ") + SDL_GetError());

    int window_width = kWindowWidth;
    int window_height = kWindowHeight;

#if OS_WIN
    const SDL_DisplayID primary_display = SDL_GetPrimaryDisplay();
    if (primary_display == 0) {
        const std::string error = SDL_GetError();
        SDL_Quit();
        return result_error("Failed to determine the primary display: " + error);
    }
    const float display_scale = SDL_GetDisplayContentScale(primary_display);
    if (display_scale <= 0.0f) {
        const std::string error = SDL_GetError();
        SDL_Quit();
        return result_error("Failed to determine the primary display scale: " + error);
    }
    window_width = static_cast<int>(std::lround(kWindowWidth * display_scale));
    window_height = static_cast<int>(std::lround(kWindowHeight * display_scale));
#endif

    m_window = SDL_CreateWindow("Zenith", window_width, window_height,
                                SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS |
                                    SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (m_window == nullptr) {
        SDL_Quit();
        return result_error(std::string("Failed to create application window: ") + SDL_GetError());
    }
    SDL_SetWindowPosition(m_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

    m_gpu_device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV |
                                           SDL_GPU_SHADERFORMAT_DXIL |
                                           SDL_GPU_SHADERFORMAT_DXBC |
                                           SDL_GPU_SHADERFORMAT_MSL |
                                           SDL_GPU_SHADERFORMAT_METALLIB,
                                       false, nullptr);
    if (m_gpu_device == nullptr) {
        window_deinit();
        return result_error(std::string("Failed to create SDL GPU device: ") + SDL_GetError());
    }
    if (!SDL_ClaimWindowForGPUDevice(m_gpu_device, m_window)) {
        const std::string error = SDL_GetError();
        window_deinit();
        return result_error("Failed to claim the application window for SDL GPU: " + error);
    }
    if (!SDL_SetGPUSwapchainParameters(m_gpu_device, m_window,
                                      SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                      SDL_GPU_PRESENTMODE_VSYNC)) {
        const std::string error = SDL_GetError();
        window_deinit();
        return result_error("Failed to configure the application swapchain: " + error);
    }

    return result_ok();
};

void Application::window_deinit() {
    if (m_gpu_device != nullptr && m_window != nullptr)
        SDL_ReleaseWindowFromGPUDevice(m_gpu_device, m_window);
    if (m_gpu_device != nullptr) {
        SDL_DestroyGPUDevice(m_gpu_device);
        m_gpu_device = nullptr;
    }
    if (m_window != nullptr) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }
    SDL_Quit();
}

void Application::run() {
    if (!m_initialized) {
        show_error_message("Application has to be initialized with init() before run()", m_window);
        return;
    }

    if (!m_ui.has_value()) {
        show_error_message("UI System is not initialized in Application", m_window);
        return;
    }

    m_quit_requested = false;
    while (!m_quit_requested) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_WINDOW_MAXIMIZED &&
                event.window.windowID == SDL_GetWindowID(m_window))
                m_window_state.maximized = true;
            if (event.type == SDL_EVENT_WINDOW_RESTORED &&
                event.window.windowID == SDL_GetWindowID(m_window))
                m_window_state.maximized = false;
            if ((event.type == SDL_EVENT_WINDOW_MOVED ||
                 event.type == SDL_EVENT_WINDOW_RESIZED ||
                 event.type == SDL_EVENT_WINDOW_RESTORED) &&
                event.window.windowID == SDL_GetWindowID(m_window) &&
                !(SDL_GetWindowFlags(m_window) & SDL_WINDOW_MAXIMIZED) &&
                !m_window_state.maximized) {
                SDL_GetWindowPosition(m_window, &m_window_state.x, &m_window_state.y);
                SDL_GetWindowSize(m_window, &m_window_state.width, &m_window_state.height);
            }
            if (event.type == SDL_EVENT_QUIT ||
                (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                 event.window.windowID == SDL_GetWindowID(m_window))) {
                m_quit_requested = true;
            }
            m_ui->process_event(event);
        }
        if (m_quit_requested)
            break;
        conversation_update(&m_conversations, &m_state, m_providers);
        m_ui->update();
        m_ui->render_frame_to_backbuffer();
    }
}
