#include "../src/providers/provider.h"
#include "../src/providers/provider_runtime.h"
#include "../src/persistence/persistent-store.h"
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <gtest/gtest.h>
#include <mutex>
#include <sqlite3.h>
#include <thread>

namespace {
std::vector<Event> run(Provider* provider) {
    EXPECT_EQ(provider->start(provider).status, ResultStatus::Ok);

    TurnRequest request;
    request.turn_id = 42;
    request.conversation_id = "thread-a";
    request.prompt = "hello";
    EXPECT_EQ(provider->submit(provider, std::move(request)).status, ResultStatus::Ok);

    std::vector<Event> events;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        auto pending = provider->poll_events(provider);
        events.insert(events.end(), pending.begin(), pending.end());
        if (!events.empty() && (events.back().kind == EventKind::TurnCompleted ||
                                events.back().kind == EventKind::TurnFailed)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return events;
}

Result codex_success(void*, const TurnRequest* request, ProviderEventSink emit, void* context) {
    EXPECT_EQ(request->prompt, "hello");
    const Event thinking{EventKind::ReasoningSummaryDelta, request->conversation_id,
                         request->turn_id, "Checking the request.", {}, {}};
    emit(context, &thinking);
    const Event first_delta{EventKind::AssistantTextDelta, request->conversation_id,
                            request->turn_id, "Codex ", {}, {}};
    emit(context, &first_delta);
    const Event second_delta{EventKind::AssistantTextDelta, request->conversation_id,
                             request->turn_id, "answer", {}, {}};
    emit(context, &second_delta);
    return result_ok();
}

Result codex_failure(void*, const TurnRequest*, ProviderEventSink, void*) {
    return result_error("codex unavailable");
}
} // namespace

namespace {
struct RuntimeContext {
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<TurnId> processed;
    bool block_first = false;
    bool first_started = false;
    bool release_first = false;
};

void runtime_process(void* opaque, const TurnRequest* request, ProviderRuntime* runtime) {
    auto* context = static_cast<RuntimeContext*>(opaque);
    {
        std::unique_lock lock(context->mutex);
        if (context->block_first && context->processed.empty()) {
            context->first_started = true;
            context->ready.notify_all();
            context->ready.wait(lock, [context] { return context->release_first; });
        }
        context->processed.push_back(request->turn_id);
    }
    const Event event{EventKind::TurnCompleted, request->conversation_id,
                      request->turn_id, std::to_string(request->turn_id), {}, {}};
    provider_runtime_emit(runtime, &event);
}

TurnRequest runtime_request(TurnId id, const char* conversation) {
    TurnRequest request;
    request.turn_id = id;
    request.conversation_id = conversation;
    return request;
}

class TemporaryDatabase {
public:
    TemporaryDatabase() {
        m_path = std::filesystem::temp_directory_path() /
            ("zenith-tests-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) + ".sqlite");
    }
    ~TemporaryDatabase() {
        std::error_code error;
        std::filesystem::remove(m_path, error);
        std::filesystem::remove(m_path.string() + "-wal", error);
        std::filesystem::remove(m_path.string() + "-shm", error);
    }
    std::string string() const { return m_path.string(); }
private:
    std::filesystem::path m_path;
};

bool execute_sql(const std::string& path, const char* sql) {
    sqlite3* database = nullptr;
    if (sqlite3_open(path.c_str(), &database) != SQLITE_OK) {
        sqlite3_close(database);
        return false;
    }
    const int result = sqlite3_exec(database, sql, nullptr, nullptr, nullptr);
    sqlite3_close(database);
    return result == SQLITE_OK;
}
}

TEST(ProviderRuntime, RejectsSubmitBeforeStart) {
    ProviderRuntime runtime;
    provider_runtime_init(&runtime, runtime_process, nullptr);
    EXPECT_EQ(provider_runtime_submit(&runtime, runtime_request(1, "thread")).status,
              ResultStatus::Error);
    provider_runtime_shutdown(&runtime);
}

TEST(ProviderRuntime, ProcessesQueuedRequestsInOrderAndEmitsOrderedEvents) {
    RuntimeContext context;
    ProviderRuntime runtime;
    provider_runtime_init(&runtime, runtime_process, &context);
    ASSERT_EQ(provider_runtime_start(&runtime).status, ResultStatus::Ok);
    ASSERT_EQ(provider_runtime_submit(&runtime, runtime_request(1, "first")).status,
              ResultStatus::Ok);
    ASSERT_EQ(provider_runtime_submit(&runtime, runtime_request(2, "second")).status,
              ResultStatus::Ok);
    ASSERT_EQ(provider_runtime_submit(&runtime, runtime_request(3, "third")).status,
              ResultStatus::Ok);
    provider_runtime_shutdown(&runtime);

    EXPECT_EQ(context.processed, (std::vector<TurnId>{1, 2, 3}));
    const auto events = provider_runtime_poll_events(&runtime);
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0].conversation_id, "first");
    EXPECT_EQ(events[1].conversation_id, "second");
    EXPECT_EQ(events[2].conversation_id, "third");
    EXPECT_EQ(events[0].kind, EventKind::TurnCompleted);
    EXPECT_TRUE(provider_runtime_poll_events(&runtime).empty());
}

TEST(ProviderRuntime, ShutdownWhileIdleReturnsAndCanRestart) {
    RuntimeContext context;
    ProviderRuntime runtime;
    provider_runtime_init(&runtime, runtime_process, &context);
    ASSERT_EQ(provider_runtime_start(&runtime).status, ResultStatus::Ok);
    provider_runtime_shutdown(&runtime);
    EXPECT_EQ(provider_runtime_submit(&runtime, runtime_request(1, "stopped")).status,
              ResultStatus::Error);
    ASSERT_EQ(provider_runtime_start(&runtime).status, ResultStatus::Ok);
    EXPECT_EQ(provider_runtime_submit(&runtime, runtime_request(7, "restarted")).status,
              ResultStatus::Ok);
    provider_runtime_shutdown(&runtime);
    EXPECT_EQ(context.processed, (std::vector<TurnId>{7}));
}

TEST(ProviderRuntime, ShutdownDrainsQueuedWork) {
    RuntimeContext context;
    context.block_first = true;
    ProviderRuntime runtime;
    provider_runtime_init(&runtime, runtime_process, &context);
    ASSERT_EQ(provider_runtime_start(&runtime).status, ResultStatus::Ok);
    ASSERT_EQ(provider_runtime_submit(&runtime, runtime_request(1, "first")).status,
              ResultStatus::Ok);
    {
        std::unique_lock lock(context.mutex);
        ASSERT_TRUE(context.ready.wait_for(lock, std::chrono::seconds(2),
                                           [&context] { return context.first_started; }));
    }
    ASSERT_EQ(provider_runtime_submit(&runtime, runtime_request(2, "queued")).status,
              ResultStatus::Ok);
    std::thread shutdown([&runtime] { provider_runtime_shutdown(&runtime); });
    {
        std::lock_guard lock(context.mutex);
        context.release_first = true;
    }
    context.ready.notify_all();
    shutdown.join();
    EXPECT_EQ(context.processed, (std::vector<TurnId>{1, 2}));
    EXPECT_EQ(provider_runtime_poll_events(&runtime).size(), 2u);
}

TEST(PersistentStore, SavesAndLoadsProjectsThreadsMessagesAndSelection) {
    TemporaryDatabase database;
    ApplicationState saved;
    saved.projects = {
        {"/work/one", true, {{"First", "first description", "thread-1", {
            {ChatMessageRole::User, "question", "", {}, {}, {}},
            {ChatMessageRole::Assistant, "answer", "private reasoning", {}, {
                {ChatSegment::Kind::Text, "before tool", {}},
                {ChatSegment::Kind::Tool, {}, {"tool-1", "shell", "echo hi", "{}",
                    "/work/one", "hi\n", "completed", 0, 12, true, true}},
                {ChatSegment::Kind::Text, "after tool", {}},
            }, {}},
        }}}},
        {"/work/two", false, {{"Second", "second description", "thread-2", {
            {ChatMessageRole::Assistant, "other project", "", {}, {}, {}},
        }}}},
    };
    saved.selected_project = 1;
    saved.selected_thread = 0;
    {
        PersistentStore store;
        ASSERT_EQ(store.open(database.string()).status, ResultStatus::Ok);
        ASSERT_EQ(store.save(saved).status, ResultStatus::Ok);
    }

    ApplicationState loaded;
    loaded.projects.clear();
    {
        PersistentStore store;
        ASSERT_EQ(store.open(database.string()).status, ResultStatus::Ok);
        ASSERT_EQ(store.load(loaded).status, ResultStatus::Ok);
    }
    ASSERT_EQ(loaded.projects.size(), 2u);
    EXPECT_EQ(loaded.projects[0].directory, std::filesystem::path("/work/one"));
    EXPECT_TRUE(loaded.projects[0].expanded);
    EXPECT_FALSE(loaded.projects[1].expanded);
    ASSERT_EQ(loaded.projects[0].threads.size(), 1u);
    EXPECT_EQ(loaded.projects[0].threads[0].id, "thread-1");
    ASSERT_EQ(loaded.projects[0].threads[0].messages.size(), 2u);
    EXPECT_EQ(loaded.projects[0].threads[0].messages[0].role, ChatMessageRole::User);
    EXPECT_EQ(loaded.projects[0].threads[0].messages[0].content, "question");
    const ChatMessage& answer = loaded.projects[0].threads[0].messages[1];
    EXPECT_EQ(answer.content, "answer");
    EXPECT_EQ(answer.reasoning, "private reasoning");
    ASSERT_EQ(answer.segments.size(), 3u);
    EXPECT_EQ(answer.segments[0].text, "before tool");
    EXPECT_EQ(answer.segments[1].kind, ChatSegment::Kind::Tool);
    EXPECT_EQ(answer.segments[1].tool.name, "shell");
    EXPECT_EQ(answer.segments[1].tool.output, "hi\n");
    EXPECT_EQ(answer.segments[1].tool.exit_code, 0);
    EXPECT_EQ(answer.segments[1].tool.duration_ms, 12);
    EXPECT_TRUE(answer.segments[1].tool.completed);
    EXPECT_EQ(answer.segments[2].text, "after tool");
    EXPECT_EQ(loaded.projects[1].threads[0].messages[0].content, "other project");
    EXPECT_EQ(loaded.selected_project, 1u);
    EXPECT_EQ(loaded.selected_thread, 0u);
}

TEST(PersistentStore, LoadsAndRestoresEmptyState) {
    TemporaryDatabase database;
    ApplicationState empty;
    empty.projects.clear();
    {
        PersistentStore store;
        ASSERT_EQ(store.open(database.string()).status, ResultStatus::Ok);
        ASSERT_EQ(store.save(empty).status, ResultStatus::Ok);
    }
    ApplicationState loaded;
    {
        PersistentStore store;
        ASSERT_EQ(store.open(database.string()).status, ResultStatus::Ok);
        ASSERT_EQ(store.load(loaded).status, ResultStatus::Ok);
    }
    EXPECT_TRUE(loaded.projects.empty());
    EXPECT_EQ(loaded.selected_project, 0u);
    EXPECT_EQ(loaded.selected_thread, 0u);
}

TEST(PersistentStore, RejectsThreadWithUnknownProjectReference) {
    TemporaryDatabase database;
    ApplicationState saved;
    {
        PersistentStore store;
        ASSERT_EQ(store.open(database.string()).status, ResultStatus::Ok);
        ASSERT_EQ(store.save(saved).status, ResultStatus::Ok);
    }
    ASSERT_TRUE(execute_sql(database.string(), "UPDATE threads SET project_position = 99"));
    ApplicationState loaded;
    PersistentStore store;
    ASSERT_EQ(store.open(database.string()).status, ResultStatus::Ok);
    EXPECT_EQ(store.load(loaded).status, ResultStatus::Error);
}

TEST(FakeProvider, AnswersAndCompletesTurn) {
    auto provider = make_fake_provider();
    const auto events = run(provider.get());
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].kind, EventKind::AssistantTextDelta);
    EXPECT_EQ(events[0].text, "How can I help you?");
    EXPECT_EQ(events[1].kind, EventKind::TurnCompleted);
}

TEST(CodexProvider, ExecutesRequestAndEmitsResponse) {
    CodexOptions options;
    options.execute = codex_success;
    auto provider = make_codex_provider(&options);
    const auto events = run(provider.get());
    ASSERT_EQ(events.size(), 4u);
    EXPECT_EQ(events[0].kind, EventKind::ReasoningSummaryDelta);
    EXPECT_EQ(events[0].text, "Checking the request.");
    EXPECT_EQ(events[1].kind, EventKind::AssistantTextDelta);
    EXPECT_EQ(events[1].text, "Codex ");
    EXPECT_EQ(events[2].text, "answer");
    EXPECT_EQ(events[3].kind, EventKind::TurnCompleted);
}

TEST(CodexProvider, ReportsExecutionFailure) {
    CodexOptions options;
    options.execute = codex_failure;
    auto provider = make_codex_provider(&options);
    const auto events = run(provider.get());
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].kind, EventKind::TurnFailed);
    EXPECT_EQ(events[0].text, "codex unavailable");
}
