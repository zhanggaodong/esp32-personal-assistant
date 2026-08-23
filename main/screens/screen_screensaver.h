#pragma once

#include "framework/app/base_app.h"
#include <esp_timer.h>
#include <lvgl.h>

// 息屏显示（ScreenScreensaver）：空闲超时后全屏显示背景图 + 时钟/日期/自定文本。
// 所有外观参数均来自 ConfigStore（screensaver.*），监听 kEventConfigChanged 即时重绘。
class ScreenScreensaver : public BaseApp {
public:
    ScreenScreensaver();
    ~ScreenScreensaver();

    const AppMetadata& metadata() override;
    void onStart() override;
    void onShow() override;
    void onHide() override;
    void onStop() override;
    void onConfigChanged(const char* key) override;

private:
    void Rebuild();          // 依据配置重建背景/文字
    void RefreshClock();     // 刷新时间/日期文本（供定时器每秒调用，仅更新 label）
    void LoadBackground();   // 释放旧的背景解码缓冲

    AppMetadata metadata_;
    bool started_ = false;

    // LVGL 对象
    lv_obj_t* bg_obj_ = nullptr;        // 背景图或纯色层
    lv_obj_t* time_label_ = nullptr;    // 时间
    lv_obj_t* date_label_ = nullptr;    // 日期
    lv_obj_t* custom_label_ = nullptr;  // 自定文本

    // 背景解码缓冲（RGB565，需 heap_caps_free）
    uint8_t* bg_buf_ = nullptr;
    int bg_w_ = 0;
    int bg_h_ = 0;
    lv_img_dsc_t* bg_dsc_ = nullptr;    // 指向 bg_buf_

    esp_timer_handle_t clock_timer_ = nullptr;  // 每秒刷新时钟
    bool visible_ = false;                     // 是否处于前台
};