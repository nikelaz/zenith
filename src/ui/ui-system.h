#ifndef UI_SYSTEM_H
#define UI_SYSTEM_H

#include "../base/result.h"
#include "../platform/file-dialogs.h"
#include "../providers/provider.h"
#include "../providers/mcp-service.h"
#include "../state/application-state.h"
#include "chat-panel.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <memory>
#include <deque>
#include <optional>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

struct ImGuiContext;
struct ImFont;

class UISystem {
private:
    struct McpOperationFeedback {
        McpTaskKind kind = McpTaskKind::Refresh;
        std::string existing_name;
        McpServer server;
        bool enabled = true;
        bool pending = true;
        bool saved = false;
        std::string error;
    };
    struct McpProviderSnapshot {
        std::vector<McpServer> servers;
        std::unordered_map<std::string, McpOperationFeedback> operation_feedback;
        std::string error;
        bool loaded = false;
        bool attempted = false;
        bool loading = false;
    };

    bool m_initialized = false;
    bool m_open_settings_requested = false;
    bool m_threads_panel_open = true;
    bool m_chat_panel_open = true;
    bool m_usage_panel_open = true;
    bool m_mcp_panel_open = true;
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
    enum class SettingsPage { Appearance, Chat, Providers, Skills };
    SettingsPage m_settings_page = SettingsPage::Appearance;
    ApplicationState& m_state;
    std::vector<ProviderPtr>& m_providers;
    ChatPanelState m_chat_panel_state;
    ChatRenderResources m_chat_resources;
    ChatRenderContext m_chat_render;
    std::unordered_map<std::string, ChatPanelState> m_thread_panels;
    std::size_t m_known_thread_count = 0;
    ConversationState& m_conversations;
    std::shared_ptr<FileDialogQueue> m_file_dialog_queue =
        std::make_shared<FileDialogQueue>();
    struct ProviderViewState {
        std::optional<UsageSnapshot> usage_snapshot;
        bool usage_loading = false;
        float usage_rotation_angle = 0.0f;
        float usage_rotation_target = 0.0f;
        std::unordered_map<std::string, SkillDiscoverySnapshot> skill_snapshots;
        std::unordered_set<std::string> skill_requests;
        std::unordered_map<std::string, McpProviderSnapshot> mcp_snapshots;
    };
    std::vector<ProviderViewState> m_provider_views;
    McpService& m_mcp_service;
    bool m_mcp_dialog_open = false;
    std::size_t m_mcp_dialog_provider = 0;
    std::string m_mcp_edit_existing_name;
    McpServer m_mcp_draft;
    std::string m_mcp_arguments_text;
    std::string m_mcp_environment_text;
    std::string m_mcp_headers_text;
    std::string m_mcp_dialog_error;
    std::string m_mcp_delete_name;
    std::size_t m_mcp_delete_provider = 0;
    SDL_GPUTexture* m_menu_icon_texture = nullptr;
    SDL_Rect m_title_bar_interactive_bounds[3] = {};

    void new_frame();
    void prepare_backbuffer();
    bool open_settings_window();
    void close_settings_window();
    void render_settings_window();
    void render_settings_contents();
    void apply_appearance_settings();
    void queue_mcp_task(McpTask task, bool explicit_refresh = false);
    void update_mcp_results();
    void render_mcp_panel();
    static SDL_HitTestResult SDLCALL title_bar_hit_test(SDL_Window* window,
                                                       const SDL_Point* point,
                                                       void* user_data);

public:
    UISystem(SDL_Window* window, SDL_GPUDevice* gpu_device, ApplicationState& state,
             std::vector<ProviderPtr>& providers, ConversationState& conversations,
             McpService& mcp_service);
    Result init();
    void deinit();
    void update();
    void render_frame_to_backbuffer();
    void process_event(const SDL_Event& event);
};

#endif
