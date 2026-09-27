#include "../base/result.h"
#include "../providers/provider.h"
#include "../state/application-state.h"
#include <GLFW/glfw3.h>
#include <string>

struct ImGuiContext;
struct ImFont;

class UISystem {
private:
    bool m_initialized = false;
    bool m_open_settings_requested = false;
    GLFWwindow* m_window = nullptr;
    GLFWwindow* m_settings_window = nullptr;
    ImGuiContext* m_main_context = nullptr;
    ImGuiContext* m_settings_context = nullptr;
    double m_settings_last_frame_time = 0.0;
    ApplicationState& m_state;
    Provider& m_provider;
    std::string m_message_input;
    std::string m_selected_model;
    std::string m_selected_reasoning_effort;
    bool m_is_generating = false;
    TurnId m_active_turn_id = 0;
    TurnId m_next_turn_id = 1;
    ImFont* m_monospace_font = nullptr;

    void new_frame();
    void prepare_backbuffer();
    void prepare_viewport();
    bool open_settings_window();
    void close_settings_window();
    void render_settings_window();
    void render_settings_contents();

public:
    UISystem(GLFWwindow* window, ApplicationState& state, Provider& provider);
    Result init();
    void deinit();
    void render_frame_to_backbuffer();
};
