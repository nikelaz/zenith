#ifndef PROVIDER_MCP_UTILS_H
#define PROVIDER_MCP_UTILS_H

#include "../base/result.h"
#include <filesystem>
#include <string>
#include <vector>

Result provider_mcp_run_cli(const std::filesystem::path& executable,
                            const std::vector<std::string>& arguments,
                            const std::filesystem::path& working_directory,
                            std::string* output);

#endif
