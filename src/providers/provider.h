#ifndef PROVIDER_H
#define PROVIDER_H

#include "../base/result.h"
#include "../state/application-state.h"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using ConversationId = std::string;
using TurnId = std::uint64_t;
using ProviderRequestId = std::string;

struct ReasoningOption {
    std::string value;
    std::string description;
};

struct PermissionOption {
    std::string value;
    std::string name;
    std::string description;
};

struct ModelOption {
    std::string id;
    std::string name;
    std::string default_reasoning_effort;
    std::vector<ReasoningOption> reasoning_efforts;
    std::string default_permission_mode;
    std::vector<PermissionOption> permission_modes;
};

struct UsageMetric {
    std::string name;
    std::string value;
    std::string detail;
    std::string period;
    std::optional<double> used;
    std::optional<double> limit;
    std::optional<double> remaining;
    std::string reset_at;
};

struct UsageSnapshot {
    std::vector<UsageMetric> metrics;
    std::string updated_at;
};

enum class SkillEntryKind { Skill, CommandOrSkill };

struct SkillEntry {
    std::string name;
    std::string description;
    std::filesystem::path path;
    std::string scope;
    std::string invocation;
    std::string input_hint;
    bool enabled = true;
    SkillEntryKind kind = SkillEntryKind::Skill;
};

struct SkillDiscoverySnapshot {
    std::filesystem::path working_directory;
    std::vector<SkillEntry> entries;
    std::vector<std::string> errors;
    std::string error;
};

struct FileReference {
    std::filesystem::path path;
};

struct FileAttachment {
    std::filesystem::path path;
    std::string filename;
    std::string media_type;
    std::vector<std::uint8_t> content;
};

enum class EventKind {
    ProviderThreadStarted,
    AssistantTextDelta,
    AssistantReasoningDelta,
    ReasoningSummaryDelta,
    ToolActivity,
    ApprovalRequested,
    TurnCompleted,
    TurnFailed,
};

struct Event {
    EventKind kind;
    ConversationId conversation_id;
    TurnId turn_id = 0;
    std::string text{};
    std::string provider_thread_id{};
    ProviderRequestId provider_request_id{};
    std::string item_id{};
    std::string cwd{};
    std::string output{};
    int exit_code = -1;
    bool tool_completed = false;
    bool output_is_delta = false;
    std::string status{};
    int duration_ms = -1;
    std::string tool_name{};
    std::string tool_arguments{};
    bool is_terminal = false;
};

struct TurnRequest {
    TurnId turn_id = 0;
    ConversationId conversation_id;
    std::string prompt;
    std::vector<ChatMessage> history;
    std::vector<FileReference> file_references;
    std::vector<FileAttachment> attachments;
    std::filesystem::path working_directory;
    std::string model;
    std::string provider_thread_id;
    std::string reasoning_effort;
    std::string permission_mode;
};

enum class ApprovalDecision { ApproveOnce, Deny };
enum class ProviderAvailability { Unknown, Available, Unavailable };

enum class McpServerTransport { Stdio, Http };

struct McpServer {
    std::string name;
    McpServerTransport transport = McpServerTransport::Stdio;
    std::string command;
    std::vector<std::string> arguments;
    std::map<std::string, std::string> environment;
    std::string url;
    std::string bearer_token_env_var;
    std::map<std::string, std::string> headers;
    std::string source;
    std::string status = "Unknown";
    std::string status_detail;
    bool enabled = true;
    bool editable = true;
    bool removable = true;
};

struct Provider;
using ProviderStartFn = Result (*)(Provider*);
using ProviderSubmitFn = Result (*)(Provider*, TurnRequest);
using ProviderRespondFn = Result (*)(Provider*, const ProviderRequestId&, ApprovalDecision);
using ProviderCancelFn = void (*)(Provider*, TurnId);
using ProviderPollFn = std::vector<Event> (*)(Provider*);
using ProviderRequestUsageFn = void (*)(Provider*);
using ProviderPollUsageFn = std::optional<UsageSnapshot> (*)(Provider*);
using ProviderRequestSkillsFn = Result (*)(Provider*, const std::filesystem::path&, bool);
using ProviderPollSkillsFn = std::vector<SkillDiscoverySnapshot> (*)(Provider*);
using ProviderListMcpServersFn = Result (*)(Provider*, const std::filesystem::path&,
                                           std::vector<McpServer>*);
using ProviderUpsertMcpServerFn = Result (*)(Provider*, const std::filesystem::path&,
                                             std::string_view, const McpServer&);
using ProviderRemoveMcpServerFn = Result (*)(Provider*, const std::filesystem::path&,
                                             std::string_view);
using ProviderSetMcpEnabledFn = Result (*)(Provider*, const std::filesystem::path&,
                                           std::string_view, bool);
using ProviderDestroyFn = void (*)(Provider*);
using ProviderEventSink = void (*)(void*, const Event*);

struct Provider {
    std::string_view name;
    void* state;
    ProviderStartFn start;
    ProviderSubmitFn submit;
    ProviderRespondFn respond_to_request;
    ProviderCancelFn cancel;
    ProviderPollFn poll_events;
    ProviderDestroyFn destroy;
    ProviderRequestUsageFn request_usage = nullptr;
    ProviderPollUsageFn poll_usage = nullptr;
    ProviderRequestSkillsFn request_skills = nullptr;
    ProviderPollSkillsFn poll_skills = nullptr;
    ProviderListMcpServersFn list_mcp_servers = nullptr;
    ProviderUpsertMcpServerFn upsert_mcp_server = nullptr;
    ProviderRemoveMcpServerFn remove_mcp_server = nullptr;
    ProviderSetMcpEnabledFn set_mcp_server_enabled = nullptr;
    std::vector<ModelOption> models{};
    std::string default_model{};
    ProviderAvailability availability = ProviderAvailability::Unknown;
    std::filesystem::path location{};
};

inline void destroy_provider(Provider* provider) {
    if (provider != nullptr && provider->destroy != nullptr)
        provider->destroy(provider);
}

using ProviderPtr = std::unique_ptr<Provider, decltype(&destroy_provider)>;

struct CodexOptions {
    std::filesystem::path executable = "codex";
    std::filesystem::path codex_home;
    std::string default_model = "gpt-6-luna";
    Result (*execute)(void*, const TurnRequest*, ProviderEventSink, void*) = nullptr;
    void* execute_context = nullptr;
};

struct GitHubCopilotOptions {
    std::filesystem::path executable = "copilot";
    std::filesystem::path diagnostics_path;
    std::string default_model = "auto";
    Result (*execute)(void*, const TurnRequest*, ProviderEventSink, void*) = nullptr;
    void* execute_context = nullptr;
};

ProviderPtr make_fake_provider();
ProviderPtr make_codex_provider(const CodexOptions* options = nullptr);
ProviderPtr make_github_copilot_provider(const GitHubCopilotOptions* options = nullptr);

#endif
