#ifndef UI_SYSTEM_H
#define UI_SYSTEM_H

#include "../base/result.h"
#include "../providers/provider.h"
#include "../state/application-state.h"
#include "chat-panel.h"
#include <GLFW/glfw3.h>
#include <string>
#include <vector>

struct ImGuiContext;
struct ImFont;

class UISystem {
private:
    bool m_initialized = false;
    bool m_open_settings_requested = false;
    bool m_threads_panel_open = true;
    bool m_chat_panel_open = true;
    bool m_dragging_title_bar = false;
    GLFWwindow* m_window = nullptr;
    GLFWwindow* m_settings_window = nullptr;
    ImGuiContext* m_main_context = nullptr;
    ImGuiContext* m_settings_context = nullptr;
    double m_settings_last_frame_time = 0.0;
    int m_applied_base_font_size = 0;
    float m_applied_ui_scale = 0.0f;
    bool m_appearance_edit_active = false;
    bool m_settings_show_appearance = true;
    ApplicationState& m_state;
    std::vector<ProviderPtr>& m_providers;
    ChatPanelState m_chat_panel_state;
    unsigned int m_menu_icon_texture = 0;
    int m_title_bar_drag_window_x = 0;
    int m_title_bar_drag_window_y = 0;
    double m_title_bar_drag_cursor_x = 0.0;
    double m_title_bar_drag_cursor_y = 0.0;

    void new_frame();
    void prepare_backbuffer();
    void prepare_viewport();
    bool open_settings_window();
    void close_settings_window();
    void render_settings_window();
    void render_settings_contents();
    void apply_appearance_settings();

public:
    UISystem(GLFWwindow* window, ApplicationState& state, std::vector<ProviderPtr>& providers);
    Result init();
    void deinit();
    void render_frame_to_backbuffer();
};

#endif
