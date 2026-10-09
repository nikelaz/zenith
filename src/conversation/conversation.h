#ifndef CONVERSATION_H
#define CONVERSATION_H

#include "../providers/provider.h"
#include "../state/application-state.h"
#include <limits>
#include <span>
#include <unordered_map>

struct ComposerState {
    std::string message_input;
    std::vector<FileReference> file_references;
    std::vector<FileAttachment> attachments;
    std::string attachment_error;
};

struct ConversationTurn {
    TurnId id = 0;
    Provider* provider = nullptr; // Providers outlive conversation work.
    std::size_t assistant_message = std::numeric_limits<std::size_t>::max();
};

struct PendingThreadMetadata {
    ConversationId conversation_id;
    Provider* provider = nullptr;
    std::string response;
};

struct ConversationState {
    TurnId next_turn_id = 1;
    std::unordered_map<ConversationId, ConversationTurn> turns;
    std::unordered_map<TurnId, PendingThreadMetadata> metadata;
};

ChatThread* application_find_thread(ApplicationState* state, std::string_view id);
std::string chat_message_text(const ChatMessage& message);
void chat_message_normalize(ChatMessage* message);
std::string provider_command_prompt(std::string prompt, std::span<const SkillEntry> commands);
void conversation_update_metadata_model(ApplicationState* state,
                                        const std::vector<ProviderPtr>& providers);
bool conversation_is_generating(const ConversationState* conversations, const ConversationId& id);
Result conversation_submit(ConversationState* conversations, ApplicationState* state,
                           Provider* provider, ConversationId id, ComposerState* composer,
                           std::span<const SkillEntry> commands);
void conversation_cancel(ConversationState* conversations, const ConversationId& id);
void conversation_apply_event(ConversationState* conversations, ApplicationState* state,
                              Provider* provider, const Event& event);
void conversation_update(ConversationState* conversations, ApplicationState* state,
                         std::vector<ProviderPtr>& providers);
void conversation_clear(ConversationState* conversations);

#endif
