#pragma once

#include <lvgl.h>

#include "framework/app/base_app.h"

// 开机默认屏（主页）：以 LVGL 完整绘制壁纸背景 + 主/副标题文字（支持跑马灯滚动）。
// 所有外观参数均来自 ConfigStore（common.title / general.* / wallpaper.*），
// 监听 kEventConfigChanged 即时重绘，响应网页改动。
class ScreenHome : public BaseApp {
public:
    ScreenHome();
    ~ScreenHome();

    const AppMetadata& metadata() override;
    void onStart() override;
    void onShow() override;
    void onHide() override;
    void onStop() override;
    void onConfigChanged(const char* key) override;

private:
    void Rebuild();      // 依据当前配置重建/更新全部 UI
    void LoadWallpaper();  // 从 LittleFS 解码壁纸

    AppMetadata metadata_;
    bool started_ = false;

    // LVGL 对象
    lv_obj_t* wallpaper_img_ = nullptr;   // 壁纸图层
    lv_obj_t* title_label_ = nullptr;     // 主标题
    lv_obj_t* subtitle_label_ = nullptr;  // 副标题

    // 壁纸解码缓冲（RGB565，需 heap_caps_free）
    uint8_t* wallpaper_buf_ = nullptr;
    int wallpaper_w_ = 0;
    int wallpaper_h_ = 0;
    lv_img_dsc_t* wallpaper_dsc_ = nullptr;  // 指向 wallpaper_buf_, 供 lv_image 引用
};