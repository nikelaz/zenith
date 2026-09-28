#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <GLFW/glfw3.h>
#include "application.h"
#include "base/result.h"
#include <tinyfiledialogs.h>
#include <string>

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
#else
int main() {
#endif
    Application app;

    Result app_init_result = app.init();
    if (app_init_result.status == ResultStatus::Error) {
        const std::string error_message(app_init_result.error);
        tinyfd_messageBox("Zenith", error_message.c_str(), "ok", "error", 1);
        return 1;
    }

    app.run();
    return 0;
}
