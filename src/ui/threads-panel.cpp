#include "threads-panel.h"
#include "imgui.h"
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <tinyfiledialogs.h>
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

void open_project(ApplicationState& state) {
    std::string default_path_storage;
    if (state.selected_project < state.projects.size())
        default_path_storage = state.projects[state.selected_project].directory.string();

    char* selected_directory = tinyfd_selectFolderDialog("Open Project",
        default_path_storage.empty() ? nullptr : default_path_storage.c_str());
    if (selected_directory == nullptr)
        return;

    std::error_code error;
    std::filesystem::path directory = normalized_directory(selected_directory, error);
    if (!std::filesystem::is_directory(directory, error) || error) {
        tinyfd_messageBox("Zenith", "The selected project directory is unavailable.",
                          "ok", "error", 1);
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

void add_thread(ApplicationState& state, std::size_t project_index) {
    ChatProject& project = state.projects[project_index];
    project.threads.insert(project.threads.begin(), {
        "New thread " + std::to_string(project.threads.size() + 1),
        "A new conversation.",
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
    constexpr float horizontal_padding = 12.0f;
    constexpr float vertical_padding = 9.0f;
    const float description_width = std::max(1.0f, width - horizontal_padding * 2.0f);
    const float description_height = thread.description.empty()
        ? 0.0f
        : ImGui::CalcTextSize(thread.description.c_str(), nullptr, false,
                              description_width).y;
    const float description_spacing = thread.description.empty() ? 0.0f : style.ItemSpacing.y;
    const float card_height = vertical_padding * 2.0f + ImGui::GetTextLineHeight() +
                              description_spacing + description_height;
    const ImVec2 card_min = ImGui::GetCursorScreenPos();
    const ImVec2 card_max(card_min.x + width, card_min.y + card_height);
    const bool selected = state.selected_project == project_index &&
                          state.selected_thread == thread_index;
    const ImVec4 card_color = selected ? style.Colors[ImGuiCol_Header]
                                       : style.Colors[ImGuiCol_FrameBg];
    ImGui::GetWindowDrawList()->AddRectFilled(card_min, card_max,
        ImGui::GetColorU32(card_color), 8.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_SelectableRounding, 8.0f);
    const bool clicked = ImGui::Selectable("##thread", selected, 0,
                                             ImVec2(width, card_height));
    ImGui::PopStyleVar();
    const bool hovered = ImGui::IsItemHovered();
    if (clicked) {
        state.selected_project = project_index;
        state.selected_thread = thread_index;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 6.0f));
    if (ImGui::BeginPopupContextItem("thread_context", ImGuiPopupFlags_MouseButtonRight)) {
        constexpr float menu_item_padding_x = 13.0f;
        constexpr float menu_item_padding_y = 8.0f;
        const ImVec2 label_size = ImGui::CalcTextSize("Delete thread");
        const ImVec2 menu_item_pos = ImGui::GetCursorScreenPos();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableRounding, 6.0f);
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
    ImGui::SetCursorScreenPos(ImVec2(text_x, title_y));
    ImGui::TextUnformatted(thread.title.c_str());
    if (!thread.description.empty()) {
        ImGui::SetCursorScreenPos(
            ImVec2(text_x, title_y + ImGui::GetTextLineHeight() + description_spacing));
        ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_TextDisabled]);
        ImGui::PushTextWrapPos(card_max.x - horizontal_padding);
        ImGui::TextUnformatted(thread.description.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    ImGui::SetCursorPos(cursor_after_card);

    if (hovered) {
        ImGui::GetWindowDrawList()->AddRect(card_min, card_max,
            ImGui::GetColorU32(ImGuiCol_HeaderHovered), 8.0f);
    }

    ImGui::PopID();
    ImGui::Spacing();
}
}

void open_project_dialog(ApplicationState& state) {
    open_project(state);
}

void render_threads_panel(ApplicationState& state) {
    static std::optional<PendingThreadDelete> pending_delete;
    static std::optional<PendingProjectDelete> pending_project_delete;
    bool open_delete_confirmation = false;
    bool open_project_delete_confirmation = false;

    ImGui::Begin("Threads");
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 4.0f));
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Projects");
    constexpr float folder_icon_size = 11.7f;
    const float label_width = ImGui::CalcTextSize("Open Project").x;
    const float new_project_width = style.FramePadding.x * 2.0f + folder_icon_size +
                                    style.ItemInnerSpacing.x + label_width;
    const float new_project_x = ImGui::GetWindowWidth() - style.WindowPadding.x -
                                new_project_width;
    ImGui::SameLine(new_project_x);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
    const bool open_project_clicked = ImGui::Button("##open-project",
        ImVec2(new_project_width, 0.0f));
    const ImVec2 button_min = ImGui::GetItemRectMin();
    const ImVec2 button_max = ImGui::GetItemRectMax();
    const float button_height = button_max.y - button_min.y;
    const float folder_y = button_min.y + (button_height - folder_icon_size) * 0.5f;
    const float folder_x = button_min.x + style.FramePadding.x;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    render_folder_icon(draw_list, ImVec2(folder_x, folder_y), folder_icon_size);
    const float label_x = folder_x + folder_icon_size + style.ItemInnerSpacing.x;
    const float label_y = button_min.y + (button_height - ImGui::GetTextLineHeight()) * 0.5f;
    draw_list->AddText(ImVec2(label_x, label_y), ImGui::GetColorU32(ImGuiCol_Text),
                       "Open Project");
    ImGui::PopStyleVar();
    if (open_project_clicked)
        open_project(state);
    ImGui::Spacing();

    for (std::size_t project_index = 0; project_index < state.projects.size(); ++project_index) {
        ChatProject& project = state.projects[project_index];
        const std::string directory = project.directory.string();
        ImGui::PushID(directory.c_str());
        const float row_height = ImGui::GetFrameHeight();
        const float action_gap = style.ItemInnerSpacing.x;
        const float action_width = row_height;
        const float header_width = std::max(1.0f,
            ImGui::GetContentRegionAvail().x - action_width - action_gap);
        const bool header_clicked = ImGui::InvisibleButton("##project-header",
            ImVec2(header_width, row_height), ImGuiButtonFlags_EnableNav);
        const bool header_hovered = ImGui::IsItemHovered();
        const bool header_active = ImGui::IsItemActive();
        const ImVec2 header_min = ImGui::GetItemRectMin();
        const ImVec2 header_max = ImGui::GetItemRectMax();

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 6.0f));
        if (ImGui::BeginPopupContextItem("project_context", ImGuiPopupFlags_MouseButtonRight)) {
            constexpr float menu_item_padding_x = 13.0f;
            constexpr float menu_item_padding_y = 8.0f;
            const ImVec2 label_size = ImGui::CalcTextSize("Delete project");
            const ImVec2 menu_item_pos = ImGui::GetCursorScreenPos();
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_SelectableRounding, 6.0f);
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

        const ImVec4 header_color = header_active
            ? style.Colors[ImGuiCol_HeaderActive]
            : header_hovered
                ? style.Colors[ImGuiCol_HeaderHovered]
                : project.expanded
                    ? style.Colors[ImGuiCol_Header]
                    : style.Colors[ImGuiCol_FrameBg];
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        draw_list->AddRectFilled(header_min, header_max, ImGui::GetColorU32(header_color), 7.0f);

        const ImVec2 arrow_center(header_min.x + row_height * 0.5f,
                                  header_min.y + row_height * 0.5f);
        ImVec2 arrow_points[3];
        if (project.expanded) {
            arrow_points[0] = ImVec2(arrow_center.x - 5.0f, arrow_center.y - 2.5f);
            arrow_points[1] = ImVec2(arrow_center.x + 5.0f, arrow_center.y - 2.5f);
            arrow_points[2] = ImVec2(arrow_center.x, arrow_center.y + 4.5f);
        } else {
            arrow_points[0] = ImVec2(arrow_center.x - 2.5f, arrow_center.y - 5.0f);
            arrow_points[1] = ImVec2(arrow_center.x - 2.5f, arrow_center.y + 5.0f);
            arrow_points[2] = ImVec2(arrow_center.x + 4.5f, arrow_center.y);
        }
        draw_list->AddTriangleFilled(arrow_points[0], arrow_points[1], arrow_points[2],
                                     ImGui::GetColorU32(ImGuiCol_Text));
        const float project_text_x = header_min.x + row_height;
        const float project_text_y = header_min.y + (row_height - ImGui::GetTextLineHeight()) * 0.5f;
        draw_list->PushClipRect(ImVec2(project_text_x, header_min.y), header_max, true);
        draw_list->AddText(ImVec2(project_text_x, project_text_y),
                           ImGui::GetColorU32(ImGuiCol_Text),
                           project_name(project.directory).c_str());
        draw_list->PopClipRect();

        ImGui::SameLine(0.0f, action_gap);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, style.Colors[ImGuiCol_FrameBg]);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, style.Colors[ImGuiCol_HeaderHovered]);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, style.Colors[ImGuiCol_HeaderActive]);
        const bool new_thread = ImGui::Button("+", ImVec2(action_width, row_height));
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
        if (new_thread)
            add_thread(state, project_index);

        if (project.expanded) {
            ImGui::Indent(style.IndentSpacing);
            ImGui::Spacing();
            for (std::size_t thread_index = 0; thread_index < project.threads.size(); ++thread_index)
                render_thread_card(state, project_index, thread_index, pending_delete,
                                   open_delete_confirmation);
            ImGui::Unindent(style.IndentSpacing);
        }
        ImGui::PopID();
        ImGui::Spacing();
    }

    if (open_project_delete_confirmation)
        ImGui::OpenPopup("Confirm project deletion");
    if (open_delete_confirmation)
        ImGui::OpenPopup("Confirm thread deletion");

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
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

            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
            const bool delete_project = ImGui::Button("Delete", ImVec2(120.0f, 0.0f));
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
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
            const bool cancel_delete = ImGui::Button("Cancel", ImVec2(120.0f, 0.0f));
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
            ImGui::Text("Delete thread \"%s\"?", project.threads[thread_index].title.c_str());
            ImGui::TextUnformatted("This cannot be undone.");
            ImGui::Spacing();

            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
            const bool delete_thread = ImGui::Button("Delete", ImVec2(120.0f, 0.0f));
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
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
            const bool cancel_delete = ImGui::Button("Cancel", ImVec2(120.0f, 0.0f));
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
