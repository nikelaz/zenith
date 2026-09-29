#include "clipboard-image.h"
#include <SDL3/SDL.h>
#include <array>
#include <string>

namespace {
struct ClipboardImageFormat {
    const char* media_type;
    const char* extension;
};

constexpr std::array<ClipboardImageFormat, 6> clipboard_image_formats = {{
    {"image/png", ".png"},
    {"image/jpeg", ".jpg"},
    {"image/webp", ".webp"},
    {"image/gif", ".gif"},
    {"image/bmp", ".bmp"},
    {"image/tiff", ".tiff"},
}};
}

Result read_clipboard_image(ClipboardImage* image) {
    *image = {};

    const ClipboardImageFormat* format = nullptr;
    for (const ClipboardImageFormat& candidate : clipboard_image_formats) {
        if (SDL_HasClipboardData(candidate.media_type)) {
            format = &candidate;
            break;
        }
    }
    if (format == nullptr)
        return result_ok();

    std::size_t size = 0;
    auto* content = static_cast<std::uint8_t*>(
        SDL_GetClipboardData(format->media_type, &size));
    if (content == nullptr)
        return result_error(std::string("Could not read the clipboard image: ") + SDL_GetError());
    if (size == 0 || size > maximum_clipboard_image_bytes) {
        SDL_free(content);
        return result_error(size == 0
            ? "The clipboard image is empty."
            : "The clipboard image exceeds the 32 MiB limit.");
    }

    image->content.assign(content, content + size);
    SDL_free(content);
    image->media_type = format->media_type;
    image->extension = format->extension;
    return result_ok();
}
