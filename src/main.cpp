#include <GLFW/glfw3.h>
#include "application.h"
#include "base/result.h"
#include <tinyfiledialogs.h>
#include <string>

int main() {
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
