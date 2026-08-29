#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <driver/rtc_io.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "system_info.h"

#ifdef CONFIG_BOARD_TYPE_JIUCHUAN
#include "config.h"
#endif

#if defined(CONFIG_APP_MODE_FRAMEWORK) || defined(CONFIG_APP_MODE_HEADLESS_VOICE)
#include "framework/framework_main.h"
#endif

#define TAG "main"

#if defined(CONFIG_APP_MODE_HEADLESS_VOICE) && defined(CONFIG_BOARD_TYPE_JIUCHUAN)
namespace {
constexpr uint64_t kHeadlessBootHoldMs = 3000;

void EarlyHeadlessPowerGate() {
    const auto wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup == ESP_SLEEP_WAKEUP_EXT0) {
        rtc_gpio_deinit(PWR_BUTTON_GPIO);
    }
    gpio_config_t button_config = {};
    button_config.intr_type = GPIO_INTR_DISABLE;
    button_config.mode = GPIO_MODE_INPUT;
    button_config.pin_bit_mask = 1ULL << PWR_BUTTON_GPIO;
    button_config.pull_down_en = GPIO_PULLDOWN_ENABLE;
    button_config.pull_up_en = GPIO_PULLUP_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&button_config));

    const bool wokeByPowerButton = wakeup == ESP_SLEEP_WAKEUP_EXT0;
    const bool coldStartWithButton = esp_reset_reason() == ESP_RST_POWERON &&
                                     gpio_get_level(PWR_BUTTON_GPIO) == 1;
    const bool requiresHold = wokeByPowerButton || coldStartWithButton;

    rtc_gpio_init(PWR_EN_GPIO);
    rtc_gpio_set_direction(PWR_EN_GPIO, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_hold_dis(PWR_EN_GPIO);
    if (!requiresHold) {
        rtc_gpio_set_level(PWR_EN_GPIO, 1);
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT0);
        rtc_gpio_deinit(PWR_BUTTON_GPIO);
        return;
    }

    const int64_t startedUs = esp_timer_get_time();
    while (gpio_get_level(PWR_BUTTON_GPIO) == 1 &&
           esp_timer_get_time() - startedUs < (int64_t)kHeadlessBootHoldMs * 1000) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    const bool heldLongEnough = gpio_get_level(PWR_BUTTON_GPIO) == 1 &&
                                esp_timer_get_time() - startedUs >=
                                    (int64_t)kHeadlessBootHoldMs * 1000;
    if (heldLongEnough) {
        rtc_gpio_set_level(PWR_EN_GPIO, 1);
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT0);
        rtc_gpio_deinit(PWR_BUTTON_GPIO);
        return;
    }

    ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(PWR_BUTTON_GPIO, 1));
    ESP_ERROR_CHECK(rtc_gpio_pulldown_en(PWR_BUTTON_GPIO));
    ESP_ERROR_CHECK(rtc_gpio_pullup_dis(PWR_BUTTON_GPIO));
    rtc_gpio_set_level(PWR_EN_GPIO, 0);
    rtc_gpio_hold_en(PWR_EN_GPIO);
    esp_deep_sleep_start();
}
}  // namespace
#endif

extern "C" void app_main(void)
{
#if defined(CONFIG_APP_MODE_HEADLESS_VOICE) && defined(CONFIG_BOARD_TYPE_JIUCHUAN)
    EarlyHeadlessPowerGate();
#endif
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
