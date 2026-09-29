#ifndef PERSISTENT_STORE_H
#define PERSISTENT_STORE_H

#include "../base/result.h"
#include "../state/application-state.h"
#include <string>
#include <string_view>

struct sqlite3;
struct WindowState {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool maximized = false;
};

class PersistentStore {
private:
    sqlite3* m_database = nullptr;
    std::string m_last_error;

    Result fail(std::string_view operation);
    Result execute(const char* sql, std::string_view operation);

public:
    PersistentStore() = default;
    ~PersistentStore();

    PersistentStore(const PersistentStore&) = delete;
    PersistentStore& operator=(const PersistentStore&) = delete;

    Result open(const std::string& path);
    Result load(ApplicationState& state);
    Result save(const ApplicationState& state);
    Result load_window(WindowState* window);
    Result save_window(const WindowState& window);
    void close();
};

#endif
