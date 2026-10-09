#include "provider_runtime.h"
#include "provider_mcp_utils.h"
#include "usage-time.h"
#include "../base/os.h"
#include "../process/child-process.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
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


static std::string string_value(const Json& value, const char* key) {
    return value.contains(key) && value[key].is_string() ? value[key].get<std::string>()
                                                         : std::string{};
}

static std::string path_utf8(const std::filesystem::path& path) {
    const std::u8string value = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

static std::string copilot_json_text(const Json& value) {
    if (value.is_string())
        return value.get<std::string>();
    if (value.is_null())
        return {};
    return value.dump(2);
}

struct CopilotState {
    ProviderRuntime runtime;
    GitHubCopilotOptions options;
    std::mutex active_mutex;
    struct ActiveTurn {
        ChildProcess* process = nullptr;
        bool cancelled = false;
    };
    std::map<TurnId, ActiveTurn> active_turns;
    ChildProcess* startup_process = nullptr;
    ChildProcess* mcp_process = nullptr;
    bool shutting_down = false;
    std::mutex startup_mutex;
    bool startup_complete = false;
    std::condition_variable startup_ready;
    bool startup_applied = false;
    ProviderAvailability startup_availability = ProviderAvailability::Unknown;
    std::filesystem::path startup_location;
    std::string startup_default_model;
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
    };
    std::deque<SkillsRequest> skills_requests;
    std::vector<SkillDiscoverySnapshot> skills_updates;
    bool skills_stopping = false;
    std::thread skills_worker;
    ChildProcess* skills_process = nullptr;
    std::mutex skills_cli_mutex;
    ChildProcess* skills_cli_process = nullptr;
};

struct CopilotStreamContext {
    ProviderRuntime* runtime;
    const TurnRequest* request;
    std::filesystem::path working_directory{};
    std::vector<SkillEntry> available_commands{};
    bool available_commands_received = false;
};

std::string utc_timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#if OS_WIN
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    std::ostringstream timestamp;
    timestamp << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return timestamp.str();
}

Json json_shape(const Json& value, int depth = 0) {
    Json shape = {{"type", value.type_name()}};
    if (depth >= 5)
        return shape;
    if (value.is_object()) {
        Json fields = Json::object();
        std::size_t count = 0;
        for (auto it = value.begin(); it != value.end() && count < 40; ++it, ++count) {
            if (it.key().size() <= 80)
                fields[it.key()] = json_shape(it.value(), depth + 1);
        }
        shape["field_count"] = value.size();
        shape["fields"] = std::move(fields);
    } else if (value.is_array()) {
        shape["length"] = value.size();
        if (!value.empty())
            shape["first_item"] = json_shape(value.front(), depth + 1);
    }
    return shape;
}

Json response_diagnostic(const Json& response) {
    Json diagnostic = {{"shape", json_shape(response)}};
    if (!response.is_object())
        return diagnostic;
    if (response.contains("id"))
        diagnostic["id"] = response["id"];
    if (response.contains("error")) {
        const Json& error = response["error"];
        if (error.is_object()) {
            if (error.contains("code"))
                diagnostic["error"]["code"] = error["code"];
            if (error.contains("message"))
                diagnostic["error"]["message"] = error["message"];
        } else if (error.is_string()) {
            diagnostic["error"] = error;
        }
    }
    const Json result = response.value("result", Json::object());
    if (result.is_object() && result.contains("stopReason")) {
        diagnostic["stop_reason_type"] = result["stopReason"].type_name();
        diagnostic["stop_reason"] = result["stopReason"];
    } else {
        diagnostic["stop_reason"] = "<missing>";
    }
    return diagnostic;
}

std::string read_diagnostic_tail(const std::filesystem::path& path) {
    if (path.empty())
        return {};
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    constexpr std::streamoff max_size = 32 * 1024;
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size > max_size)
        input.seekg(size - max_size, std::ios::beg);
    else
        input.seekg(0, std::ios::beg);
    std::string content(static_cast<std::size_t>(std::min(size, max_size)), '\0');
    input.read(content.data(), static_cast<std::streamsize>(content.size()));
    return content;
}

void append_copilot_diagnostic(const GitHubCopilotOptions& options,
                               const TurnRequest& request, const std::string& stage,
                               const std::string& error, const Json& response,
                               const std::filesystem::path& stderr_path) {
    if (options.diagnostics_path.empty())
        return;
    std::error_code filesystem_error;
    std::filesystem::create_directories(options.diagnostics_path.parent_path(), filesystem_error);
    Json entry = {
        {"timestamp", utc_timestamp()},
        {"provider", "github_copilot"},
        {"event", "turn_failed"},
        {"stage", stage},
        {"turn_id", request.turn_id},
        {"conversation_id", request.conversation_id},
        {"model", request.model.empty() ? options.default_model : request.model},
        {"working_directory", request.working_directory.string()},
        {"error", error},
        {"acp_response", response_diagnostic(response)},
        {"stderr_tail", read_diagnostic_tail(stderr_path)},
    };
    std::ofstream output(options.diagnostics_path, std::ios::app | std::ios::binary);
    if (output)
        output << entry.dump() << '\n';
}

Result start_copilot_process(const GitHubCopilotOptions* options, const std::string& model,
                             const std::string& effort, ChildProcess* process,
                             const std::filesystem::path& error_output_path = {}) {
    if (model.find_first_of("\"\\%!&|<>^\r\n") != std::string::npos ||
        effort.find_first_of("\"\\%!&|<>^\r\n") != std::string::npos)
        return result_error("Invalid GitHub Copilot model or effort identifier");

    const std::filesystem::path executable =
        child_process_resolve_executable(options->executable);
    if (executable.empty())
        return result_error("GitHub Copilot executable was not found");

    std::vector<std::string> arguments = {"--acp", "--stdio"};
    if (!model.empty())
        arguments.push_back("--model=" + model);
    if (!effort.empty())
        arguments.push_back("--effort=" + effort);
    return child_process_start(process, executable, arguments,
                               "GitHub Copilot ACP server", {}, error_output_path);
}

static bool copilot_write_message(FILE* input, const Json& message) {
    const std::string serialized = message.dump();
    return fputs(serialized.c_str(), input) >= 0 && fputc('\n', input) != EOF &&
           fflush(input) == 0;
}

static bool copilot_read_message(FILE* output, Json* message) {
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

std::string tool_content_text(const Json& content) {
    if (!content.is_array())
        return copilot_json_text(content);

    std::string result;
    for (const Json& item : content) {
        if (!result.empty())
            result += '\n';
        if (item.contains("content") && item["content"].is_object())
            result += string_value(item["content"], "text");
        else if (item.contains("text") && item["text"].is_string())
            result += string_value(item, "text");
        else
            result += copilot_json_text(item);
    }
    return result;
}

Event make_tool_activity_event(CopilotStreamContext* context, const Json& update) {
    Event event{EventKind::ToolActivity, context->request->conversation_id,
                context->request->turn_id};
    event.item_id = string_value(update, "toolCallId");
    event.tool_name = string_value(update, "name");
    event.text = string_value(update, "title");
    event.status = string_value(update, "status");
    event.tool_completed = event.status == "completed" || event.status == "failed";
    if (event.status.empty())
        event.status = event.tool_completed ? "completed" : "inProgress";

    if (update.contains("rawInput")) {
        event.tool_arguments = copilot_json_text(update["rawInput"]);
        event.text = string_value(update["rawInput"], "command");
        event.cwd = string_value(update["rawInput"], "cwd");
    }
    if (event.text.empty())
        event.text = string_value(update, "title");
    event.is_terminal = string_value(update, "kind") == "execute" ||
                        !string_value(update.value("rawInput", Json::object()), "command").empty();

    if (event.tool_completed) {
        if (update.contains("rawOutput"))
            event.output = tool_content_text(update["rawOutput"]);
        else if (update.contains("content"))
            event.output = tool_content_text(update["content"]);
        if (update.contains("rawOutput") && update["rawOutput"].is_object() &&
            update["rawOutput"].contains("exitCode") &&
            update["rawOutput"]["exitCode"].is_number_integer())
            event.exit_code = update["rawOutput"]["exitCode"].get<int>();
    }
    return event;
}

void emit_text_event(CopilotStreamContext* context, EventKind kind, const Json& content) {
    if (context->request == nullptr || string_value(content, "type") != "text")
        return;
    const Event event{kind, context->request->conversation_id, context->request->turn_id,
                      string_value(content, "text"), {}, {}};
    provider_runtime_emit(context->runtime, &event);
}

bool respond_to_permission(ChildProcess* process, const Json& message) {
    const Json params = message.value("params", Json::object());
    const Json options = params.value("options", Json::array());
    std::string option_id;
    for (const Json& option : options) {
        if (string_value(option, "kind") == "allow_once") {
            option_id = string_value(option, "optionId");
            break;
        }
    }
    if (option_id.empty()) {
        for (const Json& option : options) {
            if (string_value(option, "kind") == "reject_once") {
                option_id = string_value(option, "optionId");
                break;
            }
        }
    }
    if (option_id.empty())
        return copilot_write_message(process->input,
                             Json{{"jsonrpc", "2.0"}, {"id", message.value("id", Json())},
                                  {"result", {{"outcome", {{"outcome", "cancelled"}}}}}});

    return copilot_write_message(process->input,
                         Json{{"jsonrpc", "2.0"}, {"id", message.value("id", Json())},
                              {"result", {{"outcome", {{"outcome", "selected"},
                                                        {"optionId", option_id}}}}}});
}

void handle_server_message(ChildProcess* process, CopilotStreamContext* context,
                           const Json& message) {
    const std::string method = string_value(message, "method");
    if (method == "session/request_permission") {
        respond_to_permission(process, message);
        return;
    }
    if (method != "session/update")
        return;

    const Json update = message.value("params", Json::object()).value("update", Json::object());
    const std::string update_type = string_value(update, "sessionUpdate");
    if (update_type == "available_commands_update") {
        context->available_commands_received = true;
        context->available_commands.clear();
        const Json commands = update.value("availableCommands", Json::array());
        if (commands.is_array()) {
            for (const Json& command : commands) {
                if (!command.is_object())
                    continue;
                SkillEntry entry;
                entry.name = string_value(command, "name");
                if (entry.name.empty())
                    continue;
                entry.description = string_value(command, "description");
                entry.invocation = "/" + entry.name;
                entry.kind = SkillEntryKind::CommandOrSkill;
                const Json input = command.value("input", Json::object());
                entry.input_hint = string_value(input, "hint");
                context->available_commands.push_back(std::move(entry));
            }
        }
    }
    if (context->request == nullptr)
        return;
    if (update_type == "agent_message_chunk") {
        emit_text_event(context, EventKind::AssistantTextDelta,
                        update.value("content", Json::object()));
    } else if (update_type == "agent_thought_chunk") {
        emit_text_event(context, EventKind::AssistantReasoningDelta,
                        update.value("content", Json::object()));
    } else if (update_type == "tool_call" || update_type == "tool_call_update") {
        const Event event = make_tool_activity_event(context, update);
        provider_runtime_emit(context->runtime, &event);
    }
}

static bool copilot_wait_for_response(ChildProcess* process, int request_id, CopilotStreamContext* context,
                       Json* response, std::string* error) {
    for (;;) {
        Json message;
        if (!copilot_read_message(process->output, &message)) {
            *error = "GitHub Copilot ACP server closed its output";
            return false;
        }
        if (message.is_object() && !message.contains("method") &&
            message.contains("id") && message["id"].is_number_integer() &&
            message["id"].get<int>() == request_id) {
            if (response != nullptr)
                *response = message;
            if (message.contains("error")) {
                *error = string_value(message["error"], "message");
                if (error->empty() && message["error"].is_string())
                    *error = message["error"].get<std::string>();
                if (error->empty())
                    *error = "GitHub Copilot ACP request failed";
                return false;
            }
            return true;
        }
        handle_server_message(process, context, message);
    }
}

std::string copilot_quota_name(const std::string& key) {
    if (key == "premium_interactions")
        return "Monthly Credits";
    if (key == "chat")
        return "Chat";
    if (key == "completions")
        return "Completions";

    std::string name;
    bool capitalize = true;
    for (const char character : key) {
        if (character == '_' || character == '-') {
            name.push_back(' ');
            capitalize = true;
        } else {
            name.push_back(capitalize
                               ? static_cast<char>(std::toupper(
                                     static_cast<unsigned char>(character)))
                               : character);
            capitalize = false;
        }
    }
    return name;
}

std::string quota_number(double value) {
    std::ostringstream formatted;
    const double rounded = std::round(value);
    if (std::isfinite(value) && std::abs(value - rounded) < 0.0001) {
        formatted << static_cast<long long>(rounded);
    } else {
        formatted << std::fixed << std::setprecision(1) << value;
    }
    return formatted.str();
}

UsageSnapshot parse_copilot_usage(const Json& result) {
    UsageSnapshot snapshot;
    const Json quotas = result.value("quotaSnapshots", Json::object());
    if (!quotas.is_object())
        return snapshot;

    std::vector<std::string> keys;
    for (const char* key : {"premium_interactions"}) {
        if (quotas.contains(key))
            keys.emplace_back(key);
    }
    for (auto it = quotas.begin(); it != quotas.end(); ++it) {
        if (it.key() == "chat" || it.key() == "completions")
            continue;
        if (std::find(keys.begin(), keys.end(), it.key()) == keys.end())
            keys.push_back(it.key());
    }

    for (const std::string& key : keys) {
        const Json& quota = quotas[key];
        if (!quota.is_object())
            continue;

        const std::int64_t entitlement =
            quota.contains("entitlementRequests") && quota["entitlementRequests"].is_number()
                ? quota["entitlementRequests"].get<std::int64_t>()
                : -1;
        const double used = quota.contains("usedRequests") && quota["usedRequests"].is_number()
                                ? quota["usedRequests"].get<double>()
                                : 0.0;
        const double overage = quota.contains("overage") && quota["overage"].is_number()
                                   ? quota["overage"].get<double>()
                                   : 0.0;
        const bool unlimited = entitlement < 0 ||
            (quota.contains("isUnlimitedEntitlement") &&
             quota["isUnlimitedEntitlement"].is_boolean() &&
             quota["isUnlimitedEntitlement"].get<bool>());

        UsageMetric metric;
        metric.name = copilot_quota_name(key);
        const std::string reset_date = string_value(quota, "resetDate");
        metric.reset_at = usage_time::format_iso8601_utc(reset_date);
        if (metric.reset_at.empty())
            metric.reset_at = reset_date;
        if (unlimited) {
            metric.value = "Unlimited allowance";
            metric.detail = quota_number(used) + " requests used this period.";
        } else if (entitlement > 0) {
            double remaining_percentage =
                quota.contains("remainingPercentage") && quota["remainingPercentage"].is_number()
                    ? quota["remainingPercentage"].get<double>()
                    : 100.0 * (static_cast<double>(entitlement) - used) /
                          static_cast<double>(entitlement);
            remaining_percentage = std::clamp(remaining_percentage, 0.0, 100.0);
            metric.value = quota_number(used) + "/" +
                           quota_number(static_cast<double>(entitlement)) + " Used";
            metric.detail = "Used " + quota_number(used) + " of " +
                            quota_number(static_cast<double>(entitlement)) +
                            " included requests.";
            metric.limit = static_cast<double>(entitlement);
            metric.used = used;
            metric.remaining = static_cast<double>(entitlement) * remaining_percentage / 100.0;
        } else {
            metric.value = "No included allowance";
            metric.detail = quota_number(used) + " requests used this period.";
        }

        if (overage > 0.0)
            metric.detail += " Additional usage: " + quota_number(overage) + " requests.";
        if (!unlimited && quota.contains("usageAllowedWithExhaustedQuota") &&
            quota["usageAllowedWithExhaustedQuota"].is_boolean()) {
            metric.detail += quota["usageAllowedWithExhaustedQuota"].get<bool>()
                                 ? " Usage can continue after the allowance is exhausted."
                                 : " Usage stops when the allowance is exhausted.";
        }
        snapshot.metrics.push_back(std::move(metric));
    }
    return snapshot;
}

bool wait_for_server_response(ChildProcess* process, int request_id, Json* response,
                              std::string* error) {
    for (;;) {
        Json message;
        std::size_t content_length = 0;
        bool has_length = false;
        std::string header;
        int character;
        while ((character = fgetc(process->output)) != EOF) {
            if (character != '\n') {
                if (character != '\r')
                    header.push_back(static_cast<char>(character));
                if (header.size() > 1024) {
                    *error = "Invalid GitHub Copilot server response header";
                    return false;
                }
                continue;
            }
            if (header.empty())
                break;
            constexpr std::string_view prefix = "Content-Length: ";
            if (header.compare(0, prefix.size(), prefix) == 0) {
                try {
                    content_length = std::stoull(header.substr(prefix.size()));
                    has_length = true;
                } catch (const std::exception&) {
                    *error = "Invalid GitHub Copilot server response length";
                    return false;
                }
            }
            header.clear();
        }
        if (character == EOF || !has_length || content_length == 0 ||
            content_length > 16 * 1024 * 1024) {
            *error = "GitHub Copilot server closed its output or sent an invalid response";
            return false;
        }
        std::string body(content_length, '\0');
        if (fread(body.data(), 1, content_length, process->output) != content_length) {
            *error = "GitHub Copilot server closed its output";
            return false;
        }
        try {
            message = Json::parse(body);
        } catch (const Json::parse_error&) {
            *error = "Invalid GitHub Copilot server response";
            return false;
        }
        if (!message.is_object() || message.contains("method") ||
            !message.contains("id") || !message["id"].is_number_integer() ||
            message["id"].get<int>() != request_id)
            continue;

        if (response != nullptr)
            *response = message;
        if (message.contains("error")) {
            const Json& rpc_error = message["error"];
            *error = string_value(rpc_error, "message");
            if (error->empty() && rpc_error.is_string())
                *error = rpc_error.get<std::string>();
            if (error->empty())
                *error = "GitHub Copilot server request failed";
            return false;
        }
        return true;
    }
}

bool fetch_copilot_usage(CopilotState* state, UsageSnapshot* snapshot,
                         std::string* error) {
    child_process_ignore_sigpipe();

    std::filesystem::path executable;
    {
        std::unique_lock lock(state->startup_mutex);
        state->startup_ready.wait(lock, [state] { return state->startup_complete; });
        executable = state->startup_location;
    }
    if (executable.empty()) {
        *error = "GitHub Copilot CLI executable was not found";
        return false;
    }

    ChildProcess process;
    const std::vector<std::string> arguments = {"--server", "--stdio", "--no-auto-update"};
    Result started = child_process_start(&process, executable, arguments,
                                         "GitHub Copilot usage server");
    if (started.status == ResultStatus::Error) {
        *error = started.error;
        child_process_stop(&process);
        return false;
    }
#if OS_WIN
    _setmode(_fileno(process.input), _O_BINARY);
    _setmode(_fileno(process.output), _O_BINARY);
#endif
    {
        std::lock_guard lock(state->active_mutex);
        state->usage_process = &process;
        if (state->shutting_down)
            child_process_terminate(&process);
    }

    const Json connect = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "connect"},
                          {"params", Json::object()}};
    const Json quota_request = {{"jsonrpc", "2.0"}, {"id", 2},
                                {"method", "account.getQuota"},
                                {"params", Json::object()}};
    Json response;
    const auto write_server_message = [&process](const Json& message) {
        const std::string body = message.dump();
        const std::string header = "Content-Length: " + std::to_string(body.size()) +
                                   "\r\n\r\n";
        return fwrite(header.data(), 1, header.size(), process.input) == header.size() &&
               fwrite(body.data(), 1, body.size(), process.input) == body.size() &&
               fflush(process.input) == 0;
    };
    const bool success = write_server_message(connect) &&
        wait_for_server_response(&process, 1, nullptr, error) &&
        write_server_message(quota_request) &&
        wait_for_server_response(&process, 2, &response, error);
    if (!success && error->empty())
        *error = "Could not send GitHub Copilot quota request";
    if (success)
        *snapshot = parse_copilot_usage(response.value("result", Json::object()));

    {
        std::lock_guard lock(state->active_mutex);
        if (state->usage_process == &process)
            state->usage_process = nullptr;
    }
    child_process_stop(&process);
    return success;
}

void run_copilot_usage(void* context) {
    CopilotState* state = static_cast<CopilotState*>(context);
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
        std::string error;
        if (!fetch_copilot_usage(state, &snapshot, &error)) {
            UsageMetric metric;
            metric.name = "Usage unavailable";
            metric.value = error.empty() ? "Could not read GitHub Copilot quota." : error;
            snapshot.metrics.push_back(std::move(metric));
        }
        snapshot.updated_at = utc_timestamp();
        std::lock_guard lock(state->usage_mutex);
        state->usage_snapshot = std::move(snapshot);
        state->usage_updated = true;
    }
}

void request_copilot_usage(Provider* provider) {
    CopilotState* state = static_cast<CopilotState*>(provider->state);
    {
        std::lock_guard lock(state->usage_mutex);
        if (state->usage_stopping)
            return;
        state->usage_requested = true;
    }
    state->usage_ready.notify_one();
}

std::optional<UsageSnapshot> poll_copilot_usage(Provider* provider) {
    CopilotState* state = static_cast<CopilotState*>(provider->state);
    std::lock_guard lock(state->usage_mutex);
    if (!state->usage_updated)
        return std::nullopt;
    state->usage_updated = false;
    return state->usage_snapshot;
}

bool initialize_copilot(ChildProcess* process, CopilotStreamContext* context, Json* response,
                        std::string* error) {
    const Json initialize = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "initialize"},
        {"params", {{"protocolVersion", 1},
                    {"clientCapabilities", {{"session", {{"configOptions", Json::object()}}}}},
                    {"clientInfo", {{"name", "Zenith"}, {"version", "0.1.0"}}}}},
    };
    return copilot_write_message(process->input, initialize) &&
           copilot_wait_for_response(process, 1, context, response, error);
}

std::filesystem::path session_working_directory(const TurnRequest* request,
                                                 std::error_code* error) {
    if (!request->working_directory.empty()) {
        const std::filesystem::path absolute =
            std::filesystem::absolute(request->working_directory, *error);
        if (!*error)
            return absolute;
    }
    error->clear();
    return std::filesystem::current_path(*error);
}

bool create_copilot_session(ChildProcess* process, CopilotStreamContext* context,
                            const std::filesystem::path& working_directory, Json* response,
                            std::string* error) {
    const Json create = {
        {"jsonrpc", "2.0"},
        {"id", 2},
        {"method", "session/new"},
        {"params", {{"cwd", path_utf8(working_directory)}}},
    };
    return copilot_write_message(process->input, create) &&
           copilot_wait_for_response(process, 2, context, response, error);
}

void append_model_options(const Json& options, std::vector<ModelOption>* models) {
    if (!options.is_array())
        return;
    for (const Json& option : options) {
        if (!option.is_object())
            continue;
        if (option.contains("options") && option["options"].is_array()) {
            append_model_options(option["options"], models);
            continue;
        }
        const std::string id = string_value(option, "value");
        if (id.empty())
            continue;
        const std::string name = string_value(option, "name");
        const auto duplicate = std::find_if(models->begin(), models->end(),
                                            [&id](const ModelOption& model) {
                                                return model.id == id;
                                            });
        if (duplicate == models->end())
            models->push_back({id, name.empty() ? id : name, {}, {}, {}, {}});
    }
}

std::vector<ModelOption> models_from_session(const Json& session, std::string* default_model) {
    std::vector<ModelOption> models;
    const Json options = session.value("result", Json::object()).value("configOptions", Json::array());
    if (!options.is_array())
        return models;

    std::vector<ReasoningOption> reasoning_options;
    std::string reasoning_default;
    for (const Json& option : options) {
        const std::string category = string_value(option, "category");
        const std::string id = string_value(option, "id");
        if (string_value(option, "type") != "select")
            continue;
        if (category == "model" || id == "model") {
            *default_model = string_value(option, "currentValue");
            append_model_options(option.value("options", Json::array()), &models);
        } else if (category == "thought_level" || id == "effort") {
            reasoning_default = string_value(option, "currentValue");
            const Json values = option.value("options", Json::array());
            std::vector<ModelOption> reasoning_models;
            append_model_options(values, &reasoning_models);
            for (const ModelOption& value : reasoning_models)
                reasoning_options.push_back({value.id, value.name});
        }
    }

    for (ModelOption& model : models) {
        model.default_reasoning_effort = reasoning_default;
        model.reasoning_efforts = reasoning_options;
    }
    return models;
}

bool discover_copilot_models(CopilotState* state, ProviderRuntime* runtime,
                             ProviderAvailability* availability,
                             std::vector<ModelOption>* models, std::string* default_model) {
    child_process_ignore_sigpipe();

    ChildProcess process;
    CopilotStreamContext context{runtime, nullptr};
    std::string error;
    *availability = ProviderAvailability::Unavailable;
    Result result = start_copilot_process(&state->options, {}, {}, &process);
    if (result.status == ResultStatus::Error) {
        child_process_stop(&process);
        return false;
    }

    {
        std::lock_guard lock(state->active_mutex);
        state->startup_process = &process;
        if (state->shutting_down)
            child_process_terminate(&process);
    }

    bool success = initialize_copilot(&process, &context, nullptr, &error);
    std::error_code path_error;
    std::filesystem::path cwd = std::filesystem::current_path(path_error);
    if (path_error) {
        success = false;
        error = "Failed to determine the working directory for GitHub Copilot";
    }
    Json session;
    if (success)
        success = create_copilot_session(&process, &context, cwd, &session, &error);
    if (success) {
        *availability = ProviderAvailability::Available;
        *models = models_from_session(session, default_model);
    }
    {
        std::lock_guard lock(state->active_mutex);
        if (state->startup_process == &process)
            state->startup_process = nullptr;
    }
    child_process_stop(&process);
    return success;
}

bool fetch_copilot_cli_skill_list(CopilotState* state,
                                 const std::filesystem::path& working_directory,
                                 Json* skill_list, std::string* error) {
    std::lock_guard cli_lock(state->skills_cli_mutex);
    {
        std::lock_guard lock(state->active_mutex);
        if (state->shutting_down) {
            *error = "GitHub Copilot skill discovery is stopping";
            return false;
        }
    }

    ChildProcess process;
    const std::vector<std::string> arguments = {
        "--no-auto-update", "skill", "list", "--json"};
    const Result start_result = child_process_start(
        &process, state->options.executable, arguments,
        "GitHub Copilot skill list", {}, {}, working_directory);
    if (start_result.status == ResultStatus::Error) {
        *error = start_result.error;
        child_process_stop(&process);
        return false;
    }
    {
        std::lock_guard lock(state->active_mutex);
        state->skills_cli_process = &process;
        if (state->shutting_down)
            child_process_terminate(&process);
    }

    constexpr std::size_t max_output_size = 8 * 1024 * 1024;
    std::string output;
    int character = 0;
    bool output_too_large = false;
    while ((character = fgetc(process.output)) != EOF) {
        if (output.size() >= max_output_size) {
            output_too_large = true;
            child_process_terminate(&process);
            break;
        }
        output.push_back(static_cast<char>(character));
    }

    {
        std::lock_guard lock(state->active_mutex);
        if (state->skills_cli_process == &process)
            state->skills_cli_process = nullptr;
    }
    child_process_stop(&process);

    if (output_too_large) {
        *error = "GitHub Copilot returned an unexpectedly large skill list";
        return false;
    }
    try {
        *skill_list = Json::parse(output);
    } catch (...) {
        *error = "GitHub Copilot did not return a JSON skill list. "
                 "Update Copilot CLI to a version that supports `copilot skill list --json`.";
        return false;
    }
    if (!skill_list->is_array() &&
        !(skill_list->is_object() && skill_list->contains("skills") &&
          (*skill_list)["skills"].is_array())) {
        *error = "GitHub Copilot returned an invalid JSON skill list";
        return false;
    }
    return true;
}

bool copilot_command_matches_skill(const std::string& command_name,
                                   const std::string& skill_name) {
    if (command_name == skill_name)
        return true;
    return command_name.size() > skill_name.size() &&
           command_name.compare(command_name.size() - skill_name.size(),
                                skill_name.size(), skill_name) == 0 &&
           command_name[command_name.size() - skill_name.size() - 1] == '/';
}

std::vector<SkillEntry> copilot_skill_entries(
    const Json& skill_list, const std::vector<SkillEntry>& available_commands) {
    const Json& rows = skill_list.is_array() ? skill_list : skill_list["skills"];
    std::vector<SkillEntry> entries;
    for (const Json& skill : rows) {
        if (!skill.is_object())
            continue;
        const std::string skill_name = string_value(skill, "name");
        if (skill_name.empty())
            continue;
        const bool enabled = !skill.contains("enabled") || !skill["enabled"].is_boolean() ||
                             skill["enabled"].get<bool>();
        const bool user_invocable =
            (!skill.contains("user-invocable") || !skill["user-invocable"].is_boolean() ||
             skill["user-invocable"].get<bool>()) &&
            (!skill.contains("userInvocable") || !skill["userInvocable"].is_boolean() ||
             skill["userInvocable"].get<bool>());
        if (!enabled || !user_invocable)
            continue;

        for (const SkillEntry& command : available_commands) {
            if (!copilot_command_matches_skill(command.name, skill_name))
                continue;

            SkillEntry entry = command;
            entry.kind = SkillEntryKind::Skill;
            entry.enabled = enabled && command.enabled;
            const std::string description = string_value(skill, "description");
            if (!description.empty())
                entry.description = description;
            const std::string path = string_value(skill, "path");
            if (!path.empty())
                entry.path = std::filesystem::u8path(path);
            entry.scope = string_value(skill, "source");
            entries.push_back(std::move(entry));
        }
    }
    return entries;
}

SkillDiscoverySnapshot fetch_copilot_skills(
    CopilotState* state, const std::filesystem::path& requested_working_directory) {
    SkillDiscoverySnapshot snapshot;
    std::error_code path_error;
    const std::filesystem::path working_directory =
        std::filesystem::absolute(requested_working_directory, path_error).lexically_normal();
    if (path_error) {
        snapshot.working_directory = requested_working_directory;
        snapshot.error = "Failed to resolve the GitHub Copilot working directory: " +
                         path_error.message();
        return snapshot;
    }
    snapshot.working_directory = working_directory;

    child_process_ignore_sigpipe();
    ChildProcess process;
    const Result start_result = start_copilot_process(&state->options, {}, {}, &process);
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

    CopilotStreamContext context{nullptr, nullptr};
    context.working_directory = working_directory;
    std::string error;
    bool success = initialize_copilot(&process, &context, nullptr, &error);
    Json session;
    if (success)
        success = create_copilot_session(&process, &context, working_directory, &session,
                                         &error);
    if (success) {
        if (context.available_commands_received) {
            Json skill_list;
            std::string skill_list_error;
            if (fetch_copilot_cli_skill_list(state, working_directory, &skill_list,
                                             &skill_list_error)) {
                snapshot.entries = copilot_skill_entries(
                    skill_list, context.available_commands);
            } else {
                snapshot.error = std::move(skill_list_error);
            }
        } else {
            snapshot.error = "GitHub Copilot did not advertise available commands for this session";
        }
    } else {
        snapshot.error = error.empty() ? "GitHub Copilot skill discovery failed" : error;
    }

    {
        std::lock_guard lock(state->active_mutex);
        if (state->skills_process == &process)
            state->skills_process = nullptr;
    }
    child_process_stop(&process);
    return snapshot;
}

void run_copilot_skills(void* context) {
    CopilotState* state = static_cast<CopilotState*>(context);
    for (;;) {
        CopilotState::SkillsRequest request;
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

        SkillDiscoverySnapshot snapshot =
            fetch_copilot_skills(state, request.working_directory);
        std::lock_guard lock(state->skills_mutex);
        state->skills_updates.push_back(std::move(snapshot));
    }
}

Result request_copilot_skills(Provider* provider,
                              const std::filesystem::path& working_directory, bool) {
    CopilotState* state = static_cast<CopilotState*>(provider->state);
    {
        std::lock_guard lock(state->skills_mutex);
        if (state->skills_stopping)
            return result_error("GitHub Copilot skill discovery is stopping");
        state->skills_requests.push_back({working_directory});
    }
    state->skills_ready.notify_one();
    return result_ok();
}

std::vector<SkillDiscoverySnapshot> poll_copilot_skills(Provider* provider) {
    CopilotState* state = static_cast<CopilotState*>(provider->state);
    std::lock_guard lock(state->skills_mutex);
    std::vector<SkillDiscoverySnapshot> updates;
    updates.swap(state->skills_updates);
    return updates;
}

void publish_copilot_skills(CopilotState* state, const CopilotStreamContext& context) {
    if (!context.available_commands_received)
        return;
    Json skill_list;
    std::string error;
    if (!fetch_copilot_cli_skill_list(state, context.working_directory, &skill_list, &error))
        return;
    SkillDiscoverySnapshot snapshot;
    snapshot.working_directory = context.working_directory;
    snapshot.entries = copilot_skill_entries(skill_list, context.available_commands);
    std::lock_guard lock(state->skills_mutex);
    if (!state->skills_stopping)
        state->skills_updates.push_back(std::move(snapshot));
}

void initialize_github_copilot(void* context, ProviderRuntime*) {
    CopilotState* state = static_cast<CopilotState*>(context);
    ProviderAvailability availability = ProviderAvailability::Unavailable;
    std::filesystem::path location = child_process_resolve_executable(state->options.executable);
    std::string default_model = state->options.default_model;
    std::vector<ModelOption> models;
    if (!location.empty()) {
        discover_copilot_models(state, &state->runtime, &availability, &models,
                                &default_model);
    }

    {
        std::lock_guard lock(state->startup_mutex);
        state->startup_availability = availability;
        state->startup_location = std::move(location);
        state->startup_default_model = std::move(default_model);
        state->startup_models = std::move(models);
        state->startup_complete = true;
    }
    state->startup_ready.notify_all();
}

static std::string copilot_conversation_prompt(const TurnRequest* request) {
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

Json copilot_prompt_content(const TurnRequest* request, bool embedded_context) {
    Json content = Json::array();
    content.push_back({{"type", "text"}, {"text", copilot_conversation_prompt(request)}});
    for (const FileReference& reference : request->file_references) {
        content.push_back({
            {"type", "text"},
            {"text", "The user referenced this workspace-relative file: " +
                         reference.path.generic_string() + ". Read it if relevant."},
        });
    }
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (std::size_t index = 0; index < request->attachments.size(); ++index) {
        const FileAttachment& attachment = request->attachments[index];
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
            content.push_back({{"type", "image"}, {"data", encoded}, {"mimeType", attachment.media_type}});
        } else if (attachment.media_type.rfind("text/", 0) == 0 ||
                   attachment.media_type == "application/json") {
            content.push_back({{"type", "text"},
                {"text", "Attached file " + attachment.filename + ":\n" +
                    std::string(attachment.content.begin(), attachment.content.end())}});
        } else if (embedded_context) {
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
            content.push_back({{"type", "text"}, {"text", "Attached document: " + attachment.filename}});
            content.push_back({
                {"type", "resource"},
                {"resource", {{"uri", "attachment://zenith/" +
                                      std::to_string(request->turn_id) + "/" +
                                      std::to_string(index)},
                              {"mimeType", attachment.media_type}, {"blob", encoded}}}});
        } else {
            content.push_back({{"type", "text"},
                {"text", "The user attached " + attachment.filename + " at " +
                    attachment.path.string() +
                    ". Read this file from disk if relevant."}});
        }
    }
    return content;
}

Result run_github_copilot(CopilotState* state, const TurnRequest* request,
                          ProviderRuntime* runtime) {
    child_process_ignore_sigpipe();

    const std::string model = request->model.empty() ? state->options.default_model : request->model;
    std::filesystem::path stderr_path;
    if (!state->options.diagnostics_path.empty()) {
        std::error_code directory_error;
        const std::filesystem::path directory = state->options.diagnostics_path.parent_path();
        std::filesystem::create_directories(directory, directory_error);
        if (!directory_error)
            stderr_path = directory / ("copilot-turn-" + std::to_string(request->turn_id) +
                                       ".stderr.tmp");
    }
    ChildProcess process;
    Result result = start_copilot_process(&state->options, model, request->reasoning_effort,
                                          &process, stderr_path);
    if (result.status == ResultStatus::Error) {
        child_process_stop(&process);
        append_copilot_diagnostic(state->options, *request, "start", result.error, {},
                                  stderr_path);
        std::error_code remove_error;
        if (!stderr_path.empty())
            std::filesystem::remove(stderr_path, remove_error);
        return result;
    }
    {
        std::lock_guard lock(state->active_mutex);
        auto& active = state->active_turns.at(request->turn_id);
        active.process = &process;
        if (active.cancelled || state->shutting_down)
            child_process_terminate(&process);
    }

    CopilotStreamContext context{runtime, request};
    std::string error;
    std::string stage = "initialize";
    bool embedded_context = false;
    Json initialize_response;
    Json diagnostic_response;
    bool success = initialize_copilot(&process, &context, &initialize_response, &error);
    if (!success)
        diagnostic_response = initialize_response;
    if (success) {
        const Json capabilities = initialize_response.value("result", Json::object())
            .value("agentCapabilities", Json::object()).value("promptCapabilities", Json::object());
        embedded_context = capabilities.value("embeddedContext", false);
    }
    std::error_code path_error;
    const std::filesystem::path cwd = session_working_directory(request, &path_error);
    if (path_error) {
        success = false;
        stage = "working_directory";
        error = "Failed to determine the GitHub Copilot working directory: " +
                path_error.message();
    }
    context.working_directory = cwd;

    Json session;
    if (success) {
        stage = "session_new";
        success = create_copilot_session(&process, &context, cwd, &session, &error);
        if (!success)
            diagnostic_response = session;
    }

    std::string session_id;
    if (success) {
        session_id = string_value(session.value("result", Json::object()), "sessionId");
        if (session_id.empty()) {
            success = false;
            error = "GitHub Copilot ACP server did not return a session id";
        }
    }

    if (success) {
        stage = "session_prompt";
        Event event{EventKind::ProviderThreadStarted, request->conversation_id,
                    request->turn_id};
        event.provider_thread_id = session_id;
        provider_runtime_emit(runtime, &event);
        const Json prompt = {
            {"jsonrpc", "2.0"},
            {"id", 3},
            {"method", "session/prompt"},
            {"params", {{"sessionId", session_id},
                        {"prompt", copilot_prompt_content(request, embedded_context)}}},
        };
        Json response;
        success = copilot_write_message(process.input, prompt);
        if (!success)
            error = "Failed to write the GitHub Copilot ACP prompt request";
        if (success)
            success = copilot_wait_for_response(&process, 3, &context, &response, &error);
        if (success) {
            const Json prompt_result = response.value("result", Json::object());
            if (!prompt_result.is_object() || !prompt_result.contains("stopReason")) {
                success = false;
                error = "GitHub Copilot ACP prompt response did not include a stop reason";
            } else if (!prompt_result["stopReason"].is_string()) {
                success = false;
                error = "GitHub Copilot ACP prompt response had a non-string stop reason";
            } else {
                const std::string stop_reason = prompt_result["stopReason"].get<std::string>();
                if (stop_reason != "end_turn") {
                    success = false;
                    error = stop_reason == "cancelled" ? "GitHub Copilot turn cancelled"
                                                        : "GitHub Copilot turn stopped: " +
                                                              (stop_reason.empty()
                                                                   ? "(empty reason)"
                                                                   : stop_reason);
                }
            }
        }
        if (!success)
            diagnostic_response = response;
    }

    {
        std::lock_guard lock(state->active_mutex);
        state->active_turns.at(request->turn_id).process = nullptr;
    }
    publish_copilot_skills(state, context);
    child_process_stop(&process);
    if (!success) {
        append_copilot_diagnostic(state->options, *request, stage,
                                  error.empty() ? "GitHub Copilot ACP request failed" : error,
                                  diagnostic_response, stderr_path);
    }
    std::error_code remove_error;
    if (!stderr_path.empty())
        std::filesystem::remove(stderr_path, remove_error);
    if (!success)
        return result_error(error.empty() ? "GitHub Copilot ACP request failed" : error);
    return result_ok();
}

void process_github_copilot(void* context, const TurnRequest* request,
                            ProviderRuntime* runtime) {
    CopilotState* state = static_cast<CopilotState*>(context);
    Result result = state->options.execute != nullptr
                        ? state->options.execute(
                              state->options.execute_context, request,
                              [](void* emit_context, const Event* event) {
                                  provider_runtime_emit(static_cast<ProviderRuntime*>(emit_context),
                                                        event);
                              },
                              runtime)
                        : run_github_copilot(state, request, runtime);
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

std::string copilot_mcp_status_text(std::string status) {
    std::transform(status.begin(), status.end(), status.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    std::replace(status.begin(), status.end(), '_', ' ');
    std::replace(status.begin(), status.end(), '-', ' ');
    if (status.rfind("connected", 0) == 0)
        return "Connected";
    if (status == "starting" || status == "pending" || status == "authenticating")
        return "Starting";
    if (status == "needs auth" || status == "authentication required" ||
        status == "authenticationrequired")
        return "Authentication required";
    if (status == "failed" || status == "error")
        return "Failed";
    if (status == "disabled")
        return "Disabled";
    if (status == "enabled")
        return "Enabled";
    if (status == "not configured")
        return "Not configured";
    return status.empty() ? "Unknown" : status;
}

std::string copilot_source(const Json& entry, const std::string& source_hint) {
    if (entry.contains("source")) {
        if (entry["source"].is_string())
            return entry["source"].get<std::string>();
        if (entry["source"].is_object())
            return string_value(entry["source"], "type");
    }
    return source_hint;
}

McpServer parse_copilot_mcp_server(const Json& entry, const std::string& source_hint) {
    McpServer server;
    server.name = string_value(entry, "name");
    server.source = copilot_source(entry, source_hint);
    Json config = entry.value("config", entry.value("configuration", entry));
    if (!config.is_object())
        config = entry;
    const Json& definition = config.contains("transport") &&
            config["transport"].is_object() && !config["transport"].empty()
        ? config["transport"] : config;
    std::string type = string_value(config, "type");
    if (type.empty())
        type = string_value(config, "transport");
    if (type.empty())
        type = string_value(definition, "type");
    std::transform(type.begin(), type.end(), type.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (type == "http" || type == "sse" || type == "streamable_http") {
        server.transport = McpServerTransport::Http;
        server.url = string_value(definition, "url");
        server.headers = {};
        const Json headers = definition.value("headers", Json::object());
        if (headers.is_object()) {
            for (auto it = headers.begin(); it != headers.end(); ++it)
                if (it.value().is_string())
                    server.headers.emplace(it.key(), it.value().get<std::string>());
        }
        if (type == "sse")
            server.editable = false;
    } else if (type == "stdio" || type == "local" ||
               definition.contains("command")) {
        server.transport = McpServerTransport::Stdio;
        server.command = string_value(definition, "command");
        if (definition.contains("args") && definition["args"].is_array()) {
            for (const Json& argument : definition["args"])
                if (argument.is_string())
                    server.arguments.push_back(argument.get<std::string>());
        }
        const Json environment = definition.value("env", Json::object());
        if (environment.is_object()) {
            for (auto it = environment.begin(); it != environment.end(); ++it)
                if (it.value().is_string())
                    server.environment.emplace(it.key(), it.value().get<std::string>());
        }
    } else {
        server.editable = false;
    }

    const Json tools = config.value("tools", Json::array());
    const bool all_tools = !tools.is_array() || tools.empty() ||
        (tools.size() == 1 && tools.front().is_string() &&
         tools.front().get<std::string>() == "*");
    if (!all_tools ||
        (config.contains("timeout") && !config["timeout"].is_null()) ||
        (config.contains("cwd") && !config["cwd"].is_null()) ||
        (config.contains("oauth") && !config["oauth"].is_null()) ||
        (config.contains("oauthClientId") && !config["oauthClientId"].is_null()) ||
        (config.contains("oauthClientSecret") && !config["oauthClientSecret"].is_null()))
        server.editable = false;

    if (entry.contains("enabled") && entry["enabled"].is_boolean())
        server.enabled = entry["enabled"].get<bool>();
    else if (config.contains("enabled") && config["enabled"].is_boolean())
        server.enabled = config["enabled"].get<bool>();
    if (!server.enabled)
        server.status = "Disabled";
    else {
        std::string status = string_value(entry, "connectionStatus");
        if (status.empty())
            status = string_value(entry, "status");
        server.status = copilot_mcp_status_text(std::move(status));
        if (server.status == "Unknown")
            server.status = "Enabled";
    }
    server.status_detail = string_value(entry, "error");
    if (server.source.empty() || server.source == "user" || server.source == "User") {
        server.removable = true;
    } else {
        server.editable = false;
        server.removable = false;
    }
    if (!server.enabled)
        server.editable = false;
    if (!server.editable && server.status_detail.empty() && server.removable)
        server.status_detail = "Edit this server in the Copilot configuration.";
    return server;
}

void append_copilot_mcp_entries(const Json& value, const std::string& source_hint,
                               std::vector<McpServer>* servers) {
    if (value.is_array()) {
        for (const Json& entry : value)
            append_copilot_mcp_entries(entry, source_hint, servers);
        return;
    }
    if (!value.is_object())
        return;

    const std::string name = string_value(value, "name");
    if (!name.empty()) {
        McpServer server = parse_copilot_mcp_server(value, source_hint);
        servers->push_back(std::move(server));
        return;
    }

    for (auto it = value.begin(); it != value.end(); ++it) {
        std::string child_source = source_hint;
        std::string group_name = it.key();
        std::transform(group_name.begin(), group_name.end(), group_name.begin(),
            [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
        if (group_name.find("user") != std::string::npos)
            child_source = "user";
        else if (group_name.find("workspace") != std::string::npos ||
                 group_name.find("repository") != std::string::npos)
            child_source = "workspace";
        else if (group_name.find("plugin") != std::string::npos)
            child_source = "plugin";
        else if (group_name.find("builtin") != std::string::npos ||
                 group_name.find("built in") != std::string::npos)
            child_source = "builtin";

        if ((it.key() == "mcpServers" || it.key() == "servers") &&
            it.value().is_object()) {
            for (auto server = it.value().begin(); server != it.value().end(); ++server) {
                if (!server.value().is_object())
                    continue;
                Json named = server.value();
                named["name"] = server.key();
                append_copilot_mcp_entries(named, child_source, servers);
            }
        } else {
            append_copilot_mcp_entries(it.value(), child_source, servers);
        }
    }
}

Result run_copilot_mcp_cli(CopilotState* state,
                           const std::filesystem::path& working_directory,
                           const std::vector<std::string>& arguments,
                           std::string* output) {
    return provider_mcp_run_cli(state->options.executable, arguments,
                                working_directory, output, &state->active_mutex,
                                &state->mcp_process, &state->shutting_down);
}

Result read_copilot_mcp_servers(CopilotState* state,
                                const std::filesystem::path& working_directory,
                                std::vector<McpServer>* servers) {
    std::string output;
    Result result = run_copilot_mcp_cli(state, working_directory,
                                        {"mcp", "list", "--json"}, &output);
    if (result.status == ResultStatus::Error)
        return result;
    const std::size_t json_start = output.find_first_of("[{");
    if (json_start == std::string::npos)
        return result_error("GitHub Copilot returned invalid MCP server data");
    Json entries;
    try {
        entries = Json::parse(output.substr(json_start));
    } catch (...) {
        return result_error("GitHub Copilot returned invalid MCP server data");
    }
    servers->clear();
    append_copilot_mcp_entries(entries, "", servers);
    std::sort(servers->begin(), servers->end(), [](const McpServer& left,
                                                   const McpServer& right) {
        if (left.source != right.source)
            return left.source < right.source;
        return left.name < right.name;
    });
    return result_ok();
}

std::vector<std::string> copilot_add_arguments(const McpServer& server) {
    std::vector<std::string> arguments = {"mcp", "add"};
    if (server.transport == McpServerTransport::Http) {
        arguments.push_back("--transport");
        arguments.push_back("http");
        for (const auto& [key, value] : server.headers) {
            arguments.push_back("--header");
            arguments.push_back(key + ": " + value);
        }
        arguments.push_back(server.name);
        arguments.push_back(server.url);
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

Result remove_copilot_mcp_server(CopilotState* state,
                                 const std::filesystem::path& working_directory,
                                 std::string_view name) {
    std::string output;
    return run_copilot_mcp_cli(state, working_directory,
                               {"mcp", "remove", std::string(name)}, &output);
}

Result list_copilot_mcp_servers(Provider* provider,
                               const std::filesystem::path& working_directory,
                               std::vector<McpServer>* servers) {
    return read_copilot_mcp_servers(static_cast<CopilotState*>(provider->state),
                                    working_directory, servers);
}

Result upsert_copilot_mcp_server(Provider* provider,
                                 const std::filesystem::path& working_directory,
                                 std::string_view existing_name,
                                 const McpServer& server) {
    CopilotState* state = static_cast<CopilotState*>(provider->state);
    if (server.name.empty())
        return result_error("Enter a server name");
    if (server.transport == McpServerTransport::Stdio && server.command.empty())
        return result_error("Enter a command for the local server");
    if (server.transport == McpServerTransport::Http && server.url.empty())
        return result_error("Enter a URL for the HTTP server");

    std::vector<McpServer> configured;
    Result listed = read_copilot_mcp_servers(state, working_directory, &configured);
    if (listed.status == ResultStatus::Error)
        return listed;
    const std::string old_name = existing_name.empty() ? server.name :
                                 std::string(existing_name);
    const auto old_server = std::find_if(configured.begin(), configured.end(),
        [&old_name](const McpServer& value) { return value.name == old_name; });
    if (existing_name.empty() && old_server != configured.end())
        return result_error("A GitHub Copilot MCP server already uses that name");

    const bool replacing = old_server != configured.end();
    const McpServer old_definition = replacing ? *old_server : McpServer{};
    if (replacing) {
        if (!old_server->editable)
            return result_error("This MCP server is managed by another configuration source");
        Result removed = remove_copilot_mcp_server(state, working_directory, old_name);
        if (removed.status == ResultStatus::Error)
            return removed;
    }

    std::string output;
    Result added = run_copilot_mcp_cli(state, working_directory,
                                       copilot_add_arguments(server), &output);
    if (added.status == ResultStatus::Error && replacing) {
        std::string restore_output;
        run_copilot_mcp_cli(state, working_directory,
                            copilot_add_arguments(old_definition), &restore_output);
    }
    return added;
}

Result remove_copilot_mcp(Provider* provider,
                          const std::filesystem::path& working_directory,
                          std::string_view name) {
    return remove_copilot_mcp_server(static_cast<CopilotState*>(provider->state),
                                     working_directory, name);
}

Result set_copilot_mcp_enabled(Provider* provider,
                               const std::filesystem::path& working_directory,
                               std::string_view name, bool enabled) {
    CopilotState* state = static_cast<CopilotState*>(provider->state);
    std::string output;
    return run_copilot_mcp_cli(state, working_directory,
        {"mcp", enabled ? "enable" : "disable", std::string(name)}, &output);
}

Result start_github_copilot(Provider* provider) {
    CopilotState* state = static_cast<CopilotState*>(provider->state);
    provider->default_model = state->options.default_model;
    provider->models.push_back({provider->default_model, "Auto", {}, {}, {}, {}});
    if (state->options.execute == nullptr)
        provider_runtime_set_initialize(&state->runtime, initialize_github_copilot);
    else
        provider->availability = ProviderAvailability::Available;
    Result result = provider_runtime_start(&state->runtime);
    if (result.status == ResultStatus::Error)
        return result;
    if (state->options.execute == nullptr) {
        state->usage_worker = std::thread(run_copilot_usage, state);
        provider->request_usage = request_copilot_usage;
        provider->poll_usage = poll_copilot_usage;
        state->skills_worker = std::thread(run_copilot_skills, state);
        provider->request_skills = request_copilot_skills;
        provider->poll_skills = poll_copilot_skills;
        provider->list_mcp_servers = list_copilot_mcp_servers;
        provider->upsert_mcp_server = upsert_copilot_mcp_server;
        provider->remove_mcp_server = remove_copilot_mcp;
        provider->set_mcp_server_enabled = set_copilot_mcp_enabled;
    }
    return result_ok();
}

Result submit_github_copilot(Provider* provider, TurnRequest request) {
    CopilotState* state = static_cast<CopilotState*>(provider->state);
    const TurnId turn_id = request.turn_id;
    {
        std::lock_guard lock(state->active_mutex);
        state->active_turns.emplace(turn_id, CopilotState::ActiveTurn{});
    }
    Result result = provider_runtime_submit(&state->runtime, std::move(request));
    if (result.status == ResultStatus::Error) {
        std::lock_guard lock(state->active_mutex);
        state->active_turns.erase(turn_id);
    }
    return result;
}

Result respond_github_copilot(Provider*, const ProviderRequestId&, ApprovalDecision) {
    return result_error("GitHub Copilot has no pending approval request");
}

void cancel_github_copilot(Provider* provider, TurnId turn_id) {
    CopilotState* state = static_cast<CopilotState*>(provider->state);
    const bool queued = provider_runtime_cancel_queued(&state->runtime, turn_id);
    std::lock_guard lock(state->active_mutex);
    if (queued) {
        state->active_turns.erase(turn_id);
        return;
    }
    auto active = state->active_turns.find(turn_id);
    if (active == state->active_turns.end())
        return;
    active->second.cancelled = true;
    if (active->second.process != nullptr && child_process_running(active->second.process))
        child_process_terminate(active->second.process);
}

std::vector<Event> poll_github_copilot(Provider* provider) {
    CopilotState* state = static_cast<CopilotState*>(provider->state);
    {
        std::lock_guard lock(state->startup_mutex);
        if (state->startup_complete && !state->startup_applied) {
            provider->availability = state->startup_availability;
            provider->location = state->startup_location;
            provider->default_model = std::move(state->startup_default_model);
            if (!state->startup_models.empty())
                provider->models = std::move(state->startup_models);
            state->startup_applied = true;
        }
    }
    return provider_runtime_poll_events(&state->runtime);
}

void request_github_copilot_shutdown(Provider* provider) {
    CopilotState* state = static_cast<CopilotState*>(provider->state);
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
        if (state->skills_cli_process != nullptr &&
            child_process_running(state->skills_cli_process))
            child_process_terminate(state->skills_cli_process);
        if (state->mcp_process != nullptr && child_process_running(state->mcp_process))
            child_process_terminate(state->mcp_process);
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
    provider_runtime_request_shutdown(&state->runtime);
}

void destroy_github_copilot(Provider* provider) {
    CopilotState* state = static_cast<CopilotState*>(provider->state);
    request_github_copilot_shutdown(provider);
    if (state->usage_worker.joinable())
        state->usage_worker.join();
    if (state->skills_worker.joinable())
        state->skills_worker.join();
    provider_runtime_shutdown(&state->runtime);
    delete state;
    delete provider;
}


ProviderPtr make_github_copilot_provider(const GitHubCopilotOptions* options) {
    CopilotState* state = new CopilotState{};
    if (options != nullptr)
        state->options = *options;
    provider_runtime_init(&state->runtime, process_github_copilot, state);

    Provider* provider = new Provider{"GitHub Copilot", state, start_github_copilot,
                                      submit_github_copilot, respond_github_copilot,
                                      cancel_github_copilot, poll_github_copilot,
                                      destroy_github_copilot};
    provider->request_shutdown = request_github_copilot_shutdown;
    return ProviderPtr(provider, destroy_provider);
}
