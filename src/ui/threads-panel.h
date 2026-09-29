#ifndef THREADS_PANEL_H
#define THREADS_PANEL_H

#include "../state/application-state.h"
#include "../platform/file-dialogs.h"

struct SDL_Window;

void open_project_dialog(ApplicationState& state,
                         const std::shared_ptr<FileDialogQueue>& dialog_queue,
                         SDL_Window* window);
void apply_open_project_result(ApplicationState& state, const FileDialogResult& result,
                               SDL_Window* window);
void render_threads_panel(ApplicationState& state,
                          const std::shared_ptr<FileDialogQueue>& dialog_queue,
                          SDL_Window* window);

#endif
