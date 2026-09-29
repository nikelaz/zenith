#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#endif
#include "application.h"
#include "base/result.h"
#include "platform/message-box.h"
#include <filesystem>
#include <string>
#include <system_error>

constexpr int kWindowWidth = 1440;
constexpr int kWindowHeight = 900;

namespace {
Result application_database_path(std::filesystem::path* path) {
#ifdef _WIN32
    PWSTR local_app_data = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE,
                                                nullptr, &local_app_data);
    if (FAILED(result) || local_app_data == nullptr)
        return result_error("Failed to locate the local application data folder");
    const std::filesystem::path directory =
        std::filesystem::path(local_app_data) / L"Zenith";
    CoTaskMemFree(local_app_data);
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
        return result_error("Failed to create the Zenith data folder: " + error.message());
    *path = directory / L"Zenith.sqlite3";
#else
    *path = "Zenith.sqlite3";
#endif
    return result_ok();
}
}

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
    m_ui.emplace(m_window, m_gpu_device, m_state, m_providers);

    Result ui_init_result = m_ui->init();
    if (ui_init_result.status == ResultStatus::Error) {
        deinit();
        return ui_init_result;
    }

    m_initialized = true;

    return result_ok();
}

void Application::deinit() {
    if (m_ui) {
        m_ui->deinit();
        m_ui.reset();
    }

    if (m_initialized) {
        Result state_result = m_state_store.save(m_state);
        if (state_result.status == ResultStatus::Error) {
            const std::string error_message(state_result.error);
            show_error_message(error_message.c_str(), m_window);
        }
    }
    m_providers.clear();
    m_state_store.close();

    window_deinit();

    m_initialized = false;
}

Result Application::window_init() {
    if (!SDL_Init(SDL_INIT_VIDEO))
        return result_error(std::string("Failed to initialize SDL: ") + SDL_GetError());

    m_window = SDL_CreateWindow("Zenith", kWindowWidth, kWindowHeight,
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
            if (event.type == SDL_EVENT_QUIT ||
                (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                 event.window.windowID == SDL_GetWindowID(m_window))) {
                m_quit_requested = true;
            }
            m_ui->process_event(event);
        }
        if (m_quit_requested)
            break;
        m_ui->render_frame_to_backbuffer();
    }
}
