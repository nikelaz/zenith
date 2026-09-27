#ifndef APPLICATION_H
#define APPLICATION_H

#include "base/result.h"
#include "persistence/persistent-store.h"
#include "providers/provider.h"
#include "state/application-state.h"
#include "ui/ui-system.h"
#include <GLFW/glfw3.h>
#include <optional>

class Application {
public:
    GLFWwindow* m_window = nullptr;
    std::optional<UISystem> m_ui;

    Result init();
    void deinit();
    void run();

private:
    bool m_initialized = false;
    ApplicationState m_state;
    PersistentStore m_state_store;
    std::vector<ProviderPtr> m_providers;

    Result window_init();
    void window_deinit();
};

#endif
