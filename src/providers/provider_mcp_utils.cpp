#include "provider_mcp_utils.h"
#include "../process/child-process.h"
#include <cstdio>
#include <thread>

Result provider_mcp_run_cli(const std::filesystem::path& executable,
                            const std::vector<std::string>& arguments,
                            const std::filesystem::path& working_directory,
                            std::string* output) {
    if (output == nullptr)
        return result_error("MCP command output is required");
    output->clear();

    const std::filesystem::path resolved = child_process_resolve_executable(executable);
    if (resolved.empty())
        return result_error("Provider executable was not found");

    ChildProcess process;
    Result start = child_process_start(&process, resolved, arguments,
                                      "provider MCP command", {}, {}, working_directory);
    if (start.status == ResultStatus::Error) {
        child_process_stop(&process);
        return start;
    }

    if (process.input != nullptr) {
        fclose(process.input);
        process.input = nullptr;
    }

    std::thread output_reader([&process, output] {
        int character = 0;
        while ((character = fgetc(process.output)) != EOF)
            output->push_back(static_cast<char>(character));
    });

    int exit_code = -1;
    Result wait = child_process_wait(&process, 30000, &exit_code);
    if (wait.status == ResultStatus::Error)
        child_process_terminate(&process);
    if (output_reader.joinable())
        output_reader.join();
    child_process_stop(&process);
    if (wait.status == ResultStatus::Error)
        return wait;
    if (exit_code != 0)
        return result_error("Provider MCP command failed with exit code " +
                            std::to_string(exit_code));
    return result_ok();
}
