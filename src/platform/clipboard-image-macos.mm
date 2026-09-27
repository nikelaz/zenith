#include "clipboard-image.h"
#import <AppKit/AppKit.h>

Result read_clipboard_image(ClipboardImage* image) {
    *image = {};
    @autoreleasepool {
        NSPasteboard* clipboard = [NSPasteboard generalPasteboard];
        NSData* png = [clipboard dataForType:NSPasteboardTypePNG];
        if (png == nil) {
            NSData* tiff = [clipboard dataForType:NSPasteboardTypeTIFF];
            if (tiff == nil)
                return result_ok();
            if (tiff.length > maximum_clipboard_image_bytes)
                return result_error("The clipboard image exceeds the 32 MiB limit.");
            NSBitmapImageRep* bitmap = [NSBitmapImageRep imageRepWithData:tiff];
            png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
            if (png == nil)
                return result_error("Could not convert the clipboard image to PNG.");
        }
        if (png.length > maximum_clipboard_image_bytes)
            return result_error("The clipboard image exceeds the 32 MiB limit.");
        if (png.length == 0)
            return result_error("The clipboard image is empty.");
        const auto* bytes = static_cast<const std::uint8_t*>(png.bytes);
        image->content.assign(bytes, bytes + png.length);
        image->media_type = "image/png";
        image->extension = ".png";
    }
    return result_ok();
}
