#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <led_strip.h>
#include <esp_err.h>
#include <mutex>

// 无屏 RGB 指示灯控制器：把颜色、常亮、闪烁、呼吸全部集中在本模块。
// 不再调用依赖 Application::GetDeviceState() 的 SingleLed::OnStateChanged()。
// 灯效由独立低优先级任务驱动（40ms 步进），不占用音频锁、不阻塞 Wi-Fi/HTTP。
class HeadlessLedController {
public:
    static HeadlessLedController& Instance();

    // 初始化单颗 WS2812（GRB 排列）。仅需调用一次；gpio==GPIO_NUM_NC 时 no-op。
    esp_err_t Init(gpio_num_t gpio);

    // 状态语义（颜色/亮度集中定义，见各方法实现）：
    void ShowReady();          // 已联网可对话：绿色常亮
    void ShowWifiConnecting(); // Wi-Fi 连接中：蓝色慢呼吸
    void ShowProvisioning();   // 配网模式：蓝色 500ms 闪
    void ShowRecording();      // 录音中（按住说话）：橙色低幅呼吸
    void ShowProcessing();     // 处理中/播报中：蓝色呼吸
    void ShowError();          // 不允许录音：红色 2 次短闪后恢复待机绿
    void ShowPowerOff();       // 关机：红色 3 次短闪后熄灭
    void ShowProvisioned();    // 配网成功：绿色 3 次短闪后常亮
    void Off();

private:
    HeadlessLedController() = default;

    enum class Mode { kOff, kSolid, kBlink, kBreathing };

    struct Cfg {
        Mode mode = Mode::kOff;
        uint8_t r = 0, g = 0, b = 0;                 // 目标颜色（亮度上限按板级安全值）
        int interval_ms = 200;                        // 闪烁：单次亮/灭时长
        int times = -1;                               // 闪烁次数（亮灭各计一次），-1 无限
        uint32_t breath_period_ms = 1200;             // 呼吸周期
        uint8_t restore_r = 0, restore_g = 0, restore_b = 0;  // 瞬态结束后恢复的颜色
    };

    void SetCfg(const Cfg& cfg);
    void ApplyPixel(uint8_t r, uint8_t g, uint8_t b);

    static void AnimTask(void* arg);
    void AnimLoop();

    led_strip_handle_t strip_ = nullptr;
    TaskHandle_t task_ = nullptr;
    std::mutex mutex_;
    Cfg cfg_;
    uint64_t phase_start_us_ = 0;  // 闪烁/呼吸相位起点
};
