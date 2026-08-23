#pragma once

#include <cstdint>

#include "esp_timer.h"

#include "base_app.h"

// AppManager：管理当前激活的屏幕模块，负责切换生命周期（onHide/onShow）。
// 额外承担"空闲超时进息屏 + 按键唤醒"的调度（读 screensaver.* 配置）。
class AppManager {
public:
    static AppManager& Instance();

    // 初始化框架并显示默认屏（metadata().isMain == true 的模块）
    void Start();

    // 切换当前模块；不存在返回 false，已是当前模块返回 true
    bool SwitchTo(const char* id);

    // 在已注册模块间按注册顺序翻页切换（菜单导航）
    bool Next();
    bool Prev();

    // 切换到指定注册顺序 index 的模块
    bool SwitchToIndex(int index);

    // 在屏幕上展示一个临时文本菜单（Phase A 占位实现，复用平台 Display）
    void ShowMenu();

    // 有用户交互：重置空闲计时；若正处息屏则唤醒回主页
    void NotifyInput();

    // 周期空闲检测（由定时器回调调用）：超时则切入息屏
    void NotifyIdle();

    // 已注册模块总数
    int Count();
    const char* CurrentId() const { return current_ ? current_->metadata().id : ""; }
    BaseApp* Current() const { return current_; }

private:
    AppManager() = default;
    BaseApp* current_ = nullptr;
    bool started_ = false;

    uint64_t last_input_us_ = 0;                  // 最后一次用户输入时间(esp_timer_get_time)
    esp_timer_handle_t idle_timer_ = nullptr;     // 周期空闲检测
};