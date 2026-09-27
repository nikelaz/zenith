#ifndef UI_SCALE_H
#define UI_SCALE_H

#include "imgui.h"

inline float ui_size(float size) {
    return size * ImGui::GetStyle().FontScaleMain;
}

#endif
