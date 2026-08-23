#include "event_bus.h"

EventBus& EventBus::Instance() {
    static EventBus instance;
    return instance;
}

void EventBus::Subscribe(int32_t event_id, Handler handler) {
    if (!handler) {
        return;
    }
    handlers_[event_id].push_back(std::move(handler));
}

void EventBus::Post(int32_t event_id, void* data) {
    auto it = handlers_.find(event_id);
    if (it == handlers_.end()) {
        return;
    }
    // 拷贝一份，避免订阅回调中修改 handlers_ 导致迭代器失效
    auto handlers = it->second;
    for (auto& h : handlers) {
        h(event_id, data);
    }
}