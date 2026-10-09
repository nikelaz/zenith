#include "mcp-service.h"
#include <system_error>
#include <utility>

static void mcp_service_worker(McpService* service) {
    for (;;) {
        McpTask task;
        {
            std::unique_lock lock(service->mutex);
            service->ready.wait(lock, [service] {
                return service->stopping || !service->tasks.empty();
            });
            if (service->stopping)
                return;
            task = std::move(service->tasks.front());
            service->tasks.pop_front();
        }

        McpTaskResult result;
        result.kind = task.kind;
        result.provider_index = task.provider_index;
        result.working_directory = task.working_directory;
        if (task.kind == McpTaskKind::Upsert) {
            result.feedback_key = task.existing_name.empty()
                ? task.server.name : task.existing_name;
        } else {
            result.feedback_key = task.name;
        }
        if (task.provider_index >= (*service->providers).size()) {
            result.operation_error = "Provider is no longer available.";
        } else {
            Provider& provider = *(*service->providers)[task.provider_index];
            Result operation = result_ok();
            if (task.kind == McpTaskKind::Upsert) {
                operation = provider.upsert_mcp_server == nullptr
                    ? result_error("This provider does not support MCP server editing.")
                    : provider.upsert_mcp_server(&provider, task.working_directory,
                                                 task.existing_name, task.server);
            } else if (task.kind == McpTaskKind::Remove) {
                operation = provider.remove_mcp_server == nullptr
                    ? result_error("This provider does not support MCP server removal.")
                    : provider.remove_mcp_server(&provider, task.working_directory, task.name);
            } else if (task.kind == McpTaskKind::SetEnabled) {
                operation = provider.set_mcp_server_enabled == nullptr
                    ? result_error("This provider does not support enabling or disabling MCP servers.")
                    : provider.set_mcp_server_enabled(&provider, task.working_directory,
                                                      task.name, task.enabled);
            }
            if (operation.status == ResultStatus::Error)
                result.operation_error = operation.error;

            if (provider.list_mcp_servers != nullptr) {
                const Result listed = provider.list_mcp_servers(
                    &provider, task.working_directory, &result.servers);
                result.list_succeeded = listed.status == ResultStatus::Ok;
                if (!result.list_succeeded)
                    result.list_error = listed.error;
            } else {
                result.list_error = "This provider does not support MCP server listing.";
            }
        }

        {
            std::lock_guard lock(service->mutex);
            service->results.push_back(std::move(result));
        }
    }
}

Result mcp_service_start(McpService* service, std::vector<ProviderPtr>* providers) {
    if (service->worker.joinable())
        return result_error("MCP service is already running");
    service->providers = providers;
    service->stopping = false;
    try {
        service->worker = std::thread(mcp_service_worker, service);
    } catch (const std::system_error& error) {
        return result_error(error.what());
    }
    return result_ok();
}

void mcp_service_shutdown(McpService* service) {
    {
        std::lock_guard lock(service->mutex);
        service->stopping = true;
        service->tasks.clear();
    }
    service->ready.notify_all();
    if (service->worker.joinable())
        service->worker.join();
    service->results.clear();
    service->providers = nullptr;
}

bool mcp_service_submit(McpService* service, McpTask task) {
    {
        std::lock_guard lock(service->mutex);
        if (service->stopping || !service->worker.joinable())
            return false;
        service->tasks.push_back(std::move(task));
    }
    service->ready.notify_one();
    return true;
}

std::deque<McpTaskResult> mcp_service_take_results(McpService* service) {
    std::deque<McpTaskResult> results;
    std::lock_guard lock(service->mutex);
    results.swap(service->results);
    return results;
}
