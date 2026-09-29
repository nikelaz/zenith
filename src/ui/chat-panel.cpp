#include "chat-panel.h"
#include "../platform/clipboard-image.h"
#include "imgui.h"
#include "ui-scale.h"
#include "imgui_internal.h"
#include "imgui_md.h"
#include "misc/cpp/imgui_stdlib.h"
#include <algorithm>
#include <cfloat>
#include <cctype>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <system_error>
#include <string_view>
#include <utility>

namespace {
constexpr float chat_component_spacing = 13.0f;
constexpr float chat_line_height_ratio = 1.5f;
bool has_chat_component = false;

std::string attachment_media_type(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension == ".txt" || extension == ".log") return "text/plain";
    if (extension == ".md") return "text/markdown";
    if (extension == ".csv") return "text/csv";
    if (extension == ".json") return "application/json";
    if (extension == ".png") return "image/png";
    if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
    if (extension == ".gif") return "image/gif";
    if (extension == ".webp") return "image/webp";
    if (extension == ".heic") return "image/heic";
    if (extension == ".heif") return "image/heif";
    if (extension == ".pdf") return "application/pdf";
    if (extension == ".rtf") return "application/rtf";
    if (extension == ".doc") return "application/msword";
    if (extension == ".docx") return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
    if (extension == ".docm") return "application/vnd.ms-word.document.macroEnabled.12";
    if (extension == ".dot") return "application/msword";
    if (extension == ".dotx") return "application/vnd.openxmlformats-officedocument.wordprocessingml.template";
    if (extension == ".xls") return "application/vnd.ms-excel";
    if (extension == ".xlsx") return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
    if (extension == ".xlsm") return "application/vnd.ms-excel.sheet.macroEnabled.12";
    if (extension == ".xlsb") return "application/vnd.ms-excel.sheet.binary.macroEnabled.12";
    if (extension == ".xlt") return "application/vnd.ms-excel";
    if (extension == ".xltx") return "application/vnd.openxmlformats-officedocument.spreadsheetml.template";
    if (extension == ".ppt") return "application/vnd.ms-powerpoint";
    if (extension == ".pptx") return "application/vnd.openxmlformats-officedocument.presentationml.presentation";
    if (extension == ".pptm") return "application/vnd.ms-powerpoint.presentation.macroEnabled.12";
    if (extension == ".pps") return "application/vnd.ms-powerpoint";
    if (extension == ".ppsx") return "application/vnd.openxmlformats-officedocument.presentationml.slideshow";
    if (extension == ".potx") return "application/vnd.openxmlformats-officedocument.presentationml.template";
    if (extension == ".odt") return "application/vnd.oasis.opendocument.text";
    if (extension == ".ods") return "application/vnd.oasis.opendocument.spreadsheet";
    if (extension == ".odp") return "application/vnd.oasis.opendocument.presentation";
    if (extension == ".odg") return "application/vnd.oasis.opendocument.graphics";
    if (extension == ".odf") return "application/vnd.oasis.opendocument.formula";
    if (extension == ".fodt") return "application/vnd.oasis.opendocument.text-flat-xml";
    if (extension == ".fods") return "application/vnd.oasis.opendocument.spreadsheet-flat-xml";
    if (extension == ".fodp") return "application/vnd.oasis.opendocument.presentation-flat-xml";
    if (extension == ".ott") return "application/vnd.oasis.opendocument.text-template";
    if (extension == ".ots") return "application/vnd.oasis.opendocument.spreadsheet-template";
    if (extension == ".otp") return "application/vnd.oasis.opendocument.presentation-template";
    if (extension == ".sxw") return "application/vnd.sun.xml.writer";
    if (extension == ".sxc") return "application/vnd.sun.xml.calc";
    if (extension == ".sxi") return "application/vnd.sun.xml.impress";
    if (extension == ".pages") return "application/x-iwork-pages-sffpages";
    if (extension == ".numbers") return "application/x-iwork-numbers-sffnumbers";
    if (extension == ".key") return "application/x-iwork-keynote-sffkey";
    return "application/octet-stream";
}

void add_attachments(ChatPanelState& panel_state,
                     const std::vector<std::filesystem::path>& paths) {
    for (const std::filesystem::path& path : paths) {
        const std::string media_type = attachment_media_type(path);
        if (media_type.empty()) {
            panel_state.attachment_error = "Unsupported file type: " + path.filename().string();
            continue;
        }
        const auto duplicate = std::find_if(panel_state.attachments.begin(),
            panel_state.attachments.end(), [&](const FileAttachment& attachment) {
                return attachment.filename == path.filename().string() &&
                       attachment.media_type == media_type;
            });
        if (duplicate != panel_state.attachments.end())
            continue;
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            panel_state.attachment_error = "Could not read file: " + path.filename().string();
            continue;
        }
        FileAttachment attachment;
        attachment.path = path;
        attachment.filename = path.filename().string();
        attachment.media_type = media_type;
        attachment.content.assign(std::istreambuf_iterator<char>(file),
                                  std::istreambuf_iterator<char>());
        panel_state.attachments.push_back(std::move(attachment));
    }
}

struct ComposerClipboard {
    ChatPanelState* panel;
    const char* (*get_text)(ImGuiContext*);
    void* user_data;
};

const char* read_composer_clipboard(ImGuiContext* context) {
    ImGuiPlatformIO& platform = context->PlatformIO;
    auto* clipboard = static_cast<ComposerClipboard*>(platform.Platform_ClipboardUserData);
    ClipboardImage image;
    const Result result = read_clipboard_image(&image);
    if (result.status == ResultStatus::Ok && !image.content.empty()) {
        FileAttachment attachment;
        attachment.filename = "Screenshot" + image.extension;
        unsigned int number = 2;
        while (std::any_of(clipboard->panel->attachments.begin(),
                          clipboard->panel->attachments.end(),
                          [&](const FileAttachment& existing) {
                              return existing.filename == attachment.filename;
                          })) {
            attachment.filename = "Screenshot " + std::to_string(number++) + image.extension;
        }
        attachment.media_type = std::move(image.media_type);
        attachment.content = std::move(image.content);
        clipboard->panel->attachments.push_back(std::move(attachment));
        clipboard->panel->attachment_error.clear();
        return "";
    }
    platform.Platform_ClipboardUserData = clipboard->user_data;
    const char* text = clipboard->get_text == nullptr ? nullptr : clipboard->get_text(context);
    platform.Platform_ClipboardUserData = clipboard;
    if ((text == nullptr || text[0] == '\0') && result.status == ResultStatus::Error)
        clipboard->panel->attachment_error = result.error;
    return text;
}

void begin_chat_component() {
    if (has_chat_component) {
        ImVec2 position = ImGui::GetCursorScreenPos();
        position.y += ui_size(chat_component_spacing);
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
    const ImVec2 first(position.x + ui_size(0.5f), position.y + ui_size(1.5f));
    const ImVec2 corner(position.x + ui_size(4.5f), position.y + ui_size(5.0f));
    const ImVec2 last(position.x + ui_size(0.5f), position.y + ui_size(8.5f));
    draw_list->AddLine(first, corner, color, ui_size(1.5f));
    draw_list->AddLine(corner, last, color, ui_size(1.5f));
    draw_list->AddLine(ImVec2(position.x + ui_size(5.1f), position.y + ui_size(8.4f)),
                       ImVec2(position.x + ui_size(12.5f), position.y + ui_size(8.4f)), color, ui_size(1.5f));
}

void render_tool_icon(ImDrawList* draw_list, ImVec2 position, ImU32 color) {
    draw_list->PathLineTo(ImVec2(position.x + ui_size(5.0f), position.y + ui_size(1.7f)));
    draw_list->PathLineTo(ImVec2(position.x + ui_size(6.7f), position.y + ui_size(1.0f)));
    draw_list->PathLineTo(ImVec2(position.x + ui_size(8.9f), position.y + ui_size(1.8f)));
    draw_list->PathLineTo(ImVec2(position.x + ui_size(10.3f), position.y + ui_size(3.1f)));
    draw_list->PathLineTo(ImVec2(position.x + ui_size(10.7f), position.y + ui_size(4.9f)));
    draw_list->PathLineTo(ImVec2(position.x + ui_size(11.1f), position.y + ui_size(5.3f)));
    draw_list->PathLineTo(ImVec2(position.x + ui_size(12.2f), position.y + ui_size(5.3f)));
    draw_list->PathLineTo(ImVec2(position.x + ui_size(12.3f), position.y + ui_size(6.4f)));
    draw_list->PathLineTo(ImVec2(position.x + ui_size(11.0f), position.y + ui_size(7.7f)));
    draw_list->PathLineTo(ImVec2(position.x + ui_size(9.8f), position.y + ui_size(7.7f)));
    draw_list->PathLineTo(ImVec2(position.x + ui_size(9.4f), position.y + ui_size(6.2f)));
    draw_list->PathLineTo(ImVec2(position.x + ui_size(8.8f), position.y + ui_size(6.2f)));
    draw_list->PathLineTo(ImVec2(position.x + ui_size(7.6f), position.y + ui_size(5.7f)));
    draw_list->PathLineTo(ImVec2(position.x + ui_size(6.7f), position.y + ui_size(4.7f)));
    draw_list->PathLineTo(ImVec2(position.x + ui_size(6.2f), position.y + ui_size(3.6f)));
    draw_list->PathLineTo(ImVec2(position.x + ui_size(6.2f), position.y + ui_size(3.3f)));
    draw_list->PathLineTo(ImVec2(position.x + ui_size(5.9f), position.y + ui_size(2.7f)));
    draw_list->PathLineTo(ImVec2(position.x + ui_size(5.0f), position.y + ui_size(2.2f)));
    draw_list->PathFillConcave(color);

    draw_list->PathLineTo(ImVec2(position.x + ui_size(1.0f), position.y + ui_size(9.5f)));
    draw_list->PathLineTo(ImVec2(position.x + ui_size(5.5f), position.y + ui_size(5.0f)));
    draw_list->PathLineTo(ImVec2(position.x + ui_size(7.4f), position.y + ui_size(6.8f)));
    draw_list->PathLineTo(ImVec2(position.x + ui_size(2.9f), position.y + ui_size(11.3f)));
    draw_list->PathLineTo(ImVec2(position.x + ui_size(1.8f), position.y + ui_size(11.7f)));
    draw_list->PathLineTo(ImVec2(position.x + ui_size(1.0f), position.y + ui_size(11.3f)));
    draw_list->PathFillConcave(color);
}

void render_reasoning_icon(ImDrawList* draw_list, ImVec2 position, ImU32 color) {
    draw_list->AddLine(ImVec2(position.x + ui_size(3.5f), position.y + ui_size(2.1f)),
                       ImVec2(position.x + ui_size(8.5f), position.y + ui_size(2.1f)), color, ui_size(1.4f));
    draw_list->AddLine(ImVec2(position.x + ui_size(3.2f), position.y + ui_size(4.1f)),
                       ImVec2(position.x + ui_size(5.4f), position.y + ui_size(7.2f)), color, ui_size(1.4f));
    draw_list->AddRectFilled(ImVec2(position.x, position.y + ui_size(0.7f)),
                             ImVec2(position.x + ui_size(4.0f), position.y + ui_size(4.8f)), color, ui_size(1.0f));
    draw_list->AddRectFilled(ImVec2(position.x + ui_size(8.0f), position.y + ui_size(0.7f)),
                             ImVec2(position.x + ui_size(12.0f), position.y + ui_size(4.8f)), color, ui_size(1.0f));
    draw_list->AddRectFilled(ImVec2(position.x + ui_size(4.7f), position.y + ui_size(6.2f)),
                             ImVec2(position.x + ui_size(8.7f), position.y + ui_size(10.3f)), color, ui_size(1.0f));
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
    const float padding = ui_size(14.0f);
    const float header_padding = ui_size(4.0f);
    const float font_size = ImGui::GetFontSize();
    const float header_height = font_size + header_padding * 2.0f;
    const float line_height = ImGui::GetTextLineHeight();
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const std::size_t line_count = 1 + std::count(code.begin(), code.end(), '\n');
    const float body_height = std::min(ui_size(400.0f), line_count * line_height + padding * 2.0f);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(start, ImVec2(start.x + width, start.y + header_height + body_height),
                             ImGui::GetColorU32(ImVec4(0.105f, 0.105f, 0.105f, 1.0f)), ui_size(5.0f));
    draw_list->AddRectFilled(ImVec2(start.x, start.y + header_height - ui_size(1.0f)),
                             ImVec2(start.x + width, start.y + header_height + body_height),
                             ImGui::GetColorU32(ImVec4(0.065f, 0.065f, 0.065f, 1.0f)), ui_size(5.0f),
                             ImDrawFlags_RoundCornersBottom);
    const ImU32 icon_color = ImGui::GetColorU32(ImVec4(0.47f, 0.47f, 0.47f, 1.0f));
    const ImVec2 icon(start.x + padding, start.y + header_padding + ui_size(2.0f));
    const float icon_scale = ui_size(0.020f);
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
    draw_list->AddText(ImVec2(start.x + padding + ui_size(23.0f), start.y + header_padding),
                       ImGui::GetColorU32(ImVec4(0.82f, 0.82f, 0.82f, 1.0f)), "Code");
    register_text("Code", "Code" + 4,
                  ImVec2(start.x + padding + ui_size(23.0f), start.y + header_padding));
    const std::string language_label = display_language(language);
    if (!language_label.empty()) {
        const float label_width = ImGui::CalcTextSize("Code").x;
        draw_list->AddText(ImVec2(start.x + padding + ui_size(32.0f) + label_width,
                                  start.y + header_padding),
                           ImGui::GetColorU32(ImVec4(0.57f, 0.69f, 0.91f, 1.0f)),
                           language_label.c_str());
        register_text(language_label.c_str(), language_label.c_str() + language_label.size(),
                      ImVec2(start.x + padding + ui_size(32.0f) + label_width,
                             start.y + header_padding));
    }

    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + header_height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding, padding));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::BeginChild("##code-body", ImVec2(width, body_height),
                      ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
    ImDrawList* code_draw_list = ImGui::GetWindowDrawList();
    ImFont* font = monospace_font != nullptr ? monospace_font : ImGui::GetFont();
    ImGui::PushFont(font, 0.0f);
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
    const float corner_radius = ui_size(5.0f);
    const float header_horizontal_padding = ui_size(14.0f);
    const float header_vertical_padding = ui_size(4.0f);
    const float icon_size = ui_size(13.0f);
    const float icon_gap = ui_size(10.0f);
    const float details_horizontal_padding = ui_size(14.0f);
    const float details_vertical_padding = ui_size(7.0f);
    const float max_body_height = ui_size(280.0f);
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
    const float status_gap = has_status ? ui_size(9.0f) : 0.0f;
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
            ImVec2(card_min.x, card_min.y + row_height - ui_size(1.0f)),
            ImVec2(card_min.x + card_width, card_min.y + card_height),
            ImGui::GetColorU32(ImVec4(0.065f, 0.065f, 0.065f, 1.0f)), corner_radius,
            ImDrawFlags_RoundCornersBottom);
    }

    const ImVec2 icon_position(card_min.x + header_horizontal_padding,
                               card_min.y + header_vertical_padding +
                                   (font_size - icon_size) * 0.5f);
    if (icon == ActivityIcon::Terminal)
        render_terminal_icon(draw_list, ImVec2(icon_position.x, icon_position.y + ui_size(1.5f)),
                             icon_color);
    else if (icon == ActivityIcon::Reasoning)
        render_reasoning_icon(draw_list, icon_position, icon_color);
    else
        render_tool_icon(draw_list, icon_position, icon_color);

    const ImVec4 text_color(0.82f, 0.82f, 0.82f, 1.0f);
    ImGui::PushFont(header_font, 0.0f);
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
            ImGui::PushFont(details_font, 0.0f);
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

std::string renderable_assistant_text(const std::string& text,
                                      const std::vector<ChatAttachment>* attachments) {
    constexpr std::string_view citation_start = "\xEE\x88\x80" "filecite";
    constexpr std::string_view citation_end = "\xEE\x88\x81";
    std::string result;
    std::size_t cursor = 0;
    for (;;) {
        const std::size_t start = text.find(citation_start, cursor);
        if (start == std::string::npos) {
            result.append(text, cursor);
            break;
        }
        result.append(text, cursor, start - cursor);
        const std::size_t end = text.find(citation_end, start + citation_start.size());
        if (end == std::string::npos) {
            result.append(text, start, std::string::npos);
            break;
        }

        const std::string_view reference(text.data() + start + citation_start.size(),
                                         end - start - citation_start.size());
        const std::size_t file_index_marker = reference.rfind("file");
        if (attachments != nullptr && file_index_marker != std::string_view::npos) {
            const char* digits = reference.data() + file_index_marker + 4;
            const char* reference_end = reference.data() + reference.size();
            std::size_t file_index = 0;
            bool has_index = digits < reference_end;
            for (; digits < reference_end; ++digits) {
                if (!std::isdigit(static_cast<unsigned char>(*digits))) {
                    has_index = false;
                    break;
                }
                file_index = file_index * 10 + static_cast<std::size_t>(*digits - '0');
            }
            if (has_index && file_index < attachments->size())
                result += (*attachments)[file_index].filename;
        }
        cursor = end + citation_end.size();
    }
    return result;
}

float attachment_tag_height() {
    return ImGui::GetFontSize() + ui_size(10.0f);
}
constexpr float attachment_tag_spacing = 6.0f;
constexpr float attachment_tag_row_spacing = 5.0f;

float attachment_tag_width(const std::string& label, bool removable) {
    const float text_width = measure_text(ImGui::GetFont(), ImGui::GetFontSize(), label).x;
    return ui_size(29.0f) + text_width + ui_size(removable ? 30.0f : 9.0f);
}

std::string attachment_tag_label(const std::string& filename, float max_width,
                                 bool removable) {
    const float reserved_width = attachment_tag_width("", removable);
    return elide_tool_title(filename, std::max(1.0f, max_width - reserved_width),
                            ImGui::GetFont(), ImGui::GetFontSize());
}

bool render_attachment_tag(const char* id, ImVec2 position, float width,
                           const std::string& label, bool removable, bool bordered,
                           bool selectable_text, SDL_GPUTexture* icon_texture) {
    ImGui::SetCursorScreenPos(position);
    ImGui::InvisibleButton(id, ImVec2(width, attachment_tag_height()));
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 tag_max(position.x + width, position.y + attachment_tag_height());
    const ImVec4 background = hovered
        ? ImVec4(0.24f, 0.24f, 0.24f, 1.0f)
        : ImVec4(0.20f, 0.20f, 0.20f, 1.0f);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(position, tag_max, ImGui::GetColorU32(background), ui_size(5.0f));
    if (bordered)
        draw_list->AddRect(position, tag_max,
            ImGui::GetColorU32(ImVec4(0.34f, 0.34f, 0.34f, 1.0f)), ui_size(5.0f));

    if (icon_texture != nullptr) {
        const ImVec2 icon_min(position.x + ui_size(11.0f),
                               position.y + (attachment_tag_height() - ui_size(12.8f)) * 0.5f);
        const ImVec2 icon_max(icon_min.x + ui_size(8.8f), icon_min.y + ui_size(12.8f));
        draw_list->AddImage(ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(icon_texture))),
            icon_min, icon_max, ImVec2(0, 0), ImVec2(1, 1),
            ImGui::GetColorU32(ImVec4(0.80f, 0.80f, 0.80f, 1.0f)));
    }

    ImFont* font = ImGui::GetFont();
    const float font_size = ImGui::GetFontSize();
    const ImVec2 label_pos(position.x + ui_size(29.0f),
                           position.y + (attachment_tag_height() - font_size) * 0.5f);
    draw_list->AddText(font, font_size, label_pos,
                       ImGui::GetColorU32(ImVec4(0.90f, 0.90f, 0.90f, 1.0f)),
                       label.c_str());
    if (selectable_text)
        register_text(label.c_str(), label.c_str() + label.size(),
                      label_pos, font, font_size);

    if (!removable)
        return false;
    const float close_x = tag_max.x - ui_size(14.0f);
    const float close_y = position.y + attachment_tag_height() * 0.5f;
    const bool close_hovered = hovered && ImGui::GetIO().MousePos.x >= tag_max.x - ui_size(25.0f);
    const ImU32 close_color = ImGui::GetColorU32(close_hovered
        ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f)
        : ImVec4(0.75f, 0.75f, 0.75f, 1.0f));
    draw_list->AddLine(ImVec2(close_x - ui_size(4.0f), close_y - ui_size(4.0f)),
                       ImVec2(close_x + ui_size(4.0f), close_y + ui_size(4.0f)), close_color, ui_size(1.7f));
    draw_list->AddLine(ImVec2(close_x + ui_size(4.0f), close_y - ui_size(4.0f)),
                       ImVec2(close_x - ui_size(4.0f), close_y + ui_size(4.0f)), close_color, ui_size(1.7f));
    return close_hovered && ImGui::IsItemClicked();
}

void render_user_message(const ChatMessage& message, SDL_GPUTexture* icon_texture) {
    begin_chat_component();
    const float horizontal_padding = ui_size(12.0f);
    const float vertical_padding = ui_size(8.0f);
    constexpr float max_width_ratio = 0.8f;
    const float available_width = ImGui::GetContentRegionAvail().x;
    const float max_text_width = std::max(1.0f, available_width * max_width_ratio -
                                                   horizontal_padding * 2.0f);
    ImFont* font = ImGui::GetFont();
    const float font_size = ImGui::GetFontSize();
    const ImVec2 measured = ImGui::CalcTextSize(message.content.c_str(), nullptr, false,
                                                max_text_width);
    const float text_width = message.content.empty() ? 0.0f
        : std::min(max_text_width, std::max(1.0f, measured.x));
    const float text_height = message.content.empty() ? 0.0f
        : wrapped_text_height(message.content, max_text_width, font, font_size);
    std::vector<std::string> attachment_labels;
    std::vector<float> attachment_widths;
    float attachment_width = 0.0f;
    float row_width = 0.0f;
    std::size_t attachment_rows = 0;
    for (const ChatAttachment& attachment : message.attachments) {
        const std::string label = attachment_tag_label(
            attachment.filename, max_text_width, false);
        const float width = attachment_tag_width(label, false);
        if (attachment_rows == 0) {
            attachment_rows = 1;
        } else if (row_width > 0.0f &&
                   row_width + ui_size(attachment_tag_spacing) + width > max_text_width) {
            ++attachment_rows;
            row_width = 0.0f;
        }
        if (row_width > 0.0f)
            row_width += ui_size(attachment_tag_spacing);
        row_width += width;
        attachment_width = std::max(attachment_width, row_width);
        attachment_labels.push_back(label);
        attachment_widths.push_back(width);
    }
    const float attachment_height = attachment_rows == 0 ? 0.0f
        : attachment_rows * attachment_tag_height() +
          (attachment_rows - 1) * ui_size(attachment_tag_row_spacing);
    const float content_gap = attachment_height > 0.0f && text_height > 0.0f ? ui_size(6.0f) : 0.0f;
    const ImVec2 row_pos = ImGui::GetCursorScreenPos();
    const float bubble_width = std::max(1.0f, std::max(text_width, attachment_width)) +
                               horizontal_padding * 2.0f;
    const float bubble_height = text_height + attachment_height + content_gap +
                                vertical_padding * 2.0f;
    const ImVec2 bubble_min(row_pos.x + available_width - bubble_width, row_pos.y);
    const ImVec2 bubble_max(bubble_min.x + bubble_width, bubble_min.y + bubble_height);

    ImGui::GetWindowDrawList()->AddRectFilled(
        bubble_min, bubble_max, ImGui::GetColorU32(ImVec4(0.20f, 0.20f, 0.20f, 1.0f)), ui_size(6.0f));
    float chip_x = bubble_min.x + horizontal_padding;
    float chip_y = bubble_min.y + vertical_padding;
    for (std::size_t index = 0; index < attachment_labels.size(); ++index) {
        const float width = attachment_widths[index];
        if (chip_x > bubble_min.x + horizontal_padding &&
            chip_x + width > bubble_max.x - horizontal_padding) {
            chip_x = bubble_min.x + horizontal_padding;
            chip_y += attachment_tag_height() + ui_size(attachment_tag_row_spacing);
        }
        ImGui::PushID(static_cast<int>(index));
        render_attachment_tag("##sent-attachment", ImVec2(chip_x, chip_y), width,
                              attachment_labels[index], false, true, true, icon_texture);
        ImGui::PopID();
        chip_x += width + ui_size(attachment_tag_spacing);
    }
    if (!message.content.empty()) {
        ImGui::SetCursorScreenPos(ImVec2(bubble_min.x + horizontal_padding,
            bubble_min.y + vertical_padding + attachment_height + content_gap));
        render_wrapped_selectable_text(message.content, max_text_width);
    }
    ImGui::SetCursorScreenPos(row_pos);
    ImGui::Dummy(ImVec2(available_width, bubble_height));
}

constexpr std::size_t file_picker_result_limit = 50;
constexpr std::size_t file_picker_scan_budget = 300;
constexpr std::size_t file_picker_visible_rows = 5;
float file_picker_row_height() {
    return ImGui::GetFontSize() + ui_size(12.0f);
}
constexpr float file_picker_top_padding = 4.0f;
constexpr float file_picker_bottom_padding = 6.0f;

float file_picker_height(const ChatPanelState& panel_state) {
    const std::size_t rows = std::clamp(panel_state.file_picker_results.size(),
                                        std::size_t{1}, file_picker_visible_rows);
    return ui_size(file_picker_top_padding) + rows * file_picker_row_height() +
           ui_size(file_picker_bottom_padding);
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool find_file_reference_query(const std::string& input, ImGuiInputTextState* input_state,
                               std::size_t* replace_start, std::size_t* replace_end,
                               std::string* query) {
    if (input_state == nullptr || input_state->HasSelection())
        return false;

    const std::size_t cursor = std::min(input.size(),
        static_cast<std::size_t>(std::max(0, input_state->GetCursorPos())));
    if (cursor == 0)
        return false;

    const std::size_t at = input.rfind('@', cursor - 1);
    if (at == std::string::npos)
        return false;
    if (at > 0) {
        const unsigned char previous = static_cast<unsigned char>(input[at - 1]);
        if (!std::isspace(previous) && input[at - 1] != '(' && input[at - 1] != '[' &&
            input[at - 1] != '{' && input[at - 1] != '"' && input[at - 1] != '\'')
            return false;
    }

    const std::size_t token_end = input.find_first_of(" \t\r\n", at + 1);
    const std::size_t end = token_end == std::string::npos ? input.size() : token_end;
    if (cursor > end)
        return false;

    *replace_start = at;
    *replace_end = end;
    *query = input.substr(at + 1, end - at - 1);
    return true;
}

void sort_file_picker_results(ChatPanelState& panel_state) {
    std::sort(panel_state.file_picker_results.begin(), panel_state.file_picker_results.end(),
              [](const std::filesystem::path& left, const std::filesystem::path& right) {
                  return left.generic_string() < right.generic_string();
              });
}

void reset_file_picker_search(ChatPanelState& panel_state,
                              const std::filesystem::path& root,
                              const std::string& query) {
    panel_state.file_picker_root = root.lexically_normal();
    panel_state.file_picker_query = lowercase(query);
    panel_state.file_picker_results.clear();
    panel_state.file_picker_selected = 0;

    if (root.empty()) {
        panel_state.file_picker_scan_complete = true;
        return;
    }

    std::error_code error;
    panel_state.file_picker_iterator = std::filesystem::recursive_directory_iterator(
        root, std::filesystem::directory_options::skip_permission_denied, error);
    panel_state.file_picker_scan_complete = error ||
        panel_state.file_picker_iterator == panel_state.file_picker_end;
    if (panel_state.file_picker_scan_complete)
        sort_file_picker_results(panel_state);
}

void advance_file_picker_search(ChatPanelState& panel_state,
                                const std::filesystem::path& root) {
    if (panel_state.file_picker_scan_complete)
        return;

    std::size_t scanned = 0;
    while (panel_state.file_picker_iterator != panel_state.file_picker_end &&
           scanned < file_picker_scan_budget &&
           panel_state.file_picker_results.size() < file_picker_result_limit) {
        const std::filesystem::directory_entry entry = *panel_state.file_picker_iterator;
        ++scanned;

        std::error_code entry_error;
        if (entry.is_directory(entry_error)) {
            if (entry.path().filename() == ".git")
                panel_state.file_picker_iterator.disable_recursion_pending();
        } else if (!entry_error && entry.is_regular_file(entry_error) && !entry_error) {
            const std::filesystem::path relative = entry.path().lexically_relative(root);
            const std::string display_path = relative.generic_string();
            if (lowercase(display_path).find(panel_state.file_picker_query) !=
                std::string::npos)
                panel_state.file_picker_results.push_back(relative);
        }

        std::error_code increment_error;
        panel_state.file_picker_iterator.increment(increment_error);
        if (increment_error) {
            panel_state.file_picker_scan_complete = true;
            break;
        }
    }

    if (panel_state.file_picker_iterator == panel_state.file_picker_end ||
        panel_state.file_picker_results.size() == file_picker_result_limit) {
        panel_state.file_picker_scan_complete = true;
    }
    if (panel_state.file_picker_scan_complete)
        sort_file_picker_results(panel_state);
}

void select_file_reference(ChatPanelState& panel_state, std::string& input,
                           ImGuiInputTextState* input_state,
                           const std::filesystem::path& path) {
    const std::string reference = "@" + path.generic_string();
    input.replace(panel_state.file_picker_replace_start,
                  panel_state.file_picker_replace_end - panel_state.file_picker_replace_start,
                  reference + " ");

    const auto existing = std::find_if(panel_state.file_references.begin(),
                                       panel_state.file_references.end(),
        [&](const FileReference& item) { return item.path == path; });
    if (existing == panel_state.file_references.end())
        panel_state.file_references.push_back({path});

    if (input_state != nullptr)
        input_state->ReloadUserBufAndMoveToEnd();
    panel_state.file_picker_open = false;
    panel_state.restore_input_focus = true;
}

void render_file_picker(ChatPanelState& panel_state, std::string& input,
                        ImGuiInputTextState* input_state, ImVec2 input_position,
                        float width, bool* enter) {
    if (!panel_state.file_picker_open)
        return;

    bool selection_changed = false;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) &&
        !panel_state.file_picker_results.empty()) {
        panel_state.file_picker_selected = std::min(
            panel_state.file_picker_selected + 1,
            panel_state.file_picker_results.size() - 1);
        selection_changed = true;
    } else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) &&
               !panel_state.file_picker_results.empty()) {
        if (panel_state.file_picker_selected > 0)
            --panel_state.file_picker_selected;
        selection_changed = true;
    }

    if (*enter) {
        *enter = false;
        if (!panel_state.file_picker_results.empty()) {
            select_file_reference(panel_state, input, input_state,
                panel_state.file_picker_results[panel_state.file_picker_selected]);
            return;
        }
    }

    const float height = file_picker_height(panel_state);
    const ImVec2 popup_min(input_position.x, input_position.y - height - ui_size(8.0f));
    ImGui::SetNextWindowPos(popup_min);
    ImGui::SetNextWindowSize(ImVec2(width, height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, ui_size(file_picker_top_padding)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ui_size(6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(27.0f / 255.0f,
                                                   27.0f / 255.0f,
                                                   27.0f / 255.0f, 1.0f));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoScrollbar;
    if (ImGui::Begin("##file-reference-picker", nullptr, flags)) {
        if (panel_state.file_picker_results.empty()) {
            const ImVec2 row_min = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(width, file_picker_row_height()));
            const char* status = panel_state.file_picker_scan_complete
                ? "No matching files" : "Searching workspace...";
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(row_min.x + ui_size(12.0f),
                       row_min.y + (file_picker_row_height() - ImGui::GetFontSize()) * 0.5f),
                ImGui::GetColorU32(ImGuiCol_TextDisabled), status);
        } else {
            for (std::size_t index = 0; index < panel_state.file_picker_results.size(); ++index) {
                const std::string display_path =
                    panel_state.file_picker_results[index].generic_string();
                const bool selected = panel_state.file_picker_selected == index;
                ImGui::PushID(static_cast<int>(index));
                const bool clicked = ImGui::InvisibleButton(
                    "##file-reference-row", ImVec2(width, file_picker_row_height()));
                const ImVec2 row_min = ImGui::GetItemRectMin();
                if (selected || ImGui::IsItemHovered()) {
                    ImGui::GetWindowDrawList()->AddRectFilled(
                        row_min, ImGui::GetItemRectMax(),
                        ImGui::GetColorU32(ImVec4(51.0f / 255.0f,
                                                  51.0f / 255.0f,
                                                  51.0f / 255.0f, 1.0f)));
                }
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(row_min.x + ui_size(12.0f),
                           row_min.y + (file_picker_row_height() - ImGui::GetFontSize()) * 0.5f),
                    ImGui::GetColorU32(ImGuiCol_Text), display_path.c_str());
                if (selected && selection_changed)
                    ImGui::SetScrollHereY(0.5f);
                ImGui::PopID();
                if (clicked) {
                    select_file_reference(panel_state, input, input_state,
                                          panel_state.file_picker_results[index]);
                    break;
                }
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(4);
}

}

void render_chat_panel(ApplicationState& state, std::vector<ProviderPtr>& providers,
                       ChatPanelState& panel_state,
                       const std::shared_ptr<FileDialogQueue>& dialog_queue,
                       SDL_Window* window) {
    std::string& message_input = panel_state.message_input;
    std::size_t& selected_provider = panel_state.selected_provider;
    std::string& selected_model = panel_state.selected_model;
    std::string& selected_reasoning_effort = panel_state.selected_reasoning_effort;
    std::string& selected_permission_mode = panel_state.selected_permission_mode;
    bool& is_generating = panel_state.is_generating;
    TurnId& active_turn_id = panel_state.active_turn_id;
    TurnId& next_turn_id = panel_state.next_turn_id;
    ImFont* monospace_font = panel_state.monospace_font;
    Provider* provider = providers[selected_provider].get();
    if (!is_generating && provider->availability != ProviderAvailability::Available) {
        for (std::size_t index = 0; index < providers.size(); ++index) {
            if (providers[index]->availability == ProviderAvailability::Available) {
                selected_provider = index;
                provider = providers[index].get();
                selected_model = provider->default_model;
                selected_reasoning_effort.clear();
                selected_permission_mode.clear();
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
            selected_permission_mode.clear();
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
            const bool permission_supported = selected_permission_mode.empty() ||
                std::any_of(active->permission_modes.begin(), active->permission_modes.end(),
                    [&](const PermissionOption& option) {
                        return option.value == selected_permission_mode;
                    });
            if (!permission_supported)
                selected_permission_mode = active->default_permission_mode;
        }
    }
    ImGui::Begin("Chat");
    if (state.selected_project < state.projects.size() &&
        state.selected_thread < state.projects[state.selected_project].threads.size()) {
        ChatProject& project = state.projects[state.selected_project];
        std::vector<ChatThread>& threads = project.threads;
        const std::filesystem::path project_root = project.directory.lexically_normal();
        if (panel_state.file_references_root != project_root) {
            panel_state.file_references.clear();
            panel_state.attachments.clear();
            panel_state.attachment_error.clear();
            panel_state.file_references_root = project_root;
        }
        if (panel_state.file_picker_open && panel_state.file_picker_root != project_root)
            panel_state.file_picker_open = false;
        ChatThread& thread = threads[state.selected_thread];
        const float outer_padding = ui_size(8.0f);
        const float input_height = std::max(ui_size(58.0f),
            ImGui::GetTextLineHeight() * 2.0f + ImGui::GetStyle().FramePadding.y * 2.0f);
        const float footer_height = std::max(ui_size(38.0f),
            ImGui::GetFrameHeight() + ui_size(6.0f));
        const float max_chat_width = ui_size(960.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        const float available_width = ImGui::GetContentRegionAvail().x;
        const float chat_width = std::min(available_width, max_chat_width);
        const float tag_area_width = std::max(1.0f, chat_width - outer_padding * 2.0f);
        std::size_t attachment_rows = 0;
        float attachment_row_width = 0.0f;
        for (const FileAttachment& attachment : panel_state.attachments) {
            const std::string label = attachment_tag_label(
                attachment.filename, tag_area_width, true);
            const float width = attachment_tag_width(label, true);
            if (attachment_rows == 0) {
                attachment_rows = 1;
            } else if (attachment_row_width > 0.0f &&
                       attachment_row_width + ui_size(attachment_tag_spacing) + width > tag_area_width) {
                ++attachment_rows;
                attachment_row_width = 0.0f;
            }
            if (attachment_row_width > 0.0f)
                attachment_row_width += ui_size(attachment_tag_spacing);
            attachment_row_width += width;
        }
        const float attachment_tags_height = attachment_rows == 0 ? 0.0f
            : attachment_rows * attachment_tag_height() +
              (attachment_rows - 1) * ui_size(attachment_tag_row_spacing) + ui_size(6.0f);
        const float attachment_error_height = panel_state.attachment_error.empty() ? 0.0f
            : ImGui::GetFontSize() + ui_size(6.0f);
        const float attachment_row_height = attachment_tags_height + attachment_error_height;
        const float composer_height = outer_padding + attachment_row_height +
                                      input_height + footer_height;
        const float chat_offset = (available_width - chat_width) * 0.5f;
        const float chat_x = ImGui::GetCursorPosX() + chat_offset;
        const float available_height = ImGui::GetContentRegionAvail().y;
        const float message_height = std::max(
            1.0f, available_height - composer_height - ImGui::GetStyle().ItemSpacing.y);
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
        const std::vector<ChatAttachment>* citation_attachments = nullptr;
        for (std::size_t message_index = 0; message_index < thread.messages.size(); ++message_index) {
            const ChatMessage& message = thread.messages[message_index];
            ImGui::PushID(static_cast<int>(message_index));
            ImGui::BeginGroup();
            if (message.role == ChatMessageRole::User) {
                citation_attachments = message.attachments.empty() ? nullptr : &message.attachments;
                render_user_message(message, panel_state.attachment_icon_texture);
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
                            render_markdown_text(markdown,
                                renderable_assistant_text(segment.text, citation_attachments));
                        }
                    }
                } else {
                    for (const std::string& activity : message.tool_activities) {
                        ToolActivity tool;
                        tool.command = activity;
                        render_tool_activity(tool, monospace_font);
                    }
                    render_markdown_text(markdown,
                        renderable_assistant_text(message.content, citation_attachments));
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
        const float total_height = composer_height;
        const float send_size = ui_size(32.0f);
        const ImVec2 frame_max(input_pos.x + full_width, input_pos.y + total_height);
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        draw_list->AddRectFilled(input_pos, frame_max,
                                 ImGui::GetColorU32(ImGuiCol_FrameBg), ui_size(6.0f));
        draw_list->AddRect(input_pos, frame_max,
                           ImGui::GetColorU32(ImGuiCol_Border), ui_size(6.0f));
        const float divider_y = input_pos.y + outer_padding + attachment_row_height + input_height;
        draw_list->AddLine(ImVec2(input_pos.x + ui_size(1.0f), divider_y),
                           ImVec2(frame_max.x - ui_size(1.0f), divider_y),
                           ImGui::GetColorU32(ImGuiCol_Border), ui_size(1.0f));

        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        if (!panel_state.attachments.empty()) {
            float tag_x = input_pos.x + outer_padding;
            float tag_y = input_pos.y + outer_padding;
            for (std::size_t i = 0; i < panel_state.attachments.size(); ++i) {
                const std::string label = attachment_tag_label(
                    panel_state.attachments[i].filename, tag_area_width, true);
                const float width = attachment_tag_width(label, true);
                if (tag_x > input_pos.x + outer_padding &&
                    tag_x + width > frame_max.x - outer_padding) {
                    tag_x = input_pos.x + outer_padding;
                    tag_y += attachment_tag_height() + ui_size(attachment_tag_row_spacing);
                }
                ImGui::PushID(static_cast<int>(i));
                const bool remove = render_attachment_tag(
                    "##composer-attachment", ImVec2(tag_x, tag_y),
                    width, label, true, false, false,
                    panel_state.attachment_icon_texture);
                ImGui::PopID();
                if (remove) {
                    panel_state.attachments.erase(panel_state.attachments.begin() + i);
                    break;
                }
                tag_x += width + ui_size(attachment_tag_spacing);
            }
        }
        if (!panel_state.attachment_error.empty()) {
            ImGui::SetCursorScreenPos(ImVec2(input_pos.x + outer_padding,
                input_pos.y + outer_padding + attachment_tags_height));
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s",
                               panel_state.attachment_error.c_str());
        }
        ImGui::SetCursorScreenPos(ImVec2(input_pos.x + outer_padding,
                                         input_pos.y + outer_padding + attachment_row_height));
        if (panel_state.restore_input_focus) {
            ImGui::SetKeyboardFocusHere();
            panel_state.restore_input_focus = false;
        }
        ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
        ComposerClipboard clipboard{&panel_state, platform.Platform_GetClipboardTextFn,
                                    platform.Platform_ClipboardUserData};
        platform.Platform_GetClipboardTextFn = read_composer_clipboard;
        platform.Platform_ClipboardUserData = &clipboard;
        bool enter = ImGui::InputTextMultiline(
            "##message-input", &message_input,
            ImVec2(full_width - outer_padding * 2.0f, input_height),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine);
        platform.Platform_GetClipboardTextFn = clipboard.get_text;
        platform.Platform_ClipboardUserData = clipboard.user_data;
        const bool input_active = ImGui::IsItemActive();
        ImGuiInputTextState* input_state = ImGui::GetInputTextState(ImGui::GetItemID());
        if (panel_state.file_picker_open && input_active && input_state != nullptr &&
            (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) ||
             ImGui::IsKeyPressed(ImGuiKey_DownArrow, false))) {
            const int cursor = static_cast<int>(std::min(
                panel_state.file_picker_cursor, message_input.size()));
            input_state->SetSelection(cursor, cursor);
        }
        static std::string composer_context_selection;
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(1)) {
            composer_context_selection.clear();
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

        std::size_t replace_start = 0;
        std::size_t replace_end = 0;
        std::string file_query;
        const bool has_file_query = input_active && find_file_reference_query(
            message_input, input_state, &replace_start, &replace_end, &file_query);
        if (has_file_query) {
            panel_state.file_picker_open = true;
            panel_state.file_picker_replace_start = replace_start;
            panel_state.file_picker_replace_end = replace_end;
            panel_state.file_picker_cursor = input_state->GetCursorPos();
            if (panel_state.file_picker_root != project_root ||
                panel_state.file_picker_query != lowercase(file_query))
                reset_file_picker_search(panel_state, project_root, file_query);
        } else if (input_active) {
            panel_state.file_picker_open = false;
        }
        if (panel_state.file_picker_open)
            advance_file_picker_search(panel_state, panel_state.file_picker_root);
        if (!input_active && panel_state.file_picker_open && ImGui::IsMouseClicked(0)) {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const bool over_input = mouse.x >= input_pos.x + outer_padding &&
                mouse.x < input_pos.x + full_width - outer_padding &&
                mouse.y >= input_pos.y + outer_padding + attachment_row_height &&
                mouse.y < input_pos.y + outer_padding + attachment_row_height + input_height;
            const float popup_height = file_picker_height(panel_state);
            const ImVec2 popup_min(input_pos.x,
                                   input_pos.y - popup_height - outer_padding);
            const bool over_picker = mouse.x >= popup_min.x &&
                mouse.x < popup_min.x + full_width &&
                mouse.y >= popup_min.y && mouse.y < popup_min.y + popup_height;
            if (!over_input && !over_picker)
                panel_state.file_picker_open = false;
        }
        render_file_picker(panel_state, message_input, input_state, input_pos,
                           full_width, &enter);
        const ImVec2 send_pos(frame_max.x - send_size - outer_padding,
                              divider_y + (footer_height - send_size) * 0.5f);
        const float selector_y = divider_y + (footer_height - ImGui::GetFrameHeight()) * 0.5f;
        const float selector_x = input_pos.x + outer_padding;
        const float controls_x = selector_x;
        const float selector_available = std::max(0.0f, send_pos.x - controls_x - ui_size(50.0f));
        const float model_width = std::min(ui_size(190.0f), selector_available * 0.48f);
        const float reasoning_width = std::min(ui_size(140.0f), std::max(0.0f, selector_available * 0.28f));
        const auto active_model = std::find_if(provider->models.begin(), provider->models.end(),
            [&](const ModelOption& model) { return model.id == selected_model; });
        if (active_model != provider->models.end() && model_width >= ui_size(60.0f)) {
            ImGui::SetCursorScreenPos(ImVec2(controls_x, selector_y));
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
                                selected_permission_mode = model.default_permission_mode;
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
            if (reasoning_width >= ui_size(60.0f)) {
                const std::string effort_label = selected_reasoning_effort.empty()
                    ? "Default" : selected_reasoning_effort;
                ImGui::SetCursorScreenPos(ImVec2(controls_x + model_width + ui_size(8.0f), selector_y));
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
            if (reasoning_model != provider->models.end() &&
                !reasoning_model->permission_modes.empty()) {
                const float permission_width = std::min(
                    ui_size(140.0f), std::max(0.0f, selector_available - model_width -
                                            reasoning_width - ui_size(16.0f)));
                if (permission_width >= ui_size(60.0f)) {
                    const PermissionOption* selected_permission = nullptr;
                    for (const PermissionOption& option : reasoning_model->permission_modes) {
                        if (option.value == selected_permission_mode) {
                            selected_permission = &option;
                            break;
                        }
                    }
                    const char* permission_label = selected_permission == nullptr
                        ? "Default" : selected_permission->name.c_str();
                    ImGui::SetCursorScreenPos(ImVec2(
                        controls_x + model_width + ui_size(8.0f) + reasoning_width + ui_size(8.0f),
                        selector_y));
                    ImGui::SetNextItemWidth(permission_width);
                    ImGui::BeginDisabled(is_generating);
                    if (ImGui::BeginCombo("##permission-selector", permission_label)) {
                        if (ImGui::Selectable("Default", selected_permission_mode.empty()))
                            selected_permission_mode = reasoning_model->default_permission_mode;
                        for (const PermissionOption& option : reasoning_model->permission_modes) {
                            const bool is_selected = option.value == selected_permission_mode;
                            if (ImGui::Selectable(option.name.c_str(), is_selected))
                                selected_permission_mode = option.value;
                            if (is_selected)
                                ImGui::SetItemDefaultFocus();
                            if (ImGui::IsItemHovered() && !option.description.empty())
                                ImGui::SetTooltip("%s", option.description.c_str());
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::EndDisabled();
                }
            }
        }
        const ImVec2 attach_pos(send_pos.x - ui_size(42.0f), selector_y);
        ImGui::SetCursorScreenPos(attach_pos);
        const bool attach_clicked = ImGui::Button("##attach-files",
                                                   ImVec2(ui_size(30.0f), ImGui::GetFrameHeight()));
        const ImVec2 attach_min = ImGui::GetItemRectMin();
        const ImVec2 attach_max = ImGui::GetItemRectMax();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Attach files");
        if (panel_state.paperclip_icon_texture != nullptr) {
            const ImVec2 icon_min((attach_min.x + attach_max.x - ui_size(14.0f)) * 0.5f,
                                  (attach_min.y + attach_max.y - ui_size(16.0f)) * 0.5f);
            draw_list->AddImage(
                ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(panel_state.paperclip_icon_texture))),
                icon_min, ImVec2(icon_min.x + ui_size(14.0f), icon_min.y + ui_size(16.0f)),
                ImVec2(0, 0), ImVec2(1, 1), ImGui::GetColorU32(ImGuiCol_Text));
        }
        if (attach_clicked) {
            panel_state.attachment_error.clear();
            show_file_dialog(dialog_queue, FileDialogPurpose::AttachFiles, window);
        }
        ImGui::SetCursorScreenPos(send_pos);
        const bool send_clicked = ImGui::InvisibleButton("##send-message", ImVec2(send_size, send_size));
        const ImVec2 button_min = ImGui::GetItemRectMin();
        const ImVec2 button_max = ImGui::GetItemRectMax();
        const ImU32 button_color = ImGui::GetColorU32(
            ImGui::IsItemActive() ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f)
            : ImGui::IsItemHovered() ? ImVec4(0.42f, 0.42f, 0.42f, 1.0f)
                                     : ImVec4(0.30f, 0.30f, 0.30f, 1.0f));
        draw_list->AddRectFilled(button_min, button_max, button_color, ui_size(6.0f));
        const ImVec2 arrow_center((button_min.x + button_max.x) * 0.5f,
                                  (button_min.y + button_max.y) * 0.5f);
        const ImU32 arrow_color = ImGui::GetColorU32(ImGui::IsItemActive()
            ? ImVec4(0.08f, 0.08f, 0.08f, 1.0f)
            : ImVec4(0.94f, 0.94f, 0.94f, 1.0f));
        if (is_generating) {
            const ImVec2 half_size(ui_size(5.0f), ui_size(5.0f));
            draw_list->AddRectFilled(ImVec2(arrow_center.x - half_size.x, arrow_center.y - half_size.y),
                                     ImVec2(arrow_center.x + half_size.x, arrow_center.y + half_size.y),
                                     arrow_color, ui_size(1.0f));
        } else {
            draw_list->AddLine(ImVec2(arrow_center.x, arrow_center.y + ui_size(6.0f)),
                               ImVec2(arrow_center.x, arrow_center.y - ui_size(5.0f)), arrow_color, ui_size(2.0f));
            draw_list->AddLine(ImVec2(arrow_center.x, arrow_center.y - ui_size(5.0f)),
                               ImVec2(arrow_center.x - ui_size(4.5f), arrow_center.y - ui_size(0.5f)), arrow_color, ui_size(2.0f));
            draw_list->AddLine(ImVec2(arrow_center.x, arrow_center.y - ui_size(5.0f)),
                               ImVec2(arrow_center.x + ui_size(4.5f), arrow_center.y - ui_size(0.5f)), arrow_color, ui_size(2.0f));
        }
        ImGui::SetCursorScreenPos(input_pos);
        ImGui::Dummy(ImVec2(full_width, total_height));
        if (send_clicked && is_generating) {
            provider->cancel(provider, active_turn_id);
            is_generating = false;
            active_turn_id = 0;
        } else if (!is_generating && provider->availability == ProviderAvailability::Available &&
                   (enter || send_clicked) && (!message_input.empty() || !panel_state.attachments.empty())) {
            if (state.selected_thread > 0) {
                std::rotate(threads.begin(),
                            threads.begin() + state.selected_thread,
                            threads.begin() + state.selected_thread + 1);
                state.selected_thread = 0;
            }
            ChatThread& destination = threads[state.selected_thread];
            std::string prompt = std::move(message_input);
            message_input.clear();
            ChatMessage user_message{ChatMessageRole::User, prompt, {}, {}, {}, {}};
            for (const FileAttachment& attachment : panel_state.attachments) {
                user_message.attachments.push_back({
                    attachment.filename, attachment.media_type, attachment.content.size()});
            }
            destination.messages.push_back(std::move(user_message));
            TurnRequest request;
            request.conversation_id = destination.id;
            request.prompt = std::move(prompt);
            request.history = destination.messages;
            request.attachments = std::move(panel_state.attachments);
            panel_state.attachments.clear();
            for (const FileReference& reference : panel_state.file_references) {
                const std::string token = "@" + reference.path.generic_string();
                if (request.prompt.find(token) != std::string::npos)
                    request.file_references.push_back(reference);
            }
            panel_state.file_references.clear();
            request.working_directory = project.directory;
            request.model = selected_model;
            request.reasoning_effort = selected_reasoning_effort;
            request.permission_mode = selected_permission_mode;
            request.turn_id = next_turn_id++;
            const Result submitted = provider->submit(provider, std::move(request));
            if (submitted.status == ResultStatus::Error) {
                destination.messages.push_back(
                    {ChatMessageRole::Assistant, std::string(submitted.error), {}, {}, {}, {}});
            } else {
                active_turn_id = next_turn_id - 1;
                is_generating = true;
            }
        }
    }
    ImGui::End();
}

void apply_attachment_result(ChatPanelState& panel_state, const FileDialogResult& result) {
    if (!result.error.empty()) {
        panel_state.attachment_error = "Failed to open file dialog: " + result.error;
        return;
    }
    add_attachments(panel_state, result.paths);
}
