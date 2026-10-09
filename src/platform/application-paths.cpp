#include "application-paths.h"
#include "../base/os.h"

#include <cstdlib>
#include <string>
#include <system_error>

#if OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#endif

static Result ensure_data_directory(const std::filesystem::path& directory,
                                    std::filesystem::path* path) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
        return result_error("Failed to create the Zenith data folder: " + error.message());
    *path = directory / "Zenith.sqlite3";
    return result_ok();
}

Result application_database_path(std::filesystem::path* path) {
    if (path == nullptr)
        return result_error("Database path output is null");

#if OS_WIN
    PWSTR local_app_data = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE,
                                                nullptr, &local_app_data);
    if (FAILED(result) || local_app_data == nullptr) {
        if (local_app_data != nullptr)
            CoTaskMemFree(local_app_data);
        return result_error("Failed to locate the local application data folder");
    }
    const std::filesystem::path directory =
        std::filesystem::path(local_app_data) / L"Zenith";
    CoTaskMemFree(local_app_data);
    return ensure_data_directory(directory, path);
#elif OS_MAC
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0')
        return result_error("Failed to locate the user home folder");
    return ensure_data_directory(std::filesystem::path(home) /
                                     "Library" / "Application Support" / "Zenith",
                                 path);
#elif OS_LINUX
    const char* xdg_data_home = std::getenv("XDG_DATA_HOME");
    std::filesystem::path data_directory;
    if (xdg_data_home != nullptr && *xdg_data_home != '\0') {
        data_directory = xdg_data_home;
    } else {
        const char* home = std::getenv("HOME");
        if (home == nullptr || *home == '\0')
            return result_error("Failed to locate the user home folder");
        data_directory = std::filesystem::path(home) / ".local" / "share";
    }
    return ensure_data_directory(data_directory / "zenith", path);
#endif
}
