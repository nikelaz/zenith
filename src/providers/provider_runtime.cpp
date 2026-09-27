#include "provider_runtime.h"
#include <utility>

void run_provider(ProviderRuntime* runtime) {
    if (runtime->initialize != nullptr)
        runtime->initialize(runtime->process_context, runtime);

    for (;;) {
        TurnRequest request;
        {
            std::unique_lock lock(runtime->request_mutex);
            runtime->request_ready.wait(
                lock, [runtime] { return runtime->stopping || !runtime->requests.empty(); });
            if (runtime->stopping && runtime->requests.empty())
                return;

            request = std::move(runtime->requests.front());
            runtime->requests.pop();
        }

        runtime->process(runtime->process_context, &request, runtime);
    }
}

void provider_runtime_init(ProviderRuntime* runtime, ProviderProcessFn process,
                           void* process_context) {
    runtime->process = process;
    runtime->process_context = process_context;
    runtime->initialize = nullptr;
    runtime->stopping = false;
    runtime->next_turn_id = 1;
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
    {
        std::lock_guard lock(runtime->request_mutex);
        if (runtime->stopping || !runtime->worker.joinable())
            return result_error("Provider is not running");
        if (request.turn_id == 0)
            request.turn_id = runtime->next_turn_id++;
        runtime->requests.push(std::move(request));
    }
    runtime->request_ready.notify_one();
    return result_ok();
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

void provider_runtime_shutdown(ProviderRuntime* runtime) {
    {
        std::lock_guard lock(runtime->request_mutex);
        runtime->stopping = true;
    }
    runtime->request_ready.notify_all();
    if (runtime->worker.joinable())
        runtime->worker.join();
}
