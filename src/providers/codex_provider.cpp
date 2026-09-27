#include "provider_runtime.h"
#include <nlohmann/json.hpp>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

using Json = nlohmann::json;

namespace {
std::string string_value(const Json& value, const char* key) {
    return value.contains(key) && value[key].is_string() ? value[key].get<std::string>()
                                                         : std::string{};
}

struct CodexProcess;

struct CodexState {
    ProviderRuntime runtime;
    CodexOptions options;
    std::mutex active_mutex;
    CodexProcess* active_process = nullptr;
    TurnId active_turn_id = 0;
    bool cancel_requested = false;
};

struct CodexProcess {
    pid_t pid = -1;
    FILE* input = nullptr;
    FILE* output = nullptr;
};

struct StreamContext {
    ProviderRuntime* runtime;
    const TurnRequest* request;
    bool turn_finished = false;
    bool turn_failed = false;
    std::string error;
};

Result start_codex_process(const CodexOptions* options, CodexProcess* process) {
    int input_pipe[2];
    int output_pipe[2];
    if (pipe(input_pipe) != 0)
        return result_error("Failed to create Codex app-server pipes");
    if (pipe(output_pipe) != 0) {
        close(input_pipe[0]);
        close(input_pipe[1]);
        return result_error("Failed to create Codex app-server pipes");
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(input_pipe[0]);
        close(input_pipe[1]);
        close(output_pipe[0]);
        close(output_pipe[1]);
        return result_error("Failed to start Codex app-server");
    }

    if (pid == 0) {
        dup2(input_pipe[0], STDIN_FILENO);
        dup2(output_pipe[1], STDOUT_FILENO);
        close(input_pipe[0]);
        close(input_pipe[1]);
        close(output_pipe[0]);
        close(output_pipe[1]);
        if (!options->codex_home.empty())
            setenv("CODEX_HOME", options->codex_home.c_str(), 1);

        const std::string executable = options->executable.string();
        execlp(executable.c_str(), executable.c_str(), "app-server", nullptr);
        _exit(127);
    }

    close(input_pipe[0]);
    close(output_pipe[1]);
    process->pid = pid;
    process->input = fdopen(input_pipe[1], "w");
    process->output = fdopen(output_pipe[0], "r");
    if (process->input == nullptr || process->output == nullptr)
        return result_error("Failed to connect to Codex app-server");
    setvbuf(process->input, nullptr, _IOLBF, 0);
    return result_ok();
}

void stop_codex_process(CodexProcess* process) {
    if (process->pid > 0)
        kill(process->pid, SIGTERM);
    if (process->input != nullptr) {
        fclose(process->input);
        process->input = nullptr;
    }
    if (process->output != nullptr) {
        fclose(process->output);
        process->output = nullptr;
    }
    if (process->pid > 0) {
        int status = 0;
        while (waitpid(process->pid, &status, 0) < 0 && errno == EINTR) {
        }
        process->pid = -1;
    }
}

void force_stop_codex_process(CodexProcess* process) {
    if (process->pid > 0)
        kill(process->pid, SIGTERM);
    stop_codex_process(process);
}

bool write_message(FILE* input, const Json& message) {
    const std::string serialized = message.dump();
    return fputs(serialized.c_str(), input) >= 0 && fputc('\n', input) != EOF &&
           fflush(input) == 0;
}

bool read_message(FILE* output, Json* message) {
    char* line = nullptr;
    std::size_t capacity = 0;
    const ssize_t length = getline(&line, &capacity, output);
    if (length < 0) {
        free(line);
        return false;
    }

    try {
        *message = Json::parse(line, line + length);
    } catch (...) {
        free(line);
        return false;
    }
    free(line);
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
        if (string_value(item, "type") == "commandExecution") {
            const Event event{EventKind::ToolActivity,
                              context->request->conversation_id,
                              context->request->turn_id,
                              string_value(item, "command"), {}, {},
                              string_value(item, "id"), string_value(item, "cwd"), {}, -1, false,
                              false,
                              string_value(item, "status").empty()
                                  ? "inProgress" : string_value(item, "status"),
                              -1};
            provider_runtime_emit(context->runtime, &event);
        }
    } else if (method == "item/completed") {
        const Json item = params.value("item", Json::object());
        if (string_value(item, "type") == "commandExecution") {
            const Event event{EventKind::ToolActivity,
                              context->request->conversation_id,
                              context->request->turn_id,
                              string_value(item, "command"), {}, {},
                              string_value(item, "id"), string_value(item, "cwd"),
                              string_value(item, "aggregatedOutput"),
                              item.contains("exitCode") && item["exitCode"].is_number_integer()
                                  ? item["exitCode"].get<int>() : -1,
                              true, false,
                              string_value(item, "status").empty()
                                  ? "completed" : string_value(item, "status"),
                              item.contains("durationMs") && item["durationMs"].is_number_integer()
                                  ? item["durationMs"].get<int>() : -1};
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

bool wait_for_response(CodexProcess* process, int request_id, StreamContext* stream,
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

std::vector<ModelOption> fetch_codex_models(const CodexOptions* options,
                                            ProviderRuntime* runtime) {
    static const bool ignore_sigpipe = [] {
        std::signal(SIGPIPE, SIG_IGN);
        return true;
    }();
    (void)ignore_sigpipe;

    std::vector<ModelOption> models;
    CodexProcess process;
    if (start_codex_process(options, &process).status == ResultStatus::Error) {
        force_stop_codex_process(&process);
        return models;
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

    stop_codex_process(&process);
    return models;
}

Result run_codex(CodexState* state, const TurnRequest* request,
                 ProviderRuntime* runtime) {
    const CodexOptions* options = &state->options;
    static const bool ignore_sigpipe = [] {
        std::signal(SIGPIPE, SIG_IGN);
        return true;
    }();
    (void)ignore_sigpipe;

    CodexProcess process;
    Result result = start_codex_process(options, &process);
    if (result.status == ResultStatus::Error) {
        force_stop_codex_process(&process);
        return result;
    }

    {
        std::lock_guard lock(state->active_mutex);
        state->active_process = &process;
        if (state->cancel_requested)
            kill(process.pid, SIGTERM);
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
            {"input", Json::array({{{"type", "text"}, {"text", conversation_prompt(request)}}})},
        };
        if (!request->model.empty())
            turn_params["model"] = request->model;
        if (!request->reasoning_effort.empty())
            turn_params["effort"] = request->reasoning_effort;
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
    stop_codex_process(&process);
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
    if (state->options.execute == nullptr)
        provider->models = fetch_codex_models(&state->options, &state->runtime);
    if (provider->models.empty())
        provider->models.push_back({state->options.default_model,
                                    state->options.default_model, {}, {}});
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
    if (state->active_process != nullptr && state->active_process->pid > 0)
        kill(state->active_process->pid, SIGTERM);
}

std::vector<Event> poll_codex(Provider* provider) {
    CodexState* state = static_cast<CodexState*>(provider->state);
    return provider_runtime_poll_events(&state->runtime);
}

void destroy_codex(Provider* provider) {
    CodexState* state = static_cast<CodexState*>(provider->state);
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
