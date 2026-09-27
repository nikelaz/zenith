#ifndef CHAT_PANEL_H
#define CHAT_PANEL_H
#include "../providers/provider.h"
#include "../state/application-state.h"
#include <string>
struct ImFont;
void render_chat_panel(ApplicationState& state, std::string& message_input,
                       std::vector<ProviderPtr>& providers, std::size_t& selected_provider,
                       std::string& selected_model, std::string& selected_reasoning_effort,
                       bool& is_generating, TurnId& active_turn_id, TurnId& next_turn_id,
                       ImFont* monospace_font);
#endif
