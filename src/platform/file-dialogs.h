#ifndef FILE_DIALOGS_H
#define FILE_DIALOGS_H

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct SDL_Window;

enum class FileDialogPurpose {
    OpenProject,
    AttachFiles,
};

struct FileDialogResult {
    FileDialogPurpose purpose;
    std::vector<std::filesystem::path> paths;
    std::string error;
};

struct FileDialogQueue {
    std::mutex mutex;
    std::vector<FileDialogResult> results;
};

void show_file_dialog(const std::shared_ptr<FileDialogQueue>& queue,
                      FileDialogPurpose purpose, SDL_Window* window,
                      const char* default_location = nullptr);
std::vector<FileDialogResult> take_file_dialog_results(FileDialogQueue& queue);

#endif
