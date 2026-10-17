#include "provider_runtime.h"
#include "provider_mcp_utils.h"
#include "usage-time.h"
#include "../base/os.h"
#include "../process/child-process.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#if OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

using Json = nlohmann::json;


static const std::vector<PermissionOption>& claude_permission_modes() {
    static const std::vector<PermissionOption> modes = {
        {"plan", "Plan", "Explore and plan without making changes."},
        {"default", "Ask", "Deny any tool use that would ask for approval."},
        {"acceptEdits", "Accept edits", "Allow file edits within the working directory."},
        {"auto", "Auto", "Let Claude Code decide which actions are safe to run."},
        {"bypassPermissions", "Full access", "Allow every tool without asking."},
    };
    return modes;
}

static const std::vector<ReasoningOption>& claude_reasoning_efforts() {
    static const std::vector<ReasoningOption> efforts = {
        {"low", "Fastest responses with minimal thinking."},
        {"medium", "Balanced speed and reasoning depth."},
        {"high", "Deeper reasoning for complex tasks."},
        {"xhigh", "Extended reasoning for hard problems."},
        {"max", "Maximum reasoning depth."},
    };
    return efforts;
}

static std::vector<ModelOption> claude_models(const std::string& default_model) {
    std::vector<ModelOption> models;
    const std::pair<const char*, const char*> aliases[] = {
        {"default", "Default"}, {"fable", "Fable"}, {"opus", "Opus"},
        {"sonnet", "Sonnet"},   {"haiku", "Haiku"},
    };
    for (const auto& [id, name] : aliases)
        models.push_back({id, name, {}, claude_reasoning_efforts(), "acceptEdits",
                          claude_permission_modes()});
    const bool listed = std::any_of(models.begin(), models.end(), [&default_model](
        const ModelOption& model) { return model.id == default_model; });
    if (!listed && !default_model.empty())
        models.insert(models.begin(), {default_model, default_model, {},
                                       claude_reasoning_efforts(), "acceptEdits",
                                       claude_permission_modes()});
    return models;
}

static std::string string_value(const Json& value, const char* key) {
    return value.is_object() && value.contains(key) && value[key].is_string()
        ? value[key].get<std::string>() : std::string{};
}

static std::string path_utf8(const std::filesystem::path& path) {
    const std::u8string value = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

static std::string claude_json_text(const Json& value) {
    if (value.is_string())
        return value.get<std::string>();
    if (value.is_null())
        return {};
    return value.dump(2);
}

static std::string base64_encode(const std::vector<std::uint8_t>& content) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve((content.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < content.size(); i += 3) {
        const std::uint32_t a = content[i];
        const std::uint32_t b = i + 1 < content.size() ? content[i + 1] : 0;
        const std::uint32_t c = i + 2 < content.size() ? content[i + 2] : 0;
        const std::uint32_t value = (a << 16) | (b << 8) | c;
        encoded.push_back(alphabet[(value >> 18) & 63]);
        encoded.push_back(alphabet[(value >> 12) & 63]);
        encoded.push_back(i + 1 < content.size() ? alphabet[(value >> 6) & 63] : '=');
        encoded.push_back(i + 2 < content.size() ? alphabet[value & 63] : '=');
    }
    return encoded;
}

struct ClaudeCodeState {
    ProviderRuntime runtime;
    ClaudeCodeOptions options;
    ProviderProcesses processes;
    ProviderStartupState startup;
    ProviderUsageState usage;
    ProviderSkillsState skills;
    // Protected by usage.mutex. Keyed by the rate limit window name.
    std::map<std::string, UsageMetric> usage_windows;
    // Protected by skills.mutex. Slash commands reported by the last turn per directory.
    std::map<std::string, std::vector<std::string>> session_commands;
};

struct ClaudeStreamContext {
    ClaudeCodeState* state;
    ProviderRuntime* runtime;
    const TurnRequest* request;
    bool partial_messages = false;
    bool text_emitted = false;
    bool last_was_text = false;
    bool result_received = false;
    bool result_failed = false;
    std::string error{};
    std::map<std::string, bool> terminal_tools{};
    std::vector<std::string> slash_commands{};
    bool slash_commands_received = false;
};

static std::filesystem::path home_directory() {
#if OS_WIN
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    return home != nullptr ? std::filesystem::path(home) : std::filesystem::path{};
}

static std::filesystem::path claude_config_directory(const ClaudeCodeOptions& options) {
    if (!options.config_directory.empty())
        return options.config_directory;
    const char* configured = std::getenv("CLAUDE_CONFIG_DIR");
    if (configured != nullptr && configured[0] != '\0')
        return std::filesystem::path(configured);
    const std::filesystem::path home = home_directory();
    return home.empty() ? std::filesystem::path{} : home / ".claude";
}

// The global Claude Code state file holds user- and local-scoped MCP servers.
static std::filesystem::path claude_state_file(const ClaudeCodeOptions& options) {
    if (!options.config_directory.empty())
        return options.config_directory / ".claude.json";
    const char* configured = std::getenv("CLAUDE_CONFIG_DIR");
    if (configured != nullptr && configured[0] != '\0')
        return std::filesystem::path(configured) / ".claude.json";
    const std::filesystem::path home = home_directory();
    return home.empty() ? std::filesystem::path{} : home / ".claude.json";
}

static bool read_json_file(const std::filesystem::path& path, Json* value) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    std::ostringstream contents;
    contents << file.rdbuf();
    try {
        *value = Json::parse(contents.str());
    } catch (...) {
        return false;
    }
    return true;
}

static bool has_unsafe_argument(const std::string& value) {
    return value.find_first_of("\"\\%!&|<>^\r\n") != std::string::npos;
}

static Result start_claude_process(const ClaudeCodeOptions* options, const TurnRequest* request,
                                   ChildProcess* process,
                                   const std::filesystem::path& error_output_path) {
    const std::string model = request->model.empty() ? options->default_model : request->model;
    if (has_unsafe_argument(model) || has_unsafe_argument(request->reasoning_effort) ||
        has_unsafe_argument(request->permission_mode))
        return result_error("Invalid Claude Code model, effort or permission identifier");

    const std::filesystem::path executable =
        child_process_resolve_executable(options->executable);
    if (executable.empty())
        return result_error("Claude Code executable was not found");

    std::vector<std::string> arguments = {
        "-p", "--input-format", "stream-json", "--output-format", "stream-json",
        "--verbose", "--include-partial-messages", "--no-session-persistence",
    };
    if (!model.empty() && model != "default") {
        arguments.push_back("--model");
        arguments.push_back(model);
    }
    if (!request->reasoning_effort.empty()) {
        arguments.push_back("--effort");
        arguments.push_back(request->reasoning_effort);
    }
    if (!request->permission_mode.empty()) {
        arguments.push_back("--permission-mode");
        arguments.push_back(request->permission_mode);
    }
    std::vector<ChildProcessEnvironmentVariable> environment;
    if (!options->config_directory.empty())
        environment.push_back({"CLAUDE_CONFIG_DIR", options->config_directory});
    return child_process_start(process, executable, arguments, "Claude Code", environment,
                               error_output_path, request->working_directory);
}

static bool claude_write_message(FILE* input, const Json& message) {
    const std::string serialized = message.dump();
    return fputs(serialized.c_str(), input) >= 0 && fputc('\n', input) != EOF &&
           fflush(input) == 0;
}

// Returns false at end of output. Lines that are not JSON are skipped.
static bool claude_read_message(FILE* output, Json* message) {
    for (;;) {
        std::string line;
        int character = 0;
        while ((character = fgetc(output)) != EOF && character != '\n')
            line.push_back(static_cast<char>(character));
        if (character == EOF && line.empty())
            return false;
        try {
            *message = Json::parse(line);
            return true;
        } catch (...) {
            if (character == EOF)
                return false;
        }
    }
}

static std::string claude_conversation_prompt(const TurnRequest* request) {
    if (request->history.size() <= 1)
        return request->prompt;
    std::string prompt;
    for (const ProviderHistoryMessage& message : request->history) {
        prompt += message.role == ChatMessageRole::User ? "User: " : "Assistant: ";
        prompt += message.content;
        prompt += "\n\n";
    }
    prompt += "Continue the conversation and answer the latest user message.";
    return prompt;
}

static Json claude_prompt_content(const TurnRequest* request) {
    Json content = Json::array();
    content.push_back({{"type", "text"}, {"text", claude_conversation_prompt(request)}});
    for (const FileReference& reference : request->file_references) {
        content.push_back({
            {"type", "text"},
            {"text", "The user referenced this workspace-relative file: " +
                         reference.path.generic_string() + ". Read it if relevant."},
        });
    }
    for (const FileAttachment& attachment : request->attachments) {
        if (attachment.media_type == "image/png" || attachment.media_type == "image/jpeg" ||
            attachment.media_type == "image/gif" || attachment.media_type == "image/webp") {
            content.push_back({{"type", "text"}, {"text", "Attached image: " + attachment.filename}});
            content.push_back({{"type", "image"},
                               {"source", {{"type", "base64"},
                                           {"media_type", attachment.media_type},
                                           {"data", base64_encode(attachment.content)}}}});
        } else if (attachment.media_type == "application/pdf") {
            content.push_back({{"type", "document"},
                               {"title", attachment.filename},
                               {"source", {{"type", "base64"},
                                           {"media_type", attachment.media_type},
                                           {"data", base64_encode(attachment.content)}}}});
        } else if (attachment.media_type.rfind("text/", 0) == 0 ||
                   attachment.media_type == "application/json") {
            content.push_back({{"type", "text"},
                {"text", "Attached file " + attachment.filename + ":\n" +
                    std::string(attachment.content.begin(), attachment.content.end())}});
        } else {
            content.push_back({{"type", "text"},
                {"text", "The user attached " + attachment.filename + " at " +
                    attachment.path.string() + ". Read this file from disk if relevant."}});
        }
    }
    return content;
}

static void emit_text(ClaudeStreamContext* context, EventKind kind, std::string text) {
    if (text.empty())
        return;
    const Event event{kind, context->request->conversation_id, context->request->turn_id,
                      std::move(text), {}, {}};
    provider_runtime_emit(context->runtime, &event);
}

static void emit_assistant_text(ClaudeStreamContext* context, std::string text) {
    if (text.empty())
        return;
    if (context->text_emitted && !context->last_was_text)
        text.insert(0, "\n\n");
    context->text_emitted = true;
    context->last_was_text = true;
    emit_text(context, EventKind::AssistantTextDelta, std::move(text));
}

static std::string tool_result_text(const Json& content) {
    if (!content.is_array())
        return claude_json_text(content);
    std::string result;
    for (const Json& item : content) {
        if (!result.empty())
            result += '\n';
        if (string_value(item, "type") == "text")
            result += string_value(item, "text");
        else if (string_value(item, "type") == "image")
            result += "[image]";
        else
            result += claude_json_text(item);
    }
    return result;
}

static void emit_tool_started(ClaudeStreamContext* context, const Json& block) {
    Event event{EventKind::ToolActivity, context->request->conversation_id,
                context->request->turn_id};
    event.item_id = string_value(block, "id");
    event.tool_name = string_value(block, "name");
    event.status = "inProgress";
    const Json input = block.value("input", Json::object());
    const std::string command = string_value(input, "command");
    if ((event.tool_name == "Bash" || event.tool_name == "PowerShell") && !command.empty()) {
        event.text = command;
        event.is_terminal = true;
    } else {
        event.text = string_value(input, "description");
        event.tool_arguments = claude_json_text(input);
    }
    context->terminal_tools[event.item_id] = event.is_terminal;
    context->last_was_text = false;
    provider_runtime_emit(context->runtime, &event);
}

static void emit_tool_completed(ClaudeStreamContext* context, const Json& block) {
    Event event{EventKind::ToolActivity, context->request->conversation_id,
                context->request->turn_id};
    event.item_id = string_value(block, "tool_use_id");
    const bool failed = block.value("is_error", false);
    event.status = failed ? "failed" : "completed";
    event.tool_completed = true;
    if (block.contains("content"))
        event.output = tool_result_text(block["content"]);
    const auto terminal = context->terminal_tools.find(event.item_id);
    if (terminal != context->terminal_tools.end() && terminal->second) {
        event.is_terminal = true;
        event.exit_code = failed ? 1 : 0;
        constexpr std::string_view prefix = "Exit code ";
        if (failed && event.output.rfind(prefix, 0) == 0) {
            const std::size_t end = event.output.find_first_not_of("0123456789", prefix.size());
            const std::string digits = event.output.substr(prefix.size(),
                end == std::string::npos ? std::string::npos : end - prefix.size());
            if (!digits.empty() && digits.size() < 6)
                event.exit_code = std::stoi(digits);
        }
    }
    context->last_was_text = false;
    provider_runtime_emit(context->runtime, &event);
}

static std::string usage_period(const std::string& window) {
    if (window == "five_hour")
        return "5 hours";
    if (window == "seven_day")
        return "7 days";
    std::string period = window;
    std::replace(period.begin(), period.end(), '_', ' ');
    return period;
}

static void update_claude_usage(ClaudeCodeState* state, const Json& info) {
    std::map<std::string, UsageMetric> windows;
    const Json unified = info.value("unifiedWindows", Json::object());
    if (unified.is_object()) {
        for (auto it = unified.begin(); it != unified.end(); ++it) {
            const Json& window = it.value();
            if (!window.is_object() || !window.contains("utilization") ||
                !window["utilization"].is_number())
                continue;
            const double used = std::clamp(window["utilization"].get<double>() * 100.0,
                                           0.0, 100.0);
            UsageMetric metric;
            metric.name = it.key() == "five_hour" ? "Session limit"
                        : it.key() == "seven_day" ? "Weekly limit"
                                                  : usage_period(it.key()) + " limit";
            metric.period = usage_period(it.key());
            metric.value = std::to_string(static_cast<int>(std::lround(100.0 - used))) +
                           "% remaining";
            metric.used = used;
            metric.limit = 100.0;
            metric.remaining = 100.0 - used;
            if (window.contains("resetsAt") && window["resetsAt"].is_number_integer())
                metric.reset_at = usage_time::format_utc(
                    static_cast<std::time_t>(window["resetsAt"].get<std::int64_t>()));
            windows.emplace(it.key(), std::move(metric));
        }
    }
    if (windows.empty())
        return;

    std::lock_guard lock(state->usage.mutex);
    for (auto& [key, metric] : windows)
        state->usage_windows[key] = std::move(metric);
    UsageSnapshot snapshot;
    for (const char* key : {"five_hour", "seven_day"}) {
        const auto window = state->usage_windows.find(key);
        if (window != state->usage_windows.end())
            snapshot.metrics.push_back(window->second);
    }
    for (const auto& [key, metric] : state->usage_windows)
        if (key != "five_hour" && key != "seven_day")
            snapshot.metrics.push_back(metric);
    snapshot.updated_at = usage_time::format_utc(std::time(nullptr));
    state->usage.snapshot = std::move(snapshot);
    state->usage.updated = true;
}

static void handle_claude_message(ClaudeStreamContext* context, const Json& message) {
    if (!message.is_object())
        return;
    const std::string type = string_value(message, "type");
    const bool top_level = !message.contains("parent_tool_use_id") ||
                           message["parent_tool_use_id"].is_null();
    if (type == "system") {
        if (string_value(message, "subtype") != "init")
            return;
        const Json commands = message.value("slash_commands", Json::array());
        if (!commands.is_array())
            return;
        context->slash_commands_received = true;
        context->slash_commands.clear();
        for (const Json& command : commands)
            if (command.is_string() && !command.get<std::string>().empty())
                context->slash_commands.push_back(command.get<std::string>());
    } else if (type == "stream_event") {
        if (!top_level)
            return;
        const Json event = message.value("event", Json::object());
        const std::string event_type = string_value(event, "type");
        if (event_type != "content_block_delta")
            return;
        context->partial_messages = true;
        const Json delta = event.value("delta", Json::object());
        const std::string delta_type = string_value(delta, "type");
        if (delta_type == "text_delta")
            emit_assistant_text(context, string_value(delta, "text"));
        else if (delta_type == "thinking_delta")
            emit_text(context, EventKind::AssistantReasoningDelta,
                      string_value(delta, "thinking"));
    } else if (type == "assistant") {
        const Json content = message.value("message", Json::object())
                                 .value("content", Json::array());
        if (!content.is_array())
            return;
        for (const Json& block : content) {
            const std::string block_type = string_value(block, "type");
            if (block_type == "tool_use" || block_type == "server_tool_use") {
                emit_tool_started(context, block);
            } else if (top_level && !context->partial_messages) {
                // Older CLIs without partial messages only report complete blocks.
                if (block_type == "text")
                    emit_assistant_text(context, string_value(block, "text"));
                else if (block_type == "thinking")
                    emit_text(context, EventKind::AssistantReasoningDelta,
                              string_value(block, "thinking"));
            }
        }
    } else if (type == "user") {
        const Json content = message.value("message", Json::object())
                                 .value("content", Json::array());
        if (!content.is_array())
            return;
        for (const Json& block : content)
            if (string_value(block, "type") == "tool_result")
                emit_tool_completed(context, block);
    } else if (type == "rate_limit_event") {
        if (context->state != nullptr)
            update_claude_usage(context->state, message.value("rate_limit_info", Json::object()));
    } else if (type == "result") {
        context->result_received = true;
        context->result_failed = string_value(message, "subtype") != "success" ||
                                 message.value("is_error", false);
        if (!context->result_failed)
            return;
        context->error = string_value(message, "result");
        const Json errors = message.value("errors", Json::array());
        if (context->error.empty() && errors.is_array()) {
            for (const Json& error : errors) {
                if (!context->error.empty())
                    context->error += '\n';
                context->error += claude_json_text(error);
            }
        }
        if (context->error.empty())
            context->error = "Claude Code turn failed: " + string_value(message, "subtype");
    }
}

static std::string read_error_tail(const std::filesystem::path& path) {
    if (path.empty())
        return {};
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    std::ostringstream contents;
    contents << file.rdbuf();
    std::string text = contents.str();
    constexpr std::size_t limit = 2000;
    if (text.size() > limit)
        text = text.substr(text.size() - limit);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.pop_back();
    return text;
}

static Result run_claude_code(ClaudeCodeState* state, const TurnRequest* request,
                              ProviderRuntime* runtime);

static void publish_claude_skills(ClaudeCodeState* state,
                                  const std::filesystem::path& working_directory,
                                  const std::vector<std::string>& commands);

static void process_claude_code(void* context, const TurnRequest* request,
                                ProviderRuntime* runtime) {
    ClaudeCodeState* state = static_cast<ClaudeCodeState*>(context);
    Result result = state->options.execute != nullptr
                        ? state->options.execute(
                              state->options.execute_context, request,
                              [](void* emit_context, const Event* event) {
                                  provider_runtime_emit(static_cast<ProviderRuntime*>(emit_context),
                                                        event);
                              },
                              runtime)
                        : run_claude_code(state, request, runtime);
    bool cancelled = false;
    {
        std::lock_guard lock(state->processes.mutex);
        cancelled = state->processes.active_turns.at(request->turn_id).cancelled;
        state->processes.active_turns.erase(request->turn_id);
    }
    if (cancelled)
        return;
    if (result.status == ResultStatus::Error) {
        const Event failed{EventKind::TurnFailed, request->conversation_id, request->turn_id,
                           std::string(result.error), {}, {}};
        provider_runtime_emit(runtime, &failed);
        return;
    }

    const Event completed{EventKind::TurnCompleted, request->conversation_id, request->turn_id,
                          {}, {}, {}};
    provider_runtime_emit(runtime, &completed);
}

static Result run_claude_code(ClaudeCodeState* state, const TurnRequest* request,
                              ProviderRuntime* runtime) {
    child_process_ignore_sigpipe();

    std::error_code path_error;
    std::filesystem::path stderr_path = std::filesystem::temp_directory_path(path_error);
    if (path_error)
        stderr_path.clear();
    else
        stderr_path /= "zenith-claude-code-" + std::to_string(request->turn_id) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
            ".stderr";

    ChildProcess process;
    Result result = start_claude_process(&state->options, request, &process, stderr_path);
    if (result.status == ResultStatus::Error) {
        child_process_stop(&process);
        std::error_code remove_error;
        if (!stderr_path.empty())
            std::filesystem::remove(stderr_path, remove_error);
        return result;
    }
#if OS_WIN
    _setmode(_fileno(process.input), _O_BINARY);
    _setmode(_fileno(process.output), _O_BINARY);
#endif
    {
        std::lock_guard lock(state->processes.mutex);
        auto& active = state->processes.active_turns.at(request->turn_id);
        active.process = &process;
        if (active.cancelled || state->processes.shutting_down)
            child_process_terminate(&process);
    }

    ClaudeStreamContext context{state, runtime, request};
    const Json input = {
        {"type", "user"},
        {"message", {{"role", "user"}, {"content", claude_prompt_content(request)}}},
    };
    bool success = claude_write_message(process.input, input);
    std::string error = success ? std::string{} : "Failed to write the Claude Code prompt";
    // Closing input ends the session once the turn's result has been reported.
    fclose(process.input);
    process.input = nullptr;

    while (success) {
        Json message;
        if (!claude_read_message(process.output, &message))
            break;
        handle_claude_message(&context, message);
    }
    if (success && !context.result_received) {
        success = false;
        error = read_error_tail(stderr_path);
        if (error.empty())
            error = "Claude Code exited before the turn completed";
    }

    {
        std::lock_guard lock(state->processes.mutex);
        state->processes.active_turns.at(request->turn_id).process = nullptr;
    }
    child_process_stop(&process);
    std::error_code remove_error;
    if (!stderr_path.empty())
        std::filesystem::remove(stderr_path, remove_error);
    if (context.slash_commands_received)
        publish_claude_skills(state, request->working_directory, context.slash_commands);
    if (!success)
        return result_error(error);
    if (context.result_failed)
        return result_error(context.error);
    return result_ok();
}

static std::string trim(std::string value) {
    const std::size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
        return {};
    const std::size_t end = value.find_last_not_of(" \t\r\n");
    value = value.substr(start, end - start + 1);
    if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
        value.back() == value.front())
        value = value.substr(1, value.size() - 2);
    return value;
}

static std::map<std::string, std::string> read_frontmatter(const std::filesystem::path& path) {
    std::map<std::string, std::string> fields;
    std::ifstream file(path, std::ios::binary);
    std::string line;
    if (!file || !std::getline(file, line) || trim(line) != "---")
        return fields;
    while (std::getline(file, line)) {
        if (trim(line) == "---")
            break;
        if (line.empty() || std::isspace(static_cast<unsigned char>(line.front())))
            continue;
        const std::size_t separator = line.find(':');
        if (separator == std::string::npos)
            continue;
        fields[trim(line.substr(0, separator))] = trim(line.substr(separator + 1));
    }
    return fields;
}

static void append_skill_directory(const std::filesystem::path& directory, const char* scope,
                                   std::vector<SkillEntry>* entries) {
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error))
        return;
    for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end;
         it.increment(error)) {
        const std::filesystem::path skill_file = it->path() / "SKILL.md";
        std::error_code file_error;
        if (!std::filesystem::is_regular_file(skill_file, file_error))
            continue;
        std::map<std::string, std::string> fields = read_frontmatter(skill_file);
        if (fields["user-invocable"] == "false")
            continue;
        SkillEntry entry;
        entry.name = path_utf8(it->path().filename());
        entry.description = fields["description"];
        entry.input_hint = fields["argument-hint"];
        entry.path = skill_file;
        entry.scope = scope;
        entry.invocation = "/" + entry.name;
        entries->push_back(std::move(entry));
    }
}

static void append_command_directory(const std::filesystem::path& directory, const char* scope,
                                     std::vector<SkillEntry>* entries) {
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error))
        return;
    for (std::filesystem::recursive_directory_iterator it(directory, error), end;
         !error && it != end; it.increment(error)) {
        std::error_code file_error;
        if (it->path().extension() != ".md" || !it->is_regular_file(file_error))
            continue;
        std::filesystem::path relative = it->path().lexically_relative(directory);
        relative.replace_extension();
        std::string name = path_utf8(relative);
        std::replace(name.begin(), name.end(), '/', ':');
        std::map<std::string, std::string> fields = read_frontmatter(it->path());
        SkillEntry entry;
        entry.name = std::move(name);
        entry.description = fields["description"];
        entry.input_hint = fields["argument-hint"];
        entry.path = it->path();
        entry.scope = scope;
        entry.invocation = "/" + entry.name;
        entry.kind = SkillEntryKind::CommandOrSkill;
        entries->push_back(std::move(entry));
    }
}

static SkillDiscoverySnapshot discover_claude_skills(
    ClaudeCodeState* state, const std::filesystem::path& working_directory,
    const std::vector<std::string>& session_commands) {
    SkillDiscoverySnapshot snapshot;
    snapshot.working_directory = working_directory;
    const std::filesystem::path config = claude_config_directory(state->options);
    if (!working_directory.empty()) {
        append_skill_directory(working_directory / ".claude" / "skills", "project",
                               &snapshot.entries);
        append_command_directory(working_directory / ".claude" / "commands", "project",
                                 &snapshot.entries);
    }
    if (!config.empty()) {
        append_skill_directory(config / "skills", "user", &snapshot.entries);
        append_command_directory(config / "commands", "user", &snapshot.entries);
    }

    // Project entries take precedence over user entries with the same name.
    std::set<std::string> names;
    snapshot.entries.erase(std::remove_if(snapshot.entries.begin(), snapshot.entries.end(),
        [&names](const SkillEntry& entry) { return !names.insert(entry.name).second; }),
        snapshot.entries.end());
    for (const std::string& command : session_commands) {
        if (!names.insert(command).second)
            continue;
        SkillEntry entry;
        entry.name = command;
        entry.invocation = "/" + command;
        entry.scope = command.find(':') != std::string::npos ? "plugin" : "builtin";
        entry.kind = SkillEntryKind::CommandOrSkill;
        snapshot.entries.push_back(std::move(entry));
    }
    std::sort(snapshot.entries.begin(), snapshot.entries.end(),
              [](const SkillEntry& left, const SkillEntry& right) {
                  return left.name < right.name;
              });
    return snapshot;
}

static void publish_claude_skills(ClaudeCodeState* state,
                                  const std::filesystem::path& working_directory,
                                  const std::vector<std::string>& commands) {
    {
        std::lock_guard lock(state->skills.mutex);
        if (state->skills.stopping)
            return;
        state->session_commands[path_utf8(working_directory)] = commands;
    }
    SkillDiscoverySnapshot snapshot = discover_claude_skills(state, working_directory, commands);
    std::lock_guard lock(state->skills.mutex);
    if (!state->skills.stopping)
        state->skills.updates.push_back(std::move(snapshot));
}

static void run_claude_skills(void* context) {
    ClaudeCodeState* state = static_cast<ClaudeCodeState*>(context);
    for (;;) {
        ProviderSkillsRequest request;
        std::vector<std::string> commands;
        {
            std::unique_lock lock(state->skills.mutex);
            state->skills.ready.wait(lock, [state] {
                return state->skills.stopping || !state->skills.requests.empty();
            });
            if (state->skills.stopping)
                return;
            request = std::move(state->skills.requests.front());
            state->skills.requests.pop_front();
            const auto known = state->session_commands.find(path_utf8(request.working_directory));
            if (known != state->session_commands.end())
                commands = known->second;
        }

        SkillDiscoverySnapshot snapshot =
            discover_claude_skills(state, request.working_directory, commands);
        std::lock_guard lock(state->skills.mutex);
        state->skills.updates.push_back(std::move(snapshot));
    }
}

static Result request_claude_skills(Provider* provider,
                                    const std::filesystem::path& working_directory,
                                    bool force_reload) {
    ClaudeCodeState* state = static_cast<ClaudeCodeState*>(provider->state);
    {
        std::lock_guard lock(state->skills.mutex);
        if (state->skills.stopping)
            return result_error("Claude Code skill discovery is stopping");
        state->skills.requests.push_back({working_directory, force_reload});
    }
    state->skills.ready.notify_one();
    return result_ok();
}

static std::vector<SkillDiscoverySnapshot> poll_claude_skills(Provider* provider) {
    ClaudeCodeState* state = static_cast<ClaudeCodeState*>(provider->state);
    std::lock_guard lock(state->skills.mutex);
    std::vector<SkillDiscoverySnapshot> updates;
    updates.swap(state->skills.updates);
    return updates;
}

// Usage limits are only reported while a turn runs, so a request republishes the latest report.
static void request_claude_usage(Provider* provider) {
    ClaudeCodeState* state = static_cast<ClaudeCodeState*>(provider->state);
    std::lock_guard lock(state->usage.mutex);
    if (!state->usage.stopping)
        state->usage.updated = true;
}

static std::optional<UsageSnapshot> poll_claude_usage(Provider* provider) {
    ClaudeCodeState* state = static_cast<ClaudeCodeState*>(provider->state);
    std::lock_guard lock(state->usage.mutex);
    if (!state->usage.updated)
        return std::nullopt;
    state->usage.updated = false;
    return state->usage.snapshot;
}

static Result run_claude_mcp_cli(ClaudeCodeState* state,
                                 const std::filesystem::path& working_directory,
                                 const std::vector<std::string>& arguments,
                                 std::string* output) {
    return provider_mcp_run_cli(state->options.executable, arguments, working_directory,
                                output, &state->processes.mutex, &state->processes.mcp_process,
                                &state->processes.shutting_down);
}

static McpServer parse_claude_mcp_server(const std::string& name, const Json& config,
                                         const char* source) {
    McpServer server;
    server.name = name;
    server.source = source;
    std::string type = string_value(config, "type");
    if (type.empty() && config.contains("command"))
        type = "stdio";
    if (type == "stdio") {
        server.transport = McpServerTransport::Stdio;
        server.command = string_value(config, "command");
        const Json arguments = config.value("args", Json::array());
        if (arguments.is_array())
            for (const Json& argument : arguments)
                if (argument.is_string())
                    server.arguments.push_back(argument.get<std::string>());
        const Json environment = config.value("env", Json::object());
        if (environment.is_object())
            for (auto it = environment.begin(); it != environment.end(); ++it)
                if (it.value().is_string())
                    server.environment.emplace(it.key(), it.value().get<std::string>());
    } else if (type == "http" || type == "sse" || type == "ws") {
        server.transport = McpServerTransport::Http;
        server.url = string_value(config, "url");
        const Json headers = config.value("headers", Json::object());
        if (headers.is_object())
            for (auto it = headers.begin(); it != headers.end(); ++it)
                if (it.value().is_string())
                    server.headers.emplace(it.key(), it.value().get<std::string>());
        if (type != "http")
            server.editable = false;
    } else {
        server.editable = false;
    }
    for (const char* key : {"headersHelper", "oauth", "timeout", "cwd"})
        if (config.contains(key) && !config[key].is_null())
            server.editable = false;
    server.status = "Checking";
    return server;
}

static void append_claude_mcp_servers(const Json& servers, const char* source,
                                      std::vector<McpServer>* result) {
    if (!servers.is_object())
        return;
    for (auto it = servers.begin(); it != servers.end(); ++it) {
        if (!it.value().is_object())
            continue;
        const auto existing = std::find_if(result->begin(), result->end(),
            [&it](const McpServer& server) { return server.name == it.key(); });
        // Local servers override user servers, and both override project servers.
        if (existing != result->end())
            continue;
        result->push_back(parse_claude_mcp_server(it.key(), it.value(), source));
    }
}

static const Json* claude_project_state(const Json& state,
                                        const std::filesystem::path& working_directory) {
    const Json& projects = state.contains("projects") ? state["projects"] : Json();
    if (!projects.is_object() || working_directory.empty())
        return nullptr;
    for (const std::string& key : {path_utf8(working_directory),
                                   working_directory.string()}) {
        const auto project = projects.find(key);
        if (project != projects.end() && project->is_object())
            return &*project;
    }
    return nullptr;
}

static Result read_claude_mcp_config(ClaudeCodeState* state,
                                     const std::filesystem::path& working_directory,
                                     std::vector<McpServer>* servers) {
    servers->clear();
    Json global_state;
    const std::filesystem::path state_file = claude_state_file(state->options);
    std::error_code error;
    if (!state_file.empty() && std::filesystem::exists(state_file, error) &&
        !read_json_file(state_file, &global_state))
        return result_error("Claude Code returned invalid MCP server data");

    const Json* project = claude_project_state(global_state, working_directory);
    if (project != nullptr)
        append_claude_mcp_servers(project->value("mcpServers", Json::object()), "local", servers);
    append_claude_mcp_servers(global_state.value("mcpServers", Json::object()), "user", servers);

    Json project_file;
    if (!working_directory.empty() && read_json_file(working_directory / ".mcp.json", &project_file)) {
        const std::size_t first_project = servers->size();
        append_claude_mcp_servers(project_file.value("mcpServers", Json::object()), "project",
                                  servers);
        for (std::size_t index = first_project; index < servers->size(); ++index) {
            McpServer& server = (*servers)[index];
            server.editable = false;
            server.removable = false;
            server.status_detail = "Edit this server in .mcp.json.";
        }
    }

    std::set<std::string> disabled;
    if (project != nullptr) {
        for (const char* key : {"disabledMcpServers", "disabledMcpjsonServers"}) {
            const Json names = project->value(key, Json::array());
            if (names.is_array())
                for (const Json& name : names)
                    if (name.is_string())
                        disabled.insert(name.get<std::string>());
        }
    }
    for (McpServer& server : *servers) {
        if (server.editable == false && server.status_detail.empty() && server.removable)
            server.status_detail = "Edit this server in the Claude Code configuration.";
        if (disabled.count(server.name) != 0) {
            server.enabled = false;
            server.editable = false;
            server.status = "Disabled";
        }
    }
    return result_ok();
}

static std::string claude_mcp_status_text(std::string status) {
    std::string lower = status;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (lower.find("connected") != std::string::npos && lower.find("failed") == std::string::npos)
        return "Connected";
    if (lower.find("auth") != std::string::npos)
        return "Authentication required";
    if (lower.find("failed") != std::string::npos)
        return "Failed";
    if (lower.find("pending") != std::string::npos)
        return "Pending approval";
    if (lower.find("disabled") != std::string::npos)
        return "Disabled";
    return status.empty() ? "Unknown" : status;
}

// Parses `claude mcp list` lines of the form "name: target - <glyph> Status — detail".
static void parse_claude_mcp_list(const std::string& output,
                                  std::map<std::string, std::pair<std::string, std::string>>* statuses) {
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const std::size_t name_end = line.find(": ");
        if (name_end == std::string::npos || name_end == 0)
            continue;
        std::size_t separator = line.find(" - ", name_end);
        while (separator != std::string::npos) {
            const std::size_t glyph = separator + 3;
            if (glyph < line.size() && !std::isalnum(static_cast<unsigned char>(line[glyph])))
                break;
            separator = line.find(" - ", glyph);
        }
        if (separator == std::string::npos)
            continue;
        std::string status = line.substr(separator + 3);
        const std::size_t text_start = status.find(' ');
        status = text_start == std::string::npos ? std::string{} : status.substr(text_start + 1);
        std::string detail;
        const std::size_t dash = status.find(" \xE2\x80\x94 ");
        if (dash != std::string::npos) {
            detail = status.substr(dash + 5);
            status = status.substr(0, dash);
        }
        statuses->emplace(line.substr(0, name_end),
                          std::make_pair(claude_mcp_status_text(trim(status)), trim(detail)));
    }
}

static Result list_claude_mcp_servers(Provider* provider,
                                      const std::filesystem::path& working_directory,
                                      std::vector<McpServer>* servers) {
    ClaudeCodeState* state = static_cast<ClaudeCodeState*>(provider->state);
    Result result = read_claude_mcp_config(state, working_directory, servers);
    if (result.status == ResultStatus::Error)
        return result;

    std::string output;
    Result status_result = run_claude_mcp_cli(state, working_directory, {"mcp", "list"}, &output);
    std::map<std::string, std::pair<std::string, std::string>> statuses;
    if (status_result.status == ResultStatus::Ok)
        parse_claude_mcp_list(output, &statuses);
    for (McpServer& server : *servers) {
        if (!server.enabled)
            continue;
        const auto status = statuses.find(server.name);
        if (status_result.status == ResultStatus::Error) {
            server.status = "Unknown";
            server.status_detail = status_result.error;
        } else if (status != statuses.end()) {
            server.status = status->second.first;
            if (!status->second.second.empty())
                server.status_detail = status->second.second;
        } else {
            server.status = "Not started";
        }
    }
    // Plugin and connector servers are reported by the CLI but not stored in its config files.
    for (const auto& [name, status] : statuses) {
        const bool known = std::any_of(servers->begin(), servers->end(),
            [&name](const McpServer& server) { return server.name == name; });
        if (known)
            continue;
        McpServer server;
        server.name = name;
        server.source = "plugin";
        server.status = status.first;
        server.status_detail = status.second;
        server.editable = false;
        server.removable = false;
        servers->push_back(std::move(server));
    }
    return result_ok();
}

static std::vector<std::string> claude_add_arguments(const McpServer& server,
                                                     const std::string& scope) {
    std::vector<std::string> arguments = {"mcp", "add", "--scope", scope};
    if (server.transport == McpServerTransport::Http) {
        arguments.push_back("--transport");
        arguments.push_back("http");
        arguments.push_back(server.name);
        arguments.push_back(server.url);
        for (const auto& [key, value] : server.headers) {
            arguments.push_back("--header");
            arguments.push_back(key + ": " + value);
        }
    } else {
        arguments.push_back(server.name);
        for (const auto& [key, value] : server.environment) {
            arguments.push_back("--env");
            arguments.push_back(key + "=" + value);
        }
        arguments.push_back("--");
        arguments.push_back(server.command);
        arguments.insert(arguments.end(), server.arguments.begin(), server.arguments.end());
    }
    return arguments;
}

static Result remove_claude_mcp_server(ClaudeCodeState* state,
                                       const std::filesystem::path& working_directory,
                                       std::string_view name, const std::string& scope) {
    std::string output;
    return run_claude_mcp_cli(state, working_directory,
                              {"mcp", "remove", "--scope", scope, std::string(name)}, &output);
}

static Result upsert_claude_mcp_server(Provider* provider,
                                       const std::filesystem::path& working_directory,
                                       std::string_view existing_name, const McpServer& server) {
    ClaudeCodeState* state = static_cast<ClaudeCodeState*>(provider->state);
    if (server.name.empty())
        return result_error("Enter a server name");
    if (server.transport == McpServerTransport::Stdio && server.command.empty())
        return result_error("Enter a command for the local server");
    if (server.transport == McpServerTransport::Http && server.url.empty())
        return result_error("Enter a URL for the HTTP server");

    std::vector<McpServer> configured;
    Result listed = read_claude_mcp_config(state, working_directory, &configured);
    if (listed.status == ResultStatus::Error)
        return listed;
    const std::string old_name = existing_name.empty() ? server.name : std::string(existing_name);
    const auto old_server = std::find_if(configured.begin(), configured.end(),
        [&old_name](const McpServer& value) { return value.name == old_name; });
    if (existing_name.empty() && old_server != configured.end())
        return result_error("A Claude Code MCP server already uses that name");

    const bool replacing = old_server != configured.end();
    const McpServer old_definition = replacing ? *old_server : McpServer{};
    const std::string scope = replacing ? old_definition.source : "user";
    if (replacing) {
        if (!old_definition.editable)
            return result_error("This MCP server is managed by another configuration source");
        Result removed = remove_claude_mcp_server(state, working_directory, old_name, scope);
        if (removed.status == ResultStatus::Error)
            return removed;
    }

    std::string output;
    Result added = run_claude_mcp_cli(state, working_directory,
                                      claude_add_arguments(server, scope), &output);
    if (added.status == ResultStatus::Error && replacing) {
        std::string restore_output;
        run_claude_mcp_cli(state, working_directory, claude_add_arguments(old_definition, scope),
                           &restore_output);
    }
    return added;
}

static Result remove_claude_mcp(Provider* provider, const std::filesystem::path& working_directory,
                                std::string_view name) {
    ClaudeCodeState* state = static_cast<ClaudeCodeState*>(provider->state);
    std::vector<McpServer> configured;
    Result listed = read_claude_mcp_config(state, working_directory, &configured);
    if (listed.status == ResultStatus::Error)
        return listed;
    const auto server = std::find_if(configured.begin(), configured.end(),
        [name](const McpServer& value) { return value.name == name; });
    if (server == configured.end())
        return result_error("Claude Code has no MCP server with that name");
    if (!server->removable)
        return result_error("This MCP server is managed by another configuration source");
    return remove_claude_mcp_server(state, working_directory, name, server->source);
}

static void initialize_claude_code(void* context, ProviderRuntime*) {
    ClaudeCodeState* state = static_cast<ClaudeCodeState*>(context);
    ProviderAvailability availability = ProviderAvailability::Unavailable;
    std::filesystem::path location = child_process_resolve_executable(state->options.executable);
    if (!location.empty()) {
        std::string output;
        Result version = provider_mcp_run_cli(state->options.executable, {"--version"}, {},
                                              &output, &state->processes.mutex,
                                              &state->processes.startup_process,
                                              &state->processes.shutting_down);
        if (version.status == ResultStatus::Ok)
            availability = ProviderAvailability::Available;
    }

    {
        std::lock_guard lock(state->startup.mutex);
        state->startup.availability = availability;
        state->startup.location = std::move(location);
        state->startup.complete = true;
    }
    state->startup.ready.notify_all();
}

static Result start_claude_code(Provider* provider) {
    ClaudeCodeState* state = static_cast<ClaudeCodeState*>(provider->state);
    provider->default_model = state->options.default_model;
    provider->models = claude_models(state->options.default_model);
    if (state->options.execute == nullptr)
        provider_runtime_set_initialize(&state->runtime, initialize_claude_code);
    else
        provider->availability = ProviderAvailability::Available;
    Result result = provider_runtime_start(&state->runtime);
    if (result.status == ResultStatus::Error)
        return result;
    if (state->options.execute == nullptr) {
        provider->request_usage = request_claude_usage;
        provider->poll_usage = poll_claude_usage;
        state->skills.worker = std::thread(run_claude_skills, state);
        provider->request_skills = request_claude_skills;
        provider->poll_skills = poll_claude_skills;
        provider->list_mcp_servers = list_claude_mcp_servers;
        provider->upsert_mcp_server = upsert_claude_mcp_server;
        provider->remove_mcp_server = remove_claude_mcp;
    }
    return result_ok();
}

static Result submit_claude_code(Provider* provider, TurnRequest request) {
    ClaudeCodeState* state = static_cast<ClaudeCodeState*>(provider->state);
    if (request.turn_id == 0)
        return result_error("A turn must have a caller-assigned ID");
    const TurnId turn_id = request.turn_id;
    {
        std::lock_guard lock(state->processes.mutex);
        state->processes.active_turns.emplace(turn_id, ProviderProcesses::ActiveTurn{});
    }
    Result result = provider_runtime_submit(&state->runtime, std::move(request));
    if (result.status == ResultStatus::Error) {
        std::lock_guard lock(state->processes.mutex);
        state->processes.active_turns.erase(turn_id);
    }
    return result;
}

static Result respond_claude_code(Provider*, const ProviderRequestId&, ApprovalDecision) {
    return result_error("Claude Code has no pending approval request");
}

static void cancel_claude_code(Provider* provider, TurnId turn_id) {
    ClaudeCodeState* state = static_cast<ClaudeCodeState*>(provider->state);
    provider_processes_cancel(&state->processes, &state->runtime, turn_id);
}

static std::vector<Event> poll_claude_code(Provider* provider) {
    ClaudeCodeState* state = static_cast<ClaudeCodeState*>(provider->state);
    {
        std::lock_guard lock(state->startup.mutex);
        if (state->startup.complete && !state->startup.applied) {
            provider->availability = state->startup.availability;
            provider->location = std::move(state->startup.location);
            state->startup.applied = true;
        }
    }
    return provider_runtime_poll_events(&state->runtime);
}

static void request_claude_code_shutdown(Provider* provider) {
    ClaudeCodeState* state = static_cast<ClaudeCodeState*>(provider->state);
    provider_processes_request_shutdown(&state->processes);
    {
        std::lock_guard lock(state->usage.mutex);
        state->usage.stopping = true;
    }
    {
        std::lock_guard lock(state->skills.mutex);
        state->skills.stopping = true;
    }
    state->skills.ready.notify_all();
    provider_runtime_request_shutdown(&state->runtime);
}

static void destroy_claude_code(Provider* provider) {
    ClaudeCodeState* state = static_cast<ClaudeCodeState*>(provider->state);
    request_claude_code_shutdown(provider);
    if (state->skills.worker.joinable())
        state->skills.worker.join();
    provider_runtime_shutdown(&state->runtime);
    delete state;
    delete provider;
}


ProviderPtr make_claude_code_provider(const ClaudeCodeOptions* options) {
    ClaudeCodeState* state = new ClaudeCodeState{};
    if (options != nullptr)
        state->options = *options;
    provider_runtime_init(&state->runtime, process_claude_code, state);

    Provider* provider = new Provider{"Claude Code",       state,
                                      start_claude_code,   submit_claude_code,
                                      respond_claude_code, cancel_claude_code,
                                      poll_claude_code,    destroy_claude_code};
    provider->request_shutdown = request_claude_code_shutdown;
    return ProviderPtr(provider, destroy_provider);
}
