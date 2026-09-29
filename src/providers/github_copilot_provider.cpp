#include "provider_runtime.h"
#include "../process/child-process.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using Json = nlohmann::json;

namespace {
std::string string_value(const Json& value, const char* key) {
    return value.contains(key) && value[key].is_string() ? value[key].get<std::string>()
                                                         : std::string{};
}

std::string json_text(const Json& value) {
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
    bool shutting_down = false;
    std::mutex startup_mutex;
    bool startup_complete = false;
    bool startup_applied = false;
    ProviderAvailability startup_availability = ProviderAvailability::Unknown;
    std::filesystem::path startup_location;
    std::string startup_default_model;
    std::vector<ModelOption> startup_models;
};

struct StreamContext {
    ProviderRuntime* runtime;
    const TurnRequest* request;
};

std::string utc_timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
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

    std::vector<std::string> arguments = {"--acp", "--stdio"};
    if (!model.empty())
        arguments.push_back("--model=" + model);
    if (!effort.empty())
        arguments.push_back("--effort=" + effort);
    return child_process_start(process, options->executable, arguments,
                               "GitHub Copilot ACP server", {}, error_output_path);
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

std::string tool_content_text(const Json& content) {
    if (!content.is_array())
        return json_text(content);

    std::string result;
    for (const Json& item : content) {
        if (!result.empty())
            result += '\n';
        if (item.contains("content") && item["content"].is_object())
            result += string_value(item["content"], "text");
        else if (item.contains("text") && item["text"].is_string())
            result += string_value(item, "text");
        else
            result += json_text(item);
    }
    return result;
}

Event make_tool_activity_event(StreamContext* context, const Json& update) {
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
        event.tool_arguments = json_text(update["rawInput"]);
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

void emit_text_event(StreamContext* context, EventKind kind, const Json& content) {
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
        return write_message(process->input,
                             Json{{"jsonrpc", "2.0"}, {"id", message.value("id", Json())},
                                  {"result", {{"outcome", {{"outcome", "cancelled"}}}}}});

    return write_message(process->input,
                         Json{{"jsonrpc", "2.0"}, {"id", message.value("id", Json())},
                              {"result", {{"outcome", {{"outcome", "selected"},
                                                        {"optionId", option_id}}}}}});
}

void handle_server_message(ChildProcess* process, StreamContext* context,
                           const Json& message) {
    const std::string method = string_value(message, "method");
    if (method == "session/request_permission") {
        respond_to_permission(process, message);
        return;
    }
    if (method != "session/update" || context->request == nullptr)
        return;

    const Json update = message.value("params", Json::object()).value("update", Json::object());
    const std::string update_type = string_value(update, "sessionUpdate");
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

bool wait_for_response(ChildProcess* process, int request_id, StreamContext* context,
                       Json* response, std::string* error) {
    for (;;) {
        Json message;
        if (!read_message(process->output, &message)) {
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

bool initialize_copilot(ChildProcess* process, StreamContext* context, Json* response,
                        std::string* error) {
    const Json initialize = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "initialize"},
        {"params", {{"protocolVersion", 1},
                    {"clientCapabilities", {{"session", {{"configOptions", Json::object()}}}}},
                    {"clientInfo", {{"name", "Zenith"}, {"version", "0.1.0"}}}}},
    };
    return write_message(process->input, initialize) &&
           wait_for_response(process, 1, context, response, error);
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

bool create_copilot_session(ChildProcess* process, StreamContext* context,
                            const std::filesystem::path& working_directory, Json* response,
                            std::string* error) {
    const Json create = {
        {"jsonrpc", "2.0"},
        {"id", 2},
        {"method", "session/new"},
        {"params", {{"cwd", working_directory.string()}, {"mcpServers", Json::array()}}},
    };
    return write_message(process->input, create) &&
           wait_for_response(process, 2, context, response, error);
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
    StreamContext context{runtime, nullptr};
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

void initialize_github_copilot(void* context, ProviderRuntime*) {
    CopilotState* state = static_cast<CopilotState*>(context);
    ProviderAvailability availability = ProviderAvailability::Unavailable;
    std::filesystem::path location = child_process_resolve_executable(state->options.executable);
    std::string default_model = state->options.default_model;
    std::vector<ModelOption> models;
    if (!location.empty()) {
        state->options.executable = location;
        discover_copilot_models(state, &state->runtime, &availability, &models,
                                &default_model);
    }

    std::lock_guard lock(state->startup_mutex);
    state->startup_availability = availability;
    state->startup_location = std::move(location);
    state->startup_default_model = std::move(default_model);
    state->startup_models = std::move(models);
    state->startup_complete = true;
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

Json copilot_prompt_content(const TurnRequest* request, bool embedded_context) {
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

    StreamContext context{runtime, request};
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
        success = write_message(process.input, prompt);
        if (!success)
            error = "Failed to write the GitHub Copilot ACP prompt request";
        if (success)
            success = wait_for_response(&process, 3, &context, &response, &error);
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

Result start_github_copilot(Provider* provider) {
    CopilotState* state = static_cast<CopilotState*>(provider->state);
    provider->default_model = state->options.default_model;
    provider->models.push_back({provider->default_model, "Auto", {}, {}, {}, {}});
    if (state->options.execute == nullptr)
        provider_runtime_set_initialize(&state->runtime, initialize_github_copilot);
    else
        provider->availability = ProviderAvailability::Available;
    return provider_runtime_start(&state->runtime);
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
            provider->location = std::move(state->startup_location);
            provider->default_model = std::move(state->startup_default_model);
            if (!state->startup_models.empty())
                provider->models = std::move(state->startup_models);
            state->startup_applied = true;
        }
    }
    return provider_runtime_poll_events(&state->runtime);
}

void destroy_github_copilot(Provider* provider) {
    CopilotState* state = static_cast<CopilotState*>(provider->state);
    {
        std::lock_guard lock(state->active_mutex);
        state->shutting_down = true;
        for (auto& [id, active] : state->active_turns)
            if (active.process != nullptr && child_process_running(active.process))
                child_process_terminate(active.process);
        if (state->startup_process != nullptr && child_process_running(state->startup_process))
            child_process_terminate(state->startup_process);
    }
    provider_runtime_shutdown(&state->runtime);
    delete state;
    delete provider;
}
} // namespace

ProviderPtr make_github_copilot_provider(const GitHubCopilotOptions* options) {
    CopilotState* state = new CopilotState{};
    if (options != nullptr)
        state->options = *options;
    provider_runtime_init(&state->runtime, process_github_copilot, state);

    Provider* provider = new Provider{"GitHub Copilot", state, start_github_copilot,
                                      submit_github_copilot, respond_github_copilot,
                                      cancel_github_copilot, poll_github_copilot,
                                      destroy_github_copilot};
    return ProviderPtr(provider, destroy_provider);
}
