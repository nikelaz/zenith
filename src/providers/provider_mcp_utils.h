#ifndef PROVIDER_MCP_UTILS_H
#define PROVIDER_MCP_UTILS_H

#include "../base/result.h"
#include "../process/child-process.h"
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

Result provider_mcp_run_cli(const std::filesystem::path& executable,
                            const std::vector<std::string>& arguments,
                            const std::filesystem::path& working_directory,
                            std::string* output, std::mutex* process_mutex,
                            ChildProcess** active_process, bool* shutting_down);

#endif
