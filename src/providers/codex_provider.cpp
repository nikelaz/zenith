#include "provider_runtime.h"
#include "../process/child-process.h"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <filesystem>
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
    ChildProcess* active_process = nullptr;
    ChildProcess* startup_process = nullptr;
    TurnId active_turn_id = 0;
    bool cancel_requested = false;
    bool shutting_down = false;
    std::mutex startup_mutex;
    bool startup_complete = false;
    bool startup_applied = false;
    ProviderAvailability startup_availability = ProviderAvailability::Unknown;
    std::filesystem::path startup_location;
    std::vector<ModelOption> startup_models;
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

Result start_codex_process(const CodexOptions* options, ChildProcess* process) {
    std::vector<std::string> arguments = {"app-server"};
    std::vector<ChildProcessEnvironmentVariable> environment;
    if (!options->codex_home.empty())
        environment.push_back({"CODEX_HOME", options->codex_home});
    return child_process_start(process, options->executable, arguments, "Codex app-server",
                               environment);
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

void initialize_codex(void* context, ProviderRuntime* runtime) {
    CodexState* state = static_cast<CodexState*>(context);
    ProviderAvailability availability = ProviderAvailability::Unavailable;
    std::filesystem::path location;
    std::vector<ModelOption> models;
    if (state->options.execute == nullptr) {
        location = child_process_resolve_executable(state->options.executable);
        if (!location.empty()) {
            state->options.executable = location;
            models = fetch_codex_models(state, runtime, &availability);
        }
    } else {
        availability = ProviderAvailability::Available;
    }

    std::lock_guard lock(state->startup_mutex);
    state->startup_availability = availability;
    state->startup_location = std::move(location);
    state->startup_models = std::move(models);
    state->startup_complete = true;
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
        state->active_process = &process;
        if (state->cancel_requested || state->shutting_down)
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
        if (state->active_process == &process)
            state->active_process = nullptr;
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
        cancelled = state->active_turn_id == request->turn_id && state->cancel_requested;
        if (state->active_turn_id == request->turn_id) {
            state->active_turn_id = 0;
            state->active_process = nullptr;
            state->cancel_requested = false;
        }
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
    return provider_runtime_start(&state->runtime);
}

Result submit_codex(Provider* provider, TurnRequest request) {
    CodexState* state = static_cast<CodexState*>(provider->state);
    const TurnId turn_id = request.turn_id;
    {
        std::lock_guard lock(state->active_mutex);
        state->active_turn_id = turn_id;
        state->cancel_requested = false;
    }
    Result result = provider_runtime_submit(&state->runtime, std::move(request));
    if (result.status == ResultStatus::Error) {
        std::lock_guard lock(state->active_mutex);
        if (state->active_turn_id == turn_id) {
            state->active_turn_id = 0;
            state->cancel_requested = false;
        }
    }
    return result;
}

Result respond_codex(Provider*, const ProviderRequestId&, ApprovalDecision) {
    return result_error("Provider has no pending approval request");
}

void cancel_codex(Provider* provider, TurnId turn_id) {
    CodexState* state = static_cast<CodexState*>(provider->state);
    std::lock_guard lock(state->active_mutex);
    if (turn_id == 0 || state->active_turn_id != turn_id)
        return;
    state->cancel_requested = true;
    if (state->active_process != nullptr && child_process_running(state->active_process))
        child_process_terminate(state->active_process);
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
        if (state->active_process != nullptr && child_process_running(state->active_process))
            child_process_terminate(state->active_process);
        if (state->startup_process != nullptr && child_process_running(state->startup_process))
            child_process_terminate(state->startup_process);
    }
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
