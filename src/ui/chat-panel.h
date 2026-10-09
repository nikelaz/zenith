#ifndef CHAT_PANEL_H
#define CHAT_PANEL_H
#include "../conversation/conversation.h"
#include <span>
#include "imgui.h"
#include "../state/application-state.h"
#include "../platform/file-dialogs.h"
#include <filesystem>
#include <string>
#include <vector>
struct ImFont;
struct SDL_GPUTexture;
struct SDL_Window;

struct ChatRenderResources {
    ImFont* monospace_font = nullptr;
    SDL_GPUTexture* attachment_icon_texture = nullptr;
    SDL_GPUTexture* paperclip_icon_texture = nullptr;
};

struct TextSpan {
    std::string text;
    ImVec2 position;
    ImVec2 clip_min;
    ImVec2 clip_max;
    ImFont* font;
    float font_size;
    ImDrawList* draw_list;
};

struct TextEndpoint {
    std::size_t span = 0;
    std::size_t byte = 0;
};

struct TranscriptSelection {
    TextEndpoint anchor;
    TextEndpoint focus;
    bool tracking = false;
    bool dragged = false;
};


struct ChatRenderContext {
    TranscriptSelection* selection = nullptr;
    std::vector<TextSpan> spans;
    bool has_component = false;
    std::string markdown_code;
    std::string markdown_language;
};

struct ChatPanelState {
    bool initialized = false;
    ComposerState composer;
    TranscriptSelection selection;
    std::string composer_context_selection;
    std::size_t selected_provider = 0;
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
    std::span<const SkillEntry> slash_commands; // Borrowed from a snapshot; rebound before rendering.
    bool slash_commands_loading = false;
    bool slash_picker_open = false;
    std::size_t slash_picker_selected = 0;
    std::size_t slash_picker_replace_start = 0;
    std::size_t slash_picker_replace_end = 0;
    std::size_t slash_picker_cursor = 0;
    std::string slash_picker_query;
    bool slash_picker_query_dismissed = false;
};

void render_chat_panel(ApplicationState& state, std::vector<ProviderPtr>& providers,
                       ChatPanelState& panel_state, ConversationState& conversations,
                       ChatRenderContext& render,
                       const ChatRenderResources& resources,
                       const std::shared_ptr<FileDialogQueue>& dialog_queue,
                       SDL_Window* window);
void apply_attachment_result(ChatPanelState& panel_state, const FileDialogResult& result);
#endif
