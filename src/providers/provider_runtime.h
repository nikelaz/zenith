#ifndef PROVIDER_RUNTIME_H
#define PROVIDER_RUNTIME_H

#include "provider.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <queue>
#include <thread>

struct ChildProcess;

struct ProviderProcesses {
    struct ActiveTurn {
        ChildProcess* process = nullptr;
        bool cancelled = false;
    };
    std::mutex mutex;
    std::map<TurnId, ActiveTurn> active_turns;
    ChildProcess* startup_process = nullptr;
    ChildProcess* mcp_process = nullptr;
    ChildProcess* usage_process = nullptr;
    ChildProcess* skills_process = nullptr;
    ChildProcess* skills_cli_process = nullptr;
    bool shutting_down = false;
};

struct ProviderStartupState {
    std::mutex mutex;
    std::condition_variable ready;
    bool complete = false;
    bool applied = false;
    ProviderAvailability availability = ProviderAvailability::Unknown;
    std::filesystem::path location;
    std::string default_model;
    std::vector<ModelOption> models;
};

struct ProviderUsageState {
    std::mutex mutex;
    std::condition_variable ready;
    bool requested = false;
    bool updated = false;
    bool stopping = false;
    UsageSnapshot snapshot;
    std::thread worker;
};

struct ProviderSkillsRequest {
    std::filesystem::path working_directory;
    bool force_reload = false;
};

struct ProviderSkillsState {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<ProviderSkillsRequest> requests;
    std::vector<SkillDiscoverySnapshot> updates;
    bool stopping = false;
    std::thread worker;
};

struct ProviderTurnWorker {
    TurnId id = 0;
    std::thread thread;
    bool complete = false; // Protected by request_mutex.
};

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
    std::thread worker;
    std::vector<ProviderTurnWorker> turn_workers;
    std::mutex event_mutex;
    std::vector<Event> events;
};

void provider_runtime_init(ProviderRuntime* runtime, ProviderProcessFn process,
                           void* process_context);
void provider_runtime_set_initialize(ProviderRuntime* runtime, ProviderInitializeFn initialize);
Result provider_runtime_start(ProviderRuntime* runtime);
// Turn IDs are nonzero and unique among outstanding requests, assigned by the caller.
Result provider_runtime_submit(ProviderRuntime* runtime, TurnRequest request);
bool provider_runtime_cancel_queued(ProviderRuntime* runtime, TurnId turn_id);
std::vector<Event> provider_runtime_poll_events(ProviderRuntime* runtime);
void provider_runtime_emit(ProviderRuntime* runtime, const Event* event);
void provider_runtime_request_shutdown(ProviderRuntime* runtime);
void provider_runtime_shutdown(ProviderRuntime* runtime);
void provider_processes_cancel(ProviderProcesses* processes, ProviderRuntime* runtime,
                               TurnId turn_id);
void provider_processes_request_shutdown(ProviderProcesses* processes);

#endif
