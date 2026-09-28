#include "clipboard-image.h"
#include "../process/child-process.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <poll.h>
#include <unistd.h>

namespace {
constexpr auto clipboard_timeout = std::chrono::seconds(2);

Result read_clipboard_command(const std::filesystem::path& executable,
                              const std::vector<std::string>& arguments,
                              std::vector<std::uint8_t>* content) {
    ChildProcess process;
    Result result = child_process_start(&process, executable, arguments, "clipboard reader");
    if (result.status == ResultStatus::Error)
        return result;
    const auto deadline = std::chrono::steady_clock::now() + clipboard_timeout;
    const int descriptor = fileno(process.output);
    std::array<std::uint8_t, 16384> buffer;
    for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) {
            result = result_error("Reading the clipboard timed out. Try copying the image again.");
            break;
        }
        pollfd descriptor_state{descriptor, POLLIN, 0};
        const int ready = poll(&descriptor_state, 1, static_cast<int>(remaining));
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0) {
            result = result_error("Could not read the clipboard image.");
            break;
        }
        const ssize_t size = read(descriptor, buffer.data(), buffer.size());
        if (size == 0)
            break;
        if (size < 0) {
            if (errno == EINTR)
                continue;
            result = result_error("Could not read the clipboard image.");
            break;
        }
        if (content->size() + static_cast<std::size_t>(size) > maximum_clipboard_image_bytes) {
            result = result_error("The clipboard image exceeds the 32 MiB limit.");
            break;
        }
        content->insert(content->end(), buffer.begin(), buffer.begin() + size);
    }
    if (result.status == ResultStatus::Error) {
        content->clear();
        kill(static_cast<pid_t>(process.process_id), SIGKILL);
    }
    child_process_stop(&process);
    return result;
}
}

Result read_clipboard_image(ClipboardImage* image) {
    *image = {};
    const char* video_driver = SDL_GetCurrentVideoDriver();
    const bool wayland = video_driver != nullptr && std::string_view(video_driver) == "wayland";
    const auto executable = child_process_resolve_executable(wayland ? "wl-paste" : "xclip");
    if (executable.empty()) {
        return result_error(wayland
            ? "Pasting images requires wl-clipboard on Wayland. You can also use Attach files."
            : "Pasting images requires xclip on X11. You can also use Attach files.");
    }
    std::vector<std::uint8_t> formats;
    Result result = read_clipboard_command(executable, wayland
        ? std::vector<std::string>{"--list-types"}
        : std::vector<std::string>{"-selection", "clipboard", "-t", "TARGETS", "-o"}, &formats);
    if (result.status == ResultStatus::Error)
        return result;
    const std::string types = "\n" + std::string(formats.begin(), formats.end()) + "\n";
    if (types.find("\nimage/png\n") == std::string::npos)
        return result_ok();
    result = read_clipboard_command(executable, wayland
        ? std::vector<std::string>{"--no-newline", "--type", "image/png"}
        : std::vector<std::string>{"-selection", "clipboard", "-t", "image/png", "-o"},
        &image->content);
    if (result.status == ResultStatus::Error)
        return result;
    constexpr std::array<std::uint8_t, 8> signature = {137, 80, 78, 71, 13, 10, 26, 10};
    if (image->content.size() < signature.size() ||
        !std::equal(signature.begin(), signature.end(), image->content.begin())) {
        image->content.clear();
        return result_error("The clipboard did not contain a valid PNG image. Try copying it again.");
    }
    image->media_type = "image/png";
    image->extension = ".png";
    return result_ok();
}
