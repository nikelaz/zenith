#ifndef CHAT_PANEL_H
#define CHAT_PANEL_H
#include "../providers/provider.h"
#include "../state/application-state.h"
#include <string>
struct ImFont;

struct ChatPanelState {
    std::string message_input;
    std::size_t selected_provider = 0;
    std::string selected_model;
    std::string selected_reasoning_effort;
    std::string selected_permission_mode;
    bool is_generating = false;
    TurnId active_turn_id = 0;
    TurnId next_turn_id = 1;
    ImFont* monospace_font = nullptr;
};

void render_chat_panel(ApplicationState& state, std::vector<ProviderPtr>& providers,
                       ChatPanelState& panel_state);
#endif
