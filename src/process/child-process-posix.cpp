#include "child-process.h"
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <string>
#include <system_error>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {
std::filesystem::path executable_path(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error ||
        access(path.c_str(), X_OK) != 0)
        return {};
    const std::filesystem::path resolved = std::filesystem::canonical(path, error);
    return error ? path : resolved;
}

std::string failure_message(const char* action, const char* process_name,
                            const char* suffix = "") {
    return std::string("Failed to ") + action + " " + process_name + suffix;
}
} // namespace

std::filesystem::path child_process_resolve_executable(
    const std::filesystem::path& executable) {
    if (executable.empty())
        return {};

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
}

Result child_process_start(
    ChildProcess* process, const std::filesystem::path& executable,
    const std::vector<std::string>& arguments, const char* process_name,
    const std::vector<ChildProcessEnvironmentVariable>& environment) {
    *process = ChildProcess{};

    std::vector<std::string> argument_strings;
    argument_strings.reserve(arguments.size() + 1);
    argument_strings.push_back(executable.string());
    argument_strings.insert(argument_strings.end(), arguments.begin(), arguments.end());
    std::vector<char*> argument_values;
    argument_values.reserve(argument_strings.size() + 1);
    for (std::string& argument : argument_strings)
        argument_values.push_back(argument.data());
    argument_values.push_back(nullptr);

    int input_pipe[2];
    int output_pipe[2];
    if (pipe(input_pipe) != 0)
        return result_error(failure_message("create", process_name, " pipes"));
    if (pipe(output_pipe) != 0) {
        close(input_pipe[0]);
        close(input_pipe[1]);
        return result_error(failure_message("create", process_name, " pipes"));
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(input_pipe[0]);
        close(input_pipe[1]);
        close(output_pipe[0]);
        close(output_pipe[1]);
        return result_error(failure_message("start", process_name));
    }

    if (pid == 0) {
        if (dup2(input_pipe[0], STDIN_FILENO) < 0 ||
            dup2(output_pipe[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(input_pipe[0]);
        close(input_pipe[1]);
        close(output_pipe[0]);
        close(output_pipe[1]);
        for (const ChildProcessEnvironmentVariable& variable : environment)
            setenv(variable.name.c_str(), variable.value.c_str(), 1);
        execvp(argument_values[0], argument_values.data());
        _exit(127);
    }

    close(input_pipe[0]);
    close(output_pipe[1]);
    process->process_id = static_cast<std::intptr_t>(pid);
    process->input = fdopen(input_pipe[1], "w");
    if (process->input == nullptr)
        close(input_pipe[1]);
    process->output = fdopen(output_pipe[0], "r");
    if (process->output == nullptr)
        close(output_pipe[0]);
    if (process->input == nullptr || process->output == nullptr) {
        child_process_stop(process);
        return result_error(failure_message("connect", process_name));
    }
    setvbuf(process->input, nullptr, _IOLBF, 0);
    return result_ok();
}

bool child_process_running(const ChildProcess* process) {
    return process->process_id > 0;
}

void child_process_terminate(ChildProcess* process) {
    if (process->process_id > 0)
        kill(static_cast<pid_t>(process->process_id), SIGTERM);
}

void child_process_stop(ChildProcess* process) {
    child_process_terminate(process);
    if (process->input != nullptr) {
        fclose(process->input);
        process->input = nullptr;
    }
    if (process->output != nullptr) {
        fclose(process->output);
        process->output = nullptr;
    }
    if (process->process_id > 0) {
        int status = 0;
        while (waitpid(static_cast<pid_t>(process->process_id), &status, 0) < 0 && errno == EINTR) {
        }
        process->process_id = 0;
    }
}

void child_process_ignore_sigpipe() {
    static const bool ignored = [] {
        std::signal(SIGPIPE, SIG_IGN);
        return true;
    }();
    (void)ignored;
}
