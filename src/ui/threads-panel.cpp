#include "threads-panel.h"
#include "../platform/message-box.h"
#include "imgui.h"
#include "ui-scale.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace {
struct PendingThreadDelete {
    std::size_t project_index;
    std::size_t thread_index;
};

struct PendingProjectDelete {
    std::size_t project_index;
};

std::string next_thread_id(const ApplicationState& state) {
    static std::uint64_t next_id = 1;
    for (;;) {
        const std::string candidate = "thread-" + std::to_string(next_id++);
        const bool exists = std::any_of(state.projects.begin(), state.projects.end(),
            [&candidate](const ChatProject& project) {
                return std::any_of(project.threads.begin(), project.threads.end(),
                    [&candidate](const ChatThread& thread) { return thread.id == candidate; });
            });
        if (!exists)
            return candidate;
    }
}

std::string project_name(const std::filesystem::path& directory) {
    const std::string name = directory.filename().string();
    return name.empty() ? directory.root_path().string() : name;
}

std::string thread_title(const ChatProject& project, std::size_t thread_index) {
    const ChatThread& thread = project.threads[thread_index];
    if (!thread.title.empty())
        return thread.title;
    return thread_index == 0 ? "New Thread"
        : "New Thread #" + std::to_string(thread_index + 1);
}

std::filesystem::path normalized_directory(const std::filesystem::path& directory,
                                            std::error_code& error) {
    std::filesystem::path normalized = std::filesystem::weakly_canonical(directory, error);
    if (error) {
        error.clear();
        normalized = directory.lexically_normal();
    }
    return normalized;
}

void render_folder_icon(ImDrawList* draw_list, ImVec2 position, float size) {
    static constexpr unsigned char alpha[12][12] = {
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
        {0xa7, 0xf1, 0xf0, 0xf0, 0xf2, 0xc9, 0x3b, 0x0e, 0x0f, 0x0f, 0x0d, 0x00},
        {0xee, 0x66, 0x36, 0x38, 0x3d, 0x9b, 0xd8, 0xc4, 0xc4, 0xc4, 0xc0, 0x75},
        {0xed, 0x31, 0x00, 0x00, 0x00, 0x08, 0x41, 0x59, 0x58, 0x56, 0x81, 0xee},
        {0xed, 0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0xed},
        {0xed, 0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x34, 0xed},
        {0xed, 0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x34, 0xed},
        {0xed, 0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x34, 0xed},
        {0xed, 0x31, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x31, 0xed},
        {0xee, 0x66, 0x36, 0x39, 0x39, 0x39, 0x39, 0x39, 0x39, 0x36, 0x66, 0xee},
        {0xa7, 0xf1, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf1, 0xa7},
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    };
    const float pixel_size = size / 12.0f;
    for (int y = 0; y < 12; ++y) {
        for (int x = 0; x < 12; ++x) {
            if (alpha[y][x] == 0)
                continue;
            const ImVec2 min(position.x + x * pixel_size, position.y + y * pixel_size);
            const ImVec2 max(min.x + pixel_size, min.y + pixel_size);
            draw_list->AddRectFilled(min, max,
                ImGui::GetColorU32(ImGuiCol_Text, alpha[y][x] / 255.0f));
        }
    }
}

void apply_open_project_selection(ApplicationState& state,
                                  const std::filesystem::path& selected_directory,
                                  SDL_Window* window) {
    std::error_code error;
    std::filesystem::path directory = normalized_directory(selected_directory, error);
    if (!std::filesystem::is_directory(directory, error) || error) {
        show_error_message("The selected project directory is unavailable.", window);
        return;
    }

    for (std::size_t i = 0; i < state.projects.size(); ++i) {
        const std::filesystem::path existing =
            normalized_directory(state.projects[i].directory, error);
        if (existing == directory) {
            state.selected_project = i;
            state.selected_thread = 0;
            state.projects[i].expanded = true;
            return;
        }
    }

    state.projects.push_back({std::move(directory), true, {}});
    state.selected_project = state.projects.size() - 1;
    state.selected_thread = 0;
}

void open_project(ApplicationState& state,
                  const std::shared_ptr<FileDialogQueue>& dialog_queue,
                  SDL_Window* window) {
    std::u8string default_path_storage;
    if (state.selected_project < state.projects.size())
        default_path_storage = state.projects[state.selected_project].directory.u8string();

    show_file_dialog(dialog_queue, FileDialogPurpose::OpenProject, window,
                     default_path_storage.empty()
                         ? nullptr
                         : reinterpret_cast<const char*>(default_path_storage.c_str()));
}

void add_thread(ApplicationState& state, std::size_t project_index) {
    ChatProject& project = state.projects[project_index];
    project.threads.insert(project.threads.begin(), {
        "",
        "",
        next_thread_id(state),
        {},
    });
    project.expanded = true;
    state.selected_project = project_index;
    state.selected_thread = 0;
}

void render_thread_card(ApplicationState& state, std::size_t project_index,
                        std::size_t thread_index, std::optional<PendingThreadDelete>& pending_delete,
                        bool& open_delete_confirmation) {
    ChatProject& project = state.projects[project_index];
    ChatThread& thread = project.threads[thread_index];
    ImGui::PushID(thread.id.c_str());
    const ImGuiStyle& style = ImGui::GetStyle();
    const float width = ImGui::GetContentRegionAvail().x;
    const float horizontal_padding = ui_size(12.0f);
    const float vertical_padding = ui_size(9.0f);
    const float description_width = std::max(1.0f, width - horizontal_padding * 2.0f);
    ImFont* description_font = ImGui::GetFont();
    ImGui::PushFont(description_font, style.FontSizeBase * 0.9f);
    const float description_height = thread.description.empty()
        ? 0.0f
        : ImGui::CalcTextSize(thread.description.c_str(), nullptr, false, description_width).y;
    ImGui::PopFont();
    const float description_spacing = thread.description.empty() ? 0.0f : style.ItemSpacing.y;
    const float card_height = vertical_padding * 2.0f + ImGui::GetTextLineHeight() +
                              description_spacing + description_height;
    const ImVec2 card_min = ImGui::GetCursorScreenPos();
    const ImVec2 card_max(card_min.x + width, card_min.y + card_height);
    const bool selected = state.selected_project == project_index &&
                          state.selected_thread == thread_index;
    const bool hovered = ImGui::IsMouseHoveringRect(card_min, card_max);
    const ImVec4 card_color = selected
        ? ImVec4(0.19f, 0.19f, 0.19f, 1.0f)
        : hovered ? ImVec4(0.15f, 0.15f, 0.15f, 1.0f)
                  : ImVec4(0.11f, 0.11f, 0.11f, 1.0f);
    ImGui::GetWindowDrawList()->AddRectFilled(card_min, card_max,
        ImGui::GetColorU32(card_color), ui_size(8.0f));

    ImGui::PushStyleVar(ImGuiStyleVar_SelectableRounding, ui_size(8.0f));
    const ImVec4 transparent(0.0f, 0.0f, 0.0f, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Header, transparent);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, transparent);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, transparent);
    const bool clicked = ImGui::Selectable("##thread", false, 0,
                                            ImVec2(width, card_height));
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
    const bool item_hovered = ImGui::IsItemHovered();
    if (clicked) {
        state.selected_project = project_index;
        state.selected_thread = thread_index;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui_size(6.0f), ui_size(6.0f)));
    if (ImGui::BeginPopupContextItem("thread_context", ImGuiPopupFlags_MouseButtonRight)) {
        const float menu_item_padding_x = ui_size(13.0f);
        const float menu_item_padding_y = ui_size(8.0f);
        const ImVec2 label_size = ImGui::CalcTextSize("Delete thread");
        const ImVec2 menu_item_pos = ImGui::GetCursorScreenPos();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableRounding, ui_size(6.0f));
        const bool delete_thread = ImGui::Selectable(
            "##delete-thread-item", false, 0,
            ImVec2(label_size.x + menu_item_padding_x * 2.0f,
                   label_size.y + menu_item_padding_y * 2.0f));
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(menu_item_pos.x + menu_item_padding_x,
                   menu_item_pos.y + menu_item_padding_y),
            ImGui::GetColorU32(ImGuiCol_Text), "Delete thread");
        ImGui::PopStyleVar(2);
        if (delete_thread) {
            pending_delete = PendingThreadDelete{project_index, thread_index};
            open_delete_confirmation = true;
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();

    const ImVec2 cursor_after_card = ImGui::GetCursorPos();
    const float text_x = card_min.x + horizontal_padding;
    const float title_y = card_min.y + vertical_padding;
    const ImVec4 title_color = selected ? ImVec4(0.95f, 0.95f, 0.95f, 1.0f)
                                        : ImVec4(0.62f, 0.62f, 0.62f, 1.0f);
    const ImVec4 description_color = selected ? ImVec4(0.62f, 0.62f, 0.62f, 1.0f)
                                              : ImVec4(0.48f, 0.48f, 0.48f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, title_color);
    ImGui::SetCursorScreenPos(ImVec2(text_x, title_y));
    ImGui::TextUnformatted(thread_title(project, thread_index).c_str());
    if (!thread.description.empty()) {
        ImGui::SetCursorScreenPos(
            ImVec2(text_x, title_y + ImGui::GetTextLineHeight() + description_spacing));
        ImGui::PushFont(description_font, style.FontSizeBase * 0.9f);
        ImGui::PushStyleColor(ImGuiCol_Text, description_color);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + description_width);
        ImGui::TextUnformatted(thread.description.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    ImGui::PopStyleColor();
    ImGui::SetCursorPos(cursor_after_card);

    if (item_hovered) {
        ImGui::GetWindowDrawList()->AddRect(card_min, card_max,
            ImGui::GetColorU32(ImGuiCol_HeaderHovered), ui_size(8.0f));
    }

    ImGui::PopID();
    ImGui::Spacing();
}
}

void open_project_dialog(ApplicationState& state,
                          const std::shared_ptr<FileDialogQueue>& dialog_queue,
                          SDL_Window* window) {
    open_project(state, dialog_queue, window);
}

void apply_open_project_result(ApplicationState& state, const FileDialogResult& result,
                               SDL_Window* window) {
    if (!result.error.empty()) {
        show_error_message(("Failed to open project dialog: " + result.error).c_str(), window);
        return;
    }
    if (!result.paths.empty())
        apply_open_project_selection(state, result.paths.front(), window);
}

void render_threads_panel(ApplicationState& state,
                          const std::shared_ptr<FileDialogQueue>& dialog_queue,
                          SDL_Window* window) {
    static std::optional<PendingThreadDelete> pending_delete;
    static std::optional<PendingProjectDelete> pending_project_delete;
    bool open_delete_confirmation = false;
    bool open_project_delete_confirmation = false;

    ImGui::Begin("Threads");
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui_size(10.0f), ui_size(4.0f)));
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Projects");
    const float folder_icon_size = ui_size(11.7f);
    const float label_width = ImGui::CalcTextSize("Open Project").x;
    const float new_project_label_width = style.FramePadding.x * 2.0f + folder_icon_size +
                                          style.ItemInnerSpacing.x + label_width;
    const bool show_project_label = new_project_label_width <= ImGui::GetContentRegionAvail().x;
    const float new_project_width = show_project_label ? new_project_label_width
        : style.FramePadding.x * 2.0f + folder_icon_size;
    const float new_project_x = ImGui::GetWindowWidth() - style.WindowPadding.x -
                                new_project_width;
    if (new_project_x >= ImGui::GetCursorPosX() + ImGui::CalcTextSize("Projects").x +
                             style.ItemSpacing.x)
        ImGui::SameLine(new_project_x);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ui_size(6.0f));
    const bool open_project_clicked = ImGui::Button("##open-project",
        ImVec2(std::min(new_project_width, ImGui::GetContentRegionAvail().x), 0.0f));
    const ImVec2 button_min = ImGui::GetItemRectMin();
    const ImVec2 button_max = ImGui::GetItemRectMax();
    const float button_height = button_max.y - button_min.y;
    const float folder_y = button_min.y + (button_height - folder_icon_size) * 0.5f;
    const float folder_x = button_min.x + style.FramePadding.x;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    render_folder_icon(draw_list, ImVec2(folder_x, folder_y), folder_icon_size);
    const float label_x = folder_x + folder_icon_size + style.ItemInnerSpacing.x;
    const float label_y = button_min.y + (button_height - ImGui::GetTextLineHeight()) * 0.5f;
    if (show_project_label)
        draw_list->AddText(ImVec2(label_x, label_y), ImGui::GetColorU32(ImGuiCol_Text),
                           "Open Project");
    else if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Open Project");
    ImGui::PopStyleVar();
    if (open_project_clicked)
        open_project(state, dialog_queue, window);
    ImGui::Spacing();

    for (std::size_t project_index = 0; project_index < state.projects.size(); ++project_index) {
        ChatProject& project = state.projects[project_index];
        const std::string directory = project.directory.string();
        ImGui::PushID(directory.c_str());
        const float row_height = ImGui::GetFrameHeight();
        const float action_gap = style.ItemInnerSpacing.x;
        const float action_size = ui_size(19.2f);
        const float action_width = action_size;
        const float header_width = std::max(1.0f,
            ImGui::GetContentRegionAvail().x - action_width - action_gap);
        const bool header_clicked = ImGui::InvisibleButton("##project-header",
            ImVec2(header_width, row_height), ImGuiButtonFlags_EnableNav);
        const bool header_hovered = ImGui::IsItemHovered();
        const ImVec2 header_min = ImGui::GetItemRectMin();
        const ImVec2 header_max = ImGui::GetItemRectMax();

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui_size(6.0f), ui_size(6.0f)));
        if (ImGui::BeginPopupContextItem("project_context", ImGuiPopupFlags_MouseButtonRight)) {
            const float menu_item_padding_x = ui_size(13.0f);
            const float menu_item_padding_y = ui_size(8.0f);
            const ImVec2 label_size = ImGui::CalcTextSize("Delete project");
            const ImVec2 menu_item_pos = ImGui::GetCursorScreenPos();
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_SelectableRounding, ui_size(6.0f));
            const bool delete_project = ImGui::Selectable(
                "##delete-project-item", false, 0,
                ImVec2(label_size.x + menu_item_padding_x * 2.0f,
                       label_size.y + menu_item_padding_y * 2.0f));
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(menu_item_pos.x + menu_item_padding_x,
                       menu_item_pos.y + menu_item_padding_y),
                ImGui::GetColorU32(ImGuiCol_Text), "Delete project");
            ImGui::PopStyleVar(2);
            if (delete_project) {
                pending_project_delete = PendingProjectDelete{project_index};
                open_project_delete_confirmation = true;
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();

        if (header_clicked)
            project.expanded = !project.expanded;

        const ImVec2 arrow_center(header_min.x + row_height * 0.31f,
                                  header_min.y + row_height * 0.5f);
        ImVec2 arrow_points[3];
        if (project.expanded) {
            arrow_points[0] = ImVec2(arrow_center.x - ui_size(3.2f), arrow_center.y - ui_size(2.0f));
            arrow_points[1] = ImVec2(arrow_center.x, arrow_center.y + ui_size(2.0f));
            arrow_points[2] = ImVec2(arrow_center.x + ui_size(3.2f), arrow_center.y - ui_size(2.0f));
        } else {
            arrow_points[0] = ImVec2(arrow_center.x - ui_size(2.0f), arrow_center.y - ui_size(3.2f));
            arrow_points[1] = ImVec2(arrow_center.x + ui_size(2.0f), arrow_center.y);
            arrow_points[2] = ImVec2(arrow_center.x - ui_size(2.0f), arrow_center.y + ui_size(3.2f));
        }
        const float name_value = std::min(1.0f,
            (project.expanded ? 0.90f : 0.70f) + (header_hovered ? 0.10f : 0.0f));
        const float chevron_value = (project.expanded ? 0.64f : 0.48f) +
                                    (header_hovered ? 0.10f : 0.0f);
        const ImU32 arrow_color = ImGui::GetColorU32(
            ImVec4(chevron_value, chevron_value, chevron_value, 1.0f));
        draw_list->AddLine(arrow_points[0], arrow_points[1], arrow_color, ui_size(1.6f));
        draw_list->AddLine(arrow_points[1], arrow_points[2], arrow_color, ui_size(1.6f));
        const float project_text_x = header_min.x + row_height * 0.69f;
        const float project_text_y = header_min.y + (row_height - ImGui::GetTextLineHeight()) * 0.5f;
        draw_list->PushClipRect(ImVec2(project_text_x, header_min.y), header_max, true);
        draw_list->AddText(ImVec2(project_text_x, project_text_y),
                           ImGui::GetColorU32(
                               ImVec4(name_value, name_value, name_value, 1.0f)),
                           project_name(project.directory).c_str());
        draw_list->PopClipRect();

        ImGui::SameLine(0.0f, action_gap);
        ImGui::SetCursorScreenPos(ImVec2(header_max.x + action_gap,
                                         header_min.y + (row_height - action_size) * 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ui_size(5.6f));
        ImGui::PushStyleColor(ImGuiCol_Button, style.Colors[ImGuiCol_FrameBg]);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, style.Colors[ImGuiCol_HeaderHovered]);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, style.Colors[ImGuiCol_HeaderActive]);
        const bool new_thread = ImGui::Button("##add-thread", ImVec2(action_size, action_size));
        const ImVec2 add_button_min = ImGui::GetItemRectMin();
        const ImVec2 add_button_max = ImGui::GetItemRectMax();
        const ImVec2 add_center(
            std::floor((add_button_min.x + add_button_max.x) * 0.5f) - 0.5f,
            std::floor((add_button_min.y + add_button_max.y) * 0.5f) - 0.5f);
        const float plus_half_size = ui_size(3.2f);
        const float plus_thickness = ui_size(1.44f);
        const ImU32 add_color = ImGui::GetColorU32(ImGuiCol_Text);
        draw_list->AddLine(ImVec2(add_center.x - plus_half_size, add_center.y),
                           ImVec2(add_center.x + plus_half_size, add_center.y),
                           add_color, plus_thickness);
        draw_list->AddLine(ImVec2(add_center.x, add_center.y - plus_half_size),
                           ImVec2(add_center.x, add_center.y + plus_half_size),
                           add_color, plus_thickness);
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
        if (new_thread)
            add_thread(state, project_index);

        if (project.expanded) {
            ImGui::Spacing();
            for (std::size_t thread_index = 0; thread_index < project.threads.size(); ++thread_index)
                render_thread_card(state, project_index, thread_index, pending_delete,
                                   open_delete_confirmation);
        }
        ImGui::PopID();
        ImGui::Spacing();
    }

    if (open_project_delete_confirmation)
        ImGui::OpenPopup("Confirm project deletion");
    if (open_delete_confirmation)
        ImGui::OpenPopup("Confirm thread deletion");

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ui_size(6.0f));
    if (ImGui::BeginPopupModal("Confirm project deletion", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        const bool valid_pending = pending_project_delete &&
            pending_project_delete->project_index < state.projects.size();
        if (valid_pending) {
            const std::size_t project_index = pending_project_delete->project_index;
            const ChatProject& project = state.projects[project_index];
            ImGui::Text("Delete project \"%s\" and its saved threads?",
                        project_name(project.directory).c_str());
            ImGui::TextUnformatted("The project directory and its files will remain on disk.");
            ImGui::Spacing();

            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ui_size(6.0f));
            const bool delete_project = ImGui::Button("Delete", ImVec2(ui_size(120.0f), 0.0f));
            ImGui::PopStyleVar();
            if (delete_project) {
                state.projects.erase(state.projects.begin() +
                                     static_cast<std::ptrdiff_t>(project_index));
                if (state.projects.empty()) {
                    state.selected_project = 0;
                    state.selected_thread = 0;
                } else if (state.selected_project == project_index) {
                    state.selected_project = std::min(project_index, state.projects.size() - 1);
                    state.selected_thread = 0;
                } else if (state.selected_project > project_index) {
                    --state.selected_project;
                }
                pending_project_delete.reset();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ui_size(6.0f));
            const bool cancel_delete = ImGui::Button("Cancel", ImVec2(ui_size(120.0f), 0.0f));
            ImGui::PopStyleVar();
            if (cancel_delete) {
                pending_project_delete.reset();
                ImGui::CloseCurrentPopup();
            }
        } else {
            pending_project_delete.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Confirm thread deletion", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        const bool valid_pending = pending_delete &&
            pending_delete->project_index < state.projects.size() &&
            pending_delete->thread_index <
                state.projects[pending_delete->project_index].threads.size();
        if (valid_pending) {
            const std::size_t project_index = pending_delete->project_index;
            const std::size_t thread_index = pending_delete->thread_index;
            ChatProject& project = state.projects[project_index];
            ImGui::Text("Delete thread \"%s\"?",
                        thread_title(project, thread_index).c_str());
            ImGui::TextUnformatted("This cannot be undone.");
            ImGui::Spacing();

            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ui_size(6.0f));
            const bool delete_thread = ImGui::Button("Delete", ImVec2(ui_size(120.0f), 0.0f));
            ImGui::PopStyleVar();
            if (delete_thread) {
                project.threads.erase(project.threads.begin() +
                                      static_cast<std::ptrdiff_t>(thread_index));
                if (state.selected_project == project_index) {
                    if (project.threads.empty()) {
                        state.selected_thread = 0;
                    } else if (state.selected_thread > thread_index) {
                        --state.selected_thread;
                    } else if (state.selected_thread >= project.threads.size()) {
                        state.selected_thread = project.threads.size() - 1;
                    }
                }
                pending_delete.reset();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ui_size(6.0f));
            const bool cancel_delete = ImGui::Button("Cancel", ImVec2(ui_size(120.0f), 0.0f));
            ImGui::PopStyleVar();
            if (cancel_delete) {
                pending_delete.reset();
                ImGui::CloseCurrentPopup();
            }
        } else {
            pending_delete.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();

    ImGui::PopStyleVar();
    ImGui::End();
}
