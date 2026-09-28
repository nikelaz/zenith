#ifndef CHAT_PANEL_H
#define CHAT_PANEL_H
#include "../providers/provider.h"
#include "../state/application-state.h"
#include <filesystem>
#include <string>
#include <vector>
struct ImFont;
struct SDL_GPUTexture;

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
    SDL_GPUTexture* attachment_icon_texture = nullptr;
    SDL_GPUTexture* paperclip_icon_texture = nullptr;
    std::vector<FileReference> file_references;
    std::vector<FileAttachment> attachments;
    std::string attachment_error;
    std::filesystem::path file_references_root;
    bool file_picker_open = false;
    bool restore_input_focus = false;
    std::filesystem::path file_picker_root;
    std::string file_picker_query;
    std::filesystem::recursive_directory_iterator file_picker_iterator;
    std::filesystem::recursive_directory_iterator file_picker_end;
    std::vector<std::filesystem::path> file_picker_results;
    std::size_t file_picker_selected = 0;
    std::size_t file_picker_cursor = 0;
    std::size_t file_picker_replace_start = 0;
    std::size_t file_picker_replace_end = 0;
    bool file_picker_scan_complete = true;
};

void render_chat_panel(ApplicationState& state, std::vector<ProviderPtr>& providers,
                       ChatPanelState& panel_state);
#endif
