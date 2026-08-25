#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "system_info.h"

#if defined(CONFIG_APP_MODE_FRAMEWORK) || defined(CONFIG_APP_MODE_HEADLESS_VOICE)
#include "framework/framework_main.h"
#endif

#define TAG "main"

extern "C" void app_main(void)
{
    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

#ifdef CONFIG_APP_MODE_FRAMEWORK
    // Personal Assistant Framework 独立入口
    framework_main();
#elif defined(CONFIG_APP_MODE_HEADLESS_VOICE)
    // 无屏按键语音助手独立入口（不初始化 LCD/LVGL）
    headless_main();
#else
    // 原始小智 AI 语音助手
    // Initialize and run the application
    auto& app = Application::GetInstance();
    app.Initialize();
    app.Run();  // This function runs the main event loop and never returns
#endif
}
