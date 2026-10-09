#ifndef PLATFORM_APPLICATION_PATHS_H
#define PLATFORM_APPLICATION_PATHS_H

#include "../base/result.h"
#include <filesystem>

Result application_database_path(std::filesystem::path* path);

#endif
