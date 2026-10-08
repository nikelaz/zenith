#include <gtest/gtest.h>
#include <cstdlib>
#include <iterator>
#include "../src/ui/chat-panel.cpp"

TEST(ChatRendering, MultipleCodeBlocksHaveSeparateLayoutAndScrollState) {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    ChatMarkdown markdown;
    const std::string text = "First snippet\n\n```css\n.a { color: red; }\n```\n\n"
        "```html\n<div class=\"a\">\n  Hello\n</div>\n```";
    for (int frame = 0; frame < 3; ++frame) {
        ImGui::NewFrame();
        ImGui::SetNextWindowSize(ImVec2(500.0f, 150.0f));
        ImGui::Begin("Chat");
        has_chat_component = false;
        markdown.code_card_index = 0;
        ImGui::BeginGroup();
        render_markdown_text(markdown, text);
        ImGuiWindow* parent = ImGui::GetCurrentWindow();
        ASSERT_EQ(parent->DC.ChildWindows.Size, 2);
        ImGuiWindow* first = parent->DC.ChildWindows[0];
        ImGuiWindow* second = parent->DC.ChildWindows[1];
        EXPECT_NE(first->ID, second->ID);
        EXPECT_EQ(first->BeginCount, 1);
        EXPECT_EQ(second->BeginCount, 1);
        EXPECT_GE(second->Pos.y, first->Pos.y + first->Size.y);
        EXPECT_FALSE(parent->DC.IsSetPos);
        ImGui::EndGroup();
        ImGui::End();
        ImGui::Render();
    }
    ImGui::DestroyContext();
}

TEST(ChatRendering, SavedTranscript) {
    const char* path = std::getenv("ZENITH_RENDER_TEST_TRANSCRIPT");
    if (path == nullptr)
        GTEST_SKIP() << "Set ZENITH_RENDER_TEST_TRANSCRIPT to a Markdown transcript";
    std::ifstream input(path);
    ASSERT_TRUE(input.is_open());
    const std::string text((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    ChatMarkdown markdown;
    for (int frame = 0; frame < 4; ++frame) {
        ImGui::NewFrame();
        ImGui::SetNextWindowSize(ImVec2(800.0f, 600.0f));
        ImGui::Begin("Chat");
        has_chat_component = false;
        markdown.code_card_index = 0;
        ImGui::BeginGroup();
        render_markdown_text(markdown, text);
        EXPECT_GE(markdown.code_card_index, 2);
        ImGui::EndGroup();
        ImGui::SetScrollY(frame % 2 == 0 ? ImGui::GetScrollMaxY() : 0.0f);
        ImGui::End();
        ImGui::Render();
    }
    ImGui::DestroyContext();
}
