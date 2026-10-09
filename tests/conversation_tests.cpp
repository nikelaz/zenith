#include "../src/conversation/conversation.h"
#include "../src/providers/mcp-service.h"
#include <chrono>
#include <gtest/gtest.h>
#include <utility>

struct TestProviderState {
    std::vector<TurnRequest> requests;
    std::vector<TurnId> cancelled;
    std::vector<Event> events;
    bool reject_submit = false;
};

static Result test_submit(Provider* provider, TurnRequest request) {
    auto* state = static_cast<TestProviderState*>(provider->state);
    if (state->reject_submit)
        return result_error("submission failed");
    state->requests.push_back(std::move(request));
    return result_ok();
}

static void test_cancel(Provider* provider, TurnId id) {
    static_cast<TestProviderState*>(provider->state)->cancelled.push_back(id);
}

static std::vector<Event> test_poll(Provider* provider) {
    std::vector<Event> events;
    events.swap(static_cast<TestProviderState*>(provider->state)->events);
    return events;
}

static void test_destroy(Provider* provider) { delete provider; }

static ProviderPtr test_provider(TestProviderState* state) {
    auto* provider = new Provider{};
    provider->name = "test";
    provider->state = state;
    provider->submit = test_submit;
    provider->cancel = test_cancel;
    provider->poll_events = test_poll;
    provider->destroy = test_destroy;
    provider->availability = ProviderAvailability::Available;
    provider->default_model = "test-model";
    return ProviderPtr(provider, destroy_provider);
}

static ApplicationState test_application() {
    ApplicationState state;
    state.projects = {{"/project", true, {{"First", {}, "first", {}},
                                         {"Second", {}, "second", {}}}}};
    state.selected_thread = 1;
    return state;
}

TEST(Conversation, ReorderingPreservesConversationAndOptions) {
    ApplicationState state = test_application();
    ChatThread* second = application_find_thread(&state, "second");
    second->model = "chosen-model";
    second->reasoning_effort = "high";
    second->permission_mode = "workspace-write";
    ConversationState conversations;
    TestProviderState provider_state;
    ProviderPtr provider = test_provider(&provider_state);
    ComposerState composer;
    composer.message_input = "hello";
    ASSERT_EQ(conversation_submit(&conversations, &state, provider.get(), "second",
                                  &composer, {}).status, ResultStatus::Ok);
    EXPECT_EQ(state.selected_thread, 0u);
    ASSERT_EQ(state.projects[0].threads[0].id, "second");
    EXPECT_EQ(state.projects[0].threads[1].id, "first");
    ASSERT_EQ(provider_state.requests.size(), 1u);
    EXPECT_EQ(provider_state.requests[0].model, "chosen-model");
    EXPECT_EQ(provider_state.requests[0].reasoning_effort, "high");
    EXPECT_EQ(provider_state.requests[0].permission_mode, "workspace-write");
    EXPECT_TRUE(composer.message_input.empty());
}

TEST(Conversation, CancelAndResubmitRejectsOldAndWrongProviderEvents) {
    ApplicationState state = test_application();
    ConversationState conversations;
    TestProviderState provider_state;
    TestProviderState other_state;
    ProviderPtr provider = test_provider(&provider_state);
    ProviderPtr other = test_provider(&other_state);
    ComposerState composer;
    composer.message_input = "first request";
    ASSERT_EQ(conversation_submit(&conversations, &state, provider.get(), "second",
                                  &composer, {}).status, ResultStatus::Ok);
    const TurnId old_id = provider_state.requests.back().turn_id;
    conversation_apply_event(&conversations, &state, provider.get(),
                            {EventKind::AssistantTextDelta, "second", old_id, "partial"});
    conversation_cancel(&conversations, "second");
    EXPECT_EQ(provider_state.cancelled, (std::vector<TurnId>{old_id}));
    composer.message_input = "new request";
    ASSERT_EQ(conversation_submit(&conversations, &state, provider.get(), "second",
                                  &composer, {}).status, ResultStatus::Ok);
    const TurnId new_id = provider_state.requests.back().turn_id;
    conversation_apply_event(&conversations, &state, provider.get(),
                            {EventKind::AssistantTextDelta, "second", old_id, "stale text"});
    conversation_apply_event(&conversations, &state, provider.get(),
                            {EventKind::ToolActivity, "second", old_id, "stale tool"});
    conversation_apply_event(&conversations, &state, provider.get(),
                            {EventKind::TurnFailed, "second", old_id, "stale failure"});
    conversation_apply_event(&conversations, &state, other.get(),
                            {EventKind::AssistantTextDelta, "second", new_id, "wrong provider"});
    EXPECT_TRUE(conversation_is_generating(&conversations, "second"));
    conversation_apply_event(&conversations, &state, provider.get(),
                            {EventKind::AssistantTextDelta, "second", new_id, "new answer"});
    conversation_apply_event(&conversations, &state, provider.get(),
                            {EventKind::TurnCompleted, "second", new_id});
    EXPECT_FALSE(conversation_is_generating(&conversations, "second"));
    const auto& messages = application_find_thread(&state, "second")->messages;
    ASSERT_EQ(messages.size(), 4u);
    EXPECT_EQ(chat_message_text(messages[1]), "partial");
    EXPECT_EQ(chat_message_text(messages[3]), "new answer");
    EXPECT_TRUE(messages[3].content.empty());
}

TEST(Conversation, RequestHistoryOwnsOnlyTextAndAppliesCommandExpansion) {
    ApplicationState state = test_application();
    ChatThread* thread = application_find_thread(&state, "second");
    thread->messages.push_back({ChatMessageRole::Assistant, {}, "reasoning", {
        {ChatSegment::Kind::Text, "before", {}},
        {ChatSegment::Kind::Tool, {}, {"tool", "shell", "command", {}, {}, "large output"}},
        {ChatSegment::Kind::Text, "after", {}},
    }, {}});
    ConversationState conversations;
    TestProviderState provider_state;
    ProviderPtr provider = test_provider(&provider_state);
    ComposerState composer;
    composer.message_input = "/review file";
    const std::vector<SkillEntry> commands = {{"review", {}, {}, {}, "native-review"}};
    ASSERT_EQ(conversation_submit(&conversations, &state, provider.get(), "second",
                                  &composer, commands).status, ResultStatus::Ok);
    const TurnRequest& request = provider_state.requests.back();
    ASSERT_EQ(request.history.size(), 2u);
    EXPECT_EQ(request.history[0].content, "beforeafter");
    EXPECT_EQ(request.history[1].content, "native-review file");
    EXPECT_EQ(application_find_thread(&state, "second")->messages.back().content, "/review file");
    application_find_thread(&state, "second")->messages.clear();
    EXPECT_EQ(request.history[0].content, "beforeafter");
}

TEST(Conversation, MetadataUpdatesTheOriginatingThreadWithoutRendering) {
    ApplicationState state = test_application();
    ConversationState conversations;
    TestProviderState provider_state;
    std::vector<ProviderPtr> providers;
    providers.push_back(test_provider(&provider_state));
    ComposerState composer;
    composer.message_input = "title me";
    ASSERT_EQ(conversation_submit(&conversations, &state, providers[0].get(), "second",
                                  &composer, {}).status, ResultStatus::Ok);
    conversation_update(&conversations, &state, providers);
    ASSERT_EQ(provider_state.requests.size(), 2u);
    const TurnId metadata_id = provider_state.requests.back().turn_id;
    state.selected_thread = 1;
    provider_state.events = {
        {EventKind::AssistantTextDelta, "second", metadata_id,
         "{\"title\":\"Generated title\",\"description\":\"Generated description\"}"},
        {EventKind::TurnCompleted, "second", metadata_id},
    };
    conversation_update(&conversations, &state, providers);
    EXPECT_EQ(application_find_thread(&state, "second")->title, "Generated title");
    EXPECT_EQ(application_find_thread(&state, "first")->title, "First");
    EXPECT_TRUE(conversations.metadata.empty());
    EXPECT_EQ(provider_state.requests.size(), 2u);
}

TEST(Conversation, SubmissionFailureProducesReadableMessageAndNoActiveTurn) {
    ApplicationState state = test_application();
    ConversationState conversations;
    TestProviderState provider_state;
    provider_state.reject_submit = true;
    ProviderPtr provider = test_provider(&provider_state);
    ComposerState composer;
    composer.message_input = "hello";
    EXPECT_EQ(conversation_submit(&conversations, &state, provider.get(), "second",
                                  &composer, {}).status, ResultStatus::Error);
    EXPECT_FALSE(conversation_is_generating(&conversations, "second"));
    EXPECT_EQ(chat_message_text(application_find_thread(&state, "second")->messages.back()),
              "submission failed");
}

TEST(Conversation, NormalizesLegacyAssistantTextWithoutDuplicatingSegments) {
    ChatMessage legacy{ChatMessageRole::Assistant, "legacy answer", {}, {}, {}};
    chat_message_normalize(&legacy);
    chat_message_normalize(&legacy);
    EXPECT_TRUE(legacy.content.empty());
    ASSERT_EQ(legacy.segments.size(), 1u);
    EXPECT_EQ(chat_message_text(legacy), "legacy answer");
}

TEST(Conversation, DeletedThreadCancelsTurnAndMetadataWork) {
    ApplicationState state = test_application();
    ConversationState conversations;
    TestProviderState provider_state;
    std::vector<ProviderPtr> providers;
    providers.push_back(test_provider(&provider_state));
    ComposerState composer;
    composer.message_input = "hello";
    ASSERT_EQ(conversation_submit(&conversations, &state, providers[0].get(), "second",
                                  &composer, {}).status, ResultStatus::Ok);
    conversation_update(&conversations, &state, providers);
    ASSERT_EQ(provider_state.requests.size(), 2u);
    state.projects[0].threads.erase(state.projects[0].threads.begin());
    conversation_update(&conversations, &state, providers);
    EXPECT_TRUE(conversations.turns.empty());
    EXPECT_TRUE(conversations.metadata.empty());
    EXPECT_EQ(provider_state.cancelled.size(), 2u);
}

static Result test_list_mcp(Provider*, const std::filesystem::path&, std::vector<McpServer>* servers) {
    McpServer server;
    server.name = "test-server";
    servers->push_back(std::move(server));
    return result_ok();
}

TEST(McpService, PublishesResultsAndCanRestartAfterShutdown) {
    TestProviderState provider_state;
    std::vector<ProviderPtr> providers;
    providers.push_back(test_provider(&provider_state));
    providers[0]->list_mcp_servers = test_list_mcp;
    McpService service;
    ASSERT_EQ(mcp_service_start(&service, &providers).status, ResultStatus::Ok);
    EXPECT_EQ(mcp_service_start(&service, &providers).status, ResultStatus::Error);
    McpTask task;
    task.working_directory = "/project";
    EXPECT_TRUE(mcp_service_submit(&service, task));
    std::deque<McpTaskResult> results;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (results.empty() && std::chrono::steady_clock::now() < deadline) {
        results = mcp_service_take_results(&service);
        if (results.empty())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    mcp_service_shutdown(&service);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].list_succeeded);
    ASSERT_EQ(results[0].servers.size(), 1u);
    EXPECT_EQ(results[0].servers[0].name, "test-server");
    EXPECT_FALSE(mcp_service_submit(&service, task));
    ASSERT_EQ(mcp_service_start(&service, &providers).status, ResultStatus::Ok);
    mcp_service_shutdown(&service);
}
