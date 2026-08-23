#pragma once

#include "framework/app/base_app.h"

// 设置占位屏：后续将承载按键映射、通用配置项等设置界面。
class ScreenSettings : public BaseApp {
public:
    const AppMetadata& metadata() override;
    void onShow() override;

private:
    AppMetadata metadata_{"settings", "设置", false};
};