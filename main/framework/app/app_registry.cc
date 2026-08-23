#include "app_registry.h"

#include <esp_log.h>
#include <string.h>

#define TAG "AppRegistry"

AppRegistry& AppRegistry::Instance() {
    static AppRegistry instance;
    return instance;
}

void AppRegistry::Register(BaseApp* app) {
    if (app == nullptr) {
        return;
    }
    apps_.push_back(app);
    ESP_LOGI(TAG, "Registered app: %s (%s)", app->metadata().id, app->metadata().name);
}

BaseApp* AppRegistry::Get(const char* id) {
    for (auto* app : apps_) {
        if (app != nullptr && strcmp(app->metadata().id, id) == 0) {
            return app;
        }
    }
    return nullptr;
}

void AppRegistry::ForEach(std::function<void(BaseApp*)> fn) {
    for (auto* app : apps_) {
        if (app != nullptr) {
            fn(app);
        }
    }
}