#ifndef UI_BUTTON_H
#define UI_BUTTON_H

#include "imgui.h"
#include "ui-scale.h"

inline constexpr float ui_button_rounding = 6.0f;

inline bool ui_button(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f)) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ui_size(ui_button_rounding));
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleVar();
    return clicked;
}

inline bool ui_small_button(const char* label) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ui_size(ui_button_rounding));
    const bool clicked = ImGui::SmallButton(label);
    ImGui::PopStyleVar();
    return clicked;
}

#endif
