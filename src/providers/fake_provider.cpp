#include "provider_runtime.h"
#include <utility>


struct FakeState {
    ProviderRuntime runtime;
};

static void process_fake(void*, const TurnRequest* request, ProviderRuntime* runtime) {
    const Event response{EventKind::AssistantTextDelta,
                         request->conversation_id,
                         request->turn_id,
                         "How can I help you?",
                         {},
                         {}};
    provider_runtime_emit(runtime, &response);

    const Event completed{EventKind::TurnCompleted, request->conversation_id, request->turn_id,
                          {}, {}, {}};
    provider_runtime_emit(runtime, &completed);
}

static Result start_fake(Provider* provider) {
    FakeState* state = static_cast<FakeState*>(provider->state);
    return provider_runtime_start(&state->runtime);
}

static Result submit_fake(Provider* provider, TurnRequest request) {
    FakeState* state = static_cast<FakeState*>(provider->state);
    return provider_runtime_submit(&state->runtime, std::move(request));
}

static Result respond_fake(Provider*, const ProviderRequestId&, ApprovalDecision) {
    return result_error("Provider has no pending approval request");
}

static void cancel_fake(Provider*, TurnId) {}

static std::vector<Event> poll_fake(Provider* provider) {
    FakeState* state = static_cast<FakeState*>(provider->state);
    return provider_runtime_poll_events(&state->runtime);
}

static void destroy_fake(Provider* provider) {
    FakeState* state = static_cast<FakeState*>(provider->state);
    provider_runtime_shutdown(&state->runtime);
    delete state;
    delete provider;
}

static void request_fake_shutdown(Provider* provider) {
    FakeState* state = static_cast<FakeState*>(provider->state);
    provider_runtime_request_shutdown(&state->runtime);
}


ProviderPtr make_fake_provider() {
    FakeState* state = new FakeState{};
    provider_runtime_init(&state->runtime, process_fake, nullptr);

    Provider* provider = new Provider{"fake", state, start_fake, submit_fake, respond_fake,
                                      cancel_fake, poll_fake, destroy_fake};
    provider->request_shutdown = request_fake_shutdown;
    provider->default_model = "fake-model";
    provider->models.push_back({"fake-model", "Fake model", {}, {}, {}, {}});
    return ProviderPtr(provider, destroy_provider);
}
