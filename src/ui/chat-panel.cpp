#include "chat-panel.h"
#include "imgui.h"
#include "imgui_md.h"
#include "misc/cpp/imgui_stdlib.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <utility>

namespace {
class ChatMarkdown : public imgui_md {
public:
    ImVec4 get_color() const override {
        return m_href.empty() ? ImGui::GetStyle().Colors[ImGuiCol_Text]
                              : ImVec4(0.82f, 0.84f, 0.90f, 1.0f);
    }
};

void render_tool_icon() {
    const ImVec2 center = ImGui::GetCursorScreenPos();
    const float radius = ImGui::GetTextLineHeight() * 0.32f;
    const ImVec2 icon_center(center.x + radius, center.y + ImGui::GetTextLineHeight() * 0.5f);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImU32 color = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 1.0f));

    draw_list->AddCircle(icon_center, radius, color, 12, 1.8f);
    draw_list->AddCircleFilled(icon_center, radius * 0.3f, color, 8);
    for (int spoke = 0; spoke < 8; ++spoke) {
        const float angle = static_cast<float>(spoke) * 0.785398163f;
        const ImVec2 inner(icon_center.x + std::cos(angle) * radius * 0.78f,
                           icon_center.y + std::sin(angle) * radius * 0.78f);
        const ImVec2 outer(icon_center.x + std::cos(angle) * radius * 1.18f,
                           icon_center.y + std::sin(angle) * radius * 1.18f);
        draw_list->AddLine(inner, outer, color, 1.8f);
    }
    ImGui::Dummy(ImVec2(radius * 2.4f, ImGui::GetTextLineHeight()));
}

void render_tool_activity(const ToolActivity& tool) {
    const std::string status = tool.status.empty()
        ? (tool.completed ? "Completed" : "Running") : tool.status;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.12f, 0.12f, 0.12f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.17f, 0.17f, 0.17f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.28f, 0.28f, 0.28f, 1.0f));

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->ChannelsSplit(2);
    draw_list->ChannelsSetCurrent(1);
    const ImVec2 card_min = ImGui::GetCursorScreenPos();
    ImGui::BeginGroup();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 14.0f));
    const bool expanded = ImGui::TreeNodeEx("##tool-card", ImGuiTreeNodeFlags_SpanAvailWidth, "Tool");
    ImGui::PopStyleVar();
    ImGui::SameLine();
    render_tool_icon();
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.72f, 0.82f, 0.90f, 1.0f), "%s", status.c_str());
    if (expanded) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", tool.command.empty() ? "Command details unavailable" : tool.command.c_str());
        if (!tool.cwd.empty())
            ImGui::TextWrapped("Working directory: %s", tool.cwd.c_str());
        if (tool.exit_code.has_value())
            ImGui::Text("Exit code: %d", *tool.exit_code);
        if (tool.duration_ms.has_value())
            ImGui::Text("Duration: %d ms", *tool.duration_ms);
        if (!tool.output.empty() && ImGui::TreeNode("##tool-output", "Output")) {
            ImGui::TextWrapped("%s", tool.output.c_str());
            ImGui::TreePop();
        }
        ImGui::Dummy(ImVec2(1.0f, 10.0f));
        ImGui::TreePop();
    }
    ImGui::EndGroup();
    const ImVec2 card_max = ImGui::GetItemRectMax();
    draw_list->ChannelsSetCurrent(0);
    draw_list->AddRectFilled(card_min, ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x, card_max.y),
                             ImGui::GetColorU32(ImVec4(0.105f, 0.105f, 0.105f, 1.0f)));
    draw_list->ChannelsMerge();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
}

void render_user_message(const ChatMessage& message) {
    constexpr float horizontal_padding = 12.0f;
    constexpr float vertical_padding = 8.0f;
    constexpr float max_width_ratio = 0.8f;
    const float available_width = ImGui::GetContentRegionAvail().x;
    const float max_text_width = std::max(1.0f, available_width * max_width_ratio -
                                                   horizontal_padding * 2.0f);
    const ImVec2 measured = ImGui::CalcTextSize(message.content.c_str(), nullptr, false,
                                                max_text_width);
    const float text_width = std::min(max_text_width, std::max(1.0f, measured.x));
    const float text_height = std::max(ImGui::GetTextLineHeight(), measured.y);
    const ImVec2 row_pos = ImGui::GetCursorScreenPos();
    const float bubble_width = text_width + horizontal_padding * 2.0f;
    const float bubble_height = text_height + vertical_padding * 2.0f;
    const ImVec2 bubble_min(row_pos.x + available_width - bubble_width, row_pos.y);
    const ImVec2 bubble_max(bubble_min.x + bubble_width, bubble_min.y + bubble_height);

    ImGui::GetWindowDrawList()->AddRectFilled(
        bubble_min, bubble_max, ImGui::GetColorU32(ImVec4(0.20f, 0.20f, 0.20f, 1.0f)), 6.0f);
    ImGui::SetCursorScreenPos(ImVec2(bubble_min.x + horizontal_padding,
                                     bubble_min.y + vertical_padding));
    ImGui::PushTextWrapPos(bubble_min.x + horizontal_padding + max_text_width);
    ImGui::TextWrapped("%s", message.content.c_str());
    ImGui::PopTextWrapPos();
    ImGui::SetCursorScreenPos(row_pos);
    ImGui::Dummy(ImVec2(available_width, bubble_height));
}

}

void render_chat_panel(ApplicationState& state, std::string& message_input, Provider& provider,
                       std::string& selected_model, std::string& selected_reasoning_effort,
                       bool& is_generating, TurnId& active_turn_id, TurnId& next_turn_id,
                       std::string& progress_text, const std::string& progress_conversation_id) {
    static ChatMarkdown markdown;
    if (!provider.models.empty()) {
        const auto selected = std::find_if(provider.models.begin(), provider.models.end(),
            [&](const ModelOption& model) { return model.id == selected_model; });
        if (selected == provider.models.end()) {
            const auto preferred = std::find_if(provider.models.begin(), provider.models.end(),
                [&](const ModelOption& model) { return model.id == provider.default_model; });
            selected_model = (preferred == provider.models.end() ? provider.models.front() : *preferred).id;
            selected_reasoning_effort.clear();
        }
        const auto active = std::find_if(provider.models.begin(), provider.models.end(),
            [&](const ModelOption& model) { return model.id == selected_model; });
        if (active != provider.models.end()) {
            const bool effort_supported = std::any_of(active->reasoning_efforts.begin(),
                active->reasoning_efforts.end(), [&](const ReasoningOption& option) {
                    return option.value == selected_reasoning_effort;
                });
            if (selected_reasoning_effort.empty() || !effort_supported)
                selected_reasoning_effort = active->default_reasoning_effort;
        }
    }
    ImGui::Begin("Chat");
    if (state.selected_project < state.projects.size() &&
        state.selected_thread < state.projects[state.selected_project].threads.size()) {
        ChatProject& project = state.projects[state.selected_project];
        std::vector<ChatThread>& threads = project.threads;
        ChatThread& thread = threads[state.selected_thread];
        constexpr float outer_padding = 8.0f;
        constexpr float input_height = 58.0f;
        constexpr float footer_height = 38.0f;
        constexpr float composer_height = outer_padding + input_height + footer_height;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        const float available_height = ImGui::GetContentRegionAvail().y;
        const float message_height = std::max(
            0.0f, available_height - composer_height - ImGui::GetStyle().ItemSpacing.y);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::BeginChild("##messages", ImVec2(0.0f, message_height), false);
        const bool was_at_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 6.0f));
        for (std::size_t message_index = 0; message_index < thread.messages.size(); ++message_index) {
            const ChatMessage& message = thread.messages[message_index];
            ImGui::PushID(static_cast<int>(message_index));
            ImGui::BeginGroup();
            if (message.role == ChatMessageRole::User) {
                render_user_message(message);
            } else {
                if (!message.reasoning.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.72f, 0.82f, 1.0f));
                    if (ImGui::TreeNode("##thinking", "Thinking")) {
                        ImGui::Indent();
                        markdown.print(message.reasoning.c_str(),
                                       message.reasoning.c_str() + message.reasoning.size());
                        ImGui::Unindent();
                        ImGui::TreePop();
                    }
                    ImGui::PopStyleColor();
                }
                if (!message.segments.empty()) {
                    for (const ChatSegment& segment : message.segments) {
                        if (segment.kind == ChatSegment::Kind::Tool) {
                            ImGui::PushID(segment.tool.id.c_str());
                            render_tool_activity(segment.tool);
                            ImGui::PopID();
                        } else if (!segment.text.empty()) {
                            markdown.print(segment.text.c_str(), segment.text.c_str() + segment.text.size());
                        }
                    }
                } else {
                    for (const std::string& activity : message.tool_activities) {
                        ToolActivity tool;
                        tool.command = activity;
                        render_tool_activity(tool);
                    }
                    markdown.print(message.content.c_str(),
                                   message.content.c_str() + message.content.size());
                }
            }
            ImGui::EndGroup();
            ImGui::PopID();
            ImGui::Dummy(ImVec2(1.0f, 9.0f));
        }
        ImGui::PopStyleVar();
        if (progress_conversation_id == thread.id && !progress_text.empty()) {
            if (ImGui::TreeNode("##thinking-progress", "Thinking")) {
                markdown.print(progress_text.c_str(), progress_text.c_str() + progress_text.size());
                ImGui::TreePop();
            }
        }
        if (was_at_bottom)
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        const ImVec2 input_pos = ImGui::GetCursorScreenPos();
        const float full_width = ImGui::GetContentRegionAvail().x;
        constexpr float total_height = composer_height;
        const float send_size = 32.0f;
        const ImVec2 frame_max(input_pos.x + full_width, input_pos.y + total_height);
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        draw_list->AddRectFilled(input_pos, frame_max,
                                 ImGui::GetColorU32(ImGuiCol_FrameBg), 6.0f);
        draw_list->AddRect(input_pos, frame_max,
                           ImGui::GetColorU32(ImGuiCol_Border), 6.0f);
        const float divider_y = input_pos.y + outer_padding + input_height;
        draw_list->AddLine(ImVec2(input_pos.x + 1.0f, divider_y),
                           ImVec2(frame_max.x - 1.0f, divider_y),
                           ImGui::GetColorU32(ImGuiCol_Border), 1.0f);

        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::SetCursorScreenPos(ImVec2(input_pos.x + outer_padding,
                                         input_pos.y + outer_padding));
        const bool enter = ImGui::InputTextMultiline(
            "##message-input", &message_input,
            ImVec2(full_width - outer_padding * 2.0f, input_height),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine);
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
        const ImVec2 send_pos(frame_max.x - send_size - outer_padding,
                              divider_y + (footer_height - send_size) * 0.5f);
        const float selector_y = divider_y + (footer_height - ImGui::GetFrameHeight()) * 0.5f;
        const float selector_x = input_pos.x + outer_padding;
        const float selector_available = std::max(0.0f, send_pos.x - selector_x - 12.0f);
        const float model_width = std::min(190.0f, selector_available * 0.62f);
        const float reasoning_width = std::min(140.0f, std::max(0.0f, selector_available - model_width - 8.0f));
        const auto active_model = std::find_if(provider.models.begin(), provider.models.end(),
            [&](const ModelOption& model) { return model.id == selected_model; });
        if (active_model != provider.models.end() && model_width >= 60.0f) {
            ImGui::SetCursorScreenPos(ImVec2(selector_x, selector_y));
            ImGui::SetNextItemWidth(model_width);
            if (ImGui::BeginCombo("##model-selector", active_model->name.c_str())) {
                for (const ModelOption& model : provider.models) {
                    const bool is_selected = model.id == selected_model;
                    if (ImGui::Selectable(model.name.c_str(), is_selected)) {
                        if (!is_selected) {
                            selected_model = model.id;
                            selected_reasoning_effort = model.default_reasoning_effort;
                        }
                    }
                    if (is_selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            const auto reasoning_model = std::find_if(provider.models.begin(), provider.models.end(),
                [&](const ModelOption& model) { return model.id == selected_model; });
            if (reasoning_width >= 60.0f) {
                const std::string effort_label = selected_reasoning_effort.empty()
                    ? "Default" : selected_reasoning_effort;
                ImGui::SetCursorScreenPos(ImVec2(selector_x + model_width + 8.0f, selector_y));
                ImGui::SetNextItemWidth(reasoning_width);
                if (ImGui::BeginCombo("##reasoning-selector", effort_label.c_str())) {
                    if (reasoning_model == provider.models.end() || reasoning_model->reasoning_efforts.empty()) {
                        ImGui::BeginDisabled();
                        ImGui::Selectable("Default", true);
                        ImGui::EndDisabled();
                    } else {
                        for (const ReasoningOption& option : reasoning_model->reasoning_efforts) {
                            const bool is_selected = option.value == selected_reasoning_effort;
                            if (ImGui::Selectable(option.value.c_str(), is_selected))
                                selected_reasoning_effort = option.value;
                            if (is_selected)
                                ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
            }
        }
        ImGui::SetCursorScreenPos(send_pos);
        const bool send_clicked = ImGui::InvisibleButton("##send-message", ImVec2(send_size, send_size));
        const ImVec2 button_min = ImGui::GetItemRectMin();
        const ImVec2 button_max = ImGui::GetItemRectMax();
        const ImU32 button_color = ImGui::GetColorU32(
            ImGui::IsItemActive() ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f)
            : ImGui::IsItemHovered() ? ImVec4(0.42f, 0.42f, 0.42f, 1.0f)
                                     : ImVec4(0.30f, 0.30f, 0.30f, 1.0f));
        draw_list->AddRectFilled(button_min, button_max, button_color, 6.0f);
        const ImVec2 arrow_center((button_min.x + button_max.x) * 0.5f,
                                  (button_min.y + button_max.y) * 0.5f);
        const ImU32 arrow_color = ImGui::GetColorU32(ImGui::IsItemActive()
            ? ImVec4(0.08f, 0.08f, 0.08f, 1.0f)
            : ImVec4(0.94f, 0.94f, 0.94f, 1.0f));
        if (is_generating) {
            const ImVec2 half_size(5.0f, 5.0f);
            draw_list->AddRectFilled(ImVec2(arrow_center.x - half_size.x, arrow_center.y - half_size.y),
                                     ImVec2(arrow_center.x + half_size.x, arrow_center.y + half_size.y),
                                     arrow_color, 1.0f);
        } else {
            draw_list->AddLine(ImVec2(arrow_center.x, arrow_center.y + 6.0f),
                               ImVec2(arrow_center.x, arrow_center.y - 5.0f), arrow_color, 2.0f);
            draw_list->AddLine(ImVec2(arrow_center.x, arrow_center.y - 5.0f),
                               ImVec2(arrow_center.x - 4.5f, arrow_center.y - 0.5f), arrow_color, 2.0f);
            draw_list->AddLine(ImVec2(arrow_center.x, arrow_center.y - 5.0f),
                               ImVec2(arrow_center.x + 4.5f, arrow_center.y - 0.5f), arrow_color, 2.0f);
        }
        ImGui::SetCursorScreenPos(input_pos);
        ImGui::Dummy(ImVec2(full_width, total_height));
        if (send_clicked && is_generating) {
            provider.cancel(&provider, active_turn_id);
            is_generating = false;
            active_turn_id = 0;
            progress_text.clear();
        } else if (!is_generating && (enter || send_clicked) && !message_input.empty()) {
            if (state.selected_thread > 0) {
                std::rotate(threads.begin(),
                            threads.begin() + state.selected_thread,
                            threads.begin() + state.selected_thread + 1);
                state.selected_thread = 0;
            }
            ChatThread& destination = threads[state.selected_thread];
            std::string prompt = std::move(message_input);
            message_input.clear();
            destination.messages.push_back({ChatMessageRole::User, prompt, {}, {}, {}});
            TurnRequest request;
            request.conversation_id = destination.id;
            request.prompt = std::move(prompt);
            request.history = destination.messages;
            request.working_directory = project.directory;
            request.model = selected_model;
            request.reasoning_effort = selected_reasoning_effort;
            request.turn_id = next_turn_id++;
            const Result submitted = provider.submit(&provider, std::move(request));
            if (submitted.status == ResultStatus::Error) {
                destination.messages.push_back(
                    {ChatMessageRole::Assistant, std::string(submitted.error), {}, {}, {}});
            } else {
                active_turn_id = next_turn_id - 1;
                is_generating = true;
            }
        }
    }
    ImGui::End();
}
