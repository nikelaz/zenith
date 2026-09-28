#include "internal.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <windowsx.h>

static LRESULT CALLBACK window_management_proc(HWND hwnd, UINT message,
                                                WPARAM wparam, LPARAM lparam,
                                                UINT_PTR subclass_id,
                                                DWORD_PTR reference_data)
{
    (void) reference_data;

    if (message == WM_NCCALCSIZE)
    {
        // Keep the snap-capable window style without reserving space for its
        // native frame.  The whole window remains available to the renderer.
        return 0;
    }

    if (message == WM_NCHITTEST && !IsZoomed(hwnd))
    {
        RECT rect;
        if (GetWindowRect(hwnd, &rect))
        {
            int border_x;
            int border_y;
            if (_glfwIsWindows10Version1607OrGreaterWin32())
            {
                const UINT dpi = GetDpiForWindow(hwnd);
                border_x = GetSystemMetricsForDpi(SM_CXFRAME, dpi) +
                           GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                border_y = GetSystemMetricsForDpi(SM_CYFRAME, dpi) +
                           GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            }
            else
            {
                border_x = GetSystemMetrics(SM_CXFRAME) +
                           GetSystemMetrics(SM_CXPADDEDBORDER);
                border_y = GetSystemMetrics(SM_CYFRAME) +
                           GetSystemMetrics(SM_CXPADDEDBORDER);
            }

            const int x = GET_X_LPARAM(lparam);
            const int y = GET_Y_LPARAM(lparam);
            const int left = x < rect.left + border_x;
            const int right = x >= rect.right - border_x;
            const int top = y < rect.top + border_y;
            const int bottom = y >= rect.bottom - border_y;

            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
        }
    }

    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(hwnd, window_management_proc, subclass_id);

    return DefSubclassProc(hwnd, message, wparam, lparam);
}

GLFWAPI void zenith_enable_win32_window_management(GLFWwindow* handle)
{
    _GLFWwindow* window = (_GLFWwindow*) handle;
    if (!window || _glfw.platform.platformID != GLFW_PLATFORM_WIN32)
        return;

    HWND hwnd = window->win32.handle;
    if (!SetWindowSubclass(hwnd, window_management_proc, 0, 0))
        return;

    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    style |= WS_THICKFRAME | WS_MAXIMIZEBOX | WS_MINIMIZEBOX;
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_FRAMECHANGED);

    // DWM can draw a one-pixel border even when there is no non-client area.
    const COLORREF borderColor = DWMWA_COLOR_NONE;
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &borderColor,
                          sizeof(borderColor));
}

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
