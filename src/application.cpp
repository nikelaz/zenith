#include "application.h"
#include "base/result.h"
#include <GLFW/glfw3.h>
#include <string>
#include <tinyfiledialogs.h>

constexpr int kWindowWidth = 1440;
constexpr int kWindowHeight = 900;

static void glfw_error_callback(int code, const char* description) {
    const std::string message = "GLFW error " + std::to_string(code) + ": " +
                                (description != nullptr ? description : "Unknown error");
    tinyfd_messageBox("Zenith - GLFW error", message.c_str(), "ok", "error", 1);
}

Result Application::init() {
    Result glfw_init_result = window_init();
    if (glfw_init_result.status == ResultStatus::Error) {
        return glfw_init_result;
    }

    Result state_result = m_state_store.open("Zenith.sqlite3");
    if (state_result.status == ResultStatus::Error) {
        m_state_store.close();
        window_deinit();
        return state_result;
    }

    state_result = m_state_store.load(m_state);
    if (state_result.status == ResultStatus::Error) {
        m_state_store.close();
        window_deinit();
        return state_result;
    }

    m_provider = make_codex_provider();
    Result provider_result = m_provider->start(m_provider.get());
    if (provider_result.status == ResultStatus::Error) {

        m_state_store.close();
        window_deinit();
        return provider_result;
    }
    m_ui.emplace(m_window, m_state, *m_provider);

    if (!m_ui) {
        m_provider.reset();

        window_deinit();
        return result_error("UI System does not have a value unexpectedly");
    }

    Result ui_init_result = m_ui->init();
    if (ui_init_result.status == ResultStatus::Error) {
        m_provider.reset();
        m_state_store.close();
        window_deinit();
        return ui_init_result;
    }

    m_initialized = true;

    return result_ok();
}

void Application::deinit() {
    if (m_initialized && m_ui) {
        m_ui->deinit();
    }

    if (m_initialized) {
        Result state_result = m_state_store.save(m_state);
        if (state_result.status == ResultStatus::Error) {
            const std::string error_message(state_result.error);
            tinyfd_messageBox("Zenith", error_message.c_str(), "ok", "error", 1);
        }
    }
    m_provider.reset();
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

#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif

    m_window = glfwCreateWindow(kWindowWidth, kWindowHeight, "Zenith", nullptr, nullptr);

    if (m_window == nullptr) {
        glfwTerminate();
        return result_error("Failed to create application window");
    }

    glfwMakeContextCurrent(m_window);
    glfwSwapInterval(1);

    return result_ok();
};

void Application::window_deinit() {
    glfwDestroyWindow(m_window);
    glfwTerminate();
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
