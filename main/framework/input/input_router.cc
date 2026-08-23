#include "input_router.h"

#include <string.h>

#include <esp_log.h>

#include "config/config_store.h"
#include "event_bus.h"
#include "app/app_manager.h"

#define TAG "InputRouter"

namespace {

// 现网 Schema 里已有的按钮映射项："按钮_事件" -> 配置 key
const char* kButtonKeys[] = {
    "vol_up_click",
    "vol_down_click",
    "power_click",
    "power_long",
};
const int kButtonKeyCount = sizeof(kButtonKeys) / sizeof(kButtonKeys[0]);

}  // namespace

InputRouter& InputRouter::Instance() {
    static InputRouter instance;
    return instance;
}

KeyAction InputRouter::ParseAction(const char* value) {
    if (value == nullptr) {
        return KeyAction::kNone;
    }
    if (strcmp(value, "nav_up") == 0) return KeyAction::kNavUp;
    if (strcmp(value, "nav_down") == 0) return KeyAction::kNavDown;
    if (strcmp(value, "confirm") == 0) return KeyAction::kConfirm;
    if (strcmp(value, "back") == 0) return KeyAction::kBack;
    if (strcmp(value, "home") == 0) return KeyAction::kHome;
    if (strcmp(value, "menu") == 0) return KeyAction::kMenu;
    return KeyAction::kNone;
}

void InputRouter::ReloadMapping() {
    mapping_.clear();
    auto& store = ConfigStore::Instance();
    for (int i = 0; i < kButtonKeyCount; ++i) {
        std::string key = std::string("keys.") + kButtonKeys[i];
        mapping_[kButtonKeys[i]] = ParseAction(store.Get(key.c_str()).c_str());
    }
}

void InputRouter::Init() {
    ReloadMapping();
    // 配置变更时重载映射，令网页修改按键映射即时生效
    EventBus::Instance().Subscribe(kEventConfigChanged,
                                   [this](int32_t, void*) { this->OnConfigChanged(); });
    ESP_LOGI(TAG, "InputRouter initialized with %d key mappings", (int)mapping_.size());
}

void InputRouter::OnConfigChanged() {
    ReloadMapping();
}

void InputRouter::HandleKeyEvent(const char* button_id, const char* event) {
    if (button_id == nullptr || event == nullptr) {
        return;
    }
    // 任何按键都视为一次用户交互：重置空闲计时 + 若在息屏则唤醒回主页
    AppManager::Instance().NotifyInput();

    // 按钮 "vol_up" + 事件 "click" -> 配置键名 "vol_up_click"
    std::string cfgName = std::string(button_id) + "_" + event;
    auto it = mapping_.find(cfgName);
    if (it == mapping_.end()) {
        return;
    }
    Execute(it->second);
}

void InputRouter::Execute(KeyAction action) {
    auto& mgr = AppManager::Instance();
    switch (action) {
        case KeyAction::kNavUp:
            mgr.Prev();
            break;
        case KeyAction::kNavDown:
            mgr.Next();
            break;
        case KeyAction::kMenu:
            mgr.ShowMenu();
            break;
        case KeyAction::kHome:
        case KeyAction::kBack:
            mgr.SwitchTo("home");
            break;
        case KeyAction::kConfirm:
            // 在 AI 对话屏内，确认键作为"录音开始/停止"开关
            if (strcmp(mgr.CurrentId(), "ai_chat") == 0) {
                EventBus::Instance().Post(kEventAiChatRecord);
            }
            break;
        case KeyAction::kNone:
        default:
            break;
    }
}