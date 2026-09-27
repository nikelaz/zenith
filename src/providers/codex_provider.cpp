#include "provider_runtime.h"
#include <nlohmann/json.hpp>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cwctype>
#include <cwchar>
#include <filesystem>
#include <mutex>
#include <string>
#include <csignal>
#include <system_error>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <utility>

using Json = nlohmann::json;

namespace {
std::string string_value(const Json& value, const char* key) {
    return value.contains(key) && value[key].is_string() ? value[key].get<std::string>()
                                                         : std::string{};
}

std::filesystem::path resolve_executable(const std::filesystem::path& executable) {
    if (executable.empty())
        return {};

#ifdef _WIN32
    auto executable_path = [](const std::filesystem::path& path) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error) || error)
            return std::filesystem::path{};
        const std::filesystem::path resolved = std::filesystem::canonical(path, error);
        return error ? path : resolved;
    };

    if (executable.has_parent_path())
        return executable_path(executable);

    const std::filesystem::path extension = executable.extension();
    const wchar_t* extensions[] = {nullptr, L".exe", L".cmd", L".bat"};
    const std::size_t extension_count = extension.empty() ? 4 : 1;
    for (std::size_t index = 0; index < extension_count; ++index) {
        std::wstring resolved(32768, L'\0');
        const DWORD length = SearchPathW(nullptr, executable.c_str(), extensions[index],
                                         static_cast<DWORD>(resolved.size()), resolved.data(),
                                         nullptr);
        if (length == 0 || length >= resolved.size())
            continue;
        resolved.resize(length);
        const std::filesystem::path path(resolved);
        const std::filesystem::path canonical = executable_path(path);
        if (!canonical.empty())
            return canonical;
    }
    return {};
#else
    auto executable_path = [](const std::filesystem::path& path) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error) || error ||
            access(path.c_str(), X_OK) != 0)
            return std::filesystem::path{};
        const std::filesystem::path resolved = std::filesystem::canonical(path, error);
        return error ? path : resolved;
    };

    if (executable.has_parent_path())
        return executable_path(executable);

    const char* path_value = std::getenv("PATH");
    if (path_value == nullptr)
        return {};

    const std::string search_path(path_value);
    std::size_t start = 0;
    while (start <= search_path.size()) {
        const std::size_t end = search_path.find(':', start);
        const std::string directory = search_path.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        const std::filesystem::path candidate =
            (directory.empty() ? std::filesystem::path(".") : std::filesystem::path(directory)) /
            executable;
        const std::filesystem::path resolved = executable_path(candidate);
        if (!resolved.empty())
            return resolved;
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return {};
#endif
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

struct CodexProcess;

struct CodexState {
    ProviderRuntime runtime;
    CodexOptions options;
    std::mutex active_mutex;
    CodexProcess* active_process = nullptr;
    CodexProcess* startup_process = nullptr;
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

struct CodexProcess {
#ifdef _WIN32
    HANDLE process = nullptr;
    DWORD pid = 0;
#else
    pid_t pid = -1;
#endif
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

Result start_codex_process(const CodexOptions* options, CodexProcess* process) {
#ifdef _WIN32
    SECURITY_ATTRIBUTES security_attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE child_input = nullptr;
    HANDLE parent_input = nullptr;
    HANDLE parent_output = nullptr;
    HANDLE child_output = nullptr;
    HANDLE child_error = nullptr;
    if (!CreatePipe(&child_input, &parent_input, &security_attributes, 0) ||
        !CreatePipe(&parent_output, &child_output, &security_attributes, 0)) {
        if (child_input != nullptr)
            CloseHandle(child_input);
        if (parent_input != nullptr)
            CloseHandle(parent_input);
        if (parent_output != nullptr)
            CloseHandle(parent_output);
        if (child_output != nullptr)
            CloseHandle(child_output);
        return result_error("Failed to create Codex app-server pipes");
    }
    if (!SetHandleInformation(parent_input, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(parent_output, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(child_input);
        CloseHandle(parent_input);
        CloseHandle(parent_output);
        CloseHandle(child_output);
        return result_error("Failed to prepare Codex app-server pipes");
    }

    child_error = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &security_attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (child_error == INVALID_HANDLE_VALUE) {
        CloseHandle(child_input);
        CloseHandle(parent_input);
        CloseHandle(parent_output);
        CloseHandle(child_output);
        return result_error("Failed to prepare Codex app-server output");
    }

    const std::filesystem::path executable = options->executable;
    std::wstring extension = executable.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
    const bool is_script = extension == L".cmd" || extension == L".bat";
    std::wstring application;
    std::wstring command_line;
    if (is_script) {
        application = L"cmd.exe";
        command_line = L"cmd.exe /D /S /C \"\"" + executable.wstring() +
                       L"\" app-server\"";
    } else {
        application = executable.wstring();
        command_line = L"\"" + application + L"\" app-server";
    }
    std::vector<wchar_t> writable_command_line(command_line.begin(), command_line.end());
    writable_command_line.push_back(L'\0');

    std::vector<wchar_t> environment;
    DWORD creation_flags = CREATE_NO_WINDOW;
    if (!options->codex_home.empty()) {
        LPWCH inherited_environment = GetEnvironmentStringsW();
        std::vector<std::wstring> entries;
        if (inherited_environment != nullptr) {
            for (const wchar_t* entry = inherited_environment; *entry != L'\0';
                 entry += std::wcslen(entry) + 1) {
                entries.emplace_back(entry);
            }
            FreeEnvironmentStringsW(inherited_environment);
        }

        const std::wstring codex_home = options->codex_home.wstring();
        const std::wstring codex_home_entry = L"CODEX_HOME=" + codex_home;
        bool replaced = false;
        for (std::wstring& entry : entries) {
            const std::size_t name_start = !entry.empty() && entry[0] == L'=' ? 1 : 0;
            const std::size_t name_end = entry.find(L'=', name_start);
            if (name_end != std::wstring::npos &&
                _wcsicmp(entry.substr(0, name_end).c_str(), L"CODEX_HOME") == 0) {
                entry = codex_home_entry;
                replaced = true;
                break;
            }
        }
        if (!replaced)
            entries.push_back(codex_home_entry);
        std::sort(entries.begin(), entries.end(), [](const std::wstring& left,
                                                     const std::wstring& right) {
            return _wcsicmp(left.c_str(), right.c_str()) < 0;
        });
        for (const std::wstring& entry : entries) {
            environment.insert(environment.end(), entry.begin(), entry.end());
            environment.push_back(L'\0');
        }
        environment.push_back(L'\0');
        creation_flags |= CREATE_UNICODE_ENVIRONMENT;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = child_input;
    startup.hStdOutput = child_output;
    startup.hStdError = child_error;
    PROCESS_INFORMATION child{};
    const BOOL started = CreateProcessW(
        application.c_str(), writable_command_line.data(), nullptr, nullptr, TRUE,
        creation_flags, environment.empty() ? nullptr : environment.data(), nullptr,
        &startup, &child);
    CloseHandle(child_input);
    CloseHandle(child_output);
    CloseHandle(child_error);
    if (!started) {
        CloseHandle(parent_input);
        CloseHandle(parent_output);
        return result_error("Failed to start Codex app-server");
    }

    CloseHandle(child.hThread);
    process->process = child.hProcess;
    process->pid = child.dwProcessId;
    const int input_fd = _open_osfhandle(
        reinterpret_cast<intptr_t>(parent_input), _O_WRONLY | _O_TEXT);
    if (input_fd < 0) {
        CloseHandle(parent_input);
        CloseHandle(parent_output);
        return result_error("Failed to connect to Codex app-server");
    }
    process->input = _fdopen(input_fd, "w");
    if (process->input == nullptr) {
        _close(input_fd);
        CloseHandle(parent_output);
        return result_error("Failed to connect to Codex app-server");
    }
    const int output_fd = _open_osfhandle(
        reinterpret_cast<intptr_t>(parent_output), _O_RDONLY | _O_TEXT);
    if (output_fd < 0) {
        CloseHandle(parent_output);
        return result_error("Failed to connect to Codex app-server");
    }
    process->output = _fdopen(output_fd, "r");
    if (process->output == nullptr) {
        _close(output_fd);
        return result_error("Failed to connect to Codex app-server");
    }
    setvbuf(process->input, nullptr, _IOLBF, 0);
    return result_ok();
#else
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
#endif
}

bool codex_process_running(const CodexProcess* process) {
#ifdef _WIN32
    return process->process != nullptr;
#else
    return process->pid > 0;
#endif
}

void terminate_codex_process(CodexProcess* process) {
#ifdef _WIN32
    if (process->process != nullptr)
        TerminateProcess(process->process, 1);
#else
    if (process->pid > 0)
        kill(process->pid, SIGTERM);
#endif
}

void stop_codex_process(CodexProcess* process) {
    terminate_codex_process(process);
    if (process->input != nullptr) {
        fclose(process->input);
        process->input = nullptr;
    }
    if (process->output != nullptr) {
        fclose(process->output);
        process->output = nullptr;
    }
#ifdef _WIN32
    if (process->process != nullptr) {
        WaitForSingleObject(process->process, INFINITE);
        CloseHandle(process->process);
        process->process = nullptr;
        process->pid = 0;
    }
#else
    if (process->pid > 0) {
        int status = 0;
        while (waitpid(process->pid, &status, 0) < 0 && errno == EINTR) {
        }
        process->pid = -1;
    }
#endif
}

void force_stop_codex_process(CodexProcess* process) {
    stop_codex_process(process);
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

void ignore_sigpipe() {
#ifndef _WIN32
    static const bool ignored = [] {
        std::signal(SIGPIPE, SIG_IGN);
        return true;
    }();
    (void)ignored;
#endif
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

std::vector<ModelOption> fetch_codex_models(CodexState* state,
                                            ProviderRuntime* runtime,
                                            ProviderAvailability* availability) {
    ignore_sigpipe();

    std::vector<ModelOption> models;
    *availability = ProviderAvailability::Unavailable;
    CodexProcess process;
    if (start_codex_process(&state->options, &process).status == ResultStatus::Error) {
        force_stop_codex_process(&process);
        return models;
    }
    {
        std::lock_guard lock(state->active_mutex);
        state->startup_process = &process;
        if (state->shutting_down)
            terminate_codex_process(&process);
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
    stop_codex_process(&process);
    return models;
}

void initialize_codex(void* context, ProviderRuntime* runtime) {
    CodexState* state = static_cast<CodexState*>(context);
    ProviderAvailability availability = ProviderAvailability::Unavailable;
    std::filesystem::path location;
    std::vector<ModelOption> models;
    if (state->options.execute == nullptr) {
        location = resolve_executable(state->options.executable);
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
    ignore_sigpipe();

    CodexProcess process;
    Result result = start_codex_process(options, &process);
    if (result.status == ResultStatus::Error) {
        force_stop_codex_process(&process);
        return result;
    }

    {
        std::lock_guard lock(state->active_mutex);
        state->active_process = &process;
        if (state->cancel_requested || state->shutting_down)
            terminate_codex_process(&process);
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
    provider->models.push_back({state->options.default_model,
                                state->options.default_model, {}, {}});
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
    if (state->active_process != nullptr && codex_process_running(state->active_process))
        terminate_codex_process(state->active_process);
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
        if (state->active_process != nullptr && codex_process_running(state->active_process))
            terminate_codex_process(state->active_process);
        if (state->startup_process != nullptr && codex_process_running(state->startup_process))
            terminate_codex_process(state->startup_process);
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
