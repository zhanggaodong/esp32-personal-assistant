#include "screen_settings.h"

#include <esp_log.h>

#include "board.h"
#include "display/display.h"

#define TAG "ScreenSettings"

const AppMetadata& ScreenSettings::metadata() {
    return metadata_;
}

void ScreenSettings::onShow() {
    auto* display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->SetStatus("设置");
        display->ShowNotification("设置界面(占位)", 1500);
    }
    ESP_LOGI(TAG, "Settings screen shown");
}