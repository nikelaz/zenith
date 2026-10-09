#include "file-dialogs.h"
#include <SDL3/SDL.h>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <utility>


struct FileDialogCallbackData {
    std::shared_ptr<FileDialogQueue> queue;
    FileDialogPurpose purpose;
};

void queue_file_dialog_result(const std::shared_ptr<FileDialogQueue>& queue,
                              FileDialogResult result) {
    std::lock_guard lock(queue->mutex);
    queue->results.push_back(std::move(result));
}

void SDLCALL file_dialog_callback(void* userdata, const char* const* filelist, int) {
    std::unique_ptr<FileDialogCallbackData> callback_data(
        static_cast<FileDialogCallbackData*>(userdata));
    const std::shared_ptr<FileDialogQueue>& queue = callback_data->queue;
    FileDialogResult result{callback_data->purpose, {}, {}};

    if (filelist == nullptr) {
        result.error = SDL_GetError();
    } else {
        try {
            for (const char* const* path = filelist; *path != nullptr; ++path)
                result.paths.emplace_back(
                    std::u8string(reinterpret_cast<const char8_t*>(*path)));
        } catch (const std::exception& error) {
            result.error = std::string("Failed to read file dialog results: ") + error.what();
        } catch (...) {
            result.error = "Failed to read file dialog results";
        }
    }

    try {
        std::lock_guard lock(queue->mutex);
        queue->results.push_back(std::move(result));
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Zenith: failed to store file dialog result: %s\n", error.what());
    } catch (...) {
        std::fprintf(stderr, "Zenith: failed to store file dialog result\n");
    }
}


void show_file_dialog(const std::shared_ptr<FileDialogQueue>& queue,
                      FileDialogPurpose purpose, SDL_Window* window,
                      const char* default_location) {
    SDL_PropertiesID properties = SDL_CreateProperties();
    if (properties == 0) {
        queue_file_dialog_result(queue, {purpose, {}, SDL_GetError()});
        return;
    }

    const char* title = purpose == FileDialogPurpose::OpenProject
        ? "Open Project" : "Attach files";
    const bool properties_set =
        SDL_SetPointerProperty(properties, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, window) &&
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING, title) &&
        SDL_SetBooleanProperty(properties, SDL_PROP_FILE_DIALOG_MANY_BOOLEAN,
                               purpose == FileDialogPurpose::AttachFiles) &&
        (default_location == nullptr ||
         SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_LOCATION_STRING,
                               default_location));
    if (!properties_set) {
        const std::string error = SDL_GetError();
        SDL_DestroyProperties(properties);
        queue_file_dialog_result(queue, {purpose, {}, error});
        return;
    }

    auto* callback_data = new FileDialogCallbackData{queue, purpose};
    SDL_ShowFileDialogWithProperties(
        purpose == FileDialogPurpose::OpenProject ? SDL_FILEDIALOG_OPENFOLDER
                                                  : SDL_FILEDIALOG_OPENFILE,
        file_dialog_callback, callback_data, properties);
    SDL_DestroyProperties(properties);
}

std::vector<FileDialogResult> take_file_dialog_results(FileDialogQueue& queue) {
    std::lock_guard lock(queue.mutex);
    std::vector<FileDialogResult> results = std::move(queue.results);
    queue.results.clear();
    return results;
}
