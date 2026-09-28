#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#endif
#include "application.h"
#include "base/result.h"
#include <GLFW/glfw3.h>
#include <filesystem>
#include <string>
#include <system_error>
#include <tinyfiledialogs.h>

#ifdef _WIN32
extern "C" void zenith_enable_win32_window_management(GLFWwindow* window);
#endif

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

static void glfw_error_callback(int code, const char* description) {
    const std::string message = "GLFW error " + std::to_string(code) + ": " +
                                (description != nullptr ? description : "Unknown error");
    tinyfd_messageBox("Zenith - GLFW error", message.c_str(), "ok", "error", 1);
}

Application::~Application() {
    deinit();
}

Result Application::init() {
    Result glfw_init_result = window_init();
    if (glfw_init_result.status == ResultStatus::Error) {
        return glfw_init_result;
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
    m_providers.push_back(make_github_copilot_provider());
    for (ProviderPtr& provider : m_providers) {
        Result provider_result = provider->start(provider.get());
        if (provider_result.status == ResultStatus::Error) {
            deinit();
            return provider_result;
        }
    }
    m_ui.emplace(m_window, m_state, m_providers);

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
            tinyfd_messageBox("Zenith", error_message.c_str(), "ok", "error", 1);
        }
    }
    m_providers.clear();
    m_state_store.close();

    window_deinit();

    m_initialized = false;
}

Result Application::window_init() {
    glfwSetErrorCallback(glfw_error_callback);

    if (glfwInit() != GLFW_TRUE) {
        return result_error("Failed to initialize GLFW");
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
#ifdef _WIN32
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
#endif

#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif

    m_window = glfwCreateWindow(kWindowWidth, kWindowHeight, "Zenith", nullptr, nullptr);

    if (m_window == nullptr) {
        glfwTerminate();
        return result_error("Failed to create application window");
    }

#ifdef _WIN32
    zenith_enable_win32_window_management(m_window);
#endif

    glfwMakeContextCurrent(m_window);
    glfwSwapInterval(1);

    return result_ok();
};

void Application::window_deinit() {
    if (m_window != nullptr) {
        glfwDestroyWindow(m_window);
        m_window = nullptr;
        glfwTerminate();
    }
}

void Application::run() {
    if (!m_initialized) {
        tinyfd_messageBox("Zenith", "Application has to be initialized with init() before run()",
                          "ok", "error", 1);
        return;
    }

    if (!m_ui.has_value()) {
        tinyfd_messageBox("Zenith", "UI System is not initialized in Application", "ok", "error", 1);
        return;
    }

    while (!glfwWindowShouldClose(m_window)) {
        glfwPollEvents();

        m_ui->render_frame_to_backbuffer();

        glfwSwapBuffers(m_window);
    }
}
