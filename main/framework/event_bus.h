#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <vector>

// 事件总线：解耦模块间通信。模块发布事件，关心者订阅，互不直接引用。
enum EventId : int32_t {
    kEventConfigChanged = 1,      // 配置变更，void* 为 (char*)设备配置 key 或 nullptr
    kEventAppSwitch = 2,          // 应用切换，void* 为 (char*)目标 app id
    kEventNetworkStatus = 3,      // 网络状态变化
    kEventAiChatRecord = 4,       // AI 对话：切换"录音开始/停止"，ScreenAiChat 订阅
};

class EventBus {
public:
    using Handler = std::function<void(int32_t event_id, void* data)>;

    static EventBus& Instance();

    void Post(int32_t event_id, void* data = nullptr);
    void Subscribe(int32_t event_id, Handler handler);

private:
    EventBus() = default;
    std::map<int32_t, std::vector<Handler>> handlers_;
};