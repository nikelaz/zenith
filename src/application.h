#ifndef APPLICATION_H
#define APPLICATION_H

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <optional>

#include "base/result.h"
#include "persistence/persistent-store.h"
#include "providers/provider.h"
#include "state/application-state.h"
#include "ui/ui-system.h"

class Application {
public:
    SDL_Window* m_window = nullptr;
    std::optional<UISystem> m_ui;

    ~Application();
    Result init();
    void deinit();
    void run();

private:
    bool m_initialized = false;
    bool m_quit_requested = false;
    SDL_GPUDevice* m_gpu_device = nullptr;
    ApplicationState m_state;
    PersistentStore m_state_store;
    WindowState m_window_state;
    std::vector<ProviderPtr> m_providers;

    Result window_init();
    void window_deinit();
};

#endif
