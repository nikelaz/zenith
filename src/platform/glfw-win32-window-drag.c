#include "internal.h"

GLFWAPI int zenith_begin_win32_window_drag(GLFWwindow* handle)
{
    _GLFWwindow* window = (_GLFWwindow*) handle;
    if (!window || _glfw.platform.platformID != GLFW_PLATFORM_WIN32 ||
        window->mouseButtons[GLFW_MOUSE_BUTTON_LEFT] != GLFW_PRESS)
    {
        return GLFW_FALSE;
    }

    POINT cursor;
    if (!GetCursorPos(&cursor))
        return GLFW_FALSE;

    if (GetCapture() == window->win32.handle && !ReleaseCapture())
        return GLFW_FALSE;

    _glfwInputMouseClick(window, GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE, 0);
    SendMessageW(window->win32.handle, WM_NCLBUTTONDOWN, HTCAPTION,
                 MAKELPARAM(cursor.x, cursor.y));
    return GLFW_TRUE;
}
