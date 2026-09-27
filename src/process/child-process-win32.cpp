#include "child-process.h"
#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <fcntl.h>
#include <io.h>
#include <system_error>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {
std::filesystem::path executable_path(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error)
        return {};
    const std::filesystem::path resolved = std::filesystem::canonical(path, error);
    return error ? path : resolved;
}

std::string failure_message(const char* action, const char* process_name,
                            const char* suffix = "") {
    return std::string("Failed to ") + action + " " + process_name + suffix;
}

void close_if_open(HANDLE handle) {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
        CloseHandle(handle);
}

std::wstring quote_argument(const std::wstring& argument) {
    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'\"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(character);
            backslashes = 0;
        } else {
            quoted.append(backslashes, L'\\');
            quoted.push_back(character);
            backslashes = 0;
        }
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

std::wstring environment_entry(const ChildProcessEnvironmentVariable& variable) {
    return std::filesystem::path(variable.name).wstring() + L"=" +
           variable.value.wstring();
}
} // namespace

std::filesystem::path child_process_resolve_executable(
    const std::filesystem::path& executable) {
    if (executable.empty())
        return {};

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
        const std::filesystem::path canonical = executable_path(resolved);
        if (!canonical.empty())
            return canonical;
    }
    return {};
}

Result child_process_start(
    ChildProcess* process, const std::filesystem::path& executable,
    const std::vector<std::string>& arguments, const char* process_name,
    const std::vector<ChildProcessEnvironmentVariable>& environment) {
    *process = ChildProcess{};

    SECURITY_ATTRIBUTES security_attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE child_input = nullptr;
    HANDLE parent_input = nullptr;
    HANDLE parent_output = nullptr;
    HANDLE child_output = nullptr;
    if (!CreatePipe(&child_input, &parent_input, &security_attributes, 0) ||
        !CreatePipe(&parent_output, &child_output, &security_attributes, 0)) {
        close_if_open(child_input);
        close_if_open(parent_input);
        close_if_open(parent_output);
        close_if_open(child_output);
        return result_error(failure_message("create", process_name, " pipes"));
    }
    if (!SetHandleInformation(parent_input, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(parent_output, HANDLE_FLAG_INHERIT, 0)) {
        close_if_open(child_input);
        close_if_open(parent_input);
        close_if_open(parent_output);
        close_if_open(child_output);
        return result_error(failure_message("prepare", process_name, " pipes"));
    }

    HANDLE child_error = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     &security_attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                     nullptr);
    if (child_error == INVALID_HANDLE_VALUE) {
        close_if_open(child_input);
        close_if_open(parent_input);
        close_if_open(parent_output);
        close_if_open(child_output);
        return result_error(failure_message("prepare", process_name, " output"));
    }

    std::wstring extension = executable.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t character) {
                       return static_cast<wchar_t>(std::towlower(character));
                   });
    const bool is_script = extension == L".cmd" || extension == L".bat";
    std::wstring application = is_script ? L"cmd.exe" : executable.wstring();
    std::wstring command_line;
    if (is_script) {
        command_line = L"cmd.exe /D /S /C \"\"" + executable.wstring() + L"\"";
        for (const std::string& argument : arguments)
            command_line += L" " + quote_argument(std::filesystem::path(argument).wstring());
        command_line.push_back(L'\"');
    } else {
        command_line = quote_argument(application);
        for (const std::string& argument : arguments)
            command_line += L" " + quote_argument(std::filesystem::path(argument).wstring());
    }
    std::vector<wchar_t> writable_command_line(command_line.begin(), command_line.end());
    writable_command_line.push_back(L'\0');

    std::vector<wchar_t> environment_block;
    DWORD creation_flags = CREATE_NO_WINDOW;
    if (!environment.empty()) {
        LPWCH inherited_environment = GetEnvironmentStringsW();
        std::vector<std::wstring> entries;
        if (inherited_environment != nullptr) {
            for (const wchar_t* entry = inherited_environment; *entry != L'\0';
                 entry += std::wcslen(entry) + 1)
                entries.emplace_back(entry);
            FreeEnvironmentStringsW(inherited_environment);
        }

        for (const ChildProcessEnvironmentVariable& variable : environment) {
            const std::wstring replacement = environment_entry(variable);
            bool replaced = false;
            for (std::wstring& entry : entries) {
                const std::size_t name_start = !entry.empty() && entry[0] == L'=' ? 1 : 0;
                const std::size_t name_end = entry.find(L'=', name_start);
                const std::wstring name = replacement.substr(0, replacement.find(L'='));
                if (name_end != std::wstring::npos &&
                    _wcsicmp(entry.substr(0, name_end).c_str(), name.c_str()) == 0) {
                    entry = replacement;
                    replaced = true;
                    break;
                }
            }
            if (!replaced)
                entries.push_back(replacement);
        }
        std::sort(entries.begin(), entries.end(), [](const std::wstring& left,
                                                     const std::wstring& right) {
            return _wcsicmp(left.c_str(), right.c_str()) < 0;
        });
        for (const std::wstring& entry : entries) {
            environment_block.insert(environment_block.end(), entry.begin(), entry.end());
            environment_block.push_back(L'\0');
        }
        environment_block.push_back(L'\0');
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
        creation_flags, environment_block.empty() ? nullptr : environment_block.data(), nullptr,
        &startup, &child);
    CloseHandle(child_input);
    CloseHandle(child_output);
    CloseHandle(child_error);
    if (!started) {
        CloseHandle(parent_input);
        CloseHandle(parent_output);
        return result_error(failure_message("start", process_name));
    }

    CloseHandle(child.hThread);
    process->process_handle = child.hProcess;
    process->process_id = static_cast<std::intptr_t>(child.dwProcessId);
    const int input_fd = _open_osfhandle(
        reinterpret_cast<std::intptr_t>(parent_input), _O_WRONLY | _O_TEXT);
    if (input_fd < 0) {
        CloseHandle(parent_input);
        CloseHandle(parent_output);
        child_process_stop(process);
        return result_error(failure_message("connect", process_name));
    }
    process->input = _fdopen(input_fd, "w");
    if (process->input == nullptr) {
        _close(input_fd);
        CloseHandle(parent_output);
        child_process_stop(process);
        return result_error(failure_message("connect", process_name));
    }
    const int output_fd = _open_osfhandle(
        reinterpret_cast<std::intptr_t>(parent_output), _O_RDONLY | _O_TEXT);
    if (output_fd < 0) {
        CloseHandle(parent_output);
        child_process_stop(process);
        return result_error(failure_message("connect", process_name));
    }
    process->output = _fdopen(output_fd, "r");
    if (process->output == nullptr) {
        _close(output_fd);
        child_process_stop(process);
        return result_error(failure_message("connect", process_name));
    }
    setvbuf(process->input, nullptr, _IOLBF, 0);
    return result_ok();
}

bool child_process_running(const ChildProcess* process) {
    return process->process_handle != nullptr;
}

void child_process_terminate(ChildProcess* process) {
    if (process->process_handle != nullptr)
        TerminateProcess(static_cast<HANDLE>(process->process_handle), 1);
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
    if (process->process_handle != nullptr) {
        HANDLE handle = static_cast<HANDLE>(process->process_handle);
        WaitForSingleObject(handle, INFINITE);
        CloseHandle(handle);
        process->process_handle = nullptr;
        process->process_id = 0;
    }
}

void child_process_ignore_sigpipe() {}
