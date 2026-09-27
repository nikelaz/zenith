#ifndef APPLICATION_STATE_H
#define APPLICATION_STATE_H

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
    std::string command;
    std::string cwd;
    std::string output;
    std::string status;
    std::optional<int> exit_code;
    std::optional<int> duration_ms;
    bool completed = false;
};

struct ChatSegment {
    enum class Kind { Text, Tool } kind = Kind::Text;
    std::string text;
    ToolActivity tool;
};

struct ChatMessage {
    ChatMessageRole role;
    std::string content;
    std::string reasoning;
    std::vector<std::string> tool_activities;
    std::vector<ChatSegment> segments;
};

struct ChatThread {
    std::string title;
    std::string description;
    std::string id;
    std::vector<ChatMessage> messages;
};

struct ChatProject {
    std::filesystem::path directory;
    bool expanded = true;
    std::vector<ChatThread> threads;
};

struct ApplicationState {
    std::vector<ChatProject> projects = {
        {
            {},
            true,
            {
                {
                    "Project setup",
                    "Getting the application structure and core systems in place.",
                    "project-setup",
                    {}
                },
                {
                    "UI layout",
                    "Planning the workspace panels and how they fit together.",
                    "ui-layout",
                    {}
                },
            },
        },
    };
    std::size_t selected_project = 0;
    std::size_t selected_thread = 0;
};

#endif
