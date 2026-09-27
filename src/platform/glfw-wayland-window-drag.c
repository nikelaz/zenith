#include "internal.h"
#include "xdg-shell-client-protocol.h"

GLFWAPI int zenith_begin_wayland_window_drag(GLFWwindow* handle)
{
    _GLFWwindow* window = (_GLFWwindow*) handle;
    if (!window || _glfw.platform.platformID != GLFW_PLATFORM_WAYLAND ||
        !_glfw.wl.seat || !_glfw.wl.pointer || !_glfw.wl.serial ||
        _glfw.wl.pointerSurface != window->wl.surface ||
        window->mouseButtons[GLFW_MOUSE_BUTTON_LEFT] != GLFW_PRESS)
    {
        return GLFW_FALSE;
    }

    struct xdg_toplevel* toplevel = window->wl.xdg.toplevel;
    if (!toplevel && window->wl.libdecor.frame)
        toplevel = libdecor_frame_get_xdg_toplevel(window->wl.libdecor.frame);

    if (toplevel)
    {
        xdg_toplevel_move(toplevel, _glfw.wl.seat, _glfw.wl.serial);
        wl_display_flush(_glfw.wl.display);
        return GLFW_TRUE;
    }

    return GLFW_FALSE;
}
