#include "chat-panel.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_md.h"
#include "misc/cpp/imgui_stdlib.h"
#include <algorithm>
#include <cfloat>
#include <cctype>
#include <initializer_list>
#include <limits>
#include <string_view>
#include <utility>

namespace {
constexpr float chat_component_spacing = 13.0f;
constexpr float chat_line_height_ratio = 1.5f;
bool has_chat_component = false;

void begin_chat_component() {
    if (has_chat_component) {
        ImVec2 position = ImGui::GetCursorScreenPos();
        position.y += chat_component_spacing;
        ImGui::SetCursorScreenPos(position);
    }
    has_chat_component = true;
}

struct TextSpan {
    std::string text;
    ImVec2 position;
    ImVec2 clip_min;
    ImVec2 clip_max;
    ImFont* font;
    float font_size;
    ImDrawList* draw_list;
};

struct TextEndpoint {
    std::size_t span = 0;
    std::size_t byte = 0;
};

struct TranscriptSelection {
    std::vector<TextSpan> spans;
    TextEndpoint anchor;
    TextEndpoint focus;
    bool tracking = false;
    bool dragged = false;
};

TranscriptSelection transcript_selection;

void register_text(const char* begin, const char* end, ImVec2 position,
                   ImFont* font = nullptr, float font_size = 0.0f) {
    if (begin == end)
        return;
    if (font == nullptr)
        font = ImGui::GetFont();
    if (font_size == 0.0f)
        font_size = ImGui::GetFontSize();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImVec2 clip_min = draw_list->GetClipRectMin();
    const ImVec2 clip_max = draw_list->GetClipRectMax();
    const char* line = begin;
    float y = position.y;
    while (line < end) {
        const char* finish = std::find(line, end, '\n');
        if (finish > line)
            transcript_selection.spans.push_back({std::string(line, finish),
                ImVec2(position.x, y), clip_min, clip_max, font, font_size, draw_list});
        if (finish == end)
            break;
        line = finish + 1;
        y += font_size;
    }
}

float span_width(const TextSpan& span, std::size_t bytes) {
    return span.font->CalcTextSizeA(span.font_size, FLT_MAX, 0.0f,
                                    span.text.c_str(), span.text.c_str() + bytes).x;
}

bool is_over_selectable_text(ImVec2 point) {
    for (const TextSpan& span : transcript_selection.spans) {
        const float right = span.position.x + span_width(span, span.text.size());
        if (point.x >= span.position.x && point.x < right &&
            point.y >= span.position.y && point.y < span.position.y + span.font_size &&
            point.x >= span.clip_min.x && point.x < span.clip_max.x &&
            point.y >= span.clip_min.y && point.y < span.clip_max.y)
            return true;
    }
    return false;
}

TextEndpoint text_endpoint_at(ImVec2 point) {
    const auto& spans = transcript_selection.spans;
    TextEndpoint nearest;
    float nearest_distance = std::numeric_limits<float>::max();
    for (std::size_t index = 0; index < spans.size(); ++index) {
        const TextSpan& span = spans[index];
        const float width = span_width(span, span.text.size());
        const float left = std::max(span.position.x, span.clip_min.x);
        const float right = std::min(span.position.x + width, span.clip_max.x);
        const float top = std::max(span.position.y, span.clip_min.y);
        const float bottom = std::min(span.position.y + span.font_size, span.clip_max.y);
        if (left >= right || top >= bottom)
            continue;
        const float dx = point.x < left ? left - point.x :
                         point.x > right ? point.x - right : 0.0f;
        const float dy = point.y < top ? top - point.y :
                         point.y > bottom ? point.y - bottom : 0.0f;
        const float distance = dy * 1000.0f + dx;
        if (distance >= nearest_distance)
            continue;
        nearest_distance = distance;
        nearest.span = index;
        nearest.byte = 0;
        const float local_x = point.x - span.position.x;
        std::size_t offset = 0;
        while (offset < span.text.size()) {
            std::size_t next = offset + 1;
            while (next < span.text.size() &&
                   (static_cast<unsigned char>(span.text[next]) & 0xc0) == 0x80)
                ++next;
            if (local_x < (span_width(span, offset) + span_width(span, next)) * 0.5f)
                break;
            offset = next;
        }
        nearest.byte = offset;
    }
    return nearest;
}

bool endpoint_before(TextEndpoint a, TextEndpoint b) {
    return a.span < b.span || (a.span == b.span && a.byte < b.byte);
}

bool has_text_selection() {
    return transcript_selection.anchor.span != transcript_selection.focus.span ||
           transcript_selection.anchor.byte != transcript_selection.focus.byte;
}

std::string selected_transcript_text() {
    if (!has_text_selection() || transcript_selection.spans.empty())
        return {};
    TextEndpoint first = transcript_selection.anchor;
    TextEndpoint last = transcript_selection.focus;
    if (endpoint_before(last, first))
        std::swap(first, last);
    if (last.span >= transcript_selection.spans.size())
        return {};
    std::string result;
    for (std::size_t index = first.span; index <= last.span; ++index) {
        const TextSpan& span = transcript_selection.spans[index];
        const std::size_t begin = index == first.span ? first.byte : 0;
        const std::size_t end = index == last.span ? last.byte : span.text.size();
        if (!result.empty() && index > first.span) {
            const TextSpan& previous = transcript_selection.spans[index - 1];
            if (span.position.y > previous.position.y + previous.font_size * 0.5f ||
                span.position.x < previous.position.x)
                result += '\n';
        }
        result.append(span.text, begin, end - begin);
    }
    return result;
}

void draw_text_selection() {
    if (!has_text_selection() || transcript_selection.spans.empty())
        return;
    TextEndpoint first = transcript_selection.anchor;
    TextEndpoint last = transcript_selection.focus;
    if (endpoint_before(last, first))
        std::swap(first, last);
    if (last.span >= transcript_selection.spans.size())
        return;
    const ImU32 color = ImGui::GetColorU32(ImVec4(0.29f, 0.46f, 0.76f, 0.45f));
    for (std::size_t index = first.span; index <= last.span; ++index) {
        const TextSpan& span = transcript_selection.spans[index];
        const std::size_t begin = index == first.span ? first.byte : 0;
        const std::size_t end = index == last.span ? last.byte : span.text.size();
        if (begin == end)
            continue;
        span.draw_list->PushClipRect(span.clip_min, span.clip_max, true);
        span.draw_list->AddRectFilled(
            ImVec2(span.position.x + span_width(span, begin), span.position.y),
            ImVec2(span.position.x + span_width(span, end), span.position.y + span.font_size),
            color);
        span.draw_list->PopClipRect();
    }
}

void render_code_card(const std::string& code, const std::string& language,
                      ImFont* monospace_font);

std::string display_language(const std::string& language) {
    std::string normalized = language;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    static const std::pair<const char*, const char*> labels[] = {
        {"bash", "Bash"}, {"c", "C"}, {"cpp", "C++"}, {"c++", "C++"},
        {"csharp", "C#"}, {"cs", "C#"}, {"css", "CSS"}, {"dart", "Dart"},
        {"diff", "Diff"}, {"dockerfile", "Dockerfile"}, {"go", "Go"},
        {"golang", "Go"}, {"html", "HTML"}, {"java", "Java"},
        {"javascript", "JavaScript"}, {"js", "JavaScript"}, {"json", "JSON"},
        {"jsx", "JSX"}, {"kotlin", "Kotlin"}, {"kt", "Kotlin"},
        {"lua", "Lua"}, {"md", "Markdown"}, {"markdown", "Markdown"},
        {"objective-c", "Objective-C"}, {"objc", "Objective-C"},
        {"perl", "Perl"}, {"php", "PHP"}, {"plaintext", "Plain text"},
        {"powershell", "PowerShell"}, {"py", "Python"}, {"python", "Python"},
        {"r", "R"}, {"rb", "Ruby"}, {"ruby", "Ruby"}, {"rs", "Rust"},
        {"rust", "Rust"}, {"sass", "Sass"}, {"scss", "SCSS"},
        {"shell", "Shell"}, {"sh", "Shell"}, {"sql", "SQL"}, {"swift", "Swift"},
        {"ts", "TypeScript"}, {"typescript", "TypeScript"}, {"tsx", "TSX"},
        {"toml", "TOML"}, {"txt", "Plain text"}, {"xml", "XML"},
        {"yaml", "YAML"}, {"yml", "YAML"}, {"zig", "Zig"},
    };
    for (const auto& label : labels) {
        if (normalized == label.first)
            return label.second;
    }
    if (!language.empty()) {
        normalized[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(normalized[0])));
        return normalized;
    }
    return {};
}

class ChatMarkdown : public imgui_md {
public:
    ImFont* monospace_font = nullptr;

    ImVec4 get_color() const override {
        return m_href.empty() ? ImGui::GetStyle().Colors[ImGuiCol_Text]
                              : ImVec4(0.82f, 0.84f, 0.90f, 1.0f);
    }

protected:
    void BLOCK_UL(const MD_BLOCK_UL_DETAIL* detail, bool entering) override {
        imgui_md::BLOCK_UL(detail, entering);
        list_depth += entering ? 1 : -1;
    }

    void BLOCK_OL(const MD_BLOCK_OL_DETAIL* detail, bool entering) override {
        imgui_md::BLOCK_OL(detail, entering);
        list_depth += entering ? 1 : -1;
    }

    void BLOCK_P(bool entering) override {
        if (list_depth > 0)
            return;
        if (entering) {
            begin_chat_component();
        } else {
            ImGui::NewLine();
            ImVec2 position = ImGui::GetCursorScreenPos();
            position.y -= ImGui::GetStyle().ItemSpacing.y;
            ImGui::SetCursorScreenPos(position);
        }
    }

    void BLOCK_CODE(const MD_BLOCK_CODE_DETAIL* detail, bool entering) override {
        imgui_md::BLOCK_CODE(detail, entering);
        if (entering) {
            code.clear();
            language = detail->lang.size > 0
                ? std::string(detail->lang.text, detail->lang.size) : std::string();
        } else {
            if (!code.empty() && code.back() == '\n')
                code.pop_back();
            begin_chat_component();
            render_code_card(code, language, monospace_font);
        }
    }

    void CODE_TEXT(const char* begin, const char* end) override {
        code.append(begin, end);
    }

    void TEXT_RENDERED(const char* begin, const char* end) override {
        if (m_href.empty())
            register_text(begin, end, ImGui::GetItemRectMin());
    }

private:
    std::string code;
    std::string language;
    int list_depth = 0;
};

void render_markdown_text(ChatMarkdown& markdown, const std::string& text) {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(0.0f, ImGui::GetFontSize() * (chat_line_height_ratio - 1.0f)));
    markdown.print(text.c_str(), text.c_str() + text.size());
    ImGui::PopStyleVar();
}

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

std::size_t wrapped_line_count(const std::string& text, float width,
                               ImFont* font, float font_size) {
    if (text.empty())
        return 1;
    const char* cursor = text.c_str();
    const char* end = cursor + text.size();
    std::size_t lines = 0;
    while (cursor < end) {
        const char* hard_end = std::find(cursor, end, '\n');
        const char* wrap_end = font->CalcWordWrapPosition(font_size, cursor, hard_end, width);
        if (wrap_end == cursor)
            wrap_end = cursor + 1;
        cursor = wrap_end;
        if (cursor < hard_end) {
            while (cursor < hard_end && *cursor == ' ')
                ++cursor;
        } else if (cursor == hard_end && cursor < end) {
            ++cursor;
        }
        ++lines;
    }
    return lines;
}

float wrapped_text_height(const std::string& text, float width,
                          ImFont* font, float font_size) {
    return font_size + (wrapped_line_count(text, width, font, font_size) - 1) *
        font_size * chat_line_height_ratio;
}

void render_wrapped_selectable_text(const std::string& text, float width) {
    ImFont* font = ImGui::GetFont();
    const float font_size = ImGui::GetFontSize();
    const float line_height = font_size * chat_line_height_ratio;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
    const char* cursor = text.c_str();
    const char* end = cursor + text.size();
    std::size_t line = 0;
    while (cursor < end) {
        const char* hard_end = std::find(cursor, end, '\n');
        const char* wrap_end = font->CalcWordWrapPosition(font_size, cursor, hard_end, width);
        if (wrap_end == cursor)
            wrap_end = cursor + 1;
        const ImVec2 position(start.x, start.y + line * line_height);
        draw_list->AddText(font, font_size, position, color, cursor, wrap_end);
        register_text(cursor, wrap_end, position, font, font_size);
        cursor = wrap_end;
        if (cursor < hard_end) {
            while (cursor < hard_end && *cursor == ' ')
                ++cursor;
        } else if (cursor == hard_end && cursor < end) {
            ++cursor;
        }
        ++line;
    }
    ImGui::Dummy(ImVec2(width, font_size +
                        (line > 0 ? line - 1 : 0) * line_height));
}

void render_code_card(const std::string& code, const std::string& language,
                      ImFont* monospace_font) {
    constexpr float padding = 14.0f;
    constexpr float header_padding = 4.0f;
    const float font_size = ImGui::GetFontSize();
    const float header_height = font_size + header_padding * 2.0f;
    const float line_height = ImGui::GetTextLineHeight();
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const std::size_t line_count = 1 + std::count(code.begin(), code.end(), '\n');
    const float body_height = std::min(400.0f, line_count * line_height + padding * 2.0f);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(start, ImVec2(start.x + width, start.y + header_height + body_height),
                             ImGui::GetColorU32(ImVec4(0.105f, 0.105f, 0.105f, 1.0f)), 5.0f);
    draw_list->AddRectFilled(ImVec2(start.x, start.y + header_height - 1.0f),
                             ImVec2(start.x + width, start.y + header_height + body_height),
                             ImGui::GetColorU32(ImVec4(0.065f, 0.065f, 0.065f, 1.0f)), 5.0f,
                             ImDrawFlags_RoundCornersBottom);
    const ImU32 icon_color = ImGui::GetColorU32(ImVec4(0.47f, 0.47f, 0.47f, 1.0f));
    const ImVec2 icon(start.x + padding, start.y + header_padding + 2.0f);
    constexpr float icon_scale = 0.020f;
    const auto fill_polygon = [&](std::initializer_list<ImVec2> points) {
        ImVec2 scaled[6];
        int count = 0;
        for (const ImVec2& point : points)
            scaled[count++] = ImVec2(icon.x + point.x * icon_scale,
                                     icon.y + point.y * icon_scale);
        draw_list->AddConvexPolyFilled(scaled, count, icon_color);
    };
    fill_polygon({{137.4f, 201.3f}, {182.6f, 246.6f}, {109.3f, 320.0f},
                  {41.4f, 297.3f}});
    fill_polygon({{28.9f, 309.8f}, {41.4f, 297.3f}, {109.3f, 320.0f},
                  {41.4f, 342.6f}, {28.9f, 330.1f}});
    fill_polygon({{41.4f, 342.6f}, {109.3f, 320.0f}, {182.7f, 393.3f},
                  {137.4f, 438.6f}});
    fill_polygon({{353.2f, 87.2f}, {414.8f, 104.8f}, {286.8f, 552.8f},
                  {225.2f, 535.2f}});
    fill_polygon({{502.7f, 201.4f}, {598.7f, 297.4f}, {530.8f, 320.0f},
                  {457.4f, 246.6f}});
    fill_polygon({{598.7f, 297.4f}, {611.2f, 309.9f}, {611.2f, 330.1f},
                  {598.7f, 342.7f}, {530.8f, 320.0f}});
    fill_polygon({{530.8f, 320.0f}, {598.7f, 342.7f}, {502.7f, 438.7f},
                  {457.4f, 393.4f}});
    draw_list->AddText(ImVec2(start.x + padding + 23.0f, start.y + header_padding),
                       ImGui::GetColorU32(ImVec4(0.82f, 0.82f, 0.82f, 1.0f)), "Code");
    register_text("Code", "Code" + 4,
                  ImVec2(start.x + padding + 23.0f, start.y + header_padding));
    const std::string language_label = display_language(language);
    if (!language_label.empty()) {
        const float label_width = ImGui::CalcTextSize("Code").x;
        draw_list->AddText(ImVec2(start.x + padding + 32.0f + label_width,
                                  start.y + header_padding),
                           ImGui::GetColorU32(ImVec4(0.57f, 0.69f, 0.91f, 1.0f)),
                           language_label.c_str());
        register_text(language_label.c_str(), language_label.c_str() + language_label.size(),
                      ImVec2(start.x + padding + 32.0f + label_width,
                             start.y + header_padding));
    }

    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + header_height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding, padding));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::BeginChild("##code-body", ImVec2(width, body_height),
                      ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
    ImDrawList* code_draw_list = ImGui::GetWindowDrawList();
    ImFont* font = monospace_font != nullptr ? monospace_font : ImGui::GetFont();
    ImGui::PushFont(font, font_size);
    const ImU32 ordinary = ImGui::GetColorU32(ImVec4(0.82f, 0.82f, 0.82f, 1.0f));
    const ImU32 keyword = ImGui::GetColorU32(ImVec4(0.66f, 0.56f, 0.91f, 1.0f));
    const ImU32 literal = ImGui::GetColorU32(ImVec4(0.81f, 0.68f, 0.47f, 1.0f));
    const ImU32 comment = ImGui::GetColorU32(ImVec4(0.52f, 0.61f, 0.52f, 1.0f));
    constexpr std::string_view keywords =
        " auto bool break case char class const continue default do double else enum false "
        " float for if import include int let long namespace nullptr private public return "
        " short signed sizeof static struct switch template this true typedef unsigned using "
        " void while fn function def async await var new try catch throw interface type ";
    float widest_line = 0.0f;
    bool in_block_comment = false;
    std::size_t line_start = 0;
    std::size_t line_number = 0;
    const ImVec2 text_start = ImGui::GetCursorScreenPos();
    while (line_start <= code.size()) {
        const std::size_t line_end = code.find('\n', line_start);
        const std::size_t end = line_end == std::string::npos ? code.size() : line_end;
        float x = text_start.x;
        const float y = text_start.y + line_number * line_height;
        std::size_t pos = line_start;
        while (pos < end) {
            const std::size_t token_start = pos;
            ImU32 color = ordinary;
            if (in_block_comment) {
                const std::size_t close = code.find("*/", pos);
                pos = close == std::string::npos || close >= end ? end : close + 2;
                in_block_comment = pos == end && (close == std::string::npos || close >= end);
                color = comment;
            } else if (code.compare(pos, 2, "//") == 0 || code[pos] == '#') {
                pos = end;
                color = comment;
            } else if (code.compare(pos, 2, "/*") == 0) {
                const std::size_t close = code.find("*/", pos + 2);
                pos = close == std::string::npos || close >= end ? end : close + 2;
                in_block_comment = pos == end && (close == std::string::npos || close >= end);
                color = comment;
            } else if (code[pos] == '"' || code[pos] == '\'') {
                const char quote = code[pos++];
                while (pos < end) {
                    if (code[pos++] == '\\' && pos < end)
                        ++pos;
                    else if (code[pos - 1] == quote)
                        break;
                }
                color = literal;
            } else if (std::isdigit(static_cast<unsigned char>(code[pos]))) {
                while (pos < end && (std::isalnum(static_cast<unsigned char>(code[pos])) ||
                                     code[pos] == '.'))
                    ++pos;
                color = literal;
            } else if (std::isalpha(static_cast<unsigned char>(code[pos])) || code[pos] == '_') {
                while (pos < end && (std::isalnum(static_cast<unsigned char>(code[pos])) ||
                                     code[pos] == '_'))
                    ++pos;
                const std::string word = " " + code.substr(token_start, pos - token_start) + " ";
                if (keywords.find(word) != std::string_view::npos)
                    color = keyword;
            } else {
                ++pos;
            }
            const char* begin = code.c_str() + token_start;
            const char* finish = code.c_str() + pos;
            code_draw_list->AddText(font, font_size, ImVec2(x, y), color, begin, finish);
            register_text(begin, finish, ImVec2(x, y), font, font_size);
            x += font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, begin, finish).x;
        }
        widest_line = std::max(widest_line, x - text_start.x);
        if (line_end == std::string::npos)
            break;
        line_start = line_end + 1;
        ++line_number;
    }
    ImGui::Dummy(ImVec2(widest_line, line_count * line_height));
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
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
    const bool clicked = ImGui::InvisibleButton("##tool-card", ImVec2(card_width, row_height));
    const bool hovered = ImGui::IsItemHovered();
    if (clicked &&
        ImGui::GetIO().MouseDragMaxDistanceSqr[0] <= 9.0f) {
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
    const float body_content_height = wrapped_text_height(details, body_width,
                                                           details_font, font_size);
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
            render_wrapped_selectable_text(details, body_width);
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
    begin_chat_component();
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
    begin_chat_component();
    ImFont* sans_font = ImGui::GetFont();
    render_expandable_card("##reasoning-expanded", "Reasoning", ActivityIcon::Reasoning,
                           reasoning, sans_font, sans_font, {}, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
}

void render_user_message(const ChatMessage& message) {
    begin_chat_component();
    constexpr float horizontal_padding = 12.0f;
    constexpr float vertical_padding = 8.0f;
    constexpr float max_width_ratio = 0.8f;
    const float available_width = ImGui::GetContentRegionAvail().x;
    const float max_text_width = std::max(1.0f, available_width * max_width_ratio -
                                                   horizontal_padding * 2.0f);
    const ImVec2 measured = ImGui::CalcTextSize(message.content.c_str(), nullptr, false,
                                                max_text_width);
    const float text_width = std::min(max_text_width, std::max(1.0f, measured.x));
    const float text_height = wrapped_text_height(message.content, max_text_width,
                                                   ImGui::GetFont(), ImGui::GetFontSize());
    const ImVec2 row_pos = ImGui::GetCursorScreenPos();
    const float bubble_width = text_width + horizontal_padding * 2.0f;
    const float bubble_height = text_height + vertical_padding * 2.0f;
    const ImVec2 bubble_min(row_pos.x + available_width - bubble_width, row_pos.y);
    const ImVec2 bubble_max(bubble_min.x + bubble_width, bubble_min.y + bubble_height);

    ImGui::GetWindowDrawList()->AddRectFilled(
        bubble_min, bubble_max, ImGui::GetColorU32(ImVec4(0.20f, 0.20f, 0.20f, 1.0f)), 6.0f);
    ImGui::SetCursorScreenPos(ImVec2(bubble_min.x + horizontal_padding,
                                     bubble_min.y + vertical_padding));
    render_wrapped_selectable_text(message.content, max_text_width);
    ImGui::SetCursorScreenPos(row_pos);
    ImGui::Dummy(ImVec2(available_width, bubble_height));
}

}

void render_chat_panel(ApplicationState& state, std::string& message_input,
                       std::vector<ProviderPtr>& providers, std::size_t& selected_provider,
                       std::string& selected_model, std::string& selected_reasoning_effort,
                       bool& is_generating, TurnId& active_turn_id, TurnId& next_turn_id,
                       ImFont* monospace_font) {
    Provider* provider = providers[selected_provider].get();
    if (!is_generating && provider->availability != ProviderAvailability::Available) {
        for (std::size_t index = 0; index < providers.size(); ++index) {
            if (providers[index]->availability == ProviderAvailability::Available) {
                selected_provider = index;
                provider = providers[index].get();
                selected_model = provider->default_model;
                selected_reasoning_effort.clear();
                break;
            }
        }
    }
    static ChatMarkdown markdown;
    markdown.monospace_font = monospace_font;
    if (!provider->models.empty()) {
        const auto selected = std::find_if(provider->models.begin(), provider->models.end(),
            [&](const ModelOption& model) { return model.id == selected_model; });
        if (selected == provider->models.end()) {
            const auto preferred = std::find_if(provider->models.begin(), provider->models.end(),
                [&](const ModelOption& model) { return model.id == provider->default_model; });
            selected_model = (preferred == provider->models.end() ? provider->models.front() : *preferred).id;
            selected_reasoning_effort.clear();
        }
        const auto active = std::find_if(provider->models.begin(), provider->models.end(),
            [&](const ModelOption& model) { return model.id == selected_model; });
        if (active != provider->models.end()) {
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
        constexpr float max_chat_width = 960.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        const float available_width = ImGui::GetContentRegionAvail().x;
        const float chat_width = std::min(available_width, max_chat_width);
        const float chat_offset = (available_width - chat_width) * 0.5f;
        const float chat_x = ImGui::GetCursorPosX() + chat_offset;
        const float available_height = ImGui::GetContentRegionAvail().y;
        const float message_height = std::max(
            0.0f, available_height - composer_height - ImGui::GetStyle().ItemSpacing.y);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::SetCursorPosX(chat_x);
        ImGui::BeginChild("##messages", ImVec2(chat_width, message_height), false);
        const ImVec2 messages_min = ImGui::GetWindowPos();
        const ImVec2 messages_size = ImGui::GetWindowSize();
        const ImVec2 messages_max(messages_min.x + messages_size.x,
                                  messages_min.y + messages_size.y);
        transcript_selection.spans.clear();
        has_chat_component = false;
        const bool was_at_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
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
                            render_markdown_text(markdown, segment.text);
                        }
                    }
                } else {
                    for (const std::string& activity : message.tool_activities) {
                        ToolActivity tool;
                        tool.command = activity;
                        render_tool_activity(tool, monospace_font);
                    }
                    render_markdown_text(markdown, message.content);
                }
            }
            ImGui::EndGroup();
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
        ImGuiIO& io = ImGui::GetIO();
        const bool mouse_over_messages = io.MousePos.x >= messages_min.x &&
            io.MousePos.x < messages_max.x && io.MousePos.y >= messages_min.y &&
            io.MousePos.y < messages_max.y;
        const bool mouse_over_text = mouse_over_messages && is_over_selectable_text(io.MousePos);
        if (mouse_over_text)
            ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
        if (io.MouseClicked[0] && mouse_over_text) {
            transcript_selection.anchor = text_endpoint_at(io.MousePos);
            transcript_selection.focus = transcript_selection.anchor;
            transcript_selection.tracking = true;
            transcript_selection.dragged = false;
        }
        if (transcript_selection.tracking && io.MouseDown[0]) {
            transcript_selection.focus = text_endpoint_at(io.MousePos);
            transcript_selection.dragged = io.MouseDragMaxDistanceSqr[0] > 9.0f;
        }
        if (transcript_selection.tracking && io.MouseReleased[0]) {
            transcript_selection.focus = text_endpoint_at(io.MousePos);
            transcript_selection.tracking = false;
        }
        draw_text_selection();
        if (io.MouseClicked[1] && mouse_over_messages)
            ImGui::OpenPopup("##transcript-copy");
        if (ImGui::BeginPopup("##transcript-copy")) {
            if (ImGui::MenuItem("Copy", "Ctrl+C", false, has_text_selection()))
                ImGui::SetClipboardText(selected_transcript_text().c_str());
            ImGui::EndPopup();
        }
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C) && !io.WantTextInput &&
            has_text_selection())
            ImGui::SetClipboardText(selected_transcript_text().c_str());
        if (was_at_bottom)
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        ImGui::SetCursorPosX(chat_x);
        const ImVec2 input_pos = ImGui::GetCursorScreenPos();
        const float full_width = chat_width;
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
        static std::string composer_context_selection;
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(1)) {
            composer_context_selection.clear();
            ImGuiInputTextState* input_state = ImGui::GetInputTextState(ImGui::GetItemID());
            if (input_state != nullptr && input_state->HasSelection()) {
                const int first = input_state->GetSelectionStart();
                const int last = input_state->GetSelectionEnd();
                composer_context_selection = message_input.substr(first, last - first);
            }
        }
        if (ImGui::BeginPopupContextItem("##composer-copy")) {
            if (ImGui::MenuItem("Copy", "Ctrl+C", false,
                                !composer_context_selection.empty()))
                ImGui::SetClipboardText(composer_context_selection.c_str());
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
        const ImVec2 send_pos(frame_max.x - send_size - outer_padding,
                              divider_y + (footer_height - send_size) * 0.5f);
        const float selector_y = divider_y + (footer_height - ImGui::GetFrameHeight()) * 0.5f;
        const float selector_x = input_pos.x + outer_padding;
        const float selector_available = std::max(0.0f, send_pos.x - selector_x - 12.0f);
        const float model_width = std::min(190.0f, selector_available * 0.62f);
        const float reasoning_width = std::min(140.0f, std::max(0.0f, selector_available - model_width - 8.0f));
        const auto active_model = std::find_if(provider->models.begin(), provider->models.end(),
            [&](const ModelOption& model) { return model.id == selected_model; });
        if (active_model != provider->models.end() && model_width >= 60.0f) {
            ImGui::SetCursorScreenPos(ImVec2(selector_x, selector_y));
            ImGui::SetNextItemWidth(model_width);
            ImGui::BeginDisabled(is_generating);
            if (ImGui::BeginCombo("##model-selector", active_model->name.c_str())) {
                for (std::size_t index = 0; index < providers.size(); ++index) {
                    Provider& candidate = *providers[index];
                    ImGui::PushID(static_cast<int>(index));
                    ImGui::SeparatorText(candidate.name.data());
                    if (candidate.availability == ProviderAvailability::Available) {
                        for (const ModelOption& model : candidate.models) {
                            ImGui::PushID(model.id.c_str());
                            const bool is_selected = index == selected_provider &&
                                                     model.id == selected_model;
                            if (ImGui::Selectable(model.name.c_str(), is_selected)) {
                                selected_provider = index;
                                provider = &candidate;
                                selected_model = model.id;
                                selected_reasoning_effort = model.default_reasoning_effort;
                            }
                            if (is_selected)
                                ImGui::SetItemDefaultFocus();
                            ImGui::PopID();
                        }
                    } else {
                        ImGui::TextDisabled(candidate.availability == ProviderAvailability::Unknown
                                                ? "Discovering models..." : "Unavailable");
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            const auto reasoning_model = std::find_if(provider->models.begin(), provider->models.end(),
                [&](const ModelOption& model) { return model.id == selected_model; });
            if (reasoning_width >= 60.0f) {
                const std::string effort_label = selected_reasoning_effort.empty()
                    ? "Default" : selected_reasoning_effort;
                ImGui::SetCursorScreenPos(ImVec2(selector_x + model_width + 8.0f, selector_y));
                ImGui::SetNextItemWidth(reasoning_width);
                if (ImGui::BeginCombo("##reasoning-selector", effort_label.c_str())) {
                    if (reasoning_model == provider->models.end() || reasoning_model->reasoning_efforts.empty()) {
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
            provider->cancel(provider, active_turn_id);
            is_generating = false;
            active_turn_id = 0;
        } else if (!is_generating && provider->availability == ProviderAvailability::Available &&
                   (enter || send_clicked) && !message_input.empty()) {
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
            const Result submitted = provider->submit(provider, std::move(request));
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
