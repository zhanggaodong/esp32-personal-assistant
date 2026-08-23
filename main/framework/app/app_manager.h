#pragma once

#include "base_app.h"

// AppManager：管理当前激活的屏幕模块，负责切换生命周期（onHide/onShow）。
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

    // 已注册模块总数
    int Count();
    const char* CurrentId() const { return current_ ? current_->metadata().id : ""; }
    BaseApp* Current() const { return current_; }

private:
    AppManager() = default;
    BaseApp* current_ = nullptr;
    bool started_ = false;
};