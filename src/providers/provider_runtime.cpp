#include "provider_runtime.h"
#include "../process/child-process.h"
#include <algorithm>
#include <utility>

static bool has_completed_worker(const ProviderRuntime* runtime) {
    return std::any_of(runtime->turn_workers.begin(), runtime->turn_workers.end(),
                       [](const ProviderTurnWorker& worker) { return worker.complete; });
}

// Called by the dispatcher with request_mutex held. Completed workers no longer
// need the mutex, so joining them here cannot block another completion.
static void reap_completed_workers(ProviderRuntime* runtime) {
    for (std::size_t index = 0; index < runtime->turn_workers.size();) {
        ProviderTurnWorker& worker = runtime->turn_workers[index];
        if (worker.complete) {
            worker.thread.join();
            if (index + 1 != runtime->turn_workers.size())
                worker = std::move(runtime->turn_workers.back());
            runtime->turn_workers.pop_back();
        } else {
            ++index;
        }
    }
}

static void run_provider(ProviderRuntime* runtime) {
    if (runtime->initialize != nullptr)
        runtime->initialize(runtime->process_context, runtime);

    for (;;) {
        TurnRequest request;
        std::unique_lock lock(runtime->request_mutex);
        runtime->request_ready.wait(lock, [runtime] {
            return runtime->stopping || !runtime->requests.empty() || has_completed_worker(runtime);
        });
        reap_completed_workers(runtime);
        if (runtime->stopping && runtime->requests.empty())
            return;
        if (runtime->requests.empty())
            continue;
        request = std::move(runtime->requests.front());
        runtime->requests.pop();
        runtime->turn_workers.push_back({request.turn_id, {}, false});
        runtime->turn_workers.back().thread = std::thread(
            [runtime, request = std::move(request)] {
                runtime->process(runtime->process_context, &request, runtime);
                {
                    std::lock_guard completion_lock(runtime->request_mutex);
                    for (ProviderTurnWorker& worker : runtime->turn_workers)
                        if (worker.id == request.turn_id) {
                            worker.complete = true;
                            break;
                        }
                }
                runtime->request_ready.notify_one();
            });
    }
}

void provider_runtime_init(ProviderRuntime* runtime, ProviderProcessFn process,
                           void* process_context) {
    runtime->process = process;
    runtime->process_context = process_context;
    runtime->initialize = nullptr;
    runtime->stopping = false;
}

void provider_runtime_set_initialize(ProviderRuntime* runtime, ProviderInitializeFn initialize) {
    runtime->initialize = initialize;
}

Result provider_runtime_start(ProviderRuntime* runtime) {
    if (runtime->worker.joinable())
        return result_ok();

    {
        std::lock_guard lock(runtime->request_mutex);
        runtime->stopping = false;
    }
    runtime->worker = std::thread(run_provider, runtime);
    return result_ok();
}

Result provider_runtime_submit(ProviderRuntime* runtime, TurnRequest request) {
    if (request.turn_id == 0)
        return result_error("A turn must have a caller-assigned ID");
    {
        std::lock_guard lock(runtime->request_mutex);
        if (runtime->stopping || !runtime->worker.joinable())
            return result_error("Provider is not running");
        runtime->requests.push(std::move(request));
    }

    runtime->request_ready.notify_one();
    return result_ok();
}

bool provider_runtime_cancel_queued(ProviderRuntime* runtime, TurnId turn_id) {
    std::lock_guard lock(runtime->request_mutex);
    std::queue<TurnRequest> remaining;
    bool cancelled = false;
    while (!runtime->requests.empty()) {
        TurnRequest request = std::move(runtime->requests.front());
        runtime->requests.pop();
        if (request.turn_id == turn_id) {
            cancelled = true;
            Event event{EventKind::TurnCompleted, request.conversation_id, request.turn_id};
            provider_runtime_emit(runtime, &event);
        } else {
            remaining.push(std::move(request));
        }
    }
    runtime->requests.swap(remaining);
    return cancelled;
}

std::vector<Event> provider_runtime_poll_events(ProviderRuntime* runtime) {
    std::lock_guard lock(runtime->event_mutex);
    std::vector<Event> events;
    events.swap(runtime->events);
    return events;
}

void provider_runtime_emit(ProviderRuntime* runtime, const Event* event) {
    std::lock_guard lock(runtime->event_mutex);
    runtime->events.push_back(*event);
}

void provider_runtime_request_shutdown(ProviderRuntime* runtime) {
    {
        std::lock_guard lock(runtime->request_mutex);
        runtime->stopping = true;
        while (!runtime->requests.empty())
            runtime->requests.pop();
    }
    runtime->request_ready.notify_all();
}

void provider_runtime_shutdown(ProviderRuntime* runtime) {
    {
        std::lock_guard lock(runtime->request_mutex);
        runtime->stopping = true;
    }
    runtime->request_ready.notify_all();
    if (runtime->worker.joinable())
        runtime->worker.join();
    for (ProviderTurnWorker& worker : runtime->turn_workers)
        worker.thread.join();
    runtime->turn_workers.clear();
}

void provider_processes_cancel(ProviderProcesses* processes, ProviderRuntime* runtime,
                               TurnId turn_id) {
    const bool queued = provider_runtime_cancel_queued(runtime, turn_id);
    std::lock_guard lock(processes->mutex);
    if (queued) {
        processes->active_turns.erase(turn_id);
        return;
    }
    const auto active = processes->active_turns.find(turn_id);
    if (active == processes->active_turns.end())
        return;
    active->second.cancelled = true;
    if (active->second.process != nullptr && child_process_running(active->second.process))
        child_process_terminate(active->second.process);
}

void provider_processes_request_shutdown(ProviderProcesses* processes) {
    std::lock_guard lock(processes->mutex);
    processes->shutting_down = true;
    for (auto& [id, active] : processes->active_turns)
        if (active.process != nullptr && child_process_running(active.process))
            child_process_terminate(active.process);
    ChildProcess* auxiliary[] = {processes->startup_process, processes->mcp_process,
                                processes->usage_process, processes->skills_process,
                                processes->skills_cli_process};
    for (ChildProcess* process : auxiliary)
        if (process != nullptr && child_process_running(process))
            child_process_terminate(process);
}
