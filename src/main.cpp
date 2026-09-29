#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include "application.h"
#include "base/result.h"
#include "platform/message-box.h"
#include <string>

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
#else
int main() {
#endif
    Application app;
    SDL_SetMainReady();

    Result app_init_result = app.init();
    if (app_init_result.status == ResultStatus::Error) {
        const std::string error_message(app_init_result.error);
        show_error_message(error_message.c_str());
        return 1;
    }

    app.run();
    return 0;
}
