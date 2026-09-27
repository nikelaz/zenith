#include "chat-panel.h"
#include "imgui.h"
#include "imgui_md.h"
#include "misc/cpp/imgui_stdlib.h"
#include <algorithm>
#include <cfloat>
#include <cctype>
#include <utility>

namespace {
class ChatMarkdown : public imgui_md {
public:
    ImVec4 get_color() const override {
        return m_href.empty() ? ImGui::GetStyle().Colors[ImGuiCol_Text]
                              : ImVec4(0.82f, 0.84f, 0.90f, 1.0f);
    }
};

enum class ActivityIcon {
    Terminal,
    Tool,
    Reasoning,
};

void render_terminal_icon(ImDrawList* draw_list, ImVec2 position, ImU32 color) {
    const ImVec2 first(position.x + 0.5f, position.y + 1.5f);
    const ImVec2 corner(position.x + 4.5f, position.y + 5.0f);
    const ImVec2 last(position.x + 0.5f, position.y + 8.5f);
    draw_list->AddLine(first, corner, color, 1.5f);
    draw_list->AddLine(corner, last, color, 1.5f);
    draw_list->AddLine(ImVec2(position.x + 5.1f, position.y + 8.4f),
                       ImVec2(position.x + 12.5f, position.y + 8.4f), color, 1.5f);
}

void render_tool_icon(ImDrawList* draw_list, ImVec2 position, ImU32 color) {
    draw_list->PathLineTo(ImVec2(position.x + 5.0f, position.y + 1.7f));
    draw_list->PathLineTo(ImVec2(position.x + 6.7f, position.y + 1.0f));
    draw_list->PathLineTo(ImVec2(position.x + 8.9f, position.y + 1.8f));
    draw_list->PathLineTo(ImVec2(position.x + 10.3f, position.y + 3.1f));
    draw_list->PathLineTo(ImVec2(position.x + 10.7f, position.y + 4.9f));
    draw_list->PathLineTo(ImVec2(position.x + 11.1f, position.y + 5.3f));
    draw_list->PathLineTo(ImVec2(position.x + 12.2f, position.y + 5.3f));
    draw_list->PathLineTo(ImVec2(position.x + 12.3f, position.y + 6.4f));
    draw_list->PathLineTo(ImVec2(position.x + 11.0f, position.y + 7.7f));
    draw_list->PathLineTo(ImVec2(position.x + 9.8f, position.y + 7.7f));
    draw_list->PathLineTo(ImVec2(position.x + 9.4f, position.y + 6.2f));
    draw_list->PathLineTo(ImVec2(position.x + 8.8f, position.y + 6.2f));
    draw_list->PathLineTo(ImVec2(position.x + 7.6f, position.y + 5.7f));
    draw_list->PathLineTo(ImVec2(position.x + 6.7f, position.y + 4.7f));
    draw_list->PathLineTo(ImVec2(position.x + 6.2f, position.y + 3.6f));
    draw_list->PathLineTo(ImVec2(position.x + 6.2f, position.y + 3.3f));
    draw_list->PathLineTo(ImVec2(position.x + 5.9f, position.y + 2.7f));
    draw_list->PathLineTo(ImVec2(position.x + 5.0f, position.y + 2.2f));
    draw_list->PathFillConcave(color);

    draw_list->PathLineTo(ImVec2(position.x + 1.0f, position.y + 9.5f));
    draw_list->PathLineTo(ImVec2(position.x + 5.5f, position.y + 5.0f));
    draw_list->PathLineTo(ImVec2(position.x + 7.4f, position.y + 6.8f));
    draw_list->PathLineTo(ImVec2(position.x + 2.9f, position.y + 11.3f));
    draw_list->PathLineTo(ImVec2(position.x + 1.8f, position.y + 11.7f));
    draw_list->PathLineTo(ImVec2(position.x + 1.0f, position.y + 11.3f));
    draw_list->PathFillConcave(color);
}

void render_reasoning_icon(ImDrawList* draw_list, ImVec2 position, ImU32 color) {
    draw_list->AddLine(ImVec2(position.x + 3.5f, position.y + 2.1f),
                       ImVec2(position.x + 8.5f, position.y + 2.1f), color, 1.4f);
    draw_list->AddLine(ImVec2(position.x + 3.2f, position.y + 4.1f),
                       ImVec2(position.x + 5.4f, position.y + 7.2f), color, 1.4f);
    draw_list->AddRectFilled(ImVec2(position.x, position.y + 0.7f),
                             ImVec2(position.x + 4.0f, position.y + 4.8f), color, 1.0f);
    draw_list->AddRectFilled(ImVec2(position.x + 8.0f, position.y + 0.7f),
                             ImVec2(position.x + 12.0f, position.y + 4.8f), color, 1.0f);
    draw_list->AddRectFilled(ImVec2(position.x + 4.7f, position.y + 6.2f),
                             ImVec2(position.x + 8.7f, position.y + 10.3f), color, 1.0f);
}

std::string status_label(const ToolActivity& tool, ImVec4* color) {
    std::string normalized = tool.status;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (normalized.find("fail") != std::string::npos ||
        normalized.find("error") != std::string::npos) {
        *color = ImVec4(0.86f, 0.53f, 0.55f, 1.0f);
        return "Failed";
    }
    if (tool.completed || normalized == "completed" || normalized == "complete") {
        *color = ImVec4(0.68f, 0.82f, 0.62f, 1.0f);
        return "Completed";
    }
    if (normalized.empty() || normalized == "inprogress" || normalized == "in_progress" ||
        normalized == "running") {
        *color = ImVec4(0.57f, 0.69f, 0.91f, 1.0f);
        return "Running";
    }
    *color = ImVec4(0.70f, 0.70f, 0.70f, 1.0f);
    return tool.status;
}

ImVec2 measure_text(ImFont* font, float font_size, const std::string& text,
                    float max_width = FLT_MAX, float wrap_width = 0.0f) {
    return font->CalcTextSizeA(font_size, max_width, wrap_width, text.c_str(),
                               text.c_str() + text.size());
}

std::string elide_tool_title(const std::string& title, float max_width, ImFont* font,
                             float font_size) {
    if (measure_text(font, font_size, title).x <= max_width)
        return title;

    std::size_t end = title.size();
    while (end > 0) {
        const std::string candidate = title.substr(0, end) + "...";
        if (measure_text(font, font_size, candidate).x <= max_width)
            return candidate;
        --end;
        while (end > 0 && (static_cast<unsigned char>(title[end]) & 0xc0) == 0x80)
            --end;
    }
    return "...";
}

void render_expandable_card(const char* expanded_id_name, const std::string& title,
                           ActivityIcon icon, const std::string& details,
                           ImFont* header_font, ImFont* details_font,
                           const std::string& status, const ImVec4& status_color) {
    constexpr float corner_radius = 5.0f;
    constexpr float header_horizontal_padding = 14.0f;
    constexpr float header_vertical_padding = 4.0f;
    constexpr float icon_size = 13.0f;
    constexpr float icon_gap = 10.0f;
    constexpr float details_horizontal_padding = 14.0f;
    constexpr float details_vertical_padding = 7.0f;
    constexpr float max_body_height = 280.0f;
    const float font_size = ImGui::GetFontSize();
    const float row_height = font_size + header_vertical_padding * 2.0f;
    const ImVec2 card_min = ImGui::GetCursorScreenPos();
    const float available_width = ImGui::GetContentRegionAvail().x;
    const ImGuiID expanded_id = ImGui::GetID(expanded_id_name);
    ImGuiStorage* storage = ImGui::GetStateStorage();
    bool expanded = storage->GetBool(expanded_id, false);
    const bool has_status = !status.empty();
    const ImVec2 status_size = has_status
        ? measure_text(header_font, font_size, status) : ImVec2(0.0f, 0.0f);
    const float natural_title_width = measure_text(header_font, font_size, title).x;
    const ImVec2 natural_details_size = measure_text(details_font, font_size, details);
    const float title_offset = header_horizontal_padding + icon_size + icon_gap;
    const float status_gap = has_status ? 9.0f : 0.0f;
    const float header_width = title_offset + natural_title_width + status_gap + status_size.x +
                               header_horizontal_padding;
    const float details_width = natural_details_size.x + details_horizontal_padding * 2.0f;
    const float card_width = std::min(available_width,
                                     std::max(header_width, expanded ? details_width : 0.0f));
    ImGui::InvisibleButton("##tool-card", ImVec2(card_width, row_height));
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked()) {
        expanded = !expanded;
        storage->SetBool(expanded_id, expanded);
    }

    const float title_x = card_min.x + title_offset;
    const float title_right = card_min.x + card_width - header_horizontal_padding;
    const float status_x = title_right - status_size.x;
    const float title_max_width = std::max(
        0.0f, title_right - title_x - status_size.x - status_gap);
    const std::string clipped_title = elide_tool_title(title, title_max_width, header_font,
                                                       font_size);
    const ImVec2 title_size = measure_text(header_font, font_size, clipped_title);
    const float text_y = card_min.y + header_vertical_padding;
    const ImU32 icon_color = ImGui::GetColorU32(ImVec4(0.47f, 0.47f, 0.47f, 1.0f));
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const float body_width = std::max(
        1.0f, card_width - details_horizontal_padding * 2.0f);
    const ImVec2 details_size = measure_text(details_font, font_size, details, body_width,
                                             body_width);
    const float body_content_height = std::max(ImGui::GetTextLineHeight(), details_size.y);
    const float body_height = std::min(max_body_height,
                                       body_content_height + details_vertical_padding * 2.0f);
    const float card_height = row_height + (expanded ? body_height : 0.0f);
    const ImVec4 background = hovered ? ImVec4(0.145f, 0.145f, 0.145f, 1.0f)
                                      : ImVec4(0.105f, 0.105f, 0.105f, 1.0f);
    draw_list->AddRectFilled(card_min, ImVec2(card_min.x + card_width, card_min.y + card_height),
                             ImGui::GetColorU32(background), corner_radius);
    if (expanded) {
        draw_list->AddRectFilled(
            ImVec2(card_min.x, card_min.y + row_height - 1.0f),
            ImVec2(card_min.x + card_width, card_min.y + card_height),
            ImGui::GetColorU32(ImVec4(0.065f, 0.065f, 0.065f, 1.0f)), corner_radius,
            ImDrawFlags_RoundCornersBottom);
    }

    const ImVec2 icon_position(card_min.x + header_horizontal_padding,
                               card_min.y + header_vertical_padding +
                                   (font_size - icon_size) * 0.5f);
    if (icon == ActivityIcon::Terminal)
        render_terminal_icon(draw_list, ImVec2(icon_position.x, icon_position.y + 1.5f),
                             icon_color);
    else if (icon == ActivityIcon::Reasoning)
        render_reasoning_icon(draw_list, icon_position, icon_color);
    else
        render_tool_icon(draw_list, icon_position, icon_color);

    const ImVec4 text_color(0.82f, 0.82f, 0.82f, 1.0f);
    ImGui::PushFont(header_font, font_size);
    ImGui::SetCursorScreenPos(ImVec2(title_x, text_y));
    ImGui::PushStyleColor(ImGuiCol_Text, text_color);
    ImGui::TextUnformatted(clipped_title.c_str());
    ImGui::PopStyleColor();
    if (has_status) {
        ImGui::SetCursorScreenPos(
            ImVec2(std::max(status_x, title_x + title_size.x + status_gap), text_y));
        ImGui::PushStyleColor(ImGuiCol_Text, status_color);
        ImGui::TextUnformatted(status.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::PopFont();

    if (expanded) {
        const ImVec2 child_padding(details_horizontal_padding - corner_radius,
                                   details_vertical_padding);
        const ImVec2 child_size(std::max(1.0f, card_width - corner_radius * 2.0f),
                                body_height);
        ImGui::SetCursorScreenPos(ImVec2(card_min.x + corner_radius,
                                         card_min.y + row_height));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, child_padding);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, corner_radius);
        if (ImGui::BeginChild("##tool-details", child_size,
                              ImGuiChildFlags_AlwaysUseWindowPadding,
                              ImGuiWindowFlags_NoBackground)) {
            ImGui::PushFont(details_font, font_size);
            ImGui::TextWrapped("%s", details.c_str());
            ImGui::PopFont();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    } else {
        ImGui::SetCursorScreenPos(ImVec2(card_min.x, card_min.y + row_height));
        ImGui::Dummy(ImVec2(card_width, 0.0f));
    }
}

void render_tool_activity(const ToolActivity& tool, ImFont* monospace_font) {
    const bool is_terminal = tool.is_terminal || !tool.command.empty();
    const std::string title = is_terminal
        ? (tool.command.empty() ? "Terminal" : tool.command)
        : (tool.name.empty() ? "Tool" : tool.name);
    std::string details;
    if (is_terminal)
        details = tool.command;
    else if (!tool.arguments.empty())
        details = tool.arguments;
    if (!tool.output.empty()) {
        if (!details.empty())
            details += '\n';
        details += tool.output;
    }
    if (details.empty())
        details = !tool.cwd.empty() ? tool.cwd : "No details available";

    ImFont* default_font = ImGui::GetFont();
    ImFont* header_font = is_terminal && monospace_font != nullptr
        ? monospace_font : default_font;
    ImFont* details_font = monospace_font != nullptr ? monospace_font : default_font;
    ImVec4 status_color;
    const std::string status = status_label(tool, &status_color);
    const ActivityIcon icon = is_terminal ? ActivityIcon::Terminal : ActivityIcon::Tool;
    render_expandable_card("##tool-expanded", title, icon, details, header_font,
                           details_font, status, status_color);
}

void render_reasoning(const std::string& reasoning) {
    ImFont* sans_font = ImGui::GetFont();
    render_expandable_card("##reasoning-expanded", "Reasoning", ActivityIcon::Reasoning,
                           reasoning, sans_font, sans_font, {}, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
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
                       ImFont* monospace_font) {
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
                if (!message.reasoning.empty())
                    render_reasoning(message.reasoning);
                if (!message.segments.empty()) {
                    for (std::size_t segment_index = 0;
                         segment_index < message.segments.size(); ++segment_index) {
                        const ChatSegment& segment = message.segments[segment_index];
                        if (segment.kind == ChatSegment::Kind::Tool) {
                            if (segment.tool.id.empty())
                                ImGui::PushID(static_cast<int>(segment_index));
                            else
                                ImGui::PushID(segment.tool.id.c_str());
                            render_tool_activity(segment.tool, monospace_font);
                            ImGui::PopID();
                        } else if (!segment.text.empty()) {
                            markdown.print(segment.text.c_str(), segment.text.c_str() + segment.text.size());
                        }
                    }
                } else {
                    for (const std::string& activity : message.tool_activities) {
                        ToolActivity tool;
                        tool.command = activity;
                        render_tool_activity(tool, monospace_font);
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
