#ifndef CLIPBOARD_IMAGE_H
#define CLIPBOARD_IMAGE_H

#include "../base/result.h"
#include <cstdint>
#include <string>
#include <vector>

constexpr std::size_t maximum_clipboard_image_bytes = 32 * 1024 * 1024;

struct ClipboardImage {
    std::vector<std::uint8_t> content;
    std::string media_type;
    std::string extension;
};

Result read_clipboard_image(ClipboardImage* image);

#endif
