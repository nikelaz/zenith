#ifndef PROVIDER_RUNTIME_H
#define PROVIDER_RUNTIME_H

#include "provider.h"
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>

struct ProviderRuntime;
using ProviderProcessFn = void (*)(void*, const TurnRequest*, ProviderRuntime*);
using ProviderInitializeFn = void (*)(void*, ProviderRuntime*);

struct ProviderRuntime {
    ProviderProcessFn process;
    void* process_context;
    ProviderInitializeFn initialize = nullptr;
    std::mutex request_mutex;
    std::condition_variable request_ready;
    std::queue<TurnRequest> requests;
    bool stopping;
    TurnId next_turn_id;
    std::thread worker;
    std::vector<std::thread> turn_workers;
    std::mutex event_mutex;
    std::vector<Event> events;
};

void provider_runtime_init(ProviderRuntime* runtime, ProviderProcessFn process,
                           void* process_context);
void provider_runtime_set_initialize(ProviderRuntime* runtime, ProviderInitializeFn initialize);
Result provider_runtime_start(ProviderRuntime* runtime);
Result provider_runtime_submit(ProviderRuntime* runtime, TurnRequest request);
bool provider_runtime_cancel_queued(ProviderRuntime* runtime, TurnId turn_id);
std::vector<Event> provider_runtime_poll_events(ProviderRuntime* runtime);
void provider_runtime_emit(ProviderRuntime* runtime, const Event* event);
void provider_runtime_shutdown(ProviderRuntime* runtime);

#endif
