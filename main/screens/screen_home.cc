#include "screen_home.h"

#include <esp_log.h>

#include "board.h"

#define TAG "ScreenHome"

const AppMetadata& ScreenHome::metadata() {
    return metadata_;
}

void ScreenHome::onShow() {
    auto* display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        // 复用平台屏显示默认主页文本（Phase A 占位）
        display->SetStatus("个人桌面助手");
        display->ShowNotification("框架已启动", 2000);
    }
    ESP_LOGI(TAG, "Home screen shown");
}