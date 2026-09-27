#include "card.h"
#include "imgui.h"
#include "ui-scale.h"

bool begin_ui_card(const char* id) {
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, style.Colors[ImGuiCol_FrameBg]);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, ui_size(8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui_size(16.0f), ui_size(16.0f)));
    return ImGui::BeginChild(id, ImVec2(0.0f, 0.0f),
                             ImGuiChildFlags_AutoResizeY |
                                 ImGuiChildFlags_AlwaysUseWindowPadding,
                             ImGuiWindowFlags_NoScrollbar |
                                 ImGuiWindowFlags_NoScrollWithMouse);
}

void end_ui_card() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}
