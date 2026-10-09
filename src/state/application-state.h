#ifndef APPLICATION_STATE_H
#define APPLICATION_STATE_H

#include "chat.h"

struct ApplicationState {
    int base_font_size = 16;
    float ui_scale = 1.0f;
    bool collapse_tool_calls = true;
    std::string thread_metadata_provider;
    std::string thread_metadata_model;
    std::vector<ChatProject> projects = {
        {
            {},
            true,
            {
                {
                    "",
                    "",
                    "project-setup",
                    {}
                },
            },
        },
    };
    std::size_t selected_project = 0;
    std::size_t selected_thread = 0;
};

#endif
