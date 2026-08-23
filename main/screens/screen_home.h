#pragma once

#include "framework/app/base_app.h"

// 开机默认屏：展示框架主页。
// （Phase A 仅显示占位文本；Phase C 将在此基础上实现壁纸/标题/副标题的完整自定义渲染。）
class ScreenHome : public BaseApp {
public:
    const AppMetadata& metadata() override;
    void onShow() override;

private:
    AppMetadata metadata_{"home", "首页", true};
};