#ifndef MCP_SERVICE_H
#define MCP_SERVICE_H

#include "provider.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

enum class McpTaskKind { Refresh, Upsert, Remove, SetEnabled };
struct McpTask {
    McpTaskKind kind = McpTaskKind::Refresh;
    std::size_t provider_index = 0;
    std::filesystem::path working_directory;
    std::string existing_name;
    std::string name;
    McpServer server;
    bool enabled = true;
};
struct McpTaskResult {
    McpTaskKind kind = McpTaskKind::Refresh;
    std::size_t provider_index = 0;
    std::filesystem::path working_directory;
    std::string feedback_key;
    std::vector<McpServer> servers;
    std::string operation_error;
    std::string list_error;
    bool list_succeeded = false;
};
struct McpService {
    std::vector<ProviderPtr>* providers = nullptr; // Stable until shutdown has joined the worker.
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<McpTask> tasks;
    std::deque<McpTaskResult> results;
    std::thread worker;
    bool stopping = false;
};

Result mcp_service_start(McpService* service, std::vector<ProviderPtr>* providers);
void mcp_service_shutdown(McpService* service);
bool mcp_service_submit(McpService* service, McpTask task);
std::deque<McpTaskResult> mcp_service_take_results(McpService* service);

#endif
