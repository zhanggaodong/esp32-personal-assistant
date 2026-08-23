#pragma once

#include <functional>
#include <vector>

#include "base_app.h"

// App 模块注册表：集中管理所有 BaseApp 模块，供 AppManager 列出菜单与切换。
class AppRegistry {
public:
    static AppRegistry& Instance();

    void Register(BaseApp* app);        // 由各模块自注册
    BaseApp* Get(const char* id);       // 按 id 查找
    void ForEach(std::function<void(BaseApp*)> fn);   // 遍历全部模块
    size_t size() const { return apps_.size(); }

private:
    AppRegistry() = default;
    std::vector<BaseApp*> apps_;
};

// 全局单例用法：AppRegistry::Instance().Register(&myApp);