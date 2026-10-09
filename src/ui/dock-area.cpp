#include "dock-area.h"
#include "imgui.h"
#include "imgui_internal.h"

static void hide_dock_menu_buttons(ImGuiDockNode* node) {
    if (node == nullptr)
        return;
    node->SetLocalFlags(node->LocalFlags | ImGuiDockNodeFlags_NoWindowMenuButton |
                        ImGuiDockNodeFlags_NoCloseButton);
    hide_dock_menu_buttons(node->ChildNodes[0]);
    hide_dock_menu_buttons(node->ChildNodes[1]);
}

void render_dock_area() {
    const ImGuiID dockspace_id = ImHashStr("MainDockSpace");
    if (ImGui::DockBuilderGetNode(dockspace_id) == nullptr) {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);

        ImGuiID threads_id = 0;
        ImGuiID main_id = 0;
        ImGuiID usage_id = 0;
        ImGuiID mcp_id = 0;
        ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.25f, &threads_id, &main_id);
        ImGui::DockBuilderSplitNode(main_id, ImGuiDir_Right, 0.28f, &usage_id, &main_id);
        ImGui::DockBuilderSplitNode(usage_id, ImGuiDir_Down, 0.46f, &usage_id, &mcp_id);
        ImGui::DockBuilderDockWindow("Threads", threads_id);
        ImGui::DockBuilderDockWindow("Chat", main_id);
        ImGui::DockBuilderDockWindow("Usage & Limits", mcp_id);
        ImGui::DockBuilderDockWindow("MCP Servers", usage_id);
        ImGui::DockBuilderFinish(dockspace_id);
    }

    hide_dock_menu_buttons(ImGui::DockBuilderGetNode(dockspace_id));
    ImGui::DockSpaceOverViewport(dockspace_id, ImGui::GetMainViewport());
}
