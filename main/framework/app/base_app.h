#pragma once

#include <cstdint>

// 一个可切换的界面/功能称为"App 模块"。
// 生命周期：Register -> onStart -> onShow -> (事件回调) -> onHide -> onStop
struct AppMetadata {
    const char* id;      // 唯一 id："home" / "screensaver" / "settings" / "ai_chat"
    const char* name;    // 菜单显示名
    bool isMain;         // 是否开机默认屏
};

class BaseApp {
public:
    virtual ~BaseApp() = default;

    virtual const AppMetadata& metadata() = 0;

    virtual void onStart() {}          // 仅首次进入时分配资源
    virtual void onShow() {}           // 每次切到前台，重建可见 UI
    virtual void onHide() {}           // 每次切走，隐藏/释放临时资源
    virtual void onStop() {}           // 框架退出时释放

    virtual void onConfigChanged(const char* key) {}   // 该模块关心的配置变化
    virtual void onAppEvent(int32_t event_id, void* data) {}  // 跨模块消息(EventBus)
};