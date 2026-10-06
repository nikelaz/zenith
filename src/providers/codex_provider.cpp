#include "provider_runtime.h"
#include "provider_mcp_utils.h"
#include "../process/child-process.h"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

using Json = nlohmann::json;

namespace {
const std::vector<PermissionOption>& codex_permission_modes() {
    static const std::vector<PermissionOption> modes = {
        {"read-only", "Read only", "Allow reading files without making changes."},
        {"workspace-write", "Workspace", "Allow changes within the working directory."},
        {"danger-full-access", "Full access", "Allow access beyond the working directory."},
    };
    return modes;
}

std::string string_value(const Json& value, const char* key) {
    return value.contains(key) && value[key].is_string() ? value[key].get<std::string>()
                                                         : std::string{};
}

std::string path_utf8(const std::filesystem::path& path) {
    const std::u8string value = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

std::string json_text(const Json& value) {
    if (value.is_string())
        return value.get<std::string>();
    if (value.is_null())
        return {};
    return value.dump(2);
}

bool is_tool_item(const std::string& type) {
    return type == "commandExecution" || type == "fileChange" || type == "mcpToolCall" ||
           type == "dynamicToolCall" || type == "webSearch";
}

struct CodexState {
    ProviderRuntime runtime;
    CodexOptions options;
    std::mutex active_mutex;
    struct ActiveTurn {
        ChildProcess* process = nullptr;
        bool cancelled = false;
    };
    std::map<TurnId, ActiveTurn> active_turns;
    ChildProcess* startup_process = nullptr;
    bool shutting_down = false;
    std::mutex startup_mutex;
    std::condition_variable startup_ready;
    bool startup_complete = false;
    bool startup_applied = false;
    ProviderAvailability startup_availability = ProviderAvailability::Unknown;
    std::filesystem::path startup_location;
    std::vector<ModelOption> startup_models;
    std::mutex usage_mutex;
    std::condition_variable usage_ready;
    bool usage_requested = false;
    bool usage_updated = false;
    bool usage_stopping = false;
    UsageSnapshot usage_snapshot;
    std::thread usage_worker;
    ChildProcess* usage_process = nullptr;
    std::mutex skills_mutex;
    std::condition_variable skills_ready;
    struct SkillsRequest {
        std::filesystem::path working_directory;
        bool force_reload = false;
    };
    std::deque<SkillsRequest> skills_requests;
    std::vector<SkillDiscoverySnapshot> skills_updates;
    bool skills_stopping = false;
    std::thread skills_worker;
    ChildProcess* skills_process = nullptr;
};

struct StreamContext {
    ProviderRuntime* runtime;
    const TurnRequest* request;
    bool turn_finished = false;
    bool turn_failed = false;
    std::string error;
};

Event make_tool_activity_event(StreamContext* context, const Json& item, bool completed) {
    const std::string type = string_value(item, "type");
    Event event{EventKind::ToolActivity, context->request->conversation_id,
                context->request->turn_id};
    event.item_id = string_value(item, "id");
    event.status = string_value(item, "status");
    event.tool_completed = completed;
    if (event.status.empty())
        event.status = completed ? "completed" : "inProgress";

    if (type == "commandExecution") {
        event.text = string_value(item, "command");
        event.cwd = string_value(item, "cwd");
        event.is_terminal = true;
        if (completed) {
            event.output = string_value(item, "aggregatedOutput");
            if (item.contains("exitCode") && item["exitCode"].is_number_integer())
                event.exit_code = item["exitCode"].get<int>();
            if (item.contains("durationMs") && item["durationMs"].is_number_integer())
                event.duration_ms = item["durationMs"].get<int>();
        }
        return event;
    }

    if (type == "fileChange") {
        event.tool_name = "edit";
        if (item.contains("changes"))
            event.tool_arguments = json_text(item["changes"]);
    } else if (type == "webSearch") {
        event.tool_name = "web search";
        event.tool_arguments = string_value(item, "query");
    } else {
        event.tool_name = string_value(item, "tool");
        if (event.tool_name.empty())
            event.tool_name = type;
        if (item.contains("arguments"))
            event.tool_arguments = json_text(item["arguments"]);
    }

    if (completed) {
        if (item.contains("results"))
            event.output = json_text(item["results"]);
        else if (item.contains("result"))
            event.output = json_text(item["result"]);
        else if (item.contains("contentItems"))
            event.output = json_text(item["contentItems"]);
        else if (type == "fileChange") {
            event.output = string_value(item, "stdout");
            const std::string stderr_output = string_value(item, "stderr");
            if (!stderr_output.empty()) {
                if (!event.output.empty())
                    event.output += '\n';
                event.output += stderr_output;
            }
        }
        if (event.output.empty() && item.contains("error"))
            event.output = json_text(item["error"]);
        if (item.contains("durationMs") && item["durationMs"].is_number_integer())
            event.duration_ms = item["durationMs"].get<int>();
    }
    return event;
}

Result start_codex_process(const CodexOptions* options, ChildProcess* process,
                          const std::filesystem::path& working_directory = {}) {
    const std::filesystem::path executable =
        child_process_resolve_executable(options->executable);
    if (executable.empty())
        return result_error("Codex executable was not found");
    std::vector<std::string> arguments = {"app-server"};
    std::vector<ChildProcessEnvironmentVariable> environment;
    if (!options->codex_home.empty())
        environment.push_back({"CODEX_HOME", options->codex_home});
    return child_process_start(process, executable, arguments, "Codex app-server",
                               environment, {}, working_directory);
}

bool write_message(FILE* input, const Json& message) {
    const std::string serialized = message.dump();
    return fputs(serialized.c_str(), input) >= 0 && fputc('\n', input) != EOF &&
           fflush(input) == 0;
}

bool read_message(FILE* output, Json* message) {
    std::string line;
    int character = 0;
    while ((character = fgetc(output)) != EOF && character != '\n')
        line.push_back(static_cast<char>(character));
    if (character == EOF && line.empty())
        return false;

    try {
        *message = Json::parse(line);
    } catch (...) {
        return false;
    }
    return true;
}

void emit_stream_event(StreamContext* context, EventKind kind, std::string text) {
    const Event event{kind, context->request->conversation_id, context->request->turn_id,
                      std::move(text), {}, {}};
    provider_runtime_emit(context->runtime, &event);
}

void handle_server_message(StreamContext* context, const Json& message) {
    if (!message.contains("method"))
        return;

    const std::string method = string_value(message, "method");
    const Json params = message.value("params", Json::object());
    if (method == "item/agentMessage/delta") {
        emit_stream_event(context, EventKind::AssistantTextDelta, string_value(params, "delta"));
    } else if (method == "item/reasoning/summaryTextDelta") {
        emit_stream_event(context, EventKind::ReasoningSummaryDelta, string_value(params, "delta"));
    } else if (method == "item/started") {
        const Json item = params.value("item", Json::object());
        if (is_tool_item(string_value(item, "type"))) {
            const Event event = make_tool_activity_event(context, item, false);
            provider_runtime_emit(context->runtime, &event);
        }
    } else if (method == "item/completed") {
        const Json item = params.value("item", Json::object());
        if (is_tool_item(string_value(item, "type"))) {
            const Event event = make_tool_activity_event(context, item, true);
            provider_runtime_emit(context->runtime, &event);
        }
    } else if (method == "item/commandExecution/outputDelta") {
        const Event event{EventKind::ToolActivity,
                          context->request->conversation_id,
                          context->request->turn_id,
                          {}, {}, {},
                          string_value(params, "itemId"), {}, string_value(params, "delta"),
                          -1, false, true};
        provider_runtime_emit(context->runtime, &event);
    } else if (method == "turn/completed") {
        const Json turn = params.value("turn", Json::object());
        context->turn_failed = string_value(turn, "status") == "failed";
        context->turn_finished = true;
        if (context->turn_failed)
            context->error = string_value(turn.value("error", Json::object()), "message");
        if (context->turn_failed && context->error.empty())
            context->error = "Codex turn failed";
    } else if (method == "error") {
        context->turn_failed = true;
        context->error = string_value(params.value("error", Json::object()), "message");
        if (context->error.empty())
            context->error = "Codex turn failed";
    }
}

bool wait_for_response(ChildProcess* process, int request_id, StreamContext* stream,
                       Json* response, std::string* error) {
    for (;;) {
        Json message;
        if (!read_message(process->output, &message)) {
            *error = "Codex app-server closed its output";
            return false;
        }
        if (message.value("id", -1) == request_id) {
            if (message.contains("error")) {
                *error = string_value(message["error"], "message");
                if (error->empty())
                    *error = "Codex app-server request failed";
                return false;
            }
            if (response != nullptr)
                *response = std::move(message);
            return true;
        }
        handle_server_message(stream, message);
    }
}

std::string usage_reset_time(std::int64_t timestamp) {
    const std::time_t value = static_cast<std::time_t>(timestamp);
    std::tm time{};
#ifdef _WIN32
    if (gmtime_s(&time, &value) != 0)
        return {};
#else
    if (gmtime_r(&value, &time) == nullptr)
        return {};
#endif
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (time.tm_mon < 0 || time.tm_mon >= 12)
        return {};
    const int hour = time.tm_hour % 12 == 0 ? 12 : time.tm_hour % 12;
    char formatted[64];
    std::snprintf(formatted, sizeof(formatted), "%s %d, %d at %d:%02d %s UTC",
                  months[time.tm_mon], time.tm_mday, time.tm_year + 1900, hour,
                  time.tm_min, time.tm_hour < 12 ? "AM" : "PM");
    return formatted;
}

std::string usage_update_time() {
    const std::time_t value = std::time(nullptr);
    std::tm time{};
#ifdef _WIN32
    if (gmtime_s(&time, &value) != 0)
        return {};
#else
    if (gmtime_r(&value, &time) == nullptr)
        return {};
#endif
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (time.tm_mon < 0 || time.tm_mon >= 12)
        return {};
    const int hour = time.tm_hour % 12 == 0 ? 12 : time.tm_hour % 12;
    char formatted[64];
    std::snprintf(formatted, sizeof(formatted), "%s %d, %d at %d:%02d %s UTC",
                  months[time.tm_mon], time.tm_mday, time.tm_year + 1900, hour,
                  time.tm_min, time.tm_hour < 12 ? "AM" : "PM");
    return formatted;
}

UsageSnapshot parse_codex_usage(const Json& result) {
    UsageSnapshot snapshot;
    const Json rate_limits = result.value("rateLimits", Json::object());
    const std::string plan = string_value(rate_limits, "planType");
    if (!plan.empty()) {
        UsageMetric metric;
        metric.name = "Plan";
        metric.value = plan;
        snapshot.metrics.push_back(std::move(metric));
    }

    for (const char* key : {"primary", "secondary"}) {
        const Json window = rate_limits.value(key, Json::object());
        if (!window.is_object() || !window.contains("usedPercent") ||
            !window["usedPercent"].is_number())
            continue;
        const double used = window["usedPercent"].get<double>();
        std::string period = "Usage window";
        if (window.contains("windowDurationMins") && window["windowDurationMins"].is_number()) {
            const auto minutes = window["windowDurationMins"].get<std::int64_t>();
            if (minutes >= 1440 && minutes % 1440 == 0)
                period = std::to_string(minutes / 1440) + " day" +
                         (minutes / 1440 == 1 ? "" : "s");
            else if (minutes >= 60 && minutes % 60 == 0)
                period = std::to_string(minutes / 60) + " hour" +
                         (minutes / 60 == 1 ? "" : "s");
            else if (minutes > 0)
                period = std::to_string(minutes) + " minutes";
        }
        UsageMetric metric;
        metric.name = std::string(key) == "primary" ? "Primary limit" : "Secondary limit";
        metric.value = std::to_string(static_cast<int>(100.0 - used)) + "% remaining";
        metric.period = period;
        metric.used = used;
        metric.limit = 100.0;
        metric.remaining = 100.0 - used;
        if (window.contains("resetsAt") && window["resetsAt"].is_number_integer())
            metric.reset_at = usage_reset_time(window["resetsAt"].get<std::int64_t>());
        snapshot.metrics.push_back(std::move(metric));
    }

    const Json credits = rate_limits.value("credits", Json::object());
    if (credits.is_object() && credits.contains("balance")) {
        UsageMetric metric;
        metric.name = "Credits";
        metric.value = credits.value("unlimited", false) ? "Unlimited"
            : "Balance: " + json_text(credits["balance"]);
        snapshot.metrics.push_back(std::move(metric));
    }
    return snapshot;
}

bool fetch_codex_usage(CodexState* state, UsageSnapshot* snapshot) {
    ChildProcess process;
    if (start_codex_process(&state->options, &process).status == ResultStatus::Error) {
        child_process_stop(&process);
        return false;
    }
    {
        std::lock_guard lock(state->active_mutex);
        state->usage_process = &process;
        if (state->shutting_down)
            child_process_terminate(&process);
    }

    TurnRequest request;
    StreamContext stream{&state->runtime, &request, false, false, {}};
    std::string error;
    const Json initialize = {
        {"method", "initialize"}, {"id", 1},
        {"params", {{"clientInfo", {{"name", "Zenith"}, {"title", "Zenith"},
                                      {"version", "0.1.0"}}}}},
    };
    Json response;
    const bool success = write_message(process.input, initialize) &&
        wait_for_response(&process, 1, &stream, nullptr, &error) &&
        write_message(process.input, Json{{"method", "initialized"},
                                          {"params", Json::object()}}) &&
        write_message(process.input, Json{{"method", "account/rateLimits/read"}, {"id", 2}}) &&
        wait_for_response(&process, 2, &stream, &response, &error);
    if (success)
        *snapshot = parse_codex_usage(response.value("result", Json::object()));

    {
        std::lock_guard lock(state->active_mutex);
        if (state->usage_process == &process)
            state->usage_process = nullptr;
    }
    child_process_stop(&process);
    return success;
}

void run_codex_usage(void* context) {
    CodexState* state = static_cast<CodexState*>(context);
    for (;;) {
        {
            std::unique_lock lock(state->usage_mutex);
            state->usage_ready.wait(lock, [state] {
                return state->usage_stopping || state->usage_requested;
            });
            if (state->usage_stopping)
                return;
            state->usage_requested = false;
        }

        UsageSnapshot snapshot;
        if (!fetch_codex_usage(state, &snapshot))
            continue;
        std::lock_guard lock(state->usage_mutex);
        snapshot.updated_at = usage_update_time();
        state->usage_snapshot = std::move(snapshot);
        state->usage_updated = true;
    }
}

void request_codex_usage(Provider* provider) {
    CodexState* state = static_cast<CodexState*>(provider->state);
    {
        std::lock_guard lock(state->usage_mutex);
        if (state->usage_stopping)
            return;
        state->usage_requested = true;
    }
    state->usage_ready.notify_one();
}

std::optional<UsageSnapshot> poll_codex_usage(Provider* provider) {
    CodexState* state = static_cast<CodexState*>(provider->state);
    std::lock_guard lock(state->usage_mutex);
    if (!state->usage_updated)
        return std::nullopt;
    state->usage_updated = false;
    return state->usage_snapshot;
}

std::string conversation_prompt(const TurnRequest* request) {
    std::string prompt;
    for (const ChatMessage& message : request->history) {
        prompt += message.role == ChatMessageRole::User ? "User: " : "Assistant: ";
        prompt += message.content;
        prompt += "\n\n";
    }
    if (prompt.empty())
        return request->prompt;
    prompt += "Continue the conversation and answer the latest user message.";
    return prompt;
}

Json codex_prompt_content(const TurnRequest* request) {
    Json content = Json::array();
    content.push_back({{"type", "text"}, {"text", conversation_prompt(request)}});
    for (const FileReference& reference : request->file_references) {
        content.push_back({
            {"type", "text"},
            {"text", "The user referenced this workspace-relative file: " +
                         reference.path.generic_string() + ". Read it if relevant."},
        });
    }
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (const FileAttachment& attachment : request->attachments) {
        if (attachment.media_type.rfind("image/", 0) == 0) {
            std::string encoded;
            for (std::size_t i = 0; i < attachment.content.size(); i += 3) {
                const std::uint32_t a = attachment.content[i];
                const std::uint32_t b = i + 1 < attachment.content.size() ? attachment.content[i + 1] : 0;
                const std::uint32_t c = i + 2 < attachment.content.size() ? attachment.content[i + 2] : 0;
                const std::uint32_t value = (a << 16) | (b << 8) | c;
                encoded.push_back(alphabet[(value >> 18) & 63]);
                encoded.push_back(alphabet[(value >> 12) & 63]);
                encoded.push_back(i + 1 < attachment.content.size() ? alphabet[(value >> 6) & 63] : '=');
                encoded.push_back(i + 2 < attachment.content.size() ? alphabet[value & 63] : '=');
            }
            content.push_back({{"type", "text"}, {"text", "Attached image: " + attachment.filename}});
            content.push_back({{"type", "image"},
                               {"url", "data:" + attachment.media_type + ";base64," + encoded}});
        } else if (attachment.media_type.rfind("text/", 0) == 0 ||
                   attachment.media_type == "application/json") {
            content.push_back({{"type", "text"},
                {"text", "Attached file " + attachment.filename + ":\n" +
                    std::string(attachment.content.begin(), attachment.content.end())}});
        } else {
            content.push_back({{"type", "text"},
                {"text", "The user attached " + attachment.filename +
                    " at " + attachment.path.string() +
                    ". Read this document from disk if relevant."}});
        }
    }
    return content;
}

std::vector<ModelOption> fetch_codex_models(CodexState* state,
                                            ProviderRuntime* runtime,
                                            ProviderAvailability* availability) {
    child_process_ignore_sigpipe();

    std::vector<ModelOption> models;
    *availability = ProviderAvailability::Unavailable;
    ChildProcess process;
    if (start_codex_process(&state->options, &process).status == ResultStatus::Error) {
        child_process_stop(&process);
        return models;
    }
    {
        std::lock_guard lock(state->active_mutex);
        state->startup_process = &process;
        if (state->shutting_down)
            child_process_terminate(&process);
    }

    TurnRequest request;
    StreamContext stream{runtime, &request, false, false, {}};
    std::string error;
    Json initialize = {
        {"method", "initialize"},
        {"id", 1},
        {"params", {{"clientInfo", {{"name", "Zenith"}, {"title", "Zenith"},
                                      {"version", "0.1.0"}}}}},
    };
    bool success = write_message(process.input, initialize) &&
                   wait_for_response(&process, 1, &stream, nullptr, &error) &&
                   write_message(process.input,
                                 Json{{"method", "initialized"}, {"params", Json::object()}});
    if (success)
        *availability = ProviderAvailability::Available;
    std::string cursor;
    int request_id = 2;
    while (success) {
        Json params = {{"includeHidden", false}, {"limit", 100}};
        if (!cursor.empty())
            params["cursor"] = cursor;
        Json response;
        success = write_message(process.input,
                                Json{{"method", "model/list"}, {"id", request_id},
                                     {"params", params}}) &&
                  wait_for_response(&process, request_id, &stream, &response, &error);
        ++request_id;
        if (!success)
            break;

        const Json result = response.value("result", Json::object());
        const Json data = result.value("data", Json::array());
        if (!data.is_array())
            break;
        for (const Json& value : data) {
            if (!value.is_object() || value.value("hidden", false))
                continue;
            ModelOption model;
            model.id = string_value(value, "model");
            if (model.id.empty())
                model.id = string_value(value, "id");
            model.name = string_value(value, "displayName");
            if (model.name.empty())
                model.name = model.id;
            model.default_reasoning_effort = string_value(value, "defaultReasoningEffort");
            const Json efforts = value.value("supportedReasoningEfforts", Json::array());
            if (efforts.is_array()) {
                for (const Json& effort : efforts) {
                    if (!effort.is_object())
                        continue;
                    const std::string id = string_value(effort, "reasoningEffort");
                    if (!id.empty())
                        model.reasoning_efforts.push_back(
                            {id, string_value(effort, "description")});
                }
            }
            if (!model.id.empty())
                models.push_back(std::move(model));
        }
        cursor = string_value(result, "nextCursor");
        if (cursor.empty())
            break;
    }

    {
        std::lock_guard lock(state->active_mutex);
        if (state->startup_process == &process)
            state->startup_process = nullptr;
    }
    child_process_stop(&process);
    for (ModelOption& model : models)
        model.permission_modes = codex_permission_modes();
    return models;
}

SkillDiscoverySnapshot fetch_codex_skills(CodexState* state,
                                          const std::filesystem::path& working_directory,
                                          bool force_reload) {
    SkillDiscoverySnapshot snapshot;
    snapshot.working_directory = working_directory;

    ChildProcess process;
    const Result start_result = start_codex_process(&state->options, &process);
    if (start_result.status == ResultStatus::Error) {
        child_process_stop(&process);
        snapshot.error = start_result.error;
        return snapshot;
    }
    {
        std::lock_guard lock(state->active_mutex);
        state->skills_process = &process;
        if (state->shutting_down)
            child_process_terminate(&process);
    }

    TurnRequest request;
    StreamContext stream{&state->runtime, &request, false, false, {}};
    std::string error;
    const Json initialize = {
        {"method", "initialize"}, {"id", 1},
        {"params", {{"clientInfo", {{"name", "Zenith"}, {"title", "Zenith"},
                                      {"version", "0.1.0"}}}}},
    };
    Json response;
    bool success = write_message(process.input, initialize) &&
        wait_for_response(&process, 1, &stream, nullptr, &error) &&
        write_message(process.input, Json{{"method", "initialized"},
                                          {"params", Json::object()}});
    Json params = {{"cwds", Json::array({path_utf8(working_directory)})}};
    if (force_reload)
        params["forceReload"] = true;
    if (success) {
        success = write_message(process.input,
                                Json{{"method", "skills/list"}, {"id", 2},
                                     {"params", params}}) &&
                  wait_for_response(&process, 2, &stream, &response, &error);
    }

    if (success) {
        const Json data = response.value("result", Json::object())
                              .value("data", Json::array());
        if (!data.is_array()) {
            snapshot.error = "Codex returned an invalid skills/list response";
        } else {
            for (const Json& directory : data) {
                const Json skills = directory.value("skills", Json::array());
                if (skills.is_array()) {
                    for (const Json& value : skills) {
                        if (!value.is_object())
                            continue;
                        SkillEntry entry;
                        entry.name = string_value(value, "name");
                        entry.description = string_value(value, "description");
                        entry.invocation = "$" + entry.name;
                        const std::string path = string_value(value, "path");
                        if (!path.empty())
                            entry.path = std::filesystem::u8path(path);
                        entry.scope = string_value(value, "scope");
                        if (value.contains("enabled") && value["enabled"].is_boolean())
                            entry.enabled = value["enabled"].get<bool>();
                        if (!entry.name.empty())
                            snapshot.entries.push_back(std::move(entry));
                    }
                }
                const Json errors = directory.value("errors", Json::array());
                if (errors.is_array()) {
                    for (const Json& value : errors) {
                        if (!value.is_object())
                            continue;
                        const std::string path = string_value(value, "path");
                        const std::string message = string_value(value, "message");
                        snapshot.errors.push_back(path.empty() ? message : path + ": " + message);
                    }
                }
            }
        }
    } else {
        snapshot.error = error.empty() ? "Codex skill discovery failed" : error;
    }

    {
        std::lock_guard lock(state->active_mutex);
        if (state->skills_process == &process)
            state->skills_process = nullptr;
    }
    child_process_stop(&process);
    return snapshot;
}

void run_codex_skills(void* context) {
    CodexState* state = static_cast<CodexState*>(context);
    for (;;) {
        CodexState::SkillsRequest request;
        {
            std::unique_lock lock(state->skills_mutex);
            state->skills_ready.wait(lock, [state] {
                return state->skills_stopping || !state->skills_requests.empty();
            });
            if (state->skills_stopping)
                return;
            request = std::move(state->skills_requests.front());
            state->skills_requests.pop_front();
        }

        {
            std::unique_lock lock(state->startup_mutex);
            state->startup_ready.wait(lock, [state] { return state->startup_complete; });
        }
        {
            std::lock_guard lock(state->skills_mutex);
            if (state->skills_stopping)
                return;
        }

        SkillDiscoverySnapshot snapshot = fetch_codex_skills(
            state, request.working_directory, request.force_reload);
        std::lock_guard lock(state->skills_mutex);
        state->skills_updates.push_back(std::move(snapshot));
    }
}

Result request_codex_skills(Provider* provider,
                            const std::filesystem::path& working_directory,
                            bool force_reload) {
    CodexState* state = static_cast<CodexState*>(provider->state);
    {
        std::lock_guard lock(state->skills_mutex);
        if (state->skills_stopping)
            return result_error("Codex skill discovery is stopping");
        state->skills_requests.push_back({working_directory, force_reload});
    }
    state->skills_ready.notify_one();
    return result_ok();
}

std::vector<SkillDiscoverySnapshot> poll_codex_skills(Provider* provider) {
    CodexState* state = static_cast<CodexState*>(provider->state);
    std::lock_guard lock(state->skills_mutex);
    std::vector<SkillDiscoverySnapshot> updates;
    updates.swap(state->skills_updates);
    return updates;
}

std::string json_string(const Json& value, const char* key) {
    return value.contains(key) && value[key].is_string()
        ? value[key].get<std::string>() : std::string{};
}

std::string mcp_status_text(std::string status) {
    std::transform(status.begin(), status.end(), status.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    std::replace(status.begin(), status.end(), '_', ' ');
    if (status == "connected")
        return "Connected";
    if (status == "starting")
        return "Starting";
    if (status == "authenticationrequired" || status == "authentication required" ||
        status == "needs auth" || status == "notloggedin" || status == "not logged in")
        return "Authentication required";
    if (status == "failed")
        return "Failed";
    if (status == "disabled")
        return "Disabled";
    if (status == "cancelled")
        return "Cancelled";
    if (status == "notstarted" || status == "not started")
        return "Not started";
    return status.empty() ? "Unknown" : status;
}

Result run_codex_mcp_cli(CodexState* state, const std::filesystem::path& working_directory,
                         const std::vector<std::string>& arguments, std::string* output) {
    return provider_mcp_run_cli(state->options.executable, arguments, working_directory, output);
}

Result read_codex_mcp_config(CodexState* state,
                             const std::filesystem::path& working_directory,
                             std::vector<McpServer>* servers) {
    std::string output;
    Result result = run_codex_mcp_cli(state, working_directory,
                                      {"mcp", "list", "--json"}, &output);
    if (result.status == ResultStatus::Error)
        return result;

    Json entries;
    try {
        entries = Json::parse(output);
    } catch (...) {
        return result_error("Codex returned invalid MCP server data");
    }
    if (!entries.is_array())
        return result_error("Codex returned MCP server data in an unknown format");

    servers->clear();
    for (const Json& entry : entries) {
        if (!entry.is_object())
            continue;
        McpServer server;
        server.name = json_string(entry, "name");
        if (server.name.empty())
            continue;
        server.enabled = !entry.contains("enabled") || !entry["enabled"].is_boolean() ||
                         entry["enabled"].get<bool>();
        const Json transport = entry.value("transport", Json::object());
        const std::string type = json_string(transport, "type");
        if (type == "stdio") {
            server.transport = McpServerTransport::Stdio;
            server.command = json_string(transport, "command");
            if (transport.contains("args") && transport["args"].is_array()) {
                for (const Json& argument : transport["args"])
                    if (argument.is_string())
                        server.arguments.push_back(argument.get<std::string>());
            }
            if (transport.contains("env") && transport["env"].is_object()) {
                for (auto it = transport["env"].begin(); it != transport["env"].end(); ++it)
                    if (it.value().is_string())
                        server.environment.emplace(it.key(), it.value().get<std::string>());
            }
            if ((transport.contains("env_vars") && transport["env_vars"].is_array() &&
                 !transport["env_vars"].empty()) ||
                (transport.contains("cwd") && !transport["cwd"].is_null()))
                server.editable = false;
        } else if (type == "streamable_http" || type == "http") {
            server.transport = McpServerTransport::Http;
            server.url = json_string(transport, "url");
            server.bearer_token_env_var = json_string(transport, "bearer_token_env_var");
            if (transport.contains("http_headers") && transport["http_headers"].is_object()) {
                for (auto it = transport["http_headers"].begin();
                     it != transport["http_headers"].end(); ++it) {
                    if (it.value().is_string())
                        server.headers.emplace(it.key(), it.value().get<std::string>());
                }
            }
            if (transport.contains("env_http_headers") &&
                transport["env_http_headers"].is_object()) {
                for (auto it = transport["env_http_headers"].begin();
                     it != transport["env_http_headers"].end(); ++it) {
                    if (it.value().is_string())
                        server.headers.emplace(it.key(), "$" + it.value().get<std::string>());
                }
            }
            if (!server.headers.empty())
                server.editable = false;
            if (transport.contains("http_headers_helper") &&
                !transport["http_headers_helper"].is_null())
                server.editable = false;
        } else {
            server.editable = false;
            server.status_detail = "This transport is not editable in Zenith.";
        }
        if ((entry.contains("startup_timeout_sec") &&
             !entry["startup_timeout_sec"].is_null()) ||
            (entry.contains("tool_timeout_sec") && !entry["tool_timeout_sec"].is_null()) ||
            (entry.contains("oauth") && !entry["oauth"].is_null()))
            server.editable = false;
        if (!server.editable && server.status_detail.empty())
            server.status_detail = "Edit this server in the Codex configuration.";
        if (!server.enabled) {
            server.status = "Disabled";
            server.editable = false;
        } else {
            server.status = "Checking";
        }
        servers->push_back(std::move(server));
    }
    return result_ok();
}

Result query_codex_mcp_status(CodexState* state,
                              const std::filesystem::path& working_directory,
                              std::map<std::string, std::pair<std::string, std::string>>* statuses) {
    ChildProcess process;
    Result started = start_codex_process(&state->options, &process, working_directory);
    if (started.status == ResultStatus::Error) {
        child_process_stop(&process);
        return started;
    }

    std::string error;
    Json response;
    std::packaged_task<bool()> status_request([&process, &error, &response] {
        TurnRequest request;
        StreamContext stream{nullptr, &request, false, false, {}};
        return write_message(process.input, Json{
            {"method", "initialize"}, {"id", 1},
            {"params", {{"clientInfo", {{"name", "Zenith"}, {"title", "Zenith"},
                                          {"version", "0.1.0"}}}}},
        }) && wait_for_response(&process, 1, &stream, nullptr, &error) &&
            write_message(process.input,
                          Json{{"method", "initialized"}, {"params", Json::object()}}) &&
            write_message(process.input, Json{{"method", "mcpServerStatus/list"}, {"id", 2},
                                             {"params", Json::object()}}) &&
            wait_for_response(&process, 2, &stream, &response, &error);
    });
    std::future<bool> status_future = status_request.get_future();
    std::thread status_thread(std::move(status_request));
    const bool timed_out = status_future.wait_for(std::chrono::seconds(30)) !=
                           std::future_status::ready;
    if (timed_out)
        child_process_terminate(&process);
    const bool success = timed_out ? false : status_future.get();
    status_thread.join();
    child_process_stop(&process);
    if (timed_out)
        return result_error("Codex MCP status check timed out");
    if (!success)
        return result_error(error.empty() ? "Codex MCP status request failed" : error);

    const Json data = response.value("result", Json::object()).value("data", Json::array());
    if (!data.is_array())
        return result_error("Codex returned MCP status data in an unknown format");
    for (const Json& entry : data) {
        if (!entry.is_object())
            continue;
        const std::string name = json_string(entry, "name");
        if (name.empty())
            continue;
        std::string status = json_string(entry, "connectionStatus");
        if (status.empty())
            status = json_string(entry, "status");
        std::string detail = json_string(entry, "toolsError");
        if (detail.empty())
            detail = json_string(entry, "error");
        statuses->emplace(name, std::make_pair(mcp_status_text(std::move(status)),
                                               std::move(detail)));
    }
    return result_ok();
}

Result list_codex_mcp_servers(Provider* provider,
                             const std::filesystem::path& working_directory,
                             std::vector<McpServer>* servers) {
    CodexState* state = static_cast<CodexState*>(provider->state);
    Result result = read_codex_mcp_config(state, working_directory, servers);
    if (result.status == ResultStatus::Error)
        return result;

    std::map<std::string, std::pair<std::string, std::string>> statuses;
    Result status_result = query_codex_mcp_status(state, working_directory, &statuses);
    for (McpServer& server : *servers) {
        if (!server.enabled)
            continue;
        const auto status = statuses.find(server.name);
        if (status_result.status == ResultStatus::Error) {
            server.status = "Unknown";
            server.status_detail = status_result.error;
        } else if (status != statuses.end()) {
            server.status = status->second.first;
            server.status_detail = status->second.second;
        } else {
            server.status = "Not started";
        }
    }
    return result_ok();
}

std::vector<std::string> codex_add_arguments(const McpServer& server) {
    std::vector<std::string> arguments = {"mcp", "add"};
    if (server.transport == McpServerTransport::Http) {
        arguments.push_back(server.name);
        arguments.push_back("--url");
        arguments.push_back(server.url);
        if (!server.bearer_token_env_var.empty()) {
            arguments.push_back("--bearer-token-env-var");
            arguments.push_back(server.bearer_token_env_var);
        }
    } else {
        for (const auto& [key, value] : server.environment) {
            arguments.push_back("--env");
            arguments.push_back(key + "=" + value);
        }
        arguments.push_back(server.name);
        arguments.push_back("--");
        arguments.push_back(server.command);
        arguments.insert(arguments.end(), server.arguments.begin(), server.arguments.end());
    }
    return arguments;
}

Result remove_codex_mcp_server(CodexState* state,
                               const std::filesystem::path& working_directory,
                               std::string_view name) {
    std::string output;
    return run_codex_mcp_cli(state, working_directory,
                             {"mcp", "remove", std::string(name)}, &output);
}

Result upsert_codex_mcp_server(Provider* provider,
                              const std::filesystem::path& working_directory,
                              std::string_view existing_name, const McpServer& server) {
    CodexState* state = static_cast<CodexState*>(provider->state);
    if (server.name.empty())
        return result_error("Enter a server name");
    if (server.transport == McpServerTransport::Stdio && server.command.empty())
        return result_error("Enter a command for the local server");
    if (server.transport == McpServerTransport::Http && server.url.empty())
        return result_error("Enter a URL for the HTTP server");
    if (!server.headers.empty())
        return result_error("Codex CLI cannot save custom HTTP headers through its MCP commands");

    std::vector<McpServer> configured;
    Result listed = read_codex_mcp_config(state, working_directory, &configured);
    if (listed.status == ResultStatus::Error)
        return listed;
    const std::string old_name = existing_name.empty() ? server.name : std::string(existing_name);
    const auto old_server = std::find_if(configured.begin(), configured.end(),
        [&old_name](const McpServer& value) { return value.name == old_name; });
    if (existing_name.empty() && old_server != configured.end())
        return result_error("A Codex MCP server already uses that name");

    const bool replacing = old_server != configured.end();
    const McpServer old_definition = replacing ? *old_server : McpServer{};
    if (replacing) {
        Result removed = remove_codex_mcp_server(state, working_directory, old_name);
        if (removed.status == ResultStatus::Error)
            return removed;
    }

    std::string output;
    Result added = run_codex_mcp_cli(state, working_directory,
                                     codex_add_arguments(server), &output);
    if (added.status == ResultStatus::Error && replacing) {
        std::string restore_output;
        run_codex_mcp_cli(state, working_directory, codex_add_arguments(old_definition),
                          &restore_output);
    }
    return added;
}

Result remove_codex_mcp(Provider* provider,
                        const std::filesystem::path& working_directory,
                        std::string_view name) {
    return remove_codex_mcp_server(static_cast<CodexState*>(provider->state),
                                   working_directory, name);
}


void initialize_codex(void* context, ProviderRuntime* runtime) {
    CodexState* state = static_cast<CodexState*>(context);
    ProviderAvailability availability = ProviderAvailability::Unavailable;
    std::filesystem::path location;
    std::vector<ModelOption> models;
    if (state->options.execute == nullptr) {
        location = child_process_resolve_executable(state->options.executable);
        if (!location.empty()) {
            models = fetch_codex_models(state, runtime, &availability);
        }
    } else {
        availability = ProviderAvailability::Available;
    }

    {
        std::lock_guard lock(state->startup_mutex);
        state->startup_availability = availability;
        state->startup_location = std::move(location);
        state->startup_models = std::move(models);
        state->startup_complete = true;
    }
    state->startup_ready.notify_all();
}

Result run_codex(CodexState* state, const TurnRequest* request,
                 ProviderRuntime* runtime) {
    const CodexOptions* options = &state->options;
    child_process_ignore_sigpipe();

    ChildProcess process;
    Result result = start_codex_process(options, &process);
    if (result.status == ResultStatus::Error) {
        child_process_stop(&process);
        return result;
    }

    {
        std::lock_guard lock(state->active_mutex);
        auto& active = state->active_turns.at(request->turn_id);
        active.process = &process;
        if (active.cancelled || state->shutting_down)
            child_process_terminate(&process);
    }

    StreamContext stream{runtime, request, false, false, {}};
    std::string error;
    const Json initialize = {
        {"method", "initialize"},
        {"id", 1},
        {"params", {{"clientInfo", {{"name", "Zenith"}, {"title", "Zenith"}, {"version", "0.1.0"}}}}},
    };
    bool success = write_message(process.input, initialize) &&
                   wait_for_response(&process, 1, &stream, nullptr, &error) &&
                   write_message(process.input,
                                 Json{{"method", "initialized"}, {"params", Json::object()}});

    Json thread_params = {
        {"model", request->model.empty() ? options->default_model : request->model}};
    if (!request->working_directory.empty())
        thread_params["cwd"] = request->working_directory.string();
    Json thread_response;
    if (success) {
        success = write_message(process.input,
                                Json{{"method", "thread/start"}, {"id", 2}, {"params", thread_params}}) &&
                  wait_for_response(&process, 2, &stream, &thread_response, &error);
    }

    std::string thread_id;
    if (success) {
        try {
            thread_id = thread_response.at("result").at("thread").at("id").get<std::string>();
        } catch (...) {
            success = false;
            error = "Codex app-server did not return a thread id";
        }
    }

    if (success) {
        Json turn_params = {
            {"threadId", thread_id},
            {"input", codex_prompt_content(request)},
        };
        if (!request->model.empty())
            turn_params["model"] = request->model;
        if (!request->reasoning_effort.empty())
            turn_params["effort"] = request->reasoning_effort;
        if (request->permission_mode == "read-only") {
            turn_params["sandboxPolicy"] = {
                {"type", "readOnly"}, {"networkAccess", false}};
        } else if (request->permission_mode == "workspace-write") {
            Json writable_roots = Json::array();
            if (!request->working_directory.empty())
                writable_roots.push_back(request->working_directory.string());
            turn_params["sandboxPolicy"] = {
                {"type", "workspaceWrite"}, {"writableRoots", writable_roots},
                {"networkAccess", false}, {"excludeTmpdirEnvVar", false},
                {"excludeSlashTmp", false}};
        } else if (request->permission_mode == "danger-full-access") {
            turn_params["sandboxPolicy"] = {{"type", "dangerFullAccess"}};
        }
        if (!request->working_directory.empty())
            turn_params["cwd"] = request->working_directory.string();
        success = write_message(process.input,
                                Json{{"method", "turn/start"}, {"id", 3}, {"params", turn_params}}) &&
                  wait_for_response(&process, 3, &stream, nullptr, &error);
    }

    while (success && !stream.turn_finished) {
        Json message;
        if (!read_message(process.output, &message)) {
            success = false;
            error = "Codex app-server closed before the turn completed";
            break;
        }
        handle_server_message(&stream, message);
    }

    {
        std::lock_guard lock(state->active_mutex);
        state->active_turns.at(request->turn_id).process = nullptr;
    }
    child_process_stop(&process);
    if (!success)
        return result_error(error.empty() ? "Codex app-server request failed" : error);
    if (stream.turn_failed)
        return result_error(stream.error.empty() ? "Codex turn failed" : stream.error);
    return result_ok();
}

void process_codex(void* context, const TurnRequest* request, ProviderRuntime* runtime) {
    CodexState* state = static_cast<CodexState*>(context);
    Result result = state->options.execute != nullptr
                        ? state->options.execute(state->options.execute_context, request,
                                                 [](void* emit_context, const Event* event) {
                                                     provider_runtime_emit(
                                                         static_cast<ProviderRuntime*>(emit_context), event);
                                                 },
                                                 runtime)
                        : run_codex(state, request, runtime);
    bool cancelled = false;
    {
        std::lock_guard lock(state->active_mutex);
        cancelled = state->active_turns.at(request->turn_id).cancelled;
        state->active_turns.erase(request->turn_id);
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

Result start_codex(Provider* provider) {
    CodexState* state = static_cast<CodexState*>(provider->state);
    provider->default_model = state->options.default_model;
    provider->models.push_back({state->options.default_model,
                                state->options.default_model, {}, {}, {},
                                codex_permission_modes()});
    if (state->options.execute == nullptr)
        provider_runtime_set_initialize(&state->runtime, initialize_codex);
    else
        provider->availability = ProviderAvailability::Available;
    Result result = provider_runtime_start(&state->runtime);
    if (result.status == ResultStatus::Error)
        return result;
    if (state->options.execute == nullptr) {
        state->usage_worker = std::thread(run_codex_usage, state);
        provider->request_usage = request_codex_usage;
        provider->poll_usage = poll_codex_usage;
        state->skills_worker = std::thread(run_codex_skills, state);
        provider->request_skills = request_codex_skills;
        provider->poll_skills = poll_codex_skills;
        provider->list_mcp_servers = list_codex_mcp_servers;
        provider->upsert_mcp_server = upsert_codex_mcp_server;
        provider->remove_mcp_server = remove_codex_mcp;
    }
    return result_ok();
}

Result submit_codex(Provider* provider, TurnRequest request) {
    CodexState* state = static_cast<CodexState*>(provider->state);
    const TurnId turn_id = request.turn_id;
    {
        std::lock_guard lock(state->active_mutex);
        state->active_turns.emplace(turn_id, CodexState::ActiveTurn{});
    }
    Result result = provider_runtime_submit(&state->runtime, std::move(request));
    if (result.status == ResultStatus::Error) {
        std::lock_guard lock(state->active_mutex);
        state->active_turns.erase(turn_id);
    }
    return result;
}

Result respond_codex(Provider*, const ProviderRequestId&, ApprovalDecision) {
    return result_error("Provider has no pending approval request");
}

void cancel_codex(Provider* provider, TurnId turn_id) {
    CodexState* state = static_cast<CodexState*>(provider->state);
    const bool queued = provider_runtime_cancel_queued(&state->runtime, turn_id);
    std::lock_guard lock(state->active_mutex);
    if (queued) {
        state->active_turns.erase(turn_id);
        return;
    }
    auto active = state->active_turns.find(turn_id);
    if (turn_id == 0 || active == state->active_turns.end())
        return;
    active->second.cancelled = true;
    if (active->second.process != nullptr && child_process_running(active->second.process))
        child_process_terminate(active->second.process);
}

std::vector<Event> poll_codex(Provider* provider) {
    CodexState* state = static_cast<CodexState*>(provider->state);
    {
        std::lock_guard lock(state->startup_mutex);
        if (state->startup_complete && !state->startup_applied) {
            provider->availability = state->startup_availability;
            provider->location = std::move(state->startup_location);
            if (!state->startup_models.empty())
                provider->models = std::move(state->startup_models);
            state->startup_applied = true;
        }
    }
    return provider_runtime_poll_events(&state->runtime);
}

void destroy_codex(Provider* provider) {
    CodexState* state = static_cast<CodexState*>(provider->state);
    {
        std::lock_guard lock(state->active_mutex);
        state->shutting_down = true;
        for (auto& [id, active] : state->active_turns)
            if (active.process != nullptr && child_process_running(active.process))
                child_process_terminate(active.process);
        if (state->startup_process != nullptr && child_process_running(state->startup_process))
            child_process_terminate(state->startup_process);
        if (state->usage_process != nullptr && child_process_running(state->usage_process))
            child_process_terminate(state->usage_process);
        if (state->skills_process != nullptr && child_process_running(state->skills_process))
            child_process_terminate(state->skills_process);
    }
    {
        std::lock_guard lock(state->usage_mutex);
        state->usage_stopping = true;
    }
    state->usage_ready.notify_all();
    {
        std::lock_guard lock(state->skills_mutex);
        state->skills_stopping = true;
    }
    state->skills_ready.notify_all();
    if (state->usage_worker.joinable())
        state->usage_worker.join();
    if (state->skills_worker.joinable())
        state->skills_worker.join();
    provider_runtime_shutdown(&state->runtime);
    delete state;
    delete provider;
}
} // namespace

ProviderPtr make_codex_provider(const CodexOptions* options) {
    CodexState* state = new CodexState{};
    if (options != nullptr)
        state->options = *options;
    provider_runtime_init(&state->runtime, process_codex, state);

    Provider* provider = new Provider{"codex",       state,        start_codex, submit_codex,
                                      respond_codex, cancel_codex, poll_codex,  destroy_codex};
    return ProviderPtr(provider, destroy_provider);
}
