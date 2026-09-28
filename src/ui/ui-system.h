#ifndef UI_SYSTEM_H
#define UI_SYSTEM_H

#include "../base/result.h"
#include "../providers/provider.h"
#include "../state/application-state.h"
#include "chat-panel.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <optional>
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
    bool m_usage_panel_open = true;
    bool m_close_settings_requested = false;
    SDL_Window* m_window = nullptr;
    SDL_Window* m_settings_window = nullptr;
    SDL_GPUDevice* m_gpu_device = nullptr;
    ImGuiContext* m_main_context = nullptr;
    ImGuiContext* m_settings_context = nullptr;
    int m_applied_base_font_size = 0;
    float m_applied_ui_scale = 0.0f;
    float m_dpi_scale = 1.0f;
    float m_applied_dpi_scale = 0.0f;
    float m_settings_dpi_scale = 0.0f;
    int m_title_bar_height = 0;
    bool m_appearance_edit_active = false;
    bool m_settings_show_appearance = true;
    ApplicationState& m_state;
    std::vector<ProviderPtr>& m_providers;
    ChatPanelState m_chat_panel_state;
    std::vector<std::optional<UsageSnapshot>> m_usage_snapshots;
    std::vector<bool> m_usage_loading;
    SDL_GPUTexture* m_menu_icon_texture = nullptr;
    SDL_Rect m_title_bar_interactive_bounds[3] = {};

    void new_frame();
    void prepare_backbuffer();
    bool open_settings_window();
    void close_settings_window();
    void render_settings_window();
    void render_settings_contents();
    void apply_appearance_settings();
    static SDL_HitTestResult SDLCALL title_bar_hit_test(SDL_Window* window,
                                                       const SDL_Point* point,
                                                       void* user_data);

public:
    UISystem(SDL_Window* window, SDL_GPUDevice* gpu_device, ApplicationState& state,
             std::vector<ProviderPtr>& providers);
    Result init();
    void deinit();
    void render_frame_to_backbuffer();
    void process_event(const SDL_Event& event);
};

#endif
