#include "app_manager.h"

#include <string>
#include <string.h>

#include <esp_log.h>

#include "app_registry.h"
#include "board.h"

#define TAG "AppManager"

AppManager& AppManager::Instance() {
    static AppManager instance;
    return instance;
}

void AppManager::Start() {
    if (started_) {
        return;
    }
    BaseApp* main_app = nullptr;
    AppRegistry::Instance().ForEach([&](BaseApp* app) {
        if (app != nullptr && app->metadata().isMain) {
            main_app = app;
        }
    });

    if (main_app == nullptr) {
        ESP_LOGE(TAG, "No main app registered, framework cannot start");
        return;
    }

    current_ = main_app;
    current_->onStart();
    current_->onShow();
    started_ = true;
    ESP_LOGI(TAG, "Started with app: %s (%s)", current_->metadata().id, current_->metadata().name);
}

bool AppManager::SwitchTo(const char* id) {
    if (current_ != nullptr && current_->metadata().id[0] != '\0' &&
        strcmp(current_->metadata().id, id) == 0) {
        return true;
    }

    BaseApp* target = AppRegistry::Instance().Get(id);
    if (target == nullptr) {
        ESP_LOGW(TAG, "Unknown app id: %s", id);
        return false;
    }

    if (current_ != nullptr) {
        current_->onHide();
    }
    current_ = target;
    current_->onStart();
    current_->onShow();
    ESP_LOGI(TAG, "Switched to app: %s (%s)", current_->metadata().id, current_->metadata().name);
    return true;
}

int AppManager::Count() {
    return static_cast<int>(AppRegistry::Instance().size());
}

bool AppManager::SwitchToIndex(int index) {
    if (index < 0) {
        return false;
    }
    int i = 0;
    BaseApp* target = nullptr;
    AppRegistry::Instance().ForEach([&](BaseApp* app) {
        if (i == index) {
            target = app;
        }
        ++i;
    });
    if (target == nullptr) {
        return false;
    }
    return SwitchTo(target->metadata().id);
}

bool AppManager::Next() {
    if (current_ == nullptr) {
        return false;
    }
    int total = Count();
    if (total <= 1) {
        return false;
    }
    int index = 0;
    int cur = -1;
    AppRegistry::Instance().ForEach([&](BaseApp* app) {
        if (app == current_) {
            cur = index;
        }
        ++index;
    });
    if (cur < 0) {
        return false;
    }
    return SwitchToIndex((cur + 1) % total);
}

bool AppManager::Prev() {
    if (current_ == nullptr) {
        return false;
    }
    int total = Count();
    if (total <= 1) {
        return false;
    }
    int index = 0;
    int cur = -1;
    AppRegistry::Instance().ForEach([&](BaseApp* app) {
        if (app == current_) {
            cur = index;
        }
        ++index;
    });
    if (cur < 0) {
        return false;
    }
    return SwitchToIndex((cur - 1 + total) % total);
}

void AppManager::ShowMenu() {
    if (current_ == nullptr) {
        return;
    }
    // Phase A 占位：用文本列出菜单并高亮当前项，复用平台 Display 渲染
    std::string menu;
    int index = 0;
    AppRegistry::Instance().ForEach([&](BaseApp* app) {
        if (index > 0) {
            menu += "  ";
        }
        if (app == current_) {
            menu += "*";
        }
        menu += app->metadata().name;
        ++index;
    });

    auto* display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->SetStatus(menu.c_str());
    }
    ESP_LOGI(TAG, "Menu: %s", menu.c_str());
}