#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include "clipboard-image.h"
#include <cwchar>

namespace {
Result encode_bitmap(HBITMAP handle, ClipboardImage* image) {
    Gdiplus::Bitmap bitmap(handle, nullptr);
    if (bitmap.GetLastStatus() != Gdiplus::Ok)
        return result_error("Could not read the clipboard bitmap.");
    UINT count = 0;
    UINT size = 0;
    if (Gdiplus::GetImageEncodersSize(&count, &size) != Gdiplus::Ok || size == 0)
        return result_error("Could not find a PNG encoder.");
    std::vector<std::uint8_t> storage(size);
    auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
    if (Gdiplus::GetImageEncoders(count, size, encoders) != Gdiplus::Ok)
        return result_error("Could not find a PNG encoder.");
    const CLSID* encoder = nullptr;
    for (UINT index = 0; index < count; ++index) {
        if (std::wcscmp(encoders[index].MimeType, L"image/png") == 0) {
            encoder = &encoders[index].Clsid;
            break;
        }
    }
    if (encoder == nullptr)
        return result_error("Could not find a PNG encoder.");
    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)))
        return result_error("Could not allocate clipboard image storage.");
    Result result = result_ok();
    STATSTG stat{};
    if (bitmap.Save(stream, encoder, nullptr) != Gdiplus::Ok ||
        FAILED(stream->Stat(&stat, STATFLAG_NONAME))) {
        result = result_error("Could not convert the clipboard image to PNG.");
    } else if (stat.cbSize.QuadPart > maximum_clipboard_image_bytes) {
        result = result_error("The clipboard image exceeds the 32 MiB limit.");
    } else {
        image->content.resize(static_cast<std::size_t>(stat.cbSize.QuadPart));
        LARGE_INTEGER start{};
        ULONG read_size = 0;
        if (FAILED(stream->Seek(start, STREAM_SEEK_SET, nullptr)) ||
            FAILED(stream->Read(image->content.data(), static_cast<ULONG>(image->content.size()),
                                &read_size)) || read_size != image->content.size()) {
            image->content.clear();
            result = result_error("Could not read the encoded clipboard image.");
        }
    }
    stream->Release();
    return result;
}
}

Result read_clipboard_image(ClipboardImage* image) {
    *image = {};
    const UINT png_format = RegisterClipboardFormatW(L"PNG");
    const bool has_png = png_format != 0 && IsClipboardFormatAvailable(png_format);
    if (!has_png && !IsClipboardFormatAvailable(CF_BITMAP))
        return result_ok();
    if (!OpenClipboard(nullptr))
        return result_error("The clipboard is busy. Try pasting again.");
    Result result = result_ok();
    if (has_png) {
        HANDLE handle = GetClipboardData(png_format);
        const SIZE_T size = handle == nullptr ? 0 : GlobalSize(handle);
        if (size == 0 || size > maximum_clipboard_image_bytes) {
            result = result_error("The clipboard image is empty or exceeds the 32 MiB limit.");
        } else {
            const auto* bytes = static_cast<const std::uint8_t*>(GlobalLock(handle));
            if (bytes == nullptr) {
                result = result_error("Could not read the clipboard image.");
            } else {
                image->content.assign(bytes, bytes + size);
                GlobalUnlock(handle);
            }
        }
    } else {
        Gdiplus::GdiplusStartupInput startup;
        ULONG_PTR token = 0;
        if (Gdiplus::GdiplusStartup(&token, &startup, nullptr) != Gdiplus::Ok) {
            result = result_error("Could not initialize clipboard image conversion.");
        } else {
            result = encode_bitmap(static_cast<HBITMAP>(GetClipboardData(CF_BITMAP)), image);
            Gdiplus::GdiplusShutdown(token);
        }
    }
    CloseClipboard();
    if (result.status == ResultStatus::Ok) {
        image->media_type = "image/png";
        image->extension = ".png";
    }
    return result;
}
