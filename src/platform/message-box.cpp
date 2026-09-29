#include "message-box.h"
#include <SDL3/SDL.h>
#include <cstdio>

void show_error_message(const char* message, SDL_Window* window) {
    const char* error_message = message == nullptr ? "" : message;
    if (!SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Zenith", error_message, window))
        std::fprintf(stderr, "Zenith: %s\nSDL message box failed: %s\n",
                     error_message, SDL_GetError());
}
