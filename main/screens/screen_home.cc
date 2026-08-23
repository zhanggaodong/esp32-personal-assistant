#include "screen_home.h"

#include <cstring>

#include <esp_log.h>
#include <esp_heap_caps.h>

#include <lvgl.h>

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);

#include "board.h"
#include "display/display.h"
#include "config/config_store.h"
#include "storage/littlefs_store.h"
#include "display/lvgl_display/jpg/jpeg_to_image.h"
#include "display/lvgl_display/lvgl_image.h"

#define TAG "ScreenHome"

// 解析 "#RRGGBB" 字符串为 lv_color_t
static lv_color_t ParseColor(const std::string& s, uint8_t fallback_r, uint8_t fallback_g, uint8_t fallback_b) {
    uint8_t r = fallback_r, g = fallback_g, b = fallback_b;
    if (s.size() >= 7 && s[0] == '#') {
        auto hex2 = [](const char* p) -> int {
            int v = 0;
            for (int i = 0; i < 2; ++i) {
                char c = p[i];
                v <<= 4;
                if (c >= '0' && c <= '9') v |= (c - '0');
                else if (c >= 'a' && c <= 'f') v |= (c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') v |= (c - 'A' + 10);
            }
            return v;
        };
        r = (uint8_t)hex2(s.c_str() + 1);
        g = (uint8_t)hex2(s.c_str() + 3);
        b = (uint8_t)hex2(s.c_str() + 5);
    }
    return lv_color_make(r, g, b);
}

// 从 LittleFS 读取并解码壁纸到 wallpaper_buf_/wallpaper_dsc_
// 返回是否成功解码出壁纸
bool TryDecodeWallpaper(const std::string& file, uint8_t** out_buf, size_t* out_len,
                        int* out_w, int* out_h, lv_img_dsc_t** out_dsc) {
    std::vector<uint8_t> raw;
    if (file.empty() || !LittleFsStore::ReadFile(file.c_str(), raw) || raw.empty()) {
        return false;
    }
    uint8_t* pix = nullptr;
    size_t len = 0, w = 0, h = 0, stride = 0;
    if (jpeg_to_image(raw.data(), raw.size(), &pix, &len, &w, &h, &stride) != ESP_OK || pix == nullptr) {
        return false;
    }
    // 构造持久化的 lv_img_dsc_t（生命周期属于 ScreenHome 成员）
    lv_img_dsc_t* dsc = new lv_img_dsc_t();
    bzero(dsc, sizeof(lv_img_dsc_t));
    dsc->data_size = len;
    dsc->data = pix;                       // 不复制，直接引用解码缓冲
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.w = (int)w;
    dsc->header.h = (int)h;
    dsc->header.stride = ((int)w) * 2;

    *out_buf = pix;
    *out_len = len;
    *out_w = (int)w;
    *out_h = (int)h;
    *out_dsc = dsc;
    return true;
}

ScreenHome::ScreenHome() {
    metadata_ = {"home", "首页", true};
}

ScreenHome::~ScreenHome() {
    LoadWallpaper();
    onStop();
}

const AppMetadata& ScreenHome::metadata() {
    return metadata_;
}

void ScreenHome::LoadWallpaper() {
    // 释放上一份壁纸缓冲与 dsc（新壁纸解码前调用）
    if (wallpaper_dsc_ != nullptr) {
        delete wallpaper_dsc_;
        wallpaper_dsc_ = nullptr;
    }
    if (wallpaper_buf_ != nullptr) {
        heap_caps_free(wallpaper_buf_);
        wallpaper_buf_ = nullptr;
        wallpaper_w_ = 0;
        wallpaper_h_ = 0;
    }
}

void ScreenHome::onStart() {
    if (started_) {
        return;
    }
    started_ = true;
    ESP_LOGI(TAG, "Home screen started");
}

void ScreenHome::Rebuild() {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    DisplayLockGuard lock(display);

    auto& store = ConfigStore::Instance();

    // 屏幕根对象
    lv_obj_t* scr = lv_screen_active();

    // 1) 壁纸背景（在创建 img 前先更新解码缓冲）
    bool use_wallpaper = store.GetBool("wallpaper.enabled", false);
    std::string wp_file = store.Get("wallpaper.file");

    if (wallpaper_img_ == nullptr) {
        if (use_wallpaper && !wp_file.empty()) {
            size_t len = 0;
            if (TryDecodeWallpaper(wp_file, &wallpaper_buf_, &len, &wallpaper_w_, &wallpaper_h_, &wallpaper_dsc_)) {
                lv_obj_t* img = lv_image_create(scr);
                lv_image_set_src(img, wallpaper_dsc_);
                // 按屏幕尺寸等比放大铺满全屏（LVGL 缩放单位为 256=100%）
                if (wallpaper_w_ > 0 && wallpaper_h_ > 0) {
                    int zoom_w = (LV_HOR_RES * 256) / wallpaper_w_;
                    int zoom_h = (LV_VER_RES * 256) / wallpaper_h_;
                    lv_image_set_scale(img, zoom_w > zoom_h ? zoom_w : zoom_h);
                }
                lv_obj_set_size(img, LV_HOR_RES, LV_VER_RES);
                lv_obj_set_pos(img, 0, 0);
                wallpaper_img_ = img;
            }
        }
        if (wallpaper_img_ == nullptr) {
            // 无壁纸：纯色层打底
            lv_obj_t* bg = lv_obj_create(scr);
            lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
            wallpaper_img_ = bg;
        }
    }
    if (wallpaper_img_ != nullptr) {
        lv_obj_set_size(wallpaper_img_, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_pos(wallpaper_img_, 0, 0);
        lv_obj_set_style_border_width(wallpaper_img_, 0, 0);
        lv_obj_set_style_bg_color(wallpaper_img_, lv_color_black(), 0);
    }

    // 2) 标题
    if (title_label_ == nullptr) {
        title_label_ = lv_label_create(scr);
    }
    std::string title = store.Get("general.title");
    std::string color = store.Get("general.title_color");
    std::string pos = store.Get("general.title_position");
    bool scroll = store.GetBool("general.title_scroll", false);
    int speed = store.GetInt("general.title_scroll_speed", 5);

    lv_label_set_text(title_label_, title.c_str());
    lv_obj_set_style_text_color(title_label_, ParseColor(color, 255, 255, 255), 0);
    lv_obj_set_style_text_font(title_label_, &BUILTIN_TEXT_FONT, 0);

    lv_obj_update_layout(title_label_);
    int y = 40;
    lv_obj_set_pos(title_label_, 0, y);
    lv_obj_set_width(title_label_, LV_HOR_RES);
    lv_obj_set_style_text_align(title_label_, pos == "left" ? LV_TEXT_ALIGN_LEFT
                                  : pos == "right" ? LV_TEXT_ALIGN_RIGHT
                                  : LV_TEXT_ALIGN_CENTER, 0);

    if (scroll) {  // 跑马灯：超过宽度自动循环滚动
        lv_obj_set_width(title_label_, LV_HOR_RES * 2 / 3);  // 留出空间以触发滚动
        lv_label_set_long_mode(title_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        unsigned spd = static_cast<unsigned>(speed * 1000);
        lv_obj_set_style_anim_time(title_label_, spd <= 0 ? 5000 : spd, 0);
    } else {
        lv_label_set_long_mode(title_label_, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(title_label_, LV_HOR_RES);
    }

    // 3) 副标题
    if (subtitle_label_ == nullptr) {
        subtitle_label_ = lv_label_create(scr);
    }
    std::string subtitle = store.Get("general.subtitle");
    lv_label_set_text(subtitle_label_, subtitle.c_str());
    lv_obj_set_style_text_color(subtitle_label_, lv_color_white(), 0);
    lv_obj_set_style_text_font(subtitle_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_width(subtitle_label_, LV_HOR_RES);
    lv_obj_set_style_text_align(subtitle_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(subtitle_label_, 0, y + 34);

    ESP_LOGI(TAG, "Home rebuilt (wallpaper=%d title=%s)", wallpaper_img_ != nullptr, title.c_str());
}

void ScreenHome::onShow() {
    if (!started_) {
        onStart();
    }
    Rebuild();
    // 隐藏平台状态栏等以留出全屏绘制（可选：不隐藏，避免破坏小智状态栏）
    ESP_LOGI(TAG, "Home screen shown");
}

void ScreenHome::onHide() {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    DisplayLockGuard lock(display);
    // 隐藏本屏 LVGL 对象，避免叠加在其他屏之上
    if (wallpaper_img_ != nullptr) lv_obj_add_flag(static_cast<lv_obj_t*>(wallpaper_img_), LV_OBJ_FLAG_HIDDEN);
    if (title_label_ != nullptr) lv_obj_add_flag(static_cast<lv_obj_t*>(title_label_), LV_OBJ_FLAG_HIDDEN);
    if (subtitle_label_ != nullptr) lv_obj_add_flag(static_cast<lv_obj_t*>(subtitle_label_), LV_OBJ_FLAG_HIDDEN);
}

void ScreenHome::onStop() {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    DisplayLockGuard lock(display);
    if (wallpaper_img_ != nullptr) { lv_obj_delete(static_cast<lv_obj_t*>(wallpaper_img_)); wallpaper_img_ = nullptr; }
    if (title_label_ != nullptr) { lv_obj_delete(static_cast<lv_obj_t*>(title_label_)); title_label_ = nullptr; }
    if (subtitle_label_ != nullptr) { lv_obj_delete(static_cast<lv_obj_t*>(subtitle_label_)); subtitle_label_ = nullptr; }
    LoadWallpaper();
    ESP_LOGI(TAG, "Home screen stopped");
}

void ScreenHome::onConfigChanged(const char* key) {
    if (key == nullptr) {
        return;
    }
    // 只关心与主页相关的配置，避免每次都全局重建
    const char* home_keys[] = {"general.title", "general.subtitle", "general.title_color",
                               "general.title_position", "general.title_scroll",
                               "general.title_scroll_speed", "wallpaper.file", "wallpaper.enabled"};
    for (const char* k : home_keys) {
        if (strcmp(k, key) == 0) {
            Rebuild();
            return;
        }
    }
}