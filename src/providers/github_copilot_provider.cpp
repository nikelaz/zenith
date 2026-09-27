#include "provider_runtime.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cwctype>
#include <cstdlib>
#include <csignal>
#include <filesystem>
#include <mutex>
#include <string>
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
    const wchar_t* extensions[] = {L".exe", L".cmd", L".bat", nullptr};
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

struct CopilotProcess {
#ifdef _WIN32
    HANDLE process = nullptr;
    DWORD pid = 0;
#else
    pid_t pid = -1;
#endif
    FILE* input = nullptr;
    FILE* output = nullptr;
};

struct CopilotState {
    ProviderRuntime runtime;
    GitHubCopilotOptions options;
    std::mutex active_mutex;
    CopilotProcess* active_process = nullptr;
    CopilotProcess* startup_process = nullptr;
    TurnId active_turn_id = 0;
    bool cancel_requested = false;
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

Result start_copilot_process(const GitHubCopilotOptions* options, const std::string& model,
                             const std::string& effort, CopilotProcess* process) {
    if (model.find_first_of("\"\\%!&|<>^\r\n") != std::string::npos ||
        effort.find_first_of("\"\\%!&|<>^\r\n") != std::string::npos)
        return result_error("Invalid GitHub Copilot model or effort identifier");
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
        return result_error("Failed to create GitHub Copilot ACP server pipes");
    }
    if (!SetHandleInformation(parent_input, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(parent_output, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(child_input);
        CloseHandle(parent_input);
        CloseHandle(parent_output);
        CloseHandle(child_output);
        return result_error("Failed to prepare GitHub Copilot ACP server pipes");
    }

    child_error = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &security_attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (child_error == INVALID_HANDLE_VALUE) {
        CloseHandle(child_input);
        CloseHandle(parent_input);
        CloseHandle(parent_output);
        CloseHandle(child_output);
        return result_error("Failed to prepare GitHub Copilot ACP server output");
    }

    const std::filesystem::path executable = options->executable;
    std::wstring extension = executable.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t character) {
                       return static_cast<wchar_t>(std::towlower(character));
                   });
    const bool is_script = extension == L".cmd" || extension == L".bat";
    std::wstring arguments = L" --acp --stdio";
    if (!model.empty())
        arguments += L" \"--model=" + std::filesystem::path(model).wstring() + L"\"";
    if (!effort.empty())
        arguments += L" \"--effort=" + std::filesystem::path(effort).wstring() + L"\"";
    std::wstring application;
    std::wstring command_line;
    if (is_script) {
        application = L"cmd.exe";
        command_line = L"cmd.exe /D /S /C \"\"" + executable.wstring() +
                       L"\"" + arguments + L"\"";
    } else {
        application = executable.wstring();
        command_line = L"\"" + application + L"\"" + arguments;
    }
    std::vector<wchar_t> writable_command_line(command_line.begin(), command_line.end());
    writable_command_line.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = child_input;
    startup.hStdOutput = child_output;
    startup.hStdError = child_error;
    PROCESS_INFORMATION child{};
    const BOOL started = CreateProcessW(
        application.c_str(), writable_command_line.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr,
        &startup, &child);
    CloseHandle(child_input);
    CloseHandle(child_output);
    CloseHandle(child_error);
    if (!started) {
        CloseHandle(parent_input);
        CloseHandle(parent_output);
        return result_error("Failed to start GitHub Copilot ACP server");
    }

    CloseHandle(child.hThread);
    process->process = child.hProcess;
    process->pid = child.dwProcessId;
    const int input_fd = _open_osfhandle(
        reinterpret_cast<std::intptr_t>(parent_input), _O_WRONLY | _O_TEXT);
    if (input_fd < 0) {
        CloseHandle(parent_input);
        CloseHandle(parent_output);
        return result_error("Failed to connect to GitHub Copilot ACP server");
    }
    process->input = _fdopen(input_fd, "w");
    if (process->input == nullptr) {
        _close(input_fd);
        CloseHandle(parent_output);
        return result_error("Failed to connect to GitHub Copilot ACP server");
    }
    const int output_fd = _open_osfhandle(
        reinterpret_cast<std::intptr_t>(parent_output), _O_RDONLY | _O_TEXT);
    if (output_fd < 0) {
        CloseHandle(parent_output);
        return result_error("Failed to connect to GitHub Copilot ACP server");
    }
    process->output = _fdopen(output_fd, "r");
    if (process->output == nullptr) {
        _close(output_fd);
        return result_error("Failed to connect to GitHub Copilot ACP server");
    }
    setvbuf(process->input, nullptr, _IOLBF, 0);
    return result_ok();
#else
    std::vector<std::string> arguments = {options->executable.string(), "--acp", "--stdio"};
    if (!model.empty())
        arguments.push_back("--model=" + model);
    if (!effort.empty())
        arguments.push_back("--effort=" + effort);

    std::vector<char*> argument_values;
    argument_values.reserve(arguments.size() + 1);
    for (std::string& argument : arguments)
        argument_values.push_back(argument.data());
    argument_values.push_back(nullptr);

    int input_pipe[2];
    int output_pipe[2];
    if (pipe(input_pipe) != 0)
        return result_error("Failed to create GitHub Copilot ACP pipes");
    if (pipe(output_pipe) != 0) {
        close(input_pipe[0]);
        close(input_pipe[1]);
        return result_error("Failed to create GitHub Copilot ACP pipes");
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(input_pipe[0]);
        close(input_pipe[1]);
        close(output_pipe[0]);
        close(output_pipe[1]);
        return result_error("Failed to start GitHub Copilot ACP server");
    }

    if (pid == 0) {
        dup2(input_pipe[0], STDIN_FILENO);
        dup2(output_pipe[1], STDOUT_FILENO);
        close(input_pipe[0]);
        close(input_pipe[1]);
        close(output_pipe[0]);
        close(output_pipe[1]);
        execvp(argument_values[0], argument_values.data());
        _exit(127);
    }

    close(input_pipe[0]);
    close(output_pipe[1]);
    process->pid = pid;
    process->input = fdopen(input_pipe[1], "w");
    process->output = fdopen(output_pipe[0], "r");
    if (process->input == nullptr || process->output == nullptr) {
        if (process->input == nullptr)
            close(input_pipe[1]);
        if (process->output == nullptr)
            close(output_pipe[0]);
        return result_error("Failed to connect to GitHub Copilot ACP server");
    }
    setvbuf(process->input, nullptr, _IOLBF, 0);
    return result_ok();
#endif
}

bool copilot_process_running(const CopilotProcess* process) {
#ifdef _WIN32
    return process->process != nullptr;
#else
    return process->pid > 0;
#endif
}

void terminate_copilot_process(CopilotProcess* process) {
#ifdef _WIN32
    if (process->process != nullptr)
        TerminateProcess(process->process, 1);
#else
    if (process->pid > 0)
        kill(process->pid, SIGTERM);
#endif
}

void stop_copilot_process(CopilotProcess* process) {
    terminate_copilot_process(process);
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

bool respond_to_permission(CopilotProcess* process, const Json& message) {
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

void handle_server_message(CopilotProcess* process, StreamContext* context,
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

bool wait_for_response(CopilotProcess* process, int request_id, StreamContext* context,
                       Json* response, std::string* error) {
    for (;;) {
        Json message;
        if (!read_message(process->output, &message)) {
            *error = "GitHub Copilot ACP server closed its output";
            return false;
        }
        if (message.contains("id") && message["id"].is_number_integer() &&
            message["id"].get<int>() == request_id) {
            if (message.contains("error")) {
                *error = string_value(message["error"], "message");
                if (error->empty() && message["error"].is_string())
                    *error = message["error"].get<std::string>();
                if (error->empty())
                    *error = "GitHub Copilot ACP request failed";
                return false;
            }
            if (response != nullptr)
                *response = std::move(message);
            return true;
        }
        handle_server_message(process, context, message);
    }
}

bool initialize_copilot(CopilotProcess* process, StreamContext* context, std::string* error) {
    const Json initialize = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "initialize"},
        {"params", {{"protocolVersion", 1},
                    {"clientCapabilities", {{"session", {{"configOptions", Json::object()}}}}},
                    {"clientInfo", {{"name", "Zenith"}, {"version", "0.1.0"}}}}},
    };
    return write_message(process->input, initialize) &&
           wait_for_response(process, 1, context, nullptr, error);
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

bool create_copilot_session(CopilotProcess* process, StreamContext* context,
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
            models->push_back({id, name.empty() ? id : name, {}, {}});
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
    ignore_sigpipe();

    CopilotProcess process;
    StreamContext context{runtime, nullptr};
    std::string error;
    *availability = ProviderAvailability::Unavailable;
    Result result = start_copilot_process(&state->options, {}, {}, &process);
    if (result.status == ResultStatus::Error) {
        stop_copilot_process(&process);
        return false;
    }

    {
        std::lock_guard lock(state->active_mutex);
        state->startup_process = &process;
        if (state->shutting_down)
            terminate_copilot_process(&process);
    }

    bool success = initialize_copilot(&process, &context, &error);
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
    stop_copilot_process(&process);
    return success;
}

void initialize_github_copilot(void* context, ProviderRuntime*) {
    CopilotState* state = static_cast<CopilotState*>(context);
    ProviderAvailability availability = ProviderAvailability::Unavailable;
    std::filesystem::path location = resolve_executable(state->options.executable);
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

Result run_github_copilot(CopilotState* state, const TurnRequest* request,
                          ProviderRuntime* runtime) {
    ignore_sigpipe();

    const std::string model = request->model.empty() ? state->options.default_model : request->model;
    CopilotProcess process;
    Result result = start_copilot_process(&state->options, model, request->reasoning_effort,
                                          &process);
    if (result.status == ResultStatus::Error) {
        stop_copilot_process(&process);
        return result;
    }
    {
        std::lock_guard lock(state->active_mutex);
        state->active_process = &process;
        if (state->cancel_requested || state->shutting_down)
            terminate_copilot_process(&process);
    }

    StreamContext context{runtime, request};
    std::string error;
    bool success = initialize_copilot(&process, &context, &error);
    std::error_code path_error;
    const std::filesystem::path cwd = session_working_directory(request, &path_error);
    if (path_error) {
        success = false;
        error = "Failed to determine the GitHub Copilot working directory: " +
                path_error.message();
    }

    Json session;
    if (success)
        success = create_copilot_session(&process, &context, cwd, &session, &error);

    std::string session_id;
    if (success) {
        session_id = string_value(session.value("result", Json::object()), "sessionId");
        if (session_id.empty()) {
            success = false;
            error = "GitHub Copilot ACP server did not return a session id";
        }
    }

    if (success) {
        Event event{EventKind::ProviderThreadStarted, request->conversation_id,
                    request->turn_id};
        event.provider_thread_id = session_id;
        provider_runtime_emit(runtime, &event);
        const Json prompt = {
            {"jsonrpc", "2.0"},
            {"id", 3},
            {"method", "session/prompt"},
            {"params", {{"sessionId", session_id},
                        {"prompt", Json::array({{{"type", "text"},
                                                  {"text", conversation_prompt(request)}}})}}},
        };
        Json response;
        success = write_message(process.input, prompt) &&
                  wait_for_response(&process, 3, &context, &response, &error);
        if (success) {
            const std::string stop_reason =
                string_value(response.value("result", Json::object()), "stopReason");
            if (stop_reason != "end_turn") {
                success = false;
                error = stop_reason == "cancelled" ? "GitHub Copilot turn cancelled"
                                                    : "GitHub Copilot turn stopped: " + stop_reason;
            }
        }
    }

    {
        std::lock_guard lock(state->active_mutex);
        if (state->active_process == &process)
            state->active_process = nullptr;
    }
    stop_copilot_process(&process);
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

Result start_github_copilot(Provider* provider) {
    CopilotState* state = static_cast<CopilotState*>(provider->state);
    provider->default_model = state->options.default_model;
    provider->models.push_back({provider->default_model, "Auto", {}, {}});
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

Result respond_github_copilot(Provider*, const ProviderRequestId&, ApprovalDecision) {
    return result_error("GitHub Copilot has no pending approval request");
}

void cancel_github_copilot(Provider* provider, TurnId turn_id) {
    CopilotState* state = static_cast<CopilotState*>(provider->state);
    std::lock_guard lock(state->active_mutex);
    if (state->active_turn_id != turn_id)
        return;
    state->cancel_requested = true;
    if (state->active_process != nullptr && copilot_process_running(state->active_process))
        terminate_copilot_process(state->active_process);
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
        if (state->active_process != nullptr && copilot_process_running(state->active_process))
            terminate_copilot_process(state->active_process);
        if (state->startup_process != nullptr && copilot_process_running(state->startup_process))
            terminate_copilot_process(state->startup_process);
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
