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

void add_project(ApplicationState& state) {
    std::string default_path_storage;
    if (state.selected_project < state.projects.size())
        default_path_storage = state.projects[state.selected_project].directory.string();

    char* selected_directory = tinyfd_selectFolderDialog("New Project",
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
    const float description_width = width - style.FramePadding.x * 2.0f;
    const float description_height = ImGui::CalcTextSize(thread.description.c_str(),
                                                         nullptr, false, description_width)
                                         .y;
    const float card_height = style.FramePadding.y * 2.0f + ImGui::GetTextLineHeight() +
                              style.ItemSpacing.y + description_height;

    const bool selected = ImGui::Selectable("##thread",
        state.selected_project == project_index && state.selected_thread == thread_index,
        0, ImVec2(0.0f, card_height));
    if (selected) {
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

    const ImVec2 card_min = ImGui::GetItemRectMin();
    const ImVec2 card_max = ImGui::GetItemRectMax();
    const ImVec2 cursor_after_card = ImGui::GetCursorPos();
    const float text_x = card_min.x + style.FramePadding.x;
    const float title_y = card_min.y + style.FramePadding.y;
    ImGui::SetCursorScreenPos(ImVec2(text_x, title_y));
    ImGui::TextUnformatted(thread.title.c_str());
    ImGui::SetCursorScreenPos(
        ImVec2(text_x, title_y + ImGui::GetTextLineHeight() + style.ItemSpacing.y));
    ImGui::PushTextWrapPos(card_max.x - style.FramePadding.x);
    ImGui::TextUnformatted(thread.description.c_str());
    ImGui::PopTextWrapPos();
    ImGui::SetCursorPos(cursor_after_card);

    ImGui::PopID();
    ImGui::Spacing();
}
}

void render_threads_panel(ApplicationState& state) {
    static std::optional<PendingThreadDelete> pending_delete;
    bool open_delete_confirmation = false;

    ImGui::Begin("Threads");
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 4.0f));
    const float button_line_x = ImGui::GetCursorPosX();
    const float button_width = ImGui::GetContentRegionAvail().x;
    const float horizontal_bleed = ImGui::GetStyle().ItemSpacing.x * 0.5f;
    ImGui::SetCursorPosX(button_line_x - horizontal_bleed);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
    const bool new_project = ImGui::Button("+ New Project",
        ImVec2(button_width + horizontal_bleed * 2.0f, 0.0f));
    ImGui::PopStyleVar(2);
    ImGui::SetCursorPosX(button_line_x);
    if (new_project)
        add_project(state);
    ImGui::Spacing();

    for (std::size_t project_index = 0; project_index < state.projects.size(); ++project_index) {
        ChatProject& project = state.projects[project_index];
        const std::string directory = project.directory.string();
        ImGui::PushID(directory.c_str());
        ImGui::SetNextItemOpen(project.expanded, ImGuiCond_Always);
        const bool expanded = ImGui::TreeNodeEx("##project", ImGuiTreeNodeFlags_None,
                                                "%s", project_name(project.directory).c_str());
        project.expanded = expanded;

        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
        const bool new_thread = ImGui::SmallButton("+");
        ImGui::PopStyleVar();
        if (new_thread)
            add_thread(state, project_index);

        if (expanded) {
            for (std::size_t thread_index = 0; thread_index < project.threads.size(); ++thread_index)
                render_thread_card(state, project_index, thread_index, pending_delete,
                                   open_delete_confirmation);
            ImGui::TreePop();
        }
        ImGui::PopID();
        ImGui::Spacing();
    }

    if (open_delete_confirmation)
        ImGui::OpenPopup("Confirm thread deletion");

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
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
