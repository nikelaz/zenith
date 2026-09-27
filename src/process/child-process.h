#ifndef CHILD_PROCESS_H
#define CHILD_PROCESS_H

#include "../base/result.h"
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

struct ChildProcess {
    FILE* input = nullptr;
    FILE* output = nullptr;
    void* process_handle = nullptr;
    std::intptr_t process_id = 0;
};

struct ChildProcessEnvironmentVariable {
    std::string name;
    std::filesystem::path value;
};

std::filesystem::path child_process_resolve_executable(
    const std::filesystem::path& executable);
Result child_process_start(
    ChildProcess* process, const std::filesystem::path& executable,
    const std::vector<std::string>& arguments, const char* process_name,
    const std::vector<ChildProcessEnvironmentVariable>& environment = {});
bool child_process_running(const ChildProcess* process);
void child_process_terminate(ChildProcess* process);
void child_process_stop(ChildProcess* process);
void child_process_ignore_sigpipe();

#endif
