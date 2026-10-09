#ifndef CHAT_H
#define CHAT_H

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

enum class ChatMessageRole {
    User,
    Assistant,
};

struct ToolActivity {
    std::string id;
    std::string name;
    std::string command;
    std::string arguments;
    std::string cwd;
    std::string output;
    std::string status;
    std::optional<int> exit_code;
    std::optional<int> duration_ms;
    bool is_terminal = false;
    bool completed = false;
};

struct ChatSegment {
    enum class Kind { Text, Tool } kind = Kind::Text;
    std::string text;
    ToolActivity tool;
};

struct ChatAttachment {
    std::string filename;
    std::string media_type;
    std::size_t size_bytes = 0;
};

struct ChatMessage {
    ChatMessageRole role = ChatMessageRole::User;
    std::string content; // User text; assistant text lives in segments after normalization.
    std::string reasoning;
    std::vector<ChatSegment> segments;
    std::vector<ChatAttachment> attachments;
};

struct ChatThread {
    std::string title;
    std::string description;
    std::string id;
    std::vector<ChatMessage> messages;
    std::string provider;
    std::string model;
    std::string reasoning_effort;
    std::string permission_mode;
    bool title_generation_attempted = false;
};

struct ChatProject {
    std::filesystem::path directory;
    bool expanded = true;
    std::vector<ChatThread> threads;
};

#endif
