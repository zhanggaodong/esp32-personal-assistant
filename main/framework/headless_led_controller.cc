#include "headless_led_controller.h"

#include <esp_check.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <math.h>

#define TAG "HeadlessLed"

#define ANIM_TICK_MS 40

// 板级安全亮度上限：仅使用低亮度，避免 WS2812 过亮影响功耗或喇叭噪声。
// 颜色值语义仍为 RGB，驱动负责 GRB 排列。
static constexpr uint8_t kReadyRed = 0, kReadyGreen = 16, kReadyBlue = 0;        // 绿
static constexpr uint8_t kWifiConnRed = 0, kWifiConnGreen = 0, kWifiConnBlue = 8; // 蓝(低亮)
static constexpr uint8_t kProvisionRed = 0, kProvisionGreen = 0, kProvisionBlue = 16;
static constexpr uint8_t kRecordRed = 16, kRecordGreen = 8, kRecordBlue = 0;     // 橙
static constexpr uint8_t kProcessRed = 0, kProcessGreen = 0, kProcessBlue = 16;
static constexpr uint8_t kErrorRed = 16, kErrorGreen = 0, kErrorBlue = 0;        // 红

HeadlessLedController& HeadlessLedController::Instance() {
    static HeadlessLedController instance;
    return instance;
}

esp_err_t HeadlessLedController::Init(gpio_num_t gpio) {
    if (gpio == GPIO_NUM_NC) {
        return ESP_OK;
    }
    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = gpio;
    strip_config.max_leds = 1;
    strip_config.led_model = LED_MODEL_WS2812;
    strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;

    led_strip_rmt_config_t rmt_config = {};
    rmt_config.resolution_hz = 10 * 1000 * 1000;  // 10MHz

    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_config, &rmt_config, &strip_),
                        TAG, "led strip init failed");
    led_strip_clear(strip_);

    if (xTaskCreate(AnimTask, "headless_led", 3072, this, 4, &task_) != pdPASS) {
        ESP_LOGE(TAG, "failed to create led anim task");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "headless LED initialized on GPIO%d", gpio);
    return ESP_OK;
}

void HeadlessLedController::SetCfg(const Cfg& cfg) {
    std::lock_guard<std::mutex> lock(mutex_);
    cfg_ = cfg;
    phase_start_us_ = esp_timer_get_time();
}

void HeadlessLedController::ApplyPixel(uint8_t r, uint8_t g, uint8_t b) {
    if (strip_ == nullptr) {
        return;
    }
    esp_err_t err = led_strip_set_pixel(strip_, 0, r, g, b);
    if (err == ESP_OK) {
        led_strip_refresh(strip_);
    } else {
        ESP_LOGE(TAG, "led_strip_set_pixel failed: %s", esp_err_to_name(err));
    }
}

void HeadlessLedController::ShowReady() {
    SetCfg({Mode::kSolid, kReadyRed, kReadyGreen, kReadyBlue, 0, -1, 0,
            kReadyRed, kReadyGreen, kReadyBlue});
}

void HeadlessLedController::ShowWifiConnecting() {
    SetCfg({Mode::kBreathing, kWifiConnRed, kWifiConnGreen, kWifiConnBlue, 0, -1, 2000,
            0, 0, 0});
}

void HeadlessLedController::ShowProvisioning() {
    SetCfg({Mode::kBlink, kProvisionRed, kProvisionGreen, kProvisionBlue, 500, -1, 0,
            kProvisionRed, kProvisionGreen, kProvisionBlue});
}

void HeadlessLedController::ShowRecording() {
    SetCfg({Mode::kBreathing, kRecordRed, kRecordGreen, kRecordBlue, 0, -1, 1200,
            0, 0, 0});
}

void HeadlessLedController::ShowProcessing() {
    SetCfg({Mode::kBreathing, kProcessRed, kProcessGreen, kProcessBlue, 0, -1, 1200,
            0, 0, 0});
}

void HeadlessLedController::ShowError() {
    // 红色 2 次短闪后恢复待机绿
    SetCfg({Mode::kBlink, kErrorRed, kErrorGreen, kErrorBlue, 250, 2, 0,
            kReadyRed, kReadyGreen, kReadyBlue});
}

void HeadlessLedController::ShowProvisioned() {
    // 绿色 3 次短闪后恢复常亮绿
    SetCfg({Mode::kBlink, kReadyRed, kReadyGreen, kReadyBlue, 250, 3, 0,
            kReadyRed, kReadyGreen, kReadyBlue});
}

void HeadlessLedController::Off() {
    SetCfg({Mode::kOff, 0, 0, 0, 0, -1, 0, 0, 0, 0});
}

void HeadlessLedController::AnimTask(void* arg) {
    static_cast<HeadlessLedController*>(arg)->AnimLoop();
    vTaskDelete(nullptr);
}

void HeadlessLedController::AnimLoop() {
    for (;;) {
        uint8_t on_r = 0, on_g = 0, on_b = 0;

        {
            std::lock_guard<std::mutex> lock(mutex_);

            switch (cfg_.mode) {
                case Mode::kOff:
                case Mode::kSolid:
                    on_r = cfg_.r;
                    on_g = cfg_.g;
                    on_b = cfg_.b;
                    break;

                case Mode::kBlink: {
                    uint64_t elapsed_ms = (esp_timer_get_time() - phase_start_us_) / 1000;
                    int halves = (int)(elapsed_ms / cfg_.interval_ms);
                    bool on = (halves % 2) == 0;
                    if (cfg_.times > 0 && halves >= cfg_.times * 2) {
                        // 有限次闪烁结束：恢复"瞬态前"颜色并转为常亮
                        cfg_.mode = Mode::kSolid;
                        cfg_.r = cfg_.restore_r;
                        cfg_.g = cfg_.restore_g;
                        cfg_.b = cfg_.restore_b;
                        on_r = cfg_.r;
                        on_g = cfg_.g;
                        on_b = cfg_.b;
                    } else {
                        on_r = on ? cfg_.r : 0;
                        on_g = on ? cfg_.g : 0;
                        on_b = on ? cfg_.b : 0;
                    }
                    break;
                }

                case Mode::kBreathing: {
                    uint64_t elapsed_us = esp_timer_get_time() - phase_start_us_;
                    uint64_t period_us = (uint64_t)cfg_.breath_period_ms * 1000;
                    double phase = (double)(elapsed_us % period_us) / (double)period_us;
                    // 余弦呼吸：0.3~1.0 倍亮度，低幅避免刺眼
                    double f = 0.3 + 0.7 * (0.5 - 0.5 * cos(2.0 * M_PI * phase));
                    on_r = (uint8_t)(cfg_.r * f);
                    on_g = (uint8_t)(cfg_.g * f);
                    on_b = (uint8_t)(cfg_.b * f);
                    break;
                }
            }
        }

        ApplyPixel(on_r, on_g, on_b);
        vTaskDelay(pdMS_TO_TICKS(ANIM_TICK_MS));
    }
}